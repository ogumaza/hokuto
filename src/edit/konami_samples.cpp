// SPDX-License-Identifier: MIT

// The edits of the samples of Konami's GBA driver (see konami_samples.h).

#include "edit/konami_samples.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/layout.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/rounding.h"
#include "xsflib/bytes.h"

#include "edit/files.h"
#include "edit/gba_voices.h"
#include "edit/native_runs.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/konami.h"

namespace hokuto::edit
{
namespace
{

namespace konami = ntdlib::konami;

// The least and the largest step of a header that the edits write. The model of the driver refuses a step below 0x40 in
// a header. A voice keeps the low 16 bits of the step at its pitch, and a larger step at semitone 0 would wrap.
constexpr uint32_t kLeastStep = 0x40;
constexpr uint32_t kMostStep = 0xFFFF;

// The least and the largest period of an entry of the Dungeon Dice Monsters revision, in CPU cycles. The driver's
// model reads none outside them.
constexpr uint32_t kLeastPeriod = 0x100;
constexpr uint32_t kMostPeriod = 0x4000;

// The samples of a block in the Dungeon Dice Monsters revision: an entry contains the length in blocks, in 12 bits.
constexpr uint32_t kDungeonBlock = 16;
constexpr uint32_t kMostDungeonBlocks = 0xFFF;

// The bytes of a header before its PCM.
constexpr uint32_t kHeaderBytes = 12;

// The zeros after a new sample's PCM. The mixer moves on, then reads, and it reads past a sample's end before the note
// ends or loops: a sample for each step of 4,096 at the note's pitch. A step below 0x10000 reads 16 bytes past it at
// most at semitone 0. The games' samples are followed by zeros or by the next sample's header.
constexpr uint32_t kTailBytes = 16;

bool Dungeon(const song::KonamiRom& rom)
{
    return rom.info.revision == konami::Revision::kDungeonDiceMonsters;
}

// The address of sample `sample`'s entry in its table. A sample of a live stage's table has bit 15 set
// (ntdlib::konami::DriverInfo::Sample).
uint32_t EntryOf(const song::KonamiRom& rom, int sample)
{
    if (sample >= 0x8000 && rom.info.live_sample_table)
    {
        return rom.info.live_sample_table + 4 * static_cast<uint32_t>(sample & 0xFFF);
    }

    return rom.info.sample_table + (Dungeon(rom) ? 8 : 4) * static_cast<uint32_t>(sample);
}

// The header of sample `sample`. Throws std::invalid_argument with a message for the user when the ROM has none.
konami::SampleInfo HeaderOf(const song::KonamiRom& rom, int sample)
{
    const konami::SampleInfo info = rom.info.Sample(rom.rom, sample);
    if (sample < 0 || !info.valid)
    {
        throw std::invalid_argument("The ROM has no " + song::KonamiSampleName(sample) + ".");
    }

    return info;
}

// The step or the period of `rate`, refused outside KonamiSampleRates.
uint32_t StepOf(const song::KonamiRom& rom, uint32_t rate)
{
    const KonamiRates rates = KonamiSampleRates(rom);
    if (rate < rates.least || rate > rates.most)
    {
        throw std::invalid_argument("The rate must be from " + std::to_string(rates.least) + " to " +
                                    std::to_string(rates.most) + " Hz.");
    }

    if (Dungeon(rom))
    {
        const auto period = static_cast<uint32_t>(std::lround(static_cast<double>(ntdlib::agb::kSecondCycles) / rate));
        return std::clamp(period, kLeastPeriod, kMostPeriod);
    }

    const auto step = static_cast<uint32_t>(std::lround(rate * 4096.0 / rom.info.mix_rate));
    return std::clamp(step, kLeastStep, kMostStep);
}

std::array<uint8_t, 4> Word(uint32_t value)
{
    std::array<uint8_t, 4> word;
    xsflib::StoreLe32(word.data(), value);

    return word;
}

uint64_t OffsetOf(uint32_t address)
{
    return uint64_t{address} - ntdlib::agb::kRomBase;
}

// `file` with `bytes` in the free space at the ROM's end (format::NativeDriver::free_start), on a multiple of 4 bytes,
// and the free space after them, with the bytes' address. The last run grows, in the driver's padding, up to them.
// Throws std::invalid_argument with a message for the user for a ROM without room in its 32 MiB.
PlacedGbaBytes WithNewBytes(const format::File& file, std::span<const uint8_t> bytes)
{
    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    if (pieces.empty() || !std::holds_alternative<std::vector<uint8_t>>(pieces.back()))
    {
        throw std::invalid_argument("The ROM ends inside a song. There's no room for new bytes.");
    }

    const format::NativeDriver& driver = *file.native->driver;
    const uint64_t start = ntdlib::AlignUp<uint64_t>(driver.free_start, 4);
    const uint64_t end = start + bytes.size();
    if (end > konami::kLargestRom)
    {
        throw std::invalid_argument("No room for " + std::to_string(bytes.size()) +
                                    " more bytes. The ROM can't exceed 32 MiB.");
    }

    const std::vector<uint64_t> starts = format::NativePieceStarts(file);
    const uint64_t run_start = starts[pieces.size() - 1];
    if (start < run_start)
    {
        throw std::invalid_argument("The free space of the ROM starts before its last run.");
    }

    format::File result = WithLastRunBytes(file, static_cast<std::size_t>(start - run_start), bytes, driver.padding);
    result.native.Edit(
        [end](format::NativeArchive& native)
        {
            native.driver->free_start = static_cast<uint32_t>(ntdlib::AlignUp<uint64_t>(end, konami::kFreeSpaceMargin));
        });

    return {result, static_cast<uint32_t>(start + ntdlib::agb::kRomBase)};
}

void CheckFile(const format::File& file)
{
    if (!file.native || !file.native->driver || file.native->engine != format::Engine::kKonami)
    {
        throw std::invalid_argument("The file doesn't use Konami's driver.");
    }
}

} // namespace

KonamiRates KonamiSampleRates(const song::KonamiRom& rom)
{
    if (Dungeon(rom))
    {
        const auto cycles = static_cast<double>(ntdlib::agb::kSecondCycles);
        return {static_cast<uint32_t>(std::ceil(cycles / kMostPeriod)),
                static_cast<uint32_t>(std::floor(cycles / kLeastPeriod)), false};
    }

    const double mix = rom.info.mix_rate;
    return {static_cast<uint32_t>(std::ceil(mix * kLeastStep / 4096.0)),
            static_cast<uint32_t>(std::floor(mix * kMostStep / 4096.0))};
}

bool KonamiSampleLoops(const song::KonamiRom& rom, int sample)
{
    return !Dungeon(rom) && !rom.info.Sample(rom.rom, sample).packed;
}

std::string KonamiNoLoopReason(const song::KonamiRom& rom, int sample)
{
    if (KonamiSampleLoops(rom, sample))
    {
        return {};
    }

    return Dungeon(rom) ? "Samples of this driver can't loop." : "A packed sample can't loop.";
}

std::vector<int> KonamiSamplesSharing(const song::KonamiRom& rom, int sample, std::span<const int> samples)
{
    const konami::SampleInfo info = rom.info.Sample(rom.rom, sample);
    std::vector<int> sharing;
    if (!info.valid || Dungeon(rom))
    {
        return sharing;
    }

    for (const int other : samples)
    {
        if (other != sample && rom.info.Sample(rom.rom, other).valid &&
            rom.info.Sample(rom.rom, other).data == info.data)
        {
            sharing.push_back(other);
        }
    }

    return sharing;
}

format::File WithKonamiSampleValues(const format::File& file, const song::KonamiRom& rom, int sample,
                                    const KonamiSampleValues& values)
{
    CheckFile(file);
    const konami::SampleInfo info = HeaderOf(rom, sample);
    const uint32_t step = StepOf(rom, values.rate);
    if (values.loop_start && !KonamiSampleLoops(rom, sample))
    {
        throw std::invalid_argument(KonamiNoLoopReason(rom, sample));
    }
    if (values.loop_start && *values.loop_start >= static_cast<uint32_t>(info.length))
    {
        throw std::invalid_argument("The loop must start before the end of the sample, at sample " +
                                    std::to_string(info.length - 1) + " at most.");
    }

    const std::vector<uint64_t> starts = format::NativePieceStarts(file);
    const uint32_t entry = EntryOf(rom, sample);
    if (Dungeon(rom))
    {
        const uint32_t word = (rom.rom.U32(entry + 4) & 0xFFFF0000) | step;
        return WithArchiveBytes(file, starts, OffsetOf(entry + 4), Word(word), "The sample table");
    }

    // The step, then the loop start: -1 for none.
    const uint32_t header = rom.rom.U32(entry);
    const uint32_t loop = values.loop_start ? *values.loop_start : 0xFFFFFFFF;
    const std::array<uint8_t, 4> step_bytes = Word(step);
    const std::array<uint8_t, 4> loop_bytes = Word(loop);
    const std::array<ArchiveBytes, 2> changes{
        ArchiveBytes{OffsetOf(header), {step_bytes.begin(), step_bytes.end()}},
        ArchiveBytes{OffsetOf(header + 8), {loop_bytes.begin(), loop_bytes.end()}}};

    return WithArchiveBytes(file, starts, changes, "The sample's header");
}

IncomingKonamiSample ReadIncomingKonamiSample(std::span<const uint8_t> bytes)
{
    // The MP2K header contains the rate in 1/1024 Hz, the loop and the length (edit::EncodeGbaSample), and the samples
    // follow it.
    const IncomingWave wave = ReadIncomingGbaSample(bytes);
    constexpr std::size_t kMp2kHeader = 16;
    if (wave.bytes.size() < kMp2kHeader + wave.frames)
    {
        throw std::invalid_argument("Can't read the samples of this file.");
    }

    IncomingKonamiSample incoming;
    incoming.pcm.resize(wave.frames);
    std::ranges::transform(std::span(wave.bytes).subspan(kMp2kHeader, wave.frames), incoming.pcm.begin(),
                           [](uint8_t value) { return static_cast<int8_t>(value); });
    incoming.rate = wave.rate;
    if (wave.loop)
    {
        incoming.loop_start = wave.loop_start;
    }

    incoming.signal_to_noise = wave.signal_to_noise;

    return incoming;
}

format::File WithKonamiSample(const format::File& file, const song::KonamiRom& rom, int sample,
                              const IncomingKonamiSample& incoming)
{
    CheckFile(file);
    HeaderOf(rom, sample);
    if (incoming.pcm.empty())
    {
        throw std::invalid_argument("The file holds no samples.");
    }

    const uint32_t step = StepOf(rom, incoming.rate);
    const auto length = static_cast<uint32_t>(incoming.pcm.size());
    const uint32_t entry = EntryOf(rom, sample);
    const std::vector<uint64_t> starts = format::NativePieceStarts(file);

    if (Dungeon(rom))
    {
        if (incoming.loop_start)
        {
            throw std::invalid_argument("Samples of this driver can't loop. Remove the loop from the WAV file.");
        }

        const uint32_t blocks = (length + kDungeonBlock - 1) / kDungeonBlock;
        if (blocks > kMostDungeonBlocks)
        {
            throw std::invalid_argument("The sample is too long. This driver plays " +
                                        std::to_string(kMostDungeonBlocks * kDungeonBlock) + " samples at most.");
        }

        std::vector<uint8_t> pcm(blocks * kDungeonBlock + kTailBytes, 0);
        std::ranges::transform(incoming.pcm, pcm.begin(), [](int8_t value) { return static_cast<uint8_t>(value); });
        const PlacedGbaBytes placed = WithNewBytes(file, pcm);
        const uint32_t word = blocks << 20 | (rom.rom.U32(entry + 4) & 0x000F0000) | step;
        std::array<uint8_t, 8> bytes{};
        std::ranges::copy(Word(placed.address), bytes.begin());
        std::ranges::copy(Word(word), bytes.begin() + 4);
        return WithArchiveBytes(placed.file, format::NativePieceStarts(placed.file), OffsetOf(entry), bytes,
                                "The sample table");
    }

    if (incoming.loop_start && *incoming.loop_start >= length)
    {
        throw std::invalid_argument("The loop must start before the end of the sample.");
    }
    if (length > 0x1000000)
    {
        throw std::invalid_argument("The sample is too long. The driver plays 16,777,216 samples at most.");
    }

    // The header, then the PCM.
    std::vector<uint8_t> bytes;
    bytes.reserve(kHeaderBytes + length + kTailBytes);
    const uint32_t loop = incoming.loop_start ? *incoming.loop_start : 0xFFFFFFFF;
    for (const uint32_t word : {step, length, loop})
    {
        const std::array<uint8_t, 4> held = Word(word);
        bytes.insert(bytes.end(), held.begin(), held.end());
    }

    std::ranges::transform(incoming.pcm, std::back_inserter(bytes),
                           [](int8_t value) { return static_cast<uint8_t>(value); });
    bytes.resize(bytes.size() + kTailBytes, 0);
    const PlacedGbaBytes placed = WithNewBytes(file, bytes);

    return WithArchiveBytes(placed.file, format::NativePieceStarts(placed.file), OffsetOf(entry), Word(placed.address),
                            "The sample table");
}

} // namespace hokuto::edit
