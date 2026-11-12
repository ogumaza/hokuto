// SPDX-License-Identifier: MIT

// The menu commands: what each command does, whether it's checked or enabled, and the title that it shows.

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <utility>

#include "app/app.h"
#include "app/app_internal.h"
#include "app/recent_projects.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/items.h"

namespace hokuto::app
{
namespace
{

// Whether the Wii's `archive` holds a file whose wave data can take a new wave: a bank's or a wave sound's.
bool HoldsWaves(const ntdlib::rsar::SoundArchive& archive)
{
    return !archive.Banks().empty() ||
           std::ranges::any_of(archive.Sounds(), [](const ntdlib::rsar::SoundInfo& sound)
                               { return sound.type == ntdlib::rsar::SoundType::kWaveSound; });
}

// The starts and ends of the selected track's parts, in quarter notes.
std::vector<double> PartBounds(const std::optional<SongView>& song, const ArrangementState& state)
{
    std::vector<double> bounds;
    if (!song || state.selected_track < 0 || state.selected_track >= static_cast<int>(song->tracks.size()))
    {
        return bounds;
    }

    const double quarter = song->ticks_per_quarter;
    for (const PartView& part : song->tracks[static_cast<std::size_t>(state.selected_track)].parts)
    {
        bounds.push_back(part.start / quarter);
        bounds.push_back((part.start + part.length) / quarter);
    }

    return bounds;
}

} // namespace

void App::Execute(Command command)
{
    // A command from the Mac's menu bar comes outside SDL3's events. The wake ends any wait for an event. The next
    // frame then shows the command's result, or ends the program after Quit.
    Wake();

    // A command whose menu item is grey does nothing, whatever gave it. A text field that's being typed in keeps its
    // text before the command runs.
    if (!IsEnabled(command) || WaitForTyping([this, command] { Execute(command); }))
    {
        return;
    }

    // Open, Import and Quit close the document. The user decides about its changes first.
    switch (command)
    {
    // The window opens with the Switch chosen each time. Cancel drops the choice.
    case Command::kNewProject:
        new_project_open_ = true;
        new_project_engine_ = format::Engine::kSwitch;
        break;

    case Command::kOpen:
        AfterChanges([this] { ShowDialog(Dialog::kOpen); });
        break;

    case Command::kSave:
        Save();
        break;

    case Command::kSaveAs:
        ShowDialog(Dialog::kSaveAs);
        break;

    case Command::kImportSoundArchive:
        AfterChanges([this] { ShowDialog(Dialog::kImport); });
        break;

    case Command::kImportSequence:
        StartSequenceImport();
        break;

    case Command::kImportBank:
        ShowDialog(Dialog::kImportBank);
        break;

    case Command::kImportWaveArchive:
        ShowDialog(Dialog::kImportWaveArchive);
        break;

    case Command::kImportWave:
        ShowDialog(Dialog::kImportWave);
        break;

    // The new wave sounds copy the settings of a wave sound or a sequence sound of the archive. Without one, the import
    // says so before the dialog.
    case Command::kImportWaveSoundFile:
        if (played_.Wii()
                ? HoldsSoundOf(*played_.Wii(),
                               {ntdlib::rsar::SoundType::kWaveSound, ntdlib::rsar::SoundType::kSequence})
                : HoldsSoundOf(*played_.Sar(), {ntdlib::sar::SoundType::kWaveSound, ntdlib::sar::SoundType::kSequence}))
        {
            ShowDialog(Dialog::kImportWaveSoundFile);
        }
        else
        {
            ShowMessage(
                "Can't import a wave sound file here. New sounds copy the settings of an existing wave sound or "
                "sequence sound, and this archive has neither.");
        }
        break;

    case Command::kImportAudioFile:
        ShowDialog(Dialog::kImportAudioFile);
        break;

    case Command::kImportStream:
        ShowDialog(Dialog::kImportStream);
        break;

    case Command::kExportSoundArchive:
        StartExport(ExportTarget::kSoundArchive);
        break;

    case Command::kExportMidiSoundFont:
        StartExport(ExportTarget::kMidiSoundFont);
        break;

    case Command::kExportMidiSfz:
        StartExport(ExportTarget::kMidiSfz);
        break;

    case Command::kExportOtherConsole:
        // A sound of the Wii, a song of the GBA, a sequence of the DS and a sound of DSE go to the Wii U first.
        if (const format::File* document = Document(); document != nullptr && document->native)
        {
            const auto& native = *document->native;
            StartExport(
                SongTargetFor(format::EngineOf(format::FactsOf(native.engine).family != format::Family::kNintendoWare
                                                   ? ntdlib::sar::Flavor::kCafe
                                                   : song::OtherConsole(FlavorOf(native)))));
        }
        break;

    // The submenu's items open the files (ExecuteListed).
    case Command::kRecentProjects:
        break;

    case Command::kQuit:
        AfterChanges([this] { quit_ = true; });
        break;

    case Command::kUndo:
        if (history_)
        {
            MoveInHistory([](edit::History& history) { history.Undo(); });
        }
        break;

    case Command::kRedo:
        if (history_)
        {
            MoveInHistory([](edit::History& history) { history.Redo(); });
        }
        break;

    case Command::kUndoHistory:
        views_.undo_history = !views_.undo_history;
        break;

    // While another window holds the lower zone in front of an open Key Editor, the command brings the editor to the
    // front.
    case Command::kOpenCloseEditor:
        if (key_editor_open_ && FrontOfLowerZone() != LowerZone::kKeyEditor)
        {
            lower_front_ = LowerZone::kKeyEditor;
            ImGui::SetWindowFocus(kKeyEditor);
        }
        else if (key_editor_open_)
        {
            CloseKeyEditor();
        }
        else
        {
            OpenKeyEditor();
        }
        break;

    // Open List Editor shows the selected part's track, or else the selected track, in front of the lower zone. An
    // editor that shows that track already comes to the front as it is, unless a part is selected: the editor then goes
    // to the part. The command never closes the editor.
    case Command::kOpenListEditor:
        if (list_editor_open_ && sequence_ && ListEditorChoice().first == list_editor_track_ &&
            !ListEditorChoice().second)
        {
            lower_front_ = LowerZone::kListEditor;
            ImGui::SetWindowFocus(kListEditor);
        }
        else
        {
            OpenListEditor();
        }
        break;

    // Quantize, Cut, Copy and Paste wait for the Key Editor's next frame.
    case Command::kQuantize:
        key_editor_.request = KeyRequest::kQuantize;
        break;

    case Command::kCut:
        key_editor_.request = KeyRequest::kCut;
        break;

    case Command::kCopy:
        key_editor_.request = KeyRequest::kCopy;
        break;

    case Command::kPaste:
        key_editor_.request = KeyRequest::kPaste;
        break;

    // Select All selects every note of the Key Editor, or every command of the List Editor: the editor that has the
    // focus.
    case Command::kSelectAll:
        if (key_editor_open_ && key_editor_.focused)
        {
            key_editor_.request = KeyRequest::kSelectAll;
        }
        else if (list_editor_open_ && list_editor_.focused)
        {
            list_editor_.select_all = true;
        }
        break;

    // While the Key Editor has the focus, the zoom commands zoom the editor.
    case Command::kZoomIn:
        ++(key_editor_.focused ? key_editor_.zoom_steps : arrangement_.zoom_steps);
        break;

    case Command::kZoomOut:
        --(key_editor_.focused ? key_editor_.zoom_steps : arrangement_.zoom_steps);
        break;

    case Command::kZoomFull:
        (key_editor_.focused ? key_editor_.zoom_full : arrangement_.zoom_full) = true;
        break;

    case Command::kZoomInVertically:
        ++(key_editor_.focused ? key_editor_.vertical_zoom_steps : arrangement_.vertical_zoom_steps);
        break;

    case Command::kZoomOutVertically:
        --(key_editor_.focused ? key_editor_.vertical_zoom_steps : arrangement_.vertical_zoom_steps);
        break;

    case Command::kAddSequenceSound:
        StartSoundAddition();
        break;

    case Command::kAddBank:
        StartBankAddition();
        break;

    case Command::kAddTrack:
        EditTracks(TrackAction::kAdd);
        break;

    case Command::kDuplicateTracks:
        EditTracks(TrackAction::kDuplicate);
        break;

    case Command::kRemoveSelectedTracks:
        EditTracks(TrackAction::kRemove);
        break;

    case Command::kCopyTrack:
        EditTracks(TrackAction::kCopy);
        break;

    case Command::kPasteTrack:
        EditTracks(TrackAction::kPaste);
        break;

    case Command::kTempoTrack:
        arrangement_.tempo_row = !arrangement_.tempo_row;
        break;

    // With a sequence shown, a start ends the Replace or Import window's Play: playback takes the preview's place.
    // Without one, a start ends the window's Play alone, and the empty song stays stopped. The browser's previews play
    // on under the empty song.
    case Command::kStartStop:
        if (!sequence_ && replace_.previewing && !transport_.Playing())
        {
            StopPreview();
            break;
        }

        transport_.StartStop();
        break;

    case Command::kStart:
        if (!sequence_ && replace_.previewing && !transport_.Playing())
        {
            StopPreview();
            break;
        }

        transport_.Start();
        break;

    case Command::kStop:
        transport_.Stop();
        break;

    case Command::kCycle:
        transport_.SetCycle(!transport_.Cycle());
        break;

    // The locators go round the selected part. Cycle stays as it is.
    case Command::kLocatorsToSelection:
        if (const PartView* part = SelectedPart(song_, arrangement_))
        {
            const double quarter = song_->ticks_per_quarter;
            transport_.SetLocators(part->start / quarter, (part->start + part->length) / quarter);
        }
        break;

    case Command::kGoToStart:
        transport_.GoToStart();
        break;

    case Command::kGoToLeftLocator:
        transport_.GoToLeftLocator();
        break;

    case Command::kGoToRightLocator:
        transport_.GoToRightLocator();
        break;

    case Command::kLocateNextEvent:
        transport_.Locate(PartBounds(song_, arrangement_), 1);
        break;

    case Command::kLocatePreviousEvent:
        transport_.Locate(PartBounds(song_, arrangement_), -1);
        break;

    case Command::kRewind:
        transport_.Step(-1);
        break;

    case Command::kForward:
        transport_.Step(1);
        break;

    // While the Key Editor or the List Editor holds the lower zone, the mixer's command brings the mixer to the front
    // instead of closing it. When the mixer closes, the editor that it came in front of comes back.
    case Command::kMixer:
        {
            const LowerZone front = FrontOfLowerZone();
            if (!views_.mixer || front != LowerZone::kMixer)
            {
                views_.mixer = true;
                behind_mixer_ = front;
                lower_front_ = LowerZone::kMixer;
                ImGui::SetWindowFocus(kMixer);
            }
            else
            {
                views_.mixer = false;
                lower_front_ = behind_mixer_;
            }
            break;
        }

    case Command::kStudioSetup:
        studio_setup_open_ = true;
        break;

    case Command::kProjectSetup:
        project_setup_open_ = true;
        break;

    case Command::kPreferences:
        preferences_open_ = true;
        break;

    case Command::kInspector:
        views_.inspector = !views_.inspector;
        break;

    case Command::kBrowser:
        views_.browser = !views_.browser;
        break;

    case Command::kVariables:
        views_.variables = !views_.variables;
        break;

    // The zones, the zoom and the tracks' height go back to how a first run shows them.
    case Command::kResetLayout:
        views_ = OpenViews();
        arrangement_.beat_width = kDefaultBeatWidth;
        arrangement_.track_height = kDefaultTrackHeight;
        arrangement_.track_list_width = kDefaultTrackListWidth;
        reset_layout_ = true;
        break;
    }
}

bool App::IsChecked(Command command) const
{
    switch (command)
    {
    case Command::kUndoHistory:
        return views_.undo_history;
    case Command::kCycle:
        return transport_.Cycle();
    case Command::kMixer:
        return views_.mixer;
    case Command::kInspector:
        return views_.inspector;
    case Command::kBrowser:
        return views_.browser;
    case Command::kVariables:
        return views_.variables;
    case Command::kTempoTrack:
        return arrangement_.tempo_row;
    default:
        return false;
    }
}

bool App::IsEnabled(Command command) const
{
    // While a file opens, every command but Quit waits for it.
    if (opening_ && command != Command::kQuit)
    {
        return false;
    }

    // Save and Save As wait for a file to save. The made-up song isn't one. Project Setup waits for one too. An export
    // waits for a file with a sound archive, and MIDI for a sequence in the zones. One export goes at a time. Undo and
    // Redo wait for an action to undo or redo. A text field that's being typed in keeps their keys for its text. The
    // Replace window makes its edit from the version that it opened on, and Undo and Redo wait while it's open. An
    // import of a sequence, a bank, a wave archive or a wave sound file waits for a file with the archive's tables, for
    // the Export and Replace windows to close, and for the archive of the last edit. So do the Project menu's
    // additions. Locators to Selection waits for a selected part. The Key Editor opens on a part or a track of a
    // sequence: the made-up song's have no commands to edit. Quantize waits for the Key Editor to have the focus and to
    // take edits. So do Cut, Copy and Paste, with notes to take or to paste. Select All waits for the Key Editor or the
    // List Editor to have the focus. Duplicate Tracks and Remove Selected Tracks wait for a selected track. Track 0
    // starts the others, and stays. While a window waits for an answer, the commands that open another such window or
    // change the file wait for it to close (WindowWaits).
    if (WindowWaits())
    {
        switch (command)
        {
        case Command::kNewProject:
        case Command::kOpen:
        case Command::kSave:
        case Command::kSaveAs:
        case Command::kImportSoundArchive:
        case Command::kImportSequence:
        case Command::kImportBank:
        case Command::kImportWaveArchive:
        case Command::kImportWave:
        case Command::kImportWaveSoundFile:
        case Command::kImportAudioFile:
        case Command::kImportStream:
        case Command::kExportSoundArchive:
        case Command::kExportMidiSoundFont:
        case Command::kExportMidiSfz:
        case Command::kExportOtherConsole:
        case Command::kRecentProjects:
        case Command::kUndo:
        case Command::kRedo:
        case Command::kCut:
        case Command::kPaste:
        case Command::kQuantize:
        case Command::kAddSequenceSound:
        case Command::kAddBank:
        case Command::kProjectSetup:
            return false;
        default:
            break;
        }
    }
    const format::File* document = Document();
    const bool exports = history_ && !preview_ && document != nullptr && document->native && !export_;
    const bool typing = ImGui::GetIO().WantTextInput;

    // An import requires a file with its native archive, and no preview, replacement or export under way. Some imports
    // also wait until no version is being read (`settled`).
    const bool importable =
        history_ && !preview_ && document != nullptr && document->native && !replace_.shown && !export_;
    const bool settled = reading_version_ == nullptr;
    switch (command)
    {
    case Command::kSave:
    case Command::kSaveAs:
    case Command::kProjectSetup:
        return history_ && !preview_;
    case Command::kExportSoundArchive:
        return exports;
    case Command::kImportSequence:
        // In a file of the GBA, a MIDI file replaces the commands of a song.
        if (played_.Gba())
        {
            return importable && archive_version_ && settled;
        }
        if (played_.Ds())
        {
            return importable && settled;
        }
        return importable && (played_.Sar() || played_.Wii()) && document->native->tables && settled;
    case Command::kImportBank:
        // In a file of the GBA, the presets of a SoundFont become voices of the voice group of a song.
        if (played_.Gba())
        {
            return importable && archive_version_ && settled;
        }
        [[fallthrough]];
    case Command::kImportWaveArchive:
    case Command::kAddSequenceSound:
    case Command::kAddBank:
        if (played_.Ds())
        {
            return importable && settled;
        }
        [[fallthrough]];
    case Command::kImportWaveSoundFile:
        return importable && (played_.Sar() || played_.Wii()) && document->native->tables && settled;
    case Command::kImportWave:
    case Command::kImportAudioFile:
        if (played_.Ds())
        {
            return importable && !song::DsWaveArchives(*played_.Ds()).empty();
        }
        return importable &&
               (played_.Sar() ? !played_.Sar()->WaveArchives().empty() : played_.Wii() && HoldsWaves(*played_.Wii()));
    case Command::kImportStream:
        return importable && ((played_.Sar() && document->native->tables && !played_.Sar()->Sounds().empty()) ||
                              (played_.Wii() && !played_.Wii()->Sounds().empty()) || played_.Ds());
    case Command::kExportMidiSoundFont:
        return exports && sequence_ &&
               ((played_.Sar() && archive_version_) || played_.Wii() || (song::OnGbaOrDs(played_) && archive_version_));
    case Command::kExportMidiSfz:
        return exports && sequence_ &&
               ((played_.Sar() && archive_version_) || played_.Wii() ||
                ((played_.Ds() || played_.Dse() || (played_.Driver() && played_.Driver()->WritesSfz())) &&
                 archive_version_));
    case Command::kExportOtherConsole:
        return exports && sequence_ && archive_version_ &&
               (((played_.Sar() || played_.Wii()) && document->native->tables) || song::OnGbaOrDs(played_));
    case Command::kUndo:
        return history_ && history_->CanUndo() && !typing && !replace_.shown;
    case Command::kRedo:
        return history_ && history_->CanRedo() && !typing && !replace_.shown;
    case Command::kLocatorsToSelection:
        return SelectedPart(song_, arrangement_) != nullptr;
    case Command::kOpenCloseEditor:
        return key_editor_open_ ||
               (sequence_ && (SelectedPart(song_, arrangement_) != nullptr ||
                              (arrangement_.selected_track >= 0 &&
                               static_cast<std::size_t>(arrangement_.selected_track) < sequence_->tracks.size())));
    case Command::kOpenListEditor:
        return list_editor_open_ || sequence_ != nullptr;
    case Command::kQuantize:
        return key_editor_open_ && key_editor_.focused && reading_version_ == nullptr;
    case Command::kCut:
        return key_editor_open_ && key_editor_.focused && !key_editor_.selected.empty() && !typing &&
               reading_version_ == nullptr;
    case Command::kCopy:
        return key_editor_open_ && key_editor_.focused && !key_editor_.selected.empty() && !typing;
    case Command::kPaste:
        return key_editor_open_ && key_editor_.focused && !key_editor_.clipboard.empty() && !typing &&
               reading_version_ == nullptr;
    case Command::kSelectAll:
        return !typing && ((key_editor_open_ && key_editor_.focused) || (list_editor_open_ && list_editor_.focused));
    case Command::kAddTrack:
        return TracksEditable();
    case Command::kDuplicateTracks:
        return TracksEditable() && SelectedTrack() != nullptr;
    case Command::kRemoveSelectedTracks:
        return TracksEditable() && SelectedTrack() != nullptr &&
               (SelectedTrack()->number != 0 || played_.Gba() || played_.Dse() || played_.Driver());
    case Command::kCopyTrack:
        return TracksEditable() && SelectedTrack() != nullptr;
    case Command::kPasteTrack:
        return TracksEditable() && copied_track_.has_value();
    default:
        return true;
    }
}

std::vector<std::string> App::Listed(Command command) const
{
    return command == Command::kRecentProjects ? RecentProjectTitles(settings_.recent_projects)
                                               : std::vector<std::string>();
}

void App::ExecuteListed(Command command, std::size_t index)
{
    // A recent project opens as through File > Open, once the user has decided about the document's changes.
    Wake();
    if (command != Command::kRecentProjects || index >= settings_.recent_projects.size() || !IsEnabled(command) ||
        WaitForTyping([this, command, index] { ExecuteListed(command, index); }))
    {
        return;
    }

    const std::string path = settings_.recent_projects[index];
    AfterChanges([this, path] { OpenOrImport(path); });
}

std::string App::Title(Command command, std::string_view name) const
{
    std::string title(name);
    if (command == Command::kUndo && history_ && history_->CanUndo())
    {
        title += " " + history_->Actions()[history_->Done() - 1].name;
    }
    else if (command == Command::kRedo && history_ && history_->CanRedo())
    {
        title += " " + history_->Actions()[history_->Done()].name;
    }

    return title;
}

} // namespace hokuto::app
