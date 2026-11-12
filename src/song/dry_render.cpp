// SPDX-License-Identifier: MIT

#include "song/dry_render.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/song.h"

#include "audio/resampler.h"
#include "song/gba.h"

namespace hokuto::song
{
namespace
{

// The output of `model` from its first frame, for `seconds` or until the output ends, or until `cancel` is set.
DryRender Render(SequenceModel& model, double seconds, const std::atomic<bool>& cancel)
{
    DryRender render{model.SampleRate(), {}};
    const auto frames = static_cast<uint64_t>(
        std::ceil(std::max(seconds, 0.0) * model.SampleRate() / static_cast<double>(model.FrameSamples())));
    if (frames == 0)
    {
        return render;
    }

    render.samples.reserve(static_cast<std::size_t>(frames) * model.FrameSamples() * 2);
    for (bool plays = model.SeekFrame(0); plays && !cancel.load(std::memory_order_relaxed); plays = model.RunFrame())
    {
        const std::span<const float> output = model.FloatOutput();
        render.samples.insert(render.samples.end(), output.begin(), output.end());
        if (model.Frames() >= frames)
        {
            break;
        }
    }

    return render;
}

// A copy of `rom` whose song `sound` has no reverb. A song's header sets the reverb with bit 7 of its reverb byte, and
// otherwise uses the reverb of the driver. The copy sets 0 there.
std::shared_ptr<const GbaRom> WithoutReverb(const GbaRom& rom, uint32_t sound)
{
    auto dry = std::make_shared<GbaRom>(rom);
    dry->info.reverb = 0;
    ntdlib::mp2k::SongHeader header;
    if (ntdlib::mp2k::ReadSong(rom.rom, rom.info, static_cast<int>(sound), header))
    {
        const uint8_t* start = rom.rom.Ptr(ntdlib::agb::kRomBase);
        std::vector<uint8_t> image(start, start + rom.rom.Size());
        image[header.address + 3 - ntdlib::agb::kRomBase] = 0x80;
        dry->rom.Assign(std::move(image));
    }

    return dry;
}

} // namespace

// A render runs once from the start and never seeks. A snapshot would only take memory. On the Switch, the buses'
// volumes go to 0. The buses of the Wii U and the Wii reach the output only through effects, and the render sets no
// effects on them.
DryRender RenderDry(const Played& played, std::shared_ptr<const Sequence> sequence, Mixing mixing, double seconds,
                    const std::atomic<bool>& cancel)
{
    const Played dry = played.Gba() ? Played(WithoutReverb(*played.Gba(), sequence->sound)) : played;
    SequenceModel model(
        dry, std::move(sequence),
        ModelOptions{.mixing = mixing, .snapshot_interval = std::numeric_limits<uint32_t>::max(), .dry = true});
    if (played.Sar())
    {
        model.SetBusVolumes({0.0f, 0.0f, 0.0f});
    }

    return Render(model, seconds, cancel);
}

DrySource DryOf(const Played& played, std::shared_ptr<const Sequence> sequence, Mixing mixing)
{
    if (!played)
    {
        throw std::invalid_argument("Nothing is open to render.");
    }

    auto render = [played, sequence, mixing](double seconds, const std::atomic<bool>& cancel)
    {
        return RenderDry(played, sequence, mixing, seconds, cancel);
    };
    return {std::move(sequence), std::move(render)};
}

DryRender AtRate(const DryRender& render, int rate)
{
    if (render.rate == rate)
    {
        return render;
    }

    return {rate,
            audio::ResampleFrames(render.samples, 2, static_cast<uint32_t>(render.rate), static_cast<uint32_t>(rate))};
}

} // namespace hokuto::song
