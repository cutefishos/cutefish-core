// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>
#include <vector>

#include "common/environment.h"

// The locale variables an application is started with, from the languages the
// user picked in the services' settings.
namespace cutefish::core::language {

// The system's default language, until the user picks one.
std::string systemLanguage();
std::vector<std::string> preferredLanguages();
// POSIX names (ll_CC[@script]) for language codes such as zh_Hans_CN.
std::vector<std::string> translationLanguages(const std::vector<std::string>& codes);
// The UTF-8 locale a language code's dates and numbers need, such as
// "ja_JP.UTF-8" for "ja".
std::string localeFor(const std::string& code);
bool isAvailable(const std::string& name);
// The session's region (services' [Region] region), such as "SG"; empty
// while the system's stands.
std::string region();
// The UTF-8 locale for dates and numbers of a language in a region: the
// language's own there when the system can make it ("zh_SG.UTF-8"), else the
// region's likely one ("en_SG.UTF-8" for Japanese in Singapore); empty for a
// region without one.
std::string formatsLocale(const std::string& code, const std::string& region);
// With a region, its formats locale names the regional categories (LC_TIME,
// LC_NUMERIC, ...) once it is available; without one they stay.
void apply(Environment& environment, const std::vector<std::string>& codes, const std::string& region = {});
void apply(Environment& environment);

} // namespace cutefish::core::language
