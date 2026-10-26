// Sources of 5.1 PCM for the AC3 encoder.
#pragma once

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "ac3_encoder.h"

namespace w51 {

using Microsoft::WRL::ComPtr;

// Channel order of every PCM frame handed to the encoder: FL FR FC LFE SL SR.
constexpr uint64_t kMask51Side = 0x60F;
constexpr int kChannels = Ac3Encoder::kChannels;
constexpr int kFrameSamples = Ac3Encoder::kFrameSamples;
constexpr int kSampleRate = 48000;

// Interleaved 6-channel float FIFO.
class PcmFifo {
public:
    explicit PcmFifo(size_t capacityFrames) : buffer_(capacityFrames * kChannels), capacity_(capacityFrames) {}

    size_t Size() const { return size_; }
    // Returns how many of the oldest frames had to be discarded to make room.
    size_t Push(const float* frames, size_t count);
    void Pop(float* dst, size_t count);   // count <= Size()
    void Drop(size_t count);
    void Clear() { head_ = size_ = 0; }

private:
    std::vector<float> buffer_;
    size_t capacity_;
    size_t head_ = 0;  // index of the oldest frame
    size_t size_ = 0;
};

class PcmSource {
public:
    virtual ~PcmSource() = default;
    // Called on the output thread whenever the output (re)starts: drop anything buffered for the old one.
    virtual void Reset() {}
    // Called on the output thread once per output period before any Pull().
    virtual void Tick() {}
    // Produces exactly kFrameSamples frames (zeros when there is nothing to play).
    virtual void Pull(float* dst) = 0;
    // Appends a human-readable summary of the last interval and resets interval counters.
    virtual std::string TakeStats() { return {}; }
};

// Channel identification signal: noise bursts on FL, FC, FR, SR, SL and a 35-90 Hz sweep on LFE.
class ToneSource : public PcmSource {
public:
    // onlyChannel: encoder channel index (0..5) to repeat, or -1 to walk around all six.
    explicit ToneSource(int onlyChannel = -1) : onlyChannel_(onlyChannel) {}
    void Pull(float* dst) override;

private:
    int onlyChannel_;
    uint64_t frame_ = 0;
    uint32_t noise_ = 0x12345678;
    double lfePhase_ = 0;
    int lastSegment_ = -1;
};

// Captures the mix of a render endpoint (the Win51 virtual 5.1 output) through WASAPI loopback and
// keeps it in a jitter buffer. The two clocks are independent, so the buffer is held around the
// target level by dropping or repeating single frames.
class LoopbackSource : public PcmSource {
public:
    LoopbackSource(std::wstring endpointName, double targetMs);
    ~LoopbackSource() override;

    void Reset() override;
    void Tick() override;
    void Pull(float* dst) override;
    std::string TakeStats() override;

private:
    HRESULT Open();
    void Close();
    void Append(const BYTE* data, UINT32 frames, DWORD flags);
    void Take(float* dst, size_t count);
    void Skip(size_t count);

    std::wstring endpointName_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    WAVEFORMATEX* mixFormat_ = nullptr;
    bool mixIsFloat_ = false;
    int channelMap_[32] = {};  // source channel -> encoder channel, -1 = dropped
    ULONGLONG nextOpenTick_ = 0;
    bool openFailureLogged_ = false;
    std::atomic<bool> connected_{false};

    PcmFifo fifo_;
    size_t leadIn_ = 0;  // frames of silence played ahead of the FIFO contents, see Pull()
    bool discardPending_ = false;
    std::vector<float> scratch_;
    size_t targetFrames_;
    size_t toleranceFrames_;
    bool running_ = false;
    double smoothedFill_ = 0;

    std::mutex statsMutex_;
    struct {
        uint64_t capturedFrames = 0, silentFrames = 0, underruns = 0, drops = 0, repeats = 0, resyncs = 0, overflow = 0;
        size_t fillMin = SIZE_MAX, fillMax = 0;
        uint32_t discontinuities = 0;
    } stats_;
};

}  // namespace w51
