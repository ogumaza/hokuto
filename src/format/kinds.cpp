// SPDX-License-Identifier: MIT

#include "format/kinds.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "xsflib/files.h"

#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"

namespace hokuto::format
{
namespace
{

// The kinds of the consoles' files: the three letters after the console's letter, and the kind's name.
struct LibraryKind
{
    std::string_view letters;
    std::string_view name;
};

constexpr std::array<LibraryKind, 8> kLibraryKinds = {{
    {"SAR", "a sound archive"},
    {"SEQ", "a sequence file"},
    {"BNK", "a bank file"},
    {"WAR", "a wave archive file"},
    {"WSD", "a wave sound file"},
    {"WAV", "a wave file"},
    {"STM", "a stream file"},
    {"GRP", "a group file"},
}};

// The kinds that other extensions name.
struct NamedKind
{
    std::string_view extension;
    std::string_view name;
};

constexpr std::array<NamedKind, 8> kNamedKinds = {{
    {".hok", "a Hokuto file"},
    {".wav", "a WAV file"},
    {".aac", "an AAC file"},
    {".adts", "an AAC file"},
    {".mid", "a MIDI file"},
    {".midi", "a MIDI file"},
    {".sarc", "a SARC archive"},
    {".sdat", "a sound archive of the DS"},
}};

// The commands that take a file of a kind that File > Open doesn't take, by the start of the kind's name.
struct TakingCommand
{
    std::string_view kind;
    std::string_view command;
};

constexpr std::array<TakingCommand, 8> kTakingCommands = {{
    {"a sequence file", "File > Import > Sequence…"},
    {"a bank file", "File > Import > Bank…"},
    {"a wave archive file", "File > Import > Wave Archive…"},
    {"a wave sound file", "File > Import > Wave Sound File…"},
    {"a wave file", "File > Import > Wave…"},
    {"a stream file", "File > Import > Stream…"},
    {"a WAV file", "File > Import > Audio File… or Stream…"},
    {"an AAC file", "Replace… on a 3DS stream sound that plays an AAC stream"},
}};

bool StartsWith(std::span<const uint8_t> bytes, std::string_view magic)
{
    return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
}

std::string_view LibraryKindOf(std::string_view letters)
{
    for (const LibraryKind& kind : kLibraryKinds)
    {
        if (kind.letters == letters)
        {
            return kind.name;
        }
    }

    return {};
}

} // namespace

bool IsInputKind(std::span<const uint8_t> bytes, InputKind kind)
{
    switch (kind)
    {
    case InputKind::kMidi:
        return StartsWith(bytes, "MThd");
    case InputKind::kHokuto:
        return IsBinary(bytes) || IsText(bytes);
    case InputKind::kWiiArchive:
        return StartsWith(bytes, "RSAR");
    case InputKind::kLaterArchive:
        return StartsWith(bytes, "CSAR") || StartsWith(bytes, "FSAR");
    case InputKind::kGbaRom:
        return IsGbaRom(bytes);
    case InputKind::kDsArchive:
        return IsDsArchive(bytes);
    case InputKind::kDsImage:
        return IsDsImage(bytes);
    }

    return false;
}

std::optional<InputKind> FirstInputKind(std::span<const uint8_t> bytes, std::span<const InputKind> order)
{
    for (const InputKind kind : order)
    {
        if (IsInputKind(bytes, kind))
        {
            return kind;
        }
    }

    return std::nullopt;
}

bool HoldsDseOnly(std::span<const uint8_t> image)
{
    return DsArchivesIn(image).empty() && HoldsDseFiles(image);
}

std::string KindOfBytes(std::span<const uint8_t> bytes)
{
    if (bytes.empty())
    {
        return "an empty file";
    }
    if (IsBinary(bytes) || IsText(bytes))
    {
        return "a Hokuto file";
    }
    if (StartsWith(bytes, "RIFF") && bytes.size() >= 12 && StartsWith(bytes.subspan(8), "WAVE"))
    {
        return "a WAV file";
    }
    if (StartsWith(bytes, "SARC"))
    {
        return "a SARC archive";
    }
    if (StartsWith(bytes, "MThd"))
    {
        return "a MIDI file";
    }
    if (StartsWith(bytes, "SDAT"))
    {
        return "a sound archive of the DS";
    }

    // An AAC file in ADTS starts with the frame's sync word, 12 bits set, and a layer of 0.
    if (bytes.size() >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xF6) == 0xF0)
    {
        return "an AAC file";
    }

    // A file of the consoles' sound libraries starts with the console's letter, three letters of its kind and a byte
    // order mark. The Wii U's files are big-endian, and the Switch's little-endian.
    if (bytes.size() < 6)
    {
        return {};
    }

    const std::string_view kind = LibraryKindOf(std::string_view(reinterpret_cast<const char*>(&bytes[1]), 3));
    const bool big_endian = bytes[4] == 0xFE && bytes[5] == 0xFF;
    const bool little_endian = bytes[4] == 0xFF && bytes[5] == 0xFE;
    if (kind.empty() || (!big_endian && !little_endian))
    {
        return {};
    }

    switch (bytes[0])
    {
    case 'R':
        return std::string(kind) + " of the Wii";
    case 'C':
        return std::string(kind) + " of the 3DS";
    case 'F':
        return std::string(kind) + (big_endian ? " of the Wii U" : " of the Switch");
    default:
        break;
    }

    return {};
}

std::string KindOfName(const std::filesystem::path& file)
{
    std::string extension = xsflib::Utf8(file.extension());
    std::ranges::transform(extension, extension.begin(),
                           [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    // Hokuto's files, WAV files, AAC files and SARC archives.
    for (const NamedKind& kind : kNamedKinds)
    {
        if (kind.extension == extension)
        {
            return std::string(kind.name);
        }
    }

    // The consoles' files: ".b", a letter for the console and three for the kind.
    if (extension.size() != 6 || !extension.starts_with(".b"))
    {
        return {};
    }

    std::string letters = extension.substr(3);
    std::ranges::transform(letters, letters.begin(),
                           [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    const std::string_view kind = LibraryKindOf(letters);
    if (kind.empty())
    {
        return {};
    }

    switch (extension[2])
    {
    case 'r':
        return std::string(kind) + " of the Wii";
    case 'c':
        return std::string(kind) + " of the 3DS";
    case 'f':
        return std::string(kind) + " of the Wii U or the Switch";
    default:
        break;
    }

    return {};
}

std::string NotKindText(std::span<const uint8_t> bytes, const std::string& wanted)
{
    if (bytes.empty())
    {
        return "It's empty.";
    }

    const std::string kind = KindOfBytes(bytes);
    if (kind.empty() || kind == wanted || kind.starts_with(wanted + " of "))
    {
        return "It isn't " + wanted + ".";
    }

    return "It's " + kind + ", not " + wanted + ".";
}

std::string NotOpenedText(const std::filesystem::path& file, std::span<const uint8_t> bytes, bool import)
{
    // The kind that the bytes give, or else the kind that the name indicates.
    std::string text = (import ? "Can't import " : "Can't open ") + xsflib::Utf8(file.filename()) + ". ";
    const std::string kind = KindOfBytes(bytes);
    const std::string named = KindOfName(file);
    if (bytes.empty())
    {
        text += "It's empty. ";
    }
    else if (!kind.empty())
    {
        text += "It's " + kind + ". ";
    }
    else if (!named.empty())
    {
        text += "Its name ends in " + xsflib::Utf8(file.extension()) + ", but it isn't " + named + ". ";
    }

    // What the command takes, and the command that takes the file's kind. A MIDI file opens by its bytes, whatever its
    // name, and never comes here.
    text += import ? "File > Import > Sound Archive… imports sound archives, GBA ROMs, DS images and MIDI files."
                   : kOpenedKinds;
    for (const TakingCommand& taking : kTakingCommands)
    {
        if (kind == taking.kind || kind.starts_with(std::string(taking.kind) + " of "))
        {
            text += " Use " + std::string(taking.command) + " instead.";
            break;
        }
    }

    return text;
}

std::string AsSentence(std::string text)
{
    if (text.empty() || text.ends_with('.') || text.ends_with('?') || text.ends_with('!'))
    {
        return text;
    }

    text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));

    return text + '.';
}

} // namespace hokuto::format
