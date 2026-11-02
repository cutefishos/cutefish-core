// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef LANGUAGE_H
#define LANGUAGE_H

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/dbus.h"
#include "base/settings.h"
#include "notification.h"

class Language
{
public:
    // What a language change does to the appearance: the fallback families
    // follow the order.
    struct Appearance {
        std::function<void()> updateFontConfig;
    };

    Language(cutefish::base::Settings &settings, cutefish::base::dbus::Connection *session,
             Appearance appearance);
    ~Language();

    Language(const Language &) = delete;
    Language &operator=(const Language &) = delete;

    // org.cutefish.Services.Language at /org/cutefish/Services/Language.
    bool publish();

    std::string languageCode() const;
    void setLanguage(const std::string &code);
    std::vector<std::string> preferredLanguages() const;
    void setPreferredLanguages(const std::vector<std::string> &codes);

    // The region chosen in [Region], else the system's, such as "SG".
    std::string region() const;
    // An ISO 3166 code; empty goes back to the system's.
    void setRegion(const std::string &code);
    // 1 for Monday to 7 for Sunday; 0 for the region's.
    void setFirstWeekday(int day);
    // "metric", "us" or "uk"; empty for the region's.
    void setMeasurementSystem(const std::string &system);
    // "celsius" or "fahrenheit"; empty for the region's.
    void setTemperatureUnit(const std::string &unit);
    // 0 for the region's clock, 1 for 12-hour, 2 for 24-hour time.
    void setTwentyFourHour(int value);
    // [Region] as stored: region, firstWeekday, measurement, temperature and
    // twentyFourHour, each only while set.
    std::map<std::string, std::string> regionalPreferences() const;
    // LANG, LANGUAGE and the LC_ variables for the preferred languages, as
    // programs started now get them; LC_ALL is empty.
    std::map<std::string, std::string> environment() const;

    // A locale's language and territory, such as "en_US" for "en", as the
    // session names it.
    static std::string localeName(const std::string &code);

    // Gives this process the new language variables, before its own text
    // follows; base::SessionLanguage::apply() unless set.
    std::function<void(const std::map<std::string, std::string> &variables)> applyEnvironment;
    // Asks cutefish-locales for a locale the system lacks; `done` tells
    // whether it is there now.
    std::function<cutefish::base::dbus::PendingCall(const std::string &locale, std::function<void(bool)> done)>
            generateLocale;

private:
    void publishEnvironment();
    // A primary language or a region whose locale is missing has it
    // generated, then programs get the variables again, now with its dates
    // and numbers.
    void prepareLocale();
    void prepareLocale(const std::string &locale, cutefish::base::dbus::PendingCall &call);
    // Writes the [Region] key, or removes it for an empty value, then tells
    // the session.
    void setRegionalValue(const std::string &key, const std::string &value);
    void emitLanguageChanged();

    cutefish::base::Settings &m_settings;
    cutefish::base::dbus::Connection *m_session;
    Appearance m_appearance;
    std::unique_ptr<cutefish::base::dbus::Object> m_object;
    std::unique_ptr<Notification> m_notification;
    cutefish::base::dbus::PendingCall m_localeCall;
    cutefish::base::dbus::PendingCall m_formatsCall;
};

#endif
