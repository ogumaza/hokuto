// SPDX-License-Identifier: MIT

// The reader of the songs of Krawall, a GBA driver.

#include "ntdlib/krawall/commands.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/krawall/performance.h"

#include "song/krawall.h"
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

// The instrument that each note of `commands`, a module of Krawall, plays on `track` in each of the track's stretches,
// by the stretch and the item: the note's instrument, or else the last that the channel's entries named. `items`
// contains the item at each offset. An instrument stays from one stretch to the next.
std::map<std::pair<uint32_t, uint32_t>, uint16_t> KrawallInstruments(const ntdlib::commands::Sequence& commands,
                                                                     const std::map<uint32_t, uint32_t>& items,
                                                                     const Track& track)
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::kOne;
    std::map<std::pair<uint32_t, uint32_t>, uint16_t> instruments;
    uint16_t instrument = 0;
    for (std::size_t s = 0; s < track.stretches.size(); s++)
    {
        const auto first = items.find(track.stretches[s].first);
        const auto last = items.find(track.stretches[s].last);
        if (first == items.end() || last == items.end())
        {
            continue;
        }

        for (uint32_t i = first->second; i <= last->second && i < commands.items.size(); i++)
        {
            const ntdlib::commands::Command& command = commands.items[i];
            if (command.kind == Kind::kWait || command.kind == Kind::kOther || command.index != track.number)
            {
                continue;
            }

            if ((command.kind == Kind::kNote || command.kind == Kind::kNoteOff) && command.sample.value_or(0) != 0)
            {
                instrument = *command.sample;
            }
            else if (command.kind == Kind::kProgram && command.value.value > 0)
            {
                instrument = static_cast<uint16_t>(command.value.value / kOne);
            }

            if (command.kind == Kind::kNote)
            {
                instruments.emplace(std::pair(static_cast<uint32_t>(s), i), instrument);
            }
        }
    }

    return instruments;
}

// The item at each offset of `decoded`, a module of the driver `info`. None for a module that can't be decoded.
std::map<uint32_t, uint32_t> ItemsOfModule(const ntdlib::krawall::DriverInfo& info,
                                           const ntdlib::krawall::Decoded& decoded)
{
    namespace krawall = ntdlib::krawall;
    std::map<uint32_t, uint32_t> items;
    if (!decoded.opaque)
    {
        items = ItemsByOffset(krawall::ItemOffsets(decoded.sequence, krawall::TraitsOf(info)));
    }

    return items;
}

// Sets the program of each note of `track`, in song `sound` of `rom`. `items` contains the item of `decoded` at each
// offset.
void SetKrawallPrograms(const KrawallRom& rom, uint32_t sound, Track& track, const ntdlib::krawall::Decoded& decoded,
                        const std::map<uint32_t, uint32_t>& items)
{
    // A note's program is the sample of the game's table that its instrument plays at the note of its row. Every entry
    // of a row runs at the row's tick, and a note delay starts its note later in the row. The rows' ticks then give
    // each command's tick (edit::CommandRows).
    const std::map<std::pair<uint32_t, uint32_t>, uint16_t> instruments =
        KrawallInstruments(decoded.sequence, items, track);
    for (Note& note : track.notes)
    {
        const auto item = items.find(note.command);
        const auto instrument =
            item == items.end() ? instruments.end() : instruments.find(std::pair(note.stretch, item->second));
        const std::optional<int> sample =
            instrument == instruments.end()
                ? std::nullopt
                : rom.SampleOf(sound, instrument->second, decoded.sequence.items[item->second].key);
        note.program = static_cast<uint32_t>(std::max(sample.value_or(0), 0));
        note.bank_slot = 0;
        note.velocity = 127;
        note.midi_key = note.key;
        note.start_gives_tick = false;
    }
}

} // namespace

Sequence ReadSequence(const KrawallRom& rom, uint32_t sound)
{
    namespace krawall = ntdlib::krawall;
    const std::string name = rom.SoundName(sound);
    if (sound >= rom.SoundCount())
    {
        throw std::runtime_error("The ROM has no song " + std::to_string(sound) + ".");
    }

    // The performer plays one pass of the loop. A module that can't be read makes a performance with a warning and no
    // stretches.
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.loops = 1;
    try
    {
        const export_::krawall::Performance performance =
            export_::krawall::PerformSong(rom.ModulesRom(), rom.info, static_cast<int>(sound), settings);
        if (!performance.warnings.empty() &&
            std::ranges::all_of(performance.stretches, [](const auto& stretches) { return stretches.empty(); }))
        {
            throw std::invalid_argument("its module can't be read");
        }

        Sequence sequence = SequenceHead(performance, name, sound);
        sequence.driver = &rom.Rules();

        // The module's patterns, for the notes' instruments.
        const krawall::Decoded decoded = krawall::DecodeRomSequence(
            rom.ModulesRom(), rom.info, krawall::SequenceOfSong(rom.info, static_cast<int>(sound)));
        const std::map<uint32_t, uint32_t> items = ItemsOfModule(rom.info, decoded);

        const auto bank_name = [](uint32_t)
        {
            return std::string();
        };
        // The performance sets the volume of each channel at tick 0. A track therefore reads no start volume. With
        // one, MixAt would give the same values, and the volume would gain a point at tick 0 where it starts at 127.
        for (std::size_t i = 0; i < performance.tracks.size(); i++)
        {
            Track track = ReadTrack(static_cast<int>(i), performance.tracks[i], performance, ProgramPreset, bank_name);
            if (track.notes.empty() && track.stretches.empty())
            {
                continue;
            }

            ClipToLoop(sequence, track);

            SetKrawallPrograms(rom, sound, track, decoded, items);

            // The performer's pans run from -64 to 64.
            for (Setting& pan : track.pan)
            {
                pan.value = std::min(pan.value, 63);
            }

            track.program = track.notes.empty() ? -1 : static_cast<int>(track.notes.front().program);
            track.bank.clear();
            AddTrack(sequence, std::move(track));
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
