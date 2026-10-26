// Exclusive-mode, event-driven WASAPI renderer for an IEC 61937 (AC3 passthrough) stream.
#pragma once

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>

namespace w51 {

using Microsoft::WRL::ComPtr;

class HdmiOutput {
public:
    // Must write exactly `frames` frames (4 bytes each) of IEC 61937 data to dst.
    using FillFn = std::function<void(BYTE* dst, UINT32 frames)>;

    HdmiOutput() = default;
    HdmiOutput(const HdmiOutput&) = delete;
    HdmiOutput& operator=(const HdmiOutput&) = delete;
    ~HdmiOutput();

    // requestedPeriod in 100 ns units; 0 selects the device's minimum period.
    HRESULT Open(IMMDevice* device, REFERENCE_TIME requestedPeriod);
    void Close();

    // Pre-fills one buffer, starts the stream and services it until `stop` is set or a stream error
    // occurs (e.g. AUDCLNT_E_DEVICE_INVALIDATED when the TV is switched off). Runs on the calling thread.
    HRESULT Run(const std::atomic<bool>& stop, const FillFn& fill);

    UINT32 BufferFrames() const { return bufferFrames_; }
    REFERENCE_TIME Period() const { return period_; }

private:
    HRESULT Initialize(REFERENCE_TIME period);

    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    HANDLE event_ = nullptr;
    UINT32 bufferFrames_ = 0;
    REFERENCE_TIME period_ = 0;
    WAVEFORMATEXTENSIBLE format_ = {};
};

}  // namespace w51
