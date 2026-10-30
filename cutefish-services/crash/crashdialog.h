// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "coredump.h"

#include <functional>
#include <memory>
#include <string>

#include "base/dbus.h"
#include "base/signal.h"

namespace cutefish::ui {
class Label;
class TextArea;
class View;
class Window;
}

// One open crash window. An application dialog offers to reopen it; the
// session restarts its own processes by itself. `name` is asked again when
// the session language changes.
class CrashDialog
{
public:
    enum class Kind { Application, SessionProcess };

    CrashDialog(Kind kind, const std::string &id, std::function<std::string()> name, const std::string &icon,
                const std::string &fixedFont);
    ~CrashDialog();

    CrashDialog(const CrashDialog &) = delete;
    CrashDialog &operator=(const CrashDialog &) = delete;

    bool sessionProcess() const { return m_kind == Kind::SessionProcess; }
    const std::string &name() const { return m_name; }
    std::string reason() const;
    std::string report() const;

    void setDump(const Coredump &dump);
    void show();

    void reopen();
    void close();

    // Reopen, Close, OK, Escape or the compositor closed it.
    std::function<void()> onFinished;

    cutefish::ui::Window *window() const { return m_window.get(); }

private:
    class Content;

    void buildContent();
    void retranslate();
    void update();
    void accept();

    Kind m_kind;
    std::string m_id;
    std::function<std::string()> m_nameLookup;
    std::string m_name;
    std::string m_icon;
    std::string m_fixedFont;
    Coredump m_dump;
    bool m_hasDump = false;
    bool m_finished = false;
    bool m_reopening = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::unique_ptr<cutefish::ui::Window> m_window;
    cutefish::ui::Label *m_text = nullptr;
    cutefish::ui::View *m_reportFrame = nullptr;
    cutefish::ui::TextArea *m_report = nullptr;
    cutefish::base::Subscription m_languageChanged;
};
