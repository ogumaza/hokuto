// SPDX-License-Identifier: MIT

// What the arrangement and the mixer show of a song: its tracks, their parts and notes, and each track's place in the
// mixer. Times count ticks from the song's start.

#pragma once

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace hokuto::app
{

// A note of a part: its start and length in ticks, and its key.
struct NoteView
{
    int32_t start = 0; // from the part's start
    int32_t length = 0;
    int key = 60;
};

// A part of a track. A call's part shows the name of the subroutine that the call ran. It's linked when other calls
// play the same subroutine, in this sound or in others. An edit of a linked part's notes changes what every call plays.
struct PartView
{
    int32_t start = 0;
    int32_t length = 0;
    std::vector<NoteView> notes;
    std::size_t first = 0; // the part's first note among the track's notes (song::Track::notes)
    std::string name;      // the subroutine's name, or empty for a part outside calls
    bool linked = false;
};

// A control of a track's channel that a song sets: its volume, its pan, or its send to FX A, FX B or FX C.
enum class MixControl
{
    kVolume,
    kPan,
    kSendA,
    kSendB,
    kSendC,
};

// The number of controls of a track's channel that a song sets.
inline constexpr std::size_t kMixControlCount = 5;

// A move of a control of a track's channel, in the strip's values: a level in dB for the volume and the sends, and -1
// for left to 1 for right for the pan.
struct MixMove
{
    MixControl control = MixControl::kVolume;
    float from = 0.0f;
    float to = 0.0f;
};

// The number of FX channels that a track sends to.
inline constexpr std::size_t kSendCount = 3;

// A bank slot of a song's sound: the name of the bank in it, and the bank's programs. An empty slot has neither.
struct SlotView
{
    std::string bank;
    std::vector<uint32_t> programs;
};

// An instrument of a song's sound: a bank slot, and a program of the bank in it.
struct InstrumentChoice
{
    int slot = 0;
    int program = 0;
};

// A track of a song as the zones show it: its name, its instrument, its colour, its strip in the mixer and its parts.
struct TrackView
{
    std::string name;
    std::string bank; // the instrument's bank, or empty
    int program = -1; // the instrument's program, or -1
    int slot = -1;    // the slot of the instrument's bank, or -1
    ImU32 colour = 0; // the colour that the zones show

    // The track's colour while the tracks' colours are Rainbow. While they are Custom, a track that the user hasn't
    // given a colour shows it too.
    ImU32 rainbow_colour = 0;
    bool mute = false;
    bool solo = false;
    float volume = 0.0f; // dB
    float pan = 0.0f;    // -1 for left to 1 for right
    std::array<float, kSendCount> sends = {-std::numeric_limits<float>::infinity(),
                                           -std::numeric_limits<float>::infinity(),
                                           -std::numeric_limits<float>::infinity()}; // dB

    // The control that the user holds, or whose move waits for the new version's sequence, from the value that it had
    // when the user took it, and a move that the user has finished. A song sets the other controls as it plays. The
    // program writes a finished move into the song. Mute and solo change what's heard, not the song.
    std::optional<MixMove> held;
    std::optional<MixMove> moved;

    // An instrument that the user chose in the inspector for the notes that play the track's instrument.
    // App::ApplyInstrumentChoices makes the edit.
    std::optional<InstrumentChoice> instrument_choice;

    // What else an edit of each control changes, by MixControl, and what else an edit of the instrument changes. Each
    // is the end of a sentence that names the sound's other tracks and the other sounds that run the same commands, or
    // empty where an edit changes the track alone.
    std::array<std::string, kMixControlCount> mix_reach;
    std::string instrument_reach;

    // In dB: the meter's level, left and right, and the loudest level since the user cleared it. Minus infinity is
    // silence.
    std::array<float, 2> meter = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    float peak = -std::numeric_limits<float>::infinity();

    std::vector<PartView> parts;
};

// A variable of the song that a game can set, by its number in the commands. Its switches offer `choices`, the values
// that take the song down different paths. A variable without choices takes any value. The song starts with the
// variable at `start`, and its commands can change it as it plays. A choice that the user makes waits in `chosen` until
// the program takes it.
struct VariableView
{
    int number = 0;
    std::vector<int> choices;
    bool written = false; // the song's commands set it
    int start = -1;
    int initial = -1;         // the value that the console starts it with when the game sets none
    std::optional<int> value; // the value at the position, while the song plays
    std::optional<int> chosen;
};

// A scene of the song: a name for a set of the values that its variables start with. A scene is lit while the variables
// start with its values.
struct SceneView
{
    std::string name;
    bool lit = false;
};

// What the user does with a scene. Store makes a new scene of the values that the variables start with. Recall starts
// the variables with a scene's values. Update gives a scene the values that the variables start with. Rename gives it
// another name, and Delete removes it.
enum class SceneAction
{
    kStore,
    kRecall,
    kUpdate,
    kRename,
    kDelete,
};

// An action on the song's scenes that waits until the program takes it: the action, the scene's place among the song's
// scenes, and the new name of a scene that the user renames.
struct SceneRequest
{
    SceneAction action = SceneAction::kStore;
    std::size_t scene = 0;
    std::string name;
};

// A change of one of the song's variables while it plays: the variable takes `value` at `time`, in milliseconds from
// the song's start. A game makes such a change through the sound's handle. `tick` is the change's place in the
// arrangement.
struct ChangeView
{
    uint32_t time = 0;
    int variable = 0;
    int value = -1;
    double tick = 0.0;
};

// What the user does with a change. Add makes a new change at the position. Edit gives a change another time, variable
// or value, and Remove takes it out.
enum class ChangeAction
{
    kAdd,
    kEdit,
    kRemove,
};

// An action on the song's changes that waits until the program takes it: the action, the change's place among the
// song's changes, and the change as the user edited it. `refused` holds a message for a typed time that the change
// can't take. The program then shows the message and leaves the change as it is.
struct ChangeRequest
{
    ChangeAction action = ChangeAction::kAdd;
    std::size_t change = 0;
    ChangeView edited;
    std::string refused;
};

// A command at a place where a song's tracks read variables or branch: the number of the track that ran it, its offset
// in the sequence's bytecode, and a line that describes it, such as "Track 0: if Jump Final_Lap".
struct MarkerCommand
{
    friend bool operator==(const MarkerCommand&, const MarkerCommand&) = default;

    int track = 0;
    uint32_t offset = 0;
    std::string line;
};

// The commands that a song's tracks run at places where they read variables or branch, for the marker row. A set of
// commands holds every command that runs at a place, and recurs wherever the tracks run the same commands again, such
// as at each tick of a loop that waits for a variable. `ticks` holds the places where the set runs, in order. `branch`
// says whether a command of the set runs only if the last comparison held. `variables` holds the variables that the
// commands read, in order, and `commands` the commands, in the order of the tracks and of their runs.
struct LogicMarkerView
{
    friend bool operator==(const LogicMarkerView&, const LogicMarkerView&) = default;

    std::vector<int32_t> ticks;
    bool branch = false;
    std::vector<int> variables;
    std::vector<MarkerCommand> commands;
};

// A change of the tempo as ntdlib's performer played it: the tick, and the tempo from there on in beats a minute.
struct TempoChangeView
{
    friend bool operator==(const TempoChangeView&, const TempoChangeView&) = default;

    int32_t tick = 0;
    double bpm = 120.0;
};

// A command that sets the tempo, for the tempo row: the tick at which a track ran it, the tempo that it sets in beats a
// minute, and where it is. A tempo from a random number or a variable has no one value. `source` then names it. The
// List Editor gives it the same name. `track` is the track's number, `command` the command's offset in the sequence's
// bytecode, and `stretch` the index of the stretch of the track's commands that ran it (song::Track::stretches).
// `conditional` says whether the command runs only if the last comparison held. `track_name` is the name that the track
// list shows. `reach` is the end of a sentence that names the other sounds that run the command, or empty where none
// does. `earlier` and `later` give how far the command can move in ticks. A command that can't move has `fixed`: why,
// for the user.
struct TempoPointView
{
    friend bool operator==(const TempoPointView&, const TempoPointView&) = default;

    int32_t tick = 0;
    std::optional<double> bpm;
    int track = 0;
    uint32_t command = 0;
    uint32_t stretch = 0;
    bool conditional = false;
    std::string source;
    std::string track_name;
    std::string reach;
    uint32_t earlier = 0;
    uint32_t later = 0;
    std::string fixed;
};

// A drag of a point of the tempo row: the point, its tempo before the drag and after it in beats a minute, and the
// ticks that it moves by, later for more than 0. A tempo without one value keeps `from` and `to` at 0.
struct TempoMove
{
    TempoPointView point;
    double from = 0.0;
    double to = 0.0;
    int32_t shift = 0;
};

// A drag of an edge of a timeline's loop in the marker row: the loop before the drag, and the loop with the edge at the
// mouse, in ticks.
struct LoopMove
{
    int32_t from_start = 0;
    int32_t from_end = 0;
    int32_t start = 0;
    int32_t end = 0;
    bool end_edge = false; // the drag holds the loop's end, or else its start
};

// A song as the zones show it: its name, its time, its loop, its tracks, and the requests of the user that wait until
// the program takes them.
struct SongView
{
    std::string name;
    int ticks_per_quarter = 48;
    double tempo = 120.0;
    int beats_per_bar = 4;
    int32_t length = 0;      // ticks
    int32_t loop_start = -1; // ticks, or -1 for a song without a loop
    int32_t loop_end = -1;

    // Whether a drag of the loop's edges in the marker row moves the loop: the program edits the sequence as a
    // timeline, and the loop of its commands is the loop that plays.
    bool loop_moves = false;

    // How the program edits the sequence: as a timeline, or as a program (edit/timelines.h). `timeline_refused` says
    // why a program can't be a timeline, and is empty when it can. A click on the inspector's switch sets
    // `mode_request`: true for a timeline. The program then edits the sequence that way, and clears it.
    bool timeline = false;
    std::string timeline_refused;
    std::optional<bool> mode_request;

    // Whether the sound plays in enhanced mixing, or in strict mixing. `enhanced_refused` says why the sound can't take
    // enhanced mixing, and is empty when it can. A click on the mixer's switch sets `mixing_request`: true for enhanced
    // mixing. The program then gives the sound that mode, and clears it.
    bool enhanced = false;
    std::string enhanced_refused;
    std::optional<bool> mixing_request;

    // The engines of the mixer's switch, in the order of format::Engine: the Switch's, the Wii U's, the 3DS's and the
    // Wii's. `engine` is the shown sound's, and `file_engine` the file's. `engine_refused` says why the sound can't
    // play on each engine, and is empty where it can. `engine_notes` lists what changed in the conversion for the
    // sound's engine. `engine_error` says why the last conversion for engine `failed_engine` failed. The sound then
    // plays on the file's engine. A click on the switch sets `engine_request`. The program then gives the sound that
    // engine, and clears it. The made-up song shows no switch.
    bool engines = false;
    int engine = 0;
    int file_engine = 0;
    std::array<std::string, 4> engine_refused;
    std::vector<std::string> engine_notes;
    std::string engine_error;
    int failed_engine = -1;
    std::optional<int> engine_request;

    // The FX channels that the console's tracks send to, from FX A. The 3DS's tracks send to FX A and FX B only. The
    // mixer leaves the other sends' slots empty.
    std::size_t sends = kSendCount;

    std::vector<TrackView> tracks;
    std::vector<SlotView> slots; // the sound's bank slots, empty where they aren't known
    std::vector<VariableView> variables;
    std::vector<SceneView> scenes;
    std::optional<SceneRequest> scene_request;

    // The changes of the variables while the song plays, in the order of their times, and every variable that the
    // song's commands read. A change can set any of those. Only their numbers and choices count.
    std::vector<ChangeView> changes;
    std::vector<VariableView> read_variables;
    std::optional<ChangeRequest> change_request;

    // The sets of commands that the tracks run where they read variables or branch, in the order of their first ticks.
    // The marker row shows them.
    std::vector<LogicMarkerView> logic;

    // The changes of the tempo, in the order of their ticks, and the commands that set it, for the tempo row.
    std::vector<TempoChangeView> tempo_changes;
    std::vector<TempoPointView> tempo_points;
};

// Whether track `index` of `song` is heard: it isn't muted, and either it's soloed or no track is.
inline bool IsHeard(const SongView& song, std::size_t index)
{
    const TrackView& track = song.tracks[index];
    const bool any_solo =
        std::any_of(song.tracks.begin(), song.tracks.end(), [](const TrackView& other) { return other.solo; });
    return !track.mute && (track.solo || !any_solo);
}

} // namespace hokuto::app
