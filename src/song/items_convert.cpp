// SPDX-License-Identifier: MIT

// The conversions of the files of the Switch, the Wii U and the 3DS for another of these consoles, and the versions and
// byte orders of their headers (song/items.h).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/wave_file.h"

#include "song/archive.h"
#include "song/items.h"
#include "song/streams.h"

namespace hokuto::song
{
namespace
{

namespace sar = ntdlib::sar;

// The versions of the sequence files that go to each console: the oldest that the console's games hold. The 3DS's games
// hold 1.0.0.0 or 1.1.0.0. The Wii U's games hold 1.0.0 or 2.0.0, laid out the same way. Switch games hold 2.0.0. A
// game's sound library may refuse a file newer than those of the game. The oldest version is therefore the likeliest to
// load in any game.
constexpr uint32_t kCtrSequenceVersion = 0x01000000;
constexpr uint32_t kWiiUSequenceVersion = 0x00010000;
constexpr uint32_t kSwitchSequenceVersion = 0x00020000;

// The versions of the other files that go to each console (ExportVersions): the oldest that the console's games hold.
constexpr uint32_t kCtrBankVersion = 0x01000100;
constexpr uint32_t kCtrWaveArchiveVersion = 0x01000000;
constexpr uint32_t kCtrWaveSoundFileVersion = 0x01000000;
constexpr uint32_t kCtrWaveVersion = 0x02010000;
constexpr uint32_t kBankVersion = 0x00010000;
constexpr uint32_t kWaveArchiveVersion = 0x00010000;
constexpr uint32_t kWiiUWaveSoundFileVersion = 0x00010000;
constexpr uint32_t kSwitchWaveSoundFileVersion = 0x00010100;
constexpr uint32_t kWiiUWaveVersion = 0x00010100;
constexpr uint32_t kSwitchWaveVersion = 0x00010200;

// The versions of the stream files that go to each console. The 3DS's stream files hold 2.0.0.0 to 2.3.1.0, and the
// oldest goes there. Most of the Wii U's games read stream files only up to 3.0.0. The Switch's games read 4.0.0.
constexpr uint32_t kWiiUStreamVersion = 0x00030000;
constexpr uint32_t kSwitchStreamVersion = 0x00040000;

} // namespace

std::string ConsoleName(sar::Flavor flavor)
{
    switch (flavor)
    {
    case sar::Flavor::kCtr:
        return "3DS";
    case sar::Flavor::kCafe:
        return "Wii U";
    case sar::Flavor::kNx:
        break;
    }

    return "Switch";
}

std::string VersionText(sar::Flavor flavor, uint32_t version)
{
    std::string text = flavor == sar::Flavor::kCtr ? std::to_string(version >> 24) + "." : "";
    return text + std::to_string(version >> 16 & 0xFF) + "." + std::to_string(version >> 8 & 0xFF) + "." +
           std::to_string(version & 0xFF);
}

bool VersionsCompare(sar::Flavor a, sar::Flavor b)
{
    return (a == sar::Flavor::kCtr) == (b == sar::Flavor::kCtr);
}

bool HoldsOriginalStart(sar::Flavor flavor, uint32_t version)
{
    return version >= (flavor == sar::Flavor::kCtr ? 0x02010100u : 0x00010200u);
}

std::optional<bool> BigEndianMark(std::span<const uint8_t> file)
{
    constexpr std::size_t kMark = 0x04;
    if (file.size() < kMark + 2)
    {
        return std::nullopt;
    }
    if (file[kMark] == 0xFE && file[kMark + 1] == 0xFF)
    {
        return true;
    }
    if (file[kMark] == 0xFF && file[kMark + 1] == 0xFE)
    {
        return false;
    }

    return std::nullopt;
}

std::optional<uint32_t> HeaderVersion(std::span<const uint8_t> file)
{
    // The header holds the magic, the byte order mark at 0x04 and the version at 0x08.
    constexpr std::size_t kVersion = 0x08;
    if (file.size() < kVersion + 4)
    {
        return std::nullopt;
    }

    const std::optional<bool> big = BigEndianMark(file);
    if (!big)
    {
        return std::nullopt;
    }

    uint32_t version = 0;
    for (std::size_t i = 0; i < 4; i++)
    {
        const uint32_t byte = file[kVersion + (*big ? i : 3 - i)];
        version = version << 8 | byte;
    }

    return version;
}

sar::Flavor OtherConsole(sar::Flavor flavor)
{
    return flavor == sar::Flavor::kCafe ? sar::Flavor::kNx : sar::Flavor::kCafe;
}

std::vector<sar::Flavor> OtherConsoles(sar::Flavor flavor)
{
    std::vector<sar::Flavor> consoles = {OtherConsole(flavor)};
    for (const sar::Flavor console : {sar::Flavor::kCtr, sar::Flavor::kCafe, sar::Flavor::kNx})
    {
        if (console != flavor && console != consoles.front())
        {
            consoles.push_back(console);
        }
    }

    return consoles;
}

namespace
{

// Why console `to` can't hold the commands of `sequence` after its encoder refused them with `error`. Each command that
// the console can't write by itself counts by what it sets (SettingName). The 3DS lacks the Wii U's and the Switch's
// LFOs 1 to 3, for one. Another failure keeps the encoder's words.
std::string WhyNotHeld(const ntdlib::commands::Sequence& sequence, sar::Flavor to, const std::string& error)
{
    using ntdlib::commands::Kind;
    std::map<std::string, std::size_t> lacked;
    std::size_t total = 0;
    for (const ntdlib::commands::Command& command : sequence.items)
    {
        if (command.kind == Kind::kData || ntdlib::commands::HasTarget(command.kind))
        {
            continue;
        }

        ntdlib::commands::Sequence alone;
        alone.items = {command};
        try
        {
            ntdlib::nwsnd::EncodeSequence(alone, GrammarOf(to));
        }
        catch (const std::invalid_argument&)
        {
            lacked[SettingName(command)]++;
            total++;
        }
    }
    if (total == 0)
    {
        return "Can't convert the commands for the " + ConsoleName(to) + ". " + error;
    }

    // Such as "LFOs 1 to 3 (7)", or "LFOs 1 to 3 (2) and time prefixes (1)".
    std::string list;
    std::size_t listed = 0;
    for (const auto& [name, count] : lacked)
    {
        list += listed == 0 ? "" : listed + 1 == lacked.size() ? " and " : ", ";
        list += name + " (" + std::to_string(count) + ")";
        listed++;
    }

    return std::to_string(total) + (total == 1 ? " command in the file isn't" : " commands in the file aren't") +
           " supported on the " + ConsoleName(to) + ": " + list + ".";
}

} // namespace

ConvertedSequence ConvertSequenceFile(std::span<const uint8_t> file, sar::Flavor from, sar::Flavor to,
                                      std::span<const uint32_t> starts)
{
    // The commands as the console reads them, from the labels and the starts.
    sar::SequenceFile read;
    std::vector<ntdlib::nwsnd::LabelOffset> labels;
    ntdlib::nwsnd::Decoded decoded;
    try
    {
        read = sar::SequenceFile::Read(file);
        for (const sar::Sequence::Label& label : read.labels)
        {
            labels.push_back({label.name, label.offset});
        }

        decoded = ntdlib::nwsnd::DecodeSequence(read.data, labels, GrammarOf(from), starts);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(std::string("Can't read the file. ") + error.what());
    }

    if (decoded.opaque)
    {
        throw std::runtime_error("Commands in the file overlap or jump into each other.");
    }

    // The same commands as the target's console writes them. The labels take the offsets of their items.
    const BiquadChanges biquads = MapBiquads(decoded.sequence, decoded.entries, {.engine = format::EngineOf(from)},
                                             {.engine = format::EngineOf(to)});
    ntdlib::nwsnd::Encoded encoded;
    try
    {
        encoded = ntdlib::nwsnd::EncodeSequence(decoded.sequence, GrammarOf(to));
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error(WhyNotHeld(decoded.sequence, to, error.what()));
    }

    sar::SequenceFile converted;
    converted.flavor = to;
    converted.version = ExportVersions(ItemKind::kSequence, to).file;
    converted.data = std::move(encoded.bytes);
    for (const ntdlib::commands::Label& label : decoded.sequence.labels)
    {
        converted.labels.push_back({label.name, encoded.offsets[label.item]});
    }

    return {converted.Write(), biquads};
}

ConvertedSequence OtherConsoleSequenceFile(const sar::SoundArchive& archive, uint32_t sound, sar::Flavor to)
{
    // The sounds that play the file start where their start offsets say, with a label or without one.
    const std::span<const uint8_t> file = ItemFile(archive, {ItemKind::kSequence, sound, 0});
    std::vector<uint32_t> starts = {archive.Sounds()[sound].start_offset};
    for (const uint32_t other : SoundsSharingFile(archive, sound))
    {
        starts.push_back(archive.Sounds()[other].start_offset);
    }

    try
    {
        return ConvertSequenceFile(file, archive.ReadOptions().flavor, to, starts);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't convert the sequence file of " + archive.Sounds()[sound].name + ". " +
                                 error.what());
    }
}

FileVersions ExportVersions(ItemKind kind, sar::Flavor flavor)
{
    const bool ctr = flavor == sar::Flavor::kCtr;
    const bool wii_u = flavor == sar::Flavor::kCafe;
    const uint32_t wave = ctr ? kCtrWaveVersion : wii_u ? kWiiUWaveVersion : kSwitchWaveVersion;
    switch (kind)
    {
    case ItemKind::kSequence:
        return {ctr ? kCtrSequenceVersion : wii_u ? kWiiUSequenceVersion : kSwitchSequenceVersion, 0};
    case ItemKind::kWaveSound:
    case ItemKind::kWaveSoundFile:
        return {ctr ? kCtrWaveSoundFileVersion : wii_u ? kWiiUWaveSoundFileVersion : kSwitchWaveSoundFileVersion, 0};
    case ItemKind::kBank:
        return {ctr ? kCtrBankVersion : kBankVersion, 0};
    case ItemKind::kWaveArchive:
        return {ctr ? kCtrWaveArchiveVersion : kWaveArchiveVersion, wave};
    case ItemKind::kWave:
        break;
    case ItemKind::kStream:
        return {ctr ? kCtrStreamVersion : wii_u ? kWiiUStreamVersion : kSwitchStreamVersion, 0};
    }

    return {wave, 0};
}

std::vector<uint8_t> ConvertItemFile(std::span<const uint8_t> file, ItemKind kind, sar::Flavor from, sar::Flavor to,
                                     const FileVersions& versions)
{
    if (kind == ItemKind::kSequence)
    {
        throw std::invalid_argument("ConvertSequenceFile converts sequence files.");
    }

    // The magic gives the file's kind and its console's letter, and the byte order mark its byte order.
    const std::string format = ItemFormat(kind, from);
    const bool wii_u = from == sar::Flavor::kCafe;
    if (file.size() < 8 || !std::equal(format.begin() + 1, format.end(), file.begin()) || BigEndianMark(file) != wii_u)
    {
        throw std::runtime_error("It isn't a " + ConsoleName(from) + " " + format + " file.");
    }
    if (kind == ItemKind::kStream)
    {
        return ConvertStreamFile(file, to, versions.file);
    }

    // A bank file and a wave sound file keep their layout. A wave and a wave archive are written again.
    try
    {
        if (kind == ItemKind::kWaveArchive)
        {
            return sar::ConvertWaveArchive(sar::WaveArchiveFile::Read(file), to, versions.file, versions.waves).Write();
        }
        if (kind == ItemKind::kWave)
        {
            return sar::ConvertWave(sar::WaveFile::Read(file), to, versions.file).Write();
        }

        return sar::WithConsole(file, to, versions.file);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(error.what());
    }
}

std::vector<uint8_t> OtherConsoleFile(const sar::SoundArchive& archive, const Item& item, sar::Flavor to)
{
    if (item.kind == ItemKind::kSequence)
    {
        return OtherConsoleSequenceFile(archive, item.index, to).file;
    }

    const std::span<const uint8_t> file = ItemFile(archive, item);
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    try
    {
        return ConvertItemFile(file, item.kind, flavor, to, ExportVersions(item.kind, to));
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't convert the file of " + ItemName(archive, item) + ". " + error.what());
    }
}

} // namespace hokuto::song
