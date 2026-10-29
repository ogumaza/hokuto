// SPDX-License-Identifier: MIT

#include "measure/measure.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "song/dry_render.h"
#include "synth/render.h"
#include "synth/sf2_synth.h"
#include "synth/sfz_synth.h"

namespace hokuto::measure
{
namespace
{

constexpr double kTailSeconds = 2.0;
constexpr double kClipMargin = 1.0;
constexpr double kMostClipSeconds = 8.0;
constexpr std::size_t kMostPlaces = 3;

// The samples of `samples`, interleaved stereo, from frame `from` for `count` frames. Frames outside the samples are
// silent.
std::vector<float> Clip(const std::vector<float>& samples, int64_t from, std::size_t count)
{
    std::vector<float> clip(count * 2, 0.0f);
    const auto frames = static_cast<int64_t>(samples.size() / 2);
    for (std::size_t i = 0; i < count; i++)
    {
        const int64_t frame = from + static_cast<int64_t>(i);
        if (frame >= 0 && frame < frames)
        {
            clip[i * 2] = samples[static_cast<std::size_t>(frame) * 2];
            clip[i * 2 + 1] = samples[static_cast<std::size_t>(frame) * 2 + 1];
        }
    }

    return clip;
}

// The seconds that the renders of `conversion`'s export run: to the MIDI file's last event, and 2 s further for a
// sequence without a loop.
double ExportSeconds(const ntdlib::sequence_export::Conversion& conversion, const song::Sequence& sequence)
{
    return synth::MidiSeconds(conversion.midi) + (sequence.loop ? 0.0 : kTailSeconds);
}

// The synth's render of `conversion`'s export with `instruments` at `rate`, for `seconds`, or nothing once `cancel` is
// set.
synth::StereoRender RenderMidi(const ntdlib::sequence_export::Conversion& conversion, Instruments instruments, int rate,
                               double seconds, const std::atomic<bool>& cancel)
{
    const auto going = [&cancel](double)
    {
        return !cancel.load(std::memory_order_relaxed);
    };
    return instruments == Instruments::kSoundFont
               ? synth::RenderSoundFont(conversion.midi, conversion.sf2, rate, seconds, going)
               : synth::RenderSfz(conversion.midi, conversion.sfz, rate, seconds, going);
}

// The seconds that the renders of an export of `sequence` to another console run: to the end of the sequence, or to the
// end of its loop's first pass, and 2 s further for a sequence without a loop.
double EndOf(const song::Sequence& sequence)
{
    return song::MsOfTick(sequence, sequence.length) / 1000.0 + (sequence.loop ? 0.0 : kTailSeconds);
}

// The renders of an export to another console: `source`, the sequence's dry render, and `target`, the export's, both to
// `end` or kMostSeconds. The renders compare at the higher of the two rates. What the lower rate can't hold then
// counts.
Renders Joined(song::DryRender source, song::DryRender target, double end)
{
    const int rate = std::max(source.rate, target.rate);
    source = song::AtRate(source, rate);
    target = song::AtRate(target, rate);

    return {std::move(target.samples), std::move(source.samples), end > kMostSeconds, rate};
}

// Joined with the dry render of `target_sequence` of `target_archive` in `target_mixing`, or nothing once `cancel` is
// set.
Renders AgainstTarget(song::DryRender source, double end,
                      std::shared_ptr<const ntdlib::sar::SoundArchive> target_archive,
                      std::shared_ptr<const song::Sequence> target_sequence, song::Mixing target_mixing,
                      const std::atomic<bool>& cancel)
{
    if (cancel.load(std::memory_order_relaxed))
    {
        return {};
    }

    song::DryRender target = song::RenderDry(std::move(target_archive), std::move(target_sequence), target_mixing,
                                             std::min(end, kMostSeconds), cancel);
    if (cancel.load(std::memory_order_relaxed))
    {
        return {};
    }

    return Joined(std::move(source), std::move(target), end);
}

// The same with `target_sequence` of the Wii's `target_archive`.
Renders AgainstTarget(song::DryRender source, double end,
                      std::shared_ptr<const ntdlib::rsar::SoundArchive> target_archive,
                      std::shared_ptr<const song::Sequence> target_sequence, const std::atomic<bool>& cancel)
{
    if (cancel.load(std::memory_order_relaxed))
    {
        return {};
    }

    song::DryRender target =
        song::RenderDry(std::move(target_archive), std::move(target_sequence), std::min(end, kMostSeconds), cancel);
    if (cancel.load(std::memory_order_relaxed))
    {
        return {};
    }

    return Joined(std::move(source), std::move(target), end);
}

} // namespace

Renders RenderExport(const ntdlib::sequence_export::Conversion& conversion, Instruments instruments,
                     std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                     std::shared_ptr<const song::Sequence> sequence, song::Mixing mixing,
                     const std::atomic<bool>& cancel)
{
    // The model renders first. The synth then renders at the model's rate.
    const double end = ExportSeconds(conversion, *sequence);
    const double seconds = std::min(end, kMostSeconds);
    song::DryRender model = song::RenderDry(std::move(archive), std::move(sequence), mixing, seconds, cancel);
    if (cancel.load(std::memory_order_relaxed))
    {
        return {};
    }

    synth::StereoRender exported = RenderMidi(conversion, instruments, model.rate, seconds, cancel);
    if (cancel.load(std::memory_order_relaxed))
    {
        return {};
    }
    if (exported.rate != model.rate)
    {
        throw std::runtime_error("The synth didn't render at the model's rate.");
    }

    return {std::move(exported.samples), std::move(model.samples), end > kMostSeconds, model.rate};
}

Renders RenderExport(const ntdlib::sequence_export::Conversion& conversion, Instruments instruments,
                     std::shared_ptr<const ntdlib::rsar::SoundArchive> archive,
                     std::shared_ptr<const song::Sequence> sequence, const std::atomic<bool>& cancel)
{
    // The model renders first. The synth then renders at the model's rate.
    const double end = ExportSeconds(conversion, *sequence);
    const double seconds = std::min(end, kMostSeconds);
    song::DryRender model = song::RenderDry(std::move(archive), std::move(sequence), seconds, cancel);
    if (cancel.load(std::memory_order_relaxed))
    {
        return {};
    }

    synth::StereoRender exported = RenderMidi(conversion, instruments, model.rate, seconds, cancel);
    if (cancel.load(std::memory_order_relaxed))
    {
        return {};
    }
    if (exported.rate != model.rate)
    {
        throw std::runtime_error("The synth didn't render at the Wii's rate.");
    }

    return {std::move(exported.samples), std::move(model.samples), end > kMostSeconds, model.rate};
}

Renders RenderOtherConsole(std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                           std::shared_ptr<const song::Sequence> sequence,
                           std::shared_ptr<const ntdlib::sar::SoundArchive> target_archive,
                           std::shared_ptr<const song::Sequence> target_sequence, song::Mixing mixing,
                           song::Mixing target_mixing, const std::atomic<bool>& cancel)
{
    const double end = EndOf(*sequence);
    song::DryRender source =
        song::RenderDry(std::move(archive), std::move(sequence), mixing, std::min(end, kMostSeconds), cancel);
    return AgainstTarget(std::move(source), end, std::move(target_archive), std::move(target_sequence), target_mixing,
                         cancel);
}

Renders RenderOtherConsole(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive,
                           std::shared_ptr<const song::Sequence> sequence,
                           std::shared_ptr<const ntdlib::sar::SoundArchive> target_archive,
                           std::shared_ptr<const song::Sequence> target_sequence, song::Mixing target_mixing,
                           const std::atomic<bool>& cancel)
{
    const double end = EndOf(*sequence);
    song::DryRender source =
        song::RenderDry(std::move(archive), std::move(sequence), std::min(end, kMostSeconds), cancel);
    return AgainstTarget(std::move(source), end, std::move(target_archive), std::move(target_sequence), target_mixing,
                         cancel);
}

Renders RenderOtherConsole(std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                           std::shared_ptr<const song::Sequence> sequence,
                           std::shared_ptr<const ntdlib::rsar::SoundArchive> target_archive,
                           std::shared_ptr<const song::Sequence> target_sequence, song::Mixing mixing,
                           const std::atomic<bool>& cancel)
{
    const double end = EndOf(*sequence);
    song::DryRender source =
        song::RenderDry(std::move(archive), std::move(sequence), mixing, std::min(end, kMostSeconds), cancel);
    return AgainstTarget(std::move(source), end, std::move(target_archive), std::move(target_sequence), cancel);
}

Renders RenderOtherConsole(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive,
                           std::shared_ptr<const song::Sequence> sequence,
                           std::shared_ptr<const ntdlib::rsar::SoundArchive> target_archive,
                           std::shared_ptr<const song::Sequence> target_sequence, const std::atomic<bool>& cancel)
{
    const double end = EndOf(*sequence);
    song::DryRender source =
        song::RenderDry(std::move(archive), std::move(sequence), std::min(end, kMostSeconds), cancel);
    return AgainstTarget(std::move(source), end, std::move(target_archive), std::move(target_sequence), cancel);
}

Measure MeasureRenders(const Renders& renders)
{
    // Hokuto's render is the reference. The renders run to the end of the shorter.
    Measure measure;
    const int rate = renders.rate;
    measure.comparison = song::CompareLevels(renders.hokuto, renders.exported, rate);
    measure.seconds = static_cast<double>(std::min(renders.hokuto.size(), renders.exported.size()) / 2) / rate;
    measure.cut = renders.cut;
    measure.rate = rate;
    const auto lag = static_cast<int64_t>(std::lround(measure.comparison.lag * rate));
    for (const song::LevelPlace& place : song::WorstPlaces(measure.comparison, song::kSpanFindingDb, kMostPlaces))
    {
        const double start = std::max(place.start - kClipMargin, 0.0);
        const double length = std::min(place.start + place.seconds + kClipMargin - start, kMostClipSeconds);
        const auto from = static_cast<int64_t>(std::lround(start * rate));
        const auto count = static_cast<std::size_t>(std::lround(length * rate));
        measure.places.push_back({place, Clip(renders.exported, from + lag, count), Clip(renders.hokuto, from, count)});
    }

    return measure;
}

Measure MeasureExport(const ntdlib::sequence_export::Conversion& conversion, Instruments instruments,
                      std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                      std::shared_ptr<const song::Sequence> sequence, song::Mixing mixing,
                      const std::atomic<bool>& cancel)
{
    const Renders renders =
        RenderExport(conversion, instruments, std::move(archive), std::move(sequence), mixing, cancel);
    if (cancel.load(std::memory_order_relaxed))
    {
        return {};
    }

    return MeasureRenders(renders);
}

Measure MeasureExport(const ntdlib::sequence_export::Conversion& conversion, Instruments instruments,
                      std::shared_ptr<const ntdlib::rsar::SoundArchive> archive,
                      std::shared_ptr<const song::Sequence> sequence, const std::atomic<bool>& cancel)
{
    const Renders renders = RenderExport(conversion, instruments, std::move(archive), std::move(sequence), cancel);
    if (cancel.load(std::memory_order_relaxed))
    {
        return {};
    }

    return MeasureRenders(renders);
}

} // namespace hokuto::measure
