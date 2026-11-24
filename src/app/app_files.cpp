// SPDX-License-Identifier: MIT

// Opening and importing files, the files that the dialogs return, new files, saving, and the question about unsaved
// changes. app_dialogs.cpp contains the file dialogs.

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "ntdlib/rvl/snd/rsar_options.h"
#include "xsflib/binary.h"
#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "app/engine_names.h"
#include "app/file_dialogs.h"
#include "app/file_saves.h"
#include "app/recent_projects.h"
#include "app/system_ui.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "edit/ds_files.h"
#include "edit/dse_files.h"
#include "edit/midi_imports.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/streams.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// The ID of the window that waits while a file opens. The window's title is the file's name followed by the ID. Dear
// ImGui shows the name, and the window keeps the same ID for every file.
constexpr char kOpening[] = "###Opening";

// The ID of the window that shows while an Import or Replace window is being prepared, in the same way.
constexpr char kPreparing[] = "###Preparing";

// Begins the modal window `title` over the middle of the main window, in the style of the Import window, for a window
// that waits for work. With `open`, the window has a close button. Returns whether the window shows. The value is
// the one that ImGui::BeginPopupModal returns. Dear ImGui measures a popup in a hidden frame each time it opens. The
// window then opens in the middle at its measured size. Its text doesn't change its height, and the user can drag it.
bool BeginWaitWindow(const std::string& title, bool* open)
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Px(420.0f), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::BeginPopupModal(title.c_str(), open, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    return visible;
}

// Whether the file at `path` starts as a MIDI file.
bool IsMidiFile(const std::string& path)
{
    std::array<uint8_t, 4> head{};
    std::ifstream file(xsflib::PathFromUtf8(path), std::ios::binary);
    file.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    return file.gcount() == static_cast<std::streamsize>(head.size()) && edit::IsMidi(head);
}

// Whether the file at `path` starts as a file that the window takes: a Hokuto file in either form, a sound archive of
// the DS, the Wii, the 3DS, the Wii U or the Switch, a GBA ROM, a DS image or a MIDI file. A file that can't be read
// counts as one: the open says why. The header of a DS image fills the first 512 bytes.
bool OpensAsDocument(const std::string& path)
{
    std::ifstream file(xsflib::PathFromUtf8(path), std::ios::binary);
    if (!file)
    {
        return true;
    }

    std::vector<uint8_t> head(0x200);
    file.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(file.gcount()));

    return format::FirstInputKind(head, format::kInputOrder).has_value();
}

// The dialogs whose files open an Import or Replace window. Such a file waits while the program builds the archive of
// an edit.
constexpr std::array kImports = {Dialog::kImportSequence,      Dialog::kImportBank, Dialog::kImportWaveArchive,
                                 Dialog::kImportWaveSoundFile, Dialog::kImportWave, Dialog::kImportAudioFile,
                                 Dialog::kImportStream,        Dialog::kReplace};

// The frames that DrawPreparing draws before an Import or Replace window's preparation starts. Dear ImGui hides a
// window that fits its contents in its first frame. The next frame then reaches the display before the work starts.
constexpr int kPreparingFrames = 3;

// Puts `play` into `work` as what plays the file in front.
void TakePlay(OpenWork& work, ArchivePlay play)
{
    work.document = std::move(play.version);
    work.played = std::move(play.played);
    work.sequences = std::move(play.sequences);
    work.start_fields = std::move(play.start_fields);
}

} // namespace

void App::DropFilesOnWindow(const std::vector<std::string>& paths)
{
    if (WaitForTyping([this, paths] { DropFilesOnWindow(paths); }))
    {
        return;
    }

    const std::string& path = paths.front();
    const std::string name = xsflib::Utf8(xsflib::PathFromUtf8(path).filename());
    if (WindowWaits())
    {
        ShowMessage("Close the open window first, then drop " + name + " again.", MessageKind::kNote);
        return;
    }
    if (!OpensAsDocument(path))
    {
        ShowMessage("Can't open " + name + ". " + format::kOpenedKinds +
                    " To add a WAV or wave file, drag it onto the Bank Editor or use File > Import.");
        return;
    }
    if (MixdownRunning() && !(history_ && IsMidiFile(path)))
    {
        ShowMessage("An export is running. Drop " + name + " again once it ends.", MessageKind::kNote);
        return;
    }

    if (paths.size() > 1)
    {
        ShowMessage("Hokuto opens one file at a time. Opening " + name + ".", MessageKind::kNote);
    }

    // A MIDI file comes into the file that shows.
    if (history_ && IsMidiFile(path))
    {
        OpenSequenceImport(path);
        return;
    }
    AfterChanges([this, path] { OpenOrImport(path); });
}

void App::OpenOrImport(const std::string& path, bool import)
{
    // A MIDI file opens in a new file on the Switch's engine. The Import window then opens on it.
    if (IsMidiFile(path))
    {
        NewDocument(format::Engine::kSwitch);
        OpenSequenceImport(path);
        return;
    }

    // The window waits with Cancel while the work runs. A new open drops the work of the one before.
    if (opening_)
    {
        dropped_opens_.push_back(std::move(opening_->work));
    }
    const std::string name = xsflib::Utf8(xsflib::PathFromUtf8(path).filename());
    const auto work = [path, import]
    {
        return Opened(path, import);
    };
    opening_ = Opening{std::async(std::launch::async, work), path, name};
}

OpenWork App::Opened(const std::string& path, bool import)
{
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    std::vector<uint8_t> bytes;
    try
    {
        // A file that isn't there, such as a recent project that moved, can't be found.
        if (!xsflib::ReadFile(file, bytes))
        {
            std::error_code error;
            const bool missing = !std::filesystem::exists(file, error) && !error;
            return {.error = (missing ? "Can't find " : "Can't read ") + xsflib::Utf8(file.filename()) + "."};
        }
    }
    catch (const std::exception& error)
    {
        return {.error = "Can't read " + xsflib::Utf8(file.filename()) + ". " + format::AsSentence(error.what())};
    }

    // A Hokuto file opens. A sound archive goes in as a new file. The message names the kind of any other file.
    if (const std::optional<format::InputKind> kind = format::FirstInputKind(bytes, format::kInputOrder))
    {
        switch (*kind)
        {
        case format::InputKind::kHokuto:
            return OpenedFile(xsflib::Utf8(file.filename()), bytes);
        case format::InputKind::kWiiArchive:
            return ImportedWiiArchive(file, std::move(bytes));
        case format::InputKind::kLaterArchive:
            return ImportedArchive(file, std::move(bytes));
        case format::InputKind::kGbaRom:
            return ImportedGbaRom(file, std::move(bytes));
        case format::InputKind::kDsArchive:
        case format::InputKind::kDsImage:
            return ImportedDs(file, std::move(bytes));
        case format::InputKind::kMidi:
            break;
        }
    }

    return {.error = format::NotOpenedText(file, bytes, import)};
}

std::shared_ptr<const format::File> App::ForEditing(format::File read)
{
    // MP2K's songs, NITRO-Composer's sequences, DSE's songs and the songs of the GBA's other drivers have no timelines.
    const bool gba = read.native && read.native->engine == format::Engine::kGba;
    const bool ds = read.native && read.native->engine == format::Engine::kDs;
    const bool dse = read.native && read.native->engine == format::Engine::kDse;
    const bool driver = read.native && read.native->driver;

    return std::make_shared<const format::File>(gba || ds || dse || driver ? std::move(read)
                                                                           : edit::OpenTimelines(std::move(read)));
}

ArchivePlay App::PlayOf(const std::string& name, std::shared_ptr<const format::File> document)
{
    // The export throws for a file without a native archive. A file of the Wii builds its archive for the Wii's model,
    // and has no group files. A file of the GBA builds its ROM, with filler runs for the songs that don't fit after its
    // data (format::PlayedBytes). A file of the DS builds its archive, and plays by the driver of the cartridge that it
    // keeps.
    ArchivePlay play;
    play.version = std::move(document);
    const format::File& file = *play.version;
    const std::vector<uint8_t> built = format::PlayedBytes(file);
    const std::optional<format::Engine> engine =
        file.native ? std::optional<format::Engine>(file.native->engine) : std::nullopt;
    if (engine == format::Engine::kGba)
    {
        play.played = song::LoadGbaRom(built);
        play.sequences = edit::GbaSongSequences(file, *play.played.Gba());
    }
    else if (engine == format::Engine::kDs)
    {
        play.played = song::LoadDsArchive(built, DsBuildOfFile(file), edit::DsGameOfFile(file));
        play.sequences = format::DsSoundSequences(file, built);
    }
    else if (engine == format::Engine::kDse)
    {
        play.played = edit::DseSetOfFile(file, built);
        play.sequences = format::DseSoundSequences(file, built, song::DseSoundRefs(*play.played.Dse()));
    }
    else if (file.native && file.native->driver)
    {
        play.played = song::LoadDriverRom(*engine, built);
        play.sequences = edit::DriverSoundSequences(file, *play.played.Driver());
    }
    else if (engine == format::Engine::kWii)
    {
        play.played = song::LoadWiiArchive(built, name, ntdlib::rvl::snd::kRsarOptions);
        play.sequences = format::SequencesOfFiles(file, built, *play.played.Wii());
        play.start_fields = StartFieldsOf(file, built);
    }
    else
    {
        play.played = song::LoadArchive(built, name, format::ExportGroupFiles(file));
        play.sequences = format::SequencesOfFiles(file, built, *play.played.Sar());
        play.start_fields = StartFieldsOf(file, built);
    }

    return play;
}

OpenWork App::OpenedFile(const std::string& name, std::span<const uint8_t> bytes)
{
    // The file with its timelines laid out for editing, and the archive that it builds for playback, with the file's
    // group files (PlayOf). A file with more than one archive opens each archive the same way, and each archive plays
    // from its join alone.
    OpenWork work;
    work.text_form = format::IsText(bytes);
    try
    {
        format::Project project = format::ReadProject(bytes);
        if (project.archives.size() == 1)
        {
            // A file of version 1 keeps no starts. Its archive's SHA-1 finds them.
            std::shared_ptr<const format::File> document =
                ForEditing(format::WithOlderStarts(std::move(project.archives.front()), bytes));
            if (const std::optional<std::string> note = format::OlderDseLevelsNote(*document, bytes))
            {
                work.notes.push_back(*note);
            }
            TakePlay(work, PlayOf(name, std::move(document)));
            return work;
        }

        for (format::File& archive : project.archives)
        {
            archive = *ForEditing(std::move(archive));
        }
        work.project = std::make_shared<const format::Project>(std::move(project));
        for (const format::File& archive : work.project->archives)
        {
            work.archives.push_back(PlayOf(name, std::shared_ptr<const format::File>(work.project, &archive)));
        }
        TakePlay(work, work.archives[work.project->front]);
    }
    catch (const xsflib::FormatError& error)
    {
        return {.error = "Can't open " + name + ". " + format::AsSentence(error.what())};
    }
    catch (const std::invalid_argument& error)
    {
        return {.error = "Can't play " + name + ". " + format::AsSentence(error.what())};
    }
    catch (const std::runtime_error& error)
    {
        return {.error = error.what()};
    }
    catch (const std::exception& error)
    {
        return {.error = "Can't open " + name + ". " + format::AsSentence(error.what())};
    }

    return work;
}

OpenWork App::ImportedArchive(const std::filesystem::path& file, std::vector<uint8_t> bytes)
{
    // The new file with its timelines laid out for editing, and the archive for playback with the group files beside
    // it. The file keeps the group files, and the streams beside the archive that its file table names. The timelines
    // build their archive for playback.
    const std::string name = xsflib::Utf8(file.filename());
    OpenWork work;
    work.imported = true;
    try
    {
        work.played = song::LoadArchive(bytes, name, file);
        const ntdlib::sar::SoundArchive& read = *work.played.Sar();
        work.document = std::make_shared<const format::File>(
            edit::OpenTimelines(format::ImportArchive(bytes, read.ReadOptions().flavor, read.GroupFiles(),
                                                      song::ReadStreams(read, file), read.GroupFileGroups())));
        if (!work.document->timelines->empty())
        {
            bytes = format::ExportArchive(*work.document);
            work.played = song::LoadArchive(bytes, name, format::ExportGroupFiles(*work.document));
        }
        work.sequences = format::SequencesOfFiles(*work.document, bytes, *work.played.Sar());
        work.start_fields = StartFieldsOf(*work.document, bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        return {.error = "Can't import " + name + ". " + format::AsSentence(error.what())};
    }
    catch (const std::runtime_error& error)
    {
        return {.error = error.what()};
    }
    catch (const std::exception& error)
    {
        return {.error = "Can't import " + name + ". " + format::AsSentence(error.what())};
    }

    return work;
}

OpenWork App::ImportedWiiArchive(const std::filesystem::path& file, std::vector<uint8_t> bytes)
{
    // The new file with its timelines laid out for editing, and the archive for the Wii's model. The file's export
    // gives the game's archive back, byte for byte. The game's main.dol on the extracted disc gives the build of NW4R
    // SND, and the file keeps it. Without it, the archive's version gives the build. The file keeps the streams beside
    // the archive that its file table names. The timelines build their archive for playback.
    const std::string name = xsflib::Utf8(file.filename());
    OpenWork work;
    work.imported = true;
    try
    {
        std::optional<ntdlib::SndBuild> build;
        if (const std::optional<std::filesystem::path> dol = song::FindMainDol(file))
        {
            std::vector<uint8_t> code;
            if (xsflib::ReadFile(*dol, code))
            {
                build = song::ReadWiiBuild(code);
            }
        }
        std::map<uint32_t, std::vector<uint8_t>> streams =
            song::ReadStreams(*song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions), file);
        work.document = std::make_shared<const format::File>(
            edit::OpenTimelines(format::ImportWiiArchive(bytes, build, std::move(streams))));
        if (!work.document->timelines->empty())
        {
            bytes = format::ExportArchive(*work.document);
        }
        work.played = song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions);
        work.sequences = format::SequencesOfFiles(*work.document, bytes, *work.played.Wii());
        work.start_fields = StartFieldsOf(*work.document, bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        return {.error = "Can't import " + name + ". " + format::AsSentence(error.what())};
    }
    catch (const std::exception& error)
    {
        return {.error = "Can't import " + name + ". " + format::AsSentence(error.what())};
    }

    return work;
}

OpenWork App::ImportedGbaRom(const std::filesystem::path& file, std::vector<uint8_t> bytes)
{
    // The new file, and the ROM for playback. The file's export reproduces the ROM, byte for byte. A ROM with another
    // of the GBA's drivers is cut by its driver. A ROM with MusicPlayer2000 beside another driver gives an archive for
    // each driver (format::ImportGbaArchives), and each archive plays from its join alone.
    const std::string name = xsflib::Utf8(file.filename());
    OpenWork work;
    work.imported = true;
    try
    {
        format::ImportedArchives imported = format::ImportGbaArchives(bytes);
        work.notes = std::move(imported.notes);
        if (imported.project.archives.size() == 1)
        {
            ArchivePlay play;
            play.version = std::make_shared<const format::File>(std::move(imported.project.archives.front()));
            const format::NativeArchive& native = *play.version->native;
            if (native.driver)
            {
                play.played = song::LoadDriverRom(native.engine, std::move(bytes));
                play.sequences = edit::DriverSoundSequences(*play.version, *play.played.Driver());
            }
            else
            {
                play.played = song::LoadGbaRom(std::move(bytes));
                play.sequences = edit::GbaSongSequences(*play.version, *play.played.Gba());
            }
            TakePlay(work, std::move(play));
            return work;
        }

        work.project = std::make_shared<const format::Project>(std::move(imported.project));
        for (const format::File& archive : work.project->archives)
        {
            work.archives.push_back(PlayOf(name, std::shared_ptr<const format::File>(work.project, &archive)));
        }
        TakePlay(work, work.archives[work.project->front]);
    }
    catch (const std::exception& error)
    {
        return {.error = "Can't import " + name + ". " + format::AsSentence(error.what())};
    }

    return work;
}

OpenWork App::ImportedDs(const std::filesystem::path& file, std::vector<uint8_t> bytes)
{
    // The new file, and the archive for playback. From a cartridge image, the import uses the largest sound archive,
    // and the file keeps the image. The driver of the image's ARM7 binary plays the archive. A bare archive plays by
    // the default build. The file's export reproduces the archive or the image, byte for byte. An image without a
    // sound archive can contain DSE's files instead. The driver of its ARM9 binary plays them.
    const std::string name = xsflib::Utf8(file.filename());
    OpenWork work;
    work.imported = true;
    try
    {
        const bool image = format::IsInputKind(bytes, format::InputKind::kDsImage);
        if (image && format::HoldsDseOnly(bytes))
        {
            work.document = std::make_shared<const format::File>(format::ImportDseImage(bytes));
            const std::vector<uint8_t> set = format::ExportArchive(*work.document);
            work.played = edit::DseSetOfFile(*work.document, set);
            work.sequences = format::DseSoundSequences(*work.document, set, song::DseSoundRefs(*work.played.Dse()));
            work.notes = song::DseOpenNotes(bytes, *work.played.Dse());
            return work;
        }

        work.document =
            std::make_shared<const format::File>(image ? format::ImportDsImage(bytes) : format::ImportDsArchive(bytes));
        std::vector<uint8_t> sdat = format::ExportArchive(*work.document);
        work.sequences = format::DsSoundSequences(*work.document, sdat);
        work.played =
            song::LoadDsArchive(std::move(sdat), DsBuildOfFile(*work.document), edit::DsGameOfFile(*work.document));
    }
    catch (const std::exception& error)
    {
        return {.error = "Can't import " + name + ". " + format::AsSentence(error.what())};
    }

    return work;
}

void App::FinishOpening()
{
    // A cancelled open's work goes once it's done. The end of the work counts as an event.
    std::erase_if(dropped_opens_, Done<OpenWork>);
    if (!opening_ || !Done(opening_->work))
    {
        return;
    }

    OpenWork work = opening_->work.get();
    const std::string path = std::move(opening_->path);
    const std::string name = std::move(opening_->name);
    opening_.reset();
    Wake();
    if (!work.error.empty())
    {
        // A recent project that doesn't open leaves the list.
        const std::size_t recent = settings_.recent_projects.size();
        settings_.recent_projects = WithoutRecentProject(std::move(settings_.recent_projects), path);
        if (settings_.recent_projects.size() != recent)
        {
            ImGui::MarkIniSettingsDirty();
            work.error += " It was removed from Recent Projects.";
        }
        ShowMessage(work.error);
        return;
    }

    // An imported sound archive gives a file that stays untitled. Its dialog starts beside the archive.
    ShowDocument(std::move(work.document), std::move(work.played), std::move(work.sequences),
                 std::move(work.start_fields), name, std::move(work.project), std::move(work.archives));
    document_path_ = work.imported ? std::string() : path;
    import_path_ = work.imported ? path : std::string();
    text_form_ = work.text_form;
    if (!work.imported)
    {
        AddRecentProject(path);
    }

    std::string notes;
    for (const std::string& note : work.notes)
    {
        notes += (notes.empty() ? "" : "\n\n") + note;
    }
    if (!notes.empty())
    {
        ShowMessage(notes, MessageKind::kNote);
    }
}

void App::AddRecentProject(const std::string& path)
{
    settings_.recent_projects = WithRecentProject(std::move(settings_.recent_projects), path);
    ImGui::MarkIniSettingsDirty();
}

void App::DrawOpening()
{
    if (!opening_)
    {
        if (ImGui::IsPopupOpen(kOpening) && ImGui::BeginPopupModal(kOpening))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }

    if (!ImGui::IsPopupOpen(kOpening))
    {
        ImGui::OpenPopup(kOpening);
    }

    // A window over the middle of the main window, in the style of the Import window. The file's name is its title.
    const std::string title = opening_->name + kOpening;
    bool open = true;
    const bool visible = BeginWaitWindow(title, &open);
    bool cancel = !visible;
    if (visible)
    {
        ImGui::AlignTextToFramePadding();
        MutedText("Opening the file…");
        ImGui::SameLine();
        const float button = ImGui::CalcTextSize("Cancel").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - button, 0.0f));
        cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (cancel)
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Cancel keeps the file that shows. The work runs on to its end.
    if (cancel)
    {
        dropped_opens_.push_back(std::move(opening_->work));
        opening_.reset();
    }
}

void App::DrawPreparing()
{
    if (!preparing_)
    {
        if (ImGui::IsPopupOpen(kPreparing) && ImGui::BeginPopupModal(kPreparing))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }

    if (!ImGui::IsPopupOpen(kPreparing))
    {
        ImGui::OpenPopup(kPreparing);
    }

    // The file's name is the window's title. The pointer shows that the program is busy while the preparation runs.
    const std::string title = xsflib::Utf8(xsflib::PathFromUtf8(preparing_->second).filename()) + kPreparing;
    if (BeginWaitWindow(title, nullptr))
    {
        MutedText(preparing_->first == Dialog::kReplace ? "Preparing the replacement…" : "Preparing the import…");
        ImGui::EndPopup();
    }
    ImGui::SetMouseCursor(ImGuiMouseCursor_Progress);
    preparing_frames_++;
}

void App::OpenChosenFile()
{
    std::optional<std::pair<Dialog, std::string>> chosen;
    {
        std::lock_guard lock(chosen_mutex_);
        if (!preparing_ && (!chosen_ || std::ranges::find(kImports, chosen_->first) == kImports.end() || !Settling()))
        {
            chosen = std::exchange(chosen_, std::nullopt);
        }
    }

    // The program's thread prepares an Import or Replace window. On a large archive that takes seconds. A window that
    // says so shows first, and the preparation starts once it has reached the display.
    if (chosen && !chosen->second.empty() && std::ranges::find(kImports, chosen->first) != kImports.end())
    {
        preparing_ = std::move(chosen);
        preparing_frames_ = 0;
        return;
    }
    if (preparing_ && preparing_frames_ >= kPreparingFrames && !Settling())
    {
        chosen = std::exchange(preparing_, std::nullopt);
    }

    if (chosen && chosen->first == Dialog::kSaveAs)
    {
        // What waited for the save follows it. A cancelled dialog gives no file. It drops what waited, and so does a
        // save that fails.
        const std::function<void()> then = std::exchange(after_save_, nullptr);
        const std::string path = chosen->second.empty() ? std::string() : WithExtension(chosen->second, ".hok");
        if (!path.empty() && MayWriteTo(chosen->second, xsflib::PathFromUtf8(path)))
        {
            SaveTo(path);
            if (then && history_ && !history_->Modified())
            {
                then();
            }
        }
    }
    else if (chosen && (chosen->first == Dialog::kExport || chosen->first == Dialog::kExportFolder))
    {
        ExportTo(chosen->second);
    }
    else if (chosen && chosen->first == Dialog::kMixdownFolder)
    {
        SetMixdownFolder(chosen->second);
    }
    else if (chosen && chosen->first == Dialog::kReplace)
    {
        OpenReplacement(chosen->second);
    }
    else if (chosen && chosen->first == Dialog::kImportSequence)
    {
        OpenSequenceImport(chosen->second);
    }
    else if (chosen && (chosen->first == Dialog::kImportBank || chosen->first == Dialog::kImportWaveArchive ||
                        chosen->first == Dialog::kImportWaveSoundFile))
    {
        const edit::FileKind kind = chosen->first == Dialog::kImportBank          ? edit::FileKind::kBank
                                    : chosen->first == Dialog::kImportWaveArchive ? edit::FileKind::kWaveArchive
                                                                                  : edit::FileKind::kWaveSoundFile;
        OpenItemImport(chosen->second, kind);
    }
    else if (chosen && (chosen->first == Dialog::kImportWave || chosen->first == Dialog::kImportAudioFile))
    {
        OpenWaveImport(chosen->second, chosen->first == Dialog::kImportAudioFile);
    }
    else if (chosen && chosen->first == Dialog::kImportStream)
    {
        OpenStreamImport(chosen->second);
    }
    else if (chosen && chosen->first == Dialog::kDsiKey)
    {
        SetDsiKey(chosen->second);
    }
    else if (chosen)
    {
        OpenOrImport(chosen->second, chosen->first == Dialog::kImport);
    }
}

void App::NewDocument(format::Engine engine)
{
    // The new file, and the archive that it builds for playback. The archive contains its player alone, and on the DS
    // an empty bank.
    std::shared_ptr<const format::File> document;
    song::Played played;
    std::vector<std::optional<uint32_t>> sequences;
    std::optional<std::vector<std::optional<uint32_t>>> start_fields;
    try
    {
        document = std::make_shared<const format::File>(format::NewFile(engine));
        const std::vector<uint8_t> built = format::ExportArchive(*document);
        if (engine == format::Engine::kDs)
        {
            played = song::LoadDsArchive(built, DsBuildOfFile(*document));
            sequences = format::DsSoundSequences(*document, built);
        }
        else if (engine == format::Engine::kWii)
        {
            played = song::LoadWiiArchive(built, kUntitled, ntdlib::rvl::snd::kRsarOptions);
            sequences = format::SequencesOfFiles(*document, built, *played.Wii());
            start_fields = StartFieldsOf(*document, built);
        }
        else
        {
            played = song::LoadArchive(built, kUntitled, format::ExportGroupFiles(*document));
            sequences = format::SequencesOfFiles(*document, built, *played.Sar());
            start_fields = StartFieldsOf(*document, built);
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(std::string("Can't create a new file. ") + format::AsSentence(error.what()));
        return;
    }

    ShowDocument(std::move(document), std::move(played), std::move(sequences), std::move(start_fields), kUntitled);
    document_path_.clear();
    import_path_.clear();
    text_form_ = false;
}

void App::ShowDocument(std::shared_ptr<const format::File> document, song::Played played,
                       std::vector<std::optional<uint32_t>> sequences,
                       std::optional<std::vector<std::optional<uint32_t>>> start_fields, const std::string& name,
                       std::shared_ptr<const format::Project> project, std::vector<ArchivePlay> archives)
{
    // The file replaces the one that was open before. A preview from the last archive stops too, and so does the
    // made-up song. An export or a replacement in the last file ends.
    EndLastFileWork();
    if (project)
    {
        history_.emplace(std::move(project));
    }
    else
    {
        history_.emplace(std::move(document));
    }
    history_rows_.clear();
    action_selections_.clear();
    preview_ = false;
    archive_plays_ = std::move(archives);
    file_name_ = name;
    UpdateTitle();
    ShowPlay(
        {history_->Current(), std::move(played), std::move(sequences), std::move(start_fields), std::nullopt, {}, {}});

    // In a file with more than one archive, the browser lists each archive's items under its row.
    const std::size_t front = history_->Front();
    for (std::size_t a = 0; a < archive_plays_.size(); a++)
    {
        if (a != front)
        {
            archive_plays_[a].browser = ListedBrowserOf(a);
            archive_plays_[a].browser_state.top = true;
        }
    }
    ListArchives();
}

void App::ShowPlay(ArchivePlay play)
{
    // The zones show no sequence until the user chooses one, and the transport runs over the empty song. The editors
    // of the last archive's items close.
    copied_track_.reset();
    InstallPlay(std::move(play.version), std::move(play.played), std::move(play.sequences),
                std::move(play.start_fields));
    sequence_.reset();
    model_.reset();
    song_.reset();
    ForgetEngine();

    // The meters and the peak holds of the Stereo Out and the FX channels start again with the file. The tracks' go
    // with the song.
    master_.meter = {kSilence, kSilence};
    master_.peak = kSilence;
    fx_.meters.fill({kSilence, kSilence});
    fx_.peaks.fill(kSilence);
    effect_window_.reset();
    effect_draft_.reset();
    CloseKeyEditor();
    CloseListEditor();
    CloseItemEditors();
    transport_.SetCycle(false);
    transport_.SetPosition(0.0);
    transport_.TakeEmptySong();

    // The browser lists the archive's sounds, sequences, banks and wave archives from the top, with nothing chosen and
    // no search.
    ResetBrowser();
    arrangement_.selected_track = 0;
    arrangement_.selected_part_track = -1;
    arrangement_.selected_part = -1;

    // The arrangement shows the new file from its start and its first track. A file that keeps a view scrolls to the
    // view's place once its sound shows (RestoreView).
    arrangement_.scroll_to = std::pair(0.0, 0.0f);
    SelectKeptViewSound(play.view ? play.view : history_->Current()->view);
}

void App::InstallPlay(std::shared_ptr<const format::File> version, song::Played played,
                      std::vector<std::optional<uint32_t>> sequences,
                      std::optional<std::vector<std::optional<uint32_t>>> start_fields)
{
    played_ = std::move(played);
    archive_version_ = std::move(version);
    reading_version_.reset();
    file_sequences_ = std::move(sequences);
    start_fields_ = std::move(start_fields);
    start_values_.clear();
    told_missing_banks_.clear();
    told_unrelocatable_.clear();
    changes_.clear();
    engine_edit_.reset();
    gba_moved_groups_.clear();
}

void App::CloseItemEditors()
{
    CloseBankEditor();
    bank_to_open_.reset();
    bank_drops_.clear();
    bank_drop_.reset();
    CloseProperties();
}

std::vector<BrowserGroup> App::ListedBrowserOf(std::size_t archive)
{
    // The browser lists archive `archive` as it would in front, through ListBrowser with that archive in front for a
    // moment. Its groups don't drag: a drag names a place of the archive in front.
    const std::size_t front = history_->Front();
    ArchivePlay& play = archive_plays_[archive];
    std::swap(played_, play.played);
    std::swap(archive_version_, play.version);
    std::swap(file_sequences_, play.sequences);
    std::swap(start_fields_, play.start_fields);
    std::vector<BrowserGroup> front_groups = browser_;
    history_->SetFront(archive);
    ListBrowser();
    history_->SetFront(front);
    std::swap(played_, play.played);
    std::swap(archive_version_, play.version);
    std::swap(file_sequences_, play.sequences);
    std::swap(start_fields_, play.start_fields);
    std::swap(browser_, front_groups);
    for (BrowserGroup& group : front_groups)
    {
        for (BrowserItem& item : group.items)
        {
            item.children_drag = false;
        }
    }

    return front_groups;
}

void App::ShowFront(std::size_t left)
{
    // The work on the archive that left the front ends, and so do its sequence and its editors: they show items of
    // that archive. The archive keeps what plays it, its view, and its groups and choices in the browser.
    const std::size_t front = history_->Front();
    if (archive_plays_.size() < history_->Archives())
    {
        archive_plays_.resize(history_->Archives());
    }
    ArchivePlay& kept = archive_plays_[left];
    kept.view = CurrentView();
    kept.browser_state = browser_state_;
    EndLastFileWork();
    CloseSequence();
    CloseItemEditors();
    copied_track_.reset();
    kept.version = archive_version_;
    kept.played = played_;
    kept.sequences = file_sequences_;
    kept.start_fields = start_fields_;
    kept.browser_state.clicked = false;
    kept.browser_state.playing.reset();
    kept.browser_state.export_place.reset();
    kept.browser_state.replace_place.reset();
    kept.browser_state.edit_place.reset();
    kept.browser_state.archive.reset();

    // The archive in front plays what played it last, while that came from its version. Otherwise the program builds
    // what plays it, as an open does. An archive that can't play leaves the front again.
    ArchivePlay play = std::move(archive_plays_[front]);
    archive_plays_[front] = {};
    if (!play.version || !SameArchive(*play.version, *history_->Current()))
    {
        try
        {
            ArchivePlay built = PlayOf(file_name_, history_->Current());
            play.version = std::move(built.version);
            play.played = std::move(built.played);
            play.sequences = std::move(built.sequences);
            play.start_fields = std::move(built.start_fields);
        }
        catch (const std::exception& error)
        {
            ShowMessage(std::string("Can't play the sounds of ") + DriverName(history_->Current()->engine) + ". " +
                        format::AsSentence(error.what()));
            archive_plays_[front] = std::move(play);
            history_->SetFront(left);
            play = std::move(archive_plays_[left]);
            archive_plays_[left] = {};
        }
    }

    // The browser lists the archive again, and keeps its choices: a choice that brought the archive to the front reads
    // its sound, plays its item or opens its window. A view waits for its sound. The groups of the archive that left
    // stay in the browser under its row.
    InstallPlay(std::move(play.version), std::move(play.played), std::move(play.sequences),
                std::move(play.start_fields));
    browser_state_ = std::move(play.browser_state);
    browser_state_.archive.reset();
    ListBrowser();
    DropUnlistedChoices();
    shown_group_ = -1;
    shown_item_ = -1;
    pending_view_.reset();
    if (browser_state_.selected.group < 0)
    {
        SelectKeptViewSound(play.view ? play.view : history_->Current()->view);
    }
    else if (play.view && browser_state_.selected.group == static_cast<int>(kSequencesGroup) &&
             browser_state_.selected.item >= 0 &&
             browser_[kSequencesGroup].items[static_cast<std::size_t>(browser_state_.selected.item)].number ==
                 play.view->sound)
    {
        pending_view_ = play.view;
    }
    browser_state_.reveal = true;
    if (history_->Front() != left)
    {
        archive_plays_[left].browser = ListedBrowserOf(left);
    }
    ListArchives();

    // Every change of the front passes here, an undo or a redo across archives among them. Such a move changes whether
    // the file differs from its saved version.
    UpdateTitle();
}

void App::BringToFront(std::size_t archive)
{
    if (!history_ || archive >= history_->Archives() || archive == history_->Front())
    {
        return;
    }

    // An item's edit whose work still runs enters the history first: it belongs to the archive in front.
    TakeItemEdit(true);
    const std::size_t left = history_->Front();
    history_->SetFront(archive);
    ShowFront(left);
}

void App::EndLastFileWork()
{
    if (transport_.Playing())
    {
        transport_.Stop();
    }
    DropExport();
    DropMixdown();
    replace_.ForgetTargets();
    replace_.Drop();
    reader_.Abandon();
    item_edit_.Drop();
    StopPlayback();
    CancelCatchUp();
    worker_.Stop();
}

void App::ResetBrowser()
{
    ListBrowser();
    ListArchives();
    select_sound_.reset();
    browser_state_.filter.fill('\0');
    browser_state_.open.clear();
    browser_state_.selected = {};
    browser_state_.top = true;
    browser_state_.clicked = false;
    browser_state_.playing.reset();
    shown_group_ = -1;
    shown_item_ = -1;
}

void App::SelectKeptViewSound(const std::optional<format::EditorView>& view)
{
    // The view sets the shown sound, and the view waits for the sound to show.
    pending_view_ = view;
    const std::vector<BrowserItem>& sounds = browser_[kSequencesGroup].items;
    const auto found =
        pending_view_ ? std::ranges::find(sounds, pending_view_->sound, &BrowserItem::number) : sounds.end();
    if (found != sounds.end())
    {
        browser_state_.selected = {static_cast<int>(kSequencesGroup), static_cast<int>(found - sounds.begin())};
        browser_state_.reveal = true;
    }
    else
    {
        pending_view_.reset();
    }
}

void App::Save()
{
    if (document_path_.empty())
    {
        ShowDialog(Dialog::kSaveAs);
    }
    else
    {
        SaveTo(document_path_);
    }
}

void App::SaveTo(const std::string& path)
{
    if (!history_ || preview_)
    {
        return;
    }

    // The file takes an item's edit whose work still runs.
    TakeItemEdit(true);

    // The file's bytes in its form.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::string text;
    std::vector<uint8_t> binary;
    try
    {
        // Each archive keeps its view: the archive in front the view that shows, and another archive the view that it
        // showed last.
        format::Project saved = history_->CurrentProject();
        for (std::size_t a = 0; a < saved.archives.size(); a++)
        {
            saved.archives[a] = edit::FoldTimelines(saved.archives[a]);
            if (a == saved.front)
            {
                saved.archives[a].view = CurrentView();
            }
            else if (a < archive_plays_.size() && archive_plays_[a].view)
            {
                saved.archives[a].view = archive_plays_[a].view;
            }
        }
        if (text_form_)
        {
            text = format::WriteProjectText(saved);
        }
        else
        {
            binary = format::WriteProject(saved);
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't save " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    const std::span<const uint8_t> bytes = text_form_
                                               ? std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size())
                                               : std::span<const uint8_t>(binary);

    // A save that fails partway leaves the last save whole (SaveFile). The document's path stays the one that the user
    // chose, a symbolic link too.
    const SaveResult saved = SaveFile(file, bytes);
    if (saved == SaveResult::kReadOnly)
    {
        ShowMessage("Can't save " + name +
                    ". The file is read-only. File > Save As… saves the changes to another file.");
        return;
    }
    if (saved != SaveResult::kSaved)
    {
        ShowMessage("Can't save " + name + ". The disk may be full, or the file or folder may be read-only.");
        return;
    }

    // The saved file becomes the document's file, and the version that it holds the saved one.
    document_path_ = xsflib::Utf8(file);
    import_path_.clear();
    file_name_ = name;
    history_->MarkSaved();
    UpdateTitle();
    AddRecentProject(document_path_);
}

void App::AfterChanges(std::function<void()> then)
{
    // While the Save As dialog of an earlier request is open, a request does nothing.
    if (after_save_)
    {
        return;
    }

    // An item's edit whose work still runs counts among the changes.
    TakeItemEdit(true);

    if (!history_ || preview_ || !history_->Modified())
    {
        then();
        return;
    }

    // A save that fails shows its message, and the document stays.
    const SaveChoice choice = AskToSave();
    if (choice == SaveChoice::kDontSave)
    {
        then();
    }
    else if (choice == SaveChoice::kSave && document_path_.empty())
    {
        // Where no Save As dialog opens, nothing would ever clear `after_save_`, and every later request would do
        // nothing. The request then ends with the document unsaved.
        if (ShowDialog(Dialog::kSaveAs))
        {
            after_save_ = std::move(then);
        }
    }
    else if (choice == SaveChoice::kSave)
    {
        SaveTo(document_path_);
        if (!history_->Modified())
        {
            then();
        }
    }
}

SaveChoice App::AskToSave()
{
    // The question shows over the window. A minimized window comes back for it, and the waiting messages show first.
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
    {
        SDL_RestoreWindow(window_);
    }
    ShowMessagesBeforeBox();

    // Save answers Return, and Cancel answers Escape. In SDL3's right-to-left order, macOS puts Save on the right,
    // Cancel beside it and Don't Save on the left. The system's dialogs order them that way. Windows reads its buttons
    // the other way: Save on the left, Don't Save beside it and Cancel on the right. Programs on Windows ask in that
    // order. A box that can't be shown counts as Cancel.
#ifdef _WIN32
    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "Cancel"},
                                                {0, 0, "Don't Save"},
                                                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 2, "Save"}};
#else
    const SDL_MessageBoxButtonData buttons[] = {{0, 0, "Don't Save"},
                                                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "Cancel"},
                                                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 2, "Save"}};
#endif
    const std::string message =
        "Do you want to save the changes to " + file_name_ + "? Your changes will be lost if you don't save them.";
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING | SDL_MESSAGEBOX_BUTTONS_RIGHT_TO_LEFT;
    box.window = window_;
    box.title = "Hokuto";
    box.message = message.c_str();
    box.numbuttons = 3;
    box.buttons = buttons;
    int chosen = 1;
    if (!ShowSystemBox(box, &chosen))
    {
        SDL_Log("The question about the changes can't be shown: %s", SDL_GetError());
        return SaveChoice::kCancel;
    }

    if (chosen == 0)
    {
        return SaveChoice::kDontSave;
    }

    return chosen == 2 ? SaveChoice::kSave : SaveChoice::kCancel;
}

} // namespace hokuto::app
