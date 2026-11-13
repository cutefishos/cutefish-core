#include "compositorprocess.h"

#include "base/event_loop.h"
#include "journalstream.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

using namespace std::chrono_literals;

CompositorProcess::CompositorProcess()
{
    m_process.setOutputForwarded(true);
    m_timeout.setSingleShot(true);
    m_timeout.onTimeout = [this] {
        fail("Compositor startup timed out");
    };
    m_process.onFinished = [this](int code, bool crashed) {
        closeReadyPipe();
        if (m_stopping || m_failed)
            return;
        if (!m_ready)
            fail("Compositor exited before becoming ready");
        else if (onExited)
            onExited(crashed ? 1 : code);
    };
}

CompositorProcess::~CompositorProcess()
{
    stop();
}

void CompositorProcess::closeReadyPipe()
{
    m_timeout.stop();
    if (m_readWatch) {
        cutefish::base::EventLoop::current()->unwatch(m_readWatch);
        m_readWatch = 0;
    }
    if (m_readFd >= 0) {
        close(m_readFd);
        m_readFd = -1;
    }
}

void CompositorProcess::fail(const std::string& reason)
{
    if (m_failed || m_stopping)
        return;
    m_failed = true;
    stop();
    if (onFailed)
        onFailed(reason);
}

void CompositorProcess::readReady()
{
    // "R" and the socket name, written at once.
    char buffer[256];
    const auto count = read(m_readFd, buffer, sizeof(buffer));
    if (count < 0)
        return;
    if (count < 1 || buffer[0] != 'R') {
        fail("Compositor did not confirm startup");
        return;
    }
    std::string socket(buffer + 1, size_t(count - 1));
    if (!socket.empty() && socket.back() == '\n')
        socket.pop_back();
    if (!socket.empty())
        m_socket = socket;
    closeReadyPipe();
    m_ready = true;
    if (onReady)
        onReady();
}

void CompositorProcess::start(const std::string& program, const std::string& socket,
                              const std::string& controlSocket, int timeoutMs,
                              const std::string& xwaylandReadyFile)
{
    if (m_process.isRunning() || m_readFd >= 0)
        return;
    m_ready = false;
    m_stopping = false;
    m_failed = false;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        fail("Cannot create compositor startup pipe");
        return;
    }
    m_readFd = fds[0];
    const int writeFd = fds[1];
    // The write end is the compositor's only inherited descriptor.
    // With no socket named the compositor picks the first free wayland-N and
    // reports it with its readiness.
    m_socket = socket;
    std::vector<std::string> arguments;
    if (!socket.empty())
        arguments = {"--socket", socket};
    arguments.push_back("--ready-fd");
    arguments.push_back(std::to_string(cutefish::base::Process::kInheritedFd));
    if (!controlSocket.empty()) {
        arguments.push_back("--shell-control-socket");
        arguments.push_back(controlSocket);
    }
    if (!xwaylandReadyFile.empty()) {
        arguments.push_back("--xwayland-ready-file");
        arguments.push_back(xwaylandReadyFile);
    }
    m_readWatch = cutefish::base::EventLoop::current()->watch(m_readFd, EPOLLIN, [this](uint32_t) {
        readReady();
    });
    m_process.setInheritedFd(writeFd);
    const bool started = startWithOwnJournalStream(m_process, program, arguments);
    close(writeFd);
    if (!started) {
        fail("Cannot start " + program);
        return;
    }
    if (!m_ready && !m_failed && !m_stopping)
        m_timeout.start(std::chrono::milliseconds(timeoutMs));
}

void CompositorProcess::stop()
{
    m_stopping = true;
    closeReadyPipe();
    if (m_process.isRunning()) {
        m_process.terminate();
        if (!m_process.waitForFinished(3000ms)) {
            m_process.kill();
            m_process.waitForFinished(1000ms);
        }
    }
    m_ready = false;
}
