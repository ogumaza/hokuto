// SPDX-License-Identifier: MIT

// What a file is, for the messages that say why Hokuto doesn't take it: "an empty file", "a Hokuto file", "a WAV file",
// or a file of the consoles' sound libraries, such as "a bank file of the 3DS". The file's first bytes give its kind.
// Its extension gives the kind that its name indicates.
//
// The kinds of input that File > Open and `hok` take are known by their first bytes too, in one order.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace hokuto::format
{

// The kinds of input that File > Open, a drop on the main window and `hok` take. Each is known by the first bytes of
// the input. The tests of some kinds can match the same bytes. Every caller tests them in the order of kInputOrder.
enum class InputKind : uint8_t
{
    kMidi,         // A MIDI file: "MThd".
    kHokuto,       // A Hokuto file in either form (IsBinary or IsText).
    kWiiArchive,   // A sound archive of the Wii: "RSAR".
    kLaterArchive, // A sound archive of the 3DS, the Wii U or the Switch: "CSAR" or "FSAR".
    kGbaRom,       // A GBA ROM (IsGbaRom).
    kDsArchive,    // A sound archive of the DS (IsDsArchive).
    kDsImage,      // A DS cartridge image (IsDsImage).
};

// The one order for every caller: File > Open, a drop on the main window, `hok` and `hok import`. An input of no kind,
// or of a kind that a caller doesn't accept, is handled by the caller's fallback.
inline constexpr std::array kInputOrder = {InputKind::kMidi,         InputKind::kHokuto, InputKind::kWiiArchive,
                                           InputKind::kLaterArchive, InputKind::kGbaRom, InputKind::kDsImage,
                                           InputKind::kDsArchive};

// Whether `bytes` start as an input of `kind`. The test reads the header alone. The header of a DS image occupies the
// first 512 bytes.
bool IsInputKind(std::span<const uint8_t> bytes, InputKind kind);

// The first kind of `order` that `bytes` start as, or nothing. The tests run in the order, and stop at the first match.
std::optional<InputKind> FirstInputKind(std::span<const uint8_t> bytes, std::span<const InputKind> order);

// Whether `image`, a DS cartridge image, contains DSE's files and no sound archive of the DS (DsArchivesIn and
// HoldsDseFiles). Both tests parse the whole image. Throws xsflib::FormatError for an image that ntdlib can't read.
bool HoldsDseOnly(std::span<const uint8_t> image);

// The kind of the file that starts with `bytes`: "an empty file", "a Hokuto file", "a WAV file", "an AAC file", "a MIDI
// file", "a SARC archive", "a sound archive of the DS", or a file of the consoles' sound libraries, such as "a sound
// archive of the Wii", "a sequence file of the 3DS", "a bank file of the Wii U" or "a wave file of the Switch". Empty
// for bytes of another kind.
std::string KindOfBytes(std::span<const uint8_t> bytes);

// The kind that the extension of `file` names, in KindOfBytes's words: ".hok" gives "a Hokuto file", ".bcseq" "a
// sequence file of the 3DS", and ".bfwav" "a wave file of the Wii U or the Switch". Empty for another extension.
std::string KindOfName(const std::filesystem::path& file);

// The sentence that says that the file that starts with `bytes` isn't `wanted`, such as "a sequence file": "It's a wave
// file of the Wii U, not a sequence file." where its first bytes give another kind, "It's empty." for an empty file,
// and "It isn't a sequence file." otherwise.
std::string NotKindText(std::span<const uint8_t> bytes, const std::string& wanted);

// The sentence that names what File > Open and a drop on the main window open.
inline constexpr char kOpenedKinds[] = "Hokuto opens Hokuto files, sound archives, GBA ROMs, DS images and MIDI files.";

// Why File > Open, or with `import` File > Import > Sound Archive…, doesn't take `file`, a file that is neither a
// Hokuto file nor a sound archive: the file's kind from its first bytes, or else the kind that its extension indicates,
// and what the command takes. A file of a kind that another command takes names that command: an item of File > Import
// for a file of the consoles' sound libraries or a WAV file, and Replace… for an AAC file.
std::string NotOpenedText(const std::filesystem::path& file, std::span<const uint8_t> bytes, bool import);

// `text`, a clause such as ntdlib's message "too short to be an FSAR file", as a sentence: with a capital letter and a
// full stop. A text that ends with a full stop, a question mark or an exclamation mark is a sentence already, such as
// one of Hokuto's messages, and stays as it is: it can start with a name in lowercase.
std::string AsSentence(std::string text);

} // namespace hokuto::format
