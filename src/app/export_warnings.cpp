// SPDX-License-Identifier: MIT

// The warnings that the Export window lists, and the lines of what the measure found: sizes, growth, collisions
// on the GBA, the notes of each console, and the warnings of each kind of export.

#include "app/export_warnings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/rom_writer.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sound_build.h"

#include "app/effect_editor.h"
#include "app/export_window.h"
#include "app/time_text.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/audibility.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace hokuto::app
{

namespace
{

// The words that start a warning about the file of an item of `kind`.
const char* FileWords(song::ItemKind kind)
{
    switch (kind)
    {
    case song::ItemKind::kSequence:
        return "The sequence file";
    case song::ItemKind::kWaveSound:
    case song::ItemKind::kWaveSoundFile:
        return "The wave sound file";
    case song::ItemKind::kBank:
        return "The bank";
    case song::ItemKind::kWaveArchive:
        return "The wave archive";
    case song::ItemKind::kWave:
        return "The wave";
    case song::ItemKind::kStream:
        break;
    }

    return "The stream";
}

// The oldest and the latest version of some waves.
struct WaveVersions
{
    uint32_t oldest = 0;
    uint32_t latest = 0;
};

// The versions of the waves of wave archive `wave_archive`, or nothing when no wave's header gives one.
std::optional<WaveVersions> WaveVersionsOf(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive)
{
    std::optional<WaveVersions> versions;
    const std::size_t count = song::WaveCountOf(archive, wave_archive);
    for (uint32_t wave = 0; wave < count; wave++)
    {
        const std::optional<uint32_t> version =
            song::HeaderVersion(song::ItemFile(archive, {song::ItemKind::kWave, wave_archive, wave}));
        if (!version)
        {
            continue;
        }

        versions = versions ? WaveVersions{std::min(versions->oldest, *version), std::max(versions->latest, *version)}
                            : WaveVersions{*version, *version};
    }

    return versions;
}

// The warnings about the versions of an export of `item` of `archive` as a file of the console of `to`. Another Console
// gives the same warnings. The file takes the oldest versions that the console's games hold (song::ExportVersions). A
// file that held a later version keeps its values. A wave that goes to a version without the original start of a loop
// loses that start.
std::vector<std::string> VersionWarnings(const ntdlib::sar::SoundArchive& archive, const song::Item& item,
                                         ntdlib::sar::Flavor to)
{
    std::vector<std::string> warnings;
    const ntdlib::sar::Flavor from = archive.ReadOptions().flavor;
    const song::FileVersions versions = song::ExportVersions(item.kind, to);
    const std::string oldest = ", the oldest version that " + song::ConsoleName(to) + " games use.";
    const bool wave = item.kind == song::ItemKind::kWave;
    std::optional<uint32_t> held;
    std::optional<WaveVersions> waves_held;
    try
    {
        held = song::HeaderVersion(song::ItemFile(archive, item));
        waves_held = wave && held                                ? std::optional(WaveVersions{*held, *held})
                     : item.kind == song::ItemKind::kWaveArchive ? WaveVersionsOf(archive, item.index)
                                                                 : std::nullopt;
    }
    catch (const std::exception&)
    {
        // The export's work gives the reason.
        return warnings;
    }

    // The file's version. A wave's file is the wave. nw::snd reads a wave sound's filters only from version 1.1.0 of
    // its file on.
    if (!wave && held && song::VersionsCompare(from, to) && *held > versions.file)
    {
        const bool sequence = item.kind == song::ItemKind::kSequence;
        const bool unfiltered =
            (item.kind == song::ItemKind::kWaveSound || item.kind == song::ItemKind::kWaveSoundFile) &&
            to == ntdlib::sar::Flavor::kCafe && versions.file < 0x00010100;
        const char* kept = sequence     ? " The commands don't change."
                           : unfiltered ? " nw::snd reads wave sound filters only from version 1.1.0. The Wii U "
                                          "plays these wave sounds without filters."
                                        : " The values don't change.";
        warnings.push_back(std::string(FileWords(item.kind)) + " is exported as version " +
                           song::VersionText(to, versions.file) + oldest + " It was version " +
                           song::VersionText(from, *held) + "." + kept);
    }

    // The version of the waves: the version of the wave, or those of the waves of the wave archive. The warning names
    // the versions that they had in the same way as the version of the wave.
    if (!waves_held)
    {
        return warnings;
    }
    const uint32_t waves_to = wave ? versions.file : versions.waves;
    const bool later = song::VersionsCompare(from, to) && waves_held->latest > waves_to;
    const bool loses = song::HoldsOriginalStart(from, waves_held->latest) && !song::HoldsOriginalStart(to, waves_to);
    if (later || loses)
    {
        std::string warning = std::string(wave ? "The wave is" : "The waves are") + " exported as version " +
                              song::VersionText(to, waves_to) + oldest;
        if (later && waves_held->oldest == waves_held->latest)
        {
            warning += std::string(wave ? " It was version " : " They were version ") +
                       song::VersionText(from, waves_held->latest) + ".";
        }
        else if (later)
        {
            warning += " They were versions " + song::VersionText(from, waves_held->oldest) + " to " +
                       song::VersionText(from, waves_held->latest) + ".";
        }
        if (loses)
        {
            warning += " Version " + song::VersionText(to, waves_to) +
                       " doesn't store the original loop start. Playback is the same.";
        }
        warnings.push_back(warning);
    }

    return warnings;
}

// Adds the warning that names the other sounds whose sequences or wave sounds the file of `item` contains.
template <typename Archive>
void SharedFileWarning(const Archive& archive, const song::Item& item, std::vector<std::string>& warnings)
{
    std::vector<std::string> names;
    for (const uint32_t sound : song::SoundsSharingFile(archive, item.index))
    {
        names.push_back(archive.Sounds()[sound].name);
    }
    if (!names.empty())
    {
        warnings.push_back(
            std::string("The file also contains the ") +
            (item.kind == song::ItemKind::kSequence ? "sequences" : "wave sounds") + " of " +
            (names.size() == 1 ? std::string("1 other sound: ") : std::to_string(names.size()) + " other sounds: ") +
            NameList(names) + ".");
    }
}

// Adds the warnings about what the WAV file of the wave sound `item` leaves out.
template <typename Archive>
void WaveSoundWavWarnings(const Archive& archive, const song::Item& item, std::vector<std::string>& warnings)
{
    try
    {
        const song::WaveSoundNote note = song::WaveSoundNoteOf(archive, item.index);
        if (note.panned)
        {
            warnings.push_back("The WAV file doesn't include the wave sound's pan.");
        }
        if (note.shaped)
        {
            warnings.push_back(
                "The wave sound has an envelope that fades it in or down. The WAV file doesn't include it.");
        }
        const std::size_t channels = song::WaveChannels(archive, note.wave_archive, note.wave);
        if (channels > 2)
        {
            warnings.push_back("The wave has " + std::to_string(channels) +
                               " channels. The console plays only the first two. The WAV file contains those.");
        }
    }
    catch (const std::exception&)
    {
        // The work gives the reason.
    }
}

// Adds the warning that the WAV file of a stream sound contains the channels without the mix of its tracks.
void StreamWavWarning(const ntdlib::sar::StreamSoundInfo& stream, std::vector<std::string>& warnings)
{
    const auto plain = [](const ntdlib::sar::StreamTrackInfo& track)
    {
        return track.volume == 127 && track.pan == 64;
    };
    if (stream.tracks.size() > 1 || !std::ranges::all_of(stream.tracks, plain))
    {
        warnings.push_back("The sound mixes the stream channels as " + std::to_string(stream.tracks.size()) +
                           (stream.tracks.size() == 1 ? " track" : " tracks") +
                           " with volumes and pans. The WAV file contains the channels unmixed.");
    }
}

// Adds the warning about the waves from `first` up to `end` of `item` that have more than two channels.
template <typename Archive>
void WideWaveWarnings(const Archive& archive, const song::Item& item, uint32_t first, uint32_t end,
                      std::vector<std::string>& warnings)
{
    std::size_t wide = 0;
    std::size_t channels = 0;
    for (uint32_t wave = first; wave < end; wave++)
    {
        try
        {
            const std::size_t count = song::WaveChannels(archive, item.index, wave);
            if (count > 2)
            {
                wide++;
                channels = count;
            }
        }
        catch (const std::exception&)
        {
            // The caller reports the reason through `found`.
        }
    }
    if (item.kind == song::ItemKind::kWave && wide > 0)
    {
        warnings.push_back("The wave has " + std::to_string(channels) +
                           " channels. The console plays only the first two. The WAV file contains those.");
    }
    else if (wide > 0)
    {
        warnings.push_back(std::to_string(wide) + (wide == 1 ? " wave has" : " waves have") +
                           " more than two channels. The console plays only the first two of each. The WAV "
                           "files contain those.");
    }
}

} // namespace

std::string NameList(const std::vector<std::string>& names)
{
    constexpr std::size_t kShown = 5;
    const std::size_t shown = names.size() > kShown + 1 ? kShown : names.size();
    std::vector<std::string> listed(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(shown));
    if (shown < names.size())
    {
        listed.push_back(std::to_string(names.size() - shown) + " more");
    }

    return JoinedNames(listed);
}

std::string SizeText(std::size_t bytes)
{
    std::array<char, 32> text{};
    if (bytes < 1024)
    {
        std::snprintf(text.data(), text.size(), "%zu bytes", bytes);
    }
    else if (bytes < 1024 * 1024)
    {
        std::snprintf(text.data(), text.size(), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    }
    else
    {
        std::snprintf(text.data(), text.size(), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }

    return text.data();
}

std::string SizeChangeText(std::size_t before, std::size_t after, bool plural)
{
    if (after > before)
    {
        return (plural ? "grow by " : "grows by ") + SizeText(after - before) + ", to " + SizeText(after);
    }
    if (after < before)
    {
        return (plural ? "shrink by " : "shrinks by ") + SizeText(before - after) + ", to " + SizeText(after);
    }

    return (plural ? "stay at " : "stays at ") + SizeText(after);
}

std::pair<std::string, std::string> SizeTexts(std::size_t first, std::size_t second)
{
    std::pair<std::string, std::string> texts(SizeText(first), SizeText(second));
    if (first != second && texts.first == texts.second)
    {
        // Each size in bytes, with commas between the thousands.
        const auto bytes = [](std::size_t count)
        {
            std::string digits = std::to_string(count);
            for (std::size_t i = digits.size(); i > 3; i -= 3)
            {
                digits.insert(i - 3, ",");
            }

            return digits + " bytes";
        };
        texts = {bytes(first), bytes(second)};
    }

    return texts;
}

namespace
{

// A time in the song for a line, such as "1:23.4": tenths of a second, rounded, as minutes and seconds.
std::string MinutesText(double seconds)
{
    const long long tenths = std::llround(std::max(seconds, 0.0) * 10.0);
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%lld:%02lld.%lld", tenths / 600, tenths / 10 % 60, tenths % 10);

    return text.data();
}

} // namespace

std::string PlaceLine(const song::LevelPlace& place)
{
    // A run's windows each lie more than 6 dB off. Their mean can lie closer when they go both ways. A no-break space
    // keeps each number on the line of its unit.
    std::array<char, 200> text{};
    const char* way = place.db > 0.0 ? "louder" : "quieter";
    if (place.run && std::abs(place.db) > song::kRunFindingDb)
    {
        std::snprintf(text.data(), text.size(), "Audible at %s: for %.0f\u00a0ms, the export is %.1f\u00a0dB %s.",
                      MinutesText(place.start).c_str(), place.seconds * 1000.0, std::abs(place.db), way);
    }
    else if (place.run)
    {
        std::snprintf(text.data(), text.size(),
                      "Audible at %s: for %.0f\u00a0ms, the export is alternately more than 6\u00a0dB louder and "
                      "quieter.",
                      MinutesText(place.start).c_str(), place.seconds * 1000.0);
    }
    else
    {
        std::snprintf(text.data(), text.size(), "%s from %s to %s: the export is %.1f\u00a0dB %s.",
                      std::abs(place.db) > 3.0 ? "Audible" : "May be audible", MinutesText(place.start).c_str(),
                      MinutesText(place.start + place.seconds).c_str(), std::abs(place.db), way);
    }

    return text.data();
}

std::vector<std::string> MeasuredLines(const song::LevelComparison& comparison, const std::string& renderer,
                                       const std::string& unplayed, const std::string& departures, double seconds,
                                       bool cut, bool random)
{
    // What played, and for how long, and what neither side played.
    std::vector<std::string> lines;
    lines.push_back(renderer + " rendered " + (cut ? "the first " : "") + MinutesText(seconds) +
                    " of the export. Hokuto rendered the same section." +
                    (unplayed.empty() ? "" : " Neither render includes ") + unplayed + (unplayed.empty() ? "" : "."));
    if (!departures.empty())
    {
        lines.push_back(departures);
    }
    if (random)
    {
        lines.push_back(
            "The sound uses random numbers. Hokuto and the export get different values. Some of the differences "
            "below may come from them.");
    }
    if (comparison.windows == 0)
    {
        lines.push_back("Neither render is loud enough to measure.");
        return lines;
    }

    // How much of the sound lies close. The gain is never a finding: a listener sets the volume. A no-break space keeps
    // each number on the line of its unit.
    const auto share = [&comparison](std::size_t windows)
    {
        return static_cast<int>(
            std::lround(100.0 * static_cast<double>(windows) / static_cast<double>(comparison.windows)));
    };

    std::array<char, 200> text{};
    std::snprintf(text.data(), text.size(),
                  "%d\u00a0%% of the 50\u00a0ms windows are within 1\u00a0dB, and %d\u00a0%% within 3\u00a0dB.",
                  share(comparison.within_1db), share(comparison.within_1db + comparison.from_1_to_3db));
    lines.push_back(text.data());
    if (std::abs(comparison.gain_db) >= 1.0)
    {
        std::snprintf(text.data(), text.size(),
                      "Overall, the export is %.1f\u00a0dB %s. The measurement ignores this difference because the "
                      "listener sets the volume.",
                      std::abs(comparison.gain_db), comparison.gain_db > 0.0 ? "louder" : "quieter");
        lines.push_back(text.data());
    }
    if (comparison.places.empty())
    {
        lines.push_back("No second differs by more than 1\u00a0dB, and no 200\u00a0ms section by more than 6\u00a0dB.");
    }

    return lines;
}

std::vector<std::string> GrowthWarnings(const format::ArchiveGrowth& growth, bool groups_measured)
{
    std::vector<std::string> warnings;
    if (growth.after > growth.before)
    {
        warnings.push_back("The archive " + SizeChangeText(growth.before, growth.after) + ".");
    }
    for (const format::GroupGrowth& group : growth.groups)
    {
        warnings.push_back("Files in group " + group.name + " " + SizeChangeText(group.before, group.after, true) +
                           ".");
    }
    if (!growth.groups.empty() || (!groups_measured && !warnings.empty()))
    {
        warnings.push_back(
            "The game loads each group into a sound heap. No game file records the heap size. If a group no "
            "longer fits, the game plays silence or crashes. Test the archive in the game or an emulator.");
    }

    return warnings;
}

std::vector<std::string> DseGrowthWarnings(std::size_t before, std::size_t after)
{
    if (after <= before)
    {
        return {};
    }

    return {"The sound files " + SizeChangeText(before, after, true) + "."};
}

std::vector<std::string> DsImageWarnings(std::size_t before, std::size_t after)
{
    if (after <= before)
    {
        return {};
    }

    std::vector<std::string> warnings = {"The image " + SizeChangeText(before, after) + "."};
    if (after >= 2 * before)
    {
        warnings.push_back(
            "The sound archive no longer fits the original ROM chip. The image grows to the next chip size, and the "
            "header is updated to match. A flash cartridge must have room for the new size.");
    }

    return warnings;
}

std::vector<std::string> RomWarnings(std::size_t moved, std::size_t before, std::size_t after, RomRoom room,
                                     bool konami)
{
    std::vector<std::string> warnings;
    const std::string songs = moved == 1 ? "1 song no longer fits in its original space"
                                         : std::to_string(moved) + " songs no longer fit in their original space";
    const char* them = moved == 1 ? "it" : "them";
    if (moved > 0 && room == RomRoom::kFreeSpace)
    {
        warnings.push_back(songs + " in the ROM. The export moves " + them +
                           " to the free space after the ROM data and updates the song table.");
    }
    else if (moved > 0)
    {
        warnings.push_back(songs + " in the ROM. There's no room for " + them +
                           " after the ROM data without passing the GBA's 32 MiB limit.");
    }
    if (moved > 0 && room == RomRoom::kFiller)
    {
        warnings.push_back(std::string("The export moves the songs that don't fit into filler runs inside the ROM and "
                                       "updates the song table. A filler run is ") +
                           (konami ? "4 KiB or more of 0xFF or zero bytes" : "16 KiB or more of 0xFF bytes") +
                           " that the music driver never reads.");
    }
    else if (moved > 0 && room == RomRoom::kLeftOut)
    {
        warnings.push_back(std::string("The edits to ") + (moved == 1 ? "that song" : "those songs") +
                           " are left out. The ROM keeps " + them +
                           " as opened. To keep the edits, turn on Move songs into filler runs.");
    }
    if (after != before)
    {
        warnings.push_back("The ROM " + SizeChangeText(before, after) + ".");
    }
    if (moved > 0 && room == RomRoom::kFreeSpace)
    {
        warnings.push_back(std::string("Nothing in the ROM marks its free space. Hokuto uses the run of ") +
                           (konami ? "0xFF or zero bytes" : "0xFF bytes") +
                           " at the end of the ROM. Test the ROM in an emulator to check that the game still runs.");
    }
    else if (moved > 0 && room == RomRoom::kFiller)
    {
        warnings.push_back(
            "Other game code might still read the filler runs. Test the ROM in an emulator to check that the "
            "game still runs and the songs play.");
    }

    return warnings;
}

std::string CollisionText(const song::Collisions& collisions, const std::string& console)
{
    std::vector<std::string> kinds;
    if (collisions.cut > 0)
    {
        kinds.push_back(collisions.cut == 1 ? std::string("1 note cut short by a later note")
                                            : std::to_string(collisions.cut) + " notes cut short by later notes");
    }
    if (collisions.unstarted > 0)
    {
        kinds.push_back(std::to_string(collisions.unstarted) + (collisions.unstarted == 1 ? " note" : " notes") +
                        " that " + console + " never plays");
    }

    return NameList(kinds);
}

std::string ChannelsNote(const std::string& console)
{
    return "Each channel of " + console +
           " plays one note at a time. The Key Editor shows notes that lose their channel in red. The channel lane "
           "shows which notes use each PSG channel.";
}

std::vector<std::string> CollisionWarnings(const std::vector<std::pair<std::string, song::Collisions>>& songs,
                                           const std::string& console)
{
    constexpr std::size_t kMostSongs = 8;
    std::vector<std::string> warnings;
    for (std::size_t i = 0; i < songs.size() && i < kMostSongs; i++)
    {
        const auto& [name, added] = songs[i];
        warnings.push_back(name + ": the edits add " + CollisionText(added, console) + ".");
    }
    if (songs.size() > kMostSongs)
    {
        const std::size_t rest = songs.size() - kMostSongs;
        warnings.push_back(rest == 1 ? std::string("1 more song has such notes.")
                                     : std::to_string(rest) + " more songs have such notes.");
    }
    if (!songs.empty())
    {
        warnings.push_back(ChannelsNote(console));
    }

    return warnings;
}

std::string StreamsNote(const std::vector<std::string>& paths)
{
    const bool one = paths.size() == 1;
    return std::string("The export writes ") + (one ? "1 stream" : std::to_string(paths.size()) + " streams") +
           " next to the archive, where the game looks for " + (one ? "it: " : "them: ") + NameList(paths) +
           ". The other streams don't change.";
}

namespace
{

// Whether `hash` is one of the hashes of a DSi image's digest. The DSi's digest key updates those hashes. ntdlib's
// names of them start the same way.
bool InDigest(const ntdlib::ntr::StaleHash& hash)
{
    return hash.what.starts_with("the digest's");
}

} // namespace

std::string DsiNote(const std::vector<ntdlib::ntr::StaleHash>& stale, bool key_failed)
{
    uint32_t count = 0;
    for (const ntdlib::ntr::StaleHash& hash : stale)
    {
        count += InDigest(hash) ? hash.count : 0;
    }
    if (count == 0)
    {
        return "The image is DSi-enhanced. The export recomputed the hashes of its digest with the DSi digest key.";
    }

    return "The image is DSi-enhanced. The export leaves " +
           (count == 1 ? std::string("1 hash") : std::to_string(count) + " hashes") +
           " of its digest out of date. To update " + (count == 1 ? "it" : "them") +
           (key_failed ? ", choose a DSi digest key file that Hokuto can read in Studio Setup."
                       : ", set the DSi digest key in Studio Setup.");
}

std::optional<std::string> SignatureNote(const std::vector<ntdlib::ntr::StaleHash>& stale, bool dsi)
{
    // ntdlib names each hash. The signature for download play follows the data of a DS image.
    std::string names;
    bool runs = false;
    bool download = false;
    for (const ntdlib::ntr::StaleHash& hash : stale)
    {
        if (InDigest(hash))
        {
            continue;
        }

        names += (names.empty() ? "" : ", ") + hash.what;
        const bool download_play = hash.what.find("download play") != std::string::npos;
        download = download || download_play;
        runs = runs || !download_play;
    }
    if (names.empty())
    {
        return std::nullopt;
    }

    return "The export leaves these out of date, and no key can update them: " + names + "." +
           (!runs ? ""
            : dsi ? " A DSi in DSi mode may refuse to run the image."
                  : " A DSi or a 3DS may refuse to run the image.") +
           (download ? " Download play from the image may fail." : "");
}

std::string TheConsole(const std::string& console)
{
    return console == "DSE" || console == "Konami" || console.ends_with("'s driver") ? console : "the " + console;
}

std::string EngineNote(const std::string& console, const std::string& file_console)
{
    return "This sound plays on the " + console + " engine. The exported files contain the conversion that you hear " +
           "in Hokuto. The archive keeps the sound for " + TheConsole(file_console) + ".";
}

std::string EngineSoundsNote(const std::vector<std::string>& names, const std::string& file_console)
{
    const bool one = names.size() == 1;
    return std::string("In Hokuto, ") + (one ? "1 sound plays" : std::to_string(names.size()) + " sounds play") +
           " on another console engine: " + NameList(names) + ". The archive keeps " + (one ? "it" : "them") + " for " +
           TheConsole(file_console) + ". File > Export > Another Console… exports a conversion.";
}

std::string GroupFilesNote(const std::vector<std::string>& names)
{
    const bool one = names.size() == 1;
    const std::string prefix = std::string(song::kUserManagementFolder) + "/";
    std::vector<std::string> beside;
    std::vector<std::string> inside;
    for (const std::string& name : names)
    {
        if (name.starts_with(prefix))
        {
            inside.push_back(name.substr(prefix.size()));
        }
        else
        {
            beside.push_back(name);
        }
    }
    const std::string folder = std::string("into the ") + song::kUserManagementFolder + " folder next to ";
    std::string where = beside.empty() ? folder + "the archive: " : "next to the archive: " + NameList(beside);
    if (!beside.empty() && !inside.empty())
    {
        where += ", and " + folder + "it: ";
    }
    if (!inside.empty())
    {
        where += NameList(inside);
    }

    return std::string("The export writes ") + (one ? "1 group file" : std::to_string(names.size()) + " group files") +
           " " + where + ". If a SARC archive next to the archive or the opened file contains " +
           (one ? "that group file" : "one of them") +
           ", the export writes a copy of the SARC archive with the new group file instead, next to the archive. "
           "If the game keeps its group files in another folder, move " +
           (one ? "the file there." : "the files there.");
}

std::string UnknownGroupNote(std::size_t index)
{
    return "The edits change group file " + std::to_string(index + 1) +
           ", but Hokuto can't tell which group it belongs to. The export leaves it out.";
}

std::vector<std::string> MidiWarnings(const song::Sequence& sequence, const BusEffects& effects,
                                      const std::array<float, format::kAuxBuses>& volumes, ExportTarget target)
{
    std::vector<std::string> warnings;
    if (!sequence.read_variables.empty())
    {
        warnings.emplace_back(
            "MIDI has no variables or conditions. The export follows the path that the current variable values "
            "and changes select.");
    }

    // The Switch's converter sends FX A to the synth's reverb with CC91. The sends to FX B and FX C reach the mix dry.
    const auto has_effects = [](const std::vector<format::Effect>& bus)
    {
        return !bus.empty();
    };
    if (std::any_of(effects.begin(), effects.end(), has_effects))
    {
        warnings.push_back(std::string(target == ExportTarget::kMidiSoundFont ? "SoundFont 2" : "SFZ") +
                           " has no effects. Sends to FX A become reverb sends (CC91) in the MIDI file. Sends to "
                           "FX B and FX C are ignored.");
    }

    // The converter sends to each bus as though its FX channel's fader stood at 0 dB.
    const auto not_unity = [](float volume)
    {
        return volume != 1.0f;
    };
    if (std::any_of(volumes.begin(), volumes.end(), not_unity))
    {
        warnings.emplace_back(
            "MIDI has no FX channel volumes. The files send to each bus as if its fader were at 0 dB.");
    }

    return warnings;
}

std::vector<std::string> ItemWarnings(const ntdlib::sar::SoundArchive& archive, const song::Item& item,
                                      ExportTarget target, const std::vector<std::string>& found)
{
    // A sequence file can hold the sequences of other sounds too, and a wave sound file the wave sounds of others.
    std::vector<std::string> warnings;
    const bool file = target == ExportTarget::kItemFile || target == ExportTarget::kWiiFile || ConsoleOf(target);
    const bool sequence_file = item.kind == song::ItemKind::kSequence && file;
    const bool wave_sound_file = item.kind == song::ItemKind::kWaveSound && file;
    if (sequence_file || wave_sound_file)
    {
        SharedFileWarning(archive, item, warnings);
    }

    // A wave sound's WAV file holds the decoded wave of its first note, at the sound's pitch. The consoles play no
    // other note of a wave sound: they read none of the note events of its tracks.
    if (item.kind == song::ItemKind::kWaveSound && target == ExportTarget::kWav)
    {
        WaveSoundWavWarnings(archive, item, warnings);
    }

    // A stream sound plays its stream's channels as tracks, each with a volume and a pan. A WAV file holds the channels
    // as they are.
    if (item.kind == song::ItemKind::kStream && target == ExportTarget::kWav && item.index < archive.Sounds().size())
    {
        StreamWavWarning(archive.Sounds()[item.index].stream, warnings);
    }

    // A file for another console takes the oldest versions that the console's games hold. StreamConversionWarnings
    // gives a stream's.
    const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target);
    if (console && item.kind != song::ItemKind::kStream)
    {
        const std::vector<std::string> versions = VersionWarnings(archive, item, *console);
        warnings.insert(warnings.end(), versions.begin(), versions.end());
    }

    // The console plays only the first two channels of a wave. A WAV file holds those.
    if (target == ExportTarget::kWav && item.kind != song::ItemKind::kWaveSound && item.kind != song::ItemKind::kStream)
    {
        const uint32_t first = item.kind == song::ItemKind::kWave ? item.wave : 0;
        const uint32_t end = item.kind == song::ItemKind::kWave
                                 ? item.wave + 1
                                 : static_cast<uint32_t>(song::WaveCountOf(archive, item.index));
        WideWaveWarnings(archive, item, first, end, warnings);
    }
    warnings.insert(warnings.end(), found.begin(), found.end());

    return warnings;
}

std::vector<std::string> StreamConversionWarnings(std::span<const uint8_t> stream, ntdlib::sar::Flavor to)
{
    // An AAC stream goes in DSP-ADPCM. A stream of the Wii keeps its tracks.
    if (song::IsAdts(stream))
    {
        return {"The AAC stream is converted to DSP-ADPCM from the output of the system AAC decoder."};
    }

    std::vector<std::string> warnings;
    bool loop = false;
    bool tracks = true;
    bool from_ctr = false;
    std::optional<ntdlib::sar::StreamFile> later;
    try
    {
        if (stream.size() >= 4 && std::memcmp(stream.data(), "RSTM", 4) == 0)
        {
            const ntdlib::rsar::StreamFile file = ntdlib::rsar::StreamFile::Read(stream);
            loop = file.loop;
            tracks = !file.tracks.empty();
        }
        else
        {
            later = ntdlib::sar::StreamFile::Read(stream);
            loop = later->loop;
            tracks = later->tracks && !later->tracks->empty();
            from_ctr = later->flavor == ntdlib::sar::Flavor::kCtr;
        }
    }
    catch (const std::exception&)
    {
        return warnings;
    }

    // The stream takes a version that the console's games read: 3.0.0 for the Wii U, 4.0.0 for the Switch.
    const uint32_t version = song::ExportVersions(song::ItemKind::kStream, to).file;
    if (later && song::VersionsCompare(later->flavor, to) && later->version > version)
    {
        warnings.push_back("The stream is exported as version " + song::VersionText(to, version) + " for " +
                           song::ConsoleName(to) + " games. It was version " +
                           song::VersionText(later->flavor, later->version) + ". The samples don't change.");
    }
    if (to != ntdlib::sar::Flavor::kCtr || from_ctr)
    {
        return warnings;
    }
    if (loop)
    {
        warnings.emplace_back(
            "The 3DS tools repeat the first 128 samples of the loop after the end of a looped stream. The "
            "converted stream doesn't. Such a stream hasn't been tested in a 3DS game.");
    }
    if (!tracks)
    {
        warnings.emplace_back(
            "In 3DS archives before version 2.3.0.0, a stream sound plays the tracks stored in the stream file. The "
            "converted stream has no tracks. Replace… copies the tracks from the current stream.");
    }

    return warnings;
}

std::vector<std::string> WiiStreamWavWarnings(std::span<const uint8_t> stream, const ntdlib::SndBuild& build)
{
    std::vector<std::string> warnings;
    ntdlib::rsar::StreamFile file;
    try
    {
        file = ntdlib::rsar::StreamFile::Read(stream);
    }
    catch (const std::exception&)
    {
        return warnings;
    }

    // The builds before March 2008 play the first two channels alone. The later builds play the tracks, each with its
    // volume and pan.
    if (!build.SinceMarch2008())
    {
        if (file.channels.size() > 2)
        {
            warnings.push_back("The stream has " + std::to_string(file.channels.size()) +
                               " channels. This game's build of NW4R SND plays only the first two. The WAV file "
                               "contains all channels.");
        }
        return warnings;
    }
    const auto plain = [](const ntdlib::rsar::StreamFile::Track& track)
    {
        return track.volume == 127 && track.pan == 64;
    };
    if (file.tracks.size() > 1 || !std::ranges::all_of(file.tracks, plain))
    {
        warnings.push_back("The sound mixes the stream channels as " + std::to_string(file.tracks.size()) +
                           (file.tracks.size() == 1 ? " track" : " tracks") +
                           " with volumes and pans. The WAV file contains the channels unmixed.");
    }

    return warnings;
}

std::vector<std::string> ItemWarnings(const ntdlib::rsar::SoundArchive& archive, const song::Item& item,
                                      ExportTarget target, const std::vector<std::string>& found)
{
    // A sequence file can hold the sequences of other sounds too, and a wave sound file the wave sounds of others.
    std::vector<std::string> warnings;
    const bool sound = item.kind == song::ItemKind::kSequence || item.kind == song::ItemKind::kWaveSound;
    if (sound && (target == ExportTarget::kItemFile || ConsoleOf(target)))
    {
        SharedFileWarning(archive, item, warnings);
    }

    // A wave sound's WAV file holds the decoded wave of its first note, at the sound's pitch. NW4R SND plays no other
    // note of a wave sound.
    if (item.kind == song::ItemKind::kWaveSound && target == ExportTarget::kWav)
    {
        WaveSoundWavWarnings(archive, item, warnings);
    }

    // NW4R SND plays only the first two channels of a wave. A WAV file holds those.
    if (target == ExportTarget::kWav &&
        (item.kind == song::ItemKind::kWave || item.kind == song::ItemKind::kWaveArchive))
    {
        const uint32_t first = item.kind == song::ItemKind::kWave ? item.wave : 0;
        uint32_t end = first + 1;
        if (item.kind == song::ItemKind::kWaveArchive)
        {
            try
            {
                end = static_cast<uint32_t>(song::WaveCountOf(archive, item.index));
            }
            catch (const std::exception&)
            {
                // `found` gives the reason.
                end = 0;
            }
        }
        WideWaveWarnings(archive, item, first, end, warnings);
    }
    warnings.insert(warnings.end(), found.begin(), found.end());

    return warnings;
}

std::vector<std::string> ConverterWarnings(std::vector<std::string> lines, bool sfz)
{
    lines = song::ReportFor(std::move(lines), sfz);
    for (std::string& line : lines)
    {
        line = format::AsSentence(std::move(line));
    }

    return lines;
}

std::vector<std::string> SoundFontWarnings(const ntdlib::sequence_export::BankConversion& bank, const std::string& stem)
{
    if (bank.sound_fonts.size() < 2)
    {
        return {};
    }

    std::vector<std::string> runs;
    for (const ntdlib::sequence_export::BankSoundFont& sound_font : bank.sound_fonts)
    {
        runs.push_back(std::to_string(sound_font.first) + " to " + std::to_string(sound_font.last));
    }

    return {"A SoundFont 2 file can contain at most 65,535 generators, modulators and zones. The instruments are " +
            std::string("split into ") + std::to_string(runs.size()) + " files, for programs " + NameList(runs) +
            ". Each file name ends with its program range, for example " + song::SoundFontNames(bank, stem)[0] + "."};
}

} // namespace hokuto::app
