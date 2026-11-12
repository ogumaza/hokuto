// SPDX-License-Identifier: MIT

#include "song/quintet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/apu.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/quintet/archive_player.h"
#include "ntdlib/quintet/commands.h"
#include "ntdlib/quintet/instruments.h"
#include "ntdlib/quintet/song.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/quintet/convert.h"
#include "ntdlib/sequence_export/quintet/soundfont.h"
#include "xsflib/binary.h"

#include "format/quintet.h"
#include "song/driver_player.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace hokuto::song
{

namespace
{

namespace quintet = ntdlib::quintet;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// The key of index 0 of the pitch table: the conversion's key of a note on a square or a FIFO.
constexpr int kIndexKey = 36;

// The ticks of the performer to a quarter note: 96 ticks of the driver.
constexpr int64_t kQuarterTicks = 96 * format::kQuintetSongTicks;

// The beats a minute of a tempo of 1 at 60 frames a second. A tempo T plays T of a tick's 37 parts a frame, and a
// quarter note lasts kQuarterTicks of the parts.
constexpr double kBpmPerTempo = 60.0 * 60.0 / static_cast<double>(kQuarterTicks);

// The FIFOs' tracks, and the command that selects a square's duty.
constexpr int kFirstFifo = quintet::kPcmA;
constexpr uint16_t kDutyCommand = 0x2F;

// The rules of a revision of the driver in a Hokuto file. The A and J revisions of the driver share revision 0, and
// they start a song at other tempos. The rules of a ROM give the start tempo of its revision of the driver (`start`).
class QuintetDriverRules final : public DriverRules
{
public:
    explicit QuintetDriverRules(uint8_t revision, std::optional<quintet::Revision> start = std::nullopt)
        : DriverRules({.engine = format::Engine::kQuintet,
                       .revision = revision,
                       .start_volume = 0,
                       // A channel's commands select its instrument, and a note sets only a pitch.
                       .notes_name_samples = false,
                       .mixes_echo = false}),
          revision_(revision),
          start_(start)
    {
    }

    // The ROM's revision of the driver, where the rules give its start tempo.
    std::optional<quintet::Revision> Start() const
    {
        return start_;
    }

    // song::Runs checks whether the driver has a kind of command with a single command of value 0. A note, a rest or a
    // wait of no length has no bytes, but the driver has the kind: the command counts the bytes of a quarter note.
    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence) const override
    {
        if (sequence.items.size() == 1 && format::QuintetTimed(sequence.items[0].kind) &&
            sequence.items[0].value.value == 0)
        {
            ntdlib::commands::Sequence quarter = sequence;
            quarter.items[0].value.value = kQuarterTicks * kOne;
            return ItemOffsets(quarter);
        }

        return format::QuintetFoldedOffsets(
            sequence, quintet::ItemOffsets(format::QuintetDriverTicks(sequence), format::QuintetTraits(revision_)));
    }

    ntdlib::commands::Scale ScaleOf(Kind kind) const override
    {
        return format::QuintetScale(kind);
    }

    double TempoBpm(ntdlib::commands::Fixed value) const override
    {
        return static_cast<double>(value) / static_cast<double>(ntdlib::commands::kOne) * kBpmPerTempo;
    }

    ntdlib::commands::Fixed TempoValue(double bpm) const override
    {
        return static_cast<ntdlib::commands::Fixed>(std::llround(bpm / kBpmPerTempo)) * ntdlib::commands::kOne;
    }

    // Each channel starts at the tempo of the driver's revision.
    std::optional<double> StartTempoBpm() const override
    {
        if (!start_)
        {
            return std::nullopt;
        }

        return TempoBpm(static_cast<ntdlib::commands::Fixed>(quintet::TraitsOf(*start_).start_tempo) * kOne);
    }

    // A tempo on the first channel sets every channel's tempo, and the performer's tempo follows the first channel. A
    // tempo on another channel sets only the tempo of that channel.
    bool SetsSongTempo(int track) const override
    {
        return track == quintet::kSquare1;
    }

    // A note has no velocity: the Key Editor shows each note at 127.
    int VelocityOf(int, bool) const override
    {
        return 127;
    }

    int VolumeOf(int, bool) const override
    {
        return 127;
    }

    // A channel's volume (5E) starts at 0.
    int MostVolume(int track) const override
    {
        return QuintetMostVolume(track, start_);
    }

    bool PsgTrack(int track) const override
    {
        return track >= 0 && track < kFirstFifo;
    }

private:
    uint8_t revision_;
    std::optional<quintet::Revision> start_;
};

// The model of the driver and the GBA's sound hardware, for one ROM (ntdlib::quintet::ArchivePlayer).
//
// The driver's clock counts its ticks, 96 to a quarter note, and a note adds its whole length when it starts. The
// arrangement counts the performer's ticks, 37 to each of the driver's. ntdlib's converter sets the tempo of each frame
// to the tempo T of the first channel, in ticks of the performer, 4 at least: a frame plays T of a tick's 37 parts.
// Tick() therefore counts the tempos of the frames the same way as the converter.
class QuintetPlayer final : public ArchiveDriverPlayer<quintet::ArchivePlayer>
{
public:
    explicit QuintetPlayer(const QuintetRom& rom) : ArchiveDriverPlayer(rom.rom, rom.files), rom_(rom)
    {
    }

    void Start(uint32_t sound, bool enhanced) override
    {
        ticks_ = 0;
        quintet::ArchivePlayerOptions options;
        options.enhanced = enhanced;
        if (rom_.audition)
        {
            const QuintetRom::AuditionNote& note = *rom_.audition;
            player_.StartNote(note.song, note.channel, note.instrument, note.index, note.volume, options);
            return;
        }

        player_.Start(static_cast<int>(sound), options);
    }

    int Tracks() const override
    {
        return quintet::kPlayerTracks;
    }

    uint32_t Tick() const override
    {
        return static_cast<uint32_t>(std::min<uint64_t>(ticks_, UINT32_MAX));
    }

    bool Finished() const override
    {
        return !player_.Busy() || player_.Driver().Ended();
    }

    void RunFrame(std::vector<int16_t>& out) override
    {
        const bool ended = player_.Driver().Ended();
        player_.RunFrame(out);
        CountFrame(ended);
    }

    void RunFrame(std::vector<float>& out) override
    {
        const bool ended = player_.Driver().Ended();
        player_.RunFrame(out);
        CountFrame(ended);
    }

    std::shared_ptr<const Snapshot> Save(const Snapshot* previous) const override
    {
        const auto* earlier = dynamic_cast<const Saved*>(previous);
        return std::make_shared<const Saved>(player_.Save(earlier != nullptr ? &earlier->snapshot : nullptr), ticks_);
    }

    void Restore(const Snapshot& snapshot) override
    {
        const Saved& saved = dynamic_cast<const Saved&>(snapshot);
        player_.Restore(saved.snapshot);
        ticks_ = saved.ticks;
    }

private:
    // The slowest tempo that the converter sets for a frame (ntdlib's kMinMidiTempo).
    static constexpr uint64_t kLeastFrameTicks = 4;

    struct Saved final : Snapshot
    {
        Saved(quintet::ArchivePlayer::Snapshot saved, uint64_t at) : snapshot(std::move(saved)), ticks(at)
        {
        }

        quintet::ArchivePlayer::Snapshot snapshot;
        uint64_t ticks = 0;
    };

    // Adds the frame's ticks: the first channel's tempo after the frame's commands. The frame in which the song ends
    // counts, as in the converter. Once the song had ended before the frame (`ended`), the clock stops.
    void CountFrame(bool ended)
    {
        if (!ended)
        {
            ticks_ += std::max<uint64_t>(player_.Driver().GetChannel(quintet::kSquare1).tempo, kLeastFrameTicks);
        }
    }

    const QuintetRom& rom_;
    uint64_t ticks_ = 0;
};

// The header of song `song`, numbered across the whole game, or nothing where it can't be read.
std::optional<quintet::SongHeader> HeaderOf(const QuintetRom& rom, int song)
{
    const auto file = rom.FileOf(song);
    quintet::SongHeader header;
    if (!file || !quintet::ReadSongHeader(rom.rom, quintet::SongAddress(*file->first, file->second), header))
    {
        return std::nullopt;
    }

    return header;
}

// The values that the commands of channel `channel` of song `song` select with the command `kind` and `index`, in
// order. Nothing where the song's commands can't be read.
std::vector<uint32_t> SelectedBy(const QuintetRom& rom, int song, int channel, Kind kind, uint8_t index)
{
    const auto file = rom.FileOf(song);
    if (!file)
    {
        return {};
    }

    std::vector<uint32_t> found;
    try
    {
        const quintet::Decoded decoded =
            quintet::DecodeRomSequence(rom.rom, *file->first, quintet::SequenceOfSong(*file->first, file->second));
        if (decoded.opaque || static_cast<std::size_t>(channel) >= decoded.entries.size())
        {
            return {};
        }

        const std::vector<Command>& items = decoded.sequence.items;
        for (std::size_t item = decoded.entries[static_cast<std::size_t>(channel)]; item < items.size(); ++item)
        {
            const Command& command = items[item];
            if (command.kind == Kind::kEnd || command.kind == Kind::kData)
            {
                break;
            }

            if (command.kind == kind && command.index == index)
            {
                const auto value = static_cast<uint32_t>(command.value.value / kOne);
                if (std::ranges::find(found, value) == found.end())
                {
                    found.push_back(value);
                }
            }
        }
    }
    catch (const std::exception&)
    {
        return {};
    }

    return found;
}

// The instrument that `command` selects on channel `channel`: a square channel's duty, or the program of another
// channel. Nothing for a command that selects none.
std::optional<uint8_t> InstrumentOf(const Command& command, int channel)
{
    if (channel < quintet::kWave && command.kind == Kind::kOther && command.encoding.opcode == kDutyCommand &&
        !command.bytes.empty())
    {
        return command.bytes[0];
    }

    if (channel >= quintet::kWave && command.kind == Kind::kProgram &&
        command.index == (channel == quintet::kWave    ? 2
                          : channel == quintet::kNoise ? 1
                                                       : 0))
    {
        return static_cast<uint8_t>(command.value.value / kOne);
    }

    return std::nullopt;
}

// The instrument that channel `channel` selects before item `item` of `commands`: the last command that selects one
// in the stretches that ran up to the item. `stretches` contains the stretches of the channel in the order that they
// ran, and the item runs in stretch `stretch`. A stretch of the driver ends at each note, rest and wait. The command is
// therefore often in an earlier stretch. `offsets` contains the offset of each item. 0 where no command selects one.
uint8_t InstrumentBefore(const ntdlib::commands::Sequence& commands, std::span<const uint32_t> offsets,
                         std::span<const Stretch> stretches, std::size_t stretch, std::size_t item, int channel)
{
    const auto item_at = [offsets](uint32_t offset) -> std::optional<std::size_t>
    {
        const auto found = std::ranges::lower_bound(offsets, offset);
        if (found == offsets.end() || *found != offset)
        {
            return std::nullopt;
        }

        return static_cast<std::size_t>(found - offsets.begin());
    };
    for (std::size_t at = std::min(stretch + 1, stretches.size()); at-- > 0;)
    {
        const std::optional<std::size_t> first = item_at(stretches[at].first);
        const std::optional<std::size_t> last = item_at(stretches[at].last);
        if (!first || (at != stretch && !last))
        {
            continue;
        }

        const std::size_t end = std::min(at == stretch ? item : *last + 1, commands.items.size());
        for (std::size_t i = end; i-- > *first;)
        {
            if (const std::optional<uint8_t> instrument = InstrumentOf(commands.items[i], channel))
            {
                return *instrument;
            }
        }
    }

    return 0;
}

// An audition of a note at key `key` on channel `channel` of song `song`, with `instrument`.
DriverAudition Audition(int song, int channel, uint8_t instrument, int key)
{
    DriverAudition audition;
    audition.track = channel;
    audition.setup = {ValueCommand(Kind::kProgram, song), ValueCommand(Kind::kProgram, instrument)};
    audition.note.kind = Kind::kNote;
    audition.note.key = static_cast<uint8_t>(std::clamp(key, 0, 127));
    audition.note.velocity = 127 * kOne;
    return audition;
}

} // namespace

const DriverRules& QuintetRules(uint8_t revision)
{
    static const std::array<QuintetDriverRules, format::kQuintetMostRevision + 1> rules = {QuintetDriverRules(0),
                                                                                           QuintetDriverRules(1)};
    if (revision >= rules.size())
    {
        throw std::invalid_argument("Quintet's driver has no revision " + std::to_string(revision));
    }

    return rules[revision];
}

const QuintetRom* QuintetOf(const DriverRom* rom)
{
    return dynamic_cast<const QuintetRom*>(rom);
}

std::optional<std::pair<const quintet::DriverInfo*, int>> QuintetRom::FileOf(int song) const
{
    if (files.empty() || song < 0)
    {
        return std::nullopt;
    }

    const quintet::DriverInfo& file = files[quintet::FileOfSong(files, song)];
    const int local = song - file.first_song;
    if (local < 0 || local >= quintet::SongCount(file))
    {
        return std::nullopt;
    }

    return std::pair(&file, local);
}

const DriverRules& QuintetRom::Rules() const
{
    const std::optional<uint8_t> revision =
        files.empty() ? std::nullopt : format::QuintetRevision(quintet::TraitsOf(rom, files.front()));
    if (!revision)
    {
        return QuintetRules(0);
    }

    // The rules of the file's revision, with the start tempo of the ROM's revision of the driver.
    static const std::array<QuintetDriverRules, 3> rules = {
        QuintetDriverRules(0, quintet::Revision::kA),
        QuintetDriverRules(0, quintet::Revision::kJ),
        QuintetDriverRules(1, quintet::Revision::kS),
    };
    for (const QuintetDriverRules& rule : rules)
    {
        if (rule.Revision() == *revision && rule.Start() == files.front().revision)
        {
            return rule;
        }
    }

    return QuintetRules(*revision);
}

std::span<const uint8_t> QuintetRom::Bytes() const
{
    return {rom.Ptr(ntdlib::agb::kRomBase), rom.Size()};
}

std::vector<uint32_t> QuintetRom::Sounds() const
{
    std::vector<uint32_t> playing;
    for (uint32_t sound = 0; sound < SoundCount(); ++sound)
    {
        if (audition ? static_cast<int>(sound) == audition->song : HeaderOf(*this, static_cast<int>(sound)).has_value())
        {
            playing.push_back(sound);
        }
    }

    return playing;
}

uint32_t QuintetRom::SoundCount() const
{
    return files.empty() ? 0 : static_cast<uint32_t>(files.back().first_song + quintet::SongCount(files.back()));
}

std::string QuintetRom::SoundName(uint32_t sound) const
{
    return "Song " + ntdlib::sequence_export::agb::SongNumber(static_cast<int>(sound), static_cast<int>(SoundCount()));
}

SoundFields QuintetRom::FieldsOf(uint32_t sound) const
{
    // The header of a song contains its length, then the start of each channel (ntdlib::quintet::Cut). The first
    // channel's field names the sequence.
    SoundFields fields;
    const auto file = FileOf(static_cast<int>(sound));
    if (!file || !HeaderOf(*this, static_cast<int>(sound)))
    {
        return fields;
    }

    const uint64_t header = uint64_t{quintet::SongAddress(*file->first, file->second)} - ntdlib::agb::kRomBase;
    for (int channel = 0; channel < quintet::kChannels; channel++)
    {
        fields.tracks.push_back(header + 2 + 2 * static_cast<uint64_t>(channel));
    }

    fields.sequence = fields.tracks.front();

    return fields;
}

// The rate of the PSG after the play routine's reset of SOUNDBIAS. The model of the GBA's sound in ntdlib mixes at
// agb::kMaxOutputRate at most: the 262,144 Hz of the S revision plays at 65,536 Hz. ntdlib's ArchivePlayer uses the
// same rate.
int QuintetRom::Rate() const
{
    const int psg =
        files.empty() ? ntdlib::agb::kMaxOutputRate : static_cast<int>(quintet::PsgRate(files.front().revision));
    return std::min(psg, ntdlib::agb::kMaxOutputRate);
}

std::vector<int> QuintetRom::Samples() const
{
    std::vector<int> numbers;
    for (std::size_t sample = 0; sample < samples.size(); ++sample)
    {
        numbers.push_back(static_cast<int>(sample));
    }

    return numbers;
}

std::string QuintetRom::SampleName(int sample) const
{
    return DriverSampleName(sample);
}

namespace
{

// Sample `sample` of `rom`. Throws std::invalid_argument for a number that names no sample.
quintet::Sample SampleOf(const QuintetRom& rom, int sample)
{
    std::optional<quintet::Sample> read;
    if (sample >= 0 && static_cast<std::size_t>(sample) < rom.samples.size())
    {
        const QuintetRom::SampleKey& key = rom.samples[static_cast<std::size_t>(sample)];
        read = quintet::ReadSample(rom.rom, rom.files[key.file], key.bank, key.number);
    }
    if (!read || read->played == 0)
    {
        throw std::invalid_argument("The ROM holds no " + rom.SampleName(sample) + ".");
    }

    return *read;
}

} // namespace

double QuintetRom::SampleRate(int sample) const
{
    return SampleOf(*this, sample).rate;
}

std::vector<uint8_t> QuintetRom::SampleWav(int sample) const
{
    // The driver plays the sample's first `played` bytes. A channel's E7 sets where each note starts, ends and loops.
    const quintet::Sample read = SampleOf(*this, sample);
    return Pcm8Wav(rom, read.data, read.played, false, std::nullopt, std::max<uint32_t>(read.rate, 1));
}

SampleShape QuintetRom::ShapeOf(int sample) const
{
    return {SampleOf(*this, sample).played, std::nullopt};
}

std::vector<int> QuintetRom::SamplesOfSound(uint32_t sound) const
{
    const auto file = FileOf(static_cast<int>(sound));
    if (!file)
    {
        return {};
    }

    // The songs play the samples of bank 0, and the sound effects those of bank 1.
    const auto file_index = static_cast<std::size_t>(file->first - files.data());
    const int bank = file->second < static_cast<int>(file->first->song_addresses.size()) ? 0 : 1;
    std::set<int> played;
    for (int channel = kFirstFifo; channel < quintet::kChannels; channel++)
    {
        for (const uint32_t number : SelectedBy(*this, static_cast<int>(sound), channel, Kind::kProgram, 0))
        {
            const auto found = std::ranges::find_if(
                samples, [&](const SampleKey& key)
                { return key.file == file_index && key.bank == bank && key.number == static_cast<int>(number); });
            if (found != samples.end())
            {
                played.insert(static_cast<int>(found - samples.begin()));
            }
        }
    }

    return {played.begin(), played.end()};
}

std::vector<uint32_t> QuintetRom::TrackPrograms(int track) const
{
    // The squares select one of four duties, and the wave channel the waves of the game's wave table.
    if (track == quintet::kSquare1 || track == quintet::kSquare2)
    {
        return {0, 1, 2, 3};
    }

    return track == quintet::kWave ? waves : std::vector<uint32_t>();
}

Sequence QuintetRom::ReadSequence(uint32_t sound) const
{
    return song::ReadSequence(*this, sound);
}

ntdlib::sequence_export::Conversion QuintetRom::ConvertToMidi(uint32_t sound, const std::string& program) const
{
    const auto file = FileOf(static_cast<int>(sound));
    if (!file)
    {
        throw std::runtime_error("The ROM has no song " + std::to_string(sound) + ".");
    }

    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.program = program;
    ntdlib::sequence_export::quintet::SongFiles converted =
        ntdlib::sequence_export::quintet::ConvertSongFiles(rom, *file->first, file->second, settings);

    return AgbConversion(SoundName(sound), std::move(converted));
}

ntdlib::sequence_export::BankConversion QuintetRom::ConvertSamples(std::span<const int> numbers,
                                                                   const std::string& program) const
{
    namespace agb = ntdlib::sequence_export::agb;
    ntdlib::sequence_export::quintet::SoundfontBuilder builder(rom);
    ntdlib::sequence_export::BankConversion conversion;
    for (const int sample : numbers)
    {
        int index = -1;
        try
        {
            const quintet::Sample read = SampleOf(*this, sample);
            ntdlib::sequence_export::quintet::PcmSound sound;
            sound.start = read.data;
            sound.first = read.played;
            sound.rate = read.rate;
            index = builder.PcmSample(sound);
        }
        catch (const std::invalid_argument&)
        {
            index = -1;
        }
        if (index < 0)
        {
            conversion.warnings.push_back(SampleName(sample) + " holds no samples.");
            continue;
        }

        const int instrument = builder.AddInstrument(SampleName(sample), {agb::SampleZone(index, false)});
        builder.AddPreset(SampleName(sample), sample / 128, sample % 128, instrument);
    }

    return SamplesConversion(std::move(conversion), builder.File(), numbers, program, rom.Title());
}

std::unique_ptr<DriverPlayer> QuintetRom::MakePlayer() const
{
    return std::make_unique<QuintetPlayer>(*this);
}

DriverAudition QuintetRom::SampleAudition(int sample, int key, int) const
{
    // Key 60 plays the sample at its rate: at index 24 of the pitch table. A song of the sample's music file plays a
    // sample of bank 0, and a sound effect one of bank 1.
    SampleOf(*this, sample);
    const SampleKey& found = samples[static_cast<std::size_t>(sample)];
    const quintet::DriverInfo& file = files[found.file];
    const auto songs = static_cast<int>(file.song_addresses.size());
    for (int local = found.bank == 0 ? 0 : songs; local < (found.bank == 0 ? songs : quintet::SongCount(file)); local++)
    {
        if (HeaderOf(*this, file.first_song + local))
        {
            return Audition(file.first_song + local, kFirstFifo, static_cast<uint8_t>(found.number), key);
        }
    }

    throw std::invalid_argument("No song of the music file of " + SampleName(sample) + " can play it.");
}

std::optional<DriverAudition> QuintetRom::NoteAudition(const ntdlib::commands::Sequence& commands,
                                                       const Sequence& sequence, int track, int program, int key,
                                                       int) const
{
    // The instrument is the one that the channel selects before a note of the program on the track.
    const int song = static_cast<int>(sequence.sound);
    const auto file = FileOf(song);
    if (!file || track < 0 || track >= quintet::kChannels)
    {
        return std::nullopt;
    }

    const auto found = std::ranges::find_if(sequence.tracks, [track](const Track& t) { return t.number == track; });
    if (found == sequence.tracks.end())
    {
        return std::nullopt;
    }

    const auto note =
        std::ranges::find_if(found->notes, [program](const Note& n) { return static_cast<int>(n.program) == program; });
    if (note == found->notes.end())
    {
        return std::nullopt;
    }

    try
    {
        const std::vector<uint32_t> offsets = Rules().ItemOffsets(commands);
        const auto item = std::ranges::find(offsets, note->command);
        const auto stretch = static_cast<std::size_t>(note->stretch);
        if (item == offsets.end() || stretch >= found->stretches.size())
        {
            return std::nullopt;
        }

        const uint8_t instrument = InstrumentBefore(commands, offsets, found->stretches, stretch,
                                                    static_cast<std::size_t>(item - offsets.begin()), track);
        return Audition(song, track, instrument, key);
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
}

std::shared_ptr<const DriverRom> QuintetRom::AuditionRom(const DriverAudition& audition) const
{
    // The setup's first command names the song, and its second the instrument.
    if (audition.setup.size() < 2 || audition.track < 0 || audition.track >= quintet::kChannels)
    {
        throw std::invalid_argument("The note names no instrument of a channel.");
    }

    const int song = static_cast<int>(audition.setup[0].value.value / kOne);
    if (!HeaderOf(*this, song))
    {
        throw std::invalid_argument("The note names no song of the ROM.");
    }

    // The key of the note sets its index in the pitch table. The noise channel's notes show the drums of the
    // conversion's kit, and a drum plays at index 24. A FIFO's volume is 1 at most.
    const int index = audition.track == quintet::kNoise ? 24 : static_cast<int>(audition.note.key) - kIndexKey;
    if (index < 0 || index >= (audition.track >= kFirstFifo ? quintet::kPcmPitchEntries : quintet::kFrequencyEntries))
    {
        throw std::invalid_argument("Quintet's driver can't play this key.");
    }

    auto copy = std::make_shared<QuintetRom>(*this);
    copy->audition = AuditionNote{
        .song = song,
        .channel = audition.track,
        .instrument = static_cast<uint8_t>(std::clamp<int64_t>(audition.setup[1].value.value / kOne, 0, 255)),
        .index = index,
        .volume = static_cast<uint8_t>(audition.track >= kFirstFifo       ? 1
                                       : audition.track == quintet::kWave ? 4
                                                                          : 15)};

    return copy;
}

namespace
{

// The ROM in `bytes` with the music files of its driver read (LoadQuintetSongs).
std::shared_ptr<QuintetRom> RomWithSongs(std::vector<uint8_t> bytes)
{
    auto rom = std::make_shared<QuintetRom>();
    rom->rom.Assign(std::move(bytes));
    quintet::DriverInfo info;
    std::string error;
    if (!quintet::DetectDriver(rom->rom, {}, info, error))
    {
        throw xsflib::FormatError(error.empty() ? "the ROM shows no sign of Quintet's driver"
                                                : "Can't read the tables of Quintet's driver: " + error);
    }

    rom->files = quintet::ReadMusicFiles(rom->rom, info);
    return rom;
}

} // namespace

int QuintetMostVolume(int track, std::optional<quintet::Revision> start)
{
    if (track == quintet::kWave)
    {
        return quintet::TraitsOf(start.value_or(quintet::Revision::kJ)).wave_levels - 1;
    }

    return track >= kFirstFifo ? 1 : 15;
}

std::shared_ptr<const QuintetRom> LoadQuintetSongs(std::vector<uint8_t> bytes)
{
    return RomWithSongs(std::move(bytes));
}

std::shared_ptr<const QuintetRom> LoadQuintetRom(std::vector<uint8_t> bytes)
{
    const std::shared_ptr<QuintetRom> rom = RomWithSongs(std::move(bytes));

    // The samples of each file's banks, once each, and the waves that the songs select.
    std::set<uint32_t> headers;
    for (std::size_t f = 0; f < rom->files.size(); ++f)
    {
        const quintet::DriverInfo& file = rom->files[f];
        for (int bank = 0; bank < 2; bank++)
        {
            for (std::size_t number = 0; number < file.sample_addresses[static_cast<std::size_t>(bank)].size();
                 ++number)
            {
                const std::optional<quintet::Sample> sample =
                    quintet::ReadSample(rom->rom, file, bank, static_cast<int>(number));
                if (sample && sample->played != 0 && headers.insert(sample->header).second)
                {
                    rom->samples.push_back({f, bank, static_cast<int>(number)});
                }
            }
        }
    }

    for (const uint32_t sound : rom->Sounds())
    {
        for (const uint32_t wave : SelectedBy(*rom, static_cast<int>(sound), quintet::kWave, Kind::kProgram, 2))
        {
            if (std::ranges::find(rom->waves, wave) == rom->waves.end())
            {
                rom->waves.push_back(wave);
            }
        }
    }

    if (rom->Sounds().empty())
    {
        throw xsflib::FormatError("Quintet's music files hold no song that is in the ROM");
    }

    return rom;
}

} // namespace hokuto::song
