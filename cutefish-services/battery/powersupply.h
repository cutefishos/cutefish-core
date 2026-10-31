// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef POWERSUPPLY_H
#define POWERSUPPLY_H

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/dbus.h"
#include "base/settings.h"
#include "base/signal.h"

class Battery;

// What UPower knows of the power source and the lid, and the computer's batteries.
class PowerSupply
{
public:
    // UPower on `system`; the batteries, taken together, are published on `session`.
    PowerSupply(cutefish::base::dbus::Connection *system, cutefish::base::dbus::Connection *session,
                cutefish::base::Settings &settings);
    ~PowerSupply();

    PowerSupply(const PowerSupply &) = delete;
    PowerSupply &operator=(const PowerSupply &) = delete;

    bool onBattery() const { return m_onBattery; }
    bool hasLid() const { return m_hasLid; }
    bool lidClosed() const { return m_lidClosed; }

    cutefish::base::Signal<> powerSourceChanged;
    cutefish::base::Signal<> lidPresenceChanged;
    cutefish::base::Signal<> lidChanged;

private:
    void load();
    void readFlags(const cutefish::base::dbus::VariantMap &values);
    bool isBattery(const std::string &path) const;
    void addBattery(const std::string &path);
    void removeBattery(const std::string &path);
    void publishBatteries();

    cutefish::base::dbus::Connection *m_system;
    cutefish::base::dbus::Connection *m_session;
    cutefish::base::Settings &m_settings;
    cutefish::base::dbus::Proxy m_upower;
    cutefish::base::dbus::PendingCall m_activation;
    cutefish::base::dbus::NameWatch m_upowerWatch;
    std::string m_upowerOwner;
    std::vector<cutefish::base::dbus::Match> m_matches;
    // The UPower device paths of the batteries that power the computer.
    std::set<std::string> m_batteries;
    std::unique_ptr<Battery> m_published;
    bool m_onBattery = false;
    bool m_hasLid = false;
    bool m_lidClosed = false;
};

#endif // POWERSUPPLY_H
