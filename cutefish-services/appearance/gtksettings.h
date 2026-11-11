#ifndef GTKSETTINGS_H
#define GTKSETTINGS_H

#include <string>

struct GtkSettings
{
    std::string fontFamily;
    double fontPointSize = 9;
    // fontconfig's values, as written by writeFontConfig().
    bool fontAntialias = true;
    std::string fontHintStyle = "hintfull";
    std::string fontRgba = "rgb";
    bool darkMode = false;
    std::string iconTheme;
    std::string cursorTheme;
    int cursorSize = 24;
    bool animations = true;
};

// Mirrors the system appearance into GTK's config: settings.ini for Xwayland clients and the
// GSettings keyfile for Wayland clients.
void writeGtkSettings(const GtkSettings &settings);

// Puts the Cutefish stylesheet import at the top of the user's gtk.css for GTK 3 and 4, keeping
// the rest of the file. It is re-added at every login, even after the user removed it.
void ensureGtkCssImports();

#endif
