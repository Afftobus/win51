#include "control_pipe.h"

#include <objbase.h>
#include <sddl.h>

#include <vector>

#include "endpoints.h"
#include "log.h"
#include "win51_control.h"
#include "win51_version.h"

namespace w51 {
namespace {

// SYSTEM and administrators: full access; interactive users (the tray icon): read and write.
constexpr wchar_t kPipeSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";
constexpr DWORD kClientTimeoutMs = 2000;

// One overlapped read or write with a timeout, so a client that stops responding cannot block the server.
bool Transfer(HANDLE pipe, OVERLAPPED& ov, bool write, void* buffer, DWORD size, DWORD& done) {
    done = 0;
    ResetEvent(ov.hEvent);
    const BOOL ok = write ? WriteFile(pipe, buffer, size, nullptr, &ov) : ReadFile(pipe, buffer, size, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(ov.hEvent, kClientTimeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &done, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &done, FALSE) != FALSE;
}

const char* StateName(PipelineState state) {
    switch (state) {
        case PipelineState::Streaming: return "streaming";
        case PipelineState::Waiting: return "waiting";
        case PipelineState::Paused: return "paused";
        default: return "starting";
    }
}

const char* ReasonName(WaitReason reason) {
    switch (reason) {
        case WaitReason::NoOutput: return "no-output";
        case WaitReason::OutputBusy: return "output-busy";
        case WaitReason::OutputError: return "output-error";
        default: return "";
    }
}

}  // namespace

ControlPipe::ControlPipe(Pipeline& pipeline)
    : pipeline_(pipeline), stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

ControlPipe::~ControlPipe() {
    Stop();
    CloseHandle(stopEvent_);
}

void ControlPipe::Start() { thread_ = std::thread([this] { Serve(); }); }

void ControlPipe::Stop() {
    SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
}

void ControlPipe::Serve() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // "outputs" enumerates audio endpoints
    SECURITY_ATTRIBUTES sa = {sizeof(sa)};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor,
                                                              nullptr)) {
        Log("Control pipe: bad security descriptor: %s", HrToString(HRESULT_FROM_WIN32(GetLastError())).c_str());
        CoUninitialize();
        return;
    }
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool failureLogged = false;

    while (WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT) {
        // FIRST_PIPE_INSTANCE: fail rather than share the name with a pipe someone else created first.
        const DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
        const DWORD pipeMode = PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
        HANDLE pipe = CreateNamedPipeW(kControlPipeName, openMode, pipeMode, 1, kControlPipeBufferBytes,
                                       kControlPipeBufferBytes, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (!failureLogged) Log("Control pipe: %s", HrToString(HRESULT_FROM_WIN32(GetLastError())).c_str());
            failureLogged = true;
            WaitForSingleObject(stopEvent_, 1000);
            continue;
        }
        failureLogged = false;

        ResetEvent(ov.hEvent);
        DWORD error = ConnectNamedPipe(pipe, &ov) ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_IO_PENDING) {
            const HANDLE events[2] = {ov.hEvent, stopEvent_};
            DWORD done = 0;
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
                error = GetOverlappedResult(pipe, &ov, &done, FALSE) ? ERROR_SUCCESS : GetLastError();
            } else {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &done, TRUE);
                error = ERROR_OPERATION_ABORTED;
            }
        } else if (error == ERROR_PIPE_CONNECTED) {
            error = ERROR_SUCCESS;
        }
        if (error == ERROR_SUCCESS) ServeClient(pipe, ov);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    CloseHandle(ov.hEvent);
    LocalFree(sa.lpSecurityDescriptor);
    CoUninitialize();
}

void ControlPipe::ServeClient(HANDLE pipe, OVERLAPPED& ov) {
    std::vector<char> request(4096);
    DWORD read = 0;
    if (!Transfer(pipe, ov, false, request.data(), (DWORD)request.size(), read)) return;
    std::string reply = Handle(std::string(request.data(), read));
    DWORD written = 0;
    if (!Transfer(pipe, ov, true, reply.data(), (DWORD)reply.size(), written)) return;
    // Wait for the client to read the reply and close its end; disconnecting first would discard it.
    char drain;
    Transfer(pipe, ov, false, &drain, 1, read);
}

std::string ControlPipe::Handle(const std::string& request) {
    std::string command = request, argument;
    while (!command.empty() && (command.back() == '\n' || command.back() == '\r')) command.pop_back();
    if (const size_t space = command.find(' '); space != std::string::npos) {
        argument = command.substr(space + 1);
        command.resize(space);
    }

    if (command == "status") {
        const PipelineStatus s = pipeline_.Status();
        std::string reply;
        reply += "state=" + std::string(StateName(s.state)) + "\n";
        reply += "reason=" + std::string(ReasonName(s.reason)) + "\n";
        reply += "output=" + Narrow(s.output) + "\n";
        reply += "setting=" + Narrow(s.outputSetting) + "\n";
        reply += std::string("capture=") + (s.captureConnected ? "1" : "0") + "\n";
        reply += std::string("playing=") + (s.playing ? "1" : "0") + "\n";
        reply += "detail=" + s.detail + "\n";
        reply += "version=" WIN51_VERSION_STR "\n";
        return reply;
    }
    if (command == "outputs") return Outputs();
    if (command == "pause" || command == "resume") {
        pipeline_.SetPaused(command == "pause");
        return "ok\n";
    }
    if (command == "output") {
        pipeline_.SetOutput(Widen(argument));
        return "ok\n";
    }
    return "error unknown command\n";
}

std::string ControlPipe::Outputs() {
    // The output in use is listed even if it does not answer the format query while it is held exclusively.
    const std::wstring current = pipeline_.Status().output;
    std::string reply;
    for (auto& ep : ListRenderEndpoints(DEVICE_STATE_ACTIVE)) {
        if (ContainsNoCase(ep.name, kWin51EndpointName)) continue;
        if (ep.name != current && !SupportsExclusiveAc3(ep.device.Get())) continue;
        const UINT formFactor = EndpointFormFactorOf(ep.device.Get());
        const char* kind = formFactor == DigitalAudioDisplayDevice ? "hdmi" : formFactor == SPDIF ? "spdif" : "other";
        reply += Narrow(ep.name) + "\t" + kind + "\n";
    }
    return reply;
}

}  // namespace w51
