// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "battery.h"
#include "upower.h"

#include <chrono>
#include <cmath>
#include <vector>

#include "base/i18n.h"

using namespace cutefish::base;
using cutefish::i18n::format;
using cutefish::i18n::tr;
using cutefish::i18n::trNoop;

static constexpr auto s_lastChargedPercentKey = "Battery/LastChargedPercent";
static constexpr auto s_lastChargedSecsKey = "Battery/LastChargedSecs";
static constexpr auto s_displayDevice = "/org/freedesktop/UPower/devices/DisplayDevice";

static int64_t secondsSinceEpoch()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
}

template<typename T>
static T property(const dbus::VariantMap &properties, const char *name)
{
    const auto it = properties.find(name);
    return it == properties.end() ? T() : it->second.get<T>().value_or(T());
}

Battery::Battery(dbus::Connection *system, Settings &settings)
    : m_system(system)
    , m_settings(settings)
    , m_device(system, upower::service, s_displayDevice, upower::deviceInterface)
{
    if (system) {
        m_propertiesChanged = m_device.onPropertiesChanged(
                [this](const dbus::VariantMap &, const std::vector<std::string> &) {
                    update();
                });

        // UPower has no resume signal of its own; its readings are stale after a sleep.
        const dbus::Proxy logind(system, "org.freedesktop.login1", "/org/freedesktop/login1",
                                 "org.freedesktop.login1.Manager");
        m_prepareForSleep = logind.onSignal("PrepareForSleep", [this](const dbus::Message &message) {
            if (message.argument<bool>().value_or(true))
                return;

            m_device.send("Refresh");
            update();
        });
    }

    m_reading = read();
}

Battery::~Battery() = default;

Battery::Reading Battery::read() const
{
    const dbus::VariantMap properties = m_device.getAll().value<dbus::VariantMap>().value_or(dbus::VariantMap());

    Reading reading;
    reading.state = property<uint32_t>(properties, "State");
    reading.percent = int(std::lround(property<double>(properties, "Percentage")));
    reading.timeToEmpty = property<int64_t>(properties, "TimeToEmpty");
    reading.timeToFull = property<int64_t>(properties, "TimeToFull");
    reading.thresholdEnabled = property<bool>(properties, "ChargeThresholdEnabled");

    // Some UPower versions leave the limit out of the display device.
    for (const auto &[path, enabled] : m_batteryThresholds)
        reading.thresholdEnabled = reading.thresholdEnabled || enabled;
    return reading;
}

Battery::ChargeState Battery::Reading::chargeState() const
{
    switch (upower::State(state)) {
    case upower::State::Charging:
        return Charging;
    case upower::State::Discharging:
        return Discharging;
    case upower::State::FullyCharged:
        return FullyCharged;
    default:
        return NoCharge;
    }
}

int64_t Battery::Reading::remainingTime() const
{
    switch (chargeState()) {
    case Charging:
        return timeToFull;
    case Discharging:
        return timeToEmpty;
    default:
        return -1;
    }
}

std::string Battery::Reading::status() const
{
    switch (chargeState()) {
    case Charging:
    case Discharging: {
        // UPower reports 0 for several seconds after the power source changes,
        // until it trusts the battery's rate again.
        const int64_t time = remainingTime();
        if (time < 60)
            return tr("Calculating…");
        if (chargeState() == Charging)
            return format(tr("{time} until fully charged"), {{"time", formatDuration(time)}});
        return format(tr("{time} remaining"), {{"time", formatDuration(time)}});
    }
    case FullyCharged:
        return tr("Fully charged.");
    case NoCharge:
        break;
    }

    // Plugged in, but a charge limit holds the battery where it is.
    if (upower::State(state) == upower::State::PendingCharge)
        return thresholdEnabled ? format(tr("Charging paused at {percent}%"), {{"percent", std::to_string(percent)}})
                                : tr("Not charging");

    return {};
}

std::string Battery::formatDuration(int64_t seconds)
{
    if (seconds < 60)
        return {};

    const int64_t minutes = seconds / 60;
    const struct {
        int64_t value;
        const char *pattern;
    } parts[] = {
            // TRANSLATORS: days in a short battery time such as "1d 2h 5m".
            {minutes / (60 * 24), trNoop("{n}d")},
            // TRANSLATORS: hours in a short battery time such as "2h 5m".
            {minutes / 60 % 24, trNoop("{n}h")},
            // TRANSLATORS: minutes in a short battery time such as "2h 5m".
            {minutes % 60, trNoop("{n}m")},
    };

    std::string text;
    for (const auto &part : parts) {
        if (part.value == 0)
            continue;
        if (!text.empty())
            text += ' ';
        text += format(tr(part.pattern), {{"n", std::to_string(part.value)}});
    }
    return text;
}

std::string Battery::lastChargedTime() const
{
    // A clock set back also lands under a minute.
    const int64_t elapsed = secondsSinceEpoch() - int64_t(m_lastChargedSecs);
    if (elapsed < 60)
        // TRANSLATORS: when the battery was last fully charged, under a minute ago.
        return tr("now");

    return format(tr("{time} ago"), {{"time", formatDuration(elapsed)}});
}

bool Battery::publish(dbus::Connection *session)
{
    if (!session)
        return false;

    auto object = std::make_unique<dbus::Object>(session, "/org/cutefish/Services/Battery",
                                                 "org.cutefish.Services.Battery");
    object->addMethod("refresh", [this] { m_device.send("Refresh"); });
    object->addProperty("chargeState", [this] { return int32_t(m_reading.chargeState()); });
    object->addProperty("chargePercent", [this] { return int32_t(m_reading.percent); });
    object->addProperty("remainingTime", [this] { return m_reading.remainingTime(); });
    object->addProperty("statusString", [this] { return m_reading.status(); });
    object->addProperty("lastChargedPercent", [this] { return int32_t(m_lastChargedPercent); });
    object->addProperty("lastChargedSecs", [this] { return int64_t(m_lastChargedSecs); });
    object->addProperty("lastChargedTime", [this] { return lastChargedTime(); });
    object->addProperty("batteries", [this] {
        std::vector<dbus::ObjectPath> paths;
        for (const std::string &path : m_batteries)
            paths.push_back({path});
        return paths;
    });
    object->addSignal("chargeStateChanged", "i", {"value"});
    object->addSignal("chargePercentChanged", "i", {"value"});
    object->addSignal("remainingTimeChanged", "x", {"value"});
    object->addSignal("lastChargedPercentChanged", "");
    object->addSignal("lastChargedSecsChanged", "");
    object->addSignal("batteriesChanged", "");
    if (!object->publish())
        return false;

    m_object = std::move(object);
    m_lastChargedPercent = int(m_settings.intValue(s_lastChargedPercentKey, 0));
    m_lastChargedSecs = uint64_t(m_settings.intValue(s_lastChargedSecsKey, 0));
    return true;
}

void Battery::setBatteries(std::vector<std::string> paths)
{
    if (paths == m_batteries)
        return;

    m_batteries = std::move(paths);
    m_batteryMatches.clear();
    m_batteryThresholds.clear();
    for (const std::string &path : m_batteries) {
        const dbus::Proxy device(m_system, upower::service, path, upower::deviceInterface);
        m_batteryThresholds[path] = device.property<bool>("ChargeThresholdEnabled").value_or(false);
        // The display device announces the batteries' other changes itself.
        m_batteryMatches.push_back(device.onPropertiesChanged(
                [this, path](const dbus::VariantMap &changed, const std::vector<std::string> &) {
                    const auto it = changed.find("ChargeThresholdEnabled");
                    if (it == changed.end())
                        return;
                    m_batteryThresholds[path] = it->second.get<bool>().value_or(false);
                    update();
                }));
    }

    if (m_object)
        m_object->emitSignal("batteriesChanged");
    update();
}

void Battery::update()
{
    const Reading before = std::exchange(m_reading, read());
    if (!m_object || before == m_reading)
        return;

    const Reading &now = m_reading;
    if (before.percent != now.percent)
        m_object->emitSignal("chargePercentChanged", int32_t(now.percent));
    if (before.chargeState() != now.chargeState())
        m_object->emitSignal("chargeStateChanged", int32_t(now.chargeState()));

    // Unplugged: remember where the charge stopped.
    const bool wasPluggedIn = before.chargeState() == Charging || before.chargeState() == FullyCharged
            || upower::State(before.state) == upower::State::PendingCharge;
    if (wasPluggedIn && now.chargeState() == Discharging) {
        m_lastChargedPercent = now.percent;
        m_lastChargedSecs = uint64_t(secondsSinceEpoch());
        m_settings.setValue(s_lastChargedPercentKey, m_lastChargedPercent);
        m_settings.setValue(s_lastChargedSecsKey, m_lastChargedSecs);
        m_settings.sync();

        m_object->emitSignal("lastChargedPercentChanged");
        m_object->emitSignal("lastChargedSecsChanged");
    }

    // Also tells statusString readers, which a state change alone can alter.
    if (before.timeToFull != now.timeToFull || before.timeToEmpty != now.timeToEmpty
        || before.status() != now.status()) {
        m_object->emitSignal("remainingTimeChanged", now.remainingTime());
    }
}
