// SPDX-License-Identifier: MIT

// The sound archive that a check opens the way the program opens it: an archive of the Switch, the Wii U or the 3DS
// through ntdlib's sar, an archive of the Wii through ntdlib's rsar, a GBA ROM whose songs MusicPlayer2000 plays, or a
// sound archive of the DS (SDAT), alone or in a cartridge image. An archive of the Wii takes the game's build of NW4R
// SND from the game's main.dol, as the program's import takes it. A GBA ROM's sounds are the songs of its song table,
// by their numbers, and each song is a sequence sound that uses its voice group. A DS archive's sounds are its
// sequences (song::DsSounds), and each uses the bank that its entry names. From a cartridge image, the check uses the
// largest archive, played by the driver of the ARM7 binary of the image. From a cartridge image with DSE files and no
// sound archive, the check uses their songs and sound effects (song::DseSounds), played by the driver of its ARM9
// binary. From a GBA ROM with another GBA driver, such as Konami's driver, the check uses the sounds of that driver
// (song::DriverRom::Sounds). The checks of the edits play the sounds of every kind through the same calls. With
// --model, a check keeps its file in the model form (CheckedForm), and the files that its edits make have to save and
// open again (CheckSaved).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/commands.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/dse/commands.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/konami/commands.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/layout.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/rd2/sequencer.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"
#include "xsflib/files.h"

#include "edit/bytecode.h"
#include "edit/ds_files.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/brownie_items.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/brownie.h"
#include "song/driver.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/items.h"
#include "song/konami.h"
#include "song/krawall.h"
#include "song/rare.h"
#include "song/rd2.h"
#include "song/sequence.h"
#include "song/ubimilan.h"

namespace hokuto::tools
{

// What the checks read of a sound of either kind of archive.
struct CheckedSound
{
    std::string name;
    uint32_t file_id = 0;
    bool sequence = false;
};

// An archive of one kind: `sar`, `wii`, `gba`, `driver`, `ds` or `dse`. An archive of the Wii keeps the game's build of
// NW4R SND in `build`, or nothing when the check found no main.dol. The archive's version then gives the build.
struct CheckedArchive
{
    // The number of sounds in the archive's table of sounds.
    std::size_t SoundCount() const
    {
        return dse             ? dse->sounds.size()
               : ds            ? ds->sounds.size()
               : gba || driver ? names.size()
               : wii           ? wii->Sounds().size()
                               : sar->Sounds().size();
    }

    // Whether sound `sound` is a sequence sound. On the GBA, it's a song that plays (song::GbaSongs), and on the DS an
    // entry that contains a sequence. DSE's sounds all play a sequence. On another of the GBA's drivers, it's a sound
    // of song::DriverRom::Sounds.
    bool IsSequence(uint32_t sound) const
    {
        return dse             ? true
               : ds            ? ds->sounds[sound].has_value()
               : gba || driver ? gba_plays[sound]
               : wii           ? wii->Sounds()[sound].type == ntdlib::rsar::SoundType::kSequence
                               : sar->Sounds()[sound].type == ntdlib::sar::SoundType::kSequence;
    }

    // The ID of the file of sound `sound` in the archive's file table. On the GBA and the DS, it's the sound's number.
    uint32_t FileOf(uint32_t sound) const
    {
        return gba || driver || ds || dse ? sound : wii ? wii->Sounds()[sound].file_id : sar->Sounds()[sound].file_id;
    }

    // The name of sound `sound`.
    const std::string& NameOf(uint32_t sound) const
    {
        return gba || driver || ds || dse ? names[sound] : wii ? wii->Sounds()[sound].name : sar->Sounds()[sound].name;
    }

    // What the checks read of sound `sound`.
    CheckedSound Info(uint32_t sound) const
    {
        return {NameOf(sound), FileOf(sound), IsSequence(sound)};
    }

    // The bank in each of the sound's bank slots, by its index in the archive's bank table. A slot without a bank holds
    // nothing. A sound of the Wii plays one bank, in slot 0.
    std::vector<std::optional<uint32_t>> BanksOf(uint32_t sound) const
    {
        std::vector<std::optional<uint32_t>> banks;
        if (gba || driver)
        {
            return banks;
        }
        if (ds)
        {
            banks.push_back(song::DsBankOf(*ds, sound));
            return banks;
        }
        if (dse)
        {
            for (const std::shared_ptr<const ntdlib::dse::WaveBank>& bank : dse->sounds[sound].banks)
            {
                banks.push_back(
                    static_cast<uint32_t>(std::find(dse_banks.begin(), dse_banks.end(), bank) - dse_banks.begin()));
            }
            return banks;
        }
        if (wii)
        {
            const uint32_t bank = wii->Sounds()[sound].bank;
            banks.push_back(bank < wii->Banks().size() ? std::optional<uint32_t>(bank) : std::nullopt);
            return banks;
        }

        for (const uint32_t slot : sar->Sounds()[sound].banks)
        {
            banks.push_back(slot == ntdlib::sar::kNoItem ? std::nullopt
                                                         : std::optional<uint32_t>(ntdlib::sar::ItemNumber(slot)));
        }

        return banks;
    }

    // The programs of bank `bank` (song::ProgramsOf, song::DsBankPrograms on the DS or song::DseBankPrograms on DSE). A
    // GBA ROM has no banks.
    std::vector<uint32_t> ProgramsOf(uint32_t bank) const
    {
        return dse             ? song::DseBankPrograms(*dse_banks.at(bank))
               : ds            ? song::DsBankPrograms(*ds, bank)
               : gba || driver ? std::vector<uint32_t>()
               : wii           ? song::ProgramsOf(*wii, bank)
                               : song::ProgramsOf(*sar, bank);
    }

    // The sound as ntdlib's performer plays it (song::ReadSequence). A sound of the Wii plays by the rules of `build`.
    // A GBA song and a sound of DSE or another of the GBA's drivers play without starting values and changes, and a DS
    // sequence without changes.
    song::Sequence Read(uint32_t sound, const song::StartValues& values = {}, const song::Changes& changes = {}) const
    {
        return dse      ? song::ReadSequence(*dse, sound)
               : ds     ? song::ReadSequence(*ds, sound, values)
               : gba    ? song::ReadSequence(*gba, static_cast<int>(sound))
               : driver ? driver->ReadSequence(sound)
               : wii    ? song::ReadSequence(*wii, sound, values, changes, build)
                        : song::ReadSequence(*sar, sound, values, changes);
    }

    std::shared_ptr<const ntdlib::sar::SoundArchive> sar;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
    std::optional<ntdlib::SndBuild> build;
    std::shared_ptr<const song::GbaRom> gba;
    std::shared_ptr<const song::DriverRom> driver;
    std::shared_ptr<const song::DsArchive> ds;
    std::shared_ptr<const song::DseSet> dse;
    std::vector<std::shared_ptr<const ntdlib::dse::WaveBank>> dse_banks; // each bank of DSE's sounds, once
    std::vector<std::string> names; // the name of each song of a GBA ROM, or of each sound of the DS or of DSE
    std::vector<bool> gba_plays;    // whether each song of the GBA's song table plays, or each sound of `driver`

    // The ROM of Konami's driver, or nothing for another kind of archive.
    const song::KonamiRom* Konami() const
    {
        return song::KonamiOf(driver.get());
    }

    // The ROM of Nintendo R&D2's driver, or nothing for another kind of archive.
    const song::Rd2Rom* Rd2() const
    {
        return song::Rd2Of(driver.get());
    }

    // The ROM of Brownie Brown's driver, or nothing for another kind of archive.
    const song::BrownieRom* Brownie() const
    {
        return song::BrownieOf(driver.get());
    }

    // The ROM of Ubisoft Milan's driver, or nothing for another kind of archive.
    const song::UbiMilanRom* UbiMilan() const
    {
        return song::UbiMilanOf(driver.get());
    }

    // The ROM of Rare's driver, or nothing for another kind of archive.
    const song::RareRom* Rare() const
    {
        return song::RareOf(driver.get());
    }

    // The ROM of Krawall, or nothing for another kind of archive.
    const song::KrawallRom* Krawall() const
    {
        return song::KrawallOf(driver.get());
    }
};

// The GBA ROM `bytes` as a checked archive.
CheckedArchive GbaChecked(std::vector<uint8_t> bytes);

// A checked archive of the GBA ROM `bytes` with the driver of `engine`, another of the GBA's drivers. Its sounds run
// from 0 to the driver's SoundCount, and those that the driver's Sounds leave out don't play. On Konami's driver, each
// song comes twice: as the song, then as the game's live stage plays it.
CheckedArchive DriverChecked(format::Engine engine, std::vector<uint8_t> bytes);

// A checked archive of the DS archive `sdat` of `game`, played by `build` of the driver.
CheckedArchive DsChecked(std::vector<uint8_t> sdat, const ntdlib::ntr::snd::Build& build,
                         std::optional<song::DsGame> game);

// A checked archive of the set of DSE's files `set`, played by `build` of the driver.
CheckedArchive DseChecked(std::span<const uint8_t> set, ntdlib::dse::Build build);

// Whether hok import treats `bytes` as a cartridge image with DSE's files and no sound archive.
bool IsDseImage(std::span<const uint8_t> bytes);

// The build of the driver that plays the archive of `file`, a DS file. The program finds the build the same way: from
// the ARM7 binary of the cartridge image that the file keeps, or the default build for an archive alone.
ntdlib::ntr::snd::Build DsBuildOfChecked(const format::File& file);

// Whether `bytes` hold an archive of the Wii.
bool IsWiiArchive(std::span<const uint8_t> bytes);

// The game's build of NW4R SND for the Wii archive at `path`, as the program's import reads it: from sys/main.dol of
// the extracted disc that holds the archive (song::FindMainDol and song::ReadWiiBuild). Nothing when there's no such
// main.dol, or when it gives no build.
std::optional<ntdlib::SndBuild> GameBuildOf(const std::filesystem::path& path);

// The words that name `build`, the game's build of NW4R SND from GameBuildOf, in a summary.
std::string BuildText(const std::optional<ntdlib::SndBuild>& build);

// The archive at `path` with the bytes `bytes` and the group files beside it. As in the program, the Wii's archive is
// read for its model (ntdlib::rvl::snd::kRsarOptions), and takes the game's build (GameBuildOf).
CheckedArchive OpenChecked(const std::filesystem::path& path, std::span<const uint8_t> bytes);

// Whether the check runs with --filler (TakeFillerOption).
inline bool filler_runs = false;

// Removes --filler from `arguments`, wherever it is, and sets filler_runs when it was there.
void TakeFillerOption(std::vector<std::string>& arguments);

// `file` with the check's choice of filler runs. With --filler, the songs of a GBA ROM that don't fit after its data go
// into its filler runs (format::NativeArchive::gba_filler). A file of another console stays as it is.
format::File WithFillerChoice(format::File file);

// The file that an import of `bytes`, an archive of the kind of `archive`, makes with `group_files` beside it, in the
// order of those of `archive`. Each takes its group as the program's import gives it: on the 3DS, from the name under
// which the reader found the group file of `archive` (ntdlib::sar::SoundArchive::GroupFileGroups). The Wii's archives
// have none. An archive of the Wii takes the build of `archive`, as the program's import takes the game's build. A GBA
// ROM uses the choice of filler runs of the check (WithFillerChoice).
format::File ImportChecked(std::span<const uint8_t> bytes, const CheckedArchive& archive,
                           std::vector<std::vector<uint8_t>> group_files);

// The same, with the group files of `archive` beside `bytes`.
format::File ImportChecked(std::span<const uint8_t> bytes, const CheckedArchive& archive);

// The archive that `file` builds, called `name`, with the group files that the file builds. An archive of the Wii takes
// the build that the file keeps.
CheckedArchive BuildChecked(const format::File& file, const std::string& name);

// The sequence of `file` that each file of `archive` holds (format::SequencesOfFiles). `bytes` are the archive's. On
// the GBA, the sequence of each song, by the song's number (format::GbaSongSequence). On the DS, the sequence of each
// sound, by its number (format::DsSoundSequences and format::DseSoundSequences), from the archive or the set that
// `file` builds: `bytes` can be a cartridge image.
std::vector<std::optional<uint32_t>> SequencesOfChecked(const format::File& file, std::span<const uint8_t> bytes,
                                                        const CheckedArchive& archive);

// Where each sound of `archive` is in `file` (edit::SoundPlaces, edit::GbaSoundPlaces on the GBA, or
// edit::DsSoundPlaces on the DS and on DSE). `bytes` are the archive's.
std::vector<std::optional<edit::SoundPlace>> PlacesOfChecked(const format::File& file, std::span<const uint8_t> bytes,
                                                             const CheckedArchive& archive);

// The sequence sounds that a check reads, by their sequence, with how many sounds it read and how many it couldn't.
struct SequenceSounds
{
    std::map<uint32_t, std::vector<uint32_t>> by_sequence;
    std::size_t read = 0;
    std::size_t unreadable = 0;
};

// Reads each sound of `archive` with a place in `places` into `songs`, by their sequence. With `only`, only the sound
// of that name.
SequenceSounds ReadSequenceSounds(const CheckedArchive& archive,
                                  std::span<const std::optional<edit::SoundPlace>> places, const std::string& only,
                                  std::map<uint32_t, song::Sequence>& songs);

// Whether the check runs with --model (TakeModelOption).
inline bool model_form = false;

// Takes --model out of `arguments`, wherever it stands, and sets model_form when it was there.
void TakeModelOption(std::vector<std::string>& arguments);

// `file` in the model form when the check runs with --model (format::InModelForm), or else `file`. An import of `bytes`
// made `file`. The edits then work on the archive that the model builds. Throws std::runtime_error when that archive
// differs from `bytes`, or its group files from `file`'s, and the exceptions of InModelForm.
format::File CheckedForm(format::File file, std::span<const uint8_t> bytes);

// The options of an edit check that takes --model, --filler and --timelines, an archive, and a sound's name.
struct EditOptions
{
    bool timelines = false;
    std::filesystem::path path;
    std::string only; // the sound to check alone, or empty for every sound
};

// The options of an edit check in the command line's `arguments`. Removes --model and --filler first (TakeModelOption
// and TakeFillerOption). Prints `usage` to stderr and returns nothing when the rest doesn't fit.
std::optional<EditOptions> ParseEditOptions(std::vector<std::string> arguments, const char* usage);

} // namespace hokuto::tools
