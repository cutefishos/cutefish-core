// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "base/dbus.h"
#include "base/event_loop.h"
#include "cutefish/display/display_types.h"
#include "power/displaypolicy.h"
#include "support/wait.h"

using namespace cutefish::base;
using namespace cutefish::display;
using namespace std::chrono_literals;

static const char displayService[] = "org.cutefish.DisplayConfig.PowerTest";

namespace {

// What the fakes were asked, read from the test thread.
struct Log {
    int locks = 0;
    int suspends = 0;
    std::vector<bool> interactive;
    std::vector<bool> docked;
    std::vector<uint32_t> power;
};

class Recorder
{
public:
    Log log() const
    {
        std::lock_guard locker(m_mutex);
        return m_log;
    }

    template<typename F>
    void update(F &&change)
    {
        std::lock_guard locker(m_mutex);
        change(m_log);
    }

private:
    mutable std::mutex m_mutex;
    Log m_log;
};

// The screen saver, logind and the compositor's display service. The policy
// blocks on Lock, so they answer from a thread of their own, on a connection
// of their own. logind is on the same bus, which stands in for the system bus.
class Services
{
public:
    Services()
    {
        std::promise<void> ready;
        m_thread = std::thread([this, &ready] {
            EventLoop loop;
            m_loop = &loop;
            m_bus = dbus::Connection::open(dbus::Connection::Bus::Session, &loop);
            publish();
            ready.set_value();
            loop.run();
            m_objects.clear();
            m_bus.reset();
            if (m_lidInhibitor >= 0)
                ::close(m_lidInhibitor);
        });
        ready.get_future().wait();
    }

    ~Services()
    {
        run([this] {
            m_loop->quit();
        });
        m_thread.join();
    }

    // Runs on the fakes' thread and waits for it.
    template<typename F>
    void run(F &&function)
    {
        std::promise<void> done;
        m_loop->post([&] {
            function();
            done.set_value();
        });
        done.get_future().wait();
    }

    bool holdsLidInhibitor()
    {
        bool held = false;
        run([&] {
            if (m_lidInhibitor < 0)
                return;
            // The holder closing its end hangs up the read end kept here.
            pollfd fd {m_lidInhibitor, 0, 0};
            held = ::poll(&fd, 1, 0) == 0;
        });
        return held;
    }

    void setLocked(bool locked)
    {
        run([&] {
            m_screenSaver->emitSignal("ActiveChanged", locked);
        });
    }

    void sleepAndWake()
    {
        run([this] {
            m_logind->emitSignal("PrepareForSleep", true);
            m_logind->emitSignal("PrepareForSleep", false);
        });
    }

    void releaseSleepInhibitors()
    {
        run([this] {
            inhibited = false;
            m_bus->emitSignal("/org/freedesktop/login1", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                              std::string("org.freedesktop.login1.Manager"),
                              dbus::VariantMap {{"BlockInhibited", dbus::Variant(std::string())}},
                              std::vector<std::string>());
        });
    }

    void setBlockInhibited(const std::string &types)
    {
        run([this, types] {
            blockInhibited = types;
            m_bus->emitSignal("/org/freedesktop/login1", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                              std::string("org.freedesktop.login1.Manager"),
                              dbus::VariantMap {{"BlockInhibited", dbus::Variant(types)}},
                              std::vector<std::string>());
        });
    }

    void changeTopology()
    {
        run([this] {
            ++m_generation;
            ++m_revision;
            m_display->emitSignal("StateChanged", m_generation, m_revision);
        });
    }

    Recorder recorder;
    // Set through run().
    bool lockFails = false;
    // Refuses Suspend as logind does while a sleep inhibitor is held.
    bool inhibited = false;
    // logind's BlockInhibited: the blocked inhibitor types, colon-separated.
    std::string blockInhibited;
    bool external = false;
    int busyAnswers = 0;

private:
    void publish()
    {
        auto screenSaver = std::make_unique<dbus::Object>(m_bus.get(), "/ScreenSaver", "org.freedesktop.ScreenSaver");
        screenSaver->addMethod("Lock", "", "", [this](dbus::MethodCall &call) {
            recorder.update([](Log &log) {
                ++log.locks;
            });
            if (lockFails)
                call.replyError("org.freedesktop.DBus.Error.Failed", "Session is not available");
            else
                call.reply();
        });
        screenSaver->addSignal("ActiveChanged", "b");
        screenSaver->publish();
        m_screenSaver = screenSaver.get();
        m_objects.push_back(std::move(screenSaver));

        auto logind = std::make_unique<dbus::Object>(m_bus.get(), "/org/freedesktop/login1",
                                                     "org.freedesktop.login1.Manager");
        logind->addMethod("Inhibit", "ssss", "h", [this](dbus::MethodCall &call) {
            std::string what;
            int fds[2];
            if (!call.read(what) || what != "handle-lid-switch" || ::pipe(fds) != 0) {
                call.replyError("org.freedesktop.DBus.Error.NotSupported", what);
                return;
            }
            if (m_lidInhibitor >= 0)
                ::close(m_lidInhibitor);
            m_lidInhibitor = fds[0];
            call.reply(dbus::UnixFd::adopt(fds[1]));
        });
        logind->addMethod("Suspend", "b", "", [this](dbus::MethodCall &call) {
            bool interactive = true;
            call.read(interactive);
            recorder.update([interactive](Log &log) {
                ++log.suspends;
                log.interactive.push_back(interactive);
            });
            if (inhibited)
                call.replyError("org.freedesktop.DBus.Error.InteractiveAuthorizationRequired", "Operation inhibited");
            else
                call.reply();
        });
        logind->addProperty("BlockInhibited", [this] {
            return blockInhibited;
        });
        logind->addSignal("PrepareForSleep", "b");
        logind->publish();
        m_logind = logind.get();
        m_objects.push_back(std::move(logind));

        auto display = std::make_unique<dbus::Object>(m_bus.get(), "/org/cutefish/DisplayConfig1",
                                                      "org.cutefish.DisplayConfig1");
        display->addMethod("GetState", [this] {
            return std::make_tuple(std::string("test-instance"), std::string("seat0"), m_generation, m_revision,
                                   std::vector<PhysicalDisplay>(), std::vector<LogicalDisplay>(),
                                   std::vector<Placement>(), std::string(), uint64_t(0));
        });
        display->addMethod("SetDockedLidClosed", [this](bool closed) {
            recorder.update([closed](Log &log) {
                log.docked.push_back(closed);
            });
            if (closed && busyAnswers > 0) {
                --busyAnswers;
                return reject(ErrorCode::Busy);
            }
            if (closed && !external)
                return reject(ErrorCode::NoEnabledOutput);
            return succeed();
        });
        display->addMethod("SetDisplayPower", [this](const std::vector<std::string> &, uint32_t state) {
            recorder.update([state](Log &log) {
                log.power.push_back(state);
            });
            return succeed();
        });
        display->addSignal("StateChanged", "tt");
        display->addSignal("OperationFinished", "sbbusastt");
        display->publish();
        m_display = display.get();
        m_objects.push_back(std::move(display));

        m_bus->requestName("org.freedesktop.ScreenSaver");
        m_bus->requestName("org.freedesktop.login1");
        m_bus->requestName(displayService);
    }

    std::string reject(ErrorCode error)
    {
        const std::string token = nextToken();
        m_display->emitSignal("OperationFinished", token, false, false, uint32_t(error), std::string(),
                              std::vector<std::string>(), m_generation, m_revision);
        return token;
    }

    std::string succeed()
    {
        const std::string token = nextToken();
        m_loop->post([this, token] {
            m_display->emitSignal("OperationFinished", token, true, false, uint32_t(0), std::string(),
                                  std::vector<std::string>(), m_generation, m_revision);
        });
        return token;
    }

    std::string nextToken() { return "token-" + std::to_string(++m_serial); }

    std::thread m_thread;
    EventLoop *m_loop = nullptr;
    std::unique_ptr<dbus::Connection> m_bus;
    std::vector<std::unique_ptr<dbus::Object>> m_objects;
    dbus::Object *m_screenSaver = nullptr;
    dbus::Object *m_logind = nullptr;
    dbus::Object *m_display = nullptr;
    int m_lidInhibitor = -1;
    uint64_t m_generation = 1;
    uint64_t m_revision = 1;
    int m_serial = 0;
};

class PowerPolicyTest : public testing::Test
{
protected:
    EventLoop loop;
    Services services;

    std::unique_ptr<DisplayPolicy> start()
    {
        // No idle notifier: the tests report idleness themselves.
        dbus::Connection *bus = dbus::Connection::session();
        auto policy = std::make_unique<DisplayPolicy>(nullptr, nullptr, bus, bus, displayService);
        // Lets the policy's display client load the compositor's state first.
        testsupport::wait(loop, 100ms);
        return policy;
    }

    void setLocked(bool locked)
    {
        services.setLocked(locked);
        testsupport::wait(loop, 100ms);
    }

    template<typename F>
    bool until(F &&condition, std::chrono::milliseconds timeout = 5s)
    {
        return testsupport::waitFor(loop, std::forward<F>(condition), timeout);
    }

    Log log() const { return services.recorder.log(); }
};

} // namespace

TEST_F(PowerPolicyTest, SingleDisplayLidLocksAndSuspends)
{
    auto policy = start();
    policy->setLidPresent(true);

    EXPECT_TRUE(until([&] { return services.holdsLidInhibitor(); }));

    policy->handleLidClosed();
    ASSERT_TRUE(until([&] { return log().suspends == 1; }));
    EXPECT_EQ(log().interactive, std::vector<bool> {false});
    EXPECT_EQ(log().docked, std::vector<bool> {true});
    EXPECT_EQ(log().locks, 1);
    EXPECT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1}; }));
    EXPECT_TRUE(until([&] { return !services.holdsLidInhibitor(); }));

    policy->handleLidOpened();
    EXPECT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1, 0}; }));
    EXPECT_TRUE(until([&] { return services.holdsLidInhibitor(); }));
}

TEST_F(PowerPolicyTest, FailedLockStillSuspends)
{
    services.run([&] {
        services.lockFails = true;
    });
    auto policy = start();
    policy->setLidPresent(true);

    policy->handleLidClosed();
    ASSERT_TRUE(until([&] { return log().suspends == 1; }));
    EXPECT_EQ(log().locks, 1);
    EXPECT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1}; }));
}

TEST_F(PowerPolicyTest, DockedLidKeepsTheSessionAwake)
{
    services.run([&] {
        services.external = true;
    });
    auto policy = start();
    policy->setLidPresent(true);

    policy->handleLidClosed();
    ASSERT_TRUE(until([&] { return log().docked == std::vector<bool> {true}; }));
    testsupport::wait(loop, 200ms);
    EXPECT_EQ(log().locks, 0);
    EXPECT_EQ(log().suspends, 0);
    EXPECT_TRUE(log().power.empty());

    policy->handleLidOpened();
    ASSERT_TRUE(until([&] { return log().docked == std::vector<bool> {true, false}; }));
    EXPECT_EQ(log().suspends, 0);
    EXPECT_TRUE(log().power.empty());
}

TEST_F(PowerPolicyTest, LosingTheExternalDisplaySuspends)
{
    services.run([&] {
        services.external = true;
    });
    auto policy = start();
    policy->setLidPresent(true);
    policy->handleLidClosed();
    ASSERT_TRUE(until([&] { return log().docked == std::vector<bool> {true}; }));
    testsupport::wait(loop, 100ms);

    services.run([&] {
        services.external = false;
    });
    services.changeTopology();
    ASSERT_TRUE(until([&] { return log().suspends == 1; }));
    EXPECT_EQ(log().docked, (std::vector<bool> {true, true}));
    // The panel is already off: the lock comes from powering everything off.
    EXPECT_EQ(log().locks, 0);
    EXPECT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1}; }));

    policy->handleLidOpened();
    EXPECT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1, 0}; }));
}

TEST_F(PowerPolicyTest, KeepingAnotherExternalDisplayStaysDocked)
{
    services.run([&] {
        services.external = true;
    });
    auto policy = start();
    policy->setLidPresent(true);
    policy->handleLidClosed();
    ASSERT_TRUE(until([&] { return log().docked == std::vector<bool> {true}; }));
    testsupport::wait(loop, 100ms);

    services.changeTopology();
    ASSERT_TRUE(until([&] { return log().docked == std::vector<bool> {true, true}; }));
    testsupport::wait(loop, 200ms);
    EXPECT_EQ(log().suspends, 0);
}

TEST_F(PowerPolicyTest, ADisplayChangeInProgressIsWaitedOut)
{
    services.run([&] {
        services.external = true;
        services.busyAnswers = 2;
    });
    auto policy = start();
    policy->setLidPresent(true);

    policy->handleLidClosed();
    ASSERT_TRUE(until([&] { return log().docked == std::vector<bool> {true, true, true}; }, 3s));
    testsupport::wait(loop, 200ms);
    EXPECT_EQ(log().suspends, 0);
    EXPECT_EQ(log().locks, 0);
}

TEST_F(PowerPolicyTest, ADisplayChangeThatNeverSettlesStillSuspends)
{
    services.run([&] {
        services.external = true;
        services.busyAnswers = 1000;
    });
    auto policy = start();
    policy->setLidPresent(true);

    policy->handleLidClosed();
    ASSERT_TRUE(until([&] { return log().suspends == 1; }, 10s));
    EXPECT_EQ(log().docked.size(), 11u);
}

TEST_F(PowerPolicyTest, ALockedScreenGoesDarkSoonAndSleepsEvenWithNever)
{
    auto policy = start();
    policy->setPolicy(-1, 1);
    setLocked(true);

    policy->idleFor(30);
    EXPECT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1}; }));
    EXPECT_TRUE(until([&] { return log().suspends == 1; }));
}

TEST_F(PowerPolicyTest, AnUnlockedScreenKeepsTheUsersTimeout)
{
    auto policy = start();
    policy->setPolicy(1200, 1);

    policy->idleFor(30);
    testsupport::wait(loop, 200ms);
    EXPECT_TRUE(log().power.empty());
}

TEST_F(PowerPolicyTest, LockingAfterTheUserLeftGoesDarkAtOnce)
{
    auto policy = start();
    policy->setPolicy(1200, 900);
    policy->idleFor(30);
    testsupport::wait(loop, 100ms);
    EXPECT_TRUE(log().power.empty());

    setLocked(true);
    EXPECT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1}; }));
}

TEST_F(PowerPolicyTest, WakingTheLockScreenLightsItAgain)
{
    auto policy = start();
    policy->setPolicy(1200, 900);
    setLocked(true);
    policy->idleFor(30);
    ASSERT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1}; }));

    policy->userReturned();
    ASSERT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1, 0}; }));
    policy->idleFor(30);
    EXPECT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1, 0, 1}; }));
}

TEST_F(PowerPolicyTest, WakingWhileDockedLetsTheDisplayGoDarkAgain)
{
    services.run([&] {
        services.external = true;
    });
    auto policy = start();
    policy->setLidPresent(true);
    policy->handleLidClosed();
    ASSERT_TRUE(until([&] { return log().docked == std::vector<bool> {true}; }));
    testsupport::wait(loop, 100ms);

    policy->setPolicy(1200, 900);
    policy->idleFor(1200);
    ASSERT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1}; }));

    policy->userReturned();
    testsupport::wait(loop, 100ms);
    policy->idleFor(1200);
    ASSERT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1, 1}; }));
    // Powering every display on would light the closed panel.
    const std::vector<uint32_t> power = log().power;
    EXPECT_EQ(std::count(power.begin(), power.end(), 0u), 0);
}

TEST_F(PowerPolicyTest, PreventingSleepKeepsTheSystemAwake)
{
    auto policy = start();
    policy->setPolicy(-1, 0);
    setLocked(true);

    policy->idleFor(30);
    ASSERT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1}; }));
    testsupport::wait(loop, 1500ms);
    EXPECT_EQ(log().suspends, 0);
}

TEST_F(PowerPolicyTest, AnInhibitedSleepWaitsWithoutAPrompt)
{
    services.run([&] {
        services.inhibited = true;
    });
    auto policy = start();
    policy->setPolicy(60, 1);

    policy->idleFor(60);
    ASSERT_TRUE(until([&] { return log().suspends == 1; }));
    EXPECT_EQ(log().interactive, std::vector<bool> {false});
    testsupport::wait(loop, 1500ms);
    EXPECT_EQ(log().suspends, 1);

    services.releaseSleepInhibitors();
    ASSERT_TRUE(until([&] { return log().suspends == 2; }));
    EXPECT_EQ(log().interactive, (std::vector<bool> {false, false}));
}

TEST_F(PowerPolicyTest, AnIdleInhibitorKeepsTheSystemAwake)
{
    services.setBlockInhibited("handle-lid-switch:idle");
    auto policy = start();
    policy->setPolicy(60, 1);

    policy->idleFor(60);
    ASSERT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1}; }));
    testsupport::wait(loop, 1500ms);
    EXPECT_EQ(log().suspends, 0);

    services.setBlockInhibited("handle-lid-switch");
    ASSERT_TRUE(until([&] { return log().suspends == 1; }));
    EXPECT_EQ(log().interactive, std::vector<bool> {false});
}

TEST_F(PowerPolicyTest, AnIdleInhibitorDoesNotHoldAClosedLid)
{
    services.setBlockInhibited("idle");
    auto policy = start();
    policy->setLidPresent(true);
    EXPECT_TRUE(until([&] { return services.holdsLidInhibitor(); }));

    policy->handleLidClosed();
    ASSERT_TRUE(until([&] { return log().suspends == 1; }));
}

TEST_F(PowerPolicyTest, AnInhibitedClosedLidIsLeftToLogind)
{
    services.run([&] {
        services.inhibited = true;
    });
    auto policy = start();
    policy->setLidPresent(true);
    ASSERT_TRUE(until([&] { return services.holdsLidInhibitor(); }));

    // logind's own lid handling sleeps through the inhibitor that refused this.
    policy->handleLidClosed();
    ASSERT_TRUE(until([&] { return log().suspends == 1; }));
    EXPECT_TRUE(until([&] { return !services.holdsLidInhibitor(); }));

    // It also sleeps again after a wake with the lid still closed.
    services.sleepAndWake();
    testsupport::wait(loop, 200ms);
    EXPECT_FALSE(services.holdsLidInhibitor());

    policy->handleLidOpened();
    EXPECT_TRUE(until([&] { return services.holdsLidInhibitor(); }));
}

TEST_F(PowerPolicyTest, DockedLidStillSleepsWhenIdle)
{
    services.run([&] {
        services.external = true;
    });
    auto policy = start();
    policy->setLidPresent(true);
    policy->handleLidClosed();
    ASSERT_TRUE(until([&] { return log().docked == std::vector<bool> {true}; }));
    testsupport::wait(loop, 100ms);

    policy->setPolicy(1200, 1);
    policy->idleFor(1200);
    ASSERT_TRUE(until([&] { return log().power == std::vector<uint32_t> {1}; }));
    ASSERT_TRUE(until([&] { return log().suspends == 1; }));
    // An external display is in use, so the lid stays Cutefish's.
    EXPECT_TRUE(services.holdsLidInhibitor());
}

TEST_F(PowerPolicyTest, AWakeWithoutInputSleepsAgain)
{
    auto policy = start();
    policy->setPolicy(60, 1);
    policy->idleFor(60);
    ASSERT_TRUE(until([&] { return log().suspends == 1; }));

    services.sleepAndWake();
    ASSERT_TRUE(until([&] { return log().suspends == 2; }));

    services.sleepAndWake();
    testsupport::wait(loop, 200ms);
    policy->userReturned();
    testsupport::wait(loop, 1500ms);
    EXPECT_EQ(log().suspends, 2);
}
