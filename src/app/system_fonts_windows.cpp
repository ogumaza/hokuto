// SPDX-License-Identifier: MIT

// The system's interface fonts on Windows, Segoe UI with Yu Gothic UI or Meiryo UI for Japanese, drawn by DirectWrite.
// Dear ImGui's loader, stb_truetype, draws a glyph without the font's hints, and places it at a fraction of a pixel. On
// a display with one pixel to the point, such text comes out blurred. DirectWrite fits each glyph to the pixels as
// Windows draws text without ClearType, and every glyph starts on a pixel. The text takes the Mac's type size. A line
// of Segoe UI is 1.33 times its type size, and a line of the Mac's interface font 1.18.
//
// Under Wine on Linux, the system has none of Windows' fonts. The families of font_families.h draw the text there, and
// after them the family that GDI answers a request for an interface font with. This loader and system_fonts_other.cpp
// follow the rules of system_fonts_shared.h for the sizes, the figures, the contrast, the baseline and the roles.

#include "app/system_fonts.h"

#include <dwrite_2.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/font_families.h"
#include "app/system_fonts_shared.h"

namespace hokuto::app
{
namespace
{

using Microsoft::WRL::ComPtr;
using system_fonts::BakedSource;
using system_fonts::kFigureSpace;
using system_fonts::kTypePerSize;

// A font source: its face, where its baseline divides a line, and whether it draws the figures. The figures' source
// gives the digits one width, and makes a figure space of that width where the face has none, or one of another width.
struct Source
{
    ComPtr<IDWriteFontFace> face;
    float ascent_per_line = font_families::kAscentPerLine;
    bool figures = false;
};

// What the loader keeps for the program's run: DirectWrite, the sources, and the contrast that Windows adds to text
// drawn without ClearType, as a table from a pixel's coverage to its alpha. A deque keeps each source in its place: the
// font atlas points at them.
struct DirectWrite
{
    ComPtr<IDWriteFactory2> factory;
    std::deque<Source> sources;
    std::array<unsigned char, 256> contrast{};
};

DirectWrite& State()
{
    static DirectWrite state;
    return state;
}

// The face of the font family `family` nearest `weight`. Returns nothing if the system doesn't have the family, or if
// its nearest face is lighter than `lightest`.
ComPtr<IDWriteFontFace> FindFace(IDWriteFontCollection* fonts, const wchar_t* family, DWRITE_FONT_WEIGHT weight,
                                 int lightest)
{
    UINT32 family_index = 0;
    BOOL exists = FALSE;
    ComPtr<IDWriteFontFamily> font_family;
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteFontFace> face;
    if (FAILED(fonts->FindFamilyName(family, &family_index, &exists)) || !exists ||
        FAILED(fonts->GetFontFamily(family_index, &font_family)) ||
        FAILED(
            font_family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) ||
        static_cast<int>(font->GetWeight()) < lightest || FAILED(font->CreateFontFace(&face)))
    {
        return nullptr;
    }

    return face;
}

const Source& SourceOf(const ImFontConfig* src)
{
    return *static_cast<const Source*>(src->FontLoaderData);
}

// The glyph that `face` draws for `codepoint`, or 0 if it has none.
UINT16 GlyphOf(IDWriteFontFace* face, ImWchar codepoint)
{
    const auto character = static_cast<UINT32>(codepoint);
    UINT16 glyph = 0;
    if (FAILED(face->GetGlyphIndices(&character, 1, &glyph)))
    {
        return 0;
    }

    return glyph;
}

// The type size of a font of `size` units, in whole pixels at `density` pixels to the unit. Windows fits a font's
// glyphs to the pixels at whole sizes.
float TypePixels(float size, float density)
{
    return std::max(std::round(size * kTypePerSize * density), 1.0f);
}

// The advance of `glyph` at a type size of `type` pixels, as Windows measures it for text fitted to the pixels, rounded
// to whole pixels. Returns nothing if `glyph` is 0, or if DirectWrite can't measure the glyph.
std::optional<float> AdvancePixels(IDWriteFontFace* face, UINT16 glyph, float type)
{
    if (glyph == 0)
    {
        return std::nullopt;
    }

    DWRITE_FONT_METRICS metrics{};
    face->GetMetrics(&metrics);
    DWRITE_GLYPH_METRICS glyph_metrics{};
    if (FAILED(face->GetGdiCompatibleGlyphMetrics(type, 1.0f, nullptr, FALSE, &glyph, 1, &glyph_metrics, FALSE)))
    {
        return std::nullopt;
    }

    return std::round(static_cast<float>(glyph_metrics.advanceWidth) * type / metrics.designUnitsPerEm);
}

// The advance of the widest digit of `face` at `type` pixels.
float DigitPixels(IDWriteFontFace* face, float type)
{
    const auto advance_of = [&](ImWchar digit)
    {
        return AdvancePixels(face, GlyphOf(face, digit), type);
    };
    return system_fonts::WidestDigitPixels(advance_of);
}

bool SourceInit(ImFontAtlas* /*atlas*/, ImFontConfig* src)
{
    DirectWrite& state = State();
    if (src->FontLoaderFlags >= state.sources.size())
    {
        return false;
    }
    src->FontLoaderData = &state.sources[src->FontLoaderFlags];

    return true;
}

void SourceDestroy(ImFontAtlas* /*atlas*/, ImFontConfig* src)
{
    src->FontLoaderData = nullptr;
}

bool SourceContainsGlyph(ImFontAtlas* /*atlas*/, ImFontConfig* src, ImWchar codepoint)
{
    const Source& source = SourceOf(src);
    return GlyphOf(source.face.Get(), codepoint) != 0 || (source.figures && codepoint == kFigureSpace);
}

bool BakedInit(ImFontAtlas* /*atlas*/, ImFontConfig* src, ImFontBaked* baked, void* loader_data)
{
    const Source& source = SourceOf(src);
    auto* baked_source = new (loader_data) BakedSource;
    if (source.figures)
    {
        const float density = src->RasterizerDensity * baked->RasterizerDensity;
        baked_source->digit_pixels = DigitPixels(source.face.Get(), TypePixels(baked->Size, density));
    }

    system_fonts::PlaceBaseline(*src, *baked, source.ascent_per_line);

    return true;
}

void BakedDestroy(ImFontAtlas* /*atlas*/, ImFontConfig* /*src*/, ImFontBaked* /*baked*/, void* /*loader_data*/)
{
}

bool LoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loader_data, ImWchar codepoint,
               ImFontGlyph* out_glyph, float* out_advance_x)
{
    const Source& source = SourceOf(src);
    IDWriteFontFace* face = source.face.Get();
    const UINT16 glyph = GlyphOf(face, codepoint);
    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    const float type = TypePixels(baked->Size, density);

    // The advance as Windows measures it for text fitted to the pixels, rounded to whole pixels.
    const std::optional<system_fonts::GlyphFit> fit =
        system_fonts::FitGlyph(source.figures, static_cast<const BakedSource*>(loader_data)->digit_pixels, codepoint,
                               AdvancePixels(face, glyph, type));
    if (!fit)
    {
        return false;
    }
    if (system_fonts::LoadWithoutBitmap(*fit, codepoint, density, out_glyph, out_advance_x))
    {
        return true;
    }

    // The glyph's coverage, fitted to the pixels in grayscale, with its origin on a pixel.
    const DirectWrite& state = State();
    const FLOAT kNoAdvance = 0.0f;
    DWRITE_GLYPH_RUN run{};
    run.fontFace = face;
    run.fontEmSize = type;
    run.glyphCount = 1;
    run.glyphIndices = &glyph;
    run.glyphAdvances = &kNoAdvance;
    ComPtr<IDWriteGlyphRunAnalysis> analysis;
    RECT bounds{};
    if (FAILED(state.factory->CreateGlyphRunAnalysis(&run, nullptr, DWRITE_RENDERING_MODE_GDI_CLASSIC,
                                                     DWRITE_MEASURING_MODE_GDI_CLASSIC, DWRITE_GRID_FIT_MODE_ENABLED,
                                                     DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0.0f, 0.0f, &analysis)) ||
        FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds)))
    {
        return false;
    }

    out_glyph->Codepoint = codepoint;
    out_glyph->AdvanceX = fit->advance_pixels / density;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0)
    {
        return true;
    }

    std::vector<unsigned char> coverage(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, coverage.data(),
                                            static_cast<UINT32>(coverage.size()))))
    {
        return false;
    }

    for (unsigned char& pixel : coverage)
    {
        pixel = state.contrast[pixel];
    }

    // The bitmap, placed on the baseline. The baseline lies a whole number of units below the top of the line.
    const ImFontAtlasRectId pack_id = ImFontAtlasPackAddRect(atlas, width, height);
    if (pack_id == ImFontAtlasRectId_Invalid)
    {
        return false;
    }

    ImTextureRect* rect = ImFontAtlasPackGetRect(atlas, pack_id);
    const float baseline = IM_ROUND(baked->Ascent);
    out_glyph->X0 = (static_cast<float>(bounds.left) + fit->shift_pixels) / density;
    out_glyph->Y0 = baseline + static_cast<float>(bounds.top) / density;
    out_glyph->X1 = out_glyph->X0 + static_cast<float>(rect->w) / density;
    out_glyph->Y1 = out_glyph->Y0 + static_cast<float>(rect->h) / density;
    out_glyph->Visible = true;
    out_glyph->PackId = pack_id;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out_glyph, rect, coverage.data(), ImTextureFormat_Alpha8,
                                       width);

    return true;
}

const ImFontLoader& DirectWriteLoader()
{
    static const ImFontLoader kLoader = []
    {
        ImFontLoader loader;
        loader.Name = "DirectWrite";
        loader.FontSrcInit = SourceInit;
        loader.FontSrcDestroy = SourceDestroy;
        loader.FontSrcContainsGlyph = SourceContainsGlyph;
        loader.FontBakedInit = BakedInit;
        loader.FontBakedDestroy = BakedDestroy;
        loader.FontBakedLoadGlyph = LoadGlyph;
        loader.FontBakedSrcLoaderDataSize = sizeof(BakedSource);

        return loader;
    }();
    return kLoader;
}

// Adds `source` to `atlas`. With `merge`, it fills in the glyphs that the font added before it lacks. `spacing` is the
// space added after each glyph, in points.
ImFont* AddSource(ImFontAtlas& atlas, const Source& source, bool merge, float spacing)
{
    DirectWrite& state = State();
    ImFontConfig config;
    config.FontLoader = &DirectWriteLoader();
    config.FontLoaderFlags = static_cast<unsigned int>(state.sources.size());
    config.MergeMode = merge;
    config.GlyphExtraAdvanceX = spacing;
    state.sources.push_back(source);

    return atlas.AddFont(&config);
}

// An interface family that the system has: its face for the text, its face for the strong text, and where its
// baseline divides a line.
struct Family
{
    ComPtr<IDWriteFontFace> regular;
    ComPtr<IDWriteFontFace> strong;
    float ascent_per_line = font_families::kAscentPerLine;
};

std::wstring Wide(std::string_view name)
{
    return {name.begin(), name.end()}; // the names are ASCII
}

// The family that GDI draws a request for an interface font in `charset` with, or nothing. Windows and Wine both answer
// with a family that the system has.
std::wstring GdiFamily(BYTE charset)
{
    LOGFONTW request{};
    request.lfHeight = -13;
    request.lfCharSet = charset;
    request.lfPitchAndFamily = FF_SWISS | VARIABLE_PITCH;
    std::array<wchar_t, LF_FACESIZE> name{};
    if (HDC dc = CreateCompatibleDC(nullptr))
    {
        if (HFONT font = CreateFontIndirectW(&request))
        {
            const HGDIOBJ before = SelectObject(dc, font);
            GetTextFaceW(dc, static_cast<int>(name.size()), name.data());
            SelectObject(dc, before);
            DeleteObject(font);
        }
        DeleteDC(dc);
    }

    return name.data();
}

// The interface's families that the system has. Segoe UI alone where the system has it, as on Windows. Otherwise the
// families of the list that it has, or else the family that GDI answers with.
std::vector<Family> InterfaceFamilies(IDWriteFontCollection* collection)
{
    const auto family_named = [&](const std::wstring& name, Family& family)
    {
        family.regular = FindFace(collection, name.c_str(), DWRITE_FONT_WEIGHT_NORMAL, font_families::kLightestRegular);
        if (!family.regular || GlyphOf(family.regular.Get(), 'A') == 0)
        {
            return false;
        }
        family.strong =
            FindFace(collection, name.c_str(), DWRITE_FONT_WEIGHT_SEMI_BOLD, font_families::kLightestStrong);
        return true;
    };

    // Segoe UI keeps the division of its own line.
    Family segoe;
    if (family_named(L"Segoe UI", segoe))
    {
        DWRITE_FONT_METRICS metrics{};
        segoe.regular->GetMetrics(&metrics);
        segoe.ascent_per_line =
            static_cast<float>(metrics.ascent) / static_cast<float>(metrics.ascent + metrics.descent);
        return {segoe};
    }

    std::vector<Family> families;
    for (const std::string_view name : font_families::kInterface)
    {
        if (Family family; family_named(Wide(name), family))
        {
            families.push_back(family);
        }
    }
    if (families.empty())
    {
        if (Family family; family_named(GdiFamily(DEFAULT_CHARSET), family))
        {
            families.push_back(family);
        }
    }

    return families;
}

// The Japanese interface font of Windows 10 and 11, or of earlier versions, then the first family of the list that the
// system has, then the family that GDI answers a request for Japanese with. Only a face with kana counts.
ComPtr<IDWriteFontFace> JapaneseFace(IDWriteFontCollection* collection)
{
    std::vector<std::wstring> names = {L"Yu Gothic UI", L"Meiryo UI"};
    for (const std::string_view name : font_families::kJapanese)
    {
        names.push_back(Wide(name));
    }
    names.push_back(GdiFamily(SHIFTJIS_CHARSET));
    for (const std::wstring& name : names)
    {
        ComPtr<IDWriteFontFace> face =
            FindFace(collection, name.c_str(), DWRITE_FONT_WEIGHT_NORMAL, font_families::kLightestRegular);
        if (face && GlyphOf(face.Get(), 0x306E) != 0) // の
        {
            return face;
        }
    }

    return nullptr;
}

// The contrast that Windows adds to text drawn without ClearType.
float GrayscaleContrast(IDWriteFactory* factory)
{
    float contrast = 1.0f;
    ComPtr<IDWriteRenderingParams> params;
    ComPtr<IDWriteRenderingParams1> params1;
    if (SUCCEEDED(factory->CreateRenderingParams(&params)) && SUCCEEDED(params.As(&params1)))
    {
        contrast = std::max(params1->GetGrayscaleEnhancedContrast(), 0.0f);
    }

    return contrast;
}

} // namespace

theme::Fonts AddSystemFonts(ImFontAtlas& atlas)
{
    theme::Fonts fonts;
    DirectWrite& state = State();
    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                                   reinterpret_cast<IUnknown**>(state.factory.GetAddressOf()))) ||
        FAILED(state.factory->GetSystemFontCollection(&collection)))
    {
        return fonts;
    }
    state.contrast = system_fonts::ContrastTable(GrayscaleContrast(state.factory.Get()));

    const std::vector<Family> families = InterfaceFamilies(collection.Get());
    if (families.empty())
    {
        return fonts;
    }
    const ComPtr<IDWriteFontFace> japanese = JapaneseFace(collection.Get());

    // Every source draws on the first family's baseline.
    const float ascent_per_line = families.front().ascent_per_line;
    const auto add_face = [&](std::optional<std::size_t> family, bool strong, bool merge, bool figures, float spacing)
    {
        if (!family)
        {
            return AddSource(atlas, Source{japanese, ascent_per_line, figures}, merge, spacing);
        }
        const Family& listed = families[*family];
        const ComPtr<IDWriteFontFace>& face = strong && listed.strong ? listed.strong : listed.regular;
        return AddSource(atlas, Source{face, ascent_per_line, figures}, merge, spacing);
    };
    return system_fonts::AddRoles(families.size(), families.front().strong != nullptr, japanese != nullptr, add_face);
}

} // namespace hokuto::app
