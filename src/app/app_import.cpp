// SPDX-License-Identifier: MIT

// File > Import and Project > Add… of sequences, from the chosen file to the edit. app_import_items.cpp contains the
// imports of banks, sound fonts and other items, and app_import_waves.cpp those of waves and streams.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_import_parts.h"
#include "app/app_internal.h"
#include "edit/converter.h"
#include "edit/ds_streams.h"
#include "edit/ds_waves.h"
#include "edit/groups.h"
#include "edit/midi_imports.h"
#include "edit/new_items.h"
#include "edit/other_console.h"
#include "edit/replace.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/driver.h"
#include "song/items.h"

namespace hokuto::app
{

using import_parts::AddSoundsOfType;
using import_parts::kNewBankName;

namespace
{

// The first name of a sound that Project > Add Sequence Sound… adds.
constexpr char kNewSoundName[] = "SEQ_NEW";

// Picks the sequence sound of `source.models` that a new sequence sound copies: `shown` where the models include it, or
// else the first. Without models, the new sound copies none, and its one bank slot names the first bank of the archive
// where it has `banks`.
void ChooseSequenceModel(ReplaceSource& source, std::optional<uint32_t> shown, bool banks)
{
    if (source.models.empty())
    {
        source.sound = ntdlib::sar::kNoItem;
        source.banks =
            std::vector<uint32_t>{banks ? ntdlib::sar::ItemId(ntdlib::sar::kBankItem, 0) : ntdlib::sar::kNoItem};
    }
    else
    {
        const bool listed = shown && std::ranges::find(source.models, *shown) != source.models.end();
        source.sound = listed ? *shown : source.models.front();
    }
}

// The name and the details in the Undo History of the edit that adds sequence sound `name` from `source`.
std::pair<std::string, std::string> SequenceEditNames(const ReplaceSource& source, const std::string& name)
{
    if (source.new_song)
    {
        return {"Add Sequence Sound", name};
    }

    return {"Import Sequence", name + " from " + source.file_name};
}

} // namespace

void App::OpenDsImport(std::vector<uint8_t> bytes, const std::string& name, edit::DsNewItem kind, bool new_song,
                       edit::Incoming incoming, std::vector<uint8_t> midi)
{
    // The Import window starts from the version that the history has reached. A new sound copies the settings of the
    // sound that the zones show, or else of the archive's first.
    FollowArchiveVersion();
    ReplaceSource source;
    source.file = history_->Current();
    source.played = played_.Ds();
    source.file_name = name;
    source.new_song = new_song;
    source.incoming = std::move(incoming);
    source.midi = std::move(midi);
    source.midi_drums = edit::DrumNotesOf(source.midi);
    source.new_bank = kind == edit::DsNewItem::kBank && name == kNewBankName;
    source.refused = edit::DsAdditionsRefused(*source.file);
    if (kind == edit::DsNewItem::kSequence)
    {
        source.models = song::DsSounds(*played_.Ds());
        const bool shown = sequence_ && std::ranges::find(source.models, static_cast<uint32_t>(sequence_->sound)) !=
                                            source.models.end();
        source.sound = shown                   ? static_cast<uint32_t>(sequence_->sound)
                       : source.models.empty() ? 0
                                               : source.models.front();
    }

    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::nullopt, {}};
    replacement.wav = std::move(bytes);
    replacement.ds_new = kind;
    try
    {
        // A MIDI file's first bank proposes the pairings of its map.
        const std::vector<int> rows = FirstDsRows(replacement.source, kind, replacement.wav);
        const std::vector<uint32_t> banks = song::DsBanks(*played_.Ds());
        if (!replacement.source.midi.empty() && !rows.empty() && rows.front() >= 0 &&
            static_cast<std::size_t>(rows.front()) < banks.size())
        {
            ProposeDsMidiMap(replacement.source, banks[static_cast<std::size_t>(rows.front())]);
            replacement.wav = ReadDsMidi(replacement.source);
        }
        replacement.view =
            MakeDsImportView(replacement.source, kind, replacement.wav, FirstDsName(*played_.Ds(), name), rows);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    replace_.shown = std::move(replacement);
}

std::optional<Replacement> App::DsStreamImport(std::vector<uint8_t> bytes, const std::string& name,
                                               std::optional<uint32_t> model,
                                               std::optional<ntdlib::sar::WaveEncoding> encoding,
                                               std::optional<uint32_t> rate, const std::string& new_name)
{
    const bool stream_file = edit::IsDsStreamFile(bytes);
    edit::IncomingWave incoming;
    try
    {
        incoming = stream_file
                       ? edit::ReadIncomingDsStreamFile(bytes)
                       : edit::ReadIncomingDsStream(bytes, encoding.value_or(ntdlib::sar::WaveEncoding::kImaAdpcm),
                                                    std::nullopt, rate);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The new stream copies the settings of the archive's first stream until the user chooses another.
    ReplaceSource source;
    source.file = history_->Current();
    source.played = played_.Ds();
    source.file_name = name;
    source.refused = edit::DsAdditionsRefused(*source.file);
    source.models = song::DsStreams(*played_.Ds());
    source.sound = model.value_or(source.models.empty() ? 0 : source.models.front());
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.ds_new = edit::DsNewItem::kStream;
    replacement.view = MakeDsStreamImportView(replacement.source, incoming, !stream_file, new_name);

    return replacement;
}

std::optional<Replacement> App::DsWaveImport(std::vector<uint8_t> bytes, const std::string& name, uint32_t wave_archive,
                                             std::optional<ntdlib::sar::WaveEncoding> encoding,
                                             std::optional<uint32_t> rate)
{
    // The wave of the WAV file or the wave file.
    const bool wave_file = edit::IsDsWaveFile(bytes);
    edit::IncomingWave incoming;
    try
    {
        incoming = wave_file ? edit::ReadIncomingDsWaveFile(bytes)
                             : edit::ReadIncomingDsWave(bytes, encoding.value_or(ntdlib::sar::WaveEncoding::kImaAdpcm),
                                                        std::nullopt, rate);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached. The Import window says why it can't be made.
    ReplaceSource source = SourceAt(history_->Current(), played_.Ds(), wave_archive, name);
    auto [result, error] = TryEdit(
        [&]
        {
            return edit::WithDsWaveBytes(*source.file, *played_.Ds(), wave_archive,
                                         static_cast<uint32_t>(song::DsWaveCount(*played_.Ds(), wave_archive)),
                                         incoming.bytes);
        });

    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::nullopt, {}};
    replacement.new_wave = true;
    replacement.wav = std::move(bytes);
    replacement.view = MakeDsNewWaveView(replacement.source, incoming, !wave_file, std::move(result), std::move(error));

    return replacement;
}

void App::StartSequenceImport()
{
    // The new sound copies the settings of a sequence sound. Without one, a file in the model form and a file of the
    // Wii give it Hokuto's settings. An archive in the game's layout can't take it, and the import says so before the
    // dialog.
    if (!IsEnabled(Command::kImportSequence))
    {
        return;
    }
    if (played_.Sar() && history_->Current()->form != format::ArchiveForm::kModel &&
        !HoldsSoundOf(*played_.Sar(), {ntdlib::sar::SoundType::kSequence}))
    {
        ShowMessage(
            "Can't import a sequence here. A new sound copies the settings of an existing sequence sound, and this "
            "archive has none.");
        return;
    }

    ShowDialog(Dialog::kImportSequence);
}

void App::OpenSequenceImport(const std::string& path)
{
    // A file of DSE or another of the GBA's drivers accepts no MIDI file, and a drop says so. A busy program ignores
    // the drop.
    if (!IsEnabled(Command::kImportSequence))
    {
        if (played_.Dse() || played_.Driver())
        {
            ShowMessage(
                "Can't import " + xsflib::Utf8(xsflib::PathFromUtf8(path).filename()) +
                ". Hokuto doesn't import MIDI files into " +
                (played_.Driver() ? "songs of " + std::string(played_.Driver()->Rules().Title()) : "sounds of DSE") +
                ".");
        }
        return;
    }

    // The file, read as the import reads it.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!ReadChosenFile(file, bytes))
    {
        return;
    }

    if (played_.Gba())
    {
        OpenGbaMidiImport(std::move(bytes), name);
        return;
    }
    if (played_.Ds() && edit::IsMidi(bytes))
    {
        try
        {
            edit::DsMidiFile midi = edit::DsFileOfMidi(bytes, name);
            edit::Incoming incoming;
            incoming.midi = true;
            incoming.size = bytes.size();
            incoming.sequence = std::move(midi.song);
            incoming.warnings = std::move(midi.warnings);
            OpenDsImport(std::move(midi.file), name, edit::DsNewItem::kSequence, false, std::move(incoming),
                         std::move(bytes));
        }
        catch (const std::invalid_argument& error)
        {
            ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
        }
        return;
    }
    if (played_.Ds())
    {
        OpenDsImport(std::move(bytes), name, edit::DsNewItem::kSequence);
        return;
    }

    // A file of the Wii takes a sequence file by the rules of its build of NW4R SND, and converts one of another
    // console. A file of another console takes the biquad filters that come nearest on the archive's console. A MIDI
    // file becomes the console's commands.
    ReplaceSource source;
    try
    {
        ReadIncomingSequence(source, std::move(bytes), name, *history_->Current());
    }
    catch (const std::invalid_argument& error)
    {
        ShowMessage("Can't import " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    OpenSequenceSource(std::move(source), name, "Can't import " + name + ".");
}

void App::OpenGbaMidiImport(std::vector<uint8_t> midi, const std::string& name, std::optional<uint32_t> first)
{
    // The window offers the songs that play: the song that the browser's Replace… names, or else the shown song, first.
    Replacement replacement;
    replacement.source.file = history_->Current();
    replacement.source.places = Places();
    replacement.source.file_name = name;
    replacement.gba_songs = song::GbaSongs(*played_.Gba());
    if (!first && sequence_)
    {
        first = sequence_->sound;
    }
    const auto chosen =
        first ? std::ranges::find(replacement.gba_songs, static_cast<int>(*first)) : replacement.gba_songs.end();
    const auto song =
        static_cast<std::size_t>(chosen != replacement.gba_songs.end() ? chosen - replacement.gba_songs.begin() : 0);
    replacement.source.sound = static_cast<uint32_t>(replacement.gba_songs.at(song));
    replacement.view = MakeGbaMidiImportView(replacement.source, midi, *played_.Gba(), replacement.gba_songs, song, {});
    replacement.midi = std::move(midi);
    replace_.shown = std::move(replacement);
}

void App::StartSoundAddition()
{
    if (!IsEnabled(Command::kAddSequenceSound))
    {
        return;
    }
    if (played_.Ds())
    {
        OpenDsImport(edit::EmptyDsSequenceFile(), kNewSoundName, edit::DsNewItem::kSequence, true);
        return;
    }

    ReplaceSource source;
    source.incoming.flavor = played_.Sar() ? played_.Sar()->ReadOptions().flavor : ntdlib::sar::Flavor::kNx;
    source.incoming.wii = played_.Wii() != nullptr;
    source.incoming.sequence = edit::EmptySong(kNewSoundName);
    source.new_song = true;
    OpenSequenceSource(std::move(source), kNewSoundName, "Can't add a sound.");
}

void App::OpenSequenceSource(ReplaceSource source, const std::string& name, const std::string& what)
{
    if (played_.Wii())
    {
        OpenWiiSequenceSource(std::move(source), name);
        return;
    }

    // The new sound copies the settings of the sequence sound that the zones show, or else of the archive's first
    // sequence sound. In a file in the model form without one, it takes Hokuto's settings, and its one bank slot the
    // archive's first bank.
    AddSoundsOfType(source.models, played_.Sar()->Sounds(), ntdlib::sar::SoundType::kSequence);
    if (source.models.empty() && history_->Current()->form != format::ArchiveForm::kModel)
    {
        ShowMessage(what + " The archive has no sequence sound for the new sound to copy.");
        return;
    }

    ChooseSequenceModel(source, sequence_ ? std::optional(sequence_->sound) : std::nullopt,
                        !played_.Sar()->Banks().empty());

    // The Import window starts from the version that the history has reached. Where the archive's sounds play gives the
    // window their starts in a file that holds their commands.
    source.file = history_->Current();
    source.played = played_.Sar();
    source.file_name = name;
    if (start_fields_)
    {
        source.places = edit::SoundPlaces(*played_.Sar(), file_sequences_, *start_fields_);
    }
    FindSoundStarts(source);
    FindReaders(source);
    FindGroups(source);
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::nullopt, {}};
    const edit::ProgramMap programs = ProposeMidiMap(replacement.source);
    replacement.view = MakeImportView(replacement.source, std::nullopt, programs, {},
                                      FirstName(*replacement.source.file, *played_.Sar(), name));
    replace_.shown = std::move(replacement);
}

void App::OpenWiiSequenceSource(ReplaceSource source, const std::string& name)
{
    // The new sound copies the settings of the sequence sound that the zones show, or else of the archive's first
    // sequence sound. Without one, it takes Hokuto's settings, and its bank slot the archive's first bank.
    AddSoundsOfType(source.models, played_.Wii()->Sounds(), ntdlib::rsar::SoundType::kSequence);
    ChooseSequenceModel(source, sequence_ ? std::optional(sequence_->sound) : std::nullopt,
                        !played_.Wii()->Banks().empty());

    // The Import window starts from the version that the history has reached. Where the archive's sounds play gives the
    // window their starts in a file that holds their commands. The new sound joins groups with names. An archive that
    // NW4R's tools didn't lay out takes no new sound, and the window says so from the start.
    source.file = history_->Current();
    source.played = played_.Wii();
    source.file_name = name;
    if (start_fields_)
    {
        source.places = edit::SoundPlaces(*played_.Wii(), file_sequences_, *start_fields_);
    }
    FindSoundStarts(source);
    FindReaders(source);
    source.refused = FindGroups(source);
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::nullopt, {}};
    const edit::ProgramMap programs = ProposeMidiMap(replacement.source);
    replacement.view =
        MakeWiiImportView(replacement.source, std::nullopt, programs, {}, FirstWiiName(replacement.source, name));
    replace_.shown = std::move(replacement);
}

void App::MakeWiiImport()
{
    // The edit builds the archive again with the new sound. The browser then selects the new sound. An empty song takes
    // the sound's name. A converted sequence gives the global variables the later consoles' starting value. The Import
    // window showed that value.
    const ReplaceSource& source = replace_.shown->source;
    const ReplaceView& view = replace_.shown->view;
    const std::string name = NameOf(view);
    const std::optional<uint32_t> model = source.models.empty() ? std::nullopt : std::optional<uint32_t>(source.sound);
    const auto added = static_cast<uint32_t>(source.played.Wii()->Sounds().size());
    const auto make = [file = source.file, model, name,
                       sequence = source.new_song ? edit::EmptySong(name) : source.incoming.sequence,
                       start = view.starts[view.chosen].item, programs = ChosenPrograms(view),
                       variables = ChosenVariables(view), banks = source.banks, groups = view.joined,
                       converted = source.incoming.converted, maps = MidiMapsOf(view, source, {})]
    {
        edit::MappedVariables mapped = edit::WithVariables(sequence, start, variables);
        if (converted)
        {
            mapped.start = edit::WithLaterGlobals(mapped.sequence, mapped.start).first;
        }

        format::File made =
            edit::AddWiiSequenceSound(*file, model, name, mapped.sequence, mapped.start, programs, banks, groups);
        KeepMidiMaps(made, maps);
        return made;
    };
    auto [action, details] = SequenceEditNames(source, name);
    StartEdit(make, std::move(action), std::move(details), added);
}

void App::MakeImport()
{
    if (played_.Wii())
    {
        MakeWiiImport();
        return;
    }

    // The edit builds the archive again with the new sound. The browser then selects the new sound. An empty song takes
    // the sound's name.
    const ReplaceSource& source = replace_.shown->source;
    const ReplaceView& view = replace_.shown->view;
    const std::string name = NameOf(view);
    const std::optional<uint32_t> model = source.models.empty() ? std::nullopt : std::optional<uint32_t>(source.sound);

    // The new sound joins the chosen groups. A group that loads the model takes only the files that the model doesn't
    // require (edit::JoinGroups).
    const auto added = static_cast<uint32_t>(source.file->native->tables->sounds.size());
    const auto make =
        [file = source.file, model, name, sequence = source.new_song ? edit::EmptySong(name) : source.incoming.sequence,
         start = view.starts[view.chosen].item, programs = ChosenPrograms(view), variables = ChosenVariables(view),
         banks = source.banks, groups = view.joined, new_groups = view.new_groups, added,
         before = model ? edit::FilesOfItem(*source.played.Sar(), ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, *model))
                        : std::vector<uint32_t>(),
         maps = MidiMapsOf(view, source, {})]
    {
        const edit::MappedVariables mapped = edit::WithVariables(sequence, start, variables);
        const uint32_t item = ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, added);
        format::File made =
            edit::JoinGroups(edit::AddSequenceSound(*file, model, name, mapped.sequence, mapped.start, programs, banks),
                             groups, std::span(&item, 1), before, new_groups);
        KeepMidiMaps(made, maps);
        return made;
    };
    auto [action, details] = SequenceEditNames(source, name);
    StartEdit(make, std::move(action), std::move(details), added);
}

} // namespace hokuto::app
