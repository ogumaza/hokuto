// SPDX-License-Identifier: MIT

// Plays a sequence sound by itself on the console's model, from its start. The Replace and Import windows play a
// mapping this way, before the edit. The sound plays with the file's effects on the aux buses, at the buses' volumes,
// in its mixing mode. So do the zones.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ntdlib/sar/formats.h"

#include "audio/renderer.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::song
{

class SequencePreview final : public audio::Renderer
{
public:
    // The most that a preview lasts, in milliseconds.
    static constexpr uint64_t kLongestMs = 30000;

    // Plays `sequence`, a sequence sound of `archive` that ReadSequence read, with `effects` on the aux buses at
    // `volumes`, in `mixing`. The preview ends once the sound has ended and the last frame came out silent, or after
    // 30 s. The last frame of a preview that runs to the limit fades out. The model starts at the first Render, on the
    // render worker's thread. Throws std::invalid_argument for an archive of a console that Hokuto doesn't play.
    SequencePreview(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, std::shared_ptr<const Sequence> sequence,
                    AuxEffects effects = {}, const AuxVolumes& volumes = kFullVolumes, Mixing mixing = Mixing::kStrict);

    // The model's frames that a preview lasts at most.
    uint64_t LongestFrames() const;

    int SampleRate() const override;
    std::size_t Render(float* out, std::size_t frames) override;

private:
    // Runs the next frame of the model into frame_. Returns false at the end of the preview.
    bool NextFrame();

    SequenceModel model_;
    bool started_ = false;
    bool ended_ = false;
    uint64_t frames_ = 0;

    // The frame that Render copies from, two floats for each stereo frame, and the floats copied from it so far.
    std::vector<float> frame_;
    std::size_t frame_read_ = 0;
};

} // namespace hokuto::song
