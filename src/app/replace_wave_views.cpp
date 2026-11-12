// SPDX-License-Identifier: MIT

// The views of a wave or a WAV file that replaces a wave or comes in as a new one, on the 3DS, the Wii U, the
// Switch and the Wii.

#include "app/replace_wave_views.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "app/export_warnings.h"
#include "app/item_views.h"
#include "app/replace_common.h"
#include "app/replace_file_views.h"
#include "app/replace_view.h"
#include "edit/files.h"
#include "format/file.h"
#include "song/items.h"

namespace hokuto::app
{
namespace
{

using replace_common::AddWaveLoopWarning;
using replace_common::ChannelsText;
using replace_common::EncodingText;
using replace_common::LengthText;
using replace_common::LoopText;
using replace_common::OfferPresentLoop;
using replace_common::RateChangeText;
using replace_common::RatesOf;
using replace_common::WaveArchiveMenu;
using replace_common::WavePlayersText;
using replace_common::WaveSizeLine;
using replace_common::WaveSourceLine;
using replace_common::WavesText;
using replace_common::WavSourceLine;
using replace_common::WavWarnings;
using replace_common::WiiVersionText;
using replace_common::WiiWaveDataMenu;

// Adds to `warnings` the other wave archives with the file of the wave archive that changes, and the copies of the file
// in groups that hold other bytes (`archive_changes`).
void CopyWarnings(const ntdlib::sar::SoundArchive& archive, const edit::FileChanges& archive_changes,
                  std::vector<std::string>& warnings)
{
    const std::vector<uint32_t>& sharing = archive_changes.sharing;
    if (!sharing.empty())
    {
        std::vector<std::string> names;
        for (const uint32_t place : sharing)
        {
            names.push_back(ItemName(archive, edit::FileKind::kWaveArchive, place));
        }
        warnings.push_back(
            NameList(names) +
            (sharing.size() == 1 ? " uses the same file. It also gets" : " use the same file. They also get") +
            " the new wave.");
    }
    if (archive_changes.other_copies > 0)
    {
        const bool one = archive_changes.other_copies == 1;
        warnings.push_back((one ? std::string("A copy") : std::to_string(archive_changes.other_copies) + " copies") +
                           " of the wave archive in " + (one ? "a group differs" : "groups differ") +
                           " from it. The new wave archive replaces " + (one ? "that copy" : "those copies") + " too.");
    }
}

// Adds to `warnings` what a WAV file holds and its wave leaves out (`wav`), or the conversion of a wave file for an
// archive of `flavor`: a wave file of the other console, or one of a later version.
void IncomingWaveWarnings(const edit::IncomingWave& incoming, bool wav, ntdlib::sar::Flavor flavor,
                          std::vector<std::string>& warnings)
{
    if (wav)
    {
        WavWarnings(incoming, warnings);
    }
    else if (incoming.converted && incoming.converted->flavor == flavor)
    {
        warnings.push_back("The wave is saved as version " + song::VersionText(flavor, incoming.version) +
                           " to match the archive. The samples don't change. A game may reject a later version than "
                           "the one it uses.");
    }
    else if (incoming.converted)
    {
        warnings.push_back("This is a " + song::ConsoleName(incoming.converted->flavor) +
                           " file. It's converted to a " + song::ConsoleName(flavor) + " file (version " +
                           song::VersionText(flavor, incoming.version) + ") with the same samples.");
    }
}

// Adds to `warnings` what a WAV file holds and its wave leaves out (`wav`), or the conversion of a wave file of the
// 3DS, the Wii U or the Switch for the Wii.
void WiiWaveWarnings(const edit::IncomingWave& incoming, bool wav, std::vector<std::string>& warnings)
{
    if (wav)
    {
        WavWarnings(incoming, warnings);
    }
    else if (incoming.converted)
    {
        warnings.push_back("This is a " + song::ConsoleName(incoming.converted->flavor) +
                           " file. It's converted to a Wii wave with the same samples. Its version matches the other "
                           "waves in the wave data.");
    }
}

// The line of a wave file of the Wii that goes in, or of one of another console that goes in converted.
std::string WiiWaveSourceLine(const std::string& file_name, const edit::IncomingWave& incoming)
{
    if (incoming.converted)
    {
        return WaveSourceLine(file_name, incoming, incoming.converted->flavor);
    }

    std::string line = file_name + ": Wii wave, version " + WiiVersionText(incoming.version) + ", " +
                       EncodingText(incoming.encoding) + ", " + ChannelsText(incoming.channels) + ", " +
                       Grouped(incoming.rate) + " Hz, " + LengthText(incoming.frames, incoming.rate);
    if (incoming.loop)
    {
        line += LoopText(incoming.loop_start, incoming.frames);
    }

    return line;
}

// The head of the view of a wave: `title`, the heading of a WAV file or a wave file, `source_line`, and the encoding of
// a WAV file and the rates that it can have, with `present_rate` among them.
ReplaceView WaveViewHead(std::string title, const edit::IncomingWave& incoming, bool wav, std::string source_line,
                         std::optional<uint32_t> present_rate = std::nullopt)
{
    ReplaceView view;
    view.sequence = false;
    view.title = std::move(title);
    view.heading = wav ? "WAV file" : "Wave file";
    view.source = std::move(source_line);
    if (wav)
    {
        view.encoding = incoming.encoding;
        view.rates = RatesOf(incoming.wav_rate, present_rate);
        view.rate = static_cast<std::size_t>(std::ranges::find(view.rates, incoming.rate) - view.rates.begin());
    }

    return view;
}

// Adds to `warnings` the line of what plays the wave: the banks and the wave sounds in `players`, or none.
void AddWavePlayers(const std::vector<std::string>& players, std::vector<std::string>& warnings)
{
    if (players.empty())
    {
        warnings.push_back("No bank or wave sound uses the wave.");
    }
    else
    {
        warnings.push_back(WavePlayersText(players));
    }
}

// Adds to `warnings` the lines of the channels and the rate where the new wave changes them.
void AddWaveFormatChanges(const edit::WaveChanges& changes, const edit::IncomingWave& incoming,
                          std::vector<std::string>& warnings)
{
    if (changes.channels != incoming.channels)
    {
        warnings.push_back("The current wave is " + ChannelsText(changes.channels) + ". The new one is " +
                           ChannelsText(incoming.channels) + ".");
    }
    if (changes.rate != incoming.rate)
    {
        warnings.push_back(RateChangeText(changes.rate, incoming.rate));
    }
}

} // namespace

ReplaceView MakeWiiNewWaveView(const ReplaceSource& source, uint32_t file_id, uint32_t wave,
                               const edit::IncomingWave& incoming, bool wav, std::optional<format::File> result,
                               std::string error)
{
    // The file, and the menus of the wave data and of a WAV file's encodings.
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    ReplaceView view =
        WaveViewHead(wav ? "Import Audio File" : "Import Wave", incoming, wav,
                     wav ? WavSourceLine(source.file_name, incoming) : WiiWaveSourceLine(source.file_name, incoming));
    view.action = "Import";
    view.wave = incoming.bytes;
    const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(archive);
    const auto place = std::ranges::find(wave_data, file_id, &song::WiiWaveArchive::file_id);
    view.wave_archive = place != wave_data.end() ? static_cast<int>(place - wave_data.begin()) : 0;
    WiiWaveDataMenu(source, false, view.wave_archives);
    view.result = std::move(result);
    view.error = std::move(error);
    if (!view.result)
    {
        return view;
    }

    // Where the new wave goes, the wave file of another console, a WAV file's samples, and the sizes.
    const std::string target =
        place != wave_data.end() ? place->name : "the wave data of file " + std::to_string(file_id);
    view.warnings.push_back("The new wave becomes wave " + std::to_string(wave) + " of " + target +
                            (wave == 0 ? " (the first wave)." : " (after the existing waves).") +
                            " No region or wave sound uses it yet.");
    WiiWaveWarnings(incoming, wav, view.warnings);
    view.warnings.push_back(std::string("The new wave is ") + EncodingText(incoming.encoding) + " (" +
                            SizeText(incoming.bytes.size()) + "). The wave data takes up " +
                            SizeText(archive.WaveData(file_id).size()) +
                            " before the import. Games load wave data into a fixed memory area. Larger data may not "
                            "fit.");

    return view;
}

std::optional<std::string> SilentWaveSoundNote(const ntdlib::sar::SoundArchive& archive, uint32_t sound)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != ntdlib::sar::SoundType::kWaveSound)
    {
        return std::nullopt;
    }

    const song::WaveSoundNote note = song::WaveSoundNoteOf(archive, sound);
    const std::size_t count = song::WaveCountOf(archive, note.wave_archive);
    if (count == 0 || note.wave < count)
    {
        return std::nullopt;
    }

    const std::string& wave_archive = archive.WaveArchives()[note.wave_archive].name;

    return archive.Sounds()[sound].name + " is silent. It uses wave " + Grouped(note.wave) + " of " + wave_archive +
           ", and " + wave_archive + " has only " + WavesText(count) + ".";
}

ReplaceView MakeWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                bool wav, const edit::WaveChanges& changes, const edit::FileChanges& archive_changes,
                                std::size_t archive_size, std::optional<format::File> result, std::string error)
{
    // The wave, and the WAV file or the wave file that goes in its place.
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    ReplaceView view = WaveViewHead(
        "Replace " + song::ItemName(archive, {song::ItemKind::kWave, source.sound, wave}), incoming, wav,
        wav ? WavSourceLine(source.file_name, incoming) : WaveSourceLine(source.file_name, incoming, flavor),
        changes.rate);
    OfferPresentLoop(view, incoming, wav, changes);
    view.wave = incoming.bytes;
    view.result = std::move(result);
    view.error = std::move(error);

    // What plays the wave, and the other wave archives whose file it is.
    std::vector<std::string> players;
    for (const uint32_t bank : changes.banks)
    {
        players.push_back(ItemName(archive, edit::FileKind::kBank, bank));
    }
    for (const uint32_t sound : changes.sounds)
    {
        players.push_back(ItemName(archive, edit::FileKind::kWaveSoundFile, sound));
    }
    AddWavePlayers(players, view.warnings);
    CopyWarnings(archive, archive_changes, view.warnings);

    // The loop.
    AddWaveLoopWarning(changes.loop, incoming.loop, view.warnings);
    IncomingWaveWarnings(incoming, wav, flavor, view.warnings);
    AddWaveFormatChanges(changes, incoming, view.warnings);

    // The sizes. A game loads its wave archives into memory that it sets aside.
    view.warnings.push_back(
        WaveSizeLine(changes.encoding, changes.size, incoming.encoding, incoming.bytes.size(), nullptr));
    if (archive_size > archive_changes.size)
    {
        view.warnings.push_back("The wave archive " + SizeChangeText(archive_changes.size, archive_size) +
                                ". Games load wave archives into a fixed memory area. A larger wave archive may not "
                                "fit.");
    }

    return view;
}

ReplaceView MakeWiiWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                   bool wav, const edit::WaveChanges& changes, std::optional<format::File> result,
                                   std::string error)
{
    // The wave, and the WAV file or the wave file of the Wii that goes in its place.
    const ntdlib::rsar::SoundArchive& archive = *source.played.Wii();
    ReplaceView view = WaveViewHead(
        "Replace " + song::ItemName(archive, {song::ItemKind::kWave, source.sound, wave}), incoming, wav,
        wav ? WavSourceLine(source.file_name, incoming) : WiiWaveSourceLine(source.file_name, incoming), changes.rate);
    OfferPresentLoop(view, incoming, wav, changes);
    view.wave = incoming.bytes;
    view.result = std::move(result);
    view.error = std::move(error);

    // What plays the wave. The wave data belongs to one file: a bank's or a wave sound file's.
    std::vector<std::string> players;
    for (const uint32_t bank : changes.banks)
    {
        players.push_back(archive.Banks()[bank].name);
    }
    for (const uint32_t sound : changes.sounds)
    {
        players.push_back(archive.Sounds()[sound].name);
    }
    AddWavePlayers(players, view.warnings);

    // The loop, the channels, the rate and the sizes.
    AddWaveLoopWarning(changes.loop, incoming.loop, view.warnings);
    WiiWaveWarnings(incoming, wav, view.warnings);
    AddWaveFormatChanges(changes, incoming, view.warnings);
    view.warnings.push_back(WaveSizeLine(changes.encoding, changes.size, incoming.encoding, incoming.bytes.size(),
                                         "Games load wave data into a fixed memory area. Larger data may not fit."));

    return view;
}

ReplaceView MakeNewWaveView(const ReplaceSource& source, uint32_t wave_archive, uint32_t wave,
                            const edit::IncomingWave& incoming, bool wav, const edit::FileChanges& archive_changes,
                            std::size_t archive_size, std::optional<format::File> result, std::string error)
{
    // The file, and the menus of the wave archives and of a WAV file's encodings.
    const ntdlib::sar::SoundArchive& archive = *source.played.Sar();
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    ReplaceView view = WaveViewHead(
        wav ? "Import Audio File" : "Import Wave", incoming, wav,
        wav ? WavSourceLine(source.file_name, incoming) : WaveSourceLine(source.file_name, incoming, flavor));
    view.action = "Import";
    view.wave = incoming.bytes;
    view.wave_archive = static_cast<int>(wave_archive);
    WaveArchiveMenu(archive, view.wave_archives);
    view.result = std::move(result);
    view.error = std::move(error);
    if (!view.result)
    {
        return view;
    }

    // Where the new wave goes, and the other copies of the wave archive.
    const std::string target = ItemName(archive, edit::FileKind::kWaveArchive, wave_archive);
    view.warnings.push_back("The new wave becomes wave " + std::to_string(wave) + " of " + target +
                            (wave == 0 ? " (the first wave)." : " (after the existing waves).") +
                            " No bank or wave sound uses it yet.");
    CopyWarnings(archive, archive_changes, view.warnings);

    // A WAV file's samples, or a wave file of the other console.
    IncomingWaveWarnings(incoming, wav, flavor, view.warnings);

    // The sizes. A game loads its wave archives into memory that it sets aside.
    view.warnings.push_back(std::string("The new wave is ") + EncodingText(incoming.encoding) + " (" +
                            SizeText(incoming.bytes.size()) + "). " + target + " " +
                            SizeChangeText(archive_changes.size, archive_size) +
                            ". Games load wave archives into a fixed memory area. A larger wave archive may not fit.");

    return view;
}

} // namespace hokuto::app
