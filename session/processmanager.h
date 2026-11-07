// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/dbus.h"
#include "base/process.h"
#include "base/signal.h"
#include "base/timer.h"

class XwaylandEnvironment;
class CompositorProcess;
class ShellConnection;
class ShellProcess;
class RestartPolicy;
struct Publisher;

class ProcessManager
{
public:
    ProcessManager();
    ~ProcessManager();

    enum class Ending {
        Logout,
        Restart,
        Shutdown,
    };

    void start();
    void logout();

    // Asks every application to quit, as Quit asks one, and runs `then` once
    // all have. One that cancels or stops responding keeps the session going,
    // and the user is told which. `then` returns false when the session goes on
    // after all, as after a refused restart.
    void endAfterApplications(Ending ending, std::function<bool()> then);

    // What logout does once the display manager has been told, for when it or
    // the system is the one ending the session.
    void endSession(bool behindLoginScreen = false);

    void startDesktopProcess();
    void startDaemonProcess();
    void loadAutoStartProcess();

private:
    void leave();
    // The rest of endSession once the applications are gone or the moment
    // given them is over: the compositor outlives them.
    void finishEnding(bool behindLoginScreen);
    void stopSessionProcesses(bool behindLoginScreen);
    void notifyEndingStopped(Ending ending, const std::string &outcome, const std::string &applicationId);
    void startAfterCompositorReady();
    void publishXwaylandEnvironment(bool startDesktop);
    void startShell();
    void onShellFinished(int code, bool crashed);
    // Started once and restarted by the same bounded rule as the shell.
    void startCoreProcess(const std::string& program, const std::vector<std::string>& arguments = {});
    void stopRestarts();
    void stopShellAndDaemons();
    void stopProcesses(std::map<std::string, std::unique_ptr<cutefish::base::Process>>& processes);

private:
    std::unique_ptr<XwaylandEnvironment> m_xwaylandEnvironment;
    std::unique_ptr<CompositorProcess> m_compositorProcess;
    std::unique_ptr<ShellConnection> m_shellConnection;
    std::unique_ptr<ShellProcess> m_shellProcess;
    std::unique_ptr<RestartPolicy> m_shellRestart;
    cutefish::base::dbus::NameWatch m_servicesWatch;
    cutefish::base::dbus::PendingCall m_quitAllCall;
    cutefish::base::dbus::PendingCall m_applicationsCall;
    cutefish::base::Timer m_applicationsPoll;
    std::chrono::steady_clock::time_point m_applicationsDeadline;
    cutefish::base::Subscription m_registryChanged;
    cutefish::base::Timer m_autostartTimer;
    cutefish::base::Timer m_exitTimer;
    std::vector<std::unique_ptr<Publisher>> m_publishers;
    bool m_started = false;
    bool m_compositorReady = false;
    bool m_desktopStarted = false;
    bool m_loggingOut = false;
    bool m_ending = false;
    std::string m_waylandSocket;
    std::string m_controlSocket;
    std::map<std::string, std::unique_ptr<cutefish::base::Process>> m_coreProcesses;
    std::map<std::string, std::unique_ptr<RestartPolicy>> m_restartPolicies;
};

#endif // PROCESSMANAGER_H
