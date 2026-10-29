// SPDX-License-Identifier: MIT

// A sequence sound of a sound archive as the arrangement and the mixer show it: its tempo, its loop, and each track's
// notes, instrument, volume, pan and sends. ntdlib's performer runs the sequence's commands to find them, the way
// hacsf2 converts a sequence to MIDI. A sequence with a loop shows its intro and one pass of the loop. Each note leads
// to the command that started it, for an edit. The sequence file's labels name the subroutines that calls play. A count
// of the file's calls shows which subroutines play from more than one place.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sound_build.h"

namespace hokuto::song
{

// A stretch of a track's commands that ran one after another: from the track's start, a jump, a call, a return or a
// loop's end to the next.
using Stretch = ntdlib::sequence_export::Stretch;

// Times count ticks from the sequence's start. `command` is the offset of the note command that started the note, in
// the bytecode of the sound's sequence file. `stretch` is the stretch of the track's commands that ran it, in
// Track::stretches. `bank_slot` and `program` give the instrument that the note plays: a slot of the sound's banks, and
// a program of the bank in that slot.
struct Note
{
    friend bool operator==(const Note&, const Note&) = default;

    uint32_t start = 0;
    uint32_t length = 0;
    uint8_t key = 60;
    uint8_t velocity = 127;
    uint32_t command = 0;
    uint32_t stretch = 0;
    uint8_t bank_slot = 0;
    uint32_t program = 0;
};

// A setting of a track's mix: its volume, its pan, or its send to one of the aux buses A to C.
using Mix = ntdlib::sequence_export::Mix;

// The number of aux buses that a track sends to.
inline constexpr int kSendCount = 3;

// The value that a track's commands give a setting from `tick` on.
struct Setting
{
    friend bool operator==(const Setting&, const Setting&) = default;

    uint32_t tick = 0;
    int value = 0;
};

struct Track
{
    friend bool operator==(const Track&, const Track&) = default;

    int number = 0;   // the sequence's track, from 0 to 15
    std::string name; // "Track" and the number, as in the converters' MIDI files
    std::string bank; // the bank of the track's first note, or empty
    int program = -1; // the program of the track's first note, or -1
    std::vector<Note> notes;
    std::vector<Setting> volume;                        // the track's volume, a level from 0 to 255
    std::vector<Setting> pan;                           // the track's pan, from -64 (left) to 63 (right)
    std::array<std::vector<Setting>, kSendCount> sends; // the track's sends to the aux buses, from 0 to 255
    std::vector<Stretch> stretches;                     // in the order that they ran
};

struct Tempo
{
    friend bool operator==(const Tempo&, const Tempo&) = default;

    uint32_t tick = 0;
    double bpm = 120.0; // quarter notes a minute
};

// The range that the sequence plays over and over once it reaches `end`.
struct Loop
{
    friend bool operator==(const Loop&, const Loop&) = default;

    uint32_t start = 0;
    uint32_t end = 0;
};

// The variables that a game can set through a sound's handle, by their numbers in the commands: 0 to 15 are the
// player's variables, and 16 to 31 the global ones. Each starts at -1 until the game or a command sets it. On the Wii,
// a global variable starts at 0 (SeqPlayer::SetGlobalVariable).
inline constexpr int kGameVariables = 32;
inline constexpr int kFirstGlobal = 16;

// The values that a sound's variables take before it starts, by their numbers. A game sets them through the sound's
// handle. The variables that aren't given start at -1.
using StartValues = std::map<int, int16_t>;

// A game's change of a variable while the sound plays, through the sound's handle: variable `variable` takes `value` at
// `time`, in milliseconds from the sound's start.
struct VariableChange
{
    friend bool operator==(const VariableChange&, const VariableChange&) = default;

    uint32_t time = 0;
    int variable = 0;
    int16_t value = -1;
};

// A sound's changes, in the order of their times. Changes at the same time come in their order here.
using Changes = std::vector<VariableChange>;

// The length of a console's sound frames in milliseconds: the fraction `numerator` / `denominator`.
struct FrameMs
{
    uint64_t numerator = 5;
    uint64_t denominator = 1;
};

// The length of the sound frames of the console of `flavor`: 5 ms on the Switch, 240 samples at 48 kHz, and 3 ms on the
// Wii U, 144 samples at 48 kHz. On the 3DS, a frame lasts 160 samples at the DSP's rate of 268111856 / 8192 Hz:
// 81920000 / 16756991 ms, about 4.889 ms. ntdlib's converter gives the 3DS's frames the same length.
constexpr FrameMs FrameMsOf(ntdlib::sar::Flavor flavor)
{
    switch (flavor)
    {
    case ntdlib::sar::Flavor::kCafe:
        return {3, 1};
    case ntdlib::sar::Flavor::kCtr:
        return {81920000, 16756991};
    default:
        return {5, 1};
    }
}

// The length of the Wii's sound frames: 3 ms, 96 samples at 32 kHz. NW4R SND runs a sound frame after each of AX's
// frames.
inline constexpr FrameMs kWiiFrameMs{3, 1};

// The frame that a change at `time` comes in, on a console whose frames last `frame_ms`, counted from 0 at the sound's
// start: the first frame that starts at that time or later. The change comes before the frame's ticks.
uint64_t ChangeFrame(uint32_t time, FrameMs frame_ms);

// The whole frames in `ms` milliseconds, on a console whose frames last `frame_ms`.
uint64_t FramesIn(uint64_t ms, FrameMs frame_ms);

// The options of ntdlib's performer for a game that starts the variables with `values` and makes `changes` while the
// sound plays, on the console of `flavor`. Values and changes for variables past the game's are left out. The changes
// come in the order of their times, each in its frame.
ntdlib::sequence_export::PerformOptions PerformOptionsOf(const StartValues& values, const Changes& changes,
                                                         ntdlib::sar::Flavor flavor);

// The same options on a console whose frames last `frame_ms`. The sound's first tick plays in frame `first_tick` there.
ntdlib::sequence_export::PerformOptions PerformOptionsOf(const StartValues& values, const Changes& changes,
                                                         FrameMs frame_ms, uint64_t first_tick);

// A variable whose starting value can change what a sound plays. The sound's commands read it, in a comparison or in
// another command's argument, before they set it. `written` says whether they change it at all.
//
// `choices` holds a value for each set of outcomes that the comparisons of the variable with fixed values can have, in
// order. Two values with the same outcomes take the same branches. Of those, the choice is the starting value, or else
// the value nearest 0, positive before negative. A variable whose exact value counts has no choices: a command takes
// its starting value as an argument, compares it with another variable or a random value, or works out a new value from
// it. Neither does a variable that no comparison tests against a fixed value.
struct Variable
{
    friend bool operator==(const Variable&, const Variable&) = default;

    int number = 0;
    std::vector<int> choices;
    bool written = false;
};

struct Sequence
{
    friend bool operator==(const Sequence&, const Sequence&) = default;

    std::string name;
    uint32_t sound = 0; // the sound's index in the archive's sound table
    int timebase = 48;  // ticks a quarter note
    std::vector<Tempo> tempo;
    uint32_t length = 0;    // the ticks to the end of the sequence or of its loop, and of its last note
    uint32_t notes_end = 0; // the tick at which the last note ends, before its release
    std::optional<Loop> loop;
    bool holds = false;        // the sound holds until the game stops it: the performer ended it 10 s into the hold
    std::vector<Track> tracks; // the tracks that ran commands, with notes or without, by their numbers
    std::map<uint32_t, std::string> labels; // the sequence file's labels, by their offsets in its bytecode

    // The number of call commands in the sequence file that go to each offset. Calls from the file's other sounds count
    // too. Calls that no sound can reach don't.
    std::map<uint32_t, int> calls;

    // The variables whose starting values can change what the sound plays, in the order of their numbers. The sequence
    // plays with `start_values`, and with `changes` while it plays. The tracks' notes and settings follow them, and so
    // does a model of the sequence. The arrangement's loop is then a pass that starts after the last change.
    std::vector<Variable> variables;
    StartValues start_values;
    Changes changes;

    // Every variable that the sound's commands read, in the order of their numbers. A change while the sound plays can
    // change what it plays through any of them. The choices take every comparison into account.
    std::vector<Variable> read_variables;

    // On the Wii, the build of NW4R SND whose rules the performer read the sequence by. A model of the sequence plays
    // by the same build.
    std::optional<ntdlib::SndBuild> wii_build;
};

// A run of a track's notes that the arrangement shows as one part: notes `first` to `first + count - 1`, from tick
// `start` to tick `end`. A call's part holds the notes of one call of a subroutine. `call` is then the subroutine's
// offset in the bytecode.
struct Part
{
    uint32_t start = 0;
    uint32_t end = 0;
    std::size_t first = 0;
    std::size_t count = 0;
    std::optional<uint32_t> call;
};

// The parts of a track, on a grid of lines `grid` quarter notes apart. A part starts at the line before its first note
// and ends at the line after its last note. A gap of `gap` quarter notes or more between notes starts a new part. So do
// the loop's start and its end. A part never crosses either one. A call of a subroutine from outside any call makes a
// separate part, from the call to the return, with the notes of the calls inside it. A call that never returns ends at
// the line after its last note. Parts never overlap. The part before a call's part ends where the call starts.
std::vector<Part> SplitParts(const Sequence& sequence, const Track& track, int grid, int gap);

// The stretch of `track` that entered the call from outside any call that stretch `stretch` ran in, or nothing outside
// calls. A call's part (SplitParts) starts at that stretch.
std::optional<std::size_t> OutermostCall(const Track& track, std::size_t stretch);

// The value that a track's settings have at `tick`, or `fallback` before the first one.
int SettingAt(const std::vector<Setting>& settings, uint32_t tick, int fallback);

// The settings of `track` for `setting`, in the order of their ticks.
const std::vector<Setting>& MixSettings(const Track& track, Mix setting);

// The value of `setting` before a command sets it: a volume of 127, a pan of 0, and sends of 0.
int MixDefault(Mix setting);

// The value that `setting` of `track` has at `tick`.
int MixAt(const Track& track, Mix setting, uint32_t tick);

// The tempo at `tick`.
double TempoAt(const Sequence& sequence, uint32_t tick);

// The time of tick `tick` in milliseconds from the sequence's start, from the tempo of each stretch of ticks. Past the
// last change of the tempo, the ticks go on at the last tempo. TickOfMs is the inverse.
double MsOfTick(const Sequence& sequence, double tick);
double TickOfMs(const Sequence& sequence, double ms);

// The position of the sequence after the player has counted `ticks` ticks: `ticks` up to the loop's end, and then round
// the loop. The player counts every tick, and doesn't go back when the sequence loops.
uint32_t PositionOfTicks(const Sequence& sequence, uint64_t ticks);

// `sequence` up to the end of the first pass of `loop`, with `loop` as its loop. The notes and the settings from the
// loop's end on go, and a note that still sounds there lasts to the loop's end. For a loop that the performer found a
// whole number of passes after `loop`. Each pass of such a loop runs the same commands.
Sequence FirstPass(Sequence sequence, Loop loop);

// Whether `a` and `b` play each tick at the same time: the same timebase, the same changes of the tempo and the same
// loop. A model of one then plays the same ticks in each frame as a model of the other. An edit of a tempo, or of a
// wait before a change of the tempo or the loop's end, moves the time of the ticks after it.
bool SameTiming(const Sequence& a, const Sequence& b);

// Runs sequence sound `sound` of `archive` through ntdlib's performer, with the settings of the archive's console
// (SettingsOf), the variables' starting values `values`, and the game's `changes`. Values and changes for variables
// past the game's are left out. Throws std::runtime_error with a message for the user if it isn't a sequence sound or
// its data is damaged.
Sequence ReadSequence(const ntdlib::sar::SoundArchive& archive, uint32_t sound, const StartValues& values = {},
                      const Changes& changes = {});

// The same for sequence sound `sound` of the Wii's `archive`, with the settings of `build` of NW4R SND, the game's
// build, or else of the build that the archive's version gives (SettingsOf). The labels, the counts of calls and the
// list of variables come from the sequence file, decoded by the rules of that build.
Sequence ReadSequence(const ntdlib::rsar::SoundArchive& archive, uint32_t sound, const StartValues& values = {},
                      const Changes& changes = {}, const std::optional<ntdlib::SndBuild>& build = {});

// The variables that a game can set and that the commands that the player reaches from item `start` of `sequence` read.
// `starting` holds those whose starting values can change what the sound plays (Sequence::variables), and `all` every
// variable that the commands read (Sequence::read_variables). The paths follow the tracks that the commands open. The
// global variables start at `global_start`: 0 on the Wii.
struct SoundVariables
{
    std::vector<Variable> starting;
    std::vector<Variable> all;
};
SoundVariables VariablesOf(const ntdlib::commands::Sequence& sequence, uint32_t start, int global_start = -1);

// The value that variable `number` of `sequence` starts with when the game sets none: -1, or 0 for a global variable on
// the Wii.
int StartingValue(const Sequence& sequence, int number);

} // namespace hokuto::song
