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
#include "app/file_dialogs.h"
#include "app/recent_projects.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "edit/ds_files.h"
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
// the one that ImGui::BeginPopupModal returns.
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

// The sequence of `document` that each song of `rom`'s song table is, by the song's number (format::GbaSongSequence),
// or nothing for a song that the cut leaves among the ROM's bytes.
std::vector<std::optional<uint32_t>> GbaSongSequences(const format::File& document, const song::GbaRom& rom)
{
    std::vector<std::optional<uint32_t>> sequences;
    for (int song = 0; song < rom.info.song_count; song++)
    {
        sequences.push_back(format::GbaSongSequence(document, rom.info.song_table, song));
    }

    return sequences;
}

// The sequence of `document` that each sound of `rom`, a ROM with another of the GBA's drivers, plays
// (song::DriverRom::Sounds), or nothing for a song that the cut leaves among the ROM's bytes. A sound of Konami's
// driver that plays a song's live version plays the song's sequence.
std::vector<std::optional<uint32_t>> DriverSongSequences(const format::File& document, const song::DriverRom& rom)
{
    std::vector<std::optional<uint32_t>> sequences(rom.SoundCount());
    const std::vector<uint64_t> starts = format::NativePieceStarts(document);
    for (const uint32_t sound : rom.Sounds())
    {
        const std::optional<uint64_t> place = rom.FieldsOf(sound).sequence;
        sequences[sound] = place ? format::SequenceNamedAt(document, *place, starts) : std::nullopt;
    }

    return sequences;
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

OpenWork App::OpenedFile(const std::string& name, std::span<const uint8_t> bytes)
{
    // The file with its timelines laid out for editing, and the archive that it builds for playback, with the file's
    // group files. The export throws for a file without a native archive. A file of the Wii builds its archive for the
    // Wii's model, and has no group files. A file of the GBA builds its ROM, with filler runs for the songs that don't
    // fit after its data (format::PlayedBytes). A file of the DS builds its archive, and plays by the driver of the
    // cartridge that it keeps. MP2K's songs and NITRO-Composer's sequences have no timelines.
    OpenWork work;
    work.text_form = format::IsText(bytes);
    try
    {
        format::File read = format::ReadEitherForm(bytes);
        const bool gba = read.native && read.native->engine == format::Engine::kGba;
        const bool ds = read.native && read.native->engine == format::Engine::kDs;
        const bool dse = read.native && read.native->engine == format::Engine::kDse;
        const bool driver = read.native && read.native->driver;
        work.document = std::make_shared<const format::File>(
            gba || ds || dse || driver ? std::move(read) : edit::OpenTimelines(std::move(read)));
        const std::vector<uint8_t> built = format::PlayedBytes(*work.document);
        if (gba)
        {
            work.played = song::LoadGbaRom(built);
            work.sequences = GbaSongSequences(*work.document, *work.played.Gba());
        }
        else if (ds)
        {
            work.played = song::LoadDsArchive(built, DsBuildOfFile(*work.document), edit::DsGameOfFile(*work.document));
            work.sequences = format::DsSoundSequences(*work.document, built);
        }
        else if (dse)
        {
            work.played = song::LoadDseSet(built, DseBuildOfFile(*work.document));
            work.sequences = format::DseSoundSequences(*work.document, built, song::DseSoundRefs(*work.played.Dse()));
        }
        else if (driver)
        {
            work.played = song::LoadDriverRom(work.document->native->engine, built);
            work.sequences = DriverSongSequences(*work.document, *work.played.Driver());
        }
        else if (work.document->native && work.document->native->engine == format::Engine::kWii)
        {
            work.played = song::LoadWiiArchive(built, name, ntdlib::rvl::snd::kRsarOptions);
            work.sequences = format::SequencesOfFiles(*work.document, built, *work.played.Wii());
            work.start_fields = StartFieldsOf(*work.document, built);
        }
        else
        {
            work.played = song::LoadArchive(built, name, format::ExportGroupFiles(*work.document));
            work.sequences = format::SequencesOfFiles(*work.document, built, *work.played.Sar());
            work.start_fields = StartFieldsOf(*work.document, built);
        }
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
    // The new file, and the ROM for playback. The file's export reproduces the ROM, byte for byte.
    const std::string name = xsflib::Utf8(file.filename());
    OpenWork work;
    work.imported = true;
    try
    {
        // A ROM with another of the GBA's drivers is cut by its driver. The file's export reproduces it, byte for
        // byte, too.
        if (const format::Driver* driver = format::DriverOfRom(bytes))
        {
            work.document = std::make_shared<const format::File>(driver->Import(bytes));
            work.played = song::LoadDriverRom(driver->Facts().engine, std::move(bytes));
            work.sequences = DriverSongSequences(*work.document, *work.played.Driver());
            return work;
        }

        work.document = std::make_shared<const format::File>(format::ImportGbaRom(bytes));
        work.played = song::LoadGbaRom(std::move(bytes));
        work.sequences = GbaSongSequences(*work.document, *work.played.Gba());
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
            work.played = song::LoadDseSet(set, DseBuildOfFile(*work.document));
            work.sequences = format::DseSoundSequences(*work.document, set, song::DseSoundRefs(*work.played.Dse()));
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
                 std::move(work.start_fields), name);
    document_path_ = work.imported ? std::string() : path;
    import_path_ = work.imported ? path : std::string();
    text_form_ = work.text_form;
    if (!work.imported)
    {
        AddRecentProject(path);
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
        if (!preparing_ &&
            (!chosen_ || std::ranges::find(kImports, chosen_->first) == kImports.end() || reading_version_ == nullptr))
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
    if (preparing_ && preparing_frames_ >= kPreparingFrames && reading_version_ == nullptr)
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
                       std::optional<std::vector<std::optional<uint32_t>>> start_fields, const std::string& name)
{
    // The file replaces the one that was open before. The zones show no sequence until the user chooses one, and the
    // transport runs over the empty song. A preview from the last archive stops too, and so does the made-up song. An
    // export or a replacement in the last file ends. The editors of the last file's items close.
    EndLastFileWork();
    history_.emplace(std::move(document));
    history_rows_.clear();
    action_selections_.clear();
    copied_track_.reset();
    preview_ = false;
    played_ = std::move(played);
    archive_version_ = history_->Current();
    reading_version_.reset();
    file_sequences_ = std::move(sequences);
    start_fields_ = std::move(start_fields);
    start_values_.clear();
    told_missing_banks_.clear();
    told_unrelocatable_.clear();
    changes_.clear();
    sequence_.reset();
    model_.reset();
    song_.reset();
    ForgetEngine();
    engine_edit_.reset();
    gba_moved_groups_.clear();

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
    CloseBankEditor();
    bank_to_open_.reset();
    bank_drops_.clear();
    bank_drop_.reset();
    CloseProperties();
    file_name_ = name;
    UpdateTitle();
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

    SelectKeptViewSound();
}

void App::EndLastFileWork()
{
    if (transport_.Playing())
    {
        transport_.Stop();
    }
    DropExport();
    replace_.ForgetTargets();
    replace_.Drop();
    reader_.Abandon();
    StopPlayback();
    CancelCatchUp();
    worker_.Stop();
}

void App::ResetBrowser()
{
    ListBrowser();
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

void App::SelectKeptViewSound()
{
    // The view that a file keeps sets the shown sound, and the view waits for the sound to show.
    pending_view_ = history_->Current()->view;
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

    // The file's bytes in its form.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::string text;
    std::vector<uint8_t> binary;
    try
    {
        format::File saved = edit::FoldTimelines(*Document());
        saved.view = CurrentView();
        if (text_form_)
        {
            text = format::WriteText(saved);
        }
        else
        {
            binary = format::Write(saved);
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

    // The bytes go to a file beside the target first and then replace it. A save that fails partway leaves the last
    // save whole.
    std::filesystem::path saving = file;
    saving += ".saving";
    const std::optional<std::string> write_error = xsflib::WriteFile(saving, bytes);
    std::error_code moved;
    if (!write_error)
    {
        std::filesystem::rename(saving, file, moved);
    }
    if (write_error || moved)
    {
        std::error_code removed;
        std::filesystem::remove(saving, removed);
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
        after_save_ = std::move(then);
        ShowDialog(Dialog::kSaveAs);
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
    if (!SDL_ShowMessageBox(&box, &chosen))
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
