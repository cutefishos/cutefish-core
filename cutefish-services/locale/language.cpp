// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "language.h"

#include <algorithm>
#include <map>

#include <systemd/sd-bus.h>
#include <unicode/uloc.h>

#include "base/i18n.h"
#include "base/locale.h"
#include "base/session_language.h"
#include "base/strings.h"
#include "common/environment.h"
#include "common/language_environment.h"

using namespace cutefish::base;
using namespace cutefish::base::strings;
using cutefish::i18n::tr;
namespace language = cutefish::core::language;

static void removeAll(std::vector<std::string> &list, const std::string &value)
{
    list.erase(std::remove(list.begin(), list.end(), value), list.end());
}

static void removeDuplicates(std::vector<std::string> &list)
{
    std::vector<std::string> unique;
    for (const std::string &entry : list) {
        if (std::find(unique.begin(), unique.end(), entry) == unique.end())
            unique.push_back(entry);
    }
    list = std::move(unique);
}

Language::Language(Settings &settings, dbus::Connection *session, Appearance appearance)
    : m_settings(settings)
    , m_session(session)
    , m_appearance(std::move(appearance))
{
}

Language::~Language() = default;

bool Language::publish()
{
    if (!m_session)
        return false;
    m_object = std::make_unique<dbus::Object>(m_session, "/org/cutefish/Services/Language",
                                              "org.cutefish.Services.Language");
    m_object->addMethod("setLanguage", [this](const std::string &value) { setLanguage(value); },
                        {{"value"}, {}});
    m_object->addMethod("setPreferredLanguages",
                        [this](const std::vector<std::string> &value) { setPreferredLanguages(value); },
                        {{"value"}, {}});
    m_object->addMethod("setPreferredLanguagesQuietly",
                        [this](const std::vector<std::string> &value) { setPreferredLanguages(value, false); },
                        {{"value"}, {}});
    m_object->addMethod("setRegion", [this](const std::string &value) { setRegion(value); }, {{"value"}, {}});
    m_object->addMethod("setFirstWeekday", [this](int32_t value) { setFirstWeekday(value); }, {{"value"}, {}});
    m_object->addMethod("setMeasurementSystem", [this](const std::string &value) { setMeasurementSystem(value); },
                        {{"value"}, {}});
    m_object->addMethod("setTemperatureUnit", [this](const std::string &value) { setTemperatureUnit(value); },
                        {{"value"}, {}});
    m_object->addMethod("setTwentyFourHour", [this](int32_t value) { setTwentyFourHour(value); }, {{"value"}, {}});
    m_object->addProperty("preferredLanguages", [this] { return preferredLanguages(); });
    m_object->addProperty("languageCode", [this] { return languageCode(); });
    m_object->addProperty("environment", [this] { return environment(); });
    m_object->addProperty("region", [this] { return region(); });
    m_object->addProperty("regionalPreferences", [this] { return regionalPreferences(); });
    m_object->addSignal("languageChanged", "");
    if (!m_object->publish())
        return false;
    // A language picked before its locale was there, or on another system.
    prepareLocale();
    return true;
}

std::string Language::languageCode() const
{
    const std::string code = m_settings.stringValue("Language/language");
    return code.empty() ? language::systemLanguage() : code;
}

std::vector<std::string> Language::preferredLanguages() const
{
    const std::string primary = languageCode();
    std::vector<std::string> codes = m_settings.stringListValue("Language/preferredLanguages");
    removeAll(codes, primary);
    codes.insert(codes.begin(), primary);
    removeDuplicates(codes);
    return codes;
}

std::map<std::string, std::string> Language::environment() const
{
    cutefish::core::Environment environment = cutefish::core::processEnvironment();
    language::apply(environment, preferredLanguages(), toAsciiUpper(m_settings.stringValue("Region/region")));
    std::map<std::string, std::string> variables;
    for (const auto &[name, value] : environment) {
        if (name == "LANG" || name == "LANGUAGE" || startsWith(name, "LC_"))
            variables[name] = value;
    }
    // D-Bus cannot remove variables; an empty LC_ALL lets individual categories take effect.
    variables["LC_ALL"] = std::string();
    return variables;
}

void Language::setLanguage(const std::string &code)
{
    std::vector<std::string> codes = preferredLanguages();
    removeAll(codes, code);
    codes.insert(codes.begin(), code);
    setPreferredLanguages(codes);
}

std::string Language::localeName(const std::string &code)
{
    char likely[ULOC_FULLNAME_CAPACITY] = {};
    UErrorCode status = U_ZERO_ERROR;
    uloc_addLikelySubtags(code.c_str(), likely, sizeof(likely), &status);
    if (U_FAILURE(status))
        return "C";
    char languageCode[ULOC_LANG_CAPACITY] = {};
    char country[ULOC_COUNTRY_CAPACITY] = {};
    uloc_getLanguage(likely, languageCode, sizeof(languageCode), &status);
    uloc_getCountry(likely, country, sizeof(country), &status);
    if (U_FAILURE(status) || !*languageCode || std::string_view(languageCode) == "c")
        return "C";
    return *country ? std::string(languageCode) + '_' + country : std::string(languageCode);
}

void Language::setPreferredLanguages(const std::vector<std::string> &languages, bool notify)
{
    std::vector<std::string> codes = languages;
    removeAll(codes, std::string());
    removeDuplicates(codes);
    if (codes.empty() || codes == preferredLanguages())
        return;

    const std::string code = codes.front();
    const bool primaryChanged = code != languageCode();
    m_settings.setValue("Language/preferredLanguages", codes);

    m_settings.setValue("Language/language", code);
    m_settings.sync();
    // The CJK fallback follows the order of the languages, so it is rewritten after the sync.
    if (m_appearance.updateFontConfig)
        m_appearance.updateFontConfig();

    publishEnvironment();
    prepareLocale();

    if (!primaryChanged || !notify)
        return;

    m_notification = std::make_unique<Notification>(m_session);
    m_notification->setDesktopEntry("cutefish-settings");
    m_notification->setIconName("preferences-system");
    m_notification->setBody(tr("Some applications may need to be restarted for the change to take effect."));
    m_notification->setTimeout(10);
    m_notification->show();
}

std::string Language::region() const
{
    const std::string code = toAsciiUpper(m_settings.stringValue("Region/region"));
    return code.empty() ? Locale::systemRegion() : code;
}

std::map<std::string, std::string> Language::regionalPreferences() const
{
    std::map<std::string, std::string> values;
    for (const char *key : {"region", "firstWeekday", "measurement", "temperature", "twentyFourHour"}) {
        const std::string path = std::string("Region/") + key;
        if (m_settings.contains(path))
            values[key] = m_settings.stringValue(path);
    }
    return values;
}

void Language::setRegionalValue(const std::string &key, const std::string &value)
{
    const std::string path = "Region/" + key;
    if (value.empty() ? !m_settings.contains(path) : m_settings.stringValue(path) == value)
        return;
    if (value.empty())
        m_settings.remove(path);
    else
        m_settings.setValue(path, value);
    m_settings.sync();
    publishEnvironment();
    prepareLocale();
}

static bool isRegionCode(const std::string &code)
{
    if (code.size() == 2)
        return std::all_of(code.begin(), code.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
    if (code.size() == 3)
        return std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; });
    return false;
}

void Language::setRegion(const std::string &code)
{
    const std::string region = toAsciiUpper(code);
    if (!region.empty() && !isRegionCode(region))
        return;
    setRegionalValue("region", region);
}

void Language::setFirstWeekday(int day)
{
    if (day < 0 || day > 7)
        return;
    setRegionalValue("firstWeekday", day ? std::to_string(day) : std::string());
}

void Language::setMeasurementSystem(const std::string &system)
{
    if (!system.empty() && system != "metric" && system != "us" && system != "uk")
        return;
    setRegionalValue("measurement", system);
}

void Language::setTemperatureUnit(const std::string &unit)
{
    if (!unit.empty() && unit != "celsius" && unit != "fahrenheit")
        return;
    setRegionalValue("temperature", unit);
}

void Language::setTwentyFourHour(int value)
{
    if (value < 0 || value > 2)
        return;
    setRegionalValue("twentyFourHour", value == 0 ? std::string() : value == 2 ? "true" : "false");
}

void Language::publishEnvironment()
{
    const std::map<std::string, std::string> activationEnvironment = environment();
    std::vector<std::string> systemdEnvironment;
    for (const auto &[name, value] : activationEnvironment) {
        if (name != "LC_ALL")
            systemdEnvironment.push_back(name + '=' + value);
    }
    if (m_session) {
        const dbus::Proxy bus(m_session, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                              "org.freedesktop.DBus");
        bus.send("UpdateActivationEnvironment", activationEnvironment);

        dbus::Message systemd = m_session->newMethodCall("org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                                                         "org.freedesktop.systemd1.Manager",
                                                         "UnsetAndSetEnvironment");
        sd_bus_message_set_auto_start(systemd.get(), 0);
        systemd.append(std::vector<std::string> {"LC_ALL"}, systemdEnvironment);
        m_session->send(systemd);
    }
    // This process takes the new language itself, before its own text below;
    // the signal reaches it only later, if at all.
    if (applyEnvironment)
        applyEnvironment(activationEnvironment);
    else
        SessionLanguage::apply(activationEnvironment);
    emitLanguageChanged();
}

void Language::prepareLocale()
{
    const std::string code = languageCode();
    prepareLocale(language::localeFor(code), m_localeCall);
    prepareLocale(language::formatsLocale(code, toAsciiUpper(m_settings.stringValue("Region/region"))),
                  m_formatsCall);
}

void Language::prepareLocale(const std::string &locale, dbus::PendingCall &call)
{
    if (!generateLocale || locale.empty() || language::isAvailable(locale))
        return;
    call = generateLocale(locale, [this](bool generated) {
        if (generated)
            publishEnvironment();
    });
}

void Language::emitLanguageChanged()
{
    if (m_object)
        m_object->emitSignal("languageChanged");
}
