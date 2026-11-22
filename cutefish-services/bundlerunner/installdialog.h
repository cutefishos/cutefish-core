// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "base/signal.h"
#include "cutefish/packagekit/installer.h"

namespace cutefish::ui {
class DialogWindow;
}

// Offers to install the package a bundle needs before it can open, and shows
// the installation.
class InstallDialog
{
public:
    InstallDialog(std::string package, std::string fileName);
    ~InstallDialog();

    InstallDialog(const InstallDialog &) = delete;
    InstallDialog &operator=(const InstallDialog &) = delete;

    void show(const std::string &activationToken);

    // The package is installed; the dialog closes.
    std::function<void()> onInstalled;
    // No package source of this system has the package.
    std::function<void()> onUnavailable;
    // Closed, whichever way.
    std::function<void()> onDone;

private:
    class Content;
    // What the content shows: the question, the installation, or why it
    // did not happen.
    enum class Page {
        Ask,
        Progress,
        Unavailable,
        Failed,
    };

    void retranslate();
    void update();
    void dismiss();

    std::string m_package;
    std::string m_fileName;
    std::unique_ptr<cutefish::ui::DialogWindow> m_dialog;
    Content *m_content = nullptr;
    Page m_page = Page::Ask;
    cutefish::packagekit::Installer m_installer;
    cutefish::base::Subscription m_changed;
    cutefish::base::Subscription m_languageChanged;
};
