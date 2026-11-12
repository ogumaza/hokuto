// SPDX-License-Identifier: MIT

// The parts of song/sequence.h that its files share: the aliases, the reader of a track from a performance, the
// commands of a sequence file, and the steps that the readers of the consoles and drivers call. Only the sequence
// files of src/song include it.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sequence_export/brownie/performance.h"
#include "ntdlib/sequence_export/performer.h"

#include "song/sequence.h"

namespace hokuto::song::sequence_parts
{

namespace nwsnd = ntdlib::nwsnd;
namespace sar = ntdlib::sar;
namespace export_ = ntdlib::sequence_export;

using export_::EventKind;
using export_::TrackEvent;

// The settings of `track` for `setting`. `T` is Track or const Track.
template <typename T>
auto& TrackSettingsOf(T& track, Mix setting)
{
    switch (setting)
    {
    case Mix::kVolume:
        return track.volume;
    case Mix::kPan:
        return track.pan;
    case Mix::kSendA:
        return track.sends[0];
    case Mix::kSendB:
        return track.sends[1];
    case Mix::kSendC:
        break;
    }

    return track.sends[2];
}

void AddSetting(std::vector<Setting>& settings, uint32_t tick, int value, int fallback);

// Starts on `track` the note of the note-on `event`, with the instrument of `preset`. `sounding` contains the place in
// the track of each note that still sounds, by the note's ID.
template <typename AnyPerformance>
void StartNote(Track& track, int number, const TrackEvent& event, const AnyPerformance& performance,
               const std::optional<export_::PresetKey>& preset,
               const std::function<std::string(uint32_t slot)>& bank_name, std::map<uint32_t, std::size_t>& sounding)
{
    if (track.notes.empty() && preset)
    {
        track.bank = bank_name(preset->bank_slot);
        track.program = static_cast<int>(preset->program);
    }

    // The note's ID leads to its command.
    sounding[event.note] = track.notes.size();
    Note& note = track.notes.emplace_back(Note{event.tick, 0, event.key, event.value});
    if (event.note >= 1 && event.note <= performance.note_commands.size())
    {
        note.command = performance.note_commands[event.note - 1].offset;
        note.stretch = performance.note_commands[event.note - 1].stretch;
    }

    // The performers of MP2K and NITRO-Composer report the place of each note on the channels of the driver. Only MP2K
    // has PSG channels. DSE's performer reports only whether a later note took the note's last voice.
    if constexpr (requires { performance.note_channels; })
    {
        if (event.note >= 1 && event.note <= performance.note_channels.size())
        {
            if constexpr (requires { performance.note_channels[0].psg; })
            {
                note.psg = performance.note_channels[event.note - 1].psg;
            }

            if constexpr (requires { performance.note_channels[0].channel; })
            {
                note.channel = performance.note_channels[event.note - 1].channel;
            }

            note.cut = performance.note_channels[event.note - 1].cut_tick.has_value();
        }
    }
    // Brownie Brown's performer says whether a command started each note in its tick.
    if constexpr (requires { performance.note_causes; })
    {
        if (event.note >= 1 && event.note <= performance.note_causes.size())
        {
            note.start_gives_tick = performance.note_causes[event.note - 1] == export_::brownie::NoteCause::kNote;
        }
    }
    // Konami's performer reports the track that contains the command of each note.
    if constexpr (requires { performance.note_starts; })
    {
        if (event.note >= 1 && event.note <= performance.note_starts.size() &&
            performance.note_starts[event.note - 1].track != number)
        {
            note.command_track = static_cast<int8_t>(performance.note_starts[event.note - 1].track);
        }
    }

    if (preset)
    {
        note.bank_slot = preset->bank_slot;
        note.program = preset->program;
    }
}

// Adds to `track` its settings and stretches, from the mix and the stretches of track `number` in the performance.
// `start_volume` is the volume at which a track of a GBA driver plays until a command sets it
// (DriverRules::StartVolume). The track's volume then starts with that value at tick 0. Without it, the volume starts
// at MixDefault with no point at tick 0. The other consoles, Krawall and Ubisoft Milan's driver pass none.
template <typename AnyPerformance>
void AddTrackMix(Track& track, const AnyPerformance& performance, int number, std::optional<int> start_volume)
{
    // The track's volume, pan and sends at each change that its commands and ramps make, and the stretches of its
    // commands.
    const auto fallback = [start_volume](Mix setting)
    {
        return setting == Mix::kVolume && start_volume ? *start_volume : MixDefault(setting);
    };
    for (const export_::MixChange& change : performance.mix[static_cast<std::size_t>(number)])
    {
        AddSetting(TrackSettingsOf(track, change.setting), change.tick, change.value, fallback(change.setting));
    }

    if (start_volume && (track.volume.empty() || track.volume.front().tick > 0))
    {
        track.volume.insert(track.volume.begin(), {0, *start_volume});
    }

    track.stretches = performance.stretches[static_cast<std::size_t>(number)];
}

// A track's notes, settings and instrument, from its events in the performance. `preset_of` returns the instrument that
// a preset event selects, or nothing. The consoles' performers name their presets in Performance::presets, and MP2K's
// performer uses the number of a voice. AddTrackMix uses `start_volume`.
template <typename AnyPerformance>
Track ReadTrack(int number, std::vector<TrackEvent> events, const AnyPerformance& performance,
                const std::function<std::optional<export_::PresetKey>(uint32_t preset)>& preset_of,
                const std::function<std::string(uint32_t slot)>& bank_name,
                std::optional<int> start_volume = std::nullopt)
{
    Track track;
    track.number = number;
    const std::string& name = performance.track_names[static_cast<std::size_t>(number)];
    track.name = name.empty() ? "Track " + std::to_string(number) : name;

    // A note's off finds its on by the note's ID. A note that never ends lasts to the end of the sequence. In a
    // sequence with a loop, the performer stops at its end tick and ends there every note that still sounds. Such a
    // note lasts to the loop's end. The performer repeats a loop shorter than 10 s until the performance lasts 10 s.
    // Each note plays the instrument of the last preset before it. The track's instrument is the one that the first
    // note plays.
    std::stable_sort(events.begin(), events.end(),
                     [](const TrackEvent& a, const TrackEvent& b) { return a.tick < b.tick; });
    std::map<uint32_t, std::size_t> sounding;
    std::optional<export_::PresetKey> preset;
    for (const TrackEvent& event : events)
    {
        switch (event.kind)
        {
        case EventKind::kNoteOn:
            StartNote(track, number, event, performance, preset, bank_name, sounding);
            break;

        case EventKind::kNoteOff:
            {
                const auto found = sounding.find(event.note);
                if (found != sounding.end())
                {
                    Note& note = track.notes[found->second];
                    const bool held = performance.loop && event.tick >= performance.end_tick;
                    const uint32_t end = held ? std::min(performance.loop->end, event.tick) : event.tick;
                    note.length = end - std::min(note.start, end);
                    sounding.erase(found);
                }
                break;
            }

        case EventKind::kPreset:
            preset = preset_of(event.preset);
            break;

        default:
            break;
        }
    }

    for (const auto& [id, index] : sounding)
    {
        Note& note = track.notes[index];
        note.length = std::max(performance.end_tick, note.start) - note.start;
    }

    AddTrackMix(track, performance, number, start_volume);

    return track;
}

// A sequence named `name` for sound `sound`, with the timebase, the tempo and the loop of `performance`. A sequence
// with a loop lasts to the loop's end, and one without lasts to the performance's end, until AddTrack extends it.
template <typename AnyPerformance>
Sequence SequenceHead(const AnyPerformance& performance, std::string name, uint32_t sound)
{
    Sequence sequence;
    sequence.name = std::move(name);
    sequence.sound = sound;
    sequence.timebase = static_cast<int>(std::max<uint32_t>(performance.timebase, 1));
    for (const auto& tempo : performance.tempo)
    {
        sequence.tempo.push_back({tempo.tick, tempo.bpm});
    }

    if (performance.loop)
    {
        sequence.loop = Loop{performance.loop->start, performance.loop->end};
    }

    sequence.length = sequence.loop ? sequence.loop->end : performance.end_tick;

    return sequence;
}

// Adds to `track` the notes that the driver never started on track `index`, from a performer's list of them. MP2K's
// performer reports the PSG channel of each one too.
template <typename UnstartedNote>
void AddUnstarted(Track& track, const std::vector<UnstartedNote>& unstarted_notes, std::size_t index)
{
    for (const UnstartedNote& unstarted : unstarted_notes)
    {
        if (unstarted.track == index)
        {
            Note note{unstarted.tick, unstarted.length, unstarted.key, unstarted.velocity, unstarted.offset};
            if constexpr (requires { unstarted.psg; })
            {
                note.psg = unstarted.psg;
            }

            track.unstarted.push_back(note);
        }
    }
}

// Drops the notes and the unstarted notes of `track` that start at or after the loop's end of `sequence`.
void ClipToLoop(const Sequence& sequence, Track& track);

// Adds `track` to `sequence`. The sequence lasts at least to the end of its last note.
void AddTrack(Sequence& sequence, Track track);

// The preset of program `preset` in a sound's one bank, for the drivers whose presets are programs.
std::optional<export_::PresetKey> ProgramPreset(uint32_t preset);

// Calls `visit` with each item of `sequence` that can run after item `item`: the next item, unless the command always
// jumps, returns or ends, and the target of a jump, a call or a track start. A call's next item stands for the call's
// return. An item can be past the end of the sequence.
template <typename Visit>
void VisitNext(const ntdlib::commands::Sequence& sequence, uint32_t item, Visit&& visit)
{
    using ntdlib::commands::Kind;

    const ntdlib::commands::Command& command = sequence.items[item];
    const bool stops = command.kind == Kind::kJump || command.kind == Kind::kReturn || command.kind == Kind::kEnd;
    if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
    {
        visit(command.target);
    }

    if (!stops || command.conditional)
    {
        visit(item + 1);
    }
}

// The commands of a sequence file, the offset in the bytecode of each item and of the end, and the item at each of the
// sounds' starts.
struct FileCommands
{
    ntdlib::commands::Sequence sequence;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> entries;
};

// The outermost calls of a track and the items that a sequence reaches (sequence.cpp).
std::vector<std::optional<std::size_t>> OutermostCalls(const Track& track, std::map<std::size_t, uint32_t>& returns);
std::vector<uint32_t> Reached(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& starts);

// The variables of a sequence, and the reasons that a reader reports (sequence_variables.cpp).
std::map<uint32_t, int> CountCalls(const FileCommands& file);
SoundVariables ReadVariables(const ntdlib::commands::Sequence& sequence, uint32_t start, int global_start);
void SetCallsAndVariables(Sequence& sequence, const FileCommands& commands, std::span<const uint32_t> starts,
                          uint32_t start, int global_start);
Changes GameChanges(const Changes& changes);
std::string ReadReason(std::string reason);

// Throws std::runtime_error: `name` can't be read, with the reason that `error` contains (ReadReason).
[[noreturn]] void ThrowUnreadable(const std::string& name, const std::exception& error);

} // namespace hokuto::song::sequence_parts
