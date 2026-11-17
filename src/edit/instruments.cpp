// SPDX-License-Identifier: MIT

#include "edit/instruments.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/sequence_export/performance.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/drivers.h"
#include "edit/konami.h"
#include "edit/mp2k.h"
#include "format/archive.h"
#include "song/driver.h"
#include "song/dse.h"

namespace hokuto::edit
{

namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

// A sound has four bank slots.
constexpr int kBankSlots = 4;

// The programs of the voice group of a GBA song: the VOICE command of MusicPlayer2000 takes a byte below 0x80.
constexpr int kGbaPrograms = 128;

// The program that a track plays before a program change: 0, or none on the GBA (instruments.h).
int64_t StartingProgram(const song::Console& console)
{
    return console.engine == format::Engine::kGba ? -1 : 0;
}

// The edits of the driver of `console`, or nothing for a console without one.
const DriverEdits* DriverEditsOf(const song::Console& console)
{
    return console.driver != nullptr ? &EditsOf(*console.driver) : nullptr;
}

// Whether `command`, a program change of a track `track` of `console`, selects the track's program. On some drivers,
// only some program changes do (DriverEdits::SelectsProgram). A track of -1 uses the rules of the other consoles.
bool SelectsProgram(const song::Console& console, int track, const Command& command)
{
    if (command.kind != Kind::kProgram)
    {
        return false;
    }

    const DriverEdits* edits = DriverEditsOf(console);
    return edits == nullptr || edits->SelectsProgram(console, track, command);
}

// The message for the user where a variable, a random number or a comparison gives the instrument of a note.
constexpr const char* kDecided =
    "A variable, a random number or a comparison sets the program or bank slot of some notes on this track. To change "
    "them, edit the commands in the List Editor (Edit > Open List Editor).";

// A command that set a part of a track's instrument: an item of the sequence, or nothing for the track's start.
using Setter = std::optional<uint32_t>;

// A part of a track's instrument as the track runs, its program or its slot: the commands that may have set it last,
// and its value. The value is missing where it comes from a variable, a random number or a comparison. A command that
// runs only if the last comparison held may have set it, or the one before.
struct Selection
{
    std::set<Setter> setters = {std::nullopt};
    std::optional<int64_t> value = 0;
};

// The instruments of the notes that each command that sets a part of the instrument sets it for. Nothing stands for a
// note whose instrument comes from a variable, a random number or a comparison.
using Uses = std::map<Setter, std::set<std::optional<Program>>>;

// `selection` after `command`, a program change or a bank select at item `item`.
void Select(Selection& selection, const Command& command, uint32_t item)
{
    if (!command.conditional)
    {
        selection.setters.clear();
    }

    selection.setters.insert(item);
    const bool fixed = command.value.source == Source::kValue && !command.conditional;
    selection.value = fixed ? std::optional<int64_t>(command.value.value / kOne) : std::nullopt;
}

// A DSE track selects a bank by its id: the whole id, its high byte or its low byte. The common form has no kinds for
// these.
constexpr uint16_t kDseBank = ntdlib::dse::kBank;
constexpr uint16_t kDseBankHigh = ntdlib::dse::kBankHigh;
constexpr uint16_t kDseBankLow = ntdlib::dse::kBankLow;

// `id` after `command`, a DSE command. Other commands leave it as it is.
uint16_t DseBankAfter(uint16_t id, const Command& command)
{
    if (command.kind != Kind::kOther)
    {
        return id;
    }

    const uint16_t opcode = command.encoding.opcode;
    if (opcode == kDseBank && command.bytes.size() == 2)
    {
        return static_cast<uint16_t>(command.bytes[0] << 8 | command.bytes[1]);
    }
    if (opcode == kDseBankHigh && command.bytes.size() == 1)
    {
        return static_cast<uint16_t>(command.bytes[0] << 8 | (id & 0xFF));
    }
    if (opcode == kDseBankLow && command.bytes.size() == 1)
    {
        return static_cast<uint16_t>((id & 0xFF00) | command.bytes[0]);
    }

    return id;
}

// What the opening of a DSE track selects: the slot of its bank, and whether a command selects a bank at all.
struct DseOpeningBank
{
    int slot = 0;
    bool selected = false;
};

// The bank selects that the opening of `track`, a DSE track of sequence `sequence` of `file`, runs before time passes,
// in their order.
std::vector<Command> DseOpeningSelects(const format::File& file, uint32_t sequence, const song::Track& track)
{
    std::vector<Command> selects;
    for (const CommandRow& row : CommandRows(file, sequence, track.stretches, track.notes, std::nullopt))
    {
        const Command& command = CommandAt(file, sequence, row.command);
        if (row.stretch != 0 || command.kind == Kind::kWait || command.kind == Kind::kNote ||
            command.kind == Kind::kEnd)
        {
            break;
        }

        const uint16_t opcode = command.encoding.opcode;
        if (command.kind == Kind::kOther && (opcode == kDseBank || opcode == kDseBankHigh || opcode == kDseBankLow))
        {
            selects.push_back(command);
        }
    }

    return selects;
}

// The offset of the last program change that the opening of `track`, a DSE track of sequence `sequence` of `file`,
// runs before time passes, or nothing.
std::optional<uint32_t> DseOpeningProgram(const format::File& file, uint32_t sequence, const song::Track& track)
{
    std::optional<uint32_t> program;
    for (const CommandRow& row : CommandRows(file, sequence, track.stretches, track.notes, std::nullopt))
    {
        const Command& command = CommandAt(file, sequence, row.command);
        if (row.stretch != 0 || command.kind == Kind::kWait || command.kind == Kind::kNote ||
            command.kind == Kind::kEnd)
        {
            break;
        }

        if (command.kind == Kind::kProgram)
        {
            program = row.command;
        }
    }

    return program;
}

DseOpeningBank DseBankOfOpening(const format::File& file, uint32_t sequence, const song::Track& track)
{
    const std::vector<uint16_t> banks = DseBankIds(file, sequence);
    uint16_t id = banks.empty() ? 0 : banks.front();
    DseOpeningBank found;
    for (const Command& select : DseOpeningSelects(file, sequence, track))
    {
        id = DseBankAfter(id, select);
        found.selected = true;
    }

    const auto held = std::find(banks.begin(), banks.end(), id);
    found.slot = held == banks.end() ? 0 : static_cast<int>(held - banks.begin());

    return found;
}

// The commands that set the bank slot and the program of each note of a track, by the commands that set them. Each
// note's item gives the instruments that it played, and on DSE the id of the bank that its track had selected last.
struct Walk
{
    Uses slots;
    Uses programs;
    std::map<uint32_t, std::set<std::optional<Program>>> notes;
    std::map<uint32_t, uint16_t> dse_banks;
};

// The commands of `commands` that set the instrument of each note of a track of `console` that ran `stretches`, in the
// order that they ran. `offsets` gives the offset of each item (Offsets). `dse_banks` contains the ids of the banks in
// the slots of a DSE sound. As in song::ReadSequence, a note's slot is the place there of the bank that its track has
// selected, or 0 where they don't hold it. Throws std::invalid_argument with a message for the user where the track's
// commands aren't where `stretches` shows them.
Walk WalkTrack(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
               std::span<const song::Stretch> stretches, const song::Console& console, int track,
               std::span<const uint16_t> dse_banks = {})
{
    const DriverEdits* edits = DriverEditsOf(console);
    const bool bank_selects_program = edits != nullptr && edits->BankSelectsProgram();
    const bool notes_select_samples = edits != nullptr && edits->NotesSelectSamples();
    Walk walk;
    Selection slot;
    Selection program;
    program.value = StartingProgram(console);
    uint16_t dse_bank = dse_banks.empty() ? 0 : dse_banks.front();
    for (const song::Stretch& stretch : stretches)
    {
        const std::optional<uint32_t> first = ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = ItemAtOffset(offsets, stretch.last);
        if (!first || !last || *last < *first)
        {
            throw std::invalid_argument("The track commands aren't where the inspector shows them.");
        }

        for (uint32_t i = *first; i <= *last; ++i)
        {
            const Command& command = commands.items[i];
            if (console.engine == format::Engine::kDse)
            {
                dse_bank = DseBankAfter(dse_bank, command);
                const auto held = std::find(dse_banks.begin(), dse_banks.end(), dse_bank);
                slot.value = held == dse_banks.end() ? 0 : held - dse_banks.begin();
            }

            if (command.kind == Kind::kBankSelect)
            {
                Select(slot, command, i);

                // On some drivers, such as Nintendo R&D2's driver, a bank select also selects the first instrument of
                // its bank, program 0 (ntdlib's sequencer, SetBank).
                if (bank_selects_program)
                {
                    Command first = command;
                    first.value = {Source::kValue, 0};
                    Select(program, first, i);
                }
            }
            else if (command.kind == Kind::kProgram)
            {
                if (SelectsProgram(console, track, command))
                {
                    Select(program, command, i);
                }
            }
            else if (notes_select_samples && command.kind == Kind::kNote && command.sample)
            {
                // A note of some drivers, such as Konami's driver, names its sample. It sets its instrument alone. A
                // note that plays on the next track belongs to that track.
                if (!KonamiPlaysOnNextTrack(command))
                {
                    walk.programs[i].insert(Program{0, static_cast<int>(*command.sample)});
                }
            }
            else if (command.kind == Kind::kNote)
            {
                std::optional<Program> played;
                if (slot.value && program.value)
                {
                    played = Program{static_cast<int>(*slot.value), static_cast<int>(*program.value)};
                }

                for (const Setter& setter : slot.setters)
                {
                    walk.slots[setter].insert(played);
                }

                for (const Setter& setter : program.setters)
                {
                    walk.programs[setter].insert(played);
                }

                walk.notes[i].insert(played);
                walk.dse_banks[i] = dse_bank;
            }
        }
    }

    return walk;
}

// The setters in `uses` of the notes that play `instrument`.
std::set<Setter> SettersOf(const Uses& uses, const Program& instrument)
{
    std::set<Setter> setters;
    for (const auto& [setter, played] : uses)
    {
        if (played.contains(instrument))
        {
            setters.insert(setter);
        }
    }

    return setters;
}

// Throws std::invalid_argument with a message for the user where `to` is outside the bank slots and programs of
// `console`, or where it changes the slot from `from_slot` on a console whose tracks can't select a bank. A DSE sound
// has a slot for each bank that it loads, `dse_banks` of them. A DSE track selects one with DSE's bank selects, which
// the common form has no kind for.
void CheckInstrument(const song::Console& console, int from_slot, Program to, std::size_t dse_banks = 0)
{
    if (console.engine == format::Engine::kDse && to.slot >= 0 && to.number >= 0)
    {
        if (to.slot != from_slot && static_cast<std::size_t>(to.slot) >= dse_banks)
        {
            throw std::invalid_argument("This sound has no bank in slot " + std::to_string(to.slot) + ".");
        }
        return;
    }

    if (to.slot < 0 || to.slot >= kBankSlots || to.number < 0)
    {
        throw std::invalid_argument("Bank slots are 0–3, and program numbers start at 0.");
    }

    if (to.slot != from_slot && !TracksSelectBanks(console))
    {
        const DriverEdits* edits = DriverEditsOf(console);
        const std::optional<std::string> refused = edits != nullptr ? edits->RefusedBankSelect() : std::nullopt;
        throw std::invalid_argument(
            console.engine == format::Engine::kGba
                ? "A GBA song uses a single voice group. Its tracks can't select another."
            : console.engine == format::Engine::kDs
                ? "A DS sequence uses the single bank set in its archive entry. Its tracks can't select "
                  "another."
                : refused.value_or("A Wii sound uses only one bank. Its sequences can't select another bank "
                                   "slot."));
    }

    if (console.engine == format::Engine::kGba && to.number >= kGbaPrograms)
    {
        throw std::invalid_argument("Programs in a GBA voice group must be from 0 to 127.");
    }
}

// The command at a DSE track's opening that selects the bank of slot `slot`, of the banks with the ids `banks`. A
// channel starts without one of the sound's banks, and a program selected then stays at 0. A sequence that no sound
// plays has no banks to select.
std::vector<Command> DseOpeningSelect(std::span<const uint16_t> banks, int slot)
{
    if (static_cast<std::size_t>(slot) >= banks.size())
    {
        return {};
    }

    return {DseBankSelect(banks[static_cast<std::size_t>(slot)])};
}

// The item of the first command of a track that ran in `stretches`. New commands at the track's opening go before it.
uint32_t OpeningItem(const format::File& file, const ntdlib::commands::Sequence& commands,
                     const std::vector<uint32_t>& offsets, std::span<const song::Stretch> stretches)
{
    if (stretches.empty() || stretches.front().entry != ntdlib::sequence_export::Entry::kOpen)
    {
        throw std::invalid_argument("The track commands aren't where the inspector shows them.");
    }
    if (KeepsPlaces(file, commands))
    {
        throw std::invalid_argument("Can't add a command. Commands in this sequence must keep their positions.");
    }

    return *ItemAtOffset(offsets, stretches.front().first);
}

// `edited` after new commands at the opening of track `track`. The items from item `first` on move past them.
void AddOpening(Edited& edited, uint32_t sequence, uint32_t first, int track, std::vector<Command> opening)
{
    const OpeningInsertion added = InsertAtOpening(edited.file, sequence, first, track, std::move(opening));
    const auto moved = [&added](uint32_t& item)
    {
        if (item >= added.first)
        {
            item += added.count;
        }
    };
    for (std::optional<uint32_t>& item : edited.items)
    {
        if (item)
        {
            moved(*item);
        }
    }

    std::ranges::for_each(edited.added, moved);
}

// Whether time passes at `command`, a command of `console`, for the commands that a track's opening runs: a wait, a
// note or an end, and on some drivers other commands too (DriverEdits::PassesTime).
bool PassesTime(const song::Console& console, const Command& command)
{
    const DriverEdits* edits = DriverEditsOf(console);
    return command.kind == Kind::kWait || command.kind == Kind::kNote || command.kind == Kind::kEnd ||
           (edits != nullptr && edits->PassesTime(command));
}

// Whether `command`, a command of track `track` of `console`, selects an instrument or a bank (SelectsProgram).
bool SelectsInstrument(const song::Console& console, int track, const Command& command)
{
    return SelectsProgram(console, track, command) || command.kind == Kind::kBankSelect;
}

// `walk` also gains the notes of the track before it that play on its track: `previous` contains the stretches of that
// track. Only a revision of Konami's driver has such notes.
void AddNotesFromBefore(Walk& walk, const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                        std::span<const song::Stretch> previous)
{
    for (const song::Stretch& stretch : previous)
    {
        const std::optional<uint32_t> first = ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = ItemAtOffset(offsets, stretch.last);
        for (uint32_t i = first.value_or(1); first && last && i <= *last; ++i)
        {
            if (KonamiPlaysOnNextTrack(commands.items[i]))
            {
                walk.programs[i].insert(Program{0, static_cast<int>(*commands.items[i].sample)});
            }
        }
    }
}

// What a new program change of track `track` selects, by the index of its command (DriverEdits::ProgramIndex).
uint8_t ProgramIndexOf(const song::Console& console, int track)
{
    const DriverEdits* edits = DriverEditsOf(console);
    return edits != nullptr ? edits->ProgramIndex(console, track) : 0;
}

Command ProgramCommand(int program, uint8_t index = 0)
{
    return song::ValueCommand(Kind::kProgram, program, index);
}

Command SlotCommand(int slot)
{
    return song::ValueCommand(Kind::kBankSelect, slot);
}

// Throws std::invalid_argument with a message for the user unless each setter of `setters` sets its part of the
// instrument for notes of one instrument alone, and decides it. `other` says what else the notes play.
void CheckAlone(const Uses& uses, const std::set<Setter>& setters, const char* other)
{
    for (const Setter& setter : setters)
    {
        const std::set<std::optional<Program>>& played = uses.at(setter);
        if (played.contains(std::nullopt))
        {
            throw std::invalid_argument(kDecided);
        }
        if (played.size() > 1)
        {
            throw std::invalid_argument(std::string(other) +
                                        " To change the instrument, edit the commands in the List "
                                        "Editor (Edit > Open List Editor).");
        }
    }
}

} // namespace

bool TracksSelectBanks(const song::Console& console)
{
    return console.engine == format::Engine::kDse || song::Runs(console, Kind::kBankSelect, 0);
}

Command DseBankSelect(uint16_t id)
{
    Command select;
    select.kind = Kind::kOther;
    select.encoding.opcode = kDseBank;
    select.bytes = {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};

    return select;
}

std::vector<uint16_t> DseBankIds(const format::File& file, uint32_t sequence)
{
    const std::vector<uint8_t> set = format::ExportArchive(file);
    const std::shared_ptr<const song::DseSet> sounds = song::LoadDseSet(set, ntdlib::dse::Build::kLater);
    const std::vector<std::optional<uint32_t>> places =
        format::DseSoundSequences(file, set, song::DseSoundRefs(*sounds));
    std::vector<uint16_t> ids;
    const auto place = std::find(places.begin(), places.end(), std::optional<uint32_t>(sequence));
    if (place != places.end())
    {
        for (const auto& bank : sounds->sounds[static_cast<std::size_t>(place - places.begin())].banks)
        {
            ids.push_back(song::DseBankId(*bank));
        }
    }

    return ids;
}

bool DseOpeningSelectsBank(const format::File& file, uint32_t sequence, const song::Track& track)
{
    return !DseOpeningSelects(file, sequence, track).empty();
}

Setters InstrumentSetters(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                          std::span<const song::Stretch> stretches, Program instrument, const song::Console& console,
                          std::span<const song::Stretch> previous, int track)
{
    Walk walk = WalkTrack(commands, offsets, stretches, console, track);
    AddNotesFromBefore(walk, commands, offsets, previous);
    Setters setters;
    for (const Uses* uses : {&walk.programs, &walk.slots})
    {
        for (const Setter& setter : SettersOf(*uses, instrument))
        {
            if (setter)
            {
                setters.items.insert(*setter);
            }
            else
            {
                setters.start = true;
            }
        }
    }

    return setters;
}

namespace
{

// Throws std::invalid_argument with a message for the user for an instrument that no note of `program_uses` plays on
// `console` (EditInstrument). The message says whether a note whose instrument isn't decided may play it.
void RefuseUnplayed(const Uses& program_uses, const song::Console& console)
{
    const auto undecided = [](const auto& use)
    {
        return use.second.contains(std::nullopt);
    };
    const bool decided = std::none_of(program_uses.begin(), program_uses.end(), undecided);
    const DriverEdits* edits = DriverEditsOf(console);
    if (decided && edits != nullptr && edits->NotesPlayOnNextTrack())
    {
        throw std::invalid_argument(std::string(kNoNotesInstrumentRefusal));
    }

    throw std::invalid_argument(decided ? "No notes on this track use that instrument." : kDecided);
}

// Sets each command of `setters`, items of `commands` at `offsets`, to the value `value` (EditInstrument). `places` and
// `changed` gain the commands that change. A note that sets its sample is added to `sample_notes` instead.
void ChangeSetters(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                   const std::set<Setter>& setters, int value, std::vector<CommandPlace>& places,
                   std::vector<Command>& changed, std::vector<uint32_t>& sample_notes)
{
    for (const Setter& setter : setters)
    {
        if (setter && commands.items[*setter].kind == Kind::kNote)
        {
            sample_notes.push_back(*setter);
        }
        else if (setter)
        {
            Command command = commands.items[*setter];
            command.value.value = int64_t{value} * kOne;
            places.push_back({offsets[*setter], {}});
            changed.push_back(command);
        }
    }
}

// Throws std::invalid_argument with a message for the user when the notes of `commands` play a sample that the
// revision of Konami's driver of `console` doesn't have. The driver's encoding of the commands checks the samples.
void CheckKonamiSamples(const ntdlib::commands::Sequence& commands, const song::Console& console)
{
    try
    {
        Offsets(commands, console);
    }
    catch (const std::invalid_argument&)
    {
        throw std::invalid_argument("This revision of Konami's driver has no sample with that number.");
    }
}

// Throws std::invalid_argument with a message for the user when a command of `programs` in `commands` is a bank select
// (EditInstrument). On Nintendo R&D2's driver, a bank select selects program 0 of its bank.
void RefuseBankSelects(const ntdlib::commands::Sequence& commands, const std::set<Setter>& programs)
{
    const auto bank_select = [&commands](const Setter& setter)
    {
        return setter && commands.items[*setter].kind == Kind::kBankSelect;
    };
    if (std::ranges::any_of(programs, bank_select))
    {
        throw std::invalid_argument(
            "These notes use program 0 of the bank that a bank select selects on Nintendo R&D2's driver. To change "
            "the program, add a program change after the bank select in the List Editor (Edit > Open List Editor).");
    }
}

// Sets the sample of the notes `sample_notes` of sequence `sequence` of `result`, items before the edit, to the sample
// of `to` (EditInstrument). Throws std::invalid_argument with a message for the user for a sample that the driver of
// `console` doesn't have.
void SetSamples(Edited& result, uint32_t sequence, std::span<const uint32_t> sample_notes, Program to,
                const song::Console& console)
{
    if (sample_notes.empty())
    {
        return;
    }

    ntdlib::commands::Sequence notes = *result.file.sequences[sequence];
    for (const uint32_t note : sample_notes)
    {
        notes.items[*result.items.at(note)].sample = static_cast<uint16_t>(to.number);
    }

    result.file.sequences[sequence] = std::move(notes);
    CheckKonamiSamples(*result.file.sequences[sequence], console);
}

// A DSE bank select that goes in before item `item` of a sequence. With `entered`, code that entered the sequence at
// the item runs the new command first.
struct DseInsertion
{
    uint32_t item = 0;
    Command command;
    bool entered = false;
    bool back = false; // the select back to the bank of a run
};

// Whether `command`, a DSE command, selects a bank, or a part of its id (DseBankAfter).
bool SelectsDseBank(const Command& command)
{
    const uint16_t opcode = command.encoding.opcode;
    return command.kind == Kind::kOther && (opcode == kDseBank || opcode == kDseBankHigh || opcode == kDseBankLow);
}

// Whether `command`, a DSE command, changes where its track goes on: the loop's point (0x99), a repeat's start, end or
// exit (0x9C to 0x9E), or the track's end.
bool MovesDseTrack(const Command& command)
{
    const uint16_t opcode = command.encoding.opcode;
    return command.kind == Kind::kLoopStart || command.kind == Kind::kLoopEnd || command.kind == Kind::kEnd ||
           (command.kind == Kind::kOther && (opcode == ntdlib::dse::kLoopPoint || opcode == ntdlib::dse::kRepeatExit));
}

// Whether `command`, a DSE command, sets a part of its channel that a selection of a program resets: the bend range,
// an LFO or the envelope (ntdlib's Driver::State::SelectProgram).
bool SetsDseVoice(const Command& command)
{
    if (command.kind == Kind::kBendRange)
    {
        return true;
    }
    if (command.kind != Kind::kOther)
    {
        return false;
    }

    switch (command.encoding.opcode)
    {
    case ntdlib::dse::kEnvelopeDefaults:
    case ntdlib::dse::kAttackBegin:
    case ntdlib::dse::kAttackTime:
    case ntdlib::dse::kHoldTime:
    case ntdlib::dse::kDecayAndSustain:
    case ntdlib::dse::kSustainTime:
    case ntdlib::dse::kReleaseTime:
    case ntdlib::dse::kBendLfo:
    case ntdlib::dse::kBendLfoEnvelope:
    case ntdlib::dse::kBendLfoSwitch:
    case ntdlib::dse::kVolumeLfo:
    case ntdlib::dse::kVolumeLfoEnvelope:
    case ntdlib::dse::kVolumeLfoSwitch:
    case ntdlib::dse::kPanLfo:
    case ntdlib::dse::kPanLfoEnvelope:
    case ntdlib::dse::kPanLfoSwitch:
    case ntdlib::dse::kLfo:
    case ntdlib::dse::kLfoEnvelope:
    case ntdlib::dse::kLfoParameter:
    case ntdlib::dse::kLfoSwitch:
        return true;
    default:
        return false;
    }
}

// Whether `command`, a DSE command, selects a program or a bank. The driver then selects the channel's program again.
bool SelectsDseInstrument(const Command& command)
{
    return command.kind == Kind::kProgram || SelectsDseBank(command);
}

// Throws std::invalid_argument with a message for the user where a new bank select on a DSE track would reset settings
// that the track's commands from item `begin` to item `end` of `commands` made. The message names those commands as the
// List Editor shows them. Where they are all bend ranges, it says to remove them first: the List Editor can add them
// again. It can't add an LFO or envelope command.
[[noreturn]] void RefuseDseResets(const ntdlib::commands::Sequence& commands, uint32_t begin, uint32_t end)
{
    std::vector<std::string> names;
    for (uint32_t item = begin; item <= end; item++)
    {
        const Command& command = commands.items[item];
        if (!SetsDseVoice(command))
        {
            continue;
        }

        std::array<char, 24> name{};
        if (command.kind == Kind::kBendRange)
        {
            std::snprintf(name.data(), name.size(), "Bend Range");
        }
        else
        {
            std::snprintf(name.data(), name.size(), "Opcode 0x%X", command.encoding.opcode);
        }
        if (std::ranges::find(names, std::string(name.data())) == names.end())
        {
            names.emplace_back(name.data());
        }
    }

    std::string list;
    for (const std::string& name : names)
    {
        list += (list.empty() ? "" : ", ") + name;
    }

    // The List Editor inserts a bend range again after the new select, but no LFO or envelope command.
    if (std::ranges::all_of(names, [](const std::string& name) { return name == "Bend Range"; }))
    {
        throw std::invalid_argument(
            "Can't change the bank. A new bank select would reset this track's bend range, "
            "LFOs and envelope. Remove these commands in the List Editor first: " +
            list + ".");
    }

    throw std::invalid_argument(
        "Can't change the bank. A new bank select would reset this track's bend range, LFOs "
        "and envelope. These commands set them: " +
        list + ".");
}

// The bank and the program that a DSE track has selected before item `stop` of `commands`, running on from item
// `begin` with the bank `bank`. The program is missing where a variable, a random number or a comparison gives it.
std::pair<uint16_t, std::optional<int64_t>> DseInstrumentBefore(const ntdlib::commands::Sequence& commands,
                                                                uint32_t begin, uint32_t stop, uint16_t bank)
{
    std::optional<int64_t> program = 0;
    for (uint32_t item = begin; item < stop; item++)
    {
        const Command& command = commands.items[item];
        bank = DseBankAfter(bank, command);
        if (command.kind == Kind::kProgram)
        {
            const bool fixed = command.value.source == Source::kValue && !command.conditional;
            program = fixed ? std::optional<int64_t>(command.value.value / kOne) : std::nullopt;
        }
    }

    return {bank, program};
}

// The bank selects that give the notes of `from` on a DSE track the bank with the id `to` (EditInstrument). The track
// ran `stretches` of `commands`, whose items start at `offsets`, and `walk` contains what its notes played. `banks`
// contains the ids of the sound's banks. The notes of `from` form runs. A run ends at a note of another instrument, a
// program change, a bank select, an item where the track entered the sequence, and a command that changes where the
// track goes. Each run gets a select of `to` before it, and a select after it of the bank that its notes played before
// the edit.
//
// A bank select selects the channel's program again, and that resets the channel's bend range, LFOs and envelope
// (ntdlib's Driver::State::SelectBank and SelectProgram). The new selects therefore go where the driver resets them
// anyway. The select of `to` goes just before the program change, or just after the bank select, that comes before the
// run with no note between. The select back goes just before the next program change or bank select, and stays out
// where that is a select of a whole id. Neither is needed where nothing runs between the two. A select back also stays
// out at the track's end where the track has no loop. A run that goes on to the end of a looped track needs no select
// back where no note comes between the loop's point and its select of `to`: every later pass then plays the new bank.
// The scan for the place of that select may then cross the loop's point. Elsewhere a new select goes beside the run's
// notes, just before the item where the scan for its place stopped, and only in a track without commands that set
// those parts of the channel. The loop's point is the track's last 0x99: the driver goes back to the last one that it
// ran.
//
// Throws std::invalid_argument with a message for the user where a note of `from` plays another instrument on another
// pass, where a new select would reset such settings, where a new select after the loop's point would run on later
// passes with another instrument than on the first, and for a sequence whose commands can't move.
std::vector<DseInsertion> DseRunSelects(const format::File& file, const ntdlib::commands::Sequence& commands,
                                        const std::vector<uint32_t>& offsets, std::span<const song::Stretch> stretches,
                                        const Walk& walk, Program from, std::span<const uint16_t> banks, uint16_t to)
{
    if (KeepsPlaces(file, commands))
    {
        throw std::invalid_argument("Can't add a command. Commands in this sequence must keep their positions.");
    }

    // The items where the track entered the sequence, and the first and last items that it ran.
    std::set<uint32_t> entries;
    uint32_t begin = std::numeric_limits<uint32_t>::max();
    uint32_t end = 0;
    for (const song::Stretch& stretch : stretches)
    {
        const uint32_t first = *ItemAtOffset(offsets, stretch.first);
        entries.insert(first);
        begin = std::min(begin, first);
        end = std::max(end, *ItemAtOffset(offsets, stretch.last));
    }

    // The loop's last point, and whether the track sets a part of its channel that a bank select resets.
    std::optional<uint32_t> loop_point;
    bool sets_voice = false;
    for (uint32_t item = begin; item <= end; item++)
    {
        const Command& command = commands.items[item];
        if (command.kind == Kind::kOther && command.encoding.opcode == ntdlib::dse::kLoopPoint)
        {
            loop_point = item;
        }
        sets_voice = sets_voice || SetsDseVoice(command);
    }

    // The notes of `from`, and whether one of them plays another instrument on another pass. The edit refuses those
    // after the loop's guard below: a later pass of the loop gives them the other instrument.
    std::vector<uint32_t> notes;
    bool mixed = false;
    for (const auto& [item, played] : walk.notes)
    {
        if (!played.contains(from))
        {
            continue;
        }
        mixed = mixed || played.size() > 1;
        notes.push_back(item);
    }

    // Where a scan for a place of a new select stops: a note, a command that changes where the track goes, and an item
    // where the track entered. A scan that may cross the loop's point stops at neither the point nor the entry after
    // it.
    const auto stops = [&](uint32_t item, bool across_loop = false)
    {
        const Command& command = commands.items[item];
        if (across_loop && (item == loop_point || (loop_point && item == *loop_point + 1)))
        {
            return false;
        }
        return entries.contains(item) || command.kind == Kind::kNote || MovesDseTrack(command);
    };

    std::vector<DseInsertion> insertions;
    std::size_t next = 0;
    while (next < notes.size())
    {
        const uint32_t first = notes[next++];
        uint32_t last = first;
        for (uint32_t item = first + 1; item <= end; item++)
        {
            if (next < notes.size() && notes[next] == item && !entries.contains(item))
            {
                last = notes[next++];
            }
            else if (stops(item) || SelectsDseInstrument(commands.items[item]))
            {
                break;
            }
        }

        // The select back: just before the next program change or bank select after the run.
        bool needed = true;
        std::optional<uint32_t> back;
        uint32_t item = last + 1;
        for (; item <= end; item++)
        {
            const Command& command = commands.items[item];
            if (SelectsDseInstrument(command))
            {
                needed = command.kind != Kind::kOther || command.encoding.opcode != kDseBank;
                back = item;
                break;
            }
            if (command.kind == Kind::kEnd && !loop_point)
            {
                needed = false;
                break;
            }
            if (stops(item))
            {
                break;
            }
        }
        needed = needed && item <= end;

        // A run that goes on to the end of a looped track can take its select before the loop's point.
        const bool to_loop_end = needed && !back && loop_point && commands.items[item].kind == Kind::kEnd;

        // The select of `to`: just before the program change or just after the bank select before the run.
        std::optional<DseInsertion> select;
        for (uint32_t at = first; (to_loop_end || !entries.contains(first)) && at-- > begin;)
        {
            const Command& command = commands.items[at];
            if (command.kind == Kind::kProgram)
            {
                select = DseInsertion{at, DseBankSelect(to), entries.contains(at)};
            }
            else if (SelectsDseBank(command))
            {
                select = DseInsertion{at + 1, DseBankSelect(to), false};
            }
            if (select || stops(at, to_loop_end))
            {
                break;
            }
        }

        // Every later pass then reaches the new select with the new bank, and plays no note before it.
        const auto notes_before_select = [&]
        {
            for (uint32_t at = *loop_point + 1; at < select->item; at++)
            {
                if (commands.items[at].kind == Kind::kNote)
                {
                    return true;
                }
            }
            return false;
        };
        if (to_loop_end && select && !notes_before_select())
        {
            needed = false;
        }
        else if (!select && sets_voice)
        {
            RefuseDseResets(commands, begin, end);
        }
        else if (!select)
        {
            select = DseInsertion{first, DseBankSelect(to), entries.contains(first)};
        }

        insertions.push_back(*select);
        if (needed && !back && sets_voice)
        {
            RefuseDseResets(commands, begin, end);
        }
        if (needed)
        {
            insertions.push_back({back.value_or(item), DseBankSelect(walk.dse_banks.at(last)), false, true});
        }
    }

    // A later pass of the loop starts with the instrument that the track ends with. A new select after the loop's
    // point runs there too. The edit holds only where the first pass starts the loop with the same instrument.
    if (loop_point && std::ranges::any_of(insertions, [&](const DseInsertion& at) { return at.item > *loop_point; }))
    {
        const uint16_t start = banks.empty() ? 0 : banks.front();
        const auto at_point = DseInstrumentBefore(commands, begin, *loop_point, start);
        const auto at_end = DseInstrumentBefore(commands, begin, end + 1, start);
        if (at_point.first != at_end.first || !at_point.second || at_point.second != at_end.second)
        {
            throw std::invalid_argument(
                "Can't change the bank. This track's loop starts with another instrument than "
                "the track ends with.");
        }
    }
    if (mixed)
    {
        throw std::invalid_argument(
            "Can't change the bank. Some notes of this instrument play another instrument on another pass of the "
            "track.");
    }

    // A select back and the next run's select of `to` at the same item cancel out: nothing runs between them.
    std::vector<DseInsertion> kept;
    for (DseInsertion& insertion : insertions)
    {
        if (!kept.empty() && kept.back().back && !insertion.back && kept.back().item == insertion.item &&
            !insertion.entered)
        {
            kept.pop_back();
            continue;
        }
        kept.push_back(std::move(insertion));
    }

    return kept;
}

// `edited` after `insertions` go into sequence `sequence`. Their items name the items before the edit of `edited`.
// `Edited::added` gains the new commands.
void InsertDseSelects(Edited& edited, uint32_t sequence, std::vector<DseInsertion> insertions)
{
    if (insertions.empty())
    {
        return;
    }

    for (DseInsertion& insertion : insertions)
    {
        insertion.item = *edited.items.at(insertion.item);
    }
    std::ranges::stable_sort(insertions, {}, &DseInsertion::item);

    const ntdlib::commands::Sequence& before = *edited.file.sequences[sequence];
    ntdlib::commands::Sequence after = before;
    after.items.clear();
    std::vector<uint32_t> firsts(before.items.size() + 1);
    std::vector<uint32_t> places(before.items.size() + 1);
    std::vector<uint32_t> added;
    auto insertion = insertions.begin();
    for (uint32_t item = 0; item <= before.items.size(); item++)
    {
        std::optional<uint32_t> entered;
        for (; insertion != insertions.end() && insertion->item == item; ++insertion)
        {
            const auto at = static_cast<uint32_t>(after.items.size());
            if (insertion->entered && !entered)
            {
                entered = at;
            }
            added.push_back(at);
            after.items.push_back(insertion->command);
        }

        places[item] = static_cast<uint32_t>(after.items.size());
        firsts[item] = entered.value_or(places[item]);
        if (item < before.items.size())
        {
            after.items.push_back(before.items[item]);
        }
    }

    RenumberItems(after, edited, sequence, firsts, places);
    edited.file.sequences[sequence] = std::move(after);
    edited.added.insert(edited.added.end(), added.begin(), added.end());
}

} // namespace

Edited EditInstrument(const format::File& file, uint32_t sequence, int track, std::span<const song::Stretch> stretches,
                      Program from, Program to, std::span<const song::Stretch> previous)
{
    const song::Console console = ConsoleOf(file);
    const DriverEdits* edits = DriverEditsOf(console);
    if (edits != nullptr)
    {
        edits->CheckInstruments();
    }

    const std::vector<uint16_t> dse_banks =
        console.engine == format::Engine::kDse ? DseBankIds(file, sequence) : std::vector<uint16_t>();
    CheckInstrument(console, from.slot, to, dse_banks.size());
    if (edits != nullptr)
    {
        edits->CheckTrackInstrument(console, track);
    }

    // The track's commands in the order that they ran. Each note plays the instrument that its track has selected.
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(commands, console);
    Walk walk = WalkTrack(commands, offsets, stretches, console, track, dse_banks);
    AddNotesFromBefore(walk, commands, offsets, previous);
    const Uses& slot_uses = walk.slots;
    const Uses& program_uses = walk.programs;

    // Where no note is found that plays `from`, a note whose instrument isn't decided may play it.
    if (SettersOf(program_uses, from).empty())
    {
        RefuseUnplayed(program_uses, console);
    }

    // The commands that set the instrument of the notes that play `from`. Each has to set it for them alone.
    const std::set<Setter> programs = to.number != from.number ? SettersOf(program_uses, from) : std::set<Setter>();
    // A DSE track's notes take another bank from new bank selects around them (DseRunSelects).
    const std::set<Setter> slots = to.slot != from.slot && console.engine != format::Engine::kDse
                                       ? SettersOf(slot_uses, from)
                                       : std::set<Setter>();
    // On some drivers, such as Rare's driver, the program belongs to the channel, and every track on the channel plays
    // it.
    const std::optional<uint8_t> channel = edits != nullptr && edits->ChannelsHoldPrograms() && !programs.empty()
                                               ? edits->ProgramChannel(commands, offsets, stretches)
                                               : std::nullopt;
    CheckAlone(program_uses, programs,
               "Notes from another bank slot use the same program change as the notes of this instrument.");
    CheckAlone(slot_uses, slots, "Notes of another program use the same bank select as the notes of this instrument.");
    RefuseBankSelects(commands, programs);
    const std::vector<DseInsertion> dse_selects =
        console.engine == format::Engine::kDse && to.slot != from.slot
            ? DseRunSelects(file, commands, offsets, stretches, walk, from, dse_banks,
                            dse_banks.at(static_cast<std::size_t>(to.slot)))
            : std::vector<DseInsertion>();

    // The commands take the new values, and keep their items.
    std::vector<CommandPlace> places;
    std::vector<Command> changed;
    std::vector<uint32_t> sample_notes;
    ChangeSetters(commands, offsets, programs, to.number, places, changed, sample_notes);
    ChangeSetters(commands, offsets, slots, to.slot, places, changed, sample_notes);
    Edited result = ChangeCommands(file, sequence, places, changed);
    SetSamples(result, sequence, sample_notes, to, console);

    // Where the track's start sets the instrument, new commands at the track's start set the new one. The items from
    // the track's first command on move past them.
    std::vector<Command> opening;
    if (slots.contains(std::nullopt))
    {
        opening.push_back(SlotCommand(to.slot));
    }

    if (programs.contains(std::nullopt))
    {
        opening.push_back(ProgramCommand(to.number, channel.value_or(ProgramIndexOf(console, track))));
    }
    if (!opening.empty())
    {
        AddOpening(result, sequence, OpeningItem(file, commands, offsets, stretches), track, std::move(opening));
    }
    InsertDseSelects(result, sequence, dse_selects);

    return Writable(file, std::move(result), sequence);
}

std::optional<Program> OpeningInstrument(const format::File& file, uint32_t sequence, const song::Track& track)
{
    const song::Console console = ConsoleOf(file);
    std::optional<int64_t> slot;
    std::optional<int64_t> program;
    for (const CommandRow& row : CommandRows(file, sequence, track.stretches, track.notes, std::nullopt))
    {
        const Command& command = CommandAt(file, sequence, row.command);
        if (row.stretch != 0 || PassesTime(console, command))
        {
            break;
        }
        if (!SelectsInstrument(console, track.number, command))
        {
            continue;
        }
        if (command.conditional || command.value.source != Source::kValue)
        {
            return std::nullopt;
        }

        (command.kind == Kind::kProgram ? program : slot) = command.value.value / kOne;
    }

    if (ConsoleOf(file).engine == format::Engine::kDse)
    {
        const DseOpeningBank bank = DseBankOfOpening(file, sequence, track);
        if (bank.selected)
        {
            slot = bank.slot;
        }
    }
    if (!program && !slot)
    {
        return std::nullopt;
    }

    return Program{static_cast<int>(slot.value_or(0)), static_cast<int>(program.value_or(0))};
}

Edited EditOpeningInstrument(const format::File& file, uint32_t sequence, const song::Track& track, Program to)
{
    if (!track.notes.empty())
    {
        throw std::invalid_argument("This track has notes. Change the instrument of the notes instead.");
    }

    const song::Console console = ConsoleOf(file);
    const DriverEdits* edits = DriverEditsOf(console);
    if (edits != nullptr)
    {
        edits->CheckInstruments();
    }

    const bool dse = console.engine == format::Engine::kDse;
    const std::vector<uint16_t> dse_banks = dse ? DseBankIds(file, sequence) : std::vector<uint16_t>();
    const DseOpeningBank dse_bank = dse ? DseBankOfOpening(file, sequence, track) : DseOpeningBank();
    CheckInstrument(console, dse_bank.slot, to, dse_banks.size());
    if (edits != nullptr)
    {
        edits->CheckOpeningInstrument(console, track.number);
        edits->CheckTrackInstrument(console, track.number);
    }

    std::optional<uint8_t> channel;
    if (edits != nullptr && edits->ChannelsHoldPrograms())
    {
        const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
        channel = edits->ProgramChannel(commands, Offsets(commands, console), track.stretches);
        if (!channel)
        {
            throw std::invalid_argument("This track has no channel yet. Add a note to it first.");
        }
    }

    // The commands that the opening runs before time passes. The last of each kind selects the instrument.
    std::optional<uint32_t> program;
    std::optional<uint32_t> slot;
    for (const CommandRow& row : CommandRows(file, sequence, track.stretches, track.notes, std::nullopt))
    {
        const Command& command = CommandAt(file, sequence, row.command);
        if (row.stretch != 0 || PassesTime(console, command))
        {
            break;
        }
        if (!SelectsInstrument(console, track.number, command))
        {
            continue;
        }
        if (command.conditional || command.value.source != Source::kValue)
        {
            throw std::invalid_argument(kDecided);
        }

        (command.kind == Kind::kProgram ? program : slot) = row.command;
    }

    // The commands take the new values, and a new command sets what the opening doesn't.
    std::vector<CommandPlace> places;
    std::vector<Command> changed;
    const auto change = [&](const std::optional<uint32_t>& place, int value)
    {
        if (place)
        {
            Command command = CommandAt(file, sequence, *place);
            command.value.value = int64_t{value} * kOne;
            places.push_back({*place, {}});
            changed.push_back(command);
        }
    };
    change(program, to.number);
    change(slot, to.slot);
    Edited result = ChangeCommands(file, sequence, places, changed);

    // On DSE, a new bank select goes in after those of the opening (InsertAtOpening).
    std::vector<Command> opening = dse && (!dse_bank.selected || dse_bank.slot != to.slot)
                                       ? DseOpeningSelect(dse_banks, to.slot)
                                       : std::vector<Command>();
    if (!dse && !slot && to.slot != 0)
    {
        opening.push_back(SlotCommand(to.slot));
    }

    if (!program)
    {
        opening.push_back(ProgramCommand(to.number, channel.value_or(ProgramIndexOf(console, track.number))));
    }
    if (!opening.empty())
    {
        const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
        const std::vector<uint32_t> offsets = Offsets(commands, console);
        AddOpening(result, sequence, OpeningItem(file, commands, offsets, track.stretches), track.number,
                   std::move(opening));
    }

    return Writable(file, std::move(result), sequence);
}

Edited EditNotesWithInstrument(const format::File& file, uint32_t sequence, const song::Track& track,
                               std::span<const NewNote> notes, Program to)
{
    if (!track.notes.empty())
    {
        throw std::invalid_argument("This track has notes. Change the instrument of the notes instead.");
    }

    const song::Console console = ConsoleOf(file);
    if (const DriverEdits* edits = DriverEditsOf(console))
    {
        edits->CheckInstruments();
    }

    const bool dse = console.engine == format::Engine::kDse;
    const std::vector<uint16_t> dse_banks = dse ? DseBankIds(file, sequence) : std::vector<uint16_t>();
    const DseOpeningBank dse_bank = dse ? DseBankOfOpening(file, sequence, track) : DseOpeningBank();
    CheckInstrument(console, dse_bank.slot, to, dse_banks.size());
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(commands, console);
    const uint32_t first = OpeningItem(file, commands, offsets, track.stretches);

    // The notes go in at or after the track's first command. That command keeps its item. The new commands go before
    // it.
    Edited result = EditNotes(file, sequence, {}, notes);
    std::vector<Command> opening = dse && (!dse_bank.selected || dse_bank.slot != to.slot)
                                       ? DseOpeningSelect(dse_banks, to.slot)
                                       : std::vector<Command>();
    if (!dse && to.slot != 0)
    {
        opening.push_back(SlotCommand(to.slot));
    }

    // A DSE track's opening can select a program without a bank. That program change gets the new program, after the
    // new bank select.
    const std::optional<uint32_t> held = dse ? DseOpeningProgram(file, sequence, track) : std::nullopt;
    const std::optional<uint32_t> item = held ? ItemAtOffset(offsets, *held) : std::nullopt;
    const std::optional<uint32_t> moved = item ? result.items.at(*item) : std::nullopt;
    if (moved)
    {
        ntdlib::commands::Sequence changed = *result.file.sequences[sequence];
        changed.items[*moved].value.value = int64_t{to.number} * kOne;
        result.file.sequences[sequence] = std::move(changed);
    }
    else
    {
        opening.push_back(ProgramCommand(to.number));
    }

    AddOpening(result, sequence, first, track.number, std::move(opening));

    return Writable(file, std::move(result), sequence);
}

} // namespace hokuto::edit
