// Minimal thread-safe logging to stdout and an optional file.
#pragma once

#include <windows.h>
#include <string>

namespace w51 {

void LogOpenFile(const std::wstring& path);
void Log(_Printf_format_string_ const char* fmt, ...);

std::string Narrow(const std::wstring& s);   // UTF-16 -> UTF-8
std::wstring Widen(const std::string& s);    // UTF-8 -> UTF-16
std::string HrToString(HRESULT hr);

}  // namespace w51

// Logs and returns the HRESULT from the enclosing function when expr fails.
#define W51_RETURN_IF_FAILED(expr)                                                          \
    do {                                                                                     \
        HRESULT hr_ = (expr);                                                                \
        if (FAILED(hr_)) {                                                                   \
            w51::Log("%s failed: %s (%s:%d)", #expr, w51::HrToString(hr_).c_str(), __FILE__, \
                     __LINE__);                                                              \
            return hr_;                                                                      \
        }                                                                                    \
    } while (0)
