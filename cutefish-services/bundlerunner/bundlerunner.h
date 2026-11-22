// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <memory>
#include <string>

#include "base/dbus.h"
#include "base/signal.h"

class InstallDialog;

// org.cutefish.Services.BundleRunner: offers to install the program that runs
// application bundles, then opens the bundle.
class BundleRunner
{
public:
    explicit BundleRunner(cutefish::base::dbus::Connection *session);
    ~BundleRunner();

    BundleRunner(const BundleRunner &) = delete;
    BundleRunner &operator=(const BundleRunner &) = delete;

    bool publish();

    // False once the package sources turned out not to have it.
    bool isInstallable() const { return m_installable; }
    // False when the program cannot be had.
    bool offer(const std::string &path, const std::string &activationToken);

private:
    void open(const std::string &path);
    void installed();
    void setUnavailable();
    void dialogDone();

    cutefish::base::dbus::Connection *m_session;
    std::unique_ptr<cutefish::base::dbus::Object> m_object;
    std::unique_ptr<InstallDialog> m_dialog;
    std::string m_path;
    bool m_installable = true;
    cutefish::base::Subscription m_registryChanged;
};
