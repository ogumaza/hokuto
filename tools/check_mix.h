// SPDX-License-Identifier: MIT

// The parts of hokuto_check_mix that its files share: the settings, the totals, the plans of the edits, the notes as
// the comparisons take them, and the helpers. check_mix.cpp describes the checks.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <iterator>
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

#include "check_support.h"
#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/bytecode.h"
#include "edit/krawall.h"
#include "edit/mix.h"
#include "edit/quintet.h"
#include "edit/reach.h"
#include "edit/replace.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/engine.h"
#include "format/file.h"
#include "loop_passes.h"
#include "notes_apart.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/gba.h"
#include "song/sequence.h"

namespace hokuto::tools::mix
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
using hokuto::tools::LoopText;
using hokuto::tools::NotesApart;
using hokuto::tools::TrackNumbered;
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
    std::size_t past_end = 0; // DSE's tracks that run past their chunks
    std::size_t differ = 0;
    std::size_t note_sounds = 0;
    std::size_t psg_sounds = 0;
    std::size_t channel_sounds = 0; // on the DS, sounds whose notes take other channels after a volume edit
    std::size_t outgrown = 0;       // songs whose edits a GBA ROM without free space has no room for
    std::size_t roomless = 0;       // songs of the GBA's other drivers whose edits the ROM has no room for
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
const char* NameOf(Mix setting);

// The items of a sequence that set a setting on a track. `early` is set when a note ran before the first of them, or
// when there are none.
struct Run
{
    std::set<uint32_t> items;
    bool early = false;
    bool random = false;        // a command takes a random value
    bool variable = false;      // a command takes a variable's value
    bool ramp = false;          // a command moves the setting over time
    bool again = false;         // a command sets the default value
    bool moves = false;         // on DSE, a slide or a change moves the setting
    bool changes = false;       // on DSE, a change moves the setting by an amount
    std::set<int> targets;      // on DSE, the targets of the slides
    std::set<uint32_t> slides;  // on DSE, the items of the slides
    std::set<uint32_t> amounts; // on DSE, the items of the changes
};

// The value that `setting` starts with where no command has set it, on the GBA with `gba`, on DSE with `dse`, and on
// another of the GBA's drivers with `driver`. Each of those drivers starts a track at a volume that its rules give.
int DefaultOf(Mix setting, bool gba, bool dse, const hokuto::song::DriverRules* driver);

// The run of `setting` on `track`, or nothing when a stretch isn't made of the sequence's items.
std::optional<Run> RunOf(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& offsets,
                         const Track& track, Mix setting, bool gba, bool dse, const hokuto::song::DriverRules* driver);

// The notes of a sound of Quintet's driver on the tracks in `changed`. The level of a channel is its volume times the
// step of its envelope, and ntdlib's conversion leaves out the frames where the level is 0. A note of a track whose
// volume changed can therefore start later, end sooner or fall silent.
NotesApart QuintetNotesApart(const std::set<int>& changed);

// Whether a stretch of `track` starts or ends inside the bytes that `sequence` stores as data, or past its items: a DSE
// track that has no end runs past its chunk there, and RunOf finds no run.
bool RunsIntoData(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& offsets, const Track& track);

// The edit of one track of a sequence file in a round, and what the tool worked out about it.
struct Plan
{
    uint32_t sound = 0; // the sound that the edit came through
    int track = 0;      // the track's number
    std::vector<Stretch> stretches;
    MixEdit edit;
    Run run;
    bool gba = false;                                  // the sound is a GBA song
    bool dse = false;                                  // the sound is DSE's
    const hokuto::song::DriverRules* driver = nullptr; // the rules of another of the GBA's drivers that plays the sound
    bool trim = false;                  // the map scales or shifts the values, instead of setting one value
    std::optional<std::string> refusal; // why the sequence refuses the edit
    std::optional<uint32_t> opening;    // the item before which a new command goes
    bool jump = false;                  // a jump before the new command takes the item before it past it
    int channel = -1;                   // on DSE, the channel of the track: its other tracks show the settings too
    Reach reach;                        // the tracks and sounds that the mixer lists for the edit
};

// The value that `plan` maps `value` to.
int Mapped(const Plan& plan, int value);

// The plan for `setting` on `track` of the sound `sound` of `file`. The sound plays sequence `index`.
std::optional<Plan> PlanFor(const File& file, uint32_t index, const std::vector<uint32_t>& offsets, uint32_t sound,
                            const Track& track, Mix setting);

// Whether `edited`, the sequence after `plan`, holds the plan's edit: each command of the run with its mapped value,
// and the new command where the plan put one. Every other item stays as it was, but for the targets.
bool HoldsEdit(const ntdlib::commands::Sequence& before, const ntdlib::commands::Sequence& edited, const Plan& plan);

// A note as the check compares it: its track, start, key, length and velocity.
using Played = std::tuple<int, uint32_t, uint32_t, uint32_t, uint32_t>;

using hokuto::tools::PsgNotesApart;

// The notes of `sequence`, in order, for the check's comparisons.
std::vector<Played> NotesOf(const Sequence& sequence, const NotesApart& apart = {});

// Whether `a` and `b` play the same notes, with the same loop and length, and give every track the same settings.
bool SamePlaying(const Sequence& a, const Sequence& b);

// `settings` of a DSE track's volume as they would show against the driver's starting volume. song::ReadSequence
// compares a channel's volume with 127, and shows the starting volume at tick 0. No command has to set it. A volume
// that shows nothing at tick 0 is 127, and a command set it.
std::vector<Setting> AgainstDseStart(std::vector<Setting> settings);

// The settings that a track should show after an edit: those of `before`, each value passed through `map`, after the
// value `opening` at tick `tick` where a new command opens the track. A change in the same tick as the one before
// replaces it, and a value that doesn't change shows no change. With `gba`, on the GBA and its other drivers, a track's
// volume starts at the default at tick 0. song::ReadSequence sets the same default at tick 0. The default at tick 0
// before the track's first command then maps to nothing: no command sets it. A command that sets the default there sets
// it again (Run::again). `fallback` is the setting's value where no command has set it: the default.
template <typename Map>
std::vector<Setting> Expected(std::vector<Setting> before, Mix setting, std::optional<int> opening, uint32_t tick,
                              Map map, bool gba, int fallback)
{
    const bool gba_volume = gba && setting == Mix::kVolume;
    if (gba_volume && !before.empty() && before.front().tick == 0 && before.front().value == fallback)
    {
        before.erase(before.begin());
    }
    std::vector<Setting> expected;
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
    if (gba_volume && (expected.empty() || expected.front().tick > 0))
    {
        expected.insert(expected.begin(), {0, fallback});
    }

    return expected;
}

// Settings in a line about them, the first few of them.
std::string Text(const std::vector<Setting>& settings);

// Whether the notes of `a` and `b` differ only where the DS's channels decide them (ChannelsDecide).
bool ChannelsDecideAll(const Sequence& a, const Sequence& b);

// Prints up to four notes that only `before` plays and up to four that only `after` plays.
void PrintNotesOnlyIn(const std::string& name, const Sequence& before, const Sequence& after);

// The numbers of the tracks of `before` whose setting `setting` changed in `after`, and of those that play no more.
std::set<int> ChangedTracks(const Sequence& before, const Sequence& after, Mix setting);

// The settings of `settings` that come before tick `end`.
std::vector<Setting> Before(std::vector<Setting> settings, uint32_t end);

// `sequence` up to tick `end`: the notes that start before it, each cut at it, and the settings that come before it.
Sequence Before(Sequence sequence, uint32_t end);

// The number of notes of `sequence` that start at or after tick `end`.
std::size_t NotesFrom(const Sequence& sequence, uint32_t end);

// The tracks of `sequence` that play notes.
std::vector<const Track*> Playing(const Sequence& sequence);

// The sequence file that sound `info` plays, from `sequences`, the sequence of each file, if any.
std::optional<uint32_t> SequenceOf(const std::vector<std::optional<uint32_t>>& sequences, const CheckedSound& info);

// Checks sound `sound` after the edits of a round: its loop and its length, the reach of the edit, its notes and the
// settings of each track. `after` is the archive that the program plays after the edits, and `saved` the archive that a
// saved file contains.
void CheckSoundAgain(uint32_t sound, const CheckedArchive& archive, const File& file,
                     const std::vector<std::optional<uint32_t>>& sequences,
                     const std::vector<std::optional<Sequence>>& before, const std::map<uint32_t, Plan>& plans,
                     const CheckedArchive& saved, const CheckedArchive& after, bool timelines,
                     const std::map<uint32_t, std::vector<uint32_t>>& offsets, Mix setting, const std::string& name,
                     Totals& totals);

} // namespace hokuto::tools::mix
