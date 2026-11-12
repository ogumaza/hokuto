// SPDX-License-Identifier: MIT

// The measure of an export, for the Export window. For an export to MIDI with SoundFont 2 or SFZ files, a synth renders
// the export, and the console's model renders the sequence as Hokuto plays it. The audibility method's level measure
// compares the two (song/audibility.h). Both renders are dry: the MIDI file can't hold the effects on the aux buses,
// and the synths render without reverb or chorus. Hokuto's SoundFont 2 renderer plays SoundFonts (synth/sf2_synth.h),
// and sfizz plays SFZ files (synth/sfz_synth.h). For an export of a sound to the other console, the other console's
// model renders the new sound dry. The measure keeps clips of both renders around the places that it finds, for the
// window's Play buttons. The renders of an export to MIDI run at the rate of the console's model: 32 kHz on the Wii,
// 32,728 Hz on the 3DS in strict mixing and about 48,070 Hz in enhanced mixing, 48 kHz on the Wii U and the Switch, and
// 32,768 Hz or 65,536 Hz on the GBA in strict mixing and about 48,022 Hz in enhanced mixing. The renders of an export
// to another console compare at the higher rate of the two models.

#pragma once

#include <atomic>
#include <vector>

#include "ntdlib/sequence_export/convert.h"

#include "song/audibility.h"
#include "song/dry_render.h"

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

// Renders the export of `conversion` to MIDI with `instruments`, and the sequence of `source` dry on the model of its
// console (song::DrySource). The synth renders at the model's rate. The renders run to the MIDI file's last event. A
// sequence without a loop runs 2 s further, for its last notes' releases. A looped sequence would play on into its loop
// where the MIDI file ends. Once `cancel` is set, the renders stop and return nothing. Throws std::runtime_error with a
// message for the user when a synth can't read the files, and std::invalid_argument for an archive that Hokuto doesn't
// play. A song of the GBA is converted to MIDI with SoundFont 2 alone. Only a driver whose converter writes SFZ files
// accepts Instruments::kSfz (DriverRom::WritesSfz).
Renders RenderExport(const ntdlib::sequence_export::Conversion& conversion, Instruments instruments,
                     const song::DrySource& source, const std::atomic<bool>& cancel);

// Renders the sequence of `source` dry, and the sequence of `target`, the export to another console, dry on that
// console's model. Both run to the end of the sequence of `source`, or to the end of its loop's first pass. A sequence
// without a loop runs 2 s further, for its last notes' releases. The render at the lower rate goes to the higher
// (song::AtRate). Once `cancel` is set, the renders stop and return nothing. Throws std::invalid_argument for an
// archive that Hokuto doesn't play.
Renders RenderOtherConsole(const song::DrySource& source, const song::DrySource& target,
                           const std::atomic<bool>& cancel);

// RenderExport, then MeasureRenders. A measure that `cancel` stops returns nothing.
Measure MeasureExport(const ntdlib::sequence_export::Conversion& conversion, Instruments instruments,
                      const song::DrySource& source, const std::atomic<bool>& cancel);

// Compares the renders of `renders`, with Hokuto's render as the reference, and keeps clips of the worst places.
Measure MeasureRenders(const Renders& renders);

} // namespace hokuto::measure
