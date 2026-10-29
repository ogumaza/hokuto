// SPDX-License-Identifier: MIT

#include "edit/lengths.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "edit/effects.h"
#include "song/lengths.h"

namespace hokuto::edit
{
namespace
{

// `file` with the lengths that `measure(sound)` gives for `sounds`, its sequence sounds (see WithLengths).
std::optional<format::File> WithMeasuredLengths(
    const format::File& file, const std::vector<uint32_t>& sounds,
    const std::function<std::optional<song::Length>(uint32_t sound)>& measure, std::size_t threads,
    const LengthProgress& progress)
{
    // Each thread takes the next sound until none is left, or the progress stops the measure. A thread that runs out of
    // memory stops the others, and the measure throws once they have stopped.
    std::map<uint32_t, format::SoundLength> lengths;
    std::atomic<std::size_t> next = 0;
    std::atomic<bool> stopped = false;
    std::size_t measured = 0;
    std::exception_ptr failure;
    std::mutex mutex;
    const auto work = [&]
    {
        for (std::size_t i = next++; i < sounds.size() && !stopped; i = next++)
        {
            std::optional<song::Length> length;
            try
            {
                length = measure(sounds[i]);
            }
            catch (...)
            {
                const std::lock_guard lock(mutex);
                failure = std::current_exception();
                stopped = true;
                return;
            }

            const std::lock_guard lock(mutex);
            if (length)
            {
                format::SoundLength& kept = lengths[sounds[i]];
                kept.length = length->length;
                kept.fade = length->fade;
                kept.loops = static_cast<uint8_t>(length->loops);
                kept.silent = length->silent;
                if (length->loop)
                {
                    kept.loop = format::LoopTicks{length->loop->start, length->loop->end};
                }
            }
            measured++;
            if (progress && !progress(measured, sounds.size()))
            {
                stopped = true;
            }
        }
    };

    std::vector<std::thread> pool;
    const std::size_t count = std::min({std::max<std::size_t>(threads, 1), MostLengthThreads(), sounds.size()});
    for (std::size_t t = 1; t < count; t++)
    {
        pool.emplace_back(work);
    }
    work();
    for (std::thread& thread : pool)
    {
        thread.join();
    }
    if (failure)
    {
        std::rethrow_exception(failure);
    }
    if (stopped)
    {
        return std::nullopt;
    }

    format::File result = file;
    result.lengths = std::move(lengths);

    return result;
}

} // namespace

std::size_t MostLengthThreads()
{
    return std::max<std::size_t>(std::thread::hardware_concurrency(), 1);
}

std::optional<format::File> WithLengths(const format::File& file, const ntdlib::sar::SoundArchive& archive,
                                        std::size_t threads, const LengthProgress& progress)
{
    std::vector<uint32_t> sounds;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        if (archive.Sounds()[i].type == ntdlib::sar::SoundType::kSequence)
        {
            sounds.push_back(static_cast<uint32_t>(i));
        }
    }

    const song::AuxEffects effects = ModelEffects(file.aux_effects);
    const song::AuxVolumes volumes = file.aux_volumes;
    const auto measure = [&](uint32_t sound)
    {
        return song::MeasureLength(archive, sound, effects, volumes);
    };
    return WithMeasuredLengths(file, sounds, measure, threads, progress);
}

std::optional<format::File> WithLengths(const format::File& file, const ntdlib::rsar::SoundArchive& archive,
                                        const ntdlib::SndBuild& build, std::size_t threads,
                                        const LengthProgress& progress)
{
    std::vector<uint32_t> sounds;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        if (archive.Sounds()[i].type == ntdlib::rsar::SoundType::kSequence)
        {
            sounds.push_back(static_cast<uint32_t>(i));
        }
    }

    const song::AuxEffects effects = ModelEffects(file.aux_effects);
    const song::AuxVolumes volumes = file.aux_volumes;
    const auto measure = [&](uint32_t sound)
    {
        return song::MeasureLength(archive, build, sound, effects, volumes);
    };
    return WithMeasuredLengths(file, sounds, measure, threads, progress);
}

} // namespace hokuto::edit
