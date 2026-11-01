// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "coredump.h"

#include <gtest/gtest.h>

static const char s_message[] = R"(Process 4242 (Writer) of user 1000 dumped core.

Stack trace of thread 4242:
#0  0x00000001001f9014 n/a (n/a + 0x0)
#1  0x0000000112b7e904 n/a (n/a + 0x0)
#2  0x00005650dda92691 _ZN4host3runEv (/usr/bin/host + 0x3b691)
#3  0x0000000200000010 n/a (n/a + 0x0)

Stack trace of thread 4243:
#0  0x00007f6eaa69de98 pthread_cond_wait (libc.so.6 + 0x91e98)
#1  0x0000000112b7e4cd n/a (n/a + 0x0)
)";

TEST(CoredumpTest, ParsesAddressesAndOffsets)
{
    const std::vector<CoredumpThread> threads = Coredump::parseStackTrace(s_message);
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
    dump.threads = Coredump::parseStackTrace(s_message);
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
    dump.threads = Coredump::parseStackTrace(s_message);
    dump.addProgramSymbols("112b7e904 __CFRunLoopDoTimer\n",
                           "112b00000 112c00000 /System/CoreFoundation.framework/Versions/A/CoreFoundation\n");
    const std::string report = dump.report("Writer");
    EXPECT_NE(report.find("#1 0x0000000112b7e904 __CFRunLoopDoTimer (CoreFoundation + 0x7e904)\n"), std::string::npos);
    EXPECT_NE(report.find("#3 0x0000000200000010 ??\n"), std::string::npos);
}
