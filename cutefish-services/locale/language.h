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
    // A primary language whose locale is missing has it generated, then
    // programs get the variables again, now with its dates and numbers.
    void prepareLocale();
    void emitLanguageChanged();

    cutefish::base::Settings &m_settings;
    cutefish::base::dbus::Connection *m_session;
    Appearance m_appearance;
    std::unique_ptr<cutefish::base::dbus::Object> m_object;
    std::unique_ptr<Notification> m_notification;
    cutefish::base::dbus::PendingCall m_localeCall;
};

#endif
