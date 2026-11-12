// SPDX-License-Identifier: MIT

// The check of each sound of hokuto_check_mix after a round's edits: the edited track's reach, the notes and each
// track's settings. check_mix.cpp describes the checks.

#include "check_mix.h"

namespace hokuto::tools::mix
{
namespace
{

// Counts and prints the tracks of sound `sound`, and the sound itself, that changed with the edit of `plan` but that
// its reach leaves out. `changed` lists the tracks that changed. `passes` is whether the loop and the length stayed.
// `sound_name` is the name of the sound, and `name` that of the archive.
void CheckReach(const Plan& plan, uint32_t sound, const std::set<int>& changed, bool passes,
                const CheckedArchive& archive, const std::string& sound_name, const std::string& name, Mix setting,
                Totals& totals)
{
    const auto listed = [](const auto& list, const auto& value)
    {
        return std::find(list.begin(), list.end(), value) != list.end();
    };
    for (const int number : changed)
    {
        if (sound != plan.sound || number == plan.track)
        {
            continue;
        }

        if (listed(plan.reach.tracks, number))
        {
            ++totals.reached_tracks;
            continue;
        }

        std::printf("%s: %s: track %d's %s changed with the edit of track %d, but its reach leaves it out\n",
                    name.c_str(), sound_name.c_str(), number, NameOf(setting), plan.track);
        ++totals.unlisted;
    }

    const bool other_changed = sound != plan.sound && (!changed.empty() || !passes);
    if (other_changed && listed(plan.reach.sounds, sound))
    {
        ++totals.reached_sounds;
    }
    else if (other_changed)
    {
        std::printf("%s: %s changed with the %s edit of %s's track %d, but its reach leaves the sound out\n",
                    name.c_str(), sound_name.c_str(), NameOf(setting), archive.NameOf(plan.sound).c_str(), plan.track);
        ++totals.unlisted;
    }
}

// Counts and prints sound `sound` when its notes differ after the edit of `plan`, apart or not. `changed` lists the
// tracks that changed. `was_read` and `now_read` are the reads before and after the edit. `sound_name` is the name of
// the sound, and `name` that of the archive.
void CheckNotes(const CheckedArchive& archive, const Plan& plan, uint32_t sound, const std::set<int>& changed,
                const Sequence& was_read, const Sequence& now_read, const std::string& sound_name,
                const std::string& name, Mix setting, Totals& totals)
{
    const NotesApart psg = archive.gba     ? PsgNotesApart(*archive.gba, sound, changed)
                           : archive.Rd2() ? hokuto::tools::Rd2PsgNotesApart(*archive.Rd2(), sound, changed)
                           : plan.driver != nullptr && plan.driver->DriverEngine() == hokuto::format::Engine::kQuintet
                               ? QuintetNotesApart(changed)
                               : NotesApart();
    if (NotesOf(now_read) != NotesOf(was_read) && (setting == Mix::kVolume || setting == Mix::kPan) && psg &&
        NotesOf(now_read, psg) == NotesOf(was_read, psg))
    {
        std::printf("%s: %s: apart: notes of a PSG channel end elsewhere after the %s edits\n", name.c_str(),
                    sound_name.c_str(), NameOf(setting));
        ++totals.psg_sounds;
    }
    else if (NotesOf(now_read) != NotesOf(was_read) && setting == Mix::kVolume && archive.ds &&
             ChannelsDecideAll(was_read, now_read))
    {
        std::printf("%s: %s: apart: notes take other channels after the volume edits\n", name.c_str(),
                    sound_name.c_str());
        ++totals.channel_sounds;
    }
    else if (NotesOf(now_read) != NotesOf(was_read))
    {
        std::printf("%s: %s: the notes differ after the %s edits\n", name.c_str(), sound_name.c_str(), NameOf(setting));
        PrintNotesOnlyIn(name, was_read, now_read);
        ++totals.note_sounds;
    }
}

// One track's settings after the edits of a round: the setting of the round changes as the plan says, and the other
// settings stay.
void CheckTrackSettings(const Track& old, const Sequence& was_read, const Sequence& now_read, const Plan& plan,
                        Mix setting, const File& file, const std::map<uint32_t, std::vector<uint32_t>>& offsets,
                        uint32_t sequence, const CheckedArchive& archive, const CheckedSound& info, uint32_t end,
                        const std::string& name, Totals& totals)
{
    const Track* track = TrackNumbered(now_read, old.number);
    if (track == nullptr)
    {
        std::printf("%s: %s: track %d plays no notes after the %s edits\n", name.c_str(), info.name.c_str(), old.number,
                    NameOf(setting));
        ++totals.differ;
        return;
    }

    ++totals.tracks;
    for (const Mix shown : kSettings)
    {
        std::vector<Setting> was = hokuto::song::MixSettings(old, shown);
        std::vector<Setting> now = hokuto::song::MixSettings(*track, shown);

        if (plan.dse && shown == Mix::kVolume)
        {
            was = AgainstDseStart(std::move(was));
            now = AgainstDseStart(std::move(now));
        }
        if (shown != setting)
        {
            if (now != was)
            {
                std::printf("%s: %s: track %d's %s changed with a %s edit. Before: %s. After: %s.\n", name.c_str(),
                            info.name.c_str(), old.number, NameOf(shown), NameOf(setting), Text(was).c_str(),
                            Text(now).c_str());
                ++totals.differ;
            }
            continue;
        }

        // The edited commands that the track runs, and whether the new command opens it.
        Track channel_track = old;
        channel_track.stretches = hokuto::song::MixStretches(was_read, old);
        const std::optional<Run> run = RunOf(*file.sequences[sequence], offsets.at(sequence), channel_track, setting,
                                             plan.gba, plan.dse, plan.driver);
        if (!run && archive.dse && RunsIntoData(*file.sequences[sequence], offsets.at(sequence), channel_track))
        {
            ++totals.past_end;
            continue;
        }
        if (!run)
        {
            std::printf("%s: %s: track %d ran commands that aren't the sequence's items before the %s edits\n",
                        name.c_str(), info.name.c_str(), old.number, NameOf(setting));
            ++totals.differ;
            continue;
        }

        std::size_t edited_items = 0;
        for (const uint32_t item : run->items)
        {
            edited_items += plan.run.items.contains(item) ? 1 : 0;
        }
        const bool same_channel = plan.channel >= 0 && old.channel == plan.channel;
        const bool opened =
            plan.opening && (same_channel || (old.number == plan.track && !old.stretches.empty() &&
                                              old.stretches.front().entry == ntdlib::sequence_export::Entry::kOpen &&
                                              hokuto::edit::ItemAtOffset(offsets.at(sequence),
                                                                         old.stretches.front().first) == plan.opening));

        // A track that runs from the item before the new command's place into the place goes past the new
        // command.
        bool runs_past = false;
        for (const Stretch& stretch : old.stretches)
        {
            const std::vector<uint32_t>& at = offsets.at(sequence);
            const std::optional<uint32_t> first_item = hokuto::edit::ItemAtOffset(at, stretch.first);
            const std::optional<uint32_t> last_item = hokuto::edit::ItemAtOffset(at, stretch.last);
            runs_past = runs_past || (plan.opening && first_item && last_item && *first_item < *plan.opening &&
                                      *last_item >= *plan.opening);
        }
        totals.runs_past += runs_past ? 1 : 0;
        if (edited_items != 0 && edited_items != run->items.size())
        {
            ++totals.mixed;
            continue;
        }

        const bool mapped = edited_items != 0;
        const auto map = [&plan, mapped](int value)
        {
            return mapped ? Mapped(plan, value) : value;
        };
        const int fallback = DefaultOf(setting, plan.gba, plan.dse, plan.driver);
        const std::optional<int> opening = opened ? std::optional<int>(Mapped(plan, fallback)) : std::nullopt;
        const std::vector<Setting> expected =
            Before(Expected(was, setting, opening, old.stretches.empty() ? 0 : old.stretches.front().tick, map,
                            plan.gba || plan.driver != nullptr, fallback),
                   end);
        totals.mapped += mapped ? 1 : 0;
        totals.opened += opened ? 1 : 0;
        if (now == expected)
        {
            continue;
        }

        // A command that sets the default shows no change before the edit. Once the edit maps the default,
        // the command shows a change in its tick, before any other. A track's volume of the GBA or of one
        // of its other drivers starts with the default at tick 0 in both reads, and the change comes after
        // that (Expected).
        const int mapped_default = Mapped(plan, fallback);
        const bool start_volume = (plan.gba || plan.driver != nullptr) && setting == Mix::kVolume;
        const auto past_start = [start_volume, fallback](const std::vector<Setting>& settings)
        {
            const bool start =
                start_volume && !settings.empty() && settings.front().tick == 0 && settings.front().value == fallback;
            return start ? std::next(settings.begin()) : settings.begin();
        };
        const auto now_first = past_start(now);
        const auto was_first = past_start(was);
        if (mapped && !opened && run->again && now_first != now.end() && now_first->value == mapped_default &&
            (was_first == was.end() || now_first->tick <= was_first->tick) &&
            now == Before(Expected(was, setting, mapped_default, now_first->tick, map,
                                   plan.gba || plan.driver != nullptr, fallback),
                          end))
        {
            ++totals.again;
            continue;
        }

        if (run->ramp || run->random)
        {
            std::printf(
                "%s: %s: apart: track %d's %s %s after the edit of track %d from %d to %d. Before: "
                "%s. Expected: %s. After: %s.\n",
                name.c_str(), info.name.c_str(), old.number, NameOf(setting),
                run->ramp ? "moves over time" : "takes random values", plan.track, plan.edit.from, plan.edit.to,
                Text(was).c_str(), Text(expected).c_str(), Text(now).c_str());
            ++totals.moving;
            continue;
        }

        std::printf(
            "%s: %s: track %d's %s differs after the edit of track %d from %d to %d%s. Before: %s. "
            "Expected: %s. After: %s.\n",
            name.c_str(), info.name.c_str(), old.number, NameOf(setting), plan.track, plan.edit.from, plan.edit.to,
            opened ? ". A new command opens it" : "", Text(was).c_str(), Text(expected).c_str(), Text(now).c_str());
        ++totals.differ;
    }
}

} // namespace

std::optional<uint32_t> SequenceOf(const std::vector<std::optional<uint32_t>>& sequences, const CheckedSound& info)
{
    return info.file_id < sequences.size() ? sequences[info.file_id] : std::nullopt;
}

void CheckSoundAgain(uint32_t sound, const CheckedArchive& archive, const File& file,
                     const std::vector<std::optional<uint32_t>>& sequences,
                     const std::vector<std::optional<Sequence>>& before, const std::map<uint32_t, Plan>& plans,
                     const CheckedArchive& saved, const CheckedArchive& after, bool timelines,
                     const std::map<uint32_t, std::vector<uint32_t>>& offsets, Mix setting, const std::string& name,
                     Totals& totals)
{
    const CheckedSound info = archive.Info(sound);
    const std::optional<uint32_t> sequence = SequenceOf(sequences, info);
    if (!before[sound] || !sequence || !plans.contains(*sequence))
    {
        return;
    }

    std::optional<Sequence> read;
    try
    {
        read = after.Read(sound);
        if (timelines && !SamePlaying(saved.Read(sound), *read))
        {
            std::printf("%s: %s plays otherwise from the saved archive after the %s edits\n", name.c_str(),
                        info.name.c_str(), NameOf(setting));
            ++totals.saved;
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s can't be read after the %s edits: %s\n", name.c_str(), info.name.c_str(), NameOf(setting),
                    error.what());
        ++totals.unread;
        return;
    }

    // ntdlib's performer finds a loop where the tracks' states repeat. After an edit, it can find the same loop
    // a whole number of passes away. A sound without a loop ends once its last release falls silent, and its
    // levels decide when. Every note of both reads then has to start before the earlier end. The reads compare
    // up to the earlier end, and the sound counts apart. Any other change of the loop or the length fails.
    const bool passes = read->loop == before[sound]->loop && read->length == before[sound]->length;
    const bool fades = !passes && !before[sound]->loop && !read->loop;
    const std::optional<uint32_t> cut =
        passes ? std::nullopt
               : (fades ? std::optional<uint32_t>(std::min(before[sound]->length, read->length))
                        : EarlierEnd(*before[sound], *read));
    const std::size_t late = fades ? NotesFrom(*before[sound], *cut) + NotesFrom(*read, *cut) : 0;
    if (late != 0)
    {
        std::printf(
            "%s: %s: the sound ends at tick %u after the %s edits, and at %u before. %zu notes start at or "
            "after the earlier end.\n",
            name.c_str(), info.name.c_str(), read->length, NameOf(setting), before[sound]->length, late);
        ++totals.note_sounds;
        return;
    }

    if (fades)
    {
        std::printf("%s: %s: apart: the sound ends at tick %u after the %s edits, and at %u before.\n", name.c_str(),
                    info.name.c_str(), read->length, NameOf(setting), before[sound]->length);
    }
    if (!passes && !cut)
    {
        std::printf(
            "%s: %s: the loop or the length moves with the %s edits. Before: loop %s, %u ticks. After: "
            "loop %s, %u ticks.\n",
            name.c_str(), info.name.c_str(), NameOf(setting), LoopText(before[sound]->loop).c_str(),
            before[sound]->length, LoopText(read->loop).c_str(), read->length);
        ++totals.moved_loops;
        return;
    }

    totals.other_passes += cut && !fades ? 1 : 0;
    totals.other_ends += fades ? 1 : 0;
    const Sequence was_read = cut ? Before(*before[sound], *cut) : *before[sound];
    const Sequence now_read = cut ? Before(*read, *cut) : *read;
    const uint32_t end = cut.value_or(UINT32_MAX);

    // The tracks that the edit changed, and the other sounds with such tracks, are in the edited track's reach.
    const Plan& plan = plans.at(*sequence);
    const std::set<int> changed = ChangedTracks(was_read, now_read, setting);

    CheckReach(plan, sound, changed, passes, archive, info.name, name, setting, totals);

    // The notes stay as they were. On the GBA and on Nintendo R&D2's driver, the notes of a PSG channel on a
    // track whose volume or pan changed count apart (PsgNotesApart and Rd2PsgNotesApart). So do the notes of
    // a track of Quintet's driver whose volume changed (QuintetNotesApart).
    CheckNotes(archive, plan, sound, changed, was_read, now_read, info.name, name, setting, totals);

    // Each track's settings.
    for (const Track& old : was_read.tracks)
    {
        CheckTrackSettings(old, was_read, now_read, plan, setting, file, offsets, *sequence, archive, info, end, name,
                           totals);
    }
}

} // namespace hokuto::tools::mix
