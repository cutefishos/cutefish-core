#include "powermanager.h"

#include "../battery/powersupply.h"
#include "displaypolicy.h"

#include <algorithm>

using namespace cutefish::base;

static constexpr auto s_powerGroup = "Power/";

static int normalizedTimeout(int timeout)
{
    return timeout < 0 ? -1 : std::max(1, timeout);
}

static int readTimeout(const Settings &settings, const std::string &key, int fallback)
{
    return settings.contains(key) ? normalizedTimeout(int(settings.intValue(key))) : fallback;
}

PowerManager::PowerManager(PowerSupply *powerSupply, BrightnessManager *brightness, WaylandIdle *idle,
                           Settings &settings, dbus::Connection *session, dbus::Connection *system)
    : m_settings(settings)
    , m_session(session)
    , m_powerSupply(powerSupply)
    , m_displayPolicy(std::make_unique<DisplayPolicy>(idle, brightness, session, system))
{
    loadSettings();

    m_onBattery = m_powerSupply && m_powerSupply->onBattery();

    if (m_powerSupply) {
        m_subscriptions.push_back(m_powerSupply->powerSourceChanged.connect([this] {
            onPowerSourceChanged();
        }));
        m_subscriptions.push_back(m_powerSupply->lidPresenceChanged.connect([this] {
            m_displayPolicy->setLidPresent(m_powerSupply->hasLid());
        }));
        m_subscriptions.push_back(m_powerSupply->lidChanged.connect([this] {
            onLidChanged();
        }));

        m_displayPolicy->setLidPresent(m_powerSupply->hasLid());
    }

    applyPolicy();

    if (m_powerSupply && m_powerSupply->lidClosed())
        m_displayPolicy->handleLidClosed();
}

PowerManager::~PowerManager() = default;

bool PowerManager::publish()
{
    if (!m_session)
        return false;
    m_object = std::make_unique<dbus::Object>(m_session, "/org/cutefish/Services/Power",
                                              "org.cutefish.Services.Power");
    m_object->addMethod("setBatteryScreenOff", [this](int32_t value) { setBatteryScreenOff(value); },
                        {{"value"}, {}});
    m_object->addMethod("setACScreenOff", [this](int32_t value) { setACScreenOff(value); }, {{"value"}, {}});
    m_object->addMethod("setACPreventSleep", [this](bool value) { setACPreventSleep(value); }, {{"value"}, {}});
    m_object->addMethod("setDimDisplayTimeout", [this](int32_t value) { setDimDisplayTimeout(value); },
                        {{"value"}, {}});
    return m_object->publish();
}

void PowerManager::loadSettings()
{
    const std::string group = s_powerGroup;
    m_batteryScreenOff = readTimeout(m_settings, group + "BatteryScreenOff", 300);
    m_acScreenOff = readTimeout(m_settings, group + "ACScreenOff", 1200);
    m_acPreventSleep = m_settings.boolValue(group + "ACPreventSleep", false);
}

void PowerManager::applyPolicy()
{
    const int timeout = m_onBattery ? m_batteryScreenOff : m_acScreenOff;
    // A delay of zero keeps the system awake while the display is off.
    const int suspendDelay = m_onBattery ? 300 : (m_acPreventSleep ? 0 : 900);

    m_displayPolicy->setPolicy(timeout, suspendDelay);
}

template<typename T>
void PowerManager::writePowerSetting(const std::string &key, const T &value)
{
    m_settings.setValue(s_powerGroup + key, value);
    m_settings.sync();
}

void PowerManager::setBatteryScreenOff(int timeout)
{
    m_batteryScreenOff = normalizedTimeout(timeout);
    writePowerSetting("BatteryScreenOff", m_batteryScreenOff);
    applyPolicy();
}

void PowerManager::setACScreenOff(int timeout)
{
    m_acScreenOff = normalizedTimeout(timeout);
    writePowerSetting("ACScreenOff", m_acScreenOff);
    applyPolicy();
}

void PowerManager::setACPreventSleep(bool prevent)
{
    m_acPreventSleep = prevent;
    writePowerSetting("ACPreventSleep", m_acPreventSleep);
    applyPolicy();
}

void PowerManager::setDimDisplayTimeout(int timeout)
{
    setBatteryScreenOff(timeout);
    setACScreenOff(timeout);
}

void PowerManager::onPowerSourceChanged()
{
    m_onBattery = m_powerSupply && m_powerSupply->onBattery();
    applyPolicy();
}

void PowerManager::onLidChanged()
{
    if (!m_powerSupply)
        return;

    if (m_powerSupply->lidClosed())
        m_displayPolicy->handleLidClosed();
    else
        m_displayPolicy->handleLidOpened();
}
