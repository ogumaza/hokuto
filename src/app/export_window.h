// SPDX-License-Identifier: MIT

// The Export window of File > Export and of the browser's Export…, and the warnings that it lists. The window shows
// what an export writes, from which sequence or item, and the warnings, before anything is written.

#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sound_build.h"

#include "app/effect_editor.h"
#include "format/archive.h"
#include "song/audibility.h"
#include "song/items.h"
#include "song/sequence.h"

namespace hokuto::app
{

// What an export writes. File > Export writes the file's sound archive, or the shown sequence as MIDI with a SoundFont
// or with SFZ files. It also writes the shown sequence's sound into a new file of another console. The browser's
// Export… writes an item's file. The file also goes out as a file of another console among the 3DS, the Wii U, the
// Switch and the Wii. A wave also goes out as a WAV file, and a wave archive as a WAV file for each wave, in a folder.
// A bank's instruments go out as a SoundFont, or as several for a bank past the format's limits, or as SFZ files in a
// folder.
enum class ExportTarget
{
    kSoundArchive,
    kMidiSoundFont,
    kMidiSfz,
    kCtrSong,    // the sequence's sound in a new file of the 3DS
    kWiiUSong,   // of the Wii U
    kSwitchSong, // of the Switch
    kWiiSong,    // of the Wii
    kItemFile,
    kCtrFile,    // an item's file for the 3DS
    kWiiUFile,   // for the Wii U
    kSwitchFile, // for the Switch
    kWiiFile,    // for the Wii
    kWav,
    kSoundFont,
    kSfz,
};

// The console among the 3DS, the Wii U and the Switch that `target` writes an item's file for, or nothing for a target
// of another kind, the Wii's file included.
std::optional<ntdlib::sar::Flavor> ConsoleOf(ExportTarget target);

// The engine of the console that `target` sends the sequence's sound to, or nothing for a target of another kind.
std::optional<format::Engine> SongEngineOf(ExportTarget target);

// The target that sends the sequence's sound to the console of `engine`.
ExportTarget SongTargetFor(format::Engine engine);

// What the user did in the Export window.
enum class ExportChoice
{
    kNone,
    kCancel, // Cancel, Escape or the window's close button
    kExport, // Export…, once the warnings are in
    kTarget, // another target in the menu of targets
    kOption, // the check box of the export's option (ExportView::option_on)
    kPlay,   // a Play button of a place that the measure names (ExportView::play)
};

// What the Export window shows: its title, the targets that it offers with a line for each, the chosen target, and the
// sequence or item that the export comes from, under its heading.
struct ExportView
{
    std::string title;
    std::vector<ExportTarget> targets;
    std::vector<std::string> lines;
    std::size_t chosen = 0;
    std::string heading;
    std::string source;

    // An option of the chosen target, such as the mono waves of a song for the 3DS: its check box's label, its tooltip
    // and its state. An empty label shows no option.
    std::string option;
    std::string option_tooltip;
    bool option_on = false;

    // The top of the window once it has shown. The window keeps it while it grows.
    std::optional<float> top;

    // Why Hokuto can't write the chosen target, or nothing. The window shows the reason in place of the warnings, and
    // Export… stays off. The other targets stay in the menu.
    std::string refused;

    // The place whose Play button the user clicked, and which of its clips: the export's or Hokuto's playback.
    std::size_t play = 0;
    bool play_hokuto = false;
};

// A place that the measure of an export names: its line, and clips of both renders around it, interleaved stereo at
// `rate`. The window's Play buttons play them.
struct MeasuredPlace
{
    std::string line;
    std::vector<float> export_clip;
    std::vector<float> hokuto_clip;
    int rate = 0;
};

// What the Export window's Measured section shows of an export to MIDI or to the other console: the lines, then the
// places, then the closing line, such as a note that other players play the files otherwise. While the measure runs,
// `status` says so. A measure that failed gives the reason there.
struct MeasuredView
{
    std::string status;
    std::vector<std::string> lines;
    std::vector<MeasuredPlace> places;
    std::string closing;
};

// The lines of the Measured section: `renderer` played the export, and the measure compared it with Hokuto's playback
// over `seconds` (song::CompareLevels). `departures` names the renderer's known departures from the format's
// specification, and can be empty. `cut` says that the measure stopped before the end of the export. `random` says that
// the sound draws random numbers (edit::DrawsRandom).
std::vector<std::string> MeasuredLines(const song::LevelComparison& comparison, const std::string& renderer,
                                       const std::string& departures, double seconds, bool cut, bool random);

// The line of a place that the measure names: how audible it is, where it lies, and how much louder or quieter the
// export plays there. A run of windows more than 6 dB off, or a second more than 3 dB off, is audible. A second from 1
// to 3 dB off may be audible.
std::string PlaceLine(const song::LevelPlace& place);

// The names of `names` in a list for a warning: "A", "A and B", "A, B and C", or the first five and how many more.
std::string NameList(const std::vector<std::string>& names);

// A size for a warning: bytes up to a kilobyte, then kilobytes or megabytes to a tenth.
std::string SizeText(std::size_t bytes);

// The end of a sentence about one thing that says how its size changes from `before` to `after` bytes: "grows by
// 2.1 KB, to 38.5 MB", "shrinks by 96 bytes, to 1.1 MB" or "keeps its size, 1.1 MB". With `plural`, the words follow a
// plural subject, such as "grow by 2.1 KB, to 38.5 MB". The change itself shows where the rounding would hide it.
std::string SizeChangeText(std::size_t before, std::size_t after, bool plural = false);

// The sizes of two files for a sentence that compares them, such as "The present file takes 2.1 KB. The new one takes
// 2.3 KB.": SizeText's, or each size in bytes where two different sizes would read the same in SizeText.
std::pair<std::string, std::string> SizeTexts(std::size_t first, std::size_t second);

// The view of File > Export's export to `target`: the sound archive, or MIDI with SoundFont 2 or SFZ. `sound` names the
// sequence of a MIDI export.
ExportView FileExportView(ExportTarget target, const std::string& sound);

// The view of File > Export > Another Console…: the sequence's sound in a new file of each console other than the
// console of `from`, the file's engine. A sound of the 3DS, the Wii U or the Switch goes to the other two, the nearest
// first (song::OtherConsoles), and then to the Wii. A sound of the Wii goes to each of the three: the Wii U first, then
// the Switch and the 3DS. `sound` names the sequence.
ExportView OtherConsoleExportView(format::Engine from, const std::string& sound);

// The view of the export of `item` of `archive`. The item's file is the first target. The same file for each of the
// other two consoles among the 3DS, the Wii U and the Switch follows, the nearest first (song::OtherConsoles), and then
// for the Wii. A sequence offers MIDI with SoundFont 2 and with SFZ too, a wave sound, a wave, a wave archive and a
// stream WAV, and a bank SoundFont 2 and SFZ.
ExportView ItemExportView(const ntdlib::sar::SoundArchive& archive, const song::Item& item);

// The view of the export of `item` of the Wii's `archive`. The item's file is the first target. A bank file or a wave
// sound file takes its waves along, in an RWAR beside it. Wave data that a WAVE block describes goes out as an RWAR,
// and each of its waves as an RWAV file (song::ItemBytes). A sequence, a bank, a wave archive, a wave and a stream also
// go out as a file of each later console, the Wii U first. A sequence offers MIDI with SoundFont 2 and with SFZ too, a
// bank SoundFont 2 and SFZ, and a wave sound, a wave, a wave archive and a stream WAV.
ExportView ItemExportView(const ntdlib::rsar::SoundArchive& archive, const song::Item& item);

// The view of the export of the instrument at program `program` of the bank that `bank` names: the instrument as a
// SoundFont 2 file or as an SFZ file.
ExportView InstrumentExportView(const std::string& bank, uint32_t program);

// The warnings about an archive that grew in an export: the archive, then the files of each group that grew. A game
// loads a group's files into its sound heap, and no file gives the heap's size.
std::vector<std::string> GrowthWarnings(const format::ArchiveGrowth& growth);

// The note of an export of the archive that writes streams beside it, at `paths`: the streams that the game lacks.
std::string StreamsNote(const std::vector<std::string>& paths);

// The note of an export of the archive that writes group files beside it, under `names`: the group files that the edits
// changed. A name that starts with the userManagementFiles folder's name and a slash goes into that folder
// (song::PlaceGroupFiles).
std::string GroupFilesNote(const std::vector<std::string>& names);

// The warning of an export that leaves out group file `index`, counted from 0: an edit changed it, and Hokuto couldn't
// find its group.
std::string UnknownGroupNote(std::size_t index);

// The note of an export to MIDI of a sound that plays on the engine of `console`, another console than the file's
// `file_console`. The files hold the sound's conversion for that console.
std::string EngineNote(const std::string& console, const std::string& file_console);

// The note of an export of the archive whose sounds `names` play on other consoles' engines in Hokuto. The archive
// keeps them for `file_console`.
std::string EngineSoundsNote(const std::vector<std::string>& names, const std::string& file_console);

// What the MIDI file and the instruments of `target` can't hold of `sequence`, beside what the converter reports: the
// paths that its variables choose, the effects on the aux buses, and the buses' volumes.
std::vector<std::string> MidiWarnings(const song::Sequence& sequence, const BusEffects& effects,
                                      const std::array<float, format::kAuxBuses>& volumes, ExportTarget target);

// The warnings of an export of `item` of `archive` to `target`: the other sounds whose sequences a sequence file holds,
// or whose wave sounds a wave sound file holds, and what a WAV file leaves out: channels past the first two, and a wave
// sound's other notes, its pan and its envelope. A file for another console takes the oldest versions that the
// console's games hold, and a warning gives a version older than the file's. `found` lists what the export's work
// found: each wave of a wave archive that a WAV export leaves out, with the reason, or what the converter reports of a
// bank's instruments.
std::vector<std::string> ItemWarnings(const ntdlib::sar::SoundArchive& archive, const song::Item& item,
                                      ExportTarget target, const std::vector<std::string>& found);

// The warnings of an export of the stream file `stream` as a file of the console of `to`. A stream of a later version
// than the one that goes to that console goes out in that version. A stream that goes to the 3DS from another console
// holds none of the samples that the 3DS's tools encode past a looped stream's end. Before version 2.3.0.0 of a 3DS
// archive, a stream sound plays the tracks of its stream file, and the converted stream may hold none. A stream of the
// Wii keeps its tracks. A stream that ntdlib can't read gives no warnings: the conversion fails with the reason.
std::vector<std::string> StreamConversionWarnings(std::span<const uint8_t> stream, ntdlib::sar::Flavor to);

// The warnings of an export of `stream`, a stream file of the Wii, as a WAV file. `build` of NW4R SND plays it. A build
// before March 2008 plays only the first two channels. A later build plays the stream's tracks with their volumes and
// pans. The WAV file holds every channel as it is.
std::vector<std::string> WiiStreamWavWarnings(std::span<const uint8_t> stream, const ntdlib::SndBuild& build);

// The same for `item` of the Wii's `archive`: the other sounds whose sequences or wave sounds its file holds, and what
// a WAV file leaves out: channels past the first two, and a wave sound's pan and envelope. NW4R SND plays only a wave
// sound's first note.
std::vector<std::string> ItemWarnings(const ntdlib::rsar::SoundArchive& archive, const song::Item& item,
                                      ExportTarget target, const std::vector<std::string>& found);

// The lines that the Export window lists for an export of SFZ files (`sfz`) or SoundFont 2 files: those of a
// conversion's report (song::ConversionReport) or a bank's warnings from ntdlib's converter. A line that names only the
// other format stays out (song::ReportFor), and each line becomes a sentence.
std::vector<std::string> ConverterWarnings(std::vector<std::string> lines, bool sfz);

// The warning of an export of a bank's instruments to SoundFont 2 when they need several files, or nothing. `stem` is
// the name that the dialog offers, without its extension.
std::vector<std::string> SoundFontWarnings(const ntdlib::sequence_export::BankConversion& bank,
                                           const std::string& stem);

// Draws the Export window, a modal popup. The first call opens it. The window lies over the middle of the main window
// until the user drags it by its title bar. `place` then keeps the window's centre, and the window opens there again.
// The window stays inside the main window. `warnings` is null while the export is still being checked. Export… stays
// disabled until they're in. With more than one target, a menu offers them. A choice of another target sets
// `view.chosen` and returns kTarget. The check box of the target's option follows the menu. It stays disabled while the
// export is still being checked. A click sets `view.option_on` and returns kOption. `measured` adds the Measured
// section of an export to MIDI, or nothing when null.
// A click on a place's Play button sets `view.play` and `view.play_hokuto` and returns kPlay. The window closes itself
// when the user cancels.
ExportChoice DrawExportWindow(ExportView& view, std::optional<ImVec2>& place, const std::vector<std::string>* warnings,
                              const MeasuredView* measured = nullptr);

// Closes the Export window if it's open.
void CloseExportWindow();

} // namespace hokuto::app
