// SPDX-License-Identifier: MIT

// File > Import of waves and streams: from the chosen file to the edit.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include "ntdlib/rsar/wave_file.h"
#include "xsflib/ascii.h"
#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_import_parts.h"
#include "app/app_internal.h"
#include "edit/new_items.h"
#include "edit/streams.h"
#include "edit/waves.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/aac.h"
#include "song/driver.h"
#include "song/items.h"

namespace hokuto::app
{

using import_parts::AddSoundsOfType;

void App::OpenWaveImport(const std::string& path, bool wav)
{
    if (!IsEnabled(wav ? Command::kImportAudioFile : Command::kImportWave))
    {
        return;
    }

    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!ReadChosenFile(file, bytes))
    {
        return;
    }

    // On the DS, the new wave goes after the waves of the archive's last wave archive, until the user chooses another
    // in the Import window. A WAV file's wave is IMA-ADPCM until the user chooses another encoding.
    if (played_.Ds())
    {
        FollowArchiveVersion();
        replace_.shown = DsWaveImport(std::move(bytes), name, song::DsWaveArchives(*played_.Ds()).back());
        return;
    }

    // On the Wii, the new wave goes after the waves of the last wave data that Hokuto reads.
    if (played_.Wii())
    {
        std::optional<uint32_t> last;
        for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(*played_.Wii()))
        {
            try
            {
                song::WaveCountOf(*played_.Wii(), wave_archive.file_id);
                last = wave_archive.file_id;
            }
            catch (const std::exception&)
            {
                // The window offers the wave data all the same, and says why it can't take the wave.
            }
        }
        if (!last)
        {
            ShowMessage("Can't import " + name + ". Hokuto can't read the wave data of any bank or wave sound.");
            return;
        }

        replace_.shown = WiiWaveImport(std::move(bytes), name, *last, ntdlib::sar::WaveEncoding::kDspAdpcm, wav);
        return;
    }

    // The new wave goes after the waves of the archive's last wave archive that Hokuto reads, until the user chooses
    // another in the Import window. A WAV file's wave is DSP-ADPCM until the user chooses another encoding.
    std::optional<uint32_t> last;
    for (uint32_t i = 0; i < played_.Sar()->WaveArchives().size(); i++)
    {
        try
        {
            if (!played_.Sar()->FileData(played_.Sar()->WaveArchives()[i].file_id).empty())
            {
                song::WaveCountOf(*played_.Sar(), i);
                last = i;
            }
        }
        catch (const std::exception&)
        {
            // The window offers the wave archive all the same, and says why it can't take the wave.
        }
    }
    if (!last)
    {
        ShowMessage("Can't import " + name + ". Hokuto can't read any wave archive in this archive.");
        return;
    }

    replace_.shown = WaveImport(std::move(bytes), name, *last, ntdlib::sar::WaveEncoding::kDspAdpcm, wav);
}

std::optional<Replacement> App::WaveImport(std::vector<uint8_t> bytes, const std::string& name, uint32_t wave_archive,
                                           ntdlib::sar::WaveEncoding encoding, bool wav, std::optional<uint32_t> rate)
{
    edit::IncomingWave incoming;
    try
    {
        incoming = wav ? edit::ReadIncomingWave(bytes, *played_.Sar(), wave_archive, std::nullopt, encoding,
                                                std::nullopt, rate)
                       : edit::ReadIncomingWaveFile(bytes, *played_.Sar(), wave_archive);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The wave archive with the new wave after its waves, and what it changes. The edit starts from the version that
    // the history has reached. The Import window says why it can't be made.
    ReplaceSource source = SourceAt(history_->Current(), played_.Sar(), wave_archive, name);
    uint32_t wave = 0;
    edit::IncomingFile waves;
    waves.kind = edit::FileKind::kWaveArchive;
    edit::FileChanges changes;
    std::optional<format::File> result;
    std::string error;
    try
    {
        wave = static_cast<uint32_t>(song::WaveCountOf(*played_.Sar(), wave_archive));
        waves.bytes = edit::WaveArchiveWith(*played_.Sar(), wave_archive, std::nullopt, incoming.bytes);
        changes = edit::ChangesOf(*source.file, *played_.Sar(), wave_archive, waves);
        result = edit::WithIncomingWave(*source.file, *played_.Sar(), wave_archive, std::nullopt, incoming);
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, wave, std::move(bytes), std::nullopt, {}, true};
    replacement.view = MakeNewWaveView(replacement.source, wave_archive, wave, incoming, wav, changes,
                                       waves.bytes.size(), std::move(result), std::move(error));

    return replacement;
}

std::optional<Replacement> App::WiiWaveImport(std::vector<uint8_t> bytes, const std::string& name, uint32_t file_id,
                                              ntdlib::sar::WaveEncoding encoding, bool wav,
                                              std::optional<uint32_t> rate)
{
    // A WAV file's wave takes the version of the wave data's first wave. A wave file of another console goes in
    // converted.
    edit::IncomingWave incoming;
    std::vector<ntdlib::rsar::WaveFile> waves;
    try
    {
        waves = edit::WavesOf(*played_.Wii(), file_id);
        const uint16_t version = waves.empty() ? ntdlib::rsar::WaveFile().version : waves.front().version;
        incoming = wav ? edit::ReadIncomingWiiWave(bytes, version, encoding, std::nullopt, rate)
                       : edit::ReadIncomingWiiWaveFile(bytes);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The wave data with the new wave after its waves. The edit starts from the version that the history has reached.
    // The Import window says why it can't be made.
    ReplaceSource source = SourceAt(history_->Current(), played_.Wii(), file_id, name);
    const auto wave = static_cast<uint32_t>(waves.size());
    auto [result, error] = TryEdit(
        [&] { return edit::WithIncomingWiiWave(*source.file, *played_.Wii(), file_id, std::nullopt, incoming); });
    Replacement replacement{std::move(source), {}, std::nullopt, wave, std::move(bytes), std::nullopt, {}, true};
    replacement.view =
        MakeWiiNewWaveView(replacement.source, file_id, wave, incoming, wav, std::move(result), std::move(error));

    return replacement;
}

void App::OpenStreamImport(const std::string& path)
{
    if (!IsEnabled(Command::kImportStream))
    {
        return;
    }

    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string file_name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!ReadChosenFile(file, bytes))
    {
        return;
    }
    if (played_.Ds())
    {
        FollowArchiveVersion();
        replace_.shown = DsStreamImport(std::move(bytes), file_name, std::nullopt, std::nullopt, std::nullopt,
                                        FirstDsName(*played_.Ds(), file_name));
        return;
    }
    if (song::IsAdts(bytes))
    {
        ShowMessage("Can't import " + file_name +
                    " as a new stream because it's an AAC file. File > Import > Stream… accepts WAV and stream files. "
                    "To use an AAC file, choose Replace… on a 3DS stream sound that plays an AAC stream.");
        return;
    }

    // The new sound copies the settings of the archive's first stream sound, or else of its first sound. The user can
    // choose another in the Import window. The name comes from the file's, without the encoding that the tools' names
    // hold. A WAV file's stream is DSP-ADPCM until the user chooses another encoding there.
    uint32_t model = 0;
    std::filesystem::path stem = file.stem();
    if (xsflib::AsciiLower(xsflib::Utf8(stem.extension())) == ".dspadpcm")
    {
        stem = stem.stem();
    }
    if (played_.Wii())
    {
        // A name that the archive's sounds, banks or players hold takes a number after it.
        const auto is_stream = [](const ntdlib::rsar::SoundInfo& sound)
        {
            return sound.type == ntdlib::rsar::SoundType::kStream;
        };
        const auto stream_sound = std::ranges::find_if(played_.Wii()->Sounds(), is_stream);
        if (stream_sound != played_.Wii()->Sounds().end())
        {
            model = static_cast<uint32_t>(stream_sound - played_.Wii()->Sounds().begin());
        }
        const std::string first = xsflib::Utf8(stem).substr(0, 200);
        std::string name = first;
        for (int number = 2; WiiNameTaken(*played_.Wii(), name); number++)
        {
            name = first + "_" + std::to_string(number);
        }
        replace_.shown =
            WiiStreamImport(std::move(bytes), file_name, model, name, ntdlib::sar::WaveEncoding::kDspAdpcm);
        return;
    }

    const auto is_stream = [](const ntdlib::sar::SoundInfo& sound)
    {
        return sound.type == ntdlib::sar::SoundType::kStream;
    };
    const auto stream_sound = std::ranges::find_if(played_.Sar()->Sounds(), is_stream);
    if (stream_sound != played_.Sar()->Sounds().end())
    {
        model = static_cast<uint32_t>(stream_sound - played_.Sar()->Sounds().begin());
    }
    const std::string name = FirstName(*history_->Current(), *played_.Sar(), xsflib::Utf8(stem) + ".bfstm");
    replace_.shown = StreamImport(std::move(bytes), file_name, model, name, ntdlib::sar::WaveEncoding::kDspAdpcm);
}

std::optional<Replacement> App::StreamImport(std::vector<uint8_t> bytes, const std::string& file_name, uint32_t model,
                                             const std::string& name, ntdlib::sar::WaveEncoding encoding,
                                             std::optional<edit::IncomingStream> incoming)
{
    const bool wav = !edit::IsStreamFile(bytes);
    if (!incoming)
    {
        try
        {
            const ntdlib::sar::Flavor flavor = played_.Sar()->ReadOptions().flavor;
            const uint32_t version = edit::IncomingStreamVersion(*history_->Current(), flavor);
            incoming = wav ? edit::ReadIncomingStreamWav(bytes, flavor, version, encoding)
                           : edit::ReadIncomingStreamFile(bytes, flavor, version);
        }
        catch (const std::exception& error)
        {
            ShowMessage("Can't import " + file_name + ". " + format::AsSentence(error.what()));
            return std::nullopt;
        }
    }

    // The sounds to copy: the stream sounds first. The edit starts from the version that the history has reached.
    ReplaceSource source;
    source.file = history_->Current();
    source.played = played_.Sar();
    source.file_name = file_name;
    for (const bool streams : {true, false})
    {
        AddSoundsOfType(source.models, played_.Sar()->Sounds(), ntdlib::sar::SoundType::kStream, streams);
    }
    source.sound = model < played_.Sar()->Sounds().size() ? model : source.models.front();

    // The new sound's stream, its path and its tracks.
    std::optional<edit::NewStream> made;
    std::string error;
    try
    {
        made = edit::MakeNewStream(*played_.Sar(), source.sound, name, *incoming);
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.view = MakeStreamImportView(replacement.source, *incoming, wav, name, made, error);
    replacement.incoming_stream = std::move(incoming);
    replacement.new_stream = std::move(made);

    return replacement;
}

std::optional<Replacement> App::WiiStreamImport(std::vector<uint8_t> bytes, const std::string& file_name,
                                                uint32_t model, const std::string& name,
                                                ntdlib::sar::WaveEncoding encoding,
                                                std::optional<edit::IncomingStream> incoming)
{
    // A stream file of the Wii goes in as it is, and one of a later console converted. A WAV file goes in as a stream
    // that NW4R's tools would lay out.
    const bool wav = !edit::IsWiiStreamFile(bytes) && !edit::IsStreamFile(bytes);
    if (!incoming)
    {
        try
        {
            incoming = wav ? edit::ReadIncomingWiiStreamWav(bytes, encoding) : edit::ReadIncomingWiiStreamFile(bytes);
        }
        catch (const std::exception& error)
        {
            ShowMessage("Can't import " + file_name + ". " + format::AsSentence(error.what()));
            return std::nullopt;
        }
    }

    // The sounds to copy: the stream sounds first. The edit starts from the version that the history has reached.
    ReplaceSource source;
    source.file = history_->Current();
    source.played = played_.Wii();
    source.file_name = file_name;
    for (const bool streams : {true, false})
    {
        AddSoundsOfType(source.models, played_.Wii()->Sounds(), ntdlib::rsar::SoundType::kStream, streams);
    }
    source.sound = model < played_.Wii()->Sounds().size() ? model : source.models.front();
    source.refused = edit::WiiAdditionsRefused(*source.file);

    // The new sound's stream, its path and its entry.
    std::optional<edit::NewStream> made;
    std::string error;
    try
    {
        made = edit::MakeNewWiiStream(*played_.Wii(), source.sound, name, *incoming);
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.view = MakeWiiStreamImportView(replacement.source, *incoming, wav, name, made, error);
    replacement.incoming_stream = std::move(incoming);
    replacement.new_stream = std::move(made);

    return replacement;
}

void App::MakeStreamImport()
{
    // The edit builds the archive again with the new sound. The browser then selects the new sound.
    const ReplaceSource& source = replace_.shown->source;
    const std::string name = NameOf(replace_.shown->view);
    if (source.played.Wii())
    {
        const auto make = [file = source.file, model = source.sound, name, stream = *replace_.shown->new_stream]
        {
            return edit::AddWiiStreamSound(*file, model, name, stream);
        };
        StartEdit(make, "Import Stream", name + " from " + source.file_name,
                  static_cast<uint32_t>(source.played.Wii()->Sounds().size()));
        return;
    }

    const auto make = [file = source.file, model = source.sound, name, stream = *replace_.shown->new_stream]
    {
        return edit::AddStreamSound(*file, model, name, stream);
    };
    StartEdit(make, "Import Stream", name + " from " + source.file_name,
              static_cast<uint32_t>(source.file->native->tables->sounds.size()));
}

} // namespace hokuto::app
