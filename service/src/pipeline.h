// The encoding pipeline: PCM source -> AC3 -> IEC 61937 -> HDMI, with its live status and controls.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

#include "pcm_source.h"

namespace w51 {

struct PipelineConfig {
    std::wstring out;            // output name fragment; empty = the HDMI/DisplayPort output that accepts AC3
    bool offline = false;        // no output: encode `seconds` of audio as fast as possible
    bool persistOutput = false;  // SetOutput() also stores the choice in the registry
    int bitrateKbps = 640;
    double periodMs = 10.0;      // HDMI exclusive-mode period, 0 = device minimum
    double seconds = 0;          // 0 = until stopped
    std::wstring dumpPath;       // optional copy of the IEC 61937 stream (s16le stereo)
};

enum class PipelineState { Starting, Streaming, Waiting, Paused };

// Why the pipeline is waiting for its output.
enum class WaitReason { None, NoOutput, OutputBusy, OutputError };

struct PipelineStatus {
    PipelineState state = PipelineState::Starting;
    WaitReason reason = WaitReason::None;
    std::wstring output;         // the output in use, or the one tried last
    std::wstring outputSetting;  // configured output, empty = automatic
    std::string detail;          // last error
    bool captureConnected = false;
    bool playing = false;
};

class Pipeline {
public:
    Pipeline(PcmSource& source, PipelineConfig config);

    // Runs until `stop` is set or --seconds pass; returns the process exit code.
    int Run(std::atomic<bool>& stop);

    // Thread-safe controls for the control pipe.
    void SetPaused(bool paused);               // paused = the HDMI output is released for other programs
    void SetOutput(const std::wstring& output);
    PipelineStatus Status();

private:
    void SetState(PipelineState state, WaitReason reason, const std::wstring& output, const std::string& detail);
    // Sleeps up to ms milliseconds; returns early when stopping or when the output has to be reselected.
    void Wait(const std::atomic<bool>& stop, DWORD ms);

    PcmSource& source_;
    const PipelineConfig config_;
    std::atomic<bool> paused_{false};
    std::atomic<bool> interrupt_{false};  // leave the current output: paused, or another output chosen

    std::mutex statusMutex_;
    PipelineStatus status_;
};

}  // namespace w51
