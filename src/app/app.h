// SPDX-License-Identifier: MIT

// The program: the main window with its menus, control bar, shortcuts and zones, the Studio Setup window and the audio
// output. main.cpp passes SDL3's callbacks on to it.

#pragma once

#include <SDL3/SDL.h>
#include <imgui.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"

#include "app/audio_output.h"
#include "app/bank_editor.h"
#include "app/editor_view.h"
#include "app/effect_editor.h"
#include "app/export_window.h"
#include "app/key_edits.h"
#include "app/list_editor.h"
#include "app/list_edits.h"
#include "app/menus.h"
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
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/catch_up.h"
#include "song/items.h"
#include "song/player.h"
#include "song/sequence.h"
#include "song/sequence_reader.h"

namespace hokuto::app
{

// The windows of the lower zone.
enum class LowerZone
{
    kMixer,
    kKeyEditor,
    kListEditor,
};

class App final : public MenuTarget
{
public:
    // Opens the main window and the audio device, then opens the Hokuto file at `file` or imports the sound archive
    // there, if any. Returns nullptr after an error. The user sees the error in a message box. With `preview`, the
    // zones show a made-up song.
    static std::unique_ptr<App> Create(bool preview, const std::string& file);

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Saves the settings and closes the window and the audio device.
    ~App() override;

    // Handles an event from SDL3. Returns SDL_APP_SUCCESS when the program should quit.
    SDL_AppResult HandleEvent(const SDL_Event& event);

    // Draws a frame of the main window.
    SDL_AppResult Iterate();

    void Execute(Command command) override;
    bool IsChecked(Command command) const override;
    bool IsEnabled(Command command) const override;
    std::string Title(Command command, std::string_view name) const override;
    std::vector<std::string> Listed(Command command) const override;
    void ExecuteListed(Command command, std::size_t index) override;

private:
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
    };

    // A message box's kind: an error shows the error icon, and a note, such as why nothing plays, the information icon.
    enum class MessageKind
    {
        kError,
        kNote,
    };

    // The system's dialogs: File > Open gives a Hokuto file, File > Save As the file to save to, File > Import > Sound
    // Archive a sound archive, File > Import > Sequence, Bank, Wave Archive, Wave, Wave Sound File, Audio File and
    // Stream a file to bring in as new items, and the Export window the file to export to, or the folder for a wave
    // archive's WAV files and a bank's SFZ files. The browser's Replace… gives the file to put in the place of a
    // sound's sequence, of a bank's, a wave archive's or a wave sound's file, of a wave or of a stream.
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

        // Of an export to MIDI of a sound that the zones don't show: the sound's sequence in the file's archive, and
        // the archive and the sequence that the measure plays, on the sound's engine.
        std::shared_ptr<const song::Sequence> sequence;
        std::shared_ptr<const ntdlib::sar::SoundArchive> played;
        std::shared_ptr<const ntdlib::rsar::SoundArchive> played_wii;
        std::shared_ptr<const song::Sequence> played_sequence;
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
    // A file of the Wii builds its archive for the Wii's model. An imported sound archive gives a file that stays
    // untitled until it's saved. `error` gives the message when the file can't open.
    struct OpenWork
    {
        std::shared_ptr<const format::File> document;
        std::shared_ptr<const ntdlib::sar::SoundArchive> archive;
        std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
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

    explicit App(bool preview);

    bool Init();
    void LoadFonts();

    // The settings' part of the settings file. Dear ImGui reads and writes the file.
    void AddSettingsHandler();
    void ReadSetting(std::string_view line);
    void WriteSettings(ImGuiTextBuffer& out) const;

    void HandleShortcuts();

    // Ends the typing of a text field before a command or a drop, and keeps `run` for a later frame. `run` gives the
    // command or the drop again. Returns false while no text field is being typed in: the command or the drop goes on
    // at once.
    bool WaitForTyping(std::function<void()> run);

    // Runs the commands that waited for a text field to keep its text.
    void RunWaitingCommands();

    void DrawMenus();
    void DrawMenuItems(const std::vector<MenuItem>& items);
    void DrawControlBar();
    void DrawStudioSetup();
    void DrawPreferences();

    // Project Setup shows the file's engine, gives every sound a mixing mode, and edits the file's tags.
    void DrawProjectSetup();

    // File > New Project… opens a window for the console of a new file. Create then shows the new file (NewDocument),
    // and Cancel closes the window.
    void DrawNewProject();

    // The window that shows in the lower zone: the window in front, if it's open, or else the Key Editor, the List
    // Editor or the mixer, the first that's open.
    LowerZone FrontOfLowerZone() const;
    void DrawDeviceCombo();
    void ChooseDevice(const std::string& name);
    void PlayTestTone();

    // Plays `renderer` in place of the sequence's playback, of any preview and of an audition: the test tone, or a
    // preview.
    void PlayAlone(std::unique_ptr<audio::Renderer> renderer);

    // Plays the preview of the Replace or Import window's Play. It ends as the window closes.
    void PlayReplacePreview(std::unique_ptr<audio::Renderer> renderer);

    // Ends a preview that plays outside the transport: the browser's, a window's Play, an Export clip or the test tone.
    // Playback that follows the transport goes on.
    void StopPreview();

    // Plays `renderer` over the sequence's playback, in place of the last audition: a note that the Key Editor sounds.
    // While playback is stopped, it takes the place of a preview.
    void PlayAudition(std::unique_ptr<audio::Renderer> renderer);
    void UpdateOutput();

    // Suspends the audio device after kOutputRest without sound, and resumes it while something sounds. An open device
    // keeps the system awake, even while it plays silence. While SDL3 waits for events, a timer wakes the program when
    // the rest is due. The minimized window's frames call it too.
    void RestOutput();
    void Present();

    // The meters jump to each peak and fall back slowly. Each strip's peak keeps the highest level until the user
    // clears it.
    void UpdateMeters();

    // The pace of the frames. The window draws at the display's rate while something on it moves, and for a moment
    // after each event. Otherwise SDL3 waits for the next event before it calls Iterate again. Any thread can call Wake
    // to end the wait.
    void Pace();
    void SetWaiting(bool waiting);
    void Wake() const;

    // Shows a message for the user in a box over the main window, with the icon of its kind.
    void ShowMessage(const std::string& message, MessageKind kind = MessageKind::kError);

    // Shows a dialog. SDL3 may call the dialog's callback on another thread. The program acts on the chosen file in its
    // next frame.
    void ShowDialog(Dialog dialog);
    static void SDLCALL OpenChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL SaveChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL ImportChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL ImportSequenceChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL ImportBankChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL ImportWaveArchiveChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL ImportWaveChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL ImportWaveSoundFileChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL ImportAudioFileChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL ImportStreamChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL ExportChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL ExportFolderChosen(void* user_data, const char* const* files, int filter);
    static void SDLCALL ReplaceChosen(void* user_data, const char* const* files, int filter);
    void Choose(Dialog dialog, const char* const* files);

    // Opens the file at `path`, a UTF-8 path, on a thread of its own: a Hokuto file in either form, or else a sound
    // archive to import. A file dropped on the window and a file named on the command line open this way. A new open
    // drops the work of an open that still runs. The message of a file of another kind names File > Import > Sound
    // Archive… when `import` marks that command.
    void OpenOrImport(const std::string& path, bool import = false);

    // The work of OpenOrImport for the file at `path`. It reads the file. A Hokuto file of `name` with `bytes` opens,
    // and the program builds the archive that the file holds in memory. A sound archive at `file` of the Switch, the
    // Wii U or the 3DS, or of the Wii, comes in as a new file.
    static OpenWork Opened(const std::string& path, bool import);
    static OpenWork OpenedFile(const std::string& name, std::span<const uint8_t> bytes);
    static OpenWork ImportedArchive(const std::filesystem::path& file, std::vector<uint8_t> bytes);
    static OpenWork ImportedWiiArchive(const std::filesystem::path& file, std::vector<uint8_t> bytes);

    // Once an open's work is done, shows the file that it made, or the message that says why the file can't open.
    void FinishOpening();

    // Puts the Hokuto file at `path`, a UTF-8 path, first in File > Recent Projects.
    void AddRecentProject(const std::string& path);

    // The window that waits while an open's work runs. Cancel drops the work.
    void DrawOpening();

    // Shows a new file for `engine` (format::NewFile). The file stays untitled until it's saved.
    void NewDocument(format::Engine engine);

    // Shows the Hokuto file `document`, called `name`, and plays `archive`, the archive that it holds. `sequences`
    // gives the sequence of the file that each of the archive's files holds (format::SequencesOfFiles), and
    // `start_fields` the field that holds each sound's start (format::StartFieldsOfSounds). Without the fields, no
    // sequence can be replaced. The browser lists the archive's sounds, sequences, banks and wave archives. The file
    // starts a new history. A file of the Wii comes with `wii`, its archive, in place of `archive`. The browser then
    // lists the archive's sounds and sequences.
    void ShowDocument(std::shared_ptr<const format::File> document,
                      std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                      std::vector<std::optional<uint32_t>> sequences,
                      std::optional<std::vector<std::optional<uint32_t>>> start_fields, const std::string& name,
                      std::shared_ptr<const ntdlib::rsar::SoundArchive> wii = nullptr);

    // File > Save saves to the file that the document came from, or asks where. SaveTo saves to `path`, a UTF-8 path,
    // in the form that the file was opened in. An imported archive saves in the binary form. The file keeps the view of
    // the sound that the zones show (CurrentView).
    void Save();
    void SaveTo(const std::string& path);

    // The view of the sound that the zones show, for a file to keep: the sound, the transport, the arrangement and the
    // editors. Nothing while the zones show no sound of a file. RestoreView shows the sound that the zones show as
    // `view` keeps it. A file's view waits for its sound to show (pending_view_).
    std::optional<format::EditorView> CurrentView() const;
    void RestoreView(const format::EditorView& view);

    // The number of each track of the sequence that the zones show, in order.
    std::vector<int> TrackNumbers() const;

    // Runs `then` once the user has decided about the document's changes, before the program quits or another file
    // takes the document's place. Without changes, `then` runs at once. Otherwise the user chooses to save them, to
    // drop them or to cancel (AskToSave). A document without a file asks where to save it first. `then` waits for that
    // save. A cancelled dialog or a save that fails drops it.
    void AfterChanges(std::function<void()> then);
    SaveChoice AskToSave();

    // File > Export makes the files of `target` away from the program's thread, from the version that the history has
    // reached. The Export window lists the warnings once the work is done. Export… then opens the system's save dialog,
    // and ExportTo writes the files to `path`, a UTF-8 path. Cancel drops the export.
    void StartExport(ExportTarget target);
    void DrawExport();
    void ExportTo(const std::string& path);

    // File > Export > Another Console… offers a target for each other console. The work sends the shown sequence's
    // sound to the chosen target's console, and a choice of another target starts it again.
    void StartSongWork(Export& job);

    // File > Export's MIDI files, and the browser's Export… of a sequence to MIDI, convert sound `sound`. The work
    // reads a sound that the zones don't show for the warnings and the measure.
    void StartMidiWork(Export& job, uint32_t sound);

    // The browser's Export… exports the item at `place` the same way. The Export window offers the item's targets, and
    // a choice of another target starts the work again (StartItemWork). A wave archive's WAV files and a bank's SFZ
    // files go into a folder from the system's dialog. An item of a file of the Wii takes StartWiiItemWork.
    void StartItemExport(const BrowserPlace& place);
    void StartItemWork(Export& job);
    void StartWiiItemWork(Export& job);

    // Writes `files` into `folder`, each under its name. When the folder holds some of them already, the user chooses
    // to replace them or to cancel (AskToReplace). Returns false when the user cancels. Throws std::runtime_error with
    // a message for the user when a file can't be written.
    bool WriteIntoFolder(const std::filesystem::path& folder,
                         const std::vector<std::pair<std::string, std::span<const uint8_t>>>& files);
    bool AskToReplace(const std::string& message);

    // Whether a save or an export may write `file`: the name `chosen` that the save dialog gave, with the extension
    // that WithExtension added. The dialog asked about the chosen name only. When the extension made a new name and the
    // folder holds a file of that name, the user chooses to replace it or to cancel (AskToReplace).
    bool MayWriteTo(const std::string& chosen, const std::filesystem::path& file);

    // Ends the export that the Export window shows. Its work runs on until it's done. Its measure stops early.
    void DropExport();

    // Starts the measure of an export to MIDI or to another console whose work is done (measure/measure.h).
    void StartMeasure(Export& job);

    // The browser's Replace… puts a sequence file in the place of the sequence of the sound at `place`. The file comes
    // from the system's dialog. The Replace window then shows its starts, the programs and the warnings. Replace makes
    // the edit from the version that the history has reached, and Cancel drops it. For a bank, a wave archive or a wave
    // sound, a file of the same kind goes in the place of the item's file. For a wave of a wave archive, a WAV file
    // goes in its place, in the encoding chosen in the Replace window, or a wave file of the Switch or the Wii U. For a
    // stream sound, a stream file of the Switch or the Wii U, or a WAV file, goes in the place of its stream. The
    // Replace window shows the warnings.
    void StartReplace(const BrowserPlace& place);
    void OpenReplacement(const std::string& path);
    void OpenFileReplacement(const std::string& path);
    void OpenWaveReplacement(const std::string& path);
    void OpenStreamReplacement(const std::string& path);
    void DrawReplace();

    // Ends the replacement or the import that the Replace window shows. The work of a confirmed import runs on until
    // it's done.
    void DropReplace();

    // Starts the work of a confirmed import on a thread of its own (`make`). The Replace window waits for it, and the
    // Undo History then takes its version as `action` with `details`. The browser then selects sound `select`. The
    // version replaces the commands of the sequences `replaced` (Do).
    void StartEdit(std::function<format::File()> make, std::string action, std::string details,
                   std::optional<uint32_t> select, std::vector<uint32_t> replaced = {});

    // The Replace window's Play of a mapping: a sequence's, a bank's, a wave archive's or a wave sound file's, in the
    // place of an item or as a new one. The work makes the version that the mapping makes, builds its archive and reads
    // the sound on a thread of its own (StartPreview). The preview then takes the place of playback. A change in the
    // window, Cancel or the edit drops a preview that hasn't started.
    void PlayMapping();
    void StartPreview(std::function<format::File()> make, PreviewTarget target);

    // File > Import > Sequence… brings a sequence file in as a new sound of the archive (edit::AddSequenceSound). The
    // file comes from the system's dialog. The Import window then names the new sound and offers the sounds whose
    // settings it can copy, the file's starts and the programs. Import makes the edit from the version that the history
    // has reached, and Cancel drops it. The browser then selects the new sound, and the zones show it.
    void StartSequenceImport();
    void OpenSequenceImport(const std::string& path);
    void MakeImport();

    // Opens the Import window for `source`. The incoming sequence of `source` holds the new commands. `name` names the
    // commands' file in the window and gives the new sound's first name. A file in the model form without a sequence
    // sound gives the new sound Hokuto's settings. Another file without one shows a message that `what` starts.
    void OpenSequenceSource(ReplaceSource source, const std::string& name, const std::string& what);

    // The same for a file of the Wii (edit::AddWiiSequenceSound). A sequence file of another console comes in converted
    // (edit::ReadIncoming). The new sound has one bank slot, and joins the groups with names chosen in the window. An
    // archive without a sequence sound gives the new sound Hokuto's settings.
    void OpenWiiSequenceSource(ReplaceSource source, const std::string& name);
    void MakeWiiImport();

    // Project > Add Sequence Sound… opens the Import window with an empty song (edit::EmptySong). The new sound copies
    // the settings of a sequence sound of the archive, or takes Hokuto's (edit::AddSequenceSound). The song takes the
    // sound's name, and the program edits it as a timeline.
    void StartSoundAddition();

    // The edit that File > Import > Bank…, Wave Archive… or Wave Sound File… makes with the Import window's choices.
    // With `join`, the new items join the groups chosen in the window.
    std::function<format::File()> ItemImportEdit(bool join) const;

    // File > Import > Bank…, Wave Archive… and Wave Sound File… bring a bank file, a wave archive file or a wave sound
    // file of the archive's console in as new items (edit::AddBank, edit::AddWaveArchive and edit::AddWaveSounds). The
    // file comes from the system's dialog. The Import window then names the new items. For a bank or a wave sound file,
    // it pairs each wave archive that the file's waves name with one of the archive's. For a wave sound file, it offers
    // the sounds whose settings the new sounds can copy. Import makes the edit from the version that the history has
    // reached, and Cancel drops it.
    void OpenItemImport(const std::string& path, edit::FileKind kind);

    // Opens the Import window for `bytes`, a file of `kind` named `name`. OpenItemImport does likewise. `source` holds
    // what the window starts from beyond the file, such as Project > Add Bank…'s mark. Project > Add Bank… opens it
    // with an empty bank (StartBankAddition). The Bank Editor then opens on the new bank once the program has built the
    // version's archive (`bank_to_open_`).
    void OpenItemBytes(std::vector<uint8_t> bytes, const std::string& name, edit::FileKind kind,
                       ReplaceSource source = {});
    void StartBankAddition();
    void MakeItemImport();

    // The same for a file of the Wii (edit::AddWiiBank and edit::AddWiiWaveSounds). A bank file or a wave sound file of
    // the Wii takes the RWAR beside it in `source`, or a copy of the waves of a file of the archive. One of another
    // console comes in converted, with wave data of the archive in the place of each wave archive that it names
    // (edit::WiiFileOf). A wave archive's waves come in as the wave data of a new bank without instruments. The edit
    // runs on a thread (WiiImportedFile).
    void OpenWiiItem(edit::IncomingFile incoming, std::vector<edit::NamedWaveArchive> named, std::size_t sounds,
                     const std::string& name, ReplaceSource source);
    void OpenWiiItemBytes(std::vector<uint8_t> bytes, const std::string& name, edit::FileKind kind,
                          ReplaceSource source);
    void MakeWiiItemImport();

    // File > Import > Wave… and Audio File… bring a wave file of the Switch or the Wii U, or a WAV file (`wav`), in as
    // a new wave after the waves of a wave archive (edit::ReadIncomingWaveFile and edit::ReadIncomingWave). The file
    // comes from the system's dialog. The Import window sets the wave archive, and a WAV file's encoding. Import makes
    // the edit from the version that the history has reached, and Cancel drops it.
    void OpenWaveImport(const std::string& path, bool wav);

    // The import of `bytes` as a new wave of wave archive `wave_archive`. `bytes` is a WAV file or a wave file (`wav`)
    // named `name`. A WAV file's wave takes `encoding`, and `rate` when it's lower than the WAV file's. Returns nothing
    // after a message when the file can't go in.
    std::optional<Replacement> WaveImport(std::vector<uint8_t> bytes, const std::string& name, uint32_t wave_archive,
                                          ntdlib::sar::WaveEncoding encoding, bool wav,
                                          std::optional<uint32_t> rate = std::nullopt);

    // The same for a file of the Wii: a new wave after the waves of the wave data of file `file_id`, a bank's or a wave
    // sound file's (edit::WithIncomingWiiWave). `bytes` is a WAV file or a wave file of the Wii, the 3DS, the Wii U or
    // the Switch.
    std::optional<Replacement> WiiWaveImport(std::vector<uint8_t> bytes, const std::string& name, uint32_t file_id,
                                             ntdlib::sar::WaveEncoding encoding, bool wav,
                                             std::optional<uint32_t> rate = std::nullopt);

    // File > Import > Stream… brings a stream file of the Switch or the Wii U, or a WAV file, in as a new stream sound
    // (edit::AddStreamSound). The file comes from the system's dialog. The Import window names the new sound, offers
    // the sounds whose settings it can copy, and sets a WAV file's encoding. Import makes the edit from the version
    // that the history has reached, and Cancel drops it. The browser then selects the new sound. A file of the Wii
    // takes a stream file of the Wii or of a later console, or a WAV file (edit::AddWiiStreamSound).
    void OpenStreamImport(const std::string& path);
    void MakeStreamImport();

    // The import of `bytes`, a stream file or a WAV file named `file_name`, as new stream sound `name`, a copy of sound
    // `model`. `incoming` holds the stream once a WAV file has been encoded in `encoding`. A change of the name or the
    // model then keeps it. Returns nothing after a message when the file can't go in. WiiStreamImport does the same in
    // a file of the Wii.
    std::optional<Replacement> StreamImport(std::vector<uint8_t> bytes, const std::string& file_name, uint32_t model,
                                            const std::string& name, ntdlib::sar::WaveEncoding encoding,
                                            std::optional<edit::IncomingStream> incoming = std::nullopt);
    std::optional<Replacement> WiiStreamImport(std::vector<uint8_t> bytes, const std::string& file_name, uint32_t model,
                                               const std::string& name, ntdlib::sar::WaveEncoding encoding,
                                               std::optional<edit::IncomingStream> incoming = std::nullopt);

    // Replace… of an item of a file of the Wii: the dialog for a file of the item's kind.
    void StartWiiReplace(const BrowserPlace& place);

    // The replacement of `target` of a file of the Wii by the file at `path`. Shows a message when the file can't be
    // read. The Replace window says why a file that ntdlib reads can't go in.
    void OpenWiiFileReplacement(const std::string& path, const FileTarget& target);

    // The replacement of the file of `target` of a file of the Wii by `incoming`, a bank file or a wave sound file of
    // console `later` among the 3DS, the Wii U and the Switch, from the file named `name`. The file goes in converted,
    // in the present file's version, with the waves of the wave data of `targets` in the place of the wave archives of
    // `named`, by their places among song::WaveArchivesOf (edit::WiiFileOf). The new wave data takes the place of the
    // present one. The Replace window waits while a target is -1.
    std::optional<Replacement> WiiFileReplacement(edit::IncomingFile incoming, const std::string& name,
                                                  const FileTarget& target, ntdlib::sar::Flavor later,
                                                  std::vector<edit::NamedWaveArchive> named,
                                                  const std::vector<int>& targets);

    // The replacement of the file of `target` by `incoming`, from the file named `name`. A bank or a wave sound file
    // names the wave archives of `targets` in the place of those of `named`, by their places in the archive's list. The
    // Replace window waits while a target is -1. A bank takes the instruments of `fixes` at the programs that it lacks.
    // The file joins the groups of `joined` once the user has chosen them, and the new groups of `new_groups`
    // (ReplaceSource). Returns nothing after a message when the file can't go in.
    std::optional<Replacement> FileReplacement(edit::IncomingFile incoming, const std::string& name,
                                               const FileTarget& target, std::vector<edit::NamedWaveArchive> named,
                                               const std::vector<int>& targets,
                                               const std::vector<edit::ProgramFix>& fixes = {},
                                               std::optional<std::vector<uint32_t>> joined = std::nullopt,
                                               std::vector<std::string> new_groups = {});

    // The replacement of `target` by `wav`, a WAV file or a wave file named `name`, with a WAV file's wave in
    // `encoding`, at `rate` when it's lower than the WAV file's, and with `loop` when the WAV file has none. Returns
    // nothing after a message when the file can't go in.
    std::optional<Replacement> WaveReplacement(std::vector<uint8_t> wav, const std::string& name,
                                               const WaveTarget& target, ntdlib::sar::WaveEncoding encoding,
                                               std::optional<uint32_t> rate = std::nullopt,
                                               std::optional<edit::WaveLoop> loop = std::nullopt);

    // The same for a wave of a file of the Wii. `target` names the wave data by its file's ID. `wav` is a WAV file or a
    // wave file of the Wii.
    std::optional<Replacement> WiiWaveReplacement(std::vector<uint8_t> wav, const std::string& name,
                                                  const WaveTarget& target, ntdlib::sar::WaveEncoding encoding,
                                                  std::optional<uint32_t> rate = std::nullopt,
                                                  std::optional<edit::WaveLoop> loop = std::nullopt);

    // The replacement of the stream of stream sound `sound` by `bytes`, a stream file or a WAV file named `name`, with
    // a WAV file's stream in `encoding`. Returns nothing after a message when the file can't go in. A file of the Wii
    // takes the replacement of WiiStreamReplacement.
    std::optional<Replacement> StreamReplacement(std::vector<uint8_t> bytes, const std::string& name, uint32_t sound,
                                                 ntdlib::sar::WaveEncoding encoding);

    // The same for a 3DS stream sound that plays the AAC stream of `changes`. `bytes` is an AAC file, a WAV file or a
    // stream file of any console.
    std::optional<Replacement> AacStreamReplacement(std::vector<uint8_t> bytes, const std::string& name, uint32_t sound,
                                                    const edit::AacChanges& changes);

    // Whether the entry of stream sound `sound` of the shown archive marks an AAC stream beside the archive. The file
    // may hold the stream or lack it. A stream file that the file holds there in its place counts as no AAC stream.
    bool PlaysAacStream(uint32_t sound) const;

    // The same for a stream sound of a file of the Wii. `bytes` is a stream file of the Wii, the 3DS, the Wii U or the
    // Switch, or a WAV file.
    std::optional<Replacement> WiiStreamReplacement(std::vector<uint8_t> bytes, const std::string& name, uint32_t sound,
                                                    ntdlib::sar::WaveEncoding encoding);

    // The window's title: the program's name and the file's. An asterisk marks a file with changes since it was opened
    // or saved.
    void UpdateTitle();

    // The version of the file that the history has reached, or nullptr while no file is open. The made-up song's
    // effects make a file too.
    const format::File* Document() const;

    // The effects on the aux buses: the document's, or the made-up song's. nullptr while neither is open.
    const BusEffects* Effects() const;

    // Draws the window of the open effect. Handles the clicks on the mixer's inserts, and the edits in the window. The
    // window edits a draft of the effect. Each edit that ends becomes an action in the history.
    void DrawEffects();

    // Opens the editor of the browser's row at `place`: a bank or one of its instruments in the Bank Editor, a wave of
    // a wave archive or a stream sound's stream in its properties.
    void OpenEditor(const BrowserPlace& place);

    // Opens bank `bank`, by its place in the archive's list, in the Bank Editor, with the instrument at `program`
    // selected, or else its first. CloseBankEditor closes it.
    void OpenBankEditor(uint32_t bank, std::optional<uint32_t> program = std::nullopt);
    void CloseBankEditor();

    // Draws the Bank Editor while it's open. Its view follows each archive that the program builds, and each move in
    // the history back to the archive that the program holds (FollowHistory). A version without the bank closes it.
    // Each edit becomes an action in the history (ApplyBankEdit). The keyboard sounds its key on the selected
    // instrument on the console's model, over the playback.
    void UpdateBankEditor();
    void ApplyBankEdit(const BankEdit& bank_edit);

    // Makes `file`, the version with `model` as the Bank Editor's bank, an action in the history by `name` and
    // `details`. The editor shows the model at once, before the program has built the version's archive.
    void DoBankEdit(std::string name, std::string details, format::File file, ntdlib::sar::BankFile model);

    // Makes `file`, the version with the bank of `dropped`, an action in the history. DoBankEdit does the same. The
    // details name the programs that the drop changes. The editor then selects the first of them and the region that
    // the drop made or changed there. A slider's value that the user held belongs to the region selected before, and
    // the editor forgets it.
    void DoBankDrop(DroppedWaves dropped, format::File file);

    // Places waves on the Bank Editor's bank at `drop`: the WAV files and wave files at `paths` that the system dropped
    // there, or the wave of the browser at `place`. The files' waves go into the bank's wave archive first
    // (edit::WithBankWaves). On the Wii, they go after the bank's waves in its wave data (DropFilesOnWiiBank). The edit
    // becomes an action in the history. A file that can't go in shows a message. A text field that's being typed in
    // keeps its text first. The files then wait in `waiting_bank_drop_` while the reader builds and reads another
    // version (`reading_version_`), such as that of the field's edit.
    void DropFilesOnBank(const std::vector<std::string>& paths, const BankDrop& drop);

    // Opens the first of `paths`, the files that the system dropped on the window outside the Bank Editor or that
    // Finder opened with the program, once the user has decided about the document's changes. A text field that's being
    // typed in keeps its text first. A file that is neither a Hokuto file nor a sound archive shows a message, and the
    // document stays. So does a drop while a window waits for an answer (WindowWaits).
    void DropFilesOnWindow(const std::vector<std::string>& paths);
    void DropFilesOnWiiBank(const std::vector<std::string>& paths, const BankDrop& drop);
    void DropWaveOnBank(const BrowserPlace& place, const BankDrop& drop);

    // Opens the properties of a wave, by its wave archive's place in the archive's list and its index there, or of the
    // stream of stream sound `sound`. CloseProperties closes them.
    void OpenWaveProperties(uint32_t wave_archive, uint32_t wave);
    void OpenStreamProperties(uint32_t sound);
    void CloseProperties();

    // Draws the properties of a wave or a stream while they're open. Their view follows each archive that the program
    // builds, and each move in the history back to the archive that the program holds (FollowHistory). A version
    // without the item closes them. Each edit becomes an action in the history (ApplyWaveEdit). Play plays the item
    // round its loop until Stop (PlayProperties). Replace… and Export… open the windows that the browser's menu opens,
    // for the browser's row of the item (PropertiesPlace).
    void UpdateProperties();
    void ApplyWaveEdit(const WaveEdit& wave_edit);
    void PlayProperties();
    std::optional<BrowserPlace> PropertiesPlace() const;

    // Starts a measure of the sizes of the groups that load file `file_id` of the archive that the program has built,
    // on a thread of its own (GroupSizeLines). `sizes` takes its future, and the editors show the lines once they're
    // ready. DropGroupSizes drops the measure that `sizes` held. A measure that still runs goes on to its end in
    // `dropped_group_sizes_`. Without the list, the program would wait for it: a future of std::async waits in its
    // destructor.
    void MeasureGroups(std::future<std::vector<std::string>>& sizes, uint32_t file_id);
    void DropGroupSizes(std::future<std::vector<std::string>>& sizes);

    // Makes an edit: `file` becomes the next version. The history lists the action by `name` and `details`. The Key
    // Editor's selection then waits for `after`, a selection in the new version. Without it, the selection stays only
    // while the shown sequence stays the same. Each timeline that the edit changed takes the converter's layout, and
    // the selections follow it (edit::SettleTimelines). An edit that a timeline can't hold doesn't happen. The edit
    // replaced the commands of the sequences of `replaced` with those of a file. They and the new sequences take the
    // mode that OpenTimelines finds for them. ChangeEffects makes a version with `effects` on the aux buses.
    void Do(std::string name, std::string details, format::File file, std::optional<KeySelection> after = std::nullopt,
            std::vector<uint32_t> replaced = {});
    void ChangeEffects(const ActionNames& names, const BusEffects& effects);

    // Follows a move in the history: the title, the effects that the model plays, and the sequences. A change of the
    // effects makes a new model. The old model plays on while the new one catches up, and playback goes on without a
    // stop. A change of the archive's parts, such as an edit of a sequence, builds the archive again (ReadShown).
    void FollowVersion();

    // Follows undo, redo or a move in the Undo History from `left`, the version after the first `from` actions. A move
    // back puts back the selections of the Key Editor and the List Editor from before the first action that it undoes.
    // A move forward puts back the selections that the last action it redoes left. After a move to a version whose
    // archive differs from `left`'s, the mixer's controls and the drags and sliders of the item editors let go of the
    // values that they hold. The track to select goes too. After a move back to the archive that the program holds,
    // the views of items show that archive again. After a move to other effects, the effect's window shows the
    // version's effect.
    void FollowHistory(std::size_t from, const format::File& left);

    // Opens the item chosen in the browser. A sequence is read away from the program's thread. Once it's read, it opens
    // in the zones. An error that stopped the read shows in a message box. A click on a bank's instrument or a wave
    // archive's wave plays it, or stops it while it plays.
    void OpenBrowsed();
    void ShowSequence(song::SequenceReader::Result read);

    // Reads the sequence that the browser shows, or else the one that the zones show, away from the program's thread.
    // When the open archive came from another version of the file, the read builds the version's archive first. Nothing
    // is read while the zones show that sequence from the version's archive. Without a sequence to read, the version's
    // archive is built on the program's thread, for the browser's previews.
    void ReadShown();

    // Builds the archive of `version` on the program's thread, and lists the browser's programs and waves again from
    // it. An error shows in a message box.
    void BuildArchive(const std::shared_ptr<const format::File>& version);

    // Lists the programs of each bank and the waves of each wave archive in the browser again, from the open archive.
    void ListChildren();

    // Lists the open archive's sounds, sequences, banks and wave archives in the browser, with each item's children. A
    // file of the Wii lists its items through ListWiiBrowser.
    void ListBrowser();
    void ListWiiBrowser();

    // Follows the items of the open archive once the archive of another version has come in, on every console. The
    // browser lists the items again: a version can change what an item offers, such as a stream that Replace… or Undo
    // brings. When the items differ from the browser's, such as after an import or its undo, the places of the files'
    // sequences and of the sounds' starts follow the archive first. The browser keeps its choices of the items that
    // stay. A sound that an import added is selected.
    void FollowItems();

    // Stops playback and empties the zones. A file that has just opened shows them the same way.
    void CloseSequence();

    // Takes a read of the sequence that the zones show, from the archive of a later version. The zones show the new
    // version, and keep their selection and each track's mute and solo. A new model catches up with playback, and
    // playback goes on without a stop.
    void FollowSequence(song::SequenceReader::Result read);
    void PlayPreview(const BrowserPlace& place);
    void PlayWiiPreview(const BrowserPlace& place);

    // Gives each track of the zones' song what else an edit of its mix or its instrument changes, for the tooltips of
    // the mixer and the inspector (edit::ReachOfTracks).
    void ShowReach();

    // Gives the zones' song the places where its tracks read variables or branch, for the marker row (LogicMarkers).
    void ShowLogic();

    // Gives each track of the zones' song without notes the instrument that its opening selects, such as a new track's
    // (edit::OpeningInstrument). A note that goes in at the track's start plays it.
    void ShowOpeningInstruments();

    // Gives the zones' song the commands that set the tempo, for the tempo row (TempoPoints).
    void ShowTempo();

    // Tells the zones' song how the program edits the sequence, and whether a drag of the loop's edges moves the loop:
    // the sequence is a timeline, and its loop is the loop that plays (edit::TimelineLoop).
    void ShowMode();

    // Edits the shown sequence as a timeline or as a program (edit::AsTimeline, edit::AsProgram). The inspector's
    // switch gives the choice. The switch becomes an action in the history.
    void ApplyModeRequest();

    // Gives the shown sound the mixing mode of the request from the mixer's switch (format::WithMixing). The switch
    // becomes an action in the history.
    void ApplyMixingRequest();

    // The mixing mode of the shown sound in the version that the program shows.
    song::Mixing ShownMixing() const;

    // Gives the shown sound the engine of the request from the mixer's switch (format::WithEngine). The switch becomes
    // an action in the history.
    void ApplyEngineRequest();

    // The engine of the shown sound in the version that the program shows: the sound's engine, or the file's when
    // Hokuto can't play the sound on the sound's (edit::EngineRefused).
    format::Engine ShownEngine() const;

    // The engine whose model plays the shown sequence, and the archive and the sequence that it plays: the conversion's
    // while the shown sound plays on another console's engine, and otherwise the file's. A sound of the Wii, or a sound
    // on the Wii's engine, plays from the Wii's archive. PlayedArchive then gives nothing.
    format::Engine PlayedEngine() const;
    std::shared_ptr<const ntdlib::sar::SoundArchive> PlayedArchive() const;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> PlayedWiiArchive() const;
    const song::Sequence& PlayedSequence() const;

    // The conversion of sequence sound `sound` of `version` for the sound's engine, for the reader, or nothing when the
    // sound plays on the file's engine. The last conversion of the shown sound serves a version that builds the same
    // archive, and so does the error that stopped it. For a conversion, ConvertFor also keeps the engine in
    // `converting_engine_` and the version in `converting_version_`.
    song::SequenceReader::Convert ConvertFor(const std::shared_ptr<const format::File>& version, uint32_t sound);

    // Whether the shown sequence plays sound `sound` of `version` on the engine that the version gives the sound: its
    // conversion for that engine, or the file's sound when the conversion failed or the engine is the file's.
    bool EngineServes(const format::File& version, uint32_t sound) const;

    // Takes the conversion that `read` brought for the shown sound's engine, or its error. A new error shows in a
    // message box, and the sound plays on the file's engine. An engine that the user has just chosen, and whose
    // conversion fails, goes from the history again (DropEngineEdit). ForgetEngine drops the conversion.
    void TakeEngine(song::SequenceReader::Result& read);
    bool DropEngineEdit(format::Engine engine, uint32_t sound, bool failed);
    void ForgetEngine();

    // Starts a new model of the shown sequence with the version's effects, in the shown sound's mixing mode, and runs
    // it up to playback (ModelCatchUp). The old model plays on meanwhile. The new model runs up to the player's
    // position in ticks when the time of the ticks has changed (song::SameTiming). A model of another engine runs up to
    // the sample that the device plays. Its frames last another time than the old model's, and the player can't take
    // it over. Playback then starts again on the new model there. CancelCatchUp cancels the run.
    void StartCatchUp();
    void CancelCatchUp();

    // A new model of the shown sequence with the version's effects, in the shown sound's mixing mode, on the engine
    // that plays it (PlayedEngine). A sequence of the Wii, or a sound on the Wii's engine, plays on the Wii's model, in
    // strict mixing.
    std::shared_ptr<song::SequenceModel> MakeModel() const;

    // Opens the selected part in the Key Editor. The editor shows the part's track, from the part's start. Without a
    // selected part, it shows the selected track, from the beat of a double-click on its lane or else from the
    // position. Another song closes the editor.
    void OpenKeyEditor();
    void CloseKeyEditor();

    // Makes what the Key Editor shows of its track from the sequence that the zones show, with what an edit can change
    // in each of the track's notes, and the commands of the track's settings for the controller lanes. A track that the
    // sequence no longer has shows no notes. The selections follow the version of the sequence (FollowedSelection).
    void UpdateKeyTrack();

    // The Key Editor's selection after `edited`, an edit of sequence `sequence` of `file`: the notes of the commands at
    // `kept` in the new version, then the new notes. A selection of another version's commands keeps nothing.
    KeySelection SelectionAfter(const format::File& file, uint32_t sequence, const edit::Edited& edited,
                                const std::set<uint32_t>& kept) const;

    // The List Editor's selection after `edited`, an edit of sequence `sequence` of `file`: the selected commands in
    // the new version. A command that the edit removed leaves the selection. A selection of another version's commands
    // keeps nothing.
    KeySelection ListSelectionAfter(const format::File& file, uint32_t sequence, const edit::Edited& edited) const;

    // Draws the Key Editor while it's open. Each edit that its gestures make becomes an action in the history. An edit
    // in the controller lanes changes, adds or removes commands of the track's settings (ApplyControlEdit).
    void UpdateKeyEditor();
    void ApplyKeyEdit(const KeyEdit& edit);
    void ApplyControlEdit(const KeyEdit& edit);

    // Opens the List Editor on the selected part's track, or else on the selected track. The row where the part starts
    // is selected and scrolled to: its call's row, or its first note's (RowOfPart). Another song closes the editor.
    // ListEditorChoice gives that track's number, or else the first track's, with the part's start.
    void OpenListEditor();
    std::pair<int, std::optional<int32_t>> ListEditorChoice() const;
    void CloseListEditor();

    // Opens the List Editor on the track of the first command of a place where the tracks read variables or branch. The
    // place's commands on that track are selected, and the editor scrolls to the first.
    void OpenMarker(const MarkerPlace& place);

    // Opens the List Editor on track `track`, with the commands at the offsets `commands` selected. The editor scrolls
    // to the first row that runs one of them at tick `tick`.
    void OpenCommands(int track, std::span<const uint32_t> commands, int32_t tick);

    // Gives the command of a point of the tempo row the tempo that a drag has reached, and moves it by the drag's
    // shift. The edit becomes an action in the history.
    void ApplyTempoMove(const TempoMove& move);

    // Moves the loop of the shown timeline to where a drag of its edge in the marker row ended (edit::MoveLoop). The
    // edit becomes an action in the history.
    void ApplyLoopMove(const LoopMove& move);

    // Moves, transposes or changes the velocities of the notes of the part that the arrangement's info line shows
    // (edit::EditNotes). The edit becomes an action in the history.
    void ApplyPartEdit(const PartEdit& part_edit);

    // Makes what the List Editor shows of its track from the sequence that the zones show. The selection follows the
    // version of the sequence (FollowedSelection).
    void UpdateListTrack();

    // Draws the List Editor while it's open. Each edit that it makes becomes an action in the history. A choice of
    // another track shows that track.
    void UpdateListEditor();
    void ApplyListEdit(const ListEdit& edit);

    // Shows the subroutine of `choice` by itself in the List Editor, from the run that it names, or else from its first
    // run on the shown track, or else from its first run. A subroutine that no call of the shown sound runs doesn't
    // show.
    void OpenSubroutine(const ListEditorState::SubroutineChoice& choice);

    // Shows the track of the shown run again, with the run's call selected.
    void CloseSubroutine();

    // Finds the subroutine that the List Editor shows in `shown`, the version of the shown sequence, and its runs in
    // the shown sound. The editor then shows the track of the shown run. A subroutine that no call runs any more
    // closes.
    void FollowSubroutine(const format::Shared<ntdlib::commands::Sequence>& shown);

    // Plays the note that the Key Editor sounds, with the shown sound's banks, as an audition. The note holds while it
    // stays the same, and the last note is let go when another or none takes its place.
    void PlayFeedback(const std::optional<KeyNote>& note);

    // Writes each move of a track's control in the mixer into the sequence that the zones show. Each move becomes an
    // action in the history. A move that changes nothing, or that the sequence can't take, releases its control. The
    // control shows the song's value again.
    void ApplyMixMoves();
    bool ApplyMixMove(const song::Track& track, const MixMove& move);

    // Gives the notes that play a track's instrument the instrument that the user chose in the inspector. The edit goes
    // into the sequence that the zones show, and becomes an action in the history. An edit that the sequence can't take
    // shows a message.
    void ApplyInstrumentChoices();
    void ApplyInstrumentChoice(const song::Track& track, const InstrumentChoice& choice);

    // Takes the values that the user chose in the Variables zone for the variables of the sequence that the zones show.
    // The sequence is read again with them, and a new model catches up with playback. Playback goes on without a stop.
    // The made-up song shows the choices and nothing more.
    void ApplyVariableChoices();

    // While the sequence plays, shows the value of each variable at the sample that the device plays.
    void UpdateVariables();

    // The values that the variables of sequence sound `sound` start with, and their changes while it plays.
    song::StartValues StartValuesOf(uint32_t sound) const;
    song::Changes ChangesOf(uint32_t sound) const;

    // Shows the scenes of the sound that the zones show, from the file that the history has reached. A scene is lit
    // while the sound's variables start with its values and change as it says.
    void UpdateScenes();

    // Takes the user's action on a scene in the Variables zone. Store, Update, Rename and Delete edit the file. Each
    // one becomes an action in the history. Recall gives the sound's variables the scene's starting values and changes.
    // A choice of a value does the same.
    void ApplySceneRequest();
    void RecallScene(uint32_t sound, const format::Scene& scene);

    // Takes the user's action on a change in the Variables zone. Add makes a change at the position. Like a choice, an
    // action reads the sequence again, and playback goes on without a stop. The file keeps changes only in scenes.
    void ApplyChangeRequest();

    // Gives the song's tracks the colours set in Preferences, and takes the colour that the user picked for a track. A
    // pick is an edit of the file.
    void ShowTrackColours();
    void ApplyColourPick();

    // Takes the change that the user made in `section`, the Sound section of the inspector or the browser. A change is
    // an edit of the archive's tables. The program then builds the version's archive, and playback follows without a
    // stop.
    void ApplySoundChange(SoundSection& section);

    // Takes the tag that the user typed in `section`. The edit changes the file's tags alone.
    void ApplyTagChange(SoundSection& section);

    // Takes the change of a wave sound's values in its wave sound file that the user made in `section`. The section
    // shows the new values at once. ShowWaveSoundIn shows the values of the section's wave sound, from the archive that
    // the program has built, and the sizes of the groups that load its file.
    void ApplyWaveSoundChange(SoundSection& section);
    void ShowWaveSoundIn(SoundSection& section);

    // Whether the editors of items wait: while the program builds the archive of another version, or another window
    // waits. They take no edits meanwhile.
    bool EditorsWait() const;

    // The sound that the browser selects: a sequence sound, or another sound. Nothing for another kind of item.
    std::optional<uint32_t> BrowsedSound() const;

    // Gives the song's view the changes of sound `sound`, each at its place in the arrangement.
    void ShowChanges(uint32_t sound);

    // The time of the transport's position, in milliseconds from the song's start.
    double PositionTime() const;

    // The sound that the zones show: the sequence's sound, or 0 for the made-up song.
    std::optional<uint32_t> ShownSound() const;

    // The index among the file's sequences of the sequence that the zones show, or nothing.
    std::optional<uint32_t> ShownSequence() const;

    // The index among the file's sequences of the sequence that a sound of the open archive plays, or nothing.
    std::optional<uint32_t> SequenceOfSound(uint32_t sound) const;

    // Where each sound of the open archive plays in the version that built it (edit::SoundPlaces), on the Wii too.
    // Empty without the fields of the sounds' starts.
    std::vector<std::optional<edit::SoundPlace>> Places() const;

    // The names of the sounds of the open archive, by their places in the sound table.
    std::vector<std::string> SoundNames() const;

    // The track of the sequence that the zones show whose number is `number`, or nullptr.
    const song::Track* TrackNumbered(int number) const;

    // The track that the track list selects, or nullptr.
    const song::Track* SelectedTrack() const;

    // Whether the shown sound's tracks can take an edit: the file has the archive's tables, the archive comes from the
    // file's current version, and no window or read waits.
    bool TracksEditable() const;

    // Whether a window that waits for an answer is open: the Replace and Import windows, the Export window and the New
    // Project window. On the Mac, the menu bar works while Dear ImGui's windows wait. The commands that open another
    // such window, or change the file, wait for it to close.
    bool WindowWaits() const;

    // Project > Add Track, Duplicate Tracks, Remove Selected Tracks, Copy Track and Paste Track, on the shown sound. A
    // new track plays the selected track's instrument. Copy Track keeps the selected track, and Paste Track gives a
    // copy of it to the shown sound. A new track, a copy and a pasted track become the selection. Each edit becomes an
    // action in the history. Copy Track changes nothing.
    void EditTracks(TrackAction action);

    // Playback follows the transport: it starts and stops with it, and starts again from where the position moves to.
    // The position follows the sample that the device plays. Without a sequence, the transport runs on the frames'
    // clock.
    void UpdatePlayback();
    void StartPlayback(std::optional<uint32_t> start = std::nullopt);
    void StopPlayback();

    // Where playback starts again on a model of another engine that has caught up: the model's tick near the position
    // that the device plays, or else that position.
    uint32_t ChangeOverTick() const;

    // The stereo frames of the playback's output that the device has played, and those that it hasn't played yet: the
    // frames in the ring and in the device's buffer, at the player's rate.
    uint64_t HeardSamples() const;
    uint64_t QueuedSamples() const;

    // The transport's cycle in the sequence's ticks, for the playback clock.
    song::PlaybackClock::CycleRange CycleInTicks() const;

    // The tracks that mute and solo silence, a bit for each track's number, for the playback clock.
    uint32_t MutedTracks() const;

    // The volumes of the aux buses for the playback clock: the document's, with the level of the FX channel's fader
    // that the user holds. Playback follows a fader during the hold.
    song::AuxVolumes BusVolumes() const;

    // The FX channels' faders follow the document's volumes of the aux buses, but for the fader that the user holds. A
    // hold that ends with a new level makes an edit of the channel's volume.
    void FollowBusVolumes();
    void ApplyFxMove();

    std::string settings_path_;
    Settings settings_;
    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* gpu_ = nullptr;
    bool window_claimed_ = false;
    bool imgui_sdl3_ = false;
    bool imgui_gpu_ = false;

    audio::FrameRing ring_;
    audio::RenderWorker worker_;
    audio::FrameRing audition_ring_;
    audio::RenderWorker audition_worker_;
    AudioOutput output_;
    int audition_rate_ = 0; // the rate of the last audition

    // When something last sounded, and when the latest timer of RestOutput wakes the program, in SDL3's nanoseconds.
    uint64_t output_sounded_ = 0;
    uint64_t output_timer_ = 0;

    Transport transport_;
    std::optional<SongView> song_;
    std::string file_name_;
    OpenViews views_;
    OpenViews saved_views_; // the zones that the program's settings hold
    std::vector<BrowserGroup> browser_;
    BrowserState browser_state_;

    // The open sound archive, each browser item's index among the archive's sounds, banks or wave archives, and the
    // numbers of each item's children: a bank's programs, or a wave archive's waves. The archive came from
    // `archive_version_`, a version of the file. While the reader builds the archive of another version, that version
    // is `reading_version_`.
    std::shared_ptr<const ntdlib::sar::SoundArchive> archive_;
    std::shared_ptr<const format::File> archive_version_;
    std::shared_ptr<const format::File> reading_version_;

    // The archive of a file of the Wii, read for playback (ntdlib::rvl::snd::kRsarOptions), in place of `archive_`. It
    // came from `archive_version_` too.
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_archive_;

    // The sequence of the file that each of the archive's files holds, by the file's ID, and the field of the native
    // layout that holds each sound's start, by the sound's index. Edits leave both as they are. The fields are missing
    // when the archive's tables can't be read.
    std::vector<std::optional<uint32_t>> file_sequences_;
    std::optional<std::vector<std::optional<uint32_t>>> start_fields_;
    std::array<std::vector<uint32_t>, 5> browser_items_;
    std::array<std::vector<std::vector<uint32_t>>, 5> browser_children_;

    // The number of the track that the track list selects once the reader has read the version of a track's edit.
    std::optional<int> track_to_select_;

    // The track that Copy Track kept, for Paste Track. It lasts until another file opens.
    std::optional<edit::CopiedTrack> copied_track_;

    // The browser's item that the zones show, or that the reader reads for them.
    int shown_group_ = -1;
    int shown_item_ = -1;
    song::SequenceReader reader_;

    // The values that the user chose for the variables of each sequence sound, and their changes, by the sound's index.
    // A sequence plays with them each time that it's read. They last until another file opens. The Variables zone shows
    // the scenes of the shown sound. `scene_places_` holds each one's place among the file's scenes. `variables_state_`
    // keeps the zone's state between frames.
    std::map<uint32_t, song::StartValues> start_values_;
    std::map<uint32_t, song::Changes> changes_;
    std::vector<std::size_t> scene_places_;
    VariablesState variables_state_;

    // The sequence that the zones show, the model that plays it, and the clock of its playback while it plays. Each
    // playback of the sequence plays on the same model. The model keeps its snapshots for seeks. `playback_moves_`
    // counts the transport's moves when playback started. `metered_` counts the stereo frames of the playback's output
    // that the tracks' meters have taken.
    std::shared_ptr<const song::Sequence> sequence_;
    std::shared_ptr<song::SequenceModel> model_;
    std::shared_ptr<song::PlaybackClock> clock_;
    int playback_rate_ = 0; // the player's output rate
    uint64_t playback_moves_ = 0;
    uint64_t metered_ = 0;

    // The run of a new model after an edit. During playback, the model catches up with the player, and the clock then
    // offers it to the player. While playback is stopped, the model runs through the sequence's first pass. Its
    // snapshots then cover the next playback's seek. A playback that starts takes the model as far as it has run.
    // `engine_catch_up_` holds the engine of a model that runs for another engine than `model_engine_`.
    song::ModelCatchUp catch_up_;
    std::optional<format::Engine> engine_catch_up_;

    // The versions of the Hokuto file that the program shows, the Undo History's rows for the actions, and the Key
    // Editor's selections around each action. The made-up song's effects get a history too, with no file to save. The
    // file's path is where the user opened or saved it. The path is empty while the file is untitled. An untitled
    // file's dialog starts beside the sound archive that the file came from. The paths are in UTF-8.
    std::optional<edit::History> history_;
    std::vector<HistoryRow> history_rows_;
    std::vector<ActionSelections> action_selections_;
    bool preview_ = false;
    std::string document_path_;
    std::string import_path_;
    bool text_form_ = false;

    // The open or the import whose work runs. A cancelled open's work runs on to its end. It waits in `dropped_opens_`
    // until then.
    std::optional<Opening> opening_;
    std::vector<std::future<OpenWork>> dropped_opens_;

    // The mixer's FX channels, the effect whose window is open, the window's draft of the effect, and whether a drag or
    // a typed value is under way in the window. The model plays `model_effects_` in `model_mixing_`.
    FxChannels fx_;
    std::optional<EffectPlace> effect_window_;
    std::optional<format::Effect> effect_draft_;
    bool effect_changing_ = false;
    BusEffects model_effects_;
    song::Mixing model_mixing_ = song::Mixing::kStrict;

    // The shown sound on another console's engine: its conversion for that console, with the sound's index in the
    // conversion's archive and what changed on the way, the sequence as that console's performer reads it, and the
    // version that the conversion came from. While `engine_sound_` holds the conversion, the model plays it. The error
    // that stopped the last conversion stays in `engine_error_`, with the engine that it was for in `engine_failed_`.
    // The sound then plays on the file's engine. The reader's latest request converts for `converting_engine_` from
    // `converting_version_`. `model_engine_` is the engine whose model plays.
    std::optional<song::SequenceReader::EngineSound> engine_sound_;
    std::shared_ptr<const song::Sequence> engine_sequence_;
    std::shared_ptr<const format::File> engine_version_;
    std::string engine_error_;
    std::optional<format::Engine> engine_failed_;
    std::optional<format::Engine> converting_engine_;
    std::shared_ptr<const format::File> converting_version_;
    format::Engine model_engine_ = format::Engine::kSwitch;

    // The action that the mixer's engine switch made last. Its conversion's first read takes it.
    std::optional<EngineEdit> engine_edit_;

    // The window in front of the lower zone. The mixer, the Key Editor and the List Editor share the zone. The zone's
    // dock node has no tab bar, and Dear ImGui shows the first window docked in such a node. Only the window in front
    // is drawn, and the node then holds it alone. Opening an editor brings it to the front, and the mixer's command
    // brings the mixer. `behind_mixer_` is the window that the mixer came in front of. It comes back when the mixer
    // closes.
    LowerZone lower_front_ = LowerZone::kMixer;
    LowerZone behind_mixer_ = LowerZone::kKeyEditor;

    // The name of a window that takes the focus at the start of the next frame. At the end of a frame, Dear ImGui
    // focuses the window under a click that no item took, after any request of that frame.
    const char* focus_next_frame_ = nullptr;

    // The Key Editor: whether it's open, the number of the track that it shows, what it shows of the track, and its
    // state between frames. The selection names the commands of `key_sequence_`, the version of the shown sequence that
    // the editor shows. The selection that an edit, an undo or a redo leaves for a new version waits in
    // `key_selection_` until the editor shows a new version. The selection of the controller lanes waits in
    // `control_selection_` the same way.
    bool key_editor_open_ = false;
    int key_editor_track_ = -1;
    KeyTrack key_track_;
    KeyEditorState key_editor_;
    format::Shared<ntdlib::commands::Sequence> key_sequence_;
    std::optional<KeySelection> key_selection_;
    std::optional<KeySelection> control_selection_;

    // The List Editor: whether it's open, the number of the track that it shows, what it shows of the track, and its
    // state between frames. The selection names the commands of `list_sequence_`, the version of the shown sequence
    // that the editor shows. The selection that an edit, an undo or a redo leaves for a new version waits in
    // `list_selection_`. The Key Editor's selections work the same way.
    bool list_editor_open_ = false;
    int list_editor_track_ = 0;
    ListTrack list_track_;
    ListEditorState list_editor_;
    format::Shared<ntdlib::commands::Sequence> list_sequence_;
    std::optional<KeySelection> list_selection_;

    // The subroutine that the List Editor shows by itself, and the version of the shown sequence whose offset `first`
    // names its first command. In another version, the subroutine's label finds it.
    std::optional<ListSubroutine> list_subroutine_;
    format::Shared<ntdlib::commands::Sequence> list_subroutine_version_;

    // The note that the Key Editor sounds, and the flag that lets it go.
    std::optional<KeyNote> feedback_note_;
    std::shared_ptr<std::atomic<bool>> feedback_release_;

    // The Bank Editor: the bank that it shows by its place in the archive's list, what it shows of the bank, the
    // version that the view's archive came from, and the editor's state between frames. Then the program and the key
    // that its keyboard sounds, and the flag that lets the key go. The files that the system drops on the editor wait
    // in `bank_drops_` until the drop completes, and `bank_drop_` holds where the first of them landed. Files dropped
    // while a field was being typed in then wait in `waiting_bank_drop_` while the reader builds and reads another
    // version (`reading_version_`). The sizes of the groups that load the bank follow from a thread. `bank_to_open_`
    // holds the bank that Project > Add Bank… added, until the program has built the archive that holds it.
    std::optional<uint32_t> bank_editor_;
    std::optional<BankView> bank_view_;
    std::shared_ptr<const format::File> bank_view_version_;
    BankEditorState bank_state_;
    std::optional<std::pair<uint32_t, int>> bank_note_;
    std::shared_ptr<std::atomic<bool>> bank_release_;
    std::vector<std::string> bank_drops_;
    std::optional<BankDrop> bank_drop_;
    std::optional<WaitingBankDrop> waiting_bank_drop_;
    std::future<std::vector<std::string>> bank_group_sizes_;
    std::optional<uint32_t> bank_to_open_;

    // The files that the system drops on the window outside the Bank Editor, until the drop completes. The first opens.
    std::vector<std::string> window_drops_;

    // The properties of a wave or a stream: the wave, or the stream sound whose stream they show, what they show of the
    // item, the version that the view's archive came from, and the window's state between frames. The sizes of the
    // groups that load a wave's wave archive follow from a thread.
    std::optional<WaveTarget> properties_wave_;
    std::optional<uint32_t> properties_stream_;
    std::optional<WaveView> wave_view_;
    std::shared_ptr<const format::File> wave_view_version_;
    WaveWindowState wave_state_;
    std::future<std::vector<std::string>> wave_group_sizes_;

    // The measures of the groups that a new view, a closed editor or a section without a wave sound dropped run on to
    // their ends in `dropped_group_sizes_`. Each goes in the first frame after its end (Iterate).
    std::vector<std::future<std::vector<std::string>>> dropped_group_sizes_;

    // The file from a dialog, until the program acts on it. A cancelled Save As leaves an empty path.
    std::mutex chosen_mutex_;
    std::optional<std::pair<Dialog, std::string>> chosen_;

    // What waits for the Save As dialog that AfterChanges showed.
    std::function<void()> after_save_;

    // The commands and the drops that came while a text field was being typed in. Each runs in a later frame, once the
    // field has kept its text.
    std::vector<WaitingCommand> waiting_commands_;

    // The export that the Export window shows. A cancelled export's work runs on to its end. It waits in
    // `dropped_exports_` until then.
    std::optional<Export> export_;
    std::vector<std::future<ExportWork>> dropped_exports_;
    std::vector<std::future<MeasuredView>> dropped_measures_;

    // The centre of the Export window where the user last dragged it. The window opens there until Hokuto quits.
    std::optional<ImVec2> export_place_;

    // What the dialog of Replace… gives a file for: a sound's sequence, an item's file, a wave or a stream sound's
    // stream. Then the replacement that the Replace window shows.
    std::optional<uint32_t> replace_sound_;
    std::optional<FileTarget> replace_file_;
    std::optional<WaveTarget> replace_wave_;
    std::optional<uint32_t> replace_stream_;
    std::optional<Replacement> replace_;

    // The centre of the Replace window where the user last dragged it. The window opens there until Hokuto quits.
    std::optional<ImVec2> replace_place_;

    // The work of a cancelled import runs on to its end. It waits in `dropped_edits_` until then.
    std::vector<std::future<EditWork>> dropped_edits_;

    // The work of the Replace window's Play, until the preview starts. A dropped preview's work waits in
    // `dropped_previews_` until it's done.
    std::optional<std::future<PreviewWork>> preview_work_;
    std::vector<std::future<PreviewWork>> dropped_previews_;

    // Whether the worker plays the Replace or Import window's Play.
    bool replace_previewing_ = false;

    // The transport's presses of Stop while stopped that the program has acted on (Transport::IdleStops).
    uint64_t idle_stops_ = 0;

    // The sound that an import added, until the browser lists it and selects it.
    std::optional<uint32_t> select_sound_;

    // The view that the open file keeps, until its sound shows.
    std::optional<format::EditorView> pending_view_;

    // The sounds that have shown while the file is open. The message of a sound's banks in group files that the file
    // lacks shows once (song::MissingBanksNote).
    std::set<uint32_t> told_missing_banks_;

    // Whether SDL3 waits for an event before the next frame, whether the last frame drew for work that moves (see
    // Pace), when the last event came, in SDL3's nanoseconds, and the type of the event that Wake sends.
    bool waiting_ = false;
    bool moving_ = false;
    uint64_t last_event_ = 0;
    uint32_t wake_event_ = 0;

    ArrangementState arrangement_;
    MasterBus master_;

    // The Sound sections of the inspector and the browser. The inspector's section shows the sound that the zones show,
    // and the browser's section the sound that the browser selects.
    SoundSection sound_section_;
    SoundSection browser_sound_;
    bool native_menus_ = false;
    bool studio_setup_open_ = false;
    bool preferences_open_ = false;
    bool project_setup_open_ = false;
    TextTyping project_typing_;
    bool new_project_open_ = false;
    format::Engine new_project_engine_ = format::Engine::kSwitch;
    bool reset_layout_ = false;
    bool quit_ = false;
};

} // namespace hokuto::app
