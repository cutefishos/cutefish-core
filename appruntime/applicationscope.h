#pragma once

#include <cstdint>
#include <string>

// Transient systemd scopes for launched applications, named
// app-cutefish-<AppID>-<pid>.scope as the desktop entry specification asks,
// which is how xdg-desktop-portal learns the app id of a host application.
namespace ApplicationScope {

// The unit name, with the app id escaped the way systemd escapes unit names.
std::string unitName(const std::string &appId, uint32_t pid);

// Moves the process into its own scope under the user manager. Failure is
// logged and otherwise ignored: the application runs either way.
void adopt(const std::string &appId, uint32_t pid);

// Sends SIGKILL to every process left in the scope. A scope that already went
// with its last process is no failure.
void killAll(const std::string &unitName);

// Asks the user manager to stop every application scope that is up: the
// ones adopt() made and those applications moved themselves into, which
// the runtime no longer tracks. systemd ends their processes as the end of
// a session does, SIGTERM first and SIGKILL after its timeout.
void stopAll();

} // namespace ApplicationScope
