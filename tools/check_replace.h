// SPDX-License-Identifier: MIT

// The parts of hokuto_check_replace that its files share: the totals, the refusals of the Wii's checks, the helpers and
// the checks of each mode. check_replace.cpp describes the modes.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "audio/resampler.h"
#include "check_support.h"
#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/banks.h"
#include "edit/bytecode.h"
#include "edit/files.h"
#include "edit/groups.h"
#include "edit/new_items.h"
#include "edit/other_console.h"
#include "edit/replace.h"
#include "edit/streams.h"
#include "edit/tables.h"
#include "edit/variables.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/biquads.h"
#include "song/dry_render.h"
#include "song/items.h"
#include "song/preview.h"
#include "song/sequence.h"
#include "song/sequence_model.h"
#include "song/streams.h"

namespace hokuto::tools::replace
{

namespace audio = hokuto::audio;
namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;

using Archive = std::shared_ptr<const sar::SoundArchive>;

// What the check counted.
struct Totals
{
    std::size_t sounds = 0;
    std::size_t failed = 0;
    std::size_t in_bytes = 0;
    std::size_t unlabelled = 0;
    std::size_t other_unreached = 0;
    std::size_t other_biquads = 0; // files for another console whose biquad filters come back otherwise
    std::size_t ctr_refused = 0;   // files that reach a command that the 3DS lacks
    std::size_t crossed = 0;
    std::size_t untracked = 0;
    std::size_t loops_moved = 0; // replacements whose loop the performer found a whole number of passes away
    std::size_t files = 0;
    std::size_t files_failed = 0;
    std::size_t files_crossed = 0;
    std::size_t files_shared = 0;
    std::size_t files_outside = 0; // in neither the archive nor its group files
    std::size_t files_beside = 0;  // in the group files beside the archive alone
    std::size_t files_other_copies = 0;
    std::size_t files_placed = 0;              // files with copies in groups that name their waves at other places
    std::size_t files_placed_copies = 0;       // such copies that took another file with their places
    std::size_t files_converted = 0;           // files that took the same file from another console
    std::size_t files_converted_otherwise = 0; // wave archives whose waves came back from it with other bytes
    std::size_t files_written = 0; // other files of the kind that went in written again with the archive's versions
    std::size_t wave_archives = 0;
    std::size_t wave_archives_failed = 0;
    std::size_t wave_archives_out = 0;    // in neither the archive nor its group files, or shared by two file IDs
    std::size_t wave_archives_beside = 0; // in the group files beside the archive alone
    std::size_t waves = 0;
    std::size_t waves_wide = 0;        // waves of more than two channels
    std::size_t waves_loop_lost = 0;   // waves whose loop flag the WAV file doesn't keep
    std::size_t waves_later = 0;       // looped DSP-ADPCM waves whose later passes differ from the first
    std::size_t waves_rewritten = 0;   // waves whose files give their wave archives other bytes in their places
    std::size_t waves_other_bytes = 0; // waves that come back with other bytes from another console
    int later_peak = 0;                // the largest difference of a sample there
    double later_ms = 0.0;             // the longest time from the loop's start to the last difference

    // The waves from their WAV files as DSP-ADPCM.
    std::size_t adpcm_exact = 0;      // waves that come back exactly
    std::size_t adpcm_moved = 0;      // waves whose loop's start moves forward to a frame
    std::vector<double> adpcm_ratios; // the signal-to-noise ratios of the others, in decibels
    std::size_t adpcm_samples = 0;    // the samples that the encoder took, in all channels
    double adpcm_seconds = 0.0;       // the time that the reading of the WAV files took

    // The waves from their WAV files at half their rates, and without their loops.
    std::size_t resampled = 0;         // waves that came back at half their rates
    std::size_t resampled_low = 0;     // waves that keep their rates of 8,000 Hz or less
    std::size_t resampled_samples = 0; // the new samples, in all channels
    double resampled_seconds = 0.0;    // the time that the reading of the WAV files took
    std::size_t loops_offered = 0;     // looped waves that took the present wave's loop

    // The programs that the sounds of a bank select and the next bank lacks.
    std::size_t fixed_banks = 0;    // banks whose next bank lacks such programs
    std::size_t fixed_programs = 0; // the programs
    std::size_t fixes_present = 0;  // programs that took the present bank's instrument
    std::size_t fixes_next = 0;     // programs that took the next bank's first instrument
    std::size_t selected = 0;       // the programs that the sounds of the banks select
    std::size_t undecided = 0;      // program changes of those sounds whose program or slot a value decides

    // The new sounds of --new.
    std::size_t new_sounds = 0;
    std::size_t new_failed = 0;
    std::size_t new_crossed = 0;         // the same new sounds from the files for the other consoles
    std::size_t new_pairs = 0;           // new sounds after another new sound
    std::size_t new_no_start = 0;        // models that start where no command of their files starts
    std::size_t new_untracked = 0;       // models whose commands open tracks that they don't allocate
    std::size_t new_other_unreached = 0; // files for another console that decode otherwise
    std::size_t new_other_biquads = 0;   // files for another console whose biquad filters come back otherwise
    std::size_t new_ctr_refused = 0;     // files that reach a command that the 3DS lacks
    double new_slowest = 0.0;            // the longest time that the edit took, in seconds

    // The new banks and wave archives of --banks.
    std::size_t bank_checked = 0;
    std::size_t bank_failed = 0;
    std::size_t new_banks = 0;
    std::size_t new_wave_archives = 0;
    std::size_t bank_silent = 0;  // new sounds whose 10 s hold no sample above 0
    std::size_t bankless = 0;     // models without a bank in their slots
    std::size_t bank_missing = 0; // models whose banks or wave archives lack their files
    std::size_t bank_skipped = 0; // models that start where no command starts, or that leave out tracks
    double bank_slowest = 0.0;    // the longest time that an edit took, in seconds

    // The new wave sounds of --wave-sounds.
    std::size_t wsd_files = 0; // wave sound files that went in as new wave sounds, once for each model
    std::size_t wsd_failed = 0;
    std::size_t wsd_sounds = 0;         // new wave sounds
    std::size_t wsd_from_sequences = 0; // new wave sounds that copied a sequence sound
    std::size_t wsd_compared = 0;       // new wave sounds that played the donor's samples
    std::size_t wsd_undecoded = 0;      // new wave sounds whose samples neither side decodes
    std::size_t wsd_unplayed = 0;       // new wave sounds of wave sounds that no sound of the donor plays
    std::size_t wsd_missing = 0;        // files whose wave archives lack their files
    std::size_t wsd_wave_archives = 0;  // new wave archives
    std::size_t wsd_written = 0;        // wave archives and wave sound files written again with the archive's versions
    double wsd_slowest = 0.0;           // the longest time that an edit took, in seconds

    // The new waves of --new-waves.
    std::size_t nw_wave_archives = 0; // wave archives whose waves went in again as new waves
    std::size_t nw_failed = 0;
    std::size_t nw_waves = 0;          // waves that went in from each of their files
    std::size_t nw_empty = 0;          // wave archives without waves: they took the archive's first wave
    std::size_t nw_waveless = 0;       // wave archives without waves in archives without waves
    std::size_t nw_other_bytes = 0;    // waves whose file for another console came back with other bytes
    std::size_t nw_other_versions = 0; // waves of another version than a new wave of their wave archive takes
    std::size_t nw_written = 0;        // waves of a later version that went in written again with that version
    std::size_t nw_pairs = 0;          // second new waves after a first
    double nw_slowest = 0.0;           // the longest time that an edit took, in seconds

    // The streams of --streams.
    std::size_t st_streams = 0; // streams beside the archive that a sound plays
    std::size_t st_failed = 0;
    std::size_t st_channels = 0;
    std::size_t st_unplayed = 0;       // streams beside the archive that no sound plays
    std::size_t st_aac = 0;            // AAC streams of the 3DS beside a donor: File > Import > Stream… refuses them
    std::size_t st_aac_replaced = 0;   // AAC streams beside the archive that took their files and were encoded again
    std::size_t st_aac_missing = 0;    // AAC streams that the file lacks: each took another stream's WAV file
    std::vector<double> st_aac_ratios; // the signal-to-noise ratios of the encoded AAC streams, in decibels
    uint32_t st_aac_delay = 0;         // the encoder's delay, in samples
    std::size_t st_moved = 0;          // streams whose loop's start moves forward to a block of 16-bit PCM
    std::size_t st_written = 0;        // streams of the Wii U or the 3DS that went in written again in an older version
    std::size_t st_tracked = 0;        // streams whose sounds play the tracks of their stream files
    std::size_t st_same_details = 0;   // new sounds whose details hold the model's
    std::size_t st_exact = 0;          // streams that come back exactly as DSP-ADPCM
    std::vector<double> st_ratios;     // the signal-to-noise ratios of the others, in decibels
    std::size_t st_pairs = 0;          // second new sounds after a first
    std::size_t st_nameless = 0;       // streams of an archive without names: such an archive takes no new sound
    std::size_t st_unknown = 0;        // donors' streams for a 3DS archive whose layout of stream sounds is unknown
    std::size_t st_paired = 0;         // donors' streams that took a track for each pair of their channels
    double st_slowest = 0.0;           // the longest time that a new sound's edit took, in seconds

    // The variables of --variables.
    std::size_t va_reading = 0;  // sequence sounds whose commands read variables before they set them
    std::size_t va_checked = 0;  // such sounds that the rounds edited
    std::size_t va_failed = 0;   // edits after which a sound plays otherwise
    std::size_t va_refused = 0;  // edits that the variables' map or the replacement refused
    std::size_t va_moved = 0;    // variables that a move gave another number
    std::size_t va_fixed = 0;    // variables that took a fixed value
    std::size_t va_held = 0;     // fixed variables that another of the sound's variables holds
    std::size_t va_runs = 0;     // reads of an edited sound with starting values, compared with a read before the edit
    std::size_t va_no_start = 0; // sounds that start where no command of their files starts
    std::size_t va_runaway = 0;  // fixed values that left a track running without waits, and played otherwise
    std::set<std::string> va_reasons; // why the edits were refused

    // The groups that new and replaced items join.
    std::size_t join_items = 0; // items, or sets of new items, that joined groups
    std::size_t join_failed = 0;
    std::size_t join_refused = 0;    // joins that the edit refuses, with a message that the window shows
    std::size_t join_unproposed = 0; // items for which the windows propose no group
    std::size_t join_unreadable = 0; // groups that load the item and would take a file that Hokuto can't read
    std::size_t join_groups = 0;     // groups that took items
    std::size_t join_loading = 0;    // groups that loaded the old item or the model
    std::size_t join_beside = 0;     // groups beside the archive
    std::size_t join_copies = 0;     // files that the groups took as copies
    std::size_t join_links = 0;      // files that the groups took as links
    std::size_t join_listed = 0;     // items that the groups listed
    std::size_t join_partial = 0;    // items that the groups listed with some of their parts
    std::size_t join_unlisting = 0;  // groups whose files hold no INFX block, and list no items
    double join_slowest = 0.0;       // the longest time that a join took, in seconds

    // The new groups that the first item of each check joins.
    std::size_t made_groups = 0; // new groups that took items
    std::size_t made_failed = 0;
    std::size_t made_refused = 0;   // new groups that the edit refuses, with a message that the window shows
    std::size_t made_copies = 0;    // files that the new groups took as copies
    std::size_t made_links = 0;     // files that the new groups took as links
    std::size_t made_listed = 0;    // items that the new groups listed
    std::size_t made_unlisting = 0; // new groups whose files hold no INFX block, and list no items
};

// A group of an archive: its file, and the files that it loads.
struct Group
{
    uint32_t file_id = 0;
    std::vector<uint32_t> loads;
};

// The groups of the archive in `bytes`. The program reads a Wii U archive past damage, and ntdlib's reader then lists
// no groups. The groups therefore come from a strict read.
std::vector<Group> GroupsOf(std::span<const uint8_t> bytes, sar::Flavor flavor);

// The type of reference from a file's entry to its place in the FILE block.
constexpr uint16_t kInternalFile = 0x220C;

// The most items of an archive that join groups in each check.
constexpr std::size_t kJoins = 8;

// ntdlib's reading of the archive that a Hokuto file builds, with the group files beside it, when the reader stops at
// the first problem. Each group's contents follow: nothing for a group whose file the Hokuto file lacks or ntdlib can't
// read. Then whether each group's file holds an INFX block: the list of the group's items.
struct StrictRead
{
    std::vector<uint8_t> bytes;
    std::vector<std::vector<uint8_t>> group_files;
    std::optional<sar::SoundArchive> archive;
    std::vector<std::optional<sar::GroupContents>> groups;
    std::vector<bool> listing;
};

// The archive that `file` builds, read strictly with its group files (StrictRead).
StrictRead ReadStrictly(const format::File& file);

// The list of groups in each file's entry of the archive in `bytes`, an archive of `flavor`, by file ID. The entry of a
// file that the FILE block doesn't hold has no list. The 3DS's file table holds no lists, and gives none.
std::vector<std::optional<std::vector<uint32_t>>> ListsOfGroups(std::span<const uint8_t> bytes, sar::Flavor flavor);

// An item ID in hexadecimal.
std::string HexOf(uint32_t id);

// What is wrong with group file `now` of `flavor` after file `file_id` took `bytes` in the place of `present`, or
// nothing. The group file was `was` before. Each copy of the file has to hold what CopyProblem requires, and each other
// copy its bytes. The group file has to keep its files and items. `name` names the group file in the message.
std::string GroupFileProblem(std::span<const uint8_t> was, std::span<const uint8_t> now, sar::Flavor flavor,
                             uint32_t file_id, std::span<const uint8_t> present, std::span<const uint8_t> bytes,
                             const std::string& name, std::size_t& placed);

// What is wrong with the group files beside the archive of `replaced`, the version of a file whose archive `built` was
// after file `file_id` took `bytes`, or nothing (GroupFileProblem). Copies that name the file's waves at other places
// count in `placed`.
std::string GroupFilesProblem(const sar::SoundArchive& built, const format::File& replaced, uint32_t file_id,
                              std::span<const uint8_t> bytes, std::size_t& placed);

// The groups that join for an item, and the name of the group among them that the windows don't propose, if any.
struct Choice
{
    std::vector<uint32_t> groups;
    std::string extra;
};

// The groups that join for a new or replaced item: those that the windows propose (edit::GroupOptions) for an item that
// requires the files `after` in the place of an item, or a model, that required `before`. With `extra`, a group that
// the windows don't propose joins too: one beside the archive where `file` holds one. `groups` are the groups of
// `built`, the archive that `file` builds. Counts the items without a proposal in `totals`, and the groups that the
// windows don't offer because they would take a file that Hokuto can't read.
Choice ChosenGroups(const format::File& file, const sar::SoundArchive& built, std::span<const edit::Group> groups,
                    std::span<const uint32_t> before, std::span<const uint32_t> after, bool extra, Totals& totals);

// What is wrong when `items` of `added` join the groups of `choice` through the windows' edit, or nothing. `before`
// lists the files that the old item or the model required. Counts the join in `totals`.
std::string JoinCheck(const format::File& added, const Choice& choice, std::span<const uint32_t> items,
                      std::span<const uint32_t> before, Totals& totals);

// What is wrong when `items` of `added` join a new group through the windows' edit, or nothing. `archive` is the
// archive that `added` started from, and `before` lists the files that the old item or the model required. The new
// group goes in first, empty (edit::AddGroups). It then takes the items as any group without files does: links to the
// files that the items require, or copies when a link can't reach one of them. Counts the new group in `totals`.
std::string NewGroupCheck(const format::File& added, const sar::SoundArchive& archive, std::span<const uint32_t> items,
                          std::span<const uint32_t> before, Totals& totals);

// How many items of a check have joined the groups that the windows propose, and whether one has joined a new group.
struct Joins
{
    std::size_t count = 0;
    bool new_group = false;
};

// Whether the next item of a check joins groups: those that the windows propose in `edit_groups`, up to kJoins items,
// or a new group, for the first item.
bool JoinsGroups(const Joins& joins, std::span<const edit::Group> edit_groups);

// What is wrong when the next item of a check joins groups through the windows' edit, or nothing. Up to kJoins items
// join the groups that the windows propose (ChosenGroups, with the files `before` and the files that `after` returns,
// or none when `after` is empty), and the first item joins a new group (NewGroupCheck). `items` are the IDs that the
// item has in the file that `added` returns, `built` is the archive that `file` builds, and `before` lists the files
// that the old item or the model required. `after` runs only when the item joins proposed groups, and `added` only
// when the item joins any group. Counts the joins in `joins` and `totals`.
std::string JoinAndNewGroupCheck(const format::File& file, const sar::SoundArchive& built,
                                 std::span<const edit::Group> edit_groups, std::span<const uint32_t> items,
                                 std::span<const uint32_t> before, const std::function<std::vector<uint32_t>()>& after,
                                 const std::function<const format::File&()>& added, Joins& joins, Totals& totals);

// The summary of the joins, for an archive with groups when `groups` says so, and of the new groups.
void PrintJoins(const std::string& name, const Totals& totals, bool groups);

// A sound's sequence file as Replace… reads it for an archive of `into`, and the item where the sound starts in it, if
// an item starts there.
struct Source
{
    edit::Incoming incoming;
    std::optional<uint32_t> start;
};

// The source that sequence file `bytes` gives for an archive of `into`, for a sound that starts at byte `start_offset`.
Source SourceOf(std::span<const uint8_t> bytes, uint32_t start_offset,
                const std::optional<song::Console>& into = std::nullopt);

// Whether the commands that sound `info` plays from `source` open tracks that the sound doesn't allocate.
bool Untracked(const Source& source, const sar::SoundInfo& info);

// `played` without the places of its commands: the commands' offsets, the stretches, the labels and the calls.
song::Sequence Placeless(song::Sequence played);

// What ntdlib's performer plays of sound `sound` of `archive`, with the variables' starting values `values`, without
// the places of its commands.
song::Sequence Played(const sar::SoundArchive& archive, uint32_t sound, const song::StartValues& values = {});

// Whether the sequence file of sound `sound` of `archive` has a label at the sound's start.
bool StartsAtLabel(const sar::SoundArchive& archive, uint32_t sound);

// Whether `after` plays the notes of `before` where ntdlib's performer finds their loops a whole number of passes
// apart. The performer finds a loop where every track's state repeats. Up to the earlier loop's end, the notes then
// have to start as before, with their keys, velocities and instruments.
bool SameUpToLoop(const song::Sequence& before, const song::Sequence& after);

// What is wrong when sound `sound` of `file` takes sound `other`'s sequence file, from `source`, and bank slots, or
// nothing. `built` is the archive that `file` builds, with `groups`. `before` holds what the performer plays of each
// sound there. `replaced` takes the new version of `file`. A sound whose loop the performer finds a whole number of
// passes away counts in `moved` (SameUpToLoop).
std::string CrossProblem(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                         const sar::SoundArchive& built, std::span<const Group> groups, uint32_t sound, uint32_t other,
                         const Source& source, std::map<uint32_t, song::Sequence>& before,
                         std::optional<format::File>& replaced, std::size_t& moved);

// What is wrong when sound `sound` of `file` takes its sequence file again, from the archive and for each other
// console, or nothing. `built` is the archive that `file` builds. A file that the 3DS can't hold counts apart.
std::string SameProblem(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                        const sar::SoundArchive& built, uint32_t sound, const Source& same, Totals& totals);

// A file that Replace… replaces: its kind, the item whose context menu offers it, and its file ID. A wave sound file is
// the file of a wave sound.
struct FileItem
{
    edit::FileKind kind = edit::FileKind::kBank;
    uint32_t item = 0;
    uint32_t file_id = 0;
};

// The banks' files, the wave archives' files and the wave sound files of `archive`, each file once, in that order.
std::vector<FileItem> FileItemsOf(const sar::SoundArchive& archive);

// What is wrong with `replaced`, the version of a file whose archive holds `bytes` as the file of `item`, or nothing.
// `built` is the archive that the old version builds, with `groups`. `after` takes the archive that `replaced` builds.
// Each copy in a group holds what CopyProblem requires. The copies that named the file's waves at other places count in
// `counted`, where the call gives it.
std::string ReplacedProblem(const sar::SoundArchive& built, std::span<const Group> groups, const FileItem& item,
                            std::span<const uint8_t> bytes, const format::File& replaced, Archive& after,
                            std::size_t* counted = nullptr);

// What is wrong when the file of `item` takes `bytes`, the bytes of another file of its kind, or nothing. `built` is
// the archive that `file` builds, with `groups`. `after` takes the archive that the new version of `file` builds. The
// copies in groups that named the file's waves at other places count in `placed`, where the call gives it.
std::string FileProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                        const FileItem& item, std::span<const uint8_t> bytes, Archive& after,
                        std::size_t* placed = nullptr);

// The name of `item` in the tool's lines.
std::string NameOf(const sar::SoundArchive& archive, const FileItem& item);

// Whether another file ID of `archive` names the bytes of file `file_id` in the archive.
bool SharedFile(const sar::SoundArchive& archive, uint32_t file_id);

// Whether neither `archive` nor its group files hold file `file_id`: Replace… can't reach it.
bool Outside(const sar::SoundArchive& archive, uint32_t file_id);

// Whether only the group files beside `archive` hold file `file_id`.
bool Beside(const sar::SoundArchive& archive, uint32_t file_id);

// The item of the browser whose file is the file of `item`.
song::Item SongItemOf(const FileItem& item);

// The ID of the item whose file is the file of `item`. A wave sound file's item is a wave sound that plays it.
uint32_t ItemIdOf(const FileItem& item);

// Checks each file of a bank, a wave archive or a wave sound with itself, then with its file for each other console,
// then with the next file of its kind. A bank then takes copies of instruments at the programs that its sounds select
// and the next bank lacks (FixesProblem). `places` gives where each sound plays. After the replacement with the next
// file, the item joins the groups that the Replace window proposes (edit_groups), up to kJoins times.
void CheckFiles(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                std::span<const edit::Group> edit_groups, std::span<const std::optional<edit::SoundPlace>> places,
                const std::string& name, Totals& totals);

// The version in the header of `file`, a file of the console of `flavor`.
uint32_t HeaderVersion(std::span<const uint8_t> file, sar::Flavor flavor);

// What is wrong when wave `wave` of wave archive `item` comes back from its WAV file `wav` as DSP-ADPCM, or when the
// WAV file goes in as a new wave after the wave archive's waves without `wave`. `present` is the wave archive, and
// `pcm` holds the WAV file's samples. A loop's start moves forward to a frame, and the samples from the old start
// repeat after the loop's end, as often as it takes. The new wave has to play those samples closely, with the ratio of
// signal to noise that the Replace window shows. Each later pass of its loop has to decode to the first pass's samples.
// The new wave archive has to keep the other waves' bytes.
std::string DspAdpcmProblem(const sar::SoundArchive& built, const FileItem& item, const sar::WaveArchiveFile& present,
                            std::optional<uint32_t> wave, std::span<const uint8_t> wav, const sar::Pcm& pcm,
                            Totals& totals);

// Checks each wave of each wave archive with its WAV file. A wave archive that neither the archive nor its group files
// hold, and one whose bytes two file IDs name, stay out: Replace… refuses them.
void CheckWaves(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                const std::string& name, Totals& totals);

// The sequence sounds that --new copies: up to kNewModels of the sounds with places, spread through the sound table.
std::vector<uint32_t> NewModels(std::span<const std::optional<edit::SoundPlace>> places);

// Whether two sounds hold the same values in the tables, but for their names, files, starts and tracks.
bool SameValues(const sar::SoundInfo& a, const sar::SoundInfo& b);

// What is wrong when `after`, the archive of `built` with new items after the old ones, changes the items and files of
// `built`, or nothing. The sounds keep their values (SameValues), names, files, starts and tracks, and with
// `wave_sound_indices` their wave sound indices. The banks and wave archives keep their names and files, and `after`
// may hold more banks only with `new_banks`. Every file keeps its bytes.
std::string OtherItemsProblem(const sar::SoundArchive& built, const sar::SoundArchive& after, bool new_banks,
                              bool wave_sound_indices);

// The names of a list of items.
template <typename Item>
std::vector<std::string> NamesOf(const std::vector<Item>& items)
{
    std::vector<std::string> names;
    for (const Item& item : items)
    {
        names.push_back(item.name);
    }

    return names;
}

// Copies of the sequence sounds of NewModels in `file`, each with its file from the archive and for each other console.
// The second model's copy also goes in after the first model's. `built` is the archive that `file` builds, in `bytes`,
// with `edit_groups`. Up to kJoins copies join the groups that the Import window proposes.
void CheckNewSounds(const format::File& file, const sar::SoundArchive& built, std::span<const uint8_t> bytes,
                    std::span<const edit::Group> edit_groups, std::span<const std::optional<edit::SoundPlace>> places,
                    const std::string& name, Totals& totals);

// The bits of an item ID that give the item's place in its list.
constexpr uint32_t kIndexBits = 0x00FFFFFF;

// A name that no item of `tables` holds: `stem`, with underscores after it while an item holds the name.
std::string FreeTableName(const format::NativeTables& tables, std::string stem);

// The file of bank `bank` of `archive`, by its item ID. Empty for a bank past the archive's list or without its file.
std::span<const uint8_t> BankFile(const sar::SoundArchive& archive, uint32_t bank);

// Copies of the banks of up to kBankModels sequence sounds of `file`, with copies of their wave archives, and a copy of
// each sound that plays the new banks. `built` is the archive that `file` builds. The models are those of --new, taken
// in an order that spreads them through the sound table.
void CheckNewBanks(const format::File& file, const Archive& built,
                   std::span<const std::optional<edit::SoundPlace>> places, const std::string& name, Totals& totals);

// The wave sound files of `donor`, up to kWaveSoundFiles, as new wave sounds of `file`. `built` is the archive that
// `file` builds, with `edit_groups`, and `self` says that the donor is that archive. Each file goes in once with a wave
// sound as the model, and once with the archive's first sequence sound. Up to kJoins times, the new sounds join the
// groups that the Import window proposes.
void CheckNewWaveSounds(const format::File& file, const sar::SoundArchive& built,
                        std::span<const edit::Group> edit_groups, const sar::SoundArchive& donor, bool self,
                        const std::string& name, Totals& totals);

// Checks the new waves of each wave archive (NewWavesProblem). A wave archive that neither the archive nor its group
// files hold, and one whose bytes two file IDs name, stay out: the Import window's edit refuses them.
void CheckNewWaves(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                   const std::string& name, Totals& totals);

// Checks the variables of Replace… of a sequence (--variables). Up to kVariablePasses sounds of each sequence file take
// each round, one sound of each file in each version.
void CheckVariables(const format::File& file, const sar::SoundArchive& built,
                    std::span<const std::optional<edit::SoundPlace>> places, const std::string& name, Totals& totals);

// Whether `b`, a stream made again from the WAV file of `a`, plays the samples that `a` plays: the same samples up to
// the end of `a`'s loop, then the samples from the loop's start that the later start of `b`'s loop moved after the end.
bool PlaysSame(const sar::Pcm& a, const sar::Pcm& b);

// The streams beside `built`, the archive that `file` builds. Each stream goes in with the first stream sound that
// plays it (StreamProblem, and AacStreamProblem for an AAC stream).
void CheckStreams(const format::File& file, const sar::SoundArchive& built, const std::string& name, Totals& totals);

// The streams beside the archive of `donor` as new stream sounds of `built`, the archive that `file` builds
// (DonorStreamProblem). The model alternates between the archive's first stream sound and its first sound of another
// kind.
void CheckDonorStreams(const format::File& file, const sar::SoundArchive& built, const format::File& donor,
                       const std::string& name, Totals& totals);

// Prints the ratios of signal to noise of the waves from their WAV files as DSP-ADPCM, by tens of decibels. With
// `half_rates`, also prints the waves that came back from their WAV files at half their rates.
void PrintDspAdpcm(const std::string& name, const Totals& totals, bool half_rates);

using WiiArchive = std::shared_ptr<const ntdlib::rsar::SoundArchive>;

// The copies of file `id` in the groups of the Wii's `archive`, read from `bytes`, or of its wave data with
// `wave_data`, in the order of the groups. A copy that runs past the end of an archive that is cut short stays out.
std::vector<std::vector<uint8_t>> WiiCopies(std::span<const uint8_t> bytes, const ntdlib::rsar::SoundArchive& archive,
                                            uint32_t id, bool wave_data);

// What is wrong with the files of `after`, read from `after_bytes`, against those of `built`, read from `built_bytes`,
// or nothing. File `changed` may differ, in its file with `file` and in its wave data with `wave_data`. Every other
// file has to keep its bytes and its wave data in every copy.
std::string OtherWiiFilesProblem(std::span<const uint8_t> built_bytes, const ntdlib::rsar::SoundArchive& built,
                                 std::span<const uint8_t> after_bytes, const ntdlib::rsar::SoundArchive& after,
                                 uint32_t changed, bool file, bool wave_data);

// A sound's sequence file of the Wii as Replace… reads it, by the rules of `build`, and the item where the sound starts
// in it, if an item starts there.
Source WiiSourceOf(std::span<const uint8_t> bytes, uint32_t start_offset, const ntdlib::SndBuild& build);

// Whether the commands that the Wii's sound `info` plays from `source` open tracks that the sound doesn't allocate.
bool Untracked(const Source& source, const ntdlib::rsar::SoundInfo& info);

// What ntdlib's performer plays of sound `sound` of the Wii's `archive` by the rules of `build`, without the places of
// its commands.
song::Sequence WiiPlayed(const ntdlib::rsar::SoundArchive& archive, uint32_t sound, const ntdlib::SndBuild& build);

// Whether the sequence file of sound `sound` of the Wii's `archive` has a label at the sound's start.
bool StartsAtLabel(const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

// `bytes` with zeros up to a multiple of 32 bytes. NW4R's tools pad each file and each wave data that way.
std::vector<uint8_t> PaddedForWii(std::span<const uint8_t> bytes);

// A refusal of an edit of the Wii's: Replace…, File > Import or the Project menu refuses it with this reason. The Wii's
// checks count a refusal by its reason. Any other exception is a failure, a std::invalid_argument of a read after the
// edit included.
struct Refusal : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// A donor's sound, file or wave that the Wii's checks leave out, with the reason.
struct LeftOut : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// The result of `step`, a step that the program can refuse: an edit, or the read of a file that comes in from a donor.
// Its refusal (std::invalid_argument) goes on as a Refusal.
template <typename Step>
auto Refusable(const Step& step)
{
    try
    {
        return step();
    }
    catch (const std::invalid_argument& error)
    {
        throw Refusal(error.what());
    }
}

// The Wii's imports. Up to `limit` sounds of `type` of the Wii's `archive` whose files the archive holds, spread over
// the sound table.
std::vector<uint32_t> WiiModels(const ntdlib::rsar::SoundArchive& archive, ntdlib::rsar::SoundType type,
                                std::size_t limit);

// What an import changed in the Wii's archive besides its new items, or nothing. `before` and `after` are the models of
// the archive before and after the import, and `joining` holds the files that may join groups. Each table keeps its
// entries, and each file its bytes and its wave data. A group keeps its files, and takes only files of `joining` after
// them.
std::string WiiOthersProblem(const ntdlib::rsar::ArchiveModel& before, const ntdlib::rsar::ArchiveModel& after,
                             std::span<const uint32_t> joining);

// Checks File > Import > Sequence…, Bank…, Wave Sound File…, Wave… and Audio File… on the Wii's `file` by `mode`.
// `file` builds `built` from `built_bytes`. Prints what fails and a summary. Returns the exit code: 1 when something
// fails.
int CheckWiiImports(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                    const ntdlib::SndBuild& build, const std::string& name, const std::string& mode);

// Checks the Wii's imports of the files of `donor_path`, an archive of the 3DS, the Wii U or the Switch, in the Wii's
// `file` by `mode`. `file` builds `built` from `built_bytes`. Prints what fails and a summary. Returns the exit code: 1
// when something fails, and 2 when the donor can't be read.
int CheckWiiDonor(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                  const ntdlib::SndBuild& build, const std::string& name, const std::string& mode,
                  const std::filesystem::path& donor_path);

// What the Wii's streams counted.
struct WiiStreamTotals
{
    std::size_t streams = 0; // streams beside the archive that a sound plays, or the donor's streams
    std::size_t failed = 0;
    std::size_t channels = 0;
    std::size_t unplayed = 0;          // streams beside the archive that no sound plays
    std::size_t conversions = 0;       // streams that came back from a file of another console
    std::size_t exact_conversions = 0; // conversions that came back with the stream's bytes
    std::size_t same_wav = 0;          // streams that came back from their WAV files in 16-bit PCM with their bytes
    std::size_t moved = 0;             // streams whose loop's start moved forward to a block of 16-bit PCM
    std::size_t exact = 0;             // streams that came back exactly as DSP-ADPCM
    std::vector<double> ratios;        // the signal-to-noise ratios of the others, in decibels
    std::size_t entries = 0;           // the game's stream sounds that play a stream beside the archive
    std::size_t whole_entries = 0;     // those whose entries name every channel and track of the stream
    std::size_t new_sounds = 0;        // new stream sounds
    std::size_t same_as_model = 0;     // new sounds that played as their models play the stream
    std::size_t sounding = 0;          // new sounds whose renders hold sound
    std::size_t paired = 0;            // donors' streams that took a track for each pair of their channels
    std::size_t pairs = 0;             // second new sounds after a first
    double slowest = 0.0;              // the longest time that a new sound's edit took, in seconds
    std::map<std::string, std::size_t> refused; // the edits that Hokuto refused, by their messages
};

// What is wrong with stream file `id` beside the Wii's archive of `file`, or nothing. Stream sound `model` plays the
// stream. `built` is the archive that `file` builds from `built_bytes`, and `before` its model. An archive that the
// tools didn't lay out lacks a model. With `second`, the file in the model form takes the stream from its WAV file too,
// and a second new sound goes in after the first.
std::string WiiStreamProblem(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                             const std::optional<ntdlib::rsar::ArchiveModel>& before, const ntdlib::SndBuild& build,
                             uint32_t id, uint32_t model, bool second, WiiStreamTotals& totals);

// What is wrong with `bytes`, a stream file beside a donor, as a new stream sound of the Wii's `file` with sound
// `model` as its model, or nothing. `built` is the archive that `file` builds, and `before` its model. An archive that
// the tools didn't lay out lacks a model. The new sound takes the name of the donor's file `donor_id`. With `save`, the
// file with the new sound saves and opens again.
std::string WiiDonorStreamProblem(const format::File& file, const WiiArchive& built,
                                  const std::optional<ntdlib::rsar::ArchiveModel>& before,
                                  const ntdlib::SndBuild& build, uint32_t model, uint32_t donor_id,
                                  const std::vector<uint8_t>& bytes, bool save, WiiStreamTotals& totals);

// The check of the streams beside the Wii's archive at `path`, of `bytes`, with `arguments` as main takes them: each
// stream with the first stream sound that plays it (WiiStreamProblem), or with a donor after the archive, the donor's
// streams as new stream sounds (WiiDonorStreamProblem). Returns main's code.
int CheckWiiStreams(const std::vector<std::string>& arguments, const std::filesystem::path& path,
                    const std::vector<uint8_t>& bytes);

// The check of the Wii's archive at `path`, of `bytes`, with `arguments` as main takes them. Returns main's code.
int CheckWii(const std::vector<std::string>& arguments, const std::filesystem::path& path,
             const std::vector<uint8_t>& bytes);

} // namespace hokuto::tools::replace
