// SPDX-License-Identifier: MIT

#include "song/ubimilan.h"

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

#include "ntdlib/agb/gba.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sequence_export/agb/psg.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/ubimilan/convert.h"
#include "ntdlib/ubimilan/archive_player.h"
#include "ntdlib/ubimilan/commands.h"
#include "ntdlib/ubimilan/output.h"
#include "ntdlib/ubimilan/sequencer.h"
#include "xsflib/binary.h"

#include "format/ubimilan.h"
#include "song/driver_player.h"
#include "song/gba_bytes.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace hokuto::song
{

namespace
{

namespace ubimilan = ntdlib::ubimilan;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// The output's rate: the driver's init sets 65,536 Hz in the sound bias register.
constexpr int kUbiMilanRate = 65536;

// The keys that a note's command contains.
constexpr int kHighestKey = 127;

// The largest velocity byte of a note of the kit. The game resets on a voice above it.
constexpr int kMostKitVolume = 64;

// The longest countdown that a command's two bytes hold.
constexpr int64_t kLongestCountdown = 0xFFFF;

// The frames that a note of an audition sounds for.
constexpr int64_t kAuditionFrames = 600;

// The rules of the driver.
class UbiMilanDriverRules final : public DriverRules
{
public:
    UbiMilanDriverRules()
        : DriverRules({.engine = format::Engine::kUbiMilan,
                       .revision = 0,
                       .start_volume = 127,
                       // A note of the kit plays the sample of the kit's entry for its key.
                       .notes_name_samples = false,
                       .mixes_echo = false})
    {
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence) const override
    {
        return format::UbiMilanItemOffsets(sequence);
    }

    ntdlib::commands::Scale ScaleOf(Kind kind) const override
    {
        return ubimilan::ScaleOf(kind);
    }

    // Nearly every note of the driver's songs plays the kit. A note of the PSG gets its velocity by its channel
    // (UbiMilanVelocityOf).
    int VelocityOf(int volume, bool) const override
    {
        return UbiMilanVelocityOf(ubimilan::kKitChannel, ubimilan::kLastNoiseKey + 1, volume);
    }

    int VolumeOf(int velocity, bool) const override
    {
        return UbiMilanDriverVelocity(ubimilan::kKitChannel, ubimilan::kLastNoiseKey + 1, velocity, 0);
    }

    // The driver has no volume of a track. The fader shows each track at its start volume, the most.
    int MostVolume(int) const override
    {
        return 127;
    }

    bool PsgTrack(int track) const override
    {
        return track < ubimilan::kPsgChannels;
    }

    // A note's command contains the driver's velocity, a byte.
    std::optional<NoteVelocities> Velocities() const override
    {
        return NoteVelocities{UINT8_MAX, "A note's velocity runs from 0 to 255."};
    }
};

// The model of the driver and the GBA's sound hardware, for one ROM (ntdlib::ubimilan::ArchivePlayer).
class UbiMilanPlayer final : public ArchiveDriverPlayer<ubimilan::ArchivePlayer>
{
public:
    explicit UbiMilanPlayer(const UbiMilanRom& rom) : ArchiveDriverPlayer(rom.rom, rom.info)
    {
    }

    void Start(uint32_t sound, bool enhanced) override
    {
        ubimilan::ArchivePlayerOptions options;
        options.enhanced = enhanced;
        player_.Start(static_cast<int>(sound), options);
    }

    int Tracks() const override
    {
        return ubimilan::kTracks;
    }

    // A tick is a frame. The sequencer counts the frames whose commands have run.
    uint32_t Tick() const override
    {
        return player_.Frames() == 0 ? 0 : player_.Driver().Song().Frame();
    }

    bool Finished() const override
    {
        if (player_.Frames() == 0)
        {
            return false;
        }

        const ubimilan::Sequencer& song = player_.Driver().Song();
        return song.Reset() || !song.TrackActive();
    }
};

Command Countdown(int64_t frames)
{
    return ValueCommand(Kind::kWait, frames, 0);
}

// The level from 0 to 1 that the velocity byte of a note command sets on sequence channel `channel` at key `key`.
double LevelOf(int channel, int key, int velocity)
{
    velocity = std::clamp(velocity, 0, 255);
    if (ubimilan::PlaysKit(channel, key))
    {
        return std::min(velocity, kMostKitVolume) / static_cast<double>(kMostKitVolume);
    }
    if (channel == ubimilan::kWaveChannel)
    {
        return ntdlib::sequence_export::agb::WaveShare(static_cast<uint16_t>(velocity << 8));
    }

    return (velocity >> 4) / 15.0;
}

// The sample of a sample resource in the form that the mixer plays: 8-bit points.
std::optional<ubimilan::GameSample> SampleOf(const UbiMilanRom& rom, int resource)
{
    ubimilan::GameSample sample;
    const uint32_t address = ubimilan::ResourceAddress(rom.rom, rom.info, resource);
    if (address == 0 || !ubimilan::ReadSample(rom.rom, address, sample) || sample.length == 0)
    {
        return std::nullopt;
    }

    return sample;
}

} // namespace

const DriverRules& UbiMilanRules()
{
    static const UbiMilanDriverRules rules;
    return rules;
}

const UbiMilanRom* UbiMilanOf(const DriverRom* rom)
{
    return dynamic_cast<const UbiMilanRom*>(rom);
}

int UbiMilanVelocityOf(int channel, int key, int velocity)
{
    const double level = LevelOf(channel, key, velocity);
    if (level <= 0)
    {
        return 0;
    }

    return std::clamp(static_cast<int>(std::lround(127 * std::sqrt(level))), 1, 127);
}

int UbiMilanDriverVelocity(int channel, int key, int velocity, int old)
{
    velocity = std::clamp(velocity, 0, 127);
    const double level = (velocity / 127.0) * (velocity / 127.0);
    if (ubimilan::PlaysKit(channel, key))
    {
        const int volume = static_cast<int>(std::lround(level * kMostKitVolume));
        return velocity > 0 ? std::clamp(volume, 1, kMostKitVolume) : 0;
    }

    if (channel == ubimilan::kWaveChannel)
    {
        // The nearest of the levels that NR32 supports: bits 5 and 6 select 0, 1, 0.5 or 0.25, and bit 7 selects 0.75.
        static constexpr std::array<std::pair<int, double>, 5> kBits = {
            {{0x00, 0}, {0x20, 1}, {0x40, 0.5}, {0x60, 0.25}, {0x80, 0.75}}};
        std::size_t nearest = 0;
        for (std::size_t bits = 1; bits < kBits.size(); bits++)
        {
            if (std::abs(kBits[bits].second - level) < std::abs(kBits[nearest].second - level))
            {
                nearest = bits;
            }
        }
        if (velocity > 0 && nearest == 0)
        {
            nearest = 3;
        }

        return (old & ~0xE0) | kBits[nearest].first;
    }

    int volume = static_cast<int>(std::lround(level * 15));
    if (velocity > 0)
    {
        volume = std::clamp(volume, 1, 15);
    }

    return (old & 0x0F) | volume << 4;
}

const DriverRules& UbiMilanRom::Rules() const
{
    return UbiMilanRules();
}

std::span<const uint8_t> UbiMilanRom::Bytes() const
{
    return {rom.Ptr(ntdlib::agb::kRomBase), rom.Size()};
}

std::vector<uint32_t> UbiMilanRom::Sounds() const
{
    // A piece plays when its list of sequences can be read and its first sequence falls within the ROM. The songs past
    // DriverInfo::piece_count are samples that the game plays as music: they have no sequences, and the browser lists
    // them among the samples.
    std::vector<uint32_t> playing;
    for (uint32_t sound = 0; sound < static_cast<uint32_t>(info.piece_count); ++sound)
    {
        ubimilan::MusicPiece piece;
        if (!ubimilan::ReadMusic(rom, info, static_cast<int>(sound), piece) || piece.sequences.empty())
        {
            continue;
        }

        const uint32_t first = ubimilan::ResourceAddress(rom, info, piece.sequences.front());
        if (first != 0 && rom.U8(first) == ubimilan::kSequence)
        {
            playing.push_back(sound);
        }
    }

    return playing;
}

uint32_t UbiMilanRom::SoundCount() const
{
    return static_cast<uint32_t>(info.songs.size());
}

std::string UbiMilanRom::SoundName(uint32_t sound) const
{
    return "Song " +
           ntdlib::sequence_export::agb::SongNumber(static_cast<int>(sound), static_cast<int>(info.songs.size()));
}

SoundFields UbiMilanRom::FieldsOf(uint32_t sound) const
{
    // The resource table's field of the piece's first sequence names the sound's song. The sequence starts the one
    // track of commands.
    SoundFields fields;
    ubimilan::MusicPiece piece;
    if (!ubimilan::ReadMusic(rom, info, static_cast<int>(sound), piece) || piece.sequences.empty())
    {
        return fields;
    }

    const int sequence = piece.sequences.front();
    if (sequence < 0 || sequence >= info.resource_count)
    {
        return fields;
    }

    fields.sequence = uint64_t{info.resource_table} + 4 * static_cast<uint64_t>(sequence) - ntdlib::agb::kRomBase;
    fields.entries.push_back(0);

    return fields;
}

int UbiMilanRom::Rate() const
{
    return kUbiMilanRate;
}

std::vector<int> UbiMilanRom::Samples() const
{
    return sample_resources;
}

std::string UbiMilanRom::SampleName(int sample) const
{
    return DriverSampleName(sample);
}

void UbiMilanRom::CheckSample(int sample) const
{
    if (std::ranges::find(sample_resources, sample) == sample_resources.end() || !SampleOf(*this, sample))
    {
        throw std::invalid_argument("The ROM has no " + SampleName(sample) + ".");
    }
}

double UbiMilanRom::SampleRate(int sample) const
{
    CheckSample(sample);
    return ubimilan::PointRate(info);
}

std::vector<uint8_t> UbiMilanRom::SampleWav(int sample) const
{
    const double rate = SampleRate(sample);
    const ubimilan::GameSample game = *SampleOf(*this, sample);
    const std::optional<WavLoop> loop = game.loop && game.loop_end > game.loop_start && game.loop_end <= game.length
                                            ? std::optional(WavLoop{game.loop_start, game.loop_end})
                                            : std::nullopt;
    return Pcm8Wav(rom, game.data, game.length, false, loop, WavRate(rate));
}

SampleShape UbiMilanRom::ShapeOf(int sample) const
{
    CheckSample(sample);
    const ubimilan::GameSample game = *SampleOf(*this, sample);
    return {game.length, game.loop ? std::optional(game.loop_start) : std::nullopt};
}

std::optional<int> UbiMilanRom::KitSample(int program, int key) const
{
    if (!ubimilan::PlaysKit(ubimilan::kKitChannel, key) || info.kit == 0)
    {
        return std::nullopt;
    }

    const uint32_t entry = info.kit + 2 * static_cast<uint32_t>(program + key);
    if (!rom.Contains(entry, 2))
    {
        return std::nullopt;
    }

    const uint16_t resource = rom.U16(entry);
    if (resource == ubimilan::kNoResource || !SampleOf(*this, resource))
    {
        return std::nullopt;
    }

    return resource;
}

std::vector<int> UbiMilanRom::SamplesOfSound(uint32_t sound) const
{
    // The kit's notes play samples. Their keys and the kit's offset give each one.
    std::set<int> played;
    try
    {
        const Sequence sequence = ReadSequence(sound);
        for (const Track& track : sequence.tracks)
        {
            if (track.number != ubimilan::kKitChannel)
            {
                continue;
            }

            for (const Note& note : track.notes)
            {
                if (const std::optional<int> sample = KitSample(static_cast<int>(note.program), note.key))
                {
                    played.insert(*sample);
                }
            }
        }
    }
    catch (const std::exception&)
    {
        return {};
    }

    return {played.begin(), played.end()};
}

Sequence UbiMilanRom::ReadSequence(uint32_t sound) const
{
    return song::ReadSequence(*this, sound);
}

ntdlib::sequence_export::Conversion UbiMilanRom::ConvertToMidi(uint32_t sound, const std::string& program) const
{
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.program = program;
    ntdlib::sequence_export::ubimilan::SongFiles files =
        ntdlib::sequence_export::ubimilan::ConvertSongFiles(rom, info, static_cast<int>(sound), settings);

    return AgbConversion(SoundName(sound), std::move(files));
}

ntdlib::sequence_export::BankConversion UbiMilanRom::ConvertSamples(std::span<const int> samples,
                                                                    const std::string& program) const
{
    // Each sample at the mixer's rate, unpitched at key 60. The mixer reads 8 points at a time and checks for a loop's
    // end after each 8: a loop lasts a whole number of 8 points. In the SoundFonts that ntdlib makes of the kits of the
    // driver, the loops last whole numbers of 8 points too.
    namespace agb = ntdlib::sequence_export::agb;
    agb::Sf2File file;
    ntdlib::sequence_export::BankConversion conversion;
    for (const int sample : samples)
    {
        const std::optional<ubimilan::GameSample> game =
            std::ranges::find(sample_resources, sample) != sample_resources.end() ? SampleOf(*this, sample)
                                                                                  : std::nullopt;
        if (!game)
        {
            conversion.warnings.push_back(SampleName(sample) + " holds no samples.");
            continue;
        }

        seqlib::sf2::Sample s;
        s.name = SampleName(sample);
        s.sample_rate = static_cast<uint32_t>(std::lround(ubimilan::PointRate(info)));
        s.original_key = 60;
        uint32_t points = game->length;
        if (game->loop && game->loop_end > game->loop_start)
        {
            const uint32_t length = (game->loop_end - game->loop_start + ubimilan::kRunPoints - 1) /
                                    ubimilan::kRunPoints * ubimilan::kRunPoints;
            points = game->loop_start + length;
            s.loop_start = game->loop_start;
            s.loop_end = points;
        }

        for (uint32_t i = 0; i < points; ++i)
        {
            const uint32_t at = game->data + i;
            s.data.push_back(static_cast<int16_t>(rom.Contains(at) ? rom.S8(at) * 256 : 0));
        }

        const bool looped = agb::Looped(s);
        if (looped)
        {
            agb::LengthenLoop(s);
            agb::AddLoopGuard(s);
        }

        file.samples.push_back(std::move(s));

        const std::string name = SampleName(sample);
        const int index = static_cast<int>(file.samples.size()) - 1;
        file.instruments.push_back(agb::MakeInstrument(name, {agb::RootKeyZone(index, looped, 0, 127, 60)}));
        file.presets.push_back(agb::MakePreset(name, static_cast<uint16_t>(sample / 128),
                                               static_cast<uint16_t>(sample % 128),
                                               static_cast<int>(file.instruments.size()) - 1));
    }

    return SamplesConversion(std::move(conversion), file, samples, program, rom.Title());
}

std::unique_ptr<DriverPlayer> UbiMilanRom::MakePlayer() const
{
    return std::make_unique<UbiMilanPlayer>(*this);
}

DriverAudition UbiMilanRom::SampleAudition(int sample, int, int velocity) const
{
    CheckSample(sample);

    // The kit plays a sample unpitched. A piece starts with the kit's offset at 0, and a key above 11 plays the kit's
    // entry of its number. An offset above 11 reaches the entries further on.
    for (int entry = ubimilan::kLastNoiseKey + 1; entry <= kHighestKey + 0xFF; ++entry)
    {
        const int program = entry <= kHighestKey ? 0 : entry - kHighestKey + ubimilan::kLastNoiseKey;
        const int key = entry - program;
        if (program > 0xFF || KitSample(program, key) != sample)
        {
            continue;
        }

        DriverAudition audition;
        audition.track = ubimilan::kKitChannel;
        if (program > 0)
        {
            audition.setup = {ValueCommand(Kind::kProgram, program, ubimilan::kKitChannel)};
        }

        audition.note.kind = Kind::kNote;
        audition.note.index = ubimilan::kKitChannel;
        audition.note.key = static_cast<uint8_t>(key);
        audition.note.velocity = std::max(UbiMilanDriverVelocity(ubimilan::kKitChannel, key, velocity, 0), 1) * kOne;
        return audition;
    }

    throw std::invalid_argument("No key of the kit plays " + SampleName(sample) + ".");
}

std::optional<DriverAudition> UbiMilanRom::NoteAudition(const ntdlib::commands::Sequence&, const Sequence& sequence,
                                                        int track, int program, int key, int velocity) const
{
    // A note's program is the kit's offset on channel 9, and a PSG channel's instrument on the others.
    const auto found = std::ranges::find(sequence.tracks, track, &Track::number);
    if (found == sequence.tracks.end() || found->notes.empty() ||
        (track >= ubimilan::kPsgChannels && track != ubimilan::kKitChannel))
    {
        return std::nullopt;
    }

    const auto note = std::ranges::find_if(
        found->notes, [program](const Note& note) { return note.program == static_cast<uint32_t>(program); });
    if (note == found->notes.end())
    {
        return std::nullopt;
    }

    DriverAudition audition;
    audition.track = track;
    audition.setup = {ValueCommand(Kind::kProgram, program, static_cast<uint8_t>(track))};
    audition.note.kind = Kind::kNote;
    audition.note.index = static_cast<uint8_t>(track);
    audition.note.key = static_cast<uint8_t>(std::clamp(key, 0, kHighestKey));
    audition.note.velocity = std::max(UbiMilanDriverVelocity(track, audition.note.key, velocity, 0), 1) * kOne;

    return audition;
}

std::shared_ptr<const DriverRom> UbiMilanRom::AuditionRom(const DriverAudition& audition) const
{
    if (info.songs.empty())
    {
        throw std::invalid_argument("Can't play this note. The ROM has no piece to play it in.");
    }

    ubimilan::MusicPiece piece;
    if (!ubimilan::ReadMusic(rom, info, 0, piece))
    {
        throw std::invalid_argument("Can't read the first piece of the ROM.");
    }

    // A sequence of the setup, the note, a countdown as long as the note, its note-off and the end. A new piece plays
    // it once, in the place of the first piece. Both go after the ROM's bytes, and the resource table names them.
    ntdlib::commands::Sequence sequence;
    Command header;
    header.kind = Kind::kData;
    header.bytes = {ubimilan::kSequence, 0, 0, 0};
    sequence.items.push_back(header);
    sequence.items.push_back(Countdown(0));
    for (const Command& setup : audition.setup)
    {
        sequence.items.push_back(setup);
        sequence.items.push_back(Countdown(0));
    }

    sequence.items.push_back(audition.note);
    sequence.items.push_back(Countdown(std::min(kAuditionFrames, kLongestCountdown)));
    Command off = audition.note;
    off.kind = Kind::kNoteOff;
    off.velocity = 0;
    sequence.items.push_back(off);
    sequence.items.push_back(Countdown(0));
    sequence.items.push_back(Command{.kind = Kind::kEnd});

    const ubimilan::Encoded encoded = ubimilan::EncodeSequence(sequence);
    std::vector<uint8_t> bytes = PaddedRomCopy(rom);
    const uint32_t sequence_at = AppendWordAligned(bytes, encoded.bytes);
    const uint32_t piece_at = EndAddress(bytes);
    const int sequence_id = piece.sequences.front();
    const std::vector<uint8_t> list = {ubimilan::kMusic,
                                       0,
                                       0,
                                       0,
                                       1,
                                       0,
                                       0,
                                       0,
                                       static_cast<uint8_t>(sequence_id),
                                       static_cast<uint8_t>(sequence_id >> 8)};
    AppendWordAligned(bytes, list);

    // The resource table contains the distance of each resource from the start of the bank.
    const auto put = [&bytes, this](int resource, uint32_t address)
    {
        const uint32_t field = info.resource_table + 4 * static_cast<uint32_t>(resource) - ntdlib::agb::kRomBase;
        const uint32_t distance = address - info.bank;
        for (uint32_t i = 0; i < 4; ++i)
        {
            bytes[field + i] = static_cast<uint8_t>(distance >> (8 * i));
        }
    };
    put(sequence_id, sequence_at);
    put(info.songs.front(), piece_at);

    auto copy = std::make_shared<UbiMilanRom>();
    copy->rom.Assign(std::move(bytes));
    copy->info = info;
    copy->sample_resources = sample_resources;

    return copy;
}

std::shared_ptr<const UbiMilanRom> LoadUbiMilanRom(std::vector<uint8_t> bytes)
{
    auto rom = std::make_shared<UbiMilanRom>();
    rom->rom.Assign(std::move(bytes));
    std::string error;
    if (!ubimilan::DetectDriver(rom->rom, {}, rom->info, error))
    {
        throw xsflib::FormatError(error.empty()
                                      ? "the ROM shows no sign of Ubisoft Milan's driver"
                                      : "Hokuto can't read the sound bank of Ubisoft Milan's driver: " + error);
    }

    // The sample resources of the bank, in order.
    for (int resource = 0; resource < rom->info.resource_count; ++resource)
    {
        const uint32_t address = ubimilan::ResourceAddress(rom->rom, rom->info, resource);
        if (address != 0 && rom->rom.U8(address) == ubimilan::kSample && SampleOf(*rom, resource))
        {
            rom->sample_resources.push_back(resource);
        }
    }

    if (rom->Sounds().empty())
    {
        throw xsflib::FormatError("the sound bank of the driver points to no piece of music in the ROM");
    }

    return rom;
}

} // namespace hokuto::song
