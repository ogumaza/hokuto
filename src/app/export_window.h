// SPDX-License-Identifier: MIT

// The Export window of File > Export and of the browser's Export…, and the warnings that it lists. The window shows
// what an export writes, from which sequence or item, and the warnings, before anything is written.
// export_warnings.h declares the warnings.

#pragma once

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"

#include "format/engine.h"
#include "song/items.h"

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
    kDsSong,     // of the DS
    kItemFile,
    kCtrFile,    // an item's file for the 3DS
    kWiiUFile,   // for the Wii U
    kSwitchFile, // for the Switch
    kWiiFile,    // for the Wii
    kWav,
    kSoundFont,
    kSfz,
    kAiff, // a GBA voice's sample as an AIFF file
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

// The view of File > Export's export to `target`: the sound archive, or MIDI with SoundFont 2 or SFZ. `sound` names the
// sequence of a MIDI export.
ExportView FileExportView(ExportTarget target, const std::string& sound);

// The view of File > Export > Another Console…: the sequence's sound in a new file of each console other than the
// console of `from`, the file's engine. A sound of the 3DS, the Wii U or the Switch goes to the other two, the nearest
// first (song::OtherConsoles). A sound of another engine goes to each of the three: the Wii U first, then the Switch
// and the 3DS. The Wii follows, unless the sound comes from the Wii. NITRO-Composer of the DS comes last, unless the
// sound plays on it. `sound` names the sequence.
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

// The view of the export of a GBA ROM's voice group that `voice_group` names: a SoundFont 2 file of the voices that the
// songs select, with each voice at its program, or the group as assembly. With `program`, the voice at that program
// alone. With `sample` too, the voice's sample as the ROM contains it or as an AIFF file.
ExportView VoiceGroupExportView(const std::string& voice_group, const std::optional<uint32_t>& program,
                                bool sample = false);

// The view of the export of a sequence of a DS archive that `sequence` names: MIDI with SoundFont 2, MIDI with SFZ, and
// the sequence's file.
ExportView DsSequenceExportView(const std::string& sequence);

// The view of the export of a sound of DSE that `sound` names: MIDI with SoundFont 2, and MIDI with SFZ. A song is also
// exported as its file (`song`). A sound effect's file contains the other sound effects of its bank too.
ExportView DseSoundExportView(const std::string& sound, bool song);

// The window of a GBA song that the browser exports: as MIDI with SoundFont 2, or as assembly.
ExportView GbaSongExportView(const std::string& song);

// The view of the export of a song of another driver of the GBA, such as Konami's driver: MIDI with SoundFont 2, and
// with `sfz`, MIDI with SFZ too.
ExportView DriverSongExportView(const std::string& song, bool sfz);

// The view of the export of the samples of such a driver: all of them as a SoundFont 2 file for kBank, as a folder of
// WAV files for kWaveArchive, and one of them as a WAV file or a SoundFont 2 file otherwise. With `sfz`, a SoundFont's
// samples go out as SFZ files too.
ExportView DriverSamplesExportView(song::ItemKind kind, const std::string& source, bool sfz);

// The view of the export of an item of a DS archive of `kind` that `source` names: a bank, a wave archive, a wave or a
// stream. Each is exported as its file. A bank's instruments go out as SoundFont 2 and SFZ, and the waves and a stream
// as WAV files.
ExportView DsItemExportView(song::ItemKind kind, const std::string& source);

// The view of the export of an item of DSE's files of `kind` that `source` names: a bank, a bank with samples or a
// wave. A bank is exported as its file, and its instruments as SoundFont 2 and SFZ. A bank with samples is exported as
// its file, and its waves as WAV files. A wave has no file by itself and is exported as a WAV file.
ExportView DseItemExportView(song::ItemKind kind, const std::string& source);

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
