// SPDX-License-Identifier: MIT

// The MIDI maps of a sequence import: the programs that a MIDI file's channels propose, the drum rows that follow the
// chosen programs, and the maps that a file keeps for its banks.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "app/replace_common.h"
#include "app/replace_sequence_views.h"
#include "app/replace_view.h"
#include "edit/bytecode.h"
#include "edit/converter.h"
#include "edit/midi_imports.h"
#include "format/file.h"

namespace hokuto::app
{
namespace
{

using replace_common::PairedDrumNotes;
using replace_common::PairedProgram;
using replace_common::SoundBanks;

} // namespace

edit::ProgramMap ProposeMidiMap(ReplaceSource& source)
{
    if (source.midi.empty())
    {
        return {};
    }

    // The drum notes' pairings give new commands, and the programs' rows come from those.
    const std::vector<uint32_t> banks = source.banks ? *source.banks : SoundBanks(source, source.sound);
    const auto map_of = [&](int slot) -> const format::BankMidiMap*
    {
        const auto place = static_cast<std::size_t>(slot);
        if (slot < 0 || place >= banks.size() || banks[place] == ntdlib::sar::kNoItem)
        {
            return nullptr;
        }
        const auto map = source.file->bank_maps.find(ntdlib::sar::ItemNumber(banks[place]));
        return map != source.file->bank_maps.end() ? &map->second : nullptr;
    };
    const format::BankMidiMap* drum_map = map_of(source.midi_drums.slot);
    source.drum_notes = drum_map ? PairedDrumNotes(*drum_map, source.midi_drums.notes) : format::MidiDrumNotes();
    source.incoming =
        edit::ReadIncomingMidi(source.midi, source.file_name, edit::ConsoleOf(*source.file), source.drum_notes);

    edit::ProgramMap programs;
    for (const edit::Program& program :
         edit::ProgramsSelected(source.incoming.sequence, edit::kEmptySongStart).programs)
    {
        const format::BankMidiMap* map = map_of(program.slot);
        if (const std::optional<int> paired = map ? PairedProgram(*map, program.number) : std::nullopt)
        {
            programs[program] = *paired;
        }
    }

    return programs;
}

void FollowDrumRows(ReplaceSource& source, const ReplaceView& view)
{
    if (view.drums_changed && !source.midi.empty())
    {
        source.drum_notes = ChosenDrumNotes(view);
        source.incoming =
            edit::ReadIncomingMidi(source.midi, source.file_name, edit::ConsoleOf(*source.file), source.drum_notes);
    }
}

std::map<uint32_t, format::BankMidiMap> MidiMapsOf(const ReplaceView& view, const ReplaceSource& source,
                                                   const std::vector<uint32_t>& banks)
{
    if (source.midi.empty())
    {
        return {};
    }

    // A map starts as the file contains it. The DS's banks are numbers without a kind.
    const std::vector<uint32_t> slots = !banks.empty() ? banks
                                        : source.banks ? *source.banks
                                                       : SoundBanks(source, source.sound);
    std::map<uint32_t, format::BankMidiMap> maps;
    const auto map_of = [&](int slot) -> format::BankMidiMap*
    {
        const auto place = static_cast<std::size_t>(slot);
        if (slot < 0 || place >= slots.size() || slots[place] == ntdlib::sar::kNoItem)
        {
            return nullptr;
        }
        const uint32_t bank = ntdlib::sar::ItemNumber(slots[place]);
        const auto [map, added] = maps.try_emplace(bank);
        if (added)
        {
            if (const auto held = source.file->bank_maps.find(bank); held != source.file->bank_maps.end())
            {
                map->second = held->second;
            }
        }
        return &map->second;
    };

    // A map pairs a MIDI file's programs. A program past 127 comes from a drum note's pairing.
    for (const ProgramRow& row : view.programs)
    {
        if (format::BankMidiMap* map = row.program.number <= 127 ? map_of(row.program.slot) : nullptr)
        {
            map->programs[row.program.number] = row.target;
        }
    }
    for (const DrumRow& row : view.drums)
    {
        if (format::BankMidiMap* map = map_of(source.midi_drums.slot))
        {
            if (row.program >= 0)
            {
                map->drum_notes[row.note] = {.program = row.program, .key = row.key};
            }
            else
            {
                map->drum_notes.erase(row.note);
            }
        }
    }

    return maps;
}

void KeepMidiMaps(format::File& file, const std::map<uint32_t, format::BankMidiMap>& maps)
{
    for (const auto& [bank, map] : maps)
    {
        file.bank_maps.insert_or_assign(bank, map);
    }
}

} // namespace hokuto::app
