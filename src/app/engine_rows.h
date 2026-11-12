// SPDX-License-Identifier: MIT

#pragma once

// The facts of each engine that the program's windows show, in one table: the mixer's switch, the FX channels, the
// effects that an insert offers, the Export window's targets and the filters of the dialog that saves an archive or a
// ROM. format/engines.h has the facts that the other layers use, such as each engine's title (EngineName) and whether
// New Project offers it (EngineFacts::creatable). A new engine adds one row here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "app/export_window.h"
#include "app/mixer.h"
#include "format/engine.h"
#include "format/file.h"

namespace hokuto::app
{

// The filters of the dialog that saves a file's sound archive or ROM.
enum class ArchiveFilters : uint8_t
{
    kLaterArchive, // a sound archive of the Switch or the Wii U
    k3dsArchive,
    kWiiArchive,
    kGbaRom,
    kDs,      // a DS image for a file that keeps the image's cartridge, and a DS sound archive otherwise
    kDsImage, // a DS image
};

// The kinds that each console's inserts offer. The Switch has nn::audio's I3DL2 reverb, and the Wii U the effects of
// nw::snd's four classes. The Wii has the effects of the SDK's AXFX library. NW4R SND's four classes set them up.
// Before 2008, the reverbs take the older parameters. From 2008, they are AXFX's Exp reverbs, with the parameters of
// the Wii U's reverbs. The 3DS has the reverbs and delays of nw::snd and nn::snd. Games of 2011 link nn::snd's older
// reverb. edit::EffectName gives each kind's name.
inline constexpr std::array<format::EffectKind, 1> kSwitchKinds = {format::EffectKind::kI3dl2Reverb};
inline constexpr std::array<format::EffectKind, 4> kWiiUKinds = {
    format::EffectKind::kReverbHi, format::EffectKind::kReverbStd, format::EffectKind::kChorus,
    format::EffectKind::kDelay};
inline constexpr std::array<format::EffectKind, 4> kWiiKinds = {
    format::EffectKind::kWiiReverbHi, format::EffectKind::kWiiReverbStd, format::EffectKind::kWiiChorus,
    format::EffectKind::kWiiDelay};
inline constexpr std::array<format::EffectKind, 4> kWii2008Kinds = {
    format::EffectKind::kWiiReverbHi2008, format::EffectKind::kWiiReverbStd2008, format::EffectKind::kWiiChorus,
    format::EffectKind::kWiiDelay};
inline constexpr std::array<format::EffectKind, 5> kCtrKinds = {
    format::EffectKind::kCtrReverb, format::EffectKind::kCtrDelay, format::EffectKind::kCtrNnReverb,
    format::EffectKind::kCtrNnDelay, format::EffectKind::kCtrNnReverb2011};

// What the windows show of one engine.
struct EngineRow
{
    format::Engine engine;
    const char* label;                         // the engine's name on the mixer's switch
    const char* library;                       // the library and the hardware whose rules the engine follows
    std::size_t fx_channels;                   // the FX channels, one for each aux bus of the engine's console
    std::span<const format::EffectKind> kinds; // the kinds that an insert offers (KindsOf in effect_editor.cpp)
    std::optional<ExportTarget> song_target;   // the Export window's target that sends a sequence's sound here
    std::optional<ExportTarget> file_target;   // the target that writes an item's file for the engine's console
    ArchiveFilters filters;                    // the filters of the dialog that saves the file's archive or ROM
};

// The rows of every engine, in the order of format::Engine. A file of the Wii offers the kinds of NW4R SND's build:
// the row lists the kinds of 2008, and KindsOf returns the older ones for a build before March 2008.
inline constexpr std::array<EngineRow, format::kEngineCount> kEngineRows = {{
    {
        .engine = format::Engine::kSwitch,
        .label = "Switch",
        .library = "nn::atk on the audio renderer",
        .fx_channels = kFxChannels,
        .kinds = kSwitchKinds,
        .song_target = ExportTarget::kSwitchSong,
        .file_target = ExportTarget::kSwitchFile,
        .filters = ArchiveFilters::kLaterArchive,
    },
    {
        .engine = format::Engine::kWiiU,
        .label = "Wii U",
        .library = "nw::snd on AX",
        .fx_channels = kFxChannels,
        .kinds = kWiiUKinds,
        .song_target = ExportTarget::kWiiUSong,
        .file_target = ExportTarget::kWiiUFile,
        .filters = ArchiveFilters::kLaterArchive,
    },
    {
        .engine = format::Engine::k3ds,
        .label = "3DS",
        .library = "nn::snd on the DSP",
        .fx_channels = 2,
        .kinds = kCtrKinds,
        .song_target = ExportTarget::kCtrSong,
        .file_target = ExportTarget::kCtrFile,
        .filters = ArchiveFilters::k3dsArchive,
    },
    {
        .engine = format::Engine::kWii,
        .label = "Wii",
        .library = "NW4R SND on AX",
        .fx_channels = kFxChannels,
        .kinds = kWii2008Kinds,
        .song_target = ExportTarget::kWiiSong,
        .file_target = ExportTarget::kWiiFile,
        .filters = ArchiveFilters::kWiiArchive,
    },
    {
        .engine = format::Engine::kGba,
        .label = "GBA",
        .library = "MusicPlayer2000 on the GBA sound hardware",
        .fx_channels = 0,
        .kinds = {},                 // MusicPlayer2000's reverb is a setting of the song
        .song_target = std::nullopt, // no song converts to the GBA yet
        .file_target = std::nullopt,
        .filters = ArchiveFilters::kGbaRom,
    },
    {
        .engine = format::Engine::kDs,
        .label = "DS",
        .library = "NITRO-Composer on the DS sound unit",
        .fx_channels = 0,
        .kinds = {}, // the SDK's reverb runs on the DS's capture units, without an aux bus
        .song_target = ExportTarget::kDsSong,
        .file_target = std::nullopt,
        .filters = ArchiveFilters::kDs,
    },
    {
        .engine = format::Engine::kDse,
        .label = "DSE",
        .library = "DSE on the DS sound unit",
        .fx_channels = 0,
        .kinds = {},                 // DSE's driver has no effects
        .song_target = std::nullopt, // no song converts to DSE
        .file_target = std::nullopt,
        .filters = ArchiveFilters::kDsImage,
    },
    {
        .engine = format::Engine::kKonami,
        .label = "Konami",
        .library = "Konami's driver on the GBA sound hardware",
        .fx_channels = 0,
        .kinds = {},                 // Konami's echo is a setting of the song
        .song_target = std::nullopt, // no song converts to Konami's driver
        .file_target = std::nullopt,
        .filters = ArchiveFilters::kGbaRom,
    },
    {
        .engine = format::Engine::kRd2,
        .label = "R&D2",
        .library = "Nintendo R&D2's driver on the GBA sound hardware",
        .fx_channels = 0,
        .kinds = {},                 // the echo of Nintendo R&D2's driver is a send of each track, without an aux bus
        .song_target = std::nullopt, // no song converts to Nintendo R&D2's driver
        .file_target = std::nullopt,
        .filters = ArchiveFilters::kGbaRom,
    },
    {
        .engine = format::Engine::kBrownie,
        .label = "Brownie",
        .library = "Brownie Brown's driver on the GBA sound hardware",
        .fx_channels = 0,
        .kinds = {},                 // Brownie Brown's driver has no effects
        .song_target = std::nullopt, // no song converts to Brownie Brown's driver
        .file_target = std::nullopt,
        .filters = ArchiveFilters::kGbaRom,
    },
    {
        .engine = format::Engine::kUbiMilan,
        .label = "Ubisoft Milan",
        .library = "Ubisoft Milan's driver on the GBA sound hardware",
        .fx_channels = 0,
        .kinds = {},                 // Ubisoft Milan's driver has no effects
        .song_target = std::nullopt, // no song converts to Ubisoft Milan's driver
        .file_target = std::nullopt,
        .filters = ArchiveFilters::kGbaRom,
    },
    {
        .engine = format::Engine::kRare,
        .label = "Rare",
        .library = "Rare's driver on the GBA sound hardware",
        .fx_channels = 0,
        .kinds = {},                 // Rare's driver has no effects
        .song_target = std::nullopt, // no song converts to Rare's driver
        .file_target = std::nullopt,
        .filters = ArchiveFilters::kGbaRom,
    },
    {
        .engine = format::Engine::kQuintet,
        .label = "Quintet",
        .library = "Quintet's driver on the GBA sound hardware",
        .fx_channels = 0,
        .kinds = {},                 // Quintet's driver has no effects
        .song_target = std::nullopt, // no song converts to Quintet's driver
        .file_target = std::nullopt,
        .filters = ArchiveFilters::kGbaRom,
    },
    {
        .engine = format::Engine::kKrawall,
        .label = "Krawall",
        .library = "Krawall on the GBA sound hardware",
        .fx_channels = 0,
        .kinds = {},                 // Krawall has no effects on a bus
        .song_target = std::nullopt, // no song converts to Krawall
        .file_target = std::nullopt,
        .filters = ArchiveFilters::kGbaRom,
    },
}};

// The row of `engine`.
constexpr const EngineRow& RowOf(format::Engine engine)
{
    return kEngineRows[static_cast<std::size_t>(engine)];
}

namespace engine_rows_detail
{

// Whether kEngineRows has one row for each engine, in the order of the enum, with a label and a library. std::array
// fills a missing row with zeros without a warning.
constexpr bool RowsAgree()
{
    for (std::size_t i = 0; i < kEngineRows.size(); ++i)
    {
        const EngineRow& row = kEngineRows[i];
        if (row.engine != static_cast<format::Engine>(i) || row.label == nullptr || row.library == nullptr)
        {
            return false;
        }
    }

    return true;
}

} // namespace engine_rows_detail

static_assert(engine_rows_detail::RowsAgree(), "kEngineRows needs one row for each engine, in enum order");

} // namespace hokuto::app
