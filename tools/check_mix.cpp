// SPDX-License-Identifier: MIT

// Checks the edits of a track's mix (src/edit/mix.h) on a sound archive of the Wii, the 3DS, the Wii U or the Switch.
// The tool imports the archive into a Hokuto file the way the program does. It reads each sequence sound with ntdlib's
// performer. It then makes five rounds of edits, one for each setting: the volume, the pan, and the sends to the aux
// buses A to C. In each round, one track of each sequence file takes an edit, through the first sound that plays the
// file. The tracks that play notes take turns from round to round. The edit starts from the value that the mixer shows
// at the track's first note. A level halves, or goes from 0 to 64. A pan moves 16 steps to the right, or to the left
// from 48 up.
//
// The tool works out each edit's map from old values to new ones. Where the track's commands give the setting one
// value, the map takes every value to the new one. Otherwise it scales a level and shifts a pan. The default value
// counts where a note plays before the track's first command. A new command then opens the track with the default
// mapped, unless the map leaves the default as it is. Where the command before the track's start can run on into it, a
// jump goes before the new command. A return can run on: a sound that reaches it outside every call has no call to
// return to, and the player runs on past it. The tool also works out which edits the sequence refuses. The 3DS's tracks
// have no send to bus C, and the tool works out a refusal for each edit of that round there.
//
// After each round, the tool exports the edited file and checks four things:
//
// - The sequence refuses the edits that the tool worked out as refused, and no others.
// - The archive, imported again, gives back the edited sequences. Each edited command holds its mapped value, and a new
//   command and a jump stand where they were worked out.
// - Each sound plays the notes that it played before. ntdlib's performer can find a sound's loop a whole number of
//   passes away after an edit. The sound then compares up to the earlier loop's end, and counts apart. A sound without
//   a loop ends once its last release falls silent, and an edit of its levels can move its end. Every note of both
//   reads then has to start before the earlier end. The notes' lengths and the settings compare up to that end, and
//   the sound counts apart with a line. Any other change of the sound's loop or length fails.
// - Each track's settings change as the edit says. A track whose commands of the setting all took the edit shows each
//   of its old values mapped. A command that set the default shows no change before the edit, and a change to the
//   mapped default after it. A track that the new command opens starts with the default mapped: a track with the
//   edited track's number, opened where the edited track opened. A sound that runs on into the edited track's start
//   doesn't run the new command. Every other setting of every track stays as it was.
//
// Some tracks are counted apart: a track that runs both edited commands and other commands of the setting, and a track
// whose setting moves over time or takes random values. The values that such a track shows depend on more than the map.
//
// The tool also checks what the mixer lists before an edit (src/edit/reach.h). Every other track of the edited sound
// whose setting changed has to be among the tracks that the edited track's reach lists. Every other sound with such a
// track, or with its loop on another pass, has to be among the reach's sounds. The tool counts the listed tracks and
// sounds, and those that changed. It also gives the time of the slowest reach.
//
// With --timelines, the tool opens the file's timelines as the program does (edit/timelines.h). Each edit of a timeline
// then takes the converter's layout as the program settles it (edit::SettleTimelines). The tool counts apart the edits
// that the layout can't hold. The layout places the commands as the converter does. The tool therefore doesn't check
// where a timeline's commands stand. The file that the saved archive imports into, with the timelines' hints, has to
// give back the settled sequences. The sounds play from the archive that the program plays, with every bar in its
// track. The archive that a saved file holds, with the converter's subroutines, has to play the same notes and
// settings.
//
// The tool prints a line for each track that differs, and a summary. It returns 1 when an edit or a track differs from
// what the tool worked out, or when an export or a read fails. With a sound's name, it checks that sound alone.
//
// With --model, the file goes into the model form before the edits (format::InModelForm), and the archive that the
// model builds has to be the game's. Each file that the edits make then has to save in both forms and open again as it
// was (tools/checked_archive.h).
//
// usage: hokuto_check_mix [--model] [--timelines] <archive> [sound]

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "edit/bytecode.h"
#include "edit/mix.h"
#include "edit/reach.h"
#include "edit/replace.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/file.h"
#include "loop_passes.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace
{

using hokuto::edit::MixEdit;
using hokuto::edit::Reach;
using hokuto::edit::TrackReach;
using hokuto::format::File;
using hokuto::song::Mix;
using hokuto::song::Sequence;
using hokuto::song::Setting;
using hokuto::song::Stretch;
using hokuto::song::Track;
using hokuto::tools::CheckedArchive;
using hokuto::tools::CheckedSound;
using hokuto::tools::EarlierEnd;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

// The settings, one for each round.
constexpr std::array<Mix, 5> kSettings = {Mix::kVolume, Mix::kPan, Mix::kSendA, Mix::kSendB, Mix::kSendC};

// What the check counted, for the summary.
struct Totals
{
    std::size_t sounds = 0;
    std::size_t unread = 0;
    std::size_t edits = 0;
    std::size_t refused = 0;
    std::size_t new_commands = 0;
    std::size_t wrong_edits = 0;
    std::size_t tracks = 0;
    std::size_t mapped = 0;
    std::size_t opened = 0;
    std::size_t again = 0;
    std::size_t jumps = 0;
    std::size_t runs_past = 0;
    std::size_t differ = 0;
    std::size_t note_sounds = 0;
    std::size_t mixed = 0;
    std::size_t moving = 0;
    std::size_t other_passes = 0;
    std::size_t moved_loops = 0;
    std::size_t other_ends = 0;
    std::size_t listed_tracks = 0;
    std::size_t listed_sounds = 0;
    std::size_t reached_tracks = 0;
    std::size_t reached_sounds = 0;
    std::size_t unlisted = 0;
    std::size_t timelines = 0;
    std::size_t timeline_edits = 0;
    std::size_t timeline_refused = 0;
    std::size_t saved = 0;
    double slowest_reach = 0.0; // seconds
};

// A setting's name in a line about it.
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

// The items of a sequence that set a setting on a track. `early` is set when a note ran before the first of them, or
// when there are none.
struct Run
{
    std::set<uint32_t> items;
    bool early = false;
    bool random = false;   // a command takes a random value
    bool variable = false; // a command takes a variable's value
    bool ramp = false;     // a command moves the setting over time
    bool again = false;    // a command sets the default value
};

// The run of `setting` on `track`, or nothing when a stretch isn't made of the sequence's items.
std::optional<Run> RunOf(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& offsets,
                         const Track& track, Mix setting)
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
                run.early = run.items.empty() ? note : run.early;
                run.items.insert(i);
                run.random = run.random || command.value.source == Source::kRandom;
                run.variable = run.variable || command.value.source == Source::kVariable;
                run.ramp = run.ramp || command.time.has_value();
                run.again = run.again || (command.value.source == Source::kValue &&
                                          Whole(command.value.value) == hokuto::song::MixDefault(setting));
            }
            note = note || command.kind == Kind::kNote;
        }
    }
    run.early = run.early || run.items.empty();

    return run;
}

// The edit of one track of a sequence file in a round, and what the tool worked out about it.
struct Plan
{
    uint32_t sound = 0; // the sound that the edit came through
    int track = 0;      // the track's number
    std::vector<Stretch> stretches;
    MixEdit edit;
    Run run;
    bool trim = false;                  // the map scales or shifts the values, instead of setting one value
    std::optional<std::string> refusal; // why the sequence refuses the edit
    std::optional<uint32_t> opening;    // the item before which a new command goes
    bool jump = false;                  // a jump before the new command takes the item before it past it
    Reach reach;                        // the tracks and sounds that the mixer lists for the edit
};

// The value that `plan` maps `value` to.
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

    return std::clamp(result, hokuto::edit::MixLowest(setting), hokuto::edit::MixHighest(setting));
}

// Whether the tracks of `console` have `setting`.
bool Has(const hokuto::song::Console& console, Mix setting)
{
    if (setting == Mix::kVolume || setting == Mix::kPan)
    {
        return true;
    }

    const int bus = static_cast<int>(setting) - static_cast<int>(Mix::kSendA);
    return hokuto::song::Runs(console, Kind::kSend, static_cast<uint8_t>(bus));
}

// The plan for `setting` on `track` of the sound `sound` of `file`. The sound plays sequence `index`.
std::optional<Plan> PlanFor(const File& file, uint32_t index, const std::vector<uint32_t>& offsets, uint32_t sound,
                            const Track& track, Mix setting)
{
    const ntdlib::commands::Sequence& sequence = *file.sequences[index];
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    const std::optional<Run> run = RunOf(sequence, offsets, track, setting);
    if (!run || track.stretches.empty())
    {
        return std::nullopt;
    }

    // The edit starts from the value at the track's first note.
    Plan plan;
    plan.sound = sound;
    plan.track = track.number;
    plan.stretches = track.stretches;
    plan.run = *run;
    plan.edit.setting = setting;
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
        values.insert(hokuto::song::MixDefault(setting));
    }
    plan.trim = run->random || values.size() > 1;

    // The refusals, in the order that the edit checks them.
    const int fallback = hokuto::song::MixDefault(setting);
    const bool opens = run->early && Mapped(plan, fallback) != fallback;
    if (!Has(console, setting))
    {
        plan.refusal = "a send that the console's tracks lack";
    }
    else if (run->variable)
    {
        plan.refusal = "a variable";
    }
    else if (plan.trim && setting != Mix::kPan && plan.edit.from <= 0)
    {
        plan.refusal = "a trim from 0";
    }
    else if (opens && hokuto::edit::Fixed(sequence))
    {
        plan.refusal = "a fixed sequence";
    }
    else if (opens)
    {
        plan.opening = hokuto::edit::ItemAtOffset(offsets, track.stretches.front().first);
    }

    // A jump goes before the new command where the player can run on from the item before it: anything but a jump, a
    // return or an end that always runs, and a return that a sound reaches outside every call.
    if (plan.opening)
    {
        plan.jump = hokuto::edit::RunsInto(file, index, *plan.opening);
    }

    return plan;
}

// Whether `edited`, the sequence after `plan`, holds the plan's edit: each command of the run with its mapped value,
// and the new command where the plan put one. Every other item stays as it was, but for the targets.
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
        const int value = Mapped(plan, hokuto::song::MixDefault(plan.edit.setting));
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
        if (plan.run.items.contains(i))
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

// A note as the check compares it: its track, start, key, length and velocity.
using Played = std::tuple<int, uint32_t, uint32_t, uint32_t, uint32_t>;

// The notes of `sequence`, in order, for the check's comparisons.
std::vector<Played> NotesOf(const Sequence& sequence)
{
    std::vector<Played> notes;
    for (const Track& track : sequence.tracks)
    {
        for (const hokuto::song::Note& note : track.notes)
        {
            notes.emplace_back(track.number, note.start, note.key, note.length, note.velocity);
        }
    }
    std::sort(notes.begin(), notes.end());

    return notes;
}

// Whether `a` and `b` play the same notes, with the same loop and length, and give every track the same settings.
bool SamePlaying(const Sequence& a, const Sequence& b)
{
    if (NotesOf(a) != NotesOf(b) || a.loop != b.loop || a.length != b.length || a.tracks.size() != b.tracks.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.tracks.size(); ++i)
    {
        for (const Mix setting : kSettings)
        {
            if (a.tracks[i].number != b.tracks[i].number ||
                hokuto::song::MixSettings(a.tracks[i], setting) != hokuto::song::MixSettings(b.tracks[i], setting))
            {
                return false;
            }
        }
    }

    return true;
}

// The settings that a track should show after an edit: those of `before`, each value passed through `map`, after the
// value `opening` at tick `tick` where a new command opens the track. A change in the same tick as the one before
// replaces it, and a value that doesn't change shows no change.
template <typename Map>
std::vector<Setting> Expected(const std::vector<Setting>& before, Mix setting, std::optional<int> opening,
                              uint32_t tick, Map map)
{
    std::vector<Setting> expected;
    const int fallback = hokuto::song::MixDefault(setting);
    const auto add = [&expected, fallback](uint32_t at, int value)
    {
        if (!expected.empty() && expected.back().tick == at)
        {
            expected.pop_back();
        }
        if (value != (expected.empty() ? fallback : expected.back().value))
        {
            expected.push_back({at, value});
        }
    };
    if (opening)
    {
        add(tick, *opening);
    }
    for (const Setting& change : before)
    {
        add(change.tick, map(change.value));
    }

    return expected;
}

// Settings in a line about them, the first few of them.
std::string Text(const std::vector<Setting>& settings)
{
    std::string text;
    for (std::size_t i = 0; i < settings.size() && i < 6; ++i)
    {
        text += (i == 0 ? "" : ", ") + std::to_string(settings[i].value) + " at " + std::to_string(settings[i].tick);
    }

    return settings.empty() ? "none" : settings.size() > 6 ? text + ", ..." : text;
}

// The track with number `number` of `sequence`, or nothing.
const Track* TrackNumbered(const Sequence& sequence, int number)
{
    const auto found = std::find_if(sequence.tracks.begin(), sequence.tracks.end(),
                                    [number](const Track& track) { return track.number == number; });
    return found != sequence.tracks.end() ? &*found : nullptr;
}

// The numbers of the tracks of `before` whose setting `setting` changed in `after`, and of those that play no more.
std::set<int> ChangedTracks(const Sequence& before, const Sequence& after, Mix setting)
{
    std::set<int> changed;
    for (const Track& old : before.tracks)
    {
        const Track* track = TrackNumbered(after, old.number);
        if (track == nullptr || hokuto::song::MixSettings(*track, setting) != hokuto::song::MixSettings(old, setting))
        {
            changed.insert(old.number);
        }
    }

    return changed;
}

// The loop from `start` to `end`, or "none".
std::string LoopText(const std::optional<hokuto::song::Loop>& loop)
{
    return loop ? std::to_string(loop->start) + " to " + std::to_string(loop->end) : std::string("none");
}

// The settings of `settings` that come before tick `end`.
std::vector<Setting> Before(std::vector<Setting> settings, uint32_t end)
{
    std::erase_if(settings, [end](const Setting& change) { return change.tick >= end; });

    return settings;
}

// `sequence` up to tick `end`: the notes that start before it, each cut at it, and the settings that come before it.
Sequence Before(Sequence sequence, uint32_t end)
{
    for (Track& track : sequence.tracks)
    {
        std::erase_if(track.notes, [end](const hokuto::song::Note& note) { return note.start >= end; });
        for (hokuto::song::Note& note : track.notes)
        {
            note.length = std::min(note.length, end - note.start);
        }
        track.volume = Before(std::move(track.volume), end);
        track.pan = Before(std::move(track.pan), end);
        for (std::vector<Setting>& send : track.sends)
        {
            send = Before(std::move(send), end);
        }
    }

    return sequence;
}

// The number of notes of `sequence` that start at or after tick `end`.
std::size_t NotesFrom(const Sequence& sequence, uint32_t end)
{
    const auto late = [end](const hokuto::song::Note& note)
    {
        return note.start >= end;
    };
    std::size_t count = 0;
    for (const Track& track : sequence.tracks)
    {
        count += static_cast<std::size_t>(std::count_if(track.notes.begin(), track.notes.end(), late));
    }

    return count;
}

// The tracks of `sequence` that play notes.
std::vector<const Track*> Playing(const Sequence& sequence)
{
    std::vector<const Track*> tracks;
    for (const Track& track : sequence.tracks)
    {
        if (!track.notes.empty())
        {
            tracks.push_back(&track);
        }
    }

    return tracks;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    const bool timelines = arguments.size() > 1 && arguments[1] == "--timelines";
    const std::size_t first = timelines ? 2 : 1;
    if (arguments.size() < first + 1 || arguments.size() > first + 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_mix [--model] [--timelines] <archive> [sound]\n");
        return 2;
    }
    const std::string only = arguments.size() > first + 1 ? arguments[first + 1] : std::string();

    // The archive and the group files beside it make a Hokuto file. `playing` is the archive that the program plays.
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[first]);
    const std::string name = xsflib::Utf8(path.filename());
    std::vector<uint8_t> bytes;
    CheckedArchive archive;
    CheckedArchive playing;
    File file;
    std::vector<std::optional<uint32_t>> sequences;
    std::vector<std::optional<hokuto::edit::SoundPlace>> places;
    try
    {
        xsflib::ReadFile(path, bytes);
        archive = hokuto::tools::OpenChecked(path, bytes);
        file = hokuto::tools::CheckedForm(hokuto::tools::ImportChecked(bytes, archive), bytes);
        sequences = hokuto::tools::SequencesOfChecked(file, bytes, archive);
        places = hokuto::tools::PlacesOfChecked(file, bytes, archive);
        playing = archive;
        if (timelines)
        {
            file = hokuto::edit::OpenTimelines(std::move(file));
            playing = hokuto::tools::BuildChecked(file, name);
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    // Each sequence sound before the edits, and the first sound that plays each sequence file.
    Totals totals;
    std::vector<std::optional<Sequence>> before(archive.SoundCount());
    std::map<uint32_t, uint32_t> first_sounds;

    const auto sequence_of = [&](const CheckedSound& info)
    {
        return info.file_id < sequences.size() ? sequences[info.file_id] : std::nullopt;
    };
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        const CheckedSound info = archive.Info(sound);
        if (!info.sequence || (!only.empty() && info.name != only))
        {
            continue;
        }

        ++totals.sounds;
        try
        {
            before[sound] = playing.Read(sound);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read before the edits: %s\n", name.c_str(), info.name.c_str(), error.what());
            ++totals.unread;
            continue;
        }

        const std::optional<uint32_t> sequence = sequence_of(info);
        if (sequence && !Playing(*before[sound]).empty())
        {
            first_sounds.try_emplace(*sequence, sound);
        }
    }

    std::map<uint32_t, std::vector<uint32_t>> offsets;
    for (const auto& [sequence, sound] : first_sounds)
    {
        offsets[sequence] = hokuto::edit::Offsets(*file.sequences[sequence], hokuto::edit::ConsoleOf(file));
    }

    // The tracks and sounds that the mixer lists for an edit of each track of each sound that takes the edits.
    bool failed = false;
    std::map<uint32_t, std::vector<TrackReach>> reaches;
    for (const auto& [sequence, sound] : first_sounds)
    {
        const auto start = std::chrono::steady_clock::now();
        try
        {
            reaches[sound] = hokuto::edit::ReachOfTracks(file, places, sound, *before[sound]);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s: the reach can't be found: %s\n", name.c_str(), archive.NameOf(sound).c_str(),
                        error.what());
            failed = true;
        }
        const std::chrono::duration<double> taken = std::chrono::steady_clock::now() - start;
        totals.slowest_reach = std::max(totals.slowest_reach, taken.count());
    }

    for (std::size_t round = 0; round < kSettings.size(); ++round)
    {
        // One edit for each sequence file, on a track that plays notes.
        const Mix setting = kSettings[round];
        File edited = file;
        std::map<uint32_t, Plan> plans;
        for (const auto& [sequence, sound] : first_sounds)
        {
            const std::vector<const Track*> tracks = Playing(*before[sound]);
            const Track& track = *tracks[(sequence + round) % tracks.size()];
            std::optional<Plan> plan = PlanFor(file, sequence, offsets[sequence], sound, track, setting);
            if (!plan)
            {
                std::printf("%s: %s: track %d's stretches aren't made of items\n", name.c_str(),
                            archive.NameOf(sound).c_str(), track.number);
                failed = true;
                continue;
            }

            ++totals.edits;
            std::optional<std::string> refused;
            std::optional<File> mixed;
            try
            {
                mixed = hokuto::edit::EditMix(edited, sequence, plan->track, plan->stretches, plan->edit).file;
            }
            catch (const std::invalid_argument& error)
            {
                refused = error.what();
            }
            catch (const std::exception& error)
            {
                std::printf("%s: sequence %u can't take its edit: %s\n", name.c_str(), sequence, error.what());
                failed = true;
                continue;
            }

            if (refused.has_value() != plan->refusal.has_value())
            {
                std::printf("%s: %s: track %d's %s from %d to %d: %s, where the tool worked out %s\n", name.c_str(),
                            archive.NameOf(sound).c_str(), plan->track, NameOf(setting), plan->edit.from, plan->edit.to,
                            refused ? ("refused: " + *refused).c_str() : "taken",
                            plan->refusal ? ("a refusal for " + *plan->refusal).c_str() : "no refusal");
                ++totals.wrong_edits;
                continue;
            }

            if (refused)
            {
                ++totals.refused;
                std::printf("%s: %s: track %d's %s from %d to %d is refused for %s\n", name.c_str(),
                            archive.NameOf(sound).c_str(), plan->track, NameOf(setting), plan->edit.from, plan->edit.to,
                            plan->refusal->c_str());
                continue;
            }

            // Like the program, the tool settles an edit of a timeline in the converter's layout.
            if (hokuto::edit::IsTimeline(edited, sequence))
            {
                try
                {
                    mixed = hokuto::edit::SettleTimelines(edited, std::move(*mixed)).file;
                }
                catch (const std::invalid_argument& error)
                {
                    std::printf("%s: %s: track %d's %s from %d to %d is refused by the timeline: %s\n", name.c_str(),
                                archive.NameOf(sound).c_str(), plan->track, NameOf(setting), plan->edit.from,
                                plan->edit.to, error.what());
                    ++totals.timeline_refused;
                    continue;
                }
                ++totals.timeline_edits;
            }
            edited = std::move(*mixed);

            totals.new_commands += plan->opening ? 1 : 0;
            totals.jumps += plan->jump ? 1 : 0;
            const auto reach = reaches.find(sound);
            const auto index = static_cast<std::size_t>(&track - before[sound]->tracks.data());
            if (reach != reaches.end() && index < reach->second.size())
            {
                plan->reach = reach->second[index].mix[static_cast<std::size_t>(setting)];
                totals.listed_tracks += plan->reach.tracks.size();
                totals.listed_sounds += plan->reach.sounds.size();
            }
            plans.emplace(sequence, std::move(*plan));
        }

        // The edited archive that a saved file holds, the archive that the program plays, and the file that the first
        // imports into again. That file takes the timelines' hints that a saved file keeps.
        CheckedArchive saved;
        CheckedArchive after;
        File again;
        try
        {
            const File folded = hokuto::edit::FoldTimelines(edited);
            hokuto::tools::CheckSaved(folded);
            const std::vector<uint8_t> exported = hokuto::format::ExportArchive(folded);
            const std::vector<std::vector<uint8_t>> group_files = hokuto::format::ExportGroupFiles(folded);
            saved = hokuto::tools::BuildChecked(folded, name);
            after = timelines ? hokuto::tools::BuildChecked(edited, name) : saved;
            again = hokuto::tools::ImportChecked(exported, archive, group_files);
            if (timelines)
            {
                again.timelines = edited.timelines;
                again = hokuto::edit::OpenTimelines(std::move(again));
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: the file after the %s edits can't be exported: %s\n", name.c_str(), NameOf(setting),
                        error.what());
            return 1;
        }

        for (const auto& [sequence, plan] : plans)
        {
            const bool back = again.sequences.size() == edited.sequences.size() &&
                              again.sequences[sequence] == edited.sequences[sequence];
            const bool timeline = hokuto::edit::IsTimeline(edited, sequence);
            if (!back || (!timeline && !HoldsEdit(*file.sequences[sequence], *edited.sequences[sequence], plan)))
            {
                std::printf("%s: sequence %u: the %s edit of track %d %s\n", name.c_str(), sequence, NameOf(setting),
                            plan.track, back ? "isn't in the commands" : "doesn't come back from the archive");
                ++totals.wrong_edits;
            }
        }

        // Each sound that plays an edited sequence file, again.
        for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
        {
            const CheckedSound info = archive.Info(sound);
            const std::optional<uint32_t> sequence = sequence_of(info);
            if (!before[sound] || !sequence || !plans.contains(*sequence))
            {
                continue;
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
                std::printf("%s: %s can't be read after the %s edits: %s\n", name.c_str(), info.name.c_str(),
                            NameOf(setting), error.what());
                ++totals.unread;
                continue;
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
                continue;
            }

            if (fades)
            {
                std::printf("%s: %s: apart: the sound ends at tick %u after the %s edits, and at %u before.\n",
                            name.c_str(), info.name.c_str(), read->length, NameOf(setting), before[sound]->length);
            }
            if (!passes && !cut)
            {
                std::printf(
                    "%s: %s: the loop or the length moves with the %s edits. Before: loop %s, %u ticks. After: "
                    "loop %s, %u ticks.\n",
                    name.c_str(), info.name.c_str(), NameOf(setting), LoopText(before[sound]->loop).c_str(),
                    before[sound]->length, LoopText(read->loop).c_str(), read->length);
                ++totals.moved_loops;
                continue;
            }

            totals.other_passes += cut && !fades ? 1 : 0;
            totals.other_ends += fades ? 1 : 0;
            const Sequence was_read = cut ? Before(*before[sound], *cut) : *before[sound];
            const Sequence now_read = cut ? Before(*read, *cut) : *read;
            const uint32_t end = cut.value_or(UINT32_MAX);

            // The tracks that the edit changed, and the other sounds with such tracks, are in the edited track's reach.
            const Plan& plan = plans.at(*sequence);
            const std::set<int> changed = ChangedTracks(was_read, now_read, setting);

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
                            name.c_str(), info.name.c_str(), number, NameOf(setting), plan.track);
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
                            name.c_str(), info.name.c_str(), NameOf(setting), archive.NameOf(plan.sound).c_str(),
                            plan.track);
                ++totals.unlisted;
            }

            // The notes stay as they were.
            if (NotesOf(now_read) != NotesOf(was_read))
            {
                std::printf("%s: %s: the notes differ after the %s edits\n", name.c_str(), info.name.c_str(),
                            NameOf(setting));
                ++totals.note_sounds;
            }

            // Each track's settings.
            for (const Track& old : was_read.tracks)
            {
                const Track* track = TrackNumbered(now_read, old.number);
                if (track == nullptr)
                {
                    std::printf("%s: %s: track %d plays no notes after the %s edits\n", name.c_str(), info.name.c_str(),
                                old.number, NameOf(setting));
                    ++totals.differ;
                    continue;
                }

                ++totals.tracks;
                for (const Mix shown : kSettings)
                {
                    const std::vector<Setting>& was = hokuto::song::MixSettings(old, shown);
                    const std::vector<Setting>& now = hokuto::song::MixSettings(*track, shown);
                    if (shown != setting)
                    {
                        if (now != was)
                        {
                            std::printf("%s: %s: track %d's %s changed with a %s edit. Before: %s. After: %s.\n",
                                        name.c_str(), info.name.c_str(), old.number, NameOf(shown), NameOf(setting),
                                        Text(was).c_str(), Text(now).c_str());
                            ++totals.differ;
                        }
                        continue;
                    }

                    // The edited commands that the track runs, and whether the new command opens it.
                    const std::optional<Run> run = RunOf(*file.sequences[*sequence], offsets[*sequence], old, setting);
                    if (!run)
                    {
                        std::printf(
                            "%s: %s: track %d ran commands that aren't the sequence's items before the %s edits\n",
                            name.c_str(), info.name.c_str(), old.number, NameOf(setting));
                        ++totals.differ;
                        continue;
                    }

                    std::size_t edited_items = 0;
                    for (const uint32_t item : run->items)
                    {
                        edited_items += plan.run.items.contains(item) ? 1 : 0;
                    }
                    const bool opened =
                        plan.opening && old.number == plan.track && !old.stretches.empty() &&
                        old.stretches.front().entry == ntdlib::sequence_export::Entry::kOpen &&
                        hokuto::edit::ItemAtOffset(offsets[*sequence], old.stretches.front().first) == plan.opening;

                    // A track that runs from the item before the new command's place into the place goes past the new
                    // command.
                    bool runs_past = false;
                    for (const Stretch& stretch : old.stretches)
                    {
                        const std::vector<uint32_t>& at = offsets[*sequence];
                        const std::optional<uint32_t> first_item = hokuto::edit::ItemAtOffset(at, stretch.first);
                        const std::optional<uint32_t> last_item = hokuto::edit::ItemAtOffset(at, stretch.last);
                        runs_past = runs_past || (plan.opening && first_item && last_item &&
                                                  *first_item < *plan.opening && *last_item >= *plan.opening);
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
                    const std::optional<int> opening =
                        opened ? std::optional<int>(Mapped(plan, hokuto::song::MixDefault(setting))) : std::nullopt;
                    const std::vector<Setting> expected = Before(
                        Expected(was, setting, opening, old.stretches.empty() ? 0 : old.stretches.front().tick, map),
                        end);
                    totals.mapped += mapped ? 1 : 0;
                    totals.opened += opened ? 1 : 0;
                    if (now == expected)
                    {
                        continue;
                    }

                    // A command that sets the default shows no change before the edit. Once the edit maps the default,
                    // the command shows a change in its tick, before any other.
                    const int mapped_default = Mapped(plan, hokuto::song::MixDefault(setting));
                    if (mapped && !opened && run->again && !now.empty() && now.front().value == mapped_default &&
                        (was.empty() || now.front().tick <= was.front().tick) &&
                        now == Before(Expected(was, setting, mapped_default, now.front().tick, map), end))
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
                            run->ramp ? "moves over time" : "takes random values", plan.track, plan.edit.from,
                            plan.edit.to, Text(was).c_str(), Text(expected).c_str(), Text(now).c_str());
                        ++totals.moving;
                        continue;
                    }

                    std::printf(
                        "%s: %s: track %d's %s differs after the edit of track %d from %d to %d%s. Before: %s. "
                        "Expected: %s. After: %s.\n",
                        name.c_str(), info.name.c_str(), old.number, NameOf(setting), plan.track, plan.edit.from,
                        plan.edit.to, opened ? ". A new command opens it" : "", Text(was).c_str(),
                        Text(expected).c_str(), Text(now).c_str());
                    ++totals.differ;
                }
            }
        }
    }

    for (uint32_t sequence = 0; sequence < file.sequences.size(); ++sequence)
    {
        totals.timelines += hokuto::edit::IsTimeline(file, sequence) ? 1 : 0;
    }
    const std::string timeline_line =
        timelines ? " " + std::to_string(totals.timelines) + " sequences are timelines. They took " +
                        std::to_string(totals.timeline_edits) + " edits, and refused " +
                        std::to_string(totals.timeline_refused) + ". " + std::to_string(totals.saved) +
                        " sounds play otherwise from the saved archive."
                  : "";
    std::printf(
        "%s: %zu sequence sounds. %zu edits in 5 rounds, %zu of them refused, and %zu with a new command, %zu of "
        "those after a jump. %zu edits differ from what the tool worked out. %zu tracks checked: %zu settings mapped, "
        "%zu of them with a command that set the default, %zu opened by a new command, %zu that run on past one, and "
        "%zu that differ. %zu sounds whose notes differ, %zu whose loop or length moves otherwise, and %zu that can't "
        "be read. Apart: %zu tracks that run edited and other commands, %zu that move or take random values and "
        "differ, %zu sounds with the loop a whole number of passes away, and %zu without a loop that end elsewhere. "
        "The reaches list %zu tracks and %zu sounds. %zu of the tracks and %zu of the sounds changed. %zu tracks and "
        "sounds changed outside the reach. The slowest reach took %.1f ms.%s\n",
        name.c_str(), totals.sounds, totals.edits, totals.refused, totals.new_commands, totals.jumps,
        totals.wrong_edits, totals.tracks, totals.mapped, totals.again, totals.opened, totals.runs_past, totals.differ,
        totals.note_sounds, totals.moved_loops, totals.unread, totals.mixed, totals.moving, totals.other_passes,
        totals.other_ends, totals.listed_tracks, totals.listed_sounds, totals.reached_tracks, totals.reached_sounds,
        totals.unlisted, totals.slowest_reach * 1000.0, timeline_line.c_str());
    hokuto::tools::PrintSaves(name);

    const bool differ = totals.wrong_edits != 0 || totals.differ != 0 || totals.note_sounds != 0 || totals.saved != 0 ||
                        totals.moved_loops != 0;
    return !differ && totals.unread == 0 && totals.unlisted == 0 && !failed ? 0 : 1;
}
