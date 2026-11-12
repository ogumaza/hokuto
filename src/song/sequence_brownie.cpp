// SPDX-License-Identifier: MIT

// The reader of the songs of Brownie Brown's GBA driver, with the commands of their notes.

#include "ntdlib/brownie/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/instruments.h"

#include "format/brownie_items.h"
#include "song/brownie.h"
#include "song/sequence_parts.h"

namespace hokuto::song
{
namespace
{

namespace export_ = ntdlib::sequence_export;

using sequence_parts::AddTrack;
using sequence_parts::ClipToLoop;
using sequence_parts::ProgramPreset;
using sequence_parts::ReadTrack;
using sequence_parts::SequenceHead;
using sequence_parts::ThrowUnreadable;

// Sets on each note of the tracks of a sound of Brownie Brown's driver what its command contains: the last sample set,
// sample, duty or wave that its channel selected, and on the noise channel the command's byte as its key. The notes
// keep the performer's keys for the MIDI file.
void TakeBrownieCommands(Sequence& sequence, const ntdlib::commands::Sequence& commands, const BrownieRom& rom)
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::kOne;

    const std::vector<uint32_t> offsets = format::BrownieItemOffsets(commands, rom.info.revision);
    const std::map<uint32_t, uint32_t> items = ItemsByOffset(offsets);

    // The program of each note command that each stretch ran, by the track, the stretch and the item.
    std::map<std::tuple<int, uint32_t, uint32_t>, uint32_t> programs;
    for (const Track& track : sequence.tracks)
    {
        const std::optional<uint8_t> index = BrownieProgramIndex(rom.info.revision, track.number);
        uint32_t program = 0;
        for (std::size_t s = 0; s < track.stretches.size(); s++)
        {
            const auto first = items.find(track.stretches[s].first);
            const auto last = items.find(track.stretches[s].last);
            if (first == items.end() || last == items.end())
            {
                continue;
            }

            for (uint32_t i = first->second; i <= last->second; i++)
            {
                const ntdlib::commands::Command& command = commands.items[i];
                if (command.kind == Kind::kProgram && index && command.index == *index)
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
        const bool noise =
            track.number < ntdlib::brownie::kFirstSampleChannel && track.number % 4 == ntdlib::brownie::kNoiseChannel;
        for (Note& note : track.notes)
        {
            note.midi_key = note.key;
            note.velocity = 127;
            note.bank_slot = 0;
            const auto item = items.find(note.command);
            if (item == items.end() || commands.items[item->second].kind != Kind::kNote)
            {
                continue;
            }

            const auto program = programs.find(std::tuple(track.number, note.stretch, item->second));
            note.program = program != programs.end() ? program->second : 0;
            if (noise)
            {
                note.key = commands.items[item->second].key;
            }
        }

        track.program = track.notes.empty() ? -1 : static_cast<int>(track.notes.front().program);
        track.bank = track.notes.empty() ? std::string() : track.name;
    }
}

} // namespace

Sequence ReadSequence(const BrownieRom& rom, uint32_t sound)
{
    const std::string name = rom.SoundName(sound);
    const int song = rom.SongOf(sound);
    if (song < 0 || song >= rom.info.song_count)
    {
        throw std::runtime_error("The ROM has no sound " + std::to_string(sound) + ".");
    }

    // The performer plays one pass of the loop.
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.loops = 1;
    try
    {
        const export_::brownie::Performance performance =
            export_::brownie::PerformSong(rom.rom, rom.info, song, settings);

        Sequence sequence = SequenceHead(performance, name, sound);
        sequence.driver = &rom.Rules();

        // The tracks and the length follow the same rules as those of MP2K.
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

        const std::optional<BrownieRom::ListPlace> place = rom.ListOf(sound);
        if (place)
        {
            TakeBrownieCommands(sequence, rom.regions[place->region].sequence, rom);
        }

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
