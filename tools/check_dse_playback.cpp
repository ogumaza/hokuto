// SPDX-License-Identifier: MIT

// Checks Hokuto's playback of DSE's songs and sound effects in DS cartridge images against ntdlib's dse::Player, run
// directly a tick of DSE's driver at a time. The images' sounds come from song::LoadDseSet, played by the build of DSE
// in each image's ARM9 binary. For each sound (song::DseSounds), it checks:
//
// - From the start, the player's output matches the reference output sample for sample, after the player's first frame.
//   Each frame of the player contains two frames of the sound driver. A player from tick 0 starts with the frame that
//   plays tick 0 (SequenceModel::Seek): the model's first frame after which the position is past 0 (StartFrame). The
//   reference output starts with the model's frame 0. The comparison therefore skips the reference's frames before the
//   player's first.
// - The output of a sound without a loop that ends early ends where the reference has fallen silent: within ±8 of 0
//   on each side. A reference that stays at another level within that is counted apart. With Cycle off, the output of
//   a sound with a loop ends at the end of the loop's first pass. The reference plays on round the loop. The output
//   has to reach the loop's end.
// - Seeks to the middle of the output and to the player's second frame give the reference's frames. A seek that
//   produces no output counts as a difference.
// - A switch halfway through the output, to a new model of the same sound, produces the frames of the reference after
//   the frame that crossfades. A catch-up runs the new model up to the player's frame first. The program switches the
//   same way after an edit during playback.
// - Each note that a track plays on a voice up to the output's end is in the arrangement, on its track and key, at the
//   tick that played it. The arrangement shows the first pass of a loop, and the later passes round it. A legato that
//   goes on in the voices of the previous key of its track continues the note before it in the arrangement. The tool
//   counts the legatos apart, and those that the arrangement shows as separate notes. It counts the notes missing after
//   the first pass apart too. A note that starts no voice plays nothing. The tool counts such notes apart.
// - A sound whose output ends before its first frame has to leave the reference silent. The tool counts such sounds.
//
// With --enhanced, the player and the reference play in enhanced mixing. With --every n, the tool checks every nth
// sound of each image, and with --part k/n, part k of n of those. The tool prints a line for each sound that differs,
// and a summary. It returns 1 when any sound differs or can't be read. Missing notes get a line too, but don't count as
// a difference.
//
//     check_dse_playback [--enhanced] [--part k/n] [--every n] <image.nds>... [seconds]

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/sound.h"
#include "xsflib/console.h"
#include "xsflib/files.h"
#include "xsflib/rip/length.h"

#include "check_part.h"
#include "check_playback_steps.h"
#include "check_support.h"
#include "song/dse.h"
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

// A note that a track of the followed sequence played: the samples of the reference before the tick that played it, the
// tick, its track and the key of its voices, whether it continued as a legato, and whether it started a voice. A
// channel with random keys assigns the voices another key than the event of the note. The arrangement shows the voices'
// key.
struct PlayedNote
{
    std::size_t at = 0;
    uint32_t tick = 0;
    int track = 0;
    int key = 0;
    bool tie = false;
    bool voiced = false;
};

// What ntdlib's player produces when it runs directly: the output at a full scale of 1, and the notes.
struct Reference
{
    std::vector<float> samples;
    std::vector<PlayedNote> notes;
};

struct Totals : StepTotals
{
    std::size_t images = 0;
    std::size_t sounds = 0;
    std::size_t runs = 0;
    std::size_t looped = 0;
    std::size_t unread = 0;
    std::size_t failed = 0;
    std::size_t notes = 0;
    std::size_t missed_notes = 0;
    std::size_t missed_later = 0;
    std::size_t ties = 0;          // legatos
    std::size_t arranged_ties = 0; // legatos that the arrangement shows as separate notes
    std::size_t unvoiced = 0;      // notes that start no voice
    std::size_t silent = 0;        // runs whose output ends before its first frame
    std::size_t levels = 0;        // runs whose reference has samples other than 0 within ±8 after the output's end
    int largest_level = 0;         // the largest such sample, in steps
};

Reference RunReference(const song::DseSet& set, uint32_t sound, bool enhanced, std::size_t samples)
{
    ntdlib::dse::Player player({.build = set.build, .enhanced = enhanced});
    Reference reference;
    int64_t ticks = 0;
    std::vector<ntdlib::ntr::SoundOutput> strict;
    ntdlib::dse::SequenceHooks hooks;
    hooks.on_tick = [&ticks]
    {
        ticks++;
    };

    // The player's tick comes after the output up to it. In strict mixing, that output waits in `strict` until the run
    // ends. Each entry of `strict` contains a stereo sample.
    hooks.on_note = [&player, &reference, &ticks, &strict](const ntdlib::dse::NoteEvent& note)
    {
        if (ticks > 0)
        {
            const uint8_t key =
                note.voices != 0 ? player.Driver().GetVoice(std::countr_zero(note.voices)).key : note.key;
            reference.notes.push_back({reference.samples.size() + 2 * strict.size(), static_cast<uint32_t>(ticks - 1),
                                       note.track, key, note.legato, note.voices != 0});
        }
    };
    player.Driver().SetHooks(std::move(hooks));
    const ntdlib::dse::SequenceFiles files = set.sounds[sound].Files();
    player.LoadBanks(files);
    if (!player.Start(files))
    {
        throw std::runtime_error("DSE refuses it");
    }

    // The reference runs a tick of DSE's driver at a time, the player two of the sound driver's frames at a time.
    std::vector<int16_t> pcm;
    while (reference.samples.size() < samples * 2)
    {
        if (enhanced)
        {
            player.Run(ntdlib::dse::Player::kTickClocks, reference.samples);
        }
        else
        {
            strict.clear();
            pcm.clear();
            player.Run(ntdlib::dse::Player::kTickClocks, strict);
            ntdlib::ntr::AppendPcm16(strict, pcm);
            for (const int16_t sample : pcm)
            {
                reference.samples.push_back(static_cast<float>(sample) / 32768.0f);
            }
        }
    }

    return reference;
}

// The model's frame that a player from tick 0 starts with: the first frame after which the position is past 0.
std::size_t StartFrame(const std::shared_ptr<const song::DseSet>& set,
                       const std::shared_ptr<const song::Sequence>& sequence, song::Mixing mixing)
{
    song::SequenceModel probe(set, sequence, song::ModelOptions{.mixing = mixing});
    std::size_t frame = 0;
    for (bool more = probe.SeekFrame(0); more && probe.After() <= 0.0; more = probe.RunFrame())
    {
        frame++;
    }

    return frame;
}

// Counts the notes of `reference` up to `heard` frames of output after `offset`, and those that the arrangement of
// `sequence` misses.
void CheckNotes(const std::string& name, const std::shared_ptr<const song::Sequence>& sequence,
                const Reference& reference, std::size_t heard, std::size_t frame_floats, std::size_t offset,
                const std::string& loop, Totals& totals)
{
    // The notes up to the output's end.
    std::size_t checked = 0;
    std::size_t missed = 0;
    std::size_t missed_later = 0;
    std::size_t ties = 0;
    std::size_t arranged_ties = 0;
    std::size_t unvoiced = 0;
    for (const PlayedNote& note : reference.notes)
    {
        if (note.at > offset + heard * frame_floats)
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
        if (!note.voiced)
        {
            unvoiced++;
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
    totals.unvoiced += unvoiced;
    if (missed > 0)
    {
        std::printf("%s (loop %s): %zu of %zu notes missing from the arrangement, %zu after the first pass.\n",
                    name.c_str(), loop.c_str(), missed, checked, missed_later);
    }
}

// Checks sound `sound`, for `seconds` at most.
void CheckSound(const std::string& name, const std::shared_ptr<const song::DseSet>& set, uint32_t sound, int seconds,
                bool enhanced, Totals& totals)
{
    totals.sounds++;
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*set, sound));
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't be read. %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }
    totals.runs++;
    totals.looped += sequence->loop ? 1 : 0;

    const song::Mixing mixing = enhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict;
    const auto model = std::make_shared<song::SequenceModel>(set, sequence, song::ModelOptions{.mixing = mixing});
    const std::size_t frame_samples = model->FrameSamples();
    const std::size_t frame_floats = frame_samples * 2;
    const std::size_t frames = CheckFrames(*model, seconds);
    const std::size_t start = StartFrame(set, sequence, mixing);
    const std::size_t offset = start * frame_floats;
    Reference reference;
    try
    {
        reference = RunReference(*set, sound, enhanced, (start + frames) * frame_samples);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the reference can't play it. %s\n", name.c_str(), error.what());
        totals.failed++;
        return;
    }

    // From the start. The output's last frame can fade out, and the comparison leaves it out.
    StartResult from_start;
    try
    {
        from_start = StartCheck(model, frames, reference.samples, offset);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the player can't play it. %s\n", name.c_str(), error.what());
        totals.failed++;
        return;
    }
    const auto [ended, heard, differences] = from_start;

    // A loop's output has to reach the end of the loop's first pass. A model that fails or falls silent ends it sooner.
    const double reached = model->After();
    const bool short_loop = ended && sequence->loop && reached < static_cast<double>(sequence->loop->end);

    // After an end of the output, the reference has to stay within ±8 of 0. The reference plays on round a loop.
    const std::size_t rest = std::min(offset + heard * frame_floats, reference.samples.size());
    const std::size_t stop = std::min(offset + frames * frame_floats, reference.samples.size());
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
    const auto [seeks, failed_seeks] = SeekCheck(*model, start, heard, reference.samples, frame_floats, totals);

    // A switch halfway through the output.
    const std::string switch_text =
        SwitchCheck(set, sequence, mixing, reference.samples, heard, frames, frame_floats, offset, totals);

    const std::string loop = LoopText(sequence->loop);
    const bool nothing = heard == 0 && !sounds_on && !sequence->loop;
    totals.silent += nothing ? 1 : 0;
    if (differences + seeks + failed_seeks > 0 || sounds_on || short_loop || (heard == 0 && !nothing) ||
        !switch_text.empty())
    {
        std::string after_text =
            sounds_on ? ", and the reference sounds after the output's end: " + std::to_string(level_steps) + " steps"
                      : "";
        after_text += FailedSeeksText(failed_seeks);
        if (short_loop)
        {
            after_text +=
                ", and the output ends at tick " + std::to_string(std::lround(reached)) + ", before the loop's end";
        }
        PrintDifferences(name, differences, heard, seeks, after_text, switch_text, loop);
        totals.failed++;
    }

    CheckNotes(name, sequence, reference, heard, frame_floats, offset, loop, totals);
}

// The DSE sounds of the image at `path`.
std::shared_ptr<const song::DseSet> SetAt(const std::string& path)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(xsflib::PathFromUtf8(path), bytes))
    {
        throw std::runtime_error("can't be read");
    }

    return song::LoadDseSet(bytes);
}

// The options of a run: the mixing, the part of the sounds, the images and the seconds of each check.
struct Options
{
    bool enhanced = false;
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
            std::fprintf(stderr, "Hokuto's DSE playback check has no option %s.\n", arguments[a].c_str());
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
                     "usage: check_dse_playback [--enhanced] [--part k/n] [--every n] <image.nds>... [seconds]\n");
        return std::nullopt;
    }

    return options;
}

// Checks each sound of the image at `path` in the part that `options` selects.
void CheckPath(const std::string& path, const Options& options, Totals& totals)
{
    std::shared_ptr<const song::DseSet> set;
    try
    {
        set = SetAt(path);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s\n", path.c_str(), error.what());
        totals.unread++;
        return;
    }
    for (const std::string& warning : set->warnings)
    {
        std::printf("%s: %s\n", path.c_str(), warning.c_str());
    }
    if (set->sounds.empty())
    {
        std::printf("%s: no DSE sounds\n", path.c_str());
    }

    totals.images++;
    const std::vector<uint32_t> sounds = song::DseSounds(*set);
    for (std::size_t s = 0; s < sounds.size(); s++)
    {
        if (s % options.every == 0 && s / options.every % options.parts == options.part)
        {
            CheckSound(path + ": " + song::DseSoundFolder(*set, sounds[s]) + song::DseSoundName(*set, sounds[s]), set,
                       sounds[s], options.seconds, options.enhanced, totals);
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
        "%zu images, %zu sounds, %zu runs, %zu with a loop, %zu can't be read, %zu differ, %zu play nothing. "
        "%zu seeks. %zu of %zu switches exact. %zu of %zu notes missing from the arrangement, %zu after the first "
        "pass. %zu legatos, %zu of them separate notes in the arrangement. %zu notes start no voice. After the "
        "output's end, the reference holds samples other than 0 within ±8 in %zu runs, %d steps at most.\n",
        totals.images, totals.sounds, totals.runs, totals.looped, totals.unread, totals.failed, totals.silent,
        totals.seeks, totals.exact, totals.switches, totals.missed_notes, totals.notes, totals.missed_later,
        totals.ties, totals.arranged_ties, totals.unvoiced, totals.levels, totals.largest_level);

    return totals.failed + totals.unread > 0 ? 1 : 0;
}
