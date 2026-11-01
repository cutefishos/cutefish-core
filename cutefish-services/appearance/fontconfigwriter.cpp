#include "fontconfigwriter.h"

#include <fontconfig/fontconfig.h>
#include <set>
#include <unicode/uloc.h>

#include "base/file.h"
#include "base/paths.h"
#include "base/strings.h"

#include "fontconfig_template.h"

using namespace cutefish::base;
using namespace cutefish::base::strings;

// Every family name fontconfig knows, localized names included.
static std::set<std::string> fontFamilies()
{
    std::set<std::string> families;
    FcPattern *pattern = FcPatternCreate();
    FcObjectSet *objects = FcObjectSetBuild(FC_FAMILY, nullptr);
    FcFontSet *fonts = FcFontList(nullptr, pattern, objects);
    if (fonts) {
        for (int i = 0; i < fonts->nfont; ++i) {
            FcChar8 *family = nullptr;
            for (int n = 0; FcPatternGetString(fonts->fonts[i], FC_FAMILY, n, &family) == FcResultMatch; ++n)
                families.insert(reinterpret_cast<const char *>(family));
        }
        FcFontSetDestroy(fonts);
    }
    FcObjectSetDestroy(objects);
    FcPatternDestroy(pattern);
    return families;
}

static std::string htmlEscaped(const std::string &text)
{
    std::string result;
    for (char c : text) {
        switch (c) {
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '&': result += "&amp;"; break;
        case '"': result += "&quot;"; break;
        default: result += c; break;
        }
    }
    return result;
}

// Puts the arguments in place of %1 to %9, in one pass.
static std::string arguments(std::string_view text, const std::vector<std::string> &values)
{
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] >= '1'
            && size_t(text[i + 1] - '1') < values.size()) {
            result += values[size_t(text[i + 1] - '1')];
            ++i;
            continue;
        }
        result += text[i];
    }
    return result;
}

void writeFontConfig(const FontConfig &config)
{
    const std::set<std::string> families = fontFamilies();
    const std::string familyFont = families.contains(config.family) ? config.family
            : families.contains("Inter")                           ? std::string("Inter")
            : families.contains("Noto Sans")                       ? std::string("Noto Sans")
                                                                   : std::string("DejaVu Sans");
    const std::string fixedFont = families.contains(config.fixedFamily) ? config.fixedFamily
            : families.contains("Noto Sans Mono")                       ? std::string("Noto Sans Mono")
                                                                        : std::string("DejaVu Sans Mono");

    const std::string content = arguments(kFontConfigTemplate,
                                          {htmlEscaped(familyFont), htmlEscaped(fixedFont),
                                           config.antialias ? "true" : "false",
                                           htmlEscaped(config.hintStyle), htmlEscaped(config.rgba),
                                           htmlEscaped(config.cjkFamily)});

    // Atomic replace: a fontconfig reader starting meanwhile must never parse a partial file.
    file::writeAtomically(paths::configHome() + "/fontconfig/conf.d/99-cutefish.conf", content);
}

std::string cjkFontForLanguages(const std::vector<std::string> &languages)
{
    for (const std::string &code : languages) {
        char likely[ULOC_FULLNAME_CAPACITY] = {};
        UErrorCode status = U_ZERO_ERROR;
        uloc_addLikelySubtags(code.c_str(), likely, sizeof(likely), &status);
        char language[ULOC_LANG_CAPACITY] = {};
        char script[ULOC_SCRIPT_CAPACITY] = {};
        char country[ULOC_COUNTRY_CAPACITY] = {};
        uloc_getLanguage(likely, language, sizeof(language), &status);
        uloc_getScript(likely, script, sizeof(script), &status);
        uloc_getCountry(likely, country, sizeof(country), &status);
        if (U_FAILURE(status))
            continue;
        const std::string_view name = language;
        if (name == "ja")
            return "Noto Sans CJK JP";
        if (name == "ko")
            return "Noto Sans CJK KR";
        if (name == "zh") {
            const std::string_view territory = country;
            if (territory == "HK" || territory == "MO")
                return "Noto Sans CJK HK";
            if (std::string_view(script) == "Hant")
                return "Noto Sans CJK TC";
            return "Noto Sans CJK SC";
        }
    }

    return "Noto Sans CJK SC";
}
