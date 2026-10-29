// SPDX-License-Identifier: MIT

#include "app/export_window.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
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

#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/sar/stream_file.h"
#include "xsflib/ascii.h"

#include "app/theme.h"
#include "app/widgets.h"
#include "format/kinds.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/midi_export.h"
#include "song/streams.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// The ID of the Export window. Its title names the target.
constexpr char kExportWindow[] = "###Export";

// The Export window's margin from the top and the bottom of the main window, in points.
constexpr float kMargin = 32.0f;

// The lines of the targets of a sequence to MIDI.
constexpr char kMidiSoundFontLine[] = "The sequence as a MIDI file, with a SoundFont 2 file of its instruments";
constexpr char kMidiSfzLine[] = "The sequence as a MIDI file, with SFZ files of its instruments";

// The names of an item's kind: in the Export window's title, and as the heading of the item's name.
struct KindNames
{
    const char* title;
    const char* heading;
};

KindNames NamesOf(song::ItemKind kind)
{
    switch (kind)
    {
    case song::ItemKind::kSequence:
        return {"Sequence", "Sequence"};
    case song::ItemKind::kWaveSound:
        return {"Wave Sound", "Wave sound"};
    case song::ItemKind::kWaveSoundFile:
        return {"Wave Sound File", "Wave sound file"};
    case song::ItemKind::kBank:
        return {"Bank", "Bank"};
    case song::ItemKind::kWaveArchive:
        return {"Wave Archive", "Wave archive"};
    case song::ItemKind::kWave:
        return {"Wave", "Wave"};
    case song::ItemKind::kStream:
        return {"Stream", "Stream sound"};
    }

    return {"Item", "Item"};
}

// Whether `name`, a stream's file name, names an AAC file.
bool IsAacFile(const std::string& name)
{
    return name.size() > 4 && xsflib::AsciiLower(name.substr(name.size() - 4)) == ".aac";
}

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

// The latest version of the waves of wave archive `wave_archive`, or nothing when no wave's header gives one.
std::optional<uint32_t> LatestWaveVersion(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive)
{
    std::optional<uint32_t> latest;
    const std::size_t count = song::WaveCountOf(archive, wave_archive);
    for (uint32_t wave = 0; wave < count; wave++)
    {
        const std::optional<uint32_t> version =
            song::HeaderVersion(song::ItemFile(archive, {song::ItemKind::kWave, wave_archive, wave}));
        if (version && (!latest || *version > *latest))
        {
            latest = version;
        }
    }

    return latest;
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
    const std::string oldest = ", the oldest that the " + song::ConsoleName(to) + "'s games hold.";
    const bool wave = item.kind == song::ItemKind::kWave;
    std::optional<uint32_t> held;
    std::optional<uint32_t> waves_held;
    try
    {
        held = song::HeaderVersion(song::ItemFile(archive, item));
        waves_held = wave                                        ? held
                     : item.kind == song::ItemKind::kWaveArchive ? LatestWaveVersion(archive, item.index)
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
        const char* kept = sequence     ? " Its commands stay the same."
                           : unfiltered ? " nw::snd reads a wave sound's filters only from version 1.1.0 on. The "
                                          "Wii U plays the file's wave sounds without them."
                                        : " Its values stay the same.";
        warnings.push_back(std::string(FileWords(item.kind)) + " goes out as version " +
                           song::VersionText(to, versions.file) + oldest + " It held " +
                           song::VersionText(from, *held) + "." + kept);
    }

    // The waves' version: the wave's, or the latest of the wave archive's waves.
    if (!waves_held)
    {
        return warnings;
    }
    const uint32_t waves_to = wave ? versions.file : versions.waves;
    const bool later = song::VersionsCompare(from, to) && *waves_held > waves_to;
    const bool loses = song::HoldsOriginalStart(from, *waves_held) && !song::HoldsOriginalStart(to, waves_to);
    if (later || loses)
    {
        std::string warning = std::string(wave ? "The wave goes" : "The waves go") + " out as version " +
                              song::VersionText(to, waves_to) + oldest;
        if (later)
        {
            warning +=
                std::string(wave ? " It held " : " The latest held ") + song::VersionText(from, *waves_held) + ".";
        }
        if (loses)
        {
            warning += " Version " + song::VersionText(to, waves_to) +
                       " holds no original start of a loop. Playback starts each loop at its start either way.";
        }
        warnings.push_back(warning);
    }

    return warnings;
}

} // namespace

std::string NameList(const std::vector<std::string>& names)
{
    constexpr std::size_t kShown = 5;
    const std::size_t shown = names.size() > kShown + 1 ? kShown : names.size();
    std::string list;
    for (std::size_t i = 0; i < shown; i++)
    {
        if (i > 0)
        {
            list += i + 1 == names.size() ? " and " : ", ";
        }
        list += names[i];
    }
    if (shown < names.size())
    {
        list += " and " + std::to_string(names.size() - shown) + " more";
    }

    return list;
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

    return (plural ? "keep their size, " : "keeps its size, ") + SizeText(after);
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
std::string TimeText(double seconds)
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
        std::snprintf(text.data(), text.size(), "Audible at %s: for %.0f\u00a0ms, the export plays %.1f\u00a0dB %s.",
                      TimeText(place.start).c_str(), place.seconds * 1000.0, std::abs(place.db), way);
    }
    else if (place.run)
    {
        std::snprintf(text.data(), text.size(),
                      "Audible at %s: for %.0f\u00a0ms, the export plays more than 6\u00a0dB louder and quieter "
                      "in turn.",
                      TimeText(place.start).c_str(), place.seconds * 1000.0);
    }
    else
    {
        std::snprintf(text.data(), text.size(), "%s from %s to %s: the export plays %.1f\u00a0dB %s.",
                      std::abs(place.db) > 3.0 ? "Audible" : "May be audible", TimeText(place.start).c_str(),
                      TimeText(place.start + place.seconds).c_str(), std::abs(place.db), way);
    }

    return text.data();
}

std::vector<std::string> MeasuredLines(const song::LevelComparison& comparison, const std::string& renderer,
                                       const std::string& departures, double seconds, bool cut, bool random)
{
    // What played, and for how long. Neither side plays the effects on the aux buses: the MIDI file can't hold them.
    std::vector<std::string> lines;
    lines.push_back(renderer + " played " + (cut ? "the first " : "") + TimeText(seconds) +
                    " of the export. Hokuto played the same stretch. Neither played the effects on the aux buses.");
    if (!departures.empty())
    {
        lines.push_back(departures);
    }
    if (random)
    {
        lines.push_back(
            "The sound draws random numbers. Hokuto and the export draw different ones. The differences "
            "below can come from them.");
    }
    if (comparison.windows == 0)
    {
        lines.push_back("Neither played a sound loud enough to measure.");
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
                  "%d\u00a0%% of the sound's 50\u00a0ms windows lie within 1\u00a0dB, and %d\u00a0%% within 3\u00a0dB.",
                  share(comparison.within_1db), share(comparison.within_1db + comparison.from_1_to_3db));
    lines.push_back(text.data());
    if (std::abs(comparison.gain_db) >= 1.0)
    {
        std::snprintf(text.data(), text.size(),
                      "Overall, the export plays %.1f\u00a0dB %s. A listener sets the volume. The measure leaves the "
                      "difference out.",
                      std::abs(comparison.gain_db), comparison.gain_db > 0.0 ? "louder" : "quieter");
        lines.push_back(text.data());
    }
    if (comparison.places.empty())
    {
        lines.push_back("No second lies more than 1\u00a0dB off, and no stretch of 200\u00a0ms more than 6\u00a0dB.");
    }

    return lines;
}

ExportView FileExportView(ExportTarget target, const std::string& sound)
{
    constexpr std::array kTitles = {"Export Sound Archive", "Export MIDI and SoundFont", "Export MIDI and SFZ"};
    constexpr std::array kLines = {"The file's sound archive, rebuilt with the edits", kMidiSoundFontLine,
                                   kMidiSfzLine};
    const auto index = static_cast<std::size_t>(target);
    ExportView view;
    view.title = kTitles.at(index);
    view.targets = {target};
    view.lines = {kLines.at(index)};
    if (!sound.empty())
    {
        view.heading = "Sequence";
        view.source = sound;
    }

    return view;
}

ExportView OtherConsoleExportView(format::Engine from, const std::string& sound)
{
    ExportView view;
    view.title = "Export to Another Console";
    const std::optional<ntdlib::sar::Flavor> flavor = format::FlavorOf(from);
    const std::vector<ntdlib::sar::Flavor> consoles =
        flavor ? song::OtherConsoles(*flavor)
               : std::vector<ntdlib::sar::Flavor>{ntdlib::sar::Flavor::kCafe, ntdlib::sar::Flavor::kNx,
                                                  ntdlib::sar::Flavor::kCtr};
    for (const ntdlib::sar::Flavor console : consoles)
    {
        view.targets.push_back(SongTargetFor(format::EngineOf(console)));
        view.lines.push_back("The sequence's sound in a new .hok file of the " + song::ConsoleName(console));
    }
    if (flavor)
    {
        view.targets.push_back(ExportTarget::kWiiSong);
        view.lines.emplace_back("The sequence's sound in a new .hok file of the Wii");
    }
    view.heading = "Sequence";
    view.source = sound;

    return view;
}

std::optional<ntdlib::sar::Flavor> ConsoleOf(ExportTarget target)
{
    switch (target)
    {
    case ExportTarget::kCtrFile:
        return ntdlib::sar::Flavor::kCtr;
    case ExportTarget::kWiiUFile:
        return ntdlib::sar::Flavor::kCafe;
    case ExportTarget::kSwitchFile:
        return ntdlib::sar::Flavor::kNx;
    default:
        return std::nullopt;
    }
}

std::optional<format::Engine> SongEngineOf(ExportTarget target)
{
    switch (target)
    {
    case ExportTarget::kCtrSong:
        return format::Engine::k3ds;
    case ExportTarget::kWiiUSong:
        return format::Engine::kWiiU;
    case ExportTarget::kSwitchSong:
        return format::Engine::kSwitch;
    case ExportTarget::kWiiSong:
        return format::Engine::kWii;
    default:
        return std::nullopt;
    }
}

ExportTarget SongTargetFor(format::Engine engine)
{
    switch (engine)
    {
    case format::Engine::k3ds:
        return ExportTarget::kCtrSong;
    case format::Engine::kWiiU:
        return ExportTarget::kWiiUSong;
    case format::Engine::kWii:
        return ExportTarget::kWiiSong;
    case format::Engine::kSwitch:
        break;
    }

    return ExportTarget::kSwitchSong;
}

ExportView ItemExportView(const ntdlib::sar::SoundArchive& archive, const song::Item& item)
{
    // The item's file, then the same file for the other consoles. MIDI follows for a sequence, WAV for a wave sound, a
    // wave, a wave archive and a stream, and SoundFont 2 and SFZ for a bank.
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    const std::string format = song::ItemFormat(item.kind, flavor);
    const bool aac = item.kind == song::ItemKind::kStream && IsAacFile(song::StreamFileName(archive, item.index));
    std::string first;
    std::vector<std::pair<ExportTarget, std::string>> others;
    switch (item.kind)
    {
    case song::ItemKind::kSequence:
        first = "The " + format + " file that holds the sequence";
        others = {{ExportTarget::kMidiSoundFont, kMidiSoundFontLine}, {ExportTarget::kMidiSfz, kMidiSfzLine}};
        break;

    case song::ItemKind::kWaveSound:
        first = "The " + format + " file that holds the wave sound";
        others = {{ExportTarget::kWav, "The wave sound's wave as a WAV file, at the sound's pitch"}};
        break;

    case song::ItemKind::kWaveSoundFile:
        first = "The " + format + " file with its wave sounds";
        break;

    case song::ItemKind::kBank:
        first = "The bank's " + format + " file";
        others = {{ExportTarget::kSoundFont, "The bank's instruments as a SoundFont 2 file"},
                  {ExportTarget::kSfz, "The bank's instruments as SFZ files, one for each, in a folder"}};
        break;

    case song::ItemKind::kWaveArchive:
        first = "The wave archive's " + format + " file";
        others = {{ExportTarget::kWav, "Each wave as a WAV file, in 16-bit PCM with its loop, in a folder"}};
        break;

    case song::ItemKind::kWave:
        first = "The wave's " + format + " file";
        others = {{ExportTarget::kWav, "The wave as a WAV file, in 16-bit PCM with its loop"}};
        break;

    case song::ItemKind::kStream:
        first =
            "The stream's " + (aac ? std::string("AAC") : format) + " file, as the game keeps it beside the archive";
        others = {{ExportTarget::kWav, "The stream as a WAV file, in 16-bit PCM with every channel and its loop"}};
        break;
    }

    // The same file for each other console.
    ExportView view;
    view.title = std::string("Export ") + NamesOf(item.kind).title;
    view.targets = {ExportTarget::kItemFile};
    view.lines = {first};
    for (const ntdlib::sar::Flavor console : song::OtherConsoles(flavor))
    {
        view.targets.push_back(console == ntdlib::sar::Flavor::kCtr    ? ExportTarget::kCtrFile
                               : console == ntdlib::sar::Flavor::kCafe ? ExportTarget::kWiiUFile
                                                                       : ExportTarget::kSwitchFile);
        view.lines.push_back(aac ? "The same stream as a " + song::ItemFormat(item.kind, console) + " file for the " +
                                       song::ConsoleName(console) + ", in DSP-ADPCM"
                                 : "The same file for the " + song::ConsoleName(console));
    }

    // The Wii's file. A bank or a wave sound file of the Wii takes its waves along, in an RWAR beside it.
    const std::string wii = song::WiiItemFormat(item.kind);
    view.targets.push_back(ExportTarget::kWiiFile);
    switch (item.kind)
    {
    case song::ItemKind::kSequence:
        view.lines.push_back("The same sequence file as a " + wii + " file for the Wii");
        break;
    case song::ItemKind::kWaveSound:
    case song::ItemKind::kWaveSoundFile:
        view.lines.push_back("The same wave sound file as a " + wii +
                             " file for the Wii, with its waves in a BRWAR file beside it");
        break;
    case song::ItemKind::kBank:
        view.lines.push_back("The same bank as a " + wii +
                             " file for the Wii, with its waves in a BRWAR file beside it");
        break;
    case song::ItemKind::kWaveArchive:
        view.lines.push_back("The same waves as a " + wii + " file for the Wii");
        break;
    case song::ItemKind::kWave:
        view.lines.push_back("The same wave as a " + wii + " file for the Wii");
        break;
    case song::ItemKind::kStream:
        view.lines.push_back("The same stream as a " + wii + " file for the Wii" + (aac ? ", in DSP-ADPCM" : ""));
        break;
    }
    for (const auto& [target, line] : others)
    {
        view.targets.push_back(target);
        view.lines.push_back(line);
    }
    view.heading = NamesOf(item.kind).heading;
    view.source = song::ItemName(archive, item);

    return view;
}

namespace
{

// Adds a target to `view` for the file of an item of `kind` for each of the later consoles, the Wii U first: `before`,
// the format's name, `between`, the console's name, and `after`.
void AddLaterConsoles(ExportView& view, song::ItemKind kind, const std::string& before, const std::string& between,
                      const std::string& after = "")
{
    for (const ntdlib::sar::Flavor console :
         {ntdlib::sar::Flavor::kCafe, ntdlib::sar::Flavor::kNx, ntdlib::sar::Flavor::kCtr})
    {
        view.targets.push_back(console == ntdlib::sar::Flavor::kCtr    ? ExportTarget::kCtrFile
                               : console == ntdlib::sar::Flavor::kCafe ? ExportTarget::kWiiUFile
                                                                       : ExportTarget::kSwitchFile);
        view.lines.push_back(before + song::ItemFormat(kind, console) + between + song::ConsoleName(console) + after);
    }
}

} // namespace

ExportView ItemExportView(const ntdlib::rsar::SoundArchive& archive, const song::Item& item)
{
    const std::string format = song::WiiItemFormat(item.kind);
    ExportView view;
    view.title = std::string("Export ") + NamesOf(item.kind).title;
    view.targets = {ExportTarget::kItemFile};
    switch (item.kind)
    {
    case song::ItemKind::kSequence:
        view.lines = {"The " + format + " file that holds the sequence"};
        AddLaterConsoles(view, item.kind, "The same sequence file as a ", " file for the ");
        view.lines.insert(view.lines.end(), {kMidiSoundFontLine, kMidiSfzLine});
        view.targets.push_back(ExportTarget::kMidiSoundFont);
        view.targets.push_back(ExportTarget::kMidiSfz);
        break;

    // A bank file or a wave sound file of the Wii takes its waves along, in an RWAR beside it. File > Import reads them
    // there.
    case song::ItemKind::kWaveSound:
        view.lines = {"The " + format + " file that holds the wave sound, with its waves in a BRWAR file beside it",
                      "The wave sound's wave as a WAV file, at the sound's pitch"};
        view.targets.push_back(ExportTarget::kWav);
        break;

    case song::ItemKind::kWaveSoundFile:
        view.lines = {"The " + format + " file with its wave sounds, and its waves in a BRWAR file beside it"};
        break;

    case song::ItemKind::kBank:
        view.lines = {"The bank's " + format + " file, with its waves in a BRWAR file beside it"};
        AddLaterConsoles(view, item.kind, "The same bank as a ", " file for the ",
                         ", with its waves in a wave archive file beside it");
        view.lines.insert(view.lines.end(), {"The bank's instruments as a SoundFont 2 file",
                                             "The bank's instruments as SFZ files, one for each, in a folder"});
        view.targets.push_back(ExportTarget::kSoundFont);
        view.targets.push_back(ExportTarget::kSfz);
        break;

    // Wave data that a WAVE block describes goes out as an RWAR of its waves too (song::ItemBytes).
    case song::ItemKind::kWaveArchive:
    case song::ItemKind::kWave:
        {
            const bool wave = item.kind == song::ItemKind::kWave;
            view.lines.push_back(wave ? "The wave's " + format + " file" : "The wave archive's " + format + " file");
            AddLaterConsoles(view, item.kind, wave ? "The same wave as a " : "The same waves as a ", " file for the ");
            view.targets.push_back(ExportTarget::kWav);
            view.lines.push_back(wave ? "The wave as a WAV file, in 16-bit PCM with its loop"
                                      : "Each wave as a WAV file, in 16-bit PCM with its loop, in a folder");
            break;
        }

    // A stream goes to the later consoles too, the Wii U first, and out as a WAV file.
    case song::ItemKind::kStream:
        view.lines = {"The stream's " + format + " file, as the game keeps it beside the archive"};
        AddLaterConsoles(view, item.kind, "The same stream as a ", " file for the ");
        view.targets.push_back(ExportTarget::kWav);
        view.lines.emplace_back("The stream as a WAV file, in 16-bit PCM with every channel and its loop");
        break;
    }
    view.heading = NamesOf(item.kind).heading;
    view.source = song::ItemName(archive, item);

    return view;
}

ExportView InstrumentExportView(const std::string& bank, uint32_t program)
{
    ExportView view;
    view.title = "Export Instrument";
    view.targets = {ExportTarget::kSoundFont, ExportTarget::kSfz};
    view.lines = {"The instrument as a SoundFont 2 file, at its program", "The instrument as an SFZ file, in a folder"};
    view.heading = "Instrument";
    view.source = bank + ", program " + std::to_string(program);

    return view;
}

std::vector<std::string> GrowthWarnings(const format::ArchiveGrowth& growth)
{
    std::vector<std::string> warnings;
    if (growth.after > growth.before)
    {
        warnings.push_back("The archive " + SizeChangeText(growth.before, growth.after) + ".");
    }
    for (const format::GroupGrowth& group : growth.groups)
    {
        warnings.push_back("The files of group " + group.name + " " + SizeChangeText(group.before, group.after, true) +
                           ".");
    }
    if (!warnings.empty())
    {
        warnings.push_back(
            "The game loads each group into its sound heap. The heap's size isn't in the game's files. "
            "A group that outgrows the heap plays silence or crashes the game. Play the archive in the "
            "game or an emulator to check that it fits.");
    }

    return warnings;
}

std::string StreamsNote(const std::vector<std::string>& paths)
{
    const bool one = paths.size() == 1;
    return std::string("The export writes ") + (one ? "a stream" : std::to_string(paths.size()) + " streams") +
           " beside the archive, where the game looks for " + (one ? "it: " : "them: ") + NameList(paths) +
           ". The game holds its other streams.";
}

std::string EngineNote(const std::string& console, const std::string& file_console)
{
    return "The sound plays on the " + console + "'s engine. The files hold the sound's conversion for the " + console +
           ", as Hokuto plays it. The archive keeps the sound for the " + file_console + ".";
}

std::string EngineSoundsNote(const std::vector<std::string>& names, const std::string& file_console)
{
    const bool one = names.size() == 1;
    return std::string("In Hokuto, ") + (one ? "a sound plays" : std::to_string(names.size()) + " sounds play") +
           " on another console's engine: " + NameList(names) + ". The archive keeps " + (one ? "it" : "them") +
           " for the " + file_console + ". File > Export > Another Console… writes a sound's conversion.";
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
    const std::string folder = std::string("into the ") + song::kUserManagementFolder + " folder beside ";
    std::string where = beside.empty() ? folder + "the archive: " : "beside the archive: " + NameList(beside);
    if (!beside.empty() && !inside.empty())
    {
        where += ", and " + folder + "it: ";
    }
    if (!inside.empty())
    {
        where += NameList(inside);
    }

    return std::string("The export writes ") + (one ? "a group file" : std::to_string(names.size()) + " group files") +
           " " + where + ". When a SARC archive beside the archive, or beside the file that Hokuto opened, holds " +
           (one ? "the group's file" : "a group's file") +
           ", the new group file goes into a copy of the SARC archive instead. The copy goes beside the archive. "
           "A game can also keep its group files in another folder. Put " +
           (one ? "the file where the game keeps it." : "each file where the game keeps it.");
}

std::string UnknownGroupNote(std::size_t index)
{
    return "The edits change group file " + std::to_string(index + 1) +
           ", and Hokuto can't tell which group it holds. The export leaves it out.";
}

std::vector<std::string> MidiWarnings(const song::Sequence& sequence, const BusEffects& effects,
                                      const std::array<float, format::kAuxBuses>& volumes, ExportTarget target)
{
    std::vector<std::string> warnings;
    if (!sequence.read_variables.empty())
    {
        warnings.emplace_back(
            "MIDI holds no variables or conditions. The files hold the path that the variables' "
            "starting values and their changes give.");
    }

    // The Switch's converter sends FX A to the synth's reverb with CC91. The sends to FX B and FX C reach the mix dry.
    const auto has_effects = [](const std::vector<format::Effect>& bus)
    {
        return !bus.empty();
    };
    if (std::any_of(effects.begin(), effects.end(), has_effects))
    {
        warnings.push_back(std::string(target == ExportTarget::kMidiSoundFont ? "SoundFont 2" : "SFZ") +
                           " holds no effects. The MIDI file sends to the synth's reverb (CC91) as the tracks send "
                           "to FX A. The sends to FX B and FX C play dry.");
    }

    // The converter sends to each bus as though its FX channel's fader stood at 0 dB.
    const auto not_unity = [](float volume)
    {
        return volume != 1.0f;
    };
    if (std::any_of(volumes.begin(), volumes.end(), not_unity))
    {
        warnings.emplace_back(
            "MIDI holds no volumes of the FX channels. The files send to each bus as though its "
            "fader stood at 0 dB.");
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
        std::vector<std::string> names;
        for (const uint32_t sound : song::SoundsSharingFile(archive, item.index))
        {
            names.push_back(archive.Sounds()[sound].name);
        }
        if (!names.empty())
        {
            warnings.push_back(std::string("The file holds the ") + (sequence_file ? "sequences" : "wave sounds") +
                               " of " + std::to_string(names.size()) +
                               (names.size() == 1 ? " more sound too: " : " more sounds too: ") + NameList(names) +
                               ".");
        }
    }

    // A wave sound's WAV file holds the decoded wave of its first note, at the sound's pitch. The consoles play no
    // other note of a wave sound: they read none of the note events of its tracks.
    if (item.kind == song::ItemKind::kWaveSound && target == ExportTarget::kWav)
    {
        try
        {
            const song::WaveSoundNote note = song::WaveSoundNoteOf(archive, item.index);
            if (note.panned)
            {
                warnings.push_back("The WAV file leaves out the wave sound's pan.");
            }
            if (note.shaped)
            {
                warnings.push_back(
                    "The wave sound's envelope fades it in or down. The WAV file leaves the envelope out.");
            }
            const std::size_t channels = song::WaveChannels(archive, note.wave_archive, note.wave);
            if (channels > 2)
            {
                warnings.push_back("The wave has " + std::to_string(channels) +
                                   " channels. The console plays only the first two. The WAV file holds those.");
            }
        }
        catch (const std::exception&)
        {
            // The work gives the reason.
        }
    }

    // A stream sound plays its stream's channels as tracks, each with a volume and a pan. A WAV file holds the channels
    // as they are.
    if (item.kind == song::ItemKind::kStream && target == ExportTarget::kWav && item.index < archive.Sounds().size())
    {
        const ntdlib::sar::StreamSoundInfo& stream = archive.Sounds()[item.index].stream;
        const auto plain = [](const ntdlib::sar::StreamTrackInfo& track)
        {
            return track.volume == 127 && track.pan == 64;
        };
        if (stream.tracks.size() > 1 || !std::ranges::all_of(stream.tracks, plain))
        {
            warnings.push_back("The sound plays the stream's channels as " + std::to_string(stream.tracks.size()) +
                               (stream.tracks.size() == 1 ? " track" : " tracks") +
                               ", with their volumes and pans. The WAV file holds the channels as they are.");
        }
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
        std::size_t wide = 0;
        std::size_t channels = 0;
        const uint32_t first = item.kind == song::ItemKind::kWave ? item.wave : 0;
        const uint32_t end = item.kind == song::ItemKind::kWave
                                 ? item.wave + 1
                                 : static_cast<uint32_t>(song::WaveCountOf(archive, item.index));
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
                // `found` gives the reason.
            }
        }
        if (item.kind == song::ItemKind::kWave && wide > 0)
        {
            warnings.push_back("The wave has " + std::to_string(channels) +
                               " channels. The console plays only the first two. The WAV file holds those.");
        }
        else if (wide > 0)
        {
            warnings.push_back(std::to_string(wide) + (wide == 1 ? " wave has" : " waves have") +
                               " more than two channels. The console plays only the first two of each. The WAV "
                               "files hold those.");
        }
    }
    warnings.insert(warnings.end(), found.begin(), found.end());

    return warnings;
}

std::vector<std::string> StreamConversionWarnings(std::span<const uint8_t> stream, ntdlib::sar::Flavor to)
{
    // An AAC stream goes in DSP-ADPCM. A stream of the Wii keeps its tracks.
    if (song::IsAdts(stream))
    {
        return {"The AAC stream goes in DSP-ADPCM, from the samples of the system's AAC decoder."};
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
        warnings.push_back("The stream goes out as version " + song::VersionText(to, version) +
                           ", a version that the " + song::ConsoleName(to) + "'s games read. It held " +
                           song::VersionText(later->flavor, later->version) + ". Its samples stay the same.");
    }
    if (to != ntdlib::sar::Flavor::kCtr || from_ctr)
    {
        return warnings;
    }
    if (loop)
    {
        warnings.emplace_back(
            "The 3DS's tools encode the loop's first 128 samples after a looped stream's end. The converted stream "
            "holds none of them. No 3DS game has played such a stream.");
    }
    if (!tracks)
    {
        warnings.emplace_back(
            "Before version 2.3.0.0 of a 3DS archive, a stream sound plays the tracks of its stream file. The "
            "converted stream holds none. Replace… gives a stream the present stream's tracks.");
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
                               " channels. The game's build of NW4R SND plays only the first two. The WAV file holds "
                               "every channel.");
        }
        return warnings;
    }
    const auto plain = [](const ntdlib::rsar::StreamFile::Track& track)
    {
        return track.volume == 127 && track.pan == 64;
    };
    if (file.tracks.size() > 1 || !std::ranges::all_of(file.tracks, plain))
    {
        warnings.push_back("The sound plays the stream's channels as " + std::to_string(file.tracks.size()) +
                           (file.tracks.size() == 1 ? " track" : " tracks") +
                           ", with their volumes and pans. The WAV file holds the channels as they are.");
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
        std::vector<std::string> names;
        for (const uint32_t other : song::SoundsSharingFile(archive, item.index))
        {
            names.push_back(archive.Sounds()[other].name);
        }
        if (!names.empty())
        {
            warnings.push_back(std::string("The file holds the ") +
                               (item.kind == song::ItemKind::kSequence ? "sequences" : "wave sounds") + " of " +
                               std::to_string(names.size()) +
                               (names.size() == 1 ? " more sound too: " : " more sounds too: ") + NameList(names) +
                               ".");
        }
    }

    // A wave sound's WAV file holds the decoded wave of its first note, at the sound's pitch. NW4R SND plays no other
    // note of a wave sound.
    if (item.kind == song::ItemKind::kWaveSound && target == ExportTarget::kWav)
    {
        try
        {
            const song::WaveSoundNote note = song::WaveSoundNoteOf(archive, item.index);
            if (note.panned)
            {
                warnings.push_back("The WAV file leaves out the wave sound's pan.");
            }
            if (note.shaped)
            {
                warnings.push_back(
                    "The wave sound's envelope fades it in or down. The WAV file leaves the envelope out.");
            }
            const std::size_t channels = song::WaveChannels(archive, note.wave_archive, note.wave);
            if (channels > 2)
            {
                warnings.push_back("The wave has " + std::to_string(channels) +
                                   " channels. The console plays only the first two. The WAV file holds those.");
            }
        }
        catch (const std::exception&)
        {
            // The work gives the reason.
        }
    }

    // NW4R SND plays only the first two channels of a wave. A WAV file holds those.
    if (target == ExportTarget::kWav &&
        (item.kind == song::ItemKind::kWave || item.kind == song::ItemKind::kWaveArchive))
    {
        std::size_t wide = 0;
        std::size_t channels = 0;
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
                // `found` gives the reason.
            }
        }
        if (item.kind == song::ItemKind::kWave && wide > 0)
        {
            warnings.push_back("The wave has " + std::to_string(channels) +
                               " channels. The console plays only the first two. The WAV file holds those.");
        }
        else if (wide > 0)
        {
            warnings.push_back(std::to_string(wide) + (wide == 1 ? " wave has" : " waves have") +
                               " more than two channels. The console plays only the first two of each. The WAV "
                               "files hold those.");
        }
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

    return {"A SoundFont 2 file holds at most 65,535 generators, modulators and zones. The instruments go out as " +
            std::to_string(runs.size()) + " files, for programs " + NameList(runs) +
            ". Each file's name ends in its programs, such as " + song::SoundFontNames(bank, stem)[0] + "."};
}

ExportChoice DrawExportWindow(ExportView& view, std::optional<ImVec2>& place, const std::vector<std::string>* warnings,
                              const MeasuredView* measured)
{
    if (!ImGui::IsPopupOpen(kExportWindow))
    {
        ImGui::OpenPopup(kExportWindow);
    }

    // A window over the middle of the main window, in the style of Studio Setup. It keeps the clicks from the windows
    // under it while it's open. The user can drag it by its title bar. It then opens where the drag left it until
    // Hokuto quits: `place` keeps its centre. The warnings and the measure make the window taller after it opens. The
    // window keeps its top and grows down. It moves up only to stay inside the main window. The controls under the
    // mouse then stay where they were. While the drag goes on, the drag places the window.
    const std::string title = view.title + kExportWindow;
    const ImGuiWindow* window = ImGui::FindWindowByName(title.c_str());
    const bool dragged = window != nullptr && ImGui::GetCurrentContext()->MovingWindow == window;
    const ImVec2 size = window != nullptr ? window->Size : ImVec2(0.0f, 0.0f);
    ImVec2 centre = CentreInside(place.value_or(ImGui::GetMainViewport()->GetCenter()), size, Px(kMargin));
    if (view.top)
    {
        centre = CentreInside(ImVec2(centre.x, *view.top + size.y * 0.5f), size, Px(kMargin));
    }
    if (!dragged)
    {
        ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    // The window takes the width of its longest target where that passes 520 points, up to the main window's width. The
    // menu of targets and the chosen target then stay inside it.
    float width = Px(520.0f);
    if (view.lines.size() > 1)
    {
        float widest = 0.0f;
        for (const std::string& line : view.lines)
        {
            widest = std::max(widest, ImGui::CalcTextSize(line.c_str()).x);
        }
        const float frame = ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
        width = std::max(width, std::ceil(widest + frame + Px(theme::kDialogPadding).x * 2.0f));
        width = std::min(width, std::max(Px(520.0f), ImGui::GetMainViewport()->WorkSize.x - Px(kMargin) * 2.0f));
    }
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    const bool visible = ImGui::BeginPopupModal(title.c_str(), &open, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (!visible)
    {
        return ExportChoice::kCancel;
    }

    // A drag that moves the window gives its new place. A click on the title bar that moves nothing keeps the old one.
    // The window's corner lies on whole pixels. The first frame that shows the window gives its top.
    if (dragged)
    {
        const ImVec2 corner = ImGui::GetWindowPos();
        const ImVec2 shown = ImGui::GetWindowSize();
        const ImVec2 moved(corner.x + shown.x * 0.5f, corner.y + shown.y * 0.5f);
        if (std::abs(moved.x - centre.x) > 1.0f || std::abs(moved.y - centre.y) > 1.0f)
        {
            place = moved;
        }
    }
    if (dragged || (!view.top && !ImGui::IsWindowAppearing()))
    {
        view.top = ImGui::GetWindowPos().y;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return ExportChoice::kCancel;
    }

    // Each window opens with its warnings at the top, and the measure starts at its top when it comes in. The panes
    // keep their names from one window to the next, and Dear ImGui keeps their scroll with the names. The window's
    // storage says whether the last frame showed the measure.
    const bool appearing = ImGui::IsWindowAppearing();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID measure_shown = ImGui::GetID("measure shown");
    const bool measure_came = measured != nullptr && (appearing || !storage->GetBool(measure_shown, false));
    storage->SetBool(measure_shown, measured != nullptr);

    // What the export writes, from which sequence or item. The headings follow Studio Setup's. The text wraps at the
    // window's edge. A menu offers the targets when there are several. A choice of another target restarts the check.
    ImGui::PushTextWrapPos(0.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto heading = [draw](const char* text)
    {
        DrawCaps(draw, ImGui::GetCursorScreenPos(), theme::kTextMuted, text);
        ImGui::Dummy(CapsSize(text));
    };

    ExportChoice choice = ExportChoice::kNone;
    heading("Target");
    if (view.lines.size() > 1)
    {
        // A target too long for the menu's box ends with an ellipsis before the arrow. The window takes the longest
        // target's width where the main window allows it.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float room = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - style.FramePadding.x -
                           style.ItemInnerSpacing.x;
        const std::string chosen = FittedText(view.lines[view.chosen], room);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##target", chosen.c_str()))
        {
            for (std::size_t i = 0; i < view.lines.size(); i++)
            {
                if (ImGui::Selectable(view.lines[i].c_str(), i == view.chosen) && i != view.chosen)
                {
                    view.chosen = i;
                    choice = ExportChoice::kTarget;
                }
            }
            ImGui::EndCombo();
        }
    }
    else
    {
        ImGui::TextUnformatted(view.lines[view.chosen].c_str());
    }
    if (!view.option.empty())
    {
        ImGui::BeginDisabled(warnings == nullptr);
        if (CheckBox(view.option.c_str(), &view.option_on))
        {
            choice = ExportChoice::kOption;
        }
        ImGui::EndDisabled();
        ItemTooltip(view.option_tooltip);
    }
    if (!view.heading.empty())
    {
        ImGui::Spacing();
        heading(view.heading.c_str());
        TextWrappedAtSpaces(view.source);
    }

    // The warnings scroll once they fill the window's height. A child window wraps its text by itself. A refused target
    // gives its reason there instead.
    const bool refused = !view.refused.empty();
    ImGui::Spacing();
    heading(refused ? "Refused" : "Warnings");
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, Px(280.0f)));
    if (appearing)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##warnings", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    ImGui::PushTextWrapPos(0.0f);
    if (refused)
    {
        TextWrappedAtSpaces(view.refused);
    }
    else if (warnings == nullptr || warnings->empty())
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted),
                           warnings == nullptr ? "Checking…" : "None");
    }
    for (std::size_t i = 0; !refused && warnings != nullptr && i < warnings->size(); i++)
    {
        ImGui::Bullet();
        TextWrappedAtSpaces((*warnings)[i]);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    // The measure of an export to MIDI comes in after the warnings. Each place that it names has Play buttons under its
    // line: the export's render there, and Hokuto's playback. The measure takes the height that the main window has
    // left, less the buttons and the margin. Its places then show without a scroll where they fit. In a short main
    // window, the measure keeps 120 points.
    if (measured != nullptr)
    {
        ImGui::Spacing();
        heading("Measured");
        // Below the measure: the spacing before the buttons, the buttons and the window's padding.
        const float below =
            ImGui::GetStyle().ItemSpacing.y * 3.0f + ImGui::GetFrameHeight() + Px(theme::kDialogPadding.y);
        const float room = ImGui::GetMainViewport()->WorkSize.y - Px(kMargin) * 2.0f - ImGui::GetCursorPosY() - below;
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, std::max(Px(120.0f), room)));
        if (measure_came)
        {
            ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
        }
        ImGui::BeginChild("##measured", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
        ImGui::PushTextWrapPos(0.0f);
        if (!measured->status.empty())
        {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s", measured->status.c_str());
        }
        for (const std::string& line : measured->lines)
        {
            ImGui::Bullet();
            TextWrappedAtSpaces(line);
        }
        for (std::size_t i = 0; i < measured->places.size(); i++)
        {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Bullet();
            TextWrappedAtSpaces(measured->places[i].line);
            // The buttons start where the line's text starts, after the bullet.
            const float indent = ImGui::GetTreeNodeToLabelSpacing();
            ImGui::Indent(indent);
            if (ImGui::Button("Play Export"))
            {
                view.play = i;
                view.play_hokuto = false;
                choice = ExportChoice::kPlay;
            }
            ImGui::SameLine();
            if (ImGui::Button("Play Hokuto"))
            {
                view.play = i;
                view.play_hokuto = true;
                choice = ExportChoice::kPlay;
            }
            ImGui::Unindent(indent);
            ImGui::PopID();
        }
        if (!measured->closing.empty())
        {
            ImGui::Bullet();
            TextWrappedAtSpaces(measured->closing);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    }
    ImGui::PopTextWrapPos();

    // Cancel and Export… sit at the bottom right, with Export… on the right. macOS places a dialog's buttons the same
    // way.
    ImGui::Spacing();
    ImGui::Spacing();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Cancel").x + ImGui::CalcTextSize("Export…").x +
                          style.FramePadding.x * 4.0f + style.ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons);
    if (ImGui::Button("Cancel"))
    {
        ImGui::CloseCurrentPopup();
        choice = ExportChoice::kCancel;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(warnings == nullptr || refused || choice == ExportChoice::kTarget);
    if (PrimaryButton("Export…"))
    {
        choice = ExportChoice::kExport;
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();

    return choice;
}

void CloseExportWindow()
{
    if (ImGui::IsPopupOpen(kExportWindow) && ImGui::BeginPopupModal(kExportWindow))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace hokuto::app
