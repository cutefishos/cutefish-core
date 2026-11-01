// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "crashmanager.h"
#include "coredumpjournal.h"
#include "crashdialog.h"

#include <filesystem>

#include "base/application_bundle.h"
#include "base/application_registry.h"
#include "base/desktop_entry.h"
#include "base/event_loop.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/strings.h"

using namespace cutefish::base;
using namespace cutefish::base::strings;
using cutefish::i18n::tr;

// What cutefish-session starts and restarts; cutefish-services cannot report itself.
static const char *const s_sessionPrograms[] = {
        "cutefish-appruntime",
        "cutefish-filemanager",
        "cutefish-polkit-agent",
};

static bool isSessionProgram(const std::string &program)
{
    for (const char *name : s_sessionPrograms) {
        if (program == name)
            return true;
    }
    return false;
}

static std::optional<DesktopEntry> lookupEntry(const std::string &id)
{
    // A bundle's id is its path.
    if (!bundle::bundlePath(id).empty())
        return bundle::read(id, {});

    for (const std::string &directory : ApplicationRegistry::desktopPaths()) {
        const std::string path = directory + '/' + id + ".desktop";
        if (const auto contents = file::read(path))
            return DesktopEntry::parse(id, path, *contents);
    }
    return std::nullopt;
}

static std::string nameOf(const std::string &id)
{
    if (const auto entry = lookupEntry(id))
        return entry->name.empty() ? id : entry->name;
    return id == "cutefish-appruntime" ? std::string(tr("Application Runtime")) : id;
}

// A helper process of the application may crash while its windows live on.
static bool applicationEnded(const Coredump &dump)
{
    if (dump.pid == dump.applicationPid())
        return true;
    const auto procs = file::read("/sys/fs/cgroup" + dump.cgroup + "/cgroup.procs");
    if (!procs)
        return true;
    return trimmed(*procs).empty();
}

CrashManager::CrashManager(std::function<std::string()> fixedFont)
    : m_fixedFont(std::move(fixedFont))
    , m_journal(std::make_unique<CoredumpJournal>([this](const Coredump &dump) {
        onCoredump(dump);
    }))
{
}

CrashManager::~CrashManager() = default;

void CrashManager::onCoredump(const Coredump &dump)
{
    const std::string appId = dump.applicationId();
    if (!appId.empty()) {
        if (!applicationEnded(dump))
            return;
        if (CrashDialog *dialog = dialogFor("app:" + appId, true, appId))
            dialog->setDump(dump);
        return;
    }

    // Outside an application scope, only what the session restarts gets a dialog.
    const std::string program = std::filesystem::path(dump.executable).filename().string();
    if (!isSessionProgram(program))
        return;
    if (CrashDialog *dialog = dialogFor("process:" + program, false, program))
        dialog->setDump(dump);
}

CrashDialog *CrashManager::dialogFor(const std::string &key, bool application, const std::string &id)
{
    if (const auto it = m_dialogs.find(key); it != m_dialogs.end())
        return it->second.get();

    std::string icon = "application-x-executable";
    if (const auto entry = lookupEntry(id)) {
        if (!entry->icon.empty())
            icon = entry->icon;
    }

    auto dialog = std::make_unique<CrashDialog>(
            application ? CrashDialog::Kind::Application : CrashDialog::Kind::SessionProcess, id,
            [id] { return nameOf(id); }, icon, m_fixedFont());
    CrashDialog *shown = dialog.get();
    dialog->onFinished = [this, key] {
        const auto it = m_dialogs.find(key);
        if (it == m_dialogs.end())
            return;
        // A crash from now on gets a new dialog; this one goes once the event
        // that finished it is over.
        std::shared_ptr<CrashDialog> finished(std::move(it->second));
        m_dialogs.erase(it);
        EventLoop::current()->post([finished] {});
    };
    m_dialogs.emplace(key, std::move(dialog));
    shown->show();
    return shown;
}
