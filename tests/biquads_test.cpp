// SPDX-License-Identifier: MIT

// Tests the biquad filters of a sequence on another console (src/song/biquads.h). A biquad's transfer function gives
// the passband of each row of the presets (ntdlib::nwsnd::kBiquadPresets): H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 - a1
// z^-1 - a2 z^-2), with the coefficients in Q14, in complex numbers. ntdlib's BiquadPresetRow gives the row that a
// value selects on each console. A passband's edges lie 3 dB below its peak, found by bisection between 20 Hz and about
// 14.9 kHz. For every value of types 1 to 5 of the Switch, the value that the Wii U, the Wii and the 3DS take has to
// give edges within 0.03 octaves of the nearest that any value gives: NearestBiquadValue finds the edges on a grid of
// 96 points an octave, and each lies within a point of its place. A low-pass keeps its cutoff within 0.04 octaves, and
// a high-pass within 0.06 octaves up to the table's highest cutoff. At each of the three rates, the power at every
// point of that grid differs from the power 3 dB below its row's peak by more than 3 parts in 10 million.
//
// MapBiquads changes the types between 1 to 5 and 6 to 10 on the way to the Switch and from it. From the Switch, a
// value takes another where the flow of the track gives it one type of 1 to 5 and no other but 0: before its type, in a
// subroutine, behind an if prefix and in a loop. A type or a value from a variable stays as it is, and a random value
// takes other ends. A sequence file of the Switch goes to the Wii U and back that way (ConvertSequenceFile). The
// warning names what changed. SameButBiquads leaves out the types and values of the filters, but not their variables.

#include "song/biquads.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "song/items.h"

using hokuto::song::BiquadChanges;
using hokuto::song::BiquadTypesOf;
using hokuto::song::BiquadWarning;
using hokuto::song::Console;
using hokuto::song::ConvertSequenceFile;
using hokuto::song::MapBiquads;
using hokuto::song::NearestBiquadValue;
using hokuto::song::SameButBiquads;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;
using ntdlib::sar::Flavor;

namespace sar_files = ntdlib::testing::sar_files;

namespace
{

constexpr double kSwitchRate = 48000.0;
constexpr double kWiiURate = 32000.0;
constexpr double kCtrRate = 32728.0;

// The grid of NearestBiquadValue: 96 points an octave from 20 Hz, up to about 14,910 Hz.
constexpr int kStepsPerOctave = 96;
constexpr int kGridPoints = 917;
constexpr double kTop = static_cast<double>(kGridPoints - 1) / kStepsPerOctave;

// The consoles. No known build of NW4R SND has the Wii's date, 20080610. Such a build plays as the builds from March
// 2008.
const Console kSwitch{.flavor = Flavor::kNx};
const Console kWiiU{.flavor = Flavor::kCafe};
const Console kCtr{.flavor = Flavor::kCtr};
const Console kWii{.wii = ntdlib::SndBuild{.date = 20080610}};

// The power of row `row` of preset `type` at `frequency` Hz on a renderer at `rate`.
double PowerAt(int type, int row, double frequency, double rate)
{
    const int16_t* c = ntdlib::nwsnd::kBiquadPresets[type - 1].rows[static_cast<std::size_t>(row)];
    const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * frequency / rate);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> h =
        (static_cast<double>(c[0]) + static_cast<double>(c[1]) * z1 + static_cast<double>(c[2]) * z2) /
        (16384.0 - static_cast<double>(c[3]) * z1 - static_cast<double>(c[4]) * z2);
    return std::norm(h);
}

// The power at a place in octaves above 20 Hz.
double PowerAtOctave(int type, int row, double octave, double rate)
{
    return PowerAt(type, row, 20.0 * std::exp2(octave), rate);
}

// A passband's edges in octaves above 20 Hz, from 0 to kTop.
struct Band
{
    double low = 0.0;
    double high = 0.0;
};

// The edges of a row's passband: the places where its power crosses the power 3 dB below its peak, by bisection. A scan
// of 8 points to each of the grid's points finds the peak.
Band BandOf(int type, int row, double rate)
{
    constexpr int kScan = 8 * (kGridPoints - 1);
    double peak = 0.0;
    int top = 0;
    for (int k = 0; k <= kScan; ++k)
    {
        const double power = PowerAtOctave(type, row, kTop * k / kScan, rate);
        if (power > peak)
        {
            peak = power;
            top = k;
        }
    }
    const double floor = peak * std::pow(10.0, -0.3);
    const auto below = [&](double octave)
    {
        return PowerAtOctave(type, row, octave, rate) < floor;
    };
    const auto crossing = [&below](double outside, double inside)
    {
        for (int i = 0; i < 60; ++i)
        {
            const double middle = 0.5 * (outside + inside);
            (below(middle) ? outside : inside) = middle;
        }

        return inside;
    };

    Band band{0.0, kTop};
    if (below(0.0))
    {
        int k = 0;
        while (k < top && below(kTop * (k + 1) / kScan))
        {
            ++k;
        }
        band.low = crossing(kTop * k / kScan, kTop * (k + 1) / kScan);
    }
    if (below(kTop))
    {
        int k = kScan;
        while (k > top && below(kTop * (k - 1) / kScan))
        {
            --k;
        }
        band.high = crossing(kTop * k / kScan, kTop * (k - 1) / kScan);
    }

    return band;
}

// The row that `value` selects on `console` (ntdlib's BiquadPresetRow).
int RowOf(int type, int value, const Console& console)
{
    using ntdlib::nwsnd::BiquadPresetRow;
    if (console.wii)
    {
        using Policy = ntdlib::nwsnd::RevolutionPolicy;
        return BiquadPresetRow<Policy>(type, Policy::BiquadValueOf(value));
    }
    if (console.flavor == Flavor::kCtr)
    {
        using Policy = ntdlib::nwsnd::CtrPolicy;
        return BiquadPresetRow<Policy>(type, Policy::BiquadValueOf(value));
    }
    if (console.flavor == Flavor::kCafe)
    {
        using Policy = ntdlib::nwsnd::CafePolicy;
        return BiquadPresetRow<Policy>(type, Policy::BiquadValueOf(value));
    }
    using Policy = ntdlib::nwsnd::NxPolicy;
    return BiquadPresetRow<Policy>(type, Policy::BiquadValueOf(value));
}

// The rate of the console's renderer: the Wii's and the Wii U's AX, the 3DS's DSP, or the Switch's renderer.
double RateOf(const Console& console)
{
    if (console.wii || console.flavor == Flavor::kCafe)
    {
        return kWiiURate;
    }

    return console.flavor == Flavor::kCtr ? kCtrRate : kSwitchRate;
}

// The distance between two passbands' edges, in octaves.
double Distance(const Band& a, const Band& b)
{
    return std::hypot(a.low - b.low, a.high - b.high);
}

// Each value of the Switch takes a value whose passband lies within 0.03 octaves of the nearest: the grid puts each
// edge within a point below or above its place. Each distance therefore lies within the square root of 2 points of the
// true distance. A low-pass keeps its cutoff within 0.04 octaves. A high-pass keeps its cutoff within 0.06 octaves up
// to the other table's highest cutoff, and takes that cutoff above it.
void TestNearest()
{
    for (const Console& to : {kWiiU, kWii, kCtr})
    {
        const double rate = RateOf(to);
        for (int type = 1; type <= 5; ++type)
        {
            std::vector<Band> bands(128);
            double highest = 0.0;
            for (int value = 1; value <= 127; ++value)
            {
                bands[static_cast<std::size_t>(value)] = BandOf(type, RowOf(type, value, to), rate);
                highest = std::max(highest, bands[static_cast<std::size_t>(value)].low);
            }
            for (int value = 1; value <= 127; ++value)
            {
                const Band source = BandOf(type, RowOf(type, value, kSwitch), kSwitchRate);
                double nearest = kTop;
                for (int other = 1; other <= 127; ++other)
                {
                    nearest = std::min(nearest, Distance(bands[static_cast<std::size_t>(other)], source));
                }
                const uint8_t chosen = NearestBiquadValue(type, static_cast<uint8_t>(value), to);
                SEQLIB_REQUIRE(chosen >= 1 && chosen <= 127);
                const Band& band = bands[chosen];
                XSFLIB_CHECK(Distance(band, source) <= nearest + 2.0 * std::numbers::sqrt2 / kStepsPerOctave);
                if (type == 1)
                {
                    XSFLIB_CHECK(std::abs(band.high - source.high) <= 0.04);
                }
                if (type == 2)
                {
                    XSFLIB_CHECK(std::abs(band.low - std::min(source.low, highest)) <= 0.06);
                }
            }
        }
    }

    // The values that no row of a preset selects, and the Switch, keep their values.
    XSFLIB_CHECK(NearestBiquadValue(1, 0, kWiiU) == 0);
    XSFLIB_CHECK(NearestBiquadValue(6, 40, kWiiU) == 40);
    XSFLIB_CHECK(NearestBiquadValue(0, 40, kWiiU) == 40);
    XSFLIB_CHECK(NearestBiquadValue(1, 40, kSwitch) == 40);
}

// At each rate, the power at every point of the grid differs from the power 3 dB below its row's peak by more than 3
// parts in 10 million. A platform's rounding can't move an edge.
void TestGridMargin()
{
    double margin = 1.0;
    for (const double rate : {kSwitchRate, kWiiURate, kCtrRate})
    {
        for (int type = 1; type <= 5; ++type)
        {
            const std::size_t rows = ntdlib::nwsnd::kBiquadPresets[type - 1].rows.size();
            for (std::size_t row = 0; row < rows; ++row)
            {
                std::vector<double> powers(kGridPoints);
                for (int k = 0; k < kGridPoints; ++k)
                {
                    powers[static_cast<std::size_t>(k)] =
                        PowerAtOctave(type, static_cast<int>(row), static_cast<double>(k) / kStepsPerOctave, rate);
                }
                const double floor = *std::max_element(powers.begin(), powers.end()) * std::pow(10.0, -0.3);
                for (const double power : powers)
                {
                    margin = std::min(margin, std::abs(power - floor) / floor);
                }
            }
        }
    }

    XSFLIB_CHECK(margin > 3e-7);
}

// A command of `kind` with `value`.
Command Setting(Kind kind, int value)
{
    Command command;
    command.kind = kind;
    command.value = {.source = Source::kValue, .value = value * kOne, .high = 0};

    return command;
}

// A setting of the biquad filter's type to `type`.
Command Type(int type)
{
    return Setting(Kind::kBiquadType, type);
}

// A setting of the biquad filter's value to `value`.
Command Value(int value)
{
    return Setting(Kind::kBiquadValue, value);
}

// A command of `kind` that takes its value from `variable`.
Command FromVariable(Kind kind, int variable)
{
    Command command;
    command.kind = kind;
    command.value = {.source = Source::kVariable, .value = variable, .high = 0};

    return command;
}

// A command of `kind` with a random value from `low` to `high`.
Command Random(Kind kind, int low, int high)
{
    Command command;
    command.kind = kind;
    command.value = {.source = Source::kRandom, .value = low * kOne, .high = high * kOne};

    return command;
}

// A wait of `ticks`.
Command Wait(int ticks)
{
    return Setting(Kind::kWait, ticks);
}

// A command of `kind` that goes to item `target`, with `index`, such as a track's number.
Command WithTarget(Kind kind, uint32_t target, uint8_t index = 0)
{
    Command command;
    command.kind = kind;
    command.target = target;
    command.index = index;

    return command;
}

// A command of `kind` without a value.
Command Plain(Kind kind)
{
    Command command;
    command.kind = kind;

    return command;
}

// `command`, run only if the last comparison held.
Command Conditional(Command command)
{
    command.conditional = true;

    return command;
}

// A sequence of `items`, without labels.
Sequence Of(std::vector<Command> items)
{
    return {.items = std::move(items), .labels = {}};
}

// The value that the Wii U takes for value `value` of the Switch's type `type`.
int64_t OnTheWiiU(int type, int value)
{
    return NearestBiquadValue(type, static_cast<uint8_t>(value), kWiiU) * kOne;
}

// Types 1 to 5 go to the Switch as types 6 to 10, with their values, and back. A type of 0, a game's type, a type from
// a variable and a sequence between two other consoles stay as they are.
void TestTypes()
{
    Sequence to_switch = Of({Type(1), Value(40), Type(5), Type(0), Type(64), FromVariable(Kind::kBiquadType, 3),
                             Type(6), Plain(Kind::kEnd)});
    const BiquadChanges in = MapBiquads(to_switch, {}, kWiiU, kSwitch);
    XSFLIB_CHECK(in.types == 2 && in.values == 0 && in.band_passes == 0 && in.kept == 1);
    XSFLIB_CHECK(to_switch.items[0].value.value == 6 * kOne && to_switch.items[1].value.value == 40 * kOne);
    XSFLIB_CHECK(to_switch.items[2].value.value == 10 * kOne && to_switch.items[3].value.value == 0);
    XSFLIB_CHECK(to_switch.items[4].value.value == 64 * kOne && to_switch.items[5].value.value == 3);
    XSFLIB_CHECK(to_switch.items[6].value.value == 6 * kOne);

    Sequence from_switch = Of({Type(6), Type(10), Type(2), Type(4), Type(64), Type(0), Plain(Kind::kEnd)});
    const BiquadChanges out = MapBiquads(from_switch, {}, kSwitch, kCtr);
    XSFLIB_CHECK(out.types == 2 && out.values == 0 && out.band_passes == 1 && out.kept == 0);
    XSFLIB_CHECK(from_switch.items[0].value.value == 1 * kOne && from_switch.items[1].value.value == 5 * kOne);
    XSFLIB_CHECK(from_switch.items[2].value.value == 2 * kOne && from_switch.items[3].value.value == 4 * kOne);

    const Sequence both = Of({Type(1), Value(40), Type(7), Plain(Kind::kEnd)});
    Sequence between = both;
    const BiquadChanges none = MapBiquads(between, {}, kWiiU, kCtr);
    XSFLIB_CHECK(between == both && none.types == 0 && none.values == 0 && none.kept == 0);
    XSFLIB_CHECK(MapBiquads(between, {}, kSwitch, kSwitch).types == 0 && between == both);
    XSFLIB_CHECK(!BiquadWarning(none, kWiiU, kCtr));
}

// From the Switch, a value of types 1 to 5 takes the Wii U's nearest where the track's flow gives it one such type and
// no other but 0. A value that plays with two such types, with types 6 to 10 too, or after a type from a variable,
// keeps its value. So does a value from a variable. A random value takes other ends.
void TestValues()
{
    // A value before its type, and two values of one type.
    Sequence one = Of({Value(70), Type(2), Wait(10), Value(90), Plain(Kind::kEnd)});
    const BiquadChanges first = MapBiquads(one, {}, kSwitch, kWiiU);
    XSFLIB_CHECK(one.items[0].value.value == OnTheWiiU(2, 70) && one.items[3].value.value == OnTheWiiU(2, 90));
    XSFLIB_CHECK(first.values == (OnTheWiiU(2, 70) != 70 * kOne ? 1u : 0u) + (OnTheWiiU(2, 90) != 90 * kOne ? 1u : 0u));
    XSFLIB_CHECK(first.types == 0 && first.kept == 0 && first.values >= 1);

    // A value that plays on after the type changes, and a value that another takes the place of before the change.
    Sequence two = Of({Type(1), Value(70), Wait(10), Value(90), Type(2), Wait(10), Plain(Kind::kEnd)});
    const BiquadChanges second = MapBiquads(two, {}, kSwitch, kWiiU);
    XSFLIB_CHECK(two.items[1].value.value == OnTheWiiU(1, 70) && two.items[3].value.value == 90 * kOne);
    XSFLIB_CHECK(second.kept == 1);

    // Types 6 to 10 with the value, alone and with a type of 1 to 5.
    Sequence later = Of({Type(7), Value(70), Plain(Kind::kEnd)});
    const BiquadChanges third = MapBiquads(later, {}, kSwitch, kWiiU);
    XSFLIB_CHECK(later.items[0].value.value == 2 * kOne && later.items[1].value.value == 70 * kOne);
    XSFLIB_CHECK(third.types == 1 && third.values == 0 && third.kept == 0);
    Sequence mixed = Of({Type(2), Value(70), Wait(10), Type(7), Plain(Kind::kEnd)});
    const BiquadChanges fourth = MapBiquads(mixed, {}, kSwitch, kWiiU);
    XSFLIB_CHECK(mixed.items[1].value.value == 70 * kOne && fourth.types == 1 && fourth.kept == 1);

    // A type from a variable, a value from a variable and a random value.
    Sequence variables =
        Of({FromVariable(Kind::kBiquadType, 2), Value(70), Wait(10), Type(1), FromVariable(Kind::kBiquadValue, 4),
            Wait(10), Type(1), Random(Kind::kBiquadValue, 10, 100), Plain(Kind::kEnd)});
    const BiquadChanges fifth = MapBiquads(variables, {}, kSwitch, kWiiU);
    XSFLIB_CHECK(variables.items[1].value.value == 70 * kOne && variables.items[4].value.value == 4);
    XSFLIB_CHECK(fifth.kept == 3);
    const Command& random = variables.items[7];
    XSFLIB_CHECK(random.value.source == Source::kRandom);
    XSFLIB_CHECK(random.value.value == std::min(OnTheWiiU(1, 10), OnTheWiiU(1, 100)));
    XSFLIB_CHECK(random.value.high == std::max(OnTheWiiU(1, 10), OnTheWiiU(1, 100)));
}

// The flow of a track: a subroutine that two tracks call with other types, the same subroutine with one type, a type
// behind an if prefix, a loop whose end goes back to a value under another type, and an entry past the first item.
void TestFlow()
{
    // Track 0 opens track 1 at item 4. The subroutine at item 8 sets the value.
    const std::vector<Command> two_types = {WithTarget(Kind::kOpenTrack, 4, 1),
                                            Type(1),
                                            WithTarget(Kind::kCall, 8),
                                            Plain(Kind::kEnd),
                                            Type(2),
                                            WithTarget(Kind::kCall, 8),
                                            Plain(Kind::kEnd),
                                            Plain(Kind::kEnd),
                                            Value(70),
                                            Plain(Kind::kReturn)};
    Sequence shared = Of(two_types);
    XSFLIB_CHECK(MapBiquads(shared, {}, kSwitch, kWiiU).kept == 1 && shared.items[8].value.value == 70 * kOne);
    const std::optional<std::map<uint32_t, std::set<int>>> types = BiquadTypesOf(Of(two_types), {});
    XSFLIB_CHECK(types && types->size() == 1 && types->at(8) == std::set<int>({1, 2}));
    XSFLIB_CHECK(!BiquadTypesOf(Of(two_types), {}, 1));

    std::vector<Command> one_type = two_types;
    one_type[4] = Type(1);
    Sequence same = Of(one_type);
    XSFLIB_CHECK(MapBiquads(same, {}, kSwitch, kWiiU).kept == 0 && same.items[8].value.value == OnTheWiiU(1, 70));

    Sequence behind_if = Of({Type(1), Conditional(Type(2)), Value(70), Plain(Kind::kEnd)});
    XSFLIB_CHECK(MapBiquads(behind_if, {}, kSwitch, kWiiU).kept == 1 && behind_if.items[2].value.value == 70 * kOne);

    // The loop's second pass plays the value with type 2.
    Sequence looped = Of({Type(1), Setting(Kind::kLoopStart, 2), Value(70), Wait(10), Type(2), Wait(10),
                          Plain(Kind::kLoopEnd), Plain(Kind::kEnd)});
    XSFLIB_CHECK(MapBiquads(looped, {}, kSwitch, kWiiU).kept == 1 && looped.items[2].value.value == 70 * kOne);

    // An entry at item 2 reaches the value with type 0 alone. The first item reaches it with type 2.
    Sequence entered = Of({Type(2), Wait(10), Value(70), Plain(Kind::kEnd)});
    const std::vector<uint32_t> kEntries = {2};
    XSFLIB_CHECK(MapBiquads(entered, kEntries, kSwitch, kWiiU).kept == 0);
    XSFLIB_CHECK(entered.items[2].value.value == OnTheWiiU(2, 70));
}

// A sequence file of the Switch goes to the Wii U with the Wii U's nearest value of its high-pass, and back with type
// 7, the same filter made for 48 kHz.
void TestFiles()
{
    const std::vector<uint8_t> kBytecode = {0xB4, 0x02, 0xB5, 70, 0xFF};
    const std::vector<uint8_t> file = sar_files::Sequence(Flavor::kNx, kBytecode, {{"Song", 0}});
    const hokuto::song::ConvertedSequence wii_u = ConvertSequenceFile(file, Flavor::kNx, Flavor::kCafe, {});
    const ntdlib::sar::SequenceFile there = ntdlib::sar::SequenceFile::Read(wii_u.file);
    const auto value = static_cast<uint8_t>(OnTheWiiU(2, 70) / kOne);
    XSFLIB_CHECK(value != 70 && wii_u.biquads.values == 1 && wii_u.biquads.types == 0);
    XSFLIB_CHECK(there.data.size() >= 5 && there.data[1] == 0x02 && there.data[3] == value);

    const hokuto::song::ConvertedSequence back = ConvertSequenceFile(wii_u.file, Flavor::kCafe, Flavor::kNx, {});
    const ntdlib::sar::SequenceFile again = ntdlib::sar::SequenceFile::Read(back.file);
    XSFLIB_CHECK(back.biquads.types == 1 && back.biquads.values == 0);
    XSFLIB_CHECK(again.data.size() >= 5 && again.data[1] == 0x07 && again.data[3] == value);
}

// The warning names the other console's rate and what changed, with the verbs for one setting and for more.
void TestWarning()
{
    const std::optional<std::string> to_switch = BiquadWarning({.types = 2}, kWiiU, kSwitch);
    XSFLIB_CHECK(to_switch ==
                 "The Switch plays the biquad filters of types 1 to 5 at 48 kHz, and the Wii U at 32 kHz. Their "
                 "cutoffs lie about 1.5 times higher on the Switch. The Switch's types 6 to 10 are the same filters "
                 "made for 48 kHz. 2 settings of types 1 to 5 took types 6 to 10.");

    const std::optional<std::string> from_switch =
        BiquadWarning({.types = 1, .values = 3, .band_passes = 1, .kept = 2}, kSwitch, kCtr);
    XSFLIB_CHECK(from_switch ==
                 "The Switch plays the biquad filters of types 1 to 5 at 48 kHz, and the 3DS at 32,728 Hz. Their "
                 "cutoffs lie about 1.5 times higher on the Switch. The Switch's types 6 to 10 are the same filters "
                 "made for 48 kHz. 1 setting of types 6 to 10 took types 1 to 5. 3 settings of the value of a filter "
                 "of types 1 to 5 took the value whose passband on the 3DS lies nearest. 1 setting of a band-pass "
                 "keeps its type, and no value moves the band: it lies 0.55 octaves lower on the 3DS. 2 settings keep "
                 "their filters at the other rate: a type from a variable or a random number, or a value that plays "
                 "with more than one type.");

    const std::optional<std::string> to_wii = BiquadWarning({.band_passes = 2, .kept = 1}, kSwitch, kWii);
    XSFLIB_CHECK(to_wii ==
                 "The Switch plays the biquad filters of types 1 to 5 at 48 kHz, and the Wii at 32 kHz. Their "
                 "cutoffs lie about 1.5 times higher on the Switch. The Switch's types 6 to 10 are the same filters "
                 "made for 48 kHz. 2 settings of a band-pass keep their type, and no value moves the band: it lies "
                 "0.58 octaves lower on the Wii. 1 setting keeps its filter at the other rate: a type from a variable "
                 "or a random number, or a value that plays with more than one type.");
    XSFLIB_CHECK(!BiquadWarning({}, kSwitch, kWiiU));
}

// The types and values of the filters can differ, a random value's ends too, but not a variable or another command.
void TestSameButBiquads()
{
    XSFLIB_CHECK(SameButBiquads(Type(2), Type(7)) && SameButBiquads(Value(70), Value(65)));
    XSFLIB_CHECK(SameButBiquads(Random(Kind::kBiquadValue, 10, 100), Random(Kind::kBiquadValue, 12, 90)));
    XSFLIB_CHECK(!SameButBiquads(FromVariable(Kind::kBiquadValue, 3), FromVariable(Kind::kBiquadValue, 4)));
    XSFLIB_CHECK(!SameButBiquads(FromVariable(Kind::kBiquadValue, 3), Value(70)));
    XSFLIB_CHECK(!SameButBiquads(Value(70), FromVariable(Kind::kBiquadValue, 3)));
    XSFLIB_CHECK(!SameButBiquads(Value(70), Type(70)) && !SameButBiquads(Wait(10), Wait(12)));
    XSFLIB_CHECK(!SameButBiquads(Value(70), Conditional(Value(70))));

    const Sequence a = Of({Type(2), Value(70), Plain(Kind::kEnd)});
    Sequence b = Of({Type(7), Value(64), Plain(Kind::kEnd)});
    XSFLIB_CHECK(SameButBiquads(a, b));
    b.labels = {{.name = "Song", .item = 0}};
    XSFLIB_CHECK(!SameButBiquads(a, b));
    XSFLIB_CHECK(!SameButBiquads(a, Of({Type(2), Value(70)})));
}

} // namespace

int main()
{
    TestNearest();
    TestGridMargin();
    TestTypes();
    TestValues();
    TestFlow();
    TestFiles();
    TestWarning();
    TestSameButBiquads();

    return xsflib::testing::Summary("biquads");
}
