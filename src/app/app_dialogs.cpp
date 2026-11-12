// SPDX-License-Identifier: MIT

// The file dialogs: the filters that each dialog shows, where it opens, and the files that the user chooses.

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <span>

#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "app/engine_rows.h"
#include "app/system_ui.h"
#include "format/file.h"

namespace hokuto::app
{

namespace
{

// The sound archives that File > Import > Sound Archive shows: the Switch's and the Wii U's, the 3DS's, the Wii's and
// the DS's, the GBA's ROMs and the DS's cartridge images. Nd Cube's games name theirs `.fsst` (sub-archives) and
// `.fspj` (the resident one).
constexpr SDL_DialogFileFilter kArchiveFilters[] = {
    {"Sound archives, GBA ROMs, DS images and MIDI files", "bfsar;bcsar;brsar;fsst;fspj;sdat;gba;nds;mid;midi"}};

// The sound archive that File > Export writes, by its console: the Switch's and the Wii U's, the 3DS's, the Wii's and
// the DS's, the GBA's ROM and the DS's cartridge image. On the Mac, a name typed without an extension takes the
// filter's first. The other consoles' extensions stay out of the filter: the export would add the console's extension
// after one of them.
constexpr SDL_DialogFileFilter kLaterArchiveFilters[] = {{"Sound archives", "bfsar;fsst;fspj"}};
constexpr SDL_DialogFileFilter k3dsArchiveFilters[] = {{"Sound archives", "bcsar"}};
constexpr SDL_DialogFileFilter kWiiArchiveFilters[] = {{"Sound archives", "brsar"}};
constexpr SDL_DialogFileFilter kGbaRomFilters[] = {{"GBA ROMs", "gba"}};
constexpr SDL_DialogFileFilter kDsArchiveFilters[] = {{"Sound archives", "sdat"}};
constexpr SDL_DialogFileFilter kDsImageFilters[] = {{"DS images", "nds"}};

// The filters of the dialog that saves a file's sound archive or ROM, by the row of the file's engine. A file of the
// DS saves a DS image where the file keeps the image's cartridge (`image`), and a DS sound archive otherwise.
std::span<const SDL_DialogFileFilter> ArchiveDialogFilters(ArchiveFilters filters, bool image)
{
    switch (filters)
    {
    case ArchiveFilters::kLaterArchive:
        return kLaterArchiveFilters;
    case ArchiveFilters::k3dsArchive:
        return k3dsArchiveFilters;
    case ArchiveFilters::kWiiArchive:
        return kWiiArchiveFilters;
    case ArchiveFilters::kGbaRom:
        return kGbaRomFilters;
    case ArchiveFilters::kDs:
        return image ? std::span(kDsImageFilters) : std::span(kDsArchiveFilters);
    case ArchiveFilters::kDsImage:
        break;
    }

    return kDsImageFilters;
}

// The file of the DSi's digest key that Studio Setup chooses: its 64 bytes, under any name.
constexpr SDL_DialogFileFilter kDsiKeyFilters[] = {{"Key files", "bin"}, {"All files", "*"}};

// The Hokuto files that File > Open and File > Save As show.
constexpr SDL_DialogFileFilter kHokutoFilters[] = {{"Hokuto files", "hok"}};

// The MIDI files that File > Export writes, with their instruments beside them. A sound archive takes the filters of
// File > Import.
constexpr SDL_DialogFileFilter kMidiFilters[] = {{"MIDI files", "mid"}};

// The files of the browser's Export…, one filter for each extension: an item's file of the Wii, the 3DS, the Switch,
// the Wii U, the DS or DSE, a GBA sample, voice group or song, an AAC stream, a WAV file, a bank's SoundFont and a
// sequence's MIDI file. The dialog shows the filter of the export's file.
constexpr SDL_DialogFileFilter kItemFilters[] = {
    {"Sample files", "bin"},       {"AIFF files", "aif"},      {"Sequence files", "bfseq"},
    {"Wave sound files", "bfwsd"}, {"Bank files", "bfbnk"},    {"Wave archive files", "bfwar"},
    {"Wave files", "bfwav"},       {"Stream files", "bfstm"},  {"Sequence files", "bcseq"},
    {"Wave sound files", "bcwsd"}, {"Bank files", "bcbnk"},    {"Wave archive files", "bcwar"},
    {"Wave files", "bcwav"},       {"Stream files", "bcstm"},  {"Sequence files", "brseq"},
    {"Wave sound files", "brwsd"}, {"Bank files", "brbnk"},    {"Wave archive files", "brwar"},
    {"Wave files", "brwav"},       {"Stream files", "brstm"},  {"AAC files", "aac"},
    {"WAV files", "wav"},          {"SoundFont files", "sf2"}, {"MIDI files", "mid"},
    {"Assembly files", "s"},       {"Sequence files", "sseq"}, {"Wave archive files", "swar"},
    {"Bank files", "sbnk"},        {"Wave files", "swav"},     {"Stream files", "strm"},
    {"Bank files", "swd"},         {"Song files", "smd"}};

// The files that File > Import and the browser's Replace… show for a file of the 3DS, the Wii U or the Switch: sequence
// files, bank files, wave archive files, wave sound files and wave files of the three. A file of another of them goes
// in converted.
constexpr SDL_DialogFileFilter kSequenceFilters[] = {{"Sequence files", "bcseq;bfseq"}, {"MIDI files", "mid;midi"}};
constexpr SDL_DialogFileFilter kBankFilters[] = {{"Bank files", "bcbnk;bfbnk"}};
constexpr SDL_DialogFileFilter kBankImportFilters[] = {
    {"Bank files", "bcbnk;bfbnk"}, {"SoundFont 2 files", "sf2"}, {"SFZ files", "sfz"}};
constexpr SDL_DialogFileFilter kWaveArchiveFilters[] = {{"Wave archive files", "bcwar;bfwar"}};
constexpr SDL_DialogFileFilter kWaveSoundFileFilters[] = {{"Wave sound files", "bcwsd;bfwsd"}};
constexpr SDL_DialogFileFilter kWaveFilters[] = {{"Wave files", "bcwav;bfwav"}};
constexpr SDL_DialogFileFilter kWavFilters[] = {{"WAV files", "wav"}};
constexpr SDL_DialogFileFilter kWaveReplaceFilters[] = {{"WAV files and wave files", "wav;bcwav;bfwav"}};
constexpr SDL_DialogFileFilter kStreamFilters[] = {{"Stream files and WAV files", "bfstm;bcstm;wav"}};
constexpr SDL_DialogFileFilter kAacStreamFilters[] = {
    {"AAC files, WAV files and stream files", "aac;wav;bcstm;bfstm;brstm"}};

// The files that File > Import > Sequence… shows for a file of the GBA: MIDI files and songs as assembly.
constexpr SDL_DialogFileFilter kMidiImportFilters[] = {{"MIDI files", "mid;midi"}, {"Assembly files", "s"}};

// The files that the browser's Replace… shows for a GBA voice group: voice groups as assembly.
constexpr SDL_DialogFileFilter kGbaVoiceGroupFilters[] = {{"Assembly files", "s"}};

// The files that File > Import > Bank… shows for a GBA ROM: the presets of a SoundFont or of SFZ files go in as voices.
constexpr SDL_DialogFileFilter kGbaBankImportFilters[] = {{"SoundFont 2 files", "sf2"}, {"SFZ files", "sfz"}};

// The files that a stream sound of the Wii takes: stream files of the Wii and of the later consoles, and WAV files.
constexpr SDL_DialogFileFilter kWiiStreamFilters[] = {{"Stream files and WAV files", "brstm;bfstm;bcstm;wav"}};

// The files that File > Import and the browser's Replace… show for a file of the DS: sequence files, bank files, wave
// archive files, wave files and stream files of the DS, MIDI files, SoundFonts, SFZ files and WAV files.
constexpr SDL_DialogFileFilter kDsSequenceFilters[] = {{"Sequence files", "sseq"}, {"MIDI files", "mid;midi"}};
constexpr SDL_DialogFileFilter kDsBankFilters[] = {{"Bank files", "sbnk"}};
constexpr SDL_DialogFileFilter kDsBankImportFilters[] = {
    {"Bank files", "sbnk"}, {"SoundFont 2 files", "sf2"}, {"SFZ files", "sfz"}};
constexpr SDL_DialogFileFilter kDsWaveArchiveFilters[] = {{"Wave archive files", "swar"}};
constexpr SDL_DialogFileFilter kDsWaveReplaceFilters[] = {{"WAV files and wave files", "wav;swav"}};
constexpr SDL_DialogFileFilter kDsStreamFilters[] = {{"Stream files and WAV files", "strm;wav"}};
constexpr SDL_DialogFileFilter kDsWaveFilters[] = {{"Wave files", "swav"}};

// The files that the browser's Replace… shows for DSE's sounds and banks: song files, MIDI files and wave bank files. A
// wave is replaced with a WAV file.
constexpr SDL_DialogFileFilter kDseSoundFilters[] = {{"Song files", "smd"}, {"MIDI files", "mid;midi"}};
constexpr SDL_DialogFileFilter kDseBankFilters[] = {{"Bank files", "swd"}};

// The files that File > Import and the browser's Replace… show for a file of the Wii: sequence files, bank files, wave
// archive files, wave sound files and wave files of the Wii, and those of the 3DS, the Wii U and the Switch. The later
// consoles' files go in converted.
constexpr SDL_DialogFileFilter kWiiSequenceFilters[] = {{"Sequence files", "brseq;bcseq;bfseq"},
                                                        {"MIDI files", "mid;midi"}};
constexpr SDL_DialogFileFilter kWiiBankFilters[] = {{"Bank files", "brbnk;bcbnk;bfbnk"}};
constexpr SDL_DialogFileFilter kWiiBankImportFilters[] = {
    {"Bank files", "brbnk;bcbnk;bfbnk"}, {"SoundFont 2 files", "sf2"}, {"SFZ files", "sfz"}};
constexpr SDL_DialogFileFilter kWiiWaveArchiveFilters[] = {{"Wave archive files", "brwar;bcwar;bfwar"}};
constexpr SDL_DialogFileFilter kWiiWaveSoundFileFilters[] = {{"Wave sound files", "brwsd;bcwsd;bfwsd"}};
constexpr SDL_DialogFileFilter kWiiWaveFilters[] = {{"Wave files", "brwav;bcwav;bfwav"}};
constexpr SDL_DialogFileFilter kWiiWaveReplaceFilters[] = {{"WAV files and wave files", "wav;brwav;bcwav;bfwav"}};

} // namespace

std::span<const SDL_DialogFileFilter> App::DialogFilters(Dialog dialog) const
{
    constexpr std::array kImports = {Dialog::kImport,
                                     Dialog::kImportSequence,
                                     Dialog::kImportBank,
                                     Dialog::kImportWaveArchive,
                                     Dialog::kImportWaveSoundFile,
                                     Dialog::kImportWave,
                                     Dialog::kImportAudioFile,
                                     Dialog::kImportStream};
    std::span<const SDL_DialogFileFilter> filters = std::span(kHokutoFilters);
    if (std::ranges::find(kImports, dialog) != kImports.end())
    {
        filters = ImportDialogFilters(dialog);
    }
    else if (dialog == Dialog::kReplace)
    {
        filters = ReplaceDialogFilters();
    }
    else if (dialog == Dialog::kExport && export_ && export_->item)
    {
        // The filter of the item's file, by its extension.
        const std::string extension = xsflib::Utf8(xsflib::PathFromUtf8(export_->file_name).extension());
        filters = {};
        for (const SDL_DialogFileFilter& filter : kItemFilters)
        {
            if (extension == std::string(".") + filter.pattern)
            {
                filters = std::span(&filter, 1);
            }
        }
    }
    else if (dialog == Dialog::kExport && export_ && export_->Target() == ExportTarget::kSoundArchive)
    {
        const format::File* document = Document();
        const format::Engine console = document != nullptr ? document->engine : format::Engine::kSwitch;
        const bool image = document != nullptr && document->native && document->native->cartridge;
        filters = ArchiveDialogFilters(RowOf(console).filters, image);
    }
    else if (dialog == Dialog::kExport && export_)
    {
        filters = SongEngineOf(export_->Target()) ? std::span(kHokutoFilters) : std::span(kMidiFilters);
    }
    else if (dialog == Dialog::kExportFolder)
    {
        // A panel of folders takes no filter. On the Mac, a filter of files leaves the panel no folder to choose.
        filters = {};
    }
    else if (dialog == Dialog::kDsiKey)
    {
        filters = std::span(kDsiKeyFilters);
    }

    return filters;
}

std::span<const SDL_DialogFileFilter> App::ImportDialogFilters(Dialog dialog) const
{
    std::span<const SDL_DialogFileFilter> filters = std::span(kHokutoFilters);
    if (dialog == Dialog::kImport)
    {
        filters = std::span(kArchiveFilters);
    }
    else if (dialog == Dialog::kImportSequence)
    {
        filters = played_.Gba()   ? std::span<const SDL_DialogFileFilter>(kMidiImportFilters)
                  : played_.Wii() ? std::span(kWiiSequenceFilters)
                  : played_.Ds()  ? std::span(kDsSequenceFilters)
                                  : std::span(kSequenceFilters);
    }
    else if (dialog == Dialog::kImportBank)
    {
        filters = played_.Gba()   ? std::span<const SDL_DialogFileFilter>(kGbaBankImportFilters)
                  : played_.Wii() ? std::span<const SDL_DialogFileFilter>(kWiiBankImportFilters)
                  : played_.Ds()  ? std::span<const SDL_DialogFileFilter>(kDsBankImportFilters)
                                  : std::span<const SDL_DialogFileFilter>(kBankImportFilters);
    }
    else if (dialog == Dialog::kImportWaveArchive)
    {
        filters = played_.Wii()  ? std::span(kWiiWaveArchiveFilters)
                  : played_.Ds() ? std::span(kDsWaveArchiveFilters)
                                 : std::span(kWaveArchiveFilters);
    }
    else if (dialog == Dialog::kImportWaveSoundFile)
    {
        filters = played_.Wii() ? std::span(kWiiWaveSoundFileFilters) : std::span(kWaveSoundFileFilters);
    }
    else if (dialog == Dialog::kImportWave)
    {
        filters = played_.Wii()  ? std::span(kWiiWaveFilters)
                  : played_.Ds() ? std::span(kDsWaveFilters)
                                 : std::span(kWaveFilters);
    }
    else if (dialog == Dialog::kImportAudioFile)
    {
        filters = std::span(kWavFilters);
    }
    else if (dialog == Dialog::kImportStream)
    {
        filters = played_.Ds()    ? std::span(kDsStreamFilters)
                  : played_.Wii() ? std::span(kWiiStreamFilters)
                                  : std::span(kStreamFilters);
    }

    return filters;
}

std::span<const SDL_DialogFileFilter> App::ReplaceDialogFilters() const
{
    std::span<const SDL_DialogFileFilter> filters = std::span(kHokutoFilters);
    if (played_.Driver())
    {
        filters = std::span(kWavFilters);
    }
    else if (played_.Gba())
    {
        filters = replace_.file ? std::span<const SDL_DialogFileFilter>(kGbaVoiceGroupFilters)
                                : std::span<const SDL_DialogFileFilter>(kMidiImportFilters);
    }
    else if (played_.Dse())
    {
        filters = replace_.wave    ? std::span<const SDL_DialogFileFilter>(kWavFilters)
                  : !replace_.file ? std::span<const SDL_DialogFileFilter>(kDseSoundFilters)
                                   : std::span<const SDL_DialogFileFilter>(kDseBankFilters);
    }
    else if (played_.Ds() && replace_.stream)
    {
        filters = std::span(kDsStreamFilters);
    }
    else if (played_.Ds())
    {
        filters = replace_.wave    ? std::span<const SDL_DialogFileFilter>(kDsWaveReplaceFilters)
                  : !replace_.file ? std::span<const SDL_DialogFileFilter>(kDsSequenceFilters)
                  : replace_.file->kind == edit::FileKind::kBank ? std::span(kDsBankFilters)
                                                                 : std::span(kDsWaveArchiveFilters);
    }
    else if (replace_.file && played_.Wii())
    {
        filters = replace_.file->kind == edit::FileKind::kBank          ? std::span(kWiiBankFilters)
                  : replace_.file->kind == edit::FileKind::kWaveArchive ? std::span(kWiiWaveArchiveFilters)
                                                                        : std::span(kWiiWaveSoundFileFilters);
    }
    else if (replace_.sound && played_.Wii())
    {
        filters = std::span(kWiiSequenceFilters);
    }
    else if (replace_.wave && played_.Wii())
    {
        filters = std::span(kWiiWaveReplaceFilters);
    }
    else if (replace_.file)
    {
        filters = replace_.file->kind == edit::FileKind::kBank          ? std::span(kBankFilters)
                  : replace_.file->kind == edit::FileKind::kWaveArchive ? std::span(kWaveArchiveFilters)
                                                                        : std::span(kWaveSoundFileFilters);
    }
    else if (replace_.wave)
    {
        filters = std::span(kWaveReplaceFilters);
    }
    else if (replace_.stream && played_.Wii())
    {
        filters = std::span(kWiiStreamFilters);
    }
    else if (replace_.stream && PlaysAacStream(*replace_.stream))
    {
        filters = std::span(kAacStreamFilters);
    }
    else if (replace_.stream)
    {
        filters = std::span(kStreamFilters);
    }
    else
    {
        filters = std::span(kSequenceFilters);
    }

    return filters;
}

std::filesystem::path App::SaveAsLocation() const
{
    std::filesystem::path location = xsflib::PathFromUtf8(document_path_);
    if (location.empty() && !import_path_.empty())
    {
        location = xsflib::PathFromUtf8(import_path_).replace_extension(".hok");
    }

    return location;
}

std::optional<std::filesystem::path> App::ExportLocation(bool folder) const
{
    const std::string& source = import_path_.empty() ? document_path_ : import_path_;
    if (!export_ || source.empty())
    {
        return std::nullopt;
    }

    const std::filesystem::path beside = xsflib::PathFromUtf8(source).parent_path();

    return folder ? beside / "" : beside / xsflib::PathFromUtf8(export_->file_name);
}

void App::ShowDialog(Dialog dialog)
{
    ShowMessagesBeforeBox();

    // The dialog reads the properties when it opens. The filters have to last until it closes. SDL3 only reads them.
    const std::span<const SDL_DialogFileFilter> filters = DialogFilters(dialog);
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
                           const_cast<SDL_DialogFileFilter*>(filters.data()));
    SDL_SetNumberProperty(properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, static_cast<Sint64>(filters.size()));
    SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window_);

    SDL_FileDialogType type = SDL_FILEDIALOG_OPENFILE;
    SDL_DialogFileCallback callback = &App::Chosen<Dialog::kOpen>;
    switch (dialog)
    {
    case Dialog::kOpen:
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Open");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Open");
        break;

    // A file that hasn't been saved starts beside the sound archive that it came from, with the archive's name.
    case Dialog::kSaveAs:
        {
            type = SDL_FILEDIALOG_SAVEFILE;
            callback = &App::Chosen<Dialog::kSaveAs>;
            SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Save As");
            SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Save");
            const std::filesystem::path location = SaveAsLocation();
            if (!location.empty())
            {
                SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING, xsflib::Utf8(location).c_str());
            }
            break;
        }

    case Dialog::kImport:
        callback = &App::Chosen<Dialog::kImport>;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Sound Archive");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Import");
        break;

    case Dialog::kImportSequence:
        callback = &App::Chosen<Dialog::kImportSequence>;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Sequence");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportBank:
        callback = &App::Chosen<Dialog::kImportBank>;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Bank");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportWaveArchive:
        callback = &App::Chosen<Dialog::kImportWaveArchive>;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Wave Archive");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportWave:
        callback = &App::Chosen<Dialog::kImportWave>;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Wave");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportWaveSoundFile:
        callback = &App::Chosen<Dialog::kImportWaveSoundFile>;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Wave Sound File");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportAudioFile:
        callback = &App::Chosen<Dialog::kImportAudioFile>;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Audio File");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportStream:
        callback = &App::Chosen<Dialog::kImportStream>;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Stream");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kDsiKey:
        callback = &App::Chosen<Dialog::kDsiKey>;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Choose DSi Digest Key");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    // On the GBA, the Banks group lists voice groups. The Undo History names the action Replace Voice Group.
    case Dialog::kReplace:
        callback = &App::Chosen<Dialog::kReplace>;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                              replace_.file && played_.Gba() ? "Replace Voice Group"
                              : replace_.file                ? ReplaceAction(replace_.file->kind).c_str()
                              : replace_.wave                ? "Replace Wave"
                              : replace_.stream              ? "Replace Stream"
                                                             : "Replace Sequence");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    // An export starts beside the sound archive or the file that the document came from, with the export's name. A wave
    // archive's WAV files and a bank's SFZ files go into a folder. The dialog then starts in the folder of the archive
    // or the file. That location ends in a separator: SDL3 reads a location without one as a file's name, and the
    // dialog then starts in the folder above.
    case Dialog::kExport:
    case Dialog::kExportFolder:
        {
            const bool folder = dialog == Dialog::kExportFolder;
            const char* title = "Export";
            if (folder)
            {
                title = export_ && export_->Target() == ExportTarget::kSfz ? "Export SFZ Files" : "Export WAV Files";
            }
            type = folder ? SDL_FILEDIALOG_OPENFOLDER : SDL_FILEDIALOG_SAVEFILE;
            callback = folder ? &App::Chosen<Dialog::kExportFolder> : &App::Chosen<Dialog::kExport>;
            SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, title);
            SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Export");
            if (const std::optional<std::filesystem::path> location = ExportLocation(folder))
            {
                SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING,
                                      xsflib::Utf8(*location).c_str());
            }
            break;
        }
    }

    SDL_ShowFileDialogWithProperties(type, callback, this, properties);
    SDL_DestroyProperties(properties);
}

template <Dialog kDialog>
void SDLCALL App::Chosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(kDialog, files);
}

void App::Choose(Dialog dialog, const char* const* files)
{
    // No list means an error, and an empty list means the user cancelled. A Save As without a file still reaches the
    // next frame. What waited for the save then goes (AfterChanges).
    const bool chose = files != nullptr && files[0] != nullptr;
    if (!chose && dialog != Dialog::kSaveAs)
    {
        return;
    }

    // The wake makes the program act on the file in its next frame. The path takes the one form that the program keeps:
    // on Windows, the open dialog and the save dialog can name a long path in two forms.
    {
        std::lock_guard lock(chosen_mutex_);
        chosen_.emplace(dialog, chose ? SystemPath(files[0]) : "");
    }
    Wake();
}

} // namespace hokuto::app
