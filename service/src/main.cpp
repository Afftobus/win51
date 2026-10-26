// win51svc: encodes the Win51 virtual 5.1 output to AC3 in real time and plays it to HDMI as an
// IEC 61937 bitstream (the same signal a player sends with "AC3 passthrough" enabled).
//
//   win51svc list                 render endpoints and which of them accept AC3 passthrough
//   win51svc tone [options]       channel identification test signal, no driver needed
//   win51svc run  [options]       encode what Windows plays to the Win51 endpoint
//   win51svc service [options]    the same as run, started by the service control manager

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ac3_encoder.h"
#include "endpoints.h"
#include "hdmi_output.h"
#include "iec61937.h"
#include "log.h"
#include "pcm_source.h"

using namespace w51;

namespace {

std::atomic<bool> g_stop{false};

struct Options {
    std::wstring command;
    std::wstring in = kWin51EndpointName;  // endpoint captured by "run"
    std::wstring out;                       // empty: the HDMI/DisplayPort output that accepts AC3
    int bitrateKbps = 640;
    double periodMs = 10.0;                 // HDMI exclusive-mode period, 0 = device minimum
    double targetMs = 45.0;                 // loopback jitter buffer target
    double seconds = 0;                     // 0 = until stopped
    int onlyChannel = -1;                   // tone: repeat one channel (index in FL FR FC LFE SL SR)
    std::wstring dumpPath;                  // optional copy of the IEC 61937 stream (s16le stereo)
    std::wstring logPath;
};

void PrintUsage() {
    fputs("usage: win51svc list\n"
          "       win51svc tone|run|service [--out <name>] [--in <name>] [--bitrate <kbps>] [--period-ms <ms>]\n"
          "                                 [--target-ms <ms>] [--seconds <n>] [--dump <file>] [--log <file>]\n"
          "                                 [--only fl|fr|fc|lfe|sl|sr]   (tone: one channel only)\n",
          stderr);
}

bool ParseOptions(int argc, wchar_t** argv, Options& o) {
    if (argc < 2) return false;
    o.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::wstring key = argv[i];
        if (i + 1 >= argc) return false;
        std::wstring value = argv[++i];
        if (key == L"--out") o.out = value;
        else if (key == L"--in") o.in = value;
        else if (key == L"--bitrate") o.bitrateKbps = _wtoi(value.c_str());
        else if (key == L"--period-ms") o.periodMs = _wtof(value.c_str());
        else if (key == L"--target-ms") o.targetMs = _wtof(value.c_str());
        else if (key == L"--seconds") o.seconds = _wtof(value.c_str());
        else if (key == L"--dump") o.dumpPath = value;
        else if (key == L"--log") o.logPath = value;
        else if (key == L"--only") {
            static const wchar_t* const kNames[] = {L"fl", L"fr", L"fc", L"lfe", L"sl", L"sr"};
            o.onlyChannel = -1;
            for (int c = 0; c < 6; ++c)
                if (_wcsicmp(value.c_str(), kNames[c]) == 0) o.onlyChannel = c;
            if (o.onlyChannel < 0) return false;
        }
        else return false;
    }
    return true;
}

BOOL WINAPI CtrlHandler(DWORD event) {
    // Logoff and shutdown keep their default handling; a service hears about them from the SCM instead.
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT) return FALSE;
    g_stop = true;
    return TRUE;
}

// Sleeps up to ms milliseconds, returning early when stopping.
void SleepUnlessStopped(DWORD ms) {
    for (DWORD slept = 0; slept < ms && !g_stop; slept += 50) Sleep(50);
}

int ListCommand() {
    std::wstring defaultId;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(enumerator.GetAddressOf())))) {
        ComPtr<IMMDevice> def;
        LPWSTR id = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, def.GetAddressOf())) &&
            SUCCEEDED(def->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    EndpointInfo autoOut;
    if (FAILED(FindAc3PassthroughEndpoint(autoOut))) autoOut.id.clear();

    for (auto& ep : ListRenderEndpoints(DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED | DEVICE_STATE_DISABLED)) {
        const char* state = ep.state == DEVICE_STATE_ACTIVE      ? "active"
                            : ep.state == DEVICE_STATE_UNPLUGGED ? "unplugged"
                            : ep.state == DEVICE_STATE_DISABLED  ? "disabled"
                                                                 : "?";
        std::string mix;
        bool ac3 = false;
        if (ep.state == DEVICE_STATE_ACTIVE) {
            ac3 = SupportsExclusiveAc3(ep.device.Get());
            ComPtr<IAudioClient> client;
            WAVEFORMATEX* f = nullptr;
            if (SUCCEEDED(ep.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                              (void**)client.GetAddressOf())) &&
                SUCCEEDED(client->GetMixFormat(&f))) {
                char buf[64];
                DWORD mask = f->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                                 ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(f)->dwChannelMask
                                 : 0;
                snprintf(buf, sizeof(buf), "%u ch/%lu Hz/mask 0x%lx", f->nChannels, f->nSamplesPerSec, mask);
                mix = buf;
                CoTaskMemFree(f);
            }
        }
        Log("%-9s %s%s%s%s | %s | %s", state, Narrow(ep.name).c_str(), ep.id == defaultId ? " [default]" : "",
            ac3 ? " [AC3 passthrough]" : "", ep.id == autoOut.id ? " [auto output]" : "", mix.c_str(),
            Narrow(ep.id).c_str());
    }
    return 0;
}

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

int RunPipeline(PcmSource& source, const Options& o) {
    Ac3Encoder encoder;
    if (!encoder.Open(kSampleRate, kMask51Side, o.bitrateKbps * 1000)) return 1;

    FILE* dump = nullptr;
    if (!o.dumpPath.empty() && _wfopen_s(&dump, o.dumpPath.c_str(), L"wb") != 0) {
        Log("Cannot open dump file %s", Narrow(o.dumpPath).c_str());
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
        source.Pull(pcm.data());
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

    if (o.out == L"none") {
        // Offline: encode --seconds of audio as fast as possible, e.g. to check a --dump file.
        std::vector<BYTE> block(48000 * iec61937::kBytesPerFrame);
        const UINT32 blocks = (UINT32)std::max(1.0, o.seconds);
        for (UINT32 i = 0; i < blocks && !g_stop; ++i) {
            source.Tick();
            stream.Read(block.data(), 48000);
        }
        if (dump) fclose(dump);
        Log("Encoded %u s offline, %llu AC3 frames", blocks, (unsigned long long)stats.bursts);
        return 0;
    }

    std::thread audio([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool missingLogged = false;
        while (!g_stop) {
            EndpointInfo out;
            HRESULT hr = o.out.empty() ? FindAc3PassthroughEndpoint(out) : FindRenderEndpoint(o.out, out);
            if (FAILED(hr)) {
                if (!missingLogged) {
                    if (o.out.empty()) Log("Waiting for an HDMI output that accepts AC3 passthrough...");
                    else Log("Waiting for output \"%s\"...", Narrow(o.out).c_str());
                }
                missingLogged = true;
                SleepUnlessStopped(2000);
                continue;
            }
            missingLogged = false;
            Log("Output: %s", Narrow(out.name).c_str());
            HdmiOutput hdmi;
            if (SUCCEEDED(hdmi.Open(out.device.Get(), (REFERENCE_TIME)(o.periodMs * 10000)))) {
                source.Reset();
                stream.Reset();
                hdmi.Run(g_stop, [&](BYTE* dst, UINT32 frames) {
                    source.Tick();
                    stream.Read(dst, frames);
                });
            }
            hdmi.Close();
            SleepUnlessStopped(2000);
        }
        CoUninitialize();
    });

    const ULONGLONG start = GetTickCount64();
    ULONGLONG lastStats = start;
    while (!g_stop) {
        Sleep(200);
        const ULONGLONG now = GetTickCount64();
        if (o.seconds > 0 && now - start >= (ULONGLONG)(o.seconds * 1000)) g_stop = true;
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
            std::string sourceStats = source.TakeStats();
            Log("AC3 frames %llu (failed %llu), encode avg %.2f / max %.2f ms%s%s", (unsigned long long)bursts,
                (unsigned long long)failures, avgMs, maxMs, sourceStats.empty() ? "" : "; ", sourceStats.c_str());
        }
    }
    audio.join();
    if (dump) fclose(dump);
    Log("Stopped");
    return 0;
}

//=============================================================================
// Windows service (tools\install-service.ps1): "run" under the service control manager.

constexpr wchar_t kServiceName[] = L"win51svc";  // the driver's own service is "Win51"
Options g_serviceOptions;
SERVICE_STATUS_HANDLE g_serviceStatus = nullptr;

void ReportServiceStatus(DWORD state, DWORD exitCode = 0) {
    static DWORD checkPoint = 1;
    SERVICE_STATUS s = {};
    s.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    s.dwCurrentState = state;
    s.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    s.dwWin32ExitCode = exitCode ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
    s.dwServiceSpecificExitCode = exitCode;
    s.dwWaitHint = state == SERVICE_STOP_PENDING ? 5000 : 0;
    s.dwCheckPoint = state == SERVICE_STOP_PENDING ? checkPoint++ : 0;
    SetServiceStatus(g_serviceStatus, &s);
}

DWORD WINAPI ServiceControl(DWORD control, DWORD, void*, void*) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            ReportServiceStatus(SERVICE_STOP_PENDING);
            g_stop = true;
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD, wchar_t**) {
    g_serviceStatus = RegisterServiceCtrlHandlerExW(kServiceName, ServiceControl, nullptr);
    if (!g_serviceStatus) return;
    ReportServiceStatus(SERVICE_RUNNING);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int rc;
    {
        LoopbackSource source(g_serviceOptions.in, g_serviceOptions.targetMs);
        rc = RunPipeline(source, g_serviceOptions);
    }
    CoUninitialize();
    // A non-zero code lets the service's recovery actions restart it.
    ReportServiceStatus(SERVICE_STOPPED, (DWORD)rc);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Options o;
    if (!ParseOptions(argc, argv, o)) {
        PrintUsage();
        return 2;
    }
    if (!o.logPath.empty()) LogOpenFile(o.logPath);
    if (o.command != L"service") SetConsoleCtrlHandler(CtrlHandler, TRUE);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    int rc = 2;
    if (o.command == L"list") {
        rc = ListCommand();
    } else if (o.command == L"tone") {
        ToneSource source(o.onlyChannel);
        rc = RunPipeline(source, o);
    } else if (o.command == L"run") {
        LoopbackSource source(o.in, o.targetMs);
        rc = RunPipeline(source, o);
    } else if (o.command == L"service") {
        g_serviceOptions = o;
        const SERVICE_TABLE_ENTRYW services[] = {{const_cast<wchar_t*>(kServiceName), ServiceMain}, {nullptr, nullptr}};
        if (StartServiceCtrlDispatcherW(services)) {
            rc = 0;
        } else {
            Log("Not started by the service control manager (%s); use \"run\" from a console",
                HrToString(HRESULT_FROM_WIN32(GetLastError())).c_str());
        }
    } else {
        PrintUsage();
    }

    CoUninitialize();
    return rc;
}
