// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "application.h"

#include "base/command_line.h"
#include "base/i18n.h"
#include "base/locale.h"
#include "base/paths.h"
#include "base/process.h"
#include "base/session_language.h"
#include "base/strings.h"
#include "common/environment.h"
#include "common/language_environment.h"
#include "logs.h"
#include "power.h"

// STL
#include <filesystem>
#include <optional>

// System
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace base = cutefish::base;
namespace dbus = cutefish::base::dbus;
using namespace std::chrono_literals;

static std::optional<std::vector<std::string>> getSystemdEnvironment()
{
    dbus::Proxy manager(dbus::Connection::session(), "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager");
    // Make sure the returned type is correct.
    return manager.property<std::vector<std::string>>("Environment");
}

static bool isShellVariable(const std::string &name)
{
    return name == "_" || name.starts_with("SHLVL");
}

static bool isSessionVariable(const std::string &name)
{
    // Check is variable is specific to session.
    return name == "DISPLAY" || name == "XAUTHORITY" || name == "WAYLAND_DISPLAY"
            || name == "WAYLAND_SOCKET" || //
            name.starts_with("XDG_");
}

static bool isInputMethodModuleVariable(const std::string &name)
{
    return name == "GTK_IM_MODULE" || name == "QT_IM_MODULE";
}

static bool isEmptyVariable(const char *name)
{
    const char *value = std::getenv(name);
    return !value || !*value;
}

static void setEnvironmentVariable(const std::string &name, const std::string &value)
{
    const char *current = std::getenv(name.c_str());
    if (!current || value != current)
        setenv(name.c_str(), value.c_str(), 1);
}

static int s_terminationSocket[2] = {-1, -1};

static void onTerminationSignal(int)
{
    const int savedErrno = errno;
    const char byte = 1;
    [[maybe_unused]] const ssize_t written = ::write(s_terminationSocket[1], &byte, 1);
    errno = savedErrno;
}

Application::Application(int argc, char **argv, base::EventLoop &loop)
    : m_loop(loop)
    , m_processManager(std::make_unique<ProcessManager>())
{
    endSessionOnTerminationSignals();

    // connect to D-Bus and register as an object:
    exportSession();

    base::CommandLineParser parser;
    parser.setApplicationDescription("Cutefish Session");
    parser.addHelpOption();

    parser.process(argc, argv);

    createConfigDirectory();
    initLanguage();

    initEnvironments();

    if (!syncDBusEnvironment()) {
        // Startup error
        CF_WARNING(sessionLog()) << "Could not sync environment to dbus.";
    }

    // Import systemd's environment only after ours reached D-Bus; the other
    // order applies LANG and LC_* in the wrong order.
    unsetenv("LC_ALL");
    importSystemdEnvironment();
    // The language the session runs in is known only now.
    base::Locale::setProcessLocale();
    cutefish::i18n::setDomain("cutefish-core");

    m_startTimer.setSingleShot(true);
    m_startTimer.onTimeout = [this] {
        m_processManager->start();
    };
    m_startTimer.start(100ms);
}

Application::~Application()
{
    // Deleted here, while the loop still exists, the process manager stops
    // what it started and removes its Xwayland readiness directory.
    m_processManager.reset();
    if (m_terminationWatch)
        m_loop.unwatch(m_terminationWatch);
}

void Application::reboot()
{
    m_processManager->endAfterApplications(ProcessManager::Ending::Restart, [this] {
        const bool rebooting = Power::reboot();
        if (rebooting)
            m_loop.quit(0);
        return rebooting;
    });
}

void Application::powerOff()
{
    m_processManager->endAfterApplications(ProcessManager::Ending::Shutdown, [this] {
        const bool poweringOff = Power::powerOff();
        if (poweringOff)
            m_loop.quit(0);
        return poweringOff;
    });
}

bool Application::suspend()
{
    return Power::suspend();
}

void Application::exportSession()
{
    dbus::Connection *bus = dbus::Connection::session(&m_loop);
    if (!bus)
        return;

    m_sessionObject = std::make_unique<dbus::Object>(bus, "/Session", "org.cutefish.Session");
    m_sessionObject->addMethod("logout", [this] { logout(); });
    m_sessionObject->addMethod("reboot", [this] { reboot(); });
    m_sessionObject->addMethod("powerOff", [this] { powerOff(); });
    m_sessionObject->addMethod("suspend", [this] { return suspend(); }, {{}, {"result"}});
    m_sessionObject->addMethod("startDesktopProcess", [this] { startDesktopProcess(); });
    m_sessionObject->publish();
    bus->requestName("org.cutefish.Session");
    m_language = std::make_unique<base::SessionLanguage>(bus);
}

void Application::endSessionOnTerminationSignals()
{
    // cutefish-dm and systemd end a session with SIGTERM; dying of it would leave
    // the compositor and services holding the Wayland socket against the next login.
    // A handler, unlike signalfd's blocked mask, is not inherited by child processes.
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, s_terminationSocket)
        != 0) {
        CF_WARNING(sessionLog()) << "Cannot handle termination signals";
        return;
    }
    m_terminationWatch = m_loop.watch(s_terminationSocket[0], EPOLLIN, [this](uint32_t) {
        m_loop.unwatch(m_terminationWatch);
        m_terminationWatch = 0;
        m_processManager->endSession();
    });
    struct sigaction action = {};
    action.sa_handler = onTerminationSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (const int number : {SIGTERM, SIGINT, SIGHUP})
        ::sigaction(number, &action, nullptr);
}

void Application::initEnvironments()
{
    const std::string home = base::paths::home();

    // Set defaults
    if (isEmptyVariable("XDG_DATA_HOME"))
        setenv("XDG_DATA_HOME", (home + "/.local/share").c_str(), 1);
    if (isEmptyVariable("XDG_DESKTOP_DIR"))
        setenv("XDG_DESKTOP_DIR", (home + "/Desktop").c_str(), 1);
    if (isEmptyVariable("XDG_CONFIG_HOME"))
        setenv("XDG_CONFIG_HOME", (home + "/.config").c_str(), 1);
    if (isEmptyVariable("XDG_CACHE_HOME"))
        setenv("XDG_CACHE_HOME", (home + "/.cache").c_str(), 1);
    if (isEmptyVariable("XDG_DATA_DIRS"))
        setenv("XDG_DATA_DIRS", "/usr/local/share/:/usr/share/", 1);
    if (isEmptyVariable("XDG_CONFIG_DIRS"))
        setenv("XDG_CONFIG_DIRS", "/etc/xdg", 1);

    // Environment
    setenv("DESKTOP_SESSION", "Cutefish", 1);
    setenv("XDG_CURRENT_DESKTOP", "Cutefish", 1);
    setenv("XDG_SESSION_DESKTOP", "Cutefish", 1);

    // GLib's plain-text backend: cutefish-services writes the GTK appearance keys into
    // ~/.config/glib-2.0/settings/keyfile, and no dconf daemon is needed.
    setenv("GSETTINGS_BACKEND", "keyfile", 1);

    // Qt
    if (isEmptyVariable("WAYLAND_DISPLAY"))
        setenv("WAYLAND_DISPLAY", "cutefish-0", 1);
    setenv("QT_QPA_PLATFORMTHEME", "cutefish", 1);

    // ref:
    // https://stackoverflow.com/questions/34399993/qml-performance-issue-when-updating-an-item-in-presence-of-many-non-overlapping
    setenv("QT_QPA_UPDATE_IDLE_TIME", "10", 1);

    // im-config's /etc/profile.d script exports these for Wayland sessions; they would bypass
    // the compositor's text-input protocols. XMODIFIERS stays for Xwayland clients.
    unsetenv("GTK_IM_MODULE");
    unsetenv("QT_IM_MODULE");
}

void Application::initLanguage()
{
    cutefish::core::Environment environment = cutefish::core::processEnvironment();
    cutefish::core::language::apply(environment);
    for (const auto &[name, value] : environment) {
        if (name == "LANG" || name == "LANGUAGE" || name.starts_with("LC_"))
            setenv(name.c_str(), value.c_str(), 1);
    }
    // Export an empty value to clear LC_ALL in an already-running D-Bus daemon.
    setenv("LC_ALL", "", 1);
}

bool Application::syncDBusEnvironment()
{
    int exitCode = 0;

    // At this point all environment variables are set, let's send it to the DBus session server to
    // update the activation environment
    if (!base::paths::findExecutable("dbus-update-activation-environment").empty())
        exitCode = runSync("dbus-update-activation-environment", {"--systemd", "--all"});

    // --all only adds variables, so values exported by an earlier login stay in systemd.
    const std::string systemctl = base::paths::findExecutable("systemctl");
    if (!systemctl.empty())
        runSync(systemctl, {"--user", "unset-environment", "GTK_IM_MODULE", "QT_IM_MODULE", "LC_ALL"});

    return exitCode == 0;
}

// Import systemd user environment.
// Systemd read ~/.config/environment.d which applies to all systemd user unit.
// But it won't work if cutefishDE is not started by systemd.
void Application::importSystemdEnvironment()
{
    const auto environment = getSystemdEnvironment();
    if (!environment)
        return;

    for (const std::string &env : *environment) {
        const size_t idx = env.find('=');
        if (idx == std::string::npos || idx == 0)
            continue;

        const std::string name = env.substr(0, idx);
        if (isShellVariable(name) || isSessionVariable(name)
            || isInputMethodModuleVariable(name) || name == "LC_ALL") {
            continue;
        }
        setEnvironmentVariable(name, env.substr(idx + 1));
    }
}

void Application::createConfigDirectory()
{
    const std::string configDir = base::paths::configHome();

    std::error_code error;
    std::filesystem::create_directories(configDir, error);
    if (!std::filesystem::is_directory(configDir, error))
        CF_WARNING(sessionLog()) << "Could not create config directory XDG_CONFIG_HOME: " << configDir;
}

int Application::runSync(const std::string &program, const std::vector<std::string> &args)
{
    base::Process p(&m_loop);

    p.setOutputForwarded(true);
    int exitCode = 0;
    p.onFinished = [&exitCode](int code, bool) {
        exitCode = code;
    };
    if (!p.start(program, args)) {
        CF_WARNING(sessionLog()) << program << "could not be started";
        return 255;
    }
    p.waitForFinished(std::chrono::hours(24));

    if (exitCode)
        CF_WARNING(sessionLog()) << program << cutefish::base::strings::join(args, " ") << "exited with code" << exitCode;

    return exitCode;
}
