// SPDX-License-Identifier: MIT

// Tests the kinds of input by their first bytes (src/format/kinds.h): the one order of File > Open, `hok` and `hok
// import` against copies of the chains of tests that each of them used before, over a matrix of made-up headers. The
// one order keeps the kinds of File > Open, and changes those of `hok` and `hok import` in the cases that TestOneOrder
// lists. The four inputs that the old chains took as different kinds have checks of their own.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "xsflib/testing/check.h"

#include "format/archive.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/text.h"

namespace format = hokuto::format;

using format::InputKind;

namespace
{

// edit::IsMidi as it was.
bool IsMidi(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 4 && std::memcmp(bytes.data(), "MThd", 4) == 0;
}

// App::OpensAsDocument's test as it was, on the first 512 bytes of a file.
bool OldOpensAsDocument(std::span<const uint8_t> head)
{
    const auto starts = [&head](std::string_view magic)
    {
        return head.size() >= magic.size() && std::equal(magic.begin(), magic.end(), head.begin());
    };
    return format::IsBinary(head) || format::IsText(head) || starts("RSAR") || starts("CSAR") || starts("FSAR") ||
           format::IsGbaRom(head) || format::IsDsArchive(head) || format::IsDsImage(head) || IsMidi(head);
}

// The chain of App::Opened as it was, with the test of App::ImportedDs between a DS image and a sound archive of the
// DS. The rule for DSE's files splits the kind of a DS image after the chain, and stays out.
std::optional<InputKind> OldOpened(std::span<const uint8_t> bytes)
{
    const auto starts = [&bytes](const char* magic)
    {
        return bytes.size() >= 4 && std::memcmp(bytes.data(), magic, 4) == 0;
    };

    if (format::IsBinary(bytes) || format::IsText(bytes))
    {
        return InputKind::kHokuto;
    }
    if (starts("RSAR"))
    {
        return InputKind::kWiiArchive;
    }
    if (starts("CSAR") || starts("FSAR"))
    {
        return InputKind::kLaterArchive;
    }
    if (format::IsGbaRom(bytes))
    {
        return InputKind::kGbaRom;
    }
    if (format::IsDsArchive(bytes) || format::IsDsImage(bytes))
    {
        return format::IsDsImage(bytes) ? InputKind::kDsImage : InputKind::kDsArchive;
    }

    return std::nullopt;
}

// The chain of ReadArchive in hok.cpp as it was. A Hokuto file reads as one at the end, and an input of no other kind
// reads as a later archive. The driver of a GBA ROM and the rule for DSE's files split a kind after the chain, and stay
// out, as in OldOpened.
std::optional<InputKind> OldHokRead(std::span<const uint8_t> bytes)
{
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "RSAR", 4) == 0)
    {
        return InputKind::kWiiArchive;
    }

    const bool hok_file = hokuto::format::IsBinary(bytes) || hokuto::format::IsText(bytes);
    if (!hok_file && hokuto::format::IsGbaRom(bytes))
    {
        return InputKind::kGbaRom;
    }
    if (hokuto::format::IsDsArchive(bytes))
    {
        return InputKind::kDsArchive;
    }
    if (!hok_file && hokuto::format::IsDsImage(bytes))
    {
        return InputKind::kDsImage;
    }
    if (!hok_file)
    {
        return std::nullopt;
    }

    return InputKind::kHokuto;
}

// The chain of RunImport in hok.cpp as it was. An input of no other kind imports as a later archive.
std::optional<InputKind> OldHokImport(std::span<const uint8_t> bytes)
{
    const bool wii = bytes.size() >= 4 && std::memcmp(bytes.data(), "RSAR", 4) == 0;
    const bool ds_image = !wii && hokuto::format::IsDsImage(bytes);
    const bool gba = !wii && !ds_image && hokuto::format::IsGbaRom(bytes);

    if (ds_image)
    {
        return InputKind::kDsImage;
    }
    else if (hokuto::format::IsDsArchive(bytes))
    {
        return InputKind::kDsArchive;
    }
    else if (gba)
    {
        return InputKind::kGbaRom;
    }
    else if (wii)
    {
        return InputKind::kWiiArchive;
    }

    return std::nullopt;
}

// `size` bytes that start with `magic`, cut to the size, with the GBA's fixed value 0x96 at 0xB2 and the DS's CRC-16
// 0xCF56 at 0x15C where they fit and are asked for. The other bytes are 0.
std::vector<uint8_t> Header(std::span<const uint8_t> magic, std::size_t size, bool gba, bool ds)
{
    std::vector<uint8_t> bytes(size);
    std::copy_n(magic.begin(), std::min(magic.size(), size), bytes.begin());
    if (gba && size > 0xB2)
    {
        bytes[0xB2] = 0x96;
    }
    if (ds && size > 0x15D)
    {
        bytes[0x15C] = 0x56;
        bytes[0x15D] = 0xCF;
    }

    return bytes;
}

// The bytes of `text`.
std::vector<uint8_t> Bytes(std::string_view text)
{
    return {text.begin(), text.end()};
}

// The four inputs that the old chains took differently. Bytes that are both a GBA ROM and a DS image opened and read as
// the ROM, and imported as the image. A sound archive of the DS with the DS's CRC-16 at 0x15C opened and imported as an
// image, and read as the archive. A Hokuto file with the same bytes imported as an image too. Bytes of no kind were
// left to each caller.
void TestOldChainsDiffer()
{
    const std::vector<uint8_t> rom_image = Header({}, 0x200, true, true);
    const std::vector<uint8_t> sdat_image = Header(Bytes("SDAT"), 0x200, false, true);
    const std::vector<uint8_t> hok_image = Header(Bytes("hok "), 0x200, false, true);
    const std::vector<uint8_t> none = Header(Bytes("SARC"), 0x200, false, false);

    XSFLIB_CHECK(OldOpened(rom_image) == InputKind::kGbaRom && OldHokRead(rom_image) == InputKind::kGbaRom &&
                 OldHokImport(rom_image) == InputKind::kDsImage);
    XSFLIB_CHECK(OldOpened(sdat_image) == InputKind::kDsImage && OldHokRead(sdat_image) == InputKind::kDsArchive &&
                 OldHokImport(sdat_image) == InputKind::kDsImage);
    XSFLIB_CHECK(OldOpened(hok_image) == InputKind::kHokuto && OldHokRead(hok_image) == InputKind::kHokuto &&
                 OldHokImport(hok_image) == InputKind::kDsImage);
    XSFLIB_CHECK(!OldOpened(none) && !OldHokRead(none) && !OldHokImport(none));
}

// The kind that `hok` reads an input as, or with `import` the kind that `hok import` imports it as. A MIDI file and an
// input of no kind fall back to a later archive, and so does a Hokuto file in `hok import`.
InputKind HokKind(std::optional<InputKind> kind, bool import)
{
    const bool fallback = !kind || *kind == InputKind::kMidi || (import && *kind == InputKind::kHokuto);
    return fallback ? InputKind::kLaterArchive : *kind;
}

// The one order, for every signature, with and without the GBA bytes and the DS bytes, at each size around the places
// that the tests read. File > Open and a drop on the main window take every input as before: the order is theirs, with
// the test for a MIDI file first. `hok` and `hok import` take an input as another kind in these cases alone:
// - a DS image (0xCF56 at 0x15C) that is also a sound archive of the DS, the 3DS, the Wii U or the Switch: `hok` read
//   the first as the archive and the others as the image, and now reads each as File > Open does;
// - in `hok import`, a DS image that is also a GBA ROM, a later archive or a Hokuto file;
// - a MIDI file that is also a GBA ROM or a DS image, in both.
void TestOneOrder()
{
    const std::vector<std::vector<uint8_t>> kMagics = {
        {},
        Bytes("RSAR"),
        Bytes("CSAR"),
        Bytes("FSAR"),
        Bytes("SDAT"),
        Bytes("MThd"),
        Bytes("SARC"),
        {format::kSignature.begin(), format::kSignature.end()},
        Bytes("hok "),
        Bytes("\xEF\xBB\xBFhok "),
        Bytes("hok"),
    };
    constexpr std::array<std::size_t, 11> kSizes = {0, 3, 4, 15, 16, 24, 0xB2, 0xB3, 0x15D, 0x1FF, 0x200};

    int read_changes = 0;
    int import_changes = 0;
    for (const std::vector<uint8_t>& magic : kMagics)
    {
        for (const std::size_t size : kSizes)
        {
            for (const bool gba : {false, true})
            {
                for (const bool ds : {false, true})
                {
                    const std::vector<uint8_t> bytes = Header(magic, size, gba, ds);
                    const std::optional<InputKind> kind = format::FirstInputKind(bytes, format::kInputOrder);
                    XSFLIB_CHECK(kind == (IsMidi(bytes) ? InputKind::kMidi : OldOpened(bytes)));
                    XSFLIB_CHECK(kind.has_value() == OldOpensAsDocument(bytes));
                    XSFLIB_CHECK(format::IsInputKind(bytes, InputKind::kMidi) == IsMidi(bytes));

                    const auto is = [&bytes](InputKind of)
                    {
                        return format::IsInputKind(bytes, of);
                    };
                    const bool image = is(InputKind::kDsImage);
                    const bool midi_too = is(InputKind::kMidi) && (is(InputKind::kGbaRom) || image);
                    const bool read_changed =
                        (image && (is(InputKind::kDsArchive) || is(InputKind::kLaterArchive))) || midi_too;
                    const bool import_changed =
                        (image && (is(InputKind::kGbaRom) || is(InputKind::kLaterArchive) || is(InputKind::kHokuto))) ||
                        midi_too;
                    XSFLIB_CHECK((HokKind(kind, false) != HokKind(OldHokRead(bytes), false)) == read_changed);
                    XSFLIB_CHECK((HokKind(kind, true) != HokKind(OldHokImport(bytes), true)) == import_changed);
                    read_changes += read_changed ? 1 : 0;
                    import_changes += import_changed ? 1 : 0;
                }
            }
        }
    }
    XSFLIB_CHECK(read_changes == 15 && import_changes == 22);

    // The inputs of TestOldChainsDiffer, and a later archive with the DS's CRC-16. `hok` and `hok import` treated that
    // archive as a DS image.
    const std::vector<uint8_t> later_image = Header(Bytes("FSAR"), 0x200, false, true);
    XSFLIB_CHECK(OldHokRead(later_image) == InputKind::kDsImage && OldHokImport(later_image) == InputKind::kDsImage);
    XSFLIB_CHECK(format::FirstInputKind(later_image, format::kInputOrder) == InputKind::kLaterArchive);
    XSFLIB_CHECK(format::FirstInputKind(Header({}, 0x200, true, true), format::kInputOrder) == InputKind::kGbaRom);
    XSFLIB_CHECK(format::FirstInputKind(Header(Bytes("SDAT"), 0x200, false, true), format::kInputOrder) ==
                 InputKind::kDsImage);
    XSFLIB_CHECK(format::FirstInputKind(Header(Bytes("hok "), 0x200, false, true), format::kInputOrder) ==
                 InputKind::kHokuto);
    XSFLIB_CHECK(!format::FirstInputKind(Header(Bytes("SARC"), 0x200, false, false), format::kInputOrder));
}

// Each kind's test alone, whatever the order.
void TestEachKind()
{
    XSFLIB_CHECK(format::IsInputKind(Bytes("MThd"), InputKind::kMidi) &&
                 !format::IsInputKind(Bytes("MTh"), InputKind::kMidi));
    XSFLIB_CHECK(format::IsInputKind(Bytes("hok 1.0\n"), InputKind::kHokuto) &&
                 !format::IsInputKind(Bytes("hok"), InputKind::kHokuto));
    XSFLIB_CHECK(format::IsInputKind(Bytes("RSAR"), InputKind::kWiiArchive) &&
                 !format::IsInputKind(Bytes("RSA"), InputKind::kWiiArchive));
    XSFLIB_CHECK(format::IsInputKind(Bytes("CSAR"), InputKind::kLaterArchive) &&
                 format::IsInputKind(Bytes("FSAR"), InputKind::kLaterArchive) &&
                 !format::IsInputKind(Bytes("RSAR"), InputKind::kLaterArchive));
    XSFLIB_CHECK(format::IsInputKind(Header({}, 0xB3, true, false), InputKind::kGbaRom) &&
                 !format::IsInputKind(Header({}, 0xB2, true, false), InputKind::kGbaRom));
    XSFLIB_CHECK(format::IsInputKind(Header(Bytes("SDAT"), 16, false, false), InputKind::kDsArchive) &&
                 !format::IsInputKind(Header(Bytes("SDAT"), 15, false, false), InputKind::kDsArchive));
    XSFLIB_CHECK(format::IsInputKind(Header({}, 0x200, false, true), InputKind::kDsImage) &&
                 !format::IsInputKind(Header({}, 0x1FF, false, true), InputKind::kDsImage));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("input_kinds",
                                    []
                                    {
                                        TestOldChainsDiffer();
                                        TestOneOrder();
                                        TestEachKind();
                                    });
}
