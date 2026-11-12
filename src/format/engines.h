// SPDX-License-Identifier: MIT

// The facts of each engine in one table: its family, its console, its code and name, and what it supports. The header
// contains only constexpr code. The song layer may include it without linking the format layer.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ntdlib/sar/formats.h"

#include "format/engine.h"

namespace hokuto::format
{

// The families of engines. An engine of one family reads the same kind of archive or ROM.
enum class Family : uint8_t
{
    kNintendoWare,  // the Switch, the Wii U and the 3DS: NintendoWare's sound archives
    kNw4r,          // the Wii: NW4R SND's sound archives
    kMp2k,          // the GBA's MusicPlayer2000
    kNitroComposer, // the DS SDK's NITRO-Composer
    kDse,           // Procyon Studio's Digital Sound Elements on the DS
    kGbaDriver,     // the GBA's other drivers (format/drivers.h)
};

// The consoles that the engines run on. song::Console is another type: the engine whose player reads a sequence.
enum class Platform : uint8_t
{
    kSwitch,
    kWiiU,
    k3ds,
    kWii,
    kGba,
    kDs,
};

// The facts of one engine.
struct EngineFacts
{
    Engine engine;
    Family family;
    Platform platform;
    uint8_t code;                              // the engine's code in a .hok file, and the code of its native archive
    std::string_view name;                     // the engine's name in text, and the name of its native archive
    std::optional<ntdlib::sar::Flavor> flavor; // the flavor of a NintendoWare console's archive
    std::string_view title;                    // the engine's name in the program's messages
    bool creatable;                            // NewFile makes a new file of the engine
    bool strict_only;                          // the engine plays in strict mixing only
    bool hosts_other_consoles;                 // sounds of other consoles may play on the engine
};

// The facts of every engine, in the order of the enum.
inline constexpr std::array<EngineFacts, kEngineCount> kEngineFacts = {{
    {Engine::kSwitch, Family::kNintendoWare, Platform::kSwitch, 1, "switch", ntdlib::sar::Flavor::kNx, "Switch", true,
     false, true},
    {Engine::kWiiU, Family::kNintendoWare, Platform::kWiiU, 2, "wii_u", ntdlib::sar::Flavor::kCafe, "Wii U", true,
     false, true},
    {Engine::k3ds, Family::kNintendoWare, Platform::k3ds, 3, "3ds", ntdlib::sar::Flavor::kCtr, "3DS", true, false,
     true},
    {Engine::kWii, Family::kNw4r, Platform::kWii, 4, "wii", std::nullopt, "Wii", true, true, true},
    {Engine::kGba, Family::kMp2k, Platform::kGba, 5, "gba", std::nullopt, "GBA", false, false, false},
    {Engine::kDs, Family::kNitroComposer, Platform::kDs, 6, "ds", std::nullopt, "DS", true, false, true},
    {Engine::kDse, Family::kDse, Platform::kDs, 7, "dse", std::nullopt, "DSE", false, false, false},
    {Engine::kKonami, Family::kGbaDriver, Platform::kGba, 8, "konami", std::nullopt, "Konami's driver", false, false,
     false},
    {Engine::kRd2, Family::kGbaDriver, Platform::kGba, 9, "rd2", std::nullopt, "Nintendo R&D2's driver", false, false,
     false},
    {Engine::kBrownie, Family::kGbaDriver, Platform::kGba, 10, "brownie", std::nullopt, "Brownie Brown's driver", false,
     false, false},
    {Engine::kUbiMilan, Family::kGbaDriver, Platform::kGba, 11, "ubimilan", std::nullopt, "Ubisoft Milan's driver",
     false, false, false},
    {Engine::kRare, Family::kGbaDriver, Platform::kGba, 12, "rare", std::nullopt, "Rare's driver", false, false, false},
    {Engine::kQuintet, Family::kGbaDriver, Platform::kGba, 13, "quintet", std::nullopt, "Quintet's driver", false,
     false, false},
    {Engine::kKrawall, Family::kGbaDriver, Platform::kGba, 14, "krawall", std::nullopt, "Krawall", false, false, false},
}};

// The facts of `engine`.
constexpr const EngineFacts& FactsOf(Engine engine)
{
    return kEngineFacts[static_cast<std::size_t>(engine)];
}

// Whether `engine` is the engine of one of the GBA's drivers other than MusicPlayer2000 (format/drivers.h).
constexpr bool IsDriverEngine(Engine engine)
{
    return FactsOf(engine).family == Family::kGbaDriver;
}

// The engine of the archives of `flavor`: the Switch's for kNx, the Wii U's for kCafe and the 3DS's for kCtr.
constexpr Engine EngineOf(ntdlib::sar::Flavor flavor)
{
    for (const EngineFacts& facts : kEngineFacts)
    {
        if (facts.flavor == flavor)
        {
            return facts.engine;
        }
    }

    return Engine::kSwitch;
}

// The flavor of the archives of `engine`'s console, or nothing for the Wii, the GBA and the DS. The archives of the Wii
// and the DS are of other families, the GBA keeps its songs in the ROM, and DSE keeps its songs in files of the image.
constexpr std::optional<ntdlib::sar::Flavor> FlavorOf(Engine engine)
{
    return FactsOf(engine).flavor;
}

namespace engines_detail
{

// Whether the rows of kEngineFacts agree with each other: one row for each engine in the order of the enum, codes 1 to
// kEngineCount, a flavor for NintendoWare's consoles alone, the GBA's drivers on the GBA, and distinct names.
constexpr bool RowsAgree()
{
    for (std::size_t i = 0; i < kEngineFacts.size(); ++i)
    {
        const EngineFacts& facts = kEngineFacts[i];
        if (facts.engine != static_cast<Engine>(i) || facts.code != i + 1 || facts.name.empty() ||
            facts.title.empty() || facts.flavor.has_value() != (facts.family == Family::kNintendoWare) ||
            (facts.family == Family::kGbaDriver && facts.platform != Platform::kGba))
        {
            return false;
        }

        for (std::size_t j = 0; j < i; ++j)
        {
            if (kEngineFacts[j].name == facts.name)
            {
                return false;
            }
        }
    }

    return true;
}

} // namespace engines_detail

static_assert(engines_detail::RowsAgree(), "kEngineFacts needs one consistent row for each engine, in enum order");

} // namespace hokuto::format
