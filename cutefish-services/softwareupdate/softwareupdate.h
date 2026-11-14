// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef SOFTWAREUPDATE_H
#define SOFTWAREUPDATE_H

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "base/dbus.h"
#include "base/settings.h"
#include "base/signal.h"
#include "base/timer.h"
#include "cutefish/packagekit/updates.h"
#include "notification.h"

// System updates through PackageKit, installed while the session runs:
// checked once a day and announced when there are some, installed from
// Settings, which reads and drives them through
// org.cutefish.Services.SoftwareUpdate. After an install it suggests a
// restart until the next boot. Without PackageKit it does nothing.
class SoftwareUpdate
{
public:
    // PackageKit on `system`; the object, the notifications and the restart
    // on `session`. `settings` keeps the time of the last check and the boot
    // that installed updates.
    SoftwareUpdate(cutefish::base::dbus::Connection *system, cutefish::base::dbus::Connection *session,
                   cutefish::base::Settings &settings);
    ~SoftwareUpdate();

    SoftwareUpdate(const SoftwareUpdate &) = delete;
    SoftwareUpdate &operator=(const SoftwareUpdate &) = delete;

    bool publish();

    // Checks in the background once a day has passed since the last check
    // that went through; the service asks every hour.
    void checkIfDue();

    // Opens Settings on the page, for a click on a notification.
    std::function<void()> openSettings;
    // This boot's id; a test sets its own.
    std::function<std::string()> bootId;

private:
    void start();
    void updatesChanged();
    void checkFinished(bool success);
    void installed();
    void notifyUpdates();
    void setupNotification(Notification &notification);
    bool restartSuggested() const;
    std::vector<std::tuple<std::string, std::string, std::string>> updateList() const;
    std::string stateName() const;

    cutefish::base::dbus::Connection *m_system;
    cutefish::base::dbus::Connection *m_session;
    cutefish::base::Settings &m_settings;
    std::unique_ptr<cutefish::packagekit::Availability> m_availability;
    std::unique_ptr<cutefish::packagekit::Updates> m_updates;
    std::unique_ptr<cutefish::base::dbus::Object> m_object;
    Notification m_updatesNotification;
    Notification m_installedNotification;
    std::vector<cutefish::base::Subscription> m_subscriptions;
    cutefish::base::Timer m_timer;
    bool m_backgroundCheck = false;
    // The list goes out with a change of state only, not with every step
    // of an update.
    std::string m_announcedState;
};

#endif // SOFTWAREUPDATE_H
