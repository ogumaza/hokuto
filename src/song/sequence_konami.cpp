// SPDX-License-Identifier: MIT

// The reader of the songs of Konami's GBA driver, with the commands of their notes.

#include "ntdlib/konami/commands.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/konami/performance.h"
#include "ntdlib/sequence_export/konami/soundfont.h"
#include "ntdlib/sequence_export/ntr/performance.h"

#include "song/ds.h"
#include "song/konami.h"
#include "song/sequence_parts.h"

namespace hokuto::song
{
namespace
{

namespace export_ = ntdlib::sequence_export;

using sequence_parts::AddTrack;
using sequence_parts::ClipToLoop;
using sequence_parts::ProgramPreset;
using sequence_parts::ReadReason;
using sequence_parts::ReadTrack;
using sequence_parts::SequenceHead;

// The note commands that each stretch ran, with the duty or the wave of each: by the track, the stretch and the item.
using RanNotes = std::map<std::tuple<int, uint32_t, uint32_t>, uint32_t>;

// The items that each stretch of each track ran, by the track: a run for each stretch, in the order in which the track
// ran them. A stretch without commands has an empty run.
using Runs = std::map<int, std::vector<KonamiRun>>;

// The volume command (C0-CF) that starts a PSG note in the frame of item `item` of `commands`, among the items `first`
// to `last` of a stretch. `item` is the frame's note command, or else the last other command that the track ran in the
// frame. That command can be the wait that ends the frame. The frame's last volume command sets the volume that plays.
// Nothing where the frame has no volume command.
std::optional<uint32_t> VolumeStartAt(const ntdlib::commands::Sequence& commands, uint32_t first, uint32_t last,
                                      uint32_t item)
{
    using ntdlib::commands::Kind;
    const auto is_volume = [&](uint32_t i)
    {
        return commands.items[i].kind == Kind::kVolume && commands.items[i].index == 0;
    };
    const auto ends_frame = [&](uint32_t i)
    {
        const ntdlib::commands::Command& command = commands.items[i];
        return command.kind == Kind::kNote || (command.kind == Kind::kWait && command.value.value != 0);
    };
    if (item < first || item > last)
    {
        return std::nullopt;
    }

    std::optional<uint32_t> volume;
    if (commands.items[item].kind == Kind::kNote)
    {
        for (uint32_t i = item + 1; i <= last && !ends_frame(i); i++)
        {
            volume = is_volume(i) ? std::optional(i) : volume;
        }

        return volume;
    }

    for (uint32_t i = item; !volume && (i == item || !ends_frame(i)); i--)
    {
        volume = is_volume(i) ? std::optional(i) : volume;
        if (i == first)
        {
            break;
        }
    }

    return volume;
}

// Sets each note of the tracks of a song of Konami's driver to what its command contains, in the place of what the
// performer made of it for a MIDI file: the volume, and the sample of a sample note or else the track's duty or wave.
// Another command can start a note: a volume command starts a PSG channel again, at the pitch of the last note command
// that the track ran. Such a note takes that note command. The command needn't have started a note. A PSG note at
// volume 0 is silent, and in the Eternal Duelist and Captain Tsubasa revisions a PSG note at the track's volume only
// moves the pitch of the note that plays (ntdlib src/konami/sequencer.cpp and sequencer_wct2004.cpp, Op::kPsgNote and
// Op::kVolume). The note before it in the arrangement can therefore name another command and show another key. A note
// without a note command before it leads to no command. After a PSG note command at volume 0, the volume command sets
// the volume of the note. Such a note names the volume command and shows the command's volume as its velocity. The key
// and the duty or wave still come from the note command. The edits of such a note are in edit::EditKonamiNotes.
// `starts`, where it isn't null, gains each note that a volume command starts (KonamiVolumeStarts).
void TakeKonamiCommands(Sequence& sequence, const ntdlib::commands::Sequence& commands, const KonamiRom& rom,
                        std::vector<KonamiVolumeStart>* starts)
{
    const ntdlib::konami::Revision revision = rom.info.revision;
    export_::konami::SoundfontBuilder samples(rom.rom, rom.info);
    constexpr int kLoudest = 15; // the highest volume of a note whose volumes run from 0 to 15
    using ntdlib::commands::Kind;
    using ntdlib::commands::kOne;

    const std::vector<uint32_t> offsets = ntdlib::konami::ItemOffsets(commands, revision);
    const std::map<uint32_t, uint32_t> items = ItemsByOffset(offsets);

    Runs runs;
    RanNotes programs;
    for (const Track& track : sequence.tracks)
    {
        uint32_t program = 0;
        std::vector<KonamiRun>& ran = runs[track.number];
        ran.resize(track.stretches.size());
        for (std::size_t s = 0; s < track.stretches.size(); s++)
        {
            const auto first = items.find(track.stretches[s].first);
            const auto last = items.find(track.stretches[s].last);
            if (first == items.end() || last == items.end())
            {
                continue;
            }

            ran[s] = {first->second, last->second + 1};

            for (uint32_t i = first->second; i <= last->second; i++)
            {
                const ntdlib::commands::Command& command = commands.items[i];
                if (command.kind == Kind::kProgram && command.index < 2)
                {
                    program = static_cast<uint32_t>(command.value.value / kOne);
                }
                else if (command.kind == Kind::kNote)
                {
                    programs.emplace(std::tuple(track.number, static_cast<uint32_t>(s), i), program);
                }
            }
        }
    }

    for (Track& track : sequence.tracks)
    {
        for (Note& note : track.notes)
        {
            // The command that the performer assigns to the note, or else the note command before that command.
            const int holder = note.command_track >= 0 ? note.command_track : track.number;
            const auto found = items.find(note.command);
            const auto held = runs.find(holder);
            const std::span<const KonamiRun> ran =
                held != runs.end() ? std::span<const KonamiRun>(held->second) : std::span<const KonamiRun>();
            std::optional<std::pair<uint32_t, uint32_t>> place;
            if (found != items.end() && commands.items[found->second].kind == Kind::kNote)
            {
                place = std::pair(note.stretch, found->second);
            }
            else if (found != items.end())
            {
                const auto before = KonamiNoteCommandBefore(commands.items, ran, note.stretch, found->second, holder,
                                                            track.number, revision);
                place = before ? std::optional(std::pair(static_cast<uint32_t>(before->first), before->second))
                               : std::nullopt;

                const ntdlib::commands::Command& starter = commands.items[found->second];
                if (starts != nullptr && starter.kind == Kind::kVolume && starter.index == 0)
                {
                    starts->push_back({note.command, place ? std::optional(offsets[place->second]) : std::nullopt,
                                       holder != track.number});
                }
            }

            note.midi_key = note.key;
            note.start_gives_tick = false;
            note.bank_slot = 0;
            if (!place)
            {
                // No note command leads to this note. A PSG note keeps the key that the performer assigns it.
                note.velocity = static_cast<uint8_t>(
                    KonamiVelocityOf(kLoudest, revision, !ntdlib::konami::IsPsgTrack(track.number)));
                note.program = 0;
                note.sample_semitones = std::nullopt;
                note.command = kNoCommand;
                note.stretch = 0;
                note.command_track = int8_t{-1};
                continue;
            }

            const auto& [stretch, item] = *place;
            const ntdlib::commands::Command& command = commands.items[item];

            // A PSG note command at volume 0 starts no note. The volume command of the note's frame starts it.
            const KonamiRun run = note.stretch < ran.size() ? ran[note.stretch] : KonamiRun();
            const std::optional<uint32_t> volume =
                !command.sample && command.velocity == 0 && run.second > run.first
                    ? VolumeStartAt(commands, run.first, run.second - 1, found->second)
                    : std::nullopt;
            if (starts != nullptr && volume && commands.items[found->second].kind == Kind::kNote)
            {
                starts->push_back({offsets[*volume], offsets[item], holder != track.number});
            }
            note.command = offsets[volume.value_or(item)];
            note.stretch = volume ? note.stretch : stretch;
            const int64_t level = volume ? commands.items[*volume].value.value : command.velocity;
            note.velocity = static_cast<uint8_t>(
                KonamiVelocityOf(static_cast<int>(level / kOne), revision, command.sample.has_value()));
            const auto program = programs.find(std::tuple(holder, stretch, item));
            note.program = command.sample ? *command.sample : program != programs.end() ? program->second : 0;
            note.sample_semitones = command.sample ? std::optional(static_cast<int8_t>(command.key)) : std::nullopt;

            // The revision without a mixer works out a sample's pitch from the channel's registers.
            if (command.sample && revision != ntdlib::konami::Revision::kDungeonDiceMonsters)
            {
                note.key = static_cast<uint8_t>(
                    std::clamp(samples.DsRootKey(*command.sample) + static_cast<int8_t>(command.key), 0, 127));
            }
        }

        track.program = track.notes.empty() ? -1 : static_cast<int>(track.notes.front().program);
        track.bank = track.notes.empty() ? std::string() : track.name;
    }
}

// ReadSequence, with the notes that a volume command starts in `starts` where it isn't null.
Sequence ReadWithStarts(const KonamiRom& rom, uint32_t sound, std::vector<KonamiVolumeStart>* starts)
{
    const std::string name = KonamiSoundName(rom, sound);
    const int song = KonamiSongOf(rom, sound);
    ntdlib::konami::SongHeader header;
    if (!ntdlib::konami::ReadSongHeader(rom.rom, rom.info.song_table, song, rom.info.revision, header) ||
        !rom.rom.Contains(header.base))
    {
        throw std::runtime_error("The ROM has no song " + std::to_string(song) + ".");
    }

    // The performer plays one pass of the loop.
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.loops = 1;
    settings.version = KonamiSoundIsLive(rom, sound) ? 1 : 0;
    try
    {
        const export_::konami::Performance performance =
            export_::konami::PerformSong(rom.rom, rom.info, song, settings);

        Sequence sequence = SequenceHead(performance, name, sound);
        sequence.driver = &rom.Rules();

        // The tracks and the length follow the same rules as on MP2K.
        for (std::size_t i = 0; i < performance.tracks.size(); i++)
        {
            Track track = ReadTrack(
                static_cast<int>(i), performance.tracks[i], performance, ProgramPreset,
                [](uint32_t) { return std::string(); }, rom.Rules().StartVolume());
            if (track.notes.empty() && track.stretches.empty())
            {
                continue;
            }

            ClipToLoop(sequence, track);
            AddTrack(sequence, std::move(track));
        }

        const ntdlib::konami::Decoded decoded = ntdlib::konami::DecodeSong(rom.rom, rom.info, song);
        TakeKonamiCommands(sequence, decoded.sequence, rom, starts);

        return sequence;
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error("Can't read " + name + ". " + ReadReason(error.what()));
    }
}

} // namespace

Sequence ReadSequence(const KonamiRom& rom, uint32_t sound)
{
    return ReadWithStarts(rom, sound, nullptr);
}

std::vector<KonamiVolumeStart> KonamiVolumeStarts(const KonamiRom& rom, uint32_t sound)
{
    std::vector<KonamiVolumeStart> starts;
    ReadWithStarts(rom, sound, &starts);

    return starts;
}

} // namespace hokuto::song
