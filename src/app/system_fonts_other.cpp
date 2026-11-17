// SPDX-License-Identifier: MIT

// The system's fonts on Linux and the other systems with fontconfig. fontconfig finds the files of the families of
// font_families.h, and Dear ImGui's loader for FreeType reads and draws them. system_fonts_windows.cpp draws the same
// families under Wine, with the same roles. The first family that the system has draws the text. The others fill
// in the characters that are missing from it, and one Japanese family comes last. The text has the Mac's type size, the
// division of Segoe UI's line and advances of whole pixels. FreeType fits each glyph to the pixels' rows with its light
// hinting, as GNOME's text does by default (fontconfig's hintslight), and draws it in grey levels. Every glyph starts
// on a pixel. Every named instance of a variable font is a separate face here. This loader and
// system_fonts_windows.cpp follow the rules of system_fonts_shared.h for the sizes, the figures, the contrast, the
// baseline and the roles.

#include "app/system_fonts.h"

#include <fontconfig/fontconfig.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/freetype/imgui_freetype.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "app/font_families.h"
#include "app/system_fonts_shared.h"

namespace hokuto::app
{
namespace
{

using system_fonts::BakedSource;
using system_fonts::kFigureSpace;
using system_fonts::kTypePerSize;

// The bit of ImFontConfig::FontLoaderFlags that marks the figures' source. In the figures' source, every digit has one
// width. The source draws a figure space of that width where the face has none, or one of another width. Dear ImGui's
// loader for FreeType reads the low bits as ImGuiFreeTypeLoaderFlags. None of those flags is as high as this bit.
constexpr unsigned int kFigures = 1u << 31;

// The loader flags of every source: FreeType's light hinting, the default of GNOME's text.
constexpr unsigned int kHinting = ImGuiFreeTypeLoaderFlags_LightHinting;

// The contrast that system_fonts_windows.cpp adds when DirectWrite reports a contrast of 1. It thickens the thin
// strokes that grayscale coverage gives light text on a dark background. Without it, FreeType's hinted text in the
// preview's control bar and mixer measured 4 to 23 % less coverage, and small signs such as ∞ came out faint.
constexpr float kContrast = 1.0f;

// The bytes of each font file that a source reads, by path. A map never moves the bytes of a file. The font atlas holds
// pointers to them.
std::map<std::string, std::vector<unsigned char>>& Files()
{
    static std::map<std::string, std::vector<unsigned char>> files;
    return files;
}

// The bytes of the file at `path`, read once. Returns nothing if the file can't be read.
const std::vector<unsigned char>* FileBytes(const std::string& path)
{
    auto& files = Files();
    if (const auto found = files.find(path); found != files.end())
    {
        return &found->second;
    }

    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error)
    {
        return nullptr;
    }

    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        return nullptr;
    }

    return &files.emplace(path, std::move(bytes)).first->second;
}

// The big-endian number of `size` bytes at `offset` in `bytes`, or nothing past their end.
std::optional<std::uint32_t> Number(std::span<const unsigned char> bytes, std::size_t offset, std::size_t size)
{
    if (offset > bytes.size() || bytes.size() - offset < size)
    {
        return std::nullopt;
    }

    std::uint32_t value = 0;
    for (std::size_t i = 0; i < size; i++)
    {
        value = (value << 8) | bytes[offset + i];
    }

    return value;
}

// Where the table `tag` of the face `number` of a font file starts, or nothing. A collection (TTC) has several faces.
std::optional<std::size_t> TableStart(std::span<const unsigned char> bytes, int number, std::string_view tag)
{
    std::size_t face = 0;
    if (Number(bytes, 0, 4) == 0x74746366) // 'ttcf'
    {
        const auto faces = Number(bytes, 8, 4);
        const auto start = Number(bytes, 12 + 4 * static_cast<std::size_t>(number), 4);
        if (!faces || !start || static_cast<std::uint32_t>(number) >= *faces)
        {
            return std::nullopt;
        }
        face = *start;
    }
    else if (number != 0)
    {
        return std::nullopt;
    }

    const std::uint32_t wanted = (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[0])) << 24) |
                                 (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[1])) << 16) |
                                 (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[2])) << 8) |
                                 static_cast<std::uint32_t>(static_cast<unsigned char>(tag[3]));
    const auto tables = Number(bytes, face + 4, 2);
    for (std::uint32_t i = 0; tables && i < *tables; i++)
    {
        const std::size_t record = face + 12 + 16 * static_cast<std::size_t>(i);
        if (Number(bytes, record, 4) == wanted)
        {
            const auto start = Number(bytes, record + 8, 4);
            return start ? std::optional<std::size_t>(*start) : std::nullopt;
        }
    }

    return std::nullopt;
}

// The signed 16-bit number at `offset` in the table `tag` of a face, or nothing.
std::optional<int> TableValue(std::span<const unsigned char> bytes, int number, std::string_view tag,
                              std::size_t offset)
{
    const auto table = TableStart(bytes, number, tag);
    const auto value = table ? Number(bytes, *table + offset, 2) : std::nullopt;
    if (!value)
    {
        return std::nullopt;
    }

    return static_cast<int>(static_cast<std::int16_t>(static_cast<std::uint16_t>(*value)));
}

// A face of a font file that draws a family: the file, the face's index, and its weight (OS/2 weight class). The index
// is fontconfig's FC_INDEX. Its low 16 bits hold the face's number in the file. A collection (TTC) has several faces.
// The bits above hold the number of a named instance of a variable font, counted from 1. There, 0 is the default
// instance. FreeType reads an index in the same way.
struct Face
{
    std::string path;
    int index = 0;
    int weight = 0;
};

// The number of a face in its file.
int NumberInFile(const Face& face)
{
    return face.index & 0xFFFF;
}

// The upright faces of normal width of the family `family` that FreeType draws, with `character`. fontconfig
// matches the name as DirectWrite does: exactly, without substitutes.
std::vector<Face> FacesOf(const std::string& family, FcChar32 character)
{
    std::vector<Face> faces;
    FcPattern* pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
    FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
    FcPatternAddInteger(pattern, FC_WIDTH, FC_WIDTH_NORMAL);
    FcObjectSet* objects =
        FcObjectSetBuild(FC_FILE, FC_INDEX, FC_WEIGHT, FC_VARIABLE, FC_FONTFORMAT, FC_CHARSET, nullptr);
    FcFontSet* set = FcFontList(nullptr, pattern, objects);
    FcObjectSetDestroy(objects);
    FcPatternDestroy(pattern);
    if (set == nullptr)
    {
        return faces;
    }

    for (int i = 0; i < set->nfont; i++)
    {
        const FcPattern* font = set->fonts[i];
        FcChar8* file = nullptr;
        FcChar8* format = nullptr;
        FcCharSet* characters = nullptr;
        int index = 0;
        if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch ||
            FcPatternGetInteger(font, FC_INDEX, 0, &index) != FcResultMatch ||
            FcPatternGetString(font, FC_FONTFORMAT, 0, &format) != FcResultMatch ||
            FcPatternGetCharSet(font, FC_CHARSET, 0, &characters) != FcResultMatch)
        {
            continue;
        }

        // fontconfig lists three kinds of pattern for a variable font. A named instance has the instance's number in
        // the index's high bits. The variable font itself has FC_VARIABLE and a range of weights. The default instance
        // has neither. Each named instance and the default instance is a face with one weight. The pattern with
        // FC_VARIABLE is left out. Variable Noto Sans has no static face of weight 600, and the default instance of
        // variable Noto Sans CJK is its thinnest.
        const std::string_view kind = reinterpret_cast<const char*>(format);
        FcBool variable = FcFalse;
        double weight = 0.0;
        if ((FcPatternGetBool(font, FC_VARIABLE, 0, &variable) == FcResultMatch && variable != FcFalse) ||
            FcPatternGetDouble(font, FC_WEIGHT, 0, &weight) != FcResultMatch || (kind != "TrueType" && kind != "CFF") ||
            !FcCharSetHasChar(characters, character))
        {
            continue;
        }

        faces.push_back(Face{reinterpret_cast<const char*>(file), index,
                             static_cast<int>(std::lround(FcWeightToOpenTypeDouble(weight)))});
    }
    FcFontSetDestroy(set);

    return faces;
}

// The face of the family `family` nearest `weight`, with `character`. Returns nothing if the system doesn't have the
// family, or if its nearest face is lighter than `lightest`. Of two faces at the same distance, the heavier one is
// chosen.
std::optional<Face> FindFace(const std::string& family, int weight, int lightest, FcChar32 character)
{
    const std::vector<Face> faces = FacesOf(family, character);
    const auto nearest =
        std::min_element(faces.begin(), faces.end(),
                         [&](const Face& a, const Face& b)
                         {
                             const int a_distance = std::abs(a.weight - weight);
                             const int b_distance = std::abs(b.weight - weight);
                             return a_distance < b_distance || (a_distance == b_distance && a.weight > b.weight);
                         });
    if (nearest == faces.end() || nearest->weight < lightest)
    {
        return std::nullopt;
    }

    return *nearest;
}

// The family that fontconfig matches to `request`, such as "sans-serif", or nothing. The system's configuration sets
// the match, from the families that the system has.
std::string MatchedFamily(const char* request)
{
    std::string family;
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(request));
    if (pattern == nullptr)
    {
        return family;
    }

    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    if (FcPattern* match = FcFontMatch(nullptr, pattern, &result))
    {
        FcChar8* name = nullptr;
        if (FcPatternGetString(match, FC_FAMILY, 0, &name) == FcResultMatch)
        {
            family = reinterpret_cast<const char*>(name);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pattern);

    return family;
}

// An interface family that the system has: its face for the text, and its face for the strong text.
struct Family
{
    Face regular;
    std::optional<Face> strong;
};

// The families of the list that the system has, or else the family that fontconfig matches to "sans-serif".
std::vector<Family> InterfaceFamilies()
{
    const auto family_named = [&](const std::string& name) -> std::optional<Family>
    {
        const auto regular = FindFace(name, 400, font_families::kLightestRegular, 'A');
        if (!regular)
        {
            return std::nullopt;
        }

        return Family{*regular, FindFace(name, 600, font_families::kLightestStrong, 'A')};
    };

    std::vector<Family> families;
    for (const std::string_view name : font_families::kInterface)
    {
        if (auto family = family_named(std::string(name)))
        {
            families.push_back(*family);
        }
    }
    if (families.empty())
    {
        if (auto family = family_named(MatchedFamily("sans-serif")))
        {
            families.push_back(*family);
        }
    }

    return families;
}

// The first Japanese family of the list that the system has, then the family that fontconfig matches to
// "sans-serif:lang=ja". Only a face with kana qualifies.
std::optional<Face> JapaneseFace()
{
    std::vector<std::string> names(font_families::kJapanese.begin(), font_families::kJapanese.end());
    names.push_back(MatchedFamily("sans-serif:lang=ja"));
    for (const std::string& name : names)
    {
        if (auto face = FindFace(name, 400, font_families::kLightestRegular, 0x306E)) // の
        {
            return face;
        }
    }

    return std::nullopt;
}

const ImFontLoader& FreeType()
{
    return *ImGuiFreeType::GetFontLoader();
}

// `size` rounded up to a multiple of `alignment`.
std::size_t RoundUp(std::size_t size, std::size_t alignment)
{
    return (size + alignment - 1) / alignment * alignment;
}

// What the loader keeps for each size of a source: FreeType's data, then a BakedSource.
std::size_t BakedSourceOffset()
{
    return RoundUp(FreeType().FontBakedSrcLoaderDataSize, alignof(BakedSource));
}

BakedSource& BakedSourceOf(void* loader_data)
{
    return *std::launder(reinterpret_cast<BakedSource*>(static_cast<char*>(loader_data) + BakedSourceOffset()));
}

float DensityOf(const ImFontConfig* src, const ImFontBaked* baked)
{
    return src->RasterizerDensity * baked->RasterizerDensity;
}

// The advance of `codepoint` in whole pixels, or nothing if the source has no glyph for it.
std::optional<float> AdvancePixels(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loader_data,
                                   ImWchar codepoint)
{
    float advance = 0.0f;
    if (!FreeType().FontBakedLoadGlyph(atlas, src, baked, loader_data, codepoint, nullptr, &advance))
    {
        return std::nullopt;
    }

    return std::round(advance * DensityOf(src, baked));
}

// The advance of the widest digit of a source, in pixels. Every digit of the figures has this advance.
float DigitPixels(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loader_data)
{
    const auto advance_of = [&](ImWchar digit)
    {
        return AdvancePixels(atlas, src, baked, loader_data, digit);
    };
    return system_fonts::WidestDigitPixels(advance_of);
}

// Applies the contrast of kContrast to the coverage of a glyph's bitmap in the atlas.
void AddContrast(ImFontAtlas* atlas, const ImFontGlyph& glyph)
{
    static const std::array<unsigned char, 256> kTable = system_fonts::ContrastTable(kContrast);

    if (!glyph.Visible || glyph.PackId == ImFontAtlasRectId_Invalid)
    {
        return;
    }

    // The bitmap is in the texture already. The texture is uploaded at the frame's end.
    const ImTextureRect* rect = ImFontAtlasPackGetRect(atlas, glyph.PackId);
    ImTextureData* texture = atlas->TexData;
    const int step = texture->Format == ImTextureFormat_RGBA32 ? 4 : 1;
    const int alpha = texture->Format == ImTextureFormat_RGBA32 ? 3 : 0;
    for (int y = 0; y < rect->h; y++)
    {
        auto* row = static_cast<unsigned char*>(texture->GetPixelsAt(rect->x, rect->y + y));
        for (int x = 0; x < rect->w; x++)
        {
            unsigned char& pixel = row[x * step + alpha];
            pixel = kTable[pixel];
        }
    }
}

bool BakedInit(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loader_data)
{
    if (!FreeType().FontBakedInit(atlas, src, baked, loader_data))
    {
        return false;
    }
    auto* baked_source = new (static_cast<char*>(loader_data) + BakedSourceOffset()) BakedSource;
    if ((src->FontLoaderFlags & kFigures) != 0)
    {
        baked_source->digit_pixels = DigitPixels(atlas, src, baked, loader_data);
    }

    // The baseline divides the line of each size as Segoe UI's does.
    system_fonts::PlaceBaseline(*src, *baked, font_families::kAscentPerLine);

    return true;
}

void BakedDestroy(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loader_data)
{
    BakedSourceOf(loader_data).~BakedSource();
    FreeType().FontBakedDestroy(atlas, src, baked, loader_data);
}

bool SourceContainsGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImWchar codepoint)
{
    return FreeType().FontSrcContainsGlyph(atlas, src, codepoint) ||
           ((src->FontLoaderFlags & kFigures) != 0 && codepoint == kFigureSpace);
}

bool LoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loader_data, ImWchar codepoint,
               ImFontGlyph* out_glyph, float* out_advance_x)
{
    const float density = DensityOf(src, baked);
    const std::optional<system_fonts::GlyphFit> fit =
        system_fonts::FitGlyph((src->FontLoaderFlags & kFigures) != 0, BakedSourceOf(loader_data).digit_pixels,
                               codepoint, AdvancePixels(atlas, src, baked, loader_data, codepoint));
    if (!fit)
    {
        return false;
    }
    if (system_fonts::LoadWithoutBitmap(*fit, codepoint, density, out_glyph, out_advance_x))
    {
        return true;
    }

    if (!FreeType().FontBakedLoadGlyph(atlas, src, baked, loader_data, codepoint, out_glyph, nullptr))
    {
        return false;
    }
    out_glyph->AdvanceX = fit->advance_pixels / density;
    out_glyph->X0 += fit->shift_pixels / density;
    out_glyph->X1 += fit->shift_pixels / density;
    AddContrast(atlas, *out_glyph);

    return true;
}

// Dear ImGui's loader for FreeType, with the sizes, the baseline, the advances and the figures above. The atlas's
// loader is FreeType's. Its LoaderInit and LoaderShutdown create and destroy FreeType's library. A source's loader has
// neither function.
const ImFontLoader& FontconfigLoader()
{
    static const ImFontLoader kLoader = []
    {
        ImFontLoader loader = FreeType();
        loader.Name = "FreeType through fontconfig";
        loader.LoaderInit = nullptr;
        loader.LoaderShutdown = nullptr;
        loader.FontSrcContainsGlyph = SourceContainsGlyph;
        loader.FontBakedInit = BakedInit;
        loader.FontBakedDestroy = BakedDestroy;
        loader.FontBakedLoadGlyph = LoadGlyph;
        // Dear ImGui puts the data of a font's sources one after another in one block. The size is a multiple of
        // alignof(std::max_align_t). The data of every source then starts at that alignment. FreeType's FT_Size
        // pointer at the data's start is aligned too.
        loader.FontBakedSrcLoaderDataSize =
            RoundUp(BakedSourceOffset() + sizeof(BakedSource), alignof(std::max_align_t));

        return loader;
    }();
    return kLoader;
}

// Adds `face` to `atlas`. With `merge`, the face fills in the glyphs that are missing from the font added before it.
// `spacing` is the space added after each glyph, in points. Returns nothing if the face's file can't be read.
ImFont* AddFace(ImFontAtlas& atlas, const Face& face, bool merge, bool figures, float spacing)
{
    const std::vector<unsigned char>* bytes = FileBytes(face.path);
    const int number = NumberInFile(face);
    const auto units_per_em = bytes ? TableValue(*bytes, number, "head", 18) : std::nullopt;
    const auto ascent = bytes ? TableValue(*bytes, number, "hhea", 4) : std::nullopt;
    const auto descent = bytes ? TableValue(*bytes, number, "hhea", 6) : std::nullopt;
    if (!units_per_em || !ascent || !descent || *units_per_em <= 0 || *ascent <= *descent)
    {
        return nullptr;
    }

    // FreeType scales a font so that the line from hhea's ascent to hhea's descent has the requested size.
    // ExtraSizeScale sets the type size of the Mac's interface font instead, as for every face.
    ImFontConfig config;
    config.FontData = const_cast<unsigned char*>(bytes->data());
    config.FontDataSize = static_cast<int>(bytes->size());
    config.FontDataOwnedByAtlas = false;
    config.FontNo = static_cast<ImU32>(face.index);
    config.FontLoader = &FontconfigLoader();
    config.FontLoaderFlags = kHinting | (figures ? kFigures : 0);
    config.MergeMode = merge;
    config.GlyphExtraAdvanceX = spacing;
    config.ExtraSizeScale = kTypePerSize * static_cast<float>(*ascent - *descent) / static_cast<float>(*units_per_em);

    return atlas.AddFont(&config);
}

} // namespace

theme::Fonts AddSystemFonts(ImFontAtlas& atlas)
{
    theme::Fonts fonts;
    // Every call of fontconfig passes no configuration. fontconfig then uses the current configuration.
    // FcDefaultSubstitute loads the current configuration in any case. Only fontconfig 2.17 and later can apply the
    // defaults to another configuration.
    if (FcInit() == FcFalse)
    {
        return fonts;
    }
    atlas.SetFontLoader(ImGuiFreeType::GetFontLoader());
    const std::vector<Family> families = InterfaceFamilies();
    const std::optional<Face> japanese = JapaneseFace();
    if (families.empty())
    {
        return fonts;
    }

    // A face whose file can't be read is left out.
    const auto add_face = [&](std::optional<std::size_t> family, bool strong, bool merge, bool figures, float spacing)
    {
        if (!family)
        {
            return AddFace(atlas, *japanese, merge, figures, spacing);
        }
        const Family& listed = families[*family];
        return AddFace(atlas, strong && listed.strong ? *listed.strong : listed.regular, merge, figures, spacing);
    };
    return system_fonts::AddRoles(families.size(), families.front().strong.has_value(), japanese.has_value(), add_face);
}

} // namespace hokuto::app
