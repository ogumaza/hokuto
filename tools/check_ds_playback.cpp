// SPDX-License-Identifier: MIT

// Checks Hokuto's playback of the sequences of the DS's sound archives against ntdlib's ArchivePlayer, run directly.
// The archives come from DS cartridge images (format::DsArchivesIn), played by the build of the driver in the image's
// ARM7 binary (song::DsBuildOf), or from sound archives alone, played by the default build. An archive of an image uses
// the game of the image (song::DsGameOf). The reference starts each sound with the profile of its game, and sends the
// profile's commands right after the start. The reference reads the profile from ntdlib's table
// (ntdlib::ntr::GameProfileOf). The program reads the same table (song::DsProfileOf). The summary counts the runs with
// the game's output effect, and those whose profile sets more. For each sound with a sequence (song::DsSounds), it
// checks:
//
// - From the start, the player's output matches the output of the reference, sample for sample, after the player's
//   first frame. Each frame of the player contains two frames of the driver.
// - Output that ends early ends where the reference has fallen silent: within ±8 of 0 on each side. A reference that
//   stays at another level within that is counted apart. With Cycle off, the output of a sequence with a loop ends at
//   the end of the loop's first pass, and the reference plays on round the loop. The output has to reach the loop's
//   end.
// - Seeks to the middle of the output and to the player's second frame give the reference's frames. A seek that
//   produces no output counts as a difference.
// - A switch halfway through the output, to a new model of the same sequence, produces the frames of the reference
//   after the frame that crossfades. A catch-up runs the new model up to the player's frame first. The program switches
//   the same way after an edit or a change of the variables during playback.
// - Each note that a track plays on a channel up to the output's end is in the arrangement, on its track and key, at
//   the driver's tick that played it. The arrangement shows the first pass of a loop, and the later passes round it. A
//   tie that goes on in the channel of its track continues the note before it in the arrangement. The tool counts the
//   ties apart, and those that the arrangement shows as separate notes. It counts the notes missing after the first
//   pass apart too.
// - A sound whose output ends before its first frame has to leave the reference silent. The tool counts such sounds.
//
// With --variables, each sequence plays once for each choice of each variable that the Variables zone lists, -1 aside,
// and with 1 for a variable without choices. The arrangement reads the sequence with that starting value, and the
// reference reads it from the game settings (ntdlib::ntr::GameSettings). With --changes, each sequence plays once for
// each choice of each variable that its commands read, with 1 for a variable without choices. The variable takes the
// value while the sequence plays: halfway through the sequence's length or through the check, 2 ms into the model's
// frame. The reference receives the change through ArchivePlayer::Send before the first of the frame's two driver
// frames. The tool counts the runs whose output the change alters. With --enhanced, the player and the reference play
// in enhanced mixing. With --profiled, the tool checks only the sounds whose game's profile sets more than the output
// effect, and prints the name of each. With --every n, the tool checks every nth of those sounds of each archive, and
// with --part k/n, part k of n of those. The tool prints a line for each sound that differs, and a summary. It returns
// 1 when any sound differs or can't be read. Missing notes get a line too, but don't count as a difference.
//
//     check_ds_playback [--enhanced] [--variables | --changes] [--profiled] [--part k/n] [--every n]
//                       <image.nds | archive.sdat>... [seconds]

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/game_profiles.h"
#include "ntdlib/ntr/rom.h"
#include "xsflib/console.h"
#include "xsflib/files.h"
#include "xsflib/rip/length.h"

#include "check_part.h"
#include "check_playback_steps.h"
#include "check_support.h"
#include "format/archive.h"
#include "song/ds.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace
{

namespace song = hokuto::song;
using hokuto::tools::LoopText;
using hokuto::tools::Part;
using hokuto::tools::PartOf;
using hokuto::tools::playback_steps::ArrangedAt;
using hokuto::tools::playback_steps::CheckFrames;
using hokuto::tools::playback_steps::FailedSeeksText;
using hokuto::tools::playback_steps::PrintDifferences;
using hokuto::tools::playback_steps::SeekCheck;
using hokuto::tools::playback_steps::StartCheck;
using hokuto::tools::playback_steps::StartResult;
using hokuto::tools::playback_steps::StepTotals;
using hokuto::tools::playback_steps::SwitchCheck;

// A note that a track of player 0 played on a channel: the driver's frame and tick that played it, its track and key,
// and whether it continued on the channel of its track as a tie.
struct PlayedNote
{
    std::size_t frame = 0;
    uint32_t tick = 0;
    int track = 0;
    int key = 0;
    bool tie = false;
};

// What ntdlib's player produces when it runs directly: the output at a full scale of 1, the samples that each of the
// driver's frames ends at, and the notes.
struct Reference
{
    std::vector<float> samples;
    std::vector<std::size_t> ends;
    std::vector<PlayedNote> notes;
};

struct Totals : StepTotals
{
    std::size_t archives = 0;
    std::size_t sounds = 0;
    std::size_t runs = 0;
    std::size_t looped = 0;
    std::size_t unread = 0;
    std::size_t failed = 0;
    std::size_t notes = 0;
    std::size_t missed_notes = 0;
    std::size_t missed_later = 0;
    std::size_t ties = 0;
    std::size_t arranged_ties = 0; // ties that the arrangement shows as separate notes
    std::size_t silent = 0;        // runs whose output ends before its first frame
    std::size_t levels = 0;        // runs whose reference has samples other than 0 within ±8 after the output's end
    std::size_t altered = 0;       // runs whose change altered the reference's output
    std::size_t effects = 0;       // runs with the game's output effect, in strict mixing
    std::size_t profiled = 0;      // runs whose profile sets more than the output effect
    int largest_level = 0;         // the largest such sample, in steps
};

// The profile of sound `sound` of `archive` in ntdlib's table, by the archive's game (ntdlib::ntr::GameProfileOf).
ntdlib::ntr::GameProfile ProfileOf(const song::DsArchive& archive, uint32_t sound)
{
    if (!archive.game)
    {
        return {};
    }

    return ntdlib::ntr::GameProfileOf(archive.game->code, archive.game->path, archive.game->index,
                                      ntdlib::ntr::Label(archive.archive, *archive.sounds[sound]));
}

// Whether `profile` sets more than the output effect: variables, channels, a reverb or commands.
bool SetsMore(const ntdlib::ntr::GameProfile& profile)
{
    const ntdlib::ntr::GameSettings& settings = profile.settings;
    const auto set = [](const std::optional<int16_t>& value)
    {
        return value.has_value();
    };
    return !profile.commands.empty() || !settings.channels.empty() || !settings.track_channels.empty() ||
           settings.reverb.has_value() || std::ranges::any_of(settings.variables.local, set) ||
           std::ranges::any_of(settings.variables.global, set);
}

Reference RunReference(const song::DsArchive& archive, uint32_t sound, const song::StartValues& values,
                       const song::Changes& changes, bool enhanced, std::size_t samples)
{
    ntdlib::ntr::ArchivePlayerOptions options;
    options.build = archive.build;
    options.enhanced = enhanced;
    ntdlib::ntr::ArchivePlayer player(archive.archive, options);
    Reference reference;
    int64_t ticks = 0;
    player.on_tick = [&ticks](int number)
    {
        ticks += number == 0 ? 1 : 0;
    };
    player.on_note = [&reference, &ticks](const ntdlib::ntr::snd::NoteEvent& note)
    {
        if (note.player == 0 && note.channel >= 0 && ticks > 0)
        {
            reference.notes.push_back(
                {reference.ends.size(), static_cast<uint32_t>(ticks - 1), note.track, note.key, note.tie});
        }
    };

    ntdlib::ntr::GameProfile profile = ProfileOf(archive, sound);
    ntdlib::ntr::GameSettings& settings = profile.settings;
    for (const auto& [number, value] : values)
    {
        if (number >= 0 && number < song::kFirstGlobal)
        {
            settings.variables.local[static_cast<std::size_t>(number)] = value;
        }
        else if (number >= song::kFirstGlobal && number < song::kGameVariables)
        {
            settings.variables.global[static_cast<std::size_t>(number - song::kFirstGlobal)] = value;
        }
    }
    player.Start(*archive.sounds[sound], settings);
    if (!profile.commands.empty())
    {
        player.Send(std::move(profile.commands));
    }

    std::vector<ntdlib::ntr::SoundOutput> strict;
    std::vector<int16_t> pcm;
    while (reference.samples.size() < samples * 2)
    {
        // The game's changes come before the first of the two driver frames in the frame of the model.
        for (const song::VariableChange& change : changes)
        {
            if (2 * song::ChangeFrame(change.time, song::kDsFrameMs) == reference.ends.size())
            {
                ntdlib::ntr::Variables set;
                if (change.variable < song::kFirstGlobal)
                {
                    set.local[static_cast<std::size_t>(change.variable)] = change.value;
                }
                else
                {
                    set.global[static_cast<std::size_t>(change.variable - song::kFirstGlobal)] = change.value;
                }
                player.Send(ntdlib::ntr::VariableCommands(set));
            }
        }

        if (enhanced)
        {
            player.RunFrame(reference.samples);
        }
        else
        {
            strict.clear();
            pcm.clear();
            player.RunFrame(strict);
            ntdlib::ntr::AppendPcm16(strict, pcm);
            for (const int16_t sample : pcm)
            {
                reference.samples.push_back(static_cast<float>(sample) / 32768.0f);
            }
        }
        reference.ends.push_back(reference.samples.size());
    }

    return reference;
}

// Counts the notes of `reference` up to `heard` frames of output, and those that the arrangement of `sequence`
// misses.
void CheckNotes(const std::string& name, const std::shared_ptr<const song::Sequence>& sequence,
                const Reference& reference, std::size_t heard, std::size_t frame_floats, const std::string& loop,
                Totals& totals)
{
    // The notes up to the output's end. Each frame of the player contains two frames of the driver.
    std::size_t checked = 0;
    std::size_t missed = 0;
    std::size_t missed_later = 0;
    std::size_t ties = 0;
    std::size_t arranged_ties = 0;
    for (const PlayedNote& note : reference.notes)
    {
        if (note.frame >= reference.ends.size() || reference.ends[note.frame] > heard * frame_floats)
        {
            continue;
        }

        const bool arranged = ArrangedAt(*sequence, note.track, note.key, note.tick);
        if (note.tie)
        {
            ties++;
            arranged_ties += arranged ? 1 : 0;
            continue;
        }
        checked++;
        if (!arranged)
        {
            missed++;
            missed_later += sequence->loop && note.tick >= sequence->loop->end ? 1 : 0;
        }
    }
    totals.notes += checked;
    totals.missed_notes += missed;
    totals.missed_later += missed_later;
    totals.ties += ties;
    totals.arranged_ties += arranged_ties;
    if (missed > 0)
    {
        std::printf("%s (loop %s): %zu of %zu notes missing from the arrangement, %zu after the first pass.\n",
                    name.c_str(), loop.c_str(), missed, checked, missed_later);
    }
}

// Checks one run of sound `sound` with the starting values `values`, for `seconds` at most.
void CheckRun(const std::string& name, const std::shared_ptr<const song::DsArchive>& archive, uint32_t sound,
              const song::StartValues& values, const song::Changes& changes, int seconds, bool enhanced, Totals& totals)
{
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound, values, changes));
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't be read. %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }
    totals.runs++;
    const ntdlib::ntr::GameProfile profile = ProfileOf(*archive, sound);
    totals.effects += !enhanced && profile.settings.effect ? 1 : 0;
    totals.profiled += SetsMore(profile) ? 1 : 0;
    totals.looped += sequence->loop ? 1 : 0;

    const song::Mixing mixing = enhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict;
    const auto model = std::make_shared<song::SequenceModel>(archive, sequence, song::ModelOptions{.mixing = mixing});
    const std::size_t frame_samples = model->FrameSamples();
    const std::size_t frame_floats = frame_samples * 2;
    const std::size_t frames = CheckFrames(*model, seconds);
    Reference reference;
    try
    {
        const std::size_t samples = (frames + 2) * frame_samples;
        reference = RunReference(*archive, sound, values, changes, enhanced, samples);
        if (!changes.empty() &&
            reference.samples != RunReference(*archive, sound, values, {}, enhanced, samples).samples)
        {
            totals.altered++;
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the reference can't play it. %s\n", name.c_str(), error.what());
        totals.failed++;
        return;
    }

    // From the start. The output's last frame can fade out, and the comparison leaves it out.
    StartResult start;
    try
    {
        start = StartCheck(model, frames, reference.samples);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the player can't play it. %s\n", name.c_str(), error.what());
        totals.failed++;
        return;
    }
    const auto [ended, heard, differences] = start;

    // A loop's output has to reach the end of the loop's first pass. A model that fails or falls silent ends it sooner.
    const bool short_loop = ended && sequence->loop && model->After() < static_cast<double>(sequence->loop->end);

    // After an end of the output, the reference has to stay within ±8 of 0. The reference plays on round a loop.
    const std::size_t rest = std::min(heard * frame_floats, reference.samples.size());
    const std::size_t stop = std::min(frames * frame_floats, reference.samples.size());
    float level = 0.0f;
    if (ended && !sequence->loop && rest < stop)
    {
        for (std::size_t i = rest; i < stop; i++)
        {
            level = std::max(level, std::abs(reference.samples[i]));
        }
    }
    const int level_steps = static_cast<int>(std::lround(level * 32768.0f));
    const bool sounds_on = level_steps > xsflib::rip::kSilenceLevel;
    if (!sounds_on && level_steps > 0)
    {
        totals.levels++;
        totals.largest_level = std::max(totals.largest_level, level_steps);
    }

    // A seek to the middle of the output, then one back to the second frame.
    const auto [seeks, failed_seeks] = SeekCheck(*model, 0, heard, reference.samples, frame_floats, totals);

    // A switch halfway through the output.
    const std::string switch_text =
        SwitchCheck(archive, sequence, mixing, reference.samples, heard, frames, frame_floats, 0, totals);

    const std::string loop = LoopText(sequence->loop);
    const bool nothing = heard == 0 && !sounds_on && !sequence->loop;
    totals.silent += nothing ? 1 : 0;
    if (differences + seeks + failed_seeks > 0 || sounds_on || short_loop || (heard == 0 && !nothing) ||
        !switch_text.empty())
    {
        std::string after_text = FailedSeeksText(failed_seeks);
        if (sounds_on)
        {
            after_text +=
                ", and the reference sounds after the output's end: " + std::to_string(level_steps) + " steps";
        }
        if (short_loop)
        {
            after_text += ", and the output ends before the loop's end";
        }
        PrintDifferences(name, differences, heard, seeks, after_text, switch_text, loop);
        totals.failed++;
    }

    CheckNotes(name, sequence, reference, heard, frame_floats, loop, totals);
}

// Checks sound `sound`: once with the variables at their starting values, with --variables once for each choice, and
// with --changes once for each choice as a change.
void CheckSound(const std::string& where, const std::shared_ptr<const song::DsArchive>& archive, uint32_t sound,
                int seconds, bool enhanced, bool variables, bool changes, Totals& totals)
{
    const std::string name = where + ": " + song::DsSoundName(*archive, sound);
    totals.sounds++;
    if (!variables && !changes)
    {
        CheckRun(name, archive, sound, {}, {}, seconds, enhanced, totals);
        return;
    }

    song::Sequence plain;
    try
    {
        plain = song::ReadSequence(*archive, sound);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't be read. %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }

    // A change comes 2 ms into the model's frame halfway through the sequence's length or through the check.
    const double middle = std::min(song::MsOfTick(plain, plain.length / 2.0), seconds * 500.0);
    const uint64_t frame = song::FramesIn(static_cast<uint64_t>(middle), song::kDsFrameMs);
    const auto time = static_cast<uint32_t>(frame * song::kDsFrameMs.numerator / song::kDsFrameMs.denominator + 2);
    for (const song::Variable& variable : changes ? plain.read_variables : plain.variables)
    {
        std::vector<int> choices = variable.choices;
        if (choices.empty())
        {
            choices.push_back(1);
        }
        for (const int choice : choices)
        {
            if (choice == -1)
            {
                continue;
            }

            const auto value = static_cast<int16_t>(choice);
            const std::string text = " variable " + std::to_string(variable.number) + " at " + std::to_string(choice);
            if (changes)
            {
                CheckRun(name + " with" + text + " from " + std::to_string(time) + " ms", archive, sound, {},
                         {{time, variable.number, value}}, seconds, enhanced, totals);
            }
            else
            {
                CheckRun(name + " with" + text, archive, sound, {{variable.number, value}}, {}, seconds, enhanced,
                         totals);
            }
        }
    }
}

// The archives of the image or the sound archive at `path`, each with the place that names it.
std::vector<std::pair<std::string, std::shared_ptr<const song::DsArchive>>> ArchivesAt(const std::string& path)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(xsflib::PathFromUtf8(path), bytes))
    {
        throw std::runtime_error("can't be read");
    }

    std::vector<std::pair<std::string, std::shared_ptr<const song::DsArchive>>> archives;
    if (!hokuto::format::IsDsImage(bytes))
    {
        archives.emplace_back(path, song::LoadDsArchive(std::move(bytes)));
        return archives;
    }

    const ntdlib::ntr::snd::Build build = song::DsBuildOf(bytes);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(bytes);
    for (const hokuto::format::DsArchivePlace& place : hokuto::format::DsArchivesIn(bytes))
    {
        const std::span<const uint8_t> data = rom.FileData(place.file).subspan(place.offset, place.size);
        archives.emplace_back(
            path + ": " + place.path,
            song::LoadDsArchive(std::vector<uint8_t>(data.begin(), data.end()), build,
                                song::DsGameOf(bytes, static_cast<uint32_t>(bytes.size()), place.file, place.offset)));
    }

    return archives;
}

// The options of a run: the mixing, the variables or changes, the profiled sounds only, the part of the sounds, the
// images or archives and the seconds of each check.
struct Options
{
    bool enhanced = false;
    bool variables = false;
    bool changes = false;
    bool profiled = false;
    std::size_t part = 0;
    std::size_t parts = 1;
    std::size_t every = 1;
    std::vector<std::string> paths;
    int seconds = 30;
};

// The options in `arguments`, or nothing after a message on stderr.
std::optional<Options> ParseArguments(const std::vector<std::string>& arguments)
{
    Options options;
    for (std::size_t a = 1; a < arguments.size(); a++)
    {
        if (arguments[a] == "--enhanced")
        {
            options.enhanced = true;
        }
        else if (arguments[a] == "--variables")
        {
            options.variables = true;
        }
        else if (arguments[a] == "--changes")
        {
            options.changes = true;
        }
        else if (arguments[a] == "--profiled")
        {
            options.profiled = true;
        }
        else if (arguments[a] == "--part" && a + 1 < arguments.size())
        {
            const std::optional<Part> given = PartOf(arguments[++a]);
            if (!given)
            {
                std::fprintf(stderr, "--part takes k/n, with k from 1 to n.\n");
                return std::nullopt;
            }
            options.part = given->k - 1;
            options.parts = given->n;
        }
        else if (arguments[a] == "--every" && a + 1 < arguments.size())
        {
            const int n = std::atoi(arguments[++a].c_str());
            if (n < 1)
            {
                std::fprintf(stderr, "--every takes a number of 1 or more.\n");
                return std::nullopt;
            }
            options.every = static_cast<std::size_t>(n);
        }
        else if (a + 1 == arguments.size() && !arguments[a].empty() &&
                 arguments[a].find_first_not_of("0123456789") == std::string::npos)
        {
            options.seconds = std::atoi(arguments[a].c_str());
        }
        else if (arguments[a].starts_with("--"))
        {
            std::fprintf(stderr, "Hokuto's DS playback check has no option %s.\n", arguments[a].c_str());
            return std::nullopt;
        }
        else
        {
            options.paths.push_back(arguments[a]);
        }
    }
    if (options.paths.empty() || options.seconds <= 0)
    {
        std::fprintf(stderr,
                     "usage: check_ds_playback [--enhanced] [--variables | --changes] [--profiled] [--part k/n] "
                     "[--every n] <image.nds | archive.sdat>... [seconds]\n");
        return std::nullopt;
    }

    return options;
}

// Checks each sound of the archives at `path` in the part that `options` selects.
void CheckPath(const std::string& path, const Options& options, Totals& totals)
{
    std::vector<std::pair<std::string, std::shared_ptr<const song::DsArchive>>> archives;
    try
    {
        archives = ArchivesAt(path);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s\n", path.c_str(), error.what());
        totals.unread++;
        return;
    }
    if (archives.empty())
    {
        std::printf("%s: no sound archive\n", path.c_str());
    }

    for (const auto& [where, archive] : archives)
    {
        totals.archives++;
        std::vector<uint32_t> sounds = song::DsSounds(*archive);
        if (options.profiled)
        {
            std::erase_if(sounds, [&archive](uint32_t sound) { return !SetsMore(ProfileOf(*archive, sound)); });
            for (const uint32_t sound : sounds)
            {
                std::printf("%s: %s takes its game's profile.\n", where.c_str(),
                            song::DsSoundName(*archive, sound).c_str());
            }
        }
        for (std::size_t s = 0; s < sounds.size(); s++)
        {
            if (s % options.every == 0 && s / options.every % options.parts == options.part)
            {
                CheckSound(where, archive, sounds[s], options.seconds, options.enhanced, options.variables,
                           options.changes, totals);
            }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    const std::optional<Options> options = ParseArguments(arguments);
    if (!options)
    {
        return 2;
    }

    Totals totals;
    for (const std::string& path : options->paths)
    {
        CheckPath(path, *options, totals);
    }

    std::printf(
        "%zu archives, %zu sounds, %zu runs, %zu with a loop, %zu can't be read, %zu differ, %zu play nothing. "
        "%zu seeks. %zu of %zu switches exact. %zu of %zu notes missing from the arrangement, %zu after the first "
        "pass. %zu ties, %zu of them separate notes in the arrangement. After the output's end, the reference "
        "holds samples other than 0 within ±8 in %zu runs, %d steps at most. %zu runs with the game's output effect, "
        "and %zu whose game's profile sets more.\n",
        totals.archives, totals.sounds, totals.runs, totals.looped, totals.unread, totals.failed, totals.silent,
        totals.seeks, totals.exact, totals.switches, totals.missed_notes, totals.notes, totals.missed_later,
        totals.ties, totals.arranged_ties, totals.levels, totals.largest_level, totals.effects, totals.profiled);
    if (options->changes)
    {
        std::printf("The change altered the output of %zu runs.\n", totals.altered);
    }

    return totals.failed + totals.unread > 0 ? 1 : 0;
}
