// SPDX-License-Identifier: MIT

#include "app/menus.h"

#include "app/transport.h"
#include "app/views.h"

namespace hokuto::app
{

std::span<const Menu> Menus()
{
    // The transport's commands are in the Transport menu, and the mixer and Studio Setup in the Studio menu. Undo and
    // Redo take Ctrl+Z and Ctrl+Shift+Z. Select All takes Ctrl+A. Space starts and stops, P sets the locators to the
    // selected part, N and B go to the next and the previous start or end of a part, and the keypad does the rest. G
    // and H zoom out and in, and with Shift they change the tracks' height. Shift+F shows the whole song. Ctrl+E opens
    // the selected part in the Key Editor, and closes the editor. Open takes Ctrl+O, Save Ctrl+S and Save As
    // Ctrl+Shift+S. Open List Editor has no key. Preferences is at the end of the Edit menu. On the Mac, it's in the
    // application menu with Command-Comma. Open and Save are for Hokuto's files. Sound archives come in through File >
    // Import, and go out through File > Export with MIDI and its instruments. A sequence file comes in through File >
    // Import as a new sound of the open archive, a bank file or a wave archive file as a new bank or wave archive, and
    // a wave sound file as new wave sounds. A wave file or a WAV file (Audio File…) comes in as a new wave of a wave
    // archive. A stream file or a WAV file comes in through Stream… as a new stream sound. The Project menu holds Add
    // Track, Duplicate Tracks and Remove Selected Tracks, without keys. Copy Track and Paste Track follow them.
    // Hokuto's songs are the sounds of one file, and a track goes from one to another through Copy Track and Paste
    // Track. Tempo Track, with Ctrl+T, shows or hides the arrangement's tempo row. Project Setup, with Shift+S, shows
    // the file's engine, gives every sound a mixing mode, and edits the file's tags. New Project, with Ctrl+N, starts
    // an empty file for the Switch, the Wii U or the 3DS. Add Sequence Sound… adds a sound with an empty song to the
    // file. Recent Projects lists the files opened or saved last, at the end of the File menu. On the Mac, Quit is in
    // the application menu.
    static const std::vector<Menu> kMenus = {
        {"File",
         {
             {"New Project…", Command::kNewProject, ImGuiMod_Ctrl | ImGuiKey_N},
             {"Open…", Command::kOpen, ImGuiMod_Ctrl | ImGuiKey_O},
             {},
             {"Save", Command::kSave, ImGuiMod_Ctrl | ImGuiKey_S},
             {"Save As…", Command::kSaveAs, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S},
             {},
             {.name = "Import",
              .items = {{"Sound Archive…", Command::kImportSoundArchive},
                        {"Sequence…", Command::kImportSequence},
                        {"Bank…", Command::kImportBank},
                        {"Wave Archive…", Command::kImportWaveArchive},
                        {"Wave…", Command::kImportWave},
                        {"Wave Sound File…", Command::kImportWaveSoundFile},
                        {"Stream…", Command::kImportStream},
                        {"Audio File…", Command::kImportAudioFile}}},
             {.name = "Export",
              .items = {{"Sound Archive…", Command::kExportSoundArchive},
                        {"MIDI and SoundFont…", Command::kExportMidiSoundFont},
                        {"MIDI and SFZ…", Command::kExportMidiSfz},
                        {"Another Console…", Command::kExportOtherConsole}}},
             {},
             {.name = "Recent Projects", .command = Command::kRecentProjects, .none = "No Recent Projects"},
#ifndef __APPLE__
             {},
             {"Quit", Command::kQuit, ImGuiMod_Ctrl | ImGuiKey_Q},
#endif
         }},
        {"Edit",
         {
             {"Undo", Command::kUndo, ImGuiMod_Ctrl | ImGuiKey_Z},
             {"Redo", Command::kRedo, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z},
             {kUndoHistory, Command::kUndoHistory},
             {},
             {"Cut", Command::kCut, ImGuiMod_Ctrl | ImGuiKey_X},
             {"Copy", Command::kCopy, ImGuiMod_Ctrl | ImGuiKey_C},
             {"Paste", Command::kPaste, ImGuiMod_Ctrl | ImGuiKey_V},
             {"Select All", Command::kSelectAll, ImGuiMod_Ctrl | ImGuiKey_A},
             {},
             {"Open/Close Editor", Command::kOpenCloseEditor, ImGuiMod_Ctrl | ImGuiKey_E},
             {"Open List Editor", Command::kOpenListEditor},
             {"Quantize", Command::kQuantize, ImGuiKey_Q},
             {},
             {"Zoom In", Command::kZoomIn, ImGuiKey_H, true},
             {"Zoom Out", Command::kZoomOut, ImGuiKey_G, true},
             {"Zoom Full", Command::kZoomFull, ImGuiMod_Shift | ImGuiKey_F},
             {"Zoom In Vertically", Command::kZoomInVertically, ImGuiMod_Shift | ImGuiKey_H, true},
             {"Zoom Out Vertically", Command::kZoomOutVertically, ImGuiMod_Shift | ImGuiKey_G, true},
#ifndef __APPLE__
             {},
             {"Preferences…", Command::kPreferences, ImGuiMod_Ctrl | ImGuiKey_Comma},
#endif
         }},
        {"Project",
         {
             {"Add Sequence Sound…", Command::kAddSequenceSound},
             {"Add Bank…", Command::kAddBank},
             {},
             {"Add Track", Command::kAddTrack},
             {"Duplicate Tracks", Command::kDuplicateTracks},
             {"Remove Selected Tracks", Command::kRemoveSelectedTracks},
             {},
             {"Copy Track", Command::kCopyTrack},
             {"Paste Track", Command::kPasteTrack},
             {},
             {"Tempo Track", Command::kTempoTrack, ImGuiMod_Ctrl | ImGuiKey_T},
             {},
             {"Project Setup…", Command::kProjectSetup, ImGuiMod_Shift | ImGuiKey_S},
         }},
        {"Transport",
         {
             {"Start/Stop", Command::kStartStop, ImGuiKey_Space},
             {"Start", Command::kStart, ImGuiKey_KeypadEnter},
             {"Stop", Command::kStop, ImGuiKey_Keypad0},
             {},
             {"Cycle", Command::kCycle, ImGuiKey_KeypadDivide},
             {"Locators to Selection", Command::kLocatorsToSelection, ImGuiKey_P},
             {},
             {"Go to Project Start", Command::kGoToStart, ImGuiKey_KeypadDecimal},
             {"Go to Left Locator", Command::kGoToLeftLocator, ImGuiKey_Keypad1},
             {"Go to Right Locator", Command::kGoToRightLocator, ImGuiKey_Keypad2},
             {"Locate Next Event", Command::kLocateNextEvent, ImGuiKey_N, true},
             {"Locate Previous Event", Command::kLocatePreviousEvent, ImGuiKey_B, true},
             {kShowBars ? "Back a Bar" : "Rewind", Command::kRewind, ImGuiKey_KeypadSubtract, true},
             {kShowBars ? "Forward a Bar" : "Forward", Command::kForward, ImGuiKey_KeypadAdd, true},
         }},
        {"Studio",
         {
             {kMixer, Command::kMixer, kMixerKeys},
             {},
             {"Studio Setup…", Command::kStudioSetup},
         }},
        {"Window",
         {
             {kInspector, Command::kInspector, kInspectorKeys},
             {kBrowser, Command::kBrowser, kBrowserKeys},
             {kVariables, Command::kVariables},
             {},
             {"Reset Layout", Command::kResetLayout},
         }},
    };
    return kMenus;
}

} // namespace hokuto::app
