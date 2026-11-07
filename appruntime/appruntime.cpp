#include "appruntime.h"
#include "applicationscope.h"
#include "chromiumswitches.h"

#include "base/application_registry.h"
#include "base/event_loop.h"
#include "base/file.h"
#include "base/logging.h"
#include "base/paths.h"
#include "base/strings.h"
#include "common/detached_process.h"
#include "common/language_environment.h"
#include "common/proxy_settings.h"

#include <algorithm>
#include <filesystem>

#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace base = cutefish::base;
namespace strings = cutefish::base::strings;
using namespace std::chrono_literals;

static const base::LogCategory kLog("cutefish.appruntime");

static constexpr auto kTerminateTimeout = 5000ms;
// Taken at start: once the session is gone we belong to whatever adopted us.
static const uint32_t kSessionPid = static_cast<uint32_t>(::getppid());

AppRuntime::AppRuntime()
{
    // Instances run in sessions of their own; their ends are polled for here.
    m_reaper.setInterval(2000ms);
    m_reaper.onTimeout = [this] {
        reap();
    };
}

cutefish::core::Environment AppRuntime::applicationEnvironment()
{
    cutefish::core::Environment environment = cutefish::core::processEnvironment();
    cutefish::core::language::apply(environment);
    cutefish::core::proxy::apply(environment);
    environment.erase("XDG_ACTIVATION_TOKEN");
    return environment;
}

uint32_t AppRuntime::launchApplication(const std::string &appId, const std::vector<std::string> &arguments,
                                       const std::string &activationToken)
{
    if (appId.empty())
        return 0;

    // Callers know an application either by its entry id or by the path of
    // the desktop file they read.
    base::ApplicationRegistry &registry = base::ApplicationRegistry::instance();
    auto entry = registry.byId(appId);
    if (!entry && appId.find('/') != std::string::npos) {
        entry = registry.byPath(appId);
        if (!entry)
            entry = registry.readBundle(appId);

        if (!entry) {
            // The file name up to its last dot.
            std::string name = std::filesystem::path(appId).filename().string();
            const size_t dot = name.rfind('.');
            if (dot != std::string::npos)
                name.resize(dot);
            entry = registry.byId(name);
        }
    }

    if (!entry) {
        CF_WARNING(kLog) << "AppRuntime: no desktop entry for" << appId;
        return 0;
    }

    std::vector<std::string> command = entry->commandForArguments(arguments);
    if (command.empty())
        return 0;

    if (entry->terminal) {
        std::string terminal = base::paths::findExecutable("x-terminal-emulator");
        if (terminal.empty())
            terminal = base::paths::findExecutable("xterm");

        if (terminal.empty()) {
            CF_WARNING(kLog) << "AppRuntime: no terminal emulator for" << appId;
        } else {
            command.insert(command.begin(), {terminal, "-e"});
        }
    }

    return startProcess(entry->id, command, entry->workingDirectory, activationToken);
}

uint32_t AppRuntime::launchCommand(const std::string &appId, const std::vector<std::string> &command,
                                   const std::string &workingDirectory, const std::string &activationToken)
{
    if (command.empty() || command.front().empty())
        return 0;

    std::string id = appId;
    if (id.empty())
        id = std::filesystem::path(command.front()).filename().string();

    return startProcess(id, command, workingDirectory, activationToken);
}

uint32_t AppRuntime::startProcess(const std::string &appId, const std::vector<std::string> &command,
                                  const std::string &workingDirectory, const std::string &activationToken)
{
    cutefish::core::Environment environment = applicationEnvironment();
    const std::vector<std::string> arguments = withChromiumSwitches(command);
    if (!activationToken.empty())
        environment["XDG_ACTIVATION_TOKEN"] = activationToken;

    const pid_t started = cutefish::core::startDetached(arguments, workingDirectory, environment);
    if (started <= 0) {
        CF_WARNING(kLog) << "AppRuntime: failed to start" << strings::join(arguments, " ");
        return 0;
    }
    const uint32_t pid = static_cast<uint32_t>(started);

    // Only an id the registry knows names the scope: portals read the app id
    // back from it, and a bare executable name would pass for one.
    if (const auto entry = base::ApplicationRegistry::instance().byId(appId))
        ApplicationScope::adopt(entry->id, pid);
    m_instances[pid] = Instance{appId, startTime(pid)};

    if (!m_reaper.isActive())
        m_reaper.start();

    applicationLaunched.emit(appId, pid);

    return pid;
}

bool AppRuntime::quitApplication(const std::string &appId)
{
    if (appId.empty())
        return false;

    bool result = false;
    for (const uint32_t pid : pidsForApplication(appId))
        result |= terminate(pid);

    return result;
}

// Logout path: one call instead of one per application.
bool AppRuntime::quitAll()
{
    std::vector<uint32_t> pids;
    for (const auto &[pid, instance] : m_instances)
        pids.push_back(pid);

    bool result = false;
    for (const uint32_t pid : pids)
        result |= terminate(pid);

    // An application that moved to a scope of its own is no longer tracked;
    // the user manager stops every application scope, those included.
    ApplicationScope::stopAll();
    return result;
}

bool AppRuntime::quitByPid(uint32_t pid)
{
    return terminate(pid);
}

// The compositor has already ended the process itself; what is left is the
// rest of its application, such as helpers it started.
bool AppRuntime::forceQuitByPid(uint32_t pid)
{
    const auto it = m_instances.find(pid);
    if (it == m_instances.end())
        return false;
    // The scope exists only for an id the registry knows; see startProcess().
    const auto entry = base::ApplicationRegistry::instance().byId(it->second.appId);
    if (!entry)
        return false;
    ApplicationScope::killAll(ApplicationScope::unitName(entry->id, pid));
    return true;
}

bool AppRuntime::terminate(uint32_t pid)
{
    if (!isSafeTarget(pid) || !isAlive(pid) || !isOwnedByUser(pid))
        return false;

    // Read before SIGTERM: an exited process reads as 0, which would let the
    // delayed SIGKILL hit whatever has recycled the pid.
    const uint64_t started = startTime(pid);

    // Never signal a process group: the group is not ours to interpret, and a
    // wrong one takes down the whole login session.
    ::kill(static_cast<pid_t>(pid), SIGTERM);

    base::EventLoop::current()->startTimer(kTerminateTimeout, false, [pid, started] {
        if (isAlive(pid) && isOwnedByUser(pid) && (started == 0 || startTime(pid) == started))
            ::kill(static_cast<pid_t>(pid), SIGKILL);
    });

    if (!m_reaper.isActive())
        m_reaper.start();

    return true;
}

bool AppRuntime::isRunning(const std::string &appId) const
{
    return !pidsForApplication(appId).empty();
}

std::vector<std::string> AppRuntime::runningApplications() const
{
    std::vector<std::string> result;
    for (const auto &[pid, instance] : m_instances) {
        if (std::find(result.begin(), result.end(), instance.appId) == result.end())
            result.push_back(instance.appId);
    }
    return result;
}

std::vector<uint32_t> AppRuntime::pidsForApplication(const std::string &appId) const
{
    std::vector<uint32_t> result;
    for (const auto &[pid, instance] : m_instances) {
        if (instance.appId == appId)
            result.push_back(pid);
    }
    return result;
}

void AppRuntime::reap()
{
    std::vector<uint32_t> pids;
    for (const auto &[pid, instance] : m_instances)
        pids.push_back(pid);

    for (const uint32_t pid : pids) {
        const Instance instance = m_instances[pid];

        // A live pid that no longer belongs to the process we started has
        // been recycled: the instance is gone all the same. An unknown start
        // time only leaves liveness to go by.
        if (isAlive(pid) && (instance.startTime == 0 || startTime(pid) == instance.startTime))
            continue;

        m_instances.erase(pid);
        applicationQuit.emit(instance.appId, pid);
    }

    if (m_instances.empty())
        m_reaper.stop();
}

bool AppRuntime::isAlive(uint32_t pid)
{
    if (pid == 0)
        return false;

    // The runtime collects its children as they end, so a live pid is never
    // a zombie here.
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

bool AppRuntime::isSafeTarget(uint32_t pid)
{
    if (pid <= 1)
        return false;

    // Refuse anything that would take the session down with the application,
    // including the session's own components: they are its children, as we are.
    return pid != static_cast<uint32_t>(::getpid())
        && pid != static_cast<uint32_t>(::getpgrp())
        && pid != static_cast<uint32_t>(::getsid(0))
        && parentPid(pid) != kSessionPid;
}

// The fields after comm, which is parenthesised and may itself contain spaces;
// index 0 is field 3.
static std::vector<std::string> statFields(uint32_t pid)
{
    const auto stat = base::file::read("/proc/" + std::to_string(pid) + "/stat");
    if (!stat)
        return {};

    const std::string line = stat->substr(0, stat->find('\n'));
    const size_t commEnd = line.rfind(')');
    if (commEnd == std::string::npos || commEnd + 2 > line.size())
        return {};

    return strings::splitWhitespace(line.substr(commEnd + 2));
}

// Field 4 of /proc/pid/stat.
uint32_t AppRuntime::parentPid(uint32_t pid)
{
    const std::vector<std::string> fields = statFields(pid);
    return fields.size() < 2 ? 0 : strings::toNumber<uint32_t>(fields.at(1)).value_or(0);
}

// Field 22 of /proc/pid/stat, in clock ticks since boot.
uint64_t AppRuntime::startTime(uint32_t pid)
{
    const std::vector<std::string> fields = statFields(pid);
    // stat field 22 is the 20th one after comm.
    if (fields.size() < 20)
        return 0;

    return strings::toNumber<uint64_t>(fields.at(19)).value_or(0);
}

bool AppRuntime::isOwnedByUser(uint32_t pid)
{
    struct stat info {};
    return ::stat(("/proc/" + std::to_string(pid)).c_str(), &info) == 0 && info.st_uid == ::getuid();
}
