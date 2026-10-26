// win51svc: encodes the Win51 virtual 5.1 output to AC3 in real time and plays it to HDMI as an
// IEC 61937 bitstream (the same signal a player sends with "AC3 passthrough" enabled).
//
//   win51svc list                 render endpoints and which of them accept AC3 passthrough
//   win51svc tone [options]       channel identification test signal, no driver needed
//   win51svc run  [options]       encode what Windows plays to the Win51 endpoint
//   win51svc service [options]    the same as run, started by the service control manager
//   win51svc ctl <request>        talk to the running service (see shared\win51_control.h)
//   win51svc driver-install <inf> / driver-remove    create/update or remove the Win51 device (setup.ps1)

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "control_pipe.h"
#include "device_setup.h"
#include "endpoints.h"
#include "log.h"
#include "pcm_source.h"
#include "pipeline.h"
#include "settings.h"
#include "win51_control.h"

using namespace w51;

namespace {

std::atomic<bool> g_stop{false};

struct Options {
    std::wstring command;
    std::wstring in = kWin51EndpointName;  // endpoint captured by "run"
    std::wstring out;                       // empty: the registry setting, else automatic; "none": offline
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
          "                                 [--only fl|fr|fc|lfe|sl|sr]   (tone: one channel only)\n"
          "       win51svc ctl status|outputs|pause|resume|output [<name>]\n"
          "       win51svc driver-install <inf> | driver-remove\n",
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

// Runs the pipeline on a source; "run" and "service" also serve the control pipe.
int RunPipeline(PcmSource& source, const Options& o, bool controllable) {
    PipelineConfig config;
    config.offline = o.out == L"none";
    config.out = config.offline ? std::wstring() : o.out;
    if (controllable && o.out.empty()) config.out = LoadOutputSetting();
    config.persistOutput = o.command == L"service";
    config.bitrateKbps = o.bitrateKbps;
    config.periodMs = o.periodMs;
    config.seconds = o.seconds;
    config.dumpPath = o.dumpPath;

    Pipeline pipeline(source, config);
    ControlPipe control(pipeline);
    if (controllable && !config.offline) control.Start();
    const int rc = pipeline.Run(g_stop);
    control.Stop();
    return rc;
}

// "win51svc ctl <request>": sends the request to the running service and prints the reply.
int ControlCommand(int argc, wchar_t** argv) {
    std::wstring request;
    for (int i = 2; i < argc; ++i) request += (i > 2 ? L" " : L"") + std::wstring(argv[i]);
    std::string reply;
    if (!CallControlPipe(Narrow(request), reply)) {
        fprintf(stderr, "win51svc is not running (%s)\n", HrToString(HRESULT_FROM_WIN32(GetLastError())).c_str());
        return 1;
    }
    fputs(reply.c_str(), stdout);
    return reply.rfind("error", 0) == 0 ? 1 : 0;
}

// "driver-install <inf>" / "driver-remove": 0 = done, 3010 = done, reboot required, 1 = failed.
int DriverCommand(const std::wstring& command, const wchar_t* inf) {
    bool reboot = false;
    const HRESULT hr = command == L"driver-install" ? InstallRootDevice(inf, kWin51HardwareId, reboot)
                                                     : RemoveRootDevices(kWin51HardwareId, reboot);
    if (FAILED(hr)) {
        Log("%s failed: %s", Narrow(command).c_str(), HrToString(hr).c_str());
        return 1;
    }
    return reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : 0;
}

//=============================================================================
// Windows service (installed by setup.ps1): "run" under the service control manager.

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
        rc = RunPipeline(source, g_serviceOptions, true);
    }
    CoUninitialize();
    // A non-zero code lets the service's recovery actions restart it.
    ReportServiceStatus(SERVICE_STOPPED, (DWORD)rc);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 2 && wcscmp(argv[1], L"ctl") == 0) return ControlCommand(argc, argv);
    if (argc == 3 && wcscmp(argv[1], L"driver-install") == 0) return DriverCommand(argv[1], argv[2]);
    if (argc == 2 && wcscmp(argv[1], L"driver-remove") == 0) return DriverCommand(argv[1], nullptr);

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
        rc = RunPipeline(source, o, false);
    } else if (o.command == L"run") {
        LoopbackSource source(o.in, o.targetMs);
        rc = RunPipeline(source, o, true);
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
