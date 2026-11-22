#ifndef APPRUNTIME_H
#define APPRUNTIME_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "base/signal.h"
#include "base/timer.h"
#include "common/environment.h"

/**
 * Owns every application start of the session: nothing else in the desktop
 * spawns applications, so the runtime is the single place that knows which
 * application a process belongs to and can therefore quit it again.
 *
 * startProcess() is the only place that actually creates a process.
 */
class AppRuntime
{
public:
    AppRuntime();

    // What every launched application gets, apart from its activation token.
    static cutefish::core::Environment applicationEnvironment();

    uint32_t launchApplication(const std::string &appId, const std::vector<std::string> &arguments,
                               const std::string &activationToken = {});
    uint32_t launchCommand(const std::string &appId, const std::vector<std::string> &command,
                           const std::string &workingDirectory, const std::string &activationToken = {});
    bool quitApplication(const std::string &appId);
    bool quitAll();
    bool quitByPid(uint32_t pid);
    bool forceQuitByPid(uint32_t pid);
    bool isRunning(const std::string &appId) const;
    std::vector<std::string> runningApplications() const;
    std::vector<uint32_t> pidsForApplication(const std::string &appId) const;

    // Gets a bundle while nothing runs bundles.
    std::function<void(const std::string &path, const std::string &activationToken)> offerBundleRunner;

    cutefish::base::Signal<const std::string &, uint32_t> applicationLaunched;
    cutefish::base::Signal<const std::string &, uint32_t> applicationQuit;

private:
    struct Instance {
        std::string appId;
        // Kernel start time of the process; a recycled pid has a different one.
        uint64_t startTime = 0;
    };

    uint32_t startProcess(const std::string &appId, const std::vector<std::string> &command,
                          const std::string &workingDirectory, const std::string &activationToken);
    bool terminate(uint32_t pid);
    void reap();

    static bool isAlive(uint32_t pid);
    static bool isSafeTarget(uint32_t pid);
    static bool isOwnedByUser(uint32_t pid);
    static uint32_t parentPid(uint32_t pid);
    static uint64_t startTime(uint32_t pid);

    // pid -> instance
    std::map<uint32_t, Instance> m_instances;
    cutefish::base::Timer m_reaper;
};

#endif // APPRUNTIME_H
