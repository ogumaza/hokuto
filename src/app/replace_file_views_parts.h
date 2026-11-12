// SPDX-License-Identifier: MIT

// The parts of the file views that their files share (replace_file_views.cpp and replace_wii_file_views.cpp). The
// names are internal to those files.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "app/export_warnings.h"
#include "app/item_views.h"
#include "app/replace_common.h"
#include "app/replace_file_views.h"

namespace hokuto::app::replace_file_parts
{

// The rows of a bank or a wave sound file: each wave archive that its waves name (`named`), by its number in the file's
// archive, with the wave archive of `targets`.
std::vector<WaveArchiveRow> WaveArchiveRows(const std::vector<edit::NamedWaveArchive>& named,
                                            const std::vector<int>& targets);

// The title of the window of an import of a bank, a wave sound file or a wave archive, or of Add Bank.
std::string ItemImportTitle(const ReplaceSource& source, bool bank, bool wave_sounds);

// The warning of the numbers of the new sounds `names`, `sounds` of them from number `first`.
std::string NewSoundsNumberWarning(const std::vector<std::string>& names, std::size_t first, std::size_t sounds);

// `one` for one of `items`, and `more` for more of them.
std::string CountVerb(const std::vector<uint32_t>& items, const char* one, const char* more);

// The warning of the sizes of the current file and the new one.
void WarnFileSizes(ReplaceView& view, const edit::FileChanges& changes, const edit::IncomingFile& incoming);

// Gives `view` the waves that `read` reads from a new wave archive, for Play, with a line for each in the menu of
// `plays`. A wave archive that `read` can't read plays nothing.
template <typename Read>
void AddWaves(ReplaceView& view, Read read)
{
    try
    {
        view.waves = read();
    }
    catch (const std::exception&)
    {
        view.waves.clear();
    }
    for (std::size_t i = 0; i < view.waves.size(); i++)
    {
        view.plays.push_back("Wave " + std::to_string(i));
    }
}

// The wave sounds of a replaced wave sound file: `sound`, then the other sounds of `sharing` in their order.
std::vector<uint32_t> WithReplacedSound(uint32_t sound, const std::vector<uint32_t>& sharing);

// The names of the items of kind `of` at their `places` in the archive's lists, as a list.
template <typename Archive>
std::string ItemNames(const Archive& archive, edit::FileKind of, const std::vector<uint32_t>& places)
{
    std::vector<std::string> list;
    for (const uint32_t place : places)
    {
        list.push_back(ItemName(archive, of, place));
    }

    return NameList(list);
}

// The warnings of the other items whose file it is.
template <typename Archive>
void WarnSharing(ReplaceView& view, const Archive& archive, const ReplaceSource& source,
                 const edit::IncomingFile& incoming, const edit::FileChanges& changes, const std::string& kind)
{
    const std::vector<uint32_t>& sharing = changes.sharing;
    if (!sharing.empty() && incoming.kind == edit::FileKind::kWaveSoundFile)
    {
        view.warnings.push_back(
            "The file contains the wave sounds " +
            ItemNames(archive, edit::FileKind::kWaveSoundFile, WithReplacedSound(source.sound, sharing)) +
            ". Each one plays the wave sound at the same index in the new file.");
    }
    else if (!sharing.empty())
    {
        view.warnings.push_back(
            ItemNames(archive, incoming.kind, sharing) +
            CountVerb(sharing, " uses the same file. It also gets", " use the same file. They also get") + " the new " +
            kind + ".");
    }
}

// The warnings of what plays waves or wave sounds past the new file's end.
template <typename Archive>
void WarnPastEnd(ReplaceView& view, const Archive& archive, const edit::IncomingFile& incoming,
                 const edit::FileChanges& changes)
{
    if (incoming.kind == edit::FileKind::kWaveArchive && (!changes.past_banks.empty() || !changes.past_sounds.empty()))
    {
        std::vector<std::string> list;
        for (const uint32_t bank : changes.past_banks)
        {
            list.push_back(ItemName(archive, edit::FileKind::kBank, bank));
        }
        for (const uint32_t sound : changes.past_sounds)
        {
            list.push_back(ItemName(archive, edit::FileKind::kWaveSoundFile, sound));
        }
        view.warnings.push_back("The new wave archive has only " + Grouped(changes.count) +
                                (changes.count == 1 ? " wave. " : " waves. ") + NameList(list) +
                                (list.size() == 1 ? " uses" : " use") +
                                " waves past the end. Notes using them will be silent.");
    }
    else if (!changes.past_sounds.empty())
    {
        view.warnings.push_back("The new wave sound file has only " + Grouped(changes.count) +
                                (changes.count == 1 ? " wave sound. " : " wave sounds. ") +
                                ItemNames(archive, edit::FileKind::kWaveSoundFile, changes.past_sounds) +
                                CountVerb(changes.past_sounds, " uses a wave sound", " use wave sounds") +
                                " past the end and will be silent.");
    }
}

} // namespace hokuto::app::replace_file_parts
