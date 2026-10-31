// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "powersupply.h"
#include "battery.h"
#include "upower.h"

#include "log.h"

using namespace cutefish::base;

static bool flag(const dbus::VariantMap &values, const char *name, bool fallback)
{
    const auto it = values.find(name);
    return it == values.end() ? fallback : it->second.get<bool>().value_or(false);
}

PowerSupply::PowerSupply(dbus::Connection *system, dbus::Connection *session, Settings &settings)
    : m_system(system)
    , m_session(session)
    , m_settings(settings)
    , m_upower(system, upower::service, upower::path, upower::interface)
{
    if (!system)
        return;

    m_matches.push_back(m_upower.onSignal("DeviceAdded", [this](const dbus::Message &message) {
        if (const auto path = message.argument<dbus::ObjectPath>())
            addBattery(path->path);
    }));
    m_matches.push_back(m_upower.onSignal("DeviceRemoved", [this](const dbus::Message &message) {
        if (const auto path = message.argument<dbus::ObjectPath>())
            removeBattery(path->path);
    }));
    m_matches.push_back(m_upower.onPropertiesChanged(
            [this](const dbus::VariantMap &changed, const std::vector<std::string> &invalidated) {
                if (!invalidated.empty()) {
                    readFlags(m_upower.getAll().value<dbus::VariantMap>().value_or(changed));
                    return;
                }
                readFlags(changed);
            }));

    // UPower may start after this, or stop and come back; each new owner is read afresh.
    m_upowerWatch = system->watchName(upower::service, [this](const std::string &owner) {
        if (owner == m_upowerOwner)
            return;
        m_upowerOwner = owner;

        if (owner.empty()) {
            m_batteries.clear();
            publishBatteries();
            return;
        }
        load();
    });

    // Activates UPower if it is not running yet; the watch reads it once it is.
    m_activation = m_upower.getAllAsync([](const dbus::Reply &reply) {
        if (reply.isError())
            CF_WARNING(services::log()) << "Cannot start UPower:" << reply.error().name;
    });
}

PowerSupply::~PowerSupply() = default;

void PowerSupply::load()
{
    const dbus::Reply properties = m_upower.getAll();
    if (properties.isError()) {
        CF_WARNING(services::log()) << "Cannot read UPower:" << properties.error().name;
        return;
    }
    readFlags(properties.value<dbus::VariantMap>().value_or(dbus::VariantMap()));

    const dbus::Reply devices = m_upower.call("EnumerateDevices");
    const auto paths = devices.value<std::vector<dbus::ObjectPath>>();
    if (!paths) {
        CF_WARNING(services::log()) << "Cannot list UPower devices:" << devices.error().name;
        return;
    }

    std::set<std::string> batteries;
    for (const dbus::ObjectPath &path : *paths) {
        if (isBattery(path.path))
            batteries.insert(path.path);
    }
    if (batteries == m_batteries)
        return;

    m_batteries = std::move(batteries);
    publishBatteries();
}

void PowerSupply::readFlags(const dbus::VariantMap &values)
{
    const bool onBattery = flag(values, "OnBattery", m_onBattery);
    const bool hasLid = flag(values, "LidIsPresent", m_hasLid);
    const bool lidClosed = flag(values, "LidIsClosed", m_lidClosed);

    if (onBattery != m_onBattery) {
        m_onBattery = onBattery;
        powerSourceChanged.emit();
    }
    if (hasLid != m_hasLid) {
        m_hasLid = hasLid;
        lidPresenceChanged.emit();
    }
    if (lidClosed != m_lidClosed) {
        m_lidClosed = lidClosed;
        lidChanged.emit();
    }
}

bool PowerSupply::isBattery(const std::string &path) const
{
    // A battery of the computer, not of a mouse, a phone or a UPS; the same
    // test UPower's display device sums the batteries up by.
    const dbus::Proxy device(m_system, upower::service, path, upower::deviceInterface);
    const dbus::VariantMap values = device.getAll().value<dbus::VariantMap>().value_or(dbus::VariantMap());
    const auto type = values.find("Type");
    return type != values.end() && type->second.get<uint32_t>().value_or(0) == upower::batteryType
            && flag(values, "PowerSupply", false);
}

void PowerSupply::addBattery(const std::string &path)
{
    if (m_batteries.contains(path) || !isBattery(path))
        return;

    m_batteries.insert(path);
    publishBatteries();
}

void PowerSupply::removeBattery(const std::string &path)
{
    if (m_batteries.erase(path))
        publishBatteries();
}

void PowerSupply::publishBatteries()
{
    if (!m_session)
        return;

    if (m_batteries.empty()) {
        m_published.reset();
        return;
    }

    if (!m_published) {
        auto battery = std::make_unique<Battery>(m_system, m_settings);
        if (!battery->publish(m_session))
            return;
        m_published = std::move(battery);
    }
    m_published->setBatteries({m_batteries.begin(), m_batteries.end()});
}
