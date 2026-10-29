// SPDX-License-Identifier: MIT

// Checks Hokuto's playback of a Switch sound archive against ntdlib's model of nn::atk, run directly. For each sequence
// sound, it checks:
//
// - From the start, the player's output matches the model's from the model's first frame that runs a tick. The model
//   makes no sound before that frame.
// - From a seek, to the loop's start or to the middle of the sequence, the output matches the model's after the first
//   frame. The first frame fades in. The seek restores a snapshot that the run from the start took. A seek that lies
//   further past the snapshot than 2 s and the lead mixes only the lead before its place. A catch-up does the same
//   (see --reverb). With an effect or in enhanced mixing, the output after the first frame therefore has to lie close
//   to the model's. The model's output has to be 60 dB or more above the difference: over the next second, and over
//   the rest. The effect then lacks the tail of what came before the lead. The lead lets that tail fall by 60 dB. A
//   seek can land in the quiet tail of a sound, such as the end of a jingle. The model's output over the second
//   before the lead therefore counts instead where it's louder. A difference 80 dB or more below full scale passes
//   too: a quiet passage can lie only a few steps of the 16-bit output above silence. A seek past the point where the
//   output has faded ends the output at once. The output fades once nothing plays or sounds and a frame has come out
//   silent. An effect's tail sounds on after the last channel.
// - Output that ends early ends where the model's output has faded.
// - The clock gives each frame's position: the model's tick count before the frame, round the loop. Once a sequence
//   without a loop has ended, the position goes on at the engine's tempo at the end. After a seek, the first frame's
//   position is the start.
// - Each note that the model starts is in the arrangement, on its track and key, at a position that the frame's ticks
//   reach.
//
// The model reads the archive by the strict rules. They stop at the first problem. The player reads the archive as the
// program opens it, with the group files beside it. The tool prints a line for each sequence that differs, and a
// summary. It returns 1 when any sequence differs or can't be read. A sequence with notes missing from the arrangement
// gets a line too, but doesn't count as a difference. The notes that the model cuts in the tick they start are missing.
// They're inaudible. The arrangement shows one pass of a loop. A later pass can differ from it. A random value can
// change a note's key or a wait. A track can also loop by itself, over a length other than the sequence's loop. The
// notes missing after the first pass are counted apart.
//
// With --reverb n, both play with nn::audio's I3DL2 preset n on aux bus A. Most of the Switch's music sends to bus A.
// One more run then plays without the reverb, and switches to a model with it halfway. The program switches the same
// way after an edit during playback. A catch-up runs the new model up to the player's frame first. It mixes only the
// lead before that frame (song::ModelCatchUp). The reverb then lacks the tail of what came before the lead. After the
// frame that crossfades, the output has to lie close to the model's with the reverb. Over the next second, the model's
// output has to be 60 dB or more above the difference, or the difference 80 dB or more below full scale. The summary
// gives the lowest ratio, and counts the switches whose output matches the model's exactly and those that pass by the
// floor alone.
//
// With --variables, the checks run once for each choice of each variable that a sequence reads, -1 aside (see
// song::Variable). A variable without choices runs once, with 1. The player plays the sequence with the variable at
// that starting value, and the arrangement reads it that way too. The model runs with the value set before its first
// frame. A game sets it at the same point, through the sound's handle. The runs with -1 are the checks without the
// option.
//
// With --changes, the checks run once for each choice of each variable that a sequence reads at any time, -1 aside. A
// variable without choices runs once, with 1. The variable takes the value while the sequence plays: halfway through
// the sequence's length, or halfway through the check if that comes first, 2 ms after the start of a frame. The player
// plays the sequence with the change, and the arrangement reads it that way too. The model runs with the value set at
// the start of the change's frame (song::ChangeFrame). A game sets it between two frames too, through the sound's
// handle. The summary counts the runs whose model output differs from the run without the change.
//
// A Wii U archive gets simpler checks against ntdlib's model of nw::snd and AX, run directly with the group files
// beside the archive: the frames of Hokuto's model from the start and after seeks back and forth, and the player's
// output after its first frame. The options above are for the Switch's archives.
//
// With --effect kind, a Wii U archive's checks play with an effect of the Wii U on the TV's aux bus A, at Hokuto's
// starting values (song::WiiUStartingParameters). The kinds have the names of a Hokuto file: reverb_hi, reverb_std,
// chorus and delay. One more run then plays without the effect, and switches to a model with it halfway through the
// player's output. After the frame that crossfades, the output has to lie close to the model's with the effect, as with
// --reverb. A seek far past the last snapshot mixes only the lead before its frame. With an effect or in enhanced
// mixing, the frame after such a seek has to lie close to the model's by the Switch's rule. The second before the lead
// counts here too.
//
// With --enhanced, the model and the player play in enhanced mixing. The player's output has to be the model's output
// in floating point, sample for sample. One more run then plays in strict mixing, without the effects, and switches to
// the enhanced model halfway, as after a click on the mixer's switch. After the frame that crossfades, the output has
// to lie close to the enhanced model's, as with --reverb. On a Wii U archive, the frames from the start and after seeks
// are the model's output rounded to 16 bits.
//
// A 3DS archive plays in Hokuto's player on ntdlib's model of the DSP's audio firmware. The reference is ntdlib's
// archive mode of the 3DS with Teakra running the DSP firmware that --firmware names, such as a game's dspaudio.cdc.
// The reference runs directly like a player of the 3SF format (ntdlib/ctr/snd/engine.h): the firmware starts, then the
// sequence, and the frames follow. The reference reads the archive as archive mode reads it, by the strict rules. A
// sequence that archive mode doesn't start fails, and so does a run that stops before the check's end. Hokuto's frames
// hold the DSP's output after three frames of silence (song::CtrModel). The player's output after its first frame has
// to be that output, sample for sample. Once the output has ended, the reference has to stay silent. A seek to the
// middle of the output, and one back to the second frame, have to give the reference's frames there. The DSP always
// mixes, and a seek runs every frame from the snapshot before it. Halfway through the output, the player then switches
// to a new model of the same sequence. A catch-up runs the new model up to the player's frame. The program switches the
// same way after a change of the variables during playback. The output has to stay the reference's, rounded to 16 bits
// in the frame that crossfades, and end where it ends without the switch. As for the Switch, the tool counts the notes
// that the reference starts and the arrangement lacks. It counts the legatos among them apart: the notes of a track
// with tie or mono on that continue a sounding channel. The arrangement shows a legato as part of the note that it
// continues. --variables and --changes check the variables as on the Switch. A change comes 2 ms into the 3DS's frame
// halfway through the sequence's length or through the check. With --effect kind, both play an effect of the 3DS on aux
// bus A at the libraries' defaults. The kinds have the names of a Hokuto file: 3ds_reverb, 3ds_delay, 3ds_nn_reverb,
// 3ds_nn_delay and 3ds_nn_reverb_2011. The reference runs the effect in bus A's aux callback, on ntdlib's model of the
// library's effect (ntdlib/ctr_fx/effects.h). The switch halfway then comes from a model without the effect. The
// effects' tails can keep an offset of a step or two. After the output's end, the reference then only has to stay
// within ±8. The summary counts the runs that sent sound to bus A. --reverb is for the Switch. --part k/n checks every
// nth sequence from the kth on, for runs side by side. Teakra prints on stdout. The tool's lines then go to stderr.
//
// With --enhanced in place of --firmware, a 3DS archive plays in enhanced mixing. The reference is ntdlib's archive
// mode on the model in its enhanced mode, run directly, with its frames after four frames of silence. The player's
// output and the frames after seeks have to be the reference's in floating point, sample for sample. The switch halfway
// comes from a model in strict mixing, as after a click on the mixer's switch. The player keeps the rate of strict
// mixing. After the frame that crossfades, its frames have to be the reference's frames through a song::FrameResampler
// that starts at that frame, sample for sample.
//
// A Wii archive gets the same checks as a Wii U archive, against WSF's archive mode: ntdlib's model of NW4R SND and AX
// with the game's AX microcode on ntdlib's DSP, and the build of NW4R SND that the game's main.dol gives. Like a file
// imported with the main.dol (song::ReadWiiBuild), Hokuto's model follows the same build. It mixes with ntdlib's model
// of AX's microcode: the build of December 2006, or the build of October 2006 with NW4R SND's build of 2006 without a
// banner. AX takes the same build on both sides. --dol gives the main.dol. With --archive-build, both follow the build
// that the archive's version gives, like a file imported without the main.dol. Both play without effects, unless
// --effect kind puts an effect of the Wii on aux bus A at Hokuto's starting values (song::WiiStartingParameters). The
// kinds have the names of a Hokuto file: wii_reverb_hi, wii_reverb_hi_2008, wii_chorus, wii_delay, wii_reverb_std and
// wii_reverb_std_2008. A switch halfway, from a model without the effect, follows. The Wii mixes every frame. The
// output after the switch therefore has to be the reference's. With an effect, a run goes on after its output last had
// sound for as long as Hokuto's model rings (song::WiiRingingFrames): a delay's echo can come back after a quiet gap.
// --variables and --changes check the variables as on the Switch, with a change 2 ms into a frame of 3 ms. Those runs
// check how the values reach the model, and the reference mixes with ntdlib's model of the microcode too. The runs
// without values check the mixing. With --archive-build, the reference follows the build that the archive's version
// gives, like Hokuto's model. A sequence that differs only without it plays otherwise in the game's build. --part k/n
// checks every nth sequence from the kth on, as for the 3DS.
//
// usage: hokuto_check_playback [--reverb n | --effect kind] [--variables | --changes] [--enhanced] <archive> [seconds]
//        hokuto_check_playback --firmware <dsp firmware> [--part k/n] [--effect kind] [--variables | --changes]
//                              <3DS archive> [seconds]
//        hokuto_check_playback --enhanced [--part k/n] [--effect kind] [--variables | --changes]
//                              <3DS archive> [seconds]
//        hokuto_check_playback --dol <main.dol> [--archive-build] [--part k/n] [--effect kind]
//                              [--variables | --changes] <Wii archive> [seconds]

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

#include "song/archive.h"
#include "song/catch_up.h"
#include "song/ctr_model.h"
#include "song/frame_resampler.h"
#include "song/player.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace
{

namespace atk = ntdlib::nx::atk;
namespace cafe = ntdlib::cafe::snd;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;

// The stereo frames of a frame of the model, and the model's frames in a second and in a minute.
constexpr std::size_t kFrameSamples = atk::ArchiveModel::kFrameSamples;
constexpr song::FrameMs kFrameMs = song::FrameMsOf(sar::Flavor::kNx);
static_assert(kFrameMs.denominator == 1);
constexpr std::size_t kFramesPerSecond = 200;
constexpr double kFramesPerMinute = 60.0 * kFramesPerSecond;

// The render worker's blocks, in stereo frames.
constexpr std::size_t kBlock = 256;

// The tracks of a sequence player.
constexpr int kTracks = 16;

// The strict rules for the Switch's archives.
const sar::Options kStrict{sar::Flavor::kNx, {}, false, sar::Rules::kStopAtFirstProblem};

// The lowest ratio, in dB, of the model's output to the difference of the output after a switch from it. The lead
// before a switch lets the effects' tails fall by 60 dB.
constexpr double kSwitchRatio = 60.0;

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
Closeness ClosenessOf(double signal, double difference, std::size_t count)
{
    if (difference <= 0.0)
    {
        return {};
    }

    return {10.0 * std::log10(signal / difference), 10.0 * std::log10(difference / static_cast<double>(count))};
}

// The closeness of `samples` to `expected`, over the samples of both.
Closeness ClosenessOf(std::span<const float> samples, std::span<const float> expected)
{
    double signal = 0.0;
    double difference = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < samples.size() && i < expected.size(); i++)
    {
        const double wanted = expected[i];
        const double error = samples[i] - wanted;
        signal += wanted * wanted;
        difference += error * error;
        count++;
    }

    return ClosenessOf(signal, difference, count);
}

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

// Runs `frames` frames of sequence sound `sound` on the model with `options`, with the variables' starting values
// `variables` and the game's `changes`. A channel plays a new note when it's new in its place, or when its keys change.
ModelRun RunModel(const sar::SoundArchive& archive, uint32_t sound, std::size_t frames,
                  const atk::EngineOptions& options, const song::StartValues& variables, const song::Changes& changes)
{
    ModelRun run;
    atk::ArchiveModel model(archive, options);
    auto* player = model.engine->StartSequence(sound);
    if (player == nullptr)
    {
        return run;
    }

    for (const auto& [number, value] : variables)
    {
        player->SetVariable(*model.engine, number, value);
    }

    std::map<int, std::pair<int, int>> previous;
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        // The game's changes come before the frame.
        for (const song::VariableChange& change : changes)
        {
            if (song::ChangeFrame(change.time, kFrameMs) == frame)
            {
                player->SetVariable(*model.engine, change.variable, change.value);
            }
        }

        const bool ended = player->IsFinished();
        const double position = ended ? run.position_after.back() : static_cast<double>(player->TickCount());
        run.before.push_back(player->TickCount());
        run.position_before.push_back(position);
        run.busy.push_back(model.engine->IsBusy());
        model.engine->RunFrame();
        run.after.push_back(player->TickCount());
        run.position_after.push_back(ended ? position + player->Tempo() * player->Timebase() / kFramesPerMinute
                                           : static_cast<double>(player->TickCount()));
        const std::span<const int16_t> output = model.renderer.DeviceOutput();
        run.samples.insert(run.samples.end(), output.begin(), output.end());
        const std::span<const float> floats = model.renderer.DeviceOutputFloat();
        run.floats.insert(run.floats.end(), floats.begin(), floats.end());

        // Each channel's track, from the tracks' lists of channels.
        const auto& channels = model.engine->Channels();
        std::map<int, int> track_of;
        for (int t = 0; t < kTracks; t++)
        {
            const auto* track = player->Track(t);
            for (int place = track != nullptr ? track->ChannelList() : ntdlib::nwsnd::kNoChannel;
                 place != ntdlib::nwsnd::kNoChannel; place = channels.At(place).next_in_track)
            {
                track_of[place] = t;
            }
        }

        std::map<int, std::pair<int, int>> current;
        std::vector<NoteStart> started;
        for (const int place : channels.InUse())
        {
            const auto& channel = channels.At(place);
            const std::pair<int, int> keys = {channel.key, channel.original_key};
            current[place] = keys;
            const auto old = previous.find(place);
            if (old == previous.end() || old->second != keys)
            {
                const auto track = track_of.find(place);
                started.push_back({track != track_of.end() ? track->second : -1, channel.key, old != previous.end()});
            }
        }
        run.notes.push_back(std::move(started));
        previous = std::move(current);
    }

    return run;
}

// The first frame of `run` after which the position has passed `tick`, or the number of frames.
std::size_t FirstFrameAfter(const ModelRun& run, uint64_t tick)
{
    std::size_t frame = 0;
    while (frame < run.position_after.size() && run.position_after[frame] <= static_cast<double>(tick))
    {
        frame++;
    }

    return frame;
}

// Whether frame `frame` of `run` came out silent.
bool Silent(const ModelRun& run, std::size_t frame)
{
    const std::span<const int16_t> samples =
        std::span<const int16_t>(run.samples).subspan(frame * kFrameSamples * 2, kFrameSamples * 2);
    return std::all_of(samples.begin(), samples.end(), [](int16_t sample) { return sample == 0; });
}

// The first frame of `run` before which nothing played or sounded and after a frame that came out silent, or the number
// of frames. From there on, the model's output is silent.
std::size_t FadedFrame(const ModelRun& run)
{
    std::size_t frame = 0;
    while (frame < run.busy.size() && (run.busy[frame] || (frame > 0 && !Silent(run, frame - 1))))
    {
        frame++;
    }

    return frame;
}

// The player's output and the clock's position at the start of each frame. The worker renders in blocks, and the
// program reads the clock soon after each block. The run does the same.
struct PlayerRun
{
    std::vector<float> samples;
    std::vector<std::optional<double>> marks;
    bool ended = false;
};

// A clock for the playback of `sequence`. A looped sequence goes round its loop. The game plays it that way.
std::shared_ptr<song::PlaybackClock> ClockFor(const song::Sequence& sequence)
{
    const auto clock = std::make_shared<song::PlaybackClock>(kFrameSamples);
    if (const std::optional<song::Loop>& loop = sequence.loop)
    {
        clock->SetCycle({true, loop->start, loop->end});
    }

    return clock;
}

// Adds `frames` frames of the player's output to `run`.
void Render(song::SequencePlayer& player, const song::PlaybackClock& clock, std::size_t frames, PlayerRun& run)
{
    const std::size_t total = run.samples.size() / 2 + frames * kFrameSamples;
    std::size_t done = run.samples.size() / 2;
    run.samples.resize(total * 2);
    while (done < total && !run.ended)
    {
        const std::size_t block = std::min(kBlock, total - done);
        const std::size_t rendered = player.Render(run.samples.data() + done * 2, block);
        done += rendered;
        run.ended = rendered < block;
        while (run.marks.size() * kFrameSamples < done)
        {
            run.marks.push_back(clock.PositionAt(run.marks.size() * kFrameSamples));
        }
    }
    run.samples.resize(done * 2);
}

// The output of a player of `model`'s sequence from tick `start`, for `frames` frames.
PlayerRun RunPlayer(const std::shared_ptr<song::SequenceModel>& model, uint32_t start, std::size_t frames)
{
    const std::shared_ptr<song::PlaybackClock> clock = ClockFor(model->GetSequence());
    song::SequencePlayer player(model, start, clock);
    PlayerRun run;
    Render(player, *clock, frames, run);

    return run;
}

// The player's output for `before` frames on a model without effects in strict mixing, then on a model with `effects`
// in `mixing`, up to `frames` frames in all. A catch-up runs the new model up to the player's frame, and the player
// takes it at its next frame. The program does the same after an edit during playback.
PlayerRun RunSwitch(const std::shared_ptr<const sar::SoundArchive>& archive,
                    const std::shared_ptr<const song::Sequence>& sequence, const song::AuxEffects& effects,
                    song::Mixing mixing, std::size_t before, std::size_t frames)
{
    const std::shared_ptr<song::PlaybackClock> clock = ClockFor(*sequence);
    song::SequencePlayer player(std::make_shared<song::SequenceModel>(archive, sequence), 0, clock);
    PlayerRun run;
    Render(player, *clock, before, run);

    song::ModelCatchUp catch_up;
    catch_up.Start(std::make_shared<song::SequenceModel>(archive, sequence, effects, mixing), clock);
    while (!catch_up.Finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clock->Offer(catch_up.Take());
    Render(player, *clock, frames - before, run);

    return run;
}

// The samples of `run` that differ from the model's output from frame `first` on. The first `skip` samples don't count.
// Samples past the end of the model's run differ. The enhanced mode's samples have to match in floating point.
std::size_t Differences(const PlayerRun& run, const ModelRun& model, std::size_t first, std::size_t skip)
{
    const std::size_t offset = first * kFrameSamples * 2;
    std::size_t differences = 0;
    for (std::size_t i = skip; i < run.samples.size(); i++)
    {
        const bool past_end = offset + i >= model.samples.size();
        if (past_end || (model.floats.empty() ? std::lround(run.samples[i] * 32768.0f) != model.samples[offset + i]
                                              : run.samples[i] != model.floats[offset + i]))
        {
            differences++;
        }
    }

    return differences;
}

// The closeness of `run`'s output to the model's (ClosenessOf), over `frames` frames of `run` after its first `skip`
// samples, against the model's output from frame `first` on. It stops at the end of either.
Closeness SwitchCloseness(const PlayerRun& run, const ModelRun& model, std::size_t first, std::size_t skip,
                          std::size_t frames)
{
    const std::size_t offset = first * kFrameSamples * 2;
    const std::size_t end = std::min({run.samples.size(), skip + frames * kFrameSamples * 2,
                                      model.samples.size() - std::min(offset, model.samples.size())});
    std::vector<float> expected;
    for (std::size_t i = skip; i < end; i++)
    {
        expected.push_back(model.floats.empty() ? static_cast<float>(model.samples[offset + i]) / 32768.0f
                                                : model.floats[offset + i]);
    }

    return ClosenessOf(std::span<const float>(run.samples).subspan(skip, expected.size()), expected);
}

// The same for a seek, with the model's output over the second before frame `lead_start` as the signal where that's
// louder: the lead before the seek's place starts at that frame (see the file's comment).
Closeness SeekCloseness(const PlayerRun& run, const ModelRun& model, std::size_t first, std::size_t skip,
                        std::size_t frames, std::size_t lead_start)
{
    const std::size_t offset = first * kFrameSamples * 2;
    const std::size_t end = std::min({run.samples.size(), skip + frames * kFrameSamples * 2,
                                      model.samples.size() - std::min(offset, model.samples.size())});

    const auto wanted = [&model](std::size_t i)
    {
        return model.floats.empty() ? static_cast<double>(model.samples[i]) / 32768.0
                                    : static_cast<double>(model.floats[i]);
    };

    double signal = 0.0;
    double difference = 0.0;
    for (std::size_t i = skip; i < end; i++)
    {
        const double expected = wanted(offset + i);
        signal += expected * expected;
        difference += (run.samples[i] - expected) * (run.samples[i] - expected);
    }
    if (difference == 0.0)
    {
        return {};
    }

    const std::size_t from = (lead_start > kFramesPerSecond ? lead_start - kFramesPerSecond : 0) * kFrameSamples * 2;
    const std::size_t to = std::min(lead_start * kFrameSamples * 2, model.samples.size());
    double before = 0.0;
    for (std::size_t i = from; i < to; i++)
    {
        before += wanted(i) * wanted(i);
    }
    const std::size_t count = end > skip ? end - skip : 1;
    const double power =
        std::max(signal / static_cast<double>(count), to > from ? before / static_cast<double>(to - from) : 0.0);
    return ClosenessOf(power * static_cast<double>(count), difference, count);
}

// The frames from the second on whose position differs from the model's position before them, round the loop. A looped
// sequence's position is a whole tick count.
std::size_t WrongMarks(const PlayerRun& run, const ModelRun& model, std::size_t first, const song::Sequence& sequence)
{
    std::size_t wrong = 0;
    for (std::size_t frame = 1; frame < run.marks.size() && first + frame < model.before.size(); frame++)
    {
        const double position = model.position_before[first + frame];
        const double expected =
            sequence.loop ? song::PositionOfTicks(sequence, static_cast<uint64_t>(position)) : position;
        if (!run.marks[frame] || std::abs(*run.marks[frame] - expected) > 1e-9)
        {
            wrong++;
        }
    }

    return wrong;
}

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

// The loop's ticks for a summary line, or "none".
std::string LoopText(const song::Sequence& sequence)
{
    if (!sequence.loop)
    {
        return "none";
    }

    return std::to_string(sequence.loop->start) + "-" + std::to_string(sequence.loop->end);
}

// The starting values for a summary line, such as " with variable 0 at 1", or nothing.
std::string ValuesText(const song::StartValues& values)
{
    std::string text;
    for (const auto& [number, value] : values)
    {
        text += (text.empty() ? " with variable " : " and variable ") + std::to_string(number) + " at " +
                std::to_string(value);
    }

    return text;
}

// The changes for a summary line, such as " with variable 0 set to 1 at 7502 ms", or nothing.
std::string ChangesText(const song::Changes& changes)
{
    std::string text;
    for (const song::VariableChange& change : changes)
    {
        text += (text.empty() ? " with variable " : " and variable ") + std::to_string(change.variable) + " set to " +
                std::to_string(change.value) + " at " + std::to_string(change.time) + " ms";
    }

    return text;
}

// Totals over the archive. With --variables or --changes, `sequences` counts the runs of each choice. `affected` counts
// the runs whose model output the changes alter. `quiet_tails` counts the sequences of the Wii U and the 3DS whose
// output ended while the effect still gave samples of ±8 at most. `sent` counts the runs of the 3DS that sent sound to
// bus A. `switches` counts the switches halfway, and `exact` those whose output matched the model's after the frame
// that crossfades. `lowest` gives the lowest ratio after a switch. The stretches that pass by the floor (kFloorDb)
// don't count. `floored` counts the switches with such a stretch, and `loudest` gives the loudest difference among
// them. The fields that end in `_seek` or `_seeks` give the same for the seeks that the ratio measures.
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

// Adds the stretches of a switch or a seek to `count`, `exact`, `lowest`, `floored` and `loudest` (see Totals), and
// returns whether each passes. A stretch passes when its difference lies 60 dB or more below the model's output, or
// under the floor.
bool AddStretches(std::initializer_list<Closeness> stretches, std::size_t& count, std::size_t& exact, double& lowest,
                  std::size_t& floored, double& loudest)
{
    bool passes = true;
    bool all_exact = true;
    bool under_floor = false;
    for (const Closeness& stretch : stretches)
    {
        all_exact = all_exact && std::isinf(stretch.ratio) && stretch.ratio > 0.0;
        if (stretch.ratio < kSwitchRatio && stretch.level <= kFloorDb)
        {
            under_floor = true;
            loudest = std::max(loudest, stretch.level);
            continue;
        }
        lowest = std::min(lowest, stretch.ratio);
        passes = passes && stretch.ratio >= kSwitchRatio;
    }
    count++;
    exact += all_exact ? 1 : 0;
    floored += under_floor ? 1 : 0;

    return passes;
}

// Adds a switch to `totals`, and returns whether it passes.
bool AddSwitch(std::initializer_list<Closeness> stretches, Totals& totals)
{
    return AddStretches(stretches, totals.switches, totals.exact, totals.lowest, totals.floored, totals.loudest);
}

// Adds a seek to `totals`, and returns whether it passes.
bool AddSeek(std::initializer_list<Closeness> stretches, Totals& totals)
{
    return AddStretches(stretches, totals.seeks, totals.exact_seeks, totals.lowest_seek, totals.floored_seeks,
                        totals.loudest_seek);
}

// The worse ratio of `stretches`.
double WorstRatio(std::initializer_list<Closeness> stretches)
{
    double ratio = std::numeric_limits<double>::infinity();
    for (const Closeness& stretch : stretches)
    {
        ratio = std::min(ratio, stretch.ratio);
    }

    return ratio;
}

// The ratio after a switch as text, such as "72.4 dB".
std::string RatioText(double ratio)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f dB", ratio);

    return text;
}

// The summary's line about the switches, if any ran.
void PrintSwitches(const Totals& totals)
{
    if (totals.switches == 0)
    {
        return;
    }

    std::printf("%zu switches: %zu matched the model exactly after the frame that crossfades.", totals.switches,
                totals.exact);
    if (!std::isinf(totals.lowest))
    {
        std::printf(" The lowest ratio of the model's output to the difference was %s.",
                    RatioText(totals.lowest).c_str());
    }
    if (totals.floored > 0)
    {
        std::printf(" %zu passed by the floor: their differences lay %.1f dB or more below full scale.", totals.floored,
                    -totals.loudest);
    }
    std::printf("\n");
    if (totals.seeks == 0)
    {
        return;
    }

    std::printf("%zu seeks: %zu matched the model exactly after the first frame.", totals.seeks, totals.exact_seeks);
    if (!std::isinf(totals.lowest_seek))
    {
        std::printf(" The lowest ratio of the model's output to the difference was %s.",
                    RatioText(totals.lowest_seek).c_str());
    }
    if (totals.floored_seeks > 0)
    {
        std::printf(" %zu passed by the floor: their differences lay %.1f dB or more below full scale.",
                    totals.floored_seeks, -totals.loudest_seek);
    }
    std::printf("\n");
}

// Checks sequence sound `sound` with `effects` on the aux buses, the variables' starting values `values` and the game's
// `changes`, and prints a line when it differs. `options` give the model the same effects and mixing mode.
void CheckSequence(const sar::SoundArchive& strict, const std::shared_ptr<const sar::SoundArchive>& archive,
                   uint32_t sound, std::size_t frames, const atk::EngineOptions& options,
                   const song::AuxEffects& effects, const song::StartValues& values, const song::Changes& changes,
                   Totals& totals)
{
    const song::Mixing mixing = options.enhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict;
    const std::string name = strict.Sounds()[sound].name + ValuesText(values) + ChangesText(changes);
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound, values, changes));
    }
    catch (const std::exception& error)
    {
        std::printf("%s: unread: %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }

    std::string problems;
    const std::size_t model_frames = frames + 2 * kFramesPerSecond;
    const ModelRun model = RunModel(strict, sound, model_frames, options, values, changes);
    if (model.after.empty())
    {
        problems += " the model doesn't start it.";
    }
    if (!changes.empty() && RunModel(strict, sound, model_frames, options, values, {}).samples != model.samples)
    {
        totals.affected++;
    }

    // The runs share a model. The program's playbacks do the same. The seek then starts from a snapshot of the run from
    // the start.
    const auto shared_model = std::make_shared<song::SequenceModel>(archive, sequence, effects, mixing);

    // From the start.
    const std::size_t first = FirstFrameAfter(model, 0);
    if (!model.after.empty())
    {
        const PlayerRun run = RunPlayer(shared_model, 0, frames);
        const std::span<const int16_t> lead(model.samples.data(),
                                            std::min(model.samples.size(), first * 2 * kFrameSamples));
        if (std::any_of(lead.begin(), lead.end(), [](int16_t sample) { return sample != 0; }))
        {
            problems += " sound before the first tick.";
        }
        if (const std::size_t differences = Differences(run, model, first, 0); differences > 0)
        {
            problems += " " + std::to_string(differences) + " samples differ from the start.";
        }
        if (const std::size_t wrong = WrongMarks(run, model, first, *sequence); wrong > 0)
        {
            problems += " " + std::to_string(wrong) + " positions differ from the start.";
        }
        if (run.ended && sequence->loop)
        {
            problems += " a looped sequence ended.";
        }
        if (run.ended && first + run.samples.size() / (kFrameSamples * 2) < FadedFrame(model))
        {
            problems += " the output ended before the model's faded.";
        }
    }

    // From a seek. The comparison stops at the end of the model's run. Nothing plays from a seek past the point where
    // the last note has faded. The sequence's length can run past that point. With an effect or in enhanced mixing, the
    // ratio measures the output.
    const bool with_effects = std::any_of(effects.begin(), effects.end(), [](const auto& bus) { return !bus.empty(); });
    const uint32_t start = sequence->loop ? sequence->loop->start : sequence->length / 2;
    const std::size_t seek_first = FirstFrameAfter(model, start);
    if (start > 0 && seek_first < model.after.size() && seek_first >= FadedFrame(model))
    {
        const PlayerRun run = RunPlayer(shared_model, start, 1);
        if (!run.samples.empty() || !run.ended)
        {
            problems += " a seek past the last note's fade plays.";
        }
    }
    else if (start > 0 && seek_first < model.after.size())
    {
        const PlayerRun run = RunPlayer(shared_model, start, std::min(frames, model.after.size() - seek_first));
        if (with_effects || options.enhanced)
        {
            const std::size_t lead = shared_model->LeadFrames();
            const std::size_t lead_start = seek_first > lead ? seek_first - lead : 0;
            const Closeness next =
                SeekCloseness(run, model, seek_first, kFrameSamples * 2, kFramesPerSecond, lead_start);
            const Closeness rest = SeekCloseness(run, model, seek_first, kFrameSamples * 2, frames, lead_start);
            if (!AddSeek({next, rest}, totals))
            {
                problems +=
                    " the output after a seek lies only " + RatioText(WorstRatio({next, rest})) + " below the model's.";
            }
        }
        else if (const std::size_t differences = Differences(run, model, seek_first, kFrameSamples * 2);
                 differences > 0)
        {
            problems += " " + std::to_string(differences) + " samples differ after a seek.";
        }
        if (run.marks.empty() || run.marks[0] != static_cast<double>(song::PositionOfTicks(*sequence, start)))
        {
            problems += " the seek's first position differs.";
        }
        if (run.ended && seek_first + run.samples.size() / (kFrameSamples * 2) < FadedFrame(model))
        {
            problems += " the output ended before the model's faded after a seek.";
        }
    }

    // A switch halfway, from a model without the effects in strict mixing to one with them in the mode. After the frame
    // that crossfades, the output lies close to the model's with the effects: over the next second, and over the rest.
    if ((with_effects || options.enhanced) && !model.after.empty())
    {
        const PlayerRun run = RunSwitch(archive, sequence, effects, mixing, frames / 2, frames);
        const std::size_t skip = (frames / 2 + 1) * kFrameSamples * 2;
        if (run.samples.size() > skip)
        {
            const Closeness next = SwitchCloseness(run, model, first, skip, kFramesPerSecond);
            const Closeness rest = SwitchCloseness(run, model, first, skip, frames);
            if (!AddSwitch({next, rest}, totals))
            {
                problems += " the output after a switch lies only " + RatioText(WorstRatio({next, rest})) +
                            " below the model's.";
            }
        }
        if (run.ended && run.samples.size() > skip &&
            first + run.samples.size() / (kFrameSamples * 2) < FadedFrame(model))
        {
            problems += " the output ended before the model's faded after a switch.";
        }
    }

    // The notes, up to `frames` frames from the start. A note that the model cuts in the tick it starts is missing from
    // the arrangement, and inaudible. Missing notes are listed apart from the differences.
    const NoteCheck notes = CheckNotes(model, *sequence, first + frames);
    totals.sequences++;
    totals.looped += sequence->loop ? 1 : 0;
    totals.notes += notes.checked;
    totals.missed_notes += notes.missed;
    totals.missed_later += notes.missed_later;
    if (!problems.empty())
    {
        totals.failed++;
        std::printf("%s (loop %s):%s\n", name.c_str(), LoopText(*sequence).c_str(), problems.c_str());
    }
    if (notes.missed > 0)
    {
        std::printf("%s (loop %s): %zu of %zu notes missing from the arrangement, %zu of them after the first pass.\n",
                    name.c_str(), LoopText(*sequence).c_str(), notes.missed, notes.checked, notes.missed_later);
    }
    std::fflush(stdout);
}

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

// The run of sound `sound` of `archive` on ntdlib's model of the Wii U for `frames` frames, with `effects` on the aux
// buses, in enhanced mixing when `enhanced` is true.
WiiURun RunWiiUModel(const sar::SoundArchive& archive, uint32_t sound, std::size_t frames,
                     const std::vector<cafe::EffectSetup>& effects, bool enhanced)
{
    WiiURun run;
    cafe::EngineOptions options;
    options.effects = effects;
    options.enhanced = enhanced;
    cafe::ArchiveModel model(archive, std::move(options));
    const auto* player = model.engine->StartSequence(sound);
    if (player == nullptr)
    {
        return run;
    }

    for (std::size_t frame = 0; frame < frames; frame++)
    {
        model.engine->RunFrame();
        run.after.push_back(player->TickCount());
        const std::span<const int16_t> output = model.system.Output();
        run.samples.insert(run.samples.end(), output.begin(), output.end());
        const std::span<const float> floats = model.system.OutputFloat();
        run.floats.insert(run.floats.end(), floats.begin(), floats.end());
    }

    return run;
}

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

// The closeness of `output` to the model's output for the frame after a seek to frame `frame`, with the model's output
// over the second before frame `lead_start` as the signal where that's louder (see SeekCloseness).
Closeness WiiUSeekCloseness(std::span<const int16_t> output, const WiiURun& expected, std::size_t frame,
                            std::size_t lead_start)
{
    const std::size_t kFloats = kWiiUFrameSamples * 2;

    const auto power = [&expected](std::size_t from, std::size_t to)
    {
        double sum = 0.0;
        for (std::size_t i = from; i < to && i < expected.samples.size(); i++)
        {
            const double sample = static_cast<double>(expected.samples[i]) / 32768.0;
            sum += sample * sample;
        }

        return to > from ? sum / static_cast<double>(to - from) : 0.0;
    };

    double difference = 0.0;
    for (std::size_t i = 0; i < output.size() && frame * kFloats + i < expected.samples.size(); i++)
    {
        const double error = static_cast<double>(output[i] - expected.samples[frame * kFloats + i]) / 32768.0;
        difference += error * error;
    }
    if (difference == 0.0)
    {
        return {};
    }

    const std::size_t from = lead_start > kWiiUFramesPerSecond ? lead_start - kWiiUFramesPerSecond : 0;
    const double signal =
        std::max(power(frame * kFloats, (frame + 1) * kFloats), power(from * kFloats, lead_start * kFloats));
    return ClosenessOf(signal * static_cast<double>(output.size()), difference, output.size());
}

// The closeness of `samples` to the model's output (ClosenessOf), over `count` frames of `samples` from frame `from`
// on, against `expected`'s frames from frame `first` + `from` on. In enhanced mixing, the expected output is in
// floating point. It stops at the end of either.
Closeness WiiUSwitchCloseness(std::span<const float> samples, std::size_t from, const WiiURun& expected,
                              std::size_t first, std::size_t count)
{
    const std::size_t kFloats = kWiiUFrameSamples * 2;
    const std::size_t start = std::min(from * kFloats, samples.size());
    std::vector<float> wanted;
    for (std::size_t i = start; i < samples.size() && i < start + count * kFloats; i++)
    {
        const std::size_t at = first * kFloats + i;
        if (at >= expected.samples.size())
        {
            break;
        }
        wanted.push_back(expected.floats.empty() ? static_cast<float>(expected.samples[at]) / 32768.0f
                                                 : expected.floats[at]);
    }

    return ClosenessOf(samples.subspan(start, wanted.size()), wanted);
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
                  std::vector<float>& output)
{
    const std::size_t total = frames * frame_samples;
    while (output.size() / 2 < total)
    {
        const std::size_t block = std::min(kBlock, total - output.size() / 2);
        const std::size_t at = output.size();
        output.resize(at + block * 2);
        const std::size_t done = player.Render(output.data() + at, block);
        output.resize(at + done * 2);
        if (done < block)
        {
            return false;
        }
    }

    return true;
}

// The player's output for `before` frames on a model without effects in strict mixing, then on a model with `effects`
// in `mixing`, up to `frames` frames in all. A catch-up runs the new model up to the player's frame, and the player
// takes it at its next frame. The program does the same after an edit during playback.
std::vector<float> RunWiiUSwitch(const std::shared_ptr<const sar::SoundArchive>& archive,
                                 const std::shared_ptr<const song::Sequence>& sequence, const song::AuxEffects& effects,
                                 song::Mixing mixing, std::size_t before, std::size_t frames)
{
    const auto clock = std::make_shared<song::PlaybackClock>(kWiiUFrameSamples);
    song::SequencePlayer player(std::make_shared<song::SequenceModel>(archive, sequence), 0, clock);
    std::vector<float> output;
    if (!RenderFrames(player, before, kWiiUFrameSamples, output))
    {
        return output;
    }

    song::ModelCatchUp catch_up;
    catch_up.Start(std::make_shared<song::SequenceModel>(archive, sequence, effects, mixing), clock);
    while (!catch_up.Finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clock->Offer(catch_up.Take());
    RenderFrames(player, frames, kWiiUFrameSamples, output);

    return output;
}

// Checks Hokuto's playback of sequence sound `sound` of a Wii U archive against the model run directly: the frames of a
// run from the start, the frames after seeks back and forth, and the player's output after its first frame. That frame
// fades in. The player plays once, to the end of its loop's pass or until its last note has faded. Its last frame can
// fade out. With an effect or in enhanced mixing, a switch halfway through the sequence follows. Prints a line when
// anything differs.
void CheckWiiUSequence(const sar::SoundArchive& strict, const std::shared_ptr<const sar::SoundArchive>& archive,
                       uint32_t sound, std::size_t frames, const WiiUEffects& effects, song::Mixing mixing,
                       Totals& totals)
{
    const std::string name = strict.Sounds()[sound].name;
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound));
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't be read. %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }

    totals.sequences++;
    totals.looped += sequence->loop ? 1 : 0;
    const bool enhanced = mixing == song::Mixing::kEnhanced;
    const WiiURun expected = RunWiiUModel(strict, sound, frames, effects.setups, enhanced);

    // A run from the start, frame by frame. The model stops once nothing plays or sounds and a frame has come out
    // silent. Without an effect, the model's output after that is silent too. With an effect, a frame within ±8 counts
    // as silent once the effects have stopped ringing: ntdlib's threshold of a ringing effect. A delay that feeds back
    // can leave samples of ±1 for ever. The output after the end then has to stay within ±8.
    const auto model = std::make_shared<song::SequenceModel>(archive, sequence, effects.effects, mixing);
    std::vector<int16_t> played;
    for (bool more = model->SeekFrame(0); more && played.size() < frames * kWiiUFrameSamples * 2;
         more = model->RunFrame())
    {
        const std::span<const int16_t> output = model->Output();
        played.insert(played.end(), output.begin(), output.end());
    }
    std::size_t differences = WiiUDifferences(std::span<const int16_t>(played), 0, expected, 0, 1.0f);
    const std::size_t played_frames = played.size() / (kWiiUFrameSamples * 2);
    const std::size_t rest = std::min(played.size(), expected.samples.size());
    const auto louder_than = [&](int limit)
    {
        const auto louder = [limit](int16_t sample)
        {
            return std::abs(static_cast<int>(sample)) > limit;
        };

        return std::any_of(expected.samples.begin() + static_cast<std::ptrdiff_t>(rest), expected.samples.end(),
                           louder);
    };
    if (louder_than(effects.setups.empty() ? 0 : 8))
    {
        differences++;
    }
    else if (louder_than(0))
    {
        totals.quiet_tails++;
    }

    // Seeks back and forth restore the snapshots that the run took.
    std::size_t seeks = 0;
    for (const std::size_t frame : {played_frames / 2, played_frames / 4, std::size_t{1}, played_frames * 3 / 4})
    {
        if (frame < played_frames && model->SeekFrame(frame))
        {
            const uint64_t lead = model->LeadFrames();
            const std::size_t lead_start = frame > lead ? frame - static_cast<std::size_t>(lead) : 0;
            if (!effects.setups.empty() || enhanced)
            {
                seeks += AddSeek({WiiUSeekCloseness(model->Output(), expected, frame, lead_start)}, totals) ? 0 : 1;
            }
            else
            {
                seeks += WiiUDifferences(model->Output(), 0, expected, frame, 1.0f);
            }
        }
    }

    // The player starts at the model's first frame that runs a tick. The worker renders in blocks.
    std::size_t first = 0;
    while (first < expected.after.size() && expected.after[first] == 0)
    {
        first++;
    }
    const auto clock = std::make_shared<song::PlaybackClock>(model->FrameSamples());
    song::SequencePlayer player(model, 0, clock);
    std::vector<float> output;
    std::vector<float> block(kBlock * 2);
    while (output.size() < frames * kWiiUFrameSamples * 2)
    {
        const std::size_t done = player.Render(block.data(), kBlock);
        output.insert(output.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(done * 2));
        if (done < kBlock)
        {
            break;
        }
    }
    const std::size_t heard = output.size() / (kWiiUFrameSamples * 2);
    output.resize(heard > 1 ? (heard - 1) * kWiiUFrameSamples * 2 : 0);
    std::size_t player_first = 0;
    const std::size_t player_differences =
        WiiUDifferences(std::span<const float>(output), 1, expected, first, 1.0f / 32768.0f, &player_first);

    // A switch halfway through the sequence, from a model without the effect in strict mixing to one with it in the
    // mode. The output before the switch can end before the sequence does. The switch then comes halfway through that
    // output. After the frame that crossfades, the output lies close to the model's with the effect: over the next
    // second, and over the rest. It ends where the output without the switch ends.
    std::string switch_text = "no switch";
    bool switch_differs = false;
    const auto halfway = static_cast<std::size_t>(song::MsOfTick(*sequence, sequence->length / 2.0) /
                                                  static_cast<double>(kWiiUFrameMs.numerator));
    std::size_t before = std::min(halfway, frames / 2);
    std::vector<float> switched;
    if ((!effects.setups.empty() || enhanced) && before > 1)
    {
        switched = RunWiiUSwitch(archive, sequence, effects.effects, mixing, before, frames);
    }
    if (!switched.empty() && switched.size() < before * kWiiUFrameSamples * 2)
    {
        before = switched.size() / (kWiiUFrameSamples * 2) / 2;
        switched = before > 1 ? RunWiiUSwitch(archive, sequence, effects.effects, mixing, before, frames)
                              : std::vector<float>{};
    }
    if (!switched.empty())
    {
        const std::size_t switched_frames = switched.size() / (kWiiUFrameSamples * 2);
        switched.resize(switched_frames > 1 ? (switched_frames - 1) * kWiiUFrameSamples * 2 : 0);
        const std::span<const float> after(switched);
        const Closeness next = WiiUSwitchCloseness(after, before + 1, expected, first, kWiiUFramesPerSecond);
        const Closeness rest = WiiUSwitchCloseness(after, before + 1, expected, first, frames);
        const bool ends_elsewhere = std::min(switched_frames, frames) != std::min(heard, frames);
        switch_differs = !AddSwitch({next, rest}, totals) || ends_elsewhere;
        switch_text = "a switch " + RatioText(WorstRatio({next, rest})) + " below the model's" +
                      (ends_elsewhere ? " that ends elsewhere" : "");
    }

    if (differences + seeks + player_differences > 0 || switch_differs || played.empty())
    {
        std::printf(
            "%s: %zu of %zu frames differ from the start, %zu after seeks, %zu of %zu from the player, first "
            "%zu, %s (loop %s).\n",
            name.c_str(), differences, played_frames, seeks, player_differences, heard, player_first,
            switch_text.c_str(),
            sequence->loop
                ? (std::to_string(sequence->loop->start) + " to " + std::to_string(sequence->loop->end)).c_str()
                : "none");
        totals.failed++;
    }
    std::fflush(stdout);
}

// The 3DS's frames. Hokuto's frames of the 3DS hold the DSP's output after three frames of silence, or the enhanced
// mode's frames after four (song::CtrModel).
constexpr song::FrameMs kCtrFrameMs = song::FrameMsOf(sar::Flavor::kCtr);

// The stereo samples of a frame of the 3DS in strict mixing, or with `enhanced` in enhanced mixing.
std::size_t CtrFrameSamples(bool enhanced)
{
    return enhanced ? song::CtrModel::kEnhancedFrameSamples : song::CtrModel::kFrameSamples;
}

// The output of ntdlib's archive mode of the 3DS, run directly for `frames` frames with the variables' starting values
// `variables` and the game's `changes`, the tick count before and after each frame, and the notes that each frame
// started. `samples` holds the DSP's output after Hokuto's silence. The run's frames then line up with Hokuto's. In
// enhanced mixing, `floats` holds the enhanced mode's frames after Hokuto's silence, and `samples` the same rounded to
// 16 bits. The run stays empty when the firmware or the sequence doesn't start. Throws if the sequence's data is
// malformed.
struct CtrRun
{
    std::size_t frame_floats = 0;
    std::vector<int16_t> samples;
    std::vector<float> floats;
    std::vector<uint64_t> before;
    std::vector<uint64_t> after;
    std::vector<std::vector<NoteStart>> notes;
    bool sent = false; // whether bus A took sound, with an effect on it
};

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

// The effect in the form that Hokuto's model plays.
song::AuxEffects CtrModelEffects(const CtrEffect& effect)
{
    song::AuxEffects effects;
    if (effect.reverb)
    {
        effects[0].emplace_back(song::CtrReverb{effect.library, {}});
    }
    else
    {
        effects[0].emplace_back(song::CtrDelay{});
    }

    return effects;
}

// The frames that the effects process: 160 samples at 32,728 Hz, or in enhanced mixing 235 samples at 235 / 160 of that
// rate. The frames of both modes last as long.
ntdlib::ctr_fx::FrameFormat CtrEffectFormat(bool enhanced)
{
    if (!enhanced)
    {
        return {};
    }

    return {song::CtrModel::kEnhancedFrameSamples, 32728.0f * 235.0f / 160.0f};
}

// Whether a frame of an aux bus holds a sample other than 0.
bool Sounding(const std::array<int32_t*, 4>& channels, std::size_t samples)
{
    const auto sounding = [samples](const int32_t* channel)
    {
        return std::any_of(channel, channel + samples, [](int32_t sample) { return sample != 0; });
    };
    return std::any_of(channels.begin(), channels.end(), sounding);
}

// Teakra runs `firmware` in strict mixing. With `enhanced`, the model of the firmware runs in its enhanced mode. With
// `effect`, the effect runs in bus A's aux callback on ntdlib's model of the library's effect. That is how a game's
// callback and the SDK's effect run it. The callback goes on after the DSP has started, before the sequence starts.
CtrRun RunCtrModel(std::span<const uint8_t> firmware, const sar::SoundArchive& archive, uint32_t sound,
                   std::size_t frames, const song::StartValues& variables, const song::Changes& changes, bool enhanced,
                   const CtrEffect* effect)
{
    CtrRun run;
    run.frame_floats = CtrFrameSamples(enhanced) * 2;
    std::vector<int16_t> samples(song::CtrModel::kSilence * 2, 0);
    std::vector<float> floats(song::CtrModel::kEnhancedSilence * 2, 0.0f);
    ntdlib::ctr::snd::ArchiveModel model(enhanced ? ntdlib::ctr::snd::DspKind::kEnhancedModel
                                                  : ntdlib::ctr::snd::DspKind::kTeakra);
    const auto keep_sample = [&samples](std::array<int16_t, 2> sample)
    {
        samples.insert(samples.end(), sample.begin(), sample.end());
    };
    model.dsp->SetSampleSink(keep_sample);
    const auto keep_frame = [&floats](std::span<const float> frame)
    {
        floats.insert(floats.end(), frame.begin(), frame.end());
    };
    model.dsp->SetFrameSink(keep_frame);
    if (enhanced ? !model.Start(archive) : !model.Start(firmware, archive))
    {
        return run;
    }
    if (effect != nullptr)
    {
        const ntdlib::ctr_fx::FrameFormat format = CtrEffectFormat(enhanced);
        bool* const sent = &run.sent;
        if (effect->reverb)
        {
            model.snd.SetAuxCallback(0,
                                     [reverb = ntdlib::ctr_fx::Reverb(effect->library, {}, format), sent](
                                         const std::array<int32_t*, 4>& channels, std::size_t samples) mutable
                                     {
                                         *sent = *sent || Sounding(channels, samples);
                                         reverb.Process(channels, samples);
                                     });
        }
        else
        {
            model.snd.SetAuxCallback(0,
                                     [delay = ntdlib::ctr_fx::Delay({}, format), sent](
                                         const std::array<int32_t*, 4>& channels, std::size_t samples) mutable
                                     {
                                         *sent = *sent || Sounding(channels, samples);
                                         delay.Process(channels, samples);
                                     });
        }
    }
    ntdlib::ctr::snd::SequenceSoundPlayer* player = model.engine->StartSequence(sound);
    if (player == nullptr)
    {
        return run;
    }
    for (const auto& [number, value] : variables)
    {
        player->SetVariable(*model.engine, number, value);
    }

    // A channel plays a new note when it's new in its place, or when its keys change.
    std::map<int, std::pair<int, int>> previous;
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        // The game's changes come before the frame.
        for (const song::VariableChange& change : changes)
        {
            if (song::ChangeFrame(change.time, kCtrFrameMs) == frame)
            {
                player->SetVariable(*model.engine, change.variable, change.value);
            }
        }

        const uint64_t before = player->TickCount();
        if (!model.engine->RunFrame())
        {
            break;
        }
        run.before.push_back(before);
        run.after.push_back(player->TickCount());

        // Each channel's track, from the tracks' lists of channels.
        const ntdlib::ctr::snd::ChannelManager& channels = model.engine->Channels();
        std::map<int, int> track_of;
        for (int t = 0; t < kTracks; t++)
        {
            const auto* track = player->Track(t);
            for (int place = track != nullptr ? track->ChannelList() : ntdlib::nwsnd::kNoChannel;
                 place != ntdlib::nwsnd::kNoChannel; place = channels.At(place).next_in_track)
            {
                track_of[place] = t;
            }
        }

        std::map<int, std::pair<int, int>> current;
        std::vector<NoteStart> started;
        for (const int place : channels.InUse())
        {
            const auto& channel = channels.At(place);
            const std::pair<int, int> keys = {channel.key, channel.original_key};
            current[place] = keys;
            const auto old = previous.find(place);
            if (old == previous.end() || old->second != keys)
            {
                const auto track = track_of.find(place);
                started.push_back({track != track_of.end() ? track->second : -1, channel.key, old != previous.end()});
            }
        }
        run.notes.push_back(std::move(started));
        previous = std::move(current);
    }

    if (enhanced)
    {
        floats.resize(std::min(floats.size(), run.after.size() * run.frame_floats));
        run.floats = std::move(floats);
        for (const float sample : run.floats)
        {
            const double scaled = std::round(static_cast<double>(sample) * 32768.0);
            run.samples.push_back(static_cast<int16_t>(std::clamp(scaled, -32768.0, 32767.0)));
        }

        return run;
    }

    samples.resize(std::min(samples.size(), run.after.size() * run.frame_floats));
    run.samples = std::move(samples);

    return run;
}

// The frames of `samples` from frame `from` on that differ from the frames of `expected` from frame `first` + `from`
// on, up to the end of either. Each frame holds `frame_floats` samples. A frame matches when each of its samples is the
// expected one times `scale`.
template <class Sample, class Expected>
std::size_t CtrDifferences(std::span<const Sample> samples, std::size_t from, std::span<const Expected> expected,
                           std::size_t frame_floats, std::size_t first, float scale,
                           std::size_t* first_difference = nullptr)
{
    std::size_t differences = 0;
    for (std::size_t frame = from;
         (frame + 1) * frame_floats <= samples.size() && (first + frame + 1) * frame_floats <= expected.size(); frame++)
    {
        const std::size_t at = (first + frame) * frame_floats;
        for (std::size_t i = 0; i < frame_floats; i++)
        {
            if (static_cast<float>(samples[frame * frame_floats + i]) != static_cast<float>(expected[at + i]) * scale)
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

// The frames of the player's output in `samples` from frame `from` on that differ from the run's frames from frame
// `first` + `from` on, in the player's floats: the enhanced mode's floats, or the DSP's samples at the player's scale.
std::size_t CtrPlayerDifferences(std::span<const float> samples, std::size_t from, const CtrRun& expected,
                                 std::size_t first, std::size_t* first_difference = nullptr)
{
    if (!expected.floats.empty())
    {
        return CtrDifferences(samples, from, std::span<const float>(expected.floats), expected.frame_floats, first,
                              1.0f, first_difference);
    }

    return CtrDifferences(samples, from, std::span<const int16_t>(expected.samples), expected.frame_floats, first,
                          1.0f / 32768.0f, first_difference);
}

// Adds up to `frames` frames of the player's output to `output`, in the worker's blocks, with `frame_samples` stereo
// samples in each frame. Returns false once the output has ended.
bool RenderCtr(song::SequencePlayer& player, std::size_t frames, std::size_t frame_samples, std::vector<float>& output)
{
    const std::size_t total = output.size() / 2 + frames * frame_samples;
    while (output.size() / 2 < total)
    {
        const std::size_t block = std::min(kBlock, total - output.size() / 2);
        const std::size_t at = output.size();
        output.resize(at + block * 2);
        const std::size_t done = player.Render(output.data() + at, block);
        output.resize(at + done * 2);
        if (done < block)
        {
            return false;
        }
    }

    return true;
}

// The player's output for `before` frames in mode `from`, then on a new model of the same sequence in mode `to` with
// `effects`, up to `frames` frames in all. A catch-up runs the new model up to the player's frame, and the player takes
// it at its next frame. The player keeps the rate of mode `from`.
std::vector<float> RunCtrSwitch(const std::shared_ptr<const sar::SoundArchive>& archive,
                                const std::shared_ptr<const song::Sequence>& sequence, std::size_t before,
                                std::size_t frames, song::Mixing from, song::Mixing to, const song::AuxEffects& effects)
{
    const std::size_t frame_samples = CtrFrameSamples(from == song::Mixing::kEnhanced);
    const auto clock = std::make_shared<song::PlaybackClock>(frame_samples);
    song::SequencePlayer player(std::make_shared<song::SequenceModel>(archive, sequence, song::AuxEffects{}, from), 0,
                                clock);
    std::vector<float> output;
    if (!RenderCtr(player, before, frame_samples, output))
    {
        return output;
    }

    song::ModelCatchUp catch_up;
    catch_up.Start(std::make_shared<song::SequenceModel>(archive, sequence, effects, to), clock);
    while (!catch_up.Finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clock->Offer(catch_up.Take());
    RenderCtr(player, frames - before, frame_samples, output);

    return output;
}

// The frames of a switch's output from frame `before` + 1 on that differ from the reference of the new mode through a
// FrameResampler: in strict mixing, the player's frames have to be the enhanced mode's frames, from frame `first` +
// `before` on, at the rate of strict mixing. The resampler takes the frame that crossfades first. The output's last
// frame can fade out, and the comparison leaves it out.
std::size_t ResampledDifferences(std::span<const float> switched, const CtrRun& expected, std::size_t first,
                                 std::size_t before)
{
    const std::size_t kToFloats = song::CtrModel::kFrameSamples * 2;
    song::FrameResampler resampler(expected.frame_floats / 2, kToFloats / 2);
    std::vector<float> resampled(kToFloats);
    std::size_t differences = 0;
    for (std::size_t frame = before; (frame + 2) * kToFloats <= switched.size() &&
                                     (first + frame + 1) * expected.frame_floats <= expected.floats.size();
         frame++)
    {
        resampler.Process(std::span<const float>(expected.floats)
                              .subspan((first + frame) * expected.frame_floats, expected.frame_floats),
                          resampled);
        if (frame > before && !std::equal(resampled.begin(), resampled.end(),
                                          switched.begin() + static_cast<std::ptrdiff_t>(frame * kToFloats)))
        {
            differences++;
        }
    }

    return differences;
}

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
                      const CtrEffect* effect, Totals& totals)
{
    const std::string name = strict.Sounds()[sound].name + ValuesText(values) + ChangesText(changes);
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound, values, changes));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: can't be read. %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }

    totals.sequences++;
    totals.looped += sequence->loop ? 1 : 0;
    CtrRun expected;
    try
    {
        expected = RunCtrModel(firmware, strict, sound, frames, values, changes, enhanced, effect);
        if (!changes.empty() &&
            RunCtrModel(firmware, strict, sound, frames, values, {}, enhanced, effect).samples != expected.samples)
        {
            totals.affected++;
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: archive mode can't play it. %s\n", name.c_str(), error.what());
        totals.failed++;
        return;
    }

    // Archive mode runs every frame of the check. It stops short only when it can't start the sequence, or when the DSP
    // stops answering. The comparisons below cover only the frames that both sides hold, and would pass a short run.
    if (expected.after.size() < frames)
    {
        if (expected.after.empty())
        {
            std::fprintf(stderr, "%s: archive mode doesn't start it.\n", name.c_str());
        }
        else
        {
            std::fprintf(stderr, "%s: archive mode stops after %zu of %zu frames.\n", name.c_str(),
                         expected.after.size(), frames);
        }
        totals.failed++;
        return;
    }

    // The player starts at the model's first frame that runs a tick. The worker renders in blocks.
    std::size_t first = 0;
    while (first < expected.after.size() && expected.after[first] == 0)
    {
        first++;
    }
    const song::Mixing mixing = enhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict;
    const std::size_t frame_floats = expected.frame_floats;
    const song::AuxEffects effects = effect != nullptr ? CtrModelEffects(*effect) : song::AuxEffects{};
    totals.sent += expected.sent ? 1 : 0;
    const auto model = std::make_shared<song::SequenceModel>(archive, sequence, effects, mixing);
    const auto clock = std::make_shared<song::PlaybackClock>(model->FrameSamples());
    song::SequencePlayer player(model, 0, clock);
    std::vector<float> output;
    std::vector<float> block(kBlock * 2);
    while (output.size() < frames * frame_floats)
    {
        const std::size_t done = player.Render(block.data(), kBlock);
        output.insert(output.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(done * 2));
        if (done < kBlock)
        {
            break;
        }
    }
    const std::size_t heard = output.size() / frame_floats;
    const bool ended = heard < frames;
    output.resize(heard > 1 ? (heard - 1) * frame_floats : 0);
    std::size_t player_first = 0;
    std::size_t differences = CtrPlayerDifferences(output, 1, expected, first, &player_first);

    // The output of a sequence without a loop that ends before the check does ends where the reference has fallen
    // silent for good. A loop's output ends at the end of the loop's pass, and the reference plays on round the loop.
    // With an effect, the reference can keep an offset of a step or two, and stays within ±8.
    const std::size_t rest = std::min((first + heard) * frame_floats, expected.samples.size());
    const auto louder_than = [&expected, rest](int limit)
    {
        return std::any_of(expected.samples.begin() + static_cast<std::ptrdiff_t>(rest), expected.samples.end(),
                           [limit](int16_t sample) { return std::abs(static_cast<int>(sample)) > limit; });
    };

    const bool tail = ended && !sequence->loop;
    const bool sounds_on = tail && louder_than(effect != nullptr ? 8 : 0);
    totals.quiet_tails += tail && !sounds_on && louder_than(0) ? 1 : 0;

    // A seek to the middle of the output, then one back to the second frame.
    std::size_t seeks = 0;
    for (const std::size_t frame : {first + heard / 2, std::size_t{1}})
    {
        if (frame < first + heard && model->SeekFrame(frame))
        {
            seeks += enhanced ? CtrPlayerDifferences(model->FloatOutput(), 0, expected, frame)
                              : CtrDifferences(model->Output(), 0, std::span<const int16_t>(expected.samples),
                                               frame_floats, frame, 1.0f);
        }
    }

    // A switch halfway through the output, to a new model of the same sequence. In strict mixing, the frame that
    // crossfades sums the two models' frames, and only rounds to the reference's samples. In enhanced mixing, the
    // switch comes from strict mixing, and the player's frames after it are the reference's through a resampler. With
    // an effect, only the new model plays it, and the frames after the one that crossfades have to be the reference's.
    // The output without the effect can end sooner. The switch then comes halfway through that output.
    std::string switch_text;
    const std::size_t kStrictFloats = song::CtrModel::kFrameSamples * 2;
    std::size_t before = heard / 2;
    std::vector<float> switched;
    if (before > 1)
    {
        switched = RunCtrSwitch(archive, sequence, before, frames, song::Mixing::kStrict, mixing, effects);
    }
    if (effect != nullptr && !switched.empty() && switched.size() < before * kStrictFloats)
    {
        before = switched.size() / kStrictFloats / 2;
        switched = before > 1 ? RunCtrSwitch(archive, sequence, before, frames, song::Mixing::kStrict, mixing, effects)
                              : std::vector<float>{};
    }
    if (!switched.empty())
    {
        const std::size_t switched_frames = switched.size() / kStrictFloats;
        std::size_t switch_differences = 0;
        if (enhanced)
        {
            switch_differences = ResampledDifferences(switched, expected, first, before);
        }
        else
        {
            std::vector<int16_t> rounded(switched_frames > 1 ? (switched_frames - 1) * frame_floats : 0);
            for (std::size_t i = 0; i < rounded.size(); i++)
            {
                rounded[i] = static_cast<int16_t>(std::lround(switched[i] * 32768.0f));
            }
            switch_differences = CtrDifferences(std::span<const int16_t>(rounded), effect != nullptr ? before + 1 : 1,
                                                std::span<const int16_t>(expected.samples), frame_floats, first, 1.0f);
        }
        const bool ends_elsewhere = std::min(switched_frames, frames) != std::min(heard, frames);
        totals.switches++;
        totals.exact += switch_differences == 0 && !ends_elsewhere ? 1 : 0;
        if (switch_differences > 0)
        {
            switch_text += ", " + std::to_string(switch_differences) + " after a switch halfway";
        }
        if (ends_elsewhere)
        {
            switch_text += ", and the output after a switch ends at frame " + std::to_string(switched_frames);
        }
    }

    const std::string loop =
        sequence->loop ? std::to_string(sequence->loop->start) + " to " + std::to_string(sequence->loop->end) : "none";
    if (differences + seeks > 0 || sounds_on || heard == 0 || !switch_text.empty())
    {
        std::fprintf(
            stderr, "%s: %zu of %zu frames differ from the player, first %zu, %zu after seeks%s%s (loop %s).\n",
            name.c_str(), differences, heard, player_first, seeks,
            sounds_on ? ", and the reference sounds after the output's end" : "", switch_text.c_str(), loop.c_str());
        totals.failed++;
    }

    // The notes, up to the output's end. A note that the model cuts in the tick it starts is missing from the
    // arrangement, and inaudible. Missing notes are listed apart from the differences.
    const NoteCheck notes = CheckNotes(expected, *sequence, first + heard);
    totals.notes += notes.checked;
    totals.missed_notes += notes.missed;
    totals.missed_later += notes.missed_later;
    totals.missed_legatos += notes.missed_legatos;
    if (notes.missed > 0)
    {
        std::fprintf(
            stderr,
            "%s (loop %s): %zu of %zu notes missing from the arrangement, %zu of them legatos and %zu after the "
            "first pass.\n",
            name.c_str(), loop.c_str(), notes.missed, notes.checked, notes.missed_legatos, notes.missed_later);
    }
}

// The Wii's frames: 96 stereo samples at 32 kHz, in 3 ms.
constexpr std::size_t kWiiFrameSamples = ntdlib::rvl::ax::kSamplesPerFrame;

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

// The output of WSF's archive mode for a sequence of the Wii, for up to `frames` frames, and the tick count after each
// frame, with the variables' starting values `variables`, the game's `changes` and `effects` on the aux buses. Without
// `microcode`, ntdlib's model of the microcode of the build of AX that `build` gives mixes in place of the DSP. The run
// stops after the first frame in which nothing plays or sounds, or rings on an effect. With an effect, a run goes on
// after the output last had sound for as long as Hokuto's model does (song::WiiRingingFrames). Empty if the sequence
// doesn't start.
WiiURun RunWiiModel(const ntdlib::rsar::SoundArchive& archive, uint32_t sound, std::size_t frames,
                    const ntdlib::Microcode* microcode, ntdlib::SndBuild build, const song::StartValues& variables,
                    const song::Changes& changes, const WiiEffects& effects)
{
    WiiURun run;
    auto model = std::make_unique<ntdlib::rvl::snd::ArchiveModel>();
    model->build = build;
    if (microcode == nullptr)
    {
        model->StartWithRenderer(archive);
    }
    else if (!model->Start(*microcode, archive))
    {
        return run;
    }
    model->engine->SetEffects(effects.setups, {}, sound);
    ntdlib::rvl::snd::SeqPlayer* player = model->engine->StartSequence(sound);
    if (player == nullptr)
    {
        return run;
    }
    for (const auto& [number, value] : variables)
    {
        player->SetVariable(*model->engine, number, value);
    }

    const int ringing_frames = song::WiiRingingFrames(effects.effects);
    int ringing = 0;
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        // The game's changes come before the frame.
        for (const song::VariableChange& change : changes)
        {
            if (song::ChangeFrame(change.time, song::kWiiFrameMs) == frame)
            {
                player->SetVariable(*model->engine, change.variable, change.value);
            }
        }

        if (!model->RunFrame(run.samples))
        {
            break;
        }
        run.after.push_back(player->TickCount());
        const auto last = std::span<const int16_t>(run.samples).last(kWiiFrameSamples * 2);
        ringing = xsflib::rip::Audible(last) ? ringing_frames : std::max(ringing - 1, 0);
        if (!model->engine->IsBusy() && !(model->engine->HasEffects() && ringing > 0))
        {
            break;
        }
    }

    return run;
}

// The player's output for `before` frames on a model of the Wii without effects, then on a model with `effects`, up to
// `frames` frames in all. A catch-up runs the new model up to the player's frame, and the player takes it at its next
// frame. The program does the same after an edit during playback.
std::vector<float> RunWiiSwitch(const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive,
                                const std::shared_ptr<const song::Sequence>& sequence, const song::AuxEffects& effects,
                                std::size_t before, std::size_t frames)
{
    const auto clock = std::make_shared<song::PlaybackClock>(kWiiFrameSamples);
    song::SequencePlayer player(std::make_shared<song::SequenceModel>(archive, sequence), 0, clock);
    std::vector<float> output;
    if (!RenderFrames(player, before, kWiiFrameSamples, output))
    {
        return output;
    }

    song::ModelCatchUp catch_up;
    catch_up.Start(std::make_shared<song::SequenceModel>(archive, sequence, effects), clock);
    while (!catch_up.Finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clock->Offer(catch_up.Take());
    RenderFrames(player, frames, kWiiFrameSamples, output);

    return output;
}

// The frames of `samples` from frame `from` on that differ from `expected`'s frames from `first` on, up to the end of
// either, for the Wii's frames (see WiiUDifferences).
template <class Sample>
std::size_t WiiDifferences(std::span<const Sample> samples, std::size_t from, const WiiURun& expected,
                           std::size_t first, float scale, std::size_t* first_difference = nullptr)
{
    std::size_t differences = 0;
    const std::size_t kFloats = kWiiFrameSamples * 2;
    for (std::size_t frame = from;
         (frame + 1) * kFloats <= samples.size() && (first + frame + 1) * kFloats <= expected.samples.size(); frame++)
    {
        const std::size_t at = (first + frame) * kFloats;
        for (std::size_t i = 0; i < kFloats; i++)
        {
            if (static_cast<float>(samples[frame * kFloats + i]) !=
                static_cast<float>(expected.samples[at + i]) * scale)
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

// Checks Hokuto's playback of sequence sound `sound` of a Wii archive against WSF's archive mode, with the variables'
// starting values `values`, the game's `changes` and `effects`: the frames of a run from the start, the frames after
// seeks back and forth, and the player's output after its first frame. That frame fades in. Hokuto's run ends after a
// silent frame once nothing plays, sounds or rings on an effect. The expected output after it has to be silent, and so
// does Hokuto's output after the reference's end. With an effect, a frame within ±8 counts as silent, and the run has
// to end in the reference's last frame: the frame in which the effects stop ringing. A switch halfway through the
// sequence follows, from a model without the effect. Every frame of the Wii mixes: after the frame that crossfades, the
// output has to be the reference's. Prints a line when anything differs. Without `microcode`, the reference mixes with
// ntdlib's model of the microcode.
void CheckWiiSequence(const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive, uint32_t sound,
                      std::size_t frames, const ntdlib::Microcode* microcode, ntdlib::SndBuild build,
                      const song::StartValues& values, const song::Changes& changes, const WiiEffects& effects,
                      Totals& totals)
{
    const std::string name = archive->Sounds()[sound].name + ValuesText(values) + ChangesText(changes);
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound, values, changes, build));
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't be read. %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }

    totals.sequences++;
    totals.looped += sequence->loop ? 1 : 0;
    const WiiURun expected = RunWiiModel(*archive, sound, frames, microcode, build, values, changes, effects);
    if (!changes.empty() &&
        RunWiiModel(*archive, sound, frames, microcode, build, values, {}, effects).samples != expected.samples)
    {
        totals.affected++;
    }
    const auto model = std::make_shared<song::SequenceModel>(archive, sequence, effects.effects);
    std::vector<int16_t> played;
    for (bool more = model->SeekFrame(0); more && played.size() < frames * kWiiFrameSamples * 2;
         more = model->RunFrame())
    {
        const std::span<const int16_t> output = model->Output();
        played.insert(played.end(), output.begin(), output.end());
    }

    // The reference stops once nothing plays or sounds. The output of either past the end of the other has to be
    // silent.
    std::size_t differences = WiiDifferences(std::span<const int16_t>(played), 0, expected, 0, 1.0f);
    const std::size_t played_frames = played.size() / (kWiiFrameSamples * 2);
    const std::size_t rest = std::min(played.size(), expected.samples.size());
    const auto silent_from = [rest](std::span<const int16_t> samples)
    {
        return std::all_of(samples.begin() + static_cast<std::ptrdiff_t>(rest), samples.end(),
                           [](int16_t sample) { return sample == 0; });
    };
    if (!silent_from(expected.samples) || !silent_from(played) ||
        (!effects.setups.empty() && played.size() != expected.samples.size()))
    {
        differences++;
    }

    // Seeks back and forth restore the snapshots that the run took.
    std::size_t seeks = 0;
    for (const std::size_t frame : {played_frames / 2, played_frames / 4, std::size_t{1}, played_frames * 3 / 4})
    {
        if (frame < played_frames && model->SeekFrame(frame))
        {
            seeks += WiiDifferences(model->Output(), 0, expected, frame, 1.0f);
        }
    }

    // The player starts at the model's first frame that runs a tick. The worker renders in blocks.
    std::size_t first = 0;
    while (first < expected.after.size() && expected.after[first] == 0)
    {
        first++;
    }
    const auto clock = std::make_shared<song::PlaybackClock>(model->FrameSamples());
    song::SequencePlayer player(model, 0, clock);
    std::vector<float> output;
    std::vector<float> block(kBlock * 2);
    while (output.size() < frames * kWiiFrameSamples * 2)
    {
        const std::size_t done = player.Render(block.data(), kBlock);
        output.insert(output.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(done * 2));
        if (done < kBlock)
        {
            break;
        }
    }
    const std::size_t heard = output.size() / (kWiiFrameSamples * 2);
    output.resize(heard > 1 ? (heard - 1) * kWiiFrameSamples * 2 : 0);
    std::size_t player_first = 0;
    std::size_t player_differences =
        WiiDifferences(std::span<const float>(output), 1, expected, first, 1.0f / 32768.0f, &player_first);
    const std::size_t compared = std::min(output.size(), expected.samples.size() - first * kWiiFrameSamples * 2);
    if (std::any_of(output.begin() + static_cast<std::ptrdiff_t>(compared), output.end(),
                    [](float sample) { return sample != 0.0f; }))
    {
        player_differences++;
    }

    // The effect alters a run whose output differs from the model's without the effect.
    if (!effects.setups.empty())
    {
        const auto dry = std::make_shared<song::SequenceModel>(archive, sequence);
        std::vector<int16_t> dry_played;
        for (bool more = dry->SeekFrame(0); more && dry_played.size() < frames * kWiiFrameSamples * 2;
             more = dry->RunFrame())
        {
            dry_played.insert(dry_played.end(), dry->Output().begin(), dry->Output().end());
        }
        totals.altered += dry_played != played ? 1 : 0;
    }

    // A switch halfway through the sequence, or halfway through the output when the output ends sooner. It ends where
    // the output without the switch ends. The output before the switch has no effect, and can end sooner than the
    // output with it. The switch then comes halfway through that output, as on the Wii U.
    std::string switch_text = "no switch";
    std::size_t switch_differences = 0;
    const auto halfway = static_cast<std::size_t>(
        song::MsOfTick(*sequence, sequence->length / 2.0) /
        (static_cast<double>(song::kWiiFrameMs.numerator) / static_cast<double>(song::kWiiFrameMs.denominator)));
    std::size_t before = std::min({halfway, frames / 2, heard / 2});
    std::vector<float> switched;
    if (!effects.setups.empty() && before > 1)
    {
        switched = RunWiiSwitch(archive, sequence, effects.effects, before, frames);
    }
    if (!switched.empty() && switched.size() < before * kWiiFrameSamples * 2)
    {
        before = switched.size() / (kWiiFrameSamples * 2) / 2;
        switched = before > 1 ? RunWiiSwitch(archive, sequence, effects.effects, before, frames) : std::vector<float>{};
    }
    if (!switched.empty())
    {
        const std::size_t switched_frames = switched.size() / (kWiiFrameSamples * 2);
        switched.resize(switched_frames > 1 ? (switched_frames - 1) * kWiiFrameSamples * 2 : 0);
        switch_differences =
            WiiDifferences(std::span<const float>(switched), before + 1, expected, first, 1.0f / 32768.0f);
        switch_differences += switched_frames != heard ? 1 : 0;
        totals.switches++;
        totals.exact += switch_differences == 0 ? 1 : 0;
        switch_text = std::to_string(switch_differences) + " frames that differ after a switch";
    }

    if (differences + seeks + player_differences + switch_differences > 0 || expected.samples.empty())
    {
        std::printf(
            "%s: %zu of %zu frames differ from the start, %zu after seeks, %zu of %zu from the player, first "
            "%zu, %s (loop %s).\n",
            name.c_str(), differences, played_frames, seeks, player_differences, heard, player_first,
            switch_text.c_str(),
            sequence->loop
                ? (std::to_string(sequence->loop->start) + " to " + std::to_string(sequence->loop->end)).c_str()
                : "none");
        totals.failed++;
    }
    std::fflush(stdout);
}

// Checks every sequence of the Wii archive at `path` for `seconds`, against WSF's archive mode with the microcode and
// the build of NW4R SND in the main.dol at `dol_path`. With `variables` or `changes`, each choice of each variable that
// a sequence reads gets a run, as on the 3DS. Only the sounds whose index leaves `part` after a division by `parts`
// take part. Returns the tool's exit code.
int CheckWiiArchive(const std::filesystem::path& path, const std::filesystem::path& dol_path, int seconds,
                    bool variables, bool changes, bool archive_build, std::size_t part, std::size_t parts,
                    const WiiEffects& effects, std::string_view effect_name)
{
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> dol;
    if (!xsflib::ReadFile(path, bytes) || !xsflib::ReadFile(dol_path, dol))
    {
        std::fprintf(stderr, "The archive or the main.dol can't be read.\n");
        return 2;
    }
    const std::optional<ntdlib::Microcode> microcode = ntdlib::FindMicrocode(dol);
    if (!microcode)
    {
        std::fprintf(stderr, "The main.dol holds no AX microcode.\n");
        return 2;
    }
    std::shared_ptr<const ntdlib::rsar::SoundArchive> archive;
    try
    {
        archive = song::LoadWiiArchive(std::move(bytes), xsflib::Utf8(path.filename()), ntdlib::rvl::snd::kRsarOptions);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }

    Totals totals;
    const ntdlib::SndBuild build = archive_build ? ntdlib::sequence_export::BuildOfRsarVersion(archive->Version())
                                                 : song::ReadWiiBuild(dol).value_or(ntdlib::SndBuild{});
    const auto frames =
        static_cast<std::size_t>(song::FramesIn(static_cast<uint64_t>(seconds) * 1000, song::kWiiFrameMs));
    std::size_t reading = 0;
    for (std::size_t sound = 0; sound < archive->Sounds().size(); sound++)
    {
        if (archive->Sounds()[sound].type != ntdlib::rsar::SoundType::kSequence || sound % parts != part)
        {
            continue;
        }

        const auto index = static_cast<uint32_t>(sound);
        if (!variables && !changes)
        {
            CheckWiiSequence(archive, index, frames, &*microcode, build, {}, {}, effects, totals);
            continue;
        }

        // A sequence that can't be read gets a line from the run without values.
        song::Sequence plain;
        try
        {
            plain = song::ReadSequence(*archive, index, {}, {}, build);
        }
        catch (const std::exception&)
        {
            CheckWiiSequence(archive, index, frames, nullptr, build, {}, {}, effects, totals);
            continue;
        }

        // A change comes 2 ms into the frame halfway through the sequence's length or through the check.
        const std::vector<song::Variable>& read = changes ? plain.read_variables : plain.variables;
        const double middle = std::min(song::MsOfTick(plain, plain.length / 2.0), seconds * 500.0);
        const uint64_t frame = song::FramesIn(static_cast<uint64_t>(middle), song::kWiiFrameMs);
        const auto time =
            static_cast<uint32_t>(frame * song::kWiiFrameMs.numerator / song::kWiiFrameMs.denominator + 2);
        reading += read.empty() ? 0 : 1;
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
                    CheckWiiSequence(archive, index, frames, nullptr, build, {}, {{time, variable.number, set}},
                                     effects, totals);
                }
                else
                {
                    CheckWiiSequence(archive, index, frames, nullptr, build, {{variable.number, set}}, {}, effects,
                                     totals);
                }
            }
        }
    }

    const std::string runs = variables || changes ? std::to_string(reading) + " sequences read variables. Their " : "";
    const char* checked = changes ? "runs with a change were" : variables ? "runs with a choice were" : "Wii sequences";
    const std::string build_text = build.date != 0      ? "NW4R SND of " + std::to_string(build.date)
                                   : build.october_2006 ? std::string("NW4R SND of 2006, without a banner")
                                                        : std::string("NW4R SND of 2007, without a banner");
    std::printf("%s%zu %s checked for %d s against %s and %s, %zu with a loop. %zu differ, and %zu can't be read.\n",
                runs.c_str(), totals.sequences, checked, seconds,
                variables || changes                                                 ? "ntdlib's model of the microcode"
                : ntdlib::IdentifyBuild(*microcode) == ntdlib::AxBuild::kOctober2006 ? "the microcode of October 2006"
                                                                                     : "the microcode of December 2006",
                build_text.c_str(), totals.looped, totals.failed, totals.unread);
    if (changes)
    {
        std::printf("The change altered the model's output in %zu runs.\n", totals.affected);
    }
    if (!effects.setups.empty())
    {
        std::printf(
            "With %s on bus A, the effect altered the output of %zu runs, and %zu of %zu switches came out "
            "exact.\n",
            std::string(effect_name).c_str(), totals.altered, totals.exact, totals.switches);
    }

    return totals.failed == 0 && totals.unread == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    // --reverb n puts nn::audio's I3DL2 preset n on aux bus A, for the model and for the player. --effect puts an
    // effect of the Wii U, the Wii or the 3DS there. --variables checks the choices of the variables, and --changes
    // checks them as changes while the sequence plays. --enhanced plays in enhanced mixing.
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);

    // --firmware names the DSP firmware that the reference of a 3DS archive plays on, and --part k/n checks part k of n
    // of the sequences.
    std::string firmware_path;
    const auto firmware_option = std::find(arguments.begin(), arguments.end(), "--firmware");
    if (firmware_option != arguments.end() && firmware_option + 1 != arguments.end())
    {
        firmware_path = *(firmware_option + 1);
        arguments.erase(firmware_option, firmware_option + 2);
    }

    // --dol names the main.dol of a Wii archive's game. --archive-build takes the build from the archive's version.
    std::string dol_path;
    const auto dol_option = std::find(arguments.begin(), arguments.end(), "--dol");
    if (dol_option != arguments.end() && dol_option + 1 != arguments.end())
    {
        dol_path = *(dol_option + 1);
        arguments.erase(dol_option, dol_option + 2);
    }
    const auto archive_build_option = std::find(arguments.begin(), arguments.end(), "--archive-build");
    const bool archive_build = archive_build_option != arguments.end();
    if (archive_build)
    {
        arguments.erase(archive_build_option);
    }
    std::size_t part = 0;
    std::size_t parts = 1;
    const auto part_option = std::find(arguments.begin(), arguments.end(), "--part");
    if (part_option != arguments.end() && part_option + 1 != arguments.end())
    {
        const std::string& text = *(part_option + 1);
        const std::size_t slash = text.find('/');
        const int k = std::atoi(text.substr(0, slash).c_str());
        const int n = slash != std::string::npos ? std::atoi(text.substr(slash + 1).c_str()) : 0;
        if (k < 1 || n < k)
        {
            std::fprintf(stderr, "--part takes k/n, with k from 1 to n.\n");
            return 2;
        }
        part = static_cast<std::size_t>(k - 1);
        parts = static_cast<std::size_t>(n);
        arguments.erase(part_option, part_option + 2);
    }
    const auto enhanced_option = std::find(arguments.begin(), arguments.end(), "--enhanced");
    const bool enhanced = enhanced_option != arguments.end();
    if (enhanced)
    {
        arguments.erase(enhanced_option);
    }
    atk::EngineOptions options;
    options.enhanced = enhanced;
    song::AuxEffects effects;
    int preset = -1;
    if (arguments.size() > 2 && arguments[1] == "--reverb")
    {
        preset = std::atoi(arguments[2].c_str());
        if (preset >= 0 && preset < static_cast<int>(ntdlib::nx::audio::kI3dl2ReverbPresets.size()))
        {
            options.aux_reverbs[0] = ntdlib::nx::audio::kI3dl2ReverbPresets[static_cast<std::size_t>(preset)];
            effects[0].push_back(*options.aux_reverbs[0]);
        }
        arguments.erase(arguments.begin() + 1, arguments.begin() + 3);
    }
    WiiUEffects wii_u_effects;
    WiiEffects wii_effects;
    const CtrEffect* ctr_effect = nullptr;
    std::string effect_name;
    bool wii_u_effect = false;
    if (arguments.size() > 2 && arguments[1] == "--effect")
    {
        wii_u_effect = true;
        effect_name = arguments[2];
        for (const auto& [name, type] : kWiiUEffects)
        {
            if (arguments[2] == name)
            {
                const song::WiiUEffect effect = song::WiiUEffectOf(type, song::WiiUStartingParameters(type));
                wii_u_effects.setups.push_back({cafe::kEffectDeviceTv, 0, type, effect.words, {}});
                wii_u_effects.effects[0].push_back(effect);
            }
        }
        for (const auto& [name, type] : kWiiEffects)
        {
            if (arguments[2] == name)
            {
                const song::WiiEffect effect = song::WiiEffectOf(type, song::WiiStartingParameters(type));
                ntdlib::rvl::snd::EffectSetup& setup = wii_effects.setups.emplace_back();
                setup.type = type;
                std::copy(effect.words.begin(), effect.words.end(), setup.params.begin());
                wii_effects.effects[0].push_back(effect);
            }
        }
        for (const CtrEffect& effect : kCtrEffects)
        {
            if (arguments[2] == effect.name)
            {
                ctr_effect = &effect;
            }
        }
        arguments.erase(arguments.begin() + 1, arguments.begin() + 3);
    }
    const bool variables = arguments.size() > 1 && arguments[1] == "--variables";
    const bool changes = arguments.size() > 1 && arguments[1] == "--changes";
    if (variables || changes)
    {
        arguments.erase(arguments.begin() + 1);
    }
    const bool unknown_effect =
        wii_u_effect && wii_u_effects.setups.empty() && wii_effects.setups.empty() && ctr_effect == nullptr;
    if (arguments.size() < 2 || arguments.size() > 3 || (preset != -1 && effects[0].empty()) || unknown_effect ||
        (preset != -1 && wii_u_effect))
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_playback [--reverb n | --effect kind] [--variables | --changes] "
                     "[--enhanced] <archive> [seconds]\n"
                     "       hokuto_check_playback --firmware <dsp firmware> [--part k/n] [--effect kind] "
                     "[--variables | --changes] <3DS archive> [seconds]\n"
                     "       hokuto_check_playback --enhanced [--part k/n] [--effect kind] [--variables | --changes] "
                     "<3DS archive> [seconds]\n"
                     "       hokuto_check_playback --dol <main.dol> [--archive-build] [--part k/n] [--effect kind] "
                     "[--variables | --changes] <Wii archive> [seconds]\n");
        return 2;
    }

    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    const int seconds = arguments.size() > 2 ? std::max(std::atoi(arguments[2].c_str()), 1) : 30;
    if (!dol_path.empty())
    {
        if (preset != -1 || !wii_u_effects.setups.empty() || ctr_effect != nullptr || enhanced ||
            !firmware_path.empty())
        {
            std::fprintf(stderr,
                         "A Wii archive takes no other option but --dol, --archive-build, --part, --effect with an "
                         "effect of the Wii, --variables and --changes.\n");
            return 2;
        }

        return CheckWiiArchive(path, xsflib::PathFromUtf8(dol_path), seconds, variables, changes, archive_build, part,
                               parts, wii_effects, effect_name);
    }
    if (!wii_effects.setups.empty())
    {
        std::fprintf(stderr, "An effect of the Wii plays only on a Wii archive, with --dol.\n");
        return 2;
    }
    const std::size_t frames = static_cast<std::size_t>(seconds) * kFramesPerSecond;
    std::shared_ptr<const sar::SoundArchive> archive;
    std::optional<sar::SoundArchive> strict;
    bool wii_u = false;
    bool ctr = false;
    try
    {
        archive = song::OpenArchive(path);
        std::vector<uint8_t> bytes;
        xsflib::ReadFile(path, bytes);
        wii_u = song::FlavorOfArchive(bytes) == sar::Flavor::kCafe;
        ctr = song::FlavorOfArchive(bytes) == sar::Flavor::kCtr;
        strict.emplace(sar::SoundArchive::Load(std::move(bytes), wii_u ? kWiiUStrict
                                                                 : ctr ? ntdlib::ctr::kSarOptions
                                                                       : kStrict));

        // The model of a Wii U archive plays the group files beside it too.
        if (wii_u)
        {
            for (const std::vector<uint8_t>& group : archive->GroupFiles())
            {
                strict->AddGroupFile(group);
            }
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }

    // A 3DS archive's reference plays on the firmware, with each variable at its starting value, or with --enhanced on
    // the model's enhanced mode. It plays dry, or with an effect of the 3DS on bus A. Teakra prints the DSP's accesses
    // that it doesn't model on stdout. The tool's lines go to stderr instead.
    if (ctr)
    {
        std::vector<uint8_t> firmware;
        if (preset != -1 || (wii_u_effect && ctr_effect == nullptr) || enhanced == !firmware_path.empty())
        {
            std::fprintf(stderr,
                         "A 3DS archive takes --firmware or --enhanced, and no other option but --part, --effect with "
                         "an effect of the 3DS, --variables and --changes.\n");
            return 2;
        }
        if (!enhanced && !xsflib::ReadFile(xsflib::PathFromUtf8(firmware_path), firmware))
        {
            std::fprintf(stderr, "can't read %s\n", firmware_path.c_str());
            return 2;
        }
        ntdlib::ctr::SilenceStdout();
        const std::size_t ctr_frames =
            song::FramesIn(static_cast<uint64_t>(seconds) * 1000, song::FrameMsOf(sar::Flavor::kCtr));
        Totals totals;
        std::size_t reading = 0;
        for (std::size_t sound = 0; sound < strict->Sounds().size(); sound++)
        {
            if (strict->Sounds()[sound].type != sar::SoundType::kSequence || sound % parts != part)
            {
                continue;
            }

            const auto index = static_cast<uint32_t>(sound);
            if (!variables && !changes)
            {
                CheckCtrSequence(firmware, *strict, archive, index, ctr_frames, {}, {}, enhanced, ctr_effect, totals);
                continue;
            }

            // A sequence that can't be read gets a line from the run without values.
            song::Sequence plain;
            try
            {
                plain = song::ReadSequence(*archive, index);
            }
            catch (const std::exception&)
            {
                CheckCtrSequence(firmware, *strict, archive, index, ctr_frames, {}, {}, enhanced, ctr_effect, totals);
                continue;
            }

            // A change comes 2 ms into the frame halfway through the sequence's length or through the check.
            const std::vector<song::Variable>& read = changes ? plain.read_variables : plain.variables;
            const double middle = std::min(song::MsOfTick(plain, plain.length / 2.0), seconds * 500.0);
            const uint64_t frame = song::FramesIn(static_cast<uint64_t>(middle), kCtrFrameMs);
            const auto time = static_cast<uint32_t>(frame * kCtrFrameMs.numerator / kCtrFrameMs.denominator + 2);
            reading += read.empty() ? 0 : 1;
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
                        CheckCtrSequence(firmware, *strict, archive, index, ctr_frames, {},
                                         {{time, variable.number, set}}, enhanced, ctr_effect, totals);
                    }
                    else
                    {
                        CheckCtrSequence(firmware, *strict, archive, index, ctr_frames, {{variable.number, set}}, {},
                                         enhanced, ctr_effect, totals);
                    }
                }
            }
        }

        const std::string runs =
            variables || changes ? std::to_string(reading) + " sequences read variables. Their " : "";
        const char* checked = changes     ? "runs with a change were"
                              : variables ? "runs with a choice were"
                                          : "3DS sequences";
        std::fprintf(stderr,
                     "%s%zu %s checked for %d s, %zu with a loop. %zu differ, and %zu can't be read. %zu of %zu notes "
                     "missing from the arrangement, %zu of them legatos and %zu after the first pass of a loop. %zu of "
                     "%zu switches kept the reference's output.\n",
                     runs.c_str(), totals.sequences, checked, seconds, totals.looped, totals.failed, totals.unread,
                     totals.missed_notes, totals.notes, totals.missed_legatos, totals.missed_later, totals.exact,
                     totals.switches);
        if (changes)
        {
            std::fprintf(stderr, "The change altered the model's output in %zu runs.\n", totals.affected);
        }
        if (ctr_effect != nullptr)
        {
            std::fprintf(stderr,
                         "With %s on bus A, %zu runs sent sound to the bus. %zu ended while the reference gave samples "
                         "of ±8 at most.\n",
                         std::string(ctr_effect->name).c_str(), totals.sent, totals.quiet_tails);
        }

        return totals.failed == 0 && totals.unread == 0 ? 0 : 1;
    }
    if (ctr_effect != nullptr)
    {
        std::fprintf(stderr, "An effect of the 3DS plays only on a 3DS archive.\n");
        return 2;
    }
    if (!firmware_path.empty() || parts != 1)
    {
        std::fprintf(stderr,
                     "--firmware checks only the 3DS's archives, and --part only those of the 3DS and the Wii.\n");
        return 2;
    }

    // A Wii U archive plays dry or with its effect, with each variable at its starting value. --effect is for the Wii
    // U's archives, and --reverb, --variables and --changes for the Switch's.
    if (wii_u && (preset != -1 || variables || changes))
    {
        std::fprintf(stderr, "--reverb, --variables and --changes check only the Switch's archives.\n");
        return 2;
    }
    if (!wii_u && wii_u_effect)
    {
        std::fprintf(stderr, "--effect checks only the Wii U's archives.\n");
        return 2;
    }
    if (wii_u)
    {
        Totals totals;
        for (std::size_t sound = 0; sound < strict->Sounds().size(); sound++)
        {
            if (strict->Sounds()[sound].type == sar::SoundType::kSequence)
            {
                CheckWiiUSequence(*strict, archive, static_cast<uint32_t>(sound),
                                  static_cast<std::size_t>(seconds) * kWiiUFramesPerSecond, wii_u_effects,
                                  enhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict, totals);
            }
        }

        const std::string with = std::string(wii_u_effect ? " with " + effect_name + " on bus A" : "") +
                                 (enhanced ? " in enhanced mixing" : "");
        std::printf("%zu Wii U sequences checked for %d s%s, %zu with a loop. %zu differ, and %zu can't be read.\n",
                    totals.sequences, seconds, with.c_str(), totals.looped, totals.failed, totals.unread);
        if (wii_u_effect)
        {
            std::printf("%zu ended while the effect's tail gave samples of ±8 at most.\n", totals.quiet_tails);
        }
        PrintSwitches(totals);

        return totals.failed == 0 && totals.unread == 0 ? 0 : 1;
    }

    // With --variables, each choice of each variable that a sequence reads gets a run, -1 aside, and a variable without
    // choices a run with 1. With --changes, the variables that the sequence reads at any time get the same runs, each
    // with a change. A sequence that can't be read gets a line from the run without values.
    Totals totals;
    std::size_t reading = 0;
    for (std::size_t sound = 0; sound < strict->Sounds().size(); sound++)
    {
        if (strict->Sounds()[sound].type != sar::SoundType::kSequence)
        {
            continue;
        }

        const auto index = static_cast<uint32_t>(sound);
        if (!variables && !changes)
        {
            CheckSequence(*strict, archive, index, frames, options, effects, {}, {}, totals);
            continue;
        }

        song::Sequence plain;
        try
        {
            plain = song::ReadSequence(*archive, index);
        }
        catch (const std::exception&)
        {
            CheckSequence(*strict, archive, index, frames, options, effects, {}, {}, totals);
            continue;
        }

        // A change comes halfway through the sequence's length or through the check, 2 ms into a frame.
        const std::vector<song::Variable>& read = changes ? plain.read_variables : plain.variables;
        const double middle = std::min(song::MsOfTick(plain, plain.length / 2.0), seconds * 500.0);
        const auto time =
            static_cast<uint32_t>(static_cast<uint64_t>(middle) / kFrameMs.numerator * kFrameMs.numerator + 2);
        reading += read.empty() ? 0 : 1;
        for (const song::Variable& variable : read)
        {
            const std::vector<int> values = variable.choices.empty() ? std::vector{1} : variable.choices;
            for (const int value : values)
            {
                if (value == -1)
                {
                    continue;
                }

                const auto set = static_cast<int16_t>(value);
                if (changes)
                {
                    CheckSequence(*strict, archive, index, frames, options, effects, {}, {{time, variable.number, set}},
                                  totals);
                }
                else
                {
                    CheckSequence(*strict, archive, index, frames, options, effects, {{variable.number, set}}, {},
                                  totals);
                }
            }
        }
    }

    const std::string with = (preset >= 0 ? " with I3DL2 preset " + std::to_string(preset) + " on bus A" : "") +
                             (enhanced ? " in enhanced mixing" : "");
    const std::string runs = variables || changes ? std::to_string(reading) + " sequences read variables. Their " : "";
    const char* checked = changes ? "runs with a change were" : variables ? "runs with a choice were" : "sequences";
    std::printf(
        "%s%zu %s checked for %d s%s, %zu with a loop. %zu differ, and %zu can't be read. %zu of %zu notes missing "
        "from the arrangement, %zu of them after the first pass of a loop.\n",
        runs.c_str(), totals.sequences, checked, seconds, with.c_str(), totals.looped, totals.failed, totals.unread,
        totals.missed_notes, totals.notes, totals.missed_later);
    if (changes)
    {
        std::printf("The change altered the model's output in %zu runs.\n", totals.affected);
    }
    PrintSwitches(totals);

    return totals.failed == 0 && totals.unread == 0 ? 0 : 1;
}
