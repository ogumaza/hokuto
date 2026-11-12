// SPDX-License-Identifier: MIT

#include "song/driver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/brownie/driver.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/rd2/driver.h"
#include "seqlib/pcm.h"
#include "seqlib/wav.h"

#include "format/engines.h"
#include "song/brownie.h"
#include "song/konami.h"
#include "song/krawall.h"
#include "song/quintet.h"
#include "song/rare.h"
#include "song/rd2.h"
#include "song/ubimilan.h"

namespace hokuto::song
{

namespace
{

// The rules of revision `revision` of a driver whose rules function `kRules` takes a revision of type `Revision`.
template <typename Revision, const DriverRules& (*kRules)(Revision)>
const DriverRules& RulesAt(uint8_t revision)
{
    return kRules(static_cast<Revision>(revision));
}

// The rules of Ubisoft Milan's driver. The driver has no revisions.
const DriverRules& UbiMilanRulesAt(uint8_t /*revision*/)
{
    return UbiMilanRules();
}

// The ROM in `bytes`, loaded by the loader `kLoad` of one driver.
template <auto kLoad>
std::shared_ptr<const DriverRom> LoadAs(std::vector<uint8_t> bytes)
{
    return kLoad(std::move(bytes));
}

// One of the GBA's other drivers: its engine, the rules of a revision and the loader of a ROM.
struct DriverRow
{
    format::Engine engine;
    const DriverRules& (*rules)(uint8_t revision);
    std::shared_ptr<const DriverRom> (*load)(std::vector<uint8_t> bytes);
};

// A row for each of the GBA's other drivers, in the order of format::kEngineFacts.
constexpr std::array<DriverRow, kDriverEngineCount> kDriverRows = {{
    {format::Engine::kKonami, &RulesAt<ntdlib::konami::Revision, &KonamiRules>, &LoadAs<&LoadKonamiRom>},
    {format::Engine::kRd2, &RulesAt<ntdlib::rd2::Revision, &Rd2Rules>, &LoadAs<&LoadRd2Rom>},
    {format::Engine::kBrownie, &RulesAt<ntdlib::brownie::Revision, &BrownieRules>, &LoadAs<&LoadBrownieRom>},
    {format::Engine::kUbiMilan, &UbiMilanRulesAt, &LoadAs<&LoadUbiMilanRom>},
    {format::Engine::kRare, &RareRules, &LoadAs<&LoadRareRom>},
    {format::Engine::kQuintet, &QuintetRules, &LoadAs<&LoadQuintetRom>},
    {format::Engine::kKrawall, &KrawallRules, &LoadAs<&LoadKrawallRom>},
}};

static_assert(RowsFollowDriverEngines(kDriverRows),
              "kDriverRows needs one row for each driver engine of kEngineFacts, in that order");

// The row of `engine`. Throws std::invalid_argument for an engine without such a driver.
const DriverRow& RowOf(format::Engine engine)
{
    for (const DriverRow& row : kDriverRows)
    {
        if (row.engine == engine)
        {
            return row;
        }
    }

    throw std::invalid_argument("engine " + std::to_string(static_cast<int>(engine)) +
                                " plays none of the GBA's drivers");
}

} // namespace

const DriverRules& RulesOf(format::Engine engine, uint8_t revision)
{
    return RowOf(engine).rules(revision);
}

std::string DriverSampleName(int sample)
{
    std::array<char, 16> name{};
    std::snprintf(name.data(), name.size(), "Sample %04d", sample);
    return name.data();
}

ntdlib::commands::Command ValueCommand(ntdlib::commands::Kind kind, int64_t value, uint8_t index)
{
    ntdlib::commands::Command command;
    command.kind = kind;
    command.index = index;
    command.value = ntdlib::commands::Argument{ntdlib::commands::Source::kValue, value * ntdlib::commands::kOne, 0};
    return command;
}

std::vector<uint8_t> Pcm8Wav(const ntdlib::agb::Rom& rom, uint32_t address, uint32_t length, bool is_unsigned,
                             std::optional<WavLoop> loop, uint32_t rate)
{
    seqlib::Pcm out;
    out.channels.emplace_back(length);
    for (uint32_t i = 0; i < length; ++i)
    {
        const int point = is_unsigned ? rom.U8(address + i) - 128 : rom.S8(address + i);
        out.channels[0][i] = static_cast<int16_t>(point * 256);
    }

    if (loop)
    {
        out.loop = true;
        out.loop_start = loop->start;
        out.loop_end = loop->end;
    }

    return seqlib::WavFile(out, rate);
}

uint32_t WavRate(double rate)
{
    return static_cast<uint32_t>(std::max<long>(std::lround(rate), 1));
}

std::map<uint32_t, uint32_t> ItemsByOffset(std::span<const uint32_t> offsets)
{
    std::map<uint32_t, uint32_t> items;
    for (std::size_t i = 0; i + 1 < offsets.size(); i++)
    {
        items.emplace(offsets[i], static_cast<uint32_t>(i));
    }

    return items;
}

std::string VersionedSoundName(int song, int songs, std::span<const ntdlib::sequence_export::agb::SongVersion> versions,
                               std::size_t version)
{
    const std::string& named = version < versions.size() ? versions[version].name : std::string();
    return "Song " + ntdlib::sequence_export::agb::SongNumber(song, songs) + (named.empty() ? "" : " (" + named + ")");
}

std::shared_ptr<const DriverRom> LoadDriverRom(format::Engine engine, std::vector<uint8_t> bytes)
{
    return RowOf(engine).load(std::move(bytes));
}

} // namespace hokuto::song
