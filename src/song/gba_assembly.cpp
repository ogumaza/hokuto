// SPDX-License-Identifier: MIT

#include "song/gba_assembly.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/song.h"
#include "xsflib/text.h"

#include "song/gba_assembly_parts.h"

namespace hokuto::song
{
namespace
{

namespace mp2k = ntdlib::mp2k;
namespace commands = ntdlib::commands;

using gba_assembly_parts::kBytesOnALine;
using gba_assembly_parts::kCentre;
using gba_assembly_parts::kCommandNames;
using gba_assembly_parts::KeyName;
using gba_assembly_parts::kKeys;
using gba_assembly_parts::kMemoryOperations;
using gba_assembly_parts::kModTargets;
using gba_assembly_parts::kPatternBytes;
using gba_assembly_parts::kReverbSet;
using gba_assembly_parts::kVoiceBytes;
using gba_assembly_parts::ReadWord;
using gba_assembly_parts::ThreeDigits;
using gba_assembly_parts::TwoDigits;

// The name of the command with code `code`, or nothing. A wait is W and its length, and a note N and its length.
std::optional<std::string> CodeName(uint8_t code)
{
    if (code >= mp2k::kCmdWait && code <= mp2k::kCmdLastWait)
    {
        return "W" + TwoDigits(mp2k::ClockLength(code - mp2k::kCmdWait));
    }
    if (code > mp2k::kCmdTie)
    {
        return "N" + TwoDigits(mp2k::ClockLength(code - mp2k::kCmdTie));
    }

    const auto found = std::ranges::find(kCommandNames, code, &std::pair<uint8_t, const char*>::first);
    return found != kCommandNames.end() ? std::optional<std::string>(found->second) : std::nullopt;
}

// The symbol of a name: a character other than a letter, a digit or an underscore becomes an underscore.
std::string SymbolOf(std::string_view name)
{
    std::string symbol(name);
    std::ranges::replace_if(
        symbol, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_'; }, '_');
    if (symbol.empty() || std::isdigit(static_cast<unsigned char>(symbol.front())) != 0)
    {
        symbol.insert(0, "song_");
    }

    return symbol;
}

// The tokens of a command's arguments `args`, after a status of code `code`. Each token names the argument's byte,
// whatever command the status of a repeated command turns out to be.
std::vector<std::string> ArgumentTokens(uint8_t code, std::span<const uint8_t> args, const std::string& symbol)
{
    std::vector<std::string> tokens;
    const auto number = [](uint8_t value)
    {
        return std::to_string(int{value});
    };
    for (std::size_t i = 0; i < args.size(); i++)
    {
        const uint8_t value = args[i];
        if (code >= mp2k::kCmdTie && value < 0x80)
        {
            tokens.push_back(i == 0                     ? KeyName(value)
                             : i == 1                   ? "v" + ThreeDigits(value)
                             : value >= 1 && value <= 3 ? "gtp" + number(value)
                                                        : number(value));
        }
        else if (code == mp2k::kCmdEot && i == 0 && value < 0x80)
        {
            tokens.push_back(KeyName(value));
        }
        else if (i != 0)
        {
            tokens.push_back(number(value));
        }
        else if (code == mp2k::kCmdVol)
        {
            tokens.push_back(number(value) + "*" + symbol + "_mvl/mxv");
        }
        else if (code == mp2k::kCmdTempo)
        {
            tokens.push_back(std::to_string(2 * int{value}) + "*" + symbol + "_tbs/2");
        }
        else if (code == mp2k::kCmdKeySh)
        {
            const int shift = static_cast<int8_t>(value);
            tokens.push_back(symbol + "_key" + (shift < 0 ? "" : "+") + std::to_string(shift));
        }
        else if ((code == mp2k::kCmdPan || code == mp2k::kCmdBend || code == mp2k::kCmdTune))
        {
            const int offset = int{value} - kCentre;
            tokens.push_back(std::string("c_v") + (offset < 0 ? "" : "+") + std::to_string(offset));
        }
        else if (code == mp2k::kCmdModT && value < kModTargets.size())
        {
            tokens.emplace_back(kModTargets[value]);
        }
        else if (code == mp2k::kCmdMemAcc && value < kMemoryOperations.size())
        {
            tokens.emplace_back(kMemoryOperations[value]);
        }
        else
        {
            tokens.push_back(number(value));
        }
    }

    return tokens;
}

std::string JoinedLine(std::string_view lead, std::span<const std::string> tokens)
{
    std::string line(lead);
    for (std::size_t i = 0; i < tokens.size(); i++)
    {
        line += (i == 0 ? "" : " , ") + tokens[i];
    }

    return line + "\n";
}

// Lines of `.byte` for the bytes of a run that no track reaches.
void WriteData(std::string& out, std::span<const uint8_t> bytes)
{
    for (std::size_t at = 0; at < bytes.size(); at += kBytesOnALine)
    {
        std::vector<std::string> tokens;
        for (const uint8_t value : bytes.subspan(at, std::min(kBytesOnALine, bytes.size() - at)))
        {
            tokens.push_back(xsflib::Hex(value, 2, true));
        }

        out += JoinedLine("\t.byte\t", tokens);
    }
}

// Whether `code` is one of the commands from which a byte below 0x80 repeats the status.
bool SetsRunningStatus(uint8_t code)
{
    return code >= mp2k::kCmdVoice;
}

} // namespace

namespace gba_assembly_parts
{

// A key's name: key 60 is Cn3, and key 0 is CnM2.
std::string KeyName(int key)
{
    const int octave = key / 12 - 2;
    return std::string(kKeyNames[static_cast<std::size_t>(key % 12)]) +
           (octave < 0 ? "M" + std::to_string(-octave) : std::to_string(octave));
}

std::string TwoDigits(int value)
{
    return (value < 10 ? "0" : "") + std::to_string(value);
}

std::string ThreeDigits(int value)
{
    return (value < 100 ? "0" : "") + TwoDigits(value);
}

} // namespace gba_assembly_parts

namespace
{

// The labels of a song's items, by the items: one for each track where the track starts, and then one for each target,
// under the track that contains the target. `track_at` contains the track that starts at each item, and `target_labels`
// the label of each target.
struct SongLabels
{
    std::map<uint32_t, std::vector<std::string>> labels;
    std::map<uint32_t, int> track_at;
    std::map<uint32_t, std::string> target_labels;
};

SongLabels LabelsOfSong(const mp2k::Decoded& decoded, const std::string& symbol)
{
    const std::vector<commands::Command>& items = decoded.sequence.items;
    std::map<uint32_t, std::vector<std::string>> labels;
    std::map<uint32_t, int> track_at;
    for (std::size_t t = 0; t < decoded.entries.size(); t++)
    {
        labels[decoded.entries[t]].push_back(symbol + "_" + std::to_string(t + 1));
        track_at.emplace(decoded.entries[t], static_cast<int>(t) + 1);
    }

    std::vector<uint32_t> targets;
    for (const commands::Command& item : items)
    {
        if (commands::HasTarget(item.kind) && !item.target_outside)
        {
            targets.push_back(item.target);
        }
    }

    std::ranges::sort(targets);
    targets.erase(std::ranges::unique(targets).begin(), targets.end());

    std::map<int, int> targets_of_track;
    std::map<uint32_t, std::string> target_labels;
    for (const uint32_t target : targets)
    {
        const auto track = track_at.upper_bound(target);
        const int number = track == track_at.begin() ? 0 : std::prev(track)->second;
        const int count = ++targets_of_track[number];
        std::string label = symbol + "_" + std::to_string(number) + "_B" + std::to_string(count);
        target_labels[target] = label;
        labels[target].push_back(std::move(label));
    }

    return {std::move(labels), std::move(track_at), std::move(target_labels)};
}

// The lines that include MPlayDef.s and set the values of the song's header, named after `symbol`.
std::string HeaderEquates(const mp2k::SongHeader& header, const std::string& symbol)
{
    std::string out = "\t.include \"MPlayDef.s\"\n\n";
    const auto equ = [&](const char* part, const std::string& value)
    {
        out += "\t.equ\t" + symbol + "_" + part + ", " + value + "\n";
    };
    equ("grp", "voicegroup_" + xsflib::Hex(header.voices, 8, true).substr(2));
    equ("pri", std::to_string(int{header.priority}));
    equ("rev", (header.reverb & kReverbSet) != 0 ? "reverb_set+" + std::to_string(header.reverb & ~kReverbSet)
                                                 : std::to_string(int{header.reverb}));
    equ("mvl", "127");
    equ("key", "0");
    equ("tbs", "1");
    equ("exg", "0");
    equ("cmp", "1");
    out += "\n\t.section .rodata\n\t.global\t" + symbol + "\n\t.align\t2\n";

    return out;
}

// Adds to `out` the line of the command of `bytes`, and of its address. `running` is the running status, and `label_at`
// contains the label of each address within the song.
void AppendCommand(std::string& out, std::span<const uint8_t> bytes, uint8_t& running, const std::string& symbol,
                   const std::map<uint32_t, std::string>& label_at)
{
    const auto word_token = [&](uint32_t address)
    {
        const auto found = label_at.find(address);
        return found != label_at.end() ? found->second : xsflib::Hex(address, 8, true);
    };

    const bool repeats = bytes[0] < 0x80;
    const uint8_t code = repeats ? running : bytes[0];
    const std::span<const uint8_t> args = repeats ? bytes : bytes.subspan(1);
    running = !repeats && SetsRunningStatus(code) ? code : running;

    // A jump, a call, a repeat and a comparison end in an address.
    const bool has_address =
        !repeats && (((code == mp2k::kCmdGoto || code == mp2k::kCmdPatt) && args.size() == 4) ||
                     (code == mp2k::kCmdRept && args.size() == 5) || (code == mp2k::kCmdMemAcc && args.size() == 7));
    const std::span<const uint8_t> values = has_address ? args.first(args.size() - 4) : args;
    std::vector<std::string> tokens;
    if (!repeats)
    {
        const std::optional<std::string> named = CodeName(code);
        tokens.push_back(named ? *named : xsflib::Hex(code, 2, true));
    }

    const std::vector<std::string> arguments = ArgumentTokens(code, values, symbol);
    tokens.insert(tokens.end(), arguments.begin(), arguments.end());
    const bool waits = !repeats && code >= mp2k::kCmdWait && code <= mp2k::kCmdLastWait;
    out += JoinedLine(
        waits || has_address || code == mp2k::kCmdFine || code == mp2k::kCmdPend ? "\t.byte\t" : "\t.byte\t\t", tokens);
    if (has_address)
    {
        out += "\t .word\t" + word_token(ReadWord(args, args.size() - 4)) + "\n";
    }
}

// The lines of the song's header, after its items.
std::string HeaderLines(const GbaRom& rom, const mp2k::SongHeader& header, const std::string& symbol)
{
    std::string out;
    out += "\n@******************************************************@\n\t.align\t2\n\n" + symbol + ":\n";
    out += "\t.byte\t" + std::to_string(header.tracks.size()) + "\t@ NumTrks\n";
    out += "\t.byte\t" + std::to_string(int{rom.rom.U8(header.address + 1)}) + "\t@ NumBlks\n";
    out += "\t.byte\t" + symbol + "_pri\t@ Priority\n";
    out += "\t.byte\t" + symbol + "_rev\t@ Reverb\n\n";
    out += "\t.word\t" + symbol + "_grp\n\n";
    for (std::size_t t = 0; t < header.tracks.size(); t++)
    {
        out += "\t.word\t" + symbol + "_" + std::to_string(t + 1) + "\n";
    }

    out += "\n\t.end\n";

    return out;
}

// The commands of the song of `header`, named `title`. Throws std::invalid_argument when assembly can't give them.
mp2k::Decoded DecodedForAssembly(const GbaRom& rom, const mp2k::SongHeader& header, const std::string& title)
{
    mp2k::Decoded decoded;
    try
    {
        decoded = mp2k::DecodeSong(rom.rom, header, mp2k::NoteArguments(rom.info));
    }
    catch (const std::invalid_argument&)
    {
        throw std::invalid_argument("Can't export " + title +
                                    " as assembly. Some of its tracks aren't before its header.");
    }

    if (decoded.opaque)
    {
        throw std::invalid_argument("Can't export " + title +
                                    " as assembly. Its commands overlap, or two tracks read them with different "
                                    "values.");
    }

    return decoded;
}

} // namespace

std::string GbaSongAssembly(const GbaRom& rom, int song, std::string_view name)
{
    const std::string title = "Song " + std::to_string(song);
    mp2k::SongHeader header;
    if (!mp2k::ReadSong(rom.rom, rom.info, song, header))
    {
        throw std::invalid_argument("Can't read " + title + ".");
    }

    const mp2k::Decoded decoded = DecodedForAssembly(rom, header, title);

    const uint32_t base = *std::ranges::min_element(header.tracks);
    const mp2k::Encoded encoded = mp2k::EncodeSequence(decoded.sequence, base, mp2k::NoteArguments(rom.info));
    const auto bytes_of = [&](std::size_t item)
    {
        return std::span<const uint8_t>(encoded.bytes)
            .subspan(encoded.offsets[item], encoded.offsets[item + 1] - encoded.offsets[item]);
    };

    // The labels: one for each track where the track starts, and then one for each target, under the track that
    // contains the target.
    const std::string symbol = SymbolOf(name);
    const std::vector<commands::Command>& items = decoded.sequence.items;
    const SongLabels song_labels = LabelsOfSong(decoded, symbol);
    const std::map<uint32_t, std::vector<std::string>>& labels = song_labels.labels;
    const std::map<uint32_t, int>& track_at = song_labels.track_at;

    // The address of an item is written as its label. An address outside the song's bytes stays a number.
    std::map<uint32_t, std::string> label_at;
    for (const auto& [item, label] : song_labels.target_labels)
    {
        label_at[base + encoded.offsets[item]] = label;
    }

    std::string out = HeaderEquates(header, symbol);

    // The padding before the header stays out where `.align` restores it.
    std::size_t last = items.size();
    if (last > 0 && items[last - 1].kind == commands::Kind::kData &&
        !labels.contains(static_cast<uint32_t>(last - 1)) && bytes_of(last - 1).size() < 4 &&
        std::ranges::all_of(bytes_of(last - 1), [](uint8_t value) { return value == 0; }) &&
        encoded.bytes.size() % 4 == 0)
    {
        last--;
    }

    uint8_t running = 0;
    for (std::size_t i = 0; i < last; i++)
    {
        const std::span<const uint8_t> bytes = bytes_of(i);
        if (const auto track = track_at.find(static_cast<uint32_t>(i)); track != track_at.end())
        {
            out += "\n@********************** Track " + std::to_string(track->second) + " **********************@\n\n";
        }

        if (const auto here = labels.find(static_cast<uint32_t>(i)); here != labels.end())
        {
            for (const std::string& label : here->second)
            {
                out += label + ":\n";
            }
        }

        if (items[i].kind == commands::Kind::kData)
        {
            WriteData(out, bytes);
            continue;
        }

        if (bytes.empty())
        {
            continue;
        }

        AppendCommand(out, bytes, running, symbol, label_at);
    }

    out += HeaderLines(rom, header, symbol);

    return out;
}

std::string GbaSampleName(uint32_t address)
{
    return "DirectSoundWaveData_" + xsflib::Hex(address, 8, true).substr(2);
}

namespace
{

// The tables that the group's voices name, in the order of the voices, and their key tables and patterns. A table that
// two voices name gets the size of the larger.
struct GroupTables
{
    std::map<uint32_t, std::size_t> sizes;
    std::vector<uint32_t> tables;
    std::vector<uint32_t> key_tables;
    std::vector<uint32_t> patterns;
};

GroupTables TablesOfGroup(const GbaRom& rom, const GbaVoiceGroup& group, std::size_t programs)
{
    std::map<uint32_t, std::size_t> sizes;
    std::vector<uint32_t> tables;
    std::vector<uint32_t> key_tables;
    std::vector<uint32_t> patterns;
    const auto add_once = [](std::vector<uint32_t>& list, uint32_t address)
    {
        if (std::ranges::find(list, address) == list.end())
        {
            list.push_back(address);
        }
    };
    const auto note_pattern = [&](const mp2k::Voice& voice)
    {
        if (voice.PsgChannel() == 3 && !voice.IsSplit() && rom.rom.Contains(voice.wave, kPatternBytes))
        {
            add_once(patterns, voice.wave);
        }
    };

    for (std::size_t program = 0; program < programs; program++)
    {
        mp2k::Voice voice;
        if (!mp2k::ReadVoice(rom.rom, group.address + static_cast<uint32_t>(program * kVoiceBytes), voice))
        {
            throw std::invalid_argument("The voice group table runs past the end of the ROM.");
        }

        note_pattern(voice);
        if (!voice.IsSplit())
        {
            continue;
        }

        std::size_t entries = kKeys;
        if ((voice.type & mp2k::kVoiceKeySplit) != 0)
        {
            entries = 0;
            if (rom.rom.Contains(voice.key_table, kKeys))
            {
                add_once(key_tables, voice.key_table);
                for (uint32_t key = 0; key < kKeys; key++)
                {
                    entries = std::max<std::size_t>(entries, std::size_t{rom.rom.U8(voice.key_table + key)} + 1);
                }
            }
        }
        while (entries > 0 && !rom.rom.Contains(voice.wave, static_cast<uint32_t>(entries * kVoiceBytes)))
        {
            entries--;
        }
        // A table at the group's address would give a second label of the same name. Its word stays a number.
        if (entries > 0 && voice.wave != group.address)
        {
            add_once(tables, voice.wave);
            sizes[voice.wave] = std::max(sizes[voice.wave], entries);
        }
    }

    return {std::move(sizes), std::move(tables), std::move(key_tables), std::move(patterns)};
}

// The labels of a voice table, a key table and a pattern at `address`.
std::string TableName(uint32_t address)
{
    return "voicegroup_" + xsflib::Hex(address, 8, true).substr(2);
}

std::string KeyTableName(uint32_t address)
{
    return "KeySplitTable_" + xsflib::Hex(address, 8, true).substr(2);
}

std::string PatternName(uint32_t address)
{
    return "ProgrammableWaveData_" + xsflib::Hex(address, 8, true).substr(2);
}

// A voice's 12 bytes as the ROM stores them. A word that names a table, a pattern or a sample is written as its name. A
// `nested` voice stands in a table of a key split or a drum kit.
std::string VoiceLines(const GbaRom& rom, uint32_t address, bool nested, const GroupTables& group_tables)
{
    const std::map<uint32_t, std::size_t>& sizes = group_tables.sizes;
    const std::vector<uint32_t>& key_tables = group_tables.key_tables;
    const std::vector<uint32_t>& patterns = group_tables.patterns;
    std::string out;
    mp2k::Voice voice;
    mp2k::ReadVoice(rom.rom, address, voice);
    std::array<std::string, 4> head;
    for (uint32_t i = 0; i < head.size(); i++)
    {
        head[i] = xsflib::Hex(rom.rom.U8(address + i), 2, true);
    }

    out += JoinedLine("\t.byte\t", head);

    mp2k::Wave wave;
    std::string word = xsflib::Hex(voice.wave, 8, true);
    if (voice.IsSplit() && !nested && sizes.contains(voice.wave))
    {
        word = TableName(voice.wave);
    }
    else if (!nested && !voice.IsSplit() && voice.PsgChannel() == 3 &&
             std::ranges::find(patterns, voice.wave) != patterns.end())
    {
        word = PatternName(voice.wave);
    }
    else if (!voice.IsSplit() && voice.PsgChannel() == 0 && mp2k::ReadWave(rom.rom, voice.wave, wave))
    {
        word = GbaSampleName(voice.wave);
    }

    out += "\t.4byte\t" + word + "\n";

    if ((voice.type & mp2k::kVoiceKeySplit) != 0 && !nested &&
        std::ranges::find(key_tables, voice.key_table) != key_tables.end())
    {
        out += "\t.4byte\t" + KeyTableName(voice.key_table) + "\n";
        return out;
    }

    std::array<std::string, 4> tail;
    for (uint32_t i = 0; i < tail.size(); i++)
    {
        tail[i] = xsflib::Hex(rom.rom.U8(address + 8 + i), 2, true);
    }

    out += JoinedLine("\t.byte\t", tail);

    return out;
}

} // namespace

std::string GbaVoiceGroupAssembly(const GbaRom& rom, const GbaVoiceGroup& group)
{
    const std::vector<int> listed = GbaListedPrograms(rom, group);
    const std::size_t programs = listed.empty() ? 1 : static_cast<std::size_t>(listed.back()) + 1;

    GroupTables group_tables = TablesOfGroup(rom, group, programs);
    std::map<uint32_t, std::size_t>& sizes = group_tables.sizes;
    const std::vector<uint32_t>& tables = group_tables.tables;
    const std::vector<uint32_t>& key_tables = group_tables.key_tables;
    const std::vector<uint32_t>& patterns = group_tables.patterns;

    std::string out;

    // A pattern in a table of a key split or a drum kit stands as a number: only the group's voices name labels.
    const std::string main = TableName(group.address);
    out += "@ The voice group at " + xsflib::Hex(group.address, 8, true) + ". Each voice takes 12 bytes.\n\n";
    out += "\t.section .rodata\n\t.global\t" + main + "\n\t.align\t2\n\n" + main + ":\n";
    for (std::size_t program = 0; program < programs; program++)
    {
        const auto address = group.address + static_cast<uint32_t>(program * kVoiceBytes);
        mp2k::Voice voice;
        mp2k::ReadVoice(rom.rom, address, voice);
        out += "@ Program " + std::to_string(program) + ": " + GbaVoiceKind(voice) + "\n";
        out += VoiceLines(rom, address, false, group_tables);
    }

    for (const uint32_t table : tables)
    {
        out += "\n" + TableName(table) + ":\n";
        for (std::size_t entry = 0; entry < sizes[table]; entry++)
        {
            out += "@ Entry " + std::to_string(entry) + "\n";
            out += VoiceLines(rom, table + static_cast<uint32_t>(entry * kVoiceBytes), true, group_tables);
        }
    }

    for (const uint32_t table : key_tables)
    {
        out += "\n" + KeyTableName(table) + ":\n";
        std::vector<uint8_t> bytes;
        for (uint32_t key = 0; key < kKeys; key++)
        {
            bytes.push_back(rom.rom.U8(table + key));
        }

        WriteData(out, bytes);
    }

    for (const uint32_t pattern : patterns)
    {
        out += "\n" + PatternName(pattern) + ":\n";
        std::vector<uint8_t> bytes;
        for (uint32_t i = 0; i < kPatternBytes; i++)
        {
            bytes.push_back(rom.rom.U8(pattern + i));
        }

        WriteData(out, bytes);
    }

    out += "\n\t.end\n";

    return out;
}

} // namespace hokuto::song
