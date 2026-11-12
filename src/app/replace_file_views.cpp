// SPDX-License-Identifier: MIT

// The views of a bank, a wave archive, a wave sound file or a SoundFont that replaces an item or comes in as a
// new one, on the 3DS, the Wii U, the Switch and the Wii. replace_wii_file_views.cpp contains the views of a file of
// the Wii.

#include "app/replace_file_views.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"

#include "app/export_warnings.h"
#include "app/item_views.h"
#include "app/replace_common.h"
#include "app/replace_file_views_parts.h"
#include "app/replace_view.h"
#include "edit/ds_new_items.h"
#include "edit/files.h"
#include "edit/groups.h"
#include "edit/new_items.h"
#include "edit/other_console.h"
#include "edit/sfz_imports.h"
#include "edit/sound_font_imports.h"
#include "format/file.h"
#include "song/items.h"

namespace hokuto::app
{
namespace
{

using replace_common::FillGroups;
using replace_common::FillImportModels;
using replace_common::FillWiiGroups;
using replace_common::HoldsNames;
using replace_common::kWiiLastGroup;
using replace_common::kWiiLastGroupFiles;
using replace_common::NameInUse;
using replace_common::NameTaken;
using replace_common::NewItemsWord;
using replace_common::NoNameRefusal;
using replace_common::NumberList;
using replace_common::Placed;
using replace_common::TakenNames;
using replace_common::WaveArchiveMenu;
using replace_common::WavesText;
using replace_common::WiiTakenNames;
using replace_file_parts::AddWaves;
using replace_file_parts::CountVerb;
using replace_file_parts::ItemImportTitle;
using replace_file_parts::NewSoundsNumberWarning;
using replace_file_parts::WarnFileSizes;
using replace_file_parts::WarnPastEnd;
using replace_file_parts::WarnSharing;
using replace_file_parts::WaveArchiveRows;
using replace_file_parts::WithReplacedSound;

// Whether a wave sound of `archive` plays `file`, the bytes of a wave sound file.
bool PlaysWaveSoundFile(const ntdlib::sar::SoundArchive& archive, std::span<const uint8_t> file)
{
    const auto same_sound = [&archive, &file](const ntdlib::sar::SoundInfo& sound)
    {
        return sound.type == ntdlib::sar::SoundType::kWaveSound &&
               std::ranges::equal(archive.FileData(sound.file_id), file);
    };

    return std::ranges::any_of(archive.Sounds(), same_sound);
}

// The line of a bank file, a wave archive file or a wave sound file that goes in: the name of the file that was read,
// `what` it is, and its console, its version and its size. `flavor` gives the archive's console.
std::string IncomingSource(const std::string& file_name, const std::string& what, const edit::IncomingFile& incoming,
                           ntdlib::sar::Flavor flavor)
{
    const std::optional<edit::ConvertedFile>& converted = incoming.converted;
    const ntdlib::sar::Flavor console = converted ? converted->flavor : flavor;
    return file_name + ": " + song::ConsoleName(console) + " " + what + ", version " +
           song::VersionText(console, converted ? converted->version : incoming.version) + ", " +
           SizeText(converted ? converted->size : incoming.bytes.size());
}

// The warning about a file that goes in converted for an archive of `flavor`, or nothing: a file of the other console,
// or one of a later version than the archive's files.
std::optional<std::string> ConversionWarning(const edit::IncomingFile& incoming, ntdlib::sar::Flavor flavor)
{
    if (!incoming.converted)
    {
        return std::nullopt;
    }

    const std::optional<uint32_t>& waves = incoming.converted->wave_version;
    if (incoming.converted->flavor == flavor)
    {
        return "The file is saved as version " + song::VersionText(flavor, incoming.version) +
               " to match the archive." +
               (waves ? " Its waves are saved as version " + song::VersionText(flavor, *waves) + "." : "") +
               " No values change. A game may reject a later version than the one it uses.";
    }

    std::string warning = "This is a " + song::ConsoleName(incoming.converted->flavor) + " file. It's converted to a " +
                          song::ConsoleName(flavor) + " file (version " + song::VersionText(flavor, incoming.version) +
                          ") with the same values.";
    if (waves)
    {
        warning += " Its waves become version " + song::VersionText(flavor, *waves) + ".";
    }

    return warning;
}

// "it" for one program, "them" for more.
std::string ProgramsPronoun(const std::vector<uint32_t>& programs)
{
    return std::string(programs.size() == 1 ? "it" : "them");
}

// "program 3", or "programs 3, 5".
std::string MissingProgramsText(const std::vector<uint32_t>& programs)
{
    return std::string(programs.size() == 1 ? "program " : "programs ") + NumberList(programs);
}

// The bank in `bytes`, a file of `archive`. Nothing for bytes that ntdlib can't read.
std::optional<ntdlib::sar::Bank> ReadBankOrNone(const ntdlib::sar::SoundArchive& archive,
                                                std::span<const uint8_t> bytes)
{
    try
    {
        return std::optional<ntdlib::sar::Bank>(ntdlib::sar::Bank::Parse(bytes, archive.ReadOptions()));
    }
    catch (const std::exception&)
    {
        return std::optional<ntdlib::sar::Bank>();
    }
}

// Warns about the current bank's programs that are missing from the new one (`changes`), with the sounds that play
// the bank.
void WarnMissingPrograms(const ntdlib::sar::SoundArchive& archive, const edit::FileChanges& changes, ReplaceView& view)
{
    if (!changes.programs.empty())
    {
        std::vector<std::string> players;
        for (const uint32_t sound : changes.players)
        {
            players.push_back(archive.Sounds()[sound].name);
        }
        view.warnings.push_back("Missing from the new bank: " + MissingProgramsText(changes.programs) + "." +
                                (players.empty() ? std::string(" No sound uses the bank.")
                                                 : " Notes using " + ProgramsPronoun(changes.programs) +
                                                       " will be silent in " + NameList(players) + "."));
    }
}

// Adds a row for each program of `selections` that is missing from the new bank, `next`. `present` is the current
// bank.
void AddProgramFixRows(const edit::BankSelections& selections, const std::optional<ntdlib::sar::Bank>& present,
                       const std::optional<ntdlib::sar::Bank>& next, std::span<const edit::ProgramFix> fixes,
                       ReplaceView& view)
{
    const auto holds = [](const std::optional<ntdlib::sar::Bank>& bank, uint32_t program)
    {
        return bank && program < bank->instruments.size() && bank->instruments[program].has_value();
    };

    const auto fixed = [&fixes](uint32_t program)
    {
        return std::ranges::find(fixes, program, &edit::ProgramFix::program) != fixes.end();
    };

    // The new bank's programs, without the copies.
    std::vector<uint32_t> held;
    for (uint32_t program = 0; next && program < next->instruments.size(); program++)
    {
        if (next->instruments[program] && !fixed(program))
        {
            held.push_back(program);
        }
    }

    // A row for each program that the sounds select and the new bank lacks, with the chosen fix.
    for (const uint32_t program : selections.programs)
    {
        if (holds(next, program) && !fixed(program))
        {
            continue;
        }

        ProgramFixRow& row = view.program_fixes.emplace_back();
        row.program = program;
        if (holds(present, program))
        {
            row.items.push_back("Instrument from current bank");
            row.fixes.push_back({program, std::nullopt});
        }
        for (const uint32_t from : held)
        {
            row.items.push_back("Program " + std::to_string(from) + " from new bank");
            row.fixes.push_back({program, from});
        }
        for (std::size_t i = 0; i < row.fixes.size(); i++)
        {
            row.choice = std::ranges::find(fixes, row.fixes[i]) != fixes.end() ? static_cast<int>(i) : row.choice;
        }
    }
}

// Warns about the program changes that `selections` can't follow, and the programs of `changes` that no sound selects.
void WarnUnselectedPrograms(const edit::BankSelections& selections, const edit::FileChanges& changes, ReplaceView& view)
{
    // The program changes whose program comes from a value, and the present bank's other programs that the new one
    // lacks.
    if (selections.undecided > 0)
    {
        const bool one = selections.undecided == 1;
        view.warnings.push_back(std::to_string(selections.undecided) + (one ? " program change" : " program changes") +
                                " in sounds using this bank " + (one ? "sets its" : "set their") +
                                " program or slot from a variable or a random number. " +
                                (one ? "Its program" : "Their programs") + " can't be determined.");
    }
    std::vector<uint32_t> unselected;
    for (const uint32_t program : changes.programs)
    {
        if (std::ranges::find(selections.programs, program) == selections.programs.end())
        {
            unselected.push_back(program);
        }
    }
    if (!unselected.empty())
    {
        view.warnings.push_back("Missing from the new bank: " + MissingProgramsText(unselected) +
                                ". No sound selects " + ProgramsPronoun(unselected) + ".");
    }
}

// Adds to `view` the rows and warnings of the programs that a new bank lacks, from the present bank's programs that the
// new one lacks (`changes`). `incoming` holds the instruments of `fixes`. Each program that the sounds select and the
// new bank lacks takes a row (MakeFileReplaceView).
void AddProgramFixes(const ReplaceSource& source, const edit::IncomingFile& incoming, const edit::FileChanges& changes,
                     std::span<const edit::ProgramFix> fixes, ReplaceView& view)
{
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();

    // Without the sounds' places, a warning lists the present bank's programs that the new one lacks, with the sounds
    // that play the bank.
    if (source.places.empty() || !source.file)
    {
        WarnMissingPrograms(archive, changes, view);
        return;
    }

    // The present bank and the new one. A bank that ntdlib can't read holds no programs.
    const std::optional<ntdlib::sar::Bank> present =
        ReadBankOrNone(archive, archive.FileData(edit::FileIdOf(archive, edit::FileKind::kBank, source.sound)));
    const std::optional<ntdlib::sar::Bank> next = ReadBankOrNone(archive, incoming.bytes);

    const edit::BankSelections selections = edit::ProgramsOfBank(*source.file, archive, source.places, source.sound);
    AddProgramFixRows(selections, present, next, fixes, view);
    WarnUnselectedPrograms(selections, changes, view);
}

} // namespace

namespace replace_file_parts
{

std::vector<WaveArchiveRow> WaveArchiveRows(const std::vector<edit::NamedWaveArchive>& named,
                                            const std::vector<int>& targets)
{
    std::vector<WaveArchiveRow> rows;
    for (std::size_t k = 0; k < named.size(); k++)
    {
        WaveArchiveRow& row = rows.emplace_back();
        row.label = "Wave archive " + std::to_string(ntdlib::sar::ItemNumber(named[k].id)) + " (" +
                    WavesText(named[k].waves.size()) + ")";
        row.target = k < targets.size() ? targets[k] : -1;
    }

    return rows;
}

std::string ItemImportTitle(const ReplaceSource& source, bool bank, bool wave_sounds)
{
    return source.new_bank ? "Add Bank"
           : bank          ? "Import Bank"
           : wave_sounds   ? "Import Wave Sound File"
                           : "Import Wave Archive";
}

std::string NewSoundsNumberWarning(const std::vector<std::string>& names, std::size_t first, std::size_t sounds)
{
    return sounds == 1
               ? "The new sound is number " + std::to_string(first) + Placed(first, "sounds") +
                     ". The game plays it only if the game code requests that number or name."
               : "The new sounds " + names.front() + " to " + names.back() + " are numbers " + std::to_string(first) +
                     " to " + std::to_string(first + sounds - 1) + Placed(first, "sounds") +
                     ". The game plays them only if the game code requests those numbers or names.";
}

std::string CountVerb(const std::vector<uint32_t>& items, const char* one, const char* more)
{
    return std::string(items.size() == 1 ? one : more);
}

void WarnFileSizes(ReplaceView& view, const edit::FileChanges& changes, const edit::IncomingFile& incoming)
{
    if (changes.size == incoming.bytes.size())
    {
        view.warnings.push_back("Both files are " + SizeText(changes.size) + ".");
    }
    else
    {
        const auto [present, added] = SizeTexts(changes.size, incoming.bytes.size());
        view.warnings.push_back("The current file is " + present + ". The new one is " + added + ".");
    }
}

std::vector<uint32_t> WithReplacedSound(uint32_t sound, const std::vector<uint32_t>& sharing)
{
    std::vector<uint32_t> sounds = {sound};
    for (const uint32_t other : sharing)
    {
        if (other != sound)
        {
            sounds.push_back(other);
        }
    }

    return sounds;
}

} // namespace replace_file_parts

namespace
{

// Sets the source line of `view` for a SoundFont or, where `sfz` is true, a set of SFZ files.
void FillSoundFontSource(ReplaceView& view, const ReplaceSource& source, const edit::SoundFontImport& import, bool sfz)
{
    const std::size_t banks = import.midi_banks.size();
    if (sfz)
    {
        view.source = source.file_name + ": " + std::to_string(import.presets) +
                      (import.presets == 1 ? " instrument" : " instruments") + ", imported as " +
                      (banks == 1 ? std::string("one bank") : std::to_string(banks) + " banks") + " with samples (" +
                      SizeText(import.bytes) + ").";
    }
    else
    {
        view.source =
            source.file_name + ": " + std::to_string(import.presets) + (import.presets == 1 ? " preset" : " presets") +
            " in " +
            (banks == 1 ? std::string("one MIDI bank, imported as one bank")
                        : std::to_string(banks) + " MIDI banks, imported as " + std::to_string(banks) + " banks") +
            " with samples (" + SizeText(import.bytes) + ").";
    }
}

// The names that the archive already contains, the number of the first new bank, and the names of the new files of
// the import.
struct SoundFontNames
{
    std::vector<std::string> taken_names;
    std::size_t first_bank = 0;
    std::vector<std::string> wanted;
};

// Fills the groups of `view` for the new banks, named `names`, and returns the names that the import checks.
SoundFontNames FillSoundFontGroups(ReplaceView& view, const ReplaceSource& source, const edit::SoundFontImport& import,
                                   const std::vector<std::string>& names)
{
    const std::size_t banks = import.midi_banks.size();
    std::vector<std::string> wanted = names;
    std::vector<std::string> taken_names;
    const std::string none = banks == 1
                                 ? "No group loads the new bank. None is proposed because no sound uses it yet. If "
                                   "the game loads files through groups, it won't load this bank."
                                 : "No group loads the new banks. None is proposed because no sound uses them "
                                   "yet. If the game loads files through groups, it won't load these banks.";
    std::size_t first_bank = 0;
    if (source.played.Ds())
    {
        taken_names = edit::DsTakenNames(*source.played.Ds());
        first_bank = source.played.Ds()->archive.banks.size();
    }
    else if (source.played.Wii())
    {
        taken_names = WiiTakenNames(source);
        first_bank = source.played.Wii()->Banks().size();
        std::vector<uint32_t> new_files;
        for (std::size_t i = 0; i < banks; i++)
        {
            new_files.push_back(source.played.Wii()->FileCount() + static_cast<uint32_t>(i));
        }
        const std::string wii_none = std::string(banks == 1 ? "No named group loads the new bank. None is proposed "
                                                              "because no sound uses it yet. "
                                                            : "No named group loads the new banks. None is proposed "
                                                              "because no sound uses them yet. ") +
                                     (banks == 1 ? kWiiLastGroup : kWiiLastGroupFiles);
        FillWiiGroups(view, source, {}, new_files, "", wii_none, {{new_files.front(), import.bytes}});
    }
    else
    {
        const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
        taken_names = TakenNames(*source.file->native->tables, &archive);
        first_bank = archive.Banks().size();
        std::vector<uint32_t> new_files;
        for (std::size_t i = 0; i < 2 * banks; i++)
        {
            new_files.push_back(static_cast<uint32_t>(archive.FileCount() + i));
        }
        FillGroups(view, source, {}, new_files, "", none, {{new_files.front(), import.bytes}});
    }
    if (!source.played.Wii())
    {
        for (const std::string& each : names)
        {
            wanted.push_back(edit::SoundFontWaveArchiveName(each));
        }
    }

    return {std::move(taken_names), first_bank, std::move(wanted)};
}

// Adds the warnings about what the new banks, named `names`, leave out and where they go. The first gets bank number
// `first_bank`.
void AddSoundFontWarnings(ReplaceView& view, const ReplaceSource& source, const edit::SoundFontImport& import,
                          const std::vector<std::string>& names, std::size_t first_bank)
{
    const std::size_t banks = import.midi_banks.size();
    view.warnings = import.warnings;
    if (source.played.Wii() && !view.offers_groups)
    {
        view.warnings.emplace_back(banks == 1 ? kWiiLastGroup : kWiiLastGroupFiles);
    }
    const std::string last_bank = std::to_string(first_bank + banks - 1);
    if (!view.names)
    {
        view.warnings.push_back(banks == 1 ? "The archive has no names. The new bank appears as BANK_" + last_bank + "."
                                           : "The archive has no names. The new banks appear as BANK_" +
                                                 std::to_string(first_bank) + " to BANK_" + last_bank + ".");
    }
    if (source.played.Ds() && import.bytes > edit::kDsFileMemory)
    {
        view.warnings.push_back("The new files take up " + SizeText(import.bytes) +
                                ". The DS model in Hokuto holds only " + SizeText(edit::kDsFileMemory) +
                                " of banks and waves. A bank that doesn't fit will be silent during playback.");
    }
    view.warnings.push_back(
        banks == 1
            ? "The new bank is number " + std::to_string(first_bank) + Placed(first_bank, "banks") +
                  ". It plays once a bank slot of a sound uses it. File > Import > Sequence… sets the banks for a new "
                  "sound."
            : "The new banks " + (view.names ? names.front() + " to " + names.back() + " " : std::string()) +
                  "are numbers " + std::to_string(first_bank) + " to " + last_bank + Placed(first_bank, "banks") +
                  ". A bank plays once a bank slot of a sound uses it. File > Import > Sequence… sets the banks for a "
                  "new sound.");
}

} // namespace

ReplaceView MakeSoundFontImportView(const ReplaceSource& source, const edit::SoundFontImport& import,
                                    const std::string& name)
{
    // The file and its MIDI banks.
    ReplaceView view;
    const bool sfz = edit::IsSfzName(source.file_name);
    view.title = sfz ? "Import SFZ Files" : "Import SoundFont";
    view.action = "Import";
    view.import = true;
    view.playable = source.played.Wii() == nullptr;
    view.sequence = false;
    view.heading = sfz ? "SFZ files" : "SoundFont 2 file";
    const std::size_t banks = import.midi_banks.size();
    FillSoundFontSource(view, source, import, sfz);
    name.copy(view.name.data(), view.name.size() - 1);
    view.names = source.played.Ds() == nullptr || source.played.Ds()->archive.has_symbols;

    // The groups, and the names that the archive contains. A bank of the Wii contains its waves. The new files come
    // after the files of the archive, and the bytes of the import count with the first new file. No sound plays a new
    // bank yet.
    const std::vector<std::string> names = edit::SoundFontBankNames(name, import.midi_banks);
    const auto [taken_names, first_bank, wanted] = FillSoundFontGroups(view, source, import, names);

    // The import makes the same checks.
    const auto taken =
        std::ranges::find_if(wanted, [&taken_names](const std::string& each) { return NameTaken(taken_names, each); });
    if (source.refused)
    {
        view.error = *source.refused;
    }
    else if (!HoldsNames(source))
    {
        view.error = edit::NoNamesReason("bank");
    }
    else if (view.names && name.empty())
    {
        view.error = NoNameRefusal(banks == 1 ? "bank" : "banks");
    }
    else if (taken != wanted.end())
    {
        view.error = NameInUse(*taken);
    }
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return view;
    }

    // What the banks leave out, and where they go.
    AddSoundFontWarnings(view, source, import, names, first_bank);

    return view;
}

namespace
{

// The number of wave sounds in a wave sound file, `bytes`. For a file that Hokuto can't read, returns 0 and sets the
// reason in `unread`.
std::size_t CountWaveSounds(std::span<const uint8_t> bytes, ntdlib::sar::Flavor flavor, std::string& unread)
{
    std::size_t sounds = 0;
    try
    {
        sounds = edit::WaveSoundCount(bytes, flavor);
    }
    catch (const std::invalid_argument& error)
    {
        unread = error.what();
    }

    return sounds;
}

// The groups of the import: those that load the sound that new wave sounds copy, and the new file first, then the
// wave archives that the rows choose (`targets`).
void FillItemImportGroups(ReplaceView& view, const ReplaceSource& source, const edit::IncomingFile& incoming,
                          const std::vector<int>& targets, bool bank, bool wave_sounds, std::size_t sounds)
{
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    std::vector<uint32_t> new_files{static_cast<uint32_t>(archive.FileCount())};
    for (const int target : targets)
    {
        const auto index = static_cast<std::size_t>(target);
        if (target >= 0 && index < archive.WaveArchives().size() &&
            std::ranges::find(new_files, archive.WaveArchives()[index].file_id) == new_files.end())
        {
            new_files.push_back(archive.WaveArchives()[index].file_id);
        }
    }
    const std::map<uint32_t, std::size_t> new_sizes = {{new_files.front(), incoming.bytes.size()}};
    if (wave_sounds && source.sound < archive.Sounds().size())
    {
        const bool one_sound = sounds == 1;
        FillGroups(view, source, edit::FilesOfItem(archive, ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, source.sound)),
                   new_files,
                   "Proposed: groups that load " + archive.Sounds()[source.sound].name +
                       (one_sound ? ". The new sound uses its settings." : ". The new sounds use its settings."),
                   one_sound ? "No group loads the new sound. If the game loads sound files through groups, it won't "
                               "load this sound."
                             : "No group loads the new sounds. If the game loads sound files through groups, it won't "
                               "load them.",
                   new_sizes);
    }
    else
    {
        const std::string item = bank ? "bank" : "wave archive";
        FillGroups(view, source, {}, new_files, "",
                   "No group loads the new " + item +
                       ". None is proposed because no sound uses it yet. If the game loads files through groups, it "
                       "won't load the new " +
                       item + ".",
                   new_sizes);
    }
}

// Why the import can't go in, or an empty string: the names of the new items `names`, the rows and the archive's
// wave archives. The import makes the same checks.
std::string ItemImportError(const ReplaceView& view, const ReplaceSource& source, const std::vector<std::string>& names,
                            const std::string& name, bool bank, bool wave_sounds, std::size_t sounds)
{
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    const format::NativeTables& tables = *source.file->native->tables;
    const auto unchosen = [](const WaveArchiveRow& row)
    {
        return row.target < 0;
    };
    const std::vector<std::string> taken_names = TakenNames(tables, &archive);
    const auto taken = [&taken_names](const std::string& each)
    {
        return !each.empty() && NameTaken(taken_names, each);
    };
    const auto first_taken = std::ranges::find_if(names, taken);
    const std::string file_kind = bank ? "bank" : "wave sound file";
    if (!HoldsNames(source) && (bank || wave_sounds || !name.empty()))
    {
        return edit::NoNamesReason(bank ? "bank" : wave_sounds ? "sound" : "wave archive");
    }
    if (wave_sounds && sounds == 0)
    {
        return "The file contains no wave sounds.";
    }
    if ((bank || wave_sounds) && name.empty())
    {
        return NoNameRefusal(NewItemsWord(bank, sounds));
    }
    if (first_taken != names.end())
    {
        return NameInUse(*first_taken);
    }
    if (std::ranges::any_of(view.wave_archive_rows, unchosen) && archive.WaveArchives().empty())
    {
        return "The archive has no wave archives. Import one first with File > Import > Wave Archive…";
    }
    if (std::ranges::any_of(view.wave_archive_rows, unchosen))
    {
        return "Choose a wave archive for each wave archive the " + file_kind + " uses.";
    }

    return "";
}

// The warnings of the waves missing from the chosen wave archives, with `counts` the waves of each wave archive.
void WarnLackedWaves(ReplaceView& view, const ntdlib::sar::SoundArchive& archive,
                     const std::vector<edit::NamedWaveArchive>& named,
                     const std::vector<std::optional<std::size_t>>& counts, bool bank)
{
    const std::string player = bank ? "The bank" : "The file";
    for (std::size_t k = 0; k < named.size(); k++)
    {
        const auto target = static_cast<std::size_t>(view.wave_archive_rows[k].target);
        const std::string& target_name = archive.WaveArchives()[target].name;
        if (!counts[target])
        {
            view.warnings.push_back("Can't read " + target_name + ". Notes using its waves may be silent.");
            continue;
        }

        std::vector<uint32_t> lacked;
        for (const uint32_t wave : named[k].waves)
        {
            if (wave >= *counts[target])
            {
                lacked.push_back(wave);
            }
        }
        if (!lacked.empty())
        {
            view.warnings.push_back(target_name + " has only " + WavesText(*counts[target]) + ". " + player +
                                    " also uses " + (lacked.size() == 1 ? "wave " : "waves ") + NumberList(lacked) +
                                    " from it. " + (lacked.size() == 1 ? "Notes using it" : "Notes using them") +
                                    " will be silent.");
        }
    }
}

// The warnings about the number of the new item: those of the bank, those of the wave sounds with `names`, or those of
// the wave archive.
void AddItemNumberWarnings(ReplaceView& view, const ReplaceSource& source, const edit::IncomingFile& incoming,
                           const std::vector<std::string>& names, bool bank, bool wave_sounds, std::size_t sounds)
{
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    if (bank)
    {
        view.warnings.push_back("The new bank is number " + std::to_string(archive.Banks().size()) +
                                Placed(archive.Banks().size(), "banks") +
                                ". It plays once a bank slot of a sound uses it. File > Import > Sequence… sets the "
                                "banks for a new sound.");
    }
    else if (wave_sounds)
    {
        view.warnings.push_back(NewSoundsNumberWarning(names, archive.Sounds().size(), sounds));
        if (source.sound < archive.Sounds().size() &&
            archive.Sounds()[source.sound].type == ntdlib::sar::SoundType::kSequence)
        {
            view.warnings.push_back(archive.Sounds()[source.sound].name + " is a sequence sound. " +
                                    (sounds == 1 ? "The new sound uses" : "The new sounds use") +
                                    " its player, volume, pan, priorities and 3D settings.");
        }
    }
    else
    {
        const std::size_t waves = ntdlib::sar::WaveArchive::Parse(incoming.bytes, archive.ReadOptions()).waves.size();
        view.warnings.push_back("The new wave archive has " + WavesText(waves) + " and is number " +
                                std::to_string(archive.WaveArchives().size()) +
                                Placed(archive.WaveArchives().size(), "wave archives") +
                                ". To play its waves, import a bank that uses them with File > Import > Bank…");
        view.warnings.push_back(
            "Wave sound files can play the new waves too. Import one with File > Import > Wave Sound File…");
    }
}

// The head of the view of an import of a bank, a wave sound file or a wave archive (`incoming`) under `name`: the
// title, the source, the name and the sounds to copy.
ReplaceView ItemImportHead(const ReplaceSource& source, const edit::IncomingFile& incoming, const std::string& name)
{
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    const bool bank = incoming.kind == edit::FileKind::kBank;
    const bool wave_sounds = incoming.kind == edit::FileKind::kWaveSoundFile;
    ReplaceView view;
    view.title = ItemImportTitle(source, bank, wave_sounds);
    view.action = source.new_bank ? "Add" : "Import";
    view.import = true;
    view.playable = bank || wave_sounds;
    if (incoming.kind == edit::FileKind::kWaveArchive)
    {
        AddWaves(view, [&incoming] { return ntdlib::sar::WaveArchiveFile::Read(incoming.bytes).waves; });
    }
    view.sequence = false;
    view.heading = bank ? "Bank file" : wave_sounds ? "Wave sound file" : "Wave archive file";
    view.source =
        source.new_bank
            ? "An empty bank. It opens in the Bank Editor. Drag WAV files onto the program list to add instruments."
            : IncomingSource(source.file_name, wave_sounds ? "wave sound file" : edit::NameOf(incoming.kind) + " file",
                             incoming, flavor);
    FillImportModels(view, source, archive, name);
    view.name_hint = bank || wave_sounds ? "" : "No name";

    return view;
}

} // namespace

ReplaceView MakeItemImportView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                               const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                               const std::string& name)
{
    // The file, the name and the sounds to copy. A wave archive can go in without a name. Its waves play one at a time.
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    const bool bank = incoming.kind == edit::FileKind::kBank;
    const bool wave_sounds = incoming.kind == edit::FileKind::kWaveSoundFile;
    ReplaceView view = ItemImportHead(source, incoming, name);

    // The wave sounds of a wave sound file. For a file that Hokuto can't read, the window gets the error once the rows
    // are made.
    std::size_t sounds = 0;
    std::string unread;
    if (wave_sounds)
    {
        sounds = CountWaveSounds(incoming.bytes, flavor, unread);
    }

    // The groups that load the sound that new wave sounds copy. No sound plays a new bank or wave archive yet. The new
    // file comes first, then the wave archives that the rows choose.
    FillItemImportGroups(view, source, incoming, targets, bank, wave_sounds, sounds);

    // The archive's wave archives with their waves, or nothing for a wave archive that Hokuto can't read.
    const std::vector<std::optional<std::size_t>> counts = WaveArchiveMenu(archive, view.wave_archives);

    // The rows of a bank or a wave sound file.
    view.wave_archive_rows = WaveArchiveRows(named, targets);

    // The names of the new sounds.
    if (!unread.empty())
    {
        view.error = unread;
        return view;
    }
    const std::vector<std::string> names =
        wave_sounds ? edit::WaveSoundNames(name, sounds) : std::vector<std::string>{name};

    // The name and the rows. The import makes the same checks.
    view.error = ItemImportError(view, source, names, name, bank, wave_sounds, sounds);
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return view;
    }

    // The waves that the chosen wave archives lack.
    WarnLackedWaves(view, archive, named, counts, bank);

    // A wave sound file that no wave sound of the archive plays may come from another game. Its waves play from the
    // wave archives that the rows choose in this archive.
    if (wave_sounds && !named.empty() && !PlaysWaveSoundFile(archive, incoming.bytes))
    {
        view.warnings.push_back(
            "The file's waves come from the wave archives chosen here. A file from another game "
            "plays the wrong waves, or none, until you import that game's wave archive with File > "
            "Import > Wave Archive… and choose it.");
    }

    // The item's number and the size.
    AddItemNumberWarnings(view, source, incoming, names, bank, wave_sounds, sounds);
    if (const std::optional<std::string> warning = ConversionWarning(incoming, flavor))
    {
        view.warnings.push_back(*warning);
    }
    view.warnings.push_back("The new file takes up " + SizeText(incoming.bytes.size()) + ".");

    return view;
}

std::vector<int> FirstWaveArchives(const ntdlib::sar::SoundArchive& archive, std::span<const uint8_t> file,
                                   const std::vector<edit::NamedWaveArchive>& named,
                                   const std::vector<edit::NamedWaveArchive>& present)
{
    // A bank or a wave sound of the archive with the same file names the archive's wave archives. So does a file whose
    // wave archives the present file names too.
    const std::size_t count = archive.WaveArchives().size();
    const auto same_bank = [&archive, &file](const ntdlib::sar::BankInfo& bank)
    {
        return std::ranges::equal(archive.FileData(bank.file_id), file);
    };
    const auto in_archive = [count](const edit::NamedWaveArchive& wave_archive)
    {
        return ntdlib::sar::ItemType(wave_archive.id) == ntdlib::sar::kWaveArchiveItem &&
               ntdlib::sar::ItemNumber(wave_archive.id) < count;
    };
    const auto in_present = [&present](const edit::NamedWaveArchive& wave_archive)
    {
        return std::ranges::find(present, wave_archive.id, &edit::NamedWaveArchive::id) != present.end();
    };
    std::vector<int> targets;
    const bool same = std::ranges::any_of(archive.Banks(), same_bank) || PlaysWaveSoundFile(archive, file) ||
                      (!present.empty() && std::ranges::all_of(named, in_present));
    if (same && std::ranges::all_of(named, in_archive))
    {
        for (const edit::NamedWaveArchive& wave_archive : named)
        {
            targets.push_back(static_cast<int>(ntdlib::sar::ItemNumber(wave_archive.id)));
        }
        return targets;
    }

    // Otherwise the archive's last wave archives, in order.
    for (std::size_t k = 0; k < named.size(); k++)
    {
        const std::size_t first = count > named.size() ? count - named.size() : 0;
        targets.push_back(count == 0 ? -1 : static_cast<int>(std::min(first + k, count - 1)));
    }

    return targets;
}

std::string ItemName(const ntdlib::sar::SoundArchive& archive, edit::FileKind kind, uint32_t item)
{
    switch (kind)
    {
    case edit::FileKind::kBank:
        return archive.Banks()[item].name;
    case edit::FileKind::kWaveArchive:
        return archive.WaveArchives()[item].name;
    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return archive.Sounds()[item].name;
}

namespace
{

// ReplacedName for either kind of archive.
template <class Archive>
std::string ReplacedNameOf(const Archive& archive, edit::FileKind kind, uint32_t item)
{
    if (kind == edit::FileKind::kWaveSoundFile)
    {
        try
        {
            return song::ItemName(archive, {.kind = song::ItemKind::kWaveSoundFile, .index = item});
        }
        catch (const std::runtime_error&)
        {
        }
    }

    return ItemName(archive, kind, item);
}

} // namespace

std::string ReplacedName(const ntdlib::sar::SoundArchive& archive, edit::FileKind kind, uint32_t item)
{
    return ReplacedNameOf(archive, kind, item);
}

std::string ReplacedName(const ntdlib::rsar::SoundArchive& archive, edit::FileKind kind, uint32_t item)
{
    return ReplacedNameOf(archive, kind, item);
}

uint32_t ReplacedItem(edit::FileKind kind, uint32_t item)
{
    switch (kind)
    {
    case edit::FileKind::kBank:
        return ntdlib::sar::ItemId(ntdlib::sar::kBankItem, item);
    case edit::FileKind::kWaveArchive:
        return ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, item);
    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, item);
}

namespace
{

// The sounds that Play offers: those that play the item, the replaced wave sound first.
void FillPlaySounds(ReplaceView& view, const ntdlib::sar::SoundArchive& archive, const edit::IncomingFile& incoming,
                    uint32_t sound)
{
    view.playable = true;
    view.play_sounds = edit::SoundsPlaying(archive, incoming.kind, sound);
    if (incoming.kind == edit::FileKind::kWaveSoundFile)
    {
        std::erase(view.play_sounds, sound);
        view.play_sounds.insert(view.play_sounds.begin(), sound);
    }
    for (const uint32_t sound : view.play_sounds)
    {
        view.plays.push_back(archive.Sounds()[sound].name);
    }
}

// The warnings of the copies of the current file in groups that contain other bytes.
void WarnCopies(ReplaceView& view, const edit::FileChanges& changes)
{
    if (changes.placed_copies > 0)
    {
        const bool one = changes.placed_copies == 1;
        view.warnings.push_back((one ? std::string("A copy") : std::to_string(changes.placed_copies) + " copies") +
                                " of the current file in " + (one ? "a group uses" : "groups use") +
                                " different wave indices. " + (one ? "That copy gets" : "Those copies get") +
                                " the new file with the same indices.");
        if (changes.unplaced_waves > 0)
        {
            const bool single = changes.unplaced_waves == 1;
            const std::string count = single ? "one wave" : std::to_string(changes.unplaced_waves) + " waves";
            const std::string them = single ? "it" : "them";
            view.warnings.push_back("The current file doesn't use " + count + " of the new file. " +
                                    (one ? "That copy uses " : "Those copies use ") + "the archive " +
                                    (single ? "index for it. " : "indices for them. ") +
                                    (one ? "Its group" : "Their groups") + " may not load " + them + ".");
        }
    }
    if (changes.other_copies > 0)
    {
        const bool one = changes.other_copies == 1;
        view.warnings.push_back((one ? std::string("A copy") : std::to_string(changes.other_copies) + " copies") +
                                " of the current file in " + (one ? "a group differs" : "groups differ") +
                                " from it. The new file replaces " + (one ? "that copy" : "those copies") + " too.");
    }
}

// The warnings of the waves that the new file names and that are missing from the archive.
void WarnFileLackedWaves(ReplaceView& view, const ntdlib::sar::SoundArchive& archive, const edit::FileChanges& changes,
                         const std::string& kind)
{
    for (const edit::LackedWaves& lacked : changes.lacked)
    {
        const bool numbered = lacked.wave_archive < archive.WaveArchives().size();
        const std::string wave_archive = numbered ? archive.WaveArchives()[lacked.wave_archive].name
                                                  : "wave archive " + std::to_string(lacked.wave_archive);
        std::string text = "The new " + kind + " uses ";
        if (!numbered || lacked.whole)
        {
            text += "waves from " + wave_archive + ". Notes using them will be silent. ";
            text += numbered ? "Can't read that wave archive." : "The archive has no such wave archive.";
        }
        else
        {
            text += CountVerb(lacked.waves, "wave ", "waves ") + NumberList(lacked.waves) + " from " + wave_archive +
                    ". " + wave_archive + " has only " + Grouped(lacked.count) +
                    (lacked.count == 1 ? " wave." : " waves.") + " Notes using " +
                    CountVerb(lacked.waves, "it", "them") + " will be silent.";
        }
        view.warnings.push_back(text);
    }
}

// The warnings of the conversion, the version and the size of the new file.
void AddFileWarnings(ReplaceView& view, const ntdlib::sar::SoundArchive& archive, const edit::IncomingFile& incoming,
                     const edit::FileChanges& changes)
{
    if (const std::optional<std::string> warning = ConversionWarning(incoming, archive.ReadOptions().flavor))
    {
        view.warnings.push_back(*warning);
    }
    if (changes.version && *changes.version != incoming.version)
    {
        const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
        view.warnings.push_back("The new file is version " + song::VersionText(flavor, incoming.version) +
                                ". The current file is version " + song::VersionText(flavor, *changes.version) +
                                ". A game may reject a version its sound library doesn't support.");
    }
    WarnFileSizes(view, changes, incoming);
}

} // namespace

ReplaceView MakeFileReplaceView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                const edit::FileChanges& changes, std::optional<format::File> result, std::string error,
                                const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                                std::span<const edit::ProgramFix> fixes)
{
    // The item, and the file that goes in its place.
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    const std::string kind = edit::NameOf(incoming.kind);
    const std::string file = incoming.kind == edit::FileKind::kWaveSoundFile ? kind : kind + " file";
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + ReplacedName(archive, incoming.kind, source.sound);
    view.heading = file;
    view.heading[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(view.heading[0])));
    view.source = IncomingSource(source.file_name, file, incoming, archive.ReadOptions().flavor);
    view.result = std::move(result);
    view.error = std::move(error);

    // Play plays a sound that plays the item: the replaced wave sound first.
    FillPlaySounds(view, archive, incoming, source.sound);

    // The rows of a bank or a wave sound file. The replacement waits for a wave archive in each.
    if (!named.empty())
    {
        WaveArchiveMenu(archive, view.wave_archives);
        view.wave_archive_rows = WaveArchiveRows(named, targets);
        const auto unchosen = [](const WaveArchiveRow& row)
        {
            return row.target < 0;
        };
        if (std::ranges::any_of(view.wave_archive_rows, unchosen))
        {
            view.result.reset();
            view.error = archive.WaveArchives().empty()
                             ? std::string(
                                   "The archive has no wave archives. Import one first with File > Import > "
                                   "Wave Archive…")
                             : "Choose a wave archive for each wave archive the " + kind + " uses.";
            return view;
        }
    }

    // The groups that load the item and lack a file that the new file requires.
    const uint32_t item = ReplacedItem(incoming.kind, source.sound);
    const std::string name = ReplacedName(archive, incoming.kind, source.sound);
    const std::vector<uint32_t> after = edit::FilesOfItem(archive, item, {.file = incoming.bytes});
    FillGroups(view, source, edit::FilesOfItem(archive, item), after,
               "Proposed: groups that load " + name + " but are missing a wave archive the new file uses.",
               "No group loads " + name + ". If the game loads files through groups, " + name + " won't load.",
               after.empty() ? std::map<uint32_t, std::size_t>{}
                             : std::map<uint32_t, std::size_t>{{after.front(), incoming.bytes.size()}});

    // The other items whose file it is. The window names a wave sound file by its sound group, and the warning names
    // each of its wave sounds.
    WarnSharing(view, archive, source, incoming, changes, kind);

    // The copies in groups that hold other bytes. A copy that names the present file's waves at other places takes the
    // new file with those places.
    WarnCopies(view, changes);

    // The waves that the new file names and the archive lacks. Each warning gives the waves, the silent notes and why.
    WarnFileLackedWaves(view, archive, changes, kind);

    // A bank's programs.
    if (incoming.kind == edit::FileKind::kBank)
    {
        AddProgramFixes(source, incoming, changes, fixes, view);
    }

    // What plays waves or wave sounds past the new file's end. For a wave archive, the banks come first, then the wave
    // sounds.
    WarnPastEnd(view, archive, incoming, changes);

    // The conversion, the version and the size.
    AddFileWarnings(view, archive, incoming, changes);

    return view;
}

} // namespace hokuto::app
