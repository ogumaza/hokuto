// SPDX-License-Identifier: MIT

#include "edit/instruments.h"

#include <algorithm>
#include <cstdint>
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

// The commands that set the bank slot and the program of each note of a track, by the commands that set them.
struct Walk
{
    Uses slots;
    Uses programs;
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
// `console`, or where it changes the slot from `from_slot` on a console whose tracks can't select a bank.
void CheckInstrument(const song::Console& console, int from_slot, Program to)
{
    if (to.slot < 0 || to.slot >= kBankSlots || to.number < 0)
    {
        throw std::invalid_argument("Bank slots are 0–3, and program numbers start at 0.");
    }

    if (to.slot != from_slot && !song::Runs(console, Kind::kBankSelect, 0))
    {
        const DriverEdits* edits = DriverEditsOf(console);
        const std::optional<std::string> refused = edits != nullptr ? edits->RefusedBankSelect() : std::nullopt;
        throw std::invalid_argument(
            console.engine == format::Engine::kGba
                ? "A GBA song uses a single voice group. Its tracks can't select another."
            : console.engine == format::Engine::kDs
                ? "A DS sequence uses the single bank set in its archive entry. Its tracks can't select "
                  "another."
            : console.engine == format::Engine::kDse
                ? "Can't change the bank that a DSE track selects."
                : refused.value_or("A Wii sound uses only one bank. Its sequences can't select another bank "
                                   "slot."));
    }

    if (console.engine == format::Engine::kGba && to.number >= kGbaPrograms)
    {
        throw std::invalid_argument("Programs in a GBA voice group must be from 0 to 127.");
    }
}

// The command at a DSE track's opening that selects the bank of slot `slot`. A channel starts without one of the
// sound's banks, and a program selected then stays at 0. A sequence that no sound plays has no banks to select.
std::vector<Command> DseOpeningSelect(const format::File& file, uint32_t sequence, int slot)
{
    const std::vector<uint16_t> banks = DseBankIds(file, sequence);
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
        throw std::invalid_argument(
            "This track starts no notes with that instrument. Another track can play notes on this channel "
            "through its pan setting. Change the instrument on that track.");
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

    CheckInstrument(console, from.slot, to);
    if (edits != nullptr)
    {
        edits->CheckTrackInstrument(console, track);
    }

    // The track's commands in the order that they ran. Each note plays the instrument that its track has selected.
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(commands, console);
    Walk walk =
        WalkTrack(commands, offsets, stretches, console, track,
                  console.engine == format::Engine::kDse ? DseBankIds(file, sequence) : std::vector<uint16_t>());
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
    const std::set<Setter> slots = to.slot != from.slot ? SettersOf(slot_uses, from) : std::set<Setter>();
    // On some drivers, such as Rare's driver, the program belongs to the channel, and every track on the channel plays
    // it.
    const std::optional<uint8_t> channel = edits != nullptr && edits->ChannelsHoldPrograms() && !programs.empty()
                                               ? edits->ProgramChannel(commands, offsets, stretches)
                                               : std::nullopt;
    CheckAlone(program_uses, programs,
               "Notes from another bank slot use the same program change as the notes of this instrument.");
    CheckAlone(slot_uses, slots, "Notes of another program use the same bank select as the notes of this instrument.");
    RefuseBankSelects(commands, programs);

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

    const DseOpeningBank dse_bank =
        console.engine == format::Engine::kDse ? DseBankOfOpening(file, sequence, track) : DseOpeningBank();
    CheckInstrument(console, dse_bank.slot, to);
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

    std::vector<Command> opening = console.engine == format::Engine::kDse && !dse_bank.selected
                                       ? DseOpeningSelect(file, sequence, to.slot)
                                       : std::vector<Command>();
    if (console.engine != format::Engine::kDse && !slot && to.slot != 0)
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

    const DseOpeningBank dse_bank =
        console.engine == format::Engine::kDse ? DseBankOfOpening(file, sequence, track) : DseOpeningBank();
    CheckInstrument(console, dse_bank.slot, to);
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(commands, console);
    const uint32_t first = OpeningItem(file, commands, offsets, track.stretches);

    // The notes go in at or after the track's first command. That command keeps its item. The new commands go before
    // it.
    Edited result = EditNotes(file, sequence, {}, notes);
    std::vector<Command> opening = console.engine == format::Engine::kDse && !dse_bank.selected
                                       ? DseOpeningSelect(file, sequence, to.slot)
                                       : std::vector<Command>();
    if (console.engine != format::Engine::kDse && to.slot != 0)
    {
        opening.push_back(SlotCommand(to.slot));
    }

    // A DSE track's opening can select a program without a bank. That program change gets the new program, after the
    // new bank select.
    const std::optional<uint32_t> held =
        console.engine == format::Engine::kDse ? DseOpeningProgram(file, sequence, track) : std::nullopt;
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
