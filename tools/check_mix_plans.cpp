// SPDX-License-Identifier: MIT

// The plans of hokuto_check_mix: the edits of each setting of a track, and the runs of its commands.

#include "check_mix.h"

namespace hokuto::tools::mix
{

const char* NameOf(Mix setting)
{
    switch (setting)
    {
    case Mix::kVolume:
        return "volume";
    case Mix::kPan:
        return "pan";
    case Mix::kSendA:
        return "send A";
    case Mix::kSendB:
        return "send B";
    case Mix::kSendC:
        break;
    }

    return "send C";
}

namespace
{

// Whether `command` sets `setting`.
bool Sets(const Command& command, Mix setting)
{
    switch (setting)
    {
    case Mix::kVolume:
        return command.kind == Kind::kVolume;
    case Mix::kPan:
        return command.kind == Kind::kPan;
    case Mix::kSendA:
    case Mix::kSendB:
    case Mix::kSendC:
        break;
    }

    return command.kind == Kind::kSend && command.index == static_cast<int>(setting) - static_cast<int>(Mix::kSendA);
}

// A value of a command in whole units.
int Whole(int64_t value)
{
    return static_cast<int>(std::llround(static_cast<double>(value) / kOne));
}

} // namespace

int DefaultOf(Mix setting, bool gba, bool dse, const hokuto::song::DriverRules* driver)
{
    return dse && setting == Mix::kVolume      ? hokuto::song::kDseDefaultVolume
           : driver && setting == Mix::kVolume ? driver->StartVolume()
                                               : hokuto::song::MixDefault(setting, gba);
}

namespace
{

// DSE's opcodes of `setting`: a slide has a time of 2 bytes and its target, and a change has an amount.
bool IsSlide(const Command& command, Mix setting)
{
    const int opcode = setting == Mix::kVolume ? 0xE2 : setting == Mix::kPan ? 0xEA : 0;
    return opcode != 0 && command.kind == Kind::kOther && command.encoding.opcode == opcode &&
           command.bytes.size() == 3;
}

bool IsChange(const Command& command, Mix setting)
{
    const int opcode = setting == Mix::kVolume ? 0xE1 : setting == Mix::kPan ? 0xE9 : 0;
    return opcode != 0 && command.kind == Kind::kOther && command.encoding.opcode == opcode &&
           command.bytes.size() == 1;
}

} // namespace

std::optional<Run> RunOf(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& offsets,
                         const Track& track, Mix setting, bool gba, bool dse, const hokuto::song::DriverRules* driver)
{
    Run run;
    bool note = false;
    for (const Stretch& stretch : track.stretches)
    {
        const std::optional<uint32_t> first = hokuto::edit::ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = hokuto::edit::ItemAtOffset(offsets, stretch.last);
        if (!first || !last || *last < *first)
        {
            return std::nullopt;
        }

        for (uint32_t i = *first; i <= *last; ++i)
        {
            const Command& command = sequence.items[i];
            if (Sets(command, setting))
            {
                run.early = run.items.empty() && !run.moves ? note : run.early;
                run.items.insert(i);
                run.random = run.random || command.value.source == Source::kRandom;
                run.variable = run.variable || command.value.source == Source::kVariable;
                run.ramp = run.ramp || command.time.has_value();
                run.again = run.again || (command.value.source == Source::kValue &&
                                          Whole(command.value.value) == DefaultOf(setting, gba, dse, driver));
            }
            else if (dse && IsSlide(command, setting))
            {
                const bool timed = command.bytes[0] != 0 || command.bytes[1] != 0;
                run.early = run.items.empty() && !run.moves ? note || timed : run.early;
                run.moves = true;
                run.ramp = true;
                run.targets.insert(static_cast<int8_t>(command.bytes[2]) - (setting == Mix::kPan ? 0x40 : 0));
                run.slides.insert(i);
            }
            else if (dse && IsChange(command, setting))
            {
                run.early = run.items.empty() && !run.moves ? true : run.early;
                run.moves = true;
                run.changes = true;
                run.ramp = true;
                run.amounts.insert(i);
            }
            note = note || command.kind == Kind::kNote;
        }
    }
    run.early = run.early || (run.items.empty() && !run.moves);

    return run;
}

NotesApart QuintetNotesApart(const std::set<int>& changed)
{
    return [changed](const Track& track, const hokuto::song::Note&)
    {
        return changed.contains(track.number);
    };
}

namespace
{

// Whether `track` of Quintet's driver runs a command that adds to its volume: a volume step (F1, kVolume with index 1),
// a volume up (5F) or a volume down (6E).
bool StepsQuintetVolume(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& offsets,
                        const Track& track)
{
    for (const Stretch& stretch : track.stretches)
    {
        const std::optional<uint32_t> first = hokuto::edit::ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = hokuto::edit::ItemAtOffset(offsets, stretch.last);
        for (uint32_t i = first.value_or(0); first && last && i <= *last; ++i)
        {
            const Command& command = sequence.items[i];
            if ((command.kind == Kind::kVolume && command.index == 1) ||
                (command.kind == Kind::kOther && (command.encoding.opcode == 0x5F || command.encoding.opcode == 0x6E)))
            {
                return true;
            }
        }
    }

    return false;
}

} // namespace

bool RunsIntoData(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& offsets, const Track& track)
{
    for (const Stretch& stretch : track.stretches)
    {
        for (const uint32_t offset : {stretch.first, stretch.last})
        {
            const auto after = std::ranges::upper_bound(offsets, offset);
            const std::size_t item = static_cast<std::size_t>(after - offsets.begin());
            if (item > 0 && (item > sequence.items.size() || sequence.items[item - 1].kind == Kind::kData))
            {
                return true;
            }
        }
    }

    return false;
}

int Mapped(const Plan& plan, int value)
{
    const Mix setting = plan.edit.setting;
    int result = plan.edit.to;
    if (plan.trim && setting != Mix::kPan)
    {
        result = static_cast<int>(std::llround(static_cast<double>(value) * plan.edit.to / plan.edit.from));
    }
    else if (plan.trim)
    {
        result = value + plan.edit.to - plan.edit.from;
    }

    const int highest = plan.driver && setting == Mix::kVolume ? plan.driver->MostVolume(plan.track)
                                                               : hokuto::edit::MixHighest(setting, plan.gba);
    return std::clamp(result, hokuto::edit::MixLowest(setting), highest);
}

namespace
{

// Whether the tracks of `console` have `setting`.
bool Has(const hokuto::song::Console& console, Mix setting)
{
    if (setting == Mix::kVolume || setting == Mix::kPan)
    {
        // A driver may have no command for a volume or a pan.
        return console.driver == nullptr ||
               hokuto::song::Runs(console, setting == Mix::kVolume ? Kind::kVolume : Kind::kPan, 0);
    }

    const int bus = static_cast<int>(setting) - static_cast<int>(Mix::kSendA);
    return hokuto::song::Runs(console, Kind::kSend, static_cast<uint8_t>(bus));
}

// The item after the selects of a bank at a DSE track's opening, from item `opening` of `sequence`, or `opening`
// where none comes before the track's first note, wait or end.
uint32_t AfterBankSelects(const ntdlib::commands::Sequence& sequence, uint32_t opening)
{
    uint32_t after = opening;
    for (uint32_t item = opening; item < sequence.items.size(); ++item)
    {
        const Command& command = sequence.items[item];
        if (command.kind == Kind::kNote || command.kind == Kind::kWait || command.kind == Kind::kEnd)
        {
            break;
        }
        if (command.kind == Kind::kOther && command.encoding.opcode >= 0xA8 && command.encoding.opcode <= 0xAA)
        {
            after = item + 1;
        }
    }

    return after;
}

// Why the edit of `plan` refuses `setting` on `track` of `sequence`, whose run of commands is `run`, or nothing.
// `opens` says whether a new command is inserted at the track opening, and `after_allocation` whether it follows the
// allocation of tracks on the DS.
std::optional<std::string> RefusalOf(const hokuto::song::Console& console, const ntdlib::commands::Sequence& sequence,
                                     const std::vector<uint32_t>& offsets, const Track& track, Mix setting,
                                     const Run& run, const Plan& plan, bool opens, bool after_allocation)
{
    const bool gba = console.engine == hokuto::format::Engine::kGba;
    const auto at_allocation = [](const Command& item)
    {
        return ntdlib::commands::HasTarget(item.kind) && !item.target_outside && item.target == 0;
    };
    const bool brownie = console.driver && console.driver->DriverEngine() == hokuto::format::Engine::kBrownie;
    if (hokuto::edit::IsKrawall(console))
    {
        return "a mix on Krawall: notes and rows set it";
    }
    if (brownie && (setting == Mix::kPan || (setting == Mix::kVolume && console.driver->PsgTrack(track.number))))
    {
        return "a pan or a PSG channel's volume of Brownie Brown's driver";
    }
    if (!Has(console, setting))
    {
        return "a setting that the console's tracks don't have";
    }
    if (hokuto::edit::IsQuintet(console) && setting == Mix::kVolume && StepsQuintetVolume(sequence, offsets, track))
    {
        return "a volume step of Quintet's driver";
    }
    if (run.variable)
    {
        return "a variable";
    }
    if (plan.trim && setting != Mix::kPan && plan.edit.from <= 0)
    {
        return "a trim from 0";
    }
    if (opens && !gba && console.engine != hokuto::format::Engine::kDse && !console.driver &&
        hokuto::edit::Fixed(sequence))
    {
        return "a fixed sequence";
    }
    if (after_allocation && std::ranges::any_of(sequence.items, at_allocation))
    {
        return "a target at the DS's allocation of tracks";
    }

    return std::nullopt;
}

} // namespace

std::optional<Plan> PlanFor(const File& file, uint32_t index, const std::vector<uint32_t>& offsets, uint32_t sound,
                            const Track& track, Mix setting)
{
    const ntdlib::commands::Sequence& sequence = *file.sequences[index];
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    const bool gba = console.engine == hokuto::format::Engine::kGba;
    const std::optional<Run> run =
        RunOf(sequence, offsets, track, setting, gba, console.engine == hokuto::format::Engine::kDse, console.driver);
    if (!run || track.stretches.empty())
    {
        return std::nullopt;
    }

    // The edit starts from the value at the track's first note.
    Plan plan;
    plan.sound = sound;
    plan.track = track.number;
    plan.stretches = track.stretches;
    plan.channel = track.channel;
    plan.run = *run;
    plan.edit.setting = setting;
    plan.gba = gba;
    plan.dse = console.engine == hokuto::format::Engine::kDse;
    plan.driver = console.driver;
    plan.edit.from = hokuto::song::MixAt(track, setting, track.notes.front().start);
    if (setting == Mix::kPan)
    {
        plan.edit.to = plan.edit.from <= 47 ? plan.edit.from + 16 : plan.edit.from - 16;
    }
    else
    {
        plan.edit.to = plan.edit.from > 0 ? plan.edit.from / 2 : 64;
    }

    // One value, the default included where it plays, takes the new value. Several values scale or shift.
    std::set<int> values;
    for (const uint32_t item : run->items)
    {
        values.insert(Whole(sequence.items[item].value.value));
    }
    if (run->early)
    {
        values.insert(DefaultOf(setting, gba, console.engine == hokuto::format::Engine::kDse, console.driver));
    }
    values.insert(run->targets.begin(), run->targets.end());
    plan.trim = run->random || values.size() > 1 || run->changes;

    // On the DS, a new command at track 0's opening after the allocation of tracks at the sequence's first command is
    // inserted without a jump. The edit refuses it where a command's target is the allocation (edit::InsertAtOpening).
    const int fallback = DefaultOf(setting, gba, console.engine == hokuto::format::Engine::kDse, console.driver);
    const bool opens = run->early && Mapped(plan, fallback) != fallback;
    std::optional<uint32_t> opening =
        opens ? std::optional(hokuto::edit::ItemAtOffset(offsets, track.stretches.front().first)) : std::nullopt;

    // On DSE, a new command is inserted after the selects of a bank at the track opening. No note, wait or end comes
    // before them.
    if (opening && console.engine == hokuto::format::Engine::kDse)
    {
        opening = AfterBankSelects(sequence, *opening);
    }
    const bool after_allocation = console.engine == hokuto::format::Engine::kDs && track.number == 0 && opening == 1u &&
                                  sequence.items.front().kind == Kind::kAllocateTracks;

    // The refusals, in the order that the edit checks them. A GBA song's commands can always move. Krawall's notes and
    // rows set the mix. Brownie Brown's driver refuses every pan, and a PSG channel's volume: their values aren't
    // levels.
    plan.refusal = RefusalOf(console, sequence, offsets, track, setting, *run, plan, opens, after_allocation);
    if (!plan.refusal && opens)
    {
        plan.opening = opening;
    }

    // A jump goes before the new command where the player can run on from the item before it: anything but a jump, a
    // return or an end that always runs, and a return that a sound reaches outside every call.
    if (plan.opening)
    {
        plan.jump = !after_allocation && console.engine != hokuto::format::Engine::kDse &&
                    hokuto::edit::RunsInto(file, index, *plan.opening);
    }

    return plan;
}

bool HoldsEdit(const ntdlib::commands::Sequence& before, const ntdlib::commands::Sequence& edited, const Plan& plan)
{
    const uint32_t added_items = plan.opening ? (plan.jump ? 2 : 1) : 0;
    if (edited.items.size() != before.items.size() + added_items)
    {
        return false;
    }

    // The new command, after the jump past it.
    const auto moved = [&plan, added_items](uint32_t item)
    {
        return plan.opening && item >= *plan.opening ? item + added_items : item;
    };

    if (plan.opening && plan.jump)
    {
        const Command& jump = edited.items[*plan.opening];
        if (jump.kind != Kind::kJump || jump.conditional || jump.target != *plan.opening + 2)
        {
            return false;
        }
    }
    if (plan.opening)
    {
        const Command& added = edited.items[*plan.opening + (plan.jump ? 1 : 0)];
        const int value = Mapped(plan, DefaultOf(plan.edit.setting, plan.gba, plan.dse, plan.driver));
        if (!Sets(added, plan.edit.setting) || added.value.source != Source::kValue ||
            added.value.value != int64_t{value} * kOne || added.time || added.conditional)
        {
            return false;
        }
    }

    // Each old item, with its new value where the run holds it.
    for (uint32_t i = 0; i < before.items.size(); ++i)
    {
        Command expected = before.items[i];
        if (plan.run.slides.contains(i))
        {
            const int centre = plan.edit.setting == Mix::kPan ? 0x40 : 0;
            expected.bytes[2] =
                static_cast<uint8_t>(Mapped(plan, static_cast<int8_t>(expected.bytes[2]) - centre) + centre);
        }
        else if (plan.run.amounts.contains(i) && plan.edit.setting == Mix::kVolume)
        {
            const double scaled =
                static_cast<double>(static_cast<int8_t>(expected.bytes[0])) * plan.edit.to / plan.edit.from;
            expected.bytes[0] = static_cast<uint8_t>(std::clamp<long long>(std::llround(scaled), -128, 127));
        }
        else if (plan.run.items.contains(i))
        {
            expected.value.value = int64_t{Mapped(plan, Whole(expected.value.value))} * kOne;
            if (expected.value.source == Source::kRandom)
            {
                expected.value.high = int64_t{Mapped(plan, Whole(expected.value.high))} * kOne;
            }
        }
        Command actual = edited.items[moved(i)];
        actual.target = expected.target;
        if (actual != expected)
        {
            return false;
        }
    }

    return true;
}

} // namespace hokuto::tools::mix
