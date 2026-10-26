#include "pipeline.h"

#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "ac3_encoder.h"
#include "endpoints.h"
#include "hdmi_output.h"
#include "iec61937.h"
#include "log.h"
#include "settings.h"

namespace w51 {
namespace {

// Turns a sequence of IEC 61937 bursts into a continuous stream of 4-byte frames.
class BurstStream {
public:
    using NextBurstFn = std::function<void(uint8_t* burst)>;
    explicit BurstStream(NextBurstFn next) : next_(std::move(next)) {}

    void Reset() { offset_ = iec61937::kBurstBytes; }

    void Read(BYTE* dst, UINT32 frames) {
        size_t bytes = (size_t)frames * iec61937::kBytesPerFrame;
        while (bytes > 0) {
            if (offset_ == iec61937::kBurstBytes) {
                next_(burst_);
                offset_ = 0;
            }
            size_t n = std::min(bytes, (size_t)iec61937::kBurstBytes - offset_);
            memcpy(dst, burst_ + offset_, n);
            dst += n;
            bytes -= n;
            offset_ += n;
        }
    }

private:
    NextBurstFn next_;
    uint8_t burst_[iec61937::kBurstBytes] = {};
    size_t offset_ = iec61937::kBurstBytes;
};

}  // namespace

Pipeline::Pipeline(PcmSource& source, PipelineConfig config) : source_(source), config_(std::move(config)) {
    status_.outputSetting = config_.out;
}

void Pipeline::SetPaused(bool paused) {
    if (paused_.exchange(paused) == paused) return;
    Log(paused ? "Releasing the HDMI output" : "Taking the HDMI output again");
    interrupt_ = true;
}

void Pipeline::SetOutput(const std::wstring& output) {
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        if (status_.outputSetting == output) return;
        status_.outputSetting = output;
    }
    Log("Output setting: %s", output.empty() ? "automatic" : Narrow(output).c_str());
    if (config_.persistOutput && !SaveOutputSetting(output)) Log("Cannot store the output setting in the registry");
    interrupt_ = true;
}

PipelineStatus Pipeline::Status() {
    PipelineStatus s;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        s = status_;
    }
    s.captureConnected = source_.Connected();
    s.playing = source_.Playing();
    return s;
}

void Pipeline::SetState(PipelineState state, WaitReason reason, const std::wstring& output, const std::string& detail) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.state = state;
    status_.reason = reason;
    status_.output = output;
    status_.detail = detail;
}

void Pipeline::Wait(const std::atomic<bool>& stop, DWORD ms) {
    for (DWORD slept = 0; slept < ms && !stop && !interrupt_; slept += 50) Sleep(50);
}

int Pipeline::Run(std::atomic<bool>& stop) {
    Ac3Encoder encoder;
    if (!encoder.Open(kSampleRate, kMask51Side, config_.bitrateKbps * 1000)) return 1;

    FILE* dump = nullptr;
    if (!config_.dumpPath.empty() && _wfopen_s(&dump, config_.dumpPath.c_str(), L"wb") != 0) {
        Log("Cannot open dump file %s", Narrow(config_.dumpPath).c_str());
        return 1;
    }

    struct {
        std::mutex m;
        uint64_t bursts = 0, failures = 0;
        double encodeSumMs = 0, encodeMaxMs = 0;
    } stats;

    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    std::vector<float> pcm((size_t)kFrameSamples * kChannels);
    std::vector<uint8_t> ac3;

    BurstStream stream([&](uint8_t* burst) {
        source_.Pull(pcm.data());
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        bool ok = encoder.Encode(pcm.data(), ac3) && iec61937::PackAc3(ac3.data(), ac3.size(), burst);
        QueryPerformanceCounter(&t1);
        if (!ok) memset(burst, 0, iec61937::kBurstBytes);
        if (dump) fwrite(burst, 1, iec61937::kBurstBytes, dump);

        const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / qpf.QuadPart;
        std::lock_guard<std::mutex> lock(stats.m);
        stats.bursts++;
        stats.encodeSumMs += ms;
        stats.encodeMaxMs = std::max(stats.encodeMaxMs, ms);
        if (!ok) stats.failures++;
    });

    if (config_.offline) {
        // Encode --seconds of audio as fast as possible, e.g. to check a --dump file.
        std::vector<BYTE> block(48000 * iec61937::kBytesPerFrame);
        const UINT32 blocks = (UINT32)std::max(1.0, config_.seconds);
        for (UINT32 i = 0; i < blocks && !stop; ++i) {
            source_.Tick();
            stream.Read(block.data(), 48000);
        }
        if (dump) fclose(dump);
        Log("Encoded %u s offline, %llu AC3 frames", blocks, (unsigned long long)stats.bursts);
        return 0;
    }

    std::thread audio([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring lastMissing = L"\n";  // the output setting a "waiting" message was logged for
        std::string lastFailure;            // the last open failure that was logged
        while (!stop) {
            interrupt_ = false;
            if (paused_) {
                SetState(PipelineState::Paused, WaitReason::None, {}, {});
                Wait(stop, 1000);
                continue;
            }

            std::wstring setting;
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                setting = status_.outputSetting;
            }
            EndpointInfo out;
            HRESULT hr = setting.empty() ? FindAc3PassthroughEndpoint(out) : FindRenderEndpoint(setting, out);
            if (FAILED(hr)) {
                if (setting != lastMissing) {
                    if (setting.empty()) Log("Waiting for an HDMI output that accepts AC3 passthrough...");
                    else Log("Waiting for output \"%s\"...", Narrow(setting).c_str());
                    lastMissing = setting;
                }
                SetState(PipelineState::Waiting, WaitReason::NoOutput, {}, {});
                Wait(stop, 2000);
                continue;
            }
            lastMissing = L"\n";

            HdmiOutput hdmi;
            hr = hdmi.Open(out.device.Get(), (REFERENCE_TIME)(config_.periodMs * 10000));
            if (SUCCEEDED(hr)) {
                lastFailure.clear();
                Log("Output: %s", Narrow(out.name).c_str());
                source_.Reset();
                stream.Reset();
                SetState(PipelineState::Streaming, WaitReason::None, out.name, {});
                hr = hdmi.Run([&] { return stop || interrupt_; },
                              [&](BYTE* dst, UINT32 frames) {
                                  source_.Tick();
                                  stream.Read(dst, frames);
                              });
            } else {
                // Retried every 2 s, e.g. for the whole film while a player bitstreams to the same output.
                std::string failure = Narrow(out.name) + ": " + HrToString(hr);
                if (failure != lastFailure) Log("Cannot open output %s", failure.c_str());
                lastFailure = std::move(failure);
            }
            hdmi.Close();
            if (stop || interrupt_) continue;
            // Another program holds the output, the device went away (TV off) or it failed: retry shortly.
            SetState(PipelineState::Waiting,
                     hr == AUDCLNT_E_DEVICE_IN_USE ? WaitReason::OutputBusy : WaitReason::OutputError, out.name,
                     FAILED(hr) ? HrToString(hr) : std::string());
            Wait(stop, 2000);
        }
        CoUninitialize();
    });

    const ULONGLONG start = GetTickCount64();
    ULONGLONG lastStats = start;
    while (!stop) {
        Sleep(200);
        const ULONGLONG now = GetTickCount64();
        if (config_.seconds > 0 && now - start >= (ULONGLONG)(config_.seconds * 1000)) stop = true;
        if (now - lastStats >= 10000) {
            lastStats = now;
            uint64_t bursts, failures;
            double avgMs, maxMs;
            {
                std::lock_guard<std::mutex> lock(stats.m);
                bursts = stats.bursts;
                failures = stats.failures;
                avgMs = bursts ? stats.encodeSumMs / bursts : 0;
                maxMs = stats.encodeMaxMs;
                stats.bursts = stats.failures = 0;
                stats.encodeSumMs = stats.encodeMaxMs = 0;
            }
            std::string sourceStats = source_.TakeStats();
            Log("AC3 frames %llu (failed %llu), encode avg %.2f / max %.2f ms%s%s", (unsigned long long)bursts,
                (unsigned long long)failures, avgMs, maxMs, sourceStats.empty() ? "" : "; ", sourceStats.c_str());
        }
    }
    audio.join();
    if (dump) fclose(dump);
    Log("Stopped");
    return 0;
}

}  // namespace w51
