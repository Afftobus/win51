#include "pcm_source.h"

#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "endpoints.h"
#include "log.h"

namespace w51 {

//=============================================================================
// PcmFifo

size_t PcmFifo::Push(const float* frames, size_t count) {
    size_t discarded = 0;
    if (count > capacity_) {
        discarded += count - capacity_;
        frames += (count - capacity_) * kChannels;
        count = capacity_;
    }
    if (size_ + count > capacity_) {
        size_t excess = size_ + count - capacity_;
        Drop(excess);
        discarded += excess;
    }
    size_t tail = (head_ + size_) % capacity_;
    size_t first = std::min(count, capacity_ - tail);
    memcpy(&buffer_[tail * kChannels], frames, first * kChannels * sizeof(float));
    memcpy(&buffer_[0], frames + first * kChannels, (count - first) * kChannels * sizeof(float));
    size_ += count;
    return discarded;
}

void PcmFifo::Pop(float* dst, size_t count) {
    count = std::min(count, size_);
    size_t first = std::min(count, capacity_ - head_);
    memcpy(dst, &buffer_[head_ * kChannels], first * kChannels * sizeof(float));
    memcpy(dst + first * kChannels, &buffer_[0], (count - first) * kChannels * sizeof(float));
    Drop(count);
}

void PcmFifo::Drop(size_t count) {
    count = std::min(count, size_);
    head_ = (head_ + count) % capacity_;
    size_ -= count;
}

//=============================================================================
// ToneSource

void ToneSource::Pull(float* dst) {
    // Walk around the room like a receiver's test tone: FL, C, FR, SR, SL, then the subwoofer.
    static const int kOrder[6] = {0, 2, 1, 5, 4, 3};
    static const char* const kNames[6] = {"front left",    "center",        "front right",
                                          "surround right", "surround left", "LFE (subwoofer)"};
    const uint64_t burst = kSampleRate * 12 / 10;           // 1.2 s of signal
    const uint64_t segment = burst + kSampleRate * 6 / 10;  // + 0.6 s of silence
    const uint64_t fade = kSampleRate / 200;                // 5 ms ramps, no clicks

    memset(dst, 0, sizeof(float) * kFrameSamples * kChannels);
    for (int i = 0; i < kFrameSamples; ++i, ++frame_) {
        int seg = (int)((frame_ / segment) % 6);
        if (onlyChannel_ >= 0) seg = (int)(std::find(kOrder, kOrder + 6, onlyChannel_) - kOrder);
        if (frame_ % segment == 0 || seg != lastSegment_) {
            lastSegment_ = seg;
            Log("Test signal: %s", kNames[seg]);
        }
        const uint64_t pos = frame_ % segment;
        if (pos >= burst) continue;

        float gain = 1.0f;
        if (pos < fade) gain = (float)pos / fade;
        else if (burst - pos < fade) gain = (float)(burst - pos) / fade;

        const int ch = kOrder[seg];
        float v;
        if (ch == 3) {
            // Sweep 35 -> 90 Hz across the burst: far easier to hear on a subwoofer than a steady tone.
            const double hz = 35.0 + 55.0 * (double)pos / (double)burst;
            lfePhase_ += 6.283185307179586 * hz / kSampleRate;
            if (lfePhase_ > 6.283185307179586) lfePhase_ -= 6.283185307179586;
            v = 0.7f * (float)sin(lfePhase_);  // -3 dBFS peak; receivers add +10 dB to LFE
        } else {
            noise_ ^= noise_ << 13;
            noise_ ^= noise_ >> 17;
            noise_ ^= noise_ << 5;
            v = 0.25f * ((int32_t)noise_ / 2147483648.0f);
        }
        dst[i * kChannels + ch] = v * gain;
    }
}

//=============================================================================
// LoopbackSource

LoopbackSource::LoopbackSource(std::wstring endpointName, double targetMs)
    : endpointName_(std::move(endpointName)),
      fifo_(kSampleRate),
      targetFrames_(std::max<size_t>(kFrameSamples, (size_t)(targetMs * kSampleRate / 1000))),
      // +/-1 ms. The level seen by Pull() swings by up to 8 ms as 10 ms packets meet 32 ms frames, so a start
      // can land up to 4 ms off the target on average; the smoothed level walks it back one frame per pull.
      toleranceFrames_(kSampleRate / 1000) {
    Log("Loopback jitter buffer target %zu frames (%.1f ms)", targetFrames_, targetFrames_ * 1000.0 / kSampleRate);
}

LoopbackSource::~LoopbackSource() { Close(); }

void LoopbackSource::Close() {
    connected_ = false;
    if (client_) client_->Stop();
    capture_.Reset();
    client_.Reset();
    if (mixFormat_) {
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
    }
}

HRESULT LoopbackSource::Open() {
    Close();
    EndpointInfo ep;
    HRESULT hr = FindRenderEndpoint(endpointName_, ep);
    if (FAILED(hr)) {
        if (!openFailureLogged_) Log("Loopback: no active render endpoint matches \"%s\", waiting for it", Narrow(endpointName_).c_str());
        openFailureLogged_ = true;
        return hr;
    }
    W51_RETURN_IF_FAILED(ep.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client_.GetAddressOf()));
    W51_RETURN_IF_FAILED(client_->GetMixFormat(&mixFormat_));

    const WAVEFORMATEX* f = mixFormat_;
    DWORD mask = 0;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        mixIsFloat_ = IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && f->wBitsPerSample == 32;
        mask = x->dwChannelMask;
    } else {
        mixIsFloat_ = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && f->wBitsPerSample == 32;
    }
    if (!mixIsFloat_ || f->nSamplesPerSec != kSampleRate) {
        Log("Loopback: unsupported mix format (tag 0x%x, %u bit, %lu Hz)", f->wFormatTag, f->wBitsPerSample,
            f->nSamplesPerSec);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    if (mask == 0) mask = f->nChannels == 6 ? KSAUDIO_SPEAKER_5POINT1 : f->nChannels == 2 ? KSAUDIO_SPEAKER_STEREO : 0;

    // Map the mix channels (in mask bit order) onto FL FR FC LFE SL SR; back surrounds fold into side.
    int ch = 0;
    for (int bit = 0; bit < 32 && ch < f->nChannels; ++bit) {
        const DWORD speaker = 1u << bit;
        if (!(mask & speaker)) continue;
        int target = -1;
        switch (speaker) {
            case SPEAKER_FRONT_LEFT: target = 0; break;
            case SPEAKER_FRONT_RIGHT: target = 1; break;
            case SPEAKER_FRONT_CENTER: target = 2; break;
            case SPEAKER_LOW_FREQUENCY: target = 3; break;
            case SPEAKER_SIDE_LEFT: case SPEAKER_BACK_LEFT: target = 4; break;
            case SPEAKER_SIDE_RIGHT: case SPEAKER_BACK_RIGHT: target = 5; break;
        }
        channelMap_[ch++] = target;
    }
    for (; ch < 32; ++ch) channelMap_[ch] = -1;

    W51_RETURN_IF_FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                             1000000 /* 100 ms */, 0, mixFormat_, nullptr));
    W51_RETURN_IF_FAILED(client_->GetService(IID_PPV_ARGS(capture_.GetAddressOf())));
    W51_RETURN_IF_FAILED(client_->Start());
    Log("Loopback capture from \"%s\": %u ch float, mask 0x%lx", Narrow(ep.name).c_str(), f->nChannels, mask);
    openFailureLogged_ = false;
    connected_ = true;
    return S_OK;
}

void LoopbackSource::Reset() {
    // While the output was down nobody drained the capture, so what is queued there is stale.
    fifo_.Clear();
    leadIn_ = 0;
    running_ = false;
    discardPending_ = true;
}

void LoopbackSource::Tick() {
    if (!capture_) {
        if (GetTickCount64() < nextOpenTick_) return;
        if (FAILED(Open())) {
            Close();
            nextOpenTick_ = GetTickCount64() + 2000;
            return;
        }
    }
    HRESULT hr;
    UINT32 packet = 0;
    while (SUCCEEDED(hr = capture_->GetNextPacketSize(&packet)) && packet > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;
        if (!discardPending_) Append(data, frames, flags);
        capture_->ReleaseBuffer(frames);
    }
    discardPending_ = false;
    if (FAILED(hr)) {
        Log("Loopback capture lost: %s", HrToString(hr).c_str());
        Close();
        nextOpenTick_ = GetTickCount64() + 1000;
    }
}

void LoopbackSource::Append(const BYTE* data, UINT32 frames, DWORD flags) {
    scratch_.assign((size_t)frames * kChannels, 0.0f);
    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
        const float* in = reinterpret_cast<const float*>(data);
        const int inChannels = mixFormat_->nChannels;
        for (UINT32 i = 0; i < frames; ++i) {
            for (int c = 0; c < inChannels && c < 32; ++c) {
                const int t = channelMap_[c];
                if (t >= 0) scratch_[(size_t)i * kChannels + t] += in[(size_t)i * inChannels + c];
            }
        }
    }
    const size_t lost = fifo_.Push(scratch_.data(), frames);
    lastPacketTick_ = GetTickCount64();

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.capturedFrames += frames;
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) stats_.silentFrames += frames;
    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) stats_.discontinuities++;
    stats_.overflow += lost;
}

// Copies count frames from the front: the lead-in silence first, then captured audio.
void LoopbackSource::Take(float* dst, size_t count) {
    const size_t silence = std::min(count, leadIn_);
    memset(dst, 0, sizeof(float) * silence * kChannels);
    leadIn_ -= silence;
    fifo_.Pop(dst + silence * kChannels, count - silence);
}

// Discards count frames from the front, lead-in silence first.
void LoopbackSource::Skip(size_t count) {
    const size_t silence = std::min(count, leadIn_);
    leadIn_ -= silence;
    fifo_.Drop(count - silence);
}

void LoopbackSource::Pull(float* dst) {
    if (running_ && fifo_.Size() + leadIn_ < (size_t)kFrameSamples) {
        // The packet that is due may land a moment after this period's Tick(): the two streams run off
        // unrelated clocks at the same 10 ms period. Give it up to 2 ms before calling this an underrun.
        LARGE_INTEGER now, frequency;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&now);
        const LONGLONG deadline = now.QuadPart + frequency.QuadPart / 500;
        do {
            SwitchToThread();
            Tick();
            QueryPerformanceCounter(&now);
        } while (fifo_.Size() + leadIn_ < (size_t)kFrameSamples && now.QuadPart < deadline);
    }

    size_t fill = fifo_.Size() + leadIn_;
    if (!running_) {
        if (fill == 0) {
            memset(dst, 0, sizeof(float) * kFrameSamples * kChannels);
            return;
        }
        // Playback (re)starts: put silence in front of it so that it starts exactly at the target level.
        // Its first frames are delayed rather than lost; only a burst longer than the target loses some.
        if (fill < targetFrames_) leadIn_ += targetFrames_ - fill;
        else Skip(fill - targetFrames_);
        fill = targetFrames_;
        smoothedFill_ = (double)fill;
        running_ = true;
    } else if (fill > targetFrames_ + kFrameSamples) {
        // The output fell behind by more than a frame (it stalled): catch up at once rather than trim
        // one frame per AC3 frame for minutes.
        Skip(fill - targetFrames_);
        fill = targetFrames_;
        smoothedFill_ = (double)fill;
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.resyncs++;
    }
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.fillMin = std::min(stats_.fillMin, fill);
        stats_.fillMax = std::max(stats_.fillMax, fill);
    }

    if (fill < (size_t)kFrameSamples) {
        // Underrun (playback stopped or the capture stalled): play what is left, then start over.
        memset(dst, 0, sizeof(float) * kFrameSamples * kChannels);
        Take(dst, fill);
        running_ = false;
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.underruns++;
        return;
    }

    smoothedFill_ += ((double)fill - smoothedFill_) * 0.02;
    if (smoothedFill_ > (double)(targetFrames_ + toleranceFrames_) && fill > (size_t)kFrameSamples) {
        Skip(1);
        smoothedFill_ -= 1;
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.drops++;
    } else if (smoothedFill_ < (double)(targetFrames_ - toleranceFrames_)) {
        // Take one frame less and repeat the last one.
        Take(dst, kFrameSamples - 1);
        memcpy(dst + (size_t)(kFrameSamples - 1) * kChannels, dst + (size_t)(kFrameSamples - 2) * kChannels,
               sizeof(float) * kChannels);
        smoothedFill_ += 1;
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.repeats++;
        return;
    }
    Take(dst, kFrameSamples);
}

std::string LoopbackSource::TakeStats() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "capture %llu frames (%llu silent), buffer %zu..%zu frames, underruns %llu, drop/repeat %llu/%llu, "
             "resyncs %llu, overflow %llu, discontinuities %u%s",
             (unsigned long long)stats_.capturedFrames, (unsigned long long)stats_.silentFrames,
             stats_.fillMin == SIZE_MAX ? 0 : stats_.fillMin, stats_.fillMax, (unsigned long long)stats_.underruns,
             (unsigned long long)stats_.drops, (unsigned long long)stats_.repeats, (unsigned long long)stats_.resyncs,
             (unsigned long long)stats_.overflow, stats_.discontinuities, connected_ ? "" : " [no capture]");
    stats_ = {};
    return buf;
}

}  // namespace w51
