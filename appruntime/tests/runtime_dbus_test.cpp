// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "base/dbus.h"
#include "base/event_loop.h"
#include "base/process.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <functional>
#include <tuple>
#include <poll.h>

#include <gtest/gtest.h>

using namespace cutefish::base;

static constexpr char kService[] = "org.cutefish.AppRuntime";
static constexpr char kPath[] = "/AppRuntime";

// (ssssssouso): a unit as systemd's manager lists it.
using UnitInfo = std::tuple<std::string, std::string, std::string, std::string, std::string, std::string,
                            dbus::ObjectPath, uint32_t, std::string, dbus::ObjectPath>;

static UnitInfo activeScope(const std::string& name)
{
    return {name, name, "loaded", "active", "running", "", dbus::ObjectPath {"/org/freedesktop/systemd1/unit/x"},
            0, "", dbus::ObjectPath {"/"}};
}

namespace {

// The runtime from the build tree on a bus of its own.
class RuntimeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        bus = dbus::Connection::open(dbus::Connection::Bus::Session, &loop);
        ASSERT_TRUE(bus);
        watch = bus->watchName(kService, [this](const std::string& name) { owner = name; });
        runtimeProcess.setOutputForwarded(true);
        ASSERT_TRUE(runtimeProcess.start(APPRUNTIME_BINARY));
        ASSERT_TRUE(runUntil([&] { return !owner.empty(); }, 5000));
    }

    void TearDown() override
    {
        runtimeProcess.terminate();
        EXPECT_TRUE(runtimeProcess.waitForFinished(std::chrono::seconds(3)));
    }

    dbus::Proxy runtime() { return dbus::Proxy(bus.get(), kService, kPath, kService); }

    bool runUntil(const std::function<bool()>& condition, int milliseconds)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (!condition()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return condition();
            pollfd fd {loop.fd(), POLLIN, 0};
            ::poll(&fd, 1, 10);
            loop.dispatch();
        }
        return true;
    }

    EventLoop loop;
    std::unique_ptr<dbus::Connection> bus;
    Process runtimeProcess {&loop};
    std::string owner;
    dbus::NameWatch watch;
};

} // namespace

TEST_F(RuntimeTest, launchesTracksAndQuitsCommands)
{
    std::vector<std::pair<std::string, uint32_t>> launched;
    std::vector<std::pair<std::string, uint32_t>> quit;
    const dbus::Proxy proxy = runtime();
    const dbus::Match launchedMatch = proxy.onSignal("applicationLaunched", [&](const dbus::Message& message) {
        std::string appId;
        uint32_t pid = 0;
        if (message.read(appId, pid))
            launched.emplace_back(appId, pid);
    });
    const dbus::Match quitMatch = proxy.onSignal("applicationQuit", [&](const dbus::Message& message) {
        std::string appId;
        uint32_t pid = 0;
        if (message.read(appId, pid))
            quit.emplace_back(appId, pid);
    });

    // The form without an activation token, and the one with it.
    const uint32_t first = proxy.call("launchCommand", std::string("sleeper"),
                                      std::vector<std::string> {"sleep", "30"}, std::string())
                                   .value<uint32_t>()
                                   .value_or(0);
    const uint32_t second = proxy.call("launchCommand", std::string(), std::vector<std::string> {"sleep", "30"},
                                       std::string("/"), std::string("token"))
                                    .value<uint32_t>()
                                    .value_or(0);
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);

    EXPECT_TRUE(proxy.call("isRunning", std::string("sleeper")).value<bool>().value_or(false));
    // Without an id, the executable's name is the key.
    EXPECT_EQ(proxy.call("pidsForApplication", std::string("sleep")).value<std::vector<uint32_t>>(),
              std::vector<uint32_t> {second});
    auto running = proxy.call("runningApplications").value<std::vector<std::string>>().value_or(std::vector<std::string>());
    std::sort(running.begin(), running.end());
    EXPECT_EQ(running, (std::vector<std::string> {"sleep", "sleeper"}));

    ASSERT_TRUE(runUntil([&] { return launched.size() == 2; }, 2000));

    EXPECT_TRUE(proxy.call("quitApplication", std::string("sleeper")).value<bool>().value_or(false));
    ASSERT_TRUE(runUntil([&] { return quit.size() == 1; }, 5000));
    EXPECT_EQ(quit.front(), std::make_pair(std::string("sleeper"), first));

    EXPECT_TRUE(proxy.call("quitAll").value<bool>().value_or(false));
    ASSERT_TRUE(runUntil([&] { return quit.size() == 2; }, 5000));
    EXPECT_FALSE(proxy.call("isRunning", std::string("sleep")).value<bool>().value_or(true));
}

// A stand-in for the user manager: quitAll has it stop every application
// scope, the ones the runtime made and those applications moved into.
TEST_F(RuntimeTest, quitAllStopsEveryApplicationScope)
{
    std::vector<std::string> stopped;
    dbus::Object manager(bus.get(), "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager");
    manager.addMethod(
            "ListUnitsByPatterns",
            [](std::vector<std::string>, std::vector<std::string> patterns) {
                std::vector<UnitInfo> units;
                if (std::find(patterns.begin(), patterns.end(), "app-*.scope") != patterns.end()) {
                    units.push_back(activeScope("app-cutefish-sleeper-123.scope"));
                    units.push_back(activeScope("app-org.example.Browser-456.scope"));
                }
                return units;
            },
            dbus::ArgumentNames {{"states", "patterns"}, {"units"}});
    manager.addMethod(
            "StopUnit",
            [&stopped](std::string name, std::string) {
                stopped.push_back(name);
                return dbus::ObjectPath {"/org/freedesktop/systemd1/job/1"};
            },
            dbus::ArgumentNames {{"name", "mode"}, {"job"}});
    ASSERT_TRUE(manager.publish());
    ASSERT_TRUE(bus->requestName("org.freedesktop.systemd1", dbus::Connection::DoNotQueue));

    runtime().call("quitAll");
    ASSERT_TRUE(runUntil([&] { return stopped.size() == 2; }, 5000));
    std::sort(stopped.begin(), stopped.end());
    EXPECT_EQ(stopped, (std::vector<std::string> {"app-cutefish-sleeper-123.scope", "app-org.example.Browser-456.scope"}));
}

TEST_F(RuntimeTest, refusesWhatItCannotStart)
{
    const dbus::Proxy proxy = runtime();
    EXPECT_EQ(proxy.call("launchCommand", std::string(), std::vector<std::string> {"/does-not-exist/program"},
                         std::string())
                      .value<uint32_t>(),
              0u);
    EXPECT_EQ(proxy.call("launchApplication", std::string("no-such-entry"), std::vector<std::string>())
                      .value<uint32_t>(),
              0u);
    EXPECT_EQ(proxy.call("launchApplication", std::string("no-such-entry"), std::vector<std::string>(),
                         std::string())
                      .value<uint32_t>(),
              0u);
    // A pid it did not start.
    EXPECT_FALSE(proxy.call("forceQuitByPid", uint32_t(1)).value<bool>().value_or(true));
    EXPECT_FALSE(proxy.call("quitByPid", uint32_t(1)).value<bool>().value_or(true));
}
