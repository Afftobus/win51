// Control channel between win51svc and its clients (win51tray, "win51svc ctl").
//
// A message-mode named pipe served by win51svc. A client connects, writes one request and reads one reply.
// Requests and replies are UTF-8 text:
//
//   status          state=starting|streaming|waiting|paused
//                   reason=|no-output|output-busy|output-error   why it waits
//                   output=<endpoint name>                       in use, or the one it tried last
//                   setting=<output name fragment>               configured output, empty = automatic
//                   capture=0|1                                  the Win51 endpoint is captured
//                   playing=0|1                                  audio arrived in the last 0.5 s
//                   detail=<text>                                last error, for display
//                   version=<win51svc version>
//   outputs         one line per output that accepts AC3: <endpoint name> TAB hdmi|spdif|other
//   pause, resume   release the HDMI output / take it again        -> "ok"
//   output <name>   use this output (empty = automatic), stored     -> "ok"
//
// Unknown requests get "error <text>". Interactive users and administrators may connect.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace w51 {

constexpr wchar_t kControlPipeName[] = L"\\\\.\\pipe\\Win51";
constexpr DWORD kControlPipeBufferBytes = 64 * 1024;

// Sends one request and returns the reply. False when win51svc is not running or does not answer in time.
inline bool CallControlPipe(const std::string& request, std::string& reply, DWORD timeoutMs = 2000) {
    std::vector<char> buffer(kControlPipeBufferBytes);
    DWORD read = 0;
    if (!CallNamedPipeW(kControlPipeName, const_cast<char*>(request.data()), (DWORD)request.size(), buffer.data(),
                        (DWORD)buffer.size(), &read, timeoutMs)) {
        return false;
    }
    reply.assign(buffer.data(), read);
    return true;
}

}  // namespace w51
