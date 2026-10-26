// Server side of the control pipe (protocol in shared\win51_control.h): status and controls of a running
// pipeline for the tray icon and "win51svc ctl".
#pragma once

#include <windows.h>

#include <string>
#include <thread>

#include "pipeline.h"

namespace w51 {

class ControlPipe {
public:
    explicit ControlPipe(Pipeline& pipeline);
    ControlPipe(const ControlPipe&) = delete;
    ControlPipe& operator=(const ControlPipe&) = delete;
    ~ControlPipe();

    void Start();
    void Stop();

private:
    void Serve();
    void ServeClient(HANDLE pipe, OVERLAPPED& ov);
    std::string Handle(const std::string& request);
    std::string Outputs();

    Pipeline& pipeline_;
    HANDLE stopEvent_;
    std::thread thread_;
};

}  // namespace w51
