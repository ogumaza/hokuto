// SPDX-License-Identifier: MIT

// The GBA's sound drivers other than MusicPlayer2000 whose songs Hokuto plays, such as Konami's driver. A ROM with such
// a driver is the archive: Hokuto opens the whole ROM, and ntdlib's module of the driver finds the driver and its
// tables in it. Each driver provides the same things through the classes below: the rules of its bytes (DriverRules),
// and a ROM's sounds, samples, model and conversions (DriverRom). A driver's file in this folder contains its classes,
// and one row of kDriverRows in driver.cpp names each driver.
//
// A new driver adds:
// - its engine in format::Engine (format/engine.h) and its row in format::kEngineFacts (format/engines.h), with the
//   family format::Family::kGbaDriver;
// - format/<driver>.cpp, with its format::Driver, and its entry in format::AllDrivers (format/drivers.cpp);
// - song/<driver>.cpp, with its DriverRules and DriverRom, and its row in kDriverRows (song/driver.cpp);
// - edit/<driver>.cpp, with its edit::DriverEdits, and its row in edit::kEditsRows (edit/drivers.cpp).
// The rows of each table follow the order of format::kEngineFacts. A static_assert beside each table fails until the
// row is there. The virtual functions with a default, such as DriverRom::EditsSamples and DriverRules::Velocities,
// give the usual answer, and the new driver overrides the ones where it differs.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sequence_export/agb/settings.h"

#include "format/engine.h"
#include "format/engines.h"

namespace ntdlib::sequence_export
{
struct BankConversion;
struct Conversion;
} // namespace ntdlib::sequence_export

namespace hokuto::song
{

struct Length;
struct Sequence;

// The most tracks that a song of one of these drivers has.
inline constexpr int kMostDriverTracks = 16;

namespace driver_detail
{

// The number of rows of format::kEngineFacts in the family of the GBA's other drivers.
constexpr std::size_t CountDriverEngines()
{
    std::size_t count = 0;
    for (const format::EngineFacts& facts : format::kEngineFacts)
    {
        if (facts.family == format::Family::kGbaDriver)
        {
            ++count;
        }
    }

    return count;
}

} // namespace driver_detail

// The number of the GBA's drivers other than MusicPlayer2000: the engines of format::Family::kGbaDriver.
inline constexpr std::size_t kDriverEngineCount = driver_detail::CountDriverEngines();

// Whether `rows` has one row for each driver engine of format::kEngineFacts, in that order. A row of a table of the
// drivers names its engine in a member `engine`, as in song/driver.cpp and edit/drivers.cpp.
template <typename Row>
constexpr bool RowsFollowDriverEngines(const std::array<Row, kDriverEngineCount>& rows)
{
    std::size_t row = 0;
    for (const format::EngineFacts& facts : format::kEngineFacts)
    {
        if (facts.family != format::Family::kGbaDriver)
        {
            continue;
        }
        if (rows[row].engine != facts.engine)
        {
            return false;
        }

        ++row;
    }

    return true;
}

// The velocities that a note's command of a driver contains, for the List Editor: the highest, or nothing where a note
// has none, and the message where a typed velocity falls outside them.
struct NoteVelocities
{
    std::optional<int64_t> most;
    std::string refusal;
};

// The facts of one revision of a driver that DriverRules returns without a rule of its own.
struct DriverTraits
{
    format::Engine engine = format::Engine::kKonami;
    uint8_t revision = 0;
    int start_volume = 0;
    bool notes_name_samples = false;
    bool mixes_echo = false;
};

// The rules of the bytes of one revision of a driver: what a command contains and how the editors show it. RulesOf
// returns the same object for the same driver and revision every time.
class DriverRules
{
public:
    virtual ~DriverRules() = default;

    // The driver's engine (format::Engine), and its revision. A driver without revisions has revision 0.
    format::Engine DriverEngine() const
    {
        return traits_.engine;
    }

    uint8_t Revision() const
    {
        return traits_.revision;
    }

    // The driver's name in a sentence, such as "Konami's driver" (format::EngineFacts::title).
    std::string_view Title() const
    {
        return format::FactsOf(DriverEngine()).title;
    }

    // The byte offset of each item of `sequence` in the driver's bytes, and the size of the bytes after them. Throws
    // std::invalid_argument with the reason when the bytes can't hold an item.
    virtual std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence) const = 0;

    // A sequence of `item` alone. A check of the values of the item encodes the sequence. Most drivers encode an item
    // by itself. Krawall encodes a pattern's items after the pattern's start (format::KrawallAlone).
    virtual ntdlib::commands::Sequence Alone(const ntdlib::commands::Command& item) const
    {
        return ntdlib::commands::Sequence{{item}, {}};
    }

    // The values that the bytes contain for a command of `kind`.
    virtual ntdlib::commands::Scale ScaleOf(ntdlib::commands::Kind kind) const = 0;

    // The beats a minute that a tempo command of `value` sets, at the 60 frames a second that the driver's tempos count
    // with, and the value that sets `bpm`. Most drivers store the beats a minute in the value.
    virtual double TempoBpm(ntdlib::commands::Fixed value) const
    {
        return static_cast<double>(value) / static_cast<double>(ntdlib::commands::kOne);
    }

    virtual ntdlib::commands::Fixed TempoValue(double bpm) const
    {
        return static_cast<ntdlib::commands::Fixed>(std::llround(bpm * static_cast<double>(ntdlib::commands::kOne)));
    }

    // The beats a minute at which the driver plays a song before a command sets the tempo, counted as TempoBpm counts
    // them. Nothing where the rules don't give it, such as on a driver whose songs have no tempo.
    virtual std::optional<double> StartTempoBpm() const
    {
        return std::nullopt;
    }

    // Whether a tempo command of track `track` sets the song's tempo. Otherwise it sets only the track's tempo.
    virtual bool SetsSongTempo(int /*track*/) const
    {
        return true;
    }

    // The Key Editor shows a velocity from 1 to 127, and a note's command contains the driver's volume. VelocityOf
    // returns the velocity of a volume, and VolumeOf the volume of a velocity: the nearest, and 1 at least for a
    // velocity of 1 or more. `sample_note` is a note that plays a sample.
    virtual int VelocityOf(int volume, bool sample_note) const = 0;
    virtual int VolumeOf(int velocity, bool sample_note) const = 0;

    // The largest volume of track `track`'s commands: what the mixer's fader runs up to.
    virtual int MostVolume(int track) const = 0;

    // The volume at which a track plays until a command sets it.
    int StartVolume() const
    {
        return traits_.start_volume;
    }

    // Whether track `track` plays a channel of the GBA's PSG: a square, the wave or the noise.
    virtual bool PsgTrack(int track) const = 0;

    // Whether each note of a track that plays samples names its sample. Otherwise the track selects one.
    bool NotesNameSamples() const
    {
        return traits_.notes_name_samples;
    }

    // Whether the driver mixes an echo of the tracks into its output. Playback keeps the echo, and the dry render
    // leaves it out (DriverPlayer::SetEcho).
    bool MixesEcho() const
    {
        return traits_.mixes_echo;
    }

    // The velocities that the driver's notes use, or nothing where they use those of the other consoles, 0 to 127.
    virtual std::optional<NoteVelocities> Velocities() const
    {
        return std::nullopt;
    }

protected:
    explicit DriverRules(const DriverTraits& traits) : traits_(traits)
    {
    }

private:
    DriverTraits traits_;
};

// The rules of revision `revision` of the driver of `engine`. Throws std::invalid_argument for an engine without such a
// driver, and for a revision that the driver doesn't have.
const DriverRules& RulesOf(format::Engine engine, uint8_t revision);

// A note that a preview plays: the track that plays it, the commands before the note, such as the track's program, and
// the note's command.
struct DriverAudition
{
    int track = 4;
    std::vector<ntdlib::commands::Command> setup;
    ntdlib::commands::Command note;
};

// A driver's model of the GBA's sound hardware, playing one sound of a ROM (ntdlib's ArchivePlayer of the driver). The
// output runs at Rate in strict mixing, and at ntdlib::agb::kEnhancedRate in enhanced mixing. A frame is a frame of the
// console.
class DriverPlayer
{
public:
    // A snapshot of the player, for a seek.
    struct Snapshot
    {
        virtual ~Snapshot() = default;
    };

    virtual ~DriverPlayer() = default;

    // Starts sound `sound` of the ROM. Throws for a sound whose song can't start.
    virtual void Start(uint32_t sound, bool enhanced) = 0;

    // From the next start on, mixes the driver's echo when `on`. The console mixes the echo the same way. Off, each
    // track plays dry, and the rest plays as before. A driver without an echo (DriverRules::MixesEcho) ignores it. On
    // at first.
    virtual void SetEcho(bool /*on*/)
    {
    }

    virtual int Rate() const = 0;
    virtual bool Enhanced() const = 0;

    // The tracks that the player mutes and meters: from 0 up to this.
    virtual int Tracks() const = 0;

    // Whether the sound still plays or rings.
    virtual bool Busy() const = 0;

    // The console's frames that ran, and the driver's clock in ticks.
    virtual uint64_t Frames() const = 0;
    virtual uint32_t Tick() const = 0;

    // From the next frame on, Tick() counts the ticks of `sequence`, the arrangement of the sound that started. Most
    // drivers' clocks count the arrangement's ticks, and they ignore it. On Krawall, a tick of the player can take
    // several of the arrangement's ticks (KrawallPlayer).
    virtual void SetArrangement(const Sequence&)
    {
    }

    // Whether the driver's song has ended: it stopped, or none of its tracks plays.
    virtual bool Finished() const = 0;

    // Runs a frame of the console, and adds its stereo samples to `out`.
    virtual void RunFrame(std::vector<int16_t>& out) = 0;
    virtual void RunFrame(std::vector<float>& out) = 0;

    virtual void MuteTrack(int track, bool mute) = 0;
    virtual void MeterTracks(bool meter) = 0;
    virtual std::array<int32_t, 2> TrackPeaks(int track) const = 0;

    // A snapshot that can share memory with `previous`, an earlier snapshot of this player.
    virtual std::shared_ptr<const Snapshot> Save(const Snapshot* previous) const = 0;
    virtual void Restore(const Snapshot& snapshot) = 0;
};

// The places in a ROM's bytes of the fields of its tables that name a sound's song: the field that names the song's
// bytes, and the field of each track's start. A driver whose song contains the starts of its tracks has no track
// fields. Its sequence starts with a table of the starts of its tracks instead, and `entries` lists the entries of that
// table that start the sound's tracks: each entry of a song's table, or the one entry of a sound effect that plays one
// track of its sequence. Where several sounds share a sequence, the field that names the sequence can name the item
// where the sound starts instead: a list of the sound's tracks (`starts`).
struct SoundFields
{
    std::optional<uint64_t> sequence;
    std::vector<uint64_t> tracks;
    std::vector<int> entries;
    bool starts = false;
};

// The length of a sample and the start of its loop, in samples. A sample without a loop has no start.
struct SampleShape
{
    uint32_t length = 0;
    std::optional<uint32_t> loop_start;
};

// A bank that a song's tracks can select, in a slot of the song, and the programs that it contains.
struct DriverBankSlot
{
    std::string bank;
    std::vector<uint32_t> programs;
};

// A GBA ROM, and what ntdlib found of a driver in it. Its sounds have numbers from 0 to SoundCount, with gaps where a
// number names no song.
class DriverRom
{
public:
    virtual ~DriverRom() = default;

    virtual const DriverRules& Rules() const = 0;

    // The ROM's bytes.
    virtual std::span<const uint8_t> Bytes() const = 0;

    // The sounds that play, in order, and the end of their numbers.
    virtual std::vector<uint32_t> Sounds() const = 0;
    virtual uint32_t SoundCount() const = 0;

    // The name that Hokuto gives sound `sound`, such as "Song 03".
    virtual std::string SoundName(uint32_t sound) const = 0;

    // The places of the fields of the tables in the ROM that name the song of sound `sound`.
    virtual SoundFields FieldsOf(uint32_t sound) const = 0;

    // The rate of the console's output in strict mixing.
    virtual int Rate() const = 0;

    // The samples of the driver's table that the songs play, in order, with their names, their rates at semitone 0 and
    // their WAV files. SampleRate and SampleWav throw std::invalid_argument for a number that names no sample.
    virtual std::vector<int> Samples() const = 0;
    virtual std::string SampleName(int sample) const = 0;
    virtual double SampleRate(int sample) const = 0;
    virtual std::vector<uint8_t> SampleWav(int sample) const = 0;
    virtual SampleShape ShapeOf(int sample) const = 0;

    // Whether Hokuto replaces the samples of the driver's table and edits their values (edit/konami_samples.h). Only
    // Konami's driver supports it.
    virtual bool EditsSamples() const
    {
        return false;
    }

    // The samples that sound `sound`'s commands play, in order. Nothing for a sound whose commands Hokuto doesn't read.
    virtual std::vector<int> SamplesOfSound(uint32_t sound) const = 0;

    // The programs of the GBA's PSG that track `track`'s commands select, in order: duties on the square channels, and
    // waves on the wave channel. Nothing for the noise channel. Nothing by default: the driver's commands select none.
    virtual std::vector<uint32_t> TrackPrograms([[maybe_unused]] int track) const
    {
        return {};
    }

    // The banks of the slots that sound `sound`'s commands select, for the inspector's Bank and Program menus. A note's
    // bank slot and program then name a slot and a program of one of them. Nothing where a note's program is one of the
    // driver's samples (Samples). The default returns nothing.
    virtual std::vector<DriverBankSlot> BankSlots([[maybe_unused]] uint32_t sound) const
    {
        return {};
    }

    // Sound `sound` as the performer plays it, for the arrangement (song/sequence.h). Throws std::runtime_error with a
    // message for the user when the song can't be read.
    virtual Sequence ReadSequence(uint32_t sound) const = 0;

    // Sound `sound` as MIDI with SoundFont 2, with the default settings of
    // ntdlib::sequence_export::agb::ConvertSettings, and the samples `samples` in a SoundFont of their own
    // (song/midi_export.h). When WritesSfz returns true, both conversions contain SFZ files too.
    virtual ntdlib::sequence_export::Conversion ConvertToMidi(uint32_t sound, const std::string& program) const = 0;
    virtual ntdlib::sequence_export::BankConversion ConvertSamples(std::span<const int> samples,
                                                                   const std::string& program) const = 0;

    // Whether the driver's converter writes SFZ files.
    virtual bool WritesSfz() const
    {
        return false;
    }

    // A player of the ROM's sounds.
    virtual std::unique_ptr<DriverPlayer> MakePlayer() const = 0;

    // A note of sample `sample` on the first track that plays samples. Key 60 plays the sample at its rate.
    virtual DriverAudition SampleAudition(int sample, int key, int velocity) const = 0;

    // A note at key `key` of the Key Editor, with velocity `velocity`, on the instrument `program` of track `track` of
    // a song whose commands are `commands`, read as `sequence`. Nothing when the track plays no such note.
    virtual std::optional<DriverAudition> NoteAudition(const ntdlib::commands::Sequence& commands,
                                                       const Sequence& sequence, int track, int program, int key,
                                                       int velocity) const = 0;

    // A copy of the ROM whose sound 0 plays `audition`, then waits. Throws std::invalid_argument when the driver's
    // bytes can't hold the note.
    virtual std::shared_ptr<const DriverRom> AuditionRom(const DriverAudition& audition) const = 0;
};

// The ROM in `bytes` with the driver of `engine`. Throws xsflib::FormatError if the bytes show no sign of the driver,
// or if its tables contain no sound, and std::invalid_argument for an engine without such a driver.
std::shared_ptr<const DriverRom> LoadDriverRom(format::Engine engine, std::vector<uint8_t> bytes);

// The name of sample `sample` of a driver's table: Sample and the number in four digits (DriverRom::SampleName).
std::string DriverSampleName(int sample);

// A command of `kind` whose argument is the value `value`, on index `index`. The setups of auditions and the edits of
// src/edit that write a command of a value use it.
ntdlib::commands::Command ValueCommand(ntdlib::commands::Kind kind, int64_t value, uint8_t index = 0);

// The loop of a sample's WAV file: from the point `start` up to the point `end`.
struct WavLoop
{
    uint32_t start = 0;
    uint32_t end = 0;
};

// A WAV file of `length` 8-bit points of `rom` from `address`, with `loop` where it's given, at `rate` Hz. The points
// are signed, or with `is_unsigned` unsigned with 0x80 as silence.
std::vector<uint8_t> Pcm8Wav(const ntdlib::agb::Rom& rom, uint32_t address, uint32_t length, bool is_unsigned,
                             std::optional<WavLoop> loop, uint32_t rate);

// The rate of a WAV file of a sample at `rate` Hz: the nearest whole number, and 1 at least.
uint32_t WavRate(double rate);

// The item at each offset of `offsets`, the offsets of a sequence's items followed by the offset of their end: item i
// at offsets[i]. The end names no item.
std::map<uint32_t, uint32_t> ItemsByOffset(std::span<const uint32_t> offsets);

// The sounds of a driver whose songs come in versions, by the sound's number: each song's first version, then the other
// versions of each song, as (song, version) pairs. `version_count(song)` returns the number of versions of `song`.
template <typename VersionCount>
std::vector<std::pair<int, int>> VersionedSounds(int songs, VersionCount version_count)
{
    std::vector<std::pair<int, int>> sounds;
    for (int song = 0; song < songs; ++song)
    {
        sounds.emplace_back(song, 0);
    }

    for (int song = 0; song < songs; ++song)
    {
        const auto versions = static_cast<int>(version_count(song));
        for (int version = 1; version < versions; ++version)
        {
            sounds.emplace_back(song, version);
        }
    }

    return sounds;
}

// The name of version `version` of song `song` of `songs`, whose versions are `versions`: Song and the song's number,
// and the version's name in brackets where it has one.
std::string VersionedSoundName(int song, int songs, std::span<const ntdlib::sequence_export::agb::SongVersion> versions,
                               std::size_t version);

} // namespace hokuto::song
