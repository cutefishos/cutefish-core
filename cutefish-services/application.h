// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef APPLICATION_H
#define APPLICATION_H

#include <memory>

#include "base/settings.h"
#include "base/signal.h"

class BatteryWarning;
class BrightnessManager;
class Clipboard;
class CrashManager;
class Language;
class NightLight;
class PermissionManager;
class PowerManager;
class PowerSupply;
class ProxyService;
class SoftwareUpdate;
class ThemeManager;
class WaylandIdle;

namespace cutefish::base::dbus {
class Connection;
}

// The session's services, on the session bus as org.cutefish.Services.
class Application
{
public:
    Application();
    ~Application();

    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    void initTrash();

private:
    // This process's dialogs follow the appearance it keeps itself.
    void applyTheme();

    cutefish::base::dbus::Connection *m_session;
    cutefish::base::dbus::Connection *m_system;
    // ~/.config/cutefish/services.conf, which every part shares.
    cutefish::base::Settings m_settings;
    std::unique_ptr<WaylandIdle> m_idle;
    std::unique_ptr<ThemeManager> m_themeManager;
    std::unique_ptr<BrightnessManager> m_brightnessManager;
    std::unique_ptr<PowerSupply> m_powerSupply;
    std::unique_ptr<BatteryWarning> m_batteryWarning;
    std::unique_ptr<Language> m_language;
    std::unique_ptr<Clipboard> m_clipboard;
    std::unique_ptr<PowerManager> m_powerManager;
    std::unique_ptr<NightLight> m_nightLight;
    std::unique_ptr<PermissionManager> m_permissionManager;
    std::unique_ptr<CrashManager> m_crashManager;
    std::unique_ptr<ProxyService> m_proxyService;
    std::unique_ptr<SoftwareUpdate> m_softwareUpdate;
    cutefish::base::Subscription m_themeChanged;
};

#endif // APPLICATION_H
