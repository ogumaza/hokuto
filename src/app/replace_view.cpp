// SPDX-License-Identifier: MIT

// The results of the choices in the Replace window: the chosen rate, wave, loop, wave archives, banks, programs, drum
// notes, voices, variables and name.

#include "app/replace_view.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"

#include "edit/files.h"
#include "edit/gba_midi.h"
#include "edit/replace.h"
#include "edit/variables.h"
#include "format/file.h"

namespace hokuto::app
{

std::optional<uint32_t> ChosenRate(const ReplaceView& view)
{
    return view.rate > 0 && view.rate < view.rates.size() ? std::optional<uint32_t>(view.rates[view.rate])
                                                          : std::nullopt;
}

std::span<const uint8_t> ChosenWave(const ReplaceView& view)
{
    return view.play < view.waves.size() ? std::span<const uint8_t>(view.waves[view.play]) : view.wave;
}

std::optional<edit::WaveLoop> ChosenLoop(const ReplaceView& view)
{
    return view.take_loop ? view.present_loop : std::nullopt;
}

std::vector<int> ChosenWaveArchives(const ReplaceView& view)
{
    std::vector<int> targets;
    for (const WaveArchiveRow& row : view.wave_archive_rows)
    {
        targets.push_back(row.target);
    }

    return targets;
}

std::vector<edit::ProgramFix> ChosenFixes(const ReplaceView& view)
{
    std::vector<edit::ProgramFix> fixes;
    for (const ProgramFixRow& row : view.program_fixes)
    {
        if (row.choice >= 0 && static_cast<std::size_t>(row.choice) < row.fixes.size())
        {
            fixes.push_back(row.fixes[static_cast<std::size_t>(row.choice)]);
        }
    }

    return fixes;
}

std::vector<uint32_t> ChosenBanks(const ReplaceView& view)
{
    std::size_t length = 0;
    for (std::size_t slot = 0; slot < view.slots.size(); slot++)
    {
        length = view.slots[slot] >= 0 ? slot + 1 : length;
    }

    std::vector<uint32_t> banks;
    for (std::size_t slot = 0; slot < std::min(length, view.slots.size()); slot++)
    {
        const int bank = view.slots[slot];
        banks.push_back(bank < 0 ? ntdlib::sar::kNoItem
                                 : ntdlib::sar::ItemId(ntdlib::sar::kBankItem, static_cast<uint32_t>(bank)));
    }

    return banks;
}

std::string NameOf(const ReplaceView& view)
{
    return view.name.data();
}

edit::GbaVoiceMap ChosenGbaVoices(const ReplaceView& view)
{
    edit::GbaVoiceMap voices;
    for (const GbaInstrumentRow& row : view.gba_rows)
    {
        voices[row.instrument] = row.voice;
    }

    return voices;
}

edit::ProgramMap ChosenPrograms(const ReplaceView& view)
{
    edit::ProgramMap programs;
    for (const ProgramRow& row : view.programs)
    {
        if (row.target != row.program.number)
        {
            programs[row.program] = row.target;
        }
    }

    return programs;
}

format::MidiDrumNotes ChosenDrumNotes(const ReplaceView& view)
{
    format::MidiDrumNotes notes;
    for (const DrumRow& row : view.drums)
    {
        if (row.program >= 0)
        {
            notes[row.note] = {.program = row.program, .key = row.key};
        }
    }

    return notes;
}

edit::VariableMap ChosenVariables(const ReplaceView& view)
{
    edit::VariableMap variables;
    for (const VariableRow& row : view.variables)
    {
        if (row.choice == VariableChoice::kMove && row.to != row.number)
        {
            variables.moves[row.number] = row.to;
        }
        else if (row.choice == VariableChoice::kFix && row.read)
        {
            variables.fixed[row.number] = row.value;
        }
    }

    return variables;
}

} // namespace hokuto::app
