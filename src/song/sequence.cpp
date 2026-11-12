// SPDX-License-Identifier: MIT

#include "song/sequence.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/mp2k/performance.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "xsflib/binary.h"

#include "song/archive.h"
#include "song/ds.h"
#include "song/gba.h"
#include "song/sequence_parts.h"

namespace hokuto::song
{
namespace
{

namespace nwsnd = ntdlib::nwsnd;
namespace sar = ntdlib::sar;
namespace export_ = ntdlib::sequence_export;

using sequence_parts::AddTrack;
using sequence_parts::AddUnstarted;
using sequence_parts::ClipToLoop;
using sequence_parts::FileCommands;
using sequence_parts::GameChanges;
using sequence_parts::ProgramPreset;
using sequence_parts::ReadReason;
using sequence_parts::ReadTrack;
using sequence_parts::SequenceHead;
using sequence_parts::SetCallsAndVariables;

// The volume at which MP2K plays a track until a command sets it.
constexpr int kMp2kStartVolume = 0;

} // namespace

namespace sequence_parts
{

// Adds a setting from `tick` on. A later setting at the same tick replaces the earlier one. A value that doesn't change
// the setting is left out.
void AddSetting(std::vector<Setting>& settings, uint32_t tick, int value, int fallback)
{
    if (!settings.empty() && settings.back().tick == tick)
    {
        settings.pop_back();
    }

    const int current = settings.empty() ? fallback : settings.back().value;
    if (value != current)
    {
        settings.push_back({tick, value});
    }
}

void ClipToLoop(const Sequence& sequence, Track& track)
{
    if (sequence.loop)
    {
        const uint32_t end = sequence.loop->end;
        std::erase_if(track.notes, [end](const Note& note) { return note.start >= end; });
        std::erase_if(track.unstarted, [end](const Note& note) { return note.start >= end; });
    }
}

void AddTrack(Sequence& sequence, Track track)
{
    for (const Note& note : track.notes)
    {
        sequence.notes_end = std::max(sequence.notes_end, note.start + note.length);
    }

    sequence.length = std::max(sequence.length, sequence.notes_end);
    sequence.tracks.push_back(std::move(track));
}

std::optional<export_::PresetKey> ProgramPreset(uint32_t preset)
{
    export_::PresetKey key;
    key.program = preset;
    return std::optional(key);
}

} // namespace sequence_parts

namespace
{

// The instruments of the presets of a console's performance, by the presets' numbers.
std::function<std::optional<export_::PresetKey>(uint32_t)> PresetsOf(const export_::Performance& performance)
{
    return [&performance](uint32_t preset)
    {
        return preset < performance.presets.size() ? std::optional(performance.presets[preset]) : std::nullopt;
    };
}

} // namespace

namespace sequence_parts
{

// The calls that a track made from outside any call. For each of the track's stretches, the stretch that entered the
// call that it ran in, or nothing outside calls. `returns` takes the tick at which each call returned.
std::vector<std::optional<std::size_t>> OutermostCalls(const Track& track, std::map<std::size_t, uint32_t>& returns)
{
    std::vector<std::optional<std::size_t>> calls;
    std::optional<std::size_t> current;
    std::size_t depth = 0;
    for (std::size_t i = 0; i < track.stretches.size(); i++)
    {
        const Stretch& stretch = track.stretches[i];
        if (stretch.entry == export_::Entry::kCall)
        {
            current = depth == 0 ? std::optional<std::size_t>(i) : current;
            depth++;
        }
        else if (stretch.entry == export_::Entry::kReturn && depth > 0)
        {
            depth--;
            if (depth == 0)
            {
                returns.emplace(*current, stretch.tick);
                current.reset();
            }
        }

        calls.push_back(current);
    }

    return calls;
}

// The items of `sequence` that the player can reach from the items `starts`.
std::vector<uint32_t> Reached(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& starts)
{
    std::vector<bool> seen(sequence.items.size());
    std::vector<uint32_t> pending = starts;
    std::vector<uint32_t> reached;
    const auto add = [&pending](uint32_t next)
    {
        pending.push_back(next);
    };
    while (!pending.empty())
    {
        const uint32_t item = pending.back();
        pending.pop_back();
        if (item >= sequence.items.size() || seen[item])
        {
            continue;
        }

        seen[item] = true;
        reached.push_back(item);
        VisitNext(sequence, item, add);
    }

    return reached;
}

} // namespace sequence_parts

namespace
{

// The commands of the bytecode `data`, decoded from its `labels` and from `starts`, the starts of the sounds that play
// it. The decoder names each target by its item, and the encoder gives each item's offset. Returns nothing for bytecode
// that the codec can't give back.
std::optional<FileCommands> DecodeCommands(std::span<const uint8_t> data, const std::vector<nwsnd::LabelOffset>& labels,
                                           const std::vector<uint32_t>& starts, const nwsnd::Grammar& grammar)
{
    try
    {
        nwsnd::Decoded decoded = nwsnd::DecodeSequence(data, labels, grammar, starts);
        nwsnd::Encoded encoded = nwsnd::EncodeSequence(decoded.sequence, grammar);
        if (std::ranges::equal(encoded.bytes, data))
        {
            return FileCommands{std::move(decoded.sequence), std::move(encoded.offsets), std::move(decoded.entries)};
        }
    }
    catch (const std::invalid_argument&)
    {
    }
    catch (const std::out_of_range&)
    {
    }

    return std::nullopt;
}

// The commands of `file`, decoded from its labels and from `starts` (DecodeCommands).
std::optional<FileCommands> DecodeFile(const sar::SequenceFile& file, const std::vector<uint32_t>& starts,
                                       const nwsnd::Grammar& grammar)
{
    std::vector<nwsnd::LabelOffset> labels;
    for (const sar::Sequence::Label& label : file.labels)
    {
        labels.push_back({label.name, label.offset});
    }

    return DecodeCommands(file.data, labels, starts, grammar);
}

// Adds to `sequence` the labels, the calls and the variables of the file of sound `info`, whose commands decode by the
// rules of `flavor`.
void AddFileLabels(Sequence& sequence, const sar::SoundArchive& archive, const sar::SoundInfo& info, sar::Flavor flavor)
{
    // The labels name the subroutines that the arrangement shows. The calls show which of them other calls play
    // too, from this sound or from the other sounds of the file. The sound's commands show which variables it
    // reads. A file that can't be read plays all the same.
    std::set<uint32_t> starts;
    for (const sar::SoundInfo& other : archive.Sounds())
    {
        if (other.type == sar::SoundType::kSequence && other.file_id == info.file_id)
        {
            starts.insert(other.start_offset);
        }
    }

    const std::vector<uint32_t> entries(starts.begin(), starts.end());
    try
    {
        const sar::SequenceFile file = sar::SequenceFile::Read(archive.FileData(info.file_id));
        for (const sar::Sequence::Label& label : file.labels)
        {
            sequence.labels.emplace(label.offset, label.name);
        }

        if (const std::optional<FileCommands> commands = DecodeFile(file, entries, GrammarOf(flavor)))
        {
            SetCallsAndVariables(sequence, *commands, entries, info.start_offset, -1);
        }
    }
    catch (const xsflib::FormatError&)
    {
        sequence.labels.clear();
    }
}

// Adds to `sequence` the labels, the calls and the variables of the file of the Wii sound `info`, by the rules of the
// game's build `wii_build`.
void AddWiiFileLabels(Sequence& sequence, const ntdlib::rsar::SoundArchive& archive,
                      const ntdlib::rsar::SoundInfo& info, const ntdlib::SndBuild& wii_build)
{
    // As on the other consoles, the labels, the calls and the variables come from the sound's commands. The
    // commands decode by the rules of the build that the performer follows.
    std::set<uint32_t> starts;
    for (const ntdlib::rsar::SoundInfo& other : archive.Sounds())
    {
        if (other.type == ntdlib::rsar::SoundType::kSequence && other.file_id == info.file_id)
        {
            starts.insert(other.start_offset);
        }
    }

    const std::vector<uint32_t> entries(starts.begin(), starts.end());
    try
    {
        const std::optional<ntdlib::rsar::FileData> file = archive.File(info.file_id);
        const ntdlib::rsar::Sequence parsed =
            file ? ntdlib::rsar::Sequence::Parse(file->file, ntdlib::rvl::snd::kRsarOptions) : ntdlib::rsar::Sequence{};
        std::vector<nwsnd::LabelOffset> labels;
        for (const ntdlib::rsar::Sequence::Label& label : parsed.labels)
        {
            sequence.labels.emplace(label.offset, label.name);
            labels.push_back({label.name, label.offset});
        }

        const nwsnd::Grammar grammar = nwsnd::GrammarOf<nwsnd::RevolutionPolicy>(wii_build);
        if (const std::optional<FileCommands> commands = DecodeCommands(parsed.data, labels, entries, grammar))
        {
            SetCallsAndVariables(sequence, *commands, entries, info.start_offset, 0);
        }
    }
    catch (const xsflib::FormatError&)
    {
        sequence.labels.clear();
    }
}

} // namespace

export_::PerformOptions PerformOptionsOf(const StartValues& values, const Changes& changes, sar::Flavor flavor)
{
    // ntdlib's models of nn::atk and nw::snd start the sound in their frame 0 and play the first tick in frame 1. The
    // 3DS's archive mode plays the first tick in frame 0.
    return PerformOptionsOf(values, changes, FrameMsOf(flavor), flavor == sar::Flavor::kCtr ? 0 : 1);
}

export_::PerformOptions PerformOptionsOf(const StartValues& values, const Changes& changes, FrameMs frame_ms,
                                         uint64_t first_tick)
{
    export_::PerformOptions options;
    for (const auto& [number, value] : values)
    {
        if (number >= 0 && number < kGameVariables)
        {
            options.variables[static_cast<uint8_t>(number)] = value;
        }
    }

    // The performer counts its frames from the first tick's.
    for (const VariableChange& change : GameChanges(changes))
    {
        const uint64_t frame = ChangeFrame(change.time, frame_ms);
        options.changes.push_back(
            {frame > first_tick ? frame - first_tick : 0, static_cast<uint8_t>(change.variable), change.value});
    }

    return options;
}

Sequence ReadSequence(const sar::SoundArchive& archive, uint32_t sound, const StartValues& values,
                      const Changes& changes)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != sar::SoundType::kSequence)
    {
        throw std::runtime_error("The archive has no sequence sound " + std::to_string(sound) + ".");
    }

    // The performer sets the variables before the sequence starts, and makes the changes at the starts of their frames.
    // A game does both through the sound's handle.
    const sar::SoundInfo& info = archive.Sounds()[sound];
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const export_::Settings settings = SettingsOf(archive);
    const export_::PerformOptions options = PerformOptionsOf(values, changes, flavor);
    StartValues kept(options.variables.begin(), options.variables.end());
    Changes kept_changes = GameChanges(changes);
    try
    {
        export_::BankSet banks(archive, info, settings);
        const export_::Performance performance = export_::Perform(archive, sound, banks, options, settings);

        Sequence sequence = SequenceHead(performance, info.name, sound);
        sequence.start_values = std::move(kept);
        sequence.changes = std::move(kept_changes);
        sequence.holds = performance.holds;

        // The tracks that ran commands, with notes or without. A sequence with a loop ends with the loop. The performer
        // repeats a loop shorter than 10 s until the performance lasts 10 s, but playback goes back round the loop at
        // its end. Without a loop, the performer's end includes the last notes' release. The sequence lasts at least to
        // the end of its last note.
        for (std::size_t i = 0; i < performance.tracks.size(); i++)
        {
            Track track = ReadTrack(static_cast<int>(i), performance.tracks[i], performance, PresetsOf(performance),
                                    [&banks](uint32_t slot) { return banks.BankName(slot); });
            if (track.notes.empty() && track.stretches.empty())
            {
                continue;
            }

            ClipToLoop(sequence, track);
            AddTrack(sequence, std::move(track));
        }

        AddFileLabels(sequence, archive, info, flavor);

        return sequence;
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error("Can't read " + info.name + ". " + ReadReason(error.what()));
    }
}

Sequence ReadSequence(const ntdlib::rsar::SoundArchive& archive, uint32_t sound, const StartValues& values,
                      const Changes& changes, const std::optional<ntdlib::SndBuild>& build)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != ntdlib::rsar::SoundType::kSequence)
    {
        throw std::runtime_error("The archive has no sequence sound " + std::to_string(sound) + ".");
    }

    // The performer sets the variables before the sequence starts, and makes the changes at the starts of their frames.
    // ntdlib's model of the Wii starts the sequence when the sound starts, and plays the first tick in its frame 0. A
    // Wii sequence plays one bank.
    const ntdlib::rsar::SoundInfo& info = archive.Sounds()[sound];
    const export_::Settings settings = SettingsOf(archive, build);
    const export_::PerformOptions options = PerformOptionsOf(values, changes, kWiiFrameMs, 0);
    StartValues kept(options.variables.begin(), options.variables.end());
    Changes kept_changes = GameChanges(changes);
    try
    {
        export_::RsarBankSet banks(archive, info, settings);
        const export_::Performance performance = export_::Perform(archive, sound, banks, options, settings);

        Sequence sequence = SequenceHead(performance, info.name, sound);
        sequence.start_values = std::move(kept);
        sequence.changes = std::move(kept_changes);
        sequence.wii_build = settings.wii_build;
        sequence.holds = performance.holds;

        // The tracks and the length follow the same rules as the other consoles' (see above).
        for (std::size_t i = 0; i < performance.tracks.size(); i++)
        {
            Track track = ReadTrack(static_cast<int>(i), performance.tracks[i], performance, PresetsOf(performance),
                                    [&banks](uint32_t) { return banks.BankName(); });
            if (track.notes.empty() && track.stretches.empty())
            {
                continue;
            }

            ClipToLoop(sequence, track);
            AddTrack(sequence, std::move(track));
        }

        AddWiiFileLabels(sequence, archive, info, settings.wii_build);

        return sequence;
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error("Can't read " + info.name + ". " + ReadReason(error.what()));
    }
}

Sequence ReadSequence(const GbaRom& rom, int song)
{
    const std::string name = GbaSongName(rom, song);
    ntdlib::mp2k::SongHeader header;
    if (song < 0 || !ntdlib::mp2k::ReadSong(rom.rom, rom.info, song, header))
    {
        throw std::runtime_error("The ROM has no song " + std::to_string(song) + ".");
    }

    // The performer plays one pass of the loop. A song that its music player doesn't play makes a performance whose
    // tracks run no commands, with a warning. A song whose tracks run commands but play nothing that sounds reads
    // without notes. Every note plays the song's voice group, at the voice of its program.
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.loops = 1;
    try
    {
        const export_::mp2k::Performance performance = export_::mp2k::PerformSong(rom.rom, rom.info, song, settings);
        if (!performance.warnings.empty() &&
            std::ranges::all_of(performance.stretches, [](const auto& stretches) { return stretches.empty(); }))
        {
            throw std::runtime_error("Can't read " + name + ". " + ReadReason(performance.warnings.front()));
        }

        Sequence sequence = SequenceHead(performance, name, static_cast<uint32_t>(song));
        sequence.gba = true;

        // The tracks and the length follow the same rules as the consoles' (see above).
        const std::string bank = GbaVoiceGroupName(header.voices);
        for (std::size_t i = 0; i < performance.tracks.size(); i++)
        {
            Track track = ReadTrack(
                static_cast<int>(i), performance.tracks[i], performance, ProgramPreset,
                [&bank](uint32_t) { return bank; }, kMp2kStartVolume);
            if (track.notes.empty() && track.stretches.empty())
            {
                continue;
            }

            AddUnstarted(track, performance.unstarted, i);
            ClipToLoop(sequence, track);
            AddTrack(sequence, std::move(track));
        }

        return sequence;
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error("Can't read " + name + ". " + ReadReason(error.what()));
    }
}

std::vector<Stretch> MixStretches(const Sequence& sequence, const Track& track)
{
    std::vector<Stretch> stretches = track.stretches;
    for (const Track& other : sequence.tracks)
    {
        if (track.channel >= 0 && other.channel == track.channel && other.number != track.number)
        {
            stretches.insert(stretches.end(), other.stretches.begin(), other.stretches.end());
        }
    }

    return stretches;
}

const std::vector<Stretch>& StretchesOfNote(const Sequence& sequence, const Track& track, const Note& note)
{
    if (note.command_track >= 0 && note.command_track != track.number)
    {
        for (const Track& other : sequence.tracks)
        {
            if (other.number == note.command_track)
            {
                return other.stretches;
            }
        }
    }

    return track.stretches;
}

Collisions CollisionsOf(const Sequence& sequence)
{
    Collisions collisions;
    for (const Track& track : sequence.tracks)
    {
        collisions.cut += static_cast<std::size_t>(
            std::count_if(track.notes.begin(), track.notes.end(), [](const Note& note) { return note.cut; }));
        collisions.unstarted += track.unstarted.size();
    }

    return collisions;
}

namespace
{

// AddedGbaCollisions and AddedDsCollisions.
template <class Archive, class Numbers>
std::vector<AddedCollisions> AddedCollisionsOf(const Archive& before, const Archive& after, const Numbers& songs)
{
    std::vector<AddedCollisions> added;
    for (const auto song : songs)
    {
        Collisions then;
        Collisions now;
        try
        {
            then = CollisionsOf(ReadSequence(before, song));
            now = CollisionsOf(ReadSequence(after, song));
        }
        catch (const std::runtime_error&)
        {
            continue;
        }

        const Collisions more{now.cut > then.cut ? now.cut - then.cut : 0,
                              now.unstarted > then.unstarted ? now.unstarted - then.unstarted : 0};
        if (more.cut > 0 || more.unstarted > 0)
        {
            added.push_back({static_cast<int>(song), more});
        }
    }

    return added;
}

} // namespace

std::vector<AddedCollisions> AddedGbaCollisions(const GbaRom& before, const GbaRom& after,
                                                const std::vector<int>& songs)
{
    return AddedCollisionsOf(before, after, songs);
}

std::vector<AddedCollisions> AddedDsCollisions(const DsArchive& before, const DsArchive& after,
                                               const std::vector<uint32_t>& sounds)
{
    return AddedCollisionsOf(before, after, sounds);
}

} // namespace hokuto::song
