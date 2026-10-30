// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "coredump.h"
#include "crashdialog.h"

#include "ui/application.h"
#include "ui/text_area.h"
#include "ui/view.h"
#include "ui/window.h"

#include <gtest/gtest.h>

#include <csignal>

using namespace cutefish::ui;

template<typename T>
static T *findFirst(View *view)
{
    if (!view)
        return nullptr;
    if (auto *found = dynamic_cast<T *>(view))
        return found;
    for (const auto &child : view->children()) {
        if (T *found = findFirst<T>(child.get()))
            return found;
    }
    return nullptr;
}

static bool shown(const View *view)
{
    for (; view; view = view->parent()) {
        if (!view->isVisible())
            return false;
    }
    return true;
}

TEST(CrashDialogTest, LanguageChangeKeepsTheReportShown)
{
    int lookups = 0;
    CrashDialog dialog(CrashDialog::Kind::Application, "org.example.Editor", [&lookups] {
        ++lookups;
        return std::string("Editor");
    }, "", "monospace");
    bool finished = false;
    dialog.onFinished = [&finished] {
        finished = true;
    };
    Coredump dump;
    dump.signal = SIGSEGV;
    dump.signalName = "SIGSEGV";
    dump.executable = "/usr/bin/editor";
    dump.threads = {{"1", {{"main", "/usr/bin/editor"}}}};
    dialog.setDump(dump);
    dialog.show();
    dialog.window()->grab();

    View *before = dialog.window()->contentView();
    ASSERT_TRUE(shown(findFirst<TextArea>(before)));

    Application::instance()->languageChanged.emit();
    dialog.window()->grab();
    View *after = dialog.window()->contentView();
    EXPECT_NE(after, before);
    EXPECT_EQ(lookups, 2);
    TextArea *report = findFirst<TextArea>(after);
    ASSERT_NE(report, nullptr);
    EXPECT_TRUE(shown(report));
    EXPECT_EQ(report->text(), dump.report("Editor"));
    EXPECT_FALSE(dialog.window()->isHidden());
    EXPECT_FALSE(finished);
}

int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);
    Application application(argc, argv, Application::Platform::Headless);
    return RUN_ALL_TESTS();
}
