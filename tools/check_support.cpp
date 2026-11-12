// SPDX-License-Identifier: MIT

// The helpers that several check tools use (check_support.h).

#include "check_support.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string_view>
#include <utility>

#include "ntdlib/sdat/heap.h"

#include "song/dry_render.h"

namespace hokuto::tools
{

bool HeapFull(const std::exception& error)
{
    if (dynamic_cast<const ntdlib::sdat::HeapFullError*>(&error) != nullptr)
    {
        return true;
    }

    // A read of a sequence nests the heap error in the error that it throws (song::ReadSequence).
    try
    {
        std::rethrow_if_nested(error);
    }
    catch (const ntdlib::sdat::HeapFullError&)
    {
        return true;
    }
    catch (...)
    {
    }

    return false;
}

bool LacksRoom(const std::invalid_argument& error)
{
    const std::string_view message = error.what();
    return message.starts_with("No room for ") || message.ends_with("There's no room for new bytes.");
}

std::vector<std::size_t> SpreadIndices(std::size_t count, std::size_t most)
{
    std::vector<std::size_t> chosen;
    const std::size_t taken = std::min(count, most);
    for (std::size_t i = 0; i < taken; i++)
    {
        chosen.push_back(i * count / taken);
    }

    return chosen;
}

std::string LoopText(const std::optional<song::Loop>& loop)
{
    return loop ? std::to_string(loop->start) + " to " + std::to_string(loop->end) : std::string("none");
}

const song::Track* TrackNumbered(const song::Sequence& sound, int number)
{
    for (const song::Track& track : sound.tracks)
    {
        if (track.number == number)
        {
            return &track;
        }
    }

    return nullptr;
}

const song::Track* BusiestTrack(const song::Sequence& sound)
{
    const song::Track* busiest = nullptr;
    for (const song::Track& track : sound.tracks)
    {
        if (!track.notes.empty() && (busiest == nullptr || track.notes.size() > busiest->notes.size()))
        {
            busiest = &track;
        }
    }

    return busiest;
}

uint32_t RareFrameTicks(const song::Sequence& sequence)
{
    double fastest = 0.0;
    for (const hokuto::song::Tempo& tempo : sequence.tempo)
    {
        fastest = std::max(fastest, tempo.bpm);
    }

    return static_cast<uint32_t>(std::max(1.0, std::ceil(sequence.timebase * fastest / 3600.0)));
}

std::vector<float> RenderedDs(const std::shared_ptr<const song::DsArchive>& archive, uint32_t sound, double seconds)
{
    const std::atomic<bool> cancel = false;
    auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound));

    return song::RenderDry(archive, std::move(sequence), song::Mixing::kStrict, seconds, cancel).samples;
}

} // namespace hokuto::tools
