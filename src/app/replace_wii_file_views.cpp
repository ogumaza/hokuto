// SPDX-License-Identifier: MIT

// The views of a file of the Wii that replaces an item or comes in as new items: the item import and the replacement of
// a file, with their wave data and warnings.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"

#include "app/export_warnings.h"
#include "app/item_views.h"
#include "app/replace_common.h"
#include "app/replace_file_views.h"
#include "app/replace_file_views_parts.h"
#include "app/replace_view.h"
#include "edit/banks.h"
#include "edit/files.h"
#include "edit/new_items.h"
#include "edit/other_console.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/items.h"

namespace hokuto::app
{
namespace
{

using replace_common::AddWiiGroupWarning;
using replace_common::FillImportModels;
using replace_common::FillWiiGroups;
using replace_common::kWiiLastGroup;
using replace_common::NameInUse;
using replace_common::NameTaken;
using replace_common::NewItemsWord;
using replace_common::NoNameRefusal;
using replace_common::NumberList;
using replace_common::Placed;
using replace_common::Refused;
using replace_common::SizeGroups;
using replace_common::WavesText;
using replace_common::WiiFilesOfSound;
using replace_common::WiiTakenNames;
using replace_common::WiiVersionText;
using replace_common::WiiWaveDataMenu;
using replace_file_parts::AddWaves;
using replace_file_parts::CountVerb;
using replace_file_parts::ItemImportTitle;
using replace_file_parts::NewSoundsNumberWarning;
using replace_file_parts::WarnFileSizes;
using replace_file_parts::WarnPastEnd;
using replace_file_parts::WarnSharing;
using replace_file_parts::WaveArchiveRows;

// The line of a bank file, a wave archive file or a wave sound file that goes into an archive of the Wii: the name of
// the file that was read, what it is, and its console, its version and its size.
std::string WiiIncomingSource(const ReplaceSource& source, const edit::IncomingFile& incoming)
{
    const std::string kind = edit::NameOf(incoming.kind);
    const std::string file = incoming.kind == edit::FileKind::kWaveSoundFile ? kind : kind + " file";
    const std::optional<edit::ConvertedFile>& converted = incoming.converted;
    if (converted && !converted->wii)
    {
        return source.file_name + ": " + song::ConsoleName(converted->flavor) + " " + file + ", version " +
               song::VersionText(converted->flavor, converted->version) + ", " + SizeText(converted->size);
    }
    if (source.later)
    {
        return source.file_name + ": " + song::ConsoleName(*source.later) + " " + file + ", version " +
               song::VersionText(*source.later, incoming.version) + ", " + SizeText(incoming.bytes.size());
    }

    return source.file_name + ": Wii " + file + ", version " +
           WiiVersionText(converted ? converted->version : incoming.version) + ", " +
           SizeText(converted ? converted->size : incoming.bytes.size());
}

// The warnings about the waves that the wave data of `targets` lacks for a file of another console, by the rows of
// `named` (edit::WiiFileOf). `counts` gives the waves of each wave data of the menu.
void AddLackedWiiWaves(const ReplaceSource& source, const edit::IncomingFile& incoming,
                       const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                       const std::vector<std::optional<std::size_t>>& counts, std::vector<std::string>& warnings)
{
    const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(*source.played.Wii());
    const std::string player = incoming.kind == edit::FileKind::kBank ? "The bank" : "The file";
    for (std::size_t k = 0; k < named.size() && k < targets.size(); k++)
    {
        const auto target = static_cast<std::size_t>(targets[k]);
        if (targets[k] < 0 || target >= wave_data.size() || target >= counts.size() || !counts[target])
        {
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
            warnings.push_back(wave_data[target].name + " has only " + WavesText(*counts[target]) + ". " + player +
                               " also uses " + (lacked.size() == 1 ? "wave " : "waves ") + NumberList(lacked) +
                               " from wave archive " + std::to_string(ntdlib::sar::ItemNumber(named[k].id)) + ". " +
                               (lacked.size() == 1 ? "Notes using it" : "Notes using them") + " will be silent.");
        }
    }
}

// The number of wave sounds in the incoming wave sound file. For a file that Hokuto can't read, returns 0 and sets the
// reason in `unread`.
std::size_t CountWiiWaveSounds(const ReplaceSource& source, const edit::IncomingFile& incoming, std::string& unread)
{
    std::size_t sounds = 0;
    try
    {
        sounds =
            source.later
                ? edit::WaveSoundCount(incoming.bytes, *source.later)
                : ntdlib::rsar::WaveSoundFile::Parse(incoming.bytes, ntdlib::rvl::snd::kRsarOptions).Sounds().size();
    }
    catch (const std::exception& error)
    {
        unread = std::string("Can't read the wave sound file. ") + format::AsSentence(error.what());
    }

    return sounds;
}

// The groups with names that load the sound that new wave sounds copy, with `new_files` the new file.
void FillWiiItemGroups(ReplaceView& view, const ReplaceSource& source, const std::vector<uint32_t>& new_files,
                       bool wave_sounds, std::size_t sounds)
{
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    if (wave_sounds && source.sound < archive.Sounds().size())
    {
        const bool one_sound = sounds == 1;
        FillWiiGroups(view, source, WiiFilesOfSound(archive, source.sound), new_files,
                      "Proposed: groups that load " + archive.Sounds()[source.sound].name +
                          (one_sound ? ". The new sound uses its settings." : ". The new sounds use its settings."),
                      std::string(one_sound ? "No named group loads the new sound. If the game loads sound files "
                                              "through named groups, it won't load this sound. "
                                            : "No named group loads the new sounds. If the game loads sound files "
                                              "through named groups, it won't load them. ") +
                          kWiiLastGroup);
    }
    else
    {
        FillWiiGroups(view, source, {}, new_files, "",
                      std::string("No named group loads the new bank. None is proposed because no sound uses it "
                                  "yet. ") +
                          kWiiLastGroup);
    }
}

// The rows of the wave data. Returns the waves of each wave data that the menu lists.
std::vector<std::optional<std::size_t>> FillWiiWaveDataRows(ReplaceView& view, const ReplaceSource& source,
                                                            const std::vector<edit::NamedWaveArchive>& named,
                                                            const std::vector<int>& targets, bool bank)
{
    std::vector<std::optional<std::size_t>> counts;
    view.rows_heading = "Wave data";
    counts = WiiWaveDataMenu(source, !source.later, view.wave_archives);
    if (source.later)
    {
        view.wave_archive_rows = WaveArchiveRows(named, targets);
    }
    else
    {
        WaveArchiveRow& row = view.wave_archive_rows.emplace_back();
        row.label = bank ? "Bank waves" : "File waves";
        row.target = targets.empty() ? -1 : targets.front();
    }

    return counts;
}

// Why the import can't go in, or an empty string. `unchosen` says whether a row has no wave data, and `failure` why
// the file can't be made. The import makes the same checks.
std::string WiiItemImportError(const ReplaceView& view, const ReplaceSource& source,
                               const std::vector<std::string>& names, const std::string& name, bool unchosen,
                               const std::string& failure, bool bank, bool wave_sounds, std::size_t sounds)
{
    const std::vector<std::string> taken_names = WiiTakenNames(source);
    const auto taken = [&taken_names](const std::string& each)
    {
        return NameTaken(taken_names, each);
    };
    const auto first_taken = std::ranges::find_if(names, taken);
    const std::string file_kind = bank ? "bank" : "wave sound file";
    if (wave_sounds && sounds == 0)
    {
        return "The file contains no wave sounds.";
    }
    if (name.empty())
    {
        return NoNameRefusal(NewItemsWord(!wave_sounds, sounds));
    }
    if (first_taken != names.end())
    {
        return NameInUse(*first_taken);
    }
    if (unchosen && view.wave_archives.empty())
    {
        return "The archive has no wave data. File > Import > Wave Archive… imports waves as a new bank.";
    }
    if (unchosen)
    {
        return source.later ? "Choose wave data for each wave archive the " + file_kind + " uses."
                            : "Choose wave data for the " + file_kind + ".";
    }
    if (!failure.empty())
    {
        return failure;
    }

    return "";
}

// The warnings of the wave data and of the conversion of the `imported` file.
void AddWiiWaveDataWarnings(ReplaceView& view, const ReplaceSource& source, const edit::IncomingFile& incoming,
                            const edit::IncomingFile& imported, bool wave_sounds)
{
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    if (incoming.kind == edit::FileKind::kWaveArchive)
    {
        std::size_t waves = 0;
        try
        {
            waves = ntdlib::rsar::WaveArchiveFile::Read(incoming.bytes).waves.size();
        }
        catch (const std::exception&)
        {
            // WiiImportedFile read the waves already.
        }
        view.warnings.push_back(
            "On the Wii, waves are stored in the wave data of a bank or a wave sound file. " +
            (waves == 1 ? std::string("The wave becomes") : "The " + WavesText(waves) + " become") +
            " the wave data of a new bank with no instruments. Add regions for them in the Bank Editor, or play them "
            "through another bank imported with File > Import > Bank…");
    }
    else if (!source.new_bank && !source.later && !view.wave_archive_rows.empty())
    {
        const auto target = static_cast<std::size_t>(view.wave_archive_rows.front().target);
        const bool beside = !source.beside.empty() && target == 0;
        view.warnings.push_back(beside
                                    ? "The new file uses the waves of " + source.beside_name + " as its wave data."
                                    : "The new file uses a copy of the waves of " +
                                          song::WaveArchivesOf(archive)[target - (source.beside.empty() ? 0 : 1)].name +
                                          " as its wave data.");
    }
    if (source.later && imported.converted)
    {
        view.warnings.push_back("This is a " + song::ConsoleName(*source.later) + " file. It's converted to a Wii " +
                                edit::NameOf(incoming.kind) + (wave_sounds ? "" : " file") + " (version " +
                                WiiVersionText(imported.version) + ") with new wave data for the waves it uses.");
    }
    else if (imported.converted && imported.converted->wii)
    {
        view.warnings.push_back("The file is saved as version " + WiiVersionText(imported.version) +
                                " to match the other files of this kind in the archive. No values change. A game's "
                                "NW4R SND build may reject a later version.");
    }
    else if (imported.converted)
    {
        view.warnings.push_back("This is a " + song::ConsoleName(imported.converted->flavor) +
                                " file. Its waves are converted to Wii waves with the same samples.");
    }
}

// The warnings of a file of the Wii without wave data. Such a file uses the current wave data.
void WarnCurrentWaveData(ReplaceView& view, const ReplaceSource& source, const edit::IncomingFile& incoming,
                         const edit::FileChanges& changes, const std::string& kind)
{
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    if (!source.later && incoming.wave_data.empty() && changes.wave_block)
    {
        const std::string player = incoming.kind == edit::FileKind::kBank ? "region" : "note";
        view.warnings.push_back("The new " + kind + " uses the current wave data. " +
                                (*changes.wave_block ? "Its WAVE block gives the place of each wave in those samples."
                                                     : "Each " + player + " plays the wave at the same index in it."));
        if (!source.beside_name.empty())
        {
            const uint32_t id = edit::FileIdOf(archive, incoming.kind, source.sound);
            view.warnings.push_back(source.beside_name +
                                    " next to the file isn't used. To play its waves, use Replace… on " +
                                    ItemName(archive, edit::FileKind::kWaveArchive, id) + " in Waves afterwards.");
        }
    }
}

// The warnings of the waves that the new file names and that are missing from the wave data beside it.
void WarnWiiLackedWaves(ReplaceView& view, const edit::FileChanges& changes, const std::string& kind)
{
    for (const edit::LackedWaves& lacked : changes.lacked)
    {
        if (lacked.whole)
        {
            view.warnings.push_back("Can't read the waves of the new " + kind +
                                    " from the current wave data. Notes "
                                    "using them may be silent until you use Replace… on the wave data to give the " +
                                    kind + " its waves (for example, an RWAR written by Export…).");
        }
        else
        {
            view.warnings.push_back("The new " + kind + " uses " + CountVerb(lacked.waves, "wave ", "waves ") +
                                    NumberList(lacked.waves) + ", but the current wave data has only " +
                                    Grouped(lacked.count) + (lacked.count == 1 ? " wave" : " waves") +
                                    ". Notes using " + CountVerb(lacked.waves, "it", "them") + " will be silent.");
        }
    }
}

// The warnings of the conversion of a file of another console, and of the waves missing from its wave data.
void WarnWiiConversion(ReplaceView& view, const ReplaceSource& source, const edit::IncomingFile& incoming,
                       const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                       const std::vector<std::optional<std::size_t>>& counts, const std::string& file)
{
    if (source.later && incoming.converted)
    {
        view.warnings.push_back("This is a " + song::ConsoleName(*source.later) + " file. It's converted to a Wii " +
                                file + " (version " + WiiVersionText(incoming.version) +
                                ") with new wave data for the waves it uses.");
        AddLackedWiiWaves(source, incoming, named, targets, counts, view.warnings);
        view.warnings.push_back(
            "The new wave data replaces the current wave data. Each region or note plays the wave at the same index "
            "in the chosen wave data.");
    }
}

} // namespace

edit::IncomingFile WiiImportedFile(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                   const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets)
{
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(archive);
    const std::string kind = edit::NameOf(incoming.kind);
    if (source.new_bank)
    {
        return incoming;
    }

    // A wave archive's waves go in as the wave data of a new bank without instruments.
    if (incoming.kind == edit::FileKind::kWaveArchive)
    {
        std::vector<ntdlib::rsar::WaveFile> waves;
        try
        {
            for (const std::vector<uint8_t>& wave : ntdlib::rsar::WaveArchiveFile::Read(incoming.bytes).waves)
            {
                waves.push_back(ntdlib::rsar::WaveFile::Read(wave));
            }
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument(std::string("Can't read the waves in the wave archive file. ") +
                                        format::AsSentence(error.what()));
        }
        ntdlib::rsar::BankFile::Files files = edit::EmptyBank(archive, std::move(waves));
        return {edit::FileKind::kBank, edit::NewWiiFileVersion(archive, edit::FileKind::kBank), std::move(files.file),
                incoming.converted, std::move(files.wave_data)};
    }

    // A file of another console takes the wave data chosen in each row.
    if (source.later)
    {
        std::map<uint32_t, uint32_t> map;
        for (std::size_t k = 0; k < named.size(); k++)
        {
            const int target = k < targets.size() ? targets[k] : -1;
            if (target < 0 || static_cast<std::size_t>(target) >= wave_data.size())
            {
                throw std::invalid_argument("Choose wave data for each wave archive the " + kind + " uses.");
            }
            map[named[k].id] = wave_data[static_cast<std::size_t>(target)].file_id;
        }
        return edit::WiiFileOf(incoming, *source.later, archive, map, edit::NewWiiFileVersion(archive, incoming.kind));
    }

    // A file of the Wii takes the RWAR beside it, or a copy of the waves of a file of the archive.
    const int target = targets.empty() ? -1 : targets.front();
    const std::size_t beside = source.beside.empty() ? 0 : 1;
    if (target < 0 || static_cast<std::size_t>(target) >= wave_data.size() + beside)
    {
        throw std::invalid_argument("Choose wave data for the " + kind + ".");
    }
    const auto index = static_cast<std::size_t>(target);
    const edit::WiiWaveData data = index < beside ? edit::ReadWiiWaveData(source.beside)
                                                  : edit::WiiWaveDataOf(archive, wave_data[index - beside].file_id);
    return edit::WiiFileWithWaves(incoming, data, edit::NewWiiFileVersion(archive, incoming.kind));
}

std::vector<int> FirstWiiWaveData(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                  const std::vector<edit::NamedWaveArchive>& named, std::optional<uint32_t> present)
{
    if (source.new_bank || incoming.kind == edit::FileKind::kWaveArchive)
    {
        return {};
    }

    // The place of a file's wave data in the menus.
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(archive);
    const auto place_of = [&wave_data](uint32_t file_id)
    {
        const auto found = std::ranges::find(wave_data, file_id, &song::WiiWaveArchive::file_id);
        return found != wave_data.end() ? static_cast<int>(found - wave_data.begin()) : -1;
    };

    // A file of another console: the present file's wave data, or else the last bank's, or else the last in order. A
    // wave archive that comes in becomes the wave data of a new last bank.
    if (source.later)
    {
        int first = present ? place_of(*present) : -1;
        if (first < 0 && !archive.Banks().empty())
        {
            first = place_of(archive.Banks().back().file_id);
        }
        if (first < 0)
        {
            first = static_cast<int>(wave_data.size()) - 1;
        }
        return std::vector<int>(named.size(), first);
    }

    // A file of the Wii: the RWAR beside it, or else the wave data of a file of the archive with the same bytes.
    if (!source.beside.empty())
    {
        return {0};
    }
    for (std::size_t i = 0; i < wave_data.size(); i++)
    {
        if (std::ranges::equal(archive.FileData(wave_data[i].file_id), incoming.bytes))
        {
            return {static_cast<int>(i)};
        }
    }

    return {-1};
}

namespace
{

// The head of the view of an import of a bank, a wave sound file or a wave archive (`incoming`) under `name` on the
// Wii: the title, the source, the name and the sounds to copy.
ReplaceView WiiItemImportHead(const ReplaceSource& source, const edit::IncomingFile& incoming, const std::string& name)
{
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    const bool bank = incoming.kind == edit::FileKind::kBank;
    const bool wave_sounds = incoming.kind == edit::FileKind::kWaveSoundFile;
    ReplaceView view;
    view.title = ItemImportTitle(source, bank, wave_sounds);
    view.action = source.new_bank ? "Add" : "Import";
    view.import = true;
    view.sequence = false;
    view.heading = bank ? "Bank file" : wave_sounds ? "Wave sound file" : "Wave archive file";
    if (!source.new_bank && incoming.kind == edit::FileKind::kWaveArchive)
    {
        AddWaves(view, [&incoming] { return ntdlib::rsar::WaveArchiveFile::Read(incoming.bytes).waves; });
    }
    view.source = source.new_bank ? "An empty bank. It opens in the Bank Editor. Drag WAV files onto the program list "
                                    "to add instruments."
                                  : WiiIncomingSource(source, incoming);
    FillImportModels(view, source, archive, name);

    return view;
}

// Warns in `view` of the numbers of the new items: the `sounds` new wave sounds called `names` of a wave sound file
// (`wave_sounds`), or the new bank.
void AddWiiItemNumberWarnings(ReplaceView& view, const ReplaceSource& source, const std::vector<std::string>& names,
                              bool wave_sounds, std::size_t sounds)
{
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    if (wave_sounds)
    {
        view.warnings.push_back(NewSoundsNumberWarning(names, archive.Sounds().size(), sounds));
        if (source.sound < archive.Sounds().size() &&
            archive.Sounds()[source.sound].type == ntdlib::rsar::SoundType::kSequence)
        {
            view.warnings.push_back(archive.Sounds()[source.sound].name + " is a sequence sound. " +
                                    (sounds == 1 ? "The new sound uses" : "The new sounds use") +
                                    " its player, volume, priorities and 3D settings.");
        }
    }
    else
    {
        view.warnings.push_back("The new bank is number " + std::to_string(archive.Banks().size()) +
                                Placed(archive.Banks().size(), "banks") +
                                ". It plays once the bank slot of a sound uses it. File > Import > Sequence… sets the "
                                "bank for a new sound.");
    }
}

} // namespace

ReplaceView MakeWiiItemImportView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                  const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                                  const std::string& name)
{
    // The file, the name and the sounds to copy. A wave archive goes in as the wave data of a new bank. The new bank
    // takes a name.
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    const bool bank = incoming.kind == edit::FileKind::kBank;
    const bool wave_sounds = incoming.kind == edit::FileKind::kWaveSoundFile;
    ReplaceView view = WiiItemImportHead(source, incoming, name);

    // The wave sounds of a wave sound file. For a file that Hokuto can't read, the window gets the error once the rows
    // are made.
    std::size_t sounds = 0;
    std::string unread;
    if (wave_sounds)
    {
        sounds = CountWiiWaveSounds(source, incoming, unread);
    }

    // The groups with names that load the sound that new wave sounds copy. No sound plays a new bank yet. The new file
    // holds its wave data.
    const std::vector<uint32_t> new_files{archive.FileCount()};
    FillWiiItemGroups(view, source, new_files, wave_sounds, sounds);

    // The rows: the wave data of a bank or a wave sound file of the Wii, or wave data for each wave archive that a file
    // of another console names.
    std::vector<std::optional<std::size_t>> counts;
    if (!source.new_bank && incoming.kind != edit::FileKind::kWaveArchive)
    {
        counts = FillWiiWaveDataRows(view, source, named, targets, bank);
    }

    // The names of the new sounds.
    if (!unread.empty())
    {
        view.error = unread;
        return Refused(source, std::move(view));
    }
    const std::vector<std::string> names =
        wave_sounds ? edit::WaveSoundNames(name, sounds) : std::vector<std::string>{name};

    // The file that goes in, with its wave data.
    const bool unchosen =
        std::ranges::any_of(view.wave_archive_rows, [](const WaveArchiveRow& row) { return row.target < 0; });
    edit::IncomingFile imported;
    std::string failure;
    if (!unchosen)
    {
        try
        {
            imported = WiiImportedFile(source, incoming, named, ChosenWaveArchives(view));
        }
        catch (const std::invalid_argument& error)
        {
            failure = error.what();
        }
    }

    // The name and the rows. The import makes the same checks.
    view.error = WiiItemImportError(view, source, names, name, unchosen, failure, bank, wave_sounds, sounds);
    view.ready = view.error.empty();
    if (!view.ready)
    {
        return Refused(source, std::move(view));
    }

    // The waves that the chosen wave data lacks.
    if (source.later)
    {
        AddLackedWiiWaves(source, incoming, named, ChosenWaveArchives(view), counts, view.warnings);
    }

    // A wave sound file of another console comes from another game. Its waves play from the wave data that the rows
    // choose in this archive.
    if (wave_sounds && source.later && !named.empty())
    {
        view.warnings.push_back(
            "The file's waves come from the wave data chosen here. A file from another game plays "
            "the wrong waves, or none, until you import that game's wave archive with File > "
            "Import > Wave Archive… and choose its wave data.");
    }

    // The item's number.
    AddWiiItemNumberWarnings(view, source, names, wave_sounds, sounds);

    // The wave data, the conversion and the sizes.
    AddWiiWaveDataWarnings(view, source, incoming, imported, wave_sounds);
    view.warnings.push_back("The new file takes up " + SizeText(imported.bytes.size()) + ". Its wave data takes up " +
                            SizeText(imported.wave_data.size()) + ".");
    AddWiiGroupWarning(view);

    const auto padded = [](std::size_t bytes)
    {
        return (bytes + 31) / 32 * 32;
    };
    SizeGroups(view, source, {{new_files.front(), padded(imported.bytes.size()) + padded(imported.wave_data.size())}});

    return Refused(source, std::move(view));
}

std::string ItemName(const ntdlib::rsar::SoundArchive& archive, edit::FileKind kind, uint32_t item)
{
    switch (kind)
    {
    case edit::FileKind::kBank:
        return archive.Banks()[item].name;

    case edit::FileKind::kWaveArchive:
        for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(archive))
        {
            if (wave_archive.file_id == item)
            {
                return wave_archive.name;
            }
        }
        return "the wave data of file " + std::to_string(item);

    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return archive.Sounds()[item].name;
}

ReplaceView MakeWiiFileReplaceView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                   const edit::FileChanges& changes, std::optional<format::File> result,
                                   std::string error, const std::vector<edit::NamedWaveArchive>& named,
                                   const std::vector<int>& targets)
{
    // The item, and the file that goes in its place. A file of another console pairs each wave archive that it names
    // with wave data of the archive.
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    const std::string kind = edit::NameOf(incoming.kind);
    const std::string file = incoming.kind == edit::FileKind::kWaveSoundFile ? kind : kind + " file";
    ReplaceView view;
    view.sequence = false;
    view.title = "Replace " + ReplacedName(archive, incoming.kind, source.sound);
    view.heading = file;
    view.heading[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(view.heading[0])));
    view.source = WiiIncomingSource(source, incoming);
    view.result = std::move(result);
    view.error = std::move(error);
    std::vector<std::optional<std::size_t>> counts;
    if (source.later)
    {
        view.rows_heading = "Wave data";
        counts = WiiWaveDataMenu(source, false, view.wave_archives);
        view.wave_archive_rows = WaveArchiveRows(named, targets);
        if (std::ranges::any_of(view.wave_archive_rows, [](const WaveArchiveRow& row) { return row.target < 0; }))
        {
            view.error = "Choose wave data for each wave archive the " + file + " uses.";
            view.result.reset();
        }
    }

    // A file of another console that Hokuto couldn't convert: the error says why.
    if (source.later && !incoming.converted)
    {
        return view;
    }

    // The other items whose file it is. The window names a wave sound file by its sound group, and the warning names
    // each of its wave sounds.
    WarnSharing(view, archive, source, incoming, changes, kind);

    // A file of the Wii without wave data uses the current wave data (edit::ReplaceWiiFile). Its regions or its notes
    // name the waves there by their indices, or its WAVE block contains their places in the samples. An RWAR beside the
    // file stays out. Replace… of the wave data puts its waves in.
    WarnCurrentWaveData(view, source, incoming, changes, kind);

    // The waves that the new file names and the wave data beside it lacks.
    WarnWiiLackedWaves(view, changes, kind);

    // A bank's programs that the new bank lacks.
    if (!changes.programs.empty())
    {
        view.warnings.push_back("Missing from the new bank: " + CountVerb(changes.programs, "program ", "programs ") +
                                NumberList(changes.programs) + ". Notes using " +
                                CountVerb(changes.programs, "it", "them") + " will be silent.");
    }

    // What plays waves or wave sounds past the new file's end.
    WarnPastEnd(view, archive, incoming, changes);

    // The conversion of a file of another console, and the waves that its wave data lacks.
    WarnWiiConversion(view, source, incoming, named, targets, counts, file);

    // The version and the size.
    if (changes.version && *changes.version != incoming.version)
    {
        view.warnings.push_back("The new file is version " + WiiVersionText(incoming.version) +
                                ". The current file is version " + WiiVersionText(*changes.version) +
                                ". A game may reject a version its NW4R SND build doesn't support.");
    }
    WarnFileSizes(view, changes, incoming);

    return view;
}

} // namespace hokuto::app
