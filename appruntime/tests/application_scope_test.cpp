#include "applicationscope.h"

#include <gtest/gtest.h>

TEST(ApplicationScope, namesFollowTheDesktopEntrySpecification)
{
    EXPECT_EQ(ApplicationScope::unitName("com.obsproject.Studio", 4242),
              "app-cutefish-com.obsproject.Studio-4242.scope");
    // Dashes must not be mistaken for the launcher and random separators.
    EXPECT_EQ(ApplicationScope::unitName("cutefish-filemanager", 7),
              "app-cutefish-cutefish\\x2dfilemanager-7.scope");
    EXPECT_EQ(ApplicationScope::unitName(".hidden app", 1),
              "app-cutefish-\\x2ehidden\\x20app-1.scope");
    EXPECT_EQ(ApplicationScope::unitName("org.gnome.Evolution-alarm-notify", 2437),
              "app-cutefish-org.gnome.Evolution\\x2dalarm\\x2dnotify-2437.scope");
}

TEST(ApplicationScope, unitNamesMatchOnlyTheirOwnProcess)
{
    EXPECT_TRUE(ApplicationScope::namesProcess(ApplicationScope::unitName("com.obsproject.Studio", 123), 123));
    EXPECT_TRUE(ApplicationScope::namesProcess(ApplicationScope::unitName("cutefish-filemanager", 123), 123));
    EXPECT_FALSE(ApplicationScope::namesProcess(ApplicationScope::unitName("com.obsproject.Studio", 9123), 123));
    EXPECT_FALSE(ApplicationScope::namesProcess(ApplicationScope::unitName("com.obsproject.Studio", 1234), 123));
    EXPECT_FALSE(ApplicationScope::namesProcess(ApplicationScope::unitName("com.obsproject.Studio", 123), 23));
    EXPECT_FALSE(ApplicationScope::namesProcess("app-cutefish-com.obsproject.Studio-0123.scope", 123));
    EXPECT_FALSE(ApplicationScope::namesProcess("app-cutefish-com.obsproject.Studio-123.service", 123));
    // Only the runtime's scopes, which always carry an app id.
    EXPECT_FALSE(ApplicationScope::namesProcess("app-gnome-org.gnome.Terminal-123.scope", 123));
    EXPECT_FALSE(ApplicationScope::namesProcess("app-cutefish--123.scope", 123));
    EXPECT_FALSE(ApplicationScope::namesProcess("app-cutefish-123.scope", 123));
    // A dash the escaping would have removed is a different unit.
    EXPECT_FALSE(ApplicationScope::namesProcess("app-cutefish-com.obsproject.Studio-9-123.scope", 123));
}
