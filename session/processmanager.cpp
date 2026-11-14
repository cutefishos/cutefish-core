// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "processmanager.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <tuple>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include "base/application_registry.h"
#include "base/event_loop.h"
#include "base/i18n.h"
#include "base/paths.h"
#include "base/strings.h"
#include "common/runtime_client.h"

#include "autostartentries.h"
#include "compositorprocess.h"
#include "journalstream.h"
#include "logs.h"
#include "restartpolicy.h"
#include "shellconnection.h"
#include "shellprocess.h"
#include "userdirectories.h"
#include "xwaylandenvironment.h"

namespace base = cutefish::base;
namespace strings = cutefish::base::strings;
namespace dbus = cutefish::base::dbus;
namespace runtime = cutefish::core::runtime;
using namespace std::chrono_literals;
using cutefish::i18n::format;
using cutefish::i18n::tr;

static constexpr char kCompositor[] = "/usr/bin/cutefish-compositor";

struct Publisher
{
    base::Process process;
    base::Timer deadline;
};

static std::string environmentValue(const char *name, const char *fallback = "")
{
    const char *value = std::getenv(name);
    return value && *value ? std::string(value) : std::string(fallback);
}

// A command line split at white space, double quotes grouping, as the Exec
// lines of autostart entries are written; three quotes stand for one.
static std::vector<std::string> splitCommand(const std::string &command)
{
    std::vector<std::string> arguments;
    std::string current;
    int quoteCount = 0;
    bool inQuote = false;

    for (const char c : command) {
        if (c == '"') {
            ++quoteCount;
            if (quoteCount == 3) {
                quoteCount = 0;
                current += c;
            }
            continue;
        }
        if (quoteCount) {
            if (quoteCount == 1)
                inQuote = !inQuote;
            quoteCount = 0;
        }
        if (!inQuote && strings::isAsciiWhitespace(c)) {
            if (!current.empty()) {
                arguments.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty())
        arguments.push_back(current);

    return arguments;
}

ProcessManager::ProcessManager()
    : m_compositorProcess(std::make_unique<CompositorProcess>())
    , m_shellConnection(std::make_unique<ShellConnection>())
    , m_shellProcess(std::make_unique<ShellProcess>())
    , m_shellRestart(std::make_unique<RestartPolicy>("cutefish-filemanager", [this] { startShell(); }))
{
    m_shellProcess->onFinished = [this](int code, bool crashed) {
        onShellFinished(code, crashed);
    };
    m_autostartTimer.setSingleShot(true);
    m_autostartTimer.onTimeout = [this] {
        loadAutoStartProcess();
    };
    m_exitTimer.setSingleShot(true);
}

ProcessManager::~ProcessManager()
{
    stopShellAndDaemons();
    m_compositorProcess->stop();
}

void ProcessManager::start()
{
    if (m_started)
        return;
    m_started = true;

    // A preset socket is used as it is; otherwise the compositor picks the first
    // free wayland-N, the name sandboxed clients such as snaps may connect to.
    m_waylandSocket = environmentValue("WAYLAND_DISPLAY", "");
    const std::string runtimeDir = environmentValue("XDG_RUNTIME_DIR", "/tmp");
    m_controlSocket = runtimeDir + "/cutefish-" + std::to_string(getpid()) + "-shell.control";

    unsetenv("DISPLAY");
    unsetenv("XAUTHORITY");
    m_xwaylandEnvironment = std::make_unique<XwaylandEnvironment>();
    m_xwaylandEnvironment->onResolved = [this](bool) {
        publishXwaylandEnvironment(true);
    };
    m_xwaylandEnvironment->onLost = [this] {
        publishXwaylandEnvironment(false);
    };
    m_compositorProcess->onReady = [this] {
        m_waylandSocket = m_compositorProcess->socketName();
        setenv("WAYLAND_DISPLAY", m_waylandSocket.c_str(), 1);
        m_xwaylandEnvironment->waitForReady();
    };
    m_compositorProcess->onFailed = [this](const std::string &reason) {
        CF_CRITICAL(sessionLog()) << reason;
        stopShellAndDaemons();
        base::EventLoop::current()->quit(1);
    };
    // A compositor that dies takes the graphical session with it: there is no
    // display left to run on.
    m_compositorProcess->onExited = [this](int code) {
        stopShellAndDaemons();
        base::EventLoop::current()->quit(code);
    };
    m_compositorProcess->start(kCompositor, m_waylandSocket, m_controlSocket, 15000,
                               m_xwaylandEnvironment->readinessFile());
}

void ProcessManager::publishXwaylandEnvironment(bool startDesktop)
{
    if (m_loggingOut)
        return;
    auto publisher = std::make_unique<Publisher>();
    Publisher *raw = publisher.get();
    const auto finish = [this, raw, startDesktop] {
        raw->deadline.stop();
        const auto found = std::find_if(m_publishers.begin(), m_publishers.end(),
                                        [raw](const auto &item) { return item.get() == raw; });
        if (found != m_publishers.end()) {
            std::shared_ptr<Publisher> done(std::move(*found));
            m_publishers.erase(found);
            base::EventLoop::current()->post([done] {});
        }
        if (startDesktop && !m_loggingOut)
            startAfterCompositorReady();
    };
    raw->process.setOutputForwarded(true);
    raw->process.onFinished = [finish](int code, bool) {
        if (code != 0)
            CF_WARNING(sessionLog()) << "Could not publish Xwayland environment to activation services";
        finish();
    };
    raw->deadline.setSingleShot(true);
    raw->deadline.onTimeout = [raw] {
        raw->process.kill();
    };
    m_publishers.push_back(std::move(publisher));
    if (!raw->process.start("dbus-update-activation-environment",
                            {"--systemd", "WAYLAND_DISPLAY=" + m_waylandSocket,
                             "DISPLAY=" + environmentValue("DISPLAY"),
                             "XAUTHORITY=" + environmentValue("XAUTHORITY")})) {
        CF_WARNING(sessionLog()) << "dbus-update-activation-environment is unavailable";
        finish();
        return;
    }
    raw->deadline.start(3000ms);
}

void ProcessManager::startAfterCompositorReady()
{
    if (m_compositorReady)
        return;
    m_compositorReady = true;

    // The persistent control channel. It is what makes a shell restart possible
    // without restarting the compositor: every shell generation gets a fresh
    // Wayland socketpair over it.
    if (!m_shellConnection->connectToCompositor(m_controlSocket)) {
        CF_WARNING(sessionLog()) << "No compositor control channel; the shell will run without "
                                    "the private Cutefish protocols";
    }

    // Application autostart waits for the existing external services daemon.
    if (dbus::Connection *bus = dbus::Connection::session()) {
        m_servicesWatch = bus->watchName("org.cutefish.Services", [this](const std::string &owner) {
            if (owner.empty())
                return;
            m_servicesWatch = {};
            startDesktopProcess();
        });
    }

    startDaemonProcess();
}

void ProcessManager::startShell()
{
    if (m_loggingOut || m_shellProcess->isRunning())
        return;

    m_shellRestart->started();
    m_shellProcess->startForSession("cutefish-filemanager", m_waylandSocket,
                                    m_shellConnection->isConnected() ? m_shellConnection.get() : nullptr);
}

void ProcessManager::onShellFinished(int code, bool crashed)
{
    if (m_loggingOut || !m_desktopStarted)
        return;

    // The compositor and every application it is showing carry on, the
    // wallpaper included; only the desktop's icons disappear, and a new
    // daemon on a new trusted connection brings them back.
    m_shellRestart->exited(code, crashed);
}

void ProcessManager::logout()
{
    endAfterApplications(Ending::Logout, [this] {
        leave();
        return true;
    });
}

void ProcessManager::endAfterApplications(Ending ending, std::function<bool()> then)
{
    if (m_ending)
        return;
    m_ending = true;

    dbus::Connection *bus = dbus::Connection::session();
    if (!bus) {
        m_ending = then();
        return;
    }
    const dbus::Message quitAll = bus->newMethodCall("org.cutefish.Applications1", "/org/cutefish/Applications1",
                                                     "org.cutefish.Applications1", "QuitAll");
    // No timeout: the user may take as long as they like over an unsaved document.
    m_quitAllCall = bus->callAsync(
            quitAll,
            [this, ending, then](const dbus::Reply &reply) {
                std::string outcome;
                std::string applicationId;
                const bool answered = reply.read(outcome, applicationId);
                // Without the compositor's answer the session ends as it always did.
                if (!answered || outcome == "quit") {
                    m_ending = then();
                    return;
                }
                m_ending = false;
                notifyEndingStopped(ending, outcome, applicationId);
            },
            std::chrono::milliseconds(INT_MAX));
}

void ProcessManager::notifyEndingStopped(Ending ending, const std::string &outcome,
                                         const std::string &applicationId)
{
    base::ApplicationRegistry &registry = base::ApplicationRegistry::instance();
    // A registry created here has not scanned yet; the name and icon come from it.
    if (!applicationId.empty() && registry.isLoading() && registry.entries().empty()) {
        m_registryChanged = registry.changed.connect([this, ending, outcome, applicationId] {
            m_registryChanged.reset();
            notifyEndingStopped(ending, outcome, applicationId);
        });
        return;
    }

    const auto entry = registry.byId(applicationId);
    std::string name = entry ? entry->name : std::string();
    if (name.empty())
        name = applicationId;

    std::string summary;
    switch (ending) {
    case Ending::Logout:
        summary = tr("Log out cancelled");
        break;
    case Ending::Restart:
        summary = tr("Restart cancelled");
        break;
    case Ending::Shutdown:
        summary = tr("Shutdown cancelled");
        break;
    }
    const bool cancelled = outcome == "cancelled";
    std::string body;
    if (name.empty())
        body = cancelled ? tr("An application cancelled it.") : tr("An application did not quit.");
    else
        body = format(cancelled ? tr("{name} cancelled it.") : tr("{name} did not quit."), {{"name", name}});

    dbus::Connection *bus = dbus::Connection::session();
    if (!bus)
        return;
    dbus::Message notify = bus->newMethodCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                              "org.freedesktop.Notifications", "Notify");
    // Only a running notification server shows it; none is started for it.
    sd_bus_message_set_auto_start(notify.get(), 0);
    // The session has no desktop entry to title it.
    // TRANSLATORS: the sender of a notification from the session itself.
    notify.append(std::string(tr("System")), uint32_t(0),
                  entry && !entry->icon.empty() ? entry->icon : std::string("system-log-out"), summary, body,
                  std::vector<std::string>(), dbus::VariantMap{{"urgency", dbus::Variant(uint8_t(1))}},
                  int32_t(-1));
    bus->send(notify);
}

void ProcessManager::leave()
{
    m_loggingOut = true;
    stopRestarts();

    // Under cutefish-dm the login screen comes up on its own VT first, and the
    // reply comes once it is on screen: this session then ends where nobody sees
    // it. Another display manager does not answer, and the session ends as before.
    bool switched = false;
    if (dbus::Connection *system = dbus::Connection::system()) {
        const dbus::Message switchToGreeter =
                system->newMethodCall("org.cutefish.DisplayManager", "/org/cutefish/DisplayManager",
                                      "org.cutefish.DisplayManager", "SwitchToGreeter");
        const dbus::Reply reply = system->call(switchToGreeter, 15s);
        switched = !reply.isError();
        if (reply.isError() && reply.error().name != "org.freedesktop.DBus.Error.ServiceUnknown")
            CF_WARNING(sessionLog()) << "The login screen did not come up before logout:"
                                     << reply.error().message;
    } else {
        CF_WARNING(sessionLog()) << "The login screen did not come up before logout: no system bus";
    }

    endSession(switched);
}

void ProcessManager::endSession(bool behindLoginScreen)
{
    m_loggingOut = true;
    stopRestarts();

    // Applications belong to the runtime, so ask it to quit them, then close
    // what we started ourselves.
    if (!runtime::quitAll()) {
        for (const std::string &appId : runtime::runningApplications())
            runtime::quitApplication(appId);
    }
    // The runtime also has the user manager stop every application scope.
    // The compositor goes only once they are gone, or after a moment: an
    // application still shutting down must not lose its display first.
    m_applicationsDeadline = std::chrono::steady_clock::now() + 3s;
    m_applicationsPoll.setSingleShot(true);
    m_applicationsPoll.onTimeout = [this, behindLoginScreen] { finishEnding(behindLoginScreen); };
    finishEnding(behindLoginScreen);
}

// (ssssssouso): a unit as the user manager lists it.
using UnitInfo = std::tuple<std::string, std::string, std::string, std::string, std::string, std::string,
                            dbus::ObjectPath, uint32_t, std::string, dbus::ObjectPath>;

void ProcessManager::finishEnding(bool behindLoginScreen)
{
    dbus::Connection *bus = dbus::Connection::session();
    if (!bus || std::chrono::steady_clock::now() >= m_applicationsDeadline) {
        stopSessionProcesses(behindLoginScreen);
        return;
    }
    dbus::Message list = bus->newMethodCall("org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                                            "org.freedesktop.systemd1.Manager", "ListUnitsByPatterns");
    list.append(std::vector<std::string> {"active", "activating", "deactivating"},
                std::vector<std::string> {"app-*.scope"});
    m_applicationsCall = bus->callAsync(list, [this, behindLoginScreen](const dbus::Reply &reply) {
        std::vector<UnitInfo> units;
        // Without an answer the session ends as it always did.
        if (reply.read(units) && !units.empty()) {
            m_applicationsPoll.start(100ms);
            return;
        }
        stopSessionProcesses(behindLoginScreen);
    });
}

void ProcessManager::stopSessionProcesses(bool behindLoginScreen)
{
    m_shellProcess->stop();
    stopProcesses(m_coreProcesses);

    // The kernel takes the compositor's cursor down with it; behind the login
    // screen it stays until that one's own replaces it.
    m_exitTimer.onTimeout = [this] {
        m_compositorProcess->stop();
        base::EventLoop::current()->quit(0);
    };
    m_exitTimer.start(behindLoginScreen ? 1000ms : 0ms);
}

void ProcessManager::stopShellAndDaemons()
{
    m_loggingOut = true;
    stopRestarts();
    m_shellProcess->stop();
    stopProcesses(m_coreProcesses);
}

void ProcessManager::stopProcesses(std::map<std::string, std::unique_ptr<base::Process>> &processes)
{
    for (auto &[program, process] : processes) {
        if (!process->isRunning())
            continue;

        process->terminate();

        if (!process->waitForFinished(2000ms))
            process->kill();
    }
}

void ProcessManager::startDesktopProcess()
{
    if (!m_compositorReady || m_desktopStarted)
        return;
    m_desktopStarted = true;

    // Before the file manager, which reads the directories once.
    UserDirectories::ensure(base::paths::home(), base::paths::configHome());
    // Resident, so every file manager window and dialog of the session, the
    // dock's included, opens from one warm process; it draws the desktop on
    // the trusted connection.
    startShell();
    // Without it every administrator prompt fails, so it is restarted as the
    // core processes are.
    startCoreProcess("cutefish-polkit-agent");

    // Auto start
    m_autostartTimer.start(100ms);
}

void ProcessManager::startDaemonProcess()
{
    // The application runtime owns every application start, so it has to be up
    // before anything that may want to launch one.
    startCoreProcess("cutefish-appruntime");
    startCoreProcess("cutefish-services");
}

void ProcessManager::startCoreProcess(const std::string &program, const std::vector<std::string> &arguments)
{
    if (m_loggingOut)
        return;

    auto &process = m_coreProcesses[program];
    auto &restart = m_restartPolicies[program];
    if (!process) {
        process = std::make_unique<base::Process>();
        process->setOutputForwarded(true);
        restart = std::make_unique<RestartPolicy>(program, [this, program, arguments] {
            startCoreProcess(program, arguments);
        });
        RestartPolicy *policy = restart.get();
        process->onFinished = [policy](int code, bool crashed) {
            policy->exited(code, crashed);
        };
    }
    if (process->isRunning())
        return;

    restart->started();
    if (!startWithOwnJournalStream(*process, program, arguments)) {
        CF_WARNING(sessionLog()) << "Failed to start" << program;
        restart->exited(-1, false);
    }
}

void ProcessManager::stopRestarts()
{
    m_shellRestart->stop();
    for (auto &[program, restart] : m_restartPolicies)
        restart->stop();
}

void ProcessManager::loadAutoStartProcess()
{
    // The user's directory first, then the system's.
    std::vector<std::string> configDirectories {base::paths::configHome()};
    for (const std::string &directory : base::paths::configDirectories())
        configDirectories.push_back(directory);
    std::vector<std::string> dirs;
    for (const std::string &directory : configDirectories) {
        const std::string path = directory + "/autostart";
        std::error_code error;
        if (std::filesystem::is_directory(path, error) && std::find(dirs.begin(), dirs.end(), path) == dirs.end())
            dirs.push_back(path);
    }
    const auto desktops = strings::split(environmentValue("XDG_CURRENT_DESKTOP", "Cutefish"), ':',
                                      strings::SplitBehavior::SkipEmptyParts);
    const auto execList = autostartEntries(dirs, desktops);

    // Autostart entries are applications: the runtime starts them, and keeps
    // them quittable like any other application.
    for (const std::string &exec : execList) {
        std::vector<std::string> command = splitCommand(exec);

        // Autostart entries take no files, so their field codes expand to
        // nothing rather than to a literal "%U" argument.
        std::erase_if(command, [](const std::string &argument) {
            return argument.size() == 2 && argument.starts_with('%');
        });

        if (command.empty())
            continue;

        if (isStartedBySession(std::filesystem::path(command.front()).filename().string()))
            continue;

        runtime::launchCommand(command);
    }
}
