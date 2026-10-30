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

    // The first call activates UPower if it is not running yet.
    const dbus::Reply properties = m_upower.getAll();
    if (properties.isError())
        return;
    const dbus::VariantMap values = properties.value<dbus::VariantMap>().value_or(dbus::VariantMap());
    m_onBattery = flag(values, "OnBattery", false);
    m_hasLid = flag(values, "LidIsPresent", false);
    m_lidClosed = flag(values, "LidIsClosed", false);

    const dbus::Reply devices = m_upower.call("EnumerateDevices");
    const auto paths = devices.value<std::vector<dbus::ObjectPath>>();
    if (!paths) {
        CF_WARNING(services::log()) << "Cannot list UPower devices:" << devices.error().name;
        return;
    }
    for (const dbus::ObjectPath &path : *paths)
        addBattery(path.path);
}

PowerSupply::~PowerSupply() = default;

void PowerSupply::readFlags(const dbus::VariantMap &values)
{
    const bool onBattery = flag(values, "OnBattery", m_onBattery);
    const bool lidClosed = flag(values, "LidIsClosed", m_lidClosed);

    if (onBattery != m_onBattery) {
        m_onBattery = onBattery;
        powerSourceChanged.emit();
    }
    if (lidClosed != m_lidClosed) {
        m_lidClosed = lidClosed;
        lidChanged.emit();
    }
}

void PowerSupply::addBattery(const std::string &path)
{
    if (m_batteries.contains(path))
        return;

    // A battery of the computer, not of a mouse, a phone or a UPS; the same
    // test UPower's display device sums the batteries up by.
    const dbus::Proxy device(m_system, upower::service, path, upower::deviceInterface);
    const dbus::VariantMap values = device.getAll().value<dbus::VariantMap>().value_or(dbus::VariantMap());
    const auto type = values.find("Type");
    if (type == values.end() || type->second.get<uint32_t>().value_or(0) != upower::batteryType
        || !flag(values, "PowerSupply", false))
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
