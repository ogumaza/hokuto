// SPDX-License-Identifier: MIT

// Replace… of an item of a file of the DS, DSE, the GBA or another of the GBA's drivers.

#include <cstddef>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "edit/ds_streams.h"
#include "edit/ds_waves.h"
#include "edit/gba_voices.h"
#include "edit/konami_samples.h"
#include "edit/midi_imports.h"
#include "edit/replace.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/items.h"
#include "song/konami.h"

namespace hokuto::app
{

void App::StartGbaReplace(const BrowserPlace& place)
{
    // A song is replaced with a MIDI file or a song as assembly, and a voice group with a voice group as assembly. The
    // file comes from the dialog. The program reads it in a later frame.
    const auto group = static_cast<std::size_t>(place.group);
    const uint32_t number = browser_[group].items[static_cast<std::size_t>(place.item)].number;
    replace_.ForgetTargets();
    if (group == kSequencesGroup)
    {
        replace_.sound = number;
    }
    else if (group == kBanksGroup)
    {
        replace_.file = FileTarget{edit::FileKind::kBank, number};
    }
    else
    {
        return;
    }

    ShowDialog(Dialog::kReplace);
}

void App::StartDsReplace(const BrowserPlace& place)
{
    // The file comes from the dialog. The program reads it in a later frame.
    const auto group = static_cast<std::size_t>(place.group);
    const auto index = static_cast<std::size_t>(place.item);
    const uint32_t number = browser_[group].items[index].number;
    replace_.ForgetTargets();
    if (place.child >= 0)
    {
        replace_.wave =
            WaveTarget{number, browser_[group].items[index].child_numbers[static_cast<std::size_t>(place.child)]};
    }
    else if (group == kSequencesGroup)
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
    else if (group == kSoundsGroup)
    {
        replace_.stream = number;
    }
    else
    {
        return;
    }

    ShowDialog(Dialog::kReplace);
}

void App::StartDseReplace(const BrowserPlace& place)
{
    // The file comes from the dialog. The program reads it in a later frame. A bank with samples is replaced with a
    // wave bank file, like a bank of programs.
    const auto group = static_cast<std::size_t>(place.group);
    const auto index = static_cast<std::size_t>(place.item);
    const uint32_t number = browser_[group].items[index].number;
    replace_.ForgetTargets();
    if (place.child >= 0 && group == kWavesGroup)
    {
        replace_.wave =
            WaveTarget{number, browser_[group].items[index].child_numbers[static_cast<std::size_t>(place.child)]};
    }
    else if (place.child >= 0)
    {
        return;
    }
    else if (group == kSequencesGroup)
    {
        replace_.sound = number;
    }
    else if (group == kBanksGroup || group == kWavesGroup)
    {
        replace_.file = FileTarget{edit::FileKind::kBank, number};
    }
    else
    {
        return;
    }

    ShowDialog(Dialog::kReplace);
}

namespace
{

// Reads `bytes` into `source`, an item of `set`: a bank file where `bank_file` is true, or else a MIDI file or a song
// file. Throws for a file that can't be read.
void ReadDseIncoming(ReplaceSource& source, const song::DseSet& set, std::vector<uint8_t> bytes, bool bank_file)
{
    if (bank_file)
    {
        source.dse_bank = edit::ReadIncomingDseBank(bytes);
    }
    else if (edit::IsMidi(bytes))
    {
        source.midi_drums = edit::DrumNotesOf(bytes);
        source.midi = std::move(bytes);
        if (const std::optional<uint32_t> bank = DseFirstBank(set, source.sound))
        {
            ProposeDseMidiMap(source, *bank);
        }
        else
        {
            ReadDseMidi(source);
        }
    }
    else
    {
        source.dse_song = edit::ReadIncomingDseSong(bytes);
    }
}

} // namespace

void App::OpenDseReplacement(const std::string& path)
{
    const std::optional<uint32_t> sound = std::exchange(replace_.sound, std::nullopt);
    const std::optional<FileTarget> target = std::exchange(replace_.file, std::nullopt);
    const std::optional<WaveTarget> wave = std::exchange(replace_.wave, std::nullopt);
    replace_.stream.reset();
    if (!history_ || !played_.Dse() || (!sound && !target && !wave))
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

    FollowArchiveVersion();
    if (wave)
    {
        replace_.shown = DseWaveReplacement(std::move(bytes), name, *wave);
        return;
    }

    // The Replace window starts from the version that the history has reached. A MIDI file's programs take the
    // pairings of the map of the sound's first bank.
    ReplaceSource source = SourceAt(history_->Current(), played_.Dse(), target ? target->item : *sound, name);
    source.incoming.size = bytes.size();
    const std::string item = target ? song::DseItemName(*played_.Dse(), {song::ItemKind::kBank, source.sound})
                                    : song::DseSoundName(*played_.Dse(), source.sound);
    try
    {
        ReadDseIncoming(source, *played_.Dse(), std::move(bytes), target.has_value());
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace " + item + " with " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    Replacement replacement{
        std::move(source), {}, target ? std::optional(target->kind) : std::nullopt, std::nullopt, {}, std::nullopt, {}};
    try
    {
        replacement.view =
            target ? MakeDseBankReplaceView(replacement.source) : MakeDseSoundReplaceView(replacement.source);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace " + item + " with " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    replace_.shown = std::move(replacement);
}

std::optional<Replacement> App::DseWaveReplacement(std::vector<uint8_t> bytes, const std::string& name,
                                                   const WaveTarget& target,
                                                   std::optional<ntdlib::sar::WaveEncoding> encoding,
                                                   std::optional<uint32_t> rate, std::optional<edit::WaveLoop> loop)
{
    // The wave of the WAV file, and the current wave and what plays it.
    const song::Item item{song::ItemKind::kWave, target.wave_archive, target.wave};
    edit::IncomingWave incoming;
    edit::WaveChanges changes;
    try
    {
        changes = edit::ChangesOfDseWave(*played_.Dse(), target.wave_archive, target.wave);
        incoming = edit::ReadIncomingDsWave(bytes, encoding.value_or(changes.encoding), loop, rate);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace " + song::DseItemName(*played_.Dse(), item) + " with " + name + ". " +
                    format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached. The Replace window says why it can't be made.
    ReplaceSource source = SourceAt(history_->Current(), played_.Dse(), target.wave_archive, name);
    auto [result, error] = TryEdit(
        [&]
        {
            return edit::ReplaceDseWave(*source.file, *played_.Dse(), target.wave_archive, target.wave,
                                        {.wave = incoming.bytes, .rate = incoming.rate});
        });
    Replacement replacement{std::move(source), {}, std::nullopt, target.wave, std::move(bytes), std::nullopt, {}};
    replacement.view =
        MakeDseWaveReplaceView(replacement.source, target.wave, incoming, changes, std::move(result), std::move(error));

    return replacement;
}

void App::FollowArchiveVersion()
{
    const std::shared_ptr<const format::File> version = history_->Current();
    if (!archive_version_ || !SameArchive(*version, *archive_version_))
    {
        BuildArchive(version);
    }
}

void App::OpenDriverReplacement(const std::string& path)
{
    const std::optional<WaveTarget> wave = std::exchange(replace_.wave, std::nullopt);
    if (!history_ || !wave)
    {
        return;
    }

    // The WAV file replaces the sample at once. The history records it as one action.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!ReadChosenFile(file, bytes))
    {
        return;
    }

    try
    {
        FollowArchiveVersion();
        const song::KonamiRom* rom = song::KonamiOf(played_.Driver().get());
        if (rom == nullptr)
        {
            return;
        }
        const auto sample = static_cast<int>(wave->wave);
        format::File replaced =
            edit::WithKonamiSample(*Document(), *rom, sample, edit::ReadIncomingKonamiSample(bytes));
        Do("Replace Sample", rom->SampleName(sample) + " with " + name, std::move(replaced));
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace the sample with " + name + ". " + format::AsSentence(error.what()));
    }
}

void App::OpenDsReplacement(const std::string& path)
{
    const std::optional<uint32_t> sound = std::exchange(replace_.sound, std::nullopt);
    const std::optional<FileTarget> target = std::exchange(replace_.file, std::nullopt);
    const std::optional<WaveTarget> wave = std::exchange(replace_.wave, std::nullopt);
    const std::optional<uint32_t> stream = std::exchange(replace_.stream, std::nullopt);
    if (!history_ || !played_.Ds() || (!sound && !target && !wave && !stream))
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

    FollowArchiveVersion();
    if (wave)
    {
        replace_.shown = DsWaveReplacement(std::move(bytes), name, *wave, std::nullopt);
        return;
    }
    if (stream)
    {
        replace_.shown = DsStreamReplacement(std::move(bytes), name, *stream, std::nullopt);
        return;
    }

    // The Replace window starts from the version that the history has reached. A MIDI file is imported as a sequence
    // file of the DS.
    ReplaceSource source = SourceAt(history_->Current(), played_.Ds(), target ? target->item : *sound, name);
    if (!target && edit::IsMidi(bytes))
    {
        try
        {
            source.midi_drums = edit::DrumNotesOf(bytes);
            source.midi = std::move(bytes);
            if (const std::optional<uint32_t> bank = song::DsBankOf(*played_.Ds(), source.sound))
            {
                ProposeDsMidiMap(source, *bank);
            }
            bytes = ReadDsMidi(source);
        }
        catch (const std::invalid_argument& error)
        {
            ShowMessage("Can't replace the sequence of " + song::DsSoundName(*played_.Ds(), *sound) + " with " + name +
                        ". " + format::AsSentence(error.what()));
            return;
        }
    }
    Replacement replacement{
        std::move(source), {}, target ? std::optional(target->kind) : std::nullopt, std::nullopt, {}, std::nullopt, {}};
    try
    {
        replacement.view = target ? MakeDsFileReplaceView(replacement.source, target->kind, bytes)
                                  : MakeDsSequenceReplaceView(replacement.source, bytes);
    }
    catch (const std::exception& error)
    {
        const song::Item item{
            target && target->kind == edit::FileKind::kBank ? song::ItemKind::kBank : song::ItemKind::kWaveArchive,
            replacement.source.sound};
        ShowMessage("Can't replace " +
                    (target ? song::DsItemName(*played_.Ds(), item) : song::DsSoundName(*played_.Ds(), *sound)) +
                    " with " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    replace_.shown = std::move(replacement);
}

std::optional<Replacement> App::DsStreamReplacement(std::vector<uint8_t> bytes, const std::string& name,
                                                    uint32_t stream, std::optional<ntdlib::sar::WaveEncoding> encoding,
                                                    std::optional<uint32_t> rate, std::optional<edit::WaveLoop> loop)
{
    // The stream of the WAV file or the stream file, and the current stream.
    const bool stream_file = edit::IsDsStreamFile(bytes);
    edit::IncomingWave incoming;
    edit::WaveChanges changes;
    try
    {
        changes = edit::ChangesOfDsStream(*played_.Ds(), stream);
        incoming = stream_file ? edit::ReadIncomingDsStreamFile(bytes)
                               : edit::ReadIncomingDsStream(bytes, encoding.value_or(changes.encoding), loop, rate);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace " + song::DsStreamName(*played_.Ds(), stream) + " with " + name + ". " +
                    format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached. The Replace window says why it can't be made.
    ReplaceSource source = SourceAt(history_->Current(), played_.Ds(), stream, name);
    auto [result, error] =
        TryEdit([&] { return edit::WithDsStreamBytes(*source.file, *played_.Ds(), stream, incoming.bytes); });
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.stream = stream;
    replacement.view = MakeDsStreamReplaceView(replacement.source, incoming, !stream_file, changes, std::move(result),
                                               std::move(error));

    return replacement;
}

std::optional<Replacement> App::DsWaveReplacement(std::vector<uint8_t> bytes, const std::string& name,
                                                  const WaveTarget& target,
                                                  std::optional<ntdlib::sar::WaveEncoding> encoding,
                                                  std::optional<uint32_t> rate, std::optional<edit::WaveLoop> loop)
{
    // The wave of the WAV file or the wave file, and the current wave and what plays it.
    const bool wave_file = edit::IsDsWaveFile(bytes);
    edit::IncomingWave incoming;
    edit::WaveChanges changes;
    try
    {
        changes = edit::ChangesOfDsWave(*played_.Ds(), target.wave_archive, target.wave);
        incoming = wave_file ? edit::ReadIncomingDsWaveFile(bytes)
                             : edit::ReadIncomingDsWave(bytes, encoding.value_or(changes.encoding), loop, rate);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't replace " +
                    song::DsItemName(*played_.Ds(), {song::ItemKind::kWave, target.wave_archive, target.wave}) +
                    " with " + name + ". " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached. The Replace window says why it can't be made.
    ReplaceSource source = SourceAt(history_->Current(), played_.Ds(), target.wave_archive, name);
    auto [result, error] = TryEdit(
        [&]
        {
            return edit::WithDsWaveBytes(*source.file, *played_.Ds(), target.wave_archive, target.wave, incoming.bytes);
        });
    Replacement replacement{std::move(source), {}, std::nullopt, target.wave, std::move(bytes), std::nullopt, {}};
    replacement.view = MakeDsWaveReplaceView(replacement.source, target.wave, incoming, !wave_file, changes,
                                             std::move(result), std::move(error));

    return replacement;
}

void App::OpenGbaReplacement(const std::string& path)
{
    const std::optional<uint32_t> song = std::exchange(replace_.sound, std::nullopt);
    const std::optional<FileTarget> group = std::exchange(replace_.file, std::nullopt);
    if ((!song && !group) || !history_)
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
    if (song)
    {
        OpenGbaMidiImport(std::move(bytes), name, song);
        return;
    }

    try
    {
        const std::string text(bytes.begin(), bytes.end());
        edit::GbaGroupVersion version = edit::WithGbaVoiceGroupFile(*Document(), *played_.Gba(), group->item, text);
        DoGbaGroupVersion("Replace Voice Group", song::GbaVoiceGroupName(group->item), group->item, std::move(version));
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't add " + name + " to the voice group. " + format::AsSentence(error.what()));
    }
}

} // namespace hokuto::app
