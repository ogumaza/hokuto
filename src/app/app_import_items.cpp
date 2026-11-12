// SPDX-License-Identifier: MIT

// File > Import and Project > Add… of banks, sound fonts and other items: from the chosen file to the edit.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "ntdlib/rvl/snd/rsar_options.h"
#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_import_parts.h"
#include "app/app_internal.h"
#include "edit/new_items.h"
#include "edit/sfz_imports.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/driver.h"
#include "song/items.h"

namespace hokuto::app
{

using import_parts::AddSoundsOfType;
using import_parts::kNewBankName;

void App::OpenItemImport(const std::string& path, edit::FileKind kind)
{
    const Command command = kind == edit::FileKind::kBank          ? Command::kImportBank
                            : kind == edit::FileKind::kWaveArchive ? Command::kImportWaveArchive
                                                                   : Command::kImportWaveSoundFile;
    if (!IsEnabled(command))
    {
        return;
    }

    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    if (kind == edit::FileKind::kBank && edit::IsSfzName(name))
    {
        OpenSfzImport(file);
        return;
    }
    std::vector<uint8_t> bytes;
    if (!ReadChosenFile(file, bytes))
    {
        return;
    }
    if (kind == edit::FileKind::kBank && edit::IsSoundFont(bytes))
    {
        OpenSoundFontImport(std::move(bytes), name);
        return;
    }
    if (kind == edit::FileKind::kBank && edit::IsSoundFontName(name))
    {
        ShowMessage("Can't import " + name + ". It isn't a SoundFont 2 file.");
        return;
    }
    if (played_.Gba())
    {
        ShowMessage("Can't import " + name + ". A GBA ROM accepts only a SoundFont 2 or SFZ file here.");
        return;
    }
    if (played_.Ds())
    {
        OpenDsImport(std::move(bytes), name,
                     kind == edit::FileKind::kBank ? edit::DsNewItem::kBank : edit::DsNewItem::kWaveArchive);
        return;
    }

    // On the Wii, a bank file or a wave sound file can have its wave data beside it in an RWAR of the same name.
    // Export… writes the wave data that way.
    ReplaceSource source;
    if (played_.Wii() && kind != edit::FileKind::kWaveArchive)
    {
        std::filesystem::path beside = file;
        beside.replace_extension(".brwar");
        std::error_code error;
        if (std::filesystem::is_regular_file(beside, error) && xsflib::ReadFile(beside, source.beside))
        {
            source.beside_name = xsflib::Utf8(beside.filename());
        }
        else
        {
            source.beside.clear();
        }
    }
    OpenItemBytes(std::move(bytes), name, kind, std::move(source));
}

void App::OpenSfzImport(const std::filesystem::path& file)
{
    // SFZ files come in as the SoundFont that they make. The imports of a SoundFont then list what they leave out too.
    const std::string name = xsflib::Utf8(file.filename());
    try
    {
        const std::vector<edit::SfzInstrument> instruments = edit::SfzInstrumentsBeside(file);
        const auto read_file = [](const std::filesystem::path& path, std::vector<uint8_t>& bytes)
        {
            return xsflib::ReadFile(path, bytes);
        };
        edit::SfzSoundFont font = edit::SoundFontOfSfz(instruments, read_file);
        OpenSoundFontImport(std::move(font.sf2), edit::SfzImportName(instruments, name), std::move(font.warnings));
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
    }
}

void App::OpenGbaSoundFontImport(std::vector<uint8_t> bytes, const std::string& name, std::vector<std::string> warnings)
{
    // The window offers the songs that play, the shown song first: the song's voice group receives the presets.
    Replacement replacement;
    replacement.source.file = history_->Current();
    replacement.source.places = Places();
    replacement.source.file_name = name;
    replacement.gba_songs = song::GbaSongs(*played_.Gba());
    const auto shown = sequence_ ? std::ranges::find(replacement.gba_songs, static_cast<int>(sequence_->sound))
                                 : replacement.gba_songs.end();
    const auto chosen =
        static_cast<std::size_t>(shown != replacement.gba_songs.end() ? shown - replacement.gba_songs.begin() : 0);
    replacement.source.sound = static_cast<uint32_t>(replacement.gba_songs.at(chosen));
    try
    {
        replacement.gba_sound_font =
            std::make_shared<const edit::GbaSoundFontVoices>(edit::GbaVoicesOfSoundFont(bytes, played_.Gba()->info));
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
        return;
    }
    replacement.view = MakeGbaSoundFontImportView(replacement.source, *replacement.gba_sound_font, *played_.Gba(),
                                                  replacement.gba_songs, chosen, 0);
    replacement.view.warnings.insert(replacement.view.warnings.begin(), warnings.begin(), warnings.end());
    replacement.sound_font_warnings = std::move(warnings);
    replacement.sound_font = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    replace_.shown = std::move(replacement);
}

void App::OpenSoundFontImport(std::vector<uint8_t> bytes, const std::string& name, std::vector<std::string> warnings)
{
    if (played_.Gba())
    {
        OpenGbaSoundFontImport(std::move(bytes), name, std::move(warnings));
        return;
    }

    // The Import window starts from the version that the history has reached. The conversion runs once: a name that the
    // user types changes only the view.
    ReplaceSource source;
    if (played_.Ds())
    {
        FollowArchiveVersion();
    }
    source.file = history_->Current();
    source.played = played_.Wii()  ? song::Played(played_.Wii())
                    : played_.Ds() ? song::Played(played_.Ds())
                                   : song::Played(played_.Sar());
    source.file_name = name;
    if (played_.Ds())
    {
        source.refused = edit::DsAdditionsRefused(*source.file);
    }
    else
    {
        source.refused = FindGroups(source);
    }
    const std::string first = played_.Ds()    ? FirstDsName(*played_.Ds(), name)
                              : played_.Wii() ? FirstWiiName(source, name)
                                              : FirstName(*source.file, *played_.Sar(), name, 1);

    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::nullopt, {}};
    try
    {
        replacement.sound_font_import = edit::ReadSoundFontImport(*replacement.source.file, bytes);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
        return;
    }
    replacement.sound_font = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    replacement.view = MakeSoundFontImportView(replacement.source, *replacement.sound_font_import, first);
    replacement.view.warnings.insert(replacement.view.warnings.begin(), warnings.begin(), warnings.end());
    replacement.sound_font_warnings = std::move(warnings);
    replace_.shown = std::move(replacement);
}

void App::StartBankAddition()
{
    if (!IsEnabled(Command::kAddBank))
    {
        return;
    }
    if (played_.Ds())
    {
        OpenDsImport(edit::EmptyDsBankFile(), kNewBankName, edit::DsNewItem::kBank);
        return;
    }

    ReplaceSource source;
    source.new_bank = true;
    if (played_.Wii())
    {
        // An empty bank of the Wii comes with its wave data.
        ntdlib::rsar::BankFile::Files files = edit::EmptyBank(*played_.Wii());
        edit::IncomingFile incoming{edit::FileKind::kBank,
                                    edit::NewWiiFileVersion(*played_.Wii(), edit::FileKind::kBank),
                                    std::move(files.file), std::nullopt, std::move(files.wave_data)};
        OpenWiiItem(std::move(incoming), {}, 1, std::string(kNewBankName) + ".brbnk", std::move(source));
        return;
    }

    OpenItemBytes(edit::EmptyBank(played_.Sar()->ReadOptions().flavor), std::string(kNewBankName) + ".bfbnk",
                  edit::FileKind::kBank, std::move(source));
}

void App::OpenItemBytes(std::vector<uint8_t> bytes, const std::string& name, edit::FileKind kind, ReplaceSource source)
{
    if (played_.Wii())
    {
        OpenWiiItemBytes(std::move(bytes), name, kind, std::move(source));
        return;
    }

    // The file, read as the import reads it.
    edit::IncomingFile incoming;
    std::vector<edit::NamedWaveArchive> named;
    std::size_t sounds = 1;
    try
    {
        const ntdlib::sar::Flavor flavor = played_.Sar()->ReadOptions().flavor;
        incoming = edit::ReadIncomingFile(bytes, kind, flavor, edit::IncomingVersions(*played_.Sar(), kind));
        if (kind != edit::FileKind::kWaveArchive)
        {
            named = edit::WaveArchivesNamed(incoming.bytes, flavor);
        }
        if (kind == edit::FileKind::kWaveSoundFile)
        {
            sounds = std::max<std::size_t>(edit::WaveSoundCount(incoming.bytes, flavor), 1);
        }
    }
    catch (const std::invalid_argument& error)
    {
        ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    // New wave sounds copy the settings of a wave sound of the archive with the same file, or else of the first wave
    // sound, or else of the first sequence sound. The menu lists the wave sounds first.
    if (kind == edit::FileKind::kWaveSoundFile)
    {
        for (const ntdlib::sar::SoundType type :
             {ntdlib::sar::SoundType::kWaveSound, ntdlib::sar::SoundType::kSequence})
        {
            AddSoundsOfType(source.models, played_.Sar()->Sounds(), type);
        }
        if (source.models.empty())
        {
            ShowMessage("Can't import " + name +
                        ". The archive has no wave sound or sequence sound for the new sounds to copy.");
            return;
        }

        const auto same_file = [this, &incoming](uint32_t sound)
        {
            const ntdlib::sar::SoundInfo& info = played_.Sar()->Sounds()[sound];
            return info.type == ntdlib::sar::SoundType::kWaveSound &&
                   std::ranges::equal(played_.Sar()->FileData(info.file_id), incoming.bytes);
        };
        const auto found = std::ranges::find_if(source.models, same_file);
        source.sound = found != source.models.end() ? *found : source.models.front();
    }

    // The Import window starts from the version that the history has reached.
    source.file = history_->Current();
    source.played = played_.Sar();
    source.file_name = name;
    FindGroups(source);
    const std::vector<int> targets = FirstWaveArchives(*played_.Sar(), incoming.bytes, named);
    const std::string first = FirstName(*source.file, *played_.Sar(), name, sounds);
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::move(incoming),
                            std::move(named)};
    replacement.view = MakeItemImportView(replacement.source, *replacement.new_item, replacement.named, targets, first);
    replace_.shown = std::move(replacement);
}

void App::OpenWiiItemBytes(std::vector<uint8_t> bytes, const std::string& name, edit::FileKind kind,
                           ReplaceSource source)
{
    // The file, read as the import reads it. A bank file or a wave sound file of another console waits for the Import
    // window's wave data. A wave archive of another console goes in converted.
    edit::IncomingFile incoming;
    std::vector<edit::NamedWaveArchive> named;
    std::size_t sounds = 1;
    try
    {
        const std::optional<ntdlib::sar::Flavor> later =
            kind == edit::FileKind::kWaveArchive ? std::nullopt : edit::LaterConsoleOf(bytes, kind);
        if (later)
        {
            incoming = edit::ReadIncomingFile(bytes, kind, *later);
            named = edit::WaveArchivesNamed(incoming.bytes, *later);
            source.later = later;
            source.beside.clear();
            source.beside_name.clear();
            if (kind == edit::FileKind::kWaveSoundFile)
            {
                sounds = std::max<std::size_t>(edit::WaveSoundCount(incoming.bytes, *later), 1);
            }
        }
        else
        {
            incoming = edit::ReadWiiIncomingFile(bytes, kind);
            if (kind == edit::FileKind::kWaveSoundFile)
            {
                sounds = std::max<std::size_t>(
                    ntdlib::rsar::WaveSoundFile::Parse(incoming.bytes, ntdlib::rvl::snd::kRsarOptions).Sounds().size(),
                    1);
            }
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    OpenWiiItem(std::move(incoming), std::move(named), sounds, name, std::move(source));
}

void App::OpenWiiItem(edit::IncomingFile incoming, std::vector<edit::NamedWaveArchive> named, std::size_t sounds,
                      const std::string& name, ReplaceSource source)
{
    // New wave sounds copy the settings of a wave sound of the archive with the same file, or else of the first wave
    // sound, or else of the first sequence sound. The menu lists the wave sounds first.
    if (incoming.kind == edit::FileKind::kWaveSoundFile)
    {
        for (const ntdlib::rsar::SoundType type :
             {ntdlib::rsar::SoundType::kWaveSound, ntdlib::rsar::SoundType::kSequence})
        {
            AddSoundsOfType(source.models, played_.Wii()->Sounds(), type);
        }
        if (source.models.empty())
        {
            ShowMessage("Can't import " + name +
                        ". The archive has no wave sound or sequence sound for the new sounds to copy.");
            return;
        }

        const auto same_file = [this, &incoming](uint32_t sound)
        {
            const ntdlib::rsar::SoundInfo& info = played_.Wii()->Sounds()[sound];
            return info.type == ntdlib::rsar::SoundType::kWaveSound &&
                   std::ranges::equal(played_.Wii()->FileData(info.file_id), incoming.bytes);
        };
        const auto found = std::ranges::find_if(source.models, same_file);
        source.sound = found != source.models.end() ? *found : source.models.front();
    }

    // The Import window starts from the version that the history has reached. The new items join groups with names. An
    // archive that NW4R's tools didn't lay out takes no new item, and the window says so from the start.
    source.file = history_->Current();
    source.played = played_.Wii();
    source.file_name = name;
    source.refused = FindGroups(source);
    const std::vector<int> targets = FirstWiiWaveData(source, incoming, named);
    const std::string first = FirstWiiName(source, name, sounds);
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::move(incoming),
                            std::move(named)};
    replacement.view =
        MakeWiiItemImportView(replacement.source, *replacement.new_item, replacement.named, targets, first);
    replace_.shown = std::move(replacement);
}

void App::MakeWiiItemImport()
{
    // The work converts the file with the window's wave data, and builds the archive again with the new item. The
    // browser selects the first new sound once it lists the sound. The Bank Editor opens on a bank that Project > Add
    // Bank… adds.
    const ReplaceSource& source = replace_.shown->source;
    const ReplaceView& view = replace_.shown->view;
    const edit::IncomingFile& incoming = *replace_.shown->new_item;
    const std::string name = NameOf(view);
    const bool wave_sounds = incoming.kind == edit::FileKind::kWaveSoundFile;
    auto make = [source, incoming, named = replace_.shown->named, targets = ChosenWaveArchives(view), name,
                 groups = view.joined, wave_sounds]
    {
        const edit::IncomingFile imported = WiiImportedFile(source, incoming, named, targets);
        return wave_sounds ? edit::AddWiiWaveSounds(*source.file, source.sound, name, imported.bytes,
                                                    imported.wave_data, groups)
                           : edit::AddWiiBank(*source.file, name, imported.bytes, imported.wave_data, groups);
    };
    std::string action = source.new_bank                                 ? "Add Bank"
                         : incoming.kind == edit::FileKind::kBank        ? "Import Bank"
                         : incoming.kind == edit::FileKind::kWaveArchive ? "Import Wave Archive"
                                                                         : "Import Wave Sound File";
    std::optional<uint32_t> select;
    std::optional<uint32_t> open_bank;
    if (wave_sounds)
    {
        select = static_cast<uint32_t>(source.played.Wii()->Sounds().size());
    }
    else if (source.new_bank)
    {
        open_bank = static_cast<uint32_t>(source.played.Wii()->Banks().size());
    }
    std::string details = name + (source.new_bank ? std::string() : " from " + source.file_name);
    StartEdit(std::move(make), std::move(action), std::move(details), select);
    replace_.shown->pending->open_bank = open_bank;
}

void App::MakeItemImport()
{
    if (played_.Wii())
    {
        MakeWiiItemImport();
        return;
    }

    const ReplaceSource& source = replace_.shown->source;
    const edit::IncomingFile& incoming = *replace_.shown->new_item;
    const std::string name = NameOf(replace_.shown->view);
    std::function<format::File()> make = replace_.ItemImportEdit(true);

    // The browser selects the first new sound once it lists the sound. The Bank Editor opens on a bank that Project >
    // Add Bank… adds.
    std::string action = "Import Wave Archive";
    std::optional<uint32_t> select;
    std::optional<uint32_t> open_bank;
    if (incoming.kind == edit::FileKind::kBank)
    {
        action = source.new_bank ? "Add Bank" : "Import Bank";
        if (source.new_bank)
        {
            open_bank = static_cast<uint32_t>(source.file->native->tables->banks.size());
        }
    }
    else if (incoming.kind == edit::FileKind::kWaveSoundFile)
    {
        action = "Import Wave Sound File";
        select = static_cast<uint32_t>(source.file->native->tables->sounds.size());
    }
    std::string details = (name.empty() ? std::string("A wave archive") : name) +
                          (source.new_bank ? std::string() : " from " + source.file_name);
    StartEdit(std::move(make), std::move(action), std::move(details), select);
    replace_.shown->pending->open_bank = open_bank;
}

} // namespace hokuto::app
