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

// System updates through PackageKit's offline updates: checked once a day,
// announced when there are some, downloaded and installed at a restart from
// Settings, which reads and drives them through
// org.cutefish.Services.SoftwareUpdate. After the boot that installed them
// it reports the outcome once. Without PackageKit it does nothing.
class SoftwareUpdate
{
public:
    // PackageKit on `system`; the object, the notifications and the restart
    // on `session`. `settings` keeps the time of the last check.
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

private:
    void start();
    void updatesChanged();
    void checkFinished(bool success);
    void notifyUpdates();
    void notifyResult(const cutefish::packagekit::Updates::OfflineResult &result);
    void restartAndUpdate(cutefish::base::dbus::MethodCall &call);
    void setupNotification(Notification &notification);
    std::vector<std::tuple<std::string, std::string, std::string>> updateList() const;
    std::string stateName() const;

    cutefish::base::dbus::Connection *m_system;
    cutefish::base::dbus::Connection *m_session;
    cutefish::base::Settings &m_settings;
    std::unique_ptr<cutefish::packagekit::Availability> m_availability;
    std::unique_ptr<cutefish::packagekit::Updates> m_updates;
    std::unique_ptr<cutefish::base::dbus::Object> m_object;
    Notification m_updatesNotification;
    Notification m_resultNotification;
    std::vector<cutefish::base::Subscription> m_subscriptions;
    cutefish::base::Timer m_timer;
    cutefish::base::dbus::PendingCall m_resultCall;
    cutefish::base::dbus::PendingCall m_triggerCall;
    bool m_backgroundCheck = false;
    // The list goes out with a change of state only, not with every step
    // of a download.
    cutefish::packagekit::Updates::State m_announcedState = cutefish::packagekit::Updates::State::Unknown;
};

#endif // SOFTWAREUPDATE_H
