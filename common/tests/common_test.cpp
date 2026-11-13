// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "common/gsettings_keyfile.h"
#include "common/language_environment.h"
#include "common/proxy_settings.h"

#include "base/file.h"

#include <filesystem>

#include <gtest/gtest.h>

using namespace cutefish::core;

TEST(LanguageEnvironment, codesBecomePosixNames)
{
    EXPECT_EQ(language::translationLanguages({"zh_Hans_CN", "zh_Hant_TW", "zh_Hant_HK", "en_US", "ar", "es_419",
                                              "sr_Latn_RS", "", "en_US"}),
              (std::vector<std::string> {"zh_CN", "zh_TW", "zh_HK", "en_US", "ar_EG", "es_419", "sr_RS@latin"}));
    EXPECT_EQ(language::translationLanguages({"ja", "en", "uz_Cyrl_UZ", "hi_Latn_IN", "nb_NO"}),
              (std::vector<std::string> {"ja_JP", "en_US", "uz_UZ@cyrillic", "hi_IN@latin", "nb_NO"}));
    // Not a language: left as it is.
    EXPECT_EQ(language::translationLanguages({"C"}), std::vector<std::string> {"C"});
}

TEST(LanguageEnvironment, messagesStopAtEnglish)
{
    Environment environment {{"LC_ALL", "de_DE.UTF-8"}, {"LC_TIME", "xx_XX.UTF-8"}, {"HOME", "/home/test"}};
    language::apply(environment, {"de_DE", "en_US", "fr_FR"});

    EXPECT_FALSE(environment.contains("LC_ALL"));
    EXPECT_EQ(environment["LANGUAGE"], "de_DE:en_US");
    EXPECT_FALSE(environment["LANG"].empty());
    EXPECT_EQ(environment["LC_MESSAGES"], environment["LANG"]);
    // A locale that is not installed is replaced.
    EXPECT_EQ(environment["LC_TIME"], environment["LANG"]);
    EXPECT_EQ(environment["HOME"], "/home/test");
}

TEST(LanguageEnvironment, portugueseFallsBackToBrazilianCatalogs)
{
    Environment portugal;
    language::apply(portugal, {"pt_PT"});
    EXPECT_EQ(portugal["LANGUAGE"], "pt_PT:pt_BR");

    Environment ordered;
    language::apply(ordered, {"pt_AO", "fr_FR", "pt_BR", "en_US", "pt_MZ"});
    EXPECT_EQ(ordered["LANGUAGE"], "pt_AO:pt_BR:fr_FR:en_US");

    Environment brazil;
    language::apply(brazil, {"pt_BR", "es_MX"});
    EXPECT_EQ(brazil["LANGUAGE"], "pt_BR:es_MX");

    Environment others;
    language::apply(others, {"es_MX", "fr_CA", "de_AT"});
    EXPECT_EQ(others["LANGUAGE"], "es_MX:fr_CA:de_AT");
}

TEST(LanguageEnvironment, regionsNameTheirFormatsLocale)
{
    EXPECT_EQ(language::formatsLocale("zh_Hans_CN", "SG"), "zh_SG.UTF-8");
    EXPECT_EQ(language::formatsLocale("en_US", "SG"), "en_SG.UTF-8");
    EXPECT_EQ(language::formatsLocale("de_DE", "AT"), "de_AT.UTF-8");
    // No Japanese locale for Singapore: the region's own.
    EXPECT_EQ(language::formatsLocale("ja_JP", "SG"), "en_SG.UTF-8");
    EXPECT_EQ(language::formatsLocale("en_US", "JP"), "ja_JP.UTF-8");
    EXPECT_EQ(language::formatsLocale("en_US", ""), "");
    EXPECT_EQ(language::formatsLocale("en_US", "AQ"), "");
}

TEST(LanguageEnvironment, regionNamesTheRegionalCategories)
{
    if (!language::isAvailable("en_SG.UTF-8"))
        GTEST_SKIP() << "en_SG.UTF-8 is not generated";
    Environment environment {{"LC_TIME", "C.UTF-8"}, {"LC_COLLATE", "C.UTF-8"}};
    language::apply(environment, {"en_US"}, "SG");
    for (const char* category : {"LC_TIME", "LC_NUMERIC", "LC_MONETARY", "LC_MEASUREMENT", "LC_PAPER", "LC_ADDRESS",
                                 "LC_TELEPHONE", "LC_NAME"})
        EXPECT_EQ(environment[category], "en_SG.UTF-8") << category;
    EXPECT_EQ(environment["LC_COLLATE"], "C.UTF-8");
    EXPECT_NE(environment["LC_MESSAGES"], "en_SG.UTF-8");

    // Without a region the system's categories stay.
    Environment kept {{"LC_TIME", "C.UTF-8"}};
    language::apply(kept, {"en_US"});
    EXPECT_EQ(kept["LC_TIME"], "C.UTF-8");
    EXPECT_FALSE(kept.contains("LC_PAPER"));
}

TEST(ProxyEnvironment, manualModeSetsEveryVariable)
{
    ProxySettings settings;
    settings.mode = ProxySettings::Manual;
    settings.http = {"proxy.example.com", 3128};
    settings.httpUseAuthentication = true;
    settings.httpUser = "user name";
    settings.httpPassword = "p@ss";
    settings.socks = {"::1", 1080};
    settings.ignoreHosts = {"localhost", "*.example.com"};

    const auto variables = proxy::variables(settings);
    EXPECT_EQ(variables.at("http_proxy"), "http://user%20name:p%40ss@proxy.example.com:3128");
    // HTTPS falls back to the HTTP proxy, credentials included.
    EXPECT_EQ(variables.at("HTTPS_PROXY"), "http://user%20name:p%40ss@proxy.example.com:3128");
    EXPECT_EQ(variables.at("all_proxy"), "socks5://[::1]:1080");
    EXPECT_EQ(variables.at("NO_PROXY"), "localhost,.example.com");
    EXPECT_FALSE(variables.contains("ftp_proxy"));

    Environment environment {{"ftp_proxy", "http://stale:1"}, {"PATH", "/usr/bin"}};
    proxy::apply(environment, settings);
    EXPECT_FALSE(environment.contains("ftp_proxy"));
    EXPECT_EQ(environment.at("http_proxy"), variables.at("http_proxy"));
    EXPECT_EQ(environment.at("PATH"), "/usr/bin");

    settings.mode = ProxySettings::Auto;
    EXPECT_TRUE(proxy::variables(settings).empty());
}

TEST(GSettingsKeyfile, untouchedLinesStay)
{
    cutefish::base::file::TemporaryDirectory directory(std::filesystem::temp_directory_path().string());
    const std::string path = directory.path() + "/keyfile";
    ASSERT_TRUE(cutefish::base::file::writeAtomically(
            path, "# comment\n[system/proxy]\nmode='manual'\nother = 'kept'\n\n[org/gnome/desktop/interface]\n"
                  "gtk-theme='Adwaita'\n"));

    EXPECT_TRUE(keyfile::writeGroup(path, "system/proxy", {{"mode", gsettings::stringValue("none")}}));
    EXPECT_TRUE(keyfile::writeGroup(path, "system/proxy/http",
                                    {{"host", gsettings::stringValue("it's")}, {"port", gsettings::intValue(8080)}}));
    EXPECT_FALSE(keyfile::writeGroup(path, "system/proxy/http", {{"port", gsettings::intValue(8080)}}));

    const auto groups = keyfile::read(path);
    EXPECT_EQ(gsettings::toString(groups.at("system/proxy").at("mode")), "none");
    EXPECT_EQ(groups.at("system/proxy").at("other"), "'kept'");
    EXPECT_EQ(gsettings::toString(groups.at("system/proxy/http").at("host")), "it's");
    EXPECT_EQ(gsettings::toInt(groups.at("system/proxy/http").at("port"), 0), 8080);
    EXPECT_EQ(gsettings::toStringList(gsettings::stringListValue({"a", "b'c"}), {}),
              (std::vector<std::string> {"a", "b'c"}));
    EXPECT_TRUE(gsettings::toBool(gsettings::boolValue(true), false));

    const std::string contents = cutefish::base::file::read(path).value_or("");
    EXPECT_TRUE(contents.starts_with("# comment\n[system/proxy]\n"));
    EXPECT_NE(contents.find("other = 'kept'\n"), std::string::npos);
    EXPECT_NE(contents.find("gtk-theme='Adwaita'\n"), std::string::npos);
}
