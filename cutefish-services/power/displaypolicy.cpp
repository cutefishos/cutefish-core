// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "displaypolicy.h"
#include "brightness/brightnessmanager.h"
#include "idle/waylandidle.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "log.h"

using namespace std::chrono_literals;
using namespace cutefish::base;

static constexpr auto s_screenSaverService = "org.freedesktop.ScreenSaver";
static constexpr auto s_screenSaverPath = "/ScreenSaver";
static constexpr auto s_screenSaverInterface = "org.freedesktop.ScreenSaver";
static constexpr auto s_logindService = "org.freedesktop.login1";
static constexpr auto s_logindPath = "/org/freedesktop/login1";
static constexpr auto s_logindInterface = "org.freedesktop.login1.Manager";

// Without native display power the panel fades to a half, then to an eighth of
// its brightness, before it goes dark at the full timeout.
static int firstFadeAt(int timeout)
{
    return timeout / 2;
}

static int secondFadeAt(int timeout)
{
    return timeout * 3 / 4;
}

// An idle panel dims slowly enough not to draw the eye.
static constexpr std::chrono::milliseconds s_idleFade = 1000ms;

// Whether a colon-separated list of logind inhibitor types names "idle".
static bool listsIdle(const std::optional<std::string> &types)
{
    if (!types)
        return false;

    size_t start = 0;
    while (start <= types->size()) {
        const size_t end = std::min(types->find(':', start), types->size());
        if (types->compare(start, end - start, "idle") == 0)
            return true;
        start = end + 1;
    }
    return false;
}

// A locked screen nobody is using goes dark this soon, whatever the user chose.
static constexpr int s_lockedDisplayOffTimeout = 30;

// Bounds the wait for a display change to settle, so a closed lid still sleeps.
static constexpr std::chrono::milliseconds s_dockedLidRetryInterval = 500ms;
static constexpr int s_dockedLidRetries = 10;

using cutefish::display::DisplayClient;
using cutefish::display::ErrorCode;

DisplayPolicy::DisplayPolicy(WaylandIdle *idle, BrightnessManager *brightness,
                             dbus::Connection *session, dbus::Connection *system,
                             const std::string &displayService)
    : m_idle(idle)
    , m_brightness(brightness)
    , m_screenSaver(session, s_screenSaverService, s_screenSaverPath, s_screenSaverInterface)
    , m_logind(system, s_logindService, s_logindPath, s_logindInterface)
    , m_display(displayService)
{
    if (m_idle) {
        m_idleSubscriptions.push_back(m_idle->timeoutReached.connect([this](int identifier, int milliseconds) {
            if (!m_idleIds.contains(identifier))
                return;

            m_away = true;
            idleFor(milliseconds / 1000);
        }));
        // Every notification that fired reports the same return.
        m_idleSubscriptions.push_back(m_idle->resumed.connect([this] {
            if (std::exchange(m_away, false))
                userReturned();
        }));
    }
    if (session) {
        m_matches.push_back(m_screenSaver.onSignal("ActiveChanged", [this](const dbus::Message &message) {
            if (const auto active = message.argument<bool>())
                onScreenSaverActiveChanged(*active);
        }));
    }
    if (system) {
        m_matches.push_back(m_logind.onPropertiesChanged(
                [this](const dbus::VariantMap &changed, const std::vector<std::string> &invalidated) {
                    onLogindPropertiesChanged(changed, invalidated);
                }));
        m_matches.push_back(m_logind.onSignal("PrepareForSleep", [this](const dbus::Message &message) {
            if (const auto sleeping = message.argument<bool>())
                onPrepareForSleep(*sleeping);
        }));
    }
    m_suspendTimer.setSingleShot(true);
    m_suspendTimer.onTimeout = [this] {
        onSuspendTimerTimeout();
    };
    m_dockedLidRetryTimer.setSingleShot(true);
    m_dockedLidRetryTimer.setInterval(s_dockedLidRetryInterval);
    m_dockedLidRetryTimer.onTimeout = [this] {
        requestDockedLid(m_lidClosed);
    };
    m_wakeTimer.setSingleShot(true);
    m_wakeTimer.setInterval(0ms);
    m_wakeTimer.onTimeout = [this] {
        if (!m_lidClosed) {
            restoreDisplay();
        } else if (m_dockedLidActive && m_displayPoweredOff) {
            // Input has already woken the external displays; powering every display
            // on would also light the closed panel.
            m_displayPoweredOff = false;
            m_displayOff = false;
            m_displayOffTimer.invalidate();
        }
    };
    m_displayStateChanged = m_display.stateChanged.connect([this] {
        onDisplayStateChanged();
    });
}

DisplayPolicy::~DisplayPolicy()
{
    for (int identifier : std::exchange(m_idleIds, {}))
        m_idle->removeTimeout(identifier);
}

int DisplayPolicy::watchIdle(int seconds)
{
    if (!m_idle || seconds < 0 || seconds > INT_MAX / 1000)
        return -1;

    const int identifier = m_idle->addTimeout(seconds * 1000);
    m_idleIds.insert(identifier);
    return identifier;
}

void DisplayPolicy::unwatchIdle(int identifier)
{
    if (m_idleIds.erase(identifier))
        m_idle->removeTimeout(identifier);
}

void DisplayPolicy::setLidPresent(bool present)
{
    if (m_lidPresent == present)
        return;

    m_lidPresent = present;
    updateLidSwitchInhibitor();
}

bool DisplayPolicy::hasNativeDisplayPower() const
{
    return m_display.available();
}

bool DisplayPolicy::setDisplayPower(bool on)
{
    return m_display.setDisplayPower(on);
}

int DisplayPolicy::screenBrightness() const
{
    return m_brightness ? m_brightness->brightness() : -1;
}

void DisplayPolicy::setScreenBrightness(int value, std::chrono::milliseconds fade)
{
    if (m_brightness)
        m_brightness->setValue(value, fade);
}

void DisplayPolicy::userReturned()
{
    m_lockedIdle = false;
    if (!m_dimmed)
        return;

    m_dimmed = false;
    m_suspendRequested = false;
    if (idleSleepApplies())
        stopSuspendCountdown();

    // The compositor wakes the outputs on the same input; restore after it.
    m_wakeTimer.start();
}

void DisplayPolicy::onPrepareForSleep(bool sleeping)
{
    if (sleeping) {
        // Whoever asked for it, this sleep is the one that was due.
        m_suspendTimer.stop();
        m_suspendDue = false;
        return;
    }

    m_suspendRequested = false;

    // A wake without input (a timer, the power supply, a swallowed key) leaves the
    // display off; sleep again unless someone comes back. logind does this for a
    // closed lid once its holdoff has passed.
    if (m_displayOff && idleSleepApplies())
        startSuspendCountdown();
}

void DisplayPolicy::idleFor(int seconds)
{
    if (seconds == s_lockedDisplayOffTimeout) {
        m_lockedIdle = true;
        if (m_locked)
            turnDisplayOffWhenIdle();
    }

    // A notification queued before a policy change can still arrive.
    if (m_timeout < 0)
        return;

    if (seconds == m_timeout) {
        turnDisplayOffWhenIdle();
    } else if (!hasNativeDisplayPower()) {
        if (seconds == secondFadeAt(m_timeout))
            fade(8);
        else if (seconds == firstFadeAt(m_timeout))
            fade(2);
    }
}

void DisplayPolicy::fade(int divisor)
{
    if (m_savedBrightness < 0)
        m_savedBrightness = screenBrightness();

    if (m_savedBrightness > 0)
        setScreenBrightness(int(std::lround(double(m_savedBrightness) / divisor)), s_idleFade);

    m_dimmed = true;
}

void DisplayPolicy::turnDisplayOffWhenIdle()
{
    if (m_displayOff)
        return;

    turnDisplayOff();
    m_dimmed = true;
    startSuspendCountdown();
    trySuspend();
}

void DisplayPolicy::setPolicy(int timeout, int suspendDelay)
{
    for (int identifier : std::exchange(m_policyIdleTimeouts, {}))
        unwatchIdle(identifier);

    m_timeout = timeout;
    m_suspendDelay = std::max(0, suspendDelay);
    m_suspendTimer.stop();
    if (idleSleepApplies())
        m_suspendDue = false;

    // Watched even for "never": the lock screen still goes dark. Watching anew
    // restarts its countdown, but only a fired notification reports the return.
    if (m_lockedIdleTimeout >= 0 && !m_lockedIdle) {
        unwatchIdle(m_lockedIdleTimeout);
        m_lockedIdleTimeout = -1;
    }
    if (m_lockedIdleTimeout < 0)
        m_lockedIdleTimeout = watchIdle(s_lockedDisplayOffTimeout);

    if (timeout >= 0) {
        const int firstFade = firstFadeAt(timeout);
        const int secondFade = secondFadeAt(timeout);

        if (firstFade > 0)
            m_policyIdleTimeouts.push_back(watchIdle(firstFade));
        if (secondFade > firstFade && secondFade < timeout)
            m_policyIdleTimeouts.push_back(watchIdle(secondFade));
        m_policyIdleTimeouts.push_back(watchIdle(timeout));
    }

    if (m_displayOff && idleSleepApplies() && m_displayOffTimer.isValid() && m_suspendDelay > 0) {
        const int64_t remaining = int64_t(m_suspendDelay) * 1000 - m_displayOffTimer.elapsedMs();
        if (remaining <= 0) {
            m_suspendDue = true;
            trySuspend();
        } else {
            m_suspendTimer.start(std::chrono::milliseconds(remaining));
        }
    }
}

void DisplayPolicy::onScreenSaverActiveChanged(bool active)
{
    m_locked = active;
    if (active) {
        // Locked after the user had already left: nobody is there to see it.
        if (m_lockedIdle)
            turnDisplayOffWhenIdle();
        return;
    }
    m_dimmed = false;
    if (idleSleepApplies())
        stopSuspendCountdown();
    if (!m_lidClosed && m_displayOff)
        restoreDisplay();
}

void DisplayPolicy::onLogindPropertiesChanged(const dbus::VariantMap &changedProperties,
                                              const std::vector<std::string> &invalidatedProperties)
{
    static const char *const names[] = {"BlockInhibited", "BlockWeakInhibited", "DelayInhibited"};
    const bool inhibitorsChanged = std::any_of(std::begin(names), std::end(names), [&](const char *name) {
        return changedProperties.contains(name)
                || std::find(invalidatedProperties.begin(), invalidatedProperties.end(), name)
                != invalidatedProperties.end();
    });
    if (!inhibitorsChanged)
        return;

    trySuspend();
}

void DisplayPolicy::onSuspendTimerTimeout()
{
    if (!m_displayOff)
        return;

    m_suspendDue = true;
    trySuspend();
}

bool DisplayPolicy::lockSession()
{
    if (!m_screenSaver.isValid())
        return false;

    // Wait for the compositor to establish the lock before continuing.
    const dbus::Reply reply = m_screenSaver.call("Lock");
    if (reply.isError()) {
        CF_WARNING(services::log()) << "Unable to lock the session:" << reply.error().message;
        return false;
    }

    return true;
}

void DisplayPolicy::trySuspend()
{
    if (!m_suspendDue || !m_displayOff || m_suspendRequested)
        return;

    // An idle inhibitor holds off sleeping for idleness, as it does logind's own idle
    // action, but not a closed lid; the suspend is tried again when the inhibitors change.
    if (!m_lidSleeping
        && (listsIdle(m_logind.property<std::string>("BlockInhibited"))
            || listsIdle(m_logind.property<std::string>("BlockWeakInhibited"))))
        return;

    // Not interactive: with a sleep inhibitor held, logind would otherwise wait on a
    // password prompt nobody can see. A refusal is retried when the inhibitors change.
    m_suspendRequested = true;
    const uint64_t serial = ++m_suspendSerial;
    m_suspendCall = m_logind.callAsync(
            "Suspend",
            [this, serial](const dbus::Reply &reply) {
                if (serial != m_suspendSerial)
                    return;

                if (reply.isError()) {
                    CF_INFO(services::log()) << "Suspend refused:" << reply.error().message;
                    m_suspendRequested = false;
                    return;
                }

                m_suspendDue = false;
            },
            false);
}

void DisplayPolicy::startSuspendCountdown()
{
    m_suspendTimer.stop();
    m_suspendDue = false;

    if (!idleSleepApplies())
        return;

    m_displayOffTimer.restart();
    if (m_suspendDelay > 0)
        m_suspendTimer.start(std::chrono::seconds(m_suspendDelay));
}

void DisplayPolicy::stopSuspendCountdown()
{
    m_suspendTimer.stop();
    m_suspendDue = false;
    m_displayOffTimer.invalidate();
}

void DisplayPolicy::turnDisplayOff()
{
    if (m_displayOff)
        return;

    m_displayPoweredOff = setDisplayPower(false);
    if (!m_displayPoweredOff) {
        if (m_savedBrightness < 0)
            m_savedBrightness = screenBrightness();
        if (m_savedBrightness > 0)
            setScreenBrightness(0, s_idleFade);
    }

    m_displayOff = true;
}

void DisplayPolicy::restoreDisplay()
{
    if (std::exchange(m_displayPoweredOff, false))
        setDisplayPower(true);

    if (m_savedBrightness > 0)
        setScreenBrightness(m_savedBrightness, BrightnessManager::DefaultFade);

    m_savedBrightness = -1;
    m_displayOff = false;
    m_displayOffTimer.invalidate();
}

void DisplayPolicy::handleLidClosed()
{
    m_lidClosed = true;

    if (!m_lidPresent)
        return;

    // The compositor decides whether an external display stays in use.
    m_dockedLidRetries = 0;
    requestDockedLid(true);
}

void DisplayPolicy::sleepWithLidClosed(bool waitForLock)
{
    if (!m_displayOff) {
        // Suspend even without the lock: the compositor locks as the displays go
        // off and again before sleep.
        if (waitForLock && !lockSession())
            CF_WARNING(services::log()) << "Suspending with the lid closed before the session is locked";

        turnDisplayOff();
        m_dimmed = true;
    }

    m_lidDisplayOff = true;
    m_lidSleeping = true;
    m_suspendTimer.stop();
    m_displayOffTimer.invalidate();

    // logind's own lid handling sleeps through block inhibitors, and sleeps again
    // after a wake with the lid still closed.
    updateLidSwitchInhibitor();

    if (!m_suspendRequested) {
        m_suspendDue = true;
        trySuspend();
    }
}

bool DisplayPolicy::idleSleepApplies() const
{
    return !m_lidClosed || m_dockedLidActive;
}

void DisplayPolicy::handleLidOpened()
{
    m_lidClosed = false;
    m_lidSleeping = false;
    m_suspendRequested = false;
    stopSuspendCountdown();
    updateLidSwitchInhibitor();
    m_dockedLidRecheck = false;
    m_dockedLidRetries = 0;
    m_dockedLidRetryTimer.stop();

    if (m_dockedLidPending || m_dockedLidActive) {
        m_dockedLidPending = false;
        m_dockedLidActive = false;
        requestDockedLid(false);
        m_lidDisplayOff = false;
        return;
    }

    if (!m_lidDisplayOff)
        return;

    m_lidDisplayOff = false;
    restoreDisplay();
}

void DisplayPolicy::requestDockedLid(bool closed)
{
    m_dockedLidRetryTimer.stop();
    const uint64_t serial = ++m_dockedLidSerial;
    const bool sent = m_display.setDockedLidClosed(
            closed, [this, serial, closed](bool succeeded, ErrorCode error) {
                // A later lid event has superseded this request.
                if (serial == m_dockedLidSerial)
                    finishDockedLid(closed, succeeded, error);
            });

    m_dockedLidPending = sent && closed;
    if (!sent && closed)
        finishDockedLid(true, false, ErrorCode::Unavailable);
}

void DisplayPolicy::finishDockedLid(bool closed, bool succeeded, ErrorCode error)
{
    const bool recheck = std::exchange(m_dockedLidRecheck, false);
    m_dockedLidPending = false;

    // A display change in progress answers Busy; ask again once it settles.
    if (!succeeded && error == ErrorCode::Busy && m_dockedLidRetries < s_dockedLidRetries) {
        ++m_dockedLidRetries;
        m_dockedLidRetryTimer.start();
        return;
    }

    m_dockedLidRetries = 0;
    if (!closed)
        return;

    if (succeeded) {
        m_dockedLidActive = true;
        m_lidDisplayOff = true;
        if (recheck)
            requestDockedLid(true);
        return;
    }

    // After an undock the internal panel is already off, so a lock would wait for a
    // frame it never presents; powering every display off locks the session instead.
    const bool wasDocked = std::exchange(m_dockedLidActive, false);
    sleepWithLidClosed(!wasDocked);
}

void DisplayPolicy::onDisplayStateChanged()
{
    const DisplayClient::State &state = m_display.state();

    // Nothing is applied while a display change is still in progress.
    if (state.appliedRevision == 0)
        return;

    auto topology = std::make_tuple(state.instanceId, state.topologyGeneration, state.appliedRevision);
    if (topology == m_displayTopology)
        return;
    m_displayTopology = std::move(topology);

    // logind's lid handling is inhibited, so losing the external display while
    // docked would otherwise leave a closed laptop awake.
    if (!m_lidClosed)
        return;

    if (m_dockedLidPending)
        m_dockedLidRecheck = true;
    else if (m_dockedLidActive)
        requestDockedLid(true);
}

void DisplayPolicy::updateLidSwitchInhibitor()
{
    if (!m_lidPresent || m_lidSleeping) {
        m_lidSwitchInhibitor = dbus::UnixFd();
        return;
    }

    if (m_lidSwitchInhibitor.isValid() || m_lidSwitchInhibiting || !m_logind.isValid())
        return;

    m_lidSwitchInhibiting = true;
    m_inhibitCall = m_logind.callAsync(
            "Inhibit",
            [this](const dbus::Reply &reply) {
                m_lidSwitchInhibiting = false;
                const auto inhibitor = reply.value<dbus::UnixFd>();
                if (!inhibitor) {
                    CF_WARNING(services::log())
                            << "Unable to inhibit systemd lid handling:" << reply.error().message;
                    return;
                }

                // A lid that closed meanwhile stays with logind.
                if (m_lidPresent && !m_lidSleeping)
                    m_lidSwitchInhibitor = *inhibitor;
            },
            std::string("handle-lid-switch"), std::string("Cutefish"),
            std::string("Cutefish manages lid power policy"), std::string("block"));
}
