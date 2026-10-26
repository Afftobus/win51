// win51tray: notification area icon for win51svc. Shows what the service is doing and lets the user choose
// the output, release the HDMI output for other programs and open the log folder. It talks to the service
// through the control pipe (shared\win51_control.h) from a worker thread, so the UI never blocks on it.

#include <windows.h>
#include <commctrl.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "resource.h"
#include "win51_control.h"

namespace {

using w51::CallControlPipe;

constexpr UINT WM_TRAY = WM_APP + 1;    // notification icon callback
constexpr UINT WM_STATUS = WM_APP + 2;  // worker -> window: new Snapshot* in lParam
constexpr UINT kIconId = 1;
constexpr DWORD kPollMs = 1000;
constexpr ULONGLONG kOutputsRefreshMs = 5000;  // listing outputs queries every audio device

enum : UINT { ID_RELEASE = 100, ID_SOUND_SETTINGS, ID_LOG_FOLDER, ID_EXIT, ID_OUTPUT_AUTO = 200, ID_OUTPUT_FIRST };

//=============================================================================
// Text

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// Replaces the first "%s" in pattern.
std::wstring Format(const wchar_t* pattern, const std::wstring& value) {
    std::wstring s = pattern;
    if (const size_t at = s.find(L"%s"); at != std::wstring::npos) s.replace(at, 2, value);
    return s;
}

struct Strings {
    const wchar_t* streaming;      // %s = output
    const wchar_t* soundPlaying;
    const wchar_t* silence;
    const wchar_t* waitingHdmi;
    const wchar_t* waitingOutput;  // %s = output setting
    const wchar_t* outputBusy;     // %s = output
    const wchar_t* outputError;    // %s = error
    const wchar_t* released;
    const wchar_t* starting;
    const wchar_t* noDevice;
    const wchar_t* notRunning;
    const wchar_t* menuOutput;
    const wchar_t* menuAutomatic;
    const wchar_t* menuNotFound;   // %s = output setting
    const wchar_t* menuRelease;
    const wchar_t* menuSoundSettings;
    const wchar_t* menuLogFolder;
    const wchar_t* menuExit;
};

const Strings kEnglish = {
    L"Streaming to %s",
    L"Sound is playing",
    L"Silence",
    L"Waiting for an HDMI output",
    L"Waiting for output \"%s\"",
    L"\"%s\" is used by another program",
    L"Output error: %s",
    L"HDMI output released",
    L"Starting...",
    L"The Win51 sound device is missing",
    L"The Win51 service is not running",
    L"Output",
    L"Automatic (HDMI)",
    L"%s (not connected)",
    L"Release HDMI output",
    L"Sound settings...",
    L"Open log folder",
    L"Close",
};

const Strings kRussian = {
    L"Вывод на %s",
    L"Звук идёт",
    L"Тишина",
    L"Ожидание HDMI-выхода",
    L"Ожидание выхода «%s»",
    L"«%s» занят другой программой",
    L"Ошибка выхода: %s",
    L"HDMI-выход освобождён",
    L"Запуск…",
    L"Звуковое устройство Win51 не найдено",
    L"Служба Win51 не запущена",
    L"Аудиовыход",
    L"Автоматически (HDMI)",
    L"%s (не подключён)",
    L"Освободить HDMI-выход",
    L"Настройки звука…",
    L"Открыть папку с журналами",
    L"Закрыть",
};

const Strings& S() {
    static const Strings& strings =
        PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? kRussian : kEnglish;
    return strings;
}

//=============================================================================
// Service state

struct Snapshot {
    bool running = false;                       // the control pipe answered
    std::map<std::string, std::string> status;  // "status" reply, key=value
    std::vector<std::wstring> outputs;          // "outputs" reply, names only

    std::wstring Get(const char* key) const {
        const auto it = status.find(key);
        return it == status.end() ? std::wstring() : Widen(it->second);
    }
};

// Polls the service once a second and sends it the user's commands, off the UI thread.
class Worker {
public:
    explicit Worker(HWND window) : window_(window), thread_([this] { Loop(); }) {}
    ~Worker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

    void Send(std::string command) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(std::move(command));
        }
        wake_.notify_one();
    }

private:
    void Loop() {
        std::vector<std::wstring> outputs;
        ULONGLONG outputsTime = 0;
        for (bool first = true;; first = false) {
            std::vector<std::string> commands;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (!first) {
                    wake_.wait_for(lock, std::chrono::milliseconds(kPollMs),
                                   [this] { return quit_ || !commands_.empty(); });
                }
                if (quit_) return;
                commands.swap(commands_);
            }
            std::string reply;
            for (const auto& command : commands) CallControlPipe(command, reply);

            auto snapshot = std::make_unique<Snapshot>();
            snapshot->running = CallControlPipe("status", reply);
            if (snapshot->running) {
                ParseStatus(reply, snapshot->status);
                const ULONGLONG now = GetTickCount64();
                if (!commands.empty() || now - outputsTime >= kOutputsRefreshMs) {
                    if (CallControlPipe("outputs", reply)) outputs = ParseOutputs(reply);
                    outputsTime = now;
                }
                snapshot->outputs = outputs;
            }
            if (PostMessageW(window_, WM_STATUS, 0, (LPARAM)snapshot.get())) snapshot.release();
        }
    }

    static void ParseStatus(const std::string& reply, std::map<std::string, std::string>& status) {
        size_t start = 0;
        while (start < reply.size()) {
            size_t end = reply.find('\n', start);
            if (end == std::string::npos) end = reply.size();
            const std::string line = reply.substr(start, end - start);
            if (const size_t eq = line.find('='); eq != std::string::npos) {
                status[line.substr(0, eq)] = line.substr(eq + 1);
            }
            start = end + 1;
        }
    }

    static std::vector<std::wstring> ParseOutputs(const std::string& reply) {
        std::vector<std::wstring> names;
        size_t start = 0;
        while (start < reply.size()) {
            size_t end = reply.find('\n', start);
            if (end == std::string::npos) end = reply.size();
            const std::string line = reply.substr(start, end - start);
            if (!line.empty()) names.push_back(Widen(line.substr(0, line.find('\t'))));
            start = end + 1;
        }
        return names;
    }

    HWND window_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::string> commands_;
    bool quit_ = false;
    std::thread thread_;  // last: starts after the members it uses
};

//=============================================================================
// Window

struct App {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    UINT taskbarCreated = 0;
    HICON icons[3] = {};  // IDI_OK, IDI_WAIT, IDI_OFF
    std::unique_ptr<Snapshot> snapshot = std::make_unique<Snapshot>();
    std::unique_ptr<Worker> worker;
    int shownIcon = -1;
    std::wstring shownTip;
};
App g_app;

// Icon index and the text lines describing the current state.
int Describe(const Snapshot& s, std::wstring& line1, std::wstring& line2) {
    if (!s.running) {
        line1 = S().notRunning;
        return 2;
    }
    const std::wstring state = s.Get("state"), reason = s.Get("reason");
    if (state == L"streaming") {
        if (s.Get("capture") != L"1") {
            line1 = S().noDevice;
            return 1;
        }
        line1 = Format(S().streaming, s.Get("output"));
        line2 = s.Get("playing") == L"1" ? S().soundPlaying : S().silence;
        return 0;
    }
    if (state == L"paused") {
        line1 = S().released;
    } else if (state == L"waiting" && reason == L"output-busy") {
        line1 = Format(S().outputBusy, s.Get("output"));
    } else if (state == L"waiting" && reason == L"output-error") {
        line1 = Format(S().outputError, s.Get("detail"));
    } else if (state == L"waiting") {
        const std::wstring setting = s.Get("setting");
        line1 = setting.empty() ? std::wstring(S().waitingHdmi) : Format(S().waitingOutput, setting);
    } else {
        line1 = S().starting;
    }
    return 1;
}

void UpdateIcon(bool add) {
    std::wstring line1, line2;
    const int icon = Describe(*g_app.snapshot, line1, line2);
    std::wstring tip = L"Win51\n" + line1 + (line2.empty() ? L"" : L"\n" + line2);
    if (!add && icon == g_app.shownIcon && tip == g_app.shownTip) return;

    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_app.window;
    nid.uID = kIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_app.icons[icon];
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    if (add) {
        Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    g_app.shownIcon = icon;
    g_app.shownTip = std::move(tip);
}

void OpenLogFolder() {
    PWSTR programData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))) {
        const std::wstring folder = std::wstring(programData) + L"\\Win51";
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    CoTaskMemFree(programData);
}

void ShowMenu(int x, int y) {
    const Snapshot s = *g_app.snapshot;  // a copy: new snapshots keep arriving while the menu is open
    std::wstring line1, line2;
    Describe(s, line1, line2);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line1.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Output choice: automatic or one of the outputs that accept AC3. The setting is a name fragment;
    // it matches an output whose name contains it.
    HMENU outputs = CreatePopupMenu();
    const std::wstring setting = s.Get("setting");
    UINT checked = ID_OUTPUT_AUTO;
    AppendMenuW(outputs, MF_STRING, ID_OUTPUT_AUTO, S().menuAutomatic);
    for (size_t i = 0; i < s.outputs.size(); ++i) {
        const UINT id = ID_OUTPUT_FIRST + (UINT)i;
        AppendMenuW(outputs, MF_STRING, id, s.outputs[i].c_str());
        if (!setting.empty() && checked == ID_OUTPUT_AUTO &&
            FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | NORM_IGNORECASE, s.outputs[i].c_str(), -1,
                            setting.c_str(), -1, nullptr, nullptr, nullptr, 0) >= 0) {
            checked = id;
        }
    }
    const UINT missing = ID_OUTPUT_FIRST + (UINT)s.outputs.size();
    UINT last = missing - 1;
    if (!setting.empty() && checked == ID_OUTPUT_AUTO) {
        // The chosen output is not connected right now (e.g. the TV is off).
        AppendMenuW(outputs, MF_STRING | MF_GRAYED, missing, Format(S().menuNotFound, setting).c_str());
        checked = last = missing;
    }
    CheckMenuRadioItem(outputs, ID_OUTPUT_AUTO, last, checked, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP | (s.running ? 0 : MF_GRAYED), (UINT_PTR)outputs, S().menuOutput);

    const bool paused = s.Get("state") == L"paused";
    AppendMenuW(menu, MF_STRING | (paused ? MF_CHECKED : 0) | (s.running ? 0 : MF_GRAYED), ID_RELEASE,
                S().menuRelease);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_SOUND_SETTINGS, S().menuSoundSettings);
    AppendMenuW(menu, MF_STRING, ID_LOG_FOLDER, S().menuLogFolder);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, S().menuExit);

    // The window must be in the foreground, or the menu does not close when the user clicks elsewhere.
    SetForegroundWindow(g_app.window);
    const UINT command = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON |
                                                          (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : 0),
                                                x, y, g_app.window, nullptr);
    PostMessageW(g_app.window, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenu

    if (command == ID_RELEASE) {
        g_app.worker->Send(paused ? "resume" : "pause");
    } else if (command == ID_OUTPUT_AUTO) {
        g_app.worker->Send("output");
    } else if (command >= ID_OUTPUT_FIRST && command < missing) {
        g_app.worker->Send("output " + Narrow(s.outputs[command - ID_OUTPUT_FIRST]));
    } else if (command == ID_SOUND_SETTINGS) {
        ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl", nullptr, SW_SHOWNORMAL);
    } else if (command == ID_LOG_FOLDER) {
        OpenLogFolder();
    } else if (command == ID_EXIT) {
        DestroyWindow(g_app.window);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_TRAY:
            switch (LOWORD(lParam)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    ShowMenu(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
                    break;
            }
            return 0;
        case WM_STATUS:
            g_app.snapshot.reset((Snapshot*)lParam);
            UpdateIcon(false);
            return 0;
        case WM_DESTROY: {
            g_app.worker.reset();
            NOTIFYICONDATAW nid = {sizeof(nid)};
            nid.hWnd = window;
            nid.uID = kIconId;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            return 0;
        }
        default:
            if (message == g_app.taskbarCreated && g_app.taskbarCreated != 0) {
                UpdateIcon(true);  // Explorer restarted: the icon has to be added again
                return 0;
            }
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // One icon per user session.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\Win51Tray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_app.instance = instance;
    const int iconIds[3] = {IDI_OK, IDI_WAIT, IDI_OFF};
    for (int i = 0; i < 3; ++i) LoadIconMetric(instance, MAKEINTRESOURCEW(iconIds[i]), LIM_SMALL, &g_app.icons[i]);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"Win51Tray";
    RegisterClassExW(&wc);
    // A hidden top-level window: message-only windows do not receive the TaskbarCreated broadcast.
    g_app.window = CreateWindowExW(0, wc.lpszClassName, L"Win51", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                   instance, nullptr);
    if (!g_app.window) return 1;
    g_app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // Let a non-elevated Explorer deliver TaskbarCreated even if this process runs elevated.
    ChangeWindowMessageFilterEx(g_app.window, g_app.taskbarCreated, MSGFLT_ALLOW, nullptr);

    UpdateIcon(true);
    g_app.worker = std::make_unique<Worker>(g_app.window);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    for (HICON icon : g_app.icons) DestroyIcon(icon);
    CloseHandle(single);
    return 0;
}
