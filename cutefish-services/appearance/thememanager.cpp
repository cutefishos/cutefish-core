// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "thememanager.h"
#include "fontconfigwriter.h"
#include "gtksettings.h"
#include "loginbackground.h"
#include "scheduledfeature.h"
#include "wallpapermanager.h"

#include <algorithm>

#include "common/language_environment.h"

using namespace cutefish::base;

static const std::string s_systemFontName = "Appearance/Font";
static const std::string s_systemFixedFontName = "Appearance/FixedFont";
static const std::string s_systemPointFontSize = "Appearance/FontSize";

ThemeManager::ThemeManager(Settings &settings, dbus::Connection *session, dbus::Connection *system)
    : m_settings(settings)
    , m_session(session)
    , m_wallpaperManager(std::make_unique<WallpaperManager>(settings))
    , m_loginBackground(std::make_unique<LoginBackground>(*m_wallpaperManager, system))
{
    WallpaperManager &wallpapers = *m_wallpaperManager;
    m_subscriptions.push_back(wallpapers.wallpaperChanged.connect([this](const std::string &path) {
        emitSignal("wallpaperChanged", path);
    }));
    m_subscriptions.push_back(wallpapers.userWallpapersChanged.connect([this] {
        emitSignal("userWallpapersChanged");
    }));
    m_subscriptions.push_back(wallpapers.wallpaperFoldersChanged.connect([this] {
        emitSignal("wallpaperFoldersChanged");
    }));
    m_subscriptions.push_back(wallpapers.wallpaperFolderImagesChanged.connect([this](const std::string &folder) {
        emitSignal("wallpaperFolderImagesChanged", folder);
    }));
    m_subscriptions.push_back(wallpapers.backgroundTypeChanged.connect([this] {
        emitSignal("backgroundTypeChanged");
    }));
    m_subscriptions.push_back(wallpapers.backgroundColorChanged.connect([this] {
        emitSignal("backgroundColorChanged");
    }));

    // init value
    m_continuousCorners = m_settings.boolValue("Appearance/ContinuousCorners", true);
    m_accentColor = int(m_settings.intValue("Appearance/AccentColor", 0));
    m_cursorTheme = m_settings.stringValue("Appearance/CursorTheme", "default");
    m_cursorSize = int(m_settings.intValue("Appearance/CursorSize", 24));
    m_iconTheme = m_settings.stringValue("Appearance/IconTheme", "Cutefish");

    // Init fonts.
    if (!m_settings.contains(s_systemFixedFontName))
        m_settings.setValue(s_systemFixedFontName, "Noto Mono");

    if (!m_settings.contains(s_systemFontName))
        m_settings.setValue(s_systemFontName, "Inter");
    m_settings.sync();

    // Ash's DarkLightModeControllerImpl: the same schedule as the night
    // light, without custom times.
    m_darkMode = std::make_unique<ScheduledFeature>(
            ScheduledFeature::Keys {"Appearance", "DarkMode", "DarkModeSchedule", "DarkModeHoldUntil", {}, {}},
            settings, ScheduleUtils::currentDateTime);
    m_subscriptions.push_back(m_darkMode->enabledChanged.connect([this](bool darkMode) {
        updateGtk3Config();
        emitSignal("darkModeChanged", darkMode);
    }));
    m_subscriptions.push_back(m_darkMode->scheduleChanged.connect([this](int schedule) {
        emitSignal("darkModeScheduleChanged", int32_t(schedule));
    }));
    m_darkMode->start();

    // Update fontconfig after login.
    updateFontConfig();

    // GTK on Wayland takes these from gsettings, not settings.ini; sync them at login.
    updateGtk3Config();
    ensureGtkCssImports();
}

ThemeManager::~ThemeManager() = default;

bool ThemeManager::publish()
{
    if (!m_session)
        return false;
    m_object = std::make_unique<dbus::Object>(m_session, "/org/cutefish/Services/Appearance",
                                              "org.cutefish.Services.Appearance");
    dbus::Object &o = *m_object;
    o.addMethod("setDarkMode", [this](bool value) { setDarkMode(value); }, {{"darkMode"}, {}});
    o.addMethod("setDarkModeSchedule", [this](int32_t value) { setDarkModeSchedule(value); }, {{"schedule"}, {}});
    o.addMethod("setContinuousCorners", [this](bool value) { setContinuousCorners(value); },
                {{"continuousCorners"}, {}});
    o.addMethod("setSystemFont", [this](const std::string &value) { setSystemFont(value); }, {{"fontFamily"}, {}});
    o.addMethod("setSystemFixedFont", [this](const std::string &value) { setSystemFixedFont(value); },
                {{"fontFamily"}, {}});
    o.addMethod("setSystemFontPointSize", [this](double value) { setSystemFontPointSize(value); },
                {{"fontSize"}, {}});
    o.addMethod("setWallpaper", [this](const std::string &value) { setWallpaper(value); }, {{"path"}, {}});
    o.addMethod("removeUserWallpaper", [this](const std::string &value) { removeUserWallpaper(value); },
                {{"path"}, {}});
    o.addMethod("addWallpaperFolder", [this](const std::string &value) { addWallpaperFolder(value); },
                {{"path"}, {}});
    o.addMethod("removeWallpaperFolder", [this](const std::string &value) { removeWallpaperFolder(value); },
                {{"path"}, {}});
    o.addMethod("wallpaperFolderImages", [this](const std::string &value) { return wallpaperFolderImages(value); },
                {{"path"}, {"images"}});
    o.addMethod("setAccentColor", [this](int32_t value) { setAccentColor(value); }, {{"accentColor"}, {}});
    o.addMethod("setBackgroundColor", [this](const std::string &value) { setBackgroundColor(value); },
                {{"color"}, {}});
    o.addMethod("setBackgroundType", [this](int32_t value) { setBackgroundType(value); }, {{"type"}, {}});
    o.addMethod("setCursorTheme", [this](const std::string &value) { setCursorTheme(value); }, {{"type"}, {}});
    o.addMethod("setCursorSize", [this](int32_t value) { setCursorSize(value); }, {{"type"}, {}});
    o.addMethod("setIconTheme", [this](const std::string &value) { setIconTheme(value); }, {{"type"}, {}});
    o.addMethod("setFontRendering",
                [this](bool antialias, const std::string &hintStyle) { setFontRendering(antialias, hintStyle); },
                {{"antialias", "hintStyle"}, {}});
    o.addMethod("updateFontConfig", [this] { updateFontConfig(); });

    o.addProperty("isDarkMode", [this] { return isDarkMode(); });
    o.addProperty("darkModeSchedule", [this] { return int32_t(darkModeSchedule()); });
    o.addProperty("continuousCorners", [this] { return continuousCorners(); });
    o.addProperty("systemFont", [this] { return systemFont(); });
    o.addProperty("systemFixedFont", [this] { return systemFixedFont(); });
    o.addProperty("systemFontPointSize", [this] { return systemFontPointSize(); });
    o.addProperty("wallpaper", [this] { return wallpaper(); });
    o.addProperty("userWallpapers", [this] { return userWallpapers(); });
    o.addProperty("wallpaperFolders", [this] { return wallpaperFolders(); });
    o.addProperty("accentColor", [this] { return int32_t(accentColor()); });
    o.addProperty("backgroundType", [this] { return int32_t(backgroundType()); });
    o.addProperty("backgroundColor", [this] { return backgroundColor(); });
    o.addProperty("cursorTheme", [this] { return cursorTheme(); });
    o.addProperty("cursorSize", [this] { return int32_t(cursorSize()); });
    o.addProperty("iconTheme", [this] { return iconTheme(); });

    o.addSignal("darkModeChanged", "b");
    o.addSignal("darkModeScheduleChanged", "i");
    o.addSignal("wallpaperChanged", "s");
    o.addSignal("userWallpapersChanged", "");
    o.addSignal("wallpaperFoldersChanged", "");
    o.addSignal("wallpaperFolderImagesChanged", "s");
    o.addSignal("continuousCornersChanged", "b");
    o.addSignal("accentColorChanged", "i");
    o.addSignal("backgroundTypeChanged", "");
    o.addSignal("backgroundColorChanged", "");
    o.addSignal("cursorThemeChanged", "");
    o.addSignal("cursorSizeChanged", "");
    o.addSignal("iconThemeChanged", "");
    o.addSignal("systemFontPointSizeChanged", "");
    o.addSignal("systemFontChanged", "");
    return o.publish();
}

template<typename... Args>
void ThemeManager::emitSignal(const std::string &name, const Args &...args)
{
    if (m_object)
        m_object->emitSignal(name, args...);
    changed.emit();
}

template<typename T>
void ThemeManager::write(const std::string &key, const T &value)
{
    m_settings.setValue("Appearance/" + key, value);
    m_settings.sync();
}

bool ThemeManager::isDarkMode()
{
    return m_darkMode->enabled();
}

void ThemeManager::setDarkMode(bool darkMode)
{
    m_darkMode->setEnabled(darkMode);
}

int ThemeManager::darkModeSchedule() const
{
    return m_darkMode->schedule();
}

void ThemeManager::setDarkModeSchedule(int schedule)
{
    m_darkMode->setSchedule(schedule);
}

bool ThemeManager::continuousCorners() const
{
    return m_continuousCorners;
}

void ThemeManager::setContinuousCorners(bool value)
{
    if (m_continuousCorners == value)
        return;

    m_continuousCorners = value;
    write("ContinuousCorners", m_continuousCorners);

    emitSignal("continuousCornersChanged", m_continuousCorners);
}

int ThemeManager::accentColor()
{
    return m_accentColor;
}

void ThemeManager::setAccentColor(int accentColor)
{
    if (m_accentColor == accentColor)
        return;

    m_accentColor = accentColor;
    write("AccentColor", m_accentColor);

    emitSignal("accentColorChanged", int32_t(m_accentColor));
}

std::string ThemeManager::cursorTheme() const
{
    return m_cursorTheme;
}

void ThemeManager::setCursorTheme(const std::string &theme)
{
    if (theme.empty() || theme.size() > 255 || theme == "." || theme == ".."
        || theme.find_first_of(std::string_view("/\\\0", 3)) != std::string::npos)
        return;
    if (m_cursorTheme != theme) {
        m_cursorTheme = theme;
        write("CursorTheme", m_cursorTheme);
        applyCursorSettings();
        emitSignal("cursorThemeChanged");
    }
}

int ThemeManager::cursorSize() const
{
    return m_cursorSize;
}

void ThemeManager::setCursorSize(int size)
{
    size = std::clamp(size, 16, 64);
    if (m_cursorSize != size) {
        m_cursorSize = size;
        write("CursorSize", m_cursorSize);
        applyCursorSettings();
        emitSignal("cursorSizeChanged");
    }
}

std::string ThemeManager::systemFont()
{
    return m_settings.stringValue(s_systemFontName, "Inter");
}

void ThemeManager::setSystemFont(const std::string &fontFamily)
{
    m_settings.setValue(s_systemFontName, fontFamily);
    m_settings.sync();
    updateGtk3Config();
    updateFontConfig();

    emitSignal("systemFontChanged");
}

std::string ThemeManager::systemFixedFont()
{
    return m_settings.stringValue(s_systemFixedFontName, "Noto Mono");
}

void ThemeManager::setSystemFixedFont(const std::string &fontFamily)
{
    m_settings.setValue(s_systemFixedFontName, fontFamily);
    m_settings.sync();

    updateFontConfig();
}

double ThemeManager::systemFontPointSize()
{
    return m_settings.doubleValue(s_systemPointFontSize, 9);
}

void ThemeManager::setSystemFontPointSize(double fontSize)
{
    m_settings.setValue(s_systemPointFontSize, fontSize);
    m_settings.sync();
    updateGtk3Config();
    emitSignal("systemFontPointSizeChanged");
}

std::string ThemeManager::wallpaper()
{
    return m_wallpaperManager->wallpaper();
}

void ThemeManager::setWallpaper(const std::string &path)
{
    m_wallpaperManager->setWallpaper(path);
}

std::vector<std::string> ThemeManager::userWallpapers() const
{
    return m_wallpaperManager->userWallpapers();
}

void ThemeManager::removeUserWallpaper(const std::string &path)
{
    m_wallpaperManager->removeUserWallpaper(path);
}

std::vector<std::string> ThemeManager::wallpaperFolders() const
{
    return m_wallpaperManager->wallpaperFolders();
}

void ThemeManager::addWallpaperFolder(const std::string &path)
{
    m_wallpaperManager->addWallpaperFolder(path);
}

void ThemeManager::removeWallpaperFolder(const std::string &path)
{
    m_wallpaperManager->removeWallpaperFolder(path);
}

std::vector<std::string> ThemeManager::wallpaperFolderImages(const std::string &path)
{
    return m_wallpaperManager->wallpaperFolderImages(path);
}

int ThemeManager::backgroundType()
{
    return m_wallpaperManager->backgroundType();
}

void ThemeManager::setBackgroundType(int type)
{
    m_wallpaperManager->setBackgroundType(type);
}

std::string ThemeManager::backgroundColor()
{
    return m_wallpaperManager->backgroundColor();
}

void ThemeManager::setBackgroundColor(const std::string &color)
{
    m_wallpaperManager->setBackgroundColor(color);
}

void ThemeManager::updateGtk3Config()
{
    GtkSettings gtk;
    gtk.fontFamily = systemFont();
    gtk.fontPointSize = systemFontPointSize();

    const FontConfig font = fontConfig();
    gtk.fontAntialias = font.antialias;
    gtk.fontHintStyle = font.hintStyle;
    gtk.fontRgba = font.rgba;

    gtk.darkMode = isDarkMode();
    gtk.iconTheme = m_iconTheme.empty() ? std::string("Cutefish") : m_iconTheme;
    gtk.cursorTheme = m_cursorTheme;
    gtk.cursorSize = m_cursorSize;
    writeGtkSettings(gtk);
}

void ThemeManager::setFontRendering(bool antialias, const std::string &hintStyle)
{
    m_settings.setValue("Appearance/FontAntialias", antialias);
    m_settings.setValue("Appearance/FontHintStyle", hintStyle);
    m_settings.sync();
    updateFontConfig();
    updateGtk3Config();
}

void ThemeManager::applyCursorSettings()
{
    m_settings.sync();

    updateGtk3Config();
}

void ThemeManager::updateFontConfig()
{
    writeFontConfig(fontConfig());
}

FontConfig ThemeManager::fontConfig()
{
    FontConfig config;
    config.family = systemFont();
    config.fixedFamily = systemFixedFont();
    config.antialias = m_settings.boolValue("Appearance/FontAntialias", true);
    config.hintStyle = m_settings.stringValue("Appearance/FontHintStyle", "hintfull");
    config.cjkFamily = cjkFontForLanguages(cutefish::core::language::preferredLanguages());
    return config;
}

std::string ThemeManager::iconTheme() const
{
    return m_iconTheme;
}

void ThemeManager::setIconTheme(const std::string &iconTheme)
{
    if (m_iconTheme == iconTheme)
        return;

    m_iconTheme = iconTheme;
    // Listeners re-read the file when the signal arrives.
    write("IconTheme", m_iconTheme);
    updateGtk3Config();
    emitSignal("iconThemeChanged");
}
