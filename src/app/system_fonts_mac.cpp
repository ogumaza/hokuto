// SPDX-License-Identifier: MIT

// The system's interface font on macOS, San Francisco, drawn by Core Text. Dear ImGui's loader, stb_truetype, draws
// only the default instance of a variable font. San Francisco's default instance is its design for 28-point titles,
// with thin strokes and tight spacing. Core Text picks the design for each size and draws the glyphs as macOS does.
// Font smoothing thickens light text on a dark background. For a character that San Francisco lacks, the loader falls
// back to the font that macOS would use. Japanese takes the place of Chinese in the fallback for the characters that
// the two languages share.

#include "app/system_fonts.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

namespace hokuto::app
{
namespace
{

struct CfRelease
{
    void operator()(CFTypeRef object) const
    {
        CFRelease(object);
    }
};

// Owns a Core Foundation object, such as a CTFontRef, and releases it.
template <typename T>
using CfPtr = std::unique_ptr<std::remove_pointer_t<T>, CfRelease>;

// The styles of the interface font that the program draws with. A font source's ImFontConfig::FontLoaderFlags holds
// one.
enum class Style : unsigned int
{
    kRegular,
    kSemibold,
    kFigures, // the regular style with digits of one width
};

// AppKit's NSFontWeightSemibold. Core Text has no name for it.
constexpr CGFloat kSemiboldWeight = 0.3;

// The size at which a source measures the font, in points. Most of the program's text is this size.
constexpr CGFloat kReferenceSize = 13.0;

// The empty pixels around a glyph's bitmap. Font smoothing spreads a glyph a little past its outline.
constexpr int kPadding = 1;

// What the loader keeps for a font source.
struct Source
{
    Style style = Style::kRegular;

    // The font at kReferenceSize.
    CfPtr<CTFontRef> font;

    // Dear ImGui sizes a font by its line, from its ascent to its descent. This is the type size for each unit of that.
    CGFloat type_per_size = 1.0;

    // The glyphs of the digits 0 to 9 in kFigures. Font shaping picks them, and the font's character map gives other
    // glyphs. A zero means the glyph from the character map.
    std::array<CGGlyph, 10> digits{};
};

// The interface font in `style` at `size` points. Core Text picks the design for the size.
CfPtr<CTFontRef> CreateFont(Style style, CGFloat size)
{
    CfPtr<CTFontRef> regular(CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, nullptr));
    if (!regular || style == Style::kRegular)
    {
        return regular;
    }

    CfPtr<CFMutableDictionaryRef> attributes(
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (style == Style::kSemibold)
    {
        CfPtr<CFNumberRef> weight(CFNumberCreate(nullptr, kCFNumberCGFloatType, &kSemiboldWeight));
        const void* key = kCTFontWeightTrait;
        const void* value = weight.get();
        CfPtr<CFDictionaryRef> traits(CFDictionaryCreate(nullptr, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks));
        CFDictionarySetValue(attributes.get(), kCTFontTraitsAttribute, traits.get());
    }
    else
    {
        // The feature that gives every digit the same width.
        const int kType = kNumberSpacingType;
        const int kSelector = kMonospacedNumbersSelector;
        CfPtr<CFNumberRef> type_number(CFNumberCreate(nullptr, kCFNumberIntType, &kType));
        CfPtr<CFNumberRef> selector_number(CFNumberCreate(nullptr, kCFNumberIntType, &kSelector));
        std::array<const void*, 2> keys = {kCTFontFeatureTypeIdentifierKey, kCTFontFeatureSelectorIdentifierKey};
        std::array<const void*, 2> values = {type_number.get(), selector_number.get()};
        CfPtr<CFDictionaryRef> setting(CFDictionaryCreate(
            nullptr, keys.data(), values.data(), 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        const void* setting_value = setting.get();
        CfPtr<CFArrayRef> settings(CFArrayCreate(nullptr, &setting_value, 1, &kCFTypeArrayCallBacks));
        CFDictionarySetValue(attributes.get(), kCTFontFeatureSettingsAttribute, settings.get());
    }

    CfPtr<CTFontDescriptorRef> descriptor(CTFontCopyFontDescriptor(regular.get()));
    CfPtr<CTFontDescriptorRef> styled(CTFontDescriptorCreateCopyWithAttributes(descriptor.get(), attributes.get()));
    return CfPtr<CTFontRef>(CTFontCreateWithFontDescriptor(styled.get(), size, nullptr));
}

// Writes `character` in UTF-16 to `units`. Returns the number of code units.
CFIndex ToUtf16(char32_t character, std::array<UniChar, 2>& units)
{
    if (character < 0x10000)
    {
        units[0] = static_cast<UniChar>(character);
        return 1;
    }

    const char32_t offset = character - 0x10000;
    units[0] = static_cast<UniChar>(0xD800 + (offset >> 10));
    units[1] = static_cast<UniChar>(0xDC00 + (offset & 0x3FF));

    return 2;
}

// The glyph that `font` draws for `character` in its character map, or 0 if it has none.
CGGlyph GlyphOf(CTFontRef font, char32_t character)
{
    std::array<UniChar, 2> units{};
    const CFIndex count = ToUtf16(character, units);
    std::array<CGGlyph, 2> glyphs{};
    if (!CTFontGetGlyphsForCharacters(font, units.data(), glyphs.data(), count))
    {
        return 0;
    }

    return glyphs[0];
}

// The font that macOS falls back to after `font` for `character`. Returns nothing when no font draws the character in
// one colour. Emoji come in colour, and Core Text's last resort draws a box with the character's code in it.
CfPtr<CTFontRef> FallbackFor(CTFontRef font, char32_t character)
{
    std::array<UniChar, 2> units{};
    const CFIndex count = ToUtf16(character, units);
    CfPtr<CFStringRef> text(CFStringCreateWithCharacters(nullptr, units.data(), count));
    CfPtr<CTFontRef> fallback(CTFontCreateForStringWithLanguage(font, text.get(), CFRangeMake(0, count), CFSTR("ja")));
    if (!fallback || (CTFontGetSymbolicTraits(fallback.get()) & kCTFontTraitColorGlyphs) != 0 ||
        GlyphOf(fallback.get(), character) == 0)
    {
        return nullptr;
    }

    CfPtr<CFStringRef> name(CTFontCopyPostScriptName(fallback.get()));
    if (!name || CFStringCompare(name.get(), CFSTR("LastResort"), 0) == kCFCompareEqualTo)
    {
        return nullptr;
    }

    return fallback;
}

// The glyphs that `font` draws for the digits 0 to 9 after its features. Returns false if shaping doesn't give one
// glyph for each digit.
bool ShapeDigits(CTFontRef font, std::array<CGGlyph, 10>& digits)
{
    const void* key = kCTFontAttributeName;
    const void* value = font;
    CfPtr<CFDictionaryRef> attributes(
        CFDictionaryCreate(nullptr, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CfPtr<CFAttributedStringRef> text(CFAttributedStringCreate(nullptr, CFSTR("0123456789"), attributes.get()));
    CfPtr<CTLineRef> line(CTLineCreateWithAttributedString(text.get()));
    CFArrayRef runs = CTLineGetGlyphRuns(line.get());
    if (CFArrayGetCount(runs) != 1)
    {
        return false;
    }

    const auto run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, 0));
    if (CTRunGetGlyphCount(run) != static_cast<CFIndex>(digits.size()))
    {
        return false;
    }

    CTRunGetGlyphs(run, CFRangeMake(0, static_cast<CFIndex>(digits.size())), digits.data());

    return true;
}

// Draws `glyph` white on black with font smoothing. macOS draws light text on a dark background that way. The bitmap is
// `width` by `height` pixels, and its bottom-left corner is (`left`, `bottom`) pixels from the glyph's origin. Returns
// the glyph's coverage, a byte for each pixel, from the top row down.
std::vector<unsigned char> Rasterise(CTFontRef font, CGGlyph glyph, float density, int left, int bottom, int width,
                                     int height)
{
    // A bitmap of red, green, blue and an unused byte. Font smoothing requires an opaque bitmap.
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<unsigned char> pixels(pixel_count * 4);
    CfPtr<CGColorSpaceRef> space(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    constexpr uint32_t kLayout =
        static_cast<uint32_t>(kCGImageAlphaNoneSkipLast) | static_cast<uint32_t>(kCGBitmapByteOrder32Big);
    CfPtr<CGContextRef> context(CGBitmapContextCreate(pixels.data(), static_cast<std::size_t>(width),
                                                      static_cast<std::size_t>(height), 8,
                                                      static_cast<std::size_t>(width) * 4, space.get(), kLayout));
    if (!context)
    {
        return std::vector<unsigned char>(pixel_count);
    }

    CGContextSetGrayFillColor(context.get(), 0.0, 1.0);
    CGContextFillRect(context.get(), CGRectMake(0.0, 0.0, width, height));
    CGContextSetAllowsFontSmoothing(context.get(), true);
    CGContextSetShouldSmoothFonts(context.get(), true);
    CGContextSetGrayFillColor(context.get(), 1.0, 1.0);
    CGContextScaleCTM(context.get(), density, density);
    const CGPoint origin = CGPointMake(-left / density, -bottom / density);
    CTFontDrawGlyphs(font, &glyph, &origin, 1, context.get());

    // The green channel. Font smoothing no longer draws subpixels. The three channels match.
    std::vector<unsigned char> coverage(pixel_count);
    for (std::size_t i = 0; i < pixel_count; i++)
    {
        coverage[i] = pixels[i * 4 + 1];
    }

    return coverage;
}

Source& SourceOf(ImFontConfig* src)
{
    return *static_cast<Source*>(src->FontLoaderData);
}

// The font of a baked size. The loader keeps it in Dear ImGui's storage for the size.
CTFontRef BakedFontOf(const void* loader_data)
{
    CTFontRef font = nullptr;
    std::memcpy(&font, loader_data, sizeof(font));

    return font;
}

bool SourceInit(ImFontAtlas* /*atlas*/, ImFontConfig* src)
{
    auto source = std::make_unique<Source>();
    source->style = static_cast<Style>(src->FontLoaderFlags);
    source->font = CreateFont(source->style, kReferenceSize);
    if (!source->font)
    {
        return false;
    }

    const CGFloat line = CTFontGetAscent(source->font.get()) + CTFontGetDescent(source->font.get());
    source->type_per_size = kReferenceSize / line;
    if (source->style == Style::kFigures && !ShapeDigits(source->font.get(), source->digits))
    {
        source->digits = {};
    }
    src->FontLoaderData = source.release();

    return true;
}

void SourceDestroy(ImFontAtlas* /*atlas*/, ImFontConfig* src)
{
    delete static_cast<Source*>(src->FontLoaderData);
    src->FontLoaderData = nullptr;
}

bool SourceContainsGlyph(ImFontAtlas* /*atlas*/, ImFontConfig* src, ImWchar codepoint)
{
    const Source& source = SourceOf(src);
    return GlyphOf(source.font.get(), codepoint) != 0 || FallbackFor(source.font.get(), codepoint) != nullptr;
}

bool BakedInit(ImFontAtlas* /*atlas*/, ImFontConfig* src, ImFontBaked* baked, void* loader_data)
{
    const Source& source = SourceOf(src);
    CTFontRef font = CreateFont(source.style, baked->Size * source.type_per_size).release();
    std::memcpy(loader_data, &font, sizeof(font));
    if (font == nullptr)
    {
        return false;
    }

    if (!src->MergeMode)
    {
        baked->Ascent = ImCeil(static_cast<float>(CTFontGetAscent(font)));
        baked->Descent = ImFloor(static_cast<float>(-CTFontGetDescent(font)));
    }

    return true;
}

void BakedDestroy(ImFontAtlas* /*atlas*/, ImFontConfig* /*src*/, ImFontBaked* /*baked*/, void* loader_data)
{
    if (CTFontRef font = BakedFontOf(loader_data))
    {
        CFRelease(font);
    }
}

bool LoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loader_data, ImWchar codepoint,
               ImFontGlyph* out_glyph, float* out_advance_x)
{
    CTFontRef font = BakedFontOf(loader_data);
    if (font == nullptr)
    {
        return false;
    }

    // The glyph, from San Francisco or from the font that macOS falls back to.
    const Source& source = SourceOf(src);
    const char32_t character = codepoint;
    CGGlyph glyph = GlyphOf(font, character);
    if (source.style == Style::kFigures && character >= U'0' && character <= U'9' &&
        source.digits[character - U'0'] != 0)
    {
        glyph = source.digits[character - U'0'];
    }

    CfPtr<CTFontRef> fallback;
    CTFontRef glyph_font = font;
    if (glyph == 0)
    {
        fallback = FallbackFor(font, character);
        if (!fallback)
        {
            return false;
        }
        glyph_font = fallback.get();
        glyph = GlyphOf(glyph_font, character);
    }

    // The advance, rounded to whole pixels. Dear ImGui starts each line of text on a pixel. Every glyph then starts on
    // one, and none is blurred by falling between pixels.
    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    CGSize advance = CGSizeZero;
    CTFontGetAdvancesForGlyphs(glyph_font, kCTFontOrientationHorizontal, &glyph, &advance, 1);
    const float advance_x = std::round(static_cast<float>(advance.width) * density) / density;
    if (out_advance_x != nullptr)
    {
        *out_advance_x = advance_x;
        return true;
    }

    out_glyph->Codepoint = codepoint;
    out_glyph->AdvanceX = advance_x;
    CGRect bounds = CGRectZero;
    CTFontGetBoundingRectsForGlyphs(glyph_font, kCTFontOrientationHorizontal, &glyph, &bounds, 1);
    if (CGRectIsEmpty(bounds))
    {
        return true;
    }

    // The glyph's box in pixels from its origin, with y upward, and its place in the atlas.
    const int left = static_cast<int>(std::floor(CGRectGetMinX(bounds) * density)) - kPadding;
    const int right = static_cast<int>(std::ceil(CGRectGetMaxX(bounds) * density)) + kPadding;
    const int bottom = static_cast<int>(std::floor(CGRectGetMinY(bounds) * density)) - kPadding;
    const int top = static_cast<int>(std::ceil(CGRectGetMaxY(bounds) * density)) + kPadding;
    const ImFontAtlasRectId pack_id = ImFontAtlasPackAddRect(atlas, right - left, top - bottom);
    if (pack_id == ImFontAtlasRectId_Invalid)
    {
        return false;
    }

    ImTextureRect* rect = ImFontAtlasPackGetRect(atlas, pack_id);

    // The bitmap, placed on the baseline. The baseline lies a whole number of points below the top of the line.
    const std::vector<unsigned char> coverage =
        Rasterise(glyph_font, glyph, density, left, bottom, right - left, top - bottom);
    const float baseline = IM_ROUND(baked->Ascent);
    out_glyph->X0 = static_cast<float>(left) / density;
    out_glyph->Y0 = baseline - static_cast<float>(top) / density;
    out_glyph->X1 = out_glyph->X0 + static_cast<float>(rect->w) / density;
    out_glyph->Y1 = out_glyph->Y0 + static_cast<float>(rect->h) / density;
    out_glyph->Visible = true;
    out_glyph->PackId = pack_id;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out_glyph, rect, coverage.data(), ImTextureFormat_Alpha8,
                                       right - left);

    return true;
}

const ImFontLoader& CoreTextLoader()
{
    static const ImFontLoader kLoader = []
    {
        ImFontLoader loader;
        loader.Name = "Core Text";
        loader.FontSrcInit = SourceInit;
        loader.FontSrcDestroy = SourceDestroy;
        loader.FontSrcContainsGlyph = SourceContainsGlyph;
        loader.FontBakedInit = BakedInit;
        loader.FontBakedDestroy = BakedDestroy;
        loader.FontBakedLoadGlyph = LoadGlyph;
        loader.FontBakedSrcLoaderDataSize = sizeof(CTFontRef);

        return loader;
    }();
    return kLoader;
}

// Adds the interface font in `style` to `atlas`. `spacing` is the space added after each glyph, in points.
ImFont* AddStyle(ImFontAtlas& atlas, Style style, float spacing)
{
    ImFontConfig config;
    config.FontLoader = &CoreTextLoader();
    config.FontLoaderFlags = static_cast<unsigned int>(style);
    config.GlyphExtraAdvanceX = spacing;

    return atlas.AddFont(&config);
}

} // namespace

theme::Fonts AddSystemFonts(ImFontAtlas& atlas)
{
    // The capitals' spacing is a whole number of pixels on a display with two pixels to the point. Their glyphs then
    // start on pixels.
    theme::Fonts fonts;
    fonts.text = AddStyle(atlas, Style::kRegular, 0.0f);
    fonts.strong = AddStyle(atlas, Style::kSemibold, 0.0f);
    fonts.caps = AddStyle(atlas, Style::kSemibold, 1.0f);
    fonts.figures = AddStyle(atlas, Style::kFigures, 0.0f);

    return fonts;
}

} // namespace hokuto::app
