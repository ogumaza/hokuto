// SPDX-License-Identifier: MIT

// The helpers that the checks of hokuto_check_playback share (check_playback.h).

#include "check_playback.h"
#include "check_support.h"

namespace hokuto::tools::playback
{

namespace
{

// The lowest ratio, in dB, of the model's output to the difference of the output after a switch from it. The lead
// before a switch lets the effects' tails fall by 60 dB.
constexpr double kSwitchRatio = 60.0;

} // namespace

Closeness ClosenessOf(double signal, double difference, std::size_t count)
{
    if (difference <= 0.0)
    {
        return {};
    }

    return {10.0 * std::log10(signal / difference), 10.0 * std::log10(difference / static_cast<double>(count))};
}

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

namespace
{

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

} // namespace

bool AddSwitch(std::initializer_list<Closeness> stretches, Totals& totals)
{
    return AddStretches(stretches, totals.switches, totals.exact, totals.lowest, totals.floored, totals.loudest);
}

bool AddSeek(std::initializer_list<Closeness> stretches, Totals& totals)
{
    return AddStretches(stretches, totals.seeks, totals.exact_seeks, totals.lowest_seek, totals.floored_seeks,
                        totals.loudest_seek);
}

double WorstRatio(std::initializer_list<Closeness> stretches)
{
    double ratio = std::numeric_limits<double>::infinity();
    for (const Closeness& stretch : stretches)
    {
        ratio = std::min(ratio, stretch.ratio);
    }

    return ratio;
}

std::string RatioText(double ratio)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f dB", ratio);

    return text;
}

void PrintSwitches(const Totals& totals)
{
    if (totals.switches == 0)
    {
        return;
    }

    std::printf("%zu switches: %zu matched the model exactly after the frame that crossfades.", totals.switches,
                totals.exact);
    if (totals.lowest < std::numeric_limits<double>::infinity())
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
    if (totals.lowest_seek < std::numeric_limits<double>::infinity())
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

std::vector<int16_t> RunModelFrames(song::SequenceModel& model, std::size_t samples)
{
    std::vector<int16_t> played;
    for (bool more = model.SeekFrame(0); more && played.size() < samples; more = model.RunFrame())
    {
        const std::span<const int16_t> output = model.Output();
        played.insert(played.end(), output.begin(), output.end());
    }

    return played;
}

std::vector<float> RunPlayerBlocks(const std::shared_ptr<song::SequenceModel>& model, std::size_t samples)
{
    const auto clock = std::make_shared<song::PlaybackClock>(model->FrameSamples());
    song::SequencePlayer player(model, 0, clock);
    std::vector<float> output;
    std::vector<float> block(kBlock * 2);
    while (output.size() < samples)
    {
        const std::size_t done = player.Render(block.data(), kBlock);
        output.insert(output.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(done * 2));
        if (done < kBlock)
        {
            break;
        }
    }

    return output;
}

std::size_t FirstTickFrame(std::span<const uint64_t> after)
{
    std::size_t first = 0;
    while (first < after.size() && after[first] == 0)
    {
        first++;
    }

    return first;
}

bool EndsEarly(std::size_t first, std::size_t heard, std::size_t frames, std::size_t end)
{
    return heard < frames && first + heard + 1 < end;
}

std::size_t LoopEndFrames(std::span<const uint64_t> after, uint64_t loop_end)
{
    const auto reached =
        std::find_if(after.begin(), after.end(), [loop_end](uint64_t ticks) { return ticks >= loop_end; });
    return reached == after.end() ? after.size() : static_cast<std::size_t>(reached - after.begin()) + 1;
}

} // namespace hokuto::tools::playback
