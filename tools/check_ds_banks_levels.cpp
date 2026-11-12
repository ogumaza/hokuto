// SPDX-License-Identifier: MIT

// hokuto_check_ds_banks: the round that checks the levels that the instruments hold.

#include "check_ds_banks.h"

namespace hokuto::tools::ds_banks
{

namespace
{

// A held note's levels in dBFS from 3.4 s and from 3.9 s, each over 0.1 s, and its peak as a fraction of full scale.
// Nothing when the note ends before 4 s.
struct Held
{
    double early = -200.0;
    double late = -200.0;
    double peak = 0.0;
};

std::optional<Held> HeldLevels(const DsArchive& archive, const Place& place)
{
    constexpr int kVelocity = 127;
    auto release = std::make_shared<std::atomic<bool>>(false);
    hokuto::song::DsNotePreview preview(archive, place.place.bank, static_cast<int>(place.place.program), place.key,
                                        kVelocity, release);
    const auto rate = static_cast<std::size_t>(preview.SampleRate());
    const std::size_t frames = rate * 4;
    std::vector<float> out(frames * 2);
    if (preview.Render(out.data(), frames) < frames)
    {
        return std::nullopt;
    }

    Held held;
    const auto level = [&out, rate](std::size_t from)
    {
        double sum = 0.0;
        const std::size_t count = rate / 10;
        for (std::size_t i = from * 2; i < (from + count) * 2; ++i)
        {
            sum += static_cast<double>(out[i]) * out[i];
        }
        return 10.0 * std::log10(std::max(sum / static_cast<double>(count * 2), 1e-20));
    };
    held.early = level(rate * 34 / 10);
    held.late = level(rate * 39 / 10);
    for (const float sample : out)
    {
        held.peak = std::max(held.peak, static_cast<double>(std::abs(sample)));
    }

    return held;
}

// The seconds that a note of `region` takes to reach its sustain, by the rules of ntdlib's driver
// (snd::Driver::StepEnvelope). The attack multiplies the attenuation by its rate each frame until it reaches 0. A rate
// comes from the attack, or from the driver's table for the fastest attacks. The decay then lowers the attenuation by
// its rate each frame, down to the sustain's level. The attenuation counts 128ths of 0.1 dB.
double SustainSeconds(const sdat::Region& region)
{
    namespace tables = ntdlib::ntr::snd::tables;
    constexpr int64_t kStart = -92544;
    constexpr int kSlowAttacks = 109;
    constexpr int kMostFrames = 1 << 16;
    const int64_t attack =
        region.attack < kSlowAttacks ? 255 - region.attack : tables::kFastAttack[kHighest - region.attack];
    int64_t attenuation = kStart;
    int frames = 0;
    while (attenuation != 0 && frames < kMostFrames)
    {
        attenuation = -((-attenuation * attack) >> 8);
        frames++;
    }

    // The decay's rate (ntdlib's DecayRate).
    int64_t decay = 0x1E00 / (126 - std::min<int>(region.decay, 125));
    if (region.decay == 127)
    {
        decay = 0xFFFF;
    }
    else if (region.decay == 126)
    {
        decay = 0x3C00;
    }
    else if (region.decay < 50)
    {
        decay = region.decay * 2 + 1;
    }
    const int64_t sustain = int64_t{tables::kDecibelSquare[region.sustain]} * 128;
    frames += static_cast<int>((-sustain + decay - 1) / decay);

    return frames * static_cast<double>(ntdlib::ntr::snd::kFrameClocks) / ntdlib::ntr::kSystemClockHz;
}

// Whether `place` plays a wave that loops, a square wave or noise.
bool Holds(const DsArchive& archive, const Place& place)
{
    if (place.region.type != kPcm)
    {
        return true;
    }
    const sdat::BankInfo& info = *archive.archive.banks.at(place.place.bank);
    if (place.region.wave_archive >= info.wave_archives.size())
    {
        return false;
    }
    const uint16_t wave_archive = info.wave_archives[place.region.wave_archive];
    if (wave_archive >= archive.archive.wave_archives.size() || !archive.archive.wave_archives[wave_archive])
    {
        return false;
    }
    const uint32_t file_id = archive.archive.wave_archives[wave_archive]->file_id & sdat::kFileIdBits;
    const sdat::WaveArchive waves = sdat::WaveArchive::Parse(archive.archive.File(file_id));

    return place.region.wave < waves.waves.size() && waves.waves[place.region.wave].loop != 0;
}

// The regions of the game's banks whose sustains Levels halves, with their sustains halved, by their places.
std::map<std::size_t, sdat::Region> HalvedRegions(const Game& game)
{
    constexpr int kLeastSustain = 64;

    std::map<std::size_t, sdat::Region> halved;
    for (const auto& [at, place] : game.places)
    {
        const sdat::Region& region = place.region;
        if ((region.type == kPcm || region.type == kPsg || region.type == kNoise) && region.sustain >= kLeastSustain &&
            region.sustain <= kHighest)
        {
            sdat::Region edit = region;
            edit.sustain = static_cast<uint8_t>(region.sustain / 2);
            halved.emplace(at, edit);
        }
    }

    return halved;
}

// The places of the regions of `halved` whose notes hold and reach their sustains in time, before and after the edit.
std::vector<std::size_t> HoldingRegions(const Game& game, const std::map<std::size_t, sdat::Region>& halved,
                                        Totals& totals)
{
    constexpr double kReached = 3.15;
    std::vector<std::size_t> holding;
    for (const auto& [at, region] : halved)
    {
        // The tables of the driver contain rates for attack and decay values up to 127.
        const Place& place = game.places.at(at);
        if (place.region.attack > kHighest || place.region.decay > kHighest)
        {
            totals.past_range++;
            continue;
        }

        try
        {
            if (!Holds(*game.archive, place))
            {
                totals.ending++;
            }
            else if (SustainSeconds(place.region) > kReached || SustainSeconds(region) > kReached)
            {
                totals.slow++;
            }
            else
            {
                holding.push_back(at);
            }
        }
        catch (const std::exception&)
        {
            totals.unread_holds++;
        }
    }

    return holding;
}

// Checks that the note of `place` holds in `after` at the level that `region`, its region with the sustain halved,
// sets.
void CheckHoldingLevel(const Game& game, const DsArchive& after, const sdat::Region& region, const Place& place,
                       Totals& totals)
{
    // The level of the error of a sample cut to 10 bits: a step of 64 on the scale of 16-bit samples, with an error
    // spread evenly from 0 to a step.
    const double kCutError = 20.0 * std::log10(64.0 / 32768.0 / std::sqrt(3.0));
    try
    {
        // A note that ends or wavers before the edit counts apart. One that held before and doesn't after fails.
        const std::optional<Held> before = HeldLevels(*game.archive, place);
        const std::optional<Held> then = HeldLevels(after, place);
        const auto steady = [](const Held& held)
        {
            return std::abs(held.late - held.early) <= 0.1;
        };
        if (!before)
        {
            totals.ended++;
            return;
        }
        if (!steady(*before))
        {
            totals.unsteady++;
            return;
        }
        if (before->peak >= 0.999)
        {
            totals.clipped++;
            return;
        }
        const double wanted = (ntdlib::ntr::snd::tables::kDecibelSquare[region.sustain] -
                               ntdlib::ntr::snd::tables::kDecibelSquare[place.region.sustain]) /
                              10.0;
        if (before->late + wanted < kCutError + 20.0)
        {
            totals.quiet++;
            return;
        }
        // After the edit, the note holds when its level moves between the two windows as it did before, within 0.1 dB.
        // A note that drifts by nearly 0.1 dB before can drift by a little more at the lower level.
        const auto drift = [](const Held& held)
        {
            return held.late - held.early;
        };
        if (!then || std::abs(drift(*then) - drift(*before)) > 0.1)
        {
            std::printf("%s: %s: the edit stops the note from holding\n", game.name.c_str(), place.name.c_str());
            totals.failed++;
            return;
        }
        const double off = std::abs(then->late - before->late - wanted);
        totals.levels++;
        totals.worst = std::max(totals.worst, off);
        if (off > 0.5)
        {
            std::printf("%s: %s: the level fell by %.2f dB, and the sustains' levels by %.2f dB, from %.2f dBFS\n",
                        game.name.c_str(), place.name.c_str(), before->late - then->late, -wanted, before->late);
            totals.failed++;
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s: %s\n", game.name.c_str(), place.name.c_str(), error.what());
        totals.failed++;
    }
}

} // namespace

void Levels(const Game& game, Totals& totals)
{
    constexpr std::size_t kMostLevels = 256;

    std::map<std::size_t, sdat::Region> halved = HalvedRegions(game);
    const std::shared_ptr<const DsArchive> after = Round(game, "sustains", halved, {}, totals);
    if (after == nullptr || halved.empty())
    {
        return;
    }

    // The regions whose notes can hold: a square wave, noise, and a wave that loops. Their notes have to reach their
    // sustains before the levels count.
    const std::vector<std::size_t> holding = HoldingRegions(game, halved, totals);

    const std::size_t every = std::max<std::size_t>((holding.size() + kMostLevels - 1) / kMostLevels, 1);
    for (std::size_t index = 0; index < holding.size(); index += every)
    {
        const sdat::Region& region = halved.at(holding[index]);
        const Place& place = game.places.at(holding[index]);
        CheckHoldingLevel(game, *after, region, place, totals);
    }
}

} // namespace hokuto::tools::ds_banks
