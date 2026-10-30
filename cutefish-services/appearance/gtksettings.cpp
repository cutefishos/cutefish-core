#include "gtksettings.h"

#include <filesystem>

#include "base/file.h"
#include "base/paths.h"
#include "base/settings.h"
#include "base/strings.h"
#include "base/url.h"
#include "common/gsettings_keyfile.h"
#include "log.h"

using namespace cutefish::base;
using namespace cutefish::base::strings;
namespace gsettings = cutefish::core::gsettings;
namespace fs = std::filesystem;

static std::string gtk3SettingsIniPath()
{
    return paths::configHome() + "/gtk-3.0/settings.ini";
}

void writeGtkSettings(const GtkSettings &gtk)
{
    const std::string fontName = gtk.fontFamily + ' ' + number(gtk.fontPointSize, 'g', -1);
    // No Cutefish GTK theme ships; Adwaita's dark variant is what gtk-theme can switch to.
    const std::string gtkTheme = gtk.darkMode ? "Adwaita-dark" : "Adwaita";
    const bool subpixel = gtk.fontAntialias && gtk.fontRgba != "none";

    // Only GTK clients on Xwayland still read this, for every key the compositor's XSETTINGS
    // leaves out. On Wayland the keyfile below overrides every key the schema covers.
    {
        Settings settings(gtk3SettingsIniPath());
        for (const std::string &key : settings.childKeys())
            settings.remove(key);
        for (const std::string &group : settings.childGroups())
            settings.remove(group);
        settings.setValue("Settings/gtk-font-name", fontName);
        settings.setValue("Settings/gtk-xft-antialias", gtk.fontAntialias ? 1 : 0);
        settings.setValue("Settings/gtk-xft-hinting", gtk.fontHintStyle != "hintnone" ? 1 : 0);
        settings.setValue("Settings/gtk-xft-hintstyle", gtk.fontHintStyle);
        settings.setValue("Settings/gtk-xft-rgba", subpixel ? gtk.fontRgba : std::string("none"));
        settings.setValue("Settings/gtk-application-prefer-dark-theme", gtk.darkMode);
        settings.setValue("Settings/gtk-icon-theme-name", gtk.iconTheme);
        settings.setValue("Settings/gtk-cursor-theme-name", gtk.cursorTheme);
        settings.setValue("Settings/gtk-cursor-theme-size", gtk.cursorSize);
        settings.setValue("Settings/gtk-enable-animations", true);
        settings.setValue("Settings/gtk-theme-name", gtkTheme);
        settings.setValue("Settings/gtk-decoration-layout", "close,minimize,maximize:");
        settings.sync();
    }

    // GTK on Wayland ignores settings.ini for keys the schema covers and picks up keyfile
    // changes through GLib's file monitor.
    gsettings::writeGroup("org/gnome/desktop/interface", {
            {"font-name", fontName},
            {"font-antialiasing",
             std::string(!gtk.fontAntialias ? "none" : subpixel ? "rgba" : "grayscale")},
            // The schema's nicks are fontconfig's hintstyle constants without "hint".
            {"font-hinting", gtk.fontHintStyle.size() > 4 ? gtk.fontHintStyle.substr(4) : std::string()},
            {"font-rgba-order", subpixel ? gtk.fontRgba : std::string("rgb")},
            // "automatic" makes GTK 4 ignore the three keys above.
            {"font-rendering", std::string("manual")},
            {"gtk-theme", gtkTheme},
            {"color-scheme", std::string(gtk.darkMode ? "prefer-dark" : "default")},
            {"icon-theme", gtk.iconTheme},
            {"cursor-theme", gtk.cursorTheme},
            {"cursor-size", gtk.cursorSize},
            {"enable-animations", false},
    });
    gsettings::writeGroup("org/gnome/desktop/wm/preferences", {
            {"button-layout", std::string("close,minimize,maximize:")},
    });
}

static void ensureGtkCssImport(const std::string &gtkDir)
{
    const std::string importLine =
            "@import url(\"" + url::fromLocalFile(CUTEFISH_GTK_CSS_DIR "/" + gtkDir + ".css") + "\");";

    std::string path = paths::configHome() + '/' + gtkDir + "/gtk.css";
    // Write through a symlinked gtk.css (dotfile managers) instead of replacing the link.
    std::error_code error;
    if (fs::is_symlink(path, error) && fs::exists(path, error)) {
        const fs::path target = fs::canonical(path, error);
        if (!error)
            path = target.string();
    }

    std::vector<std::string> lines;
    if (const auto contents = file::read(path))
        lines = split(*contents, '\n');
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();

    // GTK drops an @import that follows any rule, so ours has to lead the file. Rewriting an
    // unchanged file would make every running GTK client reload its styles.
    if (!lines.empty() && lines.front() == importLine)
        return;
    std::erase(lines, importLine);
    lines.insert(lines.begin(), importLine);

    if (!file::writeAtomically(path, join(lines, "\n") + "\n"))
        CF_WARNING(services::log()) << "Cannot write" << path;
}

void ensureGtkCssImports()
{
    ensureGtkCssImport("gtk-3.0");
    ensureGtkCssImport("gtk-4.0");
}
