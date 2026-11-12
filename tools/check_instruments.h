// SPDX-License-Identifier: MIT

// The parts of hokuto_check_instruments that its files share: the totals, the archive, the comparison of notes and
// the checks of the commands. check_instruments.cpp describes the checks.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
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
#include "ntdlib/mp2k/song.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_support.h"
#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/bytecode.h"
#include "edit/drivers.h"
#include "edit/instruments.h"
#include "edit/reach.h"
#include "edit/replace.h"
#include "format/file.h"
#include "loop_passes.h"
#include "notes_apart.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/gba.h"
#include "song/sequence.h"

namespace hokuto::tools::instruments
{

namespace edit = hokuto::edit;
namespace song = hokuto::song;
using hokuto::format::File;
using hokuto::tools::BusiestTrack;
using hokuto::tools::CheckedArchive;
using hokuto::tools::EarlierEnd;
using hokuto::tools::SpreadIndices;
using hokuto::tools::TrackNumbered;

// The edits: another program of the bank, and another bank.
constexpr std::size_t kEdits = 2;
constexpr std::array<const char*, kEdits> kEditNames = {"program", "bank"};

// What the check counted, for the summary.
struct Totals
{
    std::size_t sequences = 0;
    std::size_t sounds = 0;
    std::size_t unreadable = 0;
    std::size_t taking_part = 0;
    std::array<std::size_t, kEdits> edits{};
    std::array<std::size_t, kEdits> without{};
    std::array<std::size_t, kEdits> fewer{};
    std::array<std::size_t, kEdits> more{};
    std::array<std::size_t, kEdits> notes{};
    std::array<std::size_t, kEdits> lengths{};
    std::array<std::size_t, kEdits> later{};
    std::array<std::size_t, kEdits> follow{};
    std::size_t sharing = 0;
    std::size_t reads = 0;
    std::size_t loops_apart = 0;
    std::size_t problems = 0;
    std::map<std::string, std::size_t> refusals;
    std::size_t listed_tracks = 0;
    std::size_t listed_sounds = 0;
    std::size_t reached_tracks = 0;
    std::size_t reached_sounds = 0;
    std::size_t unlisted = 0;
    double slowest_reach = 0.0;  // seconds
    std::size_t outgrown = 0;    // edits whose GBA songs outgrow their places (tools::Outgrows)
    std::size_t channels = 0;    // GBA tracks that gain or lose notes on the edited notes' channels
    std::size_t ds_channels = 0; // DS tracks that gain or lose notes that the driver never started
};

// The archive as the tool reads it: its sounds, where each plays in the file, each sequence sound as ntdlib's performer
// read it, the sounds of each sequence file, the programs of the banks that the tool has read, and what an edit of each
// track of the sounds that took part reaches.
struct Archive
{
    std::string name;
    CheckedArchive archive;
    File file;
    std::vector<std::optional<edit::SoundPlace>> places;
    std::map<uint32_t, song::Sequence> songs;
    std::map<uint32_t, std::vector<uint32_t>> by_sequence;
    std::map<uint32_t, std::vector<uint32_t>> programs; // by bank, or by voice group on the GBA
    bool gba_groups_read = false;
    std::map<std::pair<uint32_t, int>, std::vector<uint32_t>> slot_programs; // on another of the GBA's drivers

    std::map<uint32_t, std::vector<edit::TrackReach>> reaches;
};

// A note as the tool compares it: its start, key, velocity, bank slot and program.
using PlayedNote = std::tuple<uint32_t, uint8_t, uint8_t, int, int>;

// A track of a sound as the tool compares it, before and after the edit: the notes that it played, the notes that it
// has to play, the notes that it plays, and how many notes at the start match. The edited track has to play the new
// instrument in place of the old one.
struct Comparison
{
    const song::Track* before = nullptr;
    const song::Track* after = nullptr; // nullptr where the track plays no more
    bool edited = false;
    std::size_t mapped = 0;
    std::vector<PlayedNote> expected;
    std::vector<PlayedNote> played;
    std::size_t same = 0;
    bool channels = false;    // on the GBA, the notes differ only on the channels of the edited notes (ChannelsDecide)
    bool ds_channels = false; // on the DS, the notes differ only by notes that the driver never started
};

// Whether `list` holds `value`.
template <typename T>
bool Listed(const std::vector<T>& list, T value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

// The items of `sequence` that a track ran in `stretches`.
std::set<uint32_t> ItemsRun(const ntdlib::commands::Sequence& sequence, const song::Console& console,
                            const std::vector<song::Stretch>& stretches);

// The notes of `track` as the tool compares them.
std::vector<PlayedNote> NotesOf(const song::Track& track);

// The number of notes of `before` that end elsewhere in `after`. Both tracks play the same notes.
std::size_t LengthsChanged(const song::Track& before, const song::Track& after);

// Whether `fewer` is `more` without some of its notes, each of which plays `to`.
bool WithoutNotesOf(const std::vector<PlayedNote>& fewer, const std::vector<PlayedNote>& more, const edit::Program& to);

// On the GBA, the channels that the notes of `track` in GBA song `song` play on with `from`, and would play on with
// `to`: a PSG channel, 1 to 4, or 0 for a DirectSound sample. The address of the voice group is stored in `voices`.
// Nothing on another console.
std::set<int> GbaChannelsOf(const Archive& archive, uint32_t song, const song::Track& track, const edit::Program& from,
                            const edit::Program& to, uint32_t& voices);

// Whether the notes of `comparison` differ only by notes that it gains or loses on `channels` (GbaChannelsOf), with
// the voice group at `voices`. A note that it loses can't have a note that it gains at the same tick and key: such a
// note plays otherwise.
bool ChannelsDecide(const Archive& archive, uint32_t voices, const std::set<int>& channels,
                    const Comparison& comparison);

// On the DS, whether the notes of `comparison` differ only by notes that one read lists among the notes that the driver
// never started (song::Track::unstarted): each note that the track loses in the edit, and each note that it gains.
bool DsChannelsDecide(const Comparison& comparison);

// Whether `expected` and `played` differ only in notes that play `to`. The new instrument can have no region for some
// keys of a track, and a region for keys that the old instrument left out.
bool DifferOnlyInNotesOf(std::vector<PlayedNote> expected, std::vector<PlayedNote> played, const edit::Program& to);

// The first tick where a note of `comparison` that matches ends elsewhere: the earlier of its two ends. Nothing where
// none does.
std::optional<uint32_t> MovedEnd(const Comparison& comparison);

// The tick of the first note that differs in `comparison`, on either side.
uint32_t DifferenceTick(const Comparison& comparison);

// How `played` differs from `expected`: their counts, and the first note that differs.
std::string Difference(const std::vector<PlayedNote>& expected, const std::vector<PlayedNote>& played);

// The tracks of `read`, a read of a sound of `sequence`, that take the settings of a track that runs an item of
// `changed`, or of track `edited`, without being such a track: those that such a track starts, and those that they
// start. Nothing where a started track starts with settings of its own (edit::DriverEdits::StartedTracksTakeSettings).
std::set<int> TakingTracks(const ntdlib::commands::Sequence& sequence, const song::Console& console,
                           const song::Sequence& read, const std::set<uint32_t>& changed, std::optional<int> edited);

// Whether a track of `song`, a sound that plays `sequence`, ran a command after which the instrument determines which
// notes play and when: a tie or monophonic play turned on, or a note of length 0 or of a length that the sequence sets
// with the note wait on.
bool FollowsInstrument(const ntdlib::commands::Sequence& sequence, const song::Console& console,
                       const song::Sequence& song);

// The items of `before` that `edited` changed, by their indices before the edit: those that it removed, and those whose
// commands differ once their targets follow the items. A command that the edit put in before an item counts that item
// as changed.
std::set<uint32_t> ChangedItems(const ntdlib::commands::Sequence& before, const ntdlib::commands::Sequence& after,
                                const edit::Edited& edited);

// The first change that `edited` made to `before` other than a new program or slot from `to` in a program change or a
// bank select. A new command can be a program change, a bank select or a jump. A command that led to an item can lead
// to the new commands just before it. Empty where there's none.
std::string OtherChange(const ntdlib::commands::Sequence& before, const ntdlib::commands::Sequence& after,
                        const edit::Edited& edited, const edit::Program& to);

} // namespace hokuto::tools::instruments
