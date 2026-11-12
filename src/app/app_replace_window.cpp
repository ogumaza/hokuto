// SPDX-License-Identifier: MIT

// The Replace window: its results, the work of a confirmed import, and the previews of its Play.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <future>
#include <span>
#include <utility>

#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/sar/decode.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "edit/converter.h"
#include "edit/effects.h"
#include "edit/gba_voices.h"
#include "edit/groups.h"
#include "edit/midi_imports.h"
#include "edit/new_items.h"
#include "edit/replace.h"
#include "edit/streams.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/items.h"
#include "song/preview.h"
#include "song/sequence_preview.h"

namespace hokuto::app
{

void App::TakeDonePreview()
{
    if (replace_.preview_work && !replace_.shown)
    {
        replace_.dropped_previews.push_back(std::move(*replace_.preview_work));
        replace_.preview_work.reset();
    }
    else if (replace_.preview_work && Done(*replace_.preview_work))
    {
        PreviewWork work = replace_.preview_work->get();
        replace_.preview_work.reset();
        replace_.shown->view.preparing = false;
        Wake();
        if (work.renderer)
        {
            PlayReplacePreview(std::move(work.renderer));
        }
        else if (!work.note.empty())
        {
            ShowMessage(work.note, MessageKind::kNote);
        }
        else
        {
            ShowMessage("Can't play the preview. " + format::AsSentence(work.error));
        }
    }
}

void App::FinishPendingEdit()
{
    EditWork work = replace_.shown->pending->work.get();
    PendingEdit pending = std::move(*replace_.shown->pending);
    replace_.shown->pending.reset();
    Wake();
    if (work.result)
    {
        if (pending.select)
        {
            select_sound_ = pending.select;
        }
        bank_to_open_ = pending.open_bank;
        replace_.shown.reset();
        Do(std::move(pending.action), std::move(pending.details), std::move(*work.result), std::nullopt,
           std::move(pending.replaced));
    }
    else
    {
        ReplaceView& view = replace_.shown->view;
        view.error = std::move(work.error);
        view.warnings.clear();
        view.ready = false;
        view.working.clear();
    }
}

void App::ApplyReplaceChoice(ReplaceChoice choice)
{
    // A SoundFont in a GBA file: another rate converts the presets again, and another song finds another voice group's
    // programs. Import makes the edit. `source.sound` contains the chosen song.
    if (replace_.shown->sound_font && played_.Gba())
    {
        ApplyGbaSoundFontChoice(choice);
        return;
    }

    // A SoundFont: another name makes the view again, and Import makes the edit.
    if (replace_.shown->sound_font)
    {
        ApplySoundFontChoice(choice);
        return;
    }

    // A MIDI file in a GBA file: Play plays the chosen song from the new commands. Another voice makes the view again.
    // So does another song: its voice group keeps the chosen voices only if it's the same voice group. Import makes the
    // edit. `source.sound` contains the chosen song.
    if (!replace_.shown->gba_songs.empty())
    {
        ApplyGbaMidiChoice(choice);
        return;
    }

    // A file of DSE's files: Play plays the new wave, or a sound from the version that the edit makes. Another
    // encoding, rate or loop makes the new wave again. A row's new choice makes the view again, and a MIDI file's
    // drum notes give new tracks.
    if (played_.Dse())
    {
        ApplyDseChoice(choice);
        return;
    }

    // A file of the DS: Play plays the new wave, or a sound from the version that the edit makes. Another encoding,
    // rate or loop makes the new wave again.
    if (played_.Ds())
    {
        ApplyDsChoice(choice);
        return;
    }

    ApplyArchiveChoice(choice);
}

void App::DrawReplace()
{
    // A dropped import's or preview's work goes once it's done. Once its work is done, a preview plays if the window
    // that started it is still open.
    std::erase_if(replace_.dropped_edits, Done<EditWork>);
    std::erase_if(replace_.dropped_previews, Done<PreviewWork>);
    TakeDonePreview();

    // A confirmed import's version goes into the history once its work is done, and the browser then selects its new
    // sound. The window stays open with the reason when the edit fails. The end of the work counts as an event: the
    // window draws on while the browser and the zones follow the new version.
    if (replace_.shown && replace_.shown->pending && Done(replace_.shown->pending->work))
    {
        FinishPendingEdit();
    }

    // The window's Play ends as the window closes: after Cancel, after the edit, and when another file opens.
    if (!replace_.shown)
    {
        if (replace_.previewing)
        {
            StopPreview();
        }
        CloseReplaceWindow();

        // The next window records its top anew. A new view of an open window keeps it.
        replace_.top.reset();
        return;
    }

    // A preview that has played to its end no longer shows as playing, and Play reads Play again.
    if (replace_.previewing && !worker_.Playing() && ring_.Available() == 0)
    {
        replace_.previewing = false;
    }
    replace_.shown->view.previewing = replace_.previewing;

    // A change of the start, of a bank slot, of a program, of the tracks or of a wave's encoding makes the view again.
    // Replace makes the edit. While an import's work runs, the window takes only Cancel. Stop ends the preview.
    const ReplaceChoice choice = DrawReplaceWindow(replace_.shown->view, replace_.place, replace_.top);
    if (choice == ReplaceChoice::kStop)
    {
        StopPreview();
        return;
    }

    if (replace_.preview_work && choice != ReplaceChoice::kNone && choice != ReplaceChoice::kPlay)
    {
        replace_.dropped_previews.push_back(std::move(*replace_.preview_work));
        replace_.preview_work.reset();
        replace_.shown->view.preparing = false;
    }
    if (choice == ReplaceChoice::kCancel)
    {
        replace_.Drop();
        return;
    }

    if (std::exchange(replace_.shown->view.groups_changed, false))
    {
        replace_.shown->source.joined = replace_.shown->view.joined;
        replace_.shown->source.new_groups = replace_.shown->view.new_groups;
    }
    if (replace_.shown->pending)
    {
        return;
    }

    ApplyReplaceChoice(choice);
}

void App::ApplyGbaSoundFontChoice(ReplaceChoice choice)
{
    ReplaceSource& source = replace_.shown->source;
    ReplaceView& view = replace_.shown->view;
    if (choice == ReplaceChoice::kChange)
    {
        source.sound = static_cast<uint32_t>(replace_.shown->gba_songs.at(view.gba_song));
        if (view.rate != replace_.shown->gba_sound_font_rate)
        {
            try
            {
                replace_.shown->gba_sound_font =
                    std::make_shared<const edit::GbaSoundFontVoices>(edit::GbaVoicesOfSoundFont(
                        *replace_.shown->sound_font, played_.Gba()->info,
                        view.rate == 0 ? std::nullopt : std::optional<uint32_t>(view.rates.at(view.rate))));
                replace_.shown->gba_sound_font_rate = view.rate;
            }
            catch (const std::exception& error)
            {
                ShowMessage("Can't resample to that rate. " + format::AsSentence(error.what()));
            }
        }
        replace_.shown->view =
            MakeGbaSoundFontImportView(source, *replace_.shown->gba_sound_font, *played_.Gba(),
                                       replace_.shown->gba_songs, view.gba_song, replace_.shown->gba_sound_font_rate);
        replace_.shown->view.warnings.insert(replace_.shown->view.warnings.begin(),
                                             replace_.shown->sound_font_warnings.begin(),
                                             replace_.shown->sound_font_warnings.end());
    }
    else if (choice == ReplaceChoice::kReplace && view.result)
    {
        const std::optional<uint32_t> group = edit::GbaVoiceGroupOf(*source.file, source.places, source.sound);
        const uint32_t from = group.value_or(view.gba_group);
        std::string details = source.file_name + " on " + song::GbaVoiceGroupName(from);
        edit::GbaGroupVersion version{std::move(*view.result), view.gba_group};
        const std::string title = view.title;
        replace_.shown.reset();
        DoGbaGroupVersion(title, std::move(details), from, std::move(version));
    }
}

void App::ApplySoundFontChoice(ReplaceChoice choice)
{
    if (choice == ReplaceChoice::kChange)
    {
        replace_.shown->view = MakeSoundFontImportView(replace_.shown->source, *replace_.shown->sound_font_import,
                                                       NameOf(replace_.shown->view));
        replace_.shown->view.warnings.insert(replace_.shown->view.warnings.begin(),
                                             replace_.shown->sound_font_warnings.begin(),
                                             replace_.shown->sound_font_warnings.end());
    }
    else if (choice == ReplaceChoice::kPlay)
    {
        PlayMapping();
    }
    else if (choice == ReplaceChoice::kReplace && replace_.shown->view.ready)
    {
        std::string details = NameOf(replace_.shown->view) + " from " + replace_.shown->source.file_name;
        std::string title = replace_.shown->view.title;
        StartEdit(replace_.SoundFontEdit(true), std::move(title), std::move(details), std::nullopt);
    }
}

void App::ApplyGbaMidiChoice(ReplaceChoice choice)
{
    ReplaceSource& source = replace_.shown->source;
    const ReplaceView& view = replace_.shown->view;
    if (choice == ReplaceChoice::kPlay && view.result)
    {
        PreviewTarget target;
        target.sound = source.sound;
        target.mixing = format::MixingOf(*source.file, source.sound);
        StartPreview([result = *view.result] { return result; }, target);
    }
    else if (choice == ReplaceChoice::kChange)
    {
        const auto song = static_cast<uint32_t>(replace_.shown->gba_songs.at(view.gba_song));
        const bool same = edit::GbaVoiceGroupOf(*source.file, source.places, song) ==
                          edit::GbaVoiceGroupOf(*source.file, source.places, source.sound);
        const edit::GbaVoiceMap voices = same ? ChosenGbaVoices(view) : edit::GbaVoiceMap();
        source.sound = song;
        replace_.shown->view = MakeGbaMidiImportView(source, replace_.shown->midi, *played_.Gba(),
                                                     replace_.shown->gba_songs, view.gba_song, voices);
    }
    else if (choice == ReplaceChoice::kReplace && view.result)
    {
        std::string details =
            song::GbaSongName(*played_.Gba(), static_cast<int>(source.sound)) + " from " + source.file_name;
        format::File result = std::move(*replace_.shown->view.result);
        const std::string title = replace_.shown->view.title;
        replace_.shown.reset();
        Do(title, std::move(details), std::move(result));
    }
}

void App::ApplyDseChoice(ReplaceChoice choice)
{
    const ReplaceView& view = replace_.shown->view;
    ReplaceSource& source = replace_.shown->source;
    if (choice == ReplaceChoice::kPlay && !view.wave.empty())
    {
        try
        {
            PlayReplacePreview(std::make_unique<song::WavePreview>(song::DsWavePcmOf(view.wave)));
        }
        catch (const std::exception& error)
        {
            ShowMessage(std::string("Can't play the new wave. ") + format::AsSentence(error.what()));
        }
    }
    else if (choice == ReplaceChoice::kPlay && view.playable)
    {
        PlayMapping();
    }
    else if (choice == ReplaceChoice::kChange && replace_.shown->wave && view.encoding)
    {
        const WaveTarget target{source.sound, *replace_.shown->wave};
        const std::string name = source.file_name;
        replace_.shown = DseWaveReplacement(std::move(replace_.shown->wav), name, target, *view.encoding,
                                            ChosenRate(view), ChosenLoop(view));
    }
    else if (choice == ReplaceChoice::kChange && source.dse_song)
    {
        source.midi_programs = ChosenPrograms(view);
        if (!source.midi.empty())
        {
            source.drum_notes = ChosenDrumNotes(view);
            ReadDseMidi(source);
        }
        replace_.shown->view = MakeDseSoundReplaceView(source);
    }
    else if (choice == ReplaceChoice::kReplace && view.result)
    {
        const std::optional<uint32_t> wave = replace_.shown->wave;
        std::string action = wave                   ? "Replace Wave"
                             : replace_.shown->file ? ReplaceAction(*replace_.shown->file)
                                                    : "Replace Sequence";
        const std::string name = wave ? song::DseItemName(*played_.Dse(), {song::ItemKind::kWave, source.sound, *wave})
                                 : replace_.shown->file
                                     ? song::DseItemName(*played_.Dse(), {song::ItemKind::kBank, source.sound})
                                     : song::DseSoundName(*played_.Dse(), source.sound);
        std::string details = name + " from " + source.file_name;
        format::File result = std::move(*replace_.shown->view.result);
        if (const std::optional<uint32_t> bank = DseFirstBank(*played_.Dse(), source.sound);
            bank && !replace_.shown->file && !wave)
        {
            KeepMidiMaps(result, MidiMapsOf(view, source, std::vector<uint32_t>(edit::kMidiBankSlots, *bank)));
        }
        replace_.shown.reset();
        Do(std::move(action), std::move(details), std::move(result));
    }
}

void App::ApplyDsChoice(ReplaceChoice choice)
{
    const ReplaceView& view = replace_.shown->view;
    if (choice == ReplaceChoice::kPlay && !view.wave.empty())
    {
        PlayNewDsWave();
    }
    else if (choice == ReplaceChoice::kPlay && view.playable)
    {
        PlayMapping();
    }
    else if (choice == ReplaceChoice::kChange && replace_.shown->stream && view.encoding)
    {
        const uint32_t stream = *replace_.shown->stream;
        const std::string name = replace_.shown->source.file_name;
        replace_.shown = DsStreamReplacement(std::move(replace_.shown->wav), name, stream, *view.encoding,
                                             ChosenRate(view), ChosenLoop(view));
    }
    else if (choice == ReplaceChoice::kChange && replace_.shown->ds_new == edit::DsNewItem::kStream)
    {
        const std::vector<uint32_t>& models = replace_.shown->source.models;
        const std::optional<uint32_t> model =
            view.model < models.size() ? std::optional(models[view.model]) : std::nullopt;
        const std::string name = replace_.shown->source.file_name;
        const std::string new_name = NameOf(view);
        replace_.shown =
            DsStreamImport(std::move(replace_.shown->wav), name, model, view.encoding, ChosenRate(view), new_name);
    }
    else if (choice == ReplaceChoice::kChange && replace_.shown->wave && view.encoding)
    {
        const WaveTarget target{replace_.shown->source.sound, *replace_.shown->wave};
        const std::string name = replace_.shown->source.file_name;
        replace_.shown = DsWaveReplacement(std::move(replace_.shown->wav), name, target, *view.encoding,
                                           ChosenRate(view), ChosenLoop(view));
    }
    else if (choice == ReplaceChoice::kChange && replace_.shown->new_wave)
    {
        const std::vector<uint32_t> wave_archives = song::DsWaveArchives(*played_.Ds());
        const auto place = static_cast<std::size_t>(std::max(view.wave_archive.value_or(0), 0));
        const uint32_t wave_archive =
            place < wave_archives.size() ? wave_archives[place] : replace_.shown->source.sound;
        const std::string name = replace_.shown->source.file_name;
        replace_.shown =
            DsWaveImport(std::move(replace_.shown->wav), name, wave_archive, view.encoding, ChosenRate(view));
    }
    else if (choice == ReplaceChoice::kReplace && replace_.shown->new_wave && view.result)
    {
        std::string action = view.encoding ? "Import Audio File" : "Import Wave";
        std::string details = song::DsWaveArchiveName(*played_.Ds(), replace_.shown->source.sound) + " from " +
                              replace_.shown->source.file_name;
        format::File result = std::move(*replace_.shown->view.result);
        replace_.shown.reset();
        Do(std::move(action), std::move(details), std::move(result));
    }
    else if (choice == ReplaceChoice::kChange && replace_.shown->ds_new)
    {
        ChangeDsNewItem();
    }
    else if (choice == ReplaceChoice::kChange && !replace_.shown->source.midi.empty())
    {
        // A MIDI file's rows give new commands.
        ReplaceSource& source = replace_.shown->source;
        source.midi_programs = ChosenPrograms(view);
        source.drum_notes = ChosenDrumNotes(view);
        replace_.shown->wav = ReadDsMidi(source);
        replace_.shown->view = MakeDsSequenceReplaceView(source, replace_.shown->wav);
    }
    else if (choice == ReplaceChoice::kReplace && replace_.shown->ds_new && view.ready)
    {
        ImportDsNewItem();
    }
    else if (choice == ReplaceChoice::kReplace && view.result)
    {
        ReplaceDsItem();
    }
}

void App::PlayNewDsWave()
{
    const ReplaceView& view = replace_.shown->view;
    const bool stream_view = replace_.shown->stream || replace_.shown->ds_new == edit::DsNewItem::kStream;
    try
    {
        PlayReplacePreview(std::make_unique<song::WavePreview>(stream_view ? song::DsStreamPcmOf(view.wave)
                                                                           : song::DsWavePcmOf(view.wave)));
    }
    catch (const std::exception& error)
    {
        ShowMessage(std::string("Can't play the new wave. ") + format::AsSentence(error.what()));
    }
}

void App::ChangeDsNewItem()
{
    const ReplaceView& view = replace_.shown->view;

    // The bank follows the model until the user chooses another.
    ReplaceSource& source = replace_.shown->source;
    std::vector<int> rows = ChosenWaveArchives(view);
    const uint32_t model = view.model < source.models.size() ? source.models[view.model] : source.sound;
    const bool followed = *replace_.shown->ds_new == edit::DsNewItem::kSequence &&
                          rows == FirstDsRows(source, *replace_.shown->ds_new, replace_.shown->wav);
    source.sound = model;
    if (followed)
    {
        rows = FirstDsRows(source, *replace_.shown->ds_new, replace_.shown->wav);
    }

    // A MIDI file's rows give new commands. Another bank proposes the pairings of its map.
    if (!source.midi.empty())
    {
        const std::vector<uint32_t> banks = song::DsBanks(*played_.Ds());
        const std::optional<uint32_t> bank =
            !rows.empty() && rows.front() >= 0 && static_cast<std::size_t>(rows.front()) < banks.size()
                ? std::optional(banks[static_cast<std::size_t>(rows.front())])
                : std::nullopt;
        if (bank && bank != source.midi_bank)
        {
            ProposeDsMidiMap(source, *bank);
        }
        else
        {
            source.midi_programs = ChosenPrograms(view);
            source.drum_notes = ChosenDrumNotes(view);
        }
        replace_.shown->wav = ReadDsMidi(source);
    }
    replace_.shown->view = MakeDsImportView(source, *replace_.shown->ds_new, replace_.shown->wav, NameOf(view), rows);
}

void App::ImportDsNewItem()
{
    const ReplaceView& view = replace_.shown->view;
    const ReplaceSource& source = replace_.shown->source;
    const std::string name = NameOf(view);
    const bool sequence = *replace_.shown->ds_new == edit::DsNewItem::kSequence;
    std::string action = source.new_song                                       ? "Add Sequence Sound"
                         : source.new_bank                                     ? "Add Bank"
                         : sequence                                            ? "Import Sequence"
                         : *replace_.shown->ds_new == edit::DsNewItem::kBank   ? "Import Bank"
                         : *replace_.shown->ds_new == edit::DsNewItem::kStream ? "Import Stream"
                                                                               : "Import Wave Archive";
    std::string details =
        (name.empty() ? source.file_name : name) +
        (source.new_song || source.new_bank || name.empty() ? std::string() : " from " + source.file_name);
    const std::optional<uint32_t> select =
        sequence ? std::optional(static_cast<uint32_t>(source.played.Ds()->archive.sequences.size())) : std::nullopt;
    const std::optional<uint32_t> open_bank =
        source.new_bank ? std::optional(static_cast<uint32_t>(source.played.Ds()->archive.banks.size())) : std::nullopt;
    StartEdit(replace_.DsImportEdit(), std::move(action), std::move(details), select);
    if (replace_.shown && replace_.shown->pending)
    {
        replace_.shown->pending->open_bank = open_bank;
    }
}

void App::ReplaceDsItem()
{
    const ReplaceView& view = replace_.shown->view;
    const uint32_t item = replace_.shown->source.sound;
    const std::optional<edit::FileKind> kind = replace_.shown->file;
    const std::optional<uint32_t> wave = replace_.shown->wave;
    std::string action = replace_.shown->stream ? "Replace Stream"
                         : wave                 ? "Replace Wave"
                         : kind                 ? ReplaceAction(*kind)
                                                : "Replace Sequence";
    const std::string name =
        replace_.shown->stream                 ? song::DsStreamName(*played_.Ds(), item)
        : wave                                 ? song::DsItemName(*played_.Ds(), {song::ItemKind::kWave, item, *wave})
        : kind == edit::FileKind::kBank        ? song::DsItemName(*played_.Ds(), {song::ItemKind::kBank, item})
        : kind == edit::FileKind::kWaveArchive ? song::DsItemName(*played_.Ds(), {song::ItemKind::kWaveArchive, item})
                                               : song::DsSoundName(*played_.Ds(), item);
    std::string details = name + " from " + replace_.shown->source.file_name;
    format::File result = std::move(*replace_.shown->view.result);
    if (const std::optional<uint32_t> bank = song::DsBankOf(*played_.Ds(), item); bank && !kind)
    {
        KeepMidiMaps(result,
                     MidiMapsOf(view, replace_.shown->source, std::vector<uint32_t>(edit::kMidiBankSlots, *bank)));
    }
    replace_.shown.reset();
    Do(std::move(action), std::move(details), std::move(result));
}

void App::ApplyArchiveChoice(ReplaceChoice choice)
{
    if (ApplyArchiveStreamChoice(choice))
    {
        return;
    }
    if (choice == ReplaceChoice::kPlay && replace_.shown->view.playable && replace_.shown->view.wave.empty())
    {
        PlayMapping();
        return;
    }
    if (ApplyArchiveWaveChoice(choice) || ApplyArchiveImportChoice(choice) || ApplyStreamReplacementChoice(choice) ||
        ApplyArchiveFileChoice(choice))
    {
        return;
    }
    if (choice == ReplaceChoice::kChange && replace_.shown->wave && replace_.shown->view.encoding)
    {
        const WaveTarget target{replace_.shown->source.sound, *replace_.shown->wave};
        const std::string name = replace_.shown->source.file_name;
        replace_.shown =
            replace_.shown->source.played.Wii()
                ? WiiWaveReplacement(std::move(replace_.shown->wav), name, target, *replace_.shown->view.encoding,
                                     ChosenRate(replace_.shown->view), ChosenLoop(replace_.shown->view))
                : WaveReplacement(std::move(replace_.shown->wav), name, target, *replace_.shown->view.encoding,
                                  ChosenRate(replace_.shown->view), ChosenLoop(replace_.shown->view));
        return;
    }
    if (choice == ReplaceChoice::kChange)
    {
        replace_.ChangeArchiveSequence();
    }
    else if (choice == ReplaceChoice::kReplace && replace_.shown->view.result)
    {
        FinishArchiveReplace();
    }
}

void App::PlayNewWiiStream()
{
    // On the Wii, the new stream plays on NW4R SND's player of streams with the file's build. A new sound plays it
    // with the entry that the import gives it.
    try
    {
        const ntdlib::SndBuild build = format::BuildOfWiiArchive(*replace_.shown->source.file->native);
        if (!replace_.shown->incoming_stream)
        {
            PlayReplacePreview(std::make_unique<song::SoundPreview>(played_.Wii(), replace_.shown->source.sound,
                                                                    replace_.shown->view.wave, build));
        }
        else if (replace_.shown->new_stream && replace_.shown->new_stream->wii_sound)
        {
            PlayReplacePreview(std::make_unique<song::SoundPreview>(
                played_.Wii(), *replace_.shown->new_stream->wii_sound, replace_.shown->view.wave, build));
        }
        else
        {
            ShowMessage(std::string("Can't play the new stream. ") + format::AsSentence(replace_.shown->view.error));
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(std::string("Can't play the new stream. ") + format::AsSentence(error.what()));
    }
}

void App::PlayNewSarStream()
{
    // The new stream plays on the console's model with the sound's tracks. A new sound plays it with the model's
    // volume.
    try
    {
        ntdlib::sar::SoundInfo sound = played_.Sar()->Sounds().at(replace_.shown->source.sound);
        if (replace_.shown->incoming_stream)
        {
            sound.stream =
                replace_.shown->new_stream ? replace_.shown->new_stream->details : ntdlib::sar::StreamSoundInfo();
        }
        const format::Mixing mixing = format::MixingOf(*replace_.shown->source.file, replace_.shown->source.sound);
        PlayReplacePreview(std::make_unique<song::SoundPreview>(played_.Sar(), std::move(sound),
                                                                replace_.shown->view.wave, edit::ModelMixing(mixing)));
    }
    catch (const std::exception& error)
    {
        ShowMessage(std::string("Can't play the new stream. ") + format::AsSentence(error.what()));
    }
}

bool App::ApplyArchiveStreamChoice(ReplaceChoice choice)
{
    if (choice == ReplaceChoice::kPlay && played_.Wii() && (replace_.shown->stream || replace_.shown->incoming_stream))
    {
        PlayNewWiiStream();
        return true;
    }
    else if (choice == ReplaceChoice::kPlay && played_.Sar() &&
             (replace_.shown->stream || replace_.shown->incoming_stream))
    {
        PlayNewSarStream();
        return true;
    }
    else if (choice == ReplaceChoice::kChange && replace_.shown->incoming_stream)
    {
        // Another encoding makes the stream again. Another name or model keeps it.
        const ReplaceView& view = replace_.shown->view;
        const uint32_t model = view.model < replace_.shown->source.models.size()
                                   ? replace_.shown->source.models[view.model]
                                   : replace_.shown->source.sound;
        const ntdlib::sar::WaveEncoding encoding = view.encoding.value_or(replace_.shown->incoming_stream->encoding);
        std::optional<edit::IncomingStream> incoming = std::move(replace_.shown->incoming_stream);
        if (encoding != incoming->encoding)
        {
            incoming.reset();
        }
        const std::string file_name = replace_.shown->source.file_name;
        replace_.shown = replace_.shown->source.played.Wii()
                             ? WiiStreamImport(std::move(replace_.shown->wav), file_name, model, NameOf(view), encoding,
                                               std::move(incoming))
                             : StreamImport(std::move(replace_.shown->wav), file_name, model, NameOf(view), encoding,
                                            std::move(incoming));
        return true;
    }
    else if (choice == ReplaceChoice::kReplace && replace_.shown->incoming_stream && replace_.shown->view.ready &&
             replace_.shown->new_stream)
    {
        MakeStreamImport();
        return true;
    }

    return false;
}

bool App::ApplyArchiveWaveChoice(ReplaceChoice choice)
{
    if (choice == ReplaceChoice::kPlay && played_.Wii() &&
        (!replace_.shown->view.wave.empty() || !replace_.shown->view.waves.empty()))
    {
        // A wave of the Wii plays as the Wii decodes it: its first two channels, up to the end of its loop. A new wave
        // archive plays the wave chosen in the menu.
        try
        {
            PlayReplacePreview(std::make_unique<song::WavePreview>(
                edit::WavePcm(ntdlib::rsar::WaveFile::Read(ChosenWave(replace_.shown->view)))));
        }
        catch (const std::exception& error)
        {
            ShowMessage(std::string("Can't play the new wave. ") + format::AsSentence(error.what()));
        }
        return true;
    }
    else if (choice == ReplaceChoice::kPlay && played_.Sar())
    {
        // The new wave plays by itself. Playback follows the console's decoding: the wave's first two channels, up to
        // the end of its loop. A new wave archive plays the wave chosen in the menu.
        try
        {
            ntdlib::sar::Pcm pcm =
                ntdlib::sar::DecodeWave(ChosenWave(replace_.shown->view), song::ItemOptions(*played_.Sar()));
            pcm.channels.resize(std::min<std::size_t>(pcm.channels.size(), 2));
            PlayReplacePreview(std::make_unique<song::WavePreview>(std::move(pcm)));
        }
        catch (const std::exception& error)
        {
            ShowMessage(std::string("Can't play the new wave. ") + format::AsSentence(error.what()));
        }
        return true;
    }
    else if (choice == ReplaceChoice::kChange && replace_.shown->new_wave)
    {
        // Another wave archive, encoding or rate makes the new wave again. On the Wii, the menu lists the wave data of
        // the archive's files.
        const ReplaceView& view = replace_.shown->view;
        const auto wave_archive = static_cast<uint32_t>(std::max(view.wave_archive.value_or(0), 0));
        const ntdlib::sar::WaveEncoding encoding = view.encoding.value_or(ntdlib::sar::WaveEncoding::kDspAdpcm);
        const bool wav = view.encoding.has_value();
        const std::optional<uint32_t> rate = ChosenRate(view);
        const std::string name = replace_.shown->source.file_name;
        if (replace_.shown->source.played.Wii())
        {
            const std::vector<song::WiiWaveArchive> wave_data =
                song::WaveArchivesOf(*replace_.shown->source.played.Wii());
            const uint32_t file_id =
                wave_archive < wave_data.size() ? wave_data[wave_archive].file_id : replace_.shown->source.sound;
            replace_.shown = WiiWaveImport(std::move(replace_.shown->wav), name, file_id, encoding, wav, rate);
        }
        else
        {
            replace_.shown = WaveImport(std::move(replace_.shown->wav), name, wave_archive, encoding, wav, rate);
        }
        return true;
    }
    else if (choice == ReplaceChoice::kReplace && replace_.shown->new_wave && replace_.shown->view.result)
    {
        const ReplaceSource& source = replace_.shown->source;
        std::string action = replace_.shown->view.encoding ? "Import Audio File" : "Import Wave";
        const song::Item wave{song::ItemKind::kWave, source.sound, replace_.shown->wave.value_or(0)};
        std::string details = (source.played.Wii() ? song::ItemName(*source.played.Wii(), wave)
                                                   : song::ItemName(*source.played.Sar(), wave)) +
                              " from " + source.file_name;
        format::File result = std::move(*replace_.shown->view.result);
        replace_.shown.reset();
        Do(std::move(action), std::move(details), std::move(result));
        return true;
    }

    return false;
}

bool App::ApplyArchiveImportChoice(ReplaceChoice choice)
{
    if (choice == ReplaceChoice::kChange && replace_.shown->view.import && replace_.shown->new_item)
    {
        const ReplaceView& view = replace_.shown->view;
        if (view.model < replace_.shown->source.models.size())
        {
            replace_.shown->source.sound = replace_.shown->source.models[view.model];
        }
        replace_.shown->view =
            replace_.shown->source.played.Wii()
                ? MakeWiiItemImportView(replace_.shown->source, *replace_.shown->new_item, replace_.shown->named,
                                        ChosenWaveArchives(view), NameOf(view))
                : MakeItemImportView(replace_.shown->source, *replace_.shown->new_item, replace_.shown->named,
                                     ChosenWaveArchives(view), NameOf(view));
        return true;
    }
    else if (choice == ReplaceChoice::kChange && replace_.shown->view.import)
    {
        // The slots follow the model until the user chooses a slot's bank.
        const ReplaceView& view = replace_.shown->view;
        const uint32_t sound_before = replace_.shown->source.sound;
        if (view.model < replace_.shown->source.models.size())
        {
            replace_.shown->source.sound = replace_.shown->source.models[view.model];
        }
        if (view.slots_changed)
        {
            replace_.shown->source.banks = ChosenBanks(view);
        }
        edit::ProgramMap programs = ChosenPrograms(view);
        if ((view.slots_changed || replace_.shown->source.sound != sound_before) &&
            !replace_.shown->source.midi.empty())
        {
            programs = ProposeMidiMap(replace_.shown->source);
        }
        FollowDrumRows(replace_.shown->source, view);

        // An empty song's labels take the new sound's name, and the starts show them.
        if (replace_.shown->source.new_song)
        {
            replace_.shown->source.incoming.sequence = edit::EmptySong(NameOf(view));
        }
        replace_.shown->view =
            replace_.shown->source.played.Wii()
                ? MakeWiiImportView(replace_.shown->source, view.chosen, programs, ChosenVariables(view), NameOf(view))
                : MakeImportView(replace_.shown->source, view.chosen, programs, ChosenVariables(view), NameOf(view));
        return true;
    }
    else if (choice == ReplaceChoice::kReplace && replace_.shown->view.import && replace_.shown->view.ready &&
             replace_.shown->new_item)
    {
        MakeItemImport();
        return true;
    }
    else if (choice == ReplaceChoice::kReplace && replace_.shown->view.import && replace_.shown->view.ready)
    {
        MakeImport();
        return true;
    }

    return false;
}

bool App::ApplyStreamReplacementChoice(ReplaceChoice choice)
{
    if (choice == ReplaceChoice::kChange && replace_.shown->stream && replace_.shown->view.encoding)
    {
        const uint32_t sound = replace_.shown->source.sound;
        const std::string name = replace_.shown->source.file_name;
        replace_.shown = StreamReplacement(std::move(replace_.shown->wav), name, sound, *replace_.shown->view.encoding);
        return true;
    }
    else if (choice == ReplaceChoice::kReplace && replace_.shown->stream && replace_.shown->view.result)
    {
        const ReplaceSource& source = replace_.shown->source;
        std::string details = (source.played.Wii() ? source.played.Wii()->Sounds()[source.sound].name
                                                   : source.played.Sar()->Sounds()[source.sound].name) +
                              " from " + source.file_name;
        format::File result = std::move(*replace_.shown->view.result);
        replace_.shown.reset();
        Do("Replace Stream", std::move(details), std::move(result));
        return true;
    }

    return false;
}

bool App::ApplyArchiveFileChoice(ReplaceChoice choice)
{
    if (choice == ReplaceChoice::kChange && replace_.shown->file && replace_.shown->new_item &&
        !replace_.shown->source.played.Wii())
    {
        // Another wave archive or instrument in a row makes the file again. The sound that Play plays stays, and so do
        // the groups that the user chose. The Replace window of a file of the Wii has no rows.
        const FileTarget target{*replace_.shown->file, replace_.shown->source.sound};
        const std::string name = replace_.shown->source.file_name;
        const std::vector<int> targets = ChosenWaveArchives(replace_.shown->view);
        const std::vector<edit::ProgramFix> fixes = ChosenFixes(replace_.shown->view);
        const std::size_t play = replace_.shown->view.play;
        replace_.shown = FileReplacement(
            std::move(*replace_.shown->new_item), name, target, std::move(replace_.shown->named), targets, fixes,
            std::move(replace_.shown->source.joined), std::move(replace_.shown->source.new_groups));
        if (replace_.shown && play < replace_.shown->view.play_sounds.size())
        {
            replace_.shown->view.play = play;
        }
        return true;
    }
    else if (choice == ReplaceChoice::kChange && replace_.shown->file && replace_.shown->new_item &&
             replace_.shown->source.played.Wii() && replace_.shown->source.later)
    {
        // Other wave data in a row converts the file again.
        const FileTarget target{*replace_.shown->file, replace_.shown->source.sound};
        const std::string name = replace_.shown->source.file_name;
        const ntdlib::sar::Flavor later = *replace_.shown->source.later;
        const std::vector<int> targets = ChosenWaveArchives(replace_.shown->view);
        replace_.shown = WiiFileReplacement(std::move(*replace_.shown->new_item), name, target, later,
                                            std::move(replace_.shown->named), targets);
        return true;
    }

    return false;
}

void App::FinishArchiveReplace()
{
    const ReplaceSource& source = replace_.shown->source;
    const std::optional<edit::FileKind> kind = replace_.shown->file;
    const std::optional<uint32_t> wave = replace_.shown->wave;
    std::string action = wave ? "Replace Wave" : kind ? ReplaceAction(*kind) : "Replace Sequence";
    const std::string item =
        source.played.Wii() ? (wave ? song::ItemName(*source.played.Wii(), {song::ItemKind::kWave, source.sound, *wave})
                               : kind ? ReplacedName(*source.played.Wii(), *kind, source.sound)
                                      : source.played.Wii()->Sounds()[source.sound].name)
        : wave              ? song::ItemName(*source.played.Sar(), {song::ItemKind::kWave, source.sound, *wave})
        : kind              ? ReplacedName(*source.played.Sar(), *kind, source.sound)
                            : source.played.Sar()->Sounds()[source.sound].name;
    std::string details = item + " from " + source.file_name;
    std::vector<uint32_t> replaced = !wave && !kind ? ReplacedSequences(source) : std::vector<uint32_t>{};
    format::File result = std::move(*replace_.shown->view.result);
    KeepMidiMaps(result, MidiMapsOf(replace_.shown->view, source, {}));
    if (!replace_.shown->view.joined.empty() || !replace_.shown->view.new_groups.empty())
    {
        // The groups take the files that the item requires after the replacement and didn't require before. The new
        // groups take them all.
        const uint32_t id =
            kind ? ReplacedItem(*kind, source.sound) : ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, source.sound);
        const auto make = [file = std::move(result), groups = replace_.shown->view.joined,
                           new_groups = replace_.shown->view.new_groups, id,
                           before = edit::FilesOfItem(*source.played.Sar(), id)]
        {
            return edit::JoinGroups(file, groups, std::span(&id, 1), before, new_groups);
        };
        StartEdit(make, std::move(action), std::move(details), std::nullopt, std::move(replaced));
        return;
    }

    replace_.shown.reset();
    Do(std::move(action), std::move(details), std::move(result), std::nullopt, std::move(replaced));
}

void App::StartEdit(std::function<format::File()> make, std::string action, std::string details,
                    std::optional<uint32_t> select, std::vector<uint32_t> replaced)
{
    // The work catches the edit's refusal. The window waits with the controls off, and Cancel drops the work.
    const auto work = [build = std::move(make)]
    {
        EditWork done;
        try
        {
            done.result = build();
        }
        catch (const std::exception& error)
        {
            done.error = error.what();
        }

        return done;
    };
    replace_.shown->pending = PendingEdit{std::async(std::launch::async, work),
                                          std::move(action),
                                          std::move(details),
                                          select,
                                          std::move(replaced),
                                          std::nullopt};
    replace_.shown->view.working = "Building the archive…";
}

void App::PlayMapping()
{
    // The version that the mapping makes, and what plays in it.
    const ReplaceSource& source = replace_.shown->source;
    const ReplaceView& view = replace_.shown->view;
    std::function<format::File()> make;
    PreviewTarget target;
    target.mixing = source.file->mixing;
    if (replace_.shown->sound_font)
    {
        // The first new bank plays a note of its first instrument.
        make = replace_.SoundFontEdit(false);
        target.bank = static_cast<uint32_t>(source.played.Ds() ? source.played.Ds()->archive.banks.size()
                                                               : source.played.Sar()->Banks().size());
    }
    else if (replace_.shown->ds_new)
    {
        replace_.DsNewItemPreview(make, target);
    }
    else if ((replace_.shown->file || source.played.Ds() || source.played.Dse()) && view.result)
    {
        replace_.ReplacedFilePreview(make, target);
    }
    else if (view.import && view.ready && replace_.shown->new_item)
    {
        // A new bank plays a note of its first instrument, and a new wave sound file its first wave sound.
        make = replace_.ItemImportEdit(false);
        if (replace_.shown->new_item->kind == edit::FileKind::kBank)
        {
            target.bank = static_cast<uint32_t>(source.played.Sar()->Banks().size());
        }
        else
        {
            target.sound = static_cast<uint32_t>(source.played.Sar()->Sounds().size());
        }
    }
    else if (view.import && view.ready && view.chosen < view.starts.size() && source.models.empty())
    {
        // A new sequence sound without a model plays as the new sound.
        make = [file = source.file, name = NameOf(view), sequence = source.incoming.sequence,
                start = view.starts[view.chosen].item, programs = ChosenPrograms(view),
                variables = ChosenVariables(view), banks = source.banks]
        {
            const edit::MappedVariables mapped = edit::WithVariables(sequence, start, variables);
            return edit::AddSequenceSound(*file, std::nullopt, name, mapped.sequence, mapped.start, programs, banks);
        };
        target.sound = static_cast<uint32_t>(source.played.Sar()->Sounds().size());
    }
    else if (view.import && view.ready && view.chosen < view.starts.size())
    {
        replace_.ModelSequencePreview(make, target);
    }
    else if (view.result)
    {
        // A sequence in the place of a sound's sequence.
        make = [result = *view.result]
        {
            return result;
        };
        target.sound = source.sound;
        target.mixing = format::MixingOf(*source.file, source.sound);
    }
    else
    {
        return;
    }

    StartPreview(std::move(make), target);
}

namespace
{

// The preview of `target` in the GBA ROM that `version` builds, into `done`.
void GbaPreviewWork(const format::File& version, const PreviewTarget& target, song::Mixing mixing, PreviewWork& done)
{
    const std::shared_ptr<const song::GbaRom> rom = GbaRomOf(version);
    auto sequence =
        std::make_shared<const song::Sequence>(song::ReadSequence(*rom, static_cast<int>(target.sound.value_or(0))));
    done.renderer =
        std::make_unique<song::SequencePreview>(rom, std::move(sequence), song::ModelOptions{.mixing = mixing});
}

// The preview of `target` in the DSE set that `version` builds, into `done`. A bank plays a note of its first program.
void DsePreviewWork(const format::File& version, const PreviewTarget& target, song::Mixing mixing, PreviewWork& done)
{
    const std::shared_ptr<const song::DseSet> set = DseSetOf(version);
    if (target.sound)
    {
        auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*set, *target.sound));
        done.renderer =
            std::make_unique<song::SequencePreview>(set, std::move(sequence), song::ModelOptions{.mixing = mixing});
    }
    else if (target.bank)
    {
        const song::DseBank& bank = set->banks.at(*target.bank);
        const std::vector<uint32_t> programs = song::DseBankPrograms(*bank.bank);
        if (programs.empty())
        {
            done.note = "The bank has no programs. There's nothing to play.";
            return;
        }
        done.renderer =
            std::make_unique<song::DseNotePreview>(bank, static_cast<int>(programs.front()), set->build, mixing);
    }
}

// The preview of `target` in the DS archive that `version` builds, into `done`. A bank plays a note of its first
// instrument, and a wave archive its first wave.
void DsPreviewWork(const format::File& version, const PreviewTarget& target, const std::optional<song::DsGame>& ds_game,
                   song::Mixing mixing, PreviewWork& done)
{
    const std::shared_ptr<const song::DsArchive> archive = DsArchiveOf(version, ds_game);
    if (target.sound)
    {
        auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, *target.sound));
        done.renderer =
            std::make_unique<song::SequencePreview>(archive, std::move(sequence), song::ModelOptions{.mixing = mixing});
    }
    else if (target.bank)
    {
        const std::vector<uint32_t> programs = song::DsBankPrograms(*archive, *target.bank);
        if (programs.empty())
        {
            done.note = "The bank has no instruments. There's nothing to play.";
            return;
        }
        done.renderer =
            std::make_unique<song::DsNotePreview>(*archive, *target.bank, static_cast<int>(programs.front()), mixing);
    }
    else if (target.wave_archive)
    {
        done.renderer = std::make_unique<song::WavePreview>(song::DsWavePcm(*archive, *target.wave_archive, 0));
    }
}

// The preview of `target` in the archive of the later consoles that `version` builds, into `done`, with the effects
// `aux` at the buses' `volumes`.
void SarPreviewWork(const format::File& version, const PreviewTarget& target, const std::string& name,
                    const song::AuxEffects& aux, const song::AuxVolumes& volumes, song::Mixing mixing,
                    PreviewWork& done)
{
    const std::shared_ptr<const ntdlib::sar::SoundArchive> archive = SarArchiveOf(version, name);
    if (target.sound && archive->Sounds().at(*target.sound).type == ntdlib::sar::SoundType::kSequence)
    {
        auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, *target.sound));
        done.renderer = std::make_unique<song::SequencePreview>(
            archive, std::move(sequence), song::ModelOptions{.effects = aux, .mixing = mixing}, volumes);
    }
    else if (target.sound)
    {
        // A wave sound whose wave is missing from its wave archive is silent, and the window warns about it.
        if (std::optional<std::string> silent = SilentWaveSoundNote(*archive, *target.sound))
        {
            done.note = std::move(*silent);
            return;
        }

        done.renderer = std::make_unique<song::SoundPreview>(archive, *target.sound, std::vector<uint8_t>(), mixing);
    }
    else if (target.bank)
    {
        const std::vector<uint32_t> programs = song::ProgramsOf(*archive, *target.bank);
        if (programs.empty())
        {
            done.note = "The bank has no instruments. There's nothing to play.";
            return;
        }

        done.renderer = std::make_unique<song::NotePreview>(archive, *target.bank, programs.front(), mixing);
    }
    else if (target.wave_archive)
    {
        done.renderer = std::make_unique<song::WavePreview>(*archive, *target.wave_archive, 0);
    }
}

} // namespace

void App::StartPreview(std::function<format::File()> make, PreviewTarget target)
{
    // The work builds the version's archive with the group files that the file keeps. The zones' reader builds the
    // archive with them too. A sequence sound plays with the file's effects at the buses' volumes. Each item plays in
    // the target's mixing mode.
    const BusEffects* effects = Effects();
    const auto work = [build = std::move(make), target, name = file_name_, gba = played_.Gba() != nullptr,
                       ds = played_.Ds() != nullptr, dse = played_.Dse() != nullptr, ds_game = GameOf(played_.Ds()),
                       aux = edit::ModelEffects(effects != nullptr ? *effects : BusEffects{}), volumes = BusVolumes(),
                       mixing = edit::ModelMixing(target.mixing)]
    {
        PreviewWork done;
        try
        {
            const format::File version = build();

            // A GBA song plays from the ROM that the version builds.
            if (gba)
            {
                GbaPreviewWork(version, target, mixing, done);
                return done;
            }

            // A DSE sound plays from the set that the version builds, by the build of the driver in the image. A bank
            // plays a note of its first program.
            if (dse)
            {
                DsePreviewWork(version, target, mixing, done);
                return done;
            }

            // A DS sound plays from the archive that the version builds, by the build of the driver in its image. A
            // bank plays a note of its first instrument, and a wave archive its first wave.
            if (ds)
            {
                DsPreviewWork(version, target, ds_game, mixing, done);
                return done;
            }

            SarPreviewWork(version, target, name, aux, volumes, mixing, done);
        }
        catch (const std::exception& error)
        {
            done.error = error.what();
        }

        return done;
    };
    replace_.preview_work = std::async(std::launch::async, work);
    replace_.shown->view.preparing = true;
}

} // namespace hokuto::app
