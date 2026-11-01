#ifndef FONTCONFIGWRITER_H
#define FONTCONFIGWRITER_H

#include <string>
#include <vector>

struct FontConfig
{
    std::string family;
    std::string fixedFamily;
    bool antialias = true;
    std::string hintStyle = "hintfull";
    std::string rgba = "rgb";
    // Last resort for Han, kana and hangul the families above lack.
    std::string cjkFamily = "Noto Sans CJK SC";
};

// Writes ~/.config/fontconfig/conf.d/99-cutefish.conf, falling back to installed families.
void writeFontConfig(const FontConfig &config);

// The first Chinese, Japanese or Korean language decides the forms of Han text
// that carries no language of its own.
std::string cjkFontForLanguages(const std::vector<std::string> &languages);

#endif
