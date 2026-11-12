// SPDX-License-Identifier: MIT

// The reader of the songs of Ubisoft Milan's GBA driver.

#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/ubimilan/performance.h"
#include "ntdlib/sequence_export/ubimilan/soundfont.h"
#include "ntdlib/ubimilan/commands.h"

#include "song/sequence_parts.h"
#include "song/ubimilan.h"

namespace hokuto::song
{
namespace
{

namespace export_ = ntdlib::sequence_export;

using sequence_parts::AddTrack;
using sequence_parts::ClipToLoop;
using sequence_parts::EventKind;
using sequence_parts::ReadTrack;
using sequence_parts::SequenceHead;
using sequence_parts::ThrowUnreadable;
using sequence_parts::TrackEvent;

// A performance of Ubisoft Milan's driver as ReadTrack reads it. The channels share one track of commands, and each
// track contains the stretches through the piece's first sequence. A note of another sequence leads to kNoCommand.
struct UbiMilanTracks
{
    std::array<std::string, 16> track_names;
    std::vector<export_::NoteCommand> note_commands;
    decltype(export_::ubimilan::Performance::loop) loop;
    uint32_t end_tick = 0;
    std::vector<std::vector<export_::Stretch>> stretches;
    std::vector<std::vector<export_::MixChange>> mix;
};

// The performance as ReadTrack reads it, with the stretches through the piece's first sequence, `commands` of resource
// `first`.
UbiMilanTracks UbiMilanTracksOf(const export_::ubimilan::Performance& performance,
                                const ntdlib::commands::Sequence& commands, int first)
{
    UbiMilanTracks tracks;
    tracks.track_names = performance.track_names;
    tracks.loop = performance.loop;
    tracks.end_tick = performance.end_tick;
    const bool first_waits = commands.items.size() > 1 && commands.items[1].kind == ntdlib::commands::Kind::kWait &&
                             commands.items[1].value.value != 0;
    std::map<uint32_t, uint32_t> kept;
    std::vector<export_::Stretch> stretches;
    for (std::size_t i = 0; i < performance.stretches.size(); i++)
    {
        const uint32_t place = performance.stretch_sequences[i];
        if (place < performance.sequences.size() && performance.sequences[place].resource == first)
        {
            kept.emplace(static_cast<uint32_t>(i), static_cast<uint32_t>(stretches.size()));
            export_::Stretch& stretch = stretches.emplace_back(performance.stretches[i]);
            stretch.tick += first_waits && stretch.entry != export_::Entry::kOpen ? 1 : 0;
        }
    }

    tracks.stretches.assign(performance.tracks.size(), stretches);
    tracks.mix.resize(performance.tracks.size());
    for (const export_::NoteCommand& command : performance.note_commands)
    {
        const auto stretch = kept.find(command.stretch);
        tracks.note_commands.push_back(stretch == kept.end() ? export_::NoteCommand{kNoCommand, 0}
                                                             : export_::NoteCommand{command.offset, stretch->second});
    }

    return tracks;
}

// The commands of the notes of a performance. Each sequence decodes once, at its first note.
class UbiMilanNoteCommands
{
public:
    UbiMilanNoteCommands(const UbiMilanRom& rom, const export_::ubimilan::Performance& performance)
        : rom_(rom), performance_(performance)
    {
    }

    // The note command of the note whose ID is `id`, or nullptr.
    const ntdlib::commands::Command* Of(uint32_t id)
    {
        if (id < 1 || id > performance_.note_commands.size())
        {
            return nullptr;
        }

        const export_::NoteCommand& place = performance_.note_commands[id - 1];
        if (place.stretch >= performance_.stretch_sequences.size() ||
            performance_.stretch_sequences[place.stretch] >= performance_.sequences.size())
        {
            return nullptr;
        }

        const int resource = performance_.sequences[performance_.stretch_sequences[place.stretch]].resource;
        auto found = decoded_.find(resource);
        if (found == decoded_.end())
        {
            found = decoded_.emplace(resource, std::pair<ntdlib::commands::Sequence, std::map<uint32_t, std::size_t>>())
                        .first;
            try
            {
                // The waits of the decoder contain the countdowns of the driver, and ntdlib's encoder returns the
                // offsets of its items. The file's offsets (DriverRules::ItemOffsets) use waits in ticks instead.
                found->second.first = ntdlib::ubimilan::DecodeResource(rom_.rom, rom_.info, resource).sequence;
                const std::vector<uint32_t> offsets = ntdlib::ubimilan::EncodeSequence(found->second.first).offsets;
                for (std::size_t item = 0; item < offsets.size(); item++)
                {
                    found->second.second.emplace(offsets[item], item);
                }
            }
            catch (const std::invalid_argument&)
            {
                found->second = {};
            }
        }

        const auto item = found->second.second.find(place.offset);
        if (item == found->second.second.end() || item->second >= found->second.first.items.size() ||
            found->second.first.items[item->second].kind != ntdlib::commands::Kind::kNote)
        {
            return nullptr;
        }

        return &found->second.first.items[item->second];
    }

private:
    const UbiMilanRom& rom_;
    const export_::ubimilan::Performance& performance_;
    std::map<int, std::pair<ntdlib::commands::Sequence, std::map<uint32_t, std::size_t>>> decoded_;
};

// Sets the velocity of each note-on of `events` from the byte of its command (UbiMilanVelocityOf).
void SetCommandVelocities(std::vector<TrackEvent>& events, UbiMilanNoteCommands& command_of)
{
    for (TrackEvent& event : events)
    {
        const ntdlib::commands::Command* command =
            event.kind == EventKind::kNoteOn ? command_of.Of(event.note) : nullptr;
        if (command != nullptr)
        {
            const int byte = static_cast<int>(command->velocity / ntdlib::commands::kOne);
            event.value = static_cast<uint8_t>(UbiMilanVelocityOf(command->index, command->key, byte));
        }
    }
}

// Sets the key of each note of the noise to the key of its command, from the `events` of its track. The key of the MIDI
// file stays the key of the drum.
void SetNoiseKeys(std::vector<Note>& noise_notes, const std::vector<TrackEvent>& events,
                  UbiMilanNoteCommands& command_of)
{
    std::vector<TrackEvent> ons = events;
    std::stable_sort(ons.begin(), ons.end(), [](const TrackEvent& a, const TrackEvent& b) { return a.tick < b.tick; });
    std::erase_if(ons, [](const TrackEvent& event) { return event.kind != EventKind::kNoteOn; });
    for (std::size_t n = 0; n < noise_notes.size() && n < ons.size(); n++)
    {
        const ntdlib::commands::Command* command = command_of.Of(ons[n].note);
        noise_notes[n].midi_key = noise_notes[n].key;
        noise_notes[n].key = command != nullptr ? command->key : noise_notes[n].key;
    }
}

// The channels that the notes and the programs of `commands` name.
std::set<int> NamedChannels(const ntdlib::commands::Sequence& commands)
{
    std::set<int> named;
    for (const ntdlib::commands::Command& item : commands.items)
    {
        if (item.kind == ntdlib::commands::Kind::kNote || item.kind == ntdlib::commands::Kind::kNoteOff ||
            item.kind == ntdlib::commands::Kind::kProgram)
        {
            named.insert(item.index);
        }
    }

    return named;
}

// The preset key of preset number `preset`.
std::optional<export_::PresetKey> UbiMilanPreset(uint32_t preset)
{
    // A preset is 128 times the channel plus the program.
    export_::PresetKey key;
    key.program = preset % 128;
    return std::optional(key);
}

// Adds `noise_notes` to `track`, the kit's track.
void AddNoiseNotes(Track& track, std::vector<Note>& noise_notes)
{
    // A note of the noise plays the kit's program before it, or else the first program of the kit.
    for (Note& note : noise_notes)
    {
        const auto later =
            std::ranges::find_if(track.notes, [&note](const Note& other) { return other.start > note.start; });
        note.program = later != track.notes.begin() ? std::prev(later)->program
                       : track.notes.empty()        ? note.program
                                                    : track.notes.front().program;
        note.bank_slot = 0;
    }

    const std::size_t kits = track.notes.size();
    track.notes.insert(track.notes.end(), noise_notes.begin(), noise_notes.end());
    std::ranges::stable_sort(track.notes, [](const Note& a, const Note& b) { return a.start < b.start; });
    if (kits == 0)
    {
        track.program = track.notes.front().program;
    }
}

// Sets the MIDI key of each note of `track` to its key, except on the notes of the noise on the kit's track.
void SetUbiMilanMidiKeys(Track& track, bool noise_on_kit)
{
    for (Note& note : track.notes)
    {
        // A note of the noise keeps the drum's key as its MIDI key.
        if (!(noise_on_kit && note.key <= ntdlib::ubimilan::kLastNoiseKey))
        {
            note.midi_key = note.key;
        }
    }
}

// Adds to `sequence` the tracks of `performance`, a performance of a piece of `rom` whose stretches `tracks` contains.
// A channel of `named` shows without notes too.
void AddUbiMilanTracks(Sequence& sequence, const UbiMilanRom& rom, const export_::ubimilan::Performance& performance,
                       const UbiMilanTracks& tracks, const std::set<int>& named)
{
    const auto bank_name = [](uint32_t)
    {
        return std::string();
    };

    // ntdlib's performer plays a note of the PSG at velocity 127: the MIDI file sets the level by control 11. It puts
    // the noise that channel 9's keys up to 11 play on track 3, named "Noise", at keys that name drums of the
    // SoundFont. No sequence channel 3 plays notes. The Key Editor shows the commands' notes. A note of the PSG gets
    // the velocity of the byte of its command (UbiMilanVelocityOf), and a note of the noise the key of its command, on
    // track 9 with the kit's notes. The noise's note keeps the drum's key as the key of the MIDI file.
    constexpr std::size_t kNoiseTrack = 3;
    const bool noise = performance.track_names[kNoiseTrack] == "Noise";
    UbiMilanNoteCommands command_of(rom, performance);
    std::vector<std::vector<TrackEvent>> events(performance.tracks.begin(), performance.tracks.end());
    for (std::size_t i = 0; i < events.size(); i++)
    {
        if (i >= static_cast<std::size_t>(ntdlib::ubimilan::kPsgChannels) && !(noise && i == kNoiseTrack))
        {
            continue;
        }

        SetCommandVelocities(events[i], command_of);
    }

    // ReadTrack makes a note of each note-on, in the order of their ticks. The performance has no changes of volume,
    // and a track plays at 127 throughout. MixDefault returns 127 too. A track therefore reads no start volume.
    std::vector<Note> noise_notes;
    if (noise)
    {
        noise_notes =
            ReadTrack(static_cast<int>(kNoiseTrack), events[kNoiseTrack], tracks, UbiMilanPreset, bank_name).notes;
        SetNoiseKeys(noise_notes, events[kNoiseTrack], command_of);
    }

    for (std::size_t i = 0; i < events.size(); i++)
    {
        if (noise && i == kNoiseTrack)
        {
            continue;
        }

        Track track = ReadTrack(static_cast<int>(i), events[i], tracks, UbiMilanPreset, bank_name);
        const bool kit = i == static_cast<std::size_t>(ntdlib::ubimilan::kKitChannel);
        if (kit && !noise_notes.empty())
        {
            AddNoiseNotes(track, noise_notes);
        }

        if (track.notes.empty() && !named.contains(static_cast<int>(i)))
        {
            continue;
        }

        // Only channels 0 to 2 and channel 9 sound. ChannelName names only those.
        if (i < static_cast<std::size_t>(ntdlib::ubimilan::kPsgChannels) || i == ntdlib::ubimilan::kKitChannel)
        {
            track.bank = export_::ubimilan::ChannelName(static_cast<int>(i));
        }

        ClipToLoop(sequence, track);
        SetUbiMilanMidiKeys(track, kit && noise);
        AddTrack(sequence, std::move(track));
    }
}

} // namespace

Sequence ReadSequence(const UbiMilanRom& rom, uint32_t sound)
{
    const std::string name = rom.SoundName(sound);
    ntdlib::ubimilan::MusicPiece piece;
    if (!ntdlib::ubimilan::ReadMusic(rom.rom, rom.info, static_cast<int>(sound), piece) || piece.sequences.empty())
    {
        throw std::runtime_error("The ROM has no piece " + std::to_string(sound) + ".");
    }

    // The performer plays one pass of the loop.
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.loops = 1;
    try
    {
        const export_::ubimilan::Performance performance =
            export_::ubimilan::PerformSong(rom.rom, rom.info, static_cast<int>(sound), settings);
        if (performance.sequences.empty())
        {
            throw std::invalid_argument(performance.warnings.empty() ? "its list of sequences can't be read"
                                                                     : performance.warnings.front());
        }

        Sequence sequence = SequenceHead(performance, name, sound);
        sequence.driver = &rom.Rules();

        // The stretches through the piece's first sequence, the one that the sound's field names. Every track contains
        // them all: the channels share one track of commands. A note of another sequence leads to no command. A file
        // sets the first wait of the sequence to the frames before its first command at the start of the piece
        // (format/ubimilan.h). Where the track starts the sequence later, a countdown above 0 there runs the first
        // command a frame later still. The stretch then starts a tick after the frame where the track started the
        // sequence.
        const int first = performance.sequences.front().resource;
        const ntdlib::commands::Sequence commands = ntdlib::ubimilan::DecodeResource(rom.rom, rom.info, first).sequence;
        const UbiMilanTracks tracks = UbiMilanTracksOf(performance, commands, first);

        // The channels that the first sequence's commands name show without notes too.
        const std::set<int> named = NamedChannels(commands);

        AddUbiMilanTracks(sequence, rom, performance, tracks, named);

        return sequence;
    }
    catch (const std::invalid_argument& error)
    {
        ThrowUnreadable(name, error);
    }
    catch (const std::out_of_range& error)
    {
        ThrowUnreadable(name, error);
    }
}

} // namespace hokuto::song
