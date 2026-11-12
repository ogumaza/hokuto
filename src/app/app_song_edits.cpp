// SPDX-License-Identifier: MIT

// Edits of the shown song from the tempo row, the arrangement, the marker row, the mixer and the inspector, and the
// commands that add, copy or remove tracks.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <set>
#include <span>
#include <utility>

#include "app/app.h"
#include "app/app_internal.h"
#include "app/key_edits.h"
#include "app/sequence_view.h"
#include "app/tempo_row.h"
#include "app/time_text.h"
#include "app/widgets.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/converter.h"
#include "edit/instruments.h"
#include "edit/mix.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/timelines.h"
#include "edit/tracks.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/konami.h"

namespace hokuto::app
{
namespace
{

// The text of instrument `instrument` of a song's sound in the Undo History: its bank's name and its program.
std::string InstrumentText(const SongView& song, const edit::Program& instrument)
{
    const auto slot = static_cast<std::size_t>(instrument.slot);
    const std::string bank = slot < song.slots.size() && !song.slots[slot].bank.empty()
                                 ? song.slots[slot].bank
                                 : "Slot " + std::to_string(instrument.slot);
    return bank + " Program " + std::to_string(instrument.number);
}

// The names in the Undo History of a move of `track`'s control from `from` to `to`, in the values of its setting. Each
// name gives a channel or a send slot, then the parameter, such as Track 0 / Volume or 1- FX A / Level. The name of a
// pan gives the track. The details give the values as the mixer shows them, with `gba` for a song of the GBA's
// drivers, and `most` the volume at 0 dB. The name of a send slot leaves the track out, and a send's details
// name it.
ActionNames MixMoveNames(const std::string& track, MixControl control, int from, int to, bool gba, int most)
{
    const auto text = [control, gba, most](int value)
    {
        const float shown = ShownValue(control, value, gba, most);
        return control == MixControl::kPan ? PanText(shown) : LevelText(shown) + " dB";
    };
    const std::string change = "from " + text(from) + " to " + text(to);
    switch (control)
    {
    case MixControl::kVolume:
        return {track + " / Volume", change};
    case MixControl::kPan:
        return {track + " / Pan", change};
    default:
        break;
    }

    const int send = static_cast<int>(control) - static_cast<int>(MixControl::kSendA);
    const std::string slot = std::to_string(send + 1) + "- " + FxName(static_cast<std::size_t>(send));
    return {slot + " / Level", "Track: " + track + " - " + change};
}

// The note after a move of a track's `control` that stopped `stopped` of the track's settings at the end of the range
// (edit::MixEdited::stopped). A raised volume or send stops at the top. A pan stops at the edge that it moves to:
// the right one where `raised`.
std::string StoppedNote(MixControl control, std::size_t stopped, bool raised)
{
    const bool one = stopped == 1;
    const std::string count = std::to_string(stopped);
    const std::string settings = one ? " setting" : " settings";
    const std::string moves = one ? " It moves" : " They move";
    const std::string rises = one ? " It rises" : " They rise";
    switch (control)
    {
    case MixControl::kVolume:
        return count + " volume" + settings + " reached the highest volume." + rises +
               " less than the rest of the track.";
    case MixControl::kPan:
        return count + " pan" + settings + " reached the " + (raised ? "right" : "left") + " edge." + moves +
               " less than the rest of the track.";
    default:
        return count + " send" + settings + " reached the highest level." + rises + " less than the rest of the track.";
    }
}

// The edits of the notes of `part` on `track` that `part_edit` makes.
//
// Each command of the part's notes takes the edit once, in the stretch that ran its first note there.
std::vector<edit::NoteEdit> PartNoteEdits(const PartEdit& part_edit, const song::Sequence& song_sequence,
                                          const song::Track& track, const PartView& part, const format::File* document,
                                          const std::optional<uint32_t>& sequence)
{
    std::vector<edit::NoteEdit> edits;
    std::set<uint32_t> commands;
    const auto add = [&](const song::Note& note, const song::Stretch& stretch)
    {
        if (!commands.insert(note.command).second)
        {
            return;
        }

        edit::NoteEdit& note_edit = edits.emplace_back(edit::NoteEdit{.command = note.command, .stretch = stretch});
        switch (part_edit.value)
        {
        case PartValue::kStart:
            note_edit.shift = part_edit.amount;
            break;
        case PartValue::kTranspose:
            note_edit.transpose = part_edit.amount;
            break;
        case PartValue::kVelocity:
            note_edit.velocity = std::clamp(note.velocity + part_edit.amount, 1, kTopVelocity);
            break;
        }
    };

    for (std::size_t n = part.first; n < part.first + part.notes.size() && n < track.notes.size(); n++)
    {
        const song::Note& note = track.notes[n];
        add(note, song::StretchesOfNote(song_sequence, track, note)[note.stretch]);
    }

    // On the GBA and the DS, the edit includes the notes of the part that the driver never started too. In enhanced
    // mixing, they play. Each belongs to the stretch that ran its command at the start of the note.
    if (!part.unstarted.empty())
    {
        const std::vector<std::optional<edit::NotePlace>> places = edit::UnstartedPlaces(*document, *sequence, track);
        for (std::size_t n = part.unstarted_first;
             n < part.unstarted_first + part.unstarted.size() && n < places.size(); n++)
        {
            if (places[n])
            {
                add(track.unstarted[n], places[n]->stretch);
            }
        }
    }

    return edits;
}

} // namespace

void App::ApplyTempoMove(const TempoMove& move)
{
    // The point leads to the command of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = EditedSequence();
    const song::Track* track = TrackNumbered(move.point.track);
    if (!sequence || track == nullptr || move.point.stretch >= track->stretches.size())
    {
        return;
    }

    // A tempo that the console's bytes round to the command's tempo changes nothing.
    try
    {
        const std::optional<ntdlib::commands::Command> command =
            move.point.bpm ? TempoCommand(*document, *sequence, move.point, move.to) : std::nullopt;
        const ntdlib::commands::Command& old = edit::CommandAt(*document, *sequence, move.point.command);
        const bool changed = command && command->value.value != old.value.value;
        const edit::CommandPlace place{move.point.command, track->stretches[move.point.stretch]};
        if (move.shift == 0 && !changed)
        {
            return;
        }

        // The Undo History names every drag of a tempo point Edit Tempo Track. The details name the track and the
        // tempos. The details of an FX channel's volume take the same form. The editors' selections stay.
        if (move.shift == 0)
        {
            const std::string details = "Track: " + move.point.track_name + " - from " +
                                        ValueText({}, old.kind, old.value.value) + " to " +
                                        ValueText({}, command->kind, command->value.value);
            // The other tracks' commands of the same tempo at the point's tick take the new tempo too (TempoChangeOf).
            const TempoChange change = TempoChangeOf(*document, *sequence, *sequence_, move.point, *command);
            edit::Edited edited = edit::ChangeCommands(*document, *sequence, change.places, change.commands);
            DoSequenceEdit("Edit Tempo Track", details, *sequence, std::move(edited), key_editor_.selected);
            return;
        }

        // A move goes first, and the new tempo after it.
        const edit::SettingMove setting{place.command, place.stretch, move.shift};
        edit::Edited edited;
        if (changed)
        {
            const TempoChange change = TempoChangeOf(*document, *sequence, *sequence_, move.point, *command);
            edited = edit::MoveAndChangeCommands(*document, *sequence, std::span(&setting, 1), change.places,
                                                 change.commands, track->stretches);
        }
        else
        {
            edited = edit::MoveSettings(*document, *sequence, std::span(&setting, 1), track->stretches);
        }
        DoSequenceEdit("Edit Tempo Track", "Track: " + move.point.track_name + " - Tempo", *sequence, std::move(edited),
                       key_editor_.selected);
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ApplyPartEdit(const PartEdit& part_edit)
{
    if (!part_edit.refused.empty())
    {
        ShowMessage(part_edit.refused);
        return;
    }

    // The part's notes lead to the commands of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = EditedSequence();
    if (!sequence || !sequence_ || !song_ || part_edit.track < 0 ||
        static_cast<std::size_t>(part_edit.track) >= sequence_->tracks.size() ||
        static_cast<std::size_t>(part_edit.track) >= song_->tracks.size())
    {
        return;
    }

    const song::Track& track = sequence_->tracks[static_cast<std::size_t>(part_edit.track)];
    const TrackView& view = song_->tracks[static_cast<std::size_t>(part_edit.track)];
    if (part_edit.part < 0 || static_cast<std::size_t>(part_edit.part) >= view.parts.size())
    {
        return;
    }

    const PartView& part = view.parts[static_cast<std::size_t>(part_edit.part)];
    try
    {
        const std::vector<edit::NoteEdit> edits = PartNoteEdits(part_edit, *sequence_, track, part, document, sequence);
        if (edits.empty())
        {
            return;
        }

        // The Undo History names a part's new start Move, a transposition Transpose and a velocity Change Part
        // Velocity. The part's notes stay selected in the Key Editor.
        edit::Edited edited = edit::EditNotes(*document, *sequence, edits, {});
        const char* name = part_edit.value == PartValue::kStart       ? "Move"
                           : part_edit.value == PartValue::kTranspose ? "Transpose"
                                                                      : "Change Part Velocity";
        const std::size_t count = part.notes.size() + part.unstarted.size();
        const std::string details =
            "Track: " + view.name + " - " + (count == 1 ? "Note" : std::to_string(count) + " Notes");
        DoSequenceEdit(name, details, *sequence, std::move(edited), key_editor_.selected);
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ApplyLoopMove(const LoopMove& move)
{
    // The loop is the loop of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = EditedSequence();
    if (!sequence || move.start < 0 || move.end <= move.start)
    {
        return;
    }

    // The Undo History names a drag of a loop's edge Size. The details name the sound and the edge.
    try
    {
        const edit::ConvertedLoop loop{static_cast<uint32_t>(move.start), static_cast<uint32_t>(move.end)};
        edit::Edited moved = edit::MoveLoop(*document, *sequence, loop);
        const double quarter = std::max(song_ ? song_->ticks_per_quarter : 48, 1);
        const int beats = song_ ? song_->beats_per_bar : 4;
        const auto position = [quarter, beats](int32_t tick)
        {
            return WithoutFigureSpaces(PositionText(tick / quarter, beats));
        };
        const std::string details =
            "Sound: " + (song_ ? song_->name : std::string()) + " - " +
            (move.end_edge ? "Loop End from " + position(move.from_end) + " to " + position(move.end)
                           : "Loop Start from " + position(move.from_start) + " to " + position(move.start));
        DoSequenceEdit("Size", details, *sequence, std::move(moved), key_editor_.selected);
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ApplyMixMoves()
{
    for (std::size_t i = 0; song_ && i < song_->tracks.size(); i++)
    {
        TrackView& view = song_->tracks[i];
        const std::optional<MixMove> move = std::exchange(view.moved, std::nullopt);
        if (move && !(sequence_ && i < sequence_->tracks.size() && ApplyMixMove(sequence_->tracks[i], *move)))
        {
            view.held.reset();
        }
    }
}

bool App::ApplyMixMove(const song::Track& track, const MixMove& move)
{
    // The mixer's tracks lead to the commands of the version that built the open archive. A control holds its new value
    // until the new version's sequence shows it, or until a move in the history changes the archive (FollowHistory).
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = EditedSequence();
    const bool gba = sequence_ && (sequence_->gba || sequence_->driver);
    const int most = sequence_ && sequence_->driver ? sequence_->driver->MostVolume(track.number) : 127;
    const int from = MixValue(move.control, move.from, gba, most);
    // A track's volume stops at 127 on the DS and DSE, where the fader shows 0 dB. The fader and its scale stop there
    // too (SongView::volume_highest).
    const int to = played_.Ds() || played_.Dse()
                       ? std::min(MixValue(move.control, move.to, gba), edit::MixHighest(MixOf(move.control), true))
                       : MixValue(move.control, move.to, gba, most);
    if (!sequence || from == to)
    {
        return false;
    }

    try
    {
        edit::MixEdited edited = edit::EditMix(*document, *sequence, track.number,
                                               sequence_ ? song::MixStretches(*sequence_, track) : track.stretches,
                                               {MixOf(move.control), from, to}, PreviousTrackStretches(track));
        const ActionNames names = MixMoveNames(track.name, move.control, from, to, gba, most);
        const std::size_t stopped = edited.stopped;
        DoSequenceEdit(names.name, names.details, *sequence, std::move(edited), key_editor_.selected);

        // A move that stops some of the track's settings at the end of the range flattens the track's changes there. A
        // note says how many.
        if (stopped > 0)
        {
            ShowMessage(StoppedNote(move.control, stopped, to > from), MessageKind::kNote);
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
        return false;
    }

    return true;
}

void App::ApplyInstrumentChoices()
{
    for (std::size_t i = 0; song_ && i < song_->tracks.size(); i++)
    {
        const std::optional<InstrumentChoice> choice = std::exchange(song_->tracks[i].instrument_choice, std::nullopt);
        if (choice && sequence_ && i < sequence_->tracks.size())
        {
            ApplyInstrumentChoice(sequence_->tracks[i], *choice);
        }
    }
}

void App::ApplyInstrumentChoice(const song::Track& track, const InstrumentChoice& choice)
{
    // The track's instrument is the one that its first note plays. The edit goes into the version that built the open
    // archive. The mixer's edits do the same.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = EditedSequence();
    if (!sequence)
    {
        return;
    }

    const edit::Program to{choice.slot, choice.program};
    if (track.notes.empty())
    {
        // A track without notes uses the instrument in its opening.
        try
        {
            edit::Edited edited = edit::EditOpeningInstrument(*document, *sequence, track, to);
            DoSequenceEdit("Change Program", "Track: " + track.name + " - " + InstrumentText(*song_, to), *sequence,
                           std::move(edited), key_editor_.selected);
        }
        catch (const std::exception& error)
        {
            ShowMessage(error.what());
        }
        return;
    }

    const edit::Program from{track.notes.front().bank_slot, static_cast<int>(track.notes.front().program)};
    if (from == to)
    {
        return;
    }

    try
    {
        edit::Edited edited = edit::EditInstrument(*document, *sequence, track.number, track.stretches, from, to,
                                                   PreviousTrackStretches(track));
        const std::string details =
            "Track: " + track.name + " - " + InstrumentText(*song_, from) + " to " + InstrumentText(*song_, to);
        DoSequenceEdit(to.slot != from.slot ? "Change Bank" : "Change Program", details, *sequence, std::move(edited),
                       key_editor_.selected);
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

std::span<const song::Stretch> App::PreviousTrackStretches(const song::Track& track) const
{
    if (!sequence_ || !song::KonamiRevisionOf(sequence_->driver))
    {
        return {};
    }

    const auto before = std::ranges::find(sequence_->tracks, track.number - 1, &song::Track::number);
    return before != sequence_->tracks.end() ? std::span<const song::Stretch>(before->stretches)
                                             : std::span<const song::Stretch>();
}

bool App::WindowWaits() const
{
    return replace_.shown.has_value() || export_.has_value() || new_project_open_;
}

bool App::TracksEditable() const
{
    // A GBA ROM contains no tables. The headers of its songs list their tracks. A DS sequence opens its tracks itself,
    // and a DSE file lists them in its chunks.
    const format::File* document = Document();
    const bool native = document != nullptr && document->native;
    const bool tables = (played_.Sar() || played_.Wii()) && start_fields_ && native && document->native->tables;
    const bool gba = played_.Gba() && native && document->native->engine == format::Engine::kGba;
    const bool ds = (played_.Ds() && native && document->native->engine == format::Engine::kDs) ||
                    (played_.Dse() && native && document->native->engine == format::Engine::kDse);
    const bool driver = played_.Driver() && native && document->native->driver;
    return history_ && !preview_ && sequence_ && (tables || gba || ds || driver) && archive_version_ &&
           ShownSequence().has_value() && SameArchive(*document, *archive_version_) && reading_version_ == nullptr &&
           !WindowWaits();
}

void App::AddSelectedTrack(const format::File& document, const std::vector<std::optional<edit::SoundPlace>>& places,
                           uint32_t sound, const song::Track* selected)
{
    // The new track plays the instrument of the selected track's first note.
    std::optional<edit::Program> instrument;
    if (selected != nullptr && !selected->notes.empty())
    {
        const song::Note& first = selected->notes.front();
        instrument = edit::Program{first.bank_slot, static_cast<int>(first.program)};
    }
    edit::TrackEdit added = edit::AddTrack(document, places, sound, *sequence_, instrument);
    track_to_select_ = added.track;
    Do("Add Track", "Sound: " + song_->name + " - Track " + std::to_string(added.track), std::move(added.file));
}

std::string App::CopiedTrackSoundName(const format::File& document) const
{
    std::string from;
    if (played_.Gba())
    {
        from = song::GbaSongName(*played_.Gba(), static_cast<int>(copied_track_->sound));
    }
    else if (played_.Ds())
    {
        from = song::DsSoundName(*played_.Ds(), copied_track_->sound);
    }
    else if (played_.Dse())
    {
        from = song::DseSoundName(*played_.Dse(), copied_track_->sound);
    }
    else if (played_.Driver())
    {
        from = played_.Driver()->SoundName(copied_track_->sound);
    }
    else if (document.native->tables && copied_track_->sound < document.native->tables->sounds.size())
    {
        from = document.native->tables->sounds[copied_track_->sound].name;
    }

    return from;
}

void App::EditTracks(TrackAction action)
{
    if (!TracksEditable())
    {
        return;
    }

    // The edit finds each sound's start in the version that built the open archive.
    const format::File& document = *Document();
    const std::vector<std::optional<edit::SoundPlace>> places = Places();
    const uint32_t sound = sequence_->sound;
    const song::Track* selected = SelectedTrack();
    try
    {
        switch (action)
        {
        case TrackAction::kAdd:
            {
                AddSelectedTrack(document, places, sound, selected);
                break;
            }

        case TrackAction::kDuplicate:
            {
                if (selected == nullptr)
                {
                    return;
                }

                edit::TrackEdit copied = edit::DuplicateTrack(document, places, sound, selected->number);
                track_to_select_ = copied.track;
                Do("Duplicate Track",
                   "Sound: " + song_->name + " - Track " + std::to_string(selected->number) + " to Track " +
                       std::to_string(copied.track),
                   std::move(copied.file));
                break;
            }

        case TrackAction::kRemove:
            {
                if (selected == nullptr)
                {
                    return;
                }

                const int number = selected->number;
                format::File removed = edit::RemoveTrack(document, places, sound, number);
                Do("Remove Track", "Sound: " + song_->name + " - Track " + std::to_string(number), std::move(removed));
                break;
            }

        case TrackAction::kCopy:
            {
                if (selected == nullptr)
                {
                    return;
                }

                copied_track_ = edit::CopyTrack(document, places, sound, *sequence_, selected->number);
                break;
            }

        case TrackAction::kPaste:
            {
                if (!copied_track_)
                {
                    return;
                }

                // The details name the sound that the track came from.
                const std::string from = CopiedTrackSoundName(document);
                edit::TrackEdit pasted = edit::PasteTrack(document, places, sound, *sequence_, *copied_track_);
                track_to_select_ = pasted.track;
                Do("Paste Track",
                   "Sound: " + song_->name + " - Track " + std::to_string(copied_track_->track) + " of " + from +
                       " to Track " + std::to_string(pasted.track),
                   std::move(pasted.file));
                break;
            }
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

} // namespace hokuto::app
