// SPDX-License-Identifier: MIT

// A GBA ROM whose songs Konami's driver plays. ntdlib finds the driver in the ROM, with its revision, its song table
// and its other tables (ntdlib::konami::DetectDriver). A song has no name in the ROM: Hokuto names it by its number in
// the song table. ntdlib's performer plays a song on its model of the driver for the arrangement
// (ntdlib::sequence_export::konami::PerformSong), and ntdlib's ArchivePlayer plays it on its models of the driver and
// the GBA's sound hardware (song/sequence_model.h).
//
// A game can play a song in two ways: as a request leaves it, and as its live stage plays it (DriverInfo::live_songs).
// Each way is a sound of its own. Sound `song` is the song as a request leaves it. Sound `song_count + song` is the
// song of a live stage. Both play the same commands.

#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/konami/sequencer.h"

#include "song/driver.h"

namespace hokuto::song
{

// A GBA ROM, and what ntdlib found of Konami's driver in it. The functions below give what the overrides give.
// A note of a sample names its sample, and the driver has no banks.
struct KonamiRom final : DriverRom
{
    const DriverRules& Rules() const override;
    std::span<const uint8_t> Bytes() const override;
    std::vector<uint32_t> Sounds() const override;
    uint32_t SoundCount() const override;
    std::string SoundName(uint32_t sound) const override;
    SoundFields FieldsOf(uint32_t sound) const override;
    int Rate() const override;
    std::vector<int> Samples() const override;
    std::string SampleName(int sample) const override;
    double SampleRate(int sample) const override;
    std::vector<uint8_t> SampleWav(int sample) const override;
    SampleShape ShapeOf(int sample) const override;
    bool EditsSamples() const override;
    std::vector<int> SamplesOfSound(uint32_t sound) const override;
    std::vector<uint32_t> TrackPrograms(int track) const override;
    Sequence ReadSequence(uint32_t sound) const override;
    ntdlib::sequence_export::Conversion ConvertToMidi(uint32_t sound, const std::string& program) const override;
    ntdlib::sequence_export::BankConversion ConvertSamples(std::span<const int> samples,
                                                           const std::string& program) const override;
    bool WritesSfz() const override;
    std::unique_ptr<DriverPlayer> MakePlayer() const override;
    DriverAudition SampleAudition(int sample, int key, int velocity) const override;
    std::optional<DriverAudition> NoteAudition(const ntdlib::commands::Sequence& commands, const Sequence& sequence,
                                               int track, int program, int key, int velocity) const override;
    std::shared_ptr<const DriverRom> AuditionRom(const DriverAudition& audition) const override;

    ntdlib::agb::Rom rom;
    ntdlib::konami::DriverInfo info;
};

// The rules of revision `revision` of Konami's driver (song/driver.h).
const DriverRules& KonamiRules(ntdlib::konami::Revision revision);

// The revision of Konami's driver whose rules `rules` are, or nothing for the rules of another driver, and for none.
std::optional<ntdlib::konami::Revision> KonamiRevisionOf(const DriverRules* rules);

// `rom` as a ROM with Konami's driver, or nothing for another driver's ROM.
const KonamiRom* KonamiOf(const DriverRom* rom);

// The GBA ROM in `bytes`. Throws xsflib::FormatError if the bytes show no sign of Konami's driver, or if the song table
// contains no song (KonamiSounds).
std::shared_ptr<const KonamiRom> LoadKonamiRom(std::vector<uint8_t> bytes);

// The sounds of the song table: each song whose entry names bytes of the ROM, with the version of its live stage after
// it where the game has one. Two numbers can name the same song. Each one counts.
std::vector<uint32_t> KonamiSounds(const KonamiRom& rom);

// The song of sound `sound`.
int KonamiSongOf(const KonamiRom& rom, uint32_t sound);

// Whether sound `sound` plays its song as the game's live stage does.
bool KonamiSoundIsLive(const KonamiRom& rom, uint32_t sound);

// The state in which ntdlib's player starts the song of sound `sound`: what the game's request leaves, or for a sound
// of a live stage, what the live stage leaves.
ntdlib::konami::Request KonamiRequestOf(const KonamiRom& rom, uint32_t sound);

// The name that Hokuto gives sound `sound` of `rom`: "Song" and its number, with as many digits as the largest number
// of the table takes, and two at least. A song of a live stage says so.
std::string KonamiSoundName(const KonamiRom& rom, uint32_t sound);

// The rate of the console's output for the songs of `rom`: 65,536 Hz for the revisions with a mixer, and 32,768 Hz for
// the revision without one.
int KonamiRate(const KonamiRom& rom);

// The Key Editor shows a velocity from 1 to 127, and the command of a note contains the driver volume. Where a note's
// volumes run from 0 to 15, they scale to 127 for the view. The notes of a sample (`sample_note`) in the revisions that
// hold sample volumes up to 127 keep their volumes as they are. KonamiVolumeOf returns the volume of a velocity: the
// nearest, and 1 at least for a velocity of 1 or more.
int KonamiVelocityOf(int volume, ntdlib::konami::Revision revision, bool sample_note);
int KonamiVolumeOf(int velocity, ntdlib::konami::Revision revision, bool sample_note);

// Whether `command`, a note command of track `holder` of Konami's driver, starts its note on track `number`. A note
// command of the Dungeon Dice Monsters revision with index 1 plays on the track after its track, and only there. In the
// other revisions, a track's notes can play on the next track too.
bool KonamiNoteStartsOn(const ntdlib::commands::Command& command, int holder, int number,
                        ntdlib::konami::Revision revision);

// The note command at whose key a volume command (C0-CF) of Konami's driver starts a note again: the last note command
// of track `holder` among the items `first` up to `end` of `items` that starts its note on track `number`
// (KonamiNoteStartsOn). The track ran those items in their order before the volume command (ntdlib
// src/konami/sequencer.cpp and sequencer_wct2004.cpp, Op::kVolume). Nothing where the items contain no such command.
// This is the one copy of the rule. ReadSequence names each note's command by it, and edit::EditKonamiNotes checks
// with it that an edit leaves each such note at its key.
std::optional<uint32_t> KonamiNoteCommandIn(std::span<const ntdlib::commands::Command> items, uint32_t first,
                                            uint32_t end, int holder, int number, ntdlib::konami::Revision revision);

// A run of a track's commands: its first item and the item past its last.
using KonamiRun = std::pair<uint32_t, uint32_t>;

// The note command at whose key the command at item `item` of run `run` starts a note again: its run and its item
// (KonamiNoteCommandIn). `runs` lists the items that track `holder` ran, in the order in which it ran them. Nothing
// where the track ran no such note command before the item. ReadSequence passes the stretches that the driver's model
// ran, and edit::CheckKonamiVolumeStarts a walk of the commands.
std::optional<std::pair<std::size_t, uint32_t>> KonamiNoteCommandBefore(
    std::span<const ntdlib::commands::Command> items, std::span<const KonamiRun> runs, std::size_t run, uint32_t item,
    int holder, int number, ntdlib::konami::Revision revision);

// A time that a track runs a volume command (C0-CF): the item of the volume command, and the items of the note commands
// at whose keys it starts a note again there. `notes[0]` is the one for the track of the commands, and `notes[1]` the
// one for the track after it (KonamiNoteStartsOn). Nothing where the track ran no such note command before.
struct KonamiRunStart
{
    uint32_t volume = 0;
    std::array<std::optional<uint32_t>, 2> notes;
};

// Each time that a track runs a volume command of `items` along `runs`, in the order of the runs. Each note command is
// the one that KonamiNoteCommandBefore returns for that time. This function finds them all in one pass over the runs.
std::vector<KonamiRunStart> KonamiVolumeStartsAlong(std::span<const ntdlib::commands::Command> items,
                                                    std::span<const KonamiRun> runs, ntdlib::konami::Revision revision);

// A note that a volume command (C0-CF) starts, as ReadSequence names its command: the offset of the volume command, the
// offset of the note command at whose key it plays, or nothing where the track ran none, and whether the note plays on
// the track after the track of the commands.
struct KonamiVolumeStart
{
    friend auto operator<=>(const KonamiVolumeStart&, const KonamiVolumeStart&) = default;

    uint32_t volume = 0;
    std::optional<uint32_t> note;
    bool on_next = false;
};

// The notes of sound `sound` of `rom` that a volume command starts, in the order of ReadSequence's tracks and notes.
// A check compares them with the walk of the commands that the edits use (edit::KonamiWalkOf).
std::vector<KonamiVolumeStart> KonamiVolumeStarts(const KonamiRom& rom, uint32_t sound);

// The largest volume of a track's commands: what the mixer's fader runs up to.
int KonamiMostVolume(ntdlib::konami::Revision revision, int track);

// The samples of the driver's table that the songs of `rom` play, in order. A note names its sample, and a track of the
// revision without a mixer can select one. A number that names no sample of the ROM stays out. So does one that a
// song's commands can't be read for.
std::vector<int> KonamiSamples(const KonamiRom& rom);

// The samples whose rates sound `sound` takes for the notes of other samples, in order. At a pitch change of a sample
// track, the driver's output stage sets the voice's step from the header of the sample that the track's record names.
// That sample can differ from the one that the voice plays. Outside the Ultimate Masters and Live Start revisions, a
// bend names the sample by the track's duty byte. A duty or a wave command can change that byte. In the Eternal Duelist
// revision, every change but a note's start and stop counts as a pitch change, and a volume names sample 0. The rule
// follows the output stages of ntdlib::konami::Output, over two passes of the song's loop on ntdlib's sequencer. Only
// the step of such a header is read: its length and loop play no part. Nothing for the revision without a mixer.
std::vector<int> KonamiBorrowedRates(const KonamiRom& rom, uint32_t sound);

// The duties or the waves that track `track` of the songs of `rom` selects, in order. Tracks 0 and 1 select duties, and
// track 2 waves.
std::vector<uint32_t> KonamiTrackPrograms(const KonamiRom& rom, int track);

// The name that Hokuto gives sample `sample`: "Sample" and its number, with four digits. The samples of a live stage's
// table have numbers from 0x8000 on.
std::string KonamiSampleName(int sample);

// Sample `sample` of `rom` as a WAV file: its samples at its rate at semitone 0, with its loop. Throws
// std::invalid_argument for a number that names no sample, and for a sample without samples.
std::vector<uint8_t> KonamiSampleWav(const KonamiRom& rom, int sample);

// The rate of sample `sample` at semitone 0 in Hz, for the view. The mixer's rate gives it. Throws
// std::invalid_argument for a number that names no sample.
double KonamiSampleRate(const KonamiRom& rom, int sample);

// A note of sample `sample` on the first track that plays samples. Key `key` of 60 plays the sample at its rate, and
// each key moves it a semitone where the revision's notes can. `velocity` of the view's 1 to 127 gives its volume.
DriverAudition KonamiSampleAudition(const KonamiRom& rom, int sample, int key = 60, int velocity = 127);

// A note at key `key` of the Key Editor, with velocity `velocity`, on the instrument `program` of track `track` of a
// song. The commands come from the note of `sequence` on that track with that program whose key is nearest. The
// distance of `key` from the key of that note moves the semitone or the driver's note in the commands. `commands` is
// the song's sequence of commands. Nothing when the track plays no note of that program whose command the arrangement
// records.
std::optional<DriverAudition> KonamiNoteAudition(const KonamiRom& rom, const ntdlib::commands::Sequence& commands,
                                                 const Sequence& sequence, int track, int program, int key,
                                                 int velocity);

// A copy of `rom` whose only song plays `audition`, then waits. The copy's bytes grow past the ROM's: the model takes
// them. Throws std::invalid_argument when the driver's bytes can't hold the note.
std::shared_ptr<const KonamiRom> KonamiAuditionRom(const KonamiRom& rom, const DriverAudition& audition);

} // namespace hokuto::song
