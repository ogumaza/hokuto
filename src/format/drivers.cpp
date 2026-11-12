// SPDX-License-Identifier: MIT

#include "format/drivers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "format/archive.h"
#include "format/engines.h"
#include "format/file.h"

namespace hokuto::format
{

ntdlib::agb::Rom RomOf(std::span<const uint8_t> bytes)
{
    ntdlib::agb::Rom rom;
    rom.Assign(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    return rom;
}

const Driver& KonamiDriver();
const Driver& Rd2Driver();
const Driver& BrownieDriver();
const Driver& UbiMilanDriver();
const Driver& RareDriver();
const Driver& QuintetDriver();
const Driver& KrawallDriver();

namespace
{

// The number of the GBA's drivers other than MusicPlayer2000: the engines for which IsDriverEngine returns true.
constexpr std::size_t kDriverCount = []
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < kEngineCount; ++i)
    {
        if (IsDriverEngine(static_cast<Engine>(i)))
        {
            ++count;
        }
    }

    return count;
}();

// Every driver, in the order of the engines.
std::array<const Driver*, kDriverCount> AllDrivers()
{
    auto drivers = std::to_array<const Driver*>({&KonamiDriver(), &Rd2Driver(), &BrownieDriver(), &UbiMilanDriver(),
                                                 &RareDriver(), &QuintetDriver(), &KrawallDriver()});
    static_assert(std::tuple_size_v<decltype(drivers)> == kDriverCount,
                  "AllDrivers needs one driver for each driver engine");

    return drivers;
}

} // namespace

const Driver* DriverOf(Engine engine)
{
    for (const Driver* driver : AllDrivers())
    {
        if (driver->Facts().engine == engine)
        {
            return driver;
        }
    }

    return nullptr;
}

const Driver* DriverOfRom(std::span<const uint8_t> rom)
{
    for (const Driver* driver : AllDrivers())
    {
        if (driver->Holds(rom))
        {
            return driver;
        }
    }

    return nullptr;
}

const Driver* DriverOfFile(const File& file)
{
    return file.native && file.native->driver ? DriverOf(file.native->engine) : nullptr;
}

const Driver* WrittenDriverOf(const NativeArchive& native)
{
    const Driver* driver = native.driver ? DriverOf(native.engine) : nullptr;
    if (native.driver && !driver)
    {
        throw std::invalid_argument("the native archive names a driver on another console's engine");
    }
    if (!native.driver && DriverOf(native.engine))
    {
        throw std::invalid_argument("the native archive names no driver on a driver's engine");
    }
    if (driver && !driver->Facts().filler_runs && (native.gba_filler || !native.driver->filler.empty()))
    {
        throw std::invalid_argument(std::string(driver->Facts().title) + " can't put songs into filler runs");
    }
    if (driver && !driver->Facts().rooms && !native.driver->rooms.empty())
    {
        throw std::invalid_argument(std::string(driver->Facts().title) + " keeps no rooms of lists of songs");
    }

    return driver;
}

std::optional<uint32_t> SequenceNamedAt(const File& file, uint64_t place)
{
    if (!file.native || !file.native->driver)
    {
        return std::nullopt;
    }

    return SequenceNamedAt(file, place, NativePieceStarts(file));
}

std::optional<uint32_t> SequenceNamedAt(const File& file, uint64_t place, std::span<const uint64_t> starts)
{
    if (!file.native || !file.native->driver)
    {
        return std::nullopt;
    }

    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    for (const ntdlib::sar::Field& field : file.native->fields)
    {
        if (!field.at.item && !field.at.sequence && field.at.piece < pieces.size() && field.at.piece < starts.size() &&
            starts[field.at.piece] + field.at.offset == place && field.to.item && field.to.piece < pieces.size())
        {
            if (const auto* piece = std::get_if<ntdlib::sar::SequencePiece>(&pieces[field.to.piece]))
            {
                return piece->sequence;
            }
        }
    }

    return std::nullopt;
}

std::vector<uint32_t> FieldsAt(const File& file, std::span<const uint64_t> places)
{
    if (!file.native || !file.native->driver)
    {
        return {};
    }

    return FieldsAt(file, places, NativePieceStarts(file));
}

std::vector<uint32_t> FieldsAt(const File& file, std::span<const uint64_t> places, std::span<const uint64_t> starts)
{
    if (!file.native || !file.native->driver)
    {
        return {};
    }

    // A place that two fields share maps to the later field.
    std::map<uint64_t, uint32_t> fields;
    for (uint32_t index = 0; index < file.native->fields.size(); index++)
    {
        const ntdlib::sar::Field& field = file.native->fields[index];
        if (!field.at.item && !field.at.sequence && field.at.piece < starts.size() && field.to.item)
        {
            fields[starts[field.at.piece] + field.at.offset] = index;
        }
    }

    std::vector<uint32_t> found;
    for (const uint64_t place : places)
    {
        const auto field = fields.find(place);
        if (field != fields.end())
        {
            found.push_back(field->second);
        }
    }

    return found;
}

bool Driver::Foreign(const ntdlib::commands::Command& item) const
{
    return (EncodingChoicesOf(item) & ~Facts().choices) != 0;
}

} // namespace hokuto::format
