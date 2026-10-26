#include "log.h"

#include <audioclient.h>
#include <io.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace w51 {
namespace {

constexpr long long kMaxLogBytes = 10ll << 20;  // then the log moves to <name>.old and starts over

std::mutex g_logMutex;
FILE* g_logFile = nullptr;
std::wstring g_logPath;

// A new file starts with a UTF-8 BOM so that Notepad and PowerShell show the endpoint names correctly.
void OpenLogLocked() {
    g_logFile = _wfsopen(g_logPath.c_str(), L"a", _SH_DENYWR);
    if (g_logFile && _filelengthi64(_fileno(g_logFile)) == 0) fputs("\xEF\xBB\xBF", g_logFile);
}

}  // namespace

void LogOpenFile(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) fclose(g_logFile);
    g_logPath = path;
    OpenLogLocked();
}

void Log(const char* fmt, ...) {
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds, GetCurrentThreadId(), msg);

    std::lock_guard<std::mutex> lock(g_logMutex);
    fputs(line, stdout);
    fflush(stdout);
    if (g_logFile) {
        fputs(line, g_logFile);
        fflush(g_logFile);
        if (_filelengthi64(_fileno(g_logFile)) > kMaxLogBytes) {
            fclose(g_logFile);
            MoveFileExW(g_logPath.c_str(), (g_logPath + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
            OpenLogLocked();
        }
    }
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

static const char* AudioHrName(HRESULT hr) {
    switch (hr) {
#define W51_HR_NAME(x) case x: return #x;
        W51_HR_NAME(AUDCLNT_E_NOT_INITIALIZED)
        W51_HR_NAME(AUDCLNT_E_ALREADY_INITIALIZED)
        W51_HR_NAME(AUDCLNT_E_WRONG_ENDPOINT_TYPE)
        W51_HR_NAME(AUDCLNT_E_DEVICE_INVALIDATED)
        W51_HR_NAME(AUDCLNT_E_NOT_STOPPED)
        W51_HR_NAME(AUDCLNT_E_BUFFER_TOO_LARGE)
        W51_HR_NAME(AUDCLNT_E_OUT_OF_ORDER)
        W51_HR_NAME(AUDCLNT_E_UNSUPPORTED_FORMAT)
        W51_HR_NAME(AUDCLNT_E_INVALID_SIZE)
        W51_HR_NAME(AUDCLNT_E_DEVICE_IN_USE)
        W51_HR_NAME(AUDCLNT_E_BUFFER_OPERATION_PENDING)
        W51_HR_NAME(AUDCLNT_E_THREAD_NOT_REGISTERED)
        W51_HR_NAME(AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
        W51_HR_NAME(AUDCLNT_E_ENDPOINT_CREATE_FAILED)
        W51_HR_NAME(AUDCLNT_E_SERVICE_NOT_RUNNING)
        W51_HR_NAME(AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED)
        W51_HR_NAME(AUDCLNT_E_EXCLUSIVE_MODE_ONLY)
        W51_HR_NAME(AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL)
        W51_HR_NAME(AUDCLNT_E_EVENTHANDLE_NOT_SET)
        W51_HR_NAME(AUDCLNT_E_INCORRECT_BUFFER_SIZE)
        W51_HR_NAME(AUDCLNT_E_BUFFER_SIZE_ERROR)
        W51_HR_NAME(AUDCLNT_E_CPUUSAGE_EXCEEDED)
        W51_HR_NAME(AUDCLNT_E_BUFFER_ERROR)
        W51_HR_NAME(AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
        W51_HR_NAME(AUDCLNT_E_INVALID_DEVICE_PERIOD)
#undef W51_HR_NAME
        default: return nullptr;
    }
}

std::string HrToString(HRESULT hr) {
    if (const char* name = AudioHrName(hr)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "0x%08lX %s", (unsigned long)hr, name);
        return buf;
    }
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, hr, 0, (LPWSTR)&text, 0, nullptr);
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
    std::string result = buf;
    if (text) {
        std::string t = Narrow(text);
        LocalFree(text);
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
        result += " " + t;
    }
    return result;
}

}  // namespace w51
