// SPDX-License-Identifier: MIT

// What the List Editor works out about a track's commands without a window: the rows, the text of each column, the
// edits of the values that the user types, and the commands that it inserts and deletes. Its tests run without a
// window.
//
// The columns are the type, the start, the length, two data values and a comment. A note's data are its pitch and
// velocity. A command that addresses a bus, an LFO, a variable or a track names it in the first data value, and gives
// its value in the second. A jump or a call names its target in the first, and a track start in the second. Any other
// command gives its value in the first. The length holds a note's length, a wait's length and the time over which a
// setting moves to its value. The comment names the labels at the command, until the labels have rows. A command
// without a label counts there the calls, jumps and track starts that go to it, such as a jump's target by its offset.
//
// The List Editor shows each label of a program in a separate row before its command, with the label's name in the
// first data value. The comment counts the calls, jumps and track starts that go there. The editor can also show a
// subroutine by itself: the rows of one call's run of it, with the other calls in its toolbar.

#pragma once

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "edit/commands.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace hokuto::app
{

// The List Editor's columns, in their order.
enum class ListColumn
{
    kType,
    kStart,
    kLength,
    kData1,
    kData2,
    kComment,
};

inline constexpr int kListColumns = 6;

// A row of the List Editor: a command as the track ran it, with its target as an item of the sequence. `entered` is set
// where the track entered the sequence at the command: by its start, a jump, a call, a return or a loop's end. `depth`
// counts the calls and loops on the track's stack. A label's row holds the label's name in `label`, and the rest of the
// row of the label's command.
struct ListRow
{
    edit::CommandRow row;
    ntdlib::commands::Command command;
    bool entered = false;
    int depth = 0;
    std::string label;
};

// The commands of a sequence that go to an item: its calls, its jumps and its track starts.
struct Entries
{
    int calls = 0;
    int jumps = 0;
    int starts = 0;
};

// A run of a subroutine: the track that ran it, the stretch of the track's commands that a call started there, and the
// tick at which the call ran.
struct ListCall
{
    friend bool operator==(const ListCall&, const ListCall&) = default;

    int track = 0;
    uint32_t stretch = 0;
    uint32_t tick = 0;
};

// The subroutine that the List Editor shows by itself: its name, the offset of its first command, each run of it in the
// shown sound, and the run that the rows show. `calls` counts the calls in the sequence that go to it, from every
// sound. `sound_calls` counts the calls that started the runs, each once (CallsOfRuns). `loops` is set when the shown
// sound loops: the runs then stop at the end of the loop's first pass.
struct ListSubroutine
{
    std::string name;
    uint32_t first = 0;
    std::vector<ListCall> runs;
    std::size_t shown = 0;
    int calls = 0;
    int sound_calls = 0;
    bool loops = false;
};

// The track that the List Editor shows: its number, name and colour, the sequence's timebase and console, and the rows
// of its commands. `stretches` are the track's stretches. The rows name them by index. `offsets` holds the offset of
// each item of the sequence, then the end. `labels` holds the names of the labels at each item, and `targets` each
// label's name and item, in the order of the file. `entries` counts what goes to each item, and `runs` holds the
// offsets where calls in the shown sound started runs of subroutines. `tracks` holds the numbers of the tracks that ran
// commands. `timeline` is set for a sequence that the program edits as a timeline: its labels and subroutines come from
// the converter. `label_rows` is set once the labels have rows (WithLabelRows). While the editor shows a subroutine by
// itself, `subroutine` names it, and the rows are those of its run. A track whose commands can't show has the reason in
// `unavailable`. `engine` is the console whose engine plays the sound, when it isn't the file's. `engine_name` names
// the engine that plays the sound, and `file_engine_name` the file's engine.
struct ListTrack
{
    int number = 0;
    std::string name;
    ImU32 colour = 0;
    int ticks_per_quarter = 48;
    song::Console console;
    std::vector<ListRow> rows;
    std::vector<song::Stretch> stretches;
    std::vector<uint32_t> offsets;
    std::map<uint32_t, std::string> labels;
    std::vector<std::pair<std::string, uint32_t>> targets;
    std::map<uint32_t, Entries> entries;
    std::set<uint32_t> runs;
    std::vector<int> tracks;
    bool timeline = false;
    bool label_rows = false;
    std::optional<ListSubroutine> subroutine;
    std::string unavailable;
    std::optional<song::Console> engine;
    std::string engine_name;
    std::string file_engine_name;
};

// What the List Editor shows of track `number` of sequence `sequence` of `file`, from `song`, the sound as ntdlib's
// performer played it. The track can play no notes. The rows stop at the end of the first pass of the loop. The
// arrangement stops there too.
ListTrack MakeListTrack(const format::File& file, uint32_t sequence, const song::Sequence& song, int number);

// What the List Editor shows of each track of `song`, in the order of `song`'s tracks. The sequence's offsets and
// labels are worked out once for every track.
std::vector<ListTrack> MakeListTracks(const format::File& file, uint32_t sequence, const song::Sequence& song);

// `track` with a row for each label before each row of the label's command, in the order of the file. The commands'
// comments no longer name the labels. Only the List Editor shows the labels' rows. The marker row and the tempo row
// read the commands' rows alone.
ListTrack WithLabelRows(ListTrack track);

// The runs of the subroutine whose first command is at offset `first`, in `song`: each stretch of each track that a
// call started there, in the order of the tracks and then of the stretches. As the rows do, the runs stop at the end of
// the loop's first pass. A later pass's run would show no rows.
std::vector<ListCall> SubroutineRuns(const song::Sequence& song, uint32_t first);

// The row of `track` at which a part of the arrangement starts. `played` is the track as the sound played it, and
// `first` the index of the part's first note among its notes. A call's part starts at its call's row, and another part
// at its first note's row. A part without such a row starts at the first row at tick `start` or later. Returns nothing
// where no row lies there.
std::optional<std::size_t> RowOfPart(const ListTrack& track, const song::Track& played, std::size_t first,
                                     int32_t start);

// The calls of `song` that started `runs`, by their offsets, each once. A call in a loop starts a run in each pass.
int CallsOfRuns(const song::Sequence& song, std::span<const ListCall> runs);

// The count of `subroutine`'s calls that the List Editor's toolbar shows, such as "1 call in this sound, 62 elsewhere
// in the sequence", and the tooltip that says where the others lie.
std::string CallsText(const ListSubroutine& subroutine);
std::string CallsTooltip(const ListSubroutine& subroutine);

// `track` with the rows of run `subroutine.shown` of `subroutine` alone: from the stretch that the call started, up to
// the return. The rows sit as far in as the subroutine's calls and loops take them. A track that didn't run the run, or
// a run past the last, shows no rows.
ListTrack WithSubroutine(ListTrack track, ListSubroutine subroutine);

// Whether `row` is a label's row.
inline bool IsLabel(const ListRow& row)
{
    return !row.label.empty();
}

// Whether the engine that plays the sound of `track` skips the command of `row`. Its row shows grey. On another
// console's engine, the sound plays its conversion. The conversion leaves out the commands that the file's console
// skips too (edit::ForEngine). A label's row runs nothing.
bool Skipped(const ListTrack& track, const ListRow& row);

// The name of the engine whose player skips the command of `row`, for a row that Skipped greys: the file's engine
// first, then the engine that plays the sound.
const std::string& SkippingEngine(const ListTrack& track, const ListRow& row);

// The name of a command's kind in the Type column, such as Volume or Open Track.
std::string_view KindName(ntdlib::commands::Kind kind);

// Whether a command of `kind` turns something on or off. Its value shows as On or Off.
bool Switches(ntdlib::commands::Kind kind);

// A value of a command of `kind`: On or Off for a switch, or a number in the kind's unit, such as "120 BPM".
std::string ValueText(ntdlib::commands::Kind kind, ntdlib::commands::Fixed value);

// An argument of a command of `kind`: its value, a random range, or a variable.
std::string ArgumentText(ntdlib::commands::Kind kind, const ntdlib::commands::Argument& argument);

// The kinds of command that the toolbar's Insert menu offers, in groups.
struct KindGroup
{
    const char* name;
    std::vector<ntdlib::commands::Kind> kinds;
};

std::span<const KindGroup> InsertGroups();

// The text of `column` for `row` of `track`.
std::string CellText(const ListTrack& track, const ListRow& row, ListColumn column);

// Whether the user can type a value in `column` of `row`, and whether the column names the command's target. A target
// comes from the menu of the sequence's labels instead.
bool Typable(const ListTrack& track, const ListRow& row, ListColumn column);
bool NamesTarget(const ListRow& row, ListColumn column);

// An edit that the List Editor makes, for the program to carry out: a new form of the command of row `rows[0]`, a move
// of that row's note or setting by `shift` ticks, a new command before that row, or the removal of the commands of
// `rows`. The edits of labels name the label in `label`: a new label before the command of row `rows[0]`, a new name
// `new_name`, a move to the command of row `rows[0]`, and a removal. The commands of `rows` become a subroutine named
// `label`, with the deepest stack of calls and loops that they reach in `depth`. The call of row `rows[0]` gives way to
// its subroutine's commands, or takes a copy of the subroutine. The Undo History names an edit `name`. An edit that the
// commands can't take has the reason in `refused`. It changes nothing.
struct ListEdit
{
    enum class Action
    {
        kChange,
        kMove,
        kInsert,
        kRemove,
        kAddLabel,
        kRenameLabel,
        kMoveLabel,
        kRemoveLabel,
        kToSubroutine,
        kDissolveCall,
        kCopySubroutine,
    };

    Action action = Action::kChange;
    std::string name;
    std::vector<std::size_t> rows;
    ntdlib::commands::Command command;
    int32_t shift = 0;
    std::string label;
    std::string new_name;
    int depth = 0;
    std::string refused;
};

// The edit of `text`, typed into `column` of row `index` of `track`. A start moves a note the way that the Key Editor
// moves it, and a setting the way that a controller lane moves it (edit::MoveSettings). A length, a pitch, a velocity,
// a value, a bus, an LFO, a variable or a track changes the command. A value can come from a random number, typed as
// "random 0 to 127", or from a variable, typed as "variable 3". A value outside what the console's bytes hold is
// refused with its range. Text that the column can't read is refused with what the column takes. Nothing for the value
// that the row has already.
std::optional<ListEdit> TypedListEdit(const ListTrack& track, std::size_t index, ListColumn column,
                                      std::string_view text);

// `command` with the value typed as `text`. The List Editor's column of values reads the text the same way: a value
// with or without its unit, "Random 0 to 127" or "Variable 3". `refused` says why `console` can't hold the value.
// Nothing for text that the column can't read.
struct TypedArgument
{
    ntdlib::commands::Command command;
    std::string refused;
};

std::optional<TypedArgument> TypedValueOf(const song::Console& console, ntdlib::commands::Command command,
                                          std::string_view text);

// What a value takes, for the message that refuses text that TypedValueOf can't read.
std::string ValueHint();

// The edit that sends the jump, the call or the track start of row `index` to item `target`. Nothing when it goes there
// already.
std::optional<ListEdit> TargetEdit(const ListTrack& track, std::size_t index, uint32_t target);

// The edit that makes the command of row `index` run only if the last comparison held, or always.
ListEdit ConditionEdit(const ListTrack& track, std::size_t index);

// The edit that puts a new command of `kind` before row `index`, or before the last row without one (NewCommand). A
// jump or a call goes to the command after it, and a track start to the end of the sequence. Nothing on a track without
// rows.
std::optional<ListEdit> InsertEdit(const ListTrack& track, std::optional<std::size_t> index,
                                   ntdlib::commands::Kind kind);

// The edit that deletes the commands at `selected`, by their offsets. The Undo History names it Delete. Nothing without
// a selected command that the track shows.
std::optional<ListEdit> RemovalEdit(const ListTrack& track, const std::set<uint32_t>& selected);

// The place of the command of row `index` for an edit: its offset and its stretch.
edit::CommandPlace PlaceOf(const ListTrack& track, std::size_t index);

// The edit that puts a new label before the command of row `index`. The label takes the name Label, with a number after
// it where another label has the name. Nothing for a row that isn't a command's, or a timeline.
std::optional<ListEdit> LabelEdit(const ListTrack& track, std::size_t index);

// The edit that moves the label of row `label` to the command of row `index`. Nothing where the label is already there,
// or for rows that aren't a label's and a command's.
std::optional<ListEdit> LabelMoveEdit(const ListTrack& track, std::size_t label, std::size_t index);

// The edit that deletes the label of row `index`. Nothing for a row that isn't a label's.
std::optional<ListEdit> LabelRemovalEdit(const ListTrack& track, std::size_t index);

// The edit that makes a subroutine of the commands at `selected`, by their offsets, in the stretch of the first row
// that runs one of them. A selected call takes the rows of its run with it: the call goes into the subroutine, and the
// commands that it calls stay where they are. The subroutine takes the name of the last label before the commands with
// _sub after it, or sub without one, and a number after that where another label has the name. Nothing without a
// selected command that the track shows. A selection that the stretch doesn't run in one piece is refused.
std::optional<ListEdit> SubroutineEdit(const ListTrack& track, const std::set<uint32_t>& selected);

// The edits that make the call of row `index` give way to its subroutine's commands, and give it a copy of its
// subroutine. Nothing for a row that isn't a call's.
std::optional<ListEdit> DissolveEdit(const ListTrack& track, std::size_t index);
std::optional<ListEdit> RealCopyEdit(const ListTrack& track, std::size_t index);

// The stretch of the run that the call of row `index` starts, by its index among the track's stretches. Nothing for a
// row that isn't a call's, or a call that the track didn't follow.
std::optional<uint32_t> RunOfCall(const ListTrack& track, std::size_t index);

} // namespace hokuto::app
