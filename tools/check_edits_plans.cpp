// SPDX-License-Identifier: MIT

// The plans of hokuto_check_edits: the edits that each note command takes, and how they apply.

#include "check_edits.h"

namespace hokuto::tools::edits
{

namespace
{

// The ticks of a move and of a longer note. On Ubisoft Milan's driver, no command plays one tick after another
// (format/ubimilan.h), and a step of one tick would be refused nearly everywhere.
uint32_t StepOf(const File& file)
{
    return hokuto::edit::IsUbiMilan(hokuto::edit::ConsoleOf(file)) ? 2 : 1;
}

// Whether a note of `console` lasts until a later command, and its command has no length: on Konami's driver, Ubisoft
// Milan's and Rare's.
bool EndsAtLaterCommand(const hokuto::song::Console& console)
{
    return hokuto::edit::KonamiOf(console).has_value() || hokuto::edit::IsUbiMilan(console) ||
           hokuto::edit::IsRare(console);
}

} // namespace

std::map<uint32_t, uint32_t> ItemsByOffset(const ntdlib::commands::Sequence& sequence,
                                           const hokuto::song::Console& console)
{
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(sequence, console);
    std::map<uint32_t, uint32_t> items;
    for (uint32_t i = 0; i + 1 < offsets.size(); ++i)
    {
        items[offsets[i]] = i;
    }

    return items;
}

bool Brownie(const hokuto::song::Console& console)
{
    return console.driver && console.driver->DriverEngine() == hokuto::format::Engine::kBrownie;
}

Plan VelocityPlan(const File& file, const Command& command, const Plan& plan)
{
    Plan changed;
    changed.edit.command = plan.edit.command;
    changed.edit.stretch = plan.edit.stretch;
    // On Konami's driver, a volume command sets the volume of the note that it starts (song::ReadSequence).
    const int velocity =
        static_cast<int>((command.kind == Kind::kVolume ? command.value.value : command.velocity) / kOne);
    const int next = velocity <= 1 ? velocity + 1 : velocity - 1;
    const hokuto::song::DriverRules* driver = hokuto::edit::ConsoleOf(file).driver;
    changed.edit.velocity = driver ? driver->VelocityOf(next, command.sample.has_value()) : next;

    // On Ubisoft Milan's driver, a note of the PSG stores its level in some of the bits of its byte, by its channel
    // (song::UbiMilanDriverVelocity). The edit takes the nearest velocity that changes the level, and the read shows
    // the level's velocity.
    const int channel = command.index;
    if (hokuto::edit::IsUbiMilan(hokuto::edit::ConsoleOf(file)) && !ntdlib::ubimilan::PlaysKit(channel, command.key))
    {
        const int heard = hokuto::song::UbiMilanVelocityOf(channel, command.key, velocity);
        const int step = heard <= 1 ? 1 : -1;
        for (int candidate = heard + step; candidate >= 1 && candidate <= 127; candidate += step)
        {
            const int byte = hokuto::song::UbiMilanDriverVelocity(channel, command.key, candidate, velocity);
            if (byte != velocity)
            {
                changed.edit.velocity = candidate;
                changed.heard = static_cast<uint32_t>(hokuto::song::UbiMilanVelocityOf(channel, command.key, byte));
                break;
            }
        }
    }

    return changed;
}

Plan PlanFor(const File& file, uint32_t sequence, const Note& note, const hokuto::song::Stretch& stretch,
             const Command& command, std::size_t turn)
{
    Plan plan;
    plan.edit.command = note.command;
    plan.edit.stretch = stretch;
    // A note of Krawall sits on its row. It can't move or change its length.
    const std::optional<hokuto::edit::ShiftRange> range =
        hokuto::edit::IsKrawall(hokuto::edit::ConsoleOf(file))
            ? std::nullopt
            : hokuto::edit::ShiftRangeOf(file, sequence, note.command, stretch);
    const uint32_t step = StepOf(file);
    switch (turn % 4)
    {
    case 0:
        if (range && (range->later > step || range->earlier >= step))
        {
            plan.change = Change::kMove;
            plan.edit.shift = range->later > step ? static_cast<int32_t>(step) : -static_cast<int32_t>(step);
            return plan;
        }
        break;

    case 2:
        plan.change = Change::kRemove;
        plan.edit.remove = true;
        return plan;

    case 3:
        if (range && command.value.source == ntdlib::commands::Source::kValue && command.value.value % kOne == 0)
        {
            // A note of Brownie Brown's driver lasts until a later command too. Its command holds the frames up to the
            // channel's next command, and the edit changes them (HoldsEdit).
            const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
            plan.change = Change::kLonger;
            plan.longer = step;
            plan.edit.length =
                (EndsAtLaterCommand(console) || Brownie(console) ? note.length
                                                                 : static_cast<uint32_t>(command.value.value / kOne)) +
                plan.longer;
            return plan;
        }
        break;

    default:
        break;
    }

    // Otherwise the velocity changes.
    return VelocityPlan(file, command, plan);
}

std::vector<NoteEdit> EditsOf(const std::map<uint32_t, Plan>& plans)
{
    std::vector<NoteEdit> edits;
    for (const auto& [command, plan] : plans)
    {
        edits.push_back(plan.edit);
    }

    return edits;
}

hokuto::edit::Edited EditAsProgram(const File& file, uint32_t sequence, std::span<const NoteEdit> edits)
{
    hokuto::edit::Edited edited = hokuto::edit::EditNotes(file, sequence, edits);
    if (!hokuto::edit::IsTimeline(file, sequence))
    {
        return edited;
    }

    hokuto::edit::Settled settled = hokuto::edit::SettleTimelines(file, std::move(edited.file));
    const auto laid = settled.items.find(sequence);
    if (laid != settled.items.end())
    {
        for (std::optional<uint32_t>& item : edited.items)
        {
            item = item ? laid->second.at(*item) : std::nullopt;
        }
    }
    edited.file = std::move(settled.file);

    return edited;
}

std::optional<std::string> Refusal(const File& file, uint32_t sequence, const NoteEdit& edit)
{
    try
    {
        EditAsProgram(file, sequence, std::span(&edit, 1));
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return std::nullopt;
}

const char* NameOf(Change change)
{
    switch (change)
    {
    case Change::kMove:
        return "move";
    case Change::kVelocity:
        return "velocity";
    case Change::kRemove:
        return "removal";
    case Change::kLonger:
        return "longer length";
    }

    return "";
}

std::string LengthOf(const Command& command)
{
    switch (command.value.source)
    {
    case ntdlib::commands::Source::kValue:
        return std::to_string(static_cast<double>(command.value.value) / kOne) + " ticks";
    case ntdlib::commands::Source::kRandom:
        return "a random length";
    case ntdlib::commands::Source::kVariable:
        return "the length in variable " + std::to_string(command.value.value);
    }

    return "";
}

bool HoldsEdit(const Command& old, const Plan& plan, const Command* edited, const hokuto::song::Console& console)
{
    const bool quintet = hokuto::edit::IsQuintet(console);
    const bool krawall = hokuto::edit::IsKrawall(console);
    if (plan.change == Change::kRemove)
    {
        return edited == nullptr || edited->kind == Kind::kWait ||
               (quintet && edited->kind == Kind::kNoteOff && edited->value == old.value) ||
               (krawall && edited->kind == Kind::kNoteOff && edited->index == old.index);
    }

    const bool later = EndsAtLaterCommand(console);
    // A note of Brownie Brown's driver holds the frames up to the channel's next command, and a move or a new length
    // changes them.
    const bool brownie = Brownie(console);
    Command expected = old;
    if (plan.edit.velocity && hokuto::edit::IsUbiMilan(console))
    {
        expected.velocity = int64_t{hokuto::song::UbiMilanDriverVelocity(old.index, old.key, *plan.edit.velocity,
                                                                         static_cast<int>(old.velocity / kOne))} *
                            kOne;
    }
    else if (plan.edit.velocity && !quintet && !krawall)
    {
        // On Konami's driver, the volume command that starts a note sets the note's volume.
        const int64_t level = (console.driver ? console.driver->VolumeOf(*plan.edit.velocity, old.sample.has_value())
                                              : *plan.edit.velocity) *
                              kOne;
        (old.kind == Kind::kVolume ? expected.value.value : expected.velocity) = level;
    }
    if (plan.edit.length && !later && !brownie)
    {
        expected.value.value = static_cast<int64_t>(*plan.edit.length) * kOne;
        expected.encoding.value_bytes = 0;
    }
    if (edited != nullptr && console.driver && !later)
    {
        expected.encoding = edited->encoding;
        expected.bytes = edited->bytes;
    }
    if (edited != nullptr && brownie)
    {
        expected.value = edited->value;
    }

    return edited != nullptr && *edited == expected;
}

void VelocitiesForRefused(const std::string& name, const File& file, const File& edited, uint32_t sequence,
                          std::map<uint32_t, std::map<uint32_t, uint32_t>>& items, std::map<uint32_t, Plan>& by_command,
                          Totals& totals)
{
    for (auto& [offset, plan] : by_command)
    {
        if (const std::optional<std::string> refusal = Refusal(edited, sequence, plan.edit))
        {
            const Command& command = file.sequences[sequence]->items[items[sequence].at(offset)];
            std::printf(
                "%s: sequence %u refuses the %s of the note at %u, of %s, with the note wait %s at the "
                "start of its stretch: %s\n",
                name.c_str(), sequence, NameOf(plan.change), offset, LengthOf(command).c_str(),
                plan.edit.stretch.note_wait ? "on" : "off", refusal->c_str());
            ++totals.refused;
            plan = VelocityPlan(file, command, plan);
        }
    }
}

void TakeEditsOneAtATime(const File& file, const File& edited, uint32_t sequence,
                         std::map<uint32_t, std::map<uint32_t, uint32_t>>& items, std::map<uint32_t, Plan>& by_command,
                         Totals& totals)
{
    std::map<uint32_t, Plan> taken;
    for (auto& [offset, plan] : by_command)
    {
        taken[offset] = plan;
        try
        {
            EditAsProgram(edited, sequence, EditsOf(taken));
        }
        catch (const std::invalid_argument&)
        {
            const Command& command = file.sequences[sequence]->items[items[sequence].at(offset)];
            plan = VelocityPlan(file, command, plan);
            taken[offset] = plan;
            ++totals.refused;
        }
    }
}

void FindSplitRuns(const Sequence& song, uint32_t sequence, std::map<uint32_t, std::set<uint32_t>>& split,
                   Totals& totals)
{
    for (const hokuto::song::Track& track : song.tracks)
    {
        std::map<std::pair<uint32_t, uint32_t>, const Note*> last;
        for (const Note& note : track.notes)
        {
            const Note*& previous = last[{note.command, note.stretch}];
            if (previous != nullptr && note.start <= previous->start + previous->length + 1 &&
                split[sequence].insert(note.command).second)
            {
                ++totals.split;
            }
            previous = &note;
        }
    }
}

void PlanNotes(const std::string& name, const File& file, uint32_t sequence, const Sequence& song,
               const std::string& sound_name, const std::map<uint32_t, uint32_t>& by_offset,
               std::map<uint32_t, std::map<uint32_t, Plan>>& plans, std::map<uint32_t, std::set<uint32_t>>& replays,
               std::size_t& turn, Totals& totals)
{
    for (const hokuto::song::Track& track : song.tracks)
    {
        const std::optional<uint32_t> track_end = hokuto::edit::TrackEndOf(file, sequence, track.stretches);
        for (const Note& note : track.notes)
        {
            ++totals.notes;
            if (plans[sequence].contains(note.command) || !by_offset.contains(note.command))
            {
                continue;
            }

            // The program works out a note's limits as the Key Editor does. A note whose limits it can't work
            // out fails, and gets no edit.
            const Command& command = file.sequences[sequence]->items[by_offset.at(note.command)];
            if (command.kind != Kind::kNote && hokuto::edit::IsKrawall(hokuto::edit::ConsoleOf(file)))
            {
                ++totals.row_effects;
                replays[sequence].insert(note.command);
                continue;
            }

            try
            {
                Plan& plan = plans[sequence][note.command] =
                    PlanFor(file, sequence, note, hokuto::song::StretchesOfNote(song, track, note).at(note.stretch),
                            command, turn++);
                plan.edit.track_end = track_end;

                // On Konami's driver, no edit takes a note that a volume command starts where the note command
                // before it is in another block (edit::note_detail::kKonamiFarKey). Such a note gets no plan.
                const std::optional<std::string> refusal =
                    command.kind == Kind::kVolume ? Refusal(file, sequence, VelocityPlan(file, command, plan).edit)
                                                  : std::nullopt;
                if (refusal)
                {
                    std::printf("%s: %s: the note of track %d at tick %u takes no edit: %s\n", name.c_str(),
                                sound_name.c_str(), track.number, note.start, refusal->c_str());
                    plans[sequence].erase(note.command);
                }
            }
            catch (const std::invalid_argument& error)
            {
                std::printf("%s: %s: the note of track %d at tick %u can't be edited: %s\n", name.c_str(),
                            sound_name.c_str(), track.number, note.start, error.what());
                ++totals.unplanned;
            }
        }
    }
}

void VelocitiesForSplitRuns(const File& file, const std::map<uint32_t, std::set<uint32_t>>& split,
                            std::map<uint32_t, std::map<uint32_t, uint32_t>>& items,
                            std::map<uint32_t, std::map<uint32_t, Plan>>& plans)
{
    for (const auto& [sequence, commands] : split)
    {
        for (const uint32_t command : commands)
        {
            const auto plan = plans[sequence].find(command);
            if (plan != plans[sequence].end() &&
                (plan->second.change == Change::kMove || plan->second.change == Change::kLonger))
            {
                plan->second =
                    VelocityPlan(file, file.sequences[sequence]->items[items[sequence].at(command)], plan->second);
            }
        }
    }
}

void CountPlans(const std::map<uint32_t, std::map<uint32_t, Plan>>& plans, Totals& totals)
{
    for (const auto& [sequence, by_command] : plans)
    {
        for (const auto& [command, plan] : by_command)
        {
            ++totals.commands;
            totals.moved += plan.change == Change::kMove ? 1 : 0;
            totals.velocities += plan.change == Change::kVelocity ? 1 : 0;
            totals.removed += plan.change == Change::kRemove ? 1 : 0;
            totals.longer += plan.change == Change::kLonger ? 1 : 0;
        }
    }
}

} // namespace hokuto::tools::edits
