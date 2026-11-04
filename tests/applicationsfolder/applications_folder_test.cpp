// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "applicationsfolderservice.h"

#include "base/event_loop.h"
#include "base/file.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace file = cutefish::base::file;

class ApplicationsFolderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_root.isValid());
        m_folder = m_root.path() + "/Applications";
        fs::create_directory(m_folder);
        const struct group *own = getgrgid(getgid());
        ASSERT_TRUE(own);
        m_service = std::make_unique<ApplicationsFolderService>(
                ApplicationsFolderService::Paths {m_folder, uint32_t(getuid()), own->gr_name});
    }

    static mode_t modeOf(const std::string &path)
    {
        struct stat st;
        return ::lstat(path.c_str(), &st) == 0 ? st.st_mode & 07777 : 0;
    }

    // An application copied into a staging folder.
    std::string stage(const std::string &name, const std::string &version)
    {
        std::string error;
        const std::string staging = m_service->prepare(getuid(), &error);
        EXPECT_FALSE(staging.empty()) << error;
        EXPECT_EQ(modeOf(staging), mode_t(02775));
        fs::create_directories(staging + '/' + name + "/Contents/MacOS");
        EXPECT_TRUE(file::writeAtomically(staging + '/' + name + "/Contents/Info.plist", version));
        EXPECT_TRUE(file::writeAtomically(staging + '/' + name + "/Contents/MacOS/Program", "program"));
        ::chmod((staging + '/' + name + "/Contents/MacOS/Program").c_str(), 0700);
        ::chmod((staging + '/' + name + "/Contents/Info.plist").c_str(), 0400);
        fs::create_symlink("Contents/MacOS/Program", staging + '/' + name + "/Link");
        return staging;
    }

    cutefish::base::EventLoop m_loop;
    file::TemporaryDirectory m_root {fs::temp_directory_path().string(), "cutefish-test-"};
    std::string m_folder;
    std::unique_ptr<ApplicationsFolderService> m_service;
};

TEST_F(ApplicationsFolderTest, PublishesAStagedApplicationForTheGroup)
{
    const std::string staging = stage("Editor.app", "1");
    std::string error;
    EXPECT_EQ(m_service->publish(getuid(), staging, "Editor.app", false, &error), m_folder + "/Editor.app") << error;
    EXPECT_FALSE(fs::exists(staging));
    EXPECT_EQ(modeOf(m_folder + "/Editor.app"), mode_t(02775));
    EXPECT_EQ(modeOf(m_folder + "/Editor.app/Contents/MacOS/Program"), mode_t(0775));
    EXPECT_EQ(modeOf(m_folder + "/Editor.app/Contents/Info.plist"), mode_t(0664));
    EXPECT_TRUE(fs::is_symlink(m_folder + "/Editor.app/Link"));
    EXPECT_EQ(std::vector<fs::path>(fs::directory_iterator(m_folder), fs::directory_iterator()).size(), 1u);
}

TEST_F(ApplicationsFolderTest, ReplacesOnlyWhenAsked)
{
    std::string error;
    ASSERT_FALSE(m_service->publish(getuid(), stage("Editor.app", "1"), "Editor.app", false, &error).empty());
    ASSERT_TRUE(file::writeAtomically(m_folder + "/Editor.app/Contents/Old", "old"));

    EXPECT_TRUE(m_service->publish(getuid(), stage("Editor.app", "2"), "Editor.app", false, &error).empty());
    EXPECT_EQ(file::read(m_folder + "/Editor.app/Contents/Info.plist"), "1");

    EXPECT_FALSE(m_service->publish(getuid(), stage("Editor.app", "2"), "Editor.app", true, &error).empty());
    EXPECT_EQ(file::read(m_folder + "/Editor.app/Contents/Info.plist"), "2");
    EXPECT_FALSE(fs::exists(m_folder + "/Editor.app/Contents/Old"));
    EXPECT_EQ(std::vector<fs::path>(fs::directory_iterator(m_folder), fs::directory_iterator()).size(), 1u);
}

TEST_F(ApplicationsFolderTest, ReleasesAnApplicationToTheUser)
{
    std::string error;
    ASSERT_FALSE(m_service->publish(getuid(), stage("Editor.app", "1"), "Editor.app", false, &error).empty());

    const std::string staging = m_service->release(getuid(), "Editor.app", &error);
    ASSERT_FALSE(staging.empty()) << error;
    EXPECT_FALSE(fs::exists(m_folder + "/Editor.app"));
    EXPECT_EQ(file::read(staging + "/Editor.app/Contents/Info.plist"), "1");
    struct stat st;
    ASSERT_EQ(::lstat((staging + "/Editor.app/Contents/Info.plist").c_str(), &st), 0);
    EXPECT_EQ(st.st_uid, getuid());
    EXPECT_EQ(st.st_gid, getgid());
    EXPECT_TRUE(m_service->discard(getuid(), staging, &error)) << error;
    EXPECT_FALSE(fs::exists(staging));
}

TEST_F(ApplicationsFolderTest, RefusesWhatIsNotStagedByTheCaller)
{
    std::string error;
    const std::string staging = stage("Editor.app", "1");
    EXPECT_TRUE(m_service->publish(getuid() + 1, staging, "Editor.app", false, &error).empty());
    EXPECT_TRUE(m_service->publish(getuid(), staging, "../Editor.app", false, &error).empty());
    EXPECT_TRUE(m_service->publish(getuid(), m_root.path(), "Editor.app", false, &error).empty());
    EXPECT_TRUE(m_service->release(getuid(), fs::path(staging).filename().string(), &error).empty());
    EXPECT_FALSE(m_service->discard(getuid(), m_folder, &error));
    EXPECT_TRUE(fs::exists(staging + "/Editor.app"));
}
