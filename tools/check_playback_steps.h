// SPDX-License-Identifier: MIT

// The steps that the playback checks of the GBA, the DS, DSE and the GBA's other drivers share. Each check plays a
// sound with Hokuto's player and compares the output with a reference: the console's player in ntdlib, run directly.
// The family's tool runs the reference and passes its output to these steps, at a full scale of 1 in interleaved
// stereo. The steps are the output from the start (StartCheck), the seeks (SeekCheck) and a switch halfway to a new
// model after a catch-up (SwitchCheck). The library hokuto_check_support contains them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "song/played.h"
#include "song/player.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::tools::playback_steps
{

// The stereo samples that the player renders at a time.
constexpr std::size_t kBlock = 256;

// The counts of the steps. Each tool's totals derive from it.
struct StepTotals
{
    std::size_t switches = 0;
    std::size_t exact = 0;
    std::size_t seeks = 0;
};

// What the output from the start shows: whether it ended before the check's frames, the frames heard, and the frames
// that differ from the reference.
struct StartResult
{
    bool ended = false;
    std::size_t heard = 0;
    std::size_t differences = 0;
};

// What the seeks give: the frames that differ from the reference, and the seeks that give no output.
struct SeekResult
{
    std::size_t differences = 0;
    std::size_t failed = 0;
};

// What the reference plays after an end of the output.
struct EndLevels
{
    bool sounds_on = false;
    float level = 0.0f;
    float widest = 0.0f;       // the widest span on a side within a frame
    float range = 0.0f;        // the widest span on a side in all
    std::size_t widest_at = 0; // the frame after the end with the widest span
};

// The player's frames in `seconds` of the output of `model`.
std::size_t CheckFrames(const song::SequenceModel& model, int seconds);

// Renders `frames` of the player's frames into `output`. Returns false if the output ends sooner.
bool Render(song::SequencePlayer& player, std::size_t frames, std::size_t frame_samples, std::vector<float>& output);

// The frames of `output` from frame `from` on that differ from `reference`, `frame_floats` floats a frame. The
// comparison starts `offset` floats into the reference. In strict mixing, the player's floats are the reference's
// 16-bit samples, scaled.
std::size_t Differences(std::span<const float> output, std::size_t from, std::span<const float> reference,
                        std::size_t frame_floats, std::size_t offset = 0);

// The output from the start of a player of `model`, for `frames` frames at most. The comparison starts `offset` floats
// into `reference`, and leaves out the player's first frame. The output's last frame can fade out, and the comparison
// leaves it out too.
StartResult StartCheck(const std::shared_ptr<song::SequenceModel>& model, std::size_t frames,
                       std::span<const float> reference, std::size_t offset = 0);

// A seek of `model` to the middle of its output of `heard` frames, then one back to the output's second frame. The
// output starts at the model's frame `start`. Each seek counts in `totals`.
SeekResult SeekCheck(song::SequenceModel& model, std::size_t start, std::size_t heard, std::span<const float> reference,
                     std::size_t frame_floats, StepTotals& totals);

// The output of a player that switches to a new model of `sequence` after `before` frames, for `frames` frames in all.
std::vector<float> RunSwitch(const song::Played& played, const std::shared_ptr<const song::Sequence>& sequence,
                             song::Mixing mixing, std::size_t before, std::size_t frames);

// What differs when the player switches to `sequence` halfway through `heard` frames of output, for the report: the
// frames that differ from `reference` and where the output ends. The comparison starts `offset` floats into the
// reference. The switch counts in `totals`. Nothing comes back when the output after the switch matches.
std::string SwitchCheck(const song::Played& played, const std::shared_ptr<const song::Sequence>& sequence,
                        song::Mixing mixing, std::span<const float> reference, std::size_t heard, std::size_t frames,
                        std::size_t frame_floats, std::size_t offset, StepTotals& totals);

// Measures `after`, the reference after an end of the output, in frames of `frame_floats` floats. A side that spans
// more than `span` sounds on.
EndLevels LevelsAfterEnd(std::span<const float> after, std::size_t frame_floats, float span);

// Whether the arrangement contains a note on `track` and `key` that starts at the arrangement's tick of `tick`. `list`
// is the track's list of notes to look in.
bool ArrangedAt(const song::Sequence& sequence, int track, int key, uint32_t tick,
                std::vector<song::Note> song::Track::* list = &song::Track::notes);

// The text that the line of a sound adds for `failed` seeks that give no output, or nothing.
std::string FailedSeeksText(std::size_t failed);

// Prints the line of a sound that differs: `differences` of `heard` frames from the start, `seeks` after seeks, then
// `after_text` and `switch_text`, and the loop's text `loop`.
void PrintDifferences(const std::string& name, std::size_t differences, std::size_t heard, std::size_t seeks,
                      const std::string& after_text, const std::string& switch_text, const std::string& loop);

} // namespace hokuto::tools::playback_steps
