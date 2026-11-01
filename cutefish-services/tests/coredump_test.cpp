// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "coredump.h"

#include <gtest/gtest.h>

#include <csignal>

// As systemd-coredump 257 writes MESSAGE.
static const char s_libraryMessage[] =
        "Process 2112053 (svgrender) of user 1000 dumped core.\n"
        "\n"
        "Module libstdc++.so.6 from deb gcc-14-14.2.0-19.amd64\n"
        "Stack trace of thread 2112053:\n"
        "#0  0x00007fe7192a069c n/a (libc.so.6 + 0x9469c)\n"
        "#1  0x00007fe71924bc42 raise (libc.so.6 + 0x3fc42)\n"
        "#4  0x00007fe7198dcd32 _ZNK14QMessageLogger5fatalEPKcz (libQt6Core.so.6 + 0xdcd32)\n"
        "#10 0x00005650c8d33206 main (/usr/local/bin/svgrender + 0x1206)\n"
        "#11 0x0000000000000000 n/a (n/a + 0x0)\n"
        "\n"
        "Stack trace of thread 2112056:\n"
        "#0  0x00007fe7192a672e n/a (libc.so.6 + 0x9a72e)\n"
        "#1  0x00007fe71931c538 n/a (libc.so.6 + 0x110538)\n"
        "ELF object binary architecture: AMD x86-64";

static const char s_programMessage[] = R"(Process 4242 (Writer) of user 1000 dumped core.

Stack trace of thread 4242:
#0  0x00000001001f9014 n/a (n/a + 0x0)
#1  0x0000000112b7e904 n/a (n/a + 0x0)
#2  0x00005650dda92691 _ZN4host3runEv (/usr/bin/host + 0x3b691)
#3  0x0000000200000010 n/a (n/a + 0x0)

Stack trace of thread 4243:
#0  0x00007f6eaa69de98 pthread_cond_wait (libc.so.6 + 0x91e98)
#1  0x0000000112b7e4cd n/a (n/a + 0x0)
)";

TEST(CoredumpTest, ParsesThreadsAndFrames)
{
    const auto threads = Coredump::parseStackTrace(s_libraryMessage);
    ASSERT_EQ(threads.size(), 2u);
    EXPECT_EQ(threads[0].id, "2112053");
    ASSERT_EQ(threads[0].frames.size(), 5u);
    EXPECT_EQ(threads[0].frames[0].function, "");
    EXPECT_EQ(threads[0].frames[0].module, "libc.so.6");
    EXPECT_EQ(threads[0].frames[1].function, "raise");
    EXPECT_EQ(threads[0].frames[2].function, "QMessageLogger::fatal(char const*, ...) const");
    EXPECT_EQ(threads[0].frames[3].module, "/usr/local/bin/svgrender");
    EXPECT_EQ(threads[0].frames[4].module, "");
    EXPECT_EQ(threads[1].frames.size(), 2u);
}

TEST(CoredumpTest, IgnoresAMessageWithoutStack)
{
    EXPECT_TRUE(Coredump::parseStackTrace("Process 1 (x) of user 1000 dumped core.").empty());
}

TEST(CoredumpTest, KeepsWhatItCannotDemangle)
{
    EXPECT_EQ(Coredump::demangle("_Zbogus"), "_Zbogus");
    EXPECT_EQ(Coredump::demangle("main"), "main");
}

TEST(CoredumpTest, ReadsTheApplicationFromItsScope)
{
    Coredump dump;
    dump.userUnit = "app-cutefish-cutefish\\x2dsystem\\x2dmonitor-2127671.scope";
    EXPECT_EQ(dump.applicationId(), "cutefish-system-monitor");
    EXPECT_EQ(dump.applicationPid(), 2127671u);

    dump.userUnit = "app-cutefish-org.gnome.\\xe6\\x96\\x87-12.scope";
    EXPECT_EQ(dump.applicationId(), "org.gnome.\xe6\x96\x87");

    dump.userUnit = "session-662.scope";
    EXPECT_EQ(dump.applicationId(), "");
    EXPECT_EQ(dump.applicationPid(), 0u);
}

TEST(CoredumpTest, WritesAReport)
{
    Coredump dump;
    dump.signal = SIGSEGV;
    dump.signalName = "SIGSEGV";
    dump.executable = "/usr/bin/x";
    dump.threads = Coredump::parseStackTrace(s_libraryMessage);
    const std::string report = dump.report("X");
    EXPECT_EQ(report.rfind("X quit unexpectedly\nExecutable: /usr/bin/x\n", 0), 0u);
    EXPECT_NE(report.find("(SIGSEGV)"), std::string::npos);
    EXPECT_NE(report.find("\nThread 2112053:\n#0 0x00007fe7192a069c ?? (libc.so.6 + 0x9469c)\n"
                          "#1 0x00007fe71924bc42 raise (libc.so.6 + 0x3fc42)\n"), std::string::npos);
}

TEST(CoredumpTest, WritesTheLocalTime)
{
    Coredump dump;
    dump.time = 1760000000000;
    const std::string report = dump.report("X");
    const size_t time = report.find("Time: ");
    ASSERT_NE(time, std::string::npos);
    // 2025-10-09T08:53:20 in UTC; the zone is the machine's, and not written.
    const std::string value = report.substr(time + 6, report.find('\n', time) - time - 6);
    EXPECT_EQ(value.size(), 19u) << value;
    EXPECT_EQ(value[10], 'T');
}

TEST(CoredumpTest, ParsesAddressesAndOffsets)
{
    const std::vector<CoredumpThread> threads = Coredump::parseStackTrace(s_programMessage);
    ASSERT_EQ(threads.size(), 2u);
    ASSERT_EQ(threads[0].frames.size(), 4u);

    const CoredumpFrame &unknown = threads[0].frames[0];
    EXPECT_EQ(unknown.address, 0x1001f9014u);
    EXPECT_TRUE(unknown.function.empty());
    EXPECT_TRUE(unknown.module.empty());

    const CoredumpFrame &known = threads[0].frames[2];
    EXPECT_EQ(known.address, 0x5650dda92691u);
    EXPECT_EQ(known.function, "host::run()");
    EXPECT_EQ(known.module, "/usr/bin/host");
    EXPECT_EQ(known.offset, 0x3b691u);
}

TEST(CoredumpTest, ProgramSymbolsNameFramesSystemdCouldNot)
{
    Coredump dump;
    dump.threads = Coredump::parseStackTrace(s_programMessage);
    dump.addProgramSymbols("112b7e904 __CFRunLoopDoTimer\n"
                           "5650dda92691 shadowed\n"
                           "bad line\n",
                           "100000000 100400000 /Apps/Writer.app/Contents/MacOS/Writer\n"
                           "112b00000 112c00000 /System/CoreFoundation.framework/Versions/A/CoreFoundation\n"
                           "300000000 200000000 /backwards\n");

    const auto &crashed = dump.threads[0].frames;
    EXPECT_EQ(crashed[0].module, "Writer");
    EXPECT_EQ(crashed[0].offset, 0x1f9014u);
    EXPECT_TRUE(crashed[0].function.empty());
    EXPECT_EQ(crashed[1].module, "CoreFoundation");
    EXPECT_EQ(crashed[1].offset, 0x7e904u);
    EXPECT_EQ(crashed[1].function, "__CFRunLoopDoTimer");
    EXPECT_EQ(crashed[2].function, "host::run()");
    EXPECT_EQ(crashed[2].module, "/usr/bin/host");
    EXPECT_TRUE(crashed[3].module.empty());

    // Other threads get their modules, though only the crashed one's symbols.
    const auto &other = dump.threads[1].frames;
    EXPECT_EQ(other[0].module, "libc.so.6");
    EXPECT_EQ(other[1].module, "CoreFoundation");
    EXPECT_EQ(other[1].offset, 0x7e4cdu);
    EXPECT_TRUE(other[1].function.empty());
}

TEST(CoredumpTest, ReportShowsAddressesAndModuleOffsets)
{
    Coredump dump;
    dump.executable = "/usr/bin/host";
    dump.threads = Coredump::parseStackTrace(s_programMessage);
    dump.addProgramSymbols("112b7e904 __CFRunLoopDoTimer\n",
                           "112b00000 112c00000 /System/CoreFoundation.framework/Versions/A/CoreFoundation\n");
    const std::string report = dump.report("Writer");
    EXPECT_NE(report.find("#1 0x0000000112b7e904 __CFRunLoopDoTimer (CoreFoundation + 0x7e904)\n"), std::string::npos);
    EXPECT_NE(report.find("#3 0x0000000200000010 ??\n"), std::string::npos);
}
