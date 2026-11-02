// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "common/language_environment.h"

#include "base/file.h"
#include "base/locale.h"
#include "base/paths.h"
#include "base/settings.h"
#include "base/strings.h"

#include <unicode/locid.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <langinfo.h>
#include <locale.h>
#include <optional>
#include <string_view>
#include <unistd.h>

namespace cutefish::core::language {

namespace base = cutefish::base;
namespace strings = cutefish::base::strings;

static void removeDuplicates(std::vector<std::string>& list)
{
    std::vector<std::string> unique;
    for (std::string& item : list) {
        if (std::find(unique.begin(), unique.end(), item) == unique.end())
            unique.push_back(std::move(item));
    }
    list = std::move(unique);
}

// ll_CC for the code, with a script modifier where the code's script is not
// the one the territory uses by default.
static std::string posixLanguage(const std::string& code)
{
    const icu::Locale locale(code.c_str());
    if (locale.isBogus() || !*locale.getLanguage())
        return code;

    UErrorCode status = U_ZERO_ERROR;
    icu::Locale full(locale);
    full.addLikelySubtags(status);
    if (U_FAILURE(status) || !*full.getCountry())
        return code;

    std::string name = std::string(full.getLanguage()) + '_' + full.getCountry();
    icu::Locale regional(name.c_str());
    regional.addLikelySubtags(status);
    if (U_SUCCESS(status) && std::strcmp(full.getScript(), regional.getScript()) != 0) {
        const std::string script = full.getScript();
        if (script == "Latn")
            name += "@latin";
        else if (script == "Cyrl")
            name += "@cyrillic";
        else if (script == "Deva")
            name += "@devanagari";
    }
    return name;
}

static std::string utf8Locale(std::string name)
{
    const size_t modifier = name.find('@');
    name.insert(modifier == std::string::npos ? name.size() : modifier, ".UTF-8");
    return name;
}

// A locale generated while this process runs is not in the archive it mapped,
// and the C library remembers a failed lookup; cutefish-locales generates
// directories, which name it either way.
bool isAvailable(const std::string& name)
{
    locale_t locale = newlocale(LC_ALL_MASK, name.c_str(), nullptr);
    if (locale) {
        freelocale(locale);
        return true;
    }
    const std::string directory = strings::replaceAll(name, ".UTF-8", ".utf8");
    return ::access(("/usr/lib/locale/" + directory + "/LC_CTYPE").c_str(), F_OK) == 0;
}

static bool isUtf8Locale(const std::string& name)
{
    locale_t locale = newlocale(LC_ALL_MASK, name.c_str(), nullptr);
    if (!locale)
        return false;
    const std::string encoding = strings::toAsciiUpper(nl_langinfo_l(CODESET, locale));
    freelocale(locale);
    return encoding == "UTF-8" || encoding == "UTF8";
}

static std::string installedUtf8Locale()
{
    FILE* pipe = ::popen("locale -a 2>/dev/null", "r");
    if (!pipe)
        return "C.UTF-8";
    std::string output;
    char buffer[4096];
    size_t length;
    while ((length = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0)
        output.append(buffer, length);
    ::pclose(pipe);

    for (const std::string& line : strings::split(output, '\n')) {
        const std::string name(strings::trimmed(line));
        const std::string normalized = strings::toAsciiLower(strings::replaceAll(name, "-", ""));
        if (!normalized.starts_with("c.") && normalized.find("utf8") != std::string::npos && isAvailable(name))
            return name;
    }
    return "C.UTF-8";
}

static std::string fallbackLocale()
{
    if (isAvailable("en_US.UTF-8"))
        return "en_US.UTF-8";

    static const std::string installed = installedUtf8Locale();
    return installed;
}

static std::string valueOf(const Environment& environment, const std::string& name)
{
    const auto found = environment.find(name);
    return found == environment.end() ? std::string() : found->second;
}

std::string systemLanguage()
{
    return base::Locale::systemLanguage();
}

std::vector<std::string> preferredLanguages()
{
    const base::Settings settings(base::paths::cutefishConfig("services"));
    const std::string primary = settings.stringValue("Language/language");
    if (primary.empty())
        return {systemLanguage()};

    std::vector<std::string> codes{primary};
    for (const std::string& code : settings.stringListValue("Language/preferredLanguages")) {
        if (!code.empty() && code != primary)
            codes.push_back(code);
    }
    removeDuplicates(codes);
    return codes;
}

std::string localeFor(const std::string& code)
{
    return utf8Locale(posixLanguage(code));
}

std::string region()
{
    const base::Settings settings(base::paths::cutefishConfig("services"));
    return strings::toAsciiUpper(settings.stringValue("Region/region"));
}

// The UTF-8 locales /usr/share/i18n/SUPPORTED lists, which cutefish-locales
// can generate.
static const std::vector<std::string>& supportedLocales()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> result;
        const std::optional<std::string> contents = base::file::read("/usr/share/i18n/SUPPORTED");
        for (const std::string& line : strings::split(contents.value_or(std::string()), '\n')) {
            const std::vector<std::string> fields = strings::splitWhitespace(line);
            if (fields.size() == 2 && fields[1] == "UTF-8")
                result.push_back(fields[0]);
        }
        return result;
    }();
    return names;
}

static bool isSupported(const std::string& name)
{
    const std::vector<std::string>& supported = supportedLocales();
    return std::find(supported.begin(), supported.end(), name) != supported.end() || isAvailable(name);
}

std::string formatsLocale(const std::string& code, const std::string& region)
{
    if (region.empty())
        return {};
    const icu::Locale language(code.c_str());
    if (!language.isBogus() && *language.getLanguage()) {
        UErrorCode status = U_ZERO_ERROR;
        icu::Locale full(language);
        full.addLikelySubtags(status);
        const std::string regional = std::string(full.getLanguage()) + '_' + full.getScript() + '_' + region;
        const std::string name = utf8Locale(posixLanguage(regional));
        if (isSupported(name))
            return name;
    }
    UErrorCode status = U_ZERO_ERROR;
    icu::Locale likely(("und_" + region).c_str());
    likely.addLikelySubtags(status);
    if (U_FAILURE(status) || !*likely.getLanguage() || std::string_view(likely.getCountry()) != region)
        return {};
    const std::string name = utf8Locale(
            posixLanguage(std::string(likely.getLanguage()) + '_' + likely.getScript() + '_' + likely.getCountry()));
    return isSupported(name) ? name : std::string();
}

std::vector<std::string> translationLanguages(const std::vector<std::string>& codes)
{
    std::vector<std::string> languages;
    for (const std::string& code : codes) {
        if (!code.empty())
            languages.push_back(posixLanguage(code));
    }
    removeDuplicates(languages);
    return languages;
}

// What a region decides; LC_MESSAGES, LC_COLLATE and LC_CTYPE follow the
// language, LC_IDENTIFICATION describes the locale itself.
static const char* const kRegionalCategories[] = {"LC_TIME", "LC_NUMERIC", "LC_MONETARY", "LC_MEASUREMENT",
                                                  "LC_PAPER", "LC_ADDRESS", "LC_TELEPHONE", "LC_NAME"};

void apply(Environment& environment, const std::vector<std::string>& codes, const std::string& region)
{
    const std::vector<std::string> languages = translationLanguages(codes);
    if (languages.empty())
        return;

    std::string localeName = utf8Locale(languages.front());
    if (!isAvailable(localeName)) {
        // Gettext needs a valid non-C message locale even when its language packs are installed.
        localeName = fallbackLocale();
    }
    environment.erase("LC_ALL");
    for (auto& [name, value] : environment) {
        if (name.starts_with("LC_") && !value.empty() && !isAvailable(value))
            value = localeName;
    }
    const auto primary = std::find_if(codes.begin(), codes.end(), [](const std::string& code) { return !code.empty(); });
    const std::string formats = formatsLocale(*primary, region);
    if (!formats.empty() && isAvailable(formats)) {
        for (const char* category : kRegionalCategories)
            environment[category] = formats;
    }
    // LC_CTYPE overrides LANG for toolkits and Fontconfig, including valid but non-UTF-8 values like C.
    const std::string ctype = valueOf(environment, "LC_CTYPE");
    if (!ctype.empty() && !isUtf8Locale(ctype))
        environment["LC_CTYPE"] = localeName;
    environment["LANG"] = localeName;
    environment["LC_MESSAGES"] = localeName;

    // Gettext has no English catalogs to stop at, so languages after English would replace the source strings.
    std::vector<std::string> messageLanguages;
    for (const std::string& language : languages) {
        messageLanguages.push_back(language);
        if (language == "en" || language.starts_with("en_") || language.starts_with("en@"))
            break;
    }
    environment["LANGUAGE"] = strings::join(messageLanguages, ":");
}

void apply(Environment& environment)
{
    language::apply(environment, preferredLanguages(), region());
}

} // namespace cutefish::core::language
