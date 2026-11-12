// SPDX-License-Identifier: MIT

// The writing of an export: the files that the work made, written where the user chose, and the questions before a
// file is replaced.

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "xsflib/ascii.h"
#include "xsflib/files.h"

#include "app/app.h"
#include "app/file_dialogs.h"
#include "format/archive.h"
#include "format/file.h"
#include "measure/measure.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/streams.h"

namespace hokuto::app
{

namespace
{

// Whether `file`, the name `chosen` that a save dialog gave with the extension that WithExtension added, is another
// name, and the folder holds a file of that name. The dialog asked only about the chosen name. On the Mac, the dialog
// adds the filter's extension before it asks. The dialogs of Windows and Linux add none.
bool AddedNameExists(const std::string& chosen, const std::filesystem::path& file)
{
    std::error_code error;
    return xsflib::PathFromUtf8(chosen) != file && std::filesystem::exists(file, error);
}

// The question before an export replaces files that the folder `folder` holds: `existing` names them.
std::string ReplaceQuestion(const std::filesystem::path& folder, const std::vector<std::string>& existing)
{
    const std::string name = xsflib::Utf8(folder.filename().empty() ? folder : folder.filename());
    if (existing.size() == 1)
    {
        return "The folder " + name + " already contains " + existing[0] + ". Replace it?";
    }

    return "The folder " + name + " already contains " + std::to_string(existing.size()) +
           " of these files, including " + existing[0] + ". Replace them?";
}

} // namespace

void App::ExportTo(const std::string& path)
{
    if (!export_ || !export_->done)
    {
        return;
    }

    // The files go where the user chose, with the target's extension. A file that can't be written leaves the window
    // open for another try.
    const ExportWork& work = *export_->done;
    try
    {
        switch (export_->Target())
        {
        case ExportTarget::kSoundArchive:
            if (!WriteArchiveExport(path, work))
            {
                return;
            }
            break;

        // The save dialog asks before it replaces the MIDI file that the user names. The files beside it are asked
        // about here, and so is the MIDI file when the name lacked its extension.
        case ExportTarget::kMidiSoundFont:
        case ExportTarget::kMidiSfz:
            if (!WriteMidiExport(path, work))
            {
                return;
            }
            break;

        // The new file of the other console, in the binary form.
        case ExportTarget::kCtrSong:
        case ExportTarget::kWiiUSong:
        case ExportTarget::kSwitchSong:
        case ExportTarget::kWiiSong:
        case ExportTarget::kDsSong:
            if (!WriteSongExport(path, work))
            {
                return;
            }
            break;

        // An item's file, or a wave's WAV file, takes the extension of its format. A wave archive's WAV files go into
        // the chosen folder under their names.
        case ExportTarget::kItemFile:
        case ExportTarget::kCtrFile:
        case ExportTarget::kWiiUFile:
        case ExportTarget::kSwitchFile:
        case ExportTarget::kWiiFile:
        case ExportTarget::kWav:
        case ExportTarget::kAiff:
            if (!WriteItemExport(path, work))
            {
                return;
            }
            break;

        // A bank's SoundFont goes where the user chose. Instruments past the format's limits go into several SoundFonts
        // beside it instead, each under the chosen name and its programs (song::SoundFontNames).
        case ExportTarget::kSoundFont:
            if (!WriteSoundFontExport(path, work))
            {
                return;
            }
            break;

        // A bank's SFZ files go into a folder with the bank's name in the chosen folder, and their samples beside it.
        case ExportTarget::kSfz:
            if (!WriteSfzExport(path, work))
            {
                return;
            }
            break;
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
        return;
    }

    // A measure that still runs goes on its own and stops early. Its future would wait for it.
    DropExport();
}

bool App::WriteArchiveExport(const std::string& path, const ExportWork& work)
{
    // A game's archive can end in .fsst or .fspj. A name that ends in either keeps it. Another name ends in
    // the extension of the file's archive (Export::extension): .brsar on the Wii, .bcsar on the 3DS,
    // .bfsar on the Wii U and the Switch, .gba for a GBA ROM, and .nds or .sdat on the DS.
    const std::string extension = xsflib::AsciiLower(xsflib::Utf8(xsflib::PathFromUtf8(path).extension()));
    const bool archive = extension == ".fsst" || extension == ".fspj";
    const std::filesystem::path file = xsflib::PathFromUtf8(archive ? path : WithExtension(path, export_->extension));

    // The streams and the group files go beside the archive. A SARC archive beside it, or beside the file
    // that the user opened, takes the group file of a group whose file it holds. A group file whose game's
    // file lies in the userManagementFiles folder there goes into that folder. The program asks before it
    // writes over a file, and over the archive when the name lacked its extension.
    const std::filesystem::path folder = file.parent_path();
    std::vector<format::FileBeside> groups;
    if (!work.group_files.empty())
    {
        std::vector<std::filesystem::path> folders = {folder};
        if (!export_->source_folder.empty())
        {
            folders.push_back(export_->source_folder);
        }
        groups = song::PlaceGroupFiles(
            format::GroupFileOutputs(work.group_files, work.archive, song::FlavorOfArchive(work.archive),
                                     song::SarcArchivesIn(folders)),
            folders);
    }
    const std::array<const std::vector<format::FileBeside>*, 2> lists = {&work.files, &groups};
    std::vector<std::string> existing;
    if (AddedNameExists(path, file))
    {
        existing.push_back(xsflib::Utf8(file.filename()));
    }
    for (const std::vector<format::FileBeside>* list : lists)
    {
        for (const auto& [name, bytes] : *list)
        {
            std::error_code error;
            if (std::filesystem::exists(folder / xsflib::PathFromUtf8(name), error))
            {
                existing.push_back(name);
            }
        }
    }
    if (!existing.empty() && !AskToReplace(ReplaceQuestion(folder, existing)))
    {
        return false;
    }

    if (xsflib::WriteFile(file, work.archive))
    {
        throw std::runtime_error(song::WriteFailure(xsflib::Utf8(file.filename())));
    }

    for (const std::vector<format::FileBeside>* list : lists)
    {
        for (const auto& [name, bytes] : *list)
        {
            if (const std::optional<std::string> error = song::WriteStream(folder / xsflib::PathFromUtf8(name), bytes))
            {
                throw std::runtime_error(*error);
            }
        }
    }

    return true;
}

bool App::WriteMidiExport(const std::string& path, const ExportWork& work)
{
    const std::filesystem::path midi = xsflib::PathFromUtf8(WithExtension(path, ".mid"));
    const bool sound_font = export_->Target() == ExportTarget::kMidiSoundFont;
    std::vector<std::filesystem::path> files;
    if (xsflib::PathFromUtf8(path) != midi)
    {
        files.push_back(midi);
    }
    if (sound_font)
    {
        files.push_back(std::filesystem::path(midi).replace_extension(".sf2"));
    }
    else
    {
        // A sample of another export stops the export before the question.
        song::CheckSfzSamples(*work.conversion, midi);
        for (const ntdlib::sequence_export::SfzFile& file : work.conversion->sfz.files)
        {
            files.push_back(midi.parent_path() / "sfz" / midi.stem() / xsflib::PathFromUtf8(file.name));
        }
    }
    std::vector<std::string> existing;
    for (const std::filesystem::path& file : files)
    {
        std::error_code error;
        if (std::filesystem::exists(file, error))
        {
            existing.push_back(xsflib::Utf8(file.lexically_relative(midi.parent_path())));
        }
    }
    if (!existing.empty() && !AskToReplace(ReplaceQuestion(midi.parent_path(), existing)))
    {
        return false;
    }

    if (sound_font)
    {
        song::WriteMidiAndSoundFont(*work.conversion, midi);
    }
    else
    {
        song::WriteMidiAndSfz(*work.conversion, midi);
    }

    return true;
}

bool App::WriteSongExport(const std::string& path, const ExportWork& work)
{
    const std::filesystem::path file = xsflib::PathFromUtf8(WithExtension(path, ".hok"));
    if (!MayWriteTo(path, file))
    {
        return false;
    }

    if (xsflib::WriteFile(file, work.files.at(0).second))
    {
        throw std::runtime_error(song::WriteFailure(xsflib::Utf8(file.filename())));
    }

    return true;
}

bool App::WriteItemExport(const std::string& path, const ExportWork& work)
{
    if (export_->item->kind == song::ItemKind::kWaveArchive && export_->Target() == ExportTarget::kWav)
    {
        const std::vector<std::pair<std::string, std::span<const uint8_t>>> files(work.files.begin(), work.files.end());
        if (!WriteIntoFolder(xsflib::PathFromUtf8(path), files))
        {
            return false;
        }
    }
    else
    {
        const std::string extension = xsflib::Utf8(xsflib::PathFromUtf8(work.files.at(0).first).extension());
        const std::filesystem::path file = xsflib::PathFromUtf8(WithExtension(path, extension));

        // The files that go beside it take its name, with their extensions: the waves of a bank or a wave sound
        // file.
        std::vector<std::pair<std::string, std::span<const uint8_t>>> files = {
            {xsflib::Utf8(file.filename()), work.files[0].second}};
        for (std::size_t i = 1; i < work.files.size(); i++)
        {
            std::filesystem::path name = file.filename();
            name.replace_extension(xsflib::PathFromUtf8(work.files[i].first).extension());
            files.emplace_back(xsflib::Utf8(name), work.files[i].second);
        }

        // One question names every file that the export replaces, before it writes any. The save dialog asked
        // about the file itself, unless the name lacked its extension.
        if (!WriteIntoFolder(file.parent_path(), files, AddedNameExists(path, file) ? 0 : 1))
        {
            return false;
        }
    }

    return true;
}

bool App::WriteSoundFontExport(const std::string& path, const ExportWork& work)
{
    const std::filesystem::path file = xsflib::PathFromUtf8(WithExtension(path, ".sf2"));
    const std::vector<std::string> names = song::SoundFontNames(*work.bank, xsflib::Utf8(file.stem()));
    if (names.size() == 1)
    {
        if (!MayWriteTo(path, file))
        {
            return false;
        }

        if (xsflib::WriteFile(file, work.bank->sound_fonts[0].sf2))
        {
            throw std::runtime_error(song::WriteFailure(xsflib::Utf8(file.filename())));
        }
        return true;
    }

    std::vector<std::pair<std::string, std::span<const uint8_t>>> files;
    for (std::size_t i = 0; i < names.size(); i++)
    {
        files.emplace_back(names[i], work.bank->sound_fonts[i].sf2);
    }
    if (!WriteIntoFolder(file.parent_path(), files))
    {
        return false;
    }

    return true;
}

bool App::WriteSfzExport(const std::string& path, const ExportWork& work)
{
    const std::filesystem::path folder = xsflib::PathFromUtf8(path);
    const auto replace = [this, &folder](const std::vector<std::string>& existing)
    {
        return AskToReplace(ReplaceQuestion(folder / xsflib::PathFromUtf8(export_->file_name), existing));
    };
    if (!song::WriteBankSfz(*work.bank, folder, export_->file_name, replace))
    {
        return false;
    }

    return true;
}

bool App::WriteIntoFolder(const std::filesystem::path& folder,
                          const std::vector<std::pair<std::string, std::span<const uint8_t>>>& files, std::size_t asked)
{
    // The files that the folder holds already, by their names.
    std::vector<std::string> existing;
    for (std::size_t i = asked; i < files.size(); i++)
    {
        const std::string& name = files[i].first;
        std::error_code error;
        if (std::filesystem::exists(folder / xsflib::PathFromUtf8(name), error))
        {
            existing.push_back(name);
        }
    }
    if (!existing.empty() && !AskToReplace(ReplaceQuestion(folder, existing)))
    {
        return false;
    }

    for (const auto& [name, bytes] : files)
    {
        const std::filesystem::path file = folder / xsflib::PathFromUtf8(name);
        if (xsflib::WriteFile(file, bytes))
        {
            throw std::runtime_error(song::WriteFailure(name));
        }
    }

    return true;
}

bool App::AskToReplace(const std::string& message)
{
    ShowMessagesBeforeBox();

    // Cancel answers Escape, and Replace answers Return. In SDL3's right-to-left order, Replace comes on the right on
    // macOS and on the left on Windows. Each system places a dialog's main button there.
    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
                                                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Replace"}};
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING | SDL_MESSAGEBOX_BUTTONS_RIGHT_TO_LEFT;
    box.window = window_;
    box.title = "Hokuto";
    box.message = message.c_str();
    box.numbuttons = 2;
    box.buttons = buttons;
    int chosen = 0;
    return SDL_ShowMessageBox(&box, &chosen) && chosen == 1;
}

bool App::MayWriteTo(const std::string& chosen, const std::filesystem::path& file)
{
    return !AddedNameExists(chosen, file) ||
           AskToReplace(ReplaceQuestion(file.parent_path(), {xsflib::Utf8(file.filename())}));
}

} // namespace hokuto::app
