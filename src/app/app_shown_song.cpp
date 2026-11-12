// SPDX-License-Identifier: MIT

// The sequence that the zones show: showing and following it, its view, what the song shows of it, and the names and
// places of the open archive's sounds.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <span>
#include <utility>

#include "app/app.h"
#include "app/editor_view.h"
#include "app/markers.h"
#include "app/sequence_view.h"
#include "app/tempo_row.h"
#include "app/time_text.h"
#include "edit/converter.h"
#include "edit/instruments.h"
#include "edit/reach.h"
#include "edit/replace.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/items.h"

namespace hokuto::app
{
namespace
{

// The bank slots of sound `sound` of `archive`, with the programs of each slot's bank. A bank that can't be read lists
// no programs.
std::vector<SlotView> SlotsOf(const ntdlib::sar::SoundArchive& archive, uint32_t sound)
{
    std::vector<SlotView> slots;
    if (sound >= archive.Sounds().size())
    {
        return slots;
    }

    for (const uint32_t bank : archive.Sounds()[sound].banks)
    {
        SlotView& slot = slots.emplace_back();
        const uint32_t index = ntdlib::sar::ItemNumber(bank);
        if (bank == ntdlib::sar::kNoItem || index >= archive.Banks().size())
        {
            continue;
        }

        slot.bank = archive.Banks()[index].name;
        try
        {
            slot.programs = song::ProgramsOf(archive, index);
        }
        catch (const std::exception&)
        {
            slot.programs.clear();
        }
    }

    return slots;
}

// The same for sound `sound` of the Wii's `archive`. Its one slot holds the sound's bank.
std::vector<SlotView> SlotsOf(const ntdlib::rsar::SoundArchive& archive, uint32_t sound)
{
    std::vector<SlotView> slots;
    if (sound >= archive.Sounds().size())
    {
        return slots;
    }

    SlotView& slot = slots.emplace_back();
    const uint32_t bank = archive.Sounds()[sound].bank;
    if (bank >= archive.Banks().size())
    {
        return slots;
    }

    slot.bank = archive.Banks()[bank].name;
    try
    {
        slot.programs = song::ProgramsOf(archive, bank);
    }
    catch (const std::exception&)
    {
        slot.programs.clear();
    }

    return slots;
}

// The end of a sentence in a tooltip that says what `reach` changes besides a track of `read`: the sound's other
// tracks, then the other sounds of `archive`. Past four sounds, three of them stand for the rest. Empty where `reach`
// holds nothing.
std::string ReachText(std::span<const std::string> names, const song::Sequence& read, const edit::Reach& reach)
{
    std::vector<std::string> tracks;
    for (const int number : reach.tracks)
    {
        const auto found = std::find_if(read.tracks.begin(), read.tracks.end(),
                                        [number](const song::Track& track) { return track.number == number; });
        tracks.push_back(found != read.tracks.end() ? found->name : "Track " + std::to_string(number));
    }
    std::vector<std::string> sounds;
    for (const uint32_t sound : reach.sounds)
    {
        const bool named = sound < names.size() && !names[sound].empty();
        sounds.push_back(named ? names[sound] : "Sound " + std::to_string(sound));
    }

    // Such as "Track 3 of this sound, and the sounds SE_A and SE_B".
    constexpr std::size_t kMostNamed = 4;
    std::string text = tracks.empty() ? std::string() : JoinedNames(tracks) + " in this sound";
    text += !tracks.empty() && !sounds.empty() ? ", and " : "";
    if (sounds.size() == 1)
    {
        text += "the sound " + sounds.front();
    }
    else if (sounds.size() > 1 && sounds.size() <= kMostNamed)
    {
        text += "the sounds " + JoinedNames(sounds);
    }
    else if (sounds.size() > kMostNamed)
    {
        text += std::to_string(sounds.size()) + " other sounds, including " +
                JoinedNames(std::span<const std::string>(sounds).first(kMostNamed - 1));
    }

    return text;
}

// The archive, the ROM or the set that `read` came from. A read comes from one of them, or from none when its build
// failed.
song::Played PlayedOf(const song::SequenceReader::Result& read)
{
    return read.driver ? song::Played(read.driver)
           : read.gba  ? song::Played(read.gba)
           : read.ds   ? song::Played(read.ds)
           : read.dse  ? song::Played(read.dse)
           : read.wii  ? song::Played(read.wii)
                       : song::Played(read.archive);
}

} // namespace

void App::TakeReadSequence()
{
    std::optional<song::SequenceReader::Result> read = reader_.Take();
    if (read && read->ds && !read->sequence &&
        (read->sound >= read->ds->sounds.size() || !read->ds->sounds[read->sound]))
    {
        // A version of the DS without the sound, such as the one before the import that added it, closes the zones'
        // sequence. Only the version's archive lists the DS's sounds. The browser then lists its items.
        const std::shared_ptr<const format::File> version = reading_version_ ? reading_version_ : history_->Current();
        CloseSequence();
        played_ = read->ds;
        archive_version_ = version;
        FollowItems();
        read.reset();
    }
    if (read)
    {
        // A read without a build brings the archive that the program held when it asked. The program may have built
        // the same archive again since, and keeps it.
        if (read->sequence && reading_version_ != nullptr &&
            (read->archive != played_.Sar() || read->wii != played_.Wii() || read->gba != played_.Gba() ||
             read->ds != played_.Ds() || read->dse != played_.Dse() || read->driver != played_.Driver()))
        {
            played_ = PlayedOf(*read);
            archive_version_ = reading_version_;
            FollowItems();
        }
        reading_version_.reset();

        // A timeline shows the first pass of its loop, on the Wii too. The performer can find the loop a whole number
        // of passes late in a conversion for another engine as well. Playback there ends at the conversion's loop.
        const std::optional<uint32_t> sound_file =
            !read->sequence ? std::nullopt
            : played_.Sar() && read->sequence->sound < played_.Sar()->Sounds().size()
                ? std::optional(played_.Sar()->Sounds()[read->sequence->sound].file_id)
            : played_.Wii() && read->sequence->sound < played_.Wii()->Sounds().size()
                ? std::optional(played_.Wii()->Sounds()[read->sequence->sound].file_id)
                : std::nullopt;
        if (sound_file && archive_version_)
        {
            const uint32_t file = *sound_file;
            const std::optional<uint32_t> sequence =
                file < file_sequences_.size() ? file_sequences_[file] : std::nullopt;
            if (sequence && edit::IsTimeline(*archive_version_, *sequence))
            {
                read->sequence = std::make_shared<const song::Sequence>(
                    edit::TimelineRead(*archive_version_, *sequence, *read->sequence));
                if (read->engine_sequence)
                {
                    read->engine_sequence = std::make_shared<const song::Sequence>(
                        edit::TimelineRead(*archive_version_, *sequence, *read->engine_sequence));
                }
            }
        }
        if (read->sequence)
        {
            TakeEngine(*read);
        }
        if (read->sequence && sequence_ && read->sequence->sound == sequence_->sound)
        {
            FollowSequence(std::move(*read));
        }
        else
        {
            ShowSequence(std::move(*read));
        }
    }
}

void App::CloseSequence()
{
    if (transport_.Playing())
    {
        transport_.Stop();
    }
    reader_.Abandon();
    reading_version_.reset();
    StopPlayback();
    CancelCatchUp();
    CloseKeyEditor();
    CloseListEditor();
    sequence_.reset();
    model_.reset();
    song_.reset();
    ForgetEngine();
    browser_state_.selected = {};
    shown_group_ = -1;
    shown_item_ = -1;
    arrangement_.selected_track = 0;
    arrangement_.selected_part_track = -1;
    arrangement_.selected_part = -1;
    transport_.SetCycle(false);
    transport_.SetPosition(0.0);
    transport_.TakeEmptySong();
}

void App::ShowSequence(song::SequenceReader::Result read)
{
    if (!read.sequence)
    {
        ShowMessage(read.error);
        return;
    }

    // The new sequence plays from its start if the last one was playing. It plays with the file's effects, in the
    // sound's mixing mode.
    StopPlayback();
    CancelCatchUp();
    CloseKeyEditor();
    CloseListEditor();
    sequence_ = std::move(read.sequence);
    const BusEffects* effects = Effects();
    model_effects_ = effects != nullptr ? *effects : BusEffects{};
    model_mixing_ = ShownMixing();
    model_ = MakeModel();
    model_engine_ = PlayedEngine();
    song_ = MakeSongView(*sequence_, PartLabels());
    song_->slots = ShownSlots();
    ShowDriverPrograms();

    // A song of the GBA's other drivers whose commands the file keeps among the ROM's other bytes plays, and accepts no
    // edits.
    if (played_.Driver() && !ShownSequence() && told_unrelocatable_.insert(sequence_->sound).second)
    {
        ShowMessage(
            "This song can't be edited because Hokuto can't relocate its commands. It plays and exports as stored in "
            "the ROM.",
            MessageKind::kNote);
    }

    // When a bank of the sound lies in a group file that the file lacks, a message says so the first time that the
    // sound shows while the file stays open. The browser shows the bank grey, but not the sound: its other notes can
    // play.
    if (played_.Sar() && told_missing_banks_.insert(sequence_->sound).second)
    {
        const std::string note = song::MissingBanksNote(*played_.Sar(), sequence_->sound);
        if (!note.empty())
        {
            ShowMessage(note, MessageKind::kNote);
        }
    }

    arrangement_.selected_track = 0;
    arrangement_.selected_part_track = -1;
    arrangement_.selected_part = -1;

    // The transport takes the sequence's tempo. The locators go round the loop. Without one, they run from the start to
    // the beat on which the last note has ended (SongLocators). A song plays through once: Cycle starts off.
    transport_.SetTempo(song::TempoAt(*sequence_, 0));
    transport_.SetBeatsPerBar(song_->beats_per_bar);
    const Locators locators = SongLocators(*sequence_);
    transport_.SetLocators(locators.start, locators.end);
    transport_.SetCycle(false);
    transport_.SetPosition(0.0);
    ShowReach();
    ShowLogic();
    ShowOpeningInstruments();
    ShowTempo();
    ShowMode();

    // The first sound that shows after a file opens takes the file's view, if the view is the sound's.
    const std::optional<format::EditorView> view = std::exchange(pending_view_, std::nullopt);
    if (view && view->sound == sequence_->sound)
    {
        RestoreView(*view);
    }
}

std::optional<format::EditorView> App::CurrentView() const
{
    if (!sequence_ || !song_ || preview_)
    {
        return std::nullopt;
    }

    const LowerZone front = FrontOfLowerZone();
    const OpenEditors editors{key_editor_open_ ? std::optional<int>(key_editor_track_) : std::nullopt,
                              list_editor_open_ ? std::optional<int>(list_editor_track_) : std::nullopt,
                              front == LowerZone::kKeyEditor    ? format::ViewFront::kKeyEditor
                              : front == LowerZone::kListEditor ? format::ViewFront::kListEditor
                                                                : format::ViewFront::kMixer};
    return ViewOf(sequence_->sound, TrackNumbers(), transport_, arrangement_, *song_, key_editor_, editors);
}

void App::RestoreView(const format::EditorView& view)
{
    // The editors open on their tracks at the places that the view keeps.
    const OpenEditors editors = ApplyView(view, TrackNumbers(), transport_, arrangement_, *song_, key_editor_);
    if (editors.key_editor)
    {
        key_editor_open_ = true;
        key_editor_track_ = *editors.key_editor;
        ClearKeySelection();
        UpdateKeyTrack();
    }
    if (editors.list_editor)
    {
        OpenCommands(*editors.list_editor, {}, 0);
    }
    lower_front_ = editors.front == format::ViewFront::kKeyEditor    ? LowerZone::kKeyEditor
                   : editors.front == format::ViewFront::kListEditor ? LowerZone::kListEditor
                                                                     : LowerZone::kMixer;
}

std::vector<int> App::TrackNumbers() const
{
    std::vector<int> numbers;
    if (sequence_)
    {
        for (const song::Track& track : sequence_->tracks)
        {
            numbers.push_back(track.number);
        }
    }

    return numbers;
}

void App::FollowSequence(song::SequenceReader::Result read)
{
    // Each track keeps its mute, its solo and its meters, by its number. The selection of a track and of a part's track
    // follows the track's number too. A track that a track's edit added becomes the selection.
    std::map<int, TrackView> kept;
    for (std::size_t i = 0; song_ && i < song_->tracks.size() && i < sequence_->tracks.size(); i++)
    {
        kept.emplace(sequence_->tracks[i].number, song_->tracks[i]);
    }

    const auto number_at = [this](int index)
    {
        const bool valid = index >= 0 && static_cast<std::size_t>(index) < sequence_->tracks.size();
        return valid ? sequence_->tracks[static_cast<std::size_t>(index)].number : -1;
    };
    const int selected = std::exchange(track_to_select_, std::nullopt).value_or(number_at(arrangement_.selected_track));
    const int part_track = number_at(arrangement_.selected_part_track);

    // The locators that showing the sound set follow the new version, such as after Replace… or a move of the loop.
    // With Cycle on, the old loop would cut a longer song short. Without a loop, they only grow: a note edit that
    // shortens the song leaves them. The user's locators stay while the new version reaches them.
    const Locators locators =
        LocatorsAfter({transport_.CycleStart(), transport_.CycleEnd()}, *sequence_, *read.sequence);
    transport_.SetLocators(locators.start, locators.end);
    sequence_ = std::move(read.sequence);
    song_ = MakeSongView(*sequence_, PartLabels());
    song_->slots = ShownSlots();
    ShowDriverPrograms();
    KeepTrackStates(kept);

    const auto index_of = [this](int number)
    {
        const auto found = std::find_if(sequence_->tracks.begin(), sequence_->tracks.end(),
                                        [number](const song::Track& track) { return track.number == number; });
        return found != sequence_->tracks.end() ? static_cast<int>(found - sequence_->tracks.begin()) : -1;
    };
    arrangement_.selected_track = std::max(index_of(selected), 0);
    arrangement_.selected_part_track = index_of(part_track);
    if (arrangement_.selected_part_track < 0)
    {
        arrangement_.selected_part = -1;
    }

    ShowReach();
    ShowLogic();
    ShowOpeningInstruments();
    ShowTempo();
    ShowMode();

    StartCatchUp();

    // An editor whose track the new version lacks, such as after Remove Selected Tracks, shows the arrangement's
    // selected track instead. The zone in front stays in front.
    const LowerZone front = lower_front_;
    if (key_editor_open_ && TrackNumbered(key_editor_track_) == nullptr)
    {
        OpenKeyEditor();
    }
    if (list_editor_open_ && TrackNumbered(list_editor_track_) == nullptr)
    {
        OpenListEditor();
    }
    lower_front_ = front;
    UpdateKeyTrack();
    UpdateListTrack();
}

std::vector<SlotView> App::ShownSlots()
{
    return played_.Sar()      ? SlotsOf(*played_.Sar(), sequence_->sound)
           : played_.Wii()    ? SlotsOf(*played_.Wii(), sequence_->sound)
           : played_.Gba()    ? GbaSlotsOf(sequence_->sound)
           : played_.Ds()     ? DsSlotsOf(sequence_->sound)
           : played_.Dse()    ? DseSlotsOf(sequence_->sound)
           : played_.Driver() ? DriverSlots()
                              : std::vector<SlotView>();
}

void App::KeepTrackStates(const std::map<int, TrackView>& kept)
{
    for (std::size_t i = 0; i < song_->tracks.size(); i++)
    {
        const auto found = kept.find(sequence_->tracks[i].number);
        if (found != kept.end())
        {
            TrackView& track = song_->tracks[i];
            track.mute = found->second.mute;
            track.solo = found->second.solo;
            track.meter = found->second.meter;
            track.peak = found->second.peak;
        }
    }
}

void App::ShowReach()
{
    // The edits go into the version that built the open archive, and the reach comes from it too. A sound that Hokuto
    // can't edit shows nothing.
    if (!song_ || !sequence_ || !archive_version_ || !ReachShown())
    {
        return;
    }

    std::vector<edit::TrackReach> reach;
    try
    {
        reach = edit::ReachOfTracks(*archive_version_, Places(), sequence_->sound, *sequence_);
    }
    catch (const std::exception&)
    {
        return;
    }

    // A strip shows only the sends that the console's tracks have. Right after a sound is shown, the reach comes before
    // the update of the frame sets song_->sends.
    const std::vector<std::string> names = SoundNames();
    const std::size_t sends = ShownSends();
    for (std::size_t i = 0; i < reach.size() && i < song_->tracks.size(); i++)
    {
        TrackView& track = song_->tracks[i];
        for (std::size_t control = 0; control < kMixControlCount; control++)
        {
            const auto first_send = static_cast<std::size_t>(MixControl::kSendA);
            const bool shown = control < first_send || control - first_send < sends;
            const auto setting = static_cast<std::size_t>(MixOf(static_cast<MixControl>(control)));
            track.mix_reach[control] = shown ? ReachText(names, *sequence_, reach[i].mix[setting]) : std::string();
        }
        track.instrument_reach = ReachText(names, *sequence_, reach[i].instrument);
    }
}

void App::ShowLogic()
{
    // The places come from the version that built the open archive. The List Editor's rows come from it too.
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!song_ || !sequence_ || !sequence || !archive_version_)
    {
        return;
    }

    song_->logic = LogicMarkers(*archive_version_, *sequence, *sequence_);
}

void App::ShowOpeningInstruments()
{
    // The commands come from the version that built the open archive.
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!song_ || !sequence_ || !sequence || !archive_version_)
    {
        return;
    }

    for (std::size_t i = 0; i < song_->tracks.size() && i < sequence_->tracks.size(); i++)
    {
        const song::Track& track = sequence_->tracks[i];
        if (!track.notes.empty())
        {
            continue;
        }

        // A track of the GBA's other drivers has an instrument before its notes only where its channel selects one
        // (ShowDriverPrograms).
        if (played_.Driver() && song_->tracks[i].programs.empty() &&
            (played_.Driver()->Rules().PsgTrack(track.number) || played_.Driver()->Rules().NotesNameSamples()))
        {
            continue;
        }

        std::optional<edit::Program> opening;
        try
        {
            opening = edit::OpeningInstrument(*archive_version_, *sequence, track);
        }
        catch (const std::exception&)
        {
            continue;
        }

        // A track starts on slot 0, and the inspector's menus choose its instrument before it has notes.
        TrackView& view = song_->tracks[i];
        view.slot = 0;
        if (opening && static_cast<std::size_t>(opening->slot) < song_->slots.size())
        {
            view.bank = song_->slots[static_cast<std::size_t>(opening->slot)].bank;
            view.program = opening->number;
            view.slot = opening->slot;
        }
    }
}

void App::ShowMode()
{
    // The mode and the loop come from the version that built the open archive.
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!song_ || !sequence || !archive_version_)
    {
        return;
    }

    const std::optional<edit::ConvertedLoop> loop = edit::TimelineLoop(*archive_version_, *sequence);
    song_->loop_moves = loop && song_->loop_start >= 0 && loop->start == static_cast<uint32_t>(song_->loop_start) &&
                        loop->end == static_cast<uint32_t>(song_->loop_end);
    song_->timeline = edit::IsTimeline(*archive_version_, *sequence);
    song_->timeline_refused.clear();
    if (!song_->timeline)
    {
        try
        {
            edit::AsTimeline(*archive_version_, *sequence);
        }
        catch (const std::exception& error)
        {
            song_->timeline_refused = error.what();
        }
    }
}

bool App::ReachShown() const
{
    return song::OnGbaOrDs(played_) || ((played_.Sar() || played_.Wii()) && start_fields_);
}

void App::ShowTempo()
{
    // The points lead to the commands of the version that built the open archive. The List Editor's rows come from it
    // too.
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!song_ || !sequence_ || !sequence || !archive_version_)
    {
        return;
    }

    song_->tempo_points = TempoPoints(*archive_version_, *sequence, *sequence_);
    if (!ReachShown() || song_->tempo_points.empty())
    {
        return;
    }

    // The other sounds that an edit of each point's command changes. A sound that Hokuto can't edit lists none.
    std::vector<uint32_t> commands;
    for (const TempoPointView& point : song_->tempo_points)
    {
        commands.push_back(point.command);
    }
    std::vector<std::vector<uint32_t>> reaching;
    try
    {
        reaching = edit::SoundsReaching(*archive_version_, Places(), sequence_->sound, commands);
    }
    catch (const std::exception&)
    {
        return;
    }

    const std::vector<std::string> names = SoundNames();
    for (std::size_t i = 0; i < reaching.size() && i < song_->tempo_points.size(); i++)
    {
        song_->tempo_points[i].reach = ReachText(names, *sequence_, edit::Reach{{}, reaching[i]});
    }
}

std::optional<uint32_t> App::ShownSound() const
{
    if (sequence_)
    {
        return sequence_->sound;
    }

    return preview_ && song_ ? std::optional<uint32_t>(0) : std::nullopt;
}

ShownItem App::ItemShown(std::optional<uint64_t> shown) const
{
    return ShownItem{.version = history_ ? history_->Current() : nullptr,
                     .shown = shown.value_or(std::numeric_limits<uint64_t>::max())};
}

std::optional<uint32_t> App::ShownSequence() const
{
    return sequence_ ? SequenceOfSound(sequence_->sound) : std::nullopt;
}

format::Shared<ntdlib::commands::Sequence> App::VersionSequence(std::optional<uint32_t> sequence) const
{
    if (!sequence || !archive_version_ || *sequence >= archive_version_->sequences.size())
    {
        return {};
    }

    return archive_version_->sequences[*sequence];
}

std::map<uint32_t, std::string> App::PartLabels() const
{
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!sequence_ || !archive_version_ || !sequence)
    {
        return sequence_ ? sequence_->labels : std::map<uint32_t, std::string>();
    }

    return app::PartLabels(*sequence_, *archive_version_, *sequence);
}

std::optional<uint32_t> App::SequenceOfSound(uint32_t sound) const
{
    std::optional<uint32_t> file;
    if (played_.Sar() && sound < played_.Sar()->Sounds().size())
    {
        file = played_.Sar()->Sounds()[sound].file_id;
    }
    else if (played_.Wii() && sound < played_.Wii()->Sounds().size())
    {
        file = played_.Wii()->Sounds()[sound].file_id;
    }
    else if (song::OnGbaOrDs(played_))
    {
        file = sound;
    }

    return file && *file < file_sequences_.size() ? file_sequences_[*file] : std::nullopt;
}

std::vector<std::optional<edit::SoundPlace>> App::Places() const
{
    if (played_.Gba())
    {
        return edit::GbaSoundPlaces(
            file_sequences_,
            format::GbaTrackFields(*archive_version_, played_.Gba()->info.song_table, file_sequences_.size()),
            song::GbaPlayerTracks(*played_.Gba(), file_sequences_.size()));
    }
    if (played_.Driver())
    {
        return edit::DriverSoundPlaces(*archive_version_, *played_.Driver(), file_sequences_);
    }
    if (played_.Ds() || played_.Dse())
    {
        return edit::DsSoundPlaces(file_sequences_);
    }
    if (!start_fields_)
    {
        return {};
    }

    return played_.Wii()   ? edit::SoundPlaces(*played_.Wii(), file_sequences_, *start_fields_)
           : played_.Sar() ? edit::SoundPlaces(*played_.Sar(), file_sequences_, *start_fields_)
                           : std::vector<std::optional<edit::SoundPlace>>();
}

std::vector<std::string> App::SoundNames() const
{
    std::vector<std::string> names;
    if (played_.Wii())
    {
        for (const ntdlib::rsar::SoundInfo& sound : played_.Wii()->Sounds())
        {
            names.push_back(sound.name);
        }
    }
    else if (played_.Sar())
    {
        for (const ntdlib::sar::SoundInfo& sound : played_.Sar()->Sounds())
        {
            names.push_back(sound.name);
        }
    }
    else if (played_.Gba())
    {
        for (int song = 0; song < played_.Gba()->info.song_count; song++)
        {
            names.push_back(song::GbaSongName(*played_.Gba(), song));
        }
    }
    else if (played_.Ds())
    {
        for (std::size_t sound = 0; sound < played_.Ds()->sounds.size(); sound++)
        {
            names.push_back(song::DsSoundName(*played_.Ds(), static_cast<uint32_t>(sound)));
        }
    }
    else if (played_.Dse())
    {
        for (const uint32_t sound : song::DseSounds(*played_.Dse()))
        {
            names.push_back(song::DseSoundName(*played_.Dse(), sound));
        }
    }
    else if (played_.Driver())
    {
        for (std::size_t sound = 0; sound < file_sequences_.size(); sound++)
        {
            names.push_back(played_.Driver()->SoundName(static_cast<uint32_t>(sound)));
        }
    }

    return names;
}

const song::Track* App::TrackNumbered(int number) const
{
    if (!sequence_)
    {
        return nullptr;
    }

    const auto found = std::find_if(sequence_->tracks.begin(), sequence_->tracks.end(),
                                    [number](const song::Track& track) { return track.number == number; });
    return found != sequence_->tracks.end() ? &*found : nullptr;
}

const song::Track* App::SelectedTrack() const
{
    const int index = arrangement_.selected_track;
    if (!sequence_ || index < 0 || static_cast<std::size_t>(index) >= sequence_->tracks.size())
    {
        return nullptr;
    }

    return &sequence_->tracks[static_cast<std::size_t>(index)];
}

} // namespace hokuto::app
