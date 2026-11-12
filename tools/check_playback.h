// SPDX-License-Identifier: MIT

// The parts of hokuto_check_playback that its files share: the totals, the measures of closeness, the checks of notes
// and the checks of each console. check_playback.cpp describes the checks.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/ctr/dsp/quiet_stdout.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/ctr_fx/effects.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sound_build.h"
#include "xsflib/console.h"
#include "xsflib/files.h"
#include "xsflib/rip/length.h"

#include "check_playback_steps.h"
#include "check_support.h"
#include "song/archive.h"
#include "song/catch_up.h"
#include "song/ctr_model.h"
#include "song/frame_resampler.h"
#include "song/player.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::tools::playback
{

namespace atk = ntdlib::nx::atk;
namespace cafe = ntdlib::cafe::snd;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;

// The stereo frames of a frame of the model, and the model's frames in a second.
constexpr std::size_t kFrameSamples = atk::ArchiveModel::kFrameSamples;
constexpr song::FrameMs kFrameMs = song::FrameMsOf(sar::Flavor::kNx);
constexpr std::size_t kFramesPerSecond = 200;

// The render worker's blocks, in stereo frames.
using playback_steps::kBlock;

// The tracks of a sequence player.
constexpr int kTracks = 16;

// The strict rules for the Switch's archives.
const sar::Options kStrict{sar::Flavor::kNx, {}, false, sar::Rules::kStopAtFirstProblem};

// The level, in dB below full scale, under which a difference passes whatever its ratio. A quiet passage can lie only a
// few steps of the 16-bit output above silence. An integer delay line's tail can then hold steps of ±1 that the lead
// doesn't settle, such as in a Wii U game's countdown with a delay on bus A (2026-10-24).
constexpr double kFloorDb = -80.0;

// How close an output lies to the model's: the ratio of the model's output to the difference, in dB, and the
// difference's mean power, in dB below full scale. Infinity stands for no difference.
struct Closeness
{
    double ratio = std::numeric_limits<double>::infinity();
    double level = -std::numeric_limits<double>::infinity();
};

// The closeness of a difference of power `difference` from a signal of power `signal`, both summed over `count` samples
// on the scale of full scale.
Closeness ClosenessOf(double signal, double difference, std::size_t count);

// The closeness of `samples` to `expected`, over the samples of both.
Closeness ClosenessOf(std::span<const float> samples, std::span<const float> expected);

// A note that the model started: its track and key, and whether it continued a channel that was sounding. A note of a
// track with tie or mono on does that: a legato.
struct NoteStart
{
    int track = 0;
    int key = 0;
    bool legato = false;
};

// The model's output, the sequence player's tick count before and after each frame, the position before and after each
// frame, whether a sound still played or a channel still sounded before each frame, and the notes that each frame
// started. The position is the tick count until the sequence ends. It then goes on at the tempo at the end.
struct ModelRun
{
    std::vector<int16_t> samples;
    std::vector<float> floats; // the enhanced mode's output in floating point, or none
    std::vector<uint64_t> before;
    std::vector<uint64_t> after;
    std::vector<double> position_before;
    std::vector<double> position_after;
    std::vector<bool> busy;
    std::vector<std::vector<NoteStart>> notes;
};

// The player's output and the clock's position at the start of each frame. The worker renders in blocks, and the
// program reads the clock soon after each block. The run does the same.
struct PlayerRun
{
    std::vector<float> samples;
    std::vector<std::optional<double>> marks;
    bool ended = false;
};

// Adds `frames` frames of the player's output to `run`.
void Render(song::SequencePlayer& player, const song::PlaybackClock& clock, std::size_t frames, PlayerRun& run);

// The same for a seek, with the model's output over the second before frame `lead_start` as the signal where that's
// louder: the lead before the seek's place starts at that frame (see the file's comment).
Closeness SeekCloseness(const PlayerRun& run, const ModelRun& model, std::size_t first, std::size_t skip,
                        std::size_t frames, std::size_t lead_start);

// The notes that the model starts in its first `frames` frames, those that the arrangement lacks, those of them after
// the first pass of the loop, and those of them that are legatos. The arrangement shows a legato as part of the note
// that it continues.
struct NoteCheck
{
    std::size_t checked = 0;
    std::size_t missed = 0;
    std::size_t missed_later = 0;
    std::size_t missed_legatos = 0;
};

// The check of the notes that `model`, a run of a console's model, starts in its first `frames` frames, against the
// arrangement of `sequence`.
template <class Run>
NoteCheck CheckNotes(const Run& model, const song::Sequence& sequence, std::size_t frames)
{
    // A track's notes by their position and key.
    std::map<int, std::map<std::pair<uint32_t, int>, int>> notes;
    for (const song::Track& track : sequence.tracks)
    {
        for (const song::Note& note : track.notes)
        {
            notes[track.number][{note.start, note.key}]++;
        }
    }

    NoteCheck check;
    for (std::size_t frame = 0; frame < model.notes.size() && frame < frames; frame++)
    {
        for (const NoteStart& start : model.notes[frame])
        {
            const auto track = notes.find(start.track);
            bool found = false;
            for (uint64_t tick = model.before[frame]; tick < model.after[frame] && track != notes.end(); tick++)
            {
                const uint32_t position = song::PositionOfTicks(sequence, tick);
                found = found || track->second.contains({position, start.key});
            }
            check.checked++;
            check.missed += found ? 0 : 1;
            check.missed_later += !found && sequence.loop && model.before[frame] >= sequence.loop->end ? 1 : 0;
            check.missed_legatos += !found && start.legato ? 1 : 0;
        }
    }

    return check;
}

// The starting values for a summary line, such as " with variable 0 at 1", or nothing.
std::string ValuesText(const song::StartValues& values);

// The changes for a summary line, such as " with variable 0 set to 1 at 7502 ms", or nothing.
std::string ChangesText(const song::Changes& changes);

// Totals over the archive. With --variables or --changes, `sequences` counts the runs of each choice. `affected` counts
// the runs whose model output the changes alter. `quiet_tails` counts the sequences of the Wii U and the 3DS whose
// output ended while the effect still gave samples of ±8 at most. `sent` counts the runs of the 3DS that sent sound to
// bus A. `tickless` counts the 3DS sequences in which archive mode runs no tick. `switches` counts the switches
// halfway, and `exact` those whose output matched the model's after the frame that crossfades. `lowest` gives the
// lowest ratio after a switch. The stretches that pass by the floor (kFloorDb) don't count. `floored` counts the
// switches with such a stretch, and `loudest` gives the loudest difference among them. The fields that end in `_seek`
// or `_seeks` give the same for the seeks that the ratio measures.
struct Totals
{
    std::size_t sequences = 0;
    std::size_t failed = 0;
    std::size_t unread = 0;
    std::size_t looped = 0;
    std::size_t notes = 0;
    std::size_t missed_notes = 0;
    std::size_t missed_later = 0;
    std::size_t missed_legatos = 0;
    std::size_t affected = 0;
    std::size_t altered = 0;
    std::size_t quiet_tails = 0;
    std::size_t sent = 0;
    std::size_t tickless = 0;
    std::size_t switches = 0;
    std::size_t exact = 0;
    double lowest = std::numeric_limits<double>::infinity();
    std::size_t floored = 0;
    double loudest = -std::numeric_limits<double>::infinity();
    std::size_t seeks = 0;
    std::size_t exact_seeks = 0;
    double lowest_seek = std::numeric_limits<double>::infinity();
    std::size_t floored_seeks = 0;
    double loudest_seek = -std::numeric_limits<double>::infinity();
};

// Adds a switch to `totals`, and returns whether it passes.
bool AddSwitch(std::initializer_list<Closeness> stretches, Totals& totals);

// Adds a seek to `totals`, and returns whether it passes.
bool AddSeek(std::initializer_list<Closeness> stretches, Totals& totals);

// The worse ratio of `stretches`.
double WorstRatio(std::initializer_list<Closeness> stretches);

// The ratio after a switch as text, such as "72.4 dB".
std::string RatioText(double ratio);

// The summary's line about the switches, if any ran.
void PrintSwitches(const Totals& totals);

// Calls `check` with the starting values and the changes of a run once for each choice of each variable of `read`. A
// variable without choices has the value 1, and a choice of -1 is left out. With `changes`, each choice comes as a
// change at `time`, and otherwise as the variable's starting value.
template <class CheckFn>
void ForEachChoice(const std::vector<song::Variable>& read, bool changes, uint32_t time, const CheckFn& check)
{
    for (const song::Variable& variable : read)
    {
        const std::vector<int> choices = variable.choices.empty() ? std::vector{1} : variable.choices;
        for (const int value : choices)
        {
            if (value == -1)
            {
                continue;
            }

            const auto set = static_cast<int16_t>(value);
            if (changes)
            {
                check(song::StartValues{}, song::Changes{{time, variable.number, set}});
            }
            else
            {
                check(song::StartValues{{variable.number, set}}, song::Changes{});
            }
        }
    }
}

// Checks sequence sound `sound` with `effects` on the aux buses, the variables' starting values `values` and the game's
// `changes`, and prints a line when it differs. `options` give the model the same effects and mixing mode.
void CheckSequence(const sar::SoundArchive& strict, const std::shared_ptr<const sar::SoundArchive>& archive,
                   uint32_t sound, std::size_t frames, const atk::EngineOptions& options,
                   const song::AuxEffects& effects, const song::StartValues& values, const song::Changes& changes,
                   Totals& totals);

// The Wii U's frames, and the strict rules for its archives.
constexpr std::size_t kWiiUFrameSamples = cafe::ArchiveModel::kFrameSamples;
constexpr song::FrameMs kWiiUFrameMs = song::FrameMsOf(sar::Flavor::kCafe);
static_assert(kWiiUFrameMs.denominator == 1);
constexpr std::size_t kWiiUFramesPerSecond = 1000 / kWiiUFrameMs.numerator;
const sar::Options kWiiUStrict{sar::Flavor::kCafe, {}, false, sar::Rules::kStopAtFirstProblem};

// The output of ntdlib's model of the Wii U, run directly for `frames` frames, and the tick count after each frame. In
// enhanced mixing, `floats` holds the output in floating point.
struct WiiURun
{
    std::vector<int16_t> samples;
    std::vector<float> floats;
    std::vector<uint64_t> after;
};

// The model's output from its first frame on, frame by frame, until it stops or `samples` samples have come out.
std::vector<int16_t> RunModelFrames(song::SequenceModel& model, std::size_t samples);

// The output of a player of `model` from the start, rendered in blocks of kBlock stereo frames as the worker renders
// it, until a block comes out short or `samples` samples have come out.
std::vector<float> RunPlayerBlocks(const std::shared_ptr<song::SequenceModel>& model, std::size_t samples);

// The first frame that runs a tick, from the tick counts `after` each frame of a model's run, or the frame after the
// run's end when none does.
std::size_t FirstTickFrame(std::span<const uint64_t> after);

// Whether the player's output, `heard` frames from the model's frame `first` on, ended before the check's frames ran
// out and more than a frame before frame `end` of the model. The frame that fades out can end the output a frame early.
bool EndsEarly(std::size_t first, std::size_t heard, std::size_t frames, std::size_t end);

// The frames of a model's run, with the ticks `after` each frame, up to the frame that reaches tick `loop_end`, or
// `after`'s frames when none does. The player ends a looped sequence's output with that frame.
std::size_t LoopEndFrames(std::span<const uint64_t> after, uint64_t loop_end);

// The frames of `samples` from frame `from` on that differ from `expected`'s frames from `first` on, up to the end of
// either. A frame of `samples` matches when each of its samples is the expected one at full scale. In enhanced mixing,
// floats have to match the expected floats.
template <class Sample>
std::size_t WiiUDifferences(std::span<const Sample> samples, std::size_t from, const WiiURun& expected,
                            std::size_t first, float scale, std::size_t* first_difference = nullptr)
{
    const bool floats_expected = std::is_same_v<Sample, float> && !expected.floats.empty();
    std::size_t differences = 0;
    const std::size_t kFloats = kWiiUFrameSamples * 2;
    for (std::size_t frame = from;
         (frame + 1) * kFloats <= samples.size() && (first + frame + 1) * kFloats <= expected.samples.size(); frame++)
    {
        const std::size_t at = (first + frame) * kFloats;
        for (std::size_t i = 0; i < kFloats; i++)
        {
            const float sample = static_cast<float>(samples[frame * kFloats + i]);
            const float wanted =
                floats_expected ? expected.floats[at + i] : static_cast<float>(expected.samples[at + i]) * scale;
            if (sample != wanted)
            {
                if (differences == 0 && first_difference != nullptr)
                {
                    *first_difference = frame;
                }
                differences++;
                break;
            }
        }
    }

    return differences;
}

// The effects of the Wii U by the names that a Hokuto file gives them.
constexpr std::array<std::pair<std::string_view, cafe::EffectType>, 4> kWiiUEffects = {{
    {"reverb_hi", cafe::EffectType::kReverbHi},
    {"reverb_std", cafe::EffectType::kReverbStd},
    {"chorus", cafe::EffectType::kChorus},
    {"delay", cafe::EffectType::kDelay},
}};

// An effect of the Wii U on the TV's aux bus A, for the model run directly and for Hokuto's model.
struct WiiUEffects
{
    std::vector<cafe::EffectSetup> setups;
    song::AuxEffects effects;
};

// The player's output, rendered in blocks up to `frames` frames of `frame_samples` stereo samples in all. Returns false
// once the output has ended.
bool RenderFrames(song::SequencePlayer& player, std::size_t frames, std::size_t frame_samples,
                  std::vector<float>& output);

// Checks Hokuto's playback of sequence sound `sound` of a Wii U archive against the model run directly: the frames of a
// run from the start, the frames after seeks back and forth, and the player's output after its first frame. That frame
// fades in. The player plays once, to the end of its loop's pass or until its last note has faded. Its last frame can
// fade out. With an effect or in enhanced mixing, a switch halfway through the sequence follows. Prints a line when
// anything differs.
void CheckWiiUSequence(const sar::SoundArchive& strict, const std::shared_ptr<const sar::SoundArchive>& archive,
                       uint32_t sound, std::size_t frames, const WiiUEffects& effects, song::Mixing mixing,
                       Totals& totals);

// The 3DS's frames. Hokuto's frames of the 3DS hold the DSP's output after three frames of silence, or the enhanced
// mode's frames after four (song::CtrModel).
constexpr song::FrameMs kCtrFrameMs = song::FrameMsOf(sar::Flavor::kCtr);

// An effect of the 3DS on aux bus A at the libraries' defaults, by the name that a Hokuto file gives its kind: a reverb
// of a library, or a delay. nw::snd's delay and nn::snd's compute the same.
struct CtrEffect
{
    std::string_view name;
    bool reverb;
    ntdlib::ctr_fx::Library library;
};

constexpr std::array<CtrEffect, 5> kCtrEffects = {{
    {"3ds_reverb", true, ntdlib::ctr_fx::Library::kNwSnd},
    {"3ds_delay", false, ntdlib::ctr_fx::Library::kNwSnd},
    {"3ds_nn_reverb", true, ntdlib::ctr_fx::Library::kNnSnd},
    {"3ds_nn_delay", false, ntdlib::ctr_fx::Library::kNnSnd},
    {"3ds_nn_reverb_2011", true, ntdlib::ctr_fx::Library::kNnSnd2011},
}};

// Checks Hokuto's playback of sequence sound `sound` of a 3DS archive against archive mode, run directly on `firmware`,
// or with `enhanced` on the model in its enhanced mode, with the variables' starting values `values` and the game's
// `changes`: the player's output after its first frame, the silence of the reference after the output's end, the frames
// after seeks, a switch halfway to a new model, and the notes. The player's first frame fades in. In enhanced mixing,
// the switch comes from a model in strict mixing, as after a click on the mixer's switch. The player then keeps strict
// mixing's rate. With `effect` on bus A, both play the effect, and the switch comes from a model without it. The
// reference then only has to stay within ±8 after the output's end. The player plays once, to the end of its loop's
// pass or until its last note has faded, and an effect's tail after it. Its last frame can fade out. Prints a line when
// anything differs, and one when notes are missing from the arrangement.
void CheckCtrSequence(std::span<const uint8_t> firmware, const sar::SoundArchive& strict,
                      const std::shared_ptr<const sar::SoundArchive>& archive, uint32_t sound, std::size_t frames,
                      const song::StartValues& values, const song::Changes& changes, bool enhanced,
                      const CtrEffect* effect, Totals& totals);

// The effects of the Wii by the names that a Hokuto file gives them.
constexpr std::array<std::pair<std::string_view, ntdlib::rvl::snd::EffectType>, 6> kWiiEffects = {{
    {"wii_reverb_hi", ntdlib::rvl::snd::EffectType::kReverbHi},
    {"wii_reverb_hi_2008", ntdlib::rvl::snd::EffectType::kReverbHi2008},
    {"wii_chorus", ntdlib::rvl::snd::EffectType::kChorus},
    {"wii_delay", ntdlib::rvl::snd::EffectType::kDelay},
    {"wii_reverb_std", ntdlib::rvl::snd::EffectType::kReverbStd},
    {"wii_reverb_std_2008", ntdlib::rvl::snd::EffectType::kReverbStd2008},
}};

// An effect of the Wii on aux bus A, for WSF's archive mode and for Hokuto's model.
struct WiiEffects
{
    std::vector<ntdlib::rvl::snd::EffectSetup> setups;
    song::AuxEffects effects;
};

// Checks every sequence of the Wii archive at `path` for `seconds`, against WSF's archive mode with the microcode and
// the build of NW4R SND in the main.dol at `dol_path`. With `variables` or `changes`, each choice of each variable that
// a sequence reads gets a run, as on the 3DS. Only the sounds whose index leaves `part` after a division by `parts`
// take part. Returns the tool's exit code.
int CheckWiiArchive(const std::filesystem::path& path, const std::filesystem::path& dol_path, int seconds,
                    bool variables, bool changes, bool archive_build, std::size_t part, std::size_t parts,
                    const WiiEffects& effects, std::string_view effect_name);

} // namespace hokuto::tools::playback
