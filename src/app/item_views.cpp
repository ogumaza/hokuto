// SPDX-License-Identifier: MIT

// What the Bank Editor and the properties of a wave or a stream show on every console: the numbers and names in
// their text, the peaks of a wave, the steps and places of a loop's markers, and a typed program.

#include "app/item_views.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/sar/encodings.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"

#include "app/item_views_parts.h"
#include "app/time_text.h"
#include "edit/waves.h"

namespace hokuto::app
{
namespace
{

namespace sar = ntdlib::sar;
using item_views_parts::ListOf;

// The most names that a list in a sentence gives before it counts the rest.
constexpr std::size_t kListedNames = 4;

} // namespace

namespace item_views_parts
{

std::string ListOf(const std::vector<std::string>& names)
{
    const std::size_t shown = std::min(names.size(), kListedNames);
    const std::size_t others = names.size() - shown;
    std::vector<std::string> listed(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(shown));
    if (others > 0)
    {
        listed.push_back(std::to_string(others) + (others == 1 ? " other" : " others"));
    }

    return JoinedNames(listed);
}

} // namespace item_views_parts

uint32_t LoopStepOf(const WaveView& view, sar::WaveEncoding encoding)
{
    namespace sdat = ntdlib::sdat;
    if (!view.ds)
    {
        return LoopStep(encoding, view.stream);
    }

    const sdat::WaveEncoding held = sdat::EncodingOf(encoding).value_or(sdat::WaveEncoding::kImaAdpcm);
    if (view.stream)
    {
        return encoding == view.values.encoding && view.ds_block_samples != 0 ? view.ds_block_samples
                                                                              : sdat::StreamBlockSamples(held);
    }

    return sdat::SamplesPerWord(held);
}

std::string Grouped(uint64_t number)
{
    std::string digits = std::to_string(number);
    for (std::size_t at = digits.size(); at > 3; at -= 3)
    {
        digits.insert(at - 3, ",");
    }

    return digits;
}

std::string LoopMoveLine(bool stream, bool ds, uint32_t from, uint32_t to, bool repeated)
{
    const std::string unit = ds       ? "DS wave loops must start on a 32-bit boundary."
                             : stream ? "Stream loops must start on a block boundary."
                                      : "DSP-ADPCM loops must start on a 14-sample frame boundary.";
    std::string line = unit + " The loop start moves from sample " + Grouped(from) + " to " + Grouped(to) + ".";
    if (repeated)
    {
        const uint32_t moved = to - from;
        line += (moved == 1 ? std::string(" The sample in between is repeated after the loop end.")
                            : " The " + Grouped(moved) + " samples in between are repeated after the loop end.") +
                " The loop sounds the same.";
    }

    return line;
}

std::optional<std::string> ProgramRefusal(std::string_view typed, uint32_t highest)
{
    const std::string range = "Programs range from 0 to " + Grouped(highest) + ".";
    const bool negative = typed.starts_with('-');
    const std::string_view digits = negative ? typed.substr(1) : typed;
    if (typed.empty())
    {
        return range;
    }
    if (digits.empty() || !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; }))
    {
        return "\"" + std::string(typed) + "\" isn't a number. " + range;
    }

    // A number past 64 bits is out of the range too.
    const std::optional<uint64_t> program = WholeNumber(digits);
    if (negative || !program || *program > highest)
    {
        return range;
    }

    return std::nullopt;
}

std::string PanName(int pan)
{
    constexpr int kCentre = 64;
    if (pan == kCentre)
    {
        return "centre";
    }

    return std::to_string(std::abs(pan - kCentre)) + (pan < kCentre ? " left" : " right");
}

std::string ChannelsOf(const std::vector<uint8_t>& channels)
{
    std::vector<std::string> numbers;
    for (const uint8_t channel : channels)
    {
        numbers.push_back(std::to_string(channel + 1));
    }

    return (channels.size() == 1 ? "channel " : "channels ") + ListOf(numbers);
}

std::string ProgramsText(const std::vector<uint32_t>& programs)
{
    if (programs.size() == 1)
    {
        return "program " + std::to_string(programs.front());
    }

    const auto next = [](uint32_t a, uint32_t b)
    {
        return b != a + 1;
    };
    if (programs.size() >= 3 && std::ranges::adjacent_find(programs, next) == programs.end())
    {
        return "programs " + std::to_string(programs.front()) + " to " + std::to_string(programs.back());
    }

    std::vector<std::string> numbers;
    for (const uint32_t program : programs)
    {
        numbers.push_back(std::to_string(program));
    }

    return numbers.empty() ? "programs" : "programs " + JoinedNames(numbers);
}

Peaks PeaksOf(const std::vector<std::vector<int16_t>>& samples)
{
    Peaks peaks;
    for (const std::vector<int16_t>& channel : samples)
    {
        std::vector<std::pair<int16_t, int16_t>>& runs = peaks.emplace_back();
        runs.reserve((channel.size() + kPeakRun - 1) / kPeakRun);
        for (std::size_t start = 0; start < channel.size(); start += kPeakRun)
        {
            const auto first = channel.begin() + static_cast<std::ptrdiff_t>(start);
            const auto last = channel.begin() + static_cast<std::ptrdiff_t>(std::min(start + kPeakRun, channel.size()));
            const auto [low, high] = std::minmax_element(first, last);
            runs.emplace_back(*low, *high);
        }
    }

    return peaks;
}

uint32_t LoopStep(sar::WaveEncoding encoding, bool stream)
{
    if (stream)
    {
        return sar::StreamBlockSamples(encoding);
    }

    return encoding == sar::WaveEncoding::kDspAdpcm ? edit::kDspAdpcmFrame : 1;
}

std::optional<uint32_t> LoopMarkerPlace(const std::vector<std::vector<int16_t>>& samples, double target, uint32_t step,
                                        uint32_t least, uint32_t most, bool snap, double reach)
{
    // The places run over the multiples of the step from `least` up to `most`.
    if (step == 0)
    {
        return std::nullopt;
    }

    const uint64_t first = (static_cast<uint64_t>(least) + step - 1) / step;
    const uint64_t last = most / step;
    if (first > last)
    {
        return std::nullopt;
    }

    // The place nearest the target.
    const auto clamped = [first, last](double place)
    {
        return static_cast<uint64_t>(std::clamp(place, static_cast<double>(first), static_cast<double>(last)));
    };
    const uint64_t nearest = clamped(std::round(target / step));
    std::size_t length = samples.empty() ? 0 : std::numeric_limits<std::size_t>::max();
    for (const std::vector<int16_t>& channel : samples)
    {
        length = std::min(length, channel.size());
    }
    if (!snap || length == 0)
    {
        return static_cast<uint32_t>(nearest * step);
    }

    // Within reach, the place where the channels lie nearest 0, and the nearest to the target of equals.
    const uint64_t low = clamped(std::ceil((target - reach) / step));
    const uint64_t high = clamped(std::floor((target + reach) / step));
    uint64_t best = nearest;
    int64_t best_level = std::numeric_limits<int64_t>::max();
    double best_distance = std::numeric_limits<double>::max();
    for (uint64_t place = low; place <= high; ++place)
    {
        const std::size_t sample = std::min<std::size_t>(place * step, length - 1);
        int64_t level = 0;
        for (const std::vector<int16_t>& channel : samples)
        {
            level += std::abs(static_cast<int64_t>(channel[sample]));
        }
        const double distance = std::abs(static_cast<double>(place * step) - target);
        if (level < best_level || (level == best_level && distance < best_distance))
        {
            best = place;
            best_level = level;
            best_distance = distance;
        }
    }

    return static_cast<uint32_t>(best * step);
}

WaveValues WithLoopSwitched(const WaveView& view, bool on, std::optional<uint32_t> before)
{
    WaveValues values = view.values;
    values.loop = on;
    values.loop_start = 0;
    if (on && before && values.loop_end > 0)
    {
        const uint32_t step = LoopStepOf(view, values.encoding);
        values.loop_start =
            LoopMarkerPlace(view.samples, *before, step, 0, values.loop_end - 1, false, 0.0).value_or(0);
    }

    return values;
}

} // namespace hokuto::app
