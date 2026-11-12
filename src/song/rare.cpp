// SPDX-License-Identifier: MIT

#include "song/rare.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
#include "ntdlib/rare/archive_player.h"
#include "ntdlib/rare/commands.h"
#include "ntdlib/rare/sequencer.h"
#include "ntdlib/rare/song.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/rare/convert.h"
#include "ntdlib/sequence_export/rare/soundfont.h"
#include "xsflib/binary.h"

#include "format/rare.h"
#include "song/driver_player.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace hokuto::song
{

namespace
{

namespace rare = ntdlib::rare;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// The output's rate: the driver's init sets 8 bits at 65,536 Hz in the sound bias register.
constexpr int kRareRate = 65536;

// The largest value of a channel's volume (controller 7) and of a note's velocity.
constexpr int kMostValue = 127;

// The rules of a build of the driver.
class RareDriverRules final : public DriverRules
{
public:
    explicit RareDriverRules(uint8_t revision)
        : DriverRules(
              {.engine = format::Engine::kRare,
               .revision = revision,
               .start_volume = kMostValue,
               // A track selects a program of the tune's program map, and the program's instrument selects the sample.
               .notes_name_samples = false,
               .mixes_echo = false}),
          revision_(revision)
    {
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence) const override
    {
        return rare::ItemOffsets(sequence, format::RareTraits(revision_));
    }

    ntdlib::commands::Scale ScaleOf(Kind kind) const override
    {
        return rare::ScaleOf(kind);
    }

    // A tempo is the microseconds of a quarter note, 1 at least: 0 makes no time pass.
    double TempoBpm(ntdlib::commands::Fixed value) const override
    {
        const double micros = static_cast<double>(value) / static_cast<double>(ntdlib::commands::kOne);
        return micros > 0.0 ? 60'000'000.0 / micros : 0.0;
    }

    ntdlib::commands::Fixed TempoValue(double bpm) const override
    {
        const double micros = bpm > 0.0 ? std::max(std::round(60'000'000.0 / bpm), 1.0) : 1.0;
        return static_cast<ntdlib::commands::Fixed>(micros) * ntdlib::commands::kOne;
    }

    // A note has the velocity of the driver, up to 127.
    int VelocityOf(int volume, bool) const override
    {
        return std::clamp(volume, 0, kMostValue);
    }

    int VolumeOf(int velocity, bool) const override
    {
        return std::clamp(velocity, 0, kMostValue);
    }

    // A channel's volume (controller 7) runs from 0 to 127, and starts at 127.
    int MostVolume(int) const override
    {
        return kMostValue;
    }

    // The driver plays samples alone, on Direct Sound A.
    bool PsgTrack(int) const override
    {
        return false;
    }

    // A note's command contains the driver's velocity, a byte.
    std::optional<NoteVelocities> Velocities() const override
    {
        return NoteVelocities{UINT8_MAX, "A note's velocity runs from 0 to 255."};
    }

private:
    uint8_t revision_;
};

// The model of the driver and the GBA's sound hardware, for one ROM (ntdlib::rare::ArchivePlayer).
//
// A track of the driver counts a wait's ticks when the wait starts. The clock therefore counts the frames from the
// first frame. That frame sets the first tempo. ntdlib's converter times the arrangement in the GBA's frames
// (FrameStarts in sequence_export/rare/convert.cpp), and the tempo of the arrangement converts that time to ticks,
// round the loop once the time passes the loop's end.
class RarePlayer final : public ArchiveDriverPlayer<rare::ArchivePlayer>
{
public:
    explicit RarePlayer(const RareRom& rom) : ArchiveDriverPlayer(rom.rom, rom.info), rom_(rom)
    {
    }

    void Start(uint32_t sound, bool enhanced) override
    {
        rare::ArchivePlayerOptions options;
        options.enhanced = enhanced;
        options.under_water = rom_.VersionOf(sound) == ntdlib::sequence_export::rare::kUnderWater;
        if (rom_.audition)
        {
            const RareRom::AuditionNote& note = *rom_.audition;
            player_.StartNote(note.tune, note.program, note.key, note.velocity, options);
            return;
        }

        player_.Start(rom_.SongOf(sound), options);
    }

    int Tracks() const override
    {
        return rare::kPlayerTracks;
    }

    // Without an arrangement, the ticks of the track that has run the most of them.
    uint32_t Tick() const override
    {
        if (!clock_)
        {
            return static_cast<uint32_t>(player_.Tick());
        }

        const double frames = static_cast<double>(std::max<uint64_t>(player_.Frames(), 1) - 1);
        const double ticks = TicksOfMs(*clock_, frames * kFrameMs);
        return static_cast<uint32_t>(std::clamp(std::round(ticks), 0.0, static_cast<double>(UINT32_MAX)));
    }

    void SetArrangement(const Sequence& sequence) override
    {
        clock_.reset();
        if (!rom_.audition)
        {
            clock_.emplace();
            clock_->timebase = sequence.timebase;
            clock_->tempo = sequence.tempo;
            clock_->loop = sequence.loop;
        }
    }

    bool Finished() const override
    {
        return !player_.Busy() || player_.Driver().Ended();
    }

private:
    // The time of a frame, in milliseconds.
    static constexpr double kFrameMs = ntdlib::agb::FrameSeconds(1.0) * 1000.0;

    // The ticks that a player counts at `ms` of the song's time in `sequence`: up to the loop's end, and then round the
    // loop (PositionOfTicks).
    static double TicksOfMs(const Sequence& sequence, double ms)
    {
        const std::optional<Loop>& loop = sequence.loop;
        const double end = loop ? MsOfTick(sequence, loop->end) : 0.0;
        if (!loop || loop->end <= loop->start || ms < end)
        {
            return TickOfMs(sequence, ms);
        }

        const double start = MsOfTick(sequence, loop->start);
        const double pass = end - start;
        const double passes = std::floor((ms - end) / pass);
        return loop->end + passes * (loop->end - loop->start) +
               (TickOfMs(sequence, start + (ms - end - passes * pass)) - loop->start);
    }

    const RareRom& rom_;

    // The timebase, the tempo and the loop of the arrangement (SetArrangement).
    std::optional<Sequence> clock_;
};

// The header of tune `tune`, or nothing where it can't be read.
std::optional<rare::TuneHeader> HeaderOf(const RareRom& rom, int tune)
{
    if (tune < 0 || tune >= rom.info.tune_count)
    {
        return std::nullopt;
    }

    rare::TuneHeader header;
    if (!rare::ReadTuneHeader(rom.rom, rare::TuneAddress(rom.rom, rom.info, tune), header))
    {
        return std::nullopt;
    }

    return header;
}

// The instrument at `address` where it plays a sample.
std::optional<rare::Instrument> SampleInstrument(const ntdlib::agb::Rom& rom, uint32_t address)
{
    rare::Instrument instrument;
    if (address == 0 || !rare::ReadInstrument(rom, address, instrument) || !rare::SampleValid(rom, instrument))
    {
        return std::nullopt;
    }

    return instrument;
}

// Each sample instrument that a program of the tune of `header` plays, once each, in the order of the programs and the
// keys.
std::vector<uint32_t> SamplesOfTune(const ntdlib::agb::Rom& rom, const rare::TuneHeader& header)
{
    std::vector<uint32_t> found;
    for (int program = 0; program < 128; program++)
    {
        if (!rare::LookUpInstrument(rom, header, program, 60).mapped)
        {
            continue;
        }

        for (int key = 0; key < 128; key++)
        {
            const uint32_t voice = rare::LookUpInstrument(rom, header, program, key).voice;
            if (std::ranges::find(found, voice) == found.end() && SampleInstrument(rom, voice))
            {
                found.push_back(voice);
            }
        }
    }

    return found;
}

// The tune, the program and the key with which a tune plays the sample instrument at `address`, with the key nearest
// `wanted`, or nothing where no tune plays it. A program whose instrument is the sample comes first.
struct SamplePlace
{
    int tune = 0;
    int program = 0;
    int key = 0;
};

std::optional<SamplePlace> PlaceOfSample(const RareRom& rom, uint32_t address, int wanted)
{
    std::optional<SamplePlace> found;
    for (int tune = 0; tune < rom.info.tune_count; tune++)
    {
        const std::optional<rare::TuneHeader> header = HeaderOf(rom, tune);
        for (int program = 0; header && program < 128; program++)
        {
            const rare::Lookup lookup = rare::LookUpInstrument(rom.rom, *header, program, 60);
            if (!lookup.mapped)
            {
                continue;
            }
            if (lookup.program == address)
            {
                return SamplePlace{tune, program, std::clamp(wanted, 0, 127)};
            }

            for (int key = 0; key < 128; key++)
            {
                if (rare::LookUpInstrument(rom.rom, *header, program, key).voice != address)
                {
                    continue;
                }

                if (!found || (found->tune == tune && found->program == program &&
                               std::abs(key - wanted) < std::abs(found->key - wanted)))
                {
                    found = SamplePlace{tune, program, key};
                }
            }
        }
    }

    return found;
}

DriverAudition Audition(int tune, int program, int key, int velocity)
{
    DriverAudition audition;
    audition.track = tune;
    audition.setup = {ValueCommand(Kind::kProgram, program)};
    audition.note.kind = Kind::kNote;
    audition.note.key = static_cast<uint8_t>(std::clamp(key, 0, 127));
    audition.note.velocity = std::clamp(velocity, 1, kMostValue) * kOne;
    return audition;
}

} // namespace

const DriverRules& RareRules(uint8_t revision)
{
    static const std::array<RareDriverRules, format::kRareMostRevision + 1> rules = {
        RareDriverRules(0), RareDriverRules(1), RareDriverRules(2), RareDriverRules(3)};
    if (revision >= rules.size())
    {
        throw std::invalid_argument("Rare's driver has no revision " + std::to_string(revision));
    }

    return rules[revision];
}

const RareRom* RareOf(const DriverRom* rom)
{
    return dynamic_cast<const RareRom*>(rom);
}

const DriverRules& RareRom::Rules() const
{
    return RareRules(format::RareRevision(rare::TraitsOf(info)));
}

std::span<const uint8_t> RareRom::Bytes() const
{
    return {rom.Ptr(ntdlib::agb::kRomBase), rom.Size()};
}

int RareRom::SongOf(uint32_t sound) const
{
    return sound < sounds.size() ? sounds[sound].first : -1;
}

int RareRom::VersionOf(uint32_t sound) const
{
    return sound < sounds.size() ? sounds[sound].second : 0;
}

std::vector<uint32_t> RareRom::Sounds() const
{
    // A tune plays when its header reads and names a track.
    std::vector<uint32_t> playing;
    for (uint32_t sound = 0; sound < sounds.size(); ++sound)
    {
        const std::optional<rare::TuneHeader> header = HeaderOf(*this, sounds[sound].first);
        if (header && !header->tracks.empty())
        {
            playing.push_back(sound);
        }
    }

    return playing;
}

uint32_t RareRom::SoundCount() const
{
    return static_cast<uint32_t>(sounds.size());
}

std::string RareRom::SoundName(uint32_t sound) const
{
    const int song = SongOf(sound);
    return VersionedSoundName(song, rare::SongCount(info), ntdlib::sequence_export::rare::TuneVersions(info, song),
                              static_cast<std::size_t>(VersionOf(sound)));
}

SoundFields RareRom::FieldsOf(uint32_t sound) const
{
    // Each word of the tune's track list names a track's start (ntdlib::rare::Cut). The first names the sequence.
    SoundFields fields;
    const std::optional<rare::TuneHeader> header = HeaderOf(*this, SongOf(sound));
    if (!header)
    {
        return fields;
    }

    for (std::size_t track = 0; track < header->tracks.size(); track++)
    {
        fields.tracks.push_back(uint64_t{header->track_list} + 4 * track - ntdlib::agb::kRomBase);
    }
    if (!fields.tracks.empty())
    {
        fields.sequence = fields.tracks.front();
    }

    return fields;
}

int RareRom::Rate() const
{
    return kRareRate;
}

std::vector<int> RareRom::Samples() const
{
    std::vector<int> numbers;
    for (std::size_t sample = 0; sample < sample_instruments.size(); ++sample)
    {
        numbers.push_back(static_cast<int>(sample));
    }

    return numbers;
}

std::string RareRom::SampleName(int sample) const
{
    return DriverSampleName(sample);
}

namespace
{

// Sample `sample` of `rom`. Throws std::invalid_argument for a number that names no sample.
rare::Instrument SampleOf(const RareRom& rom, int sample)
{
    const std::optional<rare::Instrument> instrument =
        sample >= 0 && static_cast<std::size_t>(sample) < rom.sample_instruments.size()
            ? SampleInstrument(rom.rom, rom.sample_instruments[static_cast<std::size_t>(sample)])
            : std::nullopt;
    if (!instrument)
    {
        throw std::invalid_argument("The ROM holds no " + rom.SampleName(sample) + ".");
    }

    return *instrument;
}

} // namespace

double RareRom::SampleRate(int sample) const
{
    return SampleOf(*this, sample).rate;
}

std::vector<uint8_t> RareRom::SampleWav(int sample) const
{
    const rare::Instrument instrument = SampleOf(*this, sample);
    const uint32_t length = instrument.end - instrument.start;

    // The loop is the sample's last loop_length points.
    const std::optional<WavLoop> loop =
        instrument.Loops() ? std::optional(WavLoop{length - instrument.loop_length, length}) : std::nullopt;
    return Pcm8Wav(rom, instrument.start, length, false, loop, std::max<uint32_t>(instrument.rate, 1));
}

SampleShape RareRom::ShapeOf(int sample) const
{
    const rare::Instrument instrument = SampleOf(*this, sample);
    const uint32_t length = instrument.end - instrument.start;
    return {length, instrument.Loops() ? std::optional(length - instrument.loop_length) : std::nullopt};
}

std::vector<int> RareRom::SamplesOfSound(uint32_t sound) const
{
    const std::optional<rare::TuneHeader> header = HeaderOf(*this, SongOf(sound));
    if (!header)
    {
        return {};
    }

    std::set<int> played;
    for (const uint32_t address : SamplesOfTune(rom, *header))
    {
        const auto found = std::ranges::find(sample_instruments, address);
        if (found != sample_instruments.end())
        {
            played.insert(static_cast<int>(found - sample_instruments.begin()));
        }
    }

    return {played.begin(), played.end()};
}

std::vector<DriverBankSlot> RareRom::BankSlots(uint32_t sound) const
{
    // A tune's program map is its one bank: the programs that it maps.
    const std::optional<rare::TuneHeader> header = HeaderOf(*this, SongOf(sound));
    if (!header)
    {
        return {};
    }

    DriverBankSlot slot{SoundName(sound), {}};
    for (int program = 0; program < 128; program++)
    {
        if (rare::LookUpInstrument(rom, *header, program, 60).mapped)
        {
            slot.programs.push_back(static_cast<uint32_t>(program));
        }
    }

    return {std::move(slot)};
}

Sequence RareRom::ReadSequence(uint32_t sound) const
{
    return song::ReadSequence(*this, sound);
}

ntdlib::sequence_export::Conversion RareRom::ConvertToMidi(uint32_t sound, const std::string& program) const
{
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.program = program;
    settings.version = VersionOf(sound);
    ntdlib::sequence_export::rare::SongFiles files =
        ntdlib::sequence_export::rare::ConvertSongFiles(rom, info, SongOf(sound), settings);

    return AgbConversion(SoundName(sound), std::move(files));
}

ntdlib::sequence_export::BankConversion RareRom::ConvertSamples(std::span<const int> samples,
                                                                const std::string& program) const
{
    namespace agb = ntdlib::sequence_export::agb;
    ntdlib::sequence_export::rare::SoundfontBuilder builder(rom, info);
    ntdlib::sequence_export::BankConversion conversion;
    for (const int sample : samples)
    {
        const int index = sample >= 0 && static_cast<std::size_t>(sample) < sample_instruments.size()
                              ? builder.InstrumentFor(sample_instruments[static_cast<std::size_t>(sample)])
                              : -1;
        if (index < 0)
        {
            conversion.warnings.push_back(SampleName(sample) + " holds no samples.");
            continue;
        }

        builder.File().presets.push_back(agb::MakePreset(SampleName(sample), static_cast<uint16_t>(sample / 128),
                                                         static_cast<uint16_t>(sample % 128), index));
    }

    return SamplesConversion(std::move(conversion), builder.File(), samples, program, rom.Title());
}

std::unique_ptr<DriverPlayer> RareRom::MakePlayer() const
{
    return std::make_unique<RarePlayer>(*this);
}

DriverAudition RareRom::SampleAudition(int sample, int key, int velocity) const
{
    // Key 60 plays the sample at its rate: at the instrument's root key.
    const rare::Instrument instrument = SampleOf(*this, sample);
    const int wanted = static_cast<int>(instrument.root_key) + key - 60;
    const std::optional<SamplePlace> place =
        PlaceOfSample(*this, sample_instruments[static_cast<std::size_t>(sample)], wanted);
    if (!place)
    {
        throw std::invalid_argument("No program of the driver's tunes plays " + SampleName(sample) + ".");
    }

    return Audition(place->tune, place->program, place->key, velocity);
}

std::optional<DriverAudition> RareRom::NoteAudition(const ntdlib::commands::Sequence&, const Sequence& sequence, int,
                                                    int program, int key, int velocity) const
{
    const int tune = SongOf(sequence.sound);
    if (!HeaderOf(*this, tune) || program < 0 || program > 127)
    {
        return std::nullopt;
    }

    return Audition(tune, program, key, velocity);
}

std::shared_ptr<const DriverRom> RareRom::AuditionRom(const DriverAudition& audition) const
{
    const auto program = std::ranges::find(audition.setup, Kind::kProgram, &Command::kind);
    if (program == audition.setup.end() || !HeaderOf(*this, audition.track))
    {
        throw std::invalid_argument("The note names no program of a tune.");
    }

    auto copy = std::make_shared<RareRom>(*this);
    copy->sounds = {{audition.track, 0}};
    copy->audition = AuditionNote{
        .tune = audition.track,
        .program = static_cast<uint8_t>(std::clamp<int64_t>(program->value.value / kOne, 0, 127)),
        .key = audition.note.key,
        .velocity = static_cast<uint8_t>(std::clamp<int64_t>(audition.note.velocity / kOne, 1, kMostValue))};

    return copy;
}

std::shared_ptr<const RareRom> LoadRareRom(std::vector<uint8_t> bytes)
{
    auto rom = std::make_shared<RareRom>();
    rom->rom.Assign(std::move(bytes));
    std::string error;
    if (!rare::DetectDriver(rom->rom, {}, rom->info, error))
    {
        throw xsflib::FormatError(error.empty() ? "the ROM shows no sign of Rare's driver"
                                                : "Hokuto can't read the tables of Rare's driver: " + error);
    }

    // Each tune's first version, then the other versions.
    const int songs = rare::SongCount(rom->info);
    rom->sounds = VersionedSounds(
        songs, [&rom](int song) { return ntdlib::sequence_export::rare::TuneVersions(rom->info, song).size(); });

    // The sample instruments of each tune, once each.
    for (int song = 0; song < songs; ++song)
    {
        const std::optional<rare::TuneHeader> header = HeaderOf(*rom, song);
        for (const uint32_t address : header ? SamplesOfTune(rom->rom, *header) : std::vector<uint32_t>())
        {
            if (std::ranges::find(rom->sample_instruments, address) == rom->sample_instruments.end())
            {
                rom->sample_instruments.push_back(address);
            }
        }
    }

    if (rom->Sounds().empty())
    {
        throw xsflib::FormatError("the driver's tune table names no tune that is in the ROM");
    }

    return rom;
}

} // namespace hokuto::song
