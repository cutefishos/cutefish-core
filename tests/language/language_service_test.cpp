// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "appearance/fontconfigwriter.h"
#include "base/dbus.h"
#include "base/event_loop.h"
#include "base/file.h"
#include "base/settings.h"
#include "locale/language.h"
#include "support/wait.h"

using namespace cutefish::base;
using namespace std::chrono_literals;

namespace {

// org.freedesktop.Notifications on a connection of its own.
class Notifications
{
public:
    Notifications()
        : m_bus(dbus::Connection::open(dbus::Connection::Bus::Session))
    {
        m_object = std::make_unique<dbus::Object>(m_bus.get(), "/org/freedesktop/Notifications",
                                                  "org.freedesktop.Notifications");
        m_object->addMethod("Notify", [this](const std::string &, uint32_t, const std::string &, const std::string &,
                                             const std::string &, const std::vector<std::string> &,
                                             const dbus::VariantMap &, int32_t) {
            return uint32_t(++count);
        });
        EXPECT_TRUE(m_object->publish());
        EXPECT_TRUE(m_bus->requestName("org.freedesktop.Notifications"));
    }

    int count = 0;

private:
    std::unique_ptr<dbus::Connection> m_bus;
    std::unique_ptr<dbus::Object> m_object;
};

} // namespace

TEST(LanguageServiceTest, NotificationsOnlyForPrimaryChanges)
{
    EventLoop loop;
    file::TemporaryDirectory directory("/tmp", "language-test");
    ASSERT_TRUE(directory.isValid());
    Notifications notifications;

    Settings settings(directory.path() + "/services.conf");
    settings.setValue("Language/language", "en_US");
    settings.sync();

    int fontConfigs = 0;
    Language language(settings, dbus::Connection::session(), {[&fontConfigs] { ++fontConfigs; }});

    language.setPreferredLanguages({"en_US", "ja_JP", "de_DE"});
    testsupport::wait(loop, 30ms);
    EXPECT_EQ(notifications.count, 0);
    EXPECT_EQ(fontConfigs, 1);

    language.setPreferredLanguages({"en_US", "de_DE", "ja_JP"});
    testsupport::wait(loop, 30ms);
    EXPECT_EQ(notifications.count, 0);
    EXPECT_EQ(language.preferredLanguages(), (std::vector<std::string> {"en_US", "de_DE", "ja_JP"}));

    language.setPreferredLanguages({"ja_JP", "en_US", "de_DE"});
    EXPECT_TRUE(testsupport::waitFor(loop, [&] { return notifications.count == 1; }));
    EXPECT_EQ(language.languageCode(), "ja_JP");

    language.setPreferredLanguages({"ja_JP", "de_DE"});
    testsupport::wait(loop, 30ms);
    EXPECT_EQ(notifications.count, 1);

    language.setPreferredLanguages({"de_DE"});
    EXPECT_TRUE(testsupport::waitFor(loop, [&] { return notifications.count == 2; }));

    language.setPreferredLanguages({"de_DE"});
    testsupport::wait(loop, 30ms);
    EXPECT_EQ(notifications.count, 2);

    // Kept where the session reads it.
    Settings saved(directory.path() + "/services.conf");
    EXPECT_EQ(saved.stringValue("Language/language"), "de_DE");
    EXPECT_EQ(saved.stringListValue("Language/preferredLanguages"), std::vector<std::string> {"de_DE"});
}

TEST(LanguageServiceTest, MissingLocaleIsGeneratedThenPublishedAgain)
{
    file::TemporaryDirectory directory("/tmp", "language-test");
    ASSERT_TRUE(directory.isValid());

    Settings settings(directory.path() + "/services.conf");
    settings.setValue("Language/language", "en_US");
    settings.sync();

    Language language(settings, nullptr, {});
    int published = 0;
    language.applyEnvironment = [&published](const std::map<std::string, std::string> &) { ++published; };
    std::vector<std::string> requested;
    std::function<void(bool)> finish;
    language.generateLocale = [&](const std::string &locale, std::function<void(bool)> done) {
        requested.push_back(locale);
        finish = std::move(done);
        return dbus::PendingCall();
    };

    // Kalaallisut: a locale no test system has generated.
    language.setPreferredLanguages({"kl_GL", "en_US"});
    EXPECT_EQ(published, 1);
    ASSERT_EQ(requested, std::vector<std::string> {"kl_GL.UTF-8"});

    finish(false);
    EXPECT_EQ(published, 1);
    finish(true);
    EXPECT_EQ(published, 2);
}

TEST(LanguageServiceTest, NamesLocalesAsTheSessionDoes)
{
    EXPECT_EQ(Language::localeName("ja_JP"), "ja_JP");
    EXPECT_EQ(Language::localeName("zh_CN"), "zh_CN");
    EXPECT_EQ(Language::localeName("zh_TW"), "zh_TW");
    EXPECT_EQ(Language::localeName("en"), "en_US");
    EXPECT_EQ(Language::localeName("de"), "de_DE");
}
