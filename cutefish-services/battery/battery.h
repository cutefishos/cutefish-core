// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BATTERY_H
#define BATTERY_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/dbus.h"
#include "base/settings.h"

// The computer's batteries taken together, as UPower's display device sums
// them up, published on the session bus.
class Battery
{
public:
    // The chargeState values of org.cutefish.Services.Battery.
    enum ChargeState { NoCharge, Charging, Discharging, FullyCharged };

    Battery(cutefish::base::dbus::Connection *system, cutefish::base::Settings &settings);
    ~Battery();

    Battery(const Battery &) = delete;
    Battery &operator=(const Battery &) = delete;

    // org.cutefish.Services.Battery at /org/cutefish/Services/Battery.
    bool publish(cutefish::base::dbus::Connection *session);

    // The UPower device paths of the batteries summed up.
    void setBatteries(std::vector<std::string> paths);

    // "2h 5m"; empty under a minute.
    static std::string formatDuration(int64_t seconds);

private:
    struct Reading {
        uint32_t state = 0;
        int percent = 0;
        int64_t timeToEmpty = 0;
        int64_t timeToFull = 0;
        bool thresholdEnabled = false;

        ChargeState chargeState() const;
        // Seconds to full while charging, to empty while discharging, else -1.
        int64_t remainingTime() const;
        std::string status() const;
        bool operator==(const Reading &) const = default;
    };

    Reading read() const;
    void update();
    std::string lastChargedTime() const;

    cutefish::base::dbus::Connection *m_system;
    cutefish::base::Settings &m_settings;
    cutefish::base::dbus::Proxy m_device;
    cutefish::base::dbus::Match m_propertiesChanged;
    cutefish::base::dbus::Match m_prepareForSleep;
    std::unique_ptr<cutefish::base::dbus::Object> m_object;
    Reading m_reading;
    std::vector<std::string> m_batteries;
    std::vector<cutefish::base::dbus::Match> m_batteryMatches;
    // Each battery's ChargeThresholdEnabled, by device path.
    std::map<std::string, bool> m_batteryThresholds;

    int m_lastChargedPercent = 0;
    uint64_t m_lastChargedSecs = 0;
};

#endif // BATTERY_H
