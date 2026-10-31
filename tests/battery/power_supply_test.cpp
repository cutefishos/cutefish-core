// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "base/dbus.h"
#include "base/event_loop.h"
#include "base/settings.h"
#include "battery/powersupply.h"
#include "support/wait.h"

using namespace cutefish::base;

static const char devices[] = "/org/freedesktop/UPower/devices/";
static const char deviceInterface[] = "org.freedesktop.UPower.Device";

namespace {

// UPower with its devices and display device, on a thread of its own: the
// power supply asks it with blocking calls.
class FakeUPower
{
public:
    FakeUPower()
    {
        std::promise<bool> ready;
        m_thread = std::thread([this, &ready] { run(ready); });
        m_ready = ready.get_future().get();
    }
    ~FakeUPower()
    {
        m_handle.post([this] { m_loop->quit(); });
        m_thread.join();
    }

    bool isReady() const { return m_ready; }

    // A device of `type`, as UPower's Type property numbers them.
    void add(const std::string &name, uint32_t type, bool powerSupply)
    {
        inThread([&] {
            addDevice(name, type, powerSupply);
            m_upower->emitSignal("DeviceAdded", dbus::ObjectPath{devices + name});
        });
    }

    void remove(const std::string &name)
    {
        inThread([&] {
            m_devices.erase(name);
            m_upower->emitSignal("DeviceRemoved", dbus::ObjectPath{devices + name});
        });
    }

    // UPower stopping; its devices stay as they are for the next start.
    void stop()
    {
        inThread([&] { m_bus->releaseName("org.freedesktop.UPower"); });
    }

    // Leaves a device out of the next start, without a signal.
    void forget(const std::string &name)
    {
        inThread([&] { m_devices.erase(name); });
    }

    bool start()
    {
        bool owned = false;
        inThread([&] { owned = m_bus->requestName("org.freedesktop.UPower"); });
        return owned;
    }

    void enableThreshold(const std::string &name)
    {
        inThread([&] {
            m_thresholds[name] = true;
            m_devices[name]->emitPropertiesChanged({"ChargeThresholdEnabled"});
        });
    }

    void changeDisplay(uint32_t state, double percentage)
    {
        inThread([&] {
            m_state = state;
            m_percentage = percentage;
            m_display->emitPropertiesChanged({"State", "Percentage"});
        });
    }

private:
    template<typename F>
    void inThread(F f)
    {
        std::promise<void> done;
        m_handle.post([&] {
            f();
            done.set_value();
        });
        done.get_future().get();
    }

    void addDevice(const std::string &name, uint32_t type, bool powerSupply)
    {
        auto device = std::make_unique<dbus::Object>(m_bus.get(), devices + name, deviceInterface);
        device->addProperty("Type", [type] { return type; });
        device->addProperty("PowerSupply", [powerSupply] { return powerSupply; });
        device->addProperty("ChargeThresholdEnabled", [this, name] { return m_thresholds[name]; });
        EXPECT_TRUE(device->publish());
        m_devices[name] = std::move(device);
    }

    void run(std::promise<bool> &ready)
    {
        EventLoop loop;
        m_loop = &loop;
        m_handle = loop.handle();
        m_bus = dbus::Connection::open(dbus::Connection::Bus::Session, &loop);

        m_upower = std::make_unique<dbus::Object>(m_bus.get(), "/org/freedesktop/UPower", "org.freedesktop.UPower");
        m_upower->addProperty("OnBattery", [] { return true; });
        m_upower->addMethod("EnumerateDevices", [this] {
            std::vector<dbus::ObjectPath> paths;
            for (const auto &[name, device] : m_devices)
                paths.push_back({devices + name});
            return paths;
        });
        m_upower->addSignal("DeviceAdded", "o");
        m_upower->addSignal("DeviceRemoved", "o");

        m_display = std::make_unique<dbus::Object>(m_bus.get(), std::string(devices) + "DisplayDevice",
                                                   deviceInterface);
        m_display->addProperty("State", [this] { return m_state; });
        m_display->addProperty("Percentage", [this] { return m_percentage; });
        m_display->addProperty("TimeToEmpty", [] { return int64_t(0); });
        m_display->addProperty("TimeToFull", [] { return int64_t(0); });
        m_display->addProperty("ChargeThresholdEnabled", [] { return false; });

        // Before the power supply starts: it lists them at once.
        addDevice("battery_BAT1", 2, true);
        addDevice("battery_BAT0", 2, true);
        addDevice("mouse_hid", 5, false);
        // A peripheral reporting itself as a battery, which the display device leaves out.
        addDevice("battery_hidpp", 2, false);

        ready.set_value(m_upower->publish() && m_display->publish() && m_bus->requestName("org.freedesktop.UPower"));
        loop.run();

        m_devices.clear();
        m_display.reset();
        m_upower.reset();
        m_bus.reset();
    }

    std::thread m_thread;
    EventLoop *m_loop = nullptr;
    LoopHandle m_handle;
    bool m_ready = false;
    std::unique_ptr<dbus::Connection> m_bus;
    std::unique_ptr<dbus::Object> m_upower;
    std::unique_ptr<dbus::Object> m_display;
    std::map<std::string, std::unique_ptr<dbus::Object>> m_devices;
    std::map<std::string, bool> m_thresholds;
    uint32_t m_state = 2;
    double m_percentage = 57.4;
};

class PowerSupplyTest : public testing::Test
{
protected:
    void SetUp() override
    {
        if (!dbus::Connection::session())
            GTEST_SKIP() << "No session bus; run the tests in dbus-run-session";
        ASSERT_TRUE(upower.isReady());
        m_reader = dbus::Connection::open(dbus::Connection::Bus::Session, &loop);
    }

    // The published battery's properties, read over a connection of their own.
    dbus::VariantMap published()
    {
        dbus::VariantMap properties;
        bool answered = false;
        const dbus::Proxy battery(m_reader.get(), dbus::Connection::session()->uniqueName(),
                                  "/org/cutefish/Services/Battery", "org.cutefish.Services.Battery");
        const dbus::PendingCall call = battery.getAllAsync([&](const dbus::Reply &reply) {
            properties = reply.value<dbus::VariantMap>().value_or(dbus::VariantMap());
            answered = true;
        });
        testsupport::waitFor(loop, [&] { return answered; });
        return properties;
    }

    std::vector<std::string> batteries()
    {
        const dbus::VariantMap properties = published();
        const auto it = properties.find("batteries");
        std::vector<std::string> paths;
        if (it != properties.end()) {
            for (const dbus::ObjectPath &path : it->second.value<std::vector<dbus::ObjectPath>>())
                paths.push_back(path.path);
        }
        return paths;
    }

    std::string statusString()
    {
        const dbus::VariantMap properties = published();
        const auto it = properties.find("statusString");
        return it == properties.end() ? std::string() : it->second.value<std::string>();
    }

    int chargePercent()
    {
        const dbus::VariantMap properties = published();
        const auto it = properties.find("chargePercent");
        return it == properties.end() ? -1 : it->second.value<int32_t>(-1);
    }

    EventLoop loop;
    FakeUPower upower;
    Settings settings{testing::TempDir() + "power_supply_test.conf"};

private:
    std::unique_ptr<dbus::Connection> m_reader;
};

} // namespace

TEST_F(PowerSupplyTest, PublishesTheBatteriesTogether)
{
    // UPower is on the system bus, which the session bus stands in for.
    PowerSupply supply(dbus::Connection::session(), dbus::Connection::session(), settings);

    EXPECT_TRUE(testsupport::waitFor(loop, [&] {
        return batteries() == std::vector<std::string>{std::string(devices) + "battery_BAT0",
                                                       std::string(devices) + "battery_BAT1"};
    }));
    EXPECT_EQ(chargePercent(), 57);

    upower.changeDisplay(2, 55.6);
    EXPECT_TRUE(testsupport::waitFor(loop, [&] { return chargePercent() == 56; }));
}

TEST_F(PowerSupplyTest, AnyBatteryHoldingTheLimitPausesCharging)
{
    PowerSupply supply(dbus::Connection::session(), dbus::Connection::session(), settings);
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return batteries().size() == 2u; }));

    // UPower's pending-charge.
    upower.changeDisplay(5, 79.6);
    EXPECT_TRUE(testsupport::waitFor(loop, [&] { return statusString() == "Not charging"; }));

    upower.enableThreshold("battery_BAT1");
    EXPECT_TRUE(testsupport::waitFor(loop, [&] { return statusString() == "Charging paused at 80%"; }));
}

TEST_F(PowerSupplyTest, FollowsBatteriesComingAndGoing)
{
    PowerSupply supply(dbus::Connection::session(), dbus::Connection::session(), settings);
    ASSERT_TRUE(testsupport::waitFor(loop, [&] { return batteries().size() == 2u; }));

    upower.remove("battery_BAT1");
    EXPECT_TRUE(testsupport::waitFor(loop, [&] {
        return batteries() == std::vector<std::string>{std::string(devices) + "battery_BAT0"};
    }));

    upower.add("battery_BAT2", 2, true);
    upower.add("keyboard_hid", 6, false);
    EXPECT_TRUE(testsupport::waitFor(loop, [&] {
        return batteries() == std::vector<std::string>{std::string(devices) + "battery_BAT0",
                                                       std::string(devices) + "battery_BAT2"};
    }));

    // Without a battery nothing is published.
    upower.remove("battery_BAT0");
    upower.remove("battery_BAT2");
    EXPECT_TRUE(testsupport::waitFor(loop, [&] { return published().empty(); }));
}

TEST_F(PowerSupplyTest, FollowsUPowerRestarting)
{
    upower.stop();
    PowerSupply supply(dbus::Connection::session(), dbus::Connection::session(), settings);
    testsupport::wait(loop, std::chrono::milliseconds(200));
    EXPECT_TRUE(published().empty());

    ASSERT_TRUE(upower.start());
    EXPECT_TRUE(testsupport::waitFor(loop, [&] { return batteries().size() == 2u; }));
    EXPECT_TRUE(supply.onBattery());

    upower.stop();
    EXPECT_TRUE(testsupport::waitFor(loop, [&] { return published().empty(); }));

    upower.forget("battery_BAT1");
    ASSERT_TRUE(upower.start());
    EXPECT_TRUE(testsupport::waitFor(loop, [&] {
        return batteries() == std::vector<std::string>{std::string(devices) + "battery_BAT0"};
    }));
    EXPECT_EQ(chargePercent(), 57);
}
