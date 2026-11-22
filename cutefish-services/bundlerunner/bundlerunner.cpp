// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "bundlerunner.h"

#include <filesystem>

#include "base/application_bundle.h"
#include "base/application_registry.h"
#include "base/event_loop.h"
#include "bundlerunner/installdialog.h"
#include "common/runtime_client.h"
#include "log.h"

using namespace cutefish::base;

static constexpr const char *kPath = "/org/cutefish/Services/BundleRunner";
static constexpr const char *kInterface = "org.cutefish.Services.BundleRunner";
static constexpr const char *kPackage = "cutefish-reef";

BundleRunner::BundleRunner(dbus::Connection *session)
    : m_session(session)
{
    // Scanned before a bundle is asked for.
    ApplicationRegistry::instance();
}

BundleRunner::~BundleRunner() = default;

bool BundleRunner::publish()
{
    if (!m_session)
        return false;
    m_object = std::make_unique<dbus::Object>(m_session, kPath, kInterface);
    m_object->addProperty("installable", [this] { return m_installable; });
    m_object->addMethod(
            "offer",
            [this](const std::string &path, const std::string &activationToken) {
                return offer(path, activationToken);
            },
            {{"path", "activationToken"}, {"offered"}});
    return m_object->publish();
}

bool BundleRunner::offer(const std::string &path, const std::string &activationToken)
{
    const std::string bundle = bundle::bundlePath(path);
    if (bundle.empty())
        return false;

    ApplicationRegistry &registry = ApplicationRegistry::instance();
    if (registry.isLoading()) {
        m_registryChanged = registry.changed.connect([this, bundle, activationToken] {
            if (ApplicationRegistry::instance().isLoading())
                return;
            // Not inside the announcement, which ends this subscription.
            EventLoop::current()->post([this, bundle, activationToken] {
                m_registryChanged.reset();
                offer(bundle, activationToken);
            });
        });
        return m_installable;
    }
    if (registry.runsBundles()) {
        open(bundle);
        return true;
    }
    if (!m_installable)
        return false;

    if (!m_dialog) {
        m_path = bundle;
        m_dialog = std::make_unique<InstallDialog>(kPackage, std::filesystem::path(bundle).filename().string());
        m_dialog->onInstalled = [this] { installed(); };
        m_dialog->onUnavailable = [this] { setUnavailable(); };
        m_dialog->onDone = [this] { dialogDone(); };
    }
    m_dialog->show(activationToken);
    return true;
}

// The runtime's registry may not know the new program yet.
void BundleRunner::open(const std::string &path)
{
    const auto entry = ApplicationRegistry::instance().readBundle(path);
    if (!entry) {
        CF_WARNING(services::log()) << "Cannot read the bundle" << path;
        return;
    }
    cutefish::core::runtime::launchCommand(entry->command(), entry->workingDirectory, entry->id);
}

void BundleRunner::installed()
{
    const std::string path = m_path;
    if (ApplicationRegistry::instance().runsBundles()) {
        open(path);
        return;
    }
    m_registryChanged = ApplicationRegistry::instance().changed.connect([this, path] {
        if (!ApplicationRegistry::instance().runsBundles())
            return;
        EventLoop::current()->post([this, path] {
            m_registryChanged.reset();
            open(path);
        });
    });
}

void BundleRunner::setUnavailable()
{
    m_installable = false;
    if (m_object)
        m_object->emitPropertiesChanged({"installable"});
}

void BundleRunner::dialogDone()
{
    // Called from the dialog's own handlers: it goes after them.
    std::shared_ptr<InstallDialog> gone(std::move(m_dialog));
    EventLoop::current()->post([gone] {});
}
