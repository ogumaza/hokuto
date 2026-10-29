// SPDX-License-Identifier: MIT

#include "song/biquads.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"

#include "song/items.h"
#include "song/track_flow.h"

namespace hokuto::song
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;
using ntdlib::sar::Flavor;

// The presets made for a 32 kHz renderer are types 1 to 5: a low-pass, a high-pass and the band-passes. The Switch's
// types 6 to 10 are the same filters made for 48 kHz.
constexpr int kPresets = 5;
constexpr int kFirstBandPass = 3;

// The type that a track starts with, and the type of a setting from a variable or a random number. The first filters
// nothing.
constexpr int kNoFilter = 0;
constexpr int kUnknownType = -1;

// The value that a track starts with: 0. That value turns the filter off. A setting of a value stands for its item.
constexpr int32_t kStartingValue = -1;

// The grid on which a passband's edges lie: 20 Hz times 2^(k / 96). The last point lies at about 14,910 Hz.
constexpr double kLowest = 20.0;
constexpr int kStepsPerOctave = 96;
constexpr int kGridPoints = 917;

// The rates at which the consoles' renderers play the presets: the Switch's, the Wii's and the Wii U's, and the 3DS's.
constexpr double kSwitchRate = 48000.0;
constexpr double kWiiURate = 32000.0;
constexpr double kCtrRate = 32728.0;

// The edges of a passband: the first and the last point of the grid whose power lies within 3 dB of the peak's.
struct Edges
{
    int low = 0;
    int high = 0;
};

// The edges of each row of the presets of types 1 to 5 at a rate.
using EdgeTable = std::array<std::vector<Edges>, kPresets>;

// The power of a biquad with coefficients `c` in Q14 (b0, b1, b2, a1 and a2, for y = b0 x + b1 x1 + b2 x2 + a1 y1 + a2
// y2), at a frequency whose angle per sample has the cosine `c1`. `c2` is the cosine of twice the angle.
double Power(const int16_t* c, double c1, double c2)
{
    const double b0 = c[0] / 16384.0;
    const double b1 = c[1] / 16384.0;
    const double b2 = c[2] / 16384.0;
    const double a1 = c[3] / 16384.0;
    const double a2 = c[4] / 16384.0;
    const double numerator = b0 * b0 + b1 * b1 + b2 * b2 + 2.0 * (b0 * b1 + b1 * b2) * c1 + 2.0 * b0 * b2 * c2;
    const double denominator = 1.0 + a1 * a1 + a2 * a2 + 2.0 * (a1 * a2 - a1) * c1 - 2.0 * a2 * c2;
    return numerator / denominator;
}

// The edges of the presets' rows at `rate`. At each of the three rates, the power at every point of the grid differs
// from the power 3 dB below its row's peak by more than 3 parts in 10 million. The edges therefore come out the same on
// every platform.
EdgeTable EdgesAt(double rate)
{
    std::vector<double> c1(kGridPoints);
    std::vector<double> c2(kGridPoints);
    for (int k = 0; k < kGridPoints; ++k)
    {
        const double frequency = kLowest * std::exp2(static_cast<double>(k) / kStepsPerOctave);
        const double angle = 2.0 * std::numbers::pi * frequency / rate;
        c1[static_cast<std::size_t>(k)] = std::cos(angle);
        c2[static_cast<std::size_t>(k)] = std::cos(2.0 * angle);
    }

    EdgeTable table;
    const double kHalfPower = std::pow(10.0, -0.3);
    std::vector<double> powers(kGridPoints);
    for (int type = 1; type <= kPresets; ++type)
    {
        for (const auto& row : ntdlib::nwsnd::kBiquadPresets[type - 1].rows)
        {
            for (std::size_t k = 0; k < powers.size(); ++k)
            {
                powers[k] = Power(row, c1[k], c2[k]);
            }
            const double floor = *std::max_element(powers.begin(), powers.end()) * kHalfPower;
            const auto within = [floor](double power)
            {
                return power >= floor;
            };
            const auto low = std::find_if(powers.begin(), powers.end(), within);
            const auto high = std::find_if(powers.rbegin(), powers.rend(), within);
            table[static_cast<std::size_t>(type - 1)].push_back(
                {static_cast<int>(low - powers.begin()), static_cast<int>(powers.rend() - high) - 1});
        }
    }

    return table;
}

// The edges on the Switch, on the Wii and the Wii U, and on the 3DS.
struct EdgeTables
{
    EdgeTable nx;
    EdgeTable wii_u;
    EdgeTable ctr;
};

const EdgeTables& Tables()
{
    static const EdgeTables kTables{EdgesAt(kSwitchRate), EdgesAt(kWiiURate), EdgesAt(kCtrRate)};
    return kTables;
}

// Whether `console` is the Switch.
bool IsSwitch(const Console& console)
{
    return !console.wii && console.flavor == Flavor::kNx;
}

// The row of preset `type`'s table that `value`, from 0 to 127, selects on `console` (ntdlib's BiquadPresetRow).
int RowOn(const Console& console, int type, int value)
{
    using ntdlib::nwsnd::BiquadPresetRow;
    if (console.wii)
    {
        using Policy = ntdlib::nwsnd::RevolutionPolicy;
        return BiquadPresetRow<Policy>(type, Policy::BiquadValueOf(value));
    }

    switch (console.flavor)
    {
    case Flavor::kCtr:
        return BiquadPresetRow<ntdlib::nwsnd::CtrPolicy>(type, ntdlib::nwsnd::CtrPolicy::BiquadValueOf(value));
    case Flavor::kCafe:
        return BiquadPresetRow<ntdlib::nwsnd::CafePolicy>(type, ntdlib::nwsnd::CafePolicy::BiquadValueOf(value));
    case Flavor::kNx:
        break;
    }

    return BiquadPresetRow<ntdlib::nwsnd::NxPolicy>(type, ntdlib::nwsnd::NxPolicy::BiquadValueOf(value));
}

// The console's name in the warnings.
std::string NameOf(const Console& console)
{
    return console.wii ? "Wii" : ConsoleName(console.flavor);
}

// A filter's type, and the item of the setting of its value or kStartingValue. A track's state holds each pair that can
// be in effect there, in order.
using Pair = std::pair<int, int32_t>;
using State = std::vector<Pair>;

// The state after `command`, at `item`, from the state before it.
State After(const Command& command, uint32_t item, const State& before)
{
    State after;
    if (command.kind == Kind::kBiquadType)
    {
        const int type =
            command.value.source == Source::kValue ? static_cast<int>(command.value.value / kOne) : kUnknownType;
        for (const Pair& pair : before)
        {
            after.emplace_back(type, pair.second);
        }
    }
    else if (command.kind == Kind::kBiquadValue)
    {
        for (const Pair& pair : before)
        {
            after.emplace_back(pair.first, static_cast<int32_t>(item));
        }
    }
    else
    {
        return before;
    }

    std::sort(after.begin(), after.end());
    after.erase(std::unique(after.begin(), after.end()), after.end());

    return after;
}

} // namespace

std::optional<std::map<uint32_t, std::set<int>>> BiquadTypesOf(const Sequence& sequence,
                                                               std::span<const uint32_t> entries, std::size_t most)
{
    std::map<uint32_t, std::set<int>> types;
    if (sequence.items.empty())
    {
        return types;
    }

    // The tracks' starts: the entries, the labels, the first item and the targets of the commands that open tracks.
    std::vector<uint32_t> starts(entries.begin(), entries.end());
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        starts.push_back(label.item);
    }
    starts.push_back(0);
    for (const Command& command : sequence.items)
    {
        if (command.kind == Kind::kOpenTrack && !command.target_outside)
        {
            starts.push_back(command.target);
        }
    }
    const std::optional<std::vector<State>> states =
        TrackStates(sequence, starts, Pair{kNoFilter, kStartingValue}, After, most);
    if (!states)
    {
        return std::nullopt;
    }

    // The types that each setting of a value plays with.
    for (const State& state : *states)
    {
        for (const auto& [type, value] : state)
        {
            if (value != kStartingValue)
            {
                types[static_cast<uint32_t>(value)].insert(type);
            }
        }
    }

    return types;
}

uint8_t NearestBiquadValue(int type, uint8_t value, const Console& to)
{
    if (type < 1 || type > kPresets || value < 1 || value > 127 || IsSwitch(to))
    {
        return value;
    }

    // The Switch's row of the value, at 48 kHz, and the rows of each value on `to`.
    const std::size_t preset = static_cast<std::size_t>(type - 1);
    const Edges source = Tables().nx[preset][static_cast<std::size_t>(RowOn(Console{}, type, value))];
    const EdgeTable& target = !to.wii && to.flavor == Flavor::kCtr ? Tables().ctr : Tables().wii_u;
    uint8_t best = value;
    int64_t nearest = std::numeric_limits<int64_t>::max();
    for (int candidate = 1; candidate <= 127; ++candidate)
    {
        const Edges& edges = target[preset][static_cast<std::size_t>(RowOn(to, type, candidate))];
        const int64_t low = edges.low - source.low;
        const int64_t high = edges.high - source.high;
        const int64_t distance = low * low + high * high;
        if (distance < nearest || (distance == nearest && std::abs(candidate - value) < std::abs(best - value)))
        {
            best = static_cast<uint8_t>(candidate);
            nearest = distance;
        }
    }

    return best;
}

BiquadChanges MapBiquads(Sequence& sequence, std::span<const uint32_t> entries, const Console& from, const Console& to)
{
    const bool from_switch = IsSwitch(from);
    if (from_switch == IsSwitch(to))
    {
        return {};
    }

    // The types that each value plays with on the Switch, before the types change.
    const bool values =
        std::ranges::any_of(sequence.items, [](const Command& command) { return command.kind == Kind::kBiquadValue; });
    const std::optional<std::map<uint32_t, std::set<int>>> types =
        from_switch && values ? BiquadTypesOf(sequence, entries) : std::nullopt;

    // Types 1 to 5 of another console go to the Switch as types 6 to 10, and back.
    BiquadChanges changes;
    for (Command& command : sequence.items)
    {
        if (command.kind != Kind::kBiquadType)
        {
            continue;
        }
        if (command.value.source != Source::kValue)
        {
            changes.kept++;
            continue;
        }

        const int64_t type = command.value.value / kOne;
        if (!from_switch && type >= 1 && type <= kPresets)
        {
            command.value.value = (type + kPresets) * kOne;
            changes.types++;
        }
        else if (from_switch && type > kPresets && type <= 2 * kPresets)
        {
            command.value.value = (type - kPresets) * kOne;
            changes.types++;
        }
        else if (from_switch && type >= kFirstBandPass && type <= kPresets)
        {
            changes.band_passes++;
        }
    }
    if (!from_switch || !values)
    {
        return changes;
    }

    // A value of the Switch that plays with one type of 1 to 5 takes the value whose passband on `to` lies nearest.
    for (uint32_t item = 0; item < sequence.items.size(); ++item)
    {
        Command& command = sequence.items[item];
        if (command.kind != Kind::kBiquadValue)
        {
            continue;
        }
        if (!types)
        {
            changes.kept++;
            continue;
        }

        const auto found = types->find(item);
        if (found == types->end())
        {
            continue;
        }
        std::set<int> presets;
        bool others = false;
        for (const int type : found->second)
        {
            if (type >= 1 && type <= kPresets)
            {
                presets.insert(type);
            }
            others = others || type == kUnknownType || type > kPresets;
        }
        if (presets.empty())
        {
            continue;
        }
        if (presets.size() > 1 || others || command.value.source == Source::kVariable)
        {
            changes.kept++;
            continue;
        }

        // A random value takes other ends.
        const int type = *presets.begin();
        const auto mapped = [type, &to](int64_t fixed)
        {
            const int64_t value = fixed / kOne;
            return value >= 1 && value <= 127 ? NearestBiquadValue(type, static_cast<uint8_t>(value), to) * kOne
                                              : fixed;
        };

        const ntdlib::commands::Argument before = command.value;
        command.value.value = mapped(command.value.value);
        if (command.value.source == Source::kRandom)
        {
            command.value.high = mapped(command.value.high);
            std::tie(command.value.value, command.value.high) = std::minmax(command.value.value, command.value.high);
        }
        changes.values += command.value == before ? 0 : 1;
    }

    return changes;
}

std::optional<std::string> BiquadWarning(const BiquadChanges& changes, const Console& from, const Console& to)
{
    if (changes.types == 0 && changes.values == 0 && changes.kept == 0 && changes.band_passes == 0)
    {
        return std::nullopt;
    }

    const auto count = [](std::size_t settings)
    {
        return std::to_string(settings) + (settings == 1 ? " setting" : " settings");
    };

    const auto verb = [](std::size_t settings, const char* one, const char* more)
    {
        return std::string(settings == 1 ? one : more);
    };

    const bool from_switch = IsSwitch(from);
    const Console& other = from_switch ? to : from;
    const std::string name = NameOf(other);
    const bool ctr = !other.wii && other.flavor == Flavor::kCtr;
    std::string text = "The Switch plays the biquad filters of types 1 to 5 at 48 kHz, and the " + name + " at " +
                       (ctr ? "32,728 Hz" : "32 kHz") +
                       ". Their cutoffs lie about 1.5 times higher on the Switch. The Switch's types 6 to 10 are the "
                       "same filters made for 48 kHz.";
    if (!from_switch && changes.types > 0)
    {
        text += " " + count(changes.types) + " of types 1 to 5 took types 6 to 10.";
    }
    if (from_switch && changes.types > 0)
    {
        text += " " + count(changes.types) + " of types 6 to 10 took types 1 to 5.";
    }
    if (changes.values > 0)
    {
        text += " " + count(changes.values) + " of the value of a filter of types 1 to 5 took the value whose " +
                "passband on the " + name + " lies nearest.";
    }
    if (changes.band_passes > 0)
    {
        std::array<char, 16> octaves{};
        std::snprintf(octaves.data(), octaves.size(), "%.2f", std::log2(kSwitchRate / (ctr ? kCtrRate : kWiiURate)));
        text += " " + count(changes.band_passes) + " of a band-pass " +
                verb(changes.band_passes, "keeps its type", "keep their type") +
                ", and no value moves the band: it lies " + octaves.data() + " octaves lower on the " + name + ".";
    }
    if (changes.kept > 0)
    {
        text += " " + count(changes.kept) + " " + verb(changes.kept, "keeps its filter", "keep their filters") +
                " at the other rate: a type from a variable or a random number, or a value that plays with more than "
                "one type.";
    }

    return text;
}

bool SameButBiquads(const Command& a, const Command& b)
{
    if (a.kind != b.kind || (a.kind != Kind::kBiquadType && a.kind != Kind::kBiquadValue) ||
        a.value.source == Source::kVariable)
    {
        return a == b;
    }

    Command same = b;
    same.value.value = a.value.value;
    same.value.high = a.value.high;

    return a == same;
}

bool SameButBiquads(const Sequence& a, const Sequence& b)
{
    const auto same = [](const Command& x, const Command& y)
    {
        return SameButBiquads(x, y);
    };
    return a.labels == b.labels && std::ranges::equal(a.items, b.items, same);
}

} // namespace hokuto::song
