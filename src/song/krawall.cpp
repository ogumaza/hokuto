// SPDX-License-Identifier: MIT

#include "song/krawall.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
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
#include "ntdlib/krawall/archive_player.h"
#include "ntdlib/krawall/commands.h"
#include "ntdlib/krawall/instruments.h"
#include "ntdlib/krawall/player.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/krawall/convert.h"
#include "ntdlib/sequence_export/krawall/soundfont.h"
#include "xsflib/binary.h"

#include "format/krawall.h"
#include "song/driver_player.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace hokuto::song
{

namespace
{

namespace krawall = ntdlib::krawall;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// The row's note of C-4, from 1 for C-0, and the Key Editor's key of a sample's C-4 at its rate.
constexpr int kRootRowNote = static_cast<int>(ntdlib::sequence_export::krawall::kRootNote) + 1;
constexpr int kRootKey = ntdlib::sequence_export::krawall::kRootKey;

// The rules of Krawall's builds.
class KrawallDriverRules final : public DriverRules
{
public:
    explicit KrawallDriverRules(uint8_t revision)
        : DriverRules({.engine = format::Engine::kKrawall,
                       .revision = revision,
                       .start_volume = 64,
                       // Each note names its instrument.
                       .notes_name_samples = true,
                       .mixes_echo = false}),
          revision_(revision)
    {
    }

    std::vector<uint32_t> ItemOffsets(const ntdlib::commands::Sequence& sequence) const override
    {
        return krawall::ItemOffsets(sequence, {.wide = revision_ != 0});
    }

    ntdlib::commands::Sequence Alone(const ntdlib::commands::Command& item) const override
    {
        return format::KrawallAlone(item);
    }

    ntdlib::commands::Scale ScaleOf(Kind kind) const override
    {
        return krawall::ScaleOf(kind);
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

    // A channel's volume runs from 0 to 64. A note sets it to the volume of its sample.
    int MostVolume(int) const override
    {
        return 64;
    }

    bool PsgTrack(int) const override
    {
        return false;
    }

private:
    uint8_t revision_;
};

// The rows of an arrangement of Krawall, for the player's ticks in the arrangement's (KrawallPlayer). `ticks` contains
// the tick of each row in the order that the rows played, and the end of the last row. A loop jumps back to row
// `loop_row`, and each pass of it takes `pass` ticks.
struct ArrangedRows
{
    std::vector<uint32_t> ticks;
    std::optional<std::size_t> loop_row;
    uint32_t pass = 0;
};

// The rows of `sequence`: the stretches of a track. Every track contains the rows of the module. A sequence without
// rows has none.
std::shared_ptr<const ArrangedRows> RowsOf(const Sequence& sequence)
{
    const auto track = std::ranges::find_if(sequence.tracks, [](const Track& t) { return !t.stretches.empty(); });
    if (track == sequence.tracks.end())
    {
        return nullptr;
    }

    auto rows = std::make_shared<ArrangedRows>();
    const uint32_t end = sequence.loop ? sequence.loop->end : sequence.length;
    for (const Stretch& stretch : track->stretches)
    {
        if (stretch.tick >= end || (!rows->ticks.empty() && stretch.tick <= rows->ticks.back()))
        {
            break;
        }

        rows->ticks.push_back(stretch.tick);
    }
    if (rows->ticks.empty())
    {
        return nullptr;
    }

    rows->ticks.push_back(end);
    if (sequence.loop && sequence.loop->end > sequence.loop->start)
    {
        const auto first = std::ranges::lower_bound(rows->ticks.begin(), rows->ticks.end() - 1, sequence.loop->start);
        if (first != rows->ticks.end() - 1)
        {
            rows->loop_row = static_cast<std::size_t>(first - rows->ticks.begin());
            rows->pass = sequence.loop->end - sequence.loop->start;
        }
    }

    return rows;
}

// The rows that started in `ticks` of the player's ticks, between the end of a row at `speed` and the start of a row at
// `next_speed`. Each of them runs at one of the two speeds. More than one row starts in a frame only at speed 1 and a
// fast tempo.
std::size_t RowsBetween(uint64_t ticks, int speed, int next_speed)
{
    if (ticks == 0)
    {
        return 0;
    }
    if (ticks % static_cast<uint64_t>(next_speed) == 0)
    {
        return static_cast<std::size_t>(ticks / static_cast<uint64_t>(next_speed));
    }
    if (ticks % static_cast<uint64_t>(speed) == 0)
    {
        return static_cast<std::size_t>(ticks / static_cast<uint64_t>(speed));
    }

    return static_cast<std::size_t>(ticks);
}

// The model of Krawall and the GBA's sound hardware, for one ROM (ntdlib::krawall::ArchivePlayer).
//
// In many songs, the timing of the conversion spreads a tick of the player over several ticks of the arrangement: a
// row's ticks in the arrangement, over the row's speed (ntdlib's MakeTiming). Tick() therefore counts the arrangement's
// ticks. After each frame, the player's row, its speed and the player's tick in the row give the place in the
// arrangement's row. At the loop's end, the place returns to the loop's first row, and each pass adds the loop's ticks.
class KrawallPlayer final : public ArchiveDriverPlayer<krawall::ArchivePlayer>
{
public:
    explicit KrawallPlayer(const KrawallRom& rom) : ArchiveDriverPlayer(rom.rom, rom.info), rom_(rom)
    {
    }

    void Start(uint32_t sound, bool enhanced) override
    {
        place_ = {};
        krawall::ArchivePlayerOptions options;
        options.enhanced = enhanced;
        if (rom_.audition)
        {
            const KrawallRom::AuditionNote& note = *rom_.audition;
            player_.StartNote(note.song, note.instrument, note.note, options);
            return;
        }

        player_.Start(static_cast<int>(sound), options);
    }

    int Tracks() const override
    {
        return 16;
    }

    uint32_t Tick() const override
    {
        return rows_ ? place_.tick : static_cast<uint32_t>(player_.Tick());
    }

    void SetArrangement(const Sequence& sequence) override
    {
        rows_ = rom_.audition ? nullptr : RowsOf(sequence);
        place_ = {};
        MovePlace();
    }

    bool Finished() const override
    {
        return !player_.Busy() || !player_.Driver().Playing();
    }

    void RunFrame(std::vector<int16_t>& out) override
    {
        player_.RunFrame(out);
        MovePlace();
    }

    void RunFrame(std::vector<float>& out) override
    {
        player_.RunFrame(out);
        MovePlace();
    }

    std::shared_ptr<const Snapshot> Save(const Snapshot*) const override
    {
        return std::make_shared<const Saved>(player_.Save(), place_);
    }

    void Restore(const Snapshot& snapshot) override
    {
        const Saved& saved = dynamic_cast<const Saved&>(snapshot);
        player_.Restore(saved.snapshot);
        place_ = saved.place;
    }

private:
    // The place in the arrangement after the player's ticks so far: the row of ArrangedRows::ticks, the player's tick
    // that played it and its speed, the passes of the loop before it, and the arrangement's tick.
    struct Place
    {
        uint64_t ticks = 0;
        std::optional<std::size_t> row;
        uint64_t row_start = 0;
        int speed = 1;
        uint64_t passes = 0;
        uint32_t tick = 0;
    };

    struct Saved final : Snapshot
    {
        Saved(krawall::ArchivePlayer::Snapshot saved, const Place& at) : snapshot(std::move(saved)), place(at)
        {
        }

        krawall::ArchivePlayer::Snapshot snapshot;
        Place place;
    };

    // Moves the place on to the player's ticks. A row's first tick leaves the player's tick in the row at 0.
    void MovePlace()
    {
        if (!rows_ || player_.Tick() == place_.ticks || player_.Tick() == 0)
        {
            return;
        }

        const krawall::Player& driver = player_.Driver();
        const uint64_t ticks = player_.Tick();
        const int speed = std::max(driver.Speed(), 1);
        const uint64_t in_row = std::min<uint64_t>(static_cast<uint64_t>(std::max(driver.TickInRow(), 0)), ticks - 1);
        const uint64_t row_start = ticks - 1 - in_row;
        const std::vector<uint32_t>& row_ticks = rows_->ticks;
        const std::size_t last_row = row_ticks.size() - 2;
        if (!place_.row || row_start != place_.row_start)
        {
            const uint64_t next = place_.row ? place_.row_start + static_cast<uint64_t>(place_.speed) : 0;
            const std::size_t rows = 1 + RowsBetween(row_start > next ? row_start - next : 0, place_.speed, speed);
            for (std::size_t i = 0; i < rows; i++)
            {
                if (!place_.row)
                {
                    place_.row = 0;
                }
                else if (*place_.row < last_row)
                {
                    ++*place_.row;
                }
                else if (rows_->loop_row)
                {
                    place_.row = rows_->loop_row;
                    place_.passes++;
                }
            }

            place_.row_start = row_start;
        }

        place_.speed = speed;
        place_.ticks = ticks;

        // The ticks of the row in the arrangement, shared among the ticks of the player at its speed.
        const std::size_t row = *place_.row;
        const uint64_t length = row_ticks[row + 1] - row_ticks[row];
        const uint64_t done = std::min<uint64_t>(in_row + 1, static_cast<uint64_t>(speed));
        place_.tick = static_cast<uint32_t>(row_ticks[row] + done * length / static_cast<uint64_t>(speed) +
                                            place_.passes * rows_->pass);
    }

    const KrawallRom& rom_;
    std::shared_ptr<const ArrangedRows> rows_;
    Place place_;
};

// The header of the module of piece `sound`, or nothing where it can't be read.
std::optional<krawall::ModuleInfo> ModuleOf(const KrawallRom& rom, uint32_t sound)
{
    if (sound >= rom.info.songs.size())
    {
        return std::nullopt;
    }

    krawall::ModuleInfo module;
    if (!krawall::ReadModule(rom.ModulesRom(), rom.info.build, rom.info.songs[sound].module, module))
    {
        return std::nullopt;
    }

    return module;
}

// The instruments that the rows of a module's patterns name, from 1.
std::set<uint16_t> InstrumentsOf(const KrawallRom& rom, uint32_t sound)
{
    std::set<uint16_t> instruments;
    try
    {
        const krawall::Decoded decoded = krawall::DecodeRomSequence(
            rom.ModulesRom(), rom.info, krawall::SequenceOfSong(rom.info, static_cast<int>(sound)));
        for (const Command& command : decoded.sequence.items)
        {
            if ((command.kind == Kind::kNote || command.kind == Kind::kNoteOff) && command.sample.value_or(0) != 0)
            {
                instruments.insert(*command.sample);
            }
            else if (command.kind == Kind::kProgram && command.value.value > 0)
            {
                instruments.insert(static_cast<uint16_t>(command.value.value / kOne));
            }
        }
    }
    catch (const std::exception&)
    {
        return {};
    }

    return instruments;
}

// The samples of the game's table that the instruments of piece `sound`'s rows play.
std::set<int> SamplesOf(const KrawallRom& rom, uint32_t sound)
{
    std::set<int> samples;
    const std::optional<krawall::ModuleInfo> module = ModuleOf(rom, sound);
    if (!module)
    {
        return samples;
    }

    for (const uint16_t instrument : InstrumentsOf(rom, sound))
    {
        const std::optional<krawall::Instrument> read =
            krawall::ReadInstrument(rom.ModulesRom(), rom.info, *module, instrument);
        for (const krawall::Region& region : read ? read->regions : std::vector<krawall::Region>())
        {
            if (region.sample >= 0 && krawall::ReadSample(rom.ModulesRom(), rom.info, region.sample))
            {
                samples.insert(region.sample);
            }
        }
    }

    return samples;
}

// Sample `sample` of the game's table. Throws std::invalid_argument for a number that names no sample that the modules
// play.
krawall::Sample SampleOf(const KrawallRom& rom, int sample)
{
    const std::optional<krawall::Sample> read = std::ranges::binary_search(rom.samples, sample)
                                                    ? krawall::ReadSample(rom.ModulesRom(), rom.info, sample)
                                                    : std::nullopt;
    if (!read || read->end <= read->data)
    {
        throw std::invalid_argument("The ROM holds no " + rom.SampleName(sample) + ".");
    }

    return *read;
}

DriverAudition Audition(uint32_t sound, uint16_t instrument, int note)
{
    DriverAudition audition;
    audition.track = 0;
    for (const int64_t value : {static_cast<int64_t>(sound), static_cast<int64_t>(instrument)})
    {
        Command command;
        command.kind = Kind::kProgram;
        command.value.value = value * kOne;
        audition.setup.push_back(command);
    }

    audition.note.kind = Kind::kNote;
    audition.note.key = static_cast<uint8_t>(std::clamp(note, 1, krawall::kInstrumentNotes));
    audition.note.velocity = 127 * kOne;

    return audition;
}

} // namespace

const DriverRules& KrawallRules(uint8_t revision)
{
    static const std::array<KrawallDriverRules, 2> rules = {KrawallDriverRules(0), KrawallDriverRules(1)};
    if (revision >= rules.size())
    {
        throw std::invalid_argument("Krawall has no revision " + std::to_string(revision));
    }

    return rules[revision];
}

const KrawallRom* KrawallOf(const DriverRom* rom)
{
    return dynamic_cast<const KrawallRom*>(rom);
}

const ntdlib::agb::Rom& KrawallRom::ModulesRom() const
{
    return info.unpacked ? *info.unpacked : rom;
}

std::optional<int> KrawallRom::SampleOf(uint32_t sound, uint16_t instrument, uint8_t note) const
{
    const std::optional<krawall::ModuleInfo> module = ModuleOf(*this, sound);
    if (!module)
    {
        return std::nullopt;
    }

    const std::optional<krawall::Lookup> found =
        krawall::LookUpInstrument(ModulesRom(), info, *module, instrument, note);
    if (!found || found->sample < 0)
    {
        return std::nullopt;
    }

    return found->sample;
}

const DriverRules& KrawallRom::Rules() const
{
    return KrawallRules(krawall::TraitsOf(info).wide ? 1 : 0);
}

std::span<const uint8_t> KrawallRom::Bytes() const
{
    return {rom.Ptr(ntdlib::agb::kRomBase), rom.Size()};
}

std::vector<uint32_t> KrawallRom::Sounds() const
{
    std::vector<uint32_t> playing;
    for (uint32_t sound = 0; sound < SoundCount(); ++sound)
    {
        if (audition ? static_cast<int>(sound) == audition->song : ModuleOf(*this, sound).has_value())
        {
            playing.push_back(sound);
        }
    }

    return playing;
}

uint32_t KrawallRom::SoundCount() const
{
    return static_cast<uint32_t>(info.songs.size());
}

std::string KrawallRom::SoundName(uint32_t sound) const
{
    return "Song " + ntdlib::sequence_export::agb::SongNumber(static_cast<int>(sound), static_cast<int>(SoundCount()));
}

SoundFields KrawallRom::FieldsOf(uint32_t sound) const
{
    // The first entry of the module's table of patterns names the sequence. The modified build keeps the modules in its
    // packed music files. No field of the ROM names them.
    SoundFields fields;
    const std::optional<krawall::ModuleInfo> module = ModuleOf(*this, sound);
    if (!module || info.unpacked || module->patterns.empty())
    {
        return fields;
    }

    fields.sequence = uint64_t{info.songs[sound].module} + krawall::ModulePatterns(info.build) - ntdlib::agb::kRomBase;

    return fields;
}

int KrawallRom::Rate() const
{
    // The sound bias register's default resolution: 9 bits at 32,768 Hz (ntdlib::krawall::ArchivePlayer).
    return 32768;
}

std::vector<int> KrawallRom::Samples() const
{
    return samples;
}

std::string KrawallRom::SampleName(int sample) const
{
    std::array<char, 16> name{};
    std::snprintf(name.data(), name.size(), "Sample %03d", sample);
    return name.data();
}

double KrawallRom::SampleRate(int sample) const
{
    return ntdlib::sequence_export::krawall::SampleRate(ModulesRom(), info, song::SampleOf(*this, sample).header);
}

std::vector<uint8_t> KrawallRom::SampleWav(int sample) const
{
    // The points are 8-bit and unsigned, with 0x80 as silence.
    const krawall::Sample read = song::SampleOf(*this, sample);
    const uint32_t length = read.end - read.data;

    // The loop is the one that ShapeOf returns. seqlib::Pcm supports forward loops only. A loop back and forth is
    // stored as a forward loop over the same points.
    const std::optional<uint32_t> loop_start = ShapeOf(sample).loop_start;
    const std::optional<WavLoop> loop = loop_start ? std::optional(WavLoop{*loop_start, length}) : std::nullopt;
    const uint32_t rate = std::max<uint32_t>(static_cast<uint32_t>(SampleRate(sample)), 1);
    return Pcm8Wav(ModulesRom(), read.data, length, true, loop, rate);
}

SampleShape KrawallRom::ShapeOf(int sample) const
{
    const krawall::Sample read = song::SampleOf(*this, sample);
    const uint32_t length = read.end - read.data;
    if (read.loop == 0 || read.loop_length == 0 || read.loop_length > length)
    {
        return {length, std::nullopt};
    }

    return {length, length - read.loop_length};
}

std::vector<int> KrawallRom::SamplesOfSound(uint32_t sound) const
{
    const std::set<int> played = SamplesOf(*this, sound);
    return {played.begin(), played.end()};
}

Sequence KrawallRom::ReadSequence(uint32_t sound) const
{
    return song::ReadSequence(*this, sound);
}

ntdlib::sequence_export::Conversion KrawallRom::ConvertToMidi(uint32_t sound, const std::string& program) const
{
    if (sound >= SoundCount())
    {
        throw std::runtime_error("The ROM has no song " + std::to_string(sound) + ".");
    }

    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.program = program;
    ntdlib::sequence_export::krawall::SongFiles converted =
        ntdlib::sequence_export::krawall::ConvertSongFiles(ModulesRom(), info, static_cast<int>(sound), settings);

    return AgbConversion(SoundName(sound), std::move(converted));
}

ntdlib::sequence_export::BankConversion KrawallRom::ConvertSamples(std::span<const int> numbers,
                                                                   const std::string& program) const
{
    // The converter's set numbers its programs in the order that they come. Each gets the number of the sample here.
    ntdlib::sequence_export::krawall::InstrumentSet set;
    ntdlib::sequence_export::BankConversion conversion;
    std::map<int, int> numbered;
    for (const int sample : numbers)
    {
        try
        {
            const krawall::Sample read = song::SampleOf(*this, sample);
            numbered.emplace(set.Program(read.header, sample, 0), sample);
        }
        catch (const std::invalid_argument&)
        {
            conversion.warnings.push_back(SampleName(sample) + " holds no samples.");
        }
    }

    ntdlib::sequence_export::agb::Sf2File file = set.Build(ModulesRom(), info);
    for (auto& preset : file.presets)
    {
        const auto found = numbered.find(preset.bank * 128 + preset.program);
        if (found != numbered.end())
        {
            preset.name = SampleName(found->second);
            preset.bank = static_cast<uint16_t>(found->second / 128);
            preset.program = static_cast<uint16_t>(found->second % 128);
        }
    }

    return SamplesConversion(std::move(conversion), file, numbers, program, rom.Title());
}

std::unique_ptr<DriverPlayer> KrawallRom::MakePlayer() const
{
    return std::make_unique<KrawallPlayer>(*this);
}

DriverAudition KrawallRom::SampleAudition(int sample, int key, int) const
{
    // A piece whose module names an instrument that plays the sample at one of its notes. Key 60 plays the sample's C-4
    // at its rate: the row's note less the sample's relative note.
    const krawall::Sample read = song::SampleOf(*this, sample);
    for (const uint32_t sound : Sounds())
    {
        const std::optional<krawall::ModuleInfo> module = ModuleOf(*this, sound);
        if (!module)
        {
            continue;
        }

        for (const uint16_t instrument : InstrumentsOf(*this, sound))
        {
            const std::optional<krawall::Instrument> found =
                krawall::ReadInstrument(ModulesRom(), info, *module, instrument);
            for (const krawall::Region& region : found ? found->regions : std::vector<krawall::Region>())
            {
                if (region.sample != sample)
                {
                    continue;
                }

                const int note =
                    std::clamp(kRootRowNote + key - kRootKey - read.relative_note, region.first, region.last);
                return Audition(sound, instrument, note);
            }
        }
    }

    throw std::invalid_argument("No piece of the ROM plays " + SampleName(sample) + ".");
}

std::optional<DriverAudition> KrawallRom::NoteAudition(const ntdlib::commands::Sequence& commands,
                                                       const Sequence& sequence, int track, int program, int key,
                                                       int) const
{
    // A note of the track that plays the sample: the instrument of its row, and the distance of the key from the key of
    // the note.
    const auto found = std::ranges::find_if(sequence.tracks, [track](const Track& t) { return t.number == track; });
    if (found == sequence.tracks.end())
    {
        return std::nullopt;
    }

    try
    {
        const std::vector<uint32_t> offsets = Rules().ItemOffsets(commands);
        for (const Note& note : found->notes)
        {
            if (static_cast<int>(note.program) != program)
            {
                continue;
            }

            const auto item = std::ranges::find(offsets, note.command);
            if (item == offsets.end() || item + 1 == offsets.end())
            {
                continue;
            }

            const Command& command = commands.items[static_cast<std::size_t>(item - offsets.begin())];
            if (command.kind != Kind::kNote || command.sample.value_or(0) == 0)
            {
                continue;
            }

            return Audition(sequence.sound, *command.sample, command.key + key - static_cast<int>(note.key));
        }
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }

    return std::nullopt;
}

std::shared_ptr<const DriverRom> KrawallRom::AuditionRom(const DriverAudition& audition) const
{
    // The setup's first command names the piece, and its second the instrument.
    if (audition.setup.size() < 2)
    {
        throw std::invalid_argument("The note names no instrument of a piece.");
    }

    const auto sound = static_cast<uint32_t>(audition.setup[0].value.value / kOne);
    if (!ModuleOf(*this, sound))
    {
        throw std::invalid_argument("The note names no piece of the ROM.");
    }
    if (audition.note.key < 1 || audition.note.key > krawall::kInstrumentNotes)
    {
        throw std::invalid_argument("Krawall can't play this key.");
    }

    auto copy = std::make_shared<KrawallRom>(*this);
    copy->audition = AuditionNote{
        .song = static_cast<int>(sound),
        .instrument = static_cast<uint16_t>(std::clamp<int64_t>(audition.setup[1].value.value / kOne, 1, 0xFFFF)),
        .note = audition.note.key};

    return copy;
}

std::shared_ptr<const KrawallRom> LoadKrawallRom(std::vector<uint8_t> bytes)
{
    auto rom = std::make_shared<KrawallRom>();
    rom->rom.Assign(std::move(bytes));
    std::string error;
    if (!krawall::DetectDriver(rom->rom, {}, rom->info, error))
    {
        throw xsflib::FormatError(error.empty() ? "the ROM shows no sign of Krawall"
                                                : "Can't read Krawall's tables: " + error);
    }
    if (rom->Sounds().empty())
    {
        throw xsflib::FormatError("Krawall's tables hold no module that is in the ROM");
    }

    // The samples that the modules play, once each.
    std::set<int> samples;
    for (const uint32_t sound : rom->Sounds())
    {
        samples.merge(SamplesOf(*rom, sound));
    }

    rom->samples.assign(samples.begin(), samples.end());

    return rom;
}

} // namespace hokuto::song
