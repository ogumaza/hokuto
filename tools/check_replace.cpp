// SPDX-License-Identifier: MIT

// Checks the replacement of a sound's sequence (src/edit/replace.h), and of a bank, a wave archive or a wave sound file
// (src/edit/files.h), on a sound archive of the 3DS, the Wii U or the Switch. Like the program, the tool imports the
// archive into a Hokuto file and builds the archive again from the file.
//
// The file of each part describes the checks of the part:
//
// - check_replace_sequences.cpp: the sequences.
// - check_replace_files.cpp: the files of banks, wave archives and wave sounds.
// - check_replace_waves.cpp: the waves.
// - check_replace_joins.cpp: the groups that new and replaced items join.
// - check_replace_new_sounds.cpp: --new.
// - check_replace_new_banks.cpp: --banks.
// - check_replace_wave_sounds.cpp: --wave-sounds.
// - check_replace_new_waves.cpp: --new-waves.
// - check_replace_streams.cpp: --streams.
// - check_replace_variables.cpp: --variables.
// - check_replace_wii.cpp: the sequences, the files and the waves of an archive of the Wii.
// - check_replace_wii_streams.cpp: --streams on an archive of the Wii.
// - check_replace_wii_imports.cpp: --new, --banks, --wave-sounds and --new-waves on an archive of the Wii.
// - check_replace_wii_donor.cpp: a donor after an archive of the Wii.
//
// With --sequences, the tool checks only the sequences. With --files, it checks only the files of banks, wave archives
// and wave sounds. With --waves, it checks only the waves. It prints a line for each sound, file or wave archive that
// fails, and a summary. It returns 1 when one fails, and 2 when the archive can't be read.
//
// A 3DS archive's sounds play on ntdlib's model of the DSP's audio firmware.
//
// With --model, the file goes into the model form after the import (format::InModelForm), and the archive that the
// model builds has to be the game's. A donor stays in the native form. Of the files that the edits make, the first 64
// and every 64th after them have to save in both forms and open again as they were (tools/checked_archive.h).
//
// usage: hokuto_check_replace [--model] [--sequences | --files | --waves | --new | --banks | --new-waves |
//                                        --streams | --variables] <archive>
//        hokuto_check_replace [--model] --wave-sounds <archive> [<donor>]
//        hokuto_check_replace [--model] --streams <archive> [<donor>]
//        hokuto_check_replace [--model] --new | --new-waves | --files <Wii archive> <donor>

#include "check_replace.h"

namespace
{

namespace audio = hokuto::audio;
namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;
using hokuto::tools::replace::Archive;
using hokuto::tools::replace::CheckDonorStreams;
using hokuto::tools::replace::CheckFiles;
using hokuto::tools::replace::CheckNewBanks;
using hokuto::tools::replace::CheckNewSounds;
using hokuto::tools::replace::CheckNewWaves;
using hokuto::tools::replace::CheckNewWaveSounds;
using hokuto::tools::replace::CheckStreams;
using hokuto::tools::replace::CheckVariables;
using hokuto::tools::replace::CheckWaves;
using hokuto::tools::replace::CheckWii;
using hokuto::tools::replace::CheckWiiStreams;
using hokuto::tools::replace::CrossProblem;
using hokuto::tools::replace::FileTotals;
using hokuto::tools::replace::Group;
using hokuto::tools::replace::GroupsOf;
using hokuto::tools::replace::JoinAndNewGroupCheck;
using hokuto::tools::replace::Joins;
using hokuto::tools::replace::JoinsGroups;
using hokuto::tools::replace::JoinTotals;
using hokuto::tools::replace::NewBankTotals;
using hokuto::tools::replace::NewSoundTotals;
using hokuto::tools::replace::NewWaveTotals;
using hokuto::tools::replace::PrintDspAdpcm;
using hokuto::tools::replace::PrintJoins;
using hokuto::tools::replace::SameProblem;
using hokuto::tools::replace::SequenceTotals;
using hokuto::tools::replace::Source;
using hokuto::tools::replace::SourceOf;
using hokuto::tools::replace::StartsAtLabel;
using hokuto::tools::replace::StreamTotals;
using hokuto::tools::replace::Untracked;
using hokuto::tools::replace::VariableTotals;
using hokuto::tools::replace::WaveSoundTotals;
using hokuto::tools::replace::WaveTotals;

// With --new, checks the new sounds alone and prints their summary. Returns the exit code.
int RunNew(const format::File& file, const sar::SoundArchive& built, std::span<const uint8_t> built_bytes,
           std::span<const edit::Group> edit_groups, std::span<const std::optional<edit::SoundPlace>> places,
           const std::string& name)
{
    NewSoundTotals totals;
    JoinTotals joined;
    CheckNewSounds(file, built, built_bytes, edit_groups, places, name, totals, joined);
    std::printf(
        "%s: %zu new sounds copied a sequence sound with its file. %zu times the same sound came from a file for "
        "another console. %zu fail. %zu went in after another new sound. %zu models start where no command of "
        "their files starts. %zu models open tracks that they don't allocate, and their new sounds play those "
        "tracks. %zu files for another console decode otherwise, and %zu come back with other biquad filters. %zu "
        "files reach a command that the 3DS lacks. The slowest edit took %.2f s.\n",
        name.c_str(), totals.new_sounds, totals.new_crossed, totals.new_failed, totals.new_pairs, totals.new_no_start,
        totals.new_untracked, totals.new_other_unreached, totals.new_other_biquads, totals.new_ctr_refused,
        totals.new_slowest);
    PrintJoins(name, joined, !edit_groups.empty());

    return totals.new_failed > 0 ? 1 : 0;
}

// With --variables, checks the variables of Replace… alone and prints their summary. Returns the exit code.
int RunVariables(const format::File& file, const sar::SoundArchive& built,
                 std::span<const std::optional<edit::SoundPlace>> places, const std::string& name)
{
    VariableTotals totals;
    CheckVariables(file, built, places, name, totals);
    std::printf(
        "%s: %zu sequence sounds read variables before they set them, and %zu of them took the rounds of moves "
        "and fixed values. %zu edits fail. %zu edits were refused. %zu variables moved, and %zu took fixed values, "
        "%zu of them in another of the sound's variables. The edited sounds played %zu times with starting "
        "values. %zu edits of fixed values left a track running without waits, and played otherwise. %zu sounds "
        "start where no command of their files starts.\n",
        name.c_str(), totals.va_reading, totals.va_checked, totals.va_failed, totals.va_refused, totals.va_moved,
        totals.va_fixed, totals.va_held, totals.va_runs, totals.va_runaway, totals.va_no_start);
    for (const std::string& reason : totals.va_reasons)
    {
        std::printf("%s: refused: %s\n", name.c_str(), reason.c_str());
    }

    return totals.va_failed > 0 ? 1 : 0;
}

// With --streams and a donor, checks the donor's streams as new stream sounds and prints their summary. `donor_name`
// is the donor's argument. Returns the exit code.
int RunDonorStreams(const format::File& file, const sar::SoundArchive& built, const std::string& donor_name,
                    const std::string& name)
{
    StreamTotals totals;
    std::optional<format::File> donor;
    try
    {
        const std::filesystem::path donor_path = xsflib::PathFromUtf8(donor_name);
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(donor_path, bytes))
        {
            std::fprintf(stderr, "%s can't be read\n", donor_name.c_str());
            return 2;
        }
        const Archive opened = song::OpenArchive(donor_path);
        donor = format::ImportArchive(bytes, opened->ReadOptions().flavor, opened->GroupFiles(),
                                      song::ReadStreams(*opened, donor_path), opened->GroupFileGroups());
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", donor_name.c_str(), error.what());
        return 2;
    }
    CheckDonorStreams(file, built, *donor, name, totals);
    std::printf(
        "%s: %zu streams of the donor, with %zu channels, went in as new stream sounds. %zu fail. %zu new "
        "sounds hold their models' details. %zu streams took a track for each pair of their channels. %zu "
        "lie in an archive without names, and %zu in a 3DS archive whose layout of stream sounds is "
        "unknown: neither takes a new sound. %zu AAC streams count apart. The slowest edit took %.2f s.\n",
        name.c_str(), totals.st_streams, totals.st_channels, totals.st_failed, totals.st_same_details, totals.st_paired,
        totals.st_nameless, totals.st_unknown, totals.st_aac, totals.st_slowest);

    return totals.st_failed > 0 ? 1 : 0;
}

// With --streams, checks the streams beside the archive alone and prints their summary. Returns the exit code.
int RunStreams(const format::File& file, const sar::SoundArchive& built, const std::string& name)
{
    StreamTotals totals;
    CheckStreams(file, built, name, totals);
    const std::vector<double>& ratios = totals.st_ratios;
    const double least = ratios.empty() ? 0.0 : *std::min_element(ratios.begin(), ratios.end());
    std::printf(
        "%s: %zu streams beside the archive, with %zu channels, took their bytes again, came back from the other "
        "console and from their WAV files in 16-bit PCM, and went in as new stream sounds. %zu "
        "fail. %zu went in written again in an older version. %zu sounds play the tracks of their stream files. "
        "%zu loops moved forward to a block of 16-bit PCM. %zu new sounds hold their models' details. %zu second "
        "new sounds went in after a first. %zu lie in an archive without names, which takes no new sound. %zu "
        "streams beside the archive play in no sound. As DSP-ADPCM, %zu streams came back exactly, and %zu with "
        "a signal-to-noise ratio of at least %.1f dB. The slowest edit took %.2f s.\n",
        name.c_str(), totals.st_streams, totals.st_channels, totals.st_failed, totals.st_written, totals.st_tracked,
        totals.st_moved, totals.st_same_details, totals.st_pairs, totals.st_nameless, totals.st_unplayed,
        totals.st_exact, ratios.size(), least, totals.st_slowest);
    if (totals.st_aac_replaced + totals.st_aac_missing > 0)
    {
        const std::vector<double>& aac = totals.st_aac_ratios;
        std::printf(
            "%s: %zu AAC streams took their files, and their WAV files and files for the Wii U encoded "
            "again, after a delay of %u samples. %zu AAC stream that the file lacks took another's WAV "
            "file. The lowest signal-to-noise ratio was %.1f dB.\n",
            name.c_str(), totals.st_aac_replaced, totals.st_aac_delay, totals.st_aac_missing,
            *std::min_element(aac.begin(), aac.end()));
    }

    return totals.st_failed > 0 ? 1 : 0;
}

// With --wave-sounds, checks the new wave sounds alone, from the archive or from a donor, and prints their summary.
// `arguments` are the arguments of the tool. Returns the exit code.
int RunWaveSounds(const format::File& file, const Archive& built, std::span<const edit::Group> edit_groups,
                  const std::vector<std::string>& arguments, const std::string& name)
{
    WaveSoundTotals totals;
    JoinTotals joined;
    Archive donor = built;
    try
    {
        donor = arguments.size() == 4 ? song::OpenArchive(xsflib::PathFromUtf8(arguments[3])) : built;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", arguments[3].c_str(), error.what());
        return 2;
    }
    CheckNewWaveSounds(file, *built, edit_groups, *donor, donor == built, name, totals, joined);
    std::printf(
        "%s: %zu wave sound files went in as %zu new wave sounds, %zu of them copies of a sequence sound, with "
        "%zu new wave archives. %zu fail. %zu files went in written again with the archive's versions. %zu new "
        "wave sounds played their donors' samples, and neither side decodes %zu. %zu play wave sounds that no "
        "sound of the donor plays. %zu files' wave archives lack their files. The slowest edit took %.2f s.\n",
        name.c_str(), totals.wsd_files, totals.wsd_sounds, totals.wsd_from_sequences, totals.wsd_wave_archives,
        totals.wsd_failed, totals.wsd_written, totals.wsd_compared, totals.wsd_undecoded, totals.wsd_unplayed,
        totals.wsd_missing, totals.wsd_slowest);
    PrintJoins(name, joined, !edit_groups.empty());

    return totals.wsd_failed > 0 ? 1 : 0;
}

// With --new-waves, checks the new waves alone and prints their summary. Returns the exit code.
int RunNewWaves(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                const std::string& name)
{
    NewWaveTotals totals;
    CheckNewWaves(file, built, groups, name, totals);
    std::printf(
        "%s: %zu waves of %zu wave archives went in again as new waves, from their wave files, from their wave "
        "files for the other consoles and from their WAV files. %zu wave archives fail. %zu lie in neither the "
        "archive nor its group files, or share their file. %zu lie only in the group files beside the archive. "
        "%zu wave archives without waves took the archive's first wave, and %zu in archives without waves took a "
        "WAV file. %zu times a wave came back from another console with other bytes. %zu waves hold another "
        "version than a new wave of their wave archive takes, and %zu of them a later one: they went in written "
        "again. %zu second new waves went in after a first. The slowest edit took %.2f s.\n",
        name.c_str(), totals.nw_waves, totals.nw_wave_archives, totals.nw_failed, totals.wave_archives_out,
        totals.wave_archives_beside, totals.nw_empty, totals.nw_waveless, totals.nw_other_bytes,
        totals.nw_other_versions, totals.nw_written, totals.nw_pairs, totals.nw_slowest);
    PrintDspAdpcm(name, totals, false);

    return totals.nw_failed > 0 ? 1 : 0;
}

// With --banks, checks the new banks and wave archives alone and prints their summary. Returns the exit code.
int RunBanks(const format::File& file, const Archive& built, std::span<const std::optional<edit::SoundPlace>> places,
             const std::string& name)
{
    NewBankTotals totals;
    CheckNewBanks(file, built, places, name, totals);
    std::printf(
        "%s: %zu new sounds played copies of their models' banks: %zu new banks with %zu new wave archives. %zu "
        "fail. %zu new sounds stay silent for 10 s, as their models do. %zu models have no bank in their slots. "
        "%zu models' banks or wave archives lack their files. %zu models start where no command of their "
        "files starts, or open tracks that they don't allocate. The slowest edit took %.2f s.\n",
        name.c_str(), totals.bank_checked, totals.new_banks, totals.new_wave_archives, totals.bank_failed,
        totals.bank_silent, totals.bankless, totals.bank_missing, totals.bank_skipped, totals.bank_slowest);

    return totals.bank_failed > 0 ? 1 : 0;
}

// What is wrong when sequence sound `sound` of `built`, the archive that `file` builds, receives the file of sound
// `other` (CrossProblem), or nothing. Sound `other` plays `source`. The sound then joins groups (JoinAndNewGroupCheck).
// `before` contains the sequences that the sounds played before. Counts the crossings, the loops that moved and the
// joins.
std::string CrossAndJoin(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                         std::span<const edit::Group> edit_groups,
                         std::span<const std::optional<edit::SoundPlace>> places, uint32_t sound, uint32_t other,
                         const Source& source, std::map<uint32_t, song::Sequence>& before, Joins& joins,
                         SequenceTotals& totals, JoinTotals& joined)
{
    const sar::SoundInfo& other_info = built.Sounds()[other];
    std::optional<format::File> replaced;
    std::string problem =
        CrossProblem(file, places, built, groups, sound, other, source, before, replaced, totals.loops_moved);
    totals.crossed++;

    // The sound joins the groups that the Replace window proposes: those that load the sound and lack a
    // file of its new bank slots. The first sound also joins a new group.
    if (problem.empty() && JoinsGroups(joins, edit_groups))
    {
        const uint32_t id = sar::ItemId(sar::kSoundItem, sound);
        const std::vector<uint32_t> files_before = edit::FilesOfItem(built, id);
        const auto files_after = [&]
        {
            return edit::FilesOfItem(built, id, {.banks = other_info.banks});
        };
        const auto added = [&]() -> const format::File&
        {
            return *replaced;
        };
        problem = JoinAndNewGroupCheck(file, built, edit_groups, std::span(&id, 1), files_before, files_after, added,
                                       joins, joined);
    }

    return problem;
}

// What is wrong when sequence sound `i` of `built`, the archive that `file` builds, receives its file again and the
// files for the other consoles (SameProblem), then the next sequence sound's file (CrossAndJoin), or nothing. `before`
// contains the sequences that the sounds played before.
std::string SequenceSoundProblem(const format::File& file, const sar::SoundArchive& built,
                                 std::span<const Group> groups, std::span<const edit::Group> edit_groups,
                                 std::span<const std::optional<edit::SoundPlace>> places, std::size_t i,
                                 std::map<uint32_t, song::Sequence>& before, Joins& joins, SequenceTotals& totals,
                                 JoinTotals& joined)
{
    const auto sound = static_cast<uint32_t>(i);
    const sar::SoundInfo& info = built.Sounds()[i];
    const Source same = SourceOf(built.FileData(info.file_id), info.start_offset);
    totals.unlabelled += info.start_offset != 0 && !StartsAtLabel(built, sound) ? 1 : 0;
    const bool untracked = same.start && Untracked(same, info);
    totals.untracked += untracked ? 1 : 0;
    std::string problem = SameProblem(file, places, built, sound, same, totals);

    // The next sequence sound with another file. A sound passes its turn on when it opens tracks that it
    // doesn't allocate.
    for (std::size_t step = 1; step < places.size() && problem.empty() && !untracked; step++)
    {
        const auto other = static_cast<uint32_t>((i + step) % places.size());
        if (!places[other] || places[other]->sequence == places[i]->sequence)
        {
            continue;
        }

        const sar::SoundInfo& other_info = built.Sounds()[other];
        const Source source = SourceOf(built.FileData(other_info.file_id), other_info.start_offset);
        if (!source.start || Untracked(source, other_info))
        {
            continue;
        }

        problem =
            CrossAndJoin(file, built, groups, edit_groups, places, sound, other, source, before, joins, totals, joined);
        break;
    }

    return problem;
}

// Prints the summary of the sequence sounds.
void PrintSequences(const std::string& name, const SequenceTotals& totals)
{
    std::printf(
        "%s: %zu sequence sounds took their files again, and the files for the other consoles. %zu took another "
        "sound's file. %zu fail. %zu sounds' files stay in the archive's bytes. %zu sounds start past the first "
        "command, where their files have no label, and Replace… offers their starts. %zu times a file came back "
        "otherwise from another console, from bytes that no command reaches, and %zu times with other biquad "
        "filters. %zu files reach a command that the 3DS lacks, and don't go to the 3DS. %zu sounds open tracks "
        "that they don't allocate. With another sound's file, ntdlib's performer found the loop of %zu sounds a "
        "whole number of passes away.\n",
        name.c_str(), totals.sounds, totals.crossed, totals.failed, totals.in_bytes, totals.unlabelled,
        totals.other_unreached, totals.other_biquads, totals.ctr_refused, totals.untracked, totals.loops_moved);
}

// Prints the summary of the files of banks, wave archives and wave sounds.
void PrintFiles(const std::string& name, const FileTotals& totals)
{
    std::printf(
        "%s: %zu files of banks, wave archives and wave sounds took their bytes again, and %zu took another "
        "file's. %zu fail. %zu files have copies in groups with other bytes. %zu files have copies in groups that "
        "name their waves at other places, and %zu such copies took another file with their places. %zu files "
        "are shared by two file IDs, and %zu lie in neither the archive nor its group files. %zu files lie only in "
        "the group files beside the archive.\n",
        name.c_str(), totals.files, totals.files_crossed, totals.files_failed, totals.files_other_copies,
        totals.files_placed, totals.files_placed_copies, totals.files_shared, totals.files_outside,
        totals.files_beside);
    std::printf(
        "%s: %zu times a file of a bank, a wave archive or a wave sound took the same file from another console. "
        "%zu times a wave archive came back with other bytes, and its waves decode as before. %zu other files of "
        "the kind went in written again with the versions of the present file.\n",
        name.c_str(), totals.files_converted, totals.files_converted_otherwise, totals.files_written);
    std::printf(
        "%s: The sounds of the banks select %zu programs, and %zu program changes take their program or slot from "
        "a value. In %zu banks, the next bank lacks %zu of them. %zu took the present bank's instruments, and %zu "
        "the next bank's first instrument.\n",
        name.c_str(), totals.selected, totals.undecided, totals.fixed_banks, totals.fixed_programs,
        totals.fixes_present, totals.fixes_next);
}

// Prints the summary of the waves.
void PrintWaves(const std::string& name, const WaveTotals& totals)
{
    std::printf(
        "%s: %zu waves of %zu wave archives came back from their WAV files. %zu wave archives fail. %zu lie in "
        "neither the archive nor its group files, or share their file. %zu lie only in the group files. %zu waves "
        "hold more than two channels. %zu waves lose a loop that doesn't play. In %zu looped DSP-ADPCM waves, the "
        "loop's later passes differ from the first by up to %d in a sample, up to %.1f ms after the loop's "
        "start.\n",
        name.c_str(), totals.waves, totals.wave_archives, totals.wave_archives_failed, totals.wave_archives_out,
        totals.wave_archives_beside, totals.waves_wide, totals.waves_loop_lost, totals.waves_later, totals.later_peak,
        totals.later_ms);
    std::printf(
        "%s: Each wave also went back in from its wave file, and from its wave files for the other consoles. %zu "
        "waves gave their wave archives other bytes when their files went back in their places. %zu times a wave "
        "came back from another console with other bytes, and it decodes as before.\n",
        name.c_str(), totals.waves_rewritten, totals.waves_other_bytes);
    PrintDspAdpcm(name, totals, true);
}

// Prints a line for each part that ran: the sequence sounds unless `files_only` or `waves_only`, the files unless
// `waves_only` or `sequences_only`, the waves unless `files_only` or `sequences_only`, and the joins, for an archive
// with groups when `groups` says so, unless `waves_only`.
void PrintSummary(const std::string& name, const SequenceTotals& sequence_totals, const FileTotals& file_totals,
                  const WaveTotals& wave_totals, const JoinTotals& joined, bool files_only, bool waves_only,
                  bool sequences_only, bool groups)
{
    if (!files_only && !waves_only)
    {
        PrintSequences(name, sequence_totals);
    }
    if (!waves_only && !sequences_only)
    {
        PrintFiles(name, file_totals);
    }
    if (!files_only && !sequences_only)
    {
        PrintWaves(name, wave_totals);
    }
    if (!waves_only)
    {
        PrintJoins(name, joined, groups);
    }
}

// The options of the command line: the part of the check that runs alone, if any, and whether a donor follows a Wii
// archive.
struct Options
{
    bool files_only = false;
    bool waves_only = false;
    bool new_only = false;
    bool banks_only = false;
    bool sequences_only = false;
    bool wave_sounds_only = false;
    bool new_waves_only = false;
    bool streams_only = false;
    bool variables_only = false;
    bool wii_donor = false;
};

// The options of the command line's `arguments`, or nothing after the usage.
std::optional<Options> ParseOptions(const std::vector<std::string>& arguments)
{
    Options o;
    o.files_only = arguments.size() == 3 && arguments[1] == "--files";
    o.waves_only = arguments.size() == 3 && arguments[1] == "--waves";
    o.new_only = arguments.size() == 3 && arguments[1] == "--new";
    o.banks_only = arguments.size() == 3 && arguments[1] == "--banks";
    o.sequences_only = arguments.size() == 3 && arguments[1] == "--sequences";
    o.wave_sounds_only = (arguments.size() == 3 || arguments.size() == 4) && arguments[1] == "--wave-sounds";
    o.new_waves_only = arguments.size() == 3 && arguments[1] == "--new-waves";
    o.streams_only = (arguments.size() == 3 || arguments.size() == 4) && arguments[1] == "--streams";
    o.variables_only = arguments.size() == 3 && arguments[1] == "--variables";
    o.wii_donor = arguments.size() == 4 &&
                  (arguments[1] == "--new" || arguments[1] == "--new-waves" || arguments[1] == "--files");
    if (arguments.size() != 2 && !o.files_only && !o.waves_only && !o.new_only && !o.banks_only && !o.sequences_only &&
        !o.wave_sounds_only && !o.new_waves_only && !o.streams_only && !o.variables_only && !o.wii_donor)
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_replace [--model] [--sequences | --files | --waves | --new | --banks | "
                     "--new-waves | --streams | --variables] <archive>\n"
                     "       hokuto_check_replace [--model] --wave-sounds <archive> [<donor>]\n"
                     "       hokuto_check_replace [--model] --streams <archive> [<donor>]\n"
                     "       hokuto_check_replace [--model] --new | --new-waves | --files <Wii archive> <donor>\n");
        return std::nullopt;
    }

    return o;
}

// The file of an archive of a later console, the archive that it builds, and where each sound plays in the file.
struct ReplaceFile
{
    std::optional<format::File> imported;
    std::vector<uint8_t> built_bytes;
    Archive built;
    std::vector<Group> groups;
    std::vector<edit::Group> edit_groups;
    std::vector<std::optional<edit::SoundPlace>> places;
};

// Imports `bytes`, the archive at `path`, `name`, into a file, and builds the archive again from the file. Throws when
// a step fails.
ReplaceFile OpenReplaceFile(const std::filesystem::path& path, const std::string& name,
                            const std::vector<uint8_t>& bytes)
{
    ReplaceFile opened;
    const Archive archive = song::OpenArchive(path);
    opened.imported = format::ImportArchive(bytes, archive->ReadOptions().flavor, archive->GroupFiles(),
                                            song::ReadStreams(*archive, path), archive->GroupFileGroups());
    opened.imported = hokuto::tools::CheckedForm(std::move(*opened.imported), bytes);
    opened.built_bytes = format::ExportArchive(*opened.imported);
    opened.built = song::LoadArchive(opened.built_bytes, name, format::ExportGroupFiles(*opened.imported));
    opened.groups = GroupsOf(opened.built_bytes, opened.built->ReadOptions().flavor);
    opened.edit_groups = edit::GroupsOf(*opened.imported, *opened.built);
    opened.places =
        edit::SoundPlaces(*opened.built, format::SequencesOfFiles(*opened.imported, opened.built_bytes, *opened.built),
                          format::StartFieldsOfSounds(*opened.imported, opened.built_bytes));

    return opened;
}

// Runs the part of the check that an option of `o` runs alone, on `opened`, the file of the archive `name`, and returns
// the tool's exit status. Returns nothing without such an option. `arguments` are the arguments of the command line.
std::optional<int> RunAlone(const Options& o, const std::vector<std::string>& arguments, const ReplaceFile& opened,
                            const std::string& name)
{
    const auto& [imported, built_bytes, built, groups, edit_groups, places] = opened;
    const format::File& file = *imported;

    // With --new, the new sounds alone.
    if (o.new_only)
    {
        return RunNew(file, *built, built_bytes, edit_groups, places, name);
    }

    // With --variables, the variables of Replace… alone.
    if (o.variables_only)
    {
        return RunVariables(file, *built, places, name);
    }

    // With --streams and a donor, the donor's streams as new stream sounds.
    if (o.streams_only && arguments.size() == 4)
    {
        return RunDonorStreams(file, *built, arguments[3], name);
    }

    // With --streams, the streams beside the archive alone.
    if (o.streams_only)
    {
        return RunStreams(file, *built, name);
    }

    // With --wave-sounds, the new wave sounds alone, from the archive or from a donor.
    if (o.wave_sounds_only)
    {
        return RunWaveSounds(file, built, edit_groups, arguments, name);
    }

    // With --new-waves, the new waves alone.
    if (o.new_waves_only)
    {
        return RunNewWaves(file, *built, groups, name);
    }

    // With --banks, the new banks and wave archives alone.
    if (o.banks_only)
    {
        return RunBanks(file, built, places, name);
    }

    return std::nullopt;
}

// Each sequence sound of `opened`, the file of the archive `name`, with its file and with the files for the other
// consoles (SameProblem), then with the next sequence sound's file (CrossProblem). Counts the sounds in `totals` and
// the joins in `joined`.
void CheckSequenceSounds(const ReplaceFile& opened, const std::string& name, SequenceTotals& totals, JoinTotals& joined)
{
    const auto& [imported, built_bytes, built, groups, edit_groups, places] = opened;
    const format::File& file = *imported;
    std::map<uint32_t, song::Sequence> before;
    Joins sequence_joins;
    for (std::size_t i = 0; i < built->Sounds().size(); i++)
    {
        const sar::SoundInfo& info = built->Sounds()[i];
        if (info.type != sar::SoundType::kSequence || built->FileData(info.file_id).empty())
        {
            continue;
        }
        if (!places[i])
        {
            totals.in_bytes++;
            continue;
        }

        std::string problem;
        try
        {
            problem = SequenceSoundProblem(file, *built, groups, edit_groups, places, i, before, sequence_joins, totals,
                                           joined);
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), info.name.c_str(), problem.c_str());
            totals.failed++;
        }
        totals.sounds++;
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    const std::optional<Options> options = ParseOptions(arguments);
    if (!options)
    {
        return 2;
    }

    // The file, the archive that it builds, and where each sound plays in the file.
    const std::filesystem::path path = xsflib::PathFromUtf8(
        options->wave_sounds_only || options->streams_only || options->wii_donor ? arguments[2] : arguments.back());
    const std::string name = xsflib::Utf8(path.filename());
    const hokuto::tools::SavesLine saves_line(name);
    ReplaceFile opened;
    try
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(path, bytes))
        {
            std::fprintf(stderr, "%s can't be read\n", name.c_str());
            return 2;
        }
        if (hokuto::tools::IsWiiArchive(bytes))
        {
            return options->streams_only ? CheckWiiStreams(arguments, path, bytes) : CheckWii(arguments, path, bytes);
        }
        if (options->wii_donor)
        {
            std::fprintf(stderr, "%s: a donor after %s serves only an archive of the Wii\n", name.c_str(),
                         arguments[1].c_str());
            return 2;
        }
        opened = OpenReplaceFile(path, name, bytes);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    if (const std::optional<int> alone = RunAlone(*options, arguments, opened, name))
    {
        return *alone;
    }

    // Each sequence sound with its file and with the files for the other consoles (SameProblem), then with the next
    // sequence sound's file (CrossProblem).
    SequenceTotals sequence_totals;
    JoinTotals joined;
    if (!options->files_only && !options->waves_only)
    {
        CheckSequenceSounds(opened, name, sequence_totals, joined);
    }

    FileTotals file_totals;
    WaveTotals wave_totals;
    const bool files = !options->waves_only && !options->sequences_only;
    const bool waves = !options->files_only && !options->sequences_only;
    if (files)
    {
        CheckFiles(*opened.imported, *opened.built, opened.groups, opened.edit_groups, opened.places, name, file_totals,
                   joined);
    }
    if (waves)
    {
        CheckWaves(*opened.imported, *opened.built, opened.groups, name, wave_totals);
    }

    // A line for each part that ran.
    PrintSummary(name, sequence_totals, file_totals, wave_totals, joined, options->files_only, options->waves_only,
                 options->sequences_only, !opened.edit_groups.empty());

    return sequence_totals.failed > 0 || file_totals.files_failed > 0 || wave_totals.wave_archives_failed > 0 ? 1 : 0;
}
