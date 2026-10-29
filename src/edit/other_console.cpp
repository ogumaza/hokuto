// SPDX-License-Identifier: MIT

#include "edit/other_console.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/ctr/nnsnd/dsp_abi.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/voice.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/units.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"
#include "xsflib/binary.h"

#include "edit/bytecode.h"
#include "edit/effects.h"
#include "edit/files.h"
#include "edit/new_items.h"
#include "edit/replace.h"
#include "edit/tables.h"
#include "edit/timelines.h"
#include "edit/variables.h"
#include "edit/waves.h"
#include "edit/wii_programs.h"
#include "format/archive.h"
#include "format/codes.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/shared.h"
#include "song/archive.h"
#include "song/biquads.h"
#include "song/items.h"
#include "song/sequence.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;

static_assert(kCtrWaveMemory == ntdlib::ctr::snd::kArchiveModeFcram);
static_assert(kWiiWaveMemory == ntdlib::dsp::kMem2Size);

// The values of a sound in the archive's tables that the new sound takes through the Sound sections' edits. The new
// sound plays on the new file's player. Its bank slots name the new banks.
constexpr std::array kCopiedValues = {
    SoundValue::kVolume,      SoundValue::kPanMode, SoundValue::kPanCurve,        SoundValue::kPlayerPriority,
    SoundValue::kActorPlayer, SoundValue::kTracks,  SoundValue::kChannelPriority, SoundValue::kReleasePriorityFix};

// `names` in a sentence, such as "A, B and C".
std::string ListOf(std::span<const std::string> names)
{
    std::string list;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        list += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
    }

    return list;
}

// Whether `command` plays otherwise on console `to` than on console `from`: a command that the player of either side
// skips (song::Runs), or one that does nothing on `from` and that `to` reads as part of another command. The 3DS and
// the Wii read 0xF0 with 0xA0 to 0xBF without a byte, and the Wii U and the Switch read the settings of LFOs 1 to 3
// there. The Wii's builds before March 2008 read 0xA3 to 0xA5 as commands that do nothing. The later consoles read time
// prefixes there.
bool PlaysOtherwise(const ntdlib::commands::Command& command, const song::Console& from, const song::Console& to)
{
    if (!song::Runs(from, command.kind, command.index) || !song::Runs(to, command.kind, command.index))
    {
        return true;
    }
    if (command.kind != ntdlib::commands::Kind::kOther)
    {
        return false;
    }

    const ntdlib::nwsnd::Grammar read = song::GrammarOf(from);
    const ntdlib::nwsnd::Grammar written = song::GrammarOf(to);
    const uint16_t opcode = command.encoding.opcode;
    return (!read.extra_lfo_bytes && written.extra_lfo_bytes && opcode >= 0xF0A0 && opcode <= 0xF0BF) ||
           (!read.time_prefixes && written.time_prefixes && opcode >= 0xA3 && opcode <= 0xA5);
}

// The warning about `counts`, the commands of each kind that play otherwise on console `to` than on console `from`
// (PlaysOtherwise), or nothing. With `left_out`, the new file leaves them out. Otherwise they stay in their places.
std::optional<std::string> SkippedWarning(const std::map<std::string, std::size_t>& counts, const song::Console& from,
                                          const song::Console& to, bool left_out)
{
    std::size_t total = 0;
    std::vector<std::string> kinds;
    for (const auto& [name, count] : counts)
    {
        total += count;
        kinds.push_back(std::to_string(count) + " of " + name);
    }
    if (total == 0)
    {
        return std::nullopt;
    }

    const bool one = total == 1;
    const std::string commands = std::to_string(total) + " of the sequence's commands";
    std::string warning = left_out ? "The new file leaves out " + commands + ": "
                                   : commands + (one ? " stays in its place: " : " stay in their places: ");
    warning += ListOf(kinds) + ".";
    if (from.wii)
    {
        return warning + (one ? " The Wii's player skipped it." : " The Wii's player skipped them.");
    }
    if (to.wii)
    {
        return warning + (one ? " The Wii's player skips it." : " The Wii's player skips them.");
    }
    if (to.flavor == sar::Flavor::kCtr)
    {
        return warning + (one ? " The 3DS's player skips it." : " The 3DS's player skips them.");
    }

    return warning + (one ? " The 3DS's player skipped it. The " : " The 3DS's player skipped them. The ") +
           song::ConsoleName(to.flavor) + (left_out ? "'s player would run " : "'s player runs ") +
           (one ? "it." : "them.");
}

// The commands of `sequence` that play otherwise on console `to` than on console `from` (PlaysOtherwise), by what they
// set.
std::map<std::string, std::size_t> CountSkipped(const ntdlib::commands::Sequence& sequence, const song::Console& from,
                                                const song::Console& to)
{
    std::map<std::string, std::size_t> counts;
    for (const ntdlib::commands::Command& command : sequence.items)
    {
        if (PlaysOtherwise(command, from, to))
        {
            counts[song::SettingName(command)]++;
        }
    }

    return counts;
}

// `sequence` without the items that `leaves` marks, with item `start` where the sound starts. The targets, the labels
// and the start move to the items that stay. Code that entered at an item that leaves enters at the next item that
// stays, or at the end.
std::pair<ntdlib::commands::Sequence, uint32_t> WithoutItems(const ntdlib::commands::Sequence& sequence, uint32_t start,
                                                             const std::vector<bool>& leaves)
{
    std::vector<uint32_t> moved(sequence.items.size() + 1);
    uint32_t kept = 0;
    for (std::size_t item = 0; item <= sequence.items.size(); ++item)
    {
        moved[item] = kept;
        kept += item < sequence.items.size() && leaves[item] ? 0 : 1;
    }

    ntdlib::commands::Sequence result;
    for (std::size_t item = 0; item < sequence.items.size(); ++item)
    {
        if (leaves[item])
        {
            continue;
        }

        ntdlib::commands::Command& command = result.items.emplace_back(sequence.items[item]);
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target = moved.at(command.target);
        }
    }
    for (ntdlib::commands::Label label : sequence.labels)
    {
        label.item = moved.at(label.item);
        result.labels.push_back(std::move(label));
    }

    return {std::move(result), moved.at(start)};
}

// `sequence` without the commands that play otherwise on console `to` than on console `from` (PlaysOtherwise), with
// item `start` where the sound starts (WithoutItems).
std::pair<ntdlib::commands::Sequence, uint32_t> WithoutSkipped(const ntdlib::commands::Sequence& sequence,
                                                               uint32_t start, const song::Console& from,
                                                               const song::Console& to)
{
    std::vector<bool> leaves(sequence.items.size());
    for (std::size_t item = 0; item < sequence.items.size(); ++item)
    {
        leaves[item] = PlaysOtherwise(sequence.items[item], from, to);
    }

    return WithoutItems(sequence, start, leaves);
}

// The 3DS's player sets volume2 and the main volume at once, and their time only takes its bytes
// (ntdlib::nwsnd::CtrPolicy). The players of the Wii U and the Switch move such a value over the time. Takes the times
// of those settings out of `sequence`, a sequence of the 3DS, and gives how many went.
std::size_t WithoutCtrTimes(ntdlib::commands::Sequence& sequence)
{
    using ntdlib::commands::Kind;
    std::size_t times = 0;
    for (ntdlib::commands::Command& command : sequence.items)
    {
        if (command.time && (command.kind == Kind::kVolume2 || command.kind == Kind::kMainVolume))
        {
            command.time.reset();
            ++times;
        }
    }

    return times;
}

// The warning about `times` settings of volume2 and the main volume of a sequence of the 3DS that go to the console of
// `to`: settings that lost their times (WithoutCtrTimes), or that keep them in a sequence whose commands keep their
// places.
std::string CtrTimesWarning(std::size_t times, sar::Flavor to, bool lost)
{
    const bool one = times == 1;
    const std::string count = std::to_string(times);
    const std::string console = song::ConsoleName(to);
    if (lost)
    {
        return "The 3DS sets volume2 and the main volume at once. " +
               (one ? "The time of 1 such setting stays behind. The " + console + " sets that setting at once too."
                    : "The times of " + count + " such settings stay behind. The " + console +
                          " sets those settings at once too.");
    }

    return "The 3DS sets volume2 and the main volume at once. " +
           (one ? "1 such setting keeps its time: the file's commands keep their places. The " + console +
                      " moves that setting over the time."
                : count + " such settings keep their times: the file's commands keep their places. The " + console +
                      " moves those settings over the times.");
}

// Whether the console of `to` reads each command of `sequence` with the length that the console of `from` reads. The
// encoder refuses a sequence whose bytes keep their places when a command's length changes.
bool SameLengths(const ntdlib::commands::Sequence& sequence, sar::Flavor from, sar::Flavor to)
{
    try
    {
        return Offsets(sequence, song::Console{.flavor = from}) == Offsets(sequence, song::Console{.flavor = to});
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
}

// A file that goes in: its name and its bytes for the other console. A wave archive also gives the latest version among
// the waves that it held, and its waves in 8-bit PCM by their indices.
struct Converted
{
    std::string name;
    IncomingFile incoming;
    std::optional<uint32_t> held_waves;
    std::vector<uint32_t> pcm8;
    std::optional<std::size_t> mono_bytes; // the file's bytes with its stereo waves as mono
};

// A note of a stereo wave: its key, its velocity and its track's pan at its start, from -1 to 1.
struct NotePan
{
    uint8_t key = 60;
    uint8_t velocity = 127;
    float pan = 0.0f;
};

// The level of a stereo wave as mono on the 3DS (MonoLevelOf), and the gain that its samples take. The regions that
// play it take the rest of their gains (GainAt).
struct MonoGains
{
    MonoLevel level;
    double samples = 1.0;
};

// The stereo waves of the wave archives that go in as mono (MonoWaveOf), and what they changed.
struct Mono
{
    bool on = false;
    uint32_t pan_mode = 0; // the sound's pan mode and pan curve
    ntdlib::nwsnd::PanInfo pan_info;

    // The notes of stereo waves, by the item ID of their bank in the source archive and their programs.
    std::map<std::pair<uint32_t, uint32_t>, std::vector<NotePan>> notes;

    // The stereo waves, by the wave archive's item ID in the source archive and their indices, with each one's gains
    // once a region or the wave archive uses them (GainsOf). The wave archives that hold them, as files of the 3DS with
    // their waves in stereo, by the same IDs.
    std::map<uint32_t, std::map<uint32_t, std::optional<MonoGains>>> waves;
    std::map<uint32_t, sar::WaveArchiveFile> sources;
    std::size_t count = 0;   // the waves that went in as mono
    std::size_t regions = 0; // the regions that play them
    double least_db = 0.0;   // the least and the most of the regions' gains, in dB
    double most_db = 0.0;
    std::optional<double> lowest_ratio; // the lowest ratio of signal to noise of those in DSP-ADPCM, in dB
    std::size_t held = 0;               // the regions whose volumes stay at 255, the highest
};

// The powers of a stereo wave of `level` at pan `pan` on the 3DS (Voice::CalcMixParam): as its two channels, and as
// their mean at gain 1. In the dual mode, the first channel plays at the pan less 1 and the second at the pan plus 1.
// In the balance mode, each channel plays on its side at the pan's gain for that side. The mean plays at the pan. A
// pan's gain for the left side is the pan curve's at the pan, and for the right side the curve's at its negation.
std::pair<double, double> PowersAt(const Mono& mono, const MonoLevel& level, float pan)
{
    const auto ratio = [&](float at)
    {
        return static_cast<double>(ntdlib::nwsnd::CalcPanRatio<ntdlib::nwsnd::CtrPolicy>(
            at, mono.pan_info, ntdlib::nwsnd::OutputMode::kStereo));
    };

    double first_left = 0.0;
    double first_right = 0.0;
    double second_left = 0.0;
    double second_right = 0.0;
    if (mono.pan_mode == 1)
    {
        first_left = ratio(pan);
        second_right = ratio(0.0f - pan);
    }
    else
    {
        first_left = ratio(pan - 1.0f);
        first_right = ratio(0.0f - (pan - 1.0f));
        second_left = ratio(pan + 1.0f);
        second_right = ratio(0.0f - (pan + 1.0f));
    }

    const double stereo = level.left * (first_left * first_left + first_right * first_right) +
                          level.right * (second_left * second_left + second_right * second_right) +
                          2.0 * level.both * (first_left * second_left + first_right * second_right);
    const double mean = (level.left + level.right + 2.0 * level.both) * 0.25;
    const double left = ratio(pan);
    const double right = ratio(0.0f - pan);
    return {stereo, mean * (left * left + right * right)};
}

// The gain that gives the mean of a stereo wave of `level` the power of its two channels, over notes at `pans`: the
// square root of the ratio of the two powers, each summed over the notes (PowersAt). It stops at 8. Silence gives 1.
double GainAt(const Mono& mono, const MonoLevel& level, std::span<const float> pans)
{
    double stereo = 0.0;
    double mean = 0.0;
    for (const float pan : pans)
    {
        const auto [two, one] = PowersAt(mono, level, pan);
        stereo += two;
        mean += one;
    }

    return mean > 0.0 ? std::min(std::sqrt(stereo / mean), 8.0) : 1.0;
}

// The gains of stereo wave `index` of wave archive `id` of `mono`. The samples take as much of the gain of a note in
// the centre as their peak allows.
const MonoGains& GainsOf(Mono& mono, uint32_t id, uint32_t index)
{
    std::optional<MonoGains>& gains = mono.waves.at(id).at(index);
    if (!gains)
    {
        const MonoLevel level = MonoLevelOf(sar::WaveFile::Read(mono.sources.at(id).waves.at(index)));
        const float kCentre = 0.0f;
        const double gain = std::max(GainAt(mono, level, {&kCentre, 1}), 1.0);
        const double samples = level.peak > 0 ? std::clamp(32767.0 / level.peak, 1.0, gain) : gain;
        gains = MonoGains{level, samples};
    }

    return *gains;
}

// The song as it takes shape: the new file and the warnings, and the wave archives and banks that went in, by the
// source archive's item IDs. A wave archive maps to its place in the new archive's list, and a bank to its new item ID,
// or to kNoItem when it stays behind.
struct Song
{
    OtherConsoleSong result;
    std::map<uint32_t, uint32_t> wave_archives;
    std::map<uint32_t, uint32_t> banks;
    std::vector<std::string> older_waves;    // the wave archives whose waves lost the original starts of their loops
    std::size_t wave_bytes = 0;              // what the new wave archives take in the memory of the 3DS's model
    std::map<uint32_t, Converted> converted; // wave archives converted before they go in, by their item IDs
    Mono mono;
};

// Wave archive `id` of `archive` as a file of `to`. With `song.mono.on`, its stereo waves go in as mono, and `song`
// keeps them. Throws std::runtime_error and std::invalid_argument with a message for the user for a wave archive that
// the archive lacks, and for a file that can't be converted.
Converted ConvertWaveArchive(Song& song, const sar::SoundArchive& archive, uint32_t id, sar::Flavor to)
{
    const std::optional<std::size_t> index = sar::ItemIndex(id, sar::kWaveArchiveItem, archive.WaveArchives().size());
    if (!index)
    {
        throw std::invalid_argument("The archive has no wave archive " + std::to_string(sar::ItemNumber(id)) + ".");
    }

    const std::span<const uint8_t> file =
        song::ItemFile(archive, {.kind = song::ItemKind::kWaveArchive, .index = static_cast<uint32_t>(*index)});
    Converted converted{
        archive.WaveArchives()[*index].name, ReadIncomingFile(file, FileKind::kWaveArchive, to), {}, {}, {}};
    const std::vector<std::vector<uint8_t>> waves = sar::WaveArchiveFile::Read(file).waves;
    for (uint32_t w = 0; w < waves.size(); ++w)
    {
        const sar::WaveFile wave = sar::WaveFile::Read(waves[w]);
        converted.held_waves = std::max(converted.held_waves.value_or(0), wave.version);
        if (wave.encoding == sar::WaveEncoding::kPcm8)
        {
            converted.pcm8.push_back(w);
        }
    }
    if (!song.mono.on)
    {
        return converted;
    }

    // The stereo waves, and the bytes of the file with them as mono. They go in as mono once the trim has left out the
    // waves that the sound doesn't play (MonoWaveArchive).
    sar::WaveArchiveFile made = sar::WaveArchiveFile::Read(converted.incoming.bytes);
    sar::WaveArchiveFile whole = made;
    std::map<uint32_t, std::optional<MonoGains>>& stereo = song.mono.waves[id];
    for (uint32_t w = 0; w < made.waves.size(); ++w)
    {
        const sar::WaveFile wave = sar::WaveFile::Read(made.waves[w]);
        if (wave.channels.size() == 2)
        {
            stereo[w] = std::nullopt;
            made.waves[w].assign(MonoWaveBytes(wave), 0);
        }
    }
    converted.mono_bytes = made.Write().size();
    if (!stereo.empty())
    {
        song.mono.sources.insert_or_assign(id, std::move(whole));
    }

    return converted;
}

// `bytes`, wave archive `id` of the source archive as a file of the 3DS, with each stereo wave as mono (MonoWaveOf).
// The samples take their gain (GainsOf). `kept` gives the source's index of each wave of a wave archive that holds only
// some of them, or null for one that holds them all. `mono` counts the waves, and keeps the lowest ratio of signal to
// noise.
std::vector<uint8_t> MonoWaveArchive(std::span<const uint8_t> bytes, Mono& mono, uint32_t id,
                                     const std::vector<uint32_t>* kept)
{
    sar::WaveArchiveFile made = sar::WaveArchiveFile::Read(bytes);
    for (uint32_t w = 0; w < made.waves.size(); ++w)
    {
        const sar::WaveFile wave = sar::WaveFile::Read(made.waves[w]);
        if (wave.channels.size() != 2)
        {
            continue;
        }

        const MonoWave one = MonoWaveOf(wave, GainsOf(mono, id, kept != nullptr ? kept->at(w) : w).samples);
        made.waves[w] = one.wave.Write();
        mono.count++;
        if (one.signal_to_noise)
        {
            mono.lowest_ratio = std::min(mono.lowest_ratio.value_or(*one.signal_to_noise), *one.signal_to_noise);
        }
    }

    return made.Write();
}

// The bytes that a wave archive of `size` bytes takes in the memory of the 3DS's model. The model starts each wave
// archive at a multiple of 32 bytes.
std::size_t StoredSize(std::size_t size)
{
    return (size + 31) / 32 * 32;
}

// `bytes` in words, such as "12.3 MB".
std::string SizeText(std::size_t bytes)
{
    std::array<char, 32> text{};
    if (bytes < 1024)
    {
        std::snprintf(text.data(), text.size(), "%zu bytes", bytes);
    }
    else if (bytes < 1024 * 1024)
    {
        std::snprintf(text.data(), text.size(), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    }
    else
    {
        std::snprintf(text.data(), text.size(), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }

    return text.data();
}

// The instruments and waves that go to the 3DS when the wave archives that a sound's banks name don't fit in the memory
// of the 3DS's model together: the instruments of `programs` in each bank, and the waves of those instruments in each
// wave archive, by their indices in order. `whole` gives the bytes that the wave archives would take whole.
struct Trim
{
    std::set<uint32_t> programs;
    std::map<uint32_t, std::vector<uint32_t>> waves; // by the wave archive's item ID in the source archive
    std::size_t whole = 0;
};

// The places in the table of waves of `bank` that the regions of its instruments of `programs` name.
std::set<uint32_t> TableEntriesOf(const sar::BankFile& bank, const std::set<uint32_t>& programs)
{
    std::set<uint32_t> entries;
    for (const uint32_t program : programs)
    {
        if (program >= bank.instruments.size() || !bank.instruments[program])
        {
            continue;
        }

        for (const sar::BankFile::KeyRegion& key_region : bank.instruments[program]->key_regions)
        {
            for (const sar::BankFile::Region& region : key_region.regions)
            {
                if (region.values.wave_id_index < bank.waves.size())
                {
                    entries.insert(region.values.wave_id_index);
                }
            }
        }
    }

    return entries;
}

// The trim of banks `banks` of `archive` for a sound that plays `sequence` from item `start` on the 3DS, or nothing
// when their wave archives fit in `memory` together as files of the 3DS. The instruments of program 0 and of the
// programs that the sound's program changes select stay, in every bank. The wave archives go to the 3DS first, and
// `song` keeps them. A bank or a wave archive that can't come along takes no memory: it stays behind later. Throws
// std::invalid_argument with a message for the user for wave archives that don't fit, when a variable or a random
// number gives a program, or ntdlib can't read the values of a bank.
std::optional<Trim> TrimFor(Song& song, const sar::SoundArchive& archive, std::span<const uint32_t> banks,
                            const ntdlib::commands::Sequence& sequence, uint32_t start, std::size_t memory)
{
    // The banks' files and the wave archives that they name, each once.
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    std::vector<std::pair<std::string, std::span<const uint8_t>>> files;
    std::set<uint32_t> named;
    for (const uint32_t bank : std::set<uint32_t>(banks.begin(), banks.end()))
    {
        const std::optional<std::size_t> index = sar::ItemIndex(bank, sar::kBankItem, archive.Banks().size());
        if (!index)
        {
            continue;
        }

        try
        {
            const std::span<const uint8_t> file =
                song::ItemFile(archive, {.kind = song::ItemKind::kBank, .index = static_cast<uint32_t>(*index)});
            for (const NamedWaveArchive& wave_archive : WaveArchivesNamed(file, flavor))
            {
                named.insert(wave_archive.id);
            }
            const std::string& name = archive.Banks()[*index].name;
            files.emplace_back(name.empty() ? std::to_string(sar::ItemNumber(bank)) : name, file);
        }
        catch (const std::exception&)
        {
            // The bank stays behind.
        }
    }
    Trim trim;
    std::map<uint32_t, std::size_t> counts;
    for (const uint32_t id : named)
    {
        const std::optional<std::size_t> index =
            sar::ItemIndex(id, sar::kWaveArchiveItem, archive.WaveArchives().size());
        if (!index)
        {
            continue;
        }

        try
        {
            Converted converted = ConvertWaveArchive(song, archive, id, sar::Flavor::kCtr);
            trim.whole += StoredSize(converted.mono_bytes.value_or(converted.incoming.bytes.size()));
            counts[id] = song::WaveCountOf(archive, *index);
            song.converted.emplace(id, std::move(converted));
        }
        catch (const std::exception&)
        {
            // The wave archive stays behind with its banks.
        }
    }
    if (trim.whole <= memory)
    {
        return std::nullopt;
    }

    // The programs, and the waves of their instruments.
    const std::string too_large = "The wave archives of the sound's banks take " + SizeText(trim.whole) +
                                  ". Hokuto's model of the 3DS holds " + SizeText(memory) + " of wave archives. ";
    const Selections selections = ProgramsSelected(sequence, start);
    if (selections.undecided > 0)
    {
        throw std::invalid_argument(too_large +
                                    "A variable or a random number decides some of the sound's programs. Hokuto can't "
                                    "tell which instruments the sound leaves unplayed.");
    }
    trim.programs.insert(0);
    for (const Program& program : selections.programs)
    {
        trim.programs.insert(static_cast<uint32_t>(program.number));
    }
    std::map<uint32_t, std::set<uint32_t>> waves;
    for (const auto& [name, file] : files)
    {
        sar::BankFile bank;
        try
        {
            bank = sar::BankFile::Read(file);
        }
        catch (const std::exception&)
        {
            throw std::invalid_argument(too_large + "Hokuto can't read the values of bank " + name +
                                        ", and can't leave out its other instruments.");
        }
        for (const uint32_t entry : TableEntriesOf(bank, trim.programs))
        {
            const sar::WaveId& id = bank.waves[entry];
            const auto count = counts.find(id.wave_archive_item);
            if (count != counts.end() && id.index < count->second)
            {
                waves[id.wave_archive_item].insert(id.index);
            }
        }
    }
    for (const auto& [id, indices] : waves)
    {
        trim.waves[id].assign(indices.begin(), indices.end());
    }

    return trim;
}

// `bytes`, a wave archive file, with only its waves at `kept`, in their order.
std::vector<uint8_t> TrimmedWaveArchive(std::span<const uint8_t> bytes, std::span<const uint32_t> kept)
{
    sar::WaveArchiveFile file = sar::WaveArchiveFile::Read(bytes);
    std::vector<std::vector<uint8_t>> waves;
    for (const uint32_t index : kept)
    {
        waves.push_back(std::move(file.waves.at(index)));
    }
    file.waves = std::move(waves);

    return file.Write();
}

// `bytes`, a bank file, with only the instruments of the programs of `trim`. Its table of waves holds only the waves
// that those instruments play, in their order, at their places in the wave archives of `trim`. A wave that `trim`
// leaves out of its wave archive plays nothing. It played nothing before either.
std::vector<uint8_t> TrimmedBank(std::span<const uint8_t> bytes, const Trim& trim)
{
    sar::BankFile bank = sar::BankFile::Read(bytes);
    for (uint32_t program = 0; program < bank.instruments.size(); ++program)
    {
        if (!trim.programs.contains(program))
        {
            bank.instruments[program].reset();
        }
    }
    while (!bank.instruments.empty() && !bank.instruments.back())
    {
        bank.instruments.pop_back();
    }
    std::erase_if(bank.order, [&trim](uint32_t program) { return !trim.programs.contains(program); });

    // The table of waves, and the places of the old entries in it.
    std::map<uint32_t, uint32_t> places;
    std::vector<sar::WaveId> waves;
    for (const uint32_t entry : TableEntriesOf(bank, trim.programs))
    {
        sar::WaveId id = bank.waves[entry];
        const uint32_t index = id.index;
        id.index = sar::kNoItem;
        if (const auto kept = trim.waves.find(id.wave_archive_item); kept != trim.waves.end())
        {
            const auto at = std::ranges::lower_bound(kept->second, index);
            if (at != kept->second.end() && *at == index)
            {
                id.index = static_cast<uint32_t>(at - kept->second.begin());
            }
        }
        places[entry] = static_cast<uint32_t>(waves.size());
        waves.push_back(id);
    }
    const auto past = static_cast<uint32_t>(waves.size());
    bank.waves = std::move(waves);
    for (std::optional<sar::BankFile::Instrument>& instrument : bank.instruments)
    {
        if (!instrument)
        {
            continue;
        }

        for (sar::BankFile::KeyRegion& key_region : instrument->key_regions)
        {
            for (sar::BankFile::Region& region : key_region.regions)
            {
                const auto place = places.find(region.values.wave_id_index);
                region.values.wave_id_index = place != places.end() ? place->second : past;
            }
        }
    }

    return bank.Write();
}

// `song`'s file with `waves`, waves of wave archive `place` in 8-bit PCM, in 16-bit PCM, through the properties' edit
// of a wave's encoding (edit/waves.h). The Switch's renderer decodes no 8-bit PCM. Its voices play such a wave as
// silence. `name` names the wave archive in the warning.
void WithoutPcm8(Song& song, uint32_t place, std::span<const uint32_t> waves, const std::string& name)
{
    for (const uint32_t wave : waves)
    {
        format::File& file = song.result.file;
        const std::shared_ptr<const sar::SoundArchive> built =
            song::LoadArchive(format::ExportArchive(file), "the new archive", format::ExportGroupFiles(file));
        format::WaveModel model = WaveModelOf(file, *built, place, wave);
        model.encoding = sar::WaveEncoding::kPcm16;
        file = WithWaveModel(file, *built, place, wave, model);
    }
    const bool one = waves.size() == 1;
    song.result.warnings.push_back(std::to_string(waves.size()) + (one ? " wave of " : " waves of ") + name +
                                   (one ? " goes" : " go") +
                                   " in as 16-bit PCM, with the same samples. The Switch's renderer plays no 8-bit "
                                   "PCM.");
}

// The warning about `converted`, a file that went in for the console of `to` with an older version than it held, or
// nothing. `what` names the file, such as "Bank BANK_SE".
std::optional<std::string> OlderVersionWarning(const Converted& converted, const std::string& what, sar::Flavor to)
{
    const std::optional<ConvertedFile>& source = converted.incoming.converted;
    if (!source || !song::VersionsCompare(source->flavor, to) || source->version <= converted.incoming.version)
    {
        return std::nullopt;
    }

    return what + " goes in as version " + song::VersionText(to, converted.incoming.version) + ". It held " +
           song::VersionText(source->flavor, source->version) + ". Its values stay the same.";
}

// `bytes`, bank `id` of the source archive as a file of the 3DS. Each region that plays a wave of `mono` takes the gain
// of the wave at the pans of the region's notes (GainAt): the pan of each note's track at the note's start, plus the
// region's pan. A region without notes takes the gain at its pan. The region's volume takes the part of the gain that
// the wave's samples don't take (GainsOf), up to 255. `mono` counts the regions, and keeps the range of their gains and
// the regions held at 255. With `trim`, only the instruments of its programs change: the others stay out.
std::vector<uint8_t> WithMonoVolumes(std::span<const uint8_t> bytes, Mono& mono, uint32_t id, const Trim* trim)
{
    sar::BankFile bank = sar::BankFile::Read(bytes);
    for (uint32_t program = 0; program < bank.instruments.size(); ++program)
    {
        std::optional<sar::BankFile::Instrument>& instrument = bank.instruments[program];
        if (!instrument || (trim != nullptr && !trim->programs.contains(program)))
        {
            continue;
        }

        const auto notes = mono.notes.find({id, program});
        for (sar::BankFile::KeyRegion& key_region : instrument->key_regions)
        {
            for (sar::BankFile::Region& region : key_region.regions)
            {
                if (region.values.wave_id_index >= bank.waves.size())
                {
                    continue;
                }

                const sar::WaveId& wave = bank.waves[region.values.wave_id_index];
                const auto waves = mono.waves.find(wave.wave_archive_item);
                if (waves == mono.waves.end() || !waves->second.contains(wave.index))
                {
                    continue;
                }
                const float offset = static_cast<float>(static_cast<int>(region.values.pan) - 64) / 63.0f;
                std::vector<float> pans;
                for (const NotePan& note : notes != mono.notes.end() ? notes->second : std::vector<NotePan>{})
                {
                    if (note.key >= key_region.first_key && note.key <= key_region.last_key &&
                        note.velocity >= region.first_velocity && note.velocity <= region.last_velocity)
                    {
                        pans.push_back(note.pan + offset);
                    }
                }
                if (pans.empty())
                {
                    pans.push_back(offset);
                }
                const MonoGains& gains = GainsOf(mono, wave.wave_archive_item, wave.index);
                const double gain = GainAt(mono, gains.level, pans);
                const double db = 20.0 * std::log10(gain);
                mono.least_db = mono.regions == 0 ? db : std::min(mono.least_db, db);
                mono.most_db = mono.regions == 0 ? db : std::max(mono.most_db, db);
                mono.regions++;
                const long volume = std::lround(region.values.volume * gain / gains.samples);
                mono.held += volume > 255 ? 1 : 0;
                region.values.volume = static_cast<uint8_t>(std::clamp(volume, 0L, 255L));
            }
        }
    }

    return bank.Write();
}

// Adds bank `id` of `archive` and the wave archives that its waves name to `song` as files of `to`. Each goes in once.
// With `trim`, the bank holds only the instruments of its programs, and each wave archive only the waves of its list.
// Gives the new bank's item ID, or kNoItem for a bank that stays behind with a warning.
uint32_t AddBankOf(Song& song, const sar::SoundArchive& archive, uint32_t id, sar::Flavor to, const Trim* trim)
{
    const auto known = song.banks.find(id);
    if (known != song.banks.end())
    {
        return known->second;
    }

    // Everything that goes in is read and converted first. A bank that can't come along leaves the new file as it was.
    const std::optional<std::size_t> index = sar::ItemIndex(id, sar::kBankItem, archive.Banks().size());
    const std::string name = index && !archive.Banks()[*index].name.empty()
                                 ? archive.Banks()[*index].name
                                 : "BANK_" + std::to_string(sar::ItemNumber(id));
    std::optional<Converted> bank;
    std::vector<std::pair<uint32_t, Converted>> wave_archives;
    try
    {
        if (!index)
        {
            throw std::invalid_argument("The archive has no such bank.");
        }
        const std::span<const uint8_t> file =
            song::ItemFile(archive, {.kind = song::ItemKind::kBank, .index = static_cast<uint32_t>(*index)});
        bank = Converted{name, ReadIncomingFile(file, FileKind::kBank, to), {}, {}, {}};
        if (song.mono.on)
        {
            for (const NamedWaveArchive& named : WaveArchivesNamed(bank->incoming.bytes, to))
            {
                if (!song.wave_archives.contains(named.id) && !song.converted.contains(named.id))
                {
                    song.converted.emplace(named.id, ConvertWaveArchive(song, archive, named.id, to));
                }
            }
            if (std::ranges::any_of(song.mono.waves, [](const auto& waves) { return !waves.second.empty(); }))
            {
                try
                {
                    bank->incoming.bytes = WithMonoVolumes(bank->incoming.bytes, song.mono, id, trim);
                }
                catch (const std::exception&)
                {
                    song.result.warnings.push_back("Bank " + name +
                                                   " keeps the volumes of its regions. Hokuto can't read its values. "
                                                   "Its mono waves play quieter than in stereo.");
                }
            }
        }
        if (trim != nullptr)
        {
            bank->incoming.bytes = TrimmedBank(bank->incoming.bytes, *trim);
        }
        for (const NamedWaveArchive& named : WaveArchivesNamed(bank->incoming.bytes, to))
        {
            if (!song.wave_archives.contains(named.id))
            {
                const auto known = song.converted.find(named.id);
                Converted converted = known != song.converted.end() ? std::move(known->second)
                                                                    : ConvertWaveArchive(song, archive, named.id, to);
                if (known != song.converted.end())
                {
                    song.converted.erase(known);
                }
                std::optional<std::vector<uint32_t>> kept;
                if (trim != nullptr)
                {
                    const auto found = trim->waves.find(named.id);
                    kept = found != trim->waves.end() ? found->second : std::vector<uint32_t>{};
                    converted.incoming.bytes = TrimmedWaveArchive(converted.incoming.bytes, *kept);
                }
                if (song.mono.on)
                {
                    converted.incoming.bytes =
                        MonoWaveArchive(converted.incoming.bytes, song.mono, named.id, kept ? &*kept : nullptr);
                }
                wave_archives.emplace_back(named.id, std::move(converted));
            }
        }
    }
    catch (const std::exception& error)
    {
        song.result.warnings.push_back("Bank " + name + " stays behind, and its slot stays empty. " +
                                       format::AsSentence(error.what()));
        song.banks[id] = sar::kNoItem;
        return sar::kNoItem;
    }

    // The wave archives, then the bank. The bank names them by their places in the new archive's list.
    for (const auto& [wave_archive, converted] : wave_archives)
    {
        const auto place = static_cast<uint32_t>(song.result.file.native->tables->wave_archives.size());
        song.result.file = AddWaveArchive(song.result.file, converted.name, converted.incoming.bytes);
        song.wave_archives[wave_archive] = place;
        song.wave_bytes += StoredSize(converted.incoming.bytes.size());
        const std::string what = converted.name.empty()
                                     ? "Wave archive " + std::to_string(sar::ItemNumber(wave_archive))
                                     : "Wave archive " + converted.name;
        if (const std::optional<std::string> warning = OlderVersionWarning(converted, what, to))
        {
            song.result.warnings.push_back(*warning);
        }
        const std::optional<uint32_t>& waves = converted.incoming.converted->wave_version;
        const sar::Flavor from = archive.ReadOptions().flavor;
        if (converted.held_waves && song::HoldsOriginalStart(from, *converted.held_waves) && waves &&
            !song::HoldsOriginalStart(to, *waves))
        {
            song.older_waves.push_back(what.substr(std::string("Wave archive ").size()));
        }
        if (to == sar::Flavor::kNx && !converted.pcm8.empty())
        {
            WithoutPcm8(song, place, converted.pcm8, what.substr(std::string("Wave archive ").size()));
        }
    }
    std::map<uint32_t, uint32_t> map;
    for (const NamedWaveArchive& named : WaveArchivesNamed(bank->incoming.bytes, to))
    {
        map[named.id] = song.wave_archives.at(named.id);
    }
    song.result.file = AddBank(song.result.file, bank->name, bank->incoming.bytes, map);
    if (const std::optional<std::string> warning = OlderVersionWarning(*bank, "Bank " + bank->name, to))
    {
        song.result.warnings.push_back(*warning);
    }
    const auto place = static_cast<uint32_t>(song.result.file.native->tables->banks.size() - 1);
    song.banks[id] = sar::ItemId(sar::kBankItem, place);

    return song.banks[id];
}

// The sequence that sound `sound` of `archive` plays and the item where it starts: the sequence of `file` that the
// sound's file holds, or the commands that Hokuto reads from the file's bytes. `sequences` gives the file's sequence of
// each of the archive's files (format::SequencesOfFiles). Throws std::invalid_argument with a message for the user for
// a sequence that can't be read, and for a start where no command starts.
std::pair<ntdlib::commands::Sequence, uint32_t> SequenceOf(const format::File& file, const sar::SoundArchive& archive,
                                                           std::span<const std::optional<uint32_t>> sequences,
                                                           uint32_t sound)
{
    const sar::SoundInfo& info = archive.Sounds()[sound];
    ntdlib::commands::Sequence sequence;
    if (info.file_id < sequences.size() && sequences[info.file_id] && *sequences[info.file_id] < file.sequences.size())
    {
        sequence = *file.sequences[*sequences[info.file_id]];
    }
    else
    {
        try
        {
            sequence =
                ReadIncoming(song::ItemFile(archive, {.kind = song::ItemKind::kSequence, .index = sound})).sequence;
        }
        catch (const std::runtime_error& error)
        {
            throw std::invalid_argument(error.what());
        }
    }

    const std::optional<uint32_t> start =
        ItemAtOffset(Offsets(sequence, song::Console{.flavor = archive.ReadOptions().flavor}), info.start_offset);
    if (!start)
    {
        throw std::invalid_argument("The sound starts where no command of its sequence starts.");
    }

    return {std::move(sequence), *start};
}

// `file`, a file in the model form, after its archive gains sequence sound `name`. The sound plays the sequence file
// `bytes`, a file of the archive's console, from offset `start`, with bank slots `banks`. When the console's decoder
// reads the file's commands in their places, the file's sequences gain them. Otherwise the model keeps the file's
// bytes. The sound plays on the archive's first player, with the settings of an entry that sets nothing. Throws
// std::invalid_argument with a message for the user for a start where no command starts, and for an archive that the
// model can't lay out with the new sound.
format::File WithFixedSequence(const format::File& file, const std::string& name, std::vector<uint8_t> bytes,
                               uint32_t start, const std::vector<uint32_t>& banks)
{
    // The commands as the console's decoder reads them, laid out in their places again.
    const Incoming incoming = ReadIncoming(bytes);
    const std::optional<uint32_t> item =
        ItemAtOffset(Offsets(incoming.sequence, song::Console{.flavor = incoming.flavor}), start);
    if (!item)
    {
        throw std::invalid_argument("The sound starts where no command of its converted sequence starts.");
    }
    try
    {
        return AddSequenceSound(file, std::nullopt, name, incoming.sequence, *item, {}, banks);
    }
    catch (const std::invalid_argument&)
    {
        // The cut keeps the file in its bytes.
    }

    format::Model model = format::ModelOf(file);
    const auto id = static_cast<uint32_t>(model.archive.files.size());
    model.archive.files.push_back({.bytes = std::move(bytes)});
    sar::SoundModel sound;
    sound.name = name;
    sound.type = sar::SoundType::kSequence;
    sound.file_id = id;
    sound.player_id = sar::ItemId(sar::kPlayerItem, 0);
    sound.banks = banks;
    sound.allocate_track_flags = TracksOpened(incoming.sequence, *item);
    sound.start_offset = start;
    model.archive.sounds.push_back(std::move(sound));
    try
    {
        return format::WithModel(file, model);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't add a sound to this archive. ") +
                                    format::AsSentence(error.what()));
    }
}

// The warning about the settings of the new sound that only the model holds, when they stay behind.
constexpr std::string_view kSettingsLeft =
    "The sound's 3D settings, single play, user parameters and remote filter stay behind. ";

// `song`'s file as a save and an open give it: the archive that its model lays out. The new sound, sound `added`, takes
// the settings of `source`, the model of the source sound, that the archive's tables hold beyond the Sound sections'
// values: its remote filter, its single play, its 3D settings, its options 17 and 28 to 31, and the value after its 3D
// settings. They stay behind with a warning when the new archive can't take them.
void Settle(Song& song, uint32_t added, const std::optional<sar::SoundModel>& source)
{
    const format::Model model = format::ModelOf(song.result.file);
    if (source)
    {
        format::Model with = model;
        sar::SoundModel& sound = with.archive.sounds.at(added);
        sound.remote_filter = source->remote_filter;
        sound.single_play = source->single_play;
        sound.settings_3d = source->settings_3d;
        sound.option_17 = source->option_17;
        sound.user_options = source->user_options;
        sound.after_3d = source->after_3d;
        try
        {
            song.result.file = format::WithModel(song.result.file, with);
            return;
        }
        catch (const std::exception& error)
        {
            song.result.warnings.push_back(std::string(kSettingsLeft) + format::AsSentence(error.what()));
        }
    }

    song.result.file = format::WithModel(song.result.file, model);
}

// The parts of `file` that go with sound `sound` into `song`'s file as sound `added`: the file's tags, the sound's
// tags, scenes and tracks' colours, the aux buses' volumes and the effects that the other console runs, the sound's
// mode of mixing, and the mode of its sequence, `sequence`, when it has one. The new file holds that sequence as
// `new_sequence`, when it holds it among its sequences.
void CopyParts(Song& song, const format::File& file, uint32_t sound, uint32_t added, std::optional<uint32_t> sequence,
               std::optional<uint32_t> new_sequence)
{
    format::File& target = song.result.file;
    const bool to_wii = target.native->wii;
    const std::string console = to_wii ? std::string("Wii") : song::ConsoleName(target.native->flavor);
    target.tags = file.tags;
    if (const auto tags = file.sound_tags.find(sound); tags != file.sound_tags.end())
    {
        target.sound_tags[added] = tags->second;
    }
    for (const format::Scene& scene : file.scenes)
    {
        if (scene.sound == sound)
        {
            target.scenes.push_back(scene);
            target.scenes.back().sound = added;
        }
    }
    for (const auto& [place, colour] : file.track_colours)
    {
        if (place.sound == sound)
        {
            target.track_colours[{added, place.track}] = colour;
        }
    }
    const auto mixing = file.sound_mixing.find(sound);
    target.mixing = mixing != file.sound_mixing.end() ? mixing->second : file.mixing;
    if (const std::optional<std::string> refused = format::MixingRefused(target.engine, target.mixing))
    {
        target.mixing = format::Mixing::kStrict;
        song.result.warnings.push_back("The sound plays in strict mixing. " + *refused);
    }
    target.aux_volumes = file.aux_volumes;

    // The effects that the other console runs stay on their buses. The others stay behind. The effects of a file of the
    // Wii play on the Wii alone, and the Wii runs none of the other consoles' effects.
    const bool wii = file.native && file.native->wii;
    for (std::size_t bus = 0; bus < format::kAuxBuses; ++bus)
    {
        std::vector<std::string> left;
        for (const format::Effect& effect : file.aux_effects[bus])
        {
            if (!wii && RunsEffect(target.engine, effect.kind))
            {
                target.aux_effects[bus].push_back(effect);
            }
            else
            {
                left.emplace_back(EffectName(effect.kind));
            }
        }
        if (!left.empty())
        {
            const bool one = left.size() == 1;
            const std::string why = wii ? "Hokuto plays the Wii's effects only on the Wii."
                                    : to_wii
                                        ? "The Wii runs the effects of its SDK's AXFX library instead."
                                        : "The " + console + (one ? " runs no such effect." : " runs no such effects.");
            song.result.warnings.push_back((wii ? "The Wii's " : "The ") + ListOf(left) + " on bus " +
                                           std::string(1, static_cast<char>('A' + bus)) +
                                           (one ? " stays behind. " : " stay behind. ") + why);
        }
    }

    // A timeline stays a timeline, with its converter's hints.
    if (file.timelines)
    {
        target.timelines.emplace();
        const auto hints = sequence && new_sequence ? file.timelines->find(*sequence) : file.timelines->end();
        if (hints != file.timelines->end())
        {
            (*target.timelines)[*new_sequence] = hints->second;
        }
    }
}

// The most notes of a sound that sound at once before their releases, and the most channels of their waves at once:
// with the waves as they are, and with the stereo waves as mono. `stereo` says whether a note plays a stereo wave.
// `stereo_notes` holds the notes of stereo waves, by the item IDs of their banks and their programs.
struct Held
{
    std::size_t notes = 0;
    std::size_t channels = 0;
    std::size_t mono_channels = 0;
    bool stereo = false;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<NotePan>> stereo_notes;
};

// What ntdlib's performer reads of sound `sound` of `archive`. A note sounds from its start to its end. Each channel of
// its wave takes a voice of the console. A note whose wave the archive lacks counts for nothing. The 3DS plays each
// step of a track's pan from -64 to 63 as a 63rd of the way to a side (UpdateChannelParam).
Held HeldOf(const sar::SoundArchive& archive, uint32_t sound)
{
    const sar::Options& options = archive.ReadOptions();
    const std::vector<uint32_t>& banks = archive.Sounds()[sound].banks;
    std::map<uint32_t, std::optional<sar::Bank>> parsed;
    std::map<std::pair<uint32_t, uint32_t>, std::size_t> channels;
    const auto channels_of = [&](const song::Note& note) -> std::size_t
    {
        const uint32_t item = note.bank_slot < banks.size() ? banks[note.bank_slot] : sar::kNoItem;
        const uint32_t bank = item & 0xFFFFFF;
        if (item == sar::kNoItem || bank >= archive.Banks().size())
        {
            return 0;
        }
        auto [place, added] = parsed.try_emplace(bank);
        if (added)
        {
            const std::span<const uint8_t> bytes = archive.FileData(archive.Banks()[bank].file_id);
            if (!bytes.empty())
            {
                place->second = sar::Bank::Parse(bytes, options);
            }
        }
        const std::optional<sar::Bank>& read = place->second;
        if (!read || note.program >= read->instruments.size() || !read->instruments[note.program])
        {
            return 0;
        }
        const sar::VelocityRegion* region = read->instruments[note.program]->Find(note.key, note.velocity);
        if (region == nullptr || region->wave_id_index >= read->waves.size())
        {
            return 0;
        }
        const sar::WaveId& id = read->waves[region->wave_id_index];
        const auto known = channels.find({id.wave_archive_item, id.index});
        if (known != channels.end())
        {
            return known->second;
        }
        std::size_t count = 0;
        const uint32_t wave_archive = id.wave_archive_item & 0xFFFFFF;
        if (id.index != sar::kNoItem && wave_archive < archive.WaveArchives().size())
        {
            const std::span<const uint8_t> bytes = archive.FileData(archive.WaveArchives()[wave_archive].file_id);
            const sar::WaveArchive waves = bytes.empty() ? sar::WaveArchive{} : sar::WaveArchive::Parse(bytes, options);
            if (id.index < waves.waves.size())
            {
                count = sar::Wave::Parse(waves.waves[id.index], options).channels.size();
            }
        }
        channels.emplace(std::pair{id.wave_archive_item, id.index}, count);

        return count;
    };

    // The changes in the notes and channels that sound, by their ticks. A note that ends where another starts makes
    // room for it.
    struct Change
    {
        uint32_t tick = 0;
        int notes = 0;
        int channels = 0;
        int mono_channels = 0;
    };
    std::vector<Change> changes;
    Held held;
    for (const song::Track& track : song::ReadSequence(archive, sound).tracks)
    {
        for (const song::Note& note : track.notes)
        {
            const auto count = static_cast<int>(channels_of(note));
            if (count == 0)
            {
                continue;
            }
            const int mono = count == 2 ? 1 : count;
            held.stereo = held.stereo || count == 2;
            if (count == 2)
            {
                const auto after = std::ranges::upper_bound(track.pan, note.start, {}, &song::Setting::tick);
                const int pan = after == track.pan.begin() ? 0 : std::prev(after)->value;
                held.stereo_notes[{banks[note.bank_slot], note.program}].push_back(
                    {note.key, note.velocity, std::clamp(static_cast<float>(pan) / 63.0f, -1.0f, 1.0f)});
            }
            changes.push_back({note.start, 1, count, mono});
            changes.push_back({note.start + std::max<uint32_t>(note.length, 1), -1, -count, -mono});
        }
    }
    const auto earlier = [](const Change& a, const Change& b)
    {
        return a.tick != b.tick ? a.tick < b.tick : a.notes < b.notes;
    };
    std::ranges::sort(changes, earlier);
    int notes = 0;
    int sounding = 0;
    int mono = 0;
    for (const Change& change : changes)
    {
        notes += change.notes;
        sounding += change.channels;
        mono += change.mono_channels;
        held.notes = std::max(held.notes, static_cast<std::size_t>(notes));
        held.channels = std::max(held.channels, static_cast<std::size_t>(sounding));
        held.mono_channels = std::max(held.mono_channels, static_cast<std::size_t>(mono));
    }

    return held;
}

// Gives sound `added` of `song`'s file `values`, the values of a sound in the archive's tables that kCopiedValues
// names, through the Sound sections' edits. A value that the new archive can't take stays behind with a warning.
void CopyValues(Song& song, uint32_t added, const format::NativeSound& values)
{
    for (const SoundValue value : kCopiedValues)
    {
        const uint32_t wanted = ValueOf(values, value, 0);
        if (ValueOf(song.result.file.native->tables->sounds[added], value, 0) == wanted)
        {
            continue;
        }
        try
        {
            song.result.file = ChangeSound(song.result.file, added, {.value = value, .slot = 0, .to = wanted});
        }
        catch (const std::invalid_argument& error)
        {
            song.result.warnings.push_back("The sound's " + NameOf(value) + " stays behind. " +
                                           format::AsSentence(error.what()));
        }
    }
}

// For the value `lpf` of a low-pass command (units.h), the value whose cutoff on the later consoles lies nearest the
// cutoff that NW4R SND's `build` gives `lpf`. Builds before November 2008 work the cutoff out: 32000 Hz times 2^(10
// (value - 1)), for the value 1 + lpf held to 0 and 1. The later consoles take it from a table of 24 cutoffs from 80 Hz
// to 16 kHz, a third of an octave apart, at the table's step below the value. Of the command's values whose cutoffs lie
// nearest in octaves, the one nearest `lpf` goes in. From 16 kHz, neither filters. Later builds read the table as the
// later consoles do, and `lpf` stays.
int64_t LaterLowPass(int64_t lpf, const ntdlib::SndBuild& build)
{
    using ntdlib::commands::kOne;
    const auto value = [](int64_t fixed)
    {
        return 1.0f + static_cast<float>(static_cast<double>(fixed) / static_cast<double>(kOne));
    };
    const uint16_t wii = ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::RevolutionPolicy>(value(lpf), build);
    if (build.SinceNovember2008() || wii >= 16000)
    {
        return lpf;
    }

    // The command holds a byte: a value of (byte - 64) / 64.
    int64_t best = lpf;
    double nearest = std::numeric_limits<double>::infinity();
    for (int64_t byte = 0; byte < 128; ++byte)
    {
        const int64_t candidate = (byte - 64) * kOne / 64;
        const uint16_t later = ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::CafePolicy>(value(candidate));
        const double octaves = std::abs(std::log2(static_cast<double>(later) / static_cast<double>(wii)));
        if (octaves < nearest || (octaves == nearest && std::llabs(candidate - lpf) < std::llabs(best - lpf)))
        {
            best = candidate;
            nearest = octaves;
        }
    }

    return best;
}

// The commands of a sequence of the Wii in the forms in which the later consoles read the same commands. `build` is the
// build of NW4R SND that plays them. The Wii's end after a comparison ends the track whatever the comparison gave, and
// the end loses its comparison. A random prefix before a variable prefix gives nothing on the Wii: the variable gives
// the value. The later consoles would read a command 0xA1 after the random prefix, and the random prefix goes. NW4R SND
// sets volume2, the main volume and, before September 2009, the pitch bend at once. Their times go. A low-pass from a
// value or a random number takes the values whose cutoffs on the later consoles lie nearest (LaterLowPass).
ntdlib::commands::Sequence AsLaterConsoles(ntdlib::commands::Sequence sequence, const ntdlib::SndBuild& build)
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;
    for (ntdlib::commands::Command& command : sequence.items)
    {
        if (command.kind == Kind::kEnd)
        {
            command.conditional = false;
        }
        if (command.value.source == Source::kVariable || command.encoding.unused_variable)
        {
            command.encoding.unused_random = false;
        }
        if (command.kind == Kind::kVolume2 || command.kind == Kind::kMainVolume ||
            (command.kind == Kind::kPitchBend && !ntdlib::nwsnd::RevolutionPolicy::BendRamps(build)))
        {
            command.time.reset();
        }
        if (command.kind == Kind::kLowPass && command.value.source != Source::kVariable)
        {
            command.value.value = LaterLowPass(command.value.value, build);
            if (command.value.source == Source::kRandom)
            {
                command.value.high = LaterLowPass(command.value.high, build);
            }
        }
    }

    return sequence;
}

// Which items of `sequence`, a sequence of the Wii, hold bytes that no command reaches: data at the start, or after a
// jump without a comparison, an end or such data. The Wii's ends end their tracks whatever a comparison gave. A track
// that reaches a return outside every call runs on past it, and data after a return can play.
std::vector<bool> DeadData(const ntdlib::commands::Sequence& sequence)
{
    using ntdlib::commands::Kind;
    std::vector<bool> dead(sequence.items.size());
    for (std::size_t item = 0; item < sequence.items.size(); ++item)
    {
        if (sequence.items[item].kind != Kind::kData)
        {
            continue;
        }

        const ntdlib::commands::Command* before = item > 0 ? &sequence.items[item - 1] : nullptr;
        dead[item] = before == nullptr || dead[item - 1] || before->kind == Kind::kEnd ||
                     (before->kind == Kind::kJump && !before->conditional);
    }

    return dead;
}

// The commands of a sound's sequence file, the item where the sound starts, and the file's sequence among the sequences
// of the file that it comes from, when that file holds it.
struct WiiSequence
{
    ntdlib::commands::Sequence sequence;
    uint32_t start = 0;
    std::optional<uint32_t> held;
};

// The commands of sequence sound `sound` of `archive`, the archive that the Wii's `file` exports, and where the sound
// starts (`places`, SoundPlaces). A sequence file that stays in the archive's bytes decodes by the rules of NW4R SND's
// `build`, from its labels and the starts of the sounds that play it. Throws std::invalid_argument with a message for
// the user for a file that the archive lacks or that Hokuto can't read.
WiiSequence WiiSequenceOf(const format::File& file, const ntdlib::rsar::SoundArchive& archive,
                          std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const ntdlib::SndBuild& build)
{
    if (sound < places.size() && places[sound])
    {
        const SoundPlace& place = *places[sound];
        return {*file.sequences.at(place.sequence),
                place.start_field ? file.native->fields.at(*place.start_field).to.offset : 0, place.sequence};
    }

    const uint32_t file_id = archive.Sounds()[sound].file_id;
    const std::optional<ntdlib::rsar::FileData> data = archive.File(file_id);
    if (!data)
    {
        throw std::invalid_argument("The archive lacks the sound's sequence file.");
    }
    ntdlib::rsar::SequenceFile read;
    try
    {
        read = ntdlib::rsar::SequenceFile::Read(data->file);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the sound's sequence file. ") +
                                    format::AsSentence(error.what()));
    }
    std::vector<ntdlib::nwsnd::LabelOffset> labels;
    for (const ntdlib::rsar::Sequence::Label& label : read.labels)
    {
        labels.push_back({label.name, label.offset});
    }
    std::vector<uint32_t> entries;
    std::size_t entry = 0;
    for (uint32_t other = 0; other < archive.Sounds().size(); ++other)
    {
        const ntdlib::rsar::SoundInfo& info = archive.Sounds()[other];
        if (info.type == ntdlib::rsar::SoundType::kSequence && info.file_id == file_id)
        {
            entry = other == sound ? entries.size() : entry;
            entries.push_back(info.start_offset);
        }
    }
    ntdlib::nwsnd::Decoded decoded;
    try
    {
        decoded =
            ntdlib::nwsnd::DecodeSequence(read.data, labels, song::GrammarOf(song::Console{.wii = build}), entries);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the commands of the sound's sequence file. ") +
                                    format::AsSentence(error.what()));
    }
    if (decoded.opaque)
    {
        throw std::invalid_argument("The commands of the sound's sequence file overlap or jump into one another.");
    }

    return {std::move(decoded.sequence), decoded.entries.at(entry), std::nullopt};
}

// Gives the sound that starts at item `start` of `sequence` commands at its start that set each global variable that
// the sound reads before it sets it, and whose starting value can change what the sound plays (song::VariablesOf), to
// `wanted` while it holds `held`. NW4R SND starts the global variables at 0, and the later consoles start them at -1.
// Each such variable takes a comparison with `held` and a setting after it. A comparison of the last variable with
// itself then gives the comparison's flag its starting value. Returns the item where the sound starts, and the
// variables' numbers.
std::pair<uint32_t, std::vector<int>> WithStartingGlobals(ntdlib::commands::Sequence& sequence, uint32_t start,
                                                          int64_t held, int64_t wanted)
{
    using ntdlib::commands::Argument;
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;
    std::vector<int> globals;
    for (const song::Variable& variable : song::VariablesOf(sequence, start).starting)
    {
        if (variable.number >= kSoundVariables && variable.number < song::kGameVariables)
        {
            globals.push_back(variable.number);
        }
    }
    if (globals.empty())
    {
        return {start, {}};
    }

    std::vector<Command> commands;
    for (const int global : globals)
    {
        const auto index = static_cast<uint8_t>(global);
        commands.push_back(Command{.kind = Kind::kCompareEqual,
                                   .index = index,
                                   .value = Argument{Source::kValue, held * ntdlib::commands::kOne, 0}});
        commands.push_back(Command{.kind = Kind::kSetVariable,
                                   .conditional = true,
                                   .index = index,
                                   .value = Argument{Source::kValue, wanted * ntdlib::commands::kOne, 0}});
    }
    commands.push_back(Command{.kind = Kind::kCompareEqual,
                               .index = static_cast<uint8_t>(globals.back()),
                               .value = Argument{Source::kVariable, globals.back(), 0}});

    return {InsertAtStart(sequence, start, std::move(commands)), globals};
}

// The warning about the global variables `globals`. Commands at the sound's start set their starting values to the
// value that the console of the sound's file gives them (WithStartingGlobals). `to_wii` says whether the sound goes to
// the Wii.
std::string GlobalsWarning(std::span<const int> globals, bool to_wii)
{
    std::vector<std::string> numbers;
    for (const int global : globals)
    {
        numbers.push_back(std::to_string(global));
    }

    const bool one = globals.size() == 1;
    return "The sound reads global " + std::string(one ? "variable " : "variables ") + ListOf(numbers) +
           " before it sets " + (one ? "it" : "them") +
           ". NW4R SND starts the global variables at 0, and the later consoles start them at -1. Commands at the "
           "sound's start set " +
           (one ? "it" : "each") + (to_wii ? " to -1 while it holds 0." : " to 0 while it holds -1.");
}

// The item ID that the table of waves of a bank of the Wii names its waves' wave archive by, before the bank goes in.
// AddBank then names the new wave archive instead.
constexpr uint32_t kWiiWaves = sar::ItemId(sar::kWaveArchiveItem, 0);

// The name of the wave archive that holds the waves of the Wii's bank `bank`: WARC_ in place of BANK_ at the start of
// the bank's name, or before the name.
std::string WaveArchiveNameOf(const std::string& bank)
{
    return "WARC_" + (bank.starts_with("BANK_") ? bank.substr(5) : bank);
}

// Adds bank `index` of the Wii's `archive` to `song` as a bank of the Wii U, after a wave archive of its waves. The
// regions play with the values that NW4R SND's `build` reads (ntdlib::rsar::BankFileOf). Gives the new bank's item ID,
// or kNoItem for a bank that stays behind with a warning.
uint32_t AddWiiBank(Song& song, const ntdlib::rsar::SoundArchive& archive, uint32_t index,
                    const ntdlib::SndBuild& build)
{
    const std::string name = index < archive.Banks().size() && !archive.Banks()[index].name.empty()
                                 ? archive.Banks()[index].name
                                 : "BANK_" + std::to_string(index);
    ConvertedItem bank;
    try
    {
        bank = LaterBankFilesOf(archive, index, build, sar::Flavor::kCafe);
    }
    catch (const std::exception& error)
    {
        song.result.warnings.push_back("Bank " + name + " stays behind, and its slot stays empty. " +
                                       format::AsSentence(error.what()));
        return sar::kNoItem;
    }

    const auto place = static_cast<uint32_t>(song.result.file.native->tables->wave_archives.size());
    song.result.file = AddWaveArchive(song.result.file, WaveArchiveNameOf(name), bank.waves);
    song.result.file = AddBank(song.result.file, name, bank.file, {{kWiiWaves, place}});

    return sar::ItemId(sar::kBankItem, static_cast<uint32_t>(song.result.file.native->tables->banks.size() - 1));
}

// Sequence sound `sound` of `file`, a file of the Wii, as a new file of the Wii U. The sound's bank goes in as a bank
// of the Wii U, after a wave archive of its waves. The commands that the Wii's build of NW4R SND skips stay out, and so
// do the bytes that no command reaches (DeadData). The other commands go in as the Wii U reads them (AsLaterConsoles),
// with commands at the sound's start that give the global variables the Wii's starting value (WithStartingGlobals).
// NW4R SND plays a sound's volume above 127 at 127, and the sound takes 127. The build of October 2006 pans by the dual
// mode and the square-root curve alone, and the sound takes them. The remote filter and the user parameters stay
// behind. So do the Wii's effects. The warnings name no console but the Wii: the file can go on to another console.
// Throws std::invalid_argument with a message for the user for a sound that isn't a sequence sound, and for a sequence
// that Hokuto can't send yet.
OtherConsoleSong FromWii(const format::File& file, uint32_t sound)
{
    const format::NativeTables& tables = *file.native->tables;
    if (sound >= tables.sounds.size() || tables.sounds[sound].kind != sar::SoundType::kSequence)
    {
        throw std::invalid_argument("Only a sequence sound goes to another console.");
    }
    const format::NativeSound& source = tables.sounds[sound];

    // The archive that the file exports, and the sound's sequence and start.
    const std::vector<uint8_t> bytes = format::ExportArchive(file);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive =
        song::LoadWiiArchive(bytes, "the archive", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*file.native);
    const std::vector<std::optional<SoundPlace>> places = SoundPlaces(
        *archive, format::SequencesOfFiles(file, bytes, *archive), format::StartFieldsOfSounds(file, bytes));
    const WiiSequence read = WiiSequenceOf(file, *archive, places, sound, build);

    // The Wii's commands as the Wii U reads them, without the commands that the Wii's build skips and the bytes that no
    // command reaches.
    const song::Console reads{.wii = build};
    const song::Console writes{.flavor = sar::Flavor::kCafe};
    const ntdlib::commands::Sequence later = AsLaterConsoles(read.sequence, build);
    std::vector<bool> leaves = DeadData(later);
    for (std::size_t item = 0; item < later.items.size(); ++item)
    {
        leaves[item] = leaves[item] || PlaysOtherwise(later.items[item], reads, writes);
    }
    auto [kept, kept_start] = WithoutItems(later, read.start, leaves);
    if (Fixed(kept))
    {
        throw std::invalid_argument(
            "The sequence file's commands have to keep their places. Hokuto can't send such a "
            "sequence of the Wii to another console yet.");
    }
    const auto [start, globals] = WithStartingGlobals(kept, kept_start, -1, 0);

    Song song;
    song.result.file = format::NewFile(format::Engine::kWiiU);
    std::vector<uint32_t> slots;
    for (const uint32_t bank : source.banks)
    {
        slots.push_back(bank == sar::kNoItem ? sar::kNoItem : AddWiiBank(song, *archive, sar::ItemNumber(bank), build));
    }

    // The sound.
    const std::string name = source.name.empty() ? "SEQ_" + std::to_string(sound) : source.name;
    song.result.file = AddSequenceSound(song.result.file, std::nullopt, name, kept, start, {}, slots);
    const auto new_sequence = static_cast<uint32_t>(song.result.file.sequences.size() - 1);
    if (const std::optional<std::string> warning =
            SkippedWarning(CountSkipped(later, reads, writes), reads, writes, true))
    {
        song.result.warnings.push_back(*warning);
    }
    if (!globals.empty())
    {
        song.result.warnings.push_back(GlobalsWarning(globals, false));
    }

    // The sound's values in the archive's tables.
    const auto added = static_cast<uint32_t>(song.result.file.native->tables->sounds.size() - 1);
    format::NativeSound values = source;
    if (values.volume > 127)
    {
        song.result.warnings.push_back("The sound's volume of " + std::to_string(values.volume) +
                                       " goes in as 127. NW4R SND plays a sound's volume above 127 at 127.");
        values.volume = 127;
    }
    if (build.October2006() && (values.pan_mode != 0 || values.pan_curve != 0))
    {
        song.result.warnings.push_back(
            "The sound takes the dual pan mode and the square-root pan curve. The build of "
            "October 2006 of NW4R SND pans by them alone.");
        values.pan_mode = 0;
        values.pan_curve = 0;
    }
    CopyValues(song, added, values);
    const ntdlib::rsar::SoundInfo& info = archive->Sounds()[sound];
    if (info.remote_filter != 0 || info.user_params[0] != 0 || info.user_params[1] != 0)
    {
        song.result.warnings.push_back("The sound's remote filter and user parameters stay behind.");
    }
    Settle(song, added, std::nullopt);

    // The other sounds whose commands the sequence file holds.
    const std::size_t sharing = song::SoundsSharingFile(*archive, sound).size();
    if (sharing > 0)
    {
        song.result.warnings.push_back("The sequence file holds the commands of " + std::to_string(sharing) +
                                       (sharing == 1 ? " other sound" : " other sounds") +
                                       " too. They go along unused.");
    }

    CopyParts(song, file, sound, added, read.held, new_sequence);

    return std::move(song.result);
}

// The cutoff in Hz that the console of `flavor` gives a low-pass value from 0 to 1 (Util::CalcLpfFreq).
uint16_t LaterCutoff(float value, sar::Flavor flavor)
{
    switch (flavor)
    {
    case sar::Flavor::kCtr:
        return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::CtrPolicy>(value);
    case sar::Flavor::kCafe:
        return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::CafePolicy>(value);
    case sar::Flavor::kNx:
        break;
    }

    return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::NxPolicy>(value);
}

// For the value `lpf` of a low-pass command of the console of `from` (units.h), the value whose cutoff in NW4R SND's
// `build` lies nearest in octaves the cutoff that the console of `from` gives `lpf`: LaterLowPass the other way. Of the
// command's values at the same distance, the one nearest `lpf` goes in. From 16 kHz, neither filters: a value that
// filters nothing on the console of `from` takes the value nearest `lpf` that filters nothing on the Wii. Builds from
// November 2008 read the later consoles' table, and `lpf` stays.
int64_t WiiLowPass(int64_t lpf, sar::Flavor from, const ntdlib::SndBuild& build)
{
    using ntdlib::commands::kOne;
    if (build.SinceNovember2008())
    {
        return lpf;
    }

    const auto value = [](int64_t fixed)
    {
        return 1.0f + static_cast<float>(static_cast<double>(fixed) / static_cast<double>(kOne));
    };
    const auto wii = [&build, &value](int64_t fixed)
    {
        return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::RevolutionPolicy>(value(fixed), build);
    };

    constexpr uint16_t kOpen = 16000;
    const uint16_t later = LaterCutoff(value(lpf), from);
    if (later >= kOpen && wii(lpf) >= kOpen)
    {
        return lpf;
    }

    // The command holds a byte: a value of (byte - 64) / 64.
    constexpr double kFar = std::numeric_limits<double>::infinity();
    int64_t best = lpf;
    double nearest = kFar;
    for (int64_t byte = 0; byte < 128; ++byte)
    {
        const int64_t candidate = (byte - 64) * kOne / 64;
        const uint16_t cutoff = wii(candidate);
        const double octaves = later >= kOpen
                                   ? (cutoff >= kOpen ? 0.0 : kFar)
                                   : std::abs(std::log2(static_cast<double>(cutoff) / static_cast<double>(later)));
        if (octaves < nearest || (octaves == nearest && std::llabs(candidate - lpf) < std::llabs(best - lpf)))
        {
            best = candidate;
            nearest = octaves;
        }
    }

    return best;
}

// A sequence of a later console as NW4R SND reads it (AsWii), the settings of volume2, the main volume and the pitch
// bend whose times went, and the other settings whose times went.
struct WiiCommands
{
    ntdlib::commands::Sequence sequence;
    std::size_t times = 0;
    std::size_t untimed = 0;
};

// The commands of a sequence of the console of `from` in the forms in which NW4R SND's `build` reads the same commands:
// AsLaterConsoles the other way. The Wii's end after a comparison ends the track whatever the comparison gave. Such an
// end becomes a jump after the comparison to an end after the sequence's items, and plays as before. NW4R SND sets
// volume2, the main volume and, before September 2009, the pitch bend at once. Their times go. The builds before March
// 2008 lack the time prefixes (RevolutionPolicy::HasCommand). Every setting's time goes there, and the setting takes
// its value at once. A low-pass from a value or a random number takes the values whose cutoffs on the Wii lie nearest
// (WiiLowPass). The later consoles keep a sweep's pitch in 64ths of a semitone. The builds before March 2008 keep it in
// 32nds. A sweep's pitch takes the build's nearest pitch: the pitch that the encoder writes. The text form then holds
// the pitch too.
WiiCommands AsWii(ntdlib::commands::Sequence sequence, sar::Flavor from, const ntdlib::SndBuild& build)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;
    WiiCommands result;
    const auto end = static_cast<uint32_t>(sequence.items.size());
    const bool prefixes = ntdlib::nwsnd::RevolutionPolicy::HasCommand(build, 0xA3);
    const ntdlib::nwsnd::Scale sweep =
        ntdlib::nwsnd::ScaleOf(Kind::kSweepPitch, ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(build));
    const auto held_pitch = [&sweep](int64_t pitch)
    {
        return ntdlib::nwsnd::ToUnit(ntdlib::nwsnd::FromUnit(pitch, sweep), sweep);
    };

    bool ends = false;
    for (Command& command : sequence.items)
    {
        if (command.kind == Kind::kEnd && command.conditional)
        {
            command = Command{.kind = Kind::kJump, .conditional = true, .target = end};
            ends = true;
        }
        if (command.time && (command.kind == Kind::kVolume2 || command.kind == Kind::kMainVolume ||
                             (command.kind == Kind::kPitchBend && !ntdlib::nwsnd::RevolutionPolicy::BendRamps(build))))
        {
            command.time.reset();
            ++result.times;
        }
        else if (command.time && !prefixes)
        {
            command.time.reset();
            ++result.untimed;
        }
        if (command.kind == Kind::kLowPass && command.value.source != Source::kVariable)
        {
            command.value.value = WiiLowPass(command.value.value, from, build);
            if (command.value.source == Source::kRandom)
            {
                command.value.high = WiiLowPass(command.value.high, from, build);
            }
        }
        if (command.kind == Kind::kSweepPitch && command.value.source != Source::kVariable)
        {
            command.value.value = held_pitch(command.value.value);
            if (command.value.source == Source::kRandom)
            {
                command.value.high = held_pitch(command.value.high);
            }
        }
    }
    if (ends)
    {
        sequence.items.push_back(Command{.kind = Kind::kEnd});
    }
    result.sequence = std::move(sequence);

    return result;
}

// The warnings about the settings of `commands` whose times went (AsWii), after `warnings`.
void AddTimeWarnings(const WiiCommands& commands, std::vector<std::string>& warnings)
{
    if (commands.times > 0)
    {
        const bool one = commands.times == 1;
        warnings.push_back("NW4R SND sets volume2, the main volume and the pitch bend at once. " +
                           std::to_string(commands.times) +
                           (one ? " of the sequence's settings of them loses its time, and changes at once."
                                : " of the sequence's settings of them lose their times, and change at once."));
    }
    if (commands.untimed > 0)
    {
        const bool one = commands.untimed == 1;
        warnings.push_back("NW4R SND's builds before March 2008 have no times for settings. " +
                           std::to_string(commands.untimed) + std::string(commands.times > 0 ? " other" : "") +
                           (one ? " setting of the sequence loses its time, and changes at once."
                                : " settings of the sequence lose their times, and change at once."));
    }
}

// The version of the Wii's bank files that hold their waves in an RWAR, and of the Wii's sequence files.
constexpr uint16_t kWiiBankVersion = 0x0102;
constexpr uint16_t kWiiSequenceVersion = 0x0100;

// The version of the Wii's wave sound files that hold their waves in an RWAR.
constexpr uint16_t kWiiWaveSoundVersion = 0x0103;

// The instruments of `bank` as the later consoles' reader plays them (sar::Instrument::Find): a key region for each run
// of keys whose velocities find the same regions, and a velocity region for each run of velocities that finds one
// region. A program that plays no region at any key and velocity has no instrument.
sar::BankFile BankFileOfRegions(const sar::Bank& bank)
{
    sar::BankFile file;
    file.waves = bank.waves;
    for (const std::optional<sar::Instrument>& instrument : bank.instruments)
    {
        std::optional<sar::BankFile::Instrument>& out = file.instruments.emplace_back();
        if (!instrument)
        {
            continue;
        }

        // The region that each key plays at each velocity.
        using Column = std::array<const sar::VelocityRegion*, 128>;
        std::array<Column, 128> grid{};
        for (int key = 0; key < 128; ++key)
        {
            for (int velocity = 0; velocity < 128; ++velocity)
            {
                grid[key][velocity] = instrument->Find(key, velocity);
            }
        }

        sar::BankFile::Instrument regions;
        for (int first = 0; first < 128;)
        {
            int last = first;
            while (last + 1 < 128 && grid[last + 1] == grid[first])
            {
                ++last;
            }
            const Column& column = grid[first];
            sar::BankFile::KeyRegion key_region{
                .first_key = static_cast<uint8_t>(first), .last_key = static_cast<uint8_t>(last), .regions = {}};
            for (int low = 0; low < 128;)
            {
                int high = low;
                while (high + 1 < 128 && column[high + 1] == column[low])
                {
                    ++high;
                }
                if (column[low] != nullptr)
                {
                    key_region.regions.push_back({.first_velocity = static_cast<uint8_t>(low),
                                                  .last_velocity = static_cast<uint8_t>(high),
                                                  .values = *column[low]});
                }
                low = high + 1;
            }
            if (!key_region.regions.empty())
            {
                regions.key_regions.push_back(std::move(key_region));
            }
            first = last + 1;
        }
        if (!regions.key_regions.empty())
        {
            out = std::move(regions);
        }
    }

    return file;
}

// A bank of the Wii and its name.
struct WiiBank
{
    std::string name;
    ntdlib::rsar::BankFile::Files files;
};

// The waves of each wave archive of an archive of the 3DS, the Wii U or the Switch, by the wave archive's item, read
// once. A wave archive that the archive lacks holds none.
using WaveArchiveWaves = std::map<uint32_t, std::vector<std::vector<uint8_t>>>;

const std::vector<std::vector<uint8_t>>& WavesOf(const sar::SoundArchive& archive, uint32_t item,
                                                 WaveArchiveWaves& read)
{
    auto [place, added] = read.try_emplace(item);
    const std::optional<std::size_t> wave_archive =
        sar::ItemIndex(item, sar::kWaveArchiveItem, archive.WaveArchives().size());
    if (added && wave_archive)
    {
        const std::span<const uint8_t> bytes = song::ItemFile(
            archive, {.kind = song::ItemKind::kWaveArchive, .index = static_cast<uint32_t>(*wave_archive)});
        try
        {
            place->second =
                bytes.empty() ? std::vector<std::vector<uint8_t>>{} : sar::WaveArchiveFile::Read(bytes).waves;
        }
        catch (const std::exception&)
        {
            place->second.clear();
        }
    }

    return place->second;
}

// The name of bank `id` of `archive`, or BANK_ and its number for a bank without a name.
std::string BankNameOf(const sar::SoundArchive& archive, uint32_t id)
{
    const std::optional<std::size_t> index = sar::ItemIndex(id, sar::kBankItem, archive.Banks().size());
    return index && !archive.Banks()[*index].name.empty() ? archive.Banks()[*index].name
                                                          : "BANK_" + std::to_string(sar::ItemNumber(id));
}

// The bytes of bank `id`'s file in `archive`. Throws std::invalid_argument for a bank that the archive lacks.
std::span<const uint8_t> BankBytesOf(const sar::SoundArchive& archive, uint32_t id)
{
    const std::optional<std::size_t> index = sar::ItemIndex(id, sar::kBankItem, archive.Banks().size());
    if (!index)
    {
        throw std::invalid_argument("The archive has no such bank.");
    }
    const std::span<const uint8_t> file =
        song::ItemFile(archive, {.kind = song::ItemKind::kBank, .index = static_cast<uint32_t>(*index)});
    if (file.empty())
    {
        throw std::invalid_argument("The archive lacks its file.");
    }

    return file;
}

// The programs of bank `id` of `archive`: the entries of its table of instruments. 0 for a bank that can't come along.
uint32_t BankProgramsOf(const sar::SoundArchive& archive, uint32_t id)
{
    try
    {
        return static_cast<uint32_t>(
            sar::Bank::Parse(BankBytesOf(archive, id), archive.ReadOptions()).instruments.size());
    }
    catch (const std::exception&)
    {
        return 0;
    }
}

// `bank` with only the instruments of `programs`. Its table of waves holds only the waves that those instruments play,
// in their order. A region that plays a wave past the table still does.
sar::BankFile WithOnlyInstruments(sar::BankFile bank, const std::set<uint32_t>& programs)
{
    for (uint32_t program = 0; program < bank.instruments.size(); ++program)
    {
        if (!programs.contains(program))
        {
            bank.instruments[program].reset();
        }
    }
    while (!bank.instruments.empty() && !bank.instruments.back())
    {
        bank.instruments.pop_back();
    }

    // The table of waves, and the places of the old entries in it.
    std::map<uint32_t, uint32_t> places;
    std::vector<sar::WaveId> waves;
    for (const uint32_t entry : TableEntriesOf(bank, programs))
    {
        places[entry] = static_cast<uint32_t>(waves.size());
        waves.push_back(bank.waves[entry]);
    }
    const auto past = static_cast<uint32_t>(waves.size());
    bank.waves = std::move(waves);
    for (std::optional<sar::BankFile::Instrument>& instrument : bank.instruments)
    {
        if (!instrument)
        {
            continue;
        }

        for (sar::BankFile::KeyRegion& key_region : instrument->key_regions)
        {
            for (sar::BankFile::Region& region : key_region.regions)
            {
                const auto place = places.find(region.values.wave_id_index);
                region.values.wave_id_index = place != places.end() ? place->second : past;
            }
        }
    }

    return bank;
}

// A later console's bank with its waves as waves of the Wii (LaterBankOf). `left` counts the waves that the Wii doesn't
// play.
struct LaterBank
{
    sar::BankFile file;
    std::vector<std::optional<ntdlib::rsar::WaveFile>> waves;
    std::size_t left = 0;
};

// Bank `id` of `archive` as the later consoles' reader plays it (BankFileOfRegions), with each entry of its table of
// waves as a wave of the Wii (ntdlib::rsar::WiiWaveOf). An entry without a wave stands for a wave that the archive
// lacks, or a wave that the Wii doesn't play, such as one in IMA-ADPCM. With `kept`, the bank holds only the
// instruments of its programs, and the waves that they play. Throws for a bank that can't come along.
LaterBank LaterBankOf(const sar::SoundArchive& archive, uint32_t id, WaveArchiveWaves& read,
                      const std::optional<std::set<uint32_t>>& kept = std::nullopt)
{
    sar::BankFile file = BankFileOfRegions(sar::Bank::Parse(BankBytesOf(archive, id), archive.ReadOptions()));
    if (kept)
    {
        file = WithOnlyInstruments(std::move(file), *kept);
    }
    LaterBank bank{.file = std::move(file), .waves = {}};
    for (const sar::WaveId& wave : bank.file.waves)
    {
        const std::vector<std::vector<uint8_t>>& held = WavesOf(archive, wave.wave_archive_item, read);
        if (wave.index >= held.size())
        {
            bank.waves.emplace_back();
            continue;
        }
        try
        {
            bank.waves.push_back(ntdlib::rsar::WiiWaveOf(sar::WaveFile::Read(held[wave.index])));
        }
        catch (const std::exception&)
        {
            bank.waves.emplace_back();
            ++bank.left;
        }
    }

    return bank;
}

// The warning about `left` waves of bank `name` that the Wii doesn't play.
std::string LeftWavesWarning(std::size_t left, const std::string& name)
{
    return std::to_string(left) + " of the waves of bank " + name +
           (left == 1 ? " stays behind: a wave that the Wii doesn't play, such as a wave in IMA-ADPCM. Its regions "
                        "play nothing."
                      : " stay behind: waves that the Wii doesn't play, such as waves in IMA-ADPCM. Their regions "
                        "play nothing.");
}

// Bank `id` of `archive` as a bank of the Wii with an RWAR of its waves (ntdlib::rsar::WiiBankOf and WiiWaveOf). The
// bank plays the region that the later consoles' reader finds at each key and velocity (BankFileOfRegions). A wave that
// the archive lacks plays nothing on either console. A wave that the Wii can't play, such as one in IMA-ADPCM, stays
// behind with a warning, and its regions play nothing. A bank that can't come along stays behind with a warning, and
// the sound plays an empty bank in its place. With `kept`, the bank holds only the instruments of its programs.
WiiBank WiiBankFrom(Song& song, const sar::SoundArchive& archive, uint32_t id, WaveArchiveWaves& read,
                    const std::optional<std::set<uint32_t>>& kept)
{
    WiiBank bank{.name = BankNameOf(archive, id), .files = {}};
    try
    {
        const LaterBank later = LaterBankOf(archive, id, read, kept);
        bank.files = ntdlib::rsar::WiiBankOf(later.file, kWiiBankVersion, later.waves).Write();
        if (later.left > 0)
        {
            song.result.warnings.push_back(LeftWavesWarning(later.left, bank.name));
        }
    }
    catch (const std::exception& error)
    {
        song.result.warnings.push_back("Bank " + bank.name + " stays behind. " + format::AsSentence(error.what()) +
                                       " The sound plays an empty bank in its place.");
        bank.files = ntdlib::rsar::BankFile::FromInstruments(kWiiBankVersion, {}, {}).Write();
    }

    return bank;
}

// The instruments that slot `slot` keeps where a sound's bank takes only the instruments that the sound selects: the
// programs that the slot's notes play, and those of the notes whose slot a variable or a random number selects
// (WiiPrograms::played). Nothing where a variable can give the program of such a note: the slot keeps every instrument.
std::optional<std::set<uint32_t>> KeptOf(const WiiPrograms& programs, int slot)
{
    const auto played = [&programs](int of) -> std::optional<std::set<uint32_t>>
    {
        const auto found = programs.played.find(of);
        return found != programs.played.end() ? found->second : std::set<uint32_t>{};
    };
    std::optional<std::set<uint32_t>> kept = played(slot);
    const std::optional<std::set<uint32_t>> unknown = played(-1);
    if (!kept || !unknown)
    {
        return std::nullopt;
    }
    kept->insert(unknown->begin(), unknown->end());

    return kept;
}

// The banks of the slots that `programs` gives ranges as one bank of the Wii (WiiProgramsOf): each slot's instruments
// from the first program of its range. Each bank goes in as WiiBankFrom sends it. `banks` gives the bank in each slot
// of the sound. A slot without a bank gives its programs no instruments, and so does a bank that can't come along, with
// a warning. A wave that two banks share goes in once. The bank takes the names of the slots' banks. With `trim`, each
// slot keeps only the instruments that the sound selects (KeptOf).
WiiBank MergedWiiBank(Song& song, const sar::SoundArchive& archive, std::span<const uint32_t> banks,
                      const WiiPrograms& programs, WaveArchiveWaves& read, bool trim)
{
    const std::map<int, SlotPrograms>& slots = programs.slots;
    sar::BankFile merged;
    std::vector<std::optional<ntdlib::rsar::WaveFile>> waves;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> indexes;
    std::vector<std::string> names;
    std::vector<std::pair<std::string, std::string>> ranges;
    for (const auto& [slot, range] : slots)
    {
        const uint32_t id =
            static_cast<std::size_t>(slot) < banks.size() ? banks[static_cast<std::size_t>(slot)] : sar::kNoItem;
        std::optional<LaterBank> bank;
        if (id != sar::kNoItem)
        {
            const std::string name = BankNameOf(archive, id);
            try
            {
                bank = LaterBankOf(archive, id, read, trim ? KeptOf(programs, slot) : std::nullopt);
                const auto past = [&bank](const sar::BankFile::Region& region)
                {
                    return region.values.wave_id_index >= bank->waves.size();
                };
                const auto key_past = [&past](const sar::BankFile::KeyRegion& key)
                {
                    return std::ranges::any_of(key.regions, past);
                };
                const auto past_waves = [&key_past](const auto& instrument)
                {
                    return instrument && std::ranges::any_of(instrument->key_regions, key_past);
                };
                if (std::ranges::any_of(bank->file.instruments, past_waves))
                {
                    throw std::invalid_argument("Its regions play waves past its table of waves.");
                }
                names.push_back(name);
                if (range.count == 1)
                {
                    ranges.emplace_back(name, "at program " + std::to_string(range.first));
                }
                else if (range.count > 1)
                {
                    ranges.emplace_back(name, "at programs " + std::to_string(range.first) + " to " +
                                                  std::to_string(range.first + range.count - 1));
                }
                if (bank->left > 0)
                {
                    song.result.warnings.push_back(LeftWavesWarning(bank->left, name));
                }
            }
            catch (const std::exception& error)
            {
                song.result.warnings.push_back("Bank " + name + " stays behind. " + format::AsSentence(error.what()) +
                                               " Its programs play nothing.");
                bank.reset();
            }
        }

        // The slot's instruments, with the indexes of their waves in the new bank's table.
        for (uint32_t program = 0; program < range.count; ++program)
        {
            std::optional<sar::BankFile::Instrument>& instrument = merged.instruments.emplace_back();
            if (!bank || program >= bank->file.instruments.size() || !bank->file.instruments[program])
            {
                continue;
            }
            instrument = *bank->file.instruments[program];
            for (sar::BankFile::KeyRegion& key : instrument->key_regions)
            {
                for (sar::BankFile::Region& region : key.regions)
                {
                    const uint32_t index = region.values.wave_id_index;
                    const sar::WaveId& wave = bank->file.waves[index];
                    const auto [found, added] =
                        indexes.try_emplace({wave.wave_archive_item, wave.index}, merged.waves.size());
                    if (added)
                    {
                        merged.waves.push_back(wave);
                        waves.push_back(bank->waves[index]);
                    }
                    region.values.wave_id_index = found->second;
                }
            }
        }
    }

    WiiBank bank{.name = names.empty() ? "BANK_0" : names.front(), .files = {}};
    for (std::size_t n = 1; n < names.size(); ++n)
    {
        bank.name += "_" + names[n];
    }
    bank.files = ntdlib::rsar::WiiBankOf(merged, kWiiBankVersion, waves).Write();
    std::string warning = "A sound of the Wii plays one bank, and NW4R SND has no bank select. ";
    if (ranges.size() == 1)
    {
        warning += "The notes play the bank of one slot, " + ranges.front().first + ". It goes in " +
                   ranges.front().second + ".";
    }
    else if (!ranges.empty())
    {
        std::vector<std::string> placed;
        for (const auto& [name, place] : ranges)
        {
            placed.push_back(name + " " + place);
        }
        warning += "The notes play the banks of several slots. They go in as one bank, " + bank.name + ": " +
                   ListOf(placed) + ".";
    }
    else
    {
        warning +=
            "The notes play the banks of several slots. None of them comes along, and the sound plays an empty "
            "bank.";
    }
    std::vector<std::string> changed;
    if (programs.bank_selects > 0)
    {
        changed.push_back(programs.bank_selects == 1
                              ? "1 bank select became a program change"
                              : std::to_string(programs.bank_selects) + " bank selects became program changes");
    }
    if (programs.programs > 0)
    {
        changed.push_back(programs.programs == 1 ? "1 program change took the programs of its slot"
                                                 : std::to_string(programs.programs) +
                                                       " program changes took the programs of their slots");
    }
    if (!changed.empty())
    {
        warning += " " + ListOf(changed) + ".";
    }
    song.result.warnings.push_back(warning);

    return bank;
}

// ForOtherConsole, with the console whose sound the conversion started from as `origin` when it went to the Wii U
// first: the Wii. The warning about the biquad filters then names it, and not the Wii U.
OtherConsoleSong OtherConsoleFrom(const format::File& file, uint32_t sound, sar::Flavor to, MonoWaves mono,
                                  std::size_t ctr_wave_memory, const std::optional<song::Console>& origin)
{
    if (!file.native || !file.native->tables)
    {
        throw std::invalid_argument(
            "Hokuto can't read this archive's tables. A song for another console requires them.");
    }

    if (file.native->wii)
    {
        // A sound of the Wii goes to the Wii U first, and on from there to the 3DS or the Switch.
        OtherConsoleSong song = FromWii(file, sound);
        if (to == sar::Flavor::kCafe)
        {
            return song;
        }
        OtherConsoleSong next = OtherConsoleFrom(song.file, 0, to, mono, ctr_wave_memory,
                                                 song::Console{.wii = format::BuildOfWiiArchive(*file.native)});
        next.warnings.insert(next.warnings.begin(), song.warnings.begin(), song.warnings.end());
        return next;
    }

    const sar::Flavor from = file.native->flavor;
    if (to == from)
    {
        throw std::invalid_argument("The sound plays on the " + song::ConsoleName(to) + " already.");
    }
    const format::NativeTables& tables = *file.native->tables;
    if (sound >= tables.sounds.size() || tables.sounds[sound].kind != sar::SoundType::kSequence)
    {
        throw std::invalid_argument("Only a sequence sound goes to another console.");
    }
    const format::NativeSound& source = tables.sounds[sound];

    // The archive that the file exports, with its group files, and the sound's sequence.
    const std::vector<uint8_t> bytes = format::ExportArchive(file);
    const std::shared_ptr<const sar::SoundArchive> archive =
        song::LoadArchive(bytes, "the archive", format::ExportGroupFiles(file));
    const std::vector<std::optional<uint32_t>> sequences = format::SequencesOfFiles(file, bytes, *archive);
    const auto [sequence, start] = SequenceOf(file, *archive, sequences, sound);
    const uint32_t file_id = archive->Sounds()[sound].file_id;

    Song song;
    song.result.file = format::NewFile(format::EngineOf(to));

    // The 3DS's sound library plays 23 voices, and its DSP 24 channels of their waves. It drops the voice of the lowest
    // priority for a new one. The stereo waves go in as mono when the user asks, or when the sound holds more channels
    // at once than the 3DS plays.
    constexpr std::size_t kNotes = ntdlib::ctr::snd::VoiceManager::kVoiceCount;
    constexpr std::size_t kChannels = ntdlib::ctr::nnsnd::kNumSources;
    Held at_once;
    if (to == sar::Flavor::kCtr)
    {
        at_once = HeldOf(*archive, sound);
        song.result.stereo_waves = at_once.stereo;
        song.mono.on =
            at_once.stereo && (mono == MonoWaves::kOn || (mono == MonoWaves::kAuto && at_once.channels > kChannels));
        song.mono.pan_mode = ValueOf(source, SoundValue::kPanMode, 0);
        song.mono.pan_info = ntdlib::nwsnd::PanInfoOf(ValueOf(source, SoundValue::kPanCurve, 0));
        song.mono.notes = std::move(at_once.stereo_notes);
    }

    // The banks in the sound's slots, with their wave archives. The 3DS's model keeps the wave archives in its memory.
    const std::optional<Trim> trim = to == sar::Flavor::kCtr
                                         ? TrimFor(song, *archive, source.banks, sequence, start, ctr_wave_memory)
                                         : std::nullopt;
    std::vector<uint32_t> slots;
    for (const uint32_t bank : source.banks)
    {
        slots.push_back(bank == sar::kNoItem ? sar::kNoItem
                                             : AddBankOf(song, *archive, bank, to, trim ? &*trim : nullptr));
    }
    if (to == sar::Flavor::kCtr && song.wave_bytes > ctr_wave_memory)
    {
        throw std::invalid_argument("The wave archives of the sound's banks take " + SizeText(song.wave_bytes) +
                                    (trim ? ", even with only the instruments that the sound selects" : "") +
                                    ". Hokuto's model of the 3DS holds " + SizeText(ctr_wave_memory) +
                                    " of wave archives.");
    }
    if (trim)
    {
        song.result.warnings.push_back(
            "The banks go in with only the instruments of program 0 and of the programs that the sound selects. Their "
            "wave archives hold only those instruments' waves: " +
            SizeText(song.wave_bytes) + ". Whole, the wave archives would take " + SizeText(trim->whole) +
            ". Hokuto's model of the 3DS holds " + SizeText(ctr_wave_memory) + " of wave archives.");
    }
    song.result.mono_waves = song.mono.on && song.mono.count > 0;
    if (song.result.mono_waves)
    {
        const bool one = song.mono.count == 1;
        std::string warning =
            std::to_string(song.mono.count) +
            (one ? " stereo wave goes in as mono. It holds" : " stereo waves go in as mono. Each holds") +
            " the mean of its two channels.";
        if (at_once.channels > kChannels)
        {
            warning += " In stereo, up to " + std::to_string(at_once.channels) +
                       " channels of the sound's waves sound at once before their releases. The 3DS plays " +
                       std::to_string(kChannels) + ".";
        }
        if (song.mono.regions > 0 && song.mono.most_db > 0.05)
        {
            std::array<char, 48> db{};
            if (std::abs(song.mono.most_db - song.mono.least_db) < 0.05)
            {
                std::snprintf(db.data(), db.size(), "%.1f dB", song.mono.most_db);
            }
            else
            {
                std::snprintf(db.data(), db.size(), "%.1f to %.1f dB", song.mono.least_db, song.mono.most_db);
            }
            warning += std::string(" The mean rises by ") + db.data() +
                       " to keep the power of the two channels at the pans of the notes. The samples take as much of "
                       "the rise as their peak allows, up to the rise in the centre. The regions that play " +
                       (one ? "the wave" : "the waves") + " take the rest.";
        }
        if (song.mono.held > 0)
        {
            warning += " " + std::to_string(song.mono.held) +
                       (song.mono.held == 1 ? " region stays at volume 255, the highest, and plays"
                                            : " regions stay at volume 255, the highest, and play") +
                       " quieter than in stereo.";
        }
        if (song.mono.lowest_ratio)
        {
            std::array<char, 16> ratio{};
            std::snprintf(ratio.data(), ratio.size(), "%.1f", *song.mono.lowest_ratio);
            warning += one ? std::string(
                                 " The wave is in DSP-ADPCM and goes in encoded again. Its ratio of signal to "
                                 "noise is ") +
                                 ratio.data() + " dB."
                           : std::string(
                                 " The waves in DSP-ADPCM go in encoded again. Their ratio of signal to noise "
                                 "is ") +
                                 ratio.data() + " dB or more.";
        }
        song.result.warnings.push_back(
            warning + (one ? " The wave loses its stereo width." : " The waves lose their stereo width."));
    }
    const std::size_t channels = song.result.mono_waves ? at_once.mono_channels : at_once.channels;
    if (at_once.notes > kNotes || channels > kChannels)
    {
        song.result.warnings.push_back(
            "The 3DS plays " + std::to_string(kNotes) + " notes and " + std::to_string(kChannels) +
            " channels of their waves at once. Up to " + std::to_string(at_once.notes) +
            " of the sound's notes sound at once before their releases, with " + std::to_string(channels) +
            " channels. The 3DS drops the notes of the lowest priority for the others.");
    }
    if (!song.older_waves.empty())
    {
        song.result.warnings.push_back(
            "The waves of " + ListOf(song.older_waves) + " go in as version " +
            song::VersionText(to, song::ExportVersions(song::ItemKind::kWave, to).file) + ", the oldest that the " +
            song::ConsoleName(to) +
            "'s games hold. That version holds no original start of a loop. Playback starts each loop at its start "
            "either way.");
    }

    // The sound, with the source sound's name and values. The sequence file takes the version that Export… writes for
    // the other console. Commands that have to keep their places go in byte for byte, converted. The 3DS reads the
    // settings of LFOs 1 to 3 without their bytes, and such a command can't keep its place. Other commands leave the
    // sequence where they play otherwise on the other console. From the 3DS, volume2 and the main volume lose their
    // times (WithoutCtrTimes). Commands that keep their places keep the times too. The biquad filters take the types
    // and values that come nearest on the other console (MapBiquads), in their places or not.
    const std::string name = source.name.empty() ? "SEQ_" + std::to_string(sound) : source.name;
    const song::Console reads{.flavor = from};
    const song::Console writes{.flavor = to};
    std::optional<uint32_t> new_sequence;
    if (Fixed(sequence))
    {
        if (!SameLengths(sequence, from, to))
        {
            throw std::invalid_argument("The sequence file's commands have to keep their places, and the " +
                                        song::ConsoleName(to) + " reads some of them with other lengths.");
        }
        song::ConvertedSequence converted = song::OtherConsoleSequenceFile(*archive, sound, to);
        song.result.file = WithFixedSequence(song.result.file, name, std::move(converted.file),
                                             archive->Sounds()[sound].start_offset, slots);
        song.result.warnings.push_back(
            "The sequence file's commands have to keep their places. The file goes in converted with each command in "
            "its place.");
        if (const std::optional<std::string> warning =
                SkippedWarning(CountSkipped(sequence, reads, writes), reads, writes, false))
        {
            song.result.warnings.push_back(*warning);
        }
        ntdlib::commands::Sequence timed = sequence;
        if (const std::size_t times = from == sar::Flavor::kCtr ? WithoutCtrTimes(timed) : 0; times > 0)
        {
            song.result.warnings.push_back(CtrTimesWarning(times, to, false));
        }
        if (const std::optional<std::string> warning =
                song::BiquadWarning(converted.biquads, origin.value_or(reads), writes))
        {
            song.result.warnings.push_back(*warning);
        }
    }
    else
    {
        auto [kept, kept_start] = WithoutSkipped(sequence, start, reads, writes);
        const std::size_t times = from == sar::Flavor::kCtr ? WithoutCtrTimes(kept) : 0;
        const song::BiquadChanges biquads =
            song::MapBiquads(kept, std::span<const uint32_t>(&kept_start, 1), reads, writes);
        song.result.file = AddSequenceSound(song.result.file, std::nullopt, name, kept, kept_start, {}, slots);
        new_sequence = static_cast<uint32_t>(song.result.file.sequences.size() - 1);
        if (const std::optional<std::string> warning =
                SkippedWarning(CountSkipped(sequence, reads, writes), reads, writes, true))
        {
            song.result.warnings.push_back(*warning);
        }
        if (times > 0)
        {
            song.result.warnings.push_back(CtrTimesWarning(times, to, true));
        }
        if (const std::optional<std::string> warning = song::BiquadWarning(biquads, origin.value_or(reads), writes))
        {
            song.result.warnings.push_back(*warning);
        }
    }
    const auto added = static_cast<uint32_t>(song.result.file.native->tables->sounds.size() - 1);
    CopyValues(song, added, source);
    std::optional<sar::SoundModel> settings;
    try
    {
        settings = sar::ReadArchiveModel(bytes, from).sounds.at(sound);
    }
    catch (const std::exception& error)
    {
        song.result.warnings.push_back(std::string(kSettingsLeft) + "Hokuto can't read them from this archive. " +
                                       format::AsSentence(error.what()));
    }
    Settle(song, added, settings);

    // The version of the sequence file, and the other sounds whose commands it holds.
    const uint32_t held = sar::SequenceFile::Read(archive->FileData(file_id)).version;
    const uint32_t version = song::ExportVersions(song::ItemKind::kSequence, to).file;
    if (song::VersionsCompare(from, to) && held > version)
    {
        song.result.warnings.push_back("The sequence file goes in as version " + song::VersionText(to, version) +
                                       ", the oldest that the " + song::ConsoleName(to) + "'s games hold. It held " +
                                       song::VersionText(from, held) + ". Its commands stay the same.");
    }
    const std::size_t sharing = song::SoundsSharingFile(*archive, sound).size();
    if (sharing > 0)
    {
        song.result.warnings.push_back("The sequence file holds the commands of " + std::to_string(sharing) +
                                       (sharing == 1 ? " other sound" : " other sounds") +
                                       " too. They go along unused.");
    }

    CopyParts(song, file, sound, added, file_id < sequences.size() ? sequences[file_id] : std::nullopt, new_sequence);

    return std::move(song.result);
}

} // namespace

OtherConsoleSong ForOtherConsole(const format::File& file, uint32_t sound, sar::Flavor to, MonoWaves mono,
                                 std::size_t ctr_wave_memory)
{
    return OtherConsoleFrom(file, sound, to, mono, ctr_wave_memory, std::nullopt);
}

OtherConsoleSong ForWii(const format::File& file, uint32_t sound, const std::optional<ntdlib::SndBuild>& wii_build,
                        std::size_t wii_wave_memory)
{
    if (!file.native || !file.native->tables)
    {
        throw std::invalid_argument(
            "Hokuto can't read this archive's tables. A song for another console requires them.");
    }
    if (file.native->wii)
    {
        throw std::invalid_argument("The sound plays on the Wii already.");
    }

    const format::NativeTables& tables = *file.native->tables;
    if (sound >= tables.sounds.size() || tables.sounds[sound].kind != sar::SoundType::kSequence)
    {
        throw std::invalid_argument("Only a sequence sound goes to another console.");
    }
    const format::NativeSound& source = tables.sounds[sound];
    const sar::Flavor from = file.native->flavor;

    // The archive that the file exports, with its group files, and the sound's sequence.
    const std::vector<uint8_t> bytes = format::ExportArchive(file);
    const std::shared_ptr<const sar::SoundArchive> archive =
        song::LoadArchive(bytes, "the archive", format::ExportGroupFiles(file));
    const std::vector<std::optional<uint32_t>> sequences = format::SequencesOfFiles(file, bytes, *archive);
    const auto [sequence, start] = SequenceOf(file, *archive, sequences, sound);
    if (Fixed(sequence))
    {
        throw std::invalid_argument(
            "The sequence file's commands have to keep their places. Hokuto can't send such a sequence to the Wii "
            "yet.");
    }
    const uint32_t file_id = archive->Sounds()[sound].file_id;

    Song song;
    song.result.file = format::NewFile(format::Engine::kWii);
    if (wii_build)
    {
        format::Model built = format::ModelOf(song.result.file);
        built.wii_build = *wii_build;
        song.result.file = format::WithModel(song.result.file, built);
    }
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*song.result.file.native);

    // A sound of the Wii plays one bank, and NW4R SND has no bank select. The banks of the slots that the notes play go
    // in as one bank, one slot after another, and the program changes select their slots' programs there
    // (WiiProgramsOf).
    std::vector<uint32_t> slot_programs;
    for (const uint32_t id : source.banks)
    {
        slot_programs.push_back(id == sar::kNoItem ? 0 : BankProgramsOf(*archive, id));
    }
    const std::optional<WiiPrograms> programs = WiiProgramsOf(sequence, start, slot_programs);
    const bool merged = programs && programs->slots.size() > 1;
    const ntdlib::commands::Sequence& played = merged ? programs->sequence : sequence;

    // The commands that the Wii's build of NW4R SND skips stay out. The others go in as NW4R SND reads them (AsWii),
    // with commands at the sound's start that give the global variables the later consoles' starting value. The biquad
    // filters of the Switch take the Wii's nearest (MapBiquads).
    const song::Console reads{.flavor = from};
    const song::Console writes{.wii = build};
    auto [kept, kept_start] = WithoutSkipped(played, start, reads, writes);
    WiiCommands commands = AsWii(std::move(kept), from, build);
    const song::BiquadChanges biquads =
        song::MapBiquads(commands.sequence, std::span<const uint32_t>(&kept_start, 1), reads, writes);
    const auto [new_start, globals] = WithStartingGlobals(commands.sequence, kept_start, 0, -1);
    std::vector<format::Shared<ntdlib::commands::Sequence>> plain =
        format::WithoutChoices({format::Shared<ntdlib::commands::Sequence>(std::move(commands.sequence))});
    if (const std::optional<std::string> warning =
            SkippedWarning(CountSkipped(played, reads, writes), reads, writes, true))
    {
        song.result.warnings.push_back(*warning);
    }
    AddTimeWarnings(commands, song.result.warnings);
    if (const std::optional<std::string> warning = song::BiquadWarning(biquads, reads, writes))
    {
        song.result.warnings.push_back(*warning);
    }
    if (!globals.empty())
    {
        song.result.warnings.push_back(GlobalsWarning(globals, true));
    }

    // The banks of the slots that the notes play, or the bank in the sound's first slot. The Wii's model keeps the
    // bank's waves in MEM2. When they don't fit, the bank takes only the instruments that the sound selects.
    WaveArchiveWaves read;
    const uint32_t first = source.banks.empty() ? sar::kNoItem : source.banks.front();
    const std::size_t warned = song.result.warnings.size();
    const auto bank_of = [&](bool trim)
    {
        if (merged)
        {
            return MergedWiiBank(song, *archive, source.banks, *programs, read, trim);
        }
        if (first != sar::kNoItem)
        {
            return WiiBankFrom(song, *archive, first, read, trim ? KeptOf(*programs, 0) : std::nullopt);
        }

        song.result.warnings.push_back(
            "The sound's first bank slot is empty. A sound of the Wii plays one bank, and the sound plays an empty "
            "bank.");

        return WiiBank{"BANK_0", ntdlib::rsar::BankFile::FromInstruments(kWiiBankVersion, {}, {}).Write()};
    };
    WiiBank bank = bank_of(false);
    const std::size_t whole = bank.files.wave_data.size();
    if (whole > wii_wave_memory)
    {
        const std::string holds = "Hokuto's model of the Wii holds " + SizeText(wii_wave_memory) + " of waves.";
        if (!programs)
        {
            throw std::invalid_argument("The waves of the sound's bank take " + SizeText(whole) + ". " + holds +
                                        " Hokuto can't follow the sequence's programs, and can't tell which "
                                        "instruments the sound leaves unplayed.");
        }
        song.result.warnings.resize(warned);
        bank = bank_of(true);
        const std::size_t kept = bank.files.wave_data.size();
        if (kept > wii_wave_memory)
        {
            const bool whole_slots =
                std::ranges::any_of(programs->played, [](const auto& slot) { return !slot.second.has_value(); });
            throw std::invalid_argument(
                "The waves of the sound's bank take " + SizeText(kept) +
                ", even with only the instruments that the sound selects. " + holds +
                (whole_slots ? " A variable decides some of the sound's programs, and their banks keep every "
                               "instrument."
                             : ""));
        }
        song.result.warnings.push_back(
            "The bank goes in with only the instruments that the sound selects. Its waves "
            "take " +
            SizeText(kept) + ". With every instrument, they would take " + SizeText(whole) + ". " + holds);
    }
    if (merged)
    {
        for (const auto& [slot, range] : programs->slots)
        {
            song.result.slot_programs[slot] = range.first;
        }
    }

    // The banks of the other slots stay behind.
    std::vector<std::string> others;
    for (std::size_t slot = 0; slot < source.banks.size(); ++slot)
    {
        const uint32_t id = source.banks[slot];
        if (id != sar::kNoItem && (merged ? !programs->slots.contains(static_cast<int>(slot)) : slot > 0))
        {
            others.push_back(BankNameOf(*archive, id));
        }
    }
    if (!others.empty())
    {
        const bool one = others.size() == 1;
        std::string stays = (one ? "The bank in the sound's other slot stays behind: "
                                 : "The banks in the sound's other slots stay behind: ") +
                            ListOf(others) + ".";
        if (!programs)
        {
            stays +=
                " A sound of the Wii plays one bank. The notes that select another slot play the first slot's "
                "bank. Hokuto can't follow the sequence's bank selects.";
        }
        else if (programs->unknown_slots)
        {
            stays += one ? " Only a variable or a random number selects its slot."
                         : " Only a variable or a random number selects their slots.";
        }
        else
        {
            stays += one ? " None of the sound's notes plays it." : " None of the sound's notes plays them.";
        }
        song.result.warnings.push_back(stays);
    }
    if (programs && programs->unsure_notes > 0)
    {
        const bool one = programs->unsure_notes == 1;
        song.result.warnings.push_back(
            std::to_string(programs->unsure_notes) +
            (one ? " of the sequence's note commands may play another instrument on the Wii than on the "
                 : " of the sequence's note commands may play other instruments on the Wii than on the ") +
            song::ConsoleName(from) +
            (one ? ". A variable or a random number decides its bank slot or program, or the tracks reach it with more "
                   "than one."
                 : ". A variable or a random number decides their bank slots or programs, or the tracks reach them "
                   "with more than one."));
    }

    // The sound's settings that the Sound sections don't edit: the remote filter goes along. The 3D settings, the
    // single play and the user parameters stay behind: NW4R SND reads other fields.
    std::optional<sar::SoundModel> settings;
    try
    {
        settings = sar::ReadArchiveModel(bytes, from).sounds.at(sound);
    }
    catch (const std::exception& error)
    {
        song.result.warnings.push_back(std::string(kSettingsLeft) + "Hokuto can't read them from this archive. " +
                                       format::AsSentence(error.what()));
    }
    if (settings &&
        (settings->single_play || settings->option_17 || settings->settings_3d != sar::Sound3dModel{} ||
         std::ranges::any_of(settings->user_options, [](const auto& option) { return option.has_value(); })))
    {
        song.result.warnings.push_back(
            "The sound's 3D settings, single play and user parameters stay behind. NW4R SND reads other fields.");
    }

    // The new file's archive: the bank, the sequence file and the sound.
    const std::string name = source.name.empty() ? "SEQ_" + std::to_string(sound) : source.name;
    format::Model model = format::ModelOf(song.result.file);
    ntdlib::rsar::ArchiveModel& wii = *model.wii;
    const auto bank_file = static_cast<uint32_t>(wii.files.size());
    wii.files.push_back({.bytes = std::move(bank.files.file), .wave_data = std::move(bank.files.wave_data)});
    wii.banks.push_back({.name = bank.name, .file_id = bank_file});
    const auto sequence_file = static_cast<uint32_t>(wii.files.size());
    wii.files.emplace_back();
    const auto added = static_cast<uint32_t>(wii.sounds.size());
    wii.sounds.push_back({.name = name,
                          .type = ntdlib::rsar::SoundType::kSequence,
                          .file_id = sequence_file,
                          .player_id = 0,
                          .bank = static_cast<uint32_t>(wii.banks.size() - 1)});
    const auto new_sequence = static_cast<uint32_t>(song.result.file.sequences.size());
    song.result.file.sequences.push_back(std::move(plain.front()));
    model.sequences[sequence_file] = {.sequence = new_sequence, .version = kWiiSequenceVersion};
    model.starts[added] = new_start;
    try
    {
        song.result.file = format::WithModel(song.result.file, model);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't write the sound's sequence for the Wii. ") +
                                    format::AsSentence(error.what()));
    }

    // The sound's values in the archive's tables. NW4R SND plays a sound's volume above 127 at 127.
    format::NativeSound values = source;
    if (values.volume > 127)
    {
        std::array<char, 16> db{};
        std::snprintf(db.data(), db.size(), "%.1f", 20.0 * std::log10(static_cast<double>(values.volume) / 127.0));
        song.result.warnings.push_back("The sound's volume of " + std::to_string(values.volume) +
                                       " goes in as 127, and the sound plays " + db.data() +
                                       " dB quieter. NW4R SND plays a sound's volume above 127 at 127.");
        values.volume = 127;
    }
    CopyValues(song, added, values);

    // The file as a save and an open give it: the archive that its model lays out, with the sound's remote filter.
    format::Model settled = format::ModelOf(song.result.file);
    settled.wii->sounds.at(added).remote_filter = settings ? settings->remote_filter : uint8_t{0};
    song.result.file = format::WithModel(song.result.file, settled);

    // The other sounds whose commands the sequence file holds.
    const std::size_t sharing = song::SoundsSharingFile(*archive, sound).size();
    if (sharing > 0)
    {
        song.result.warnings.push_back("The sequence file holds the commands of " + std::to_string(sharing) +
                                       (sharing == 1 ? " other sound" : " other sounds") +
                                       " too. They go along unused.");
    }

    CopyParts(song, file, sound, added, file_id < sequences.size() ? sequences[file_id] : std::nullopt, new_sequence);

    return std::move(song.result);
}

std::optional<std::string> EngineRefused(const format::File& file, format::Engine engine)
{
    if (engine == file.engine)
    {
        return std::nullopt;
    }
    if (!file.native || !file.native->tables)
    {
        return "Hokuto can't read this archive's tables. A sound on another console's engine requires them.";
    }

    return std::nullopt;
}

OtherConsoleSong ForEngine(const format::File& file, uint32_t sound, format::Engine engine)
{
    if (const std::optional<std::string> refused = EngineRefused(file, engine))
    {
        throw std::invalid_argument(*refused);
    }
    if (engine == file.engine)
    {
        throw std::invalid_argument("The sound plays on the file's engine already.");
    }

    const std::optional<sar::Flavor> to = format::FlavorOf(engine);
    return to ? ForOtherConsole(FoldTimelines(file), sound, *to) : ForWii(FoldTimelines(file), sound);
}

CommandsForWii WiiCommandsOf(const ntdlib::commands::Sequence& sequence, sar::Flavor from,
                             const ntdlib::SndBuild& build)
{
    if (Fixed(sequence))
    {
        throw std::invalid_argument(
            "The sequence file's commands have to keep their places. Hokuto can't put such a sequence into an archive "
            "of the Wii yet.");
    }

    // The commands that the build skips stay out. The others go in as NW4R SND reads them (AsWii), without the
    // encoder's choices of the other console. The biquad filters of the Switch take the Wii's nearest (MapBiquads).
    const song::Console reads{.flavor = from};
    const song::Console writes{.wii = build};
    WiiCommands commands = AsWii(WithoutSkipped(sequence, 0, reads, writes).first, from, build);
    const song::BiquadChanges biquads = song::MapBiquads(commands.sequence, {}, reads, writes);
    CommandsForWii result{
        .sequence =
            *format::WithoutChoices({format::Shared<ntdlib::commands::Sequence>(std::move(commands.sequence))}).front(),
        .warnings = {}};
    if (const std::optional<std::string> warning =
            SkippedWarning(CountSkipped(sequence, reads, writes), reads, writes, true))
    {
        result.warnings.push_back(*warning);
    }
    AddTimeWarnings(commands, result.warnings);
    if (const std::optional<std::string> warning = song::BiquadWarning(biquads, reads, writes))
    {
        result.warnings.push_back(*warning);
    }

    return result;
}

std::pair<uint32_t, std::vector<int>> WithLaterGlobals(ntdlib::commands::Sequence& sequence, uint32_t start)
{
    return WithStartingGlobals(sequence, start, 0, -1);
}

std::string LaterGlobalsWarning(std::span<const int> globals)
{
    return GlobalsWarning(globals, true);
}

ntdlib::rsar::BankFile WiiBankOfFile(std::span<const uint8_t> bytes, sar::Flavor flavor, uint16_t version,
                                     const std::vector<std::optional<ntdlib::rsar::WaveFile>>& waves)
{
    sar::Bank bank;
    try
    {
        bank = sar::Bank::Parse(bytes, sar::Options{.flavor = flavor});
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the bank. ") + format::AsSentence(error.what()));
    }

    return ntdlib::rsar::WiiBankOf(BankFileOfRegions(bank), version, waves);
}

namespace
{

// `sequence` encoded with `grammar`, and the offset of each of its labels. Throws std::invalid_argument with a message
// for the user when the encoder can't write the commands.
std::pair<std::vector<uint8_t>, std::vector<ntdlib::nwsnd::LabelOffset>> EncodedFile(
    const ntdlib::commands::Sequence& sequence, const ntdlib::nwsnd::Grammar& grammar, const std::string& console)
{
    ntdlib::nwsnd::Encoded encoded;
    try
    {
        encoded = ntdlib::nwsnd::EncodeSequence(sequence, grammar);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument("The " + console + " can't hold the sequence file's commands. " +
                                    format::AsSentence(error.what()));
    }
    std::vector<ntdlib::nwsnd::LabelOffset> labels;
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        labels.push_back({label.name, encoded.offsets[label.item]});
    }

    return {std::move(encoded.bytes), std::move(labels)};
}

} // namespace

ConvertedItem WiiSequenceFileOf(const sar::SoundArchive& archive, uint32_t sound)
{
    // The commands as the console reads them, from the labels and the starts of the sounds that play the file.
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != sar::SoundType::kSequence)
    {
        throw std::invalid_argument("The archive has no such sequence sound.");
    }

    const sar::Flavor from = archive.ReadOptions().flavor;
    std::span<const uint8_t> bytes;
    sar::SequenceFile read;
    try
    {
        bytes = song::ItemFile(archive, {.kind = song::ItemKind::kSequence, .index = sound});
        read = sar::SequenceFile::Read(bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the sequence file. ") +
                                    format::AsSentence(error.what()));
    }
    std::vector<uint32_t> starts = {archive.Sounds()[sound].start_offset};
    for (const uint32_t other : song::SoundsSharingFile(archive, sound))
    {
        starts.push_back(archive.Sounds()[other].start_offset);
    }
    const ntdlib::commands::Sequence sequence = SequenceFileCommands(read, bytes, starts);
    if (Fixed(sequence))
    {
        throw std::invalid_argument(
            "The sequence file's commands have to keep their places. Hokuto can't convert such a sequence for the Wii "
            "yet.");
    }

    // The commands as the build of a new file of the Wii reads them.
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*format::NewFile(format::Engine::kWii).native);
    CommandsForWii commands = WiiCommandsOf(sequence, from, build);
    auto [data, offsets] = EncodedFile(commands.sequence, song::GrammarOf(song::Console{.wii = build}), "Wii");
    ntdlib::rsar::SequenceFile file;
    file.version = kWiiSequenceVersion;
    file.data = std::move(data);
    for (const ntdlib::nwsnd::LabelOffset& label : offsets)
    {
        file.labels.push_back({label.name, label.offset});
    }

    return {.file = file.Write(), .waves = {}, .warnings = std::move(commands.warnings)};
}

ConvertedItem WiiBankFilesOf(const sar::SoundArchive& archive, uint32_t index)
{
    const uint32_t id = sar::ItemId(sar::kBankItem, index);
    WaveArchiveWaves read;
    try
    {
        const LaterBank later = LaterBankOf(archive, id, read);
        ntdlib::rsar::BankFile::Files files = ntdlib::rsar::WiiBankOf(later.file, kWiiBankVersion, later.waves).Write();
        ConvertedItem result{.file = std::move(files.file), .waves = std::move(files.wave_data), .warnings = {}};
        if (later.left > 0)
        {
            result.warnings.push_back(LeftWavesWarning(later.left, BankNameOf(archive, id)));
        }

        return result;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("Hokuto can't convert bank " + BankNameOf(archive, id) + " for the Wii. " +
                                    format::AsSentence(error.what()));
    }
}

ConvertedItem WiiWaveSoundFilesOf(const sar::SoundArchive& archive, uint32_t sound)
{
    try
    {
        // Each entry of the file's table of waves takes its wave as a wave of the Wii.
        const sar::WaveSoundFile file = sar::WaveSoundFile::Parse(
            song::ItemFile(archive, {.kind = song::ItemKind::kWaveSoundFile, .index = sound}), archive.ReadOptions());
        WaveArchiveWaves read;
        std::vector<std::optional<ntdlib::rsar::WaveFile>> waves;
        std::size_t left = 0;
        for (const sar::WaveId& wave : file.waves)
        {
            const std::vector<std::vector<uint8_t>>& held = WavesOf(archive, wave.wave_archive_item, read);
            if (wave.index >= held.size())
            {
                waves.emplace_back();
                continue;
            }
            try
            {
                waves.push_back(ntdlib::rsar::WiiWaveOf(sar::WaveFile::Read(held[wave.index])));
            }
            catch (const std::exception&)
            {
                waves.emplace_back();
                ++left;
            }
        }

        ntdlib::rsar::WsdFile::Files files = ntdlib::rsar::WiiWsdOf(file, kWiiWaveSoundVersion, waves).Write();
        ConvertedItem result{.file = std::move(files.file), .waves = std::move(files.wave_data), .warnings = {}};
        if (left > 0)
        {
            result.warnings.push_back(
                std::to_string(left) +
                (left == 1 ? " of the file's waves stays behind: a wave that the Wii doesn't play, such as a wave in "
                             "IMA-ADPCM. Its notes play nothing."
                           : " of the file's waves stay behind: waves that the Wii doesn't play, such as waves in "
                             "IMA-ADPCM. Their notes play nothing."));
        }

        return result;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't convert the wave sound file for the Wii. ") +
                                    format::AsSentence(error.what()));
    }
}

ConvertedItem LaterSequenceFileOf(const ntdlib::rsar::SoundArchive& archive, uint32_t sound,
                                  const ntdlib::SndBuild& build, sar::Flavor to)
{
    // The commands as the build reads them, from the labels and the starts of the sounds that play the file.
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != ntdlib::rsar::SoundType::kSequence)
    {
        throw std::invalid_argument("The archive has no such sequence sound.");
    }

    const uint32_t file_id = archive.Sounds()[sound].file_id;
    const std::optional<ntdlib::rsar::FileData> data = archive.File(file_id);
    if (!data)
    {
        throw std::invalid_argument("The archive lacks the sound's sequence file.");
    }
    ntdlib::rsar::SequenceFile read;
    try
    {
        read = ntdlib::rsar::SequenceFile::Read(data->file);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the sequence file. ") +
                                    format::AsSentence(error.what()));
    }
    std::vector<uint32_t> starts;
    for (const ntdlib::rsar::SoundInfo& info : archive.Sounds())
    {
        if (info.type == ntdlib::rsar::SoundType::kSequence && info.file_id == file_id)
        {
            starts.push_back(info.start_offset);
        }
    }
    const song::Console reads{.wii = build};
    const ntdlib::commands::Sequence sequence = SequenceFileCommands(read, data->file, build, starts);

    // The commands as the later consoles read them, without those that the build skips and the bytes that no command
    // reaches. The biquad filters take the other console's types and values.
    const song::Console writes{.flavor = to};
    const ntdlib::commands::Sequence later = AsLaterConsoles(sequence, build);
    std::vector<bool> leaves = DeadData(later);
    for (std::size_t item = 0; item < later.items.size(); ++item)
    {
        leaves[item] = leaves[item] || PlaysOtherwise(later.items[item], reads, writes);
    }
    ntdlib::commands::Sequence kept = WithoutItems(later, 0, leaves).first;
    if (Fixed(kept))
    {
        throw std::invalid_argument(
            "The sequence file's commands have to keep their places. Hokuto can't convert such a sequence of the Wii "
            "for another console yet.");
    }
    const song::BiquadChanges biquads = song::MapBiquads(kept, {}, reads, writes);
    ConvertedItem result;
    if (const std::optional<std::string> warning =
            SkippedWarning(CountSkipped(later, reads, writes), reads, writes, true))
    {
        result.warnings.push_back(*warning);
    }
    if (const std::optional<std::string> warning = song::BiquadWarning(biquads, reads, writes))
    {
        result.warnings.push_back(*warning);
    }

    const ntdlib::commands::Sequence plain =
        *format::WithoutChoices({format::Shared<ntdlib::commands::Sequence>(std::move(kept))}).front();
    auto [bytes, offsets] = EncodedFile(plain, song::GrammarOf(to), song::ConsoleName(to));
    sar::SequenceFile file;
    file.flavor = to;
    file.version = song::ExportVersions(song::ItemKind::kSequence, to).file;
    file.data = std::move(bytes);
    for (const ntdlib::nwsnd::LabelOffset& label : offsets)
    {
        file.labels.push_back({label.name, label.offset});
    }
    result.file = file.Write();

    return result;
}

ConvertedItem LaterBankFilesOf(const ntdlib::rsar::SoundArchive& archive, uint32_t index, const ntdlib::SndBuild& build,
                               sar::Flavor to)
{
    if (index >= archive.Banks().size())
    {
        throw std::invalid_argument("The archive has no such bank.");
    }

    const uint32_t file_id = archive.Banks()[index].file_id;
    const std::optional<ntdlib::rsar::FileData> data = archive.File(file_id);
    if (!data)
    {
        throw std::invalid_argument("The archive lacks its file.");
    }
    if (archive.WaveDataMissing(file_id))
    {
        throw std::invalid_argument("The archive lacks its waves.");
    }

    // The bank's waves go into a wave archive of their own, in the bank's order.
    const ntdlib::rsar::Bank read =
        ntdlib::rsar::Bank::Parse(data->file, data->wave_data, ntdlib::rvl::snd::kRsarOptions);
    const song::FileVersions versions = song::ExportVersions(song::ItemKind::kWaveArchive, to);
    sar::WaveArchiveFile wave_archive{.flavor = to, .version = versions.file, .waves = {}};
    for (const ntdlib::rsar::Wave& wave : read.Waves())
    {
        wave_archive.waves.push_back(ntdlib::rsar::WaveFileOf(wave, to, versions.waves).Write());
    }

    return {.file = ntdlib::rsar::BankFileOf(read, build, to, song::ExportVersions(song::ItemKind::kBank, to).file,
                                             kWiiWaves)
                        .Write(),
            .waves = wave_archive.Write(),
            .warnings = {}};
}

} // namespace hokuto::edit
