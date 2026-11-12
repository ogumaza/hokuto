// SPDX-License-Identifier: MIT

// Tests what each layer says about each engine against one table of expected facts. A new engine without a row fails
// here: the table has a row for each enumerator of format::Engine, in the order of the enum.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ntdlib/sar/formats.h"
#include "xsflib/testing/check.h"

#include "app/engine_names.h"
#include "app/engine_rows.h"
#include "app/export_window.h"
#include "edit/drivers.h"
#include "edit/effects.h"
#include "edit/other_console.h"
#include "format/codes.h"
#include "format/drivers.h"
#include "format/engine.h"
#include "format/engines.h"
#include "format/file.h"
#include "format/model.h"
#include "song/driver.h"

namespace
{

using hokuto::app::ArchiveFilters;
using hokuto::app::ExportTarget;
using hokuto::format::EffectKind;
using hokuto::format::Engine;
using ntdlib::sar::Flavor;

// What the layers say about one engine.
struct ExpectedRow
{
    Engine engine;
    uint8_t code;                 // format::CodeOf
    std::string_view name;        // format::NameOf
    std::optional<Flavor> flavor; // format::FlavorOf
    bool is_driver;               // format::IsDriverEngine, format::DriverOf, song::RulesOf and edit::EditsOf
    bool creatable;               // format::NewFile makes a file rather than throwing
    bool strict_only;             // format::MixingRefused refuses enhanced mixing
    bool runs_i3dl2;              // edit::RunsEffect of the Switch's I3DL2 reverb
    bool runs_cafe;               // of the Wii U's FxReverbHi
    bool runs_wii;                // of the Wii's reverb
    bool runs_ctr;                // of the 3DS's reverb
    std::string_view title;       // app::EngineName
    std::string_view the_engine;  // app::TheEngine
    ExportTarget song_target;     // app::SongTargetFor
    bool hosts_other_consoles;    // edit::EngineRefused lets a sound of another console play on the engine
};

constexpr std::array<ExpectedRow, hokuto::format::kEngineCount> kExpected = {{
    {Engine::kSwitch, 1, "switch", Flavor::kNx, false, true, false, true, false, false, false, "Switch",
     "the Switch engine", ExportTarget::kSwitchSong, true},
    {Engine::kWiiU, 2, "wii_u", Flavor::kCafe, false, true, false, false, true, false, false, "Wii U",
     "the Wii U engine", ExportTarget::kWiiUSong, true},
    {Engine::k3ds, 3, "3ds", Flavor::kCtr, false, true, false, false, false, false, true, "3DS", "the 3DS engine",
     ExportTarget::kCtrSong, true},
    {Engine::kWii, 4, "wii", std::nullopt, false, true, true, false, false, true, false, "Wii", "the Wii engine",
     ExportTarget::kWiiSong, true},
    {Engine::kGba, 5, "gba", std::nullopt, false, false, false, false, false, false, false, "GBA", "the GBA engine",
     ExportTarget::kSwitchSong, false},
    {Engine::kDs, 6, "ds", std::nullopt, false, true, false, false, false, false, false, "DS", "the DS engine",
     ExportTarget::kDsSong, true},
    {Engine::kDse, 7, "dse", std::nullopt, false, false, false, false, false, false, false, "DSE", "the DSE engine",
     ExportTarget::kSwitchSong, false},
    {Engine::kKonami, 8, "konami", std::nullopt, true, false, false, false, false, false, false, "Konami's driver",
     "Konami's driver", ExportTarget::kSwitchSong, false},
    {Engine::kRd2, 9, "rd2", std::nullopt, true, false, false, false, false, false, false, "Nintendo R&D2's driver",
     "Nintendo R&D2's driver", ExportTarget::kSwitchSong, false},
    {Engine::kBrownie, 10, "brownie", std::nullopt, true, false, false, false, false, false, false,
     "Brownie Brown's driver", "Brownie Brown's driver", ExportTarget::kSwitchSong, false},
    {Engine::kUbiMilan, 11, "ubimilan", std::nullopt, true, false, false, false, false, false, false,
     "Ubisoft Milan's driver", "Ubisoft Milan's driver", ExportTarget::kSwitchSong, false},
    {Engine::kRare, 12, "rare", std::nullopt, true, false, false, false, false, false, false, "Rare's driver",
     "Rare's driver", ExportTarget::kSwitchSong, false},
    {Engine::kQuintet, 13, "quintet", std::nullopt, true, false, false, false, false, false, false, "Quintet's driver",
     "Quintet's driver", ExportTarget::kSwitchSong, false},
    {Engine::kKrawall, 14, "krawall", std::nullopt, true, false, false, false, false, false, false, "Krawall",
     "the Krawall engine", ExportTarget::kSwitchSong, false},
}};

// What the program's windows show of one engine.
struct ExpectedWindowRow
{
    Engine engine;
    std::size_t fx_channels; // app::FxChannelsOf
    std::size_t kinds;      // the kinds of effect that an insert offers (for a file of the Wii, from the build of 2008)
    ArchiveFilters filters; // the filters of App::DialogFilters for the file's archive or ROM
};

constexpr std::array<ExpectedWindowRow, hokuto::format::kEngineCount> kExpectedWindows = {{
    {Engine::kSwitch, 3, 1, ArchiveFilters::kLaterArchive},
    {Engine::kWiiU, 3, 4, ArchiveFilters::kLaterArchive},
    {Engine::k3ds, 2, 5, ArchiveFilters::k3dsArchive},
    {Engine::kWii, 3, 4, ArchiveFilters::kWiiArchive},
    {Engine::kGba, 0, 0, ArchiveFilters::kGbaRom},
    {Engine::kDs, 0, 0, ArchiveFilters::kDs},
    {Engine::kDse, 0, 0, ArchiveFilters::kDsImage},
    {Engine::kKonami, 0, 0, ArchiveFilters::kGbaRom},
    {Engine::kRd2, 0, 0, ArchiveFilters::kGbaRom},
    {Engine::kBrownie, 0, 0, ArchiveFilters::kGbaRom},
    {Engine::kUbiMilan, 0, 0, ArchiveFilters::kGbaRom},
    {Engine::kRare, 0, 0, ArchiveFilters::kGbaRom},
    {Engine::kQuintet, 0, 0, ArchiveFilters::kGbaRom},
    {Engine::kKrawall, 0, 0, ArchiveFilters::kGbaRom},
}};

// Whether the table has a row for each engine, in the order of the enum. std::array fills a missing row with zeros
// without a warning.
constexpr bool EveryEngineHasARow()
{
    for (std::size_t i = 0; i < kExpected.size(); ++i)
    {
        if (kExpected[i].engine != static_cast<Engine>(i) || kExpected[i].code == 0)
        {
            return false;
        }
    }

    return true;
}

static_assert(EveryEngineHasARow(), "kExpected needs a row for each engine, in the order of format::Engine");

// Checks one column of one engine's row, and names both on a failure.
void CheckColumn(bool equal, const ExpectedRow& row, const char* column)
{
    if (!equal)
    {
        std::fprintf(stderr, "engine %d (%.*s): %s differs\n", static_cast<int>(row.engine),
                     static_cast<int>(row.name.size()), row.name.data(), column);
    }
    XSFLIB_CHECK(equal);
}

// Whether format::NewFile makes a file of `engine` rather than throwing.
bool Creates(Engine engine)
{
    try
    {
        static_cast<void>(hokuto::format::NewFile(engine));
        return true;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
}

// Whether song::RulesOf returns the rules of a revision of the driver of `engine`, and edit::EditsOf its edits, rather
// than throwing. The rules and the edits name the same engine.
bool HasDriverRules(Engine engine)
{
    try
    {
        const hokuto::song::DriverRules& rules = hokuto::song::RulesOf(engine, 0);
        const hokuto::edit::DriverEdits& edits = hokuto::edit::EditsOf(rules);
        static_cast<void>(edits);
        return rules.DriverEngine() == engine;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
}

// The format layer: the codes, the names, the flavors, the drivers, the files that NewFile makes and the mixings.
void TestFormat()
{
    for (const ExpectedRow& row : kExpected)
    {
        CheckColumn(hokuto::format::CodeOf(row.engine) == row.code, row, "CodeOf");
        CheckColumn(hokuto::format::NameOf(row.engine) == row.name, row, "NameOf");
        CheckColumn(hokuto::format::EngineOfCode(row.code) == row.engine, row, "EngineOfCode");
        CheckColumn(hokuto::format::EngineNamed(row.name) == row.engine, row, "EngineNamed");
        CheckColumn(hokuto::format::FlavorOf(row.engine) == row.flavor, row, "FlavorOf");
        CheckColumn(hokuto::format::IsDriverEngine(row.engine) == row.is_driver, row, "IsDriverEngine");
        CheckColumn((hokuto::format::DriverOf(row.engine) != nullptr) == row.is_driver, row, "DriverOf");
        CheckColumn(!row.is_driver || hokuto::format::DriverOf(row.engine)->Facts().title == row.title, row,
                    "the driver's title");
        CheckColumn(Creates(row.engine) == row.creatable, row, "NewFile");
        CheckColumn(
            hokuto::format::MixingRefused(row.engine, hokuto::format::Mixing::kEnhanced).has_value() == row.strict_only,
            row, "MixingRefused");
        CheckColumn(!hokuto::format::MixingRefused(row.engine, hokuto::format::Mixing::kStrict), row,
                    "MixingRefused of strict mixing");
    }
}

// The song and edit layers: the drivers' rules and edits, the effects that each engine runs, and the engines that
// play other consoles' sounds.
void TestSongAndEdit()
{
    // A file of the DS: a sound of it may play on every engine that hosts other consoles' sounds.
    const hokuto::format::File ds_file = hokuto::format::NewFile(Engine::kDs);
    for (const ExpectedRow& row : kExpected)
    {
        CheckColumn(HasDriverRules(row.engine) == row.is_driver, row, "RulesOf and EditsOf");
        CheckColumn(hokuto::edit::RunsEffect(row.engine, EffectKind::kI3dl2Reverb) == row.runs_i3dl2, row,
                    "RunsEffect of the I3DL2 reverb");
        CheckColumn(hokuto::edit::RunsEffect(row.engine, EffectKind::kReverbHi) == row.runs_cafe, row,
                    "RunsEffect of the Wii U's reverb");
        CheckColumn(hokuto::edit::RunsEffect(row.engine, EffectKind::kWiiReverbHi) == row.runs_wii, row,
                    "RunsEffect of the Wii's reverb");
        CheckColumn(hokuto::edit::RunsEffect(row.engine, EffectKind::kCtrReverb) == row.runs_ctr, row,
                    "RunsEffect of the 3DS's reverb");
        CheckColumn(!hokuto::edit::EngineRefused(ds_file, row.engine).has_value() == row.hosts_other_consoles, row,
                    "EngineRefused");
    }
}

// The table of format/engines.h agrees with what the layers say.
void TestTable()
{
    for (const ExpectedRow& row : kExpected)
    {
        const hokuto::format::EngineFacts& facts = hokuto::format::FactsOf(row.engine);
        CheckColumn(facts.engine == row.engine, row, "the table's engine");
        CheckColumn(facts.code == row.code, row, "the table's code");
        CheckColumn(facts.name == row.name, row, "the table's name");
        CheckColumn(facts.flavor == row.flavor, row, "the table's flavor");
        CheckColumn((facts.family == hokuto::format::Family::kGbaDriver) == row.is_driver, row, "the table's family");
        CheckColumn(facts.title == row.title, row, "the table's title");
        CheckColumn(facts.creatable == row.creatable, row, "the table's creatable");
        CheckColumn(facts.strict_only == row.strict_only, row, "the table's strict_only");
        CheckColumn(facts.hosts_other_consoles == row.hosts_other_consoles, row, "the table's hosts_other_consoles");
    }
}

// The program's names of the engines, and the consoles that the Export window sends a song to.
void TestApp()
{
    for (const ExpectedRow& row : kExpected)
    {
        CheckColumn(hokuto::app::EngineName(row.engine) == row.title, row, "EngineName");
        CheckColumn(hokuto::app::TheEngine(row.engine) == row.the_engine, row, "TheEngine");
        CheckColumn(hokuto::app::SongTargetFor(row.engine) == row.song_target, row, "SongTargetFor");
    }
}

// The table of app/engine_rows.h agrees with the Export window's targets and with what the windows show.
void TestWindowRows()
{
    for (const ExpectedRow& row : kExpected)
    {
        const hokuto::app::EngineRow& window = hokuto::app::RowOf(row.engine);
        const ExpectedWindowRow& expected = kExpectedWindows[static_cast<std::size_t>(row.engine)];
        CheckColumn(window.engine == row.engine && expected.engine == row.engine, row, "the windows' engine");
        CheckColumn(window.song_target.value_or(ExportTarget::kSwitchSong) == row.song_target, row,
                    "the windows' song_target");
        CheckColumn(!window.song_target || hokuto::app::SongEngineOf(*window.song_target) == row.engine, row,
                    "SongEngineOf");
        CheckColumn(!window.file_target || hokuto::app::ConsoleOf(*window.file_target) == row.flavor, row, "ConsoleOf");
        CheckColumn(window.fx_channels == expected.fx_channels, row, "the windows' fx_channels");
        CheckColumn(window.kinds.size() == expected.kinds, row, "the windows' kinds");
        CheckColumn(window.filters == expected.filters, row, "the windows' filters");
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("engines",
                                    []
                                    {
                                        TestFormat();
                                        TestSongAndEdit();
                                        TestApp();
                                        TestTable();
                                        TestWindowRows();
                                    });
}
