// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "softwareupdate.h"

#include <chrono>
#include <ctime>
#include <utility>

#include "base/i18n.h"
#include "base/process.h"
#include "log.h"

using namespace cutefish::base;
using cutefish::i18n::format;
using cutefish::i18n::tr;
using cutefish::i18n::trn;
using cutefish::packagekit::Updates;

static constexpr const char *kPath = "/org/cutefish/Services/SoftwareUpdate";
static constexpr const char *kInterface = "org.cutefish.Services.SoftwareUpdate";
static constexpr const char *kLastCheckKey = "SoftwareUpdate/LastCheck";
static constexpr int64_t kCheckAge = 24 * 60 * 60;
// The first check waits for the session to settle.
static constexpr std::chrono::minutes kFirstCheckDelay(2);
static constexpr std::chrono::hours kCheckInterval(1);

SoftwareUpdate::SoftwareUpdate(dbus::Connection *system, dbus::Connection *session, Settings &settings)
    : openSettings([] { Process::startDetached("cutefish-settings", {"-m", "softwareupdate"}); })
    , m_system(system)
    , m_session(session)
    , m_settings(settings)
    , m_updatesNotification(session)
    , m_resultNotification(session)
{
    setupNotification(m_updatesNotification);
    setupNotification(m_resultNotification);

    m_timer.setSingleShot(true);
    m_timer.onTimeout = [this] {
        checkIfDue();
        m_timer.start(kCheckInterval);
    };

    m_availability = std::make_unique<cutefish::packagekit::Availability>(m_system, [this](bool available) {
        if (available)
            start();
        else
            CF_INFO(services::log()) << "PackageKit is not available; software updates are off";
    });
}

SoftwareUpdate::~SoftwareUpdate() = default;

bool SoftwareUpdate::publish()
{
    if (!m_session)
        return false;
    m_object = std::make_unique<dbus::Object>(m_session, kPath, kInterface);
    m_object->addProperty("state", [this] { return stateName(); });
    m_object->addProperty("updates", [this] { return updateList(); });
    m_object->addProperty("progress", [this] { return int32_t(m_updates ? m_updates->progress() : -1); });
    m_object->addProperty("error", [this] { return m_updates ? m_updates->error() : std::string(); });
    m_object->addMethod("check", [this] {
        if (!m_updates || m_updates->isBusy())
            return;
        m_backgroundCheck = false;
        m_updates->check(true);
    });
    m_object->addMethod("download", [this] {
        if (m_updates)
            m_updates->download(true);
    });
    m_object->addMethod("cancel", [this] {
        if (m_updates)
            m_updates->cancelDownload();
    });
    m_object->addMethod("restartAndUpdate", "", "", [this](dbus::MethodCall &call) { restartAndUpdate(call); });
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

void SoftwareUpdate::start()
{
    m_updates = std::make_unique<Updates>(m_system);
    m_subscriptions.push_back(m_updates->changed.connect([this] { updatesChanged(); }));
    m_subscriptions.push_back(m_updates->checkFinished.connect([this](bool success) { checkFinished(success); }));

    m_resultCall = m_updates->readOfflineResult([this](const std::optional<Updates::OfflineResult> &result) {
        if (!result)
            return;
        notifyResult(*result);
        // Read once: the next session does not report it again.
        m_updates->clearOfflineResult();
    });

    m_updates->load();
    m_timer.start(kFirstCheckDelay);
}

void SoftwareUpdate::updatesChanged()
{
    const Updates::State state = m_updates->state();
    if (!m_object)
        return;
    if (state != m_announcedState) {
        m_announcedState = state;
        m_object->emitPropertiesChanged({"state", "updates", "progress", "error"});
    } else {
        m_object->emitPropertiesChanged({"progress"});
    }
}

void SoftwareUpdate::checkFinished(bool success)
{
    const bool background = std::exchange(m_backgroundCheck, false);
    if (!success)
        return;
    m_settings.setValue(kLastCheckKey, int64_t(std::time(nullptr)));
    m_settings.sync();
    const Updates::State state = m_updates->state();
    if (background && (state == Updates::State::Available || state == Updates::State::Ready))
        notifyUpdates();
}

void SoftwareUpdate::notifyUpdates()
{
    const uint64_t count = m_updates->packages().size();
    if (m_updates->state() == Updates::State::Ready) {
        m_updatesNotification.setSummary(tr("Updates Ready to Install"));
        m_updatesNotification.setBody(trn("Restart to install {n} update.", "Restart to install {n} updates.", count));
    } else {
        m_updatesNotification.setSummary(tr("Software Updates Available"));
        m_updatesNotification.setBody(trn("{n} update is available for this computer.",
                                          "{n} updates are available for this computer.", count));
    }
    m_updatesNotification.show();
}

void SoftwareUpdate::notifyResult(const Updates::OfflineResult &result)
{
    if (result.success) {
        m_resultNotification.setSummary(tr("Updates Installed"));
        m_resultNotification.setBody(
                trn("{n} update was installed.", "{n} updates were installed.", result.packageIds.size()));
    } else {
        m_resultNotification.setSummary(tr("Updates Not Installed"));
        // TRANSLATORS: {reason} is the package manager's own message, in English.
        m_resultNotification.setBody(result.error.empty()
                                             ? std::string(tr("The updates could not be installed."))
                                             : format(tr("The updates could not be installed: {reason}"),
                                                      {{"reason", result.error}}));
    }
    m_resultNotification.show();
}

void SoftwareUpdate::restartAndUpdate(dbus::MethodCall &call)
{
    if (!m_updates || m_updates->state() != Updates::State::Ready) {
        call.replyError("org.cutefish.Services.SoftwareUpdate.Error.NotReady", "No downloaded updates to install");
        return;
    }
    m_triggerCall = m_updates->trigger([this, call](bool success, const std::string &error) mutable {
        if (!success) {
            CF_WARNING(services::log()) << "Cannot set up the offline update:" << error;
            call.replyError("org.cutefish.Services.SoftwareUpdate.Error.Failed", error);
            return;
        }
        // The session quits the applications before it restarts, and stays
        // if one of them keeps it; the update then waits for the next boot.
        dbus::Proxy sessionManager(m_session, "org.cutefish.Session", "/Session", "org.cutefish.Session");
        sessionManager.send("reboot");
        call.reply();
    });
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
        return "up-to-date";
    case Updates::State::Available:
        return "available";
    case Updates::State::Downloading:
        return "downloading";
    case Updates::State::Ready:
        return "ready";
    case Updates::State::Error:
        return "error";
    }
    return "unknown";
}
