// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "base/dbus.h"
#include "base/event_loop.h"
#include "base/settings.h"
#include "softwareupdate/softwareupdate.h"
#include "support/wait.h"

using namespace cutefish::base;
using namespace std::chrono_literals;

static const char kPackageKit[] = "org.freedesktop.PackageKit";
static const char kPackageKitPath[] = "/org/freedesktop/PackageKit";

namespace {

struct Notice {
    uint32_t replacesId;
    std::string summary;
    std::string body;
    std::vector<std::string> actions;
};

using PackageEntry = std::tuple<uint32_t, std::string, std::string>;

// PackageKit, the notification server and the session, on a connection of
// their own, so every call crosses the bus. PackageKit is on the system bus,
// which the session bus stands in for.
class Fakes
{
public:
    Fakes()
        : m_bus(dbus::Connection::open(dbus::Connection::Bus::Session))
    {
        m_daemon = std::make_unique<dbus::Object>(m_bus.get(), kPackageKitPath, kPackageKit);
        m_daemon->addMethod("CreateTransaction", [this] { return createTransaction(); });
        EXPECT_TRUE(m_daemon->publish());

        m_offline = std::make_unique<dbus::Object>(m_bus.get(), kPackageKitPath, "org.freedesktop.PackageKit.Offline");
        m_offline->addMethod("GetPrepared", [this] { return prepared; });
        m_offline->addMethod("Trigger", [this](const std::string &action) {
            calls.push_back("Trigger " + action);
        });
        m_offline->addMethod("ClearResults", [this] {
            calls.push_back("ClearResults");
            result.reset();
        });
        m_offline->addMethod("GetResults", "", "basutus", [this](dbus::MethodCall &call) {
            if (!result) {
                call.replyError("org.freedesktop.PackageKit.Offline.Error.NoData", "no update results available");
                return;
            }
            const auto &[success, ids, error] = *result;
            call.reply(success, ids, uint32_t(22), uint64_t(1794000000), uint32_t(success ? 0 : 4), error);
        });
        EXPECT_TRUE(m_offline->publish());

        m_notifications = std::make_unique<dbus::Object>(m_bus.get(), "/org/freedesktop/Notifications",
                                                         "org.freedesktop.Notifications");
        m_notifications->addMethod("Notify", "susssasa{sv}i", "u", [this](dbus::MethodCall &call) {
            std::string appName;
            uint32_t replacesId = 0;
            std::string icon;
            std::string summary;
            std::string body;
            std::vector<std::string> actions;
            dbus::VariantMap hints;
            int32_t timeout = 0;
            ASSERT_TRUE(call.read(appName, replacesId, icon, summary, body, actions, hints, timeout));
            notices.push_back({replacesId, summary, body, actions});
            call.reply(replacesId ? replacesId : uint32_t(notices.size()));
        });
        m_notifications->addMethod("CloseNotification", [](uint32_t) {});
        m_notifications->addSignal("NotificationClosed", "uu");
        m_notifications->addSignal("ActionInvoked", "us");
        EXPECT_TRUE(m_notifications->publish());

        m_session = std::make_unique<dbus::Object>(m_bus.get(), "/Session", "org.cutefish.Session");
        m_session->addMethod("reboot", [this] { calls.push_back("reboot"); });
        EXPECT_TRUE(m_session->publish());

        EXPECT_TRUE(m_bus->requestName("org.freedesktop.Notifications"));
        EXPECT_TRUE(m_bus->requestName("org.cutefish.Session"));
    }

    bool registerPackageKit() { return m_bus->requestName(kPackageKit); }

    void click(uint32_t id) { m_notifications->emitSignal("ActionInvoked", id, std::string("default")); }

    std::vector<PackageEntry> updates = {
        {5, "tzdata;2026f-0+deb13u1;all;debian-stable-updates", "time zone and daylight-saving time data"},
        {8, "openssl;3.5.4-1~deb13u2;amd64;debian-security", "Secure Sockets Layer toolkit"},
    };
    std::vector<std::string> prepared;
    bool failRefresh = false;
    // Leaves a download running until it is cancelled.
    bool holdDownload = false;
    // Success, the package ids and the error of the last offline update.
    std::optional<std::tuple<bool, std::vector<std::string>, std::string>> result;

    // PackageKit's methods and the session's reboot, in order.
    std::vector<std::string> calls;
    std::vector<Notice> notices;

private:
    dbus::ObjectPath createTransaction()
    {
        const std::string path = "/" + std::to_string(++m_transactions) + "_test";
        auto object = std::make_unique<dbus::Object>(m_bus.get(), path, "org.freedesktop.PackageKit.Transaction");
        dbus::Object *transaction = object.get();
        object->addSignal("Packages", "a(uss)");
        object->addSignal("ErrorCode", "us");
        object->addSignal("Finished", "uu");
        object->addMethod("SetHints", [](const std::vector<std::string> &) {});
        object->addMethod("Cancel", [this, transaction] {
            calls.push_back("Cancel");
            transaction->emitSignal("Finished", uint32_t(3), uint32_t(10));
        });
        object->addMethod("RefreshCache", [this, transaction](bool) {
            calls.push_back("RefreshCache");
            if (failRefresh)
                transaction->emitSignal("ErrorCode", uint32_t(2), std::string("No network connection"));
            transaction->emitSignal("Finished", uint32_t(failRefresh ? 2 : 1), uint32_t(10));
        });
        object->addMethod("GetUpdates", [this, transaction](uint64_t) {
            calls.push_back("GetUpdates");
            transaction->emitSignal("Packages", updates);
            transaction->emitSignal("Finished", uint32_t(1), uint32_t(10));
        });
        object->addMethod("UpdatePackages", [this, transaction](uint64_t, const std::vector<std::string> &ids) {
            calls.push_back("UpdatePackages");
            if (holdDownload)
                return;
            prepared = ids;
            transaction->emitSignal("Finished", uint32_t(1), uint32_t(10));
        });
        EXPECT_TRUE(object->publish());
        m_transactionObjects[path] = std::move(object);
        return {path};
    }

    std::unique_ptr<dbus::Connection> m_bus;
    std::unique_ptr<dbus::Object> m_daemon;
    std::unique_ptr<dbus::Object> m_offline;
    std::unique_ptr<dbus::Object> m_notifications;
    std::unique_ptr<dbus::Object> m_session;
    std::map<std::string, std::unique_ptr<dbus::Object>> m_transactionObjects;
    int m_transactions = 0;
};

class SoftwareUpdateTest : public testing::Test
{
protected:
    void SetUp() override
    {
        char directory[] = "/tmp/softwareupdate-test-XXXXXX";
        ASSERT_TRUE(mkdtemp(directory));
        m_directory = directory;
        settings = std::make_unique<Settings>(m_directory + "/services.conf");
    }

    void TearDown() override
    {
        service.reset();
        settings.reset();
        std::filesystem::remove_all(m_directory);
    }

    void start(bool withPackageKit = true)
    {
        if (withPackageKit)
            ASSERT_TRUE(fakes.registerPackageKit());
        service = std::make_unique<SoftwareUpdate>(dbus::Connection::session(), dbus::Connection::session(),
                                                   *settings);
        service->openSettings = [this] { ++settingsOpened; };
        ASSERT_TRUE(service->publish());
        m_proxy = dbus::Proxy(dbus::Connection::session(), dbus::Connection::session()->uniqueName(),
                              "/org/cutefish/Services/SoftwareUpdate", "org.cutefish.Services.SoftwareUpdate");
        m_changed = m_proxy.onPropertiesChanged([this](const dbus::VariantMap &changed,
                                                       const std::vector<std::string> &) {
            for (const auto &[name, value] : changed)
                m_properties.insert_or_assign(name, value);
        });
    }

    size_t updateCount()
    {
        std::optional<dbus::Reply> reply;
        dbus::PendingCall pending = m_proxy.getPropertyAsync("updates", [&](const dbus::Reply &answer) {
            reply = answer;
        });
        testsupport::waitFor(loop, [&] { return reply.has_value(); });
        const std::optional<dbus::Variant> value = reply ? reply->value<dbus::Variant>() : std::nullopt;
        using Entries = std::vector<std::tuple<std::string, std::string, std::string>>;
        return value ? value->get<Entries>().value_or(Entries()).size() : 0;
    }

    // As the last PropertiesChanged had them.
    std::string state() { return announced("state"); }
    std::string announced(const std::string &name)
    {
        const auto it = m_properties.find(name);
        return it == m_properties.end() ? std::string() : it->second.value<std::string>();
    }

    // Asks the service, which answers on this thread, so the call cannot block.
    std::string read(const std::string &name)
    {
        std::optional<dbus::Reply> reply;
        dbus::PendingCall pending = m_proxy.getPropertyAsync(name, [&](const dbus::Reply &answer) { reply = answer; });
        testsupport::waitFor(loop, [&] { return reply.has_value(); });
        const std::optional<dbus::Variant> value = reply ? reply->value<dbus::Variant>() : std::nullopt;
        return value ? value->get<std::string>().value_or(std::string()) : std::string();
    }

    // Calls the service as Settings does; the reply comes once the loop runs.
    std::optional<dbus::Reply> call(const std::string &method)
    {
        std::optional<dbus::Reply> reply;
        dbus::PendingCall pending = m_proxy.callAsync(method, [&](const dbus::Reply &answer) { reply = answer; });
        testsupport::waitFor(loop, [&] { return reply.has_value(); });
        return reply;
    }

    bool settlesIn(const std::string &expected)
    {
        return testsupport::waitFor(loop, [&] { return state() == expected; });
    }

    void settle() { testsupport::wait(loop, 150ms); }

    EventLoop loop;
    Fakes fakes;
    std::unique_ptr<Settings> settings;
    std::unique_ptr<SoftwareUpdate> service;
    int settingsOpened = 0;

private:
    std::string m_directory;
    dbus::Proxy m_proxy;
    dbus::Match m_changed;
    dbus::VariantMap m_properties;
};

} // namespace

TEST_F(SoftwareUpdateTest, ASessionStartsFromTheKnownLists)
{
    start();
    ASSERT_TRUE(settlesIn("available"));
    EXPECT_EQ(fakes.calls, (std::vector<std::string> {"GetUpdates"}));
    settle();
    EXPECT_TRUE(fakes.notices.empty());
    EXPECT_FALSE(settings->contains("SoftwareUpdate/LastCheck"));
}

TEST_F(SoftwareUpdateTest, ADueCheckAnnouncesTheUpdates)
{
    start();
    ASSERT_TRUE(settlesIn("available"));
    fakes.calls.clear();

    service->checkIfDue();
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return fakes.notices.size() == 1; }));
    EXPECT_EQ(fakes.calls, (std::vector<std::string> {"RefreshCache", "GetUpdates"}));
    EXPECT_EQ(fakes.notices[0].summary, "Software Updates Available");
    EXPECT_EQ(fakes.notices[0].body, "2 updates are available for this computer.");
    EXPECT_EQ(fakes.notices[0].actions, (std::vector<std::string> {"default", ""}));
    const int64_t lastCheck = settings->intValue("SoftwareUpdate/LastCheck", 0);
    EXPECT_LE(std::time(nullptr) - lastCheck, 5);

    // Not again on the same day.
    service->checkIfDue();
    settle();
    EXPECT_EQ(fakes.calls.size(), 2u);
    EXPECT_EQ(fakes.notices.size(), 1u);

    fakes.click(1);
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return settingsOpened == 1; }));
}

TEST_F(SoftwareUpdateTest, ADayAfterTheLastCheckItChecksAgain)
{
    settings->setValue("SoftwareUpdate/LastCheck", int64_t(std::time(nullptr) - 3600));
    start();
    ASSERT_TRUE(settlesIn("available"));
    service->checkIfDue();
    settle();
    EXPECT_EQ(fakes.calls, (std::vector<std::string> {"GetUpdates"}));

    settings->setValue("SoftwareUpdate/LastCheck", int64_t(std::time(nullptr) - 25 * 3600));
    service->checkIfDue();
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return fakes.notices.size() == 1; }));
    EXPECT_EQ(fakes.calls, (std::vector<std::string> {"GetUpdates", "RefreshCache", "GetUpdates"}));
}

TEST_F(SoftwareUpdateTest, AFailedBackgroundCheckStaysQuiet)
{
    fakes.failRefresh = true;
    start();
    ASSERT_TRUE(settlesIn("available"));
    service->checkIfDue();
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return fakes.calls.size() == 2; }));
    settle();
    EXPECT_EQ(read("state"), "available");
    EXPECT_EQ(read("error"), "");
    EXPECT_EQ(updateCount(), 2u);
    EXPECT_TRUE(fakes.notices.empty());
    // Tried again at the next hour.
    EXPECT_FALSE(settings->contains("SoftwareUpdate/LastCheck"));
}

TEST_F(SoftwareUpdateTest, ReadyStaysReadyWhenABackgroundCheckFails)
{
    fakes.prepared = {"tzdata;2026f-0+deb13u1;all;debian-stable-updates",
                      "openssl;3.5.4-1~deb13u2;amd64;debian-security"};
    fakes.failRefresh = true;
    start();
    ASSERT_TRUE(settlesIn("ready"));
    service->checkIfDue();
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return fakes.calls.size() == 2; }));
    settle();
    EXPECT_EQ(fakes.calls, (std::vector<std::string> {"GetUpdates", "RefreshCache"}));
    EXPECT_EQ(read("state"), "ready");
    EXPECT_EQ(updateCount(), 2u);
    EXPECT_TRUE(fakes.notices.empty());
    EXPECT_FALSE(settings->contains("SoftwareUpdate/LastCheck"));

    // The next hourly poll tries again.
    fakes.failRefresh = false;
    service->checkIfDue();
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return fakes.notices.size() == 1; }));
    EXPECT_EQ(fakes.notices[0].summary, "Updates Ready to Install");
    EXPECT_EQ(read("state"), "ready");
    EXPECT_TRUE(settings->contains("SoftwareUpdate/LastCheck"));
}

TEST_F(SoftwareUpdateTest, SettingsDownloadsAndRestarts)
{
    start();
    ASSERT_TRUE(settlesIn("available"));

    // Nothing to install yet.
    std::optional<dbus::Reply> reply = call("restartAndUpdate");
    ASSERT_TRUE(reply);
    EXPECT_TRUE(reply->isError());

    ASSERT_TRUE(call("download"));
    ASSERT_TRUE(settlesIn("ready"));
    EXPECT_EQ(fakes.prepared.size(), 2u);

    reply = call("restartAndUpdate");
    ASSERT_TRUE(reply);
    EXPECT_FALSE(reply->isError());
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return fakes.calls.back() == "reboot"; }));
    EXPECT_EQ(fakes.calls, (std::vector<std::string> {"GetUpdates", "UpdatePackages", "Trigger reboot", "reboot"}));
}

TEST_F(SoftwareUpdateTest, SettingsStopsADownload)
{
    fakes.holdDownload = true;
    start();
    ASSERT_TRUE(settlesIn("available"));
    ASSERT_TRUE(call("download"));
    ASSERT_TRUE(settlesIn("downloading"));
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return fakes.calls.size() == 2; }));
    ASSERT_TRUE(call("cancel"));
    ASSERT_TRUE(settlesIn("available"));
    EXPECT_EQ(fakes.calls, (std::vector<std::string> {"GetUpdates", "UpdatePackages", "Cancel"}));
    EXPECT_EQ(read("error"), "");
    EXPECT_EQ(updateCount(), 2u);
    EXPECT_TRUE(fakes.prepared.empty());
}

TEST_F(SoftwareUpdateTest, ACheckFromSettingsDoesNotNotify)
{
    start();
    ASSERT_TRUE(settlesIn("available"));
    ASSERT_TRUE(call("check"));
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return fakes.calls.size() == 3; }));
    ASSERT_TRUE(settlesIn("available"));
    settle();
    EXPECT_TRUE(fakes.notices.empty());
    EXPECT_TRUE(settings->contains("SoftwareUpdate/LastCheck"));
}

TEST_F(SoftwareUpdateTest, ReportsAnInstallOnce)
{
    fakes.result = std::make_tuple(true, std::vector<std::string> {"a;1;all;installed", "b;2;all;installed"},
                                   std::string());
    start();
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return fakes.notices.size() == 1; }));
    EXPECT_EQ(fakes.notices[0].summary, "Updates Installed");
    EXPECT_EQ(fakes.notices[0].body, "2 updates were installed.");
    ASSERT_TRUE(testsupport::waitFor(loop, [&] {
        return std::find(fakes.calls.begin(), fakes.calls.end(), "ClearResults") != fakes.calls.end();
    }));
    EXPECT_FALSE(fakes.result);
}

TEST_F(SoftwareUpdateTest, ReportsAFailedInstall)
{
    fakes.result = std::make_tuple(false, std::vector<std::string>(), std::string("dpkg returned an error code (1)"));
    start();
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return fakes.notices.size() == 1; }));
    EXPECT_EQ(fakes.notices[0].summary, "Updates Not Installed");
    EXPECT_EQ(fakes.notices[0].body, "The updates could not be installed: dpkg returned an error code (1)");
}

TEST_F(SoftwareUpdateTest, WithoutPackageKitNothingHappens)
{
    start(false);
    settle();
    service->checkIfDue();
    settle();
    EXPECT_EQ(read("state"), "unknown");
    EXPECT_TRUE(fakes.calls.empty());
    EXPECT_TRUE(fakes.notices.empty());
    std::optional<dbus::Reply> reply = call("check");
    ASSERT_TRUE(reply);
    settle();
    EXPECT_TRUE(fakes.calls.empty());
}
