// SPDX-License-Identifier: MIT

// The round of hokuto_check_commands that adds settings to the rows.

#include "check_commands.h"

namespace hokuto::tools::commands
{

namespace
{

// The new settings of the fifth round in one sequence: the sound on whose tracks they went, and the track and the tick
// of each. `items` holds the new item of each item of the sequence, and `added` the items of the new settings.
struct Placed
{
    uint32_t sound = 0;
    std::vector<int> tracks;
    std::vector<uint32_t> ticks;
    std::vector<std::optional<uint32_t>> items;
    std::vector<uint32_t> added;
};

} // namespace

std::span<const hokuto::song::Stretch> RowsOfSound(const hokuto::song::Sequence& song)
{
    for (const hokuto::song::Track& track : song.tracks)
    {
        if (!track.stretches.empty())
        {
            return track.stretches;
        }
    }

    return {};
}

namespace
{

// The `count`th new setting of the fifth round: a volume, a pan or a second volume, in turn. None of them changes a
// note. With `gba`, a volume or a pan: MusicPlayer2000 has no second volume.
Command SettingNumbered(std::size_t count, bool gba)
{
    constexpr std::array<std::pair<Kind, int>, 3> kSettings = {
        {{Kind::kVolume, 77}, {Kind::kPan, 20}, {Kind::kVolume2, 90}}};
    const auto& [kind, value] = kSettings[count % (gba ? 2 : kSettings.size())];
    Command command;
    command.kind = kind;
    command.value = ntdlib::commands::Argument{Source::kValue, value * kOne, 0};

    return command;
}

// The ticks of the fifth round's settings on `track`: the starts of its first note and of a note halfway through its
// notes, and the ticks halfway from each of them to the next note that starts later.
std::vector<uint32_t> SettingTicks(const hokuto::song::Track& track)
{
    const std::vector<hokuto::song::Note>& notes = track.notes;
    std::set<uint32_t> ticks;
    for (const std::size_t index : {std::size_t{0}, notes.size() / 2})
    {
        if (index >= notes.size())
        {
            continue;
        }

        const uint32_t start = notes[index].start;
        const auto later = [start](const hokuto::song::Note& note)
        {
            return note.start > start;
        };
        const auto next = std::find_if(notes.begin() + static_cast<std::ptrdiff_t>(index), notes.end(), later);
        ticks.insert(start);
        if (next != notes.end() && next->start > start + 1)
        {
            ticks.insert(start + (next->start - start) / 2);
        }
    }

    return {ticks.begin(), ticks.end()};
}

// The fifth round's check of the commands that `before` and `after` ran, the sound of `placed` before and after its
// settings went into sequence `sequence`. `file` holds the sequence before, and `edited` after. Each new setting has to
// run at its tick, before the notes that its stretch starts there. When the loop starts a whole number of passes
// earlier, the rows after the edit end sooner. A setting past their end stays out, and `past` counts it. Every other
// command has to run at its tick. Gives the number of problems.
std::size_t CheckRows(const std::string& name, const File& file, const File& edited, uint32_t sequence,
                      const hokuto::song::Sequence& before, const hokuto::song::Sequence& after, const Placed& placed,
                      std::size_t& past)
{
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    const std::vector<uint32_t> old_offsets = hokuto::edit::Offsets(*file.sequences[sequence], console);
    const ntdlib::commands::Sequence& commands = *edited.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, console);
    std::size_t problems = 0;

    // Each new setting, before the notes of its stretch at its tick.
    const uint32_t end = after.loop ? after.loop->end : std::numeric_limits<uint32_t>::max();
    for (std::size_t setting = 0; setting < placed.ticks.size(); ++setting)
    {
        const uint32_t offset = offsets.at(placed.added.at(setting));
        const uint32_t tick = placed.ticks[setting];
        if (tick >= end)
        {
            ++past;
            continue;
        }
        const std::vector<hokuto::edit::CommandRow> rows = RowsOf(edited, sequence, after, placed.tracks[setting]);
        const auto ran = [offset, tick](const hokuto::edit::CommandRow& row)
        {
            return row.command == offset && row.tick == tick;
        };
        const auto row = std::find_if(rows.begin(), rows.end(), ran);
        bool late = row == rows.end();
        for (auto earlier = rows.begin(); !late && earlier != row; ++earlier)
        {
            const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, earlier->command);
            late = earlier->stretch == row->stretch && earlier->tick == tick && item &&
                   commands.items.at(*item).kind == Kind::kNote;
        }
        if (late)
        {
            std::printf(
                "%s: sequence %u: the new setting at tick %u of track %d doesn't run at its tick before its "
                "notes\n",
                name.c_str(), sequence, tick, placed.tracks[setting]);
            ++problems;
        }
    }

    // Every other command, at its tick. A command whose tick isn't known before can run at any tick. A wait that a new
    // setting splits keeps its item in the piece after the setting. When the loop ends earlier, the rows after the edit
    // stop at its end.
    const ntdlib::commands::Sequence& old_commands = *file.sequences[sequence];
    for (const hokuto::song::Track& track : before.tracks)
    {
        std::multiset<std::pair<uint32_t, uint32_t>> ran;
        for (const hokuto::edit::CommandRow& row : RowsOf(edited, sequence, after, track.number))
        {
            if (row.tick)
            {
                ran.emplace(row.command, *row.tick);
            }
        }
        for (const hokuto::edit::CommandRow& row : RowsOf(file, sequence, before, track.number))
        {
            const std::optional<uint32_t> old_item = hokuto::edit::ItemAtOffset(old_offsets, row.command);
            if (!row.tick || *row.tick >= end || !old_item || old_commands.items.at(*old_item).kind == Kind::kWait)
            {
                continue;
            }

            const std::optional<uint32_t> item = placed.items.at(*old_item);
            const auto found = item ? ran.find({offsets.at(*item), *row.tick}) : ran.end();
            if (found == ran.end())
            {
                std::printf(
                    "%s: sequence %u: the command at %u of track %d no longer runs at tick %u after the new "
                    "settings\n",
                    name.c_str(), sequence, row.command, track.number, *row.tick);
                ++problems;
                break;
            }
            ran.erase(found);
        }
    }

    return problems;
}

// The new settings at the ticks of each track of `song` that sequence `sequence` of `file` accepts alone. Each gets the
// next number of `count`, and `place` receives its track and tick. A refusal counts in `refusals` and `totals`.
std::vector<hokuto::edit::NewSetting> TrySettingsAlone(const File& file, const CheckedArchive& archive,
                                                       uint32_t sequence, const hokuto::song::Sequence& song,
                                                       std::size_t& count, Placed& place,
                                                       std::map<std::string, std::size_t>& refusals, Totals& totals)
{
    std::vector<hokuto::edit::NewSetting> settings;
    for (const hokuto::song::Track& track : song.tracks)
    {
        for (const uint32_t tick : SettingTicks(track))
        {
            const std::optional<hokuto::song::Stretch> stretch = hokuto::edit::StretchAt(track.stretches, tick);
            if (!stretch)
            {
                continue;
            }

            const hokuto::edit::NewSetting setting{*stretch, tick, SettingNumbered(count++, archive.gba != nullptr)};
            try
            {
                hokuto::edit::AddSettings(file, sequence, {&setting, 1});
                settings.push_back(setting);
                place.tracks.push_back(track.number);
                place.ticks.push_back(tick);
            }
            catch (const std::invalid_argument& error)
            {
                ++refusals[error.what()];
                ++totals.settings_refused;
            }
        }
    }

    return settings;
}

// Checks that each sound of the sequences in `placed` plays its notes again from `edited`, the file with the new
// settings, as it played `songs` from `file`.
void CheckSettingSounds(const std::string& name, const File& file, const File& edited, const CheckedArchive& archive,
                        const std::vector<std::optional<uint32_t>>& sequences,
                        const std::map<uint32_t, hokuto::song::Sequence>& songs,
                        const std::map<uint32_t, Placed>& placed, Totals& totals)
{
    // Each sound of those sequences plays its notes again from the archive with the settings.
    CheckedArchive after;
    try
    {
        hokuto::tools::CheckSaved(edited);
        after = hokuto::tools::BuildChecked(edited, name);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file with the new settings can't be exported: %s\n", name.c_str(), error.what());
        ++totals.problems;
        return;
    }
    for (const auto& [sound, before] : songs)
    {
        const CheckedSound info = archive.Info(sound);
        const uint32_t sequence = *sequences[info.file_id];
        const auto place = placed.find(sequence);
        if (place == placed.end())
        {
            continue;
        }

        ++totals.setting_sounds;
        try
        {
            const hokuto::song::Sequence read = after.Read(sound);
            const std::vector<Played> notes = NotesOf(before);
            totals.setting_notes += notes.size();
            bool moved = false;
            if (!SamePlaying(before, read, moved))
            {
                // On the GBA, a volume or a pan changes the level of a PSG channel's notes. A quiet note can then fall
                // silent, or a silent note sound (tools::PsgNotesApart). On the DS, the driver assigns a new note the
                // quietest channel, and a volume or a pan changes which channel that is. A note can then find another
                // channel, or none (SameNoteCommands).
                const std::set<int> tracks(place->second.tracks.begin(), place->second.tracks.end());
                // On Konami's driver, a pan decides which PSG channel plays a note (tools::KonamiPsgNotesApart). On
                // Nintendo R&D2's driver, a PSG note ends once its level falls to 0 (tools::Rd2PsgNotesApart). On
                // Brownie Brown's driver, a volume or a pan can silence a PSG note (tools::BrowniePsgNotesApart).
                const hokuto::tools::NotesApart psg =
                    archive.gba         ? hokuto::tools::PsgNotesApart(*archive.gba, sound, tracks)
                    : archive.Konami()  ? hokuto::tools::KonamiPsgNotesApart(archive.Konami()->info.revision)
                    : archive.Rd2()     ? hokuto::tools::Rd2PsgNotesApart(*archive.Rd2(), sound, tracks)
                    : archive.Brownie() ? hokuto::tools::BrowniePsgNotesApart(tracks)
                                        : hokuto::tools::NotesApart();
                if (archive.ds && SameNoteCommands(before, read, moved))
                {
                    ++totals.setting_channels;
                }
                else if (archive.Konami() && (!psg || !SamePlaying(before, read, moved, 0, psg)) &&
                         CutByVolume(edited, sequence, before, read, psg))
                {
                    ++totals.setting_cuts;
                }
                else if (!psg || !SamePlaying(before, read, moved, 0, psg))
                {
                    std::printf("%s: %s plays other notes after the new settings: %zu notes before, %zu after\n",
                                name.c_str(), info.name.c_str(), notes.size(), NotesOf(read).size());
                    ++totals.problems;
                    continue;
                }
                else
                {
                    ++totals.psg_sounds;
                }
            }

            totals.moved_loops += moved && !PartPass(before, read) ? 1 : 0;
            totals.part_passes += PartPass(before, read) ? 1 : 0;
            if (place->second.sound == sound)
            {
                totals.problems +=
                    CheckRows(name, file, edited, sequence, before, read, place->second, totals.settings_past);
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the new settings: %s\n", name.c_str(), info.name.c_str(),
                        error.what());
            ++totals.problems;
        }
    }
}

} // namespace

void NewSettings(const std::string& name, const File& file, const CheckedArchive& archive,
                 const std::vector<std::optional<uint32_t>>& sequences,
                 const std::map<uint32_t, hokuto::song::Sequence>& songs, Totals& totals)
{
    // Each setting goes in by itself first. Those that go in by themselves then go in together.
    File edited = file;
    std::map<uint32_t, Placed> placed;
    std::set<uint32_t> tried;
    std::map<std::string, std::size_t> refusals;
    std::size_t count = 0;
    for (const auto& [sound, song] : songs)
    {
        const uint32_t sequence = *sequences[archive.FileOf(sound)];
        if (!tried.insert(sequence).second)
        {
            continue;
        }

        Placed place{.sound = sound};
        const std::vector<hokuto::edit::NewSetting> settings =
            TrySettingsAlone(file, archive, sequence, song, count, place, refusals, totals);
        if (settings.empty())
        {
            continue;
        }

        try
        {
            hokuto::edit::Edited added = hokuto::edit::AddSettings(edited, sequence, settings);
            if (hokuto::tools::Outgrows(archive, added.file, sequence))
            {
                ++totals.outgrown;
                continue;
            }
            edited = std::move(added.file);
            place.items = std::move(added.items);
            place.added = std::move(added.added);
            totals.settings += settings.size();
            ++totals.setting_sequences;
            placed.emplace(sequence, std::move(place));
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u: %zu new settings go in one at a time but not together: %s\n", name.c_str(),
                        sequence, settings.size(), error.what());
            ++totals.problems;
        }
    }
    for (const auto& [message, refused] : refusals)
    {
        std::printf("%s: %zu new settings were refused: %s\n", name.c_str(), refused, message.c_str());
    }

    CheckSettingSounds(name, file, edited, archive, sequences, songs, placed, totals);
}

} // namespace hokuto::tools::commands
