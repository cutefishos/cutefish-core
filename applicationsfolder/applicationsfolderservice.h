// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "base/dbus.h"
#include "base/timer.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Puts applications into the shared Applications folder, and takes them out,
// for users who may not write it, once an administrator agreed. The user
// copies an application into a staging folder of their own inside it, which
// is then published under its name; an application taken out lands in such a
// folder, the user's to move on and then discard.
class ApplicationsFolderService
{
public:
    struct Paths
    {
        std::string folder = "/Applications";
        // Own what is published; the group owns the folder too.
        uint32_t owner = 0;
        std::string group = "sudo";
    };

    ApplicationsFolderService();
    explicit ApplicationsFolderService(Paths paths, std::chrono::milliseconds idleTimeout = std::chrono::seconds(60));
    ~ApplicationsFolderService();

    // Exports org.cutefish.ApplicationsFolder at /org/cutefish/ApplicationsFolder;
    // false when the object cannot be added.
    bool exportTo(cutefish::base::dbus::Connection *bus);

    // The work itself, for the caller with that user id; each answers the
    // path it made, or empty after setting `error`.
    std::string prepare(uint32_t user, std::string *error) const;
    std::string publish(uint32_t user, const std::string &staging, const std::string &name, bool replace,
                        std::string *error) const;
    std::string release(uint32_t user, const std::string &name, std::string *error) const;
    bool discard(uint32_t user, const std::string &staging, std::string *error) const;

    // Nothing called for a while and nothing waiting for an answer.
    std::function<void()> onIdle;

private:
    // Calls `then` with the caller's user id once polkit agreed, asking an
    // administrator when `ask` is set; replies with an error otherwise.
    void withCaller(cutefish::base::dbus::MethodCall &call, bool ask,
                    std::function<void(cutefish::base::dbus::MethodCall &, uint32_t)> then);
    // The staging folder's name when `path` is one the user owns.
    std::string stagingName(uint32_t user, const std::string &path) const;

    Paths m_paths;
    cutefish::base::Timer m_idleTimer;
    cutefish::base::dbus::Connection *m_bus = nullptr;
    std::unique_ptr<cutefish::base::dbus::Object> m_object;
    int m_pending = 0;
};
