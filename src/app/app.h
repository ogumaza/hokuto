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

#include "app/app_work.h"
#include "app/audio_output.h"
#include "app/bank_editor.h"
#include "app/effect_editor.h"
#include "app/export_window.h"
#include "app/item_views.h"
#include "app/key_edits.h"
#include "app/list_editor.h"
#include "app/list_edits.h"
#include "app/menus.h"
#include "app/replace_flow.h"
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
#include "edit/notes.h"
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
    explicit App(bool preview);

    bool Init();

    // The steps of Init that can fail: the main window at the display's `scale`, the GPU device that draws it, and Dear
    // ImGui's back ends with the program's look. Each returns false after an error. A message box shows the error.
    bool CreateMainWindow(float scale);
    bool CreateGpu();
    bool StartImGuiBackends(float scale);

    void LoadFonts();

    // The files of a drop (HandleEvent). Each file that lands on the Bank Editor waits with the place where the first
    // one landed. The others wait for the window. Once the drop is complete, the files go to the bank or the window.
    void TakeDroppedFile(const SDL_DropEvent& drop);
    void FinishDrop();

    // The settings' part of the settings file. Dear ImGui reads and writes the file.
    void AddSettingsHandler();
    void ReadSetting(std::string_view line);
    void WriteSettings(ImGuiTextBuffer& out) const;

    // Opens the file that a dialog chose, or saves or exports to it (Iterate).
    void OpenChosenFile();

    // Shows the sequence that the reader has read, with the archive of the version that a read built (Iterate).
    void TakeReadSequence();

    // Starts what the browser's rows asked for in this frame: an export, a replacement or an editor (Iterate).
    void ApplyBrowserRequests();

    // Applies what the arrangement asked for in this frame: an editor, a marker, a tempo point, a move or an edit
    // (Iterate).
    void ApplyArrangementRequests();

    // Shows the effects on the aux buses in the mixer's FX channels (Iterate). Returns the effects, or nullptr without
    // them.
    const BusEffects* UpdateMixerEffects();

    // Shows the shown sound's mixing mode, engine and sends in the mixer's switches and strips (Iterate).
    void UpdateMixerSwitches(SongView& song);

    // The space in which the zones dock, with the default layout where none is saved (Iterate).
    void DrawZoneSpace();

    // The hint that an empty arrangement shows (Iterate).
    std::string ArrangementHint() const;

    // Updates and draws the mixer, and applies its moves of faders and effects (Iterate). `song` is the shown song, if
    // any. The controls wait while the program is `busy`.
    void UpdateMixer(SongView* song, bool busy);

    // Draws the windows that float over the zones: the setup windows, Export, Replace and the progress of work
    // (Iterate).
    void DrawWindows();

    void HandleShortcuts();

    // Ends the typing of a text field before a command or a drop, and keeps `run` for a later frame. `run` gives the
    // command or the drop again. Returns false while no text field is being typed in: the command or the drop goes on
    // at once.
    bool WaitForTyping(std::function<void()> run);

    // Runs the commands that waited for a text field to keep its text.
    void RunWaitingCommands();

    void DrawMenus();
    void DrawMenuItems(const std::vector<MenuItem>& items);
    void DrawListedMenu(const MenuItem& item);
    void DrawControlBar();
    void DrawStudioSetup();

    // The sections of Studio Setup: the audio device with its format and a test tone, and the DSi's digest key.
    void DrawAudioSection();
    void DrawDsKeySection();

    // Keeps `path` as the file of the DSi's digest key in the program's settings, or no file for an empty path. A file
    // that doesn't contain a key leaves the setting as it was, and a message says why.
    void SetDsiKey(const std::string& path);
    void DrawPreferences();

    // Project Setup shows the file's engine, gives every sound a mixing mode, and edits the file's tags.
    void DrawProjectSetup();

    // The Tags section of Project Setup: a box of text for each key that the program offers. Returns the tag that the
    // user changed, by its key.
    std::optional<std::pair<std::string, std::string>> DrawTagsSection(const format::File* document);

    // File > New Project… opens a window for the console of a new file. Create then shows the new file (NewDocument),
    // and Cancel closes the window.
    void DrawNewProject();

    // The Project section of New Project: a menu of the consoles of a new file, and what the new file contains.
    void DrawNewProjectEngine();

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

    // Marks every preview as stopped: the previews of the browser, the Wave window and the Replace window.
    void ForgetPreviews();

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

    // Logs a message for the user and shows it in a box over the main window, with the icon of its kind. The box opens
    // at the end of the first frame in which no mouse button is down (MessageQueue). Messages keep their order. One
    // that already waits isn't added or logged again.
    void ShowMessage(const std::string& message, MessageKind kind = MessageKind::kError);
    void ShowWaitingMessages();

    // Shows the waiting messages before a direct box or a system dialog opens, so that they keep their order. Those
    // open only from controls that act at the release, keys, the window's close and dialogs: with no button down. A
    // button that is down all the same goes to the log.
    void ShowMessagesBeforeBox();
    void OpenMessageBoxes(bool button_down);

    // Reads a file that the user chose or dropped into `bytes`. Shows a message and returns false when the file can't
    // be read.
    bool ReadChosenFile(const std::filesystem::path& file, std::vector<uint8_t>& bytes);

    // Shows a dialog. SDL3 may call the dialog's callback on another thread. The program acts on the chosen file in its
    // next frame.
    void ShowDialog(Dialog dialog);

    // The filters of `dialog`'s files. ImportDialogFilters returns the filters of File > Import, and
    // ReplaceDialogFilters the filters of Replace…, by the item that the user replaces.
    std::span<const SDL_DialogFileFilter> DialogFilters(Dialog dialog) const;
    std::span<const SDL_DialogFileFilter> ImportDialogFilters(Dialog dialog) const;
    std::span<const SDL_DialogFileFilter> ReplaceDialogFilters() const;

    // Where Save As starts: the file's place, or beside the sound archive that it came from, with the archive's name.
    std::filesystem::path SaveAsLocation() const;

    // Where an export starts, beside the sound archive or the file that the document came from, or nothing.
    std::optional<std::filesystem::path> ExportLocation(bool folder) const;

    // The callback of the file dialog of `kDialog`. SDL3 calls it with the chosen file, on any thread.
    template <Dialog kDialog>
    static void SDLCALL Chosen(void* user_data, const char* const* files, int filter);
    void Choose(Dialog dialog, const char* const* files);

    // Opens the file at `path`, a UTF-8 path, on a thread of its own: a Hokuto file in either form, or else a sound
    // archive to import. A file dropped on the window and a file named on the command line open this way. A new open
    // drops the work of an open that still runs. The message of a file of another kind names File > Import > Sound
    // Archive… when `import` marks that command.
    void OpenOrImport(const std::string& path, bool import = false);

    // The work of OpenOrImport for the file at `path`. It reads the file. A Hokuto file of `name` with `bytes` opens,
    // and the program builds the archive that the file holds in memory. A sound archive at `file` of the Switch, the
    // Wii U or the 3DS, or of the Wii, comes in as a new file. So does a GBA ROM whose songs MusicPlayer2000 plays, and
    // a sound archive of the DS or a DS cartridge image that contains one.
    static OpenWork Opened(const std::string& path, bool import);
    static OpenWork OpenedFile(const std::string& name, std::span<const uint8_t> bytes);
    static OpenWork ImportedArchive(const std::filesystem::path& file, std::vector<uint8_t> bytes);
    static OpenWork ImportedWiiArchive(const std::filesystem::path& file, std::vector<uint8_t> bytes);
    static OpenWork ImportedGbaRom(const std::filesystem::path& file, std::vector<uint8_t> bytes);
    static OpenWork ImportedDs(const std::filesystem::path& file, std::vector<uint8_t> bytes);

    // Once an open's work is done, shows the file that it made, or the message that says why the file can't open.
    void FinishOpening();

    // Puts the Hokuto file at `path`, a UTF-8 path, first in File > Recent Projects.
    void AddRecentProject(const std::string& path);

    // The window that waits while an open's work runs. Cancel drops the work.
    void DrawOpening();

    // The window that shows while the program prepares an Import or Replace window for a file from a dialog
    // (OpenChosenFile). The preparation runs on the program's thread. The window therefore has no Cancel.
    void DrawPreparing();

    // Shows a new file for `engine` (format::NewFile). The file stays untitled until it's saved.
    void NewDocument(format::Engine engine);

    // Shows the Hokuto file `document`, called `name`, and plays `played`, the archive, the ROM or the set that it
    // contains. `sequences` gives the sequence of the file that each of the archive's files contains
    // (format::SequencesOfFiles), and `start_fields` the field that contains each sound's start
    // (format::StartFieldsOfSounds). Without the fields, no sequence can be replaced. The browser lists the archive's
    // sounds, sequences, banks and wave archives. The file starts a new history. For the Wii's archive, the browser
    // lists the archive's sounds and sequences. For a GBA ROM, it lists the ROM's songs. For a DS archive, it lists the
    // archive's sequences.
    void ShowDocument(std::shared_ptr<const format::File> document, song::Played played,
                      std::vector<std::optional<uint32_t>> sequences,
                      std::optional<std::vector<std::optional<uint32_t>>> start_fields, const std::string& name);

    // Ends the work on the last file: playback, a preview, an export, a replacement, a read and the worker.
    void EndLastFileWork();

    // Lists the items of the new file in the browser, with nothing chosen or shown and no search.
    void ResetBrowser();

    // Selects the sound of the view that the file keeps, or drops the view when the browser doesn't list the sound.
    void SelectKeptViewSound();

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

    // Takes the files and the warnings of the export work once the work is done, and offers the option that they call
    // for.
    void TakeExportWork();

    // Drops the work for the old target or option after `choice`, and starts the work again.
    void RestartExportWork(ExportChoice choice);

    // Writes the exported archive at `path`, with the streams and the group files beside it. Returns false when the
    // user keeps the files that the export would replace.
    bool WriteArchiveExport(const std::string& path, const ExportWork& work);

    // Writes the MIDI file at `path`, with its SoundFont or its SFZ files. Returns false when the user keeps the files
    // that the export would replace.
    bool WriteMidiExport(const std::string& path, const ExportWork& work);

    // Writes the new file of the other console at `path`. Returns false when the user keeps the file there.
    bool WriteSongExport(const std::string& path, const ExportWork& work);

    // Writes the item's file, or the wave's WAV file, at `path`, with the files beside it. Returns false when the user
    // keeps the files that the export would replace.
    bool WriteItemExport(const std::string& path, const ExportWork& work);

    // Writes the bank's SoundFont at `path`, or its SoundFonts beside it. Returns false when the user keeps the files
    // that the export would replace.
    bool WriteSoundFontExport(const std::string& path, const ExportWork& work);

    // Writes the bank's SFZ files into the folder at `path`. Returns false when the user keeps the files that the
    // export would replace.
    bool WriteSfzExport(const std::string& path, const ExportWork& work);

    // File > Export's archive, or its GBA ROM. A GBA ROM without room for the songs that their edits make longer offers
    // the file's choice for them as the window's option (format::NativeArchive::gba_filler). Without it, their edits
    // stay out of the ROM. With it, the songs go into the ROM's filler runs. A click on the option changes the choice
    // with an edit, and starts the work again.
    void StartArchiveWork(Export& job);

    // The names of the sounds of `version` that play on another console's engine. The file's tables name them, or
    // else the archive or the ROM that the zones play.
    std::vector<std::string> EngineSoundNames(const format::File& version) const;

    // File > Export > Another Console… offers a target for each other console. The work sends the shown sequence's
    // sound to the chosen target's console, and a choice of another target starts it again.
    void StartSongWork(Export& job);

    // File > Export's MIDI files, and the browser's Export… of a sequence to MIDI, convert sound `sound`. The work
    // reads a sound that the zones don't show for the warnings and the measure.
    void StartMidiWork(Export& job, uint32_t sound);

    // The name of the MIDI file of `sound`: the sound's name made safe for a file.
    std::string MidiFileName(uint32_t sound) const;

    // File > Export's MIDI file and SoundFont of song `song` of a GBA ROM (song::ConvertToMidi).
    void StartGbaMidiWork(Export& job, uint32_t song);

    // The browser's export of a GBA song as assembly, in the form of the GBA decompilations' songs
    // (song::GbaSongAssembly).
    void StartGbaAssemblyWork(Export& job);

    // The same for a voice group as assembly, or a voice's sample as the ROM contains it or as an AIFF file. The file's
    // name follows the voice group's SoundFont.
    void StartGbaBankFileWork(Export& job);

    // File > Export's MIDI file with its SoundFont or its SFZ files of sound `sound` of a DS archive. ntdlib's
    // converter of NITRO-Composer sequences converts the sound (song::ConvertToMidi).
    void StartDsMidiWork(Export& job, uint32_t sound);
    void StartDseMidiWork(Export& job, uint32_t sound);

    // The same for sound `sound` of a ROM with another of the GBA's drivers: a MIDI file with its SoundFont.
    void StartDriverMidiWork(Export& job, uint32_t sound);

    // The work of StartGbaMidiWork, StartDsMidiWork, StartDseMidiWork and StartDriverMidiWork, from `source`. The
    // source that the zones play serves when it came from the version. Otherwise the work first builds the version's
    // source. The MIDI file takes the tags. A sound that the zones don't show gives the warnings from its sequence, and
    // the measure plays that sequence.
    template <typename Source>
    void StartSourceMidiWork(Export& job, uint32_t sound, MidiSource<Source> source);

    // The browser's export of a bank, an instrument, a wave archive, a wave or a stream of a DS archive: its file, a
    // bank's instruments as SoundFont 2 or SFZ files, or the waves and a stream as WAV files.
    void StartDsItemWork(Export& job);

    // The browser's export of a bank, an instrument, a bank with samples or a wave of DSE's files: its file, a bank's
    // instruments as SoundFont 2 or SFZ files, or the waves as WAV files.
    void StartDseItemWork(Export& job);

    // The browser's export of the samples of a ROM with another of the GBA's drivers, or of one of them: a SoundFont 2
    // file or WAV files.
    void StartDriverItemWork(Export& job);

    // The browser's Export… exports the item at `place` the same way. The Export window offers the item's targets, and
    // a choice of another target starts the work again (StartItemWork). A wave archive's WAV files and a bank's SFZ
    // files go into a folder from the system's dialog. An item of a file of the Wii takes StartWiiItemWork. A voice
    // group of a GBA ROM, or a voice of it, uses StartGbaVoiceGroupWork. The four that follow start the export of an
    // item of a ROM with another of the GBA's drivers, of DSE's files, of a DS archive and of a GBA ROM.
    void StartItemExport(const BrowserPlace& place);
    void StartDriverItemExport(const BrowserPlace& place);
    void StartDseItemExport(const BrowserPlace& place);
    void StartDsItemExport(const BrowserPlace& place);
    void StartGbaItemExport(const BrowserPlace& place);
    void StartItemWork(Export& job);

    // The export of an item of a sound archive of the 3DS, the Wii U, the Switch or the Wii (StartItemExport), and the
    // item at `place`, at index `index` of group `group` of the browser.
    void StartSarItemExport(const BrowserPlace& place);
    song::Item SarItemAt(const BrowserPlace& place, std::size_t group, std::size_t index) const;

    // The work of an item of the archive of the 3DS, the Wii U or the Switch (StartItemWork), and the name of its file
    // that the dialog offers.
    void StartSarItemWork(Export& job);
    std::string SarItemFileName(const Export& job) const;

    // The work of an item of the Wii's archive (StartItemWork), and the name of its file that the dialog offers.
    void StartWiiItemWork(Export& job);
    std::string WiiItemFileName(const Export& job) const;

    void StartGbaVoiceGroupWork(Export& job);

    // Writes `files` into `folder`, each under its name. When the folder holds some of them already, the user chooses
    // to replace them or to cancel (AskToReplace). Returns false when the user cancels. Throws std::runtime_error with
    // a message for the user when a file can't be written.
    // The question leaves out the first `asked` files: a save dialog asked about them.
    bool WriteIntoFolder(const std::filesystem::path& folder,
                         const std::vector<std::pair<std::string, std::span<const uint8_t>>>& files,
                         std::size_t asked = 0);
    bool AskToReplace(const std::string& message);

    // Whether a save or an export may write `file`: the name `chosen` that the save dialog gave, with the extension
    // that WithExtension added. The dialog asked about the chosen name only. When the extension made a new name and the
    // folder holds a file of that name, the user chooses to replace it or to cancel (AskToReplace).
    bool MayWriteTo(const std::string& chosen, const std::filesystem::path& file);

    // Ends the export that the Export window shows. Its work runs on until it's done. Its measure stops early.
    void DropExport();

    // Moves the unfinished work of `job` and its measure into the lists of dropped work (dropped_exports_ and
    // dropped_measures_). The work runs on until it's done. The measure stops early.
    void DropExportWork(Export& job);

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

    // The Replace… of the sequence of the sound in `replace_.sound` with the file at `path` (OpenReplacement).
    void OpenSequenceReplacement(const std::string& path);

    // The steps of DrawReplace: a preview whose work is done plays, or its message shows. A confirmed import whose work
    // is done is added to the history, or the window shows why it failed. A choice in the window is passed to the
    // handler for the console of the file.
    void TakeDonePreview();
    void FinishPendingEdit();
    void ApplyReplaceChoice(ReplaceChoice choice);

    // The handlers of the user's choice in the Replace window (DrawReplace), by the incoming file: a SoundFont in a GBA
    // file, a SoundFont, a MIDI file in a GBA file, a file for DSE's files, a file for the DS, and the other files.
    void ApplyGbaSoundFontChoice(ReplaceChoice choice);
    void ApplySoundFontChoice(ReplaceChoice choice);
    void ApplyGbaMidiChoice(ReplaceChoice choice);
    void ApplyDseChoice(ReplaceChoice choice);
    void ApplyDsChoice(ReplaceChoice choice);

    // Plays the DS's wave or stream that the Replace window makes from the incoming file (ApplyDsChoice).
    void PlayNewDsWave();

    // Makes the view of a new DS item again from the choices in the Replace window.
    void ChangeDsNewItem();

    // Starts the edit that imports a new DS item.
    void ImportDsNewItem();

    // Makes a DS replacement an action in the history.
    void ReplaceDsItem();
    void ApplyArchiveChoice(ReplaceChoice choice);

    // Plays, makes again or imports a new stream from the Replace window's choice. Returns whether the choice was
    // one of these.
    bool ApplyArchiveStreamChoice(ReplaceChoice choice);

    // Plays the new stream of the Replace window on the Wii or on a later console (ApplyArchiveStreamChoice).
    void PlayNewWiiStream();
    void PlayNewSarStream();

    // Plays, makes again or imports a new wave from the Replace window's choice. Returns whether the choice was one of
    // these.
    bool ApplyArchiveWaveChoice(ReplaceChoice choice);

    // Makes the import's view again, or makes the import, from the Replace window's choice. Returns whether the
    // choice was one of these.
    bool ApplyArchiveImportChoice(ReplaceChoice choice);

    // Makes a stream's replacement again, or replaces the stream, from the Replace window's choice. Returns whether
    // the choice was one of these.
    bool ApplyStreamReplacementChoice(ReplaceChoice choice);

    // Makes a file's replacement again from another choice in its rows. Returns whether the choice was one of these.
    bool ApplyArchiveFileChoice(ReplaceChoice choice);

    // Makes the replacement an action in the history. A replacement that joins groups starts an edit for them.
    void FinishArchiveReplace();

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

    // Starts the work of the Replace window's Play on a thread. The work builds the version that `make` makes and
    // prepares `target` to play.
    void StartPreview(std::function<format::File()> make, PreviewTarget target);

    // File > Import > Sequence… brings a sequence file in as a new sound of the archive (edit::AddSequenceSound). The
    // file comes from the system's dialog. The Import window then names the new sound and offers the sounds whose
    // settings it can copy, the file's starts and the programs. Import makes the edit from the version that the history
    // has reached, and Cancel drops it. The browser then selects the new sound, and the zones show it.
    void StartSequenceImport();
    void OpenSequenceImport(const std::string& path);
    void MakeImport();

    // In a file of the GBA, File > Import > Sequence… replaces the commands of a song with a MIDI file, `midi`, named
    // `name` (edit::WithGbaMidiSong). The Import window offers the songs, the shown song first, and pairs each
    // instrument with a voice of the song's voice group. Play plays the song from the new commands. Import makes the
    // edit, and the file's instrument map of the voice group keeps the pairings for the next import.
    void OpenGbaMidiImport(std::vector<uint8_t> midi, const std::string& name,
                           std::optional<uint32_t> first = std::nullopt);

    // Reads the file that the dialog chose for a GBA song or voice group. A song's file opens the MIDI import's window
    // with that song. A file for a voice group replaces the voice group at once.
    void OpenGbaReplacement(const std::string& path);

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

    // File > Import > Bank… on a SoundFont 2 file or an SFZ file.
    void OpenSfzImport(const std::filesystem::path& file);
    void OpenSoundFontImport(std::vector<uint8_t> bytes, const std::string& name,
                             std::vector<std::string> warnings = {});
    void OpenGbaSoundFontImport(std::vector<uint8_t> bytes, const std::string& name, std::vector<std::string> warnings);

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

    // The browser's Replace… of a GBA song or voice group: asks for the file.
    void StartGbaReplace(const BrowserPlace& place);

    // Replace… of an item of a file of the Wii: the dialog for a file of the item's kind.
    void StartWiiReplace(const BrowserPlace& place);

    // On the DS, a sequence sound is replaced with a sequence file, a bank with a bank file and a wave archive with a
    // wave archive file. A wave takes a WAV file or a wave file.
    void StartDsReplace(const BrowserPlace& place);

    // Replace… of a sample of Konami's driver, and of an item of the archive of the 3DS, the Wii U or the Switch. Each
    // follows the checks of the place in StartReplace.
    void StartDriverReplace(const BrowserPlace& place);
    void StartSarReplace(const BrowserPlace& place);

    // Puts the WAV file at `path` in the place of the sample of Konami's driver that `replace_.wave` names
    // (edit::WithKonamiSample).
    void OpenDriverReplacement(const std::string& path);

    // Reads the file that the dialog chose for the DS's item, and opens the Replace window.
    void OpenDsReplacement(const std::string& path);

    // On DSE, a sound is replaced with a song file or a MIDI file, and a bank with a wave bank file. A wave of a bank
    // with samples is replaced with a WAV file (edit/dse_files.h).
    void StartDseReplace(const BrowserPlace& place);

    // Reads the file that the dialog chose for DSE's item, and opens the Replace window.
    void OpenDseReplacement(const std::string& path);

    // The Replace window of `bytes`, a WAV file, in the place of `target` on DSE: wave `wave` of the bank at place
    // `wave_archive` of the set's banks. The new wave uses `encoding`, or else the encoding of the current wave.
    // Returns nothing after a message for a file that can't be read.
    std::optional<Replacement> DseWaveReplacement(std::vector<uint8_t> bytes, const std::string& name,
                                                  const WaveTarget& target,
                                                  std::optional<ntdlib::sar::WaveEncoding> encoding = std::nullopt,
                                                  std::optional<uint32_t> rate = std::nullopt,
                                                  std::optional<edit::WaveLoop> loop = std::nullopt);

    // Opens the Import window of a new item of the DS's archive, from a file's bytes. With `new_song`, the bytes
    // contain an empty song.
    void OpenDsImport(std::vector<uint8_t> bytes, const std::string& name, edit::DsNewItem kind, bool new_song = false,
                      edit::Incoming incoming = {}, std::vector<uint8_t> midi = {});

    // The window of a WAV file or a wave file of the DS as a new wave after the waves of `wave_archive`.
    std::optional<Replacement> DsWaveImport(std::vector<uint8_t> bytes, const std::string& name, uint32_t wave_archive,
                                            std::optional<ntdlib::sar::WaveEncoding> encoding = std::nullopt,
                                            std::optional<uint32_t> rate = std::nullopt);

    // The Replace window of DS stream `stream`, and the Import window of a new one, with the WAV file or the stream
    // file `bytes`. A stream from a WAV file uses `encoding`, `rate` and `loop`.
    std::optional<Replacement> DsStreamReplacement(std::vector<uint8_t> bytes, const std::string& name, uint32_t stream,
                                                   std::optional<ntdlib::sar::WaveEncoding> encoding,
                                                   std::optional<uint32_t> rate = std::nullopt,
                                                   std::optional<edit::WaveLoop> loop = std::nullopt);
    std::optional<Replacement> DsStreamImport(std::vector<uint8_t> bytes, const std::string& name,
                                              std::optional<uint32_t> model,
                                              std::optional<ntdlib::sar::WaveEncoding> encoding,
                                              std::optional<uint32_t> rate, const std::string& new_name);

    // The Replace window of `bytes`, a WAV file or a wave file of the DS, in the place of `target`. A WAV file is
    // encoded with `encoding`, or else with the encoding of the current wave. Returns nothing after a message for a
    // file that can't be read.
    std::optional<Replacement> DsWaveReplacement(std::vector<uint8_t> bytes, const std::string& name,
                                                 const WaveTarget& target,
                                                 std::optional<ntdlib::sar::WaveEncoding> encoding,
                                                 std::optional<uint32_t> rate = std::nullopt,
                                                 std::optional<edit::WaveLoop> loop = std::nullopt);

    // Builds the archive, the ROM or the set of the version that the history has reached, unless the program has it
    // already (BuildArchive). On the DS, an edit of the archive's files follows its FAT, and a read of the version may
    // still be running. A version whose archive is the one that the program has (SameArchive), such as one that only
    // changes a track's colour, builds nothing.
    void FollowArchiveVersion();

    // The replacement of `target` of a file of the Wii by the file at `path`. Shows a message when the file can't be
    // read. The Replace window says why a file that ntdlib reads can't go in.
    void OpenWiiFileReplacement(const std::string& path, const FileTarget& target);

    // The replacement of `target`, the item named `item`, by `bytes`, a file named `name` of console `later`
    // (OpenWiiFileReplacement).
    void OpenWiiLaterFileReplacement(const std::vector<uint8_t>& bytes, const std::string& name,
                                     const std::string& item, const FileTarget& target, ntdlib::sar::Flavor later);

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

    // Whether an archive or a ROM is open, on any console.
    bool ArchiveOpen() const;

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

    // UpdateBankEditor on the Wii and the later consoles.
    void UpdateSarBankEditor();

    // Opens the Bank Editor on the bank that Project > Add Bank… added (`bank_to_open_`) once the program has the
    // archive of the version that the history has reached, on every console.
    void OpenAddedBank();

    // The same for a GBA voice group, by its address in `bank_editor_`. An edit changes a voice's bytes, or a wave's
    // pattern, in place (edit/gba_voices.h). The editor shows the new values at once.
    void UpdateGbaBankEditor();
    void ApplyGbaBankEdit(const BankEdit& bank_edit);

    // ApplyGbaBankEdit for an edit of one voice or one wave's pattern, or a cleared voice.
    void ApplyGbaVoiceEdit(const BankEdit& bank_edit);

    // The same for a DS bank, by its index in `bank_editor_`. An edit changes a region's bytes in place
    // (edit/ds_banks.h). The editor shows the new values at once.
    void UpdateDsBankEditor();
    void ApplyDsBankEdit(const BankEdit& bank_edit);
    void UpdateDseBankEditor();
    void ApplyDseBankEdit(const BankEdit& bank_edit);
    // The samples of a ROM with another of the GBA's drivers. On Konami's driver, an edit changes a sample's rate or
    // loop in the sample table (edit/konami_samples.h).
    void UpdateDriverBankEditor();
    void ApplyDriverBankEdit(const BankEdit& bank_edit);
    // Builds `driver_bank_view_` from the ROM that the program has built, with the songs of the last ROM. A thread
    // reads the songs of the new ROM (SongsOfDriverSamples). TakeDriverSampleSongs shows them once they're ready.
    // DropDriverSampleSongs drops the read. A read that still runs goes on to its end in `dropped_sample_songs_`.
    void FollowDriverBankView();
    void TakeDriverSampleSongs();
    void DropDriverSampleSongs();

    // Builds the Bank Editor's view again when the program has the archive of another version. Returns false, with
    // the editor closed, when the bank can't be edited.
    bool FollowBankArchive();

    // Builds the Bank Editor's view again by `make` when the program has the archive of another version. A failed build
    // shows `refusal` with the reason, where the reason doesn't start with it, and closes the editor. Returns false
    // then.
    bool FollowBankView(const std::function<void()>& make, std::string_view refusal);

    // Drops the files that waited for a field's text on the open bank, once the reader has stopped.
    void TakeWaitingBankDrop();

    // Sounds the key that the Bank Editor's keyboard sounds on the selected program, while `sounds`, when it isn't the
    // last note, and lets the last note go. `make` makes the preview of a note, a program and its key, with the flag
    // that releases it. A preview that can't be made or played shows `refusal` with the reason.
    void SoundBankNote(bool sounds,
                       const std::function<std::unique_ptr<audio::Renderer>(std::pair<uint32_t, int>,
                                                                            std::shared_ptr<std::atomic<bool>>)>& make,
                       std::string_view refusal);

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
    void DropFilesOnWiiBank(const std::vector<std::string>& paths, const BankDrop& drop);
    void DropWaveOnBank(const BrowserPlace& place, const BankDrop& drop);

    // The drop of files on a bank of the archive of the 3DS, the Wii U or the Switch (DropFilesOnBank). The drop edits
    // `document`.
    void DropFilesOnSarBank(const std::vector<std::string>& paths, const BankDrop& drop, const format::File& document);

    // Opens the first of `paths`, the files that the system dropped on the window outside the Bank Editor or that
    // Finder opened with the program, once the user has decided about the document's changes. A text field that's being
    // typed in keeps its text first. A file that is neither a Hokuto file, a sound archive nor a GBA ROM shows a
    // message, and the document stays. So does a drop while a window waits for an answer (WindowWaits).
    void DropFilesOnWindow(const std::vector<std::string>& paths);

    // The same on a DS bank. Each file is a WAV file or a wave file of the DS (SWAV), and its wave goes after the waves
    // of the wave archive in the bank's first slot that names one. A wave of the browser has to be in a wave archive of
    // the bank's slots. DoDsBankDrop makes `file`, the version with the bank of `dropped`, an action in the history.
    void DropFilesOnDsBank(const std::vector<std::string>& paths, const BankDrop& drop);
    void DropWaveOnDsBank(const BrowserPlace& place, const BankDrop& drop);
    void DoDsBankDrop(edit::DroppedDsWaves dropped, format::File file);

    // On the GBA, one file of assembly replaces the voice group. Otherwise each file is a WAV file or a sample's file
    // (edit::ReadIncomingGbaSample). It is written into the ROM's free space as a new sample. One file on a voice sets
    // the sample of the voice, and each file on the list of programs makes a voice at a free program. A new voice can
    // move the voice group's table (edit::WithGbaVoicesAt). DoGbaGroupVersion makes `version`, an edit of the voice
    // group at `from`, an action. Where the table moved, the action's details name the group's new address, and so do
    // the tracks. The move is recorded, and a Bank Editor on the group follows the table, then and through Undo and
    // Redo.
    void DropFilesOnGbaBank(const std::vector<std::string>& paths, const BankDrop& drop);
    void DoGbaGroupVersion(std::string name, std::string details, uint32_t from, edit::GbaGroupVersion version);

    // Steps of DropFilesOnGbaBank. DropGbaAssembly puts a dropped file of assembly in the voice group's place and
    // returns true, or returns false for files of another kind. ReadDroppedGbaSamples reads the samples of the files
    // at `paths`, or shows why one can't be read and returns nothing.
    bool DropGbaAssembly(const std::vector<std::string>& paths);
    std::optional<std::vector<std::vector<uint8_t>>> ReadDroppedGbaSamples(const std::vector<std::string>& paths);

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
    format::File FileWithWaveValues(const format::File& document, const WaveValues& values);
    void PlayProperties();
    std::optional<BrowserPlace> PropertiesPlace() const;

    // Makes the view of the properties again for the archive of the current version. Returns false, with the window
    // closed, when the version has no such wave or stream, or the view fails.
    bool RebuildWaveView(bool wii, bool wii_stream, bool ds, bool ds_stream, bool dse);

    // RebuildWaveView on the DS and DSE, and on the Wii and the later consoles. Each throws when the view fails.
    bool RebuildDsWaveView(bool ds_stream, bool dse);
    bool RebuildSarWaveView(bool wii, bool wii_stream);

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

    // Moves in the history by `move`, a call of Undo, Redo or GoTo, and follows the move (FollowHistory).
    void MoveInHistory(const std::function<void(edit::History&)>& move);

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

    // The sound of the sequence that the zones show next: the sound of the browsed sequence, or else that of the shown
    // sequence.
    uint32_t ShownSequenceSound(bool browsed) const;

    // The functions that ReadShown calls to read `sound`, one for each console: the GBA, the other drivers of the GBA,
    // the DS, DSE, the Wii and the later consoles.
    void ReadShownGba(const std::shared_ptr<const format::File>& version, uint32_t sound);
    void ReadShownDriver(const std::shared_ptr<const format::File>& version, uint32_t sound);
    void ReadShownDs(const std::shared_ptr<const format::File>& version, uint32_t sound);
    void ReadShownDse(const std::shared_ptr<const format::File>& version, uint32_t sound);
    void ReadShownWii(const std::shared_ptr<const format::File>& version, uint32_t sound);
    void ReadShownSar(const std::shared_ptr<const format::File>& version, uint32_t sound);

    // Builds the archive of `version` on the program's thread, and lists the browser's programs and waves again from
    // it. An error shows in a message box.
    void BuildArchive(const std::shared_ptr<const format::File>& version);

    // Lists the programs of each bank and the waves of each wave archive in the browser again, from the open archive.
    void ListChildren();

    // Lists the open archive's sounds, sequences, banks and wave archives in the browser, with each item's children. A
    // file of the Wii lists its items through ListWiiBrowser, a file of the GBA its songs and voice groups through
    // ListGbaBrowser, a file of the DS its sequences, streams, banks and wave archives through ListDsBrowser, and a
    // file of DSE's files its songs, sound effects, banks and waves through ListDseBrowser.
    void ListBrowser();

    // The browser's items of an archive of the 3DS, the Wii U or the Switch (ListBrowser): its sounds, its banks, its
    // wave archives and its wave sound files, and the items whose files are in group files missing from the file.
    void ListSarBrowser();
    void ListSarSounds(const format::File& version);
    void MarkMissingGroupItems();

    // Every item exports but a stream sound whose stream the file lacks, and Export… says why. A wave archive's waves
    // export too, and so do a wave sound file's wave sounds.
    void AddSarBrowserItem(std::size_t group, const std::string& item_name, std::size_t index,
                           std::vector<uint32_t> children, bool exports);
    void ListWiiBrowser();
    void ListGbaBrowser();
    void ListDsBrowser();
    void ListDseBrowser();

    // The same for a ROM with another of the GBA's drivers: its songs and its samples.
    void ListDriverBrowser();

    // The voice groups that the songs of `played_.Gba()` play (song::GbaVoiceGroups), worked out once for each ROM. The
    // browser lists them, and the inspector's Program menu offers each group's programs.
    const std::vector<song::GbaVoiceGroup>& GbaGroups();

    // The one bank slot of GBA song `song`: its voice group, with the programs that the ROM's songs select there.
    std::vector<SlotView> GbaSlotsOf(uint32_t song);

    // The one bank slot of DS sound `sound`: the bank that its entry names, with the programs that contain instruments.
    std::vector<SlotView> DsSlotsOf(uint32_t sound) const;

    // The bank slots of DSE's sound `sound`: its banks, each with the programs that contain instruments.
    std::vector<SlotView> DseSlotsOf(uint32_t sound) const;

    // The slot that the inspector's instrument menus show for a song of another of the GBA's drivers: the driver's one
    // table, with the samples that the songs play. The samples and the PSG tracks' programs stay for each ROM.
    // ShowDriverPrograms sets the programs of the PSG tracks of the song.
    const std::vector<int>& DriverSampleList();
    std::vector<SlotView> DriverSlots();
    void ShowDriverPrograms();

    // Follows the items of the open archive once the archive of another version has come in, on every console. The
    // browser lists the items again: a version can change what an item offers, such as a stream that Replace… or Undo
    // brings. When the items differ from the browser's, such as after an import or its undo, the places of the files'
    // sequences and of the sounds' starts follow the archive first. The browser keeps its choices of the items that
    // stay. A sound that an import added is selected.
    void FollowItems();

    // Steps of FollowItems: the places of the sequences and of the starts in the archive's version, and the browser's
    // choices of items that are no longer listed.
    void ReadFileSequences();
    void DropUnlistedChoices();

    // Stops playback and empties the zones. A file that has just opened shows them the same way.
    void CloseSequence();

    // Takes a read of the sequence that the zones show, from the archive of a later version. The zones show the new
    // version, and keep their selection and each track's mute and solo. A new model catches up with playback, and
    // playback goes on without a stop.
    void FollowSequence(song::SequenceReader::Result read);

    // The slots of the shown sequence, from the archive, the ROM or the set that the program has open.
    std::vector<SlotView> ShownSlots();

    // Sets the mute, the solo and the meters of each track of the shown song to those of the track with its number in
    // `kept`.
    void KeepTrackStates(const std::map<int, TrackView>& kept);

    void PlayPreview(const BrowserPlace& place);
    void PlayWiiPreview(const BrowserPlace& place);
    void PlayGbaPreview(const BrowserPlace& place);
    void PlayDsPreview(const BrowserPlace& place);
    void PlayDsePreview(const BrowserPlace& place);
    void PlayDriverPreview(const BrowserPlace& place);
    void PlaySarPreview(const BrowserPlace& place);

    // The item that the browser lists at `place`, or nothing when the place names no listed item.
    std::optional<ListedPlace> ListedAt(const BrowserPlace& place) const;

    // The mixing mode of the current version, or strict mixing without a file.
    format::Mixing FileMixing() const;

    // Plays the preview that `make` makes by itself, as the item at `place` (PlayAlone). An error of `make` shows in a
    // message box, after `what`.
    void PlayBrowserPreview(const BrowserPlace& place, const char* what,
                            const std::function<std::unique_ptr<audio::Renderer>()>& make);

    // Gives each track of the zones' song what else an edit of its mix or its instrument changes, for the tooltips of
    // the mixer and the inspector (edit::ReachOfTracks).
    void ShowReach();

    // Whether the zones can show the reach of an edit: the open file is one whose sounds Hokuto can edit. ShowReach and
    // ShowTempo test it.
    bool ReachShown() const;

    // Gives the zones' song the places where its tracks read variables or branch, for the marker row (LogicMarkers).
    void ShowLogic();

    // Gives each track of the zones' song without notes the instrument that its opening selects, such as a new track's
    // (edit::OpeningInstrument). A note that goes in at the track's start plays it.
    void ShowOpeningInstruments();

    // What a first note of `track`, a track of the shown sequence, requires: the instrument that its opening has to
    // select for the note to sound, or why no instrument can.
    struct FirstNote
    {
        std::optional<edit::Program> instrument;
        std::string refused;
    };
    FirstNote FirstNoteOf(const song::Track& track) const;

    // Whether the channel of `track`, a DSE track of the shown sequence, has a bank: a track of the channel plays
    // notes, or its opening selects a bank (edit::DseOpeningSelectsBank). A track whose commands the edits can't follow
    // counts as one with a bank.
    bool DseChannelHasBank(const song::Track& track) const;

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

    // The number of sends, from FX A, that the strips show: the sends that the tracks of both the file's console and
    // the shown engine have. A file in the model form shows every send.
    std::size_t ShownSends() const;

    // The engine whose model plays the shown sequence, and the archive and the sequence that it plays: the conversion's
    // while the shown sound plays on another console's engine, and otherwise the file's. A sound of the Wii, or a sound
    // on the Wii's engine, plays from the Wii's archive. PlayedArchive then gives nothing.
    format::Engine PlayedEngine() const;
    std::shared_ptr<const ntdlib::sar::SoundArchive> PlayedArchive() const;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> PlayedWiiArchive() const;
    std::shared_ptr<const song::DsArchive> PlayedDsArchive() const;
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

    // Drops the Key Editor's selected notes, its gesture and the selection that waits for a version.
    void ClearKeySelection();

    // Makes what the Key Editor shows of its track from the sequence that the zones show, with what an edit can change
    // in each of the track's notes, and the commands of the track's settings for the controller lanes. A track that the
    // sequence no longer has shows no notes. The selections follow the version of the sequence (FollowedSelection).
    void UpdateKeyTrack();

    // Fills the Key Editor's notes from `track`, for UpdateKeyTrack, and adds the place of each note's command that
    // isn't in `commands` yet to `places`.
    void FillKeyNotes(const song::Track* track, std::vector<edit::NotePlace>& places, std::set<uint32_t>& commands);

    // Fills the Key Editor's notes that the driver never started on `track`, for UpdateKeyTrack.
    void FillUnstartedKeyNotes(const song::Track* track);

    // Fills the Key Editor's lane of channels with every track's notes on the PSG channels, for UpdateKeyTrack. The
    // notes of `track` show as the notes of the shown track.
    void FillPsgChannelNotes(const song::Track* track);

    // Fills the controller lanes of `track` in `sequence` and the limits of each command in `places`, for
    // UpdateKeyTrack. The controls' commands join `places` and `commands`.
    void FillKeyControlsAndLimits(const std::optional<uint32_t>& sequence, const song::Track* track,
                                  std::vector<edit::NotePlace>& places, std::set<uint32_t>& commands);

    // The Key Editor's selection after `edited`, an edit of sequence `sequence` of `file`: the notes of the commands at
    // `kept` in the new version, then the new notes. A selection of another version's commands keeps nothing.
    KeySelection SelectionAfter(const format::File& file, uint32_t sequence, const edit::Edited& edited,
                                const std::set<uint32_t>& kept) const;

    // The List Editor's selection after `edited`, an edit of sequence `sequence` of `file`: the selected commands in
    // the new version. A command that the edit removed leaves the selection. A selection of another version's commands
    // keeps nothing.
    KeySelection ListSelectionAfter(const format::File& file, uint32_t sequence, const edit::Edited& edited) const;

    // The index of the shown sequence among the sequences of the current version, or nothing. An edit of the shown
    // sequence is applied to the version that built the open archive: the zones' notes and commands lead to its
    // commands. With any other version, the result is nothing. Document() isn't null when the result has a value.
    std::optional<uint32_t> EditedSequence() const;

    // Makes `edited`, an edit of sequence `sequence` of the current version, the next version (Do). The history lists
    // the action by `name` and `details`. The Key Editor keeps the notes of the commands at `kept` (SelectionAfter),
    // and the List Editor keeps its selected commands (ListSelectionAfter).
    void DoSequenceEdit(std::string name, std::string details, uint32_t sequence, edit::Edited edited,
                        const std::set<uint32_t>& kept);

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

    // Drops the List Editor's selected rows and label, its anchor, its typing and the selection that waits for a
    // version.
    void ClearListSelection();

    // Shows track `track` in the List Editor, from its first row, with nothing selected and no subroutine shown.
    void ShowListTrack(int track);

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

    // The version that `edit` makes of the commands of `sequence` in `document`, from the place of the first row.
    edit::Edited ListEditOf(const ListEdit& edit, const format::File& document, uint32_t sequence,
                            const edit::CommandPlace& place, std::size_t first) const;

    // Selects in the List Editor what `edit` changed, moved or added in `edited`, whose commands are `commands` at
    // `offsets`.
    void SelectListAfter(const ListEdit& edit, const edit::Edited& edited, const format::File& document,
                         uint32_t sequence, const edit::CommandPlace& place, const ntdlib::commands::Sequence& commands,
                         const std::vector<uint32_t>& offsets);

    // Keeps the List Editor on the subroutine that it shows after `edit`.
    void FollowListSubroutine(const ListEdit& edit, const edit::Edited& edited, const format::File& document,
                              uint32_t sequence, const ntdlib::commands::Sequence& commands,
                              const std::vector<uint32_t>& offsets);

    // What the Undo History names after the track for `edit`: the command, or the label or subroutine.
    std::string ListEditName(const ListEdit& edit, std::size_t first) const;

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

    // Starts the audition of `note` for PlayFeedback on the shown sound's console, with `release` to let it go.
    // Returns false when the note can't play.
    bool PlayFeedbackPreview(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release);

    // Starts the audition of `note` on the DS's engine, for PlayFeedbackPreview. Returns false when the note can't
    // play.
    bool PlayEngineDsFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release);

    // Starts the audition of `note` on the GBA's other drivers, for PlayFeedbackPreview. Returns false when the note
    // can't play.
    bool PlayDriverFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release);

    // Starts the audition of `note` on the Wii's engine, for PlayFeedbackPreview.
    void PlayEngineWiiFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release);

    // The same as PlayEngineDsFeedback on the GBA, the DS and DSE.
    bool PlayGbaFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release);
    bool PlayDsFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release);
    bool PlayDseFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release);

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

    // The stretches of the track before `track` in the shown sequence on Konami's driver, where a note of that track
    // can play on `track`. Other drivers give none.
    std::span<const song::Stretch> PreviousTrackStretches(const song::Track& track) const;

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

    // Shows `sound` of the current version in `section`. The values of a DS sound are stored in the archive's bytes.
    // While the reader's thread builds the version's archive, the section keeps its view.
    void ShowSoundIn(SoundSection& section, std::optional<uint32_t> sound);

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

    // The version that the history has reached, and `shown`, the bank, the wave or the sound that an editor shows in
    // it. A value that the user types into the editor while the program is busy waits for its item there (ShownItem).
    ShownItem ItemShown(std::optional<uint64_t> shown) const;

    // The index among the file's sequences of the sequence that the zones show, or nothing.
    std::optional<uint32_t> ShownSequence() const;

    // Sequence `sequence` in the version that built the open archive, or nothing.
    format::Shared<ntdlib::commands::Sequence> VersionSequence(std::optional<uint32_t> sequence) const;

    // The labels that name the parts of the shown song, by their offsets (app::PartLabels): on the GBA, the DS and with
    // Konami's driver, those of the version that the zones show.
    std::map<uint32_t, std::string> PartLabels() const;

    // The index among the file's sequences of the sequence that a sound of the open archive plays, or nothing.
    std::optional<uint32_t> SequenceOfSound(uint32_t sound) const;

    // Where each sound of the open archive plays in the version that built it (edit::SoundPlaces), on the Wii too.
    // Empty there without the fields of the sounds' starts. On the GBA, where each song of the ROM plays
    // (edit::GbaSoundPlaces), and with the GBA's other drivers through edit::DriverSoundPlaces. On the DS and DSE,
    // where each sound's sequence starts (edit::DsSoundPlaces).
    std::vector<std::optional<edit::SoundPlace>> Places() const;

    // The names of the sounds of the open archive, by their places in the sound table, or of a GBA ROM's songs, by
    // their numbers.
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

    // Adds a track to `sound` with the instrument of the first note of the `selected` track, for EditTracks.
    void AddSelectedTrack(const format::File& document, const std::vector<std::optional<edit::SoundPlace>>& places,
                          uint32_t sound, const song::Track* selected);

    // The name of the sound that the copied track came from, for the details of Paste Track.
    std::string CopiedTrackSoundName(const format::File& document) const;

    // Playback follows the transport: it starts and stops with it, and starts again from where the position moves to.
    // The position follows the sample that the device plays. Without a sequence, the transport runs on the frames'
    // clock.
    void UpdatePlayback();
    void StartPlayback(std::optional<uint32_t> start = std::nullopt);
    void StopPlayback();

    // Where playback starts again on a model of another engine that has caught up: the model's tick near the position
    // that the device plays, or else that position.
    uint32_t ChangeOverTick() const;

    // The transport's position in the ticks of the sequence that the model plays (song::PlayedTickOf).
    double PositionInModel() const;

    // How many ticks the model that a catch-up makes counts for each tick of the model that plays
    // (song::CountedTicksOn), at the clock's count. A model of `other_engine` plays a conversion, or the sound itself
    // in place of one. On the same engine, only a conversion's catch-up maps the ticks. The ticks of a conversion of a
    // sound of Konami's driver follow the beat of its MIDI file, and an edit can move that beat.
    double CatchUpScale(bool other_engine) const;

    // The stereo frames of the playback's output that the device has played, and those that it hasn't played yet: the
    // frames in the ring and in the device's buffer, at the player's rate.
    uint64_t HeardSamples() const;
    uint64_t QueuedSamples() const;

    // The transport's cycle in the sequence's ticks, for the playback clock.
    song::PlaybackClock::CycleRange CycleInTicks() const;

    // A mask of the numbers of the tracks of the model that plays that play track `number` of the shown sound.
    uint32_t PlayedTracks(int number) const;

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

    // The open sound archive, ROM or set. It came from `archive_version_`, a version of the file. While the reader
    // builds the archive of another version, that version is `reading_version_`.
    song::Played played_;
    std::shared_ptr<const format::File> archive_version_;
    std::shared_ptr<const format::File> reading_version_;

    // The samples of the ROM of another of the GBA's drivers, the programs of its PSG tracks, and the ROM that they
    // belong to (DriverSampleList).
    std::vector<int> driver_samples_;
    std::map<int, std::vector<uint32_t>> driver_psg_programs_; // by the PSG tracks' numbers
    std::weak_ptr<const song::DriverRom> driver_samples_rom_;

    // GbaGroups's voice groups, and the ROM that they belong to.
    std::vector<song::GbaVoiceGroup> gba_groups_;
    std::weak_ptr<const song::GbaRom> gba_groups_rom_;

    // The sequence of the file that each of the archive's files contains, by the file's ID, or that each song of a GBA
    // ROM is, by its number. Then the field of the native layout that contains each sound's start, by the sound's
    // index. Edits leave both as they are. The fields are missing when the archive's tables can't be read, on the GBA,
    // the DS and DSE, and with the GBA's other drivers.
    std::vector<std::optional<uint32_t>> file_sequences_;
    std::optional<std::vector<std::optional<uint32_t>>> start_fields_;

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

    // The tracks of a conversion of a song of the GBA or of Konami's driver, a sequence of the DS or a sound of DSE
    // that play each of the sound's tracks (song::TracksOfConversion). Elsewhere, a conversion's tracks keep the
    // sound's numbers.
    std::optional<std::array<uint32_t, 32>> engine_tracks_;
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
    // holds the bank that Project > Add Bank… added, until the program has built the archive that holds it. On the
    // GBA, `bank_editor_` contains the voice group's address, and `gba_bank_view_` replaces `bank_view_`. On the
    // DS, `ds_bank_view_` does, and on DSE `dse_bank_view_`, with `bank_editor_` holding an index in the set's banks.
    // With the GBA's other drivers, `driver_bank_view_` shows the samples that the ROM's songs play.
    // `driver_sample_songs_` contains the songs that the view shows. A thread reads the songs of a new ROM into
    // `reading_sample_songs_`.
    std::optional<uint32_t> bank_editor_;
    std::optional<BankView> bank_view_;
    std::optional<GbaBankView> gba_bank_view_;
    std::optional<DsBankView> ds_bank_view_;
    std::optional<DseBankView> dse_bank_view_;
    std::optional<DriverBankView> driver_bank_view_;
    std::shared_ptr<const format::File> bank_view_version_;
    DriverSampleSongs driver_sample_songs_;
    std::future<DriverSampleSongs> reading_sample_songs_;
    std::vector<std::future<DriverSampleSongs>> dropped_sample_songs_;
    BankEditorState bank_state_;
    std::optional<std::pair<uint32_t, int>> bank_note_;
    std::shared_ptr<std::atomic<bool>> bank_release_;
    std::vector<std::string> bank_drops_;
    std::optional<BankDrop> bank_drop_;
    std::optional<WaitingBankDrop> waiting_bank_drop_;
    std::future<std::vector<std::string>> bank_group_sizes_;
    std::optional<uint32_t> bank_to_open_;

    // Every move of a voice group's table in the file's history, by both of its addresses: the Bank Editor follows a
    // group through Undo and Redo (FollowedGbaGroup).
    GbaGroupMoves gba_moved_groups_;

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

    // A file from a dialog for an Import or Replace window, and the frames that DrawPreparing has drawn for it. The
    // preparation starts once that window shows.
    std::optional<std::pair<Dialog, std::string>> preparing_;
    int preparing_frames_ = 0;

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

    // The state of Replace… and of the imports, from the file dialog to the work of the window's Play.
    ReplaceFlow replace_;

    // The transport's presses of Stop while stopped that the program has acted on (Transport::IdleStops).
    uint64_t idle_stops_ = 0;

    // The sound that an import added, until the browser lists it and selects it.
    std::optional<uint32_t> select_sound_;

    // The view that the open file keeps, until its sound shows.
    std::optional<format::EditorView> pending_view_;

    // The sounds that have shown while the file is open. The message of a sound's banks in group files that the file
    // lacks shows once (song::MissingBanksNote).
    std::set<uint32_t> told_missing_banks_;

    // The songs of the GBA's other drivers that have shown while the file is open. The message that a song can't be
    // edited because its commands can't move shows once for each.
    std::set<uint32_t> told_unrelocatable_;

    // Whether SDL3 waits for an event before the next frame, whether the last frame drew for work that moves (see
    // Pace), when the last event came, in SDL3's nanoseconds, and the type of the event that Wake sends.
    bool waiting_ = false;
    bool moving_ = false;
    uint64_t last_event_ = 0;
    uint32_t wake_event_ = 0;

    // The messages that wait for a frame with no mouse button down (ShowMessage).
    MessageQueue messages_;

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
