// SPDX-License-Identifier: MIT

// Replace… of an item of a file of the Wii or a later console: the dialog, and the replacement of a file, a wave or a
// stream.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "ntdlib/rsar/wave_file.h"
#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "edit/new_items.h"
#include "edit/replace.h"
#include "edit/streams.h"
#include "edit/waves.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/aac.h"
#include "song/items.h"
#include "song/streams.h"

namespace hokuto::app
{

void App::StartReplace(const BrowserPlace& place)
{
    const auto group = static_cast<std::size_t>(place.group);
    const auto index = static_cast<std::size_t>(place.item);
    const bool item =
        place.group >= 0 && place.item >= 0 && group < browser_.size() && index < browser_[group].items.size();
    const auto child = static_cast<std::size_t>(place.child);
    const bool listed = item && place.child >= 0 && child < browser_[group].items[index].child_numbers.size();
    const bool wave = listed && group == kWavesGroup;
    const bool wave_sound_child = listed && group == kWaveSoundFilesGroup;
    if (!item || (place.child >= 0 && !wave && !wave_sound_child) || export_ || replace_.shown || !history_ ||
        preview_ || !played_ || (played_.Driver() && !played_.Driver()->EditsSamples()))
    {
        return;
    }
    if (played_.Driver())
    {
        StartDriverReplace(place);
        return;
    }
    if (played_.Gba())
    {
        StartGbaReplace(place);
        return;
    }
    if (played_.Dse())
    {
        StartDseReplace(place);
        return;
    }
    if (played_.Ds())
    {
        StartDsReplace(place);
        return;
    }
    if (played_.Wii())
    {
        StartWiiReplace(place);
        return;
    }
    StartSarReplace(place);
}

void App::StartDriverReplace(const BrowserPlace& place)
{
    const auto group = static_cast<std::size_t>(place.group);
    const auto index = static_cast<std::size_t>(place.item);
    const auto child = static_cast<std::size_t>(place.child);
    const bool wave = place.child >= 0 && group == kWavesGroup;
    // A sample of Konami's driver is replaced with a WAV file. The file comes from the dialog. The program reads it in
    // a later frame.
    replace_.wave.reset();
    if (wave)
    {
        replace_.wave = WaveTarget{0, browser_[group].items[index].child_numbers[child]};
        ShowDialog(Dialog::kReplace);
    }
}

void App::StartSarReplace(const BrowserPlace& place)
{
    const auto group = static_cast<std::size_t>(place.group);
    const auto index = static_cast<std::size_t>(place.item);
    const auto child = static_cast<std::size_t>(place.child);
    // StartReplace allows a child only of a wave archive or of a wave sound file.
    const bool wave = place.child >= 0 && group == kWavesGroup;
    const bool wave_sound_child = place.child >= 0 && group == kWaveSoundFilesGroup;

    // A sequence sound takes a sequence file. A bank, a wave archive or a wave sound takes a file of its kind, and a
    // wave takes a WAV file. A stream sound takes a stream file or a WAV file. A wave sound under its wave sound file
    // takes a wave sound file. So does a wave sound in the sounds' list. The file comes from the dialog. The program
    // reads it in a later frame.
    const uint32_t number =
        wave_sound_child ? browser_[group].items[index].child_numbers[child] : browser_[group].items[index].number;
    const bool wave_sound = group == kSoundsGroup && number < played_.Sar()->Sounds().size() &&
                            played_.Sar()->Sounds()[number].type == ntdlib::sar::SoundType::kWaveSound;
    const bool stream = group == kSoundsGroup && song::StreamFileOf(*played_.Sar(), number).has_value();
    replace_.ForgetTargets();
    if (wave)
    {
        replace_.wave = WaveTarget{number, browser_[group].items[index].child_numbers[child]};
    }
    else if (group == kSequencesGroup && start_fields_)
    {
        replace_.sound = number;
    }
    else if (group == kBanksGroup)
    {
        replace_.file = FileTarget{edit::FileKind::kBank, number};
    }
    else if (group == kWavesGroup)
    {
        replace_.file = FileTarget{edit::FileKind::kWaveArchive, number};
    }
    else if (wave_sound || group == kWaveSoundFilesGroup)
    {
        replace_.file = FileTarget{edit::FileKind::kWaveSoundFile, number};
    }
    else if (stream)
    {
        replace_.stream = number;
    }
    else
    {
        return;
    }

    ShowDialog(Dialog::kReplace);
}

void App::StartWiiReplace(const BrowserPlace& place)
{
    // On the Wii, a sequence sound takes a sequence file of the Wii, and a bank or a wave sound a file of its kind. A
    // wave archive is the wave data of a bank or a wave sound file. It takes an RWAR where the file keeps its waves in
    // one. A wave of the wave data takes a WAV file or a wave file of the Wii. A stream sound takes a stream file of
    // the Wii, the 3DS, the Wii U or the Switch, or a WAV file. The file comes from the dialog. The program reads it in
    // a later frame.
    const auto group = static_cast<std::size_t>(place.group);
    const auto index = static_cast<std::size_t>(place.item);
    const bool wave_sound_child = place.child >= 0 && group == kWaveSoundFilesGroup;
    const uint32_t number = wave_sound_child
                                ? browser_[group].items[index].child_numbers[static_cast<std::size_t>(place.child)]
                                : browser_[group].items[index].number;
    const bool wave_sound = group == kSoundsGroup && number < played_.Wii()->Sounds().size() &&
                            played_.Wii()->Sounds()[number].type == ntdlib::rsar::SoundType::kWaveSound;
    const bool stream = group == kSoundsGroup && song::StreamFileOf(*played_.Wii(), number).has_value();
    replace_.ForgetTargets();
    if (place.child >= 0 && group == kWavesGroup)
    {
        replace_.wave =
            WaveTarget{number, browser_[group].items[index].child_numbers[static_cast<std::size_t>(place.child)]};
        ShowDialog(Dialog::kReplace);
        return;
    }
    if (place.child >= 0 && !wave_sound_child)
    {
        return;
    }

    if (group == kSequencesGroup && start_fields_)
    {
        replace_.sound = number;
    }
    else if (group == kBanksGroup)
    {
        replace_.file = FileTarget{edit::FileKind::kBank, number};
    }
    else if (group == kWavesGroup)
    {
        replace_.file = FileTarget{edit::FileKind::kWaveArchive, number};
    }
    else if (wave_sound || group == kWaveSoundFilesGroup)
    {
        replace_.file = FileTarget{edit::FileKind::kWaveSoundFile, number};
    }
    else if (stream)
    {
        replace_.stream = number;
    }
    else
    {
        return;
    }

    ShowDialog(Dialog::kReplace);
}

void App::OpenReplacement(const std::string& path)
{
    if (played_.Driver())
    {
        OpenDriverReplacement(path);
        return;
    }
    if (played_.Gba())
    {
        OpenGbaReplacement(path);
        return;
    }
    if (played_.Dse())
    {
        OpenDseReplacement(path);
        return;
    }
    if (played_.Ds())
    {
        OpenDsReplacement(path);
        return;
    }
    if (replace_.file)
    {
        OpenFileReplacement(path);
        return;
    }
    if (replace_.wave)
    {
        OpenWaveReplacement(path);
        return;
    }
    if (replace_.stream)
    {
        OpenStreamReplacement(path);
        return;
    }
    OpenSequenceReplacement(path);
}

void App::OpenSequenceReplacement(const std::string& path)
{
    const std::optional<uint32_t> sound = std::exchange(replace_.sound, std::nullopt);
    if (!sound || !history_ || (!played_.Sar() && !played_.Wii()) || !start_fields_)
    {
        return;
    }

    // The file, read as the replacement reads it.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!ReadChosenFile(file, bytes))
    {
        return;
    }

    // A Wii archive takes a sequence file of the Wii, read by the rules of the archive's build of NW4R SND. A file of
    // another console takes the biquad filters that come nearest on the archive's console.
    ReplaceSource source;
    const std::shared_ptr<const format::File> current = history_->Current();
    try
    {
        ReadIncomingSequence(source, std::move(bytes), name, *current);
    }
    catch (const std::invalid_argument& error)
    {
        const std::string sound_name =
            played_.Wii() ? played_.Wii()->Sounds()[*sound].name : played_.Sar()->Sounds()[*sound].name;
        ShowMessage("Can't replace the sequence of " + sound_name + " with " + name + ". " +
                    format::AsSentence(error.what()));
        return;
    }

    // The Replace window starts from the version that the history has reached.
    source.file = current;
    source.played = played_.Wii() ? song::Played(played_.Wii()) : song::Played(played_.Sar());
    source.places = played_.Wii() ? edit::SoundPlaces(*played_.Wii(), file_sequences_, *start_fields_)
                                  : edit::SoundPlaces(*played_.Sar(), file_sequences_, *start_fields_);
    source.sound = *sound;
    source.file_name = name;
    FindSoundStarts(source);
    FindReaders(source);
    FindGroups(source);
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::nullopt, {}};
    const edit::ProgramMap programs = ProposeMidiMap(replacement.source);
    replacement.view = MakeReplaceView(replacement.source, std::nullopt, programs, {}, false);
    replace_.shown = std::move(replacement);
}

void App::OpenFileReplacement(const std::string& path)
{
    const std::optional<FileTarget> target = std::exchange(replace_.file, std::nullopt);
    if (!target || !history_ || (!played_.Sar() && !played_.Wii()))
    {
        return;
    }
    if (played_.Wii())
    {
        OpenWiiFileReplacement(path, *target);
        return;
    }

    // The file, read as the replacement reads it.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!ReadChosenFile(file, bytes))
    {
        return;
    }

    edit::IncomingFile incoming;
    std::vector<edit::NamedWaveArchive> named;
    std::vector<int> targets;
    try
    {
        const ntdlib::sar::Flavor flavor = played_.Sar()->ReadOptions().flavor;
        incoming = edit::ReadIncomingFile(bytes, target->kind, flavor,
                                          edit::IncomingVersions(*played_.Sar(), target->kind, target->item));
        if (target->kind != edit::FileKind::kWaveArchive)
        {
            // The new file's wave archives keep their numbers where the present file names them too.
            named = edit::WaveArchivesNamed(incoming.bytes, flavor);
            std::vector<edit::NamedWaveArchive> present;
            try
            {
                present = edit::WaveArchivesNamed(
                    played_.Sar()->FileData(edit::FileIdOf(*played_.Sar(), target->kind, target->item)), flavor);
            }
            catch (const std::invalid_argument&)
            {
                // A present file that ntdlib can't read names no wave archive.
            }
            targets = FirstWaveArchives(*played_.Sar(), incoming.bytes, named, present);
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace " + ReplacedName(*played_.Sar(), target->kind, target->item) + " with " + name +
                    ". " + format::AsSentence(error.what()));
        return;
    }

    replace_.shown = FileReplacement(std::move(incoming), name, *target, std::move(named), targets);
}

void App::OpenWiiFileReplacement(const std::string& path, const FileTarget& target)
{
    // The file, read as the replacement reads it.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!ReadChosenFile(file, bytes))
    {
        return;
    }

    const std::string item = ReplacedName(*played_.Wii(), target.kind, target.item);

    // A bank file or a wave sound file of another console goes in converted, with the wave data chosen in the Replace
    // window. Its rows start at the item's wave data.
    const std::optional<ntdlib::sar::Flavor> later =
        target.kind == edit::FileKind::kWaveArchive ? std::nullopt : edit::LaterConsoleOf(bytes, target.kind);
    if (later)
    {
        OpenWiiLaterFileReplacement(bytes, name, item, target, *later);
        return;
    }

    edit::IncomingFile incoming;
    edit::FileChanges changes;
    ReplaceSource source = SourceAt(history_->Current(), played_.Wii(), target.item, name);
    if (start_fields_)
    {
        source.places = edit::SoundPlaces(*played_.Wii(), file_sequences_, *start_fields_);
    }
    if (target.kind != edit::FileKind::kWaveArchive)
    {
        // The window names an RWAR of the file's name beside it. The new file uses the current wave data instead.
        std::filesystem::path beside = file;
        beside.replace_extension(".brwar");
        std::error_code error;
        if (std::filesystem::is_regular_file(beside, error))
        {
            source.beside_name = xsflib::Utf8(beside.filename());
        }
    }
    try
    {
        incoming = edit::ReadWiiIncomingFile(bytes, target.kind);
        changes = edit::ChangesOf(*source.file, *played_.Wii(), target.item, incoming);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace " + item + " with " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    // The edit starts from the version that the history has reached. The Replace window says why it can't be made.
    auto [result, error] =
        TryEdit([&] { return edit::ReplaceWiiFile(*source.file, *played_.Wii(), target.item, incoming); });
    Replacement replacement{std::move(source), {}, target.kind, std::nullopt, {}, std::move(incoming), {}};
    replacement.view =
        MakeWiiFileReplaceView(replacement.source, *replacement.new_item, changes, std::move(result), std::move(error));
    replace_.shown = std::move(replacement);
}

void App::OpenWiiLaterFileReplacement(const std::vector<uint8_t>& bytes, const std::string& name,
                                      const std::string& item, const FileTarget& target, ntdlib::sar::Flavor later)
{
    edit::IncomingFile incoming;
    std::vector<edit::NamedWaveArchive> named;
    std::vector<int> targets;
    try
    {
        incoming = edit::ReadIncomingFile(bytes, target.kind, later);
        named = edit::WaveArchivesNamed(incoming.bytes, later);
        ReplaceSource first;
        first.played = played_.Wii();
        first.later = later;
        targets = FirstWiiWaveData(first, incoming, named, edit::FileIdOf(*played_.Wii(), target.kind, target.item));
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace " + item + " with " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    replace_.shown = WiiFileReplacement(std::move(incoming), name, target, later, std::move(named), targets);
}

std::optional<Replacement> App::WiiFileReplacement(edit::IncomingFile incoming, const std::string& name,
                                                   const FileTarget& target, ntdlib::sar::Flavor later,
                                                   std::vector<edit::NamedWaveArchive> named,
                                                   const std::vector<int>& targets)
{
    // The file in the present file's version with the chosen wave data, and what it changes. The edit starts from the
    // version that the history has reached. The Replace window says why it can't be made.
    ReplaceSource source = SourceAt(history_->Current(), played_.Wii(), target.item, name);
    if (start_fields_)
    {
        source.places = edit::SoundPlaces(*played_.Wii(), file_sequences_, *start_fields_);
    }
    source.later = later;
    edit::IncomingFile mapped = incoming;
    edit::FileChanges changes;
    std::optional<format::File> result;
    std::string error;
    try
    {
        const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(*played_.Wii());
        std::map<uint32_t, uint32_t> map;
        for (std::size_t k = 0; k < named.size(); k++)
        {
            const int each = k < targets.size() ? targets[k] : -1;
            if (each < 0 || static_cast<std::size_t>(each) >= wave_data.size())
            {
                throw std::invalid_argument(
                    "Choose wave data in this archive for every wave archive that the file uses.");
            }

            map[named[k].id] = wave_data[static_cast<std::size_t>(each)].file_id;
        }
        mapped = edit::WiiFileOf(incoming, later, *played_.Wii(), map,
                                 edit::PresentWiiVersion(*played_.Wii(), target.kind, target.item));
        changes = edit::ChangesOf(*source.file, *played_.Wii(), target.item, mapped);
    }
    catch (const std::exception& failure)
    {
        mapped = incoming;
        error = failure.what();
    }
    if (error.empty())
    {
        try
        {
            result = edit::ReplaceWiiFile(*source.file, *played_.Wii(), target.item, mapped);
        }
        catch (const std::invalid_argument& failure)
        {
            error = failure.what();
        }
    }
    Replacement replacement{std::move(source), {}, target.kind, std::nullopt, {}, std::move(incoming),
                            std::move(named)};
    replacement.view = MakeWiiFileReplaceView(replacement.source, mapped, changes, std::move(result), std::move(error),
                                              replacement.named, targets);

    return replacement;
}

std::optional<Replacement> App::FileReplacement(edit::IncomingFile incoming, const std::string& name,
                                                const FileTarget& target, std::vector<edit::NamedWaveArchive> named,
                                                const std::vector<int>& targets,
                                                const std::vector<edit::ProgramFix>& fixes,
                                                std::optional<std::vector<uint32_t>> joined,
                                                std::vector<std::string> new_groups)
{
    // The file with the chosen wave archives, and what it changes. A bank then takes the chosen instruments. The edit
    // starts from the version that the history has reached. The Replace window says why it can't be made.
    ReplaceSource source = SourceAt(history_->Current(), played_.Sar(), target.item, name);
    if (start_fields_)
    {
        source.places = edit::SoundPlaces(*played_.Sar(), file_sequences_, *start_fields_);
    }
    FindGroups(source);
    source.joined = std::move(joined);
    source.new_groups = std::move(new_groups);
    edit::IncomingFile mapped = incoming;
    const bool chosen = std::ranges::none_of(targets, [](int each) { return each < 0; });
    std::optional<format::File> result;
    std::string error;
    edit::FileChanges changes;
    try
    {
        if (!named.empty() && chosen)
        {
            std::map<uint32_t, uint32_t> map;
            for (std::size_t k = 0; k < named.size() && k < targets.size(); k++)
            {
                map[named[k].id] = static_cast<uint32_t>(targets[k]);
            }
            mapped.bytes = edit::WithWaveArchiveMap(*source.file, incoming.bytes, map);
        }
        changes = edit::ChangesOf(*source.file, *played_.Sar(), target.item, mapped);
    }
    catch (const std::exception& failure)
    {
        ShowMessage("Can't replace " + ReplacedName(*played_.Sar(), target.kind, target.item) + " with " + name + ". " +
                    format::AsSentence(failure.what()));
        return std::nullopt;
    }

    try
    {
        if (target.kind == edit::FileKind::kBank && !fixes.empty())
        {
            mapped.bytes = edit::WithProgramFixes(*played_.Sar(), target.item, mapped.bytes, fixes);
        }
        if (chosen)
        {
            result =
                edit::ReplaceFile(*source.file, edit::FileIdOf(*played_.Sar(), target.kind, target.item), mapped.bytes);
        }
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, target.kind, std::nullopt, {}, std::move(incoming),
                            std::move(named)};
    replacement.view = MakeFileReplaceView(replacement.source, mapped, changes, std::move(result), std::move(error),
                                           replacement.named, targets, fixes);

    return replacement;
}

void App::OpenWaveReplacement(const std::string& path)
{
    const std::optional<WaveTarget> target = std::exchange(replace_.wave, std::nullopt);
    if (!target || !history_ || (!played_.Sar() && !played_.Wii()))
    {
        return;
    }

    // A WAV file goes in as DSP-ADPCM until the user chooses another encoding in the Replace window.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!ReadChosenFile(file, bytes))
    {
        return;
    }

    replace_.shown = played_.Wii()
                         ? WiiWaveReplacement(std::move(bytes), name, *target, ntdlib::sar::WaveEncoding::kDspAdpcm)
                         : WaveReplacement(std::move(bytes), name, *target, ntdlib::sar::WaveEncoding::kDspAdpcm);
}

std::optional<Replacement> App::WaveReplacement(std::vector<uint8_t> wav, const std::string& name,
                                                const WaveTarget& target, ntdlib::sar::WaveEncoding encoding,
                                                std::optional<uint32_t> rate, std::optional<edit::WaveLoop> loop)
{
    // The wave of the WAV file or the wave file, the wave archive that it makes, and what the two change.
    const std::string wave_name =
        song::ItemName(*played_.Sar(), {song::ItemKind::kWave, target.wave_archive, target.wave});
    const bool wave_file = edit::IsWaveFile(wav);
    edit::IncomingWave incoming;
    edit::WaveChanges changes;
    edit::IncomingFile wave_archive;
    edit::FileChanges archive_changes;
    try
    {
        incoming = wave_file ? edit::ReadIncomingWaveFile(wav, *played_.Sar(), target.wave_archive, target.wave)
                             : edit::ReadIncomingWave(wav, *played_.Sar(), target.wave_archive, target.wave, encoding,
                                                      loop, rate);
        changes = edit::ChangesOfWave(*played_.Sar(), target.wave_archive, target.wave);
        wave_archive.kind = edit::FileKind::kWaveArchive;
        wave_archive.bytes = edit::WaveArchiveWith(*played_.Sar(), target.wave_archive, target.wave, incoming.bytes);
        archive_changes = edit::ChangesOf(*history_->Current(), *played_.Sar(), target.wave_archive, wave_archive);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace " + wave_name + " with " + name + ". " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached. The Replace window says why it can't be made.
    ReplaceSource source = SourceAt(history_->Current(), played_.Sar(), target.wave_archive, name);
    auto [result, error] = TryEdit(
        [&]
        { return edit::WithIncomingWave(*source.file, *played_.Sar(), target.wave_archive, target.wave, incoming); });
    Replacement replacement{std::move(source), {}, std::nullopt, target.wave, std::move(wav), std::nullopt, {}};
    replacement.view =
        MakeWaveReplaceView(replacement.source, target.wave, incoming, !wave_file, changes, archive_changes,
                            wave_archive.bytes.size(), std::move(result), std::move(error));

    return replacement;
}

std::optional<Replacement> App::WiiWaveReplacement(std::vector<uint8_t> wav, const std::string& name,
                                                   const WaveTarget& target, ntdlib::sar::WaveEncoding encoding,
                                                   std::optional<uint32_t> rate, std::optional<edit::WaveLoop> loop)
{
    // The wave of the WAV file or the wave file, and the present wave and what plays it. A WAV file's wave takes the
    // present wave's version.
    const std::string wave_name =
        song::ItemName(*played_.Wii(), {song::ItemKind::kWave, target.wave_archive, target.wave});
    const bool wave_file = edit::IsWiiWaveFile(wav) || edit::IsWaveFile(wav);
    edit::IncomingWave incoming;
    edit::WaveChanges changes;
    try
    {
        changes = edit::ChangesOfWiiWave(*played_.Wii(), target.wave_archive, target.wave);
        const uint16_t version = edit::PresentWave(*played_.Wii(), target.wave_archive, target.wave).version;
        incoming = wave_file ? edit::ReadIncomingWiiWaveFile(wav)
                             : edit::ReadIncomingWiiWave(wav, version, encoding, loop, rate);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace " + wave_name + " with " + name + ". " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached. The Replace window says why it can't be made.
    ReplaceSource source = SourceAt(history_->Current(), played_.Wii(), target.wave_archive, name);
    auto [result, error] = TryEdit(
        [&]
        {
            return edit::WithIncomingWiiWave(*source.file, *played_.Wii(), target.wave_archive, target.wave, incoming);
        });
    Replacement replacement{std::move(source), {}, std::nullopt, target.wave, std::move(wav), std::nullopt, {}};
    replacement.view = MakeWiiWaveReplaceView(replacement.source, target.wave, incoming, !wave_file, changes,
                                              std::move(result), std::move(error));

    return replacement;
}

void App::OpenStreamReplacement(const std::string& path)
{
    const std::optional<uint32_t> sound = std::exchange(replace_.stream, std::nullopt);
    if (!sound || !history_ || (!played_.Sar() && !played_.Wii()))
    {
        return;
    }

    // A WAV file goes in as DSP-ADPCM until the user chooses another encoding in the Replace window.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!ReadChosenFile(file, bytes))
    {
        return;
    }

    replace_.shown = StreamReplacement(std::move(bytes), name, *sound, ntdlib::sar::WaveEncoding::kDspAdpcm);
}

std::optional<Replacement> App::StreamReplacement(std::vector<uint8_t> bytes, const std::string& name, uint32_t sound,
                                                  ntdlib::sar::WaveEncoding encoding)
{
    if (played_.Wii())
    {
        return WiiStreamReplacement(std::move(bytes), name, sound, encoding);
    }

    // A sound that plays an AAC stream takes an AAC stream.
    std::optional<edit::AacChanges> aac;
    try
    {
        aac = edit::ChangesOfAacStream(*history_->Current(), *played_.Sar(), sound);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace the stream of " + played_.Sar()->Sounds().at(sound).name + " with " + name + ". " +
                    format::AsSentence(error.what()));
        return std::nullopt;
    }
    if (aac)
    {
        return AacStreamReplacement(std::move(bytes), name, sound, *aac);
    }

    // The stream of the stream file or the WAV file, and the stream that it replaces.
    const bool stream_file = edit::IsStreamFile(bytes);
    const ntdlib::sar::Flavor flavor = played_.Sar()->ReadOptions().flavor;
    edit::IncomingStream incoming;
    edit::StreamChanges changes;
    std::optional<std::vector<ntdlib::sar::StreamFile::Track>> given;
    try
    {
        const uint32_t version = edit::IncomingStreamVersion(*history_->Current(), flavor);
        incoming = stream_file ? edit::ReadIncomingStreamFile(bytes, flavor, version)
                               : edit::ReadIncomingStreamWav(bytes, flavor, version, encoding);
        changes = edit::ChangesOfStream(*history_->Current(), *played_.Sar(), sound);
        given = edit::SoundTracksFor(incoming, changes);
        incoming = edit::WithSoundTracks(std::move(incoming), changes);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace the stream of " + played_.Sar()->Sounds().at(sound).name + " with " + name + ". " +
                    format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached.
    ReplaceSource source = SourceAt(history_->Current(), played_.Sar(), sound, name);
    auto [result, error] = TryEdit([&] { return edit::ReplaceStream(*source.file, changes.file, incoming.bytes); });
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.stream = changes.file;
    replacement.view = MakeStreamReplaceView(replacement.source, incoming, !stream_file, changes, given,
                                             std::move(result), std::move(error));

    return replacement;
}

std::optional<Replacement> App::AacStreamReplacement(std::vector<uint8_t> bytes, const std::string& name,
                                                     uint32_t sound, const edit::AacChanges& changes)
{
    // The system's encoder makes the new stream, unless an AAC file of the present rate and channels came in.
    edit::IncomingAac incoming;
    try
    {
        incoming = edit::ReadIncomingAac(bytes, changes);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace the stream of " + played_.Sar()->Sounds().at(sound).name + " with " + name + ". " +
                    format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached.
    ReplaceSource source = SourceAt(history_->Current(), played_.Sar(), sound, name);
    auto [result, error] = TryEdit([&] { return edit::ReplaceAacStream(*source.file, changes, incoming); });
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.stream = changes.file;
    replacement.view =
        MakeAacStreamReplaceView(replacement.source, incoming, changes, std::move(result), std::move(error));

    // Play plays the new stream decoded, with its loop.
    try
    {
        replacement.view.wave = song::PlayableStream(incoming.bytes, song::AacStream{incoming.loop});
    }
    catch (const std::exception&)
    {
        replacement.view.wave.clear();
    }

    return replacement;
}

bool App::PlaysAacStream(uint32_t sound) const
{
    if (!played_.Sar() || !history_ || !history_->Current()->native)
    {
        return false;
    }

    const std::optional<uint32_t> stream = song::StreamFileOf(*played_.Sar(), sound);
    const auto& streams = history_->Current()->native->streams;
    const auto found = stream ? streams.find(*stream) : streams.end();
    return stream && song::AacStreamOf(*played_.Sar(), sound) &&
           (found == streams.end() || song::IsAdts(*found->second.bytes));
}

std::optional<Replacement> App::WiiStreamReplacement(std::vector<uint8_t> bytes, const std::string& name,
                                                     uint32_t sound, ntdlib::sar::WaveEncoding encoding)
{
    // A stream file of the Wii goes in as it is, and one of a later console converted. A WAV file goes in as a stream
    // that NW4R's tools would lay out. Either of the last two takes the present stream's tracks where they fit.
    const bool stream_file = edit::IsWiiStreamFile(bytes) || edit::IsStreamFile(bytes);
    edit::IncomingStream incoming;
    edit::StreamChanges changes;
    try
    {
        changes = edit::ChangesOfWiiStream(*history_->Current(), *played_.Wii(), sound);
        incoming = stream_file ? edit::ReadIncomingWiiStreamFile(bytes, changes.wii_tracks)
                               : edit::ReadIncomingWiiStreamWav(bytes, encoding, changes.wii_tracks);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace the stream of " + played_.Wii()->Sounds().at(sound).name + " with " + name + ". " +
                    format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached.
    ReplaceSource source = SourceAt(history_->Current(), played_.Wii(), sound, name);
    auto [result, error] = TryEdit([&] { return edit::ReplaceStream(*source.file, changes.file, incoming.bytes); });
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.stream = changes.file;
    replacement.view = MakeWiiStreamReplaceView(replacement.source, incoming, !stream_file, changes, std::move(result),
                                                std::move(error));

    return replacement;
}

} // namespace hokuto::app
