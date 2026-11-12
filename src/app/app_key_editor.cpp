// SPDX-License-Identifier: MIT

// The Key Editor: its track, the selection after an edit, and its edits of notes and controls. app_key_feedback.cpp
// contains the notes that it sounds.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>

#include "app/app.h"
#include "app/app_internal.h"
#include "app/lanes.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/drivers.h"
#include "edit/instruments.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/driver.h"

namespace hokuto::app
{
namespace
{

// The key that the Key Editor shows a track without notes round: middle C.
constexpr int kMiddleC = 60;

// The semitones that the last transpose of `controls` before `tick` sets, from a track's commands (ControlsOf), or 0. A
// transpose from a random number or a variable counts as 0.
int TransposeAt(std::span<const KeyControl> controls, uint32_t tick)
{
    int semitones = 0;
    for (const KeyControl& control : controls)
    {
        const ntdlib::commands::Argument& value = control.command.value;
        if (control.command.kind == ntdlib::commands::Kind::kTranspose && control.tick <= tick)
        {
            const bool decided = value.source == ntdlib::commands::Source::kValue;
            semitones = decided ? static_cast<int>(value.value / ntdlib::commands::kOne / 100) : 0;
        }
    }

    return semitones;
}

// The note to add for `drawn`, a note that the Key Editor drew on `track`, in sequence `sequence` of `file`. The note
// goes into the last stretch of the track's commands that started at or before its tick. Its key leaves out the
// transpose of the note before it, or else of the track's first note. A track without notes takes the transpose of
// `controls`, the commands of its settings (ControlsOf). On Nintendo R&D2's driver, its key on such a track also leaves
// out the pitch that the region of a square or of the wave adds to it (edit::Rd2CommandKey). On Brownie Brown's driver,
// it leaves out the key of the PSG's lowest note (edit::BrownieCommandKey). Throws std::invalid_argument for a tick
// before the track's first stretch. On Konami's driver, a track without notes that runs no commands, or whose commands
// end where they start, has no room for a note (edit::kKonamiNoRoom).
edit::NewNote DrawnNoteOf(const format::File& file, uint32_t sequence, const song::Track& track,
                          std::span<const KeyControl> controls, const KeyNote& drawn)
{
    const song::Console console = edit::ConsoleOf(file);
    const edit::DriverEdits* driver_edits = console.driver ? &edit::EditsOf(*console.driver) : nullptr;
    const edit::DrawnKeyNote drawn_note{
        drawn.start, drawn.length, drawn.key, drawn.velocity, drawn.bank_slot, drawn.program, drawn.sample_semitones};
    const std::optional<song::Stretch> stretch = edit::StretchAt(track.stretches, drawn.start);
    if (driver_edits != nullptr)
    {
        if (std::optional<edit::NewNote> placed =
                driver_edits->PlaceDrawnNote(file, sequence, track, stretch, drawn_note))
        {
            return *placed;
        }
    }
    if (!stretch)
    {
        throw std::invalid_argument("Can't add a note at that tick on this track.");
    }
    if (track.notes.empty())
    {
        const int transpose = TransposeAt(controls, drawn.start);
        int key = drawn.key - transpose;
        if (driver_edits != nullptr)
        {
            key = driver_edits->FirstNoteKey(file, sequence, track.number, drawn_note, transpose, key);
        }
        return {*stretch, drawn.start, key, drawn.velocity, drawn.length};
    }

    // The track's notes come in the order that they start.
    const song::Note* before = &track.notes.front();
    for (const song::Note& note : track.notes)
    {
        if (note.start > drawn.start)
        {
            break;
        }

        before = &note;
    }

    const int transpose = before->key - edit::CommandKey(file, sequence, before->command);
    return {*stretch, drawn.start, drawn.key - transpose, drawn.velocity, drawn.length};
}

// DrawnNoteOf, and on Ubisoft Milan's driver with the track's channel in the note's command (edit/ubimilan.h).
edit::NewNote NewNoteOf(const format::File& file, uint32_t sequence, const song::Track& track,
                        std::span<const KeyControl> controls, const KeyNote& drawn)
{
    edit::NewNote note = DrawnNoteOf(file, sequence, track, controls, drawn);
    const song::Console console = edit::ConsoleOf(file);
    if (const std::optional<uint8_t> index =
            console.driver ? edit::EditsOf(*console.driver).NewNoteIndex(track.number) : std::nullopt)
    {
        note.index = *index;
    }

    return note;
}

// A control edit of the Key Editor as the new version, the commands that it keeps, and the number of commands that it
// edits.
struct ControlEdit
{
    edit::Edited edited;
    std::vector<uint32_t> kept;
    std::size_t count = 0;
};

// The version that `edit` of the controls of `track` makes from sequence `sequence` of `document`. Each command's
// edit runs in its stretch in `stretches`.
ControlEdit ControlEditOf(const KeyEdit& edit, const format::File& document, uint32_t sequence,
                          const song::Track* track, const std::map<uint32_t, song::Stretch>& stretches)
{
    const auto place = [&stretches](uint32_t offset)
    {
        return edit::CommandPlace{offset, stretches.at(offset)};
    };
    edit::Edited edited;
    std::vector<uint32_t> kept;
    std::size_t count = 0;
    if (!edit.removed_controls.empty())
    {
        std::vector<edit::CommandPlace> places;
        for (const uint32_t offset : edit.removed_controls)
        {
            places.push_back(place(offset));
        }
        edited = edit::RemoveCommands(document, sequence, places);
        count = places.size();
    }
    else if (!edit.added_controls.empty())
    {
        std::vector<edit::NewSetting> settings;
        for (const auto& [tick, command] : edit.added_controls)
        {
            const std::optional<song::Stretch> stretch = edit::StretchAt(track->stretches, tick);
            if (!stretch)
            {
                throw std::invalid_argument("Can't add a command at that tick on this track.");
            }

            settings.push_back({*stretch, tick, command});
        }
        edited = edit::AddSettings(document, sequence, settings);
        count = settings.size();
    }
    else
    {
        // The commands move first, and then take their new values.
        std::vector<edit::SettingMove> moves;
        for (const auto& [offset, shift] : edit.moved_controls)
        {
            moves.push_back({offset, stretches.at(offset), shift});
            kept.push_back(offset);
        }
        std::vector<edit::CommandPlace> places;
        std::vector<ntdlib::commands::Command> commands;
        for (const auto& [offset, command] : edit.changed_controls)
        {
            places.push_back(place(offset));
            commands.push_back(command);
            if (!edit.moved_controls.contains(offset))
            {
                kept.push_back(offset);
            }
        }
        edited = moves.empty()
                     ? edit::ChangeCommands(document, sequence, places, commands)
                     : edit::MoveAndChangeCommands(document, sequence, moves, places, commands, track->stretches);
        count = kept.size();
    }

    return {std::move(edited), std::move(kept), count};
}

// The selection of the controls of track `track` after `edited`: the commands that the edit keeps, and the new ones.
KeySelection ControlsSelectedAfter(const format::File& document, uint32_t sequence, int track,
                                   const edit::Edited& edited, const std::vector<uint32_t>& kept)
{
    const ntdlib::commands::Sequence& commands = *edited.file.sequences.at(sequence);
    const std::vector<uint32_t> offsets = edit::Offsets(commands, edit::ConsoleOf(edited.file));
    KeySelection after{edited.file.sequences[sequence], track, {}};
    for (const uint32_t offset : kept)
    {
        const std::optional<uint32_t> item = edited.items.at(edit::ItemOf(document, sequence, offset));
        if (item && *item < commands.items.size())
        {
            after.commands.insert(offsets[*item]);
        }
    }
    for (const uint32_t item : edited.added)
    {
        after.commands.insert(offsets.at(item));
    }

    return after;
}

// The name of `edit` in the Undo History: the setting of a single command, or the number of commands.
std::string ControlEditName(const KeyEdit& edit, const format::File& document, uint32_t sequence, std::size_t count)
{
    std::string what = std::to_string(count) + " Commands";

    const auto setting_name = [](const ntdlib::commands::Command& command)
    {
        return LaneName(SettingOf(command).value_or(LaneSetting{command.kind}));
    };
    if (count == 1 && !edit.added_controls.empty())
    {
        what = setting_name(edit.added_controls.front().second);
    }
    else if (count == 1 && !edit.changed_controls.empty())
    {
        what = setting_name(edit.changed_controls.begin()->second);
    }
    else if (count == 1 && !edit.moved_controls.empty())
    {
        what = setting_name(edit::CommandAt(document, sequence, edit.moved_controls.begin()->first));
    }
    else if (count == 1)
    {
        what = setting_name(edit::CommandAt(document, sequence, *edit.removed_controls.begin()));
    }

    return what;
}

} // namespace

void App::OpenKeyEditor()
{
    // The selected part's track opens, or else the selected track. A track without notes has no parts.
    const std::optional<double> beat = std::exchange(arrangement_.open_beat, std::nullopt);
    const PartView* part = SelectedPart(song_, arrangement_);
    const int index = part != nullptr ? arrangement_.selected_part_track : arrangement_.selected_track;
    if (!sequence_ || index < 0 || static_cast<std::size_t>(index) >= sequence_->tracks.size())
    {
        return;
    }

    // The editor scrolls to the part's start, with the middle of the part's keys in the middle of the window. A track
    // opens at the beat of the double-click on its lane, or else at the position, with the middle of the track's keys
    // there. A track without notes opens round middle C.
    const auto by_key = [](const NoteView& a, const NoteView& b)
    {
        return a.key < b.key;
    };
    const auto by_note_key = [](const song::Note& a, const song::Note& b)
    {
        return a.key < b.key;
    };
    key_editor_open_ = true;
    lower_front_ = LowerZone::kKeyEditor;
    key_editor_track_ = sequence_->tracks[static_cast<std::size_t>(index)].number;
    ClearKeySelection();
    key_editor_.scroll_to.reset();
    const double quarters = beat.value_or(std::max(transport_.Position(), 0.0));
    key_editor_.scroll_tick = part != nullptr ? static_cast<uint32_t>(std::max(part->start, 0))
                                              : static_cast<uint32_t>(quarters * sequence_->timebase);
    const std::vector<song::Note>& notes = sequence_->tracks[static_cast<std::size_t>(index)].notes;
    const std::vector<NoteView>* part_notes = part == nullptr        ? nullptr
                                              : !part->notes.empty() ? &part->notes
                                                                     : &part->unstarted;
    if (part_notes != nullptr && !part_notes->empty())
    {
        const auto [lowest, highest] = std::minmax_element(part_notes->begin(), part_notes->end(), by_key);
        key_editor_.scroll_key = (lowest->key + highest->key) / 2;
    }
    else if (!notes.empty())
    {
        const auto [lowest, highest] = std::minmax_element(notes.begin(), notes.end(), by_note_key);
        key_editor_.scroll_key = (lowest->key + highest->key) / 2;
    }
    else
    {
        key_editor_.scroll_key = kMiddleC;
    }
    UpdateKeyTrack();
}

void App::CloseKeyEditor()
{
    key_editor_open_ = false;
    ClearKeySelection();
}

void App::ClearKeySelection()
{
    key_editor_.selected.clear();
    key_editor_.gesture = KeyGesture();
    key_editor_.typed_waiting = {};
    key_selection_.reset();
}

void App::FillKeyNotes(const song::Track* track, std::vector<edit::NotePlace>& places, std::set<uint32_t>& commands)
{
    for (const song::Note& note : track->notes)
    {
        key_track_.notes.push_back({.start = note.start,
                                    .length = note.length,
                                    .key = note.key,
                                    .velocity = note.velocity,
                                    .command = note.command,
                                    .bank_slot = note.bank_slot,
                                    .program = note.program,
                                    .psg = note.psg,
                                    .cut = note.cut,
                                    .sample_semitones = note.sample_semitones});
        if (commands.insert(note.command).second)
        {
            places.push_back({.command = note.command,
                              .stretch = song::StretchesOfNote(*sequence_, *track, note)[note.stretch],
                              .note = true});
        }
    }
}

void App::FillUnstartedKeyNotes(const song::Track* track)
{
    for (const song::Note& note : track->unstarted)
    {
        key_track_.unstarted.push_back({.start = note.start,
                                        .length = note.length,
                                        .key = note.key,
                                        .velocity = note.velocity,
                                        .command = note.command,
                                        .psg = note.psg});
    }
}

void App::FillPsgChannelNotes(const song::Track* track)
{
    for (std::size_t i = 0; i < sequence_->tracks.size() && i < song_->tracks.size(); i++)
    {
        const song::Track& other = sequence_->tracks[i];
        const auto add = [this, &other, i, track](const song::Note& note, bool unstarted)
        {
            if (note.psg != 0)
            {
                key_track_.channels.push_back({.channel = note.psg,
                                               .start = note.start,
                                               .length = note.length,
                                               .key = note.key,
                                               .track = song_->tracks[i].name,
                                               .colour = song_->tracks[i].colour,
                                               .shown = &other == track,
                                               .cut = note.cut,
                                               .unstarted = unstarted});
            }
        };
        for (const song::Note& note : other.notes)
        {
            add(note, false);
        }
        for (const song::Note& note : other.unstarted)
        {
            add(note, true);
        }
    }
}

void App::FillKeyControlsAndLimits(const std::optional<uint32_t>& sequence, const song::Track* track,
                                   std::vector<edit::NotePlace>& places, std::set<uint32_t>& commands)
{
    if (sequence && archive_version_)
    {
        try
        {
            key_track_.controls = ControlsOf(*archive_version_, *sequence, *sequence_, *track);
            for (const KeyControl& control : key_track_.controls)
            {
                if (commands.insert(control.offset).second)
                {
                    places.push_back({.command = control.offset, .stretch = track->stretches.at(control.stretch)});
                }
            }
            const std::optional<uint32_t> track_end = edit::TrackEndOf(*archive_version_, *sequence, track->stretches);
            for (edit::NotePlace& place : places)
            {
                place.track_end = track_end;
            }
            const std::vector<edit::NoteLimits> limits = edit::LimitsOf(
                *archive_version_, *sequence, places,
                sequence_->loop ? std::optional<uint32_t>(sequence_->loop->end) : std::nullopt, track->stretches);
            for (std::size_t i = 0; i < places.size(); i++)
            {
                key_editor_.limits.emplace(places[i].command, limits[i]);
            }
        }
        catch (const std::exception&)
        {
            key_editor_.limits.clear();
            key_track_.controls.clear();
        }
    }
}

void App::UpdateKeyTrack()
{
    // The selection follows the version of the shown sequence that the editor shows.
    const std::optional<uint32_t> sequence = ShownSequence();
    const format::Shared<ntdlib::commands::Sequence> shown = VersionSequence(sequence);
    key_editor_.selected = FollowedSelection({key_sequence_, key_editor_track_, std::move(key_editor_.selected)},
                                             std::exchange(key_selection_, std::nullopt), shown);
    key_editor_.selected_controls =
        FollowedSelection({key_sequence_, key_editor_track_, std::move(key_editor_.selected_controls)},
                          std::exchange(control_selection_, std::nullopt), shown);
    key_sequence_ = shown;

    key_track_.notes.clear();
    key_track_.controls.clear();
    key_track_.unstarted.clear();
    key_track_.channels.clear();
    key_track_.console = {};
    key_editor_.limits.clear();
    const song::Track* track = TrackNumbered(key_editor_track_);
    if (track == nullptr || !song_)
    {
        return;
    }

    // The editor shows the track's name, colour and notes from the arrangement. Each command's limits come from the
    // stretch that ran its first note.
    const auto index = static_cast<std::size_t>(track - sequence_->tracks.data());
    key_track_.name = song_->tracks[index].name;
    key_track_.colour = song_->tracks[index].colour;
    key_track_.ticks_per_quarter = sequence_->timebase;
    key_track_.song_length = sequence_->length;
    key_track_.number = track->number;

    // A track without notes plays the instrument of its opening, or the one that its first note requires.
    const FirstNote first = FirstNoteOf(*track);
    const TrackView& track_view = song_->tracks[index];
    key_track_.first_slot = first.instrument ? first.instrument->slot : std::max(track_view.slot, 0);
    key_track_.first_program = first.instrument          ? static_cast<uint32_t>(first.instrument->number)
                               : track_view.program >= 0 ? static_cast<uint32_t>(track_view.program)
                                                         : 0;
    std::vector<edit::NotePlace> places;
    std::set<uint32_t> commands;
    FillKeyNotes(track, places, commands);

    // On the GBA and the DS, the notes that the driver never started, and every track's notes on the PSG channels.
    FillUnstartedKeyNotes(track);
    FillPsgChannelNotes(track);

    // The lane of channels and the tooltips of cut notes take the file's console, even in a sequence that Hokuto can't
    // edit.
    if (archive_version_ && archive_version_->native)
    {
        key_track_.console = edit::ConsoleOf(*archive_version_);
    }

    // A sequence that Hokuto can't edit leaves every note fixed, and its controller lanes empty. The edit of a note
    // there shows why. The limits of a lane's command come from the stretch that ran it first.
    FillKeyControlsAndLimits(sequence, track, places, commands);
}

KeySelection App::SelectionAfter(const format::File& file, uint32_t sequence, const edit::Edited& edited,
                                 const std::set<uint32_t>& kept) const
{
    KeySelection selection{edited.file.sequences.at(sequence), key_editor_track_, {}};
    std::vector<uint32_t> commands;
    if (key_sequence_.SharesWith(file.sequences.at(sequence)))
    {
        commands.assign(kept.begin(), kept.end());
    }
    if (commands.empty() && edited.added.empty())
    {
        return selection;
    }

    // A command that the edit can't follow leaves nothing selected.
    try
    {
        const std::vector<uint32_t> after = edit::CommandsAfter(file, sequence, edited, commands);
        selection.commands.insert(after.begin(), after.end());
    }
    catch (const std::invalid_argument&)
    {
        selection.commands.clear();
    }

    return selection;
}

KeySelection App::ListSelectionAfter(const format::File& file, uint32_t sequence, const edit::Edited& edited) const
{
    KeySelection selection{edited.file.sequences.at(sequence), list_editor_track_, {}};
    if (list_editor_.selected.empty() || !list_sequence_.SharesWith(file.sequences.at(sequence)))
    {
        return selection;
    }

    // Each selected command's item leads to its item in the new version.
    const std::vector<uint32_t> before = edit::Offsets(*file.sequences[sequence], edit::ConsoleOf(file));
    const std::vector<uint32_t> after = edit::Offsets(*edited.file.sequences[sequence], edit::ConsoleOf(file));
    for (const uint32_t offset : list_editor_.selected)
    {
        const std::optional<uint32_t> item = edit::ItemAtOffset(before, offset);
        const std::optional<uint32_t> moved = item && *item < edited.items.size() ? edited.items[*item] : std::nullopt;
        if (moved && *moved + 1 < after.size())
        {
            selection.commands.insert(after[*moved]);
        }
    }

    return selection;
}

std::optional<uint32_t> App::EditedSequence() const
{
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = ShownSequence();
    if (document == nullptr || !archive_version_ || !sequence || !SameArchive(*document, *archive_version_))
    {
        return std::nullopt;
    }

    return sequence;
}

void App::DoSequenceEdit(std::string name, std::string details, uint32_t sequence, edit::Edited edited,
                         const std::set<uint32_t>& kept)
{
    const format::File& document = *Document();
    KeySelection after = SelectionAfter(document, sequence, edited, kept);
    list_selection_ = ListSelectionAfter(document, sequence, edited);
    Do(std::move(name), std::move(details), std::move(edited.file), std::move(after));
}

void App::UpdateKeyEditor()
{
    if (!key_editor_open_ || !sequence_ || FrontOfLowerZone() != LowerZone::kKeyEditor)
    {
        key_editor_.request = KeyRequest::kNone;
        key_editor_.focused = false;
        PlayFeedback(std::nullopt);
        return;
    }

    // While the reader builds the archive of another version, the notes wait for it.
    const bool busy = reading_version_ != nullptr;
    const ShownItem shown = ItemShown(ShownSound()).At({static_cast<uint64_t>(key_editor_track_), 0, 0});
    const std::optional<KeyEdit> edit =
        DrawKeyEditor(key_track_, key_editor_, transport_, shown, busy, &key_editor_open_);
    if (!key_editor_open_)
    {
        CloseKeyEditor();
    }
    else if (edit)
    {
        ApplyKeyEdit(*edit);
    }
    PlayFeedback(key_editor_open_ ? key_editor_.sounding : std::nullopt);
}

App::FirstNote App::FirstNoteOf(const song::Track& track) const
{
    const auto index = static_cast<std::size_t>(&track - sequence_->tracks.data());
    if (!track.notes.empty() || !song_ || !archive_version_ || index >= song_->tracks.size())
    {
        return {};
    }
    const song::Console console = edit::ConsoleOf(*archive_version_);
    if (console.engine == format::Engine::kGba || console.driver)
    {
        return {};
    }

    // On DSE, a channel starts without one of the sound's banks. A program that a track's opening selects then plays
    // nothing, unless a track of the channel selects a bank for it. Such a track selects a bank before its first note.
    const TrackView& view = song_->tracks[index];
    const bool unselected = console.engine == format::Engine::kDse && !DseChannelHasBank(track);
    if (view.program >= 0 && !unselected)
    {
        return {};
    }

    // Such a track keeps the program that it shows, where the shown slot's bank contains it.
    const std::vector<SlotView>& slots = song_->slots;
    const auto shown_slot = static_cast<std::size_t>(std::max(view.slot, 0));
    if (view.program >= 0 && shown_slot < slots.size() &&
        std::ranges::find(slots[shown_slot].programs, static_cast<uint32_t>(view.program)) !=
            slots[shown_slot].programs.end())
    {
        return {edit::Program{static_cast<int>(shown_slot), view.program}, {}};
    }

    // Without a command that selects another, a track plays program 0 of slot 0. Where that plays nothing, the result
    // is the first program of the first bank that contains an instrument. On DSE, a first note on a channel without a
    // bank always requires an instrument.
    if (console.engine != format::Engine::kDse && !slots.empty() &&
        std::ranges::find(slots.front().programs, 0u) != slots.front().programs.end())
    {
        return {};
    }
    for (std::size_t slot = 0; slot < slots.size(); slot++)
    {
        if (!slots[slot].programs.empty() && (slot == 0 || song::Runs(console, ntdlib::commands::Kind::kBankSelect, 0)))
        {
            return {edit::Program{static_cast<int>(slot), static_cast<int>(slots[slot].programs.front())}, {}};
        }
    }

    return {std::nullopt, console.engine == format::Engine::kDse
                              ? "The banks of this sound have no instruments. New notes would be silent, and the "
                                "editors show only notes that sound."
                              : "The banks of this sound have no instruments. New notes would be silent, and the "
                                "editors show only notes that sound. To add a bank, use File > Import > Bank…, then "
                                "assign it in the Sound section."};
}

bool App::DseChannelHasBank(const song::Track& track) const
{
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!sequence || !archive_version_)
    {
        return true;
    }

    // A track of the channel that plays notes has a bank. So does one whose opening selects a bank.
    for (const song::Track& other : sequence_->tracks)
    {
        if (other.channel != track.channel)
        {
            continue;
        }
        try
        {
            if (!other.notes.empty() || edit::DseOpeningSelectsBank(*archive_version_, *sequence, other))
            {
                return true;
            }
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }
    }

    return false;
}

void App::ApplyKeyEdit(const KeyEdit& edit)
{
    if (!edit.refused.empty())
    {
        ShowMessage(edit.refused);
        return;
    }
    if (!edit.changed_controls.empty() || !edit.moved_controls.empty() || !edit.added_controls.empty() ||
        !edit.removed_controls.empty())
    {
        ApplyControlEdit(edit);
        return;
    }

    // The editor's notes lead to the commands of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = EditedSequence();
    const song::Track* track = TrackNumbered(key_editor_track_);
    if (!sequence || track == nullptr)
    {
        return;
    }

    // Each command's edit runs in the stretch that ran its first note. A new length can't pass the track's end.
    std::map<uint32_t, song::Stretch> stretches;
    for (const song::Note& note : track->notes)
    {
        stretches.emplace(note.command, song::StretchesOfNote(*sequence_, *track, note)[note.stretch]);
    }
    const std::optional<uint32_t> track_end = edit::TrackEndOf(*document, *sequence, track->stretches);
    std::vector<edit::NoteEdit> edits;
    for (const KeyChange& change : edit.changes)
    {
        const auto stretch = stretches.find(change.command);
        if (stretch != stretches.end())
        {
            edits.push_back({change.command, stretch->second, change.shift, change.transpose, change.velocity,
                             change.length, change.remove, track_end});
        }
    }
    if (edits.empty() && edit.added.empty())
    {
        return;
    }

    // The edited notes stay selected. New notes become the selection.
    try
    {
        std::vector<edit::NewNote> additions;
        for (const KeyNote& added : edit.added)
        {
            additions.push_back(NewNoteOf(*document, *sequence, *track, key_track_.controls, added));
        }

        // The editors show only the notes that sound. A first note on a track without an instrument adds one to the
        // track.
        const FirstNote first = !additions.empty() ? FirstNoteOf(*track) : FirstNote();
        if (!first.refused.empty())
        {
            ShowMessage(first.refused);
            return;
        }
        edit::Edited edited =
            first.instrument ? edit::EditNotesWithInstrument(*document, *sequence, *track, additions, *first.instrument)
                             : edit::EditNotes(*document, *sequence, edits, additions);

        // The Undo History names the track and the notes.
        const std::size_t notes = edits.size() + additions.size();
        DoSequenceEdit(edit.name,
                       "Track: " + key_track_.name + " - " + (notes == 1 ? "Note" : std::to_string(notes) + " Notes"),
                       *sequence, std::move(edited), edit.added.empty() ? key_editor_.selected : std::set<uint32_t>());
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ApplyControlEdit(const KeyEdit& edit)
{
    // The lanes' points lead to the commands of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = EditedSequence();
    const song::Track* track = TrackNumbered(key_editor_track_);
    if (!sequence || track == nullptr)
    {
        return;
    }

    // Each command's edit runs in the stretch that ran it first. A new command goes into the last stretch that starts
    // by its tick (edit::StretchAt). A new note goes there too.
    std::map<uint32_t, song::Stretch> stretches;
    for (const KeyControl& control : key_track_.controls)
    {
        stretches.emplace(control.offset, track->stretches.at(control.stretch));
    }

    try
    {
        auto [edited, kept, count] = ControlEditOf(edit, *document, *sequence, track, stretches);

        // The changed commands and the new ones stay selected in the new version.
        control_selection_ = ControlsSelectedAfter(*document, *sequence, key_editor_track_, edited, kept);

        // The Undo History names the track and the setting.
        const std::string what = ControlEditName(edit, *document, *sequence, count);

        // The selected notes and the List Editor's selected commands stay selected.
        DoSequenceEdit(edit.name, "Track: " + key_track_.name + " - " + what, *sequence, std::move(edited),
                       key_editor_.selected);
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

} // namespace hokuto::app
