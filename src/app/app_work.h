// SPDX-License-Identifier: MIT

// The program's types for its work and its windows (app.h): the settings, the dialogs, the work of an export, an
// opening, an edit and a preview, and what waits for a later frame. App keeps them as its members.

#pragma once

#include <SDL3/SDL.h>
#include <imgui.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"

#include "app/audio_output.h"
#include "app/bank_editor.h"
#include "app/effect_editor.h"
#include "app/export_window.h"
#include "app/item_views.h"
#include "app/key_edits.h"
#include "app/list_editor.h"
#include "app/list_edits.h"
#include "app/menus.h"
#include "app/message_queue.h"
#include "app/replace_window.h"
#include "app/song_view.h"
#include "app/sound_section.h"
#include "app/transport.h"
#include "app/views.h"
#include "app/wave_window.h"
#include "audio/frame_ring.h"
#include "audio/render_worker.h"
#include "edit/bytecode.h"
#include "edit/files.h"
#include "edit/history.h"
#include "edit/new_items.h"
#include "edit/other_console.h"
#include "edit/replace.h"
#include "edit/sound_font_imports.h"
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/catch_up.h"
#include "song/driver.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/items.h"
#include "song/played.h"
#include "song/player.h"
#include "song/sequence.h"
#include "song/sequence_reader.h"

namespace hokuto::app
{

// How the tracks take their colours: each the next colour of the rainbow, all one grey, or each the colour that the
// user gave it in the file. A track without one takes its rainbow colour.
enum class TrackColours
{
    kRainbow,
    kStandard,
    kCustom,
};

// The settings that the program keeps between runs. They share a file with Dear ImGui's layout, in the user's
// settings folder. The window's size is its size when it isn't maximised, in SDL3's window coordinates. The
// layout's version is the version of the default layout that the saved one started from. The arrangement keeps the
// track list's width. File > Recent Projects lists the recent projects (app/recent_projects.h).
struct Settings
{
    std::string device;
    int window_width = 0;
    int window_height = 0;
    int layout_version = 0;
    TrackColours track_colours = TrackColours::kRainbow;
    std::vector<std::string> recent_projects;
    std::string dsi_key; // the file of the DSi's digest key, or empty
};

// The system's dialogs: File > Open gives a Hokuto file, File > Save As the file to save to, File > Import > Sound
// Archive a sound archive, File > Import > Sequence, Bank, Wave Archive, Wave, Wave Sound File, Audio File and Stream a
// file to bring in as new items, and the Export window the file to export to, or the folder for a wave archive's WAV
// files and a bank's SFZ files. The browser's Replace… gives the file to put in the place of a sound's sequence, of a
// bank's, a wave archive's or a wave sound's file, of a wave or of a stream. Choose… in Studio Setup selects the file
// of the DSi's digest key.
enum class Dialog
{
    kOpen,
    kSaveAs,
    kImport,
    kImportSequence,
    kImportBank,
    kImportWaveArchive,
    kImportWave,
    kImportWaveSoundFile,
    kImportAudioFile,
    kImportStream,
    kExport,
    kExportFolder,
    kReplace,
    kDsiKey,
};

// What the user chose in the question about a document's changes (AskToSave).
enum class SaveChoice
{
    kSave,
    kDontSave,
    kCancel,
};

// What an export makes away from the program's thread: the archive's bytes, the sequence's files, a bank's
// instruments, or an item's files by their names, and the warnings. An archive's group files that the edits changed
// wait for the folder that the user chooses. An error stops the export.
struct ExportWork
{
    std::vector<uint8_t> archive;
    std::optional<ntdlib::sequence_export::Conversion> conversion;
    std::optional<ntdlib::sequence_export::BankConversion> bank;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    std::vector<format::ChangedGroupFile> group_files;
    std::vector<std::string> warnings;
    std::string error;

    // Of a song for the 3DS: whether the sound plays stereo waves, and whether they went in as mono.
    bool stereo_waves = false;
    bool mono_waves = false;

    // Of a GBA ROM: whether songs outgrow their places, and the ROM has no room for them after its data
    // (format::LacksRoom).
    bool lacks_room = false;

    // Of an export to MIDI of a sound that the zones don't show: the sound's sequence in the file's archive, and
    // the archive and the sequence that the measure plays, on the sound's engine.
    std::shared_ptr<const song::Sequence> sequence;
    song::Played played;
    std::shared_ptr<const song::Sequence> played_sequence;
};

// Runs `fill` away from the program's thread for an export. An exception from `fill` stops the export, and the work
// reports its message as the error.
template <typename Fill>
static std::future<ExportWork> ExportWorkOf(Fill fill)
{
    const auto run = [fill = std::move(fill)]() mutable
    {
        ExportWork work;
        try
        {
            fill(work);
        }
        catch (const std::exception& error)
        {
            work.error = error.what();
        }

        return work;
    };
    return std::async(std::launch::async, run);
}

// The parts of a MIDI export that differ between a GBA ROM, a ROM with another of the GBA's drivers, a DS archive
// and DSE's files (StartSourceMidiWork): the sound's name, the source that the zones play, how the work builds the
// version's source, converts the sound and reads its sequence, and whether the converter's warnings cover SFZ
// files.
template <typename Source>
struct MidiSource
{
    std::string name;
    std::shared_ptr<const Source> shown;
    std::function<std::shared_ptr<const Source>(const format::File&)> load;
    std::function<ntdlib::sequence_export::Conversion(const Source&)> convert;
    std::function<song::Sequence(const Source&)> read;
    bool sfz = true;
};

// An export from File > Export or the browser's Export… until its files are written or the user cancels it: what
// the Export window shows, the item that the export comes from, the name of the file that the dialog offers, the
// folder of the file that the user opened, and the work, then what the work made.
struct Export
{
    ExportTarget Target() const
    {
        return view.targets[view.chosen];
    }

    ExportView view;
    std::optional<song::Item> item;

    // The program of a bank's instrument that goes out by itself, for a bank's item, and the bank's last program
    // with an instrument.
    std::optional<uint32_t> program;
    uint32_t last_program = 0;

    std::string file_name;
    std::filesystem::path source_folder;

    // The extension of the archive that an export of the sound archive writes: .bfsar, .bcsar or .brsar for a
    // console's sound archive, .gba for a GBA ROM, .nds for a DS image and .sdat for a DS archive.
    std::string extension;

    std::future<ExportWork> work;
    std::optional<ExportWork> done;

    // The mono waves of a song for the 3DS: the choice of the window's check box, or Hokuto's.
    edit::MonoWaves mono = edit::MonoWaves::kAuto;

    // The measure of an export to MIDI, once the work is done: its work, a flag that cancels it, and what it found.
    std::future<MeasuredView> measure;
    std::shared_ptr<std::atomic<bool>> cancel_measure;
    std::optional<MeasuredView> measured;
};

// What an import makes away from the program's thread: the new version of the file, or why the edit can't be made.
struct EditWork
{
    std::optional<format::File> result;
    std::string error;
};

// What File > Open or File > Import makes away from the program's thread: the file with its timelines laid out for
// editing, the archive that it builds for playback, the sequences of its files and the field of each sound's start.
// A file of the Wii builds its archive for the Wii's model, a file of the GBA its ROM, and a file of the DS its
// archive with the build of the cartridge's driver. An imported sound archive gives a file that stays untitled
// until it's saved. `error` gives the message when the file can't open.
struct OpenWork
{
    std::shared_ptr<const format::File> document;
    song::Played played;
    std::vector<std::optional<uint32_t>> sequences;
    std::optional<std::vector<std::optional<uint32_t>>> start_fields;
    bool imported = false;
    bool text_form = false;
    std::string error;
};

// An open or an import while its work runs: the work, and the path and the name of the file.
struct Opening
{
    std::future<OpenWork> work;
    std::string path;
    std::string name;
};

// An import that the user confirmed, while its work runs: the work, the action and the details that the Undo
// History shows for the new version, and the sound that the browser selects once it lists it. A replacement that
// puts files into groups runs the same way. `replaced` gives the sequences whose commands it replaces.
struct PendingEdit
{
    std::future<EditWork> work;
    std::string action;
    std::string details;
    std::optional<uint32_t> select;
    std::vector<uint32_t> replaced;
    std::optional<uint32_t> open_bank;
};

// A replacement from the browser's Replace…, or an import from File > Import, until the user makes it or cancels
// it: what it starts from, what the Replace window shows, and for a file in the place of another, the file's kind.
// For a WAV file in the place of a wave, `wave` gives the wave's index in the wave archive of `source.sound`, and
// `wav` holds the WAV file's bytes for another encoding. A bank, a wave archive or a wave sound file that comes in
// as new items, or in the place of an item's file, is `new_item`. `new_item` holds the item as it was read. For a
// bank or a wave sound file, `named` lists the wave archives that its waves name.
// A WAV file or a wave file that comes in as a new wave sets `new_wave`. `wave` then gives the new wave's index,
// and `wav` holds the file's bytes for another wave archive or encoding. For a stream file or a WAV file in the
// place of a stream sound's stream, `stream` gives the stream's file ID, `source.sound` gives the sound, and `wav`
// holds the file's bytes for another encoding. An import builds the whole archive again once the user confirms it.
// `pending` holds its work until it's done.
struct Replacement
{
    ReplaceSource source;
    ReplaceView view;
    std::optional<edit::FileKind> file;
    std::optional<uint32_t> wave;
    std::vector<uint8_t> wav;
    std::optional<edit::IncomingFile> new_item;
    std::vector<edit::NamedWaveArchive> named;
    bool new_wave = false;
    std::optional<PendingEdit> pending = std::nullopt;
    std::optional<uint32_t> stream = std::nullopt;

    // File > Import > Stream…: the stream of the stream file or the WAV file in `wav`, and the new sound's stream.
    std::optional<edit::IncomingStream> incoming_stream = std::nullopt;
    std::optional<edit::NewStream> new_stream = std::nullopt;

    // A file of the DS that comes in as a new item, with its bytes in `wav`.
    std::optional<edit::DsNewItem> ds_new = std::nullopt;

    // A MIDI file that comes into a GBA file: its bytes, and the songs that the window offers (song::GbaSongs). A
    // GBA ROM always contains a song that plays (song::LoadGbaRom). Only this import fills `gba_songs`.
    std::vector<uint8_t> midi = {};
    std::vector<int> gba_songs = {};

    // A SoundFont that comes in as new banks: its bytes, and what it adds to the archive.
    std::shared_ptr<const std::vector<uint8_t>> sound_font = nullptr;
    std::optional<edit::SoundFontImport> sound_font_import = std::nullopt;

    // A SoundFont that comes into a GBA file: its presets as voices, at the chosen rate of the view's menu.
    std::shared_ptr<const edit::GbaSoundFontVoices> gba_sound_font = nullptr;
    std::size_t gba_sound_font_rate = 0;

    // What SFZ files leave out, first among the window's warnings.
    std::vector<std::string> sound_font_warnings = {};
};

// What the Replace window's Play plays in the version that the mapping makes: sound `sound`, or else a note of the
// first instrument of bank `bank`, or else the first wave of wave archive `wave_archive`. A sequence sound and a
// wave sound play on the console's model.
struct PreviewTarget
{
    std::optional<uint32_t> sound;
    std::optional<uint32_t> bank;
    std::optional<uint32_t> wave_archive;
    format::Mixing mixing = format::Mixing::kStrict;
};

// What the Replace window's Play makes away from the program's thread: the preview, or why it can't play.
struct PreviewWork
{
    std::unique_ptr<audio::Renderer> renderer;
    std::string error;
    std::string note; // why nothing plays, when nothing went wrong
};

// An item that the browser lists at a place: its group and its index there, the number that the item has in the
// file, and the number of the place's child when the item lists that child.
struct ListedPlace
{
    std::size_t group = 0;
    std::size_t item = 0;
    uint32_t number = 0;
    std::optional<uint32_t> child;
};

// The item whose file Replace… replaces: a bank, a wave archive or a wave sound, by its place in the archive's
// list.
struct FileTarget
{
    edit::FileKind kind = edit::FileKind::kBank;
    uint32_t item = 0;
};

// The wave whose place a WAV file takes: its wave archive, by its place in the archive's list, and its index there.
struct WaveTarget
{
    friend bool operator==(const WaveTarget&, const WaveTarget&) = default;

    uint32_t wave_archive = 0;
    uint32_t wave = 0;
};

// The Key Editor's selection before an action of the history, and the selection that the action left for its
// version. The List Editor's selections around the action follow. Undo puts back the selections before, and redo
// the selections after.
struct ActionSelections
{
    std::optional<KeySelection> before;
    std::optional<KeySelection> after;
    std::optional<KeySelection> list_before;
    std::optional<KeySelection> list_after;
};

// An action that the mixer's engine switch made: how many actions were done after it, and the sound and the engine
// that it set.
struct EngineEdit
{
    std::size_t done = 0;
    uint32_t sound = 0;
    format::Engine engine = format::Engine::kSwitch;
};

// A command or a drop that came while a text field was being typed in, and Dear ImGui's frame at the time. `run`
// gives the command or the drop again.
struct WaitingCommand
{
    std::function<void()> run;
    int frame = 0;
};

// Files that the system dropped on the Bank Editor while a text field was being typed in, and where the first of
// them landed.
struct WaitingBankDrop
{
    std::vector<std::string> paths;
    BankDrop drop;
};

} // namespace hokuto::app
