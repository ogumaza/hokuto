// SPDX-License-Identifier: MIT

// The measure of an export, for the Export window. For an export to MIDI with SoundFont 2 or SFZ files, a synth renders
// the export, and the console's model renders the sequence as Hokuto plays it. The audibility method's level measure
// compares the two (song/audibility.h). Both renders are dry: the MIDI file can't hold the effects on the aux buses,
// and the synths render without reverb or chorus. Hokuto's SoundFont 2 renderer plays SoundFonts (synth/sf2_synth.h),
// and sfizz plays SFZ files (synth/sfz_synth.h). For an export of a sound to the other console, the other console's
// model renders the new sound dry. The measure keeps clips of both renders around the places that it finds, for the
// window's Play buttons. The renders of an export to MIDI run at the rate of the console's model: 32 kHz on the Wii,
// 32,728 Hz on the 3DS in strict mixing and about 48,070 Hz in enhanced mixing, and 48 kHz on the Wii U and the Switch.
// The renders of an export to another console compare at the higher rate of the two models.

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"

#include "song/audibility.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::measure
{

// The instruments that go with an export's MIDI file.
enum class Instruments
{
    kSoundFont,
    kSfz,
};

// The measure stops after 5 minutes of the export.
inline constexpr double kMostSeconds = 300.0;

// A place that the measure found, with clips of both renders around it: from a second before it to a second after, 8 s
// at most. The export's clip comes from the same time as Hokuto's once the renders are lined up.
struct PlaceClips
{
    song::LevelPlace place;
    std::vector<float> export_clip;
    std::vector<float> hokuto_clip;
};

// What the measure gives: the comparison, the seconds that it compared, whether it stopped at kMostSeconds, the rate of
// the clips, and the clips of up to three places that lie more than 1 dB off, the furthest off first.
struct Measure
{
    song::LevelComparison comparison;
    double seconds = 0.0;
    bool cut = false;
    int rate = 0;
    std::vector<PlaceClips> places;
};

// The two renders that a measure compares, interleaved stereo at `rate`: the synth's render of the export, and the
// model's dry render of the sequence. `cut` tells whether they stopped at kMostSeconds.
struct Renders
{
    std::vector<float> exported;
    std::vector<float> hokuto;
    bool cut = false;
    int rate = 48000;
};

// Renders the export of `conversion` to MIDI with `instruments`, and `sequence` of `archive` dry on the console's model
// in `mixing`. The synth renders at the model's rate. The renders run to the MIDI file's last event. A sequence without
// a loop runs 2 s further, for its last notes' releases. A looped sequence would play on into its loop where the MIDI
// file ends. Once `cancel` is set, the renders stop and return nothing. Throws std::runtime_error with a message for
// the user when a synth can't read the files, and std::invalid_argument for an archive that Hokuto doesn't play.
Renders RenderExport(const ntdlib::sequence_export::Conversion& conversion, Instruments instruments,
                     std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                     std::shared_ptr<const song::Sequence> sequence, song::Mixing mixing,
                     const std::atomic<bool>& cancel);

// The same renders for `sequence` of the Wii's `archive`, at 32 kHz. The archive has to be read by
// ntdlib::rvl::snd::kRsarOptions.
Renders RenderExport(const ntdlib::sequence_export::Conversion& conversion, Instruments instruments,
                     std::shared_ptr<const ntdlib::rsar::SoundArchive> archive,
                     std::shared_ptr<const song::Sequence> sequence, const std::atomic<bool>& cancel);

// Renders `sequence` of `archive` dry on its console's model in `mixing`, and `target_sequence` of `target_archive` on
// its console's model in `target_mixing`. The target's render is the export's. Both run to the end of `sequence`, or to
// the end of its loop's first pass. A sequence without a loop runs 2 s further, for its last notes' releases. The
// render at the lower rate goes to the higher (song::AtRate). Once `cancel` is set, the renders stop and return
// nothing. Throws std::invalid_argument for an archive that Hokuto doesn't play.
Renders RenderOtherConsole(std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                           std::shared_ptr<const song::Sequence> sequence,
                           std::shared_ptr<const ntdlib::sar::SoundArchive> target_archive,
                           std::shared_ptr<const song::Sequence> target_sequence, song::Mixing mixing,
                           song::Mixing target_mixing, const std::atomic<bool>& cancel);

// The same renders for `sequence` of the Wii's `archive`, dry on the Wii's model at 32 kHz. The archive has to be read
// by ntdlib::rvl::snd::kRsarOptions.
Renders RenderOtherConsole(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive,
                           std::shared_ptr<const song::Sequence> sequence,
                           std::shared_ptr<const ntdlib::sar::SoundArchive> target_archive,
                           std::shared_ptr<const song::Sequence> target_sequence, song::Mixing target_mixing,
                           const std::atomic<bool>& cancel);

// The same renders for `target_sequence` of the Wii's `target_archive`, dry on the Wii's model at 32 kHz: from
// `sequence` of `archive` in `mixing`, or of the Wii's `archive`, such as a sound's conversion on the Wii's engine. The
// Wii's archives have to be read by ntdlib::rvl::snd::kRsarOptions.
Renders RenderOtherConsole(std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                           std::shared_ptr<const song::Sequence> sequence,
                           std::shared_ptr<const ntdlib::rsar::SoundArchive> target_archive,
                           std::shared_ptr<const song::Sequence> target_sequence, song::Mixing mixing,
                           const std::atomic<bool>& cancel);
Renders RenderOtherConsole(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive,
                           std::shared_ptr<const song::Sequence> sequence,
                           std::shared_ptr<const ntdlib::rsar::SoundArchive> target_archive,
                           std::shared_ptr<const song::Sequence> target_sequence, const std::atomic<bool>& cancel);

// Compares the renders of `renders`, with Hokuto's render as the reference, and keeps clips of the worst places.
Measure MeasureRenders(const Renders& renders);

// RenderExport, then MeasureRenders. A measure that `cancel` stops returns nothing.
Measure MeasureExport(const ntdlib::sequence_export::Conversion& conversion, Instruments instruments,
                      std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                      std::shared_ptr<const song::Sequence> sequence, song::Mixing mixing,
                      const std::atomic<bool>& cancel);

// The same for `sequence` of the Wii's `archive`.
Measure MeasureExport(const ntdlib::sequence_export::Conversion& conversion, Instruments instruments,
                      std::shared_ptr<const ntdlib::rsar::SoundArchive> archive,
                      std::shared_ptr<const song::Sequence> sequence, const std::atomic<bool>& cancel);

} // namespace hokuto::measure
