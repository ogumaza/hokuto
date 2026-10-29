// SPDX-License-Identifier: MIT

#include "format/command_values.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/units.h"

#include "format/archive.h"

namespace hokuto::format
{
namespace
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Source;

// The console's values that HeldRange tests: the ends of every width that a command's bytes give an argument, and the
// values just past them.
constexpr int64_t kTriedValues[] = {
    -(int64_t{1} << 32),
    -(int64_t{1} << 31),
    -32769,
    -32768,
    -129,
    -128,
    -1,
    0,
    1,
    127,
    128,
    255,
    256,
    32767,
    32768,
    65535,
    65536,
    (int64_t{1} << 28) - 1,
    int64_t{1} << 28,
    (int64_t{1} << 31) - 1,
    int64_t{1} << 31,
    (int64_t{1} << 32) - 1,
    int64_t{1} << 32,
};

// The values of `scale` among kTriedValues that `command` holds by the rules of `grammar` once `set` puts them in.
template <typename Set>
std::optional<ValueRange> TriedRange(const ntdlib::nwsnd::Grammar& grammar, const Command& command,
                                     const ntdlib::nwsnd::Scale& scale, const Set& set)
{
    std::optional<ValueRange> range;
    for (const int64_t value : kTriedValues)
    {
        const ntdlib::commands::Fixed unit = ntdlib::nwsnd::ToUnit(value, scale);
        Command tried = command;
        set(tried, unit);
        if (!Holds(grammar, tried))
        {
            continue;
        }

        range =
            range ? ValueRange{std::min(range->lowest, unit), std::max(range->highest, unit)} : ValueRange{unit, unit};
    }

    return range;
}

} // namespace

ntdlib::nwsnd::Grammar GrammarOf(ntdlib::sar::Flavor flavor)
{
    switch (flavor)
    {
    case ntdlib::sar::Flavor::kCafe:
        return ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::CafePolicy>();
    case ntdlib::sar::Flavor::kCtr:
        return ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::CtrPolicy>();
    case ntdlib::sar::Flavor::kNx:
        break;
    }

    return ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::NxPolicy>();
}

ntdlib::nwsnd::Grammar GrammarOf(const NativeArchive& native)
{
    return native.wii ? ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(BuildOfWiiArchive(native))
                      : GrammarOf(native.flavor);
}

bool Holds(const ntdlib::nwsnd::Grammar& grammar, Command command)
{
    command.target = 0;
    command.target_outside = false;
    try
    {
        ntdlib::nwsnd::EncodeSequence(ntdlib::commands::Sequence{{std::move(command)}, {}}, grammar);
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }

    return true;
}

std::optional<ValueRange> HeldVelocities(const ntdlib::nwsnd::Grammar& grammar, const Command& command)
{
    if (command.kind != Kind::kNote)
    {
        return std::nullopt;
    }

    const auto set = [](Command& tried, ntdlib::commands::Fixed unit)
    {
        tried.velocity = unit;
    };
    return TriedRange(grammar, command, ntdlib::nwsnd::kLevelScale, set);
}

std::optional<ValueRange> HeldRange(const ntdlib::nwsnd::Grammar& grammar, const Command& command, Source source,
                                    bool time)
{
    if (command.kind == Kind::kData || command.kind == Kind::kOther ||
        (!time && !ntdlib::commands::UnitOf(command.kind)))
    {
        return std::nullopt;
    }

    // The values that the encoder writes and the player reads back, from the console's values on either side of each
    // width's ends.
    ntdlib::nwsnd::Scale scale = time ? ntdlib::nwsnd::kTicksScale : ntdlib::nwsnd::ScaleOf(command.kind, grammar);
    if (source == Source::kVariable)
    {
        scale = ntdlib::nwsnd::Scale{1, 0};
    }
    const auto set = [source, time](Command& tried, ntdlib::commands::Fixed unit)
    {
        (time ? tried.time.emplace() : tried.value) = Argument{source, unit, source == Source::kRandom ? unit : 0};
    };
    return TriedRange(grammar, command, scale, set);
}

} // namespace hokuto::format
