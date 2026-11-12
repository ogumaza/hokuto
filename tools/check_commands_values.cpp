// SPDX-License-Identifier: MIT

// The rounds of hokuto_check_commands that edit the values of commands, insert them and remove them.

#include "check_commands.h"

namespace hokuto::tools::commands
{

// The track whose range a step stays in. The check steps a value to another one that the command's bytes hold. On
// Konami's driver, a sampled track's range covers every value that the bytes of a volume hold. The other drivers and
// the engines give every track the same range.
constexpr int kStepTrack = 4;

std::optional<Command> Stepped(const hokuto::song::Console& console, const Command& command)
{
    if (command.kind == Kind::kNote || command.kind == Kind::kData || command.kind == Kind::kOther)
    {
        return std::nullopt;
    }
    const bool countdown = UbiMilanConsole(console) && command.kind == Kind::kWait;

    const auto step = [&](const ntdlib::commands::Argument& argument, const ntdlib::nwsnd::Scale& scale,
                          bool time) -> std::optional<int64_t>
    {
        const std::optional<hokuto::edit::ValueRange> range =
            hokuto::edit::ArgumentRange(console, kStepTrack, command, Source::kValue, time);
        if (argument.source != Source::kValue || !range)
        {
            return std::nullopt;
        }

        const int64_t value = ntdlib::nwsnd::FromUnit(argument.value, scale);
        for (int64_t next : {value + 1, value - 1})
        {
            if (countdown && next == 1)
            {
                next = value == 0 ? 2 : value;
            }
            const ntdlib::commands::Fixed unit = ntdlib::nwsnd::ToUnit(next, scale);
            if (unit != argument.value && unit >= range->lowest && unit <= range->highest)
            {
                return unit;
            }
        }

        return std::nullopt;
    };

    Command stepped = command;
    if (ntdlib::commands::UnitOf(command.kind))
    {
        const ntdlib::nwsnd::Scale scale =
            console.engine == hokuto::format::Engine::kGba ? ntdlib::mp2k::ScaleOf(command.kind)
            : console.driver ? console.driver->ScaleOf(command.kind)
                             : ntdlib::nwsnd::ScaleOf(command.kind, hokuto::song::GrammarOf(console));
        if (const std::optional<int64_t> value = step(command.value, scale, false))
        {
            stepped.value.value = *value;
            return stepped;
        }
    }
    if (command.time)
    {
        if (const std::optional<int64_t> time = step(*command.time, ntdlib::nwsnd::kTicksScale, true))
        {
            stepped.time->value = *time;
            return stepped;
        }
    }

    return std::nullopt;
}

Command WithoutEncoding(Command command)
{
    command.encoding = {};

    return command;
}

void KeepValues(const std::string& name, const File& file, uint32_t sequence, const std::vector<uint32_t>& chosen,
                Totals& totals)
{
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    for (const uint32_t item : chosen)
    {
        // The editors change no command that the common form has no kind for. DSE has many of them.
        if (commands.items[item].kind == Kind::kOther)
        {
            continue;
        }

        try
        {
            const Edited edited =
                hokuto::edit::ChangeCommand(file, sequence, {offsets[item], {}}, commands.items[item]);
            if (!(edited.file == file))
            {
                std::printf("%s: sequence %u: the command at %u changes when it takes its values again\n", name.c_str(),
                            sequence, offsets[item]);
                ++totals.problems;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u: the command at %u refuses its values: %s\n", name.c_str(), sequence,
                        offsets[item], error.what());
            ++totals.problems;
        }
        ++totals.kept;
    }
}

bool AllocationFirst(const File& file, const ntdlib::commands::Sequence& commands, uint32_t item)
{
    return hokuto::edit::ConsoleOf(file).engine == hokuto::format::Engine::kDs && item == 0 &&
           !commands.items.empty() && commands.items[0].kind == Kind::kAllocateTracks;
}

namespace
{

// Whether `edited` contains `file` again, but for notes and waits of sequence `sequence` that store their values in
// their bytes where `file` uses the values of the last note or wait of the track. Only Nintendo R&D2's driver has such
// notes and waits.
bool Respelled(const File& file, const File& edited, uint32_t sequence)
{
    if (!hokuto::edit::Rd2Of(hokuto::edit::ConsoleOf(file)) || sequence >= edited.sequences.size() ||
        sequence >= file.sequences.size())
    {
        return false;
    }

    const std::vector<Command>& was = file.sequences[sequence]->items;
    ntdlib::commands::Sequence now = *edited.sequences[sequence];
    if (now.items.size() != was.size())
    {
        return false;
    }

    bool respelled = false;
    for (std::size_t item = 0; item < was.size(); item++)
    {
        Command& command = now.items[item];
        if ((command.kind == Kind::kNote || command.kind == Kind::kWait) && was[item].encoding.repeated &&
            !command.encoding.repeated)
        {
            command.encoding = was[item].encoding;
            command.bytes = was[item].bytes;
            respelled = true;
        }
    }
    File back = edited;
    back.sequences[sequence] = std::move(now);

    return respelled && back == file;
}

// Whether `edited` contains `file` again, but for a length byte of Brownie Brown's driver in sequence `sequence` that
// no channel of `file` reads. A hold that is inserted before a command that doesn't wait, such as a setting or a loop's
// start, can use the length that such a byte chose. Once the hold is removed again, the byte chooses a length that no
// command reads, and the removal drops it (edit::RemoveCommands). A hold that took a length byte of its own leaves the
// same bytes behind.
bool DroppedUnreadLength(const File& file, const File& edited, uint32_t sequence)
{
    if (!hokuto::edit::BrownieOf(hokuto::edit::ConsoleOf(file)) || sequence >= edited.sequences.size() ||
        sequence >= file.sequences.size())
    {
        return false;
    }

    const std::vector<Command>& was = file.sequences[sequence]->items;
    const std::vector<Command>& now = edited.sequences[sequence]->items;
    if (was.size() != now.size() + 1)
    {
        return false;
    }
    // The first command that differs, without the targets: those after the byte move back by one.
    const auto same = [](Command a, Command b)
    {
        a.target = 0;
        b.target = 0;
        return a == b;
    };
    const auto at = static_cast<uint32_t>(std::ranges::mismatch(was, now, same).in1 - was.begin());
    if (at >= was.size() || !hokuto::format::BrownieLengthByte(was[at]))
    {
        return false;
    }

    // The file that the removal leaves without the byte.
    const auto moved = [at](uint32_t item)
    {
        return item <= at ? item : item - 1;
    };
    ntdlib::commands::Sequence without = *file.sequences[sequence];
    without.items.erase(without.items.begin() + at);
    for (Command& command : without.items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target = moved(command.target);
        }
    }
    for (ntdlib::commands::Label& label : without.labels)
    {
        label.item = moved(label.item);
    }
    File back = file;
    back.sequences[sequence] = std::move(without);
    hokuto::edit::MoveItemPlaces(back, sequence, moved);

    return back == edited;
}

} // namespace

void InsertAndRemove(const std::string& name, const File& file, uint32_t sequence, const std::vector<uint32_t>& chosen,
                     Totals& totals)
{
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    const bool gba = console.engine == hokuto::format::Engine::kGba;
    const bool waits = hokuto::song::Runs(console, Kind::kWait, 0);
    for (const uint32_t item : chosen)
    {
        for (const bool entered : {false, true})
        {
            if (!waits)
            {
                try
                {
                    hokuto::edit::InsertCommand(file, sequence, PlaceOf(file, sequence, item), entered, NoWait());
                    std::printf("%s: sequence %u: a wait went in before item %u, on a driver without waits\n",
                                name.c_str(), sequence, item);
                    ++totals.problems;
                }
                catch (const std::invalid_argument&)
                {
                    ++totals.without_waits;
                }
                continue;
            }
            if (gba && !entered && item > 0 && commands.items[item - 1].kind == Kind::kData)
            {
                ++totals.after_data;
                continue;
            }
            if (!entered && item > 0 && TableEntry(file, commands.items[item - 1]))
            {
                ++totals.after_table;
                continue;
            }
            if (AllocationFirst(file, commands, item))
            {
                try
                {
                    hokuto::edit::InsertCommand(file, sequence, PlaceOf(file, sequence, item), entered, NoWait());
                    std::printf("%s: sequence %u: a wait went in before the allocation of tracks\n", name.c_str(),
                                sequence);
                    ++totals.problems;
                }
                catch (const std::invalid_argument&)
                {
                    ++totals.before_allocation;
                }
                continue;
            }

            try
            {
                const Edited inserted =
                    hokuto::edit::InsertCommand(file, sequence, PlaceOf(file, sequence, item), entered, NoWait());
                if (hokuto::edit::KonamiOf(hokuto::edit::ConsoleOf(file)) ||
                    UbiMilanConsole(hokuto::edit::ConsoleOf(file)))
                {
                    if (!(inserted.file == file))
                    {
                        std::printf("%s: sequence %u: a wait before item %u%s changes the file\n", name.c_str(),
                                    sequence, item, entered ? " that code enters" : "");
                        ++totals.problems;
                    }
                    ++totals.round_trips;
                    continue;
                }
                const CommandPlace wait = PlaceOf(inserted.file, sequence, inserted.added.at(0));
                const Edited removed = hokuto::edit::RemoveCommands(inserted.file, sequence, {&wait, 1});
                if (!(removed.file == file) && DroppedUnreadLength(file, removed.file, sequence))
                {
                    ++totals.dropped_lengths;
                }
                else if (!(removed.file == file) && Respelled(file, removed.file, sequence))
                {
                    ++totals.respelled;
                }
                else if (!(removed.file == file))
                {
                    std::printf("%s: sequence %u: a wait before item %u%s doesn't go again cleanly\n", name.c_str(),
                                sequence, item, entered ? " that code enters" : "");
                    ++totals.problems;
                }
            }
            catch (const std::exception& error)
            {
                std::printf("%s: sequence %u: a wait can't go in before item %u and go again: %s\n", name.c_str(),
                            sequence, item, error.what());
                ++totals.problems;
            }
            ++totals.round_trips;
        }
    }
}

} // namespace hokuto::tools::commands
