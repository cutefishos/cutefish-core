// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "application.h"

#include "appearance/thememanager.h"
#include "battery/batterywarning.h"
#include "battery/powersupply.h"
#include "brightness/brightnessmanager.h"
#include "clipboard/clipboard.h"
#include "crash/crashmanager.h"
#include "locale/language.h"
#include "nightlight/nightlight.h"
#include "permissions/permissionmanager.h"
#include "idle/waylandidle.h"
#include "power/powermanager.h"
#include "proxy/proxyservice.h"

#include "base/dbus.h"
#include "base/paths.h"
#include "ui/application.h"
#include "ui/theme.h"
#include "log.h"

#include <chrono>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace cutefish::base;
namespace ui = cutefish::ui;

Application::Application()
    : m_session(dbus::Connection::session())
    , m_system(dbus::Connection::system())
    , m_settings(paths::cutefishConfig("services"))
{
    m_idle = std::make_unique<WaylandIdle>();
    m_themeManager = std::make_unique<ThemeManager>(m_settings, m_session, m_system);
    m_brightnessManager = std::make_unique<BrightnessManager>(m_session, m_system);
    m_powerSupply = std::make_unique<PowerSupply>(m_system, m_session, m_settings);
    m_batteryWarning = std::make_unique<BatteryWarning>(m_system, m_session);
    m_language = std::make_unique<Language>(
            m_settings, m_session,
            Language::Appearance {[this] { m_themeManager->updateFontConfig(); }});
    // The crash dialogs and permission prompts in this process follow too.
    m_language->applyEnvironment = [](const std::map<std::string, std::string> &variables) {
        ui::Application::instance()->setLanguageEnvironment(variables);
    };
    m_language->generateLocale = [this](const std::string &locale, std::function<void(bool)> done) {
        dbus::Proxy locales(m_system, "org.cutefish.Locales", "/org/cutefish/Locales", "org.cutefish.Locales");
        // Generating takes a second or two per locale.
        locales.setTimeout(std::chrono::minutes(2));
        return locales.callAsync(
                "generate",
                [done = std::move(done)](const dbus::Reply &reply) {
                    if (reply.isError())
                        CF_WARNING(services::log()) << "Cannot generate the locale:" << reply.error().message;
                    done(!reply.isError());
                },
                std::vector<std::string> {locale});
    };
    m_clipboard = std::make_unique<Clipboard>();
    m_powerManager = std::make_unique<PowerManager>(m_powerSupply.get(), m_brightnessManager.get(),
                                                    m_idle.get(), m_settings, m_session, m_system);
    m_nightLight = std::make_unique<NightLight>(m_settings);
    m_permissionManager = std::make_unique<PermissionManager>(m_session);
    m_crashManager = std::make_unique<CrashManager>([this] { return m_themeManager->systemFixedFont(); });
    m_proxyService = std::make_unique<ProxyService>(m_session);

    initTrash();

    applyTheme();
    m_themeChanged = m_themeManager->changed.connect([this] {
        applyTheme();
    });

    // Register all service objects before publishing the well-known name.
    // sd-bus rejects a whole object over one malformed member, silently.
    const auto published = [](bool ok, const char *object) {
        if (!ok)
            CF_WARNING(services::log()) << "Cannot publish" << object;
    };
    published(m_themeManager->publish(), "Appearance");
    published(m_brightnessManager->publish(), "Brightness");
    published(m_language->publish(), "Language");
    published(m_powerManager->publish(), "Power");
    published(m_nightLight->publish(m_session), "NightLight");
    published(m_permissionManager->publish(), "Permissions");

    if (!m_session || !m_session->requestName("org.cutefish.Services"))
        CF_WARNING(services::log()) << "Cannot own org.cutefish.Services";
}

Application::~Application()
{
    // The actions go before the idle manager they registered with.
    m_powerManager.reset();
}

void Application::applyTheme()
{
    // The accents, in the order of the setting's index.
    static const ui::Color accents[] = {
        ui::accent::blue, ui::accent::red, ui::accent::green, ui::accent::purple,
        ui::accent::pink, ui::accent::orange, ui::accent::grey,
    };

    ThemeManager &appearance = *m_themeManager;
    ui::Theme theme = appearance.isDarkMode() ? ui::Theme::dark() : ui::Theme::light();
    const int accent = appearance.accentColor();
    if (accent >= 0 && accent < int(std::size(accents)))
        theme.highlight = accents[accent];
    if (appearance.systemFontPointSize() > 0)
        theme.fontPointSize = appearance.systemFontPointSize();
    theme.fontFamily = appearance.systemFont();
    theme.fixedFontFamily = appearance.systemFixedFont();
    if (!appearance.iconTheme().empty())
        theme.iconTheme = appearance.iconTheme();
    theme.continuousCorners = appearance.continuousCorners();
    ui::Theme::setCurrent(theme);
}

void Application::initTrash()
{
    const std::string trashDir = paths::dataHome() + "/Trash";

    if (::mkdir(trashDir.c_str(), 0700) != 0)
        return;

    struct stat buff;
    uid_t uid = getuid();

    if (::lstat(trashDir.c_str(), &buff) != 0)
        return;

    if ((buff.st_uid == uid) && // must be owned by user
        ((buff.st_mode & 0777) == 0700)) {
        // check subdirs
        const std::string infoDir = trashDir + "/info";
        const std::string filesDir = trashDir + "/files";

        ::mkdir(infoDir.c_str(), 0777);
        ::mkdir(filesDir.c_str(), 0777);
    } else {
        ::rmdir(trashDir.c_str());
    }
}
