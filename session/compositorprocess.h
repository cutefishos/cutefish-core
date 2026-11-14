#pragma once

#include <functional>
#include <string>
#include <vector>

#include "base/process.h"
#include "base/timer.h"

// Session lifecycle only. No window-management or shell UI traffic crosses here.
class CompositorProcess final
{
public:
    CompositorProcess();
    ~CompositorProcess();
    void start(const std::string& program, const std::string& socket, const std::string& controlSocket,
               int timeoutMs = 15000, const std::string& xwaylandReadyFile = {});
    void stop();

    bool isReady() const { return m_ready; }
    // The Wayland socket the compositor listens on: the one it was given,
    // or once ready the one it picked.
    const std::string& socketName() const { return m_socket; }

    std::function<void()> onReady;
    std::function<void(const std::string& reason)> onFailed;
    std::function<void(int code)> onExited;

private:
    void closeReadyPipe();
    void fail(const std::string& reason);
    void readReady();
    cutefish::base::Process m_process;
    cutefish::base::Timer m_timeout;
    int m_readFd = -1;
    int m_readWatch = 0;
    bool m_ready = false;
    std::string m_socket;
    bool m_stopping = false;
    bool m_failed = false;
};
