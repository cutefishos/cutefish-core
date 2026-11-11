// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include "fontconfigwriter.h"

#include <memory>
#include <string>
#include <vector>

#include "base/dbus.h"
#include "base/settings.h"
#include "base/signal.h"

namespace cutefish::powerprofiles {
class PowerProfiles;
}

class LoginBackground;
class ScheduledFeature;
class WallpaperManager;

class ThemeManager
{
public:
    ThemeManager(cutefish::base::Settings &settings, cutefish::base::dbus::Connection *session,
                 cutefish::base::dbus::Connection *system);
    ~ThemeManager();

    ThemeManager(const ThemeManager &) = delete;
    ThemeManager &operator=(const ThemeManager &) = delete;

    // org.cutefish.Services.Appearance at /org/cutefish/Services/Appearance.
    bool publish();

    bool isDarkMode();
    // A manual switch holds until the schedule's next turn.
    void setDarkMode(bool darkMode);

    // ScheduledFeature::Schedule, without Custom: dark from sunset to sunrise.
    int darkModeSchedule() const;
    void setDarkModeSchedule(int schedule);

    bool continuousCorners() const;
    void setContinuousCorners(bool value);

    std::string systemFont();
    void setSystemFont(const std::string &fontFamily);

    std::string systemFixedFont();
    void setSystemFixedFont(const std::string &fontFamily);

    double systemFontPointSize();
    void setSystemFontPointSize(double fontSize);

    std::string wallpaper();
    void setWallpaper(const std::string &path);
    std::vector<std::string> userWallpapers() const;
    void removeUserWallpaper(const std::string &path);

    std::vector<std::string> wallpaperFolders() const;
    void addWallpaperFolder(const std::string &path);
    void removeWallpaperFolder(const std::string &path);
    std::vector<std::string> wallpaperFolderImages(const std::string &path);

    int backgroundType();
    void setBackgroundType(int type);

    std::string backgroundColor();
    void setBackgroundColor(const std::string &color);

    int accentColor();
    void setAccentColor(int accentColor);

    std::string cursorTheme() const;
    void setCursorTheme(const std::string &theme);

    int cursorSize() const;
    void setCursorSize(int size);

    void updateGtk3Config();
    void setFontRendering(bool antialias, const std::string &hintStyle);
    void applyCursorSettings();

    std::string iconTheme() const;
    void setIconTheme(const std::string &iconTheme);

    void updateFontConfig();

    // After any change the interface announces.
    cutefish::base::Signal<> changed;

private:
    FontConfig fontConfig();
    template<typename... Args>
    void emitSignal(const std::string &name, const Args &...args);
    template<typename T>
    void write(const std::string &key, const T &value);

    cutefish::base::Settings &m_settings;
    cutefish::base::dbus::Connection *m_session;
    std::unique_ptr<WallpaperManager> m_wallpaperManager;
    std::unique_ptr<LoginBackground> m_loginBackground;
    std::unique_ptr<ScheduledFeature> m_darkMode;
    std::unique_ptr<cutefish::powerprofiles::PowerProfiles> m_powerProfiles;
    std::unique_ptr<cutefish::base::dbus::Object> m_object;
    std::vector<cutefish::base::Subscription> m_subscriptions;

    bool m_continuousCorners = true;
    int m_accentColor = 0;

    std::string m_cursorTheme;
    int m_cursorSize = 24;

    std::string m_iconTheme;
};

#endif
