// SPDX-License-Identifier: MIT

// The parts of hokuto_check_commands that its files share: the totals, the notes as the rounds compare them, the
// helpers and the rounds. check_commands.cpp describes the rounds.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/konami/commands.h"
#include "ntdlib/krawall/commands.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/sequencer.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/nwsnd/units.h"
#include "ntdlib/quintet/song.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/rare/song.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "app/song_view.h"
#include "app/tempo_row.h"
#include "check_support.h"
#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/brownie.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/drivers.h"
#include "edit/konami.h"
#include "edit/krawall.h"
#include "edit/notes.h"
#include "edit/rd2.h"
#include "edit/reach.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/brownie_items.h"
#include "format/engine.h"
#include "format/file.h"
#include "format/quintet.h"
#include "notes_apart.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/rare.h"
#include "song/sequence.h"

namespace hokuto::tools::commands
{

using hokuto::app::TempoPointView;
using hokuto::edit::CommandPlace;
using hokuto::edit::Edited;
using hokuto::format::File;
using hokuto::tools::CheckedArchive;
using hokuto::tools::CheckedSound;
using hokuto::tools::RareFrameTicks;
using hokuto::tools::SpreadIndices;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

// The commands of a sequence that take part in the rounds of single edits, the waits of the third round, the commands
// that take a new tempo in the sixth, and the commands of each track that move in the seventh.
constexpr std::size_t kMostCommands = 256;
constexpr std::size_t kMostWaits = 64;
constexpr std::size_t kMostTempos = 8;
constexpr std::size_t kMostMoves = 4;

// The fewest ticks that a note lasts in the sixth round's comparisons that a sound gains or loses only notes of a tick.
// A new tempo changes how many ticks a frame runs. The models of the consoles can then end a note of a tick in the
// frame where it starts, and the arrangement leaves such a note out.
constexpr uint32_t kTickNotes = 2;

// What QuintetFramesDecide counts apart.
struct QuintetApart
{
    std::size_t moved = 0;       // notes that start elsewhere
    std::size_t split = 0;       // notes that the conversion split off a command's note in one read only
    std::size_t drums = 0;       // notes of the noise channel whose drum the conversion numbers otherwise
    std::size_t resets = 0;      // notes that start more than a frame away, after resets of their counts
    std::size_t late = 0;        // notes that start up to kQuintetLateFrames past what the resets allow
    std::size_t short_notes = 0; // notes shorter than kQuintetShortFrames that sound in one read only
};

// How many frames past the resets' frames a note of Quintet's driver can start after a new tempo, and how many frames
// a note can last at most and still sound in one read only (QuintetFramesDecide).
constexpr uint32_t kQuintetLateFrames = 2;
constexpr uint32_t kQuintetShortFrames = 2;

// What the check counted, for the summary.
struct Totals
{
    std::size_t sequences = 0;
    std::size_t fixed = 0;
    std::size_t kept = 0;
    std::size_t round_trips = 0;
    std::size_t waits = 0;
    std::size_t sounds = 0;
    std::size_t notes = 0;
    std::size_t values = 0;
    std::size_t times = 0;
    std::size_t refused = 0;
    std::size_t settings = 0;
    std::size_t setting_sequences = 0;
    std::size_t settings_refused = 0;
    std::size_t setting_sounds = 0;
    std::size_t setting_notes = 0;
    std::size_t moved_loops = 0;
    std::size_t settings_past = 0;
    std::size_t psg_sounds = 0;
    std::size_t after_data = 0;        // waits that no code would reach after data, on the GBA
    std::size_t after_table = 0;       // waits after a table of tracks, where no code would reach them
    std::size_t dropped_lengths = 0;   // waits whose removal also took out a length byte that no channel read
    std::size_t respelled = 0;         // waits after whose removal notes and waits of R&D2's driver give their values
    std::size_t part_passes = 0;       // sounds whose loop starts part of a pass away (PartPass)
    std::size_t move_part_passes = 0;  // the same after the moves
    std::size_t before_allocation = 0; // waits refused before the allocation of tracks, on the DS
    std::size_t without_waits = 0;     // waits refused on a driver without a wait command
    std::size_t tempo_effects = 0;     // changes of the tempo that Krawall's effects set, without a point
    std::size_t setting_channels = 0;  // sounds whose notes find other channels after the new settings, on the DS
    std::size_t move_channels = 0;     // the same after the moves
    std::size_t move_past_end = 0;     // DSE's sounds with a track that runs past its chunk, after the moves
    std::size_t setting_cuts = 0;      // Konami's sounds that lose notes to a new volume after a note in its tick
    std::size_t move_cuts = 0;         // the same after the moves
    std::size_t outgrown = 0;          // a round's edits of a song that a GBA ROM without room can't hold
    std::size_t tempo_sequences = 0;
    std::size_t tempo_points = 0;
    std::size_t tempo_sources = 0;
    std::size_t tempo_overridden = 0;
    std::size_t tempo_conditional = 0;
    std::size_t tempo_unpointed = 0;
    std::size_t tempo_edits = 0;
    std::size_t tempo_sounds = 0;
    std::size_t tempo_random = 0;
    std::size_t tempo_waits = 0;
    std::size_t tempo_others = 0;
    std::size_t tempo_listed = 0;
    std::size_t tempo_reached = 0;
    std::size_t tempo_moves = 0;
    std::size_t tempo_farthest = 0;
    std::size_t tempo_move_sounds = 0;
    std::size_t tempo_move_random = 0;
    std::size_t tempo_move_waits = 0;
    std::size_t tempo_move_short = 0;
    std::size_t tempo_short = 0;
    std::size_t tempo_repeats = 0;      // sounds whose loop starts elsewhere after the new tempos (SameOverLoops)
    std::size_t tempo_move_repeats = 0; // the same after the moved tempos
    std::size_t tempo_channels = 0;
    std::size_t tempo_move_channels = 0;
    std::size_t tempo_psg = 0;
    std::size_t tempo_overtaken = 0;
    // The points past the end of a sound without a loop (PastEnd), before the drags and after them.
    std::size_t tempo_past_end = 0;
    std::size_t tempo_drag_past_end = 0;
    std::size_t tempo_move_psg = 0;
    std::size_t tempo_voices = 0;        // sounds of R&D2's driver whose voices take other notes after the new tempos
    std::size_t tempo_move_voices = 0;   // the same after the moved tempos
    std::size_t tempo_alone = 0;         // the notes of those sounds that only one read contains
    std::size_t tempo_frames = 0;        // tunes of Rare's driver whose frames take other notes after the new tempos
    std::size_t tempo_move_frames = 0;   // the same after the moved tempos
    std::size_t move_frames = 0;         // the same after the moves of settings
    std::size_t frames_alone = 0;        // the notes of those tunes that only one read contains
    std::size_t quintet_frames = 0;      // songs of Quintet's driver whose notes start in other frames after new tempos
    std::size_t quintet_move_frames = 0; // the same after the moved tempos
    QuintetApart quintet_apart;          // what those songs' notes do otherwise
    std::size_t tempo_partners = 0;      // the other tracks' tempo commands that took a point's new tempo
    std::size_t moves = 0;
    std::size_t move_sequences = 0;
    std::size_t move_sounds = 0;
    std::size_t move_notes = 0;
    std::size_t move_waits = 0;
    std::size_t move_loops = 0;
    std::size_t move_repeats = 0;
    std::size_t move_psg = 0;
    std::size_t move_calls = 0;
    std::size_t move_starts = 0;    // moves of a volume that the check of Konami's volume starts refused
    std::size_t move_cut_songs = 0; // moves that the check refused in a song whose loop cuts a track short
    std::size_t problems = 0;
};

// The moves of the seventh round in one sequence: the sound whose tracks they took, the shift of each moved command by
// its offset, and the new item of each item of the sequence. The sixth round moves points of the tempo row the same
// way.
struct Moved
{
    uint32_t sound = 0;
    std::map<uint32_t, int32_t> shifts;
    std::vector<std::optional<uint32_t>> items;
    std::set<int> levels;               // the tracks whose volume or pan moved
    std::map<uint32_t, int64_t> rows{}; // on Krawall, the rows that each moved command moved by
};

// The rows of a sound of Krawall: the stretches of a track. Each track contains the rows of the module. For a sound
// without them, the function returns none.
std::span<const hokuto::song::Stretch> RowsOfSound(const hokuto::song::Sequence& song);

// Whether `item`, an item of a sequence of `file`, is an entry of a table of tracks (edit::DriverEdits::IsTableEntry).
// No track runs such an entry.
bool TableEntry(const File& file, const Command& item);

// A wait of no ticks.
Command NoWait();

// How a console converts the beats a minute of a tempo command. The performer uses `ratio` of the value of a tempo.
// MusicPlayer2000 counts its tempos in frames of 60 Hz, and the GBA runs its frames at about 59.73 Hz: kFrameRate / 60.
// NITRO-Composer adds the tempo to a counter once a frame, and runs a tick for each 240 in it. A beat takes 48 ticks,
// and a frame kFrameClocks of the DS's clock: about 0.99977. The other consoles play the value. MusicPlayer2000 adds
// the tempo to a counter once a frame too, and runs a tick for each kTickCounter in it. A new tempo counts from the
// next frame. The ticks left in the frame run at the old tempo: up to `lag`, 3 at MusicPlayer2000's fastest tempo, 510
// beats a minute. On the DS, the performer applies a new tempo from the tick of the command. On Quintet's driver,
// ntdlib's performer measures a quarter note of `quarter_ticks` of its ticks in whole microseconds
// (ntdlib::agb::QuarterMicros).
struct TempoRules
{
    double ratio = 1.0;
    uint32_t lag = 0;
    bool whole_microseconds = false; // DSE's driver keeps a beat's length in whole microseconds
    uint64_t quarter_ticks = 0;      // the performer's ticks a quarter note on Quintet's driver, where not 0
};

TempoRules TempoRulesOf(const CheckedArchive& archive);

// Whether tick `tick` is past the end of `song`, a sound without a loop. A sound whose last change comes more than 10 s
// after its last sound has died away ends where that sound dies away (ntdlib's EndsAtLastSound). The performer records
// no tempo after that end.
bool PastEnd(const hokuto::song::Sequence& song, uint32_t tick);

// The place of item `item` of sequence `sequence` of `file`.
CommandPlace PlaceOf(const File& file, uint32_t sequence, uint32_t item);

// Whether `console` runs Ubisoft Milan's driver.
bool UbiMilanConsole(const hokuto::song::Console& console);

// `command` with its value a step from the value that it has, or with its time a step from its time when its value
// can't move. A step is the console's smallest change. Nothing for a command with neither. On Ubisoft Milan's driver,
// no countdown stores a wait of one tick after a command (format/ubimilan.h), and a wait of no ticks steps to two.
std::optional<Command> Stepped(const hokuto::song::Console& console, const Command& command);

// `command` without what its bytes held beyond its meaning.
Command WithoutEncoding(Command command);

// The notes that a sound plays: each one's track, start, key, length and velocity.
using Played = std::tuple<int, uint32_t, uint8_t, uint32_t, uint8_t>;

// Whether a track of `sound` of DSE runs past its chunk: a note's command is in the bytes that `sequence` of `file`
// stores as data. A wait that moves changes the state in which the track reads those bytes.
bool ReadsData(const File& file, uint32_t sequence, const hokuto::song::Sequence& sound);

// The notes of `sequence`, in order, without those that `apart` leaves out.
std::vector<Played> NotesOf(const hokuto::song::Sequence& sequence, const hokuto::tools::NotesApart& apart = {});

// Whether the archive's sounds are songs of Konami's driver.
inline bool konami_song = false;

// Whether the archive's sounds are songs of Nintendo R&D2's driver.
inline bool rd2_song = false;

// Whether the archive's sounds are sounds of Brownie Brown's driver.
inline bool brownie_song = false;

// Whether `after` loops over a loop as long as that of `before`, from a start that is part of a pass away. After an
// edit of a setting, ntdlib's performer can find a song of Nintendo R&D2's driver to repeat from a later or an earlier
// tick: the edit changes the state of a track that it compares. On Brownie Brown's driver, the performer starts a
// channel's loop with its second pass when its first pass plays with other settings. A new setting or a moved one
// before the place that the channel jumps back to applies in the first pass alone. That channel's loop then starts a
// pass later, and the sound's loop part of a pass later.
bool PartPass(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after);

// Whether `after` plays the notes of `before`: each note at its start, with its key and velocity. A new setting can
// change how long a note sounds: the arrangement ends a note once it fades below hearing. ntdlib's performer finds a
// loop where a track's state repeats, and a new setting or a wait of its own can change the state that it compares. The
// loop can then start a whole number of passes earlier or later, and the arrangement shows the passes before it. Up to
// the earlier loop's end, the notes then have to start as before. On the drivers of Nintendo R&D2 and Brownie Brown,
// the loop can also start part of a pass away (PartPass), and the notes have to start as before up to the earlier
// loop's end. The performer plays 15 minutes at most. A sound whose loop takes longer has no loop, and a new tempo can
// bring the loop inside the limit or take it out. Up to the end of the read that stopped at the limit, the notes then
// have to start as before. `moved` becomes true when the loop moved. Notes shorter than `shortest` ticks don't count,
// and neither do those that `apart` leaves out.
bool SamePlaying(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after, bool& moved,
                 uint32_t shortest = 0, const hokuto::tools::NotesApart& apart = {});

// Whether `before` and `after`, reads of sequence `sequence` of `file` on Nintendo R&D2's driver, play the same notes
// as SamePlaying compares them, but for notes that the driver's voices decide. A new tempo moves the frames where notes
// end and release. The driver assigns a note no voice while each voice plays a note of higher priority, and a PSG note
// ends once its envelope falls to 0. A note can then start in one read and not in the other: each note that only one
// read contains has to start at a tick where its track runs a note command. `alone` counts those notes.
bool VoicesDecide(const File& file, uint32_t sequence, const hokuto::song::Sequence& before,
                  const hokuto::song::Sequence& after, bool& moved, std::size_t& alone);

// The loop of a track of a tune: the tick where it starts and its length, and the keys of the notes at that tick
// that come before the loop's start. Those notes play only on the first pass.
struct TrackLoop
{
    uint32_t start = 0;
    uint32_t length = 0;
    std::multiset<uint8_t> before;
};

// The loop of each looping track of a tune, by the track's number.
using TrackLoops = std::map<int, TrackLoop>;

// How SameOverLoops repeats the loops of a tune of Rare's driver: each track's loop in each read (RareTrackLoops), and
// the most ticks of a frame (RareFrameTicks).
struct RareRepeats
{
    TrackLoops before;
    TrackLoops after;
    uint32_t frame = 0;
};

// Whether `after` plays the notes of `before` when both loop for the same number of ticks from other starts. ntdlib's
// performer finds the loop where the state of every track starts to repeat, and a setting that moves on a track that
// ends can move that tick. The song whose loop ends sooner repeats its loop up to the later loop's end. Up to there,
// each note has to start as before, with its key and velocity. `apart` leaves notes out of both.
//
// On Rare's driver (`rare`), each track repeats the loop of its commands. Where the least common multiple of the
// tracks' loops is more than 8 times the longest, ntdlib's plan uses the longest as the loop of the song
// (agb::LoopLength), and the other tracks don't repeat with it. A note at the tick of a track's loop start that comes
// before the loop's start plays only once. As in FramesDecide, a note that only one read contains can also stand alone:
// a moved tempo puts the later ticks in other frames. `alone` counts those notes.
bool SameOverLoops(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after,
                   const hokuto::tools::NotesApart& apart, const RareRepeats* rare = nullptr,
                   std::size_t* alone = nullptr);

// How SameOverLoops repeats `before`, sound `sound` of `archive`, and `after`, the same sound of `edited`, on Rare's
// driver. Nothing for another driver.
std::optional<RareRepeats> RareRepeatsOf(const CheckedArchive& archive, const CheckedArchive& edited, uint32_t sound,
                                         const hokuto::song::Sequence& before, const hokuto::song::Sequence& after);

// Whether `before` and `after`, reads of a tune of Rare's driver, play the same notes as SamePlaying compares them, but
// for notes that the driver's frames decide. The time of each delay is computed at the tempo in force when its track
// reads it, with 8 bits of fraction of a microsecond. A new or moved tempo, and a delay that an edit splits, can move a
// track's later ticks to other frames. A key that two voices of a MIDI channel start in one frame is one note as loud
// as both. The note is on the track whose voice started first. A note starts a tick late where another note of its key
// still sounds (ntdlib's conversion of the driver). A note that only one read contains can therefore stand alone where
// either read contains a note of its track and key at its tick with another velocity, or a tick away. It can also stand
// alone where either read contains a note of its key that starts a frame before it or less, on its track or another: a
// new tempo can put two voices of the key in one frame, or in two. `alone` counts those notes.
bool FramesDecide(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after, bool& moved,
                  std::size_t& alone);

// The ticks where each track of `song`, sequence `sequence` of `file`, a song of Quintet's driver, starts its count of
// ticks again: where the track runs a tempo (7F), and where the first channel runs one. A 7F sets its channel's count
// of ticks and frames to 0, and one of the first channel sets the counts of every channel (ntdlib's Sequencer, 0x7F). A
// stretch's commands run at its tick. Each track's ticks come in order.
std::map<int, std::vector<uint32_t>> QuintetResets(const File& file, uint32_t sequence,
                                                   const hokuto::song::Sequence& song);

// Whether `before` and `after`, reads of a song of Quintet's driver, play the same notes, but for the frames that they
// start in. Each note and rest of the driver lasts whole frames, and a new or moved tempo puts a later note in a frame
// that starts up to a frame from its tick. Three more effects of the frames follow from it:
// - Each reset of a channel's count of ticks (QuintetResets) drops the part of a frame that the channel had counted.
//   A note can then start up to a frame away for each reset before it, and one more.
// - The conversion can give a note's command more than one note, where the channel's sound changes within the note's
//   frames. In one stretch, only the first note of each command counts.
// - The conversion numbers the noise channel's drums in the order that their sounds first appear (H-375), and a new
//   tempo changes the frames of a drum's sound. A noise note's key then doesn't count.
// Two effects aren't modelled, and count apart up to a limit:
// - A channel can sound frames after its command: the conversion then starts the note later within its wait. Those
//   frames last as many ticks as the tempo, and a new tempo moves such a note by ticks for each frame. A note can start
//   up to kQuintetLateFrames past what the resets allow.
// - A note shorter than kQuintetShortFrames, such as a grace note, can sound in one read only. Its sound can then fall
//   within frames that the start of the next note or a later sound uses.
// Up to the frames that the resets allow before the earlier read's end, each track's first notes of its commands have
// to come in the same order in both reads, with the same commands and, but on the noise channel, the same keys. The
// loop's start and end move with the notes, and a song without notes can end a frame elsewhere. `resets` contains the
// resets of `before`, and `apart` counts what differs.
bool QuintetFramesDecide(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after,
                         const std::map<int, std::vector<uint32_t>>& resets, QuintetApart& apart);

// Whether a sound of Konami's driver can play otherwise because an edit put a volume after a note in the note's tick:
// the edited sequence contains such a volume (KonamiVolumesAfterNotes), and the sound loses notes and gains none.
bool CutByVolume(const File& edited, uint32_t sequence, const hokuto::song::Sequence& before,
                 const hokuto::song::Sequence& after, const hokuto::tools::NotesApart& apart);

// Whether `before` and `after`, songs of the GBA or sounds of the DS, run the same note commands. Their notes compare
// as in SamePlaying. The notes that the driver never started count too (song::Track::unstarted). A new tempo moves the
// frames where earlier notes end and release, and a note can then find a channel that it didn't find before, or lose
// one. With `psg_apart`, the notes of the PSG channels stay out. A new tempo changes how many frames a note lasts. The
// envelope of a PSG channel moves once a frame, and a note whose envelope rises from silence can then end before it
// sounds.
bool SameNoteCommands(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after, bool& moved,
                      bool psg_apart = false);

// The rows of the track numbered `number` of `song` up to the end of the loop's first pass. Sequence `sequence` of
// `file` played `song`. None for a track that ran no commands.
std::vector<hokuto::edit::CommandRow> RowsOf(const File& file, uint32_t sequence, const hokuto::song::Sequence& song,
                                             int number);

// Whether a track of `song` ran a note that waits until the track's notes have ended: a note of length 0, or of a
// length that the sequence decides as it plays, with the note wait on. The wait lasts a time rather than a number of
// ticks, and a new tempo changes the ticks that it takes. Sequence `sequence` of `file` played `song`.
bool WaitsForNotes(const File& file, uint32_t sequence, const hokuto::song::Sequence& song);

// The first round: each chosen command of sequence `sequence` takes its values again.
void KeepValues(const std::string& name, const File& file, uint32_t sequence, const std::vector<uint32_t>& chosen,
                Totals& totals);

// Whether item `item` of `commands`, a sequence of `file`, is an allocation of tracks that NITRO-Composer reads only as
// the sequence's first command. A command can't go before it (edit::InsertCommand).
bool AllocationFirst(const File& file, const ntdlib::commands::Sequence& commands, uint32_t item);

// The second round: a wait goes in before each chosen command of sequence `sequence`, and goes again. On the GBA, a
// wait after data that code doesn't enter stays out: no code would reach it, and MP2K's decoder keeps such bytes as
// data. The List Editor enters at a command after data (CommandRow::entered). On Nintendo R&D2's driver, a wait right
// after a table of tracks that code doesn't enter stays out the same way. On the DS, a wait before the allocation of
// tracks at the sequence's first command has to be refused (AllocationFirst). On Konami's driver, a command has one
// delay before it. A wait of no ticks joins the delay beside it, and the file stays as it was. On Ubisoft Milan's
// driver, a wait is inserted before each command, and a wait of no ticks joins it the same way. A driver without a wait
// command has to refuse each wait. Krawall is such a driver: a wait ends a row and counts the rows, and the driver's
// bytes hold no wait alone.
void InsertAndRemove(const std::string& name, const File& file, uint32_t sequence, const std::vector<uint32_t>& chosen,
                     Totals& totals);

// The fifth round: new settings on the tracks of the first sound of each sequence in `songs`, in one version of the
// file. `songs` holds the sounds as they played before, and `sequences` the sequence of each file of `archive`.
void NewSettings(const std::string& name, const File& file, const CheckedArchive& archive,
                 const std::vector<std::optional<uint32_t>>& sequences,
                 const std::map<uint32_t, hokuto::song::Sequence>& songs, Totals& totals);

// The seventh round's check of the commands that `before` and `after` ran, the sound of `moved` before and after the
// moves in sequence `sequence`. `file` holds the sequence before, and `edited` after. Each moved command has to run at
// its tick and the move's, before the notes that its stretch starts there. On Krawall, it has to join the entry of its
// channel in its new row instead (OneEntry). Every other command has to run at its tick.
// A wait that a move splits or joins can run at other ticks. When the loop ends earlier, the rows after the edit stop
// at its end. They also stop where the performer stops reading the sound, and a move past that tick stays out. Gives
// the number of problems.
std::size_t CheckMovedRows(const std::string& name, const File& file, const File& edited, uint32_t sequence,
                           const hokuto::song::Sequence& before, const hokuto::song::Sequence& after,
                           const Moved& moved);

// The seventh round: settings move in time on the tracks of the first sound of each sequence in `songs`, in one version
// of the file (MovesOf). `songs` holds the sounds as they played before, and `sequences` the sequence of each file of
// `archive`. Each move goes by itself first, and then the moves of every sequence go together.
void SettingMoves(const std::string& name, const File& file, const CheckedArchive& archive,
                  const std::vector<std::optional<uint32_t>>& sequences,
                  const std::map<uint32_t, hokuto::song::Sequence>& songs, Totals& totals);

// Prints the seventh round's figures. With `gba`, also the sounds whose notes of a PSG channel differ.
void PrintMoveTotals(const std::string& name, const Totals& totals, bool gba);

// Prints the sixth round's figures. With `gba`, also the sounds whose notes find other channels, and those whose notes
// of a PSG channel differ. With `rd2`, also the sounds whose voices take other notes (VoicesDecide). With `rare`, also
// the tunes whose frames take other notes (FramesDecide), after the moves of settings too.
void PrintTempoTotals(const std::string& name, const Totals& totals, bool gba, bool rd2, bool rare, bool quintet);

// The sixth round: the tempo row (src/app/tempo_row.h). The first sound that ntdlib's performer reads of each checked
// sequence gives the points. Each point with a value that no later command overrides in its tick has to set the tempo
// that the performer plays from that tick. Each change of the tempo before the end of the loop's first pass has a point
// at its tick, or the tool counts it. Up to kMostTempos commands of each sequence then take a tempo 7 BPM away through
// the tempo row's drag, in one version of the file. The drag also changes the commands of the same tempo on the other
// tracks at the tick of the point (app::TempoChangeOf). The first sound of each edited sequence has to play its notes
// as before, at their ticks, and each point's tempo, the new one where its command changed. A sound that draws random
// numbers and plays other notes is counted apart: the draws follow the frames, and a new tempo moves the frames. So is
// a sound of the GBA or the DS whose notes find other channels, a sound of the GBA whose notes differ only on the PSG
// channels (SameNoteCommands), and a sound of Nintendo R&D2's driver whose voices take other notes (VoicesDecide). Each
// other sound of an edited sequence that plays otherwise has to be among the sounds that the tempo row lists for an
// edited command. `places` gives where each sound plays (edit::SoundPlaces). The points then move in time (TempoMoves).
// A song of Konami's driver has no tempo, and the round checks nothing there. A point past the end of a sound without a
// loop counts apart (PastEnd), before the drags and after them.
void TempoRow(const std::string& name, const File& file, const CheckedArchive& archive,
              const std::vector<std::optional<uint32_t>>& sequences,
              std::span<const std::optional<hokuto::edit::SoundPlace>> places, const std::set<uint32_t>& checked,
              Totals& totals);

} // namespace hokuto::tools::commands
