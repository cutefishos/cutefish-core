// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "softwareupdate.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <utility>

#include "base/i18n.h"
#include "base/process.h"
#include "log.h"

using namespace cutefish::base;
using cutefish::i18n::tr;
using cutefish::i18n::trn;
using cutefish::packagekit::Updates;

static constexpr const char *kPath = "/org/cutefish/Services/SoftwareUpdate";
static constexpr const char *kInterface = "org.cutefish.Services.SoftwareUpdate";
static constexpr const char *kLastCheckKey = "SoftwareUpdate/LastCheck";
static constexpr const char *kRestartBootKey = "SoftwareUpdate/RestartBoot";
static constexpr int64_t kCheckAge = 24 * 60 * 60;
// The first check waits for the session to settle.
static constexpr std::chrono::minutes kFirstCheckDelay(2);
static constexpr std::chrono::hours kCheckInterval(1);

static std::string currentBootId()
{
    std::string id;
    std::ifstream("/proc/sys/kernel/random/boot_id") >> id;
    return id;
}

SoftwareUpdate::SoftwareUpdate(dbus::Connection *system, dbus::Connection *session, Settings &settings)
    : openSettings([] { Process::startDetached("cutefish-settings", {"-m", "softwareupdate"}); })
    , bootId(currentBootId)
    , m_system(system)
    , m_session(session)
    , m_settings(settings)
    , m_updatesNotification(session)
    , m_installedNotification(session)
{
    setupNotification(m_updatesNotification);
    setupNotification(m_installedNotification);

    m_timer.setSingleShot(true);
    m_timer.onTimeout = [this] {
        if (m_updates)
            checkIfDue();
        else
            findPackageKit();
        m_timer.start(kCheckInterval);
    };

    findPackageKit();
}

SoftwareUpdate::~SoftwareUpdate() = default;

bool SoftwareUpdate::publish()
{
    if (!m_session)
        return false;
    m_object = std::make_unique<dbus::Object>(m_session, kPath, kInterface);
    m_object->addProperty("state", [this] { return stateName(); });
    m_object->addProperty("updates", [this] { return updateList(); });
    m_object->addProperty("phase", [this] {
        return std::string(m_updates && m_updates->phase() == Updates::Phase::Installing ? "installing"
                                                                                         : "downloading");
    });
    m_object->addProperty("progress", [this] { return int32_t(m_updates ? m_updates->progress() : -1); });
    m_object->addProperty("canCancel", [this] { return m_updates && m_updates->canCancel(); });
    m_object->addProperty("error", [this] { return m_updates ? m_updates->error() : std::string(); });
    m_object->addMethod("check", [this] {
        if (!m_updates) {
            findPackageKit();
            return;
        }
        if (m_updates->isBusy())
            return;
        m_backgroundCheck = false;
        m_updates->check(true);
    });
    m_object->addMethod("update", [this] {
        if (m_updates)
            m_updates->update(true);
    });
    m_object->addMethod("cancel", [this] {
        if (m_updates)
            m_updates->cancel();
    });
    m_object->addMethod("restart", [this] {
        // The session quits the applications before it restarts, and stays
        // if one of them keeps it.
        dbus::Proxy sessionManager(m_session, "org.cutefish.Session", "/Session", "org.cutefish.Session");
        sessionManager.send("reboot");
    });
    return m_object->publish();
}

void SoftwareUpdate::checkIfDue()
{
    if (!m_updates || m_updates->isBusy() || m_updates->isCheckingInBackground())
        return;
    const int64_t lastCheck = m_settings.intValue(kLastCheckKey, 0);
    if (std::time(nullptr) - lastCheck < kCheckAge)
        return;
    m_backgroundCheck = true;
    m_updates->checkInBackground();
}

// PackageKit can be installed while the session runs, so a missing one is
// looked for again on every check.
void SoftwareUpdate::findPackageKit()
{
    m_availability = std::make_unique<cutefish::packagekit::Availability>(m_system, [this](bool available) {
        if (available && !m_updates) {
            start();
        } else if (!available) {
            CF_INFO(services::log()) << "PackageKit is not available; software updates are off";
            if (!m_timer.isActive())
                m_timer.start(kCheckInterval);
        }
    });
}

void SoftwareUpdate::start()
{
    m_updates = std::make_unique<Updates>(m_system);
    m_subscriptions.push_back(m_updates->changed.connect([this] { updatesChanged(); }));
    m_subscriptions.push_back(m_updates->checkFinished.connect([this](bool success) { checkFinished(success); }));
    m_subscriptions.push_back(m_updates->installed.connect([this] { installed(); }));
    m_updates->load();
    m_timer.start(kFirstCheckDelay);
}

void SoftwareUpdate::updatesChanged()
{
    if (!m_object)
        return;
    const std::string state = stateName();
    if (state != m_announcedState) {
        m_announcedState = state;
        m_object->emitPropertiesChanged({"state", "updates", "phase", "progress", "canCancel", "error"});
    } else {
        m_object->emitPropertiesChanged({"phase", "progress", "canCancel"});
    }
}

void SoftwareUpdate::checkFinished(bool success)
{
    const bool background = std::exchange(m_backgroundCheck, false);
    if (!success)
        return;
    m_settings.setValue(kLastCheckKey, int64_t(std::time(nullptr)));
    m_settings.sync();
    if (background && m_updates->state() == Updates::State::Available)
        notifyUpdates();
}

void SoftwareUpdate::installed()
{
    // PackageKit's apt backend asks for a restart only when a package left
    // /run/reboot-required, which Debian's packages do not, so every
    // install suggests one.
    m_settings.setValue(kRestartBootKey, bootId());
    m_settings.sync();
    m_installedNotification.setSummary(tr("Updates Installed"));
    m_installedNotification.setBody(tr("Restart the computer to finish installing them."));
    m_installedNotification.show();
}

void SoftwareUpdate::notifyUpdates()
{
    const uint64_t count = m_updates->packages().size();
    m_updatesNotification.setSummary(tr("Software Updates Available"));
    m_updatesNotification.setBody(trn("{n} update is available for this computer.",
                                      "{n} updates are available for this computer.", count));
    m_updatesNotification.show();
}

void SoftwareUpdate::setupNotification(Notification &notification)
{
    notification.setDesktopEntry("cutefish-settings");
    notification.setIconName("system-software-install");
    notification.setActivatable(true);
    m_subscriptions.push_back(notification.activated.connect([this] {
        if (openSettings)
            openSettings();
    }));
}

bool SoftwareUpdate::restartSuggested() const
{
    const std::string boot = m_settings.stringValue(kRestartBootKey);
    return !boot.empty() && boot == bootId();
}

std::vector<std::tuple<std::string, std::string, std::string>> SoftwareUpdate::updateList() const
{
    std::vector<std::tuple<std::string, std::string, std::string>> list;
    if (!m_updates)
        return list;
    for (const cutefish::packagekit::Package &package : m_updates->packages())
        list.emplace_back(package.name(), package.version(), package.summary);
    return list;
}

std::string SoftwareUpdate::stateName() const
{
    switch (m_updates ? m_updates->state() : Updates::State::Unknown) {
    case Updates::State::Unknown:
        return "unknown";
    case Updates::State::Checking:
        return "checking";
    case Updates::State::UpToDate:
        return restartSuggested() ? "restart" : "up-to-date";
    case Updates::State::Available:
        return "available";
    case Updates::State::Updating:
        return "updating";
    case Updates::State::Error:
        return "error";
    }
    return "unknown";
}
