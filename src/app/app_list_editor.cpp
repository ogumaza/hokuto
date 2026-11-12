// SPDX-License-Identifier: MIT

// The List Editor: its track, markers, commands and subroutines, and its edits.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <span>
#include <utility>

#include "app/app.h"
#include "app/app_internal.h"
#include "app/engine_names.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/notes.h"
#include "edit/subroutines.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/archive.h"

namespace hokuto::app
{

std::pair<int, std::optional<int32_t>> App::ListEditorChoice() const
{
    // The selected part's track, or else the selected track, or else the first track.
    const PartView* part = SelectedPart(song_, arrangement_);
    const auto tracks = static_cast<int>(sequence_->tracks.size());
    std::optional<int32_t> start;
    int number = sequence_->tracks.empty() ? 0 : sequence_->tracks.front().number;
    if (part != nullptr && arrangement_.selected_part_track >= 0 && arrangement_.selected_part_track < tracks)
    {
        number = sequence_->tracks[static_cast<std::size_t>(arrangement_.selected_part_track)].number;
        start = part->start;
    }
    else if (arrangement_.selected_track >= 0 && arrangement_.selected_track < tracks)
    {
        number = sequence_->tracks[static_cast<std::size_t>(arrangement_.selected_track)].number;
    }

    return {number, start};
}

void App::OpenListEditor()
{
    if (!sequence_)
    {
        return;
    }

    const auto [number, start] = ListEditorChoice();
    list_editor_open_ = true;
    lower_front_ = LowerZone::kListEditor;
    ShowListTrack(number);

    // The editor selects the row where the part starts, and scrolls to it: a call's row, or the part's first note's. A
    // row after a wait of unknown length has no start.
    const PartView* part = SelectedPart(song_, arrangement_);
    const auto played = static_cast<std::size_t>(arrangement_.selected_part_track);
    if (start && part != nullptr && played < sequence_->tracks.size())
    {
        if (const std::optional<std::size_t> row =
                RowOfPart(list_track_, sequence_->tracks[played], part->first, *start))
        {
            list_editor_.selected = {list_track_.rows[*row].row.command};
            list_editor_.anchor = *row;
            list_editor_.scroll_row = *row;
        }
    }
}

void App::OpenMarker(const MarkerPlace& place)
{
    if (!sequence_ || !song_ || place.marker >= song_->logic.size() || song_->logic[place.marker].commands.empty())
    {
        return;
    }

    // The place's commands on the track of its first command.
    const LogicMarkerView& marker = song_->logic[place.marker];
    const int track = marker.commands.front().track;
    std::vector<uint32_t> commands;
    for (const MarkerCommand& command : marker.commands)
    {
        if (command.track == track)
        {
            commands.push_back(command.offset);
        }
    }
    OpenCommands(track, commands, place.tick);
}

void App::OpenCommands(int track, std::span<const uint32_t> commands, int32_t tick)
{
    list_editor_open_ = true;
    lower_front_ = LowerZone::kListEditor;
    ShowListTrack(track);

    // The commands, and the first row that runs one of them at the tick.
    list_editor_.selected.insert(commands.begin(), commands.end());
    for (std::size_t i = 0; i < list_track_.rows.size(); i++)
    {
        const ListRow& row = list_track_.rows[i];
        if (!IsLabel(row) && row.row.tick == static_cast<uint32_t>(tick) &&
            list_editor_.selected.contains(row.row.command))
        {
            list_editor_.scroll_row = i;
            break;
        }
    }
}

void App::CloseListEditor()
{
    list_editor_open_ = false;
    ClearListSelection();
    list_subroutine_.reset();
}

void App::ClearListSelection()
{
    list_editor_.selected.clear();
    list_editor_.selected_label.reset();
    list_editor_.anchor.reset();
    list_editor_.typing.reset();
    list_selection_.reset();
}

void App::ShowListTrack(int track)
{
    list_editor_track_ = track;
    ClearListSelection();
    list_subroutine_.reset();
    UpdateListTrack();
    list_editor_.scroll_row = 0;
}

void App::UpdateListTrack()
{
    // The selection follows the version of the shown sequence that the editor shows.
    const std::optional<uint32_t> sequence = ShownSequence();
    const format::Shared<ntdlib::commands::Sequence> shown = VersionSequence(sequence);
    list_editor_.selected = FollowedSelection({list_sequence_, list_editor_track_, std::move(list_editor_.selected)},
                                              std::exchange(list_selection_, std::nullopt), shown);
    list_sequence_ = shown;
    if (!list_editor_open_ || !sequence_ || !sequence || !archive_version_)
    {
        list_track_ = ListTrack{};
        return;
    }

    // The editor shows the track's name and colour from the arrangement. A subroutine that it shows by itself shows the
    // rows of one run. Each label has a row before its command.
    FollowSubroutine(shown);
    list_track_ = MakeListTrack(*archive_version_, *sequence, *sequence_, list_editor_track_);
    for (std::size_t i = 0; song_ && i < sequence_->tracks.size() && i < song_->tracks.size(); i++)
    {
        if (sequence_->tracks[i].number == list_editor_track_)
        {
            list_track_.name = song_->tracks[i].name;
            list_track_.colour = song_->tracks[i].colour;
        }
    }
    if (list_subroutine_)
    {
        list_track_ = WithSubroutine(std::move(list_track_), *list_subroutine_);
    }
    list_track_ = WithLabelRows(std::move(list_track_));

    // A command that the player of the engine that plays the sound skips shows grey. A conversion of a song of the GBA
    // or of another of the GBA's drivers, a sequence of the DS or a sound of DSE contains other commands.
    if (engine_sound_ && !song::OnGbaOrDs(played_))
    {
        list_track_.engine =
            engine_sound_->played.Ds() ? song::Console{.engine = format::Engine::kDs}
            : engine_sound_->played.Wii()
                ? song::Console{.engine = format::Engine::kWii, .wii = engine_sound_->wii_build}
                : song::Console{.engine = format::EngineOf(engine_sound_->played.Sar()->ReadOptions().flavor)};
    }
    list_track_.engine_name = EngineName(PlayedEngine());
    const format::File* document = Document();
    list_track_.file_engine_name = EngineName(document != nullptr ? document->engine : PlayedEngine());
}

void App::FollowSubroutine(const format::Shared<ntdlib::commands::Sequence>& shown)
{
    if (!list_subroutine_ || !shown || !sequence_ || !archive_version_)
    {
        list_subroutine_.reset();
        return;
    }

    // In another version, the subroutine's label finds its first command.
    const std::vector<uint32_t> offsets = edit::Offsets(*shown, edit::ConsoleOf(*archive_version_));
    if (!list_subroutine_version_.SharesWith(shown))
    {
        const auto& labels = shown->labels;
        const auto label = std::find_if(labels.begin(), labels.end(),
                                        [this](const auto& other) { return other.name == list_subroutine_->name; });
        if (list_subroutine_->name.empty() || label == labels.end() || label->item >= offsets.size())
        {
            list_subroutine_.reset();
            return;
        }

        list_subroutine_->first = offsets[label->item];
        list_subroutine_version_ = shown;
    }

    // The runs in the shown sound, and the calls in the sequence. The shown run keeps its place among the runs.
    ListSubroutine& subroutine = *list_subroutine_;
    subroutine.runs = SubroutineRuns(*sequence_, subroutine.first);
    subroutine.sound_calls = CallsOfRuns(*sequence_, subroutine.runs);
    subroutine.loops = sequence_->loop.has_value();
    const std::optional<uint32_t> item = edit::ItemAtOffset(offsets, subroutine.first);
    if (subroutine.runs.empty() || !item)
    {
        list_subroutine_.reset();
        return;
    }

    subroutine.shown = std::min(subroutine.shown, subroutine.runs.size() - 1);
    subroutine.calls = static_cast<int>(edit::CallsTo(*shown, *item).size());
    list_editor_track_ = subroutine.runs[subroutine.shown].track;
}

void App::OpenSubroutine(const ListEditorState::SubroutineChoice& choice)
{
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!sequence || !sequence_ || !archive_version_ || *sequence >= archive_version_->sequences.size())
    {
        return;
    }

    // The subroutine takes the name of the first label at its first command, in the order of the file.
    const format::Shared<ntdlib::commands::Sequence>& shown = archive_version_->sequences[*sequence];
    const std::vector<uint32_t> offsets = edit::Offsets(*shown, edit::ConsoleOf(*archive_version_));
    const std::optional<uint32_t> item = edit::ItemAtOffset(offsets, choice.first);
    ListSubroutine subroutine;
    subroutine.first = choice.first;
    for (const auto& label : shown->labels)
    {
        if (item && label.item == *item)
        {
            subroutine.name = label.name;
            break;
        }
    }
    subroutine.runs = SubroutineRuns(*sequence_, choice.first);
    subroutine.sound_calls = CallsOfRuns(*sequence_, subroutine.runs);
    subroutine.loops = sequence_->loop.has_value();
    if (subroutine.runs.empty())
    {
        ShowMessage("The current sound never calls this subroutine.", MessageKind::kNote);
        return;
    }

    // The chosen run, or the first on the shown track, or the first. std::find of a ListCall fails to compile under
    // clang-cl with Microsoft's STL 14.51 (microsoft/STL#6294).
    const auto& runs = subroutine.runs;
    auto run = choice.run ? std::find_if(runs.begin(), runs.end(),
                                         [&choice](const ListCall& other) { return other == *choice.run; })
                          : runs.end();
    if (run == runs.end())
    {
        run = std::find_if(runs.begin(), runs.end(),
                           [this](const ListCall& other) { return other.track == list_editor_track_; });
    }
    subroutine.shown = run == runs.end() ? 0 : static_cast<std::size_t>(run - runs.begin());

    list_subroutine_ = std::move(subroutine);
    list_subroutine_version_ = shown;
    ClearListSelection();
    UpdateListTrack();
    list_editor_.scroll_row = 0;
}

void App::CloseSubroutine()
{
    if (!list_subroutine_)
    {
        return;
    }

    const std::optional<ListCall> run = list_subroutine_->shown < list_subroutine_->runs.size()
                                            ? std::optional<ListCall>(list_subroutine_->runs[list_subroutine_->shown])
                                            : std::nullopt;
    list_subroutine_.reset();
    ClearListSelection();
    if (run)
    {
        list_editor_track_ = run->track;
    }
    UpdateListTrack();

    // The call that started the run ends the stretch before it.
    list_editor_.scroll_row = 0;
    for (std::size_t i = list_track_.rows.size(); run && i-- > 0;)
    {
        const ListRow& row = list_track_.rows[i];
        if (!IsLabel(row) && row.row.stretch + 1 == run->stretch && row.command.kind == ntdlib::commands::Kind::kCall)
        {
            list_editor_.selected = {row.row.command};
            list_editor_.anchor = i;
            list_editor_.scroll_row = i;
            break;
        }
    }
}

void App::UpdateListEditor()
{
    if (!list_editor_open_ || !sequence_ || FrontOfLowerZone() != LowerZone::kListEditor)
    {
        list_editor_.focused = false;
        return;
    }

    // While the reader builds the archive of another version, the edits wait for it.
    const bool busy = reading_version_ != nullptr;
    const std::optional<ListEdit> edit =
        DrawListEditor(list_track_, list_editor_, transport_, ItemShown(ShownSound()), busy, &list_editor_open_);
    if (!list_editor_open_)
    {
        CloseListEditor();
        return;
    }

    // The edit goes first: it names the rows that the editor showed. Another track then shows from its first row. A
    // subroutine shows by itself, or its track shows again.
    if (edit)
    {
        ApplyListEdit(*edit);
    }
    if (const std::optional<int> chosen = std::exchange(list_editor_.chosen_track, std::nullopt))
    {
        ShowListTrack(*chosen);
    }
    if (const auto choice = std::exchange(list_editor_.open_subroutine, std::nullopt))
    {
        OpenSubroutine(*choice);
    }
    if (std::exchange(list_editor_.close_subroutine, false))
    {
        CloseSubroutine();
    }
}

void App::ApplyListEdit(const ListEdit& edit)
{
    if (!edit.refused.empty())
    {
        ShowMessage(edit.refused);
        return;
    }

    // The editor's rows lead to the commands of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = EditedSequence();
    if (!sequence || edit.rows.empty() || edit.rows.front() >= list_track_.rows.size())
    {
        return;
    }

    try
    {
        // A change, a move and a new command go through the row's place. A deletion takes each row's command.
        const std::size_t first = edit.rows.front();
        const edit::CommandPlace place = PlaceOf(list_track_, first);
        edit::Edited edited = ListEditOf(edit, *document, *sequence, place, first);

        // The changed, moved or new command stays selected in the new version, and so does a new call or a new copy of
        // a subroutine's commands. A label that an edit adds, renames or moves stays selected. The Key Editor's notes
        // stay selected too.
        const ntdlib::commands::Sequence& commands = *edited.file.sequences.at(*sequence);
        const std::vector<uint32_t> offsets = edit::Offsets(commands, edit::ConsoleOf(edited.file));
        SelectListAfter(edit, edited, *document, *sequence, place, commands, offsets);
        KeySelection notes = SelectionAfter(*document, *sequence, edited, key_editor_.selected);

        // A renamed subroutine keeps showing by itself. One without a label follows its first command.
        FollowListSubroutine(edit, edited, *document, *sequence, commands, offsets);

        // The Undo History names the track and the command, or the label or subroutine.
        const std::string what = ListEditName(edit, first);
        Do(edit.name, "Track: " + list_track_.name + " - " + what, std::move(edited.file), std::move(notes));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

edit::Edited App::ListEditOf(const ListEdit& edit, const format::File& document, uint32_t sequence,
                             const edit::CommandPlace& place, std::size_t first) const
{
    edit::Edited edited;
    switch (edit.action)
    {
    case ListEdit::Action::kChange:
        edited = edit::ChangeCommand(document, sequence, place, edit.command);
        break;

    case ListEdit::Action::kMove:
        if (list_track_.rows[first].command.kind == ntdlib::commands::Kind::kNote)
        {
            const edit::NoteEdit note{.command = place.command, .stretch = place.stretch, .shift = edit.shift};
            edited = edit::EditNotes(document, sequence, std::span(&note, 1));
        }
        else
        {
            const edit::SettingMove move{.command = place.command, .stretch = place.stretch, .shift = edit.shift};
            edited = edit::MoveSettings(document, sequence, std::span(&move, 1), list_track_.stretches);
        }
        break;

    case ListEdit::Action::kInsert:
        edited = edit::InsertCommand(document, sequence, place, list_track_.rows[first].entered, edit.command);
        break;

    case ListEdit::Action::kRemove:
        {
            std::vector<edit::CommandPlace> places;
            for (const std::size_t row : edit.rows)
            {
                places.push_back(PlaceOf(list_track_, row));
            }
            edited = edit::RemoveCommands(document, sequence, places);
            break;
        }

    case ListEdit::Action::kAddLabel:
        edited = edit::AddLabel(document, sequence, place.command, edit.label);
        break;

    case ListEdit::Action::kRenameLabel:
        edited = edit::RenameLabel(document, sequence, edit.label, edit.new_name);
        break;

    case ListEdit::Action::kMoveLabel:
        edited = edit::MoveLabel(document, sequence, edit.label, place.command);
        break;

    case ListEdit::Action::kRemoveLabel:
        edited = edit::RemoveLabel(document, sequence, edit.label);
        break;

    case ListEdit::Action::kToSubroutine:
        {
            std::vector<uint32_t> selected;
            for (const std::size_t row : edit.rows)
            {
                selected.push_back(list_track_.rows.at(row).row.command);
            }
            edited = edit::ToSubroutine(document, sequence, selected, edit.depth, edit.label);
            break;
        }

    case ListEdit::Action::kDissolveCall:
        edited = edit::DissolveCall(document, sequence, place.command);
        break;

    case ListEdit::Action::kCopySubroutine:
        edited = edit::CopySubroutine(document, sequence, place.command);
        break;
    }

    return edited;
}

void App::SelectListAfter(const ListEdit& edit, const edit::Edited& edited, const format::File& document,
                          uint32_t sequence, const edit::CommandPlace& place,
                          const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets)
{
    using Action = ListEdit::Action;
    std::vector<std::optional<uint32_t>> items;
    std::optional<std::string> label;
    switch (edit.action)
    {
    case Action::kInsert:
    case Action::kToSubroutine:
        if (!edited.added.empty())
        {
            items.emplace_back(edited.added.front());
        }
        break;

    case Action::kDissolveCall:
        items.assign(edited.added.begin(), edited.added.end());
        break;

    case Action::kAddLabel:
    case Action::kMoveLabel:
        label = edit.label;
        break;

    case Action::kRenameLabel:
        label = edit.new_name;
        break;

    case Action::kRemove:
    case Action::kRemoveLabel:
        break;

    default:
        items.push_back(edited.items.at(edit::ItemOf(document, sequence, place.command)));
        break;
    }

    KeySelection after{edited.file.sequences[sequence], list_editor_track_, {}};
    for (const std::optional<uint32_t>& item : items)
    {
        if (item && *item < commands.items.size())
        {
            after.commands.insert(offsets[*item]);
        }
    }
    list_selection_ = std::move(after);
    list_editor_.selected_label = label;
}

void App::FollowListSubroutine(const ListEdit& edit, const edit::Edited& edited, const format::File& document,
                               uint32_t sequence, const ntdlib::commands::Sequence& commands,
                               const std::vector<uint32_t>& offsets)
{
    using Action = ListEdit::Action;
    if (list_subroutine_ && edit.action == Action::kRenameLabel && edit.label == list_subroutine_->name)
    {
        list_subroutine_->name = edit.new_name;
    }
    else if (list_subroutine_ && list_subroutine_->name.empty())
    {
        const std::optional<uint32_t> moved =
            edited.items.at(edit::ItemOf(document, sequence, list_subroutine_->first));
        if (moved && *moved < commands.items.size())
        {
            list_subroutine_->first = offsets[*moved];
            list_subroutine_version_ = edited.file.sequences[sequence];
        }
    }
}

std::string App::ListEditName(const ListEdit& edit, std::size_t first) const
{
    using Action = ListEdit::Action;
    const bool inserted = edit.action == Action::kInsert;
    std::string what = edit.rows.size() == 1
                           ? std::string(KindName(inserted ? edit.command.kind : list_track_.rows[first].command.kind))
                           : std::to_string(edit.rows.size()) + " Commands";
    switch (edit.action)
    {
    case Action::kAddLabel:
    case Action::kMoveLabel:
    case Action::kRemoveLabel:
    case Action::kToSubroutine:
        what = edit.label;
        break;

    case Action::kRenameLabel:
        what = edit.label + " to " + edit.new_name;
        break;

    case Action::kDissolveCall:
    case Action::kCopySubroutine:
        what = CellText(list_track_, list_track_.rows[first], ListColumn::kData1);
        break;

    default:
        break;
    }

    return what;
}

} // namespace hokuto::app
