// SPDX-License-Identifier: MIT

// The system's interface fonts on Windows, Segoe UI with Yu Gothic UI or Meiryo UI for Japanese, drawn by DirectWrite.
// Dear ImGui's loader, stb_truetype, draws a glyph without the font's hints, and places it at a fraction of a pixel. On
// a display with one pixel to the point, such text comes out blurred. DirectWrite fits each glyph to the pixels as
// Windows draws text without ClearType, and every glyph starts on a pixel. The text takes the Mac's type size. A line
// of Segoe UI is 1.33 times its type size, and a line of the Mac's interface font 1.18.

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
#include <initializer_list>
#include <vector>

namespace hokuto::app
{
namespace
{

using Microsoft::WRL::ComPtr;

// The type size for each unit of a font's size. Dear ImGui sizes a font by its line. The Mac's interface font has a
// line of 1.18 times its type size.
constexpr float kTypePerSize = 1.0f / 1.18f;

// What the loader keeps for the program's run: DirectWrite, the font faces that the sources draw, and the contrast that
// Windows adds to text drawn without ClearType, as a table from a pixel's coverage to its alpha.
struct DirectWrite
{
    ComPtr<IDWriteFactory2> factory;
    std::vector<ComPtr<IDWriteFontFace>> faces;
    std::array<unsigned char, 256> contrast{};
};

DirectWrite& State()
{
    static DirectWrite state;
    return state;
}

// The face of the font family `family` at `weight`. Returns nothing if the system doesn't have the family.
ComPtr<IDWriteFontFace> FindFace(IDWriteFontCollection* fonts, const wchar_t* family, DWRITE_FONT_WEIGHT weight)
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
        FAILED(font->CreateFontFace(&face)))
    {
        return nullptr;
    }

    return face;
}

// The face of the first of `families` that the system has, at `weight`.
ComPtr<IDWriteFontFace> FindFirst(IDWriteFontCollection* fonts, std::initializer_list<const wchar_t*> families,
                                  DWRITE_FONT_WEIGHT weight)
{
    for (const wchar_t* family : families)
    {
        if (ComPtr<IDWriteFontFace> face = FindFace(fonts, family, weight))
        {
            return face;
        }
    }

    return nullptr;
}

IDWriteFontFace* FaceOf(const ImFontConfig* src)
{
    return static_cast<IDWriteFontFace*>(src->FontLoaderData);
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

bool SourceInit(ImFontAtlas* /*atlas*/, ImFontConfig* src)
{
    const DirectWrite& state = State();
    if (src->FontLoaderFlags >= state.faces.size())
    {
        return false;
    }
    src->FontLoaderData = state.faces[src->FontLoaderFlags].Get();

    return true;
}

void SourceDestroy(ImFontAtlas* /*atlas*/, ImFontConfig* src)
{
    src->FontLoaderData = nullptr;
}

bool SourceContainsGlyph(ImFontAtlas* /*atlas*/, ImFontConfig* src, ImWchar codepoint)
{
    return GlyphOf(FaceOf(src), codepoint) != 0;
}

bool BakedInit(ImFontAtlas* /*atlas*/, ImFontConfig* src, ImFontBaked* baked, void* /*loader_data*/)
{
    // The baseline lies where it divides the font's line in the size. A merged font draws on the first font's.
    if (!src->MergeMode)
    {
        DWRITE_FONT_METRICS metrics{};
        FaceOf(src)->GetMetrics(&metrics);
        const float ascent = static_cast<float>(metrics.ascent);
        const float line = ascent + static_cast<float>(metrics.descent);
        baked->Ascent = IM_ROUND(baked->Size * ascent / line);
        baked->Descent = baked->Ascent - baked->Size;
    }

    return true;
}

void BakedDestroy(ImFontAtlas* /*atlas*/, ImFontConfig* /*src*/, ImFontBaked* /*baked*/, void* /*loader_data*/)
{
}

bool LoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* /*loader_data*/, ImWchar codepoint,
               ImFontGlyph* out_glyph, float* out_advance_x)
{
    IDWriteFontFace* face = FaceOf(src);
    const UINT16 glyph = GlyphOf(face, codepoint);
    if (glyph == 0)
    {
        return false;
    }

    // The advance as Windows measures it for text fitted to the pixels, rounded to whole pixels. Dear ImGui starts each
    // line of text on a pixel. Every glyph then starts on one, and none is blurred by falling between pixels.
    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    const float type = TypePixels(baked->Size, density);
    DWRITE_FONT_METRICS metrics{};
    face->GetMetrics(&metrics);
    DWRITE_GLYPH_METRICS glyph_metrics{};
    if (FAILED(face->GetGdiCompatibleGlyphMetrics(type, 1.0f, nullptr, FALSE, &glyph, 1, &glyph_metrics, FALSE)))
    {
        return false;
    }

    const float advance_x =
        std::round(static_cast<float>(glyph_metrics.advanceWidth) * type / metrics.designUnitsPerEm) / density;
    if (out_advance_x != nullptr)
    {
        *out_advance_x = advance_x;
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
    out_glyph->AdvanceX = advance_x;
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
    out_glyph->X0 = static_cast<float>(bounds.left) / density;
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
        loader.FontBakedSrcLoaderDataSize = 0;

        return loader;
    }();
    return kLoader;
}

// Adds `face` to `atlas`. With `merge`, it fills in the glyphs that the font added before it lacks. `spacing` is the
// space added after each glyph, in points.
ImFont* AddFace(ImFontAtlas& atlas, const ComPtr<IDWriteFontFace>& face, bool merge, float spacing)
{
    DirectWrite& state = State();
    ImFontConfig config;
    config.FontLoader = &DirectWriteLoader();
    config.FontLoaderFlags = static_cast<unsigned int>(state.faces.size());
    config.MergeMode = merge;
    config.GlyphExtraAdvanceX = spacing;
    state.faces.push_back(face);

    return atlas.AddFont(&config);
}

// The table from a pixel's coverage to its alpha with the contrast that Windows adds to text drawn without ClearType.
// The contrast thickens the thin strokes that grayscale coverage gives light text on a dark background.
std::array<unsigned char, 256> ContrastTable(IDWriteFactory* factory)
{
    float contrast = 1.0f;
    ComPtr<IDWriteRenderingParams> params;
    ComPtr<IDWriteRenderingParams1> params1;
    if (SUCCEEDED(factory->CreateRenderingParams(&params)) && SUCCEEDED(params.As(&params1)))
    {
        contrast = std::max(params1->GetGrayscaleEnhancedContrast(), 0.0f);
    }

    std::array<unsigned char, 256> table{};
    for (std::size_t i = 0; i < table.size(); i++)
    {
        const float coverage = static_cast<float>(i) / 255.0f;
        const float alpha = coverage * (contrast + 1.0f) / (coverage * contrast + 1.0f);
        table[i] = static_cast<unsigned char>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
    }

    return table;
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
    state.contrast = ContrastTable(state.factory.Get());

    // The interface font, and the Japanese interface font of Windows 10 and 11, or of earlier versions.
    const ComPtr<IDWriteFontFace> regular = FindFace(collection.Get(), L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL);
    const ComPtr<IDWriteFontFace> semibold = FindFace(collection.Get(), L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const ComPtr<IDWriteFontFace> japanese =
        FindFirst(collection.Get(), {L"Yu Gothic UI", L"Meiryo UI"}, DWRITE_FONT_WEIGHT_NORMAL);

    // The text and the strong text, each followed by the Japanese font.
    if (regular)
    {
        fonts.text = AddFace(atlas, regular, false, 0.0f);
        if (japanese)
        {
            AddFace(atlas, japanese, true, 0.0f);
        }
    }
    if (semibold)
    {
        fonts.strong = AddFace(atlas, semibold, false, 0.0f);
        if (japanese)
        {
            AddFace(atlas, japanese, true, 0.0f);
        }
        fonts.caps = AddFace(atlas, semibold, false, 1.0f);
    }

    // The figures are the text's font. The regular weight of Segoe UI gives its digits one width. The Mac's figures
    // have one width too. Times pad their fields with figure spaces (U+2007). Where Segoe UI has no glyph for them, a
    // space takes their place, and Dear ImGui draws no question mark.
    if (regular)
    {
        fonts.figures = AddFace(atlas, regular, false, 0.0f);
        if (fonts.figures != nullptr && !fonts.figures->IsGlyphInFont(0x2007))
        {
            fonts.figures->AddRemapChar(0x2007, ' ');
        }
        if (japanese)
        {
            AddFace(atlas, japanese, true, 0.0f);
        }
    }

    return fonts;
}

} // namespace hokuto::app
