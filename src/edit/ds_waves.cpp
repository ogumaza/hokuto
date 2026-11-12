// SPDX-License-Identifier: MIT

#include "edit/ds_waves.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/sar/encodings.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "edit/ds_files.h"
#include "edit/native_runs.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/ds.h"

namespace hokuto::edit
{
namespace
{

namespace sdat = ntdlib::sdat;

// A wave's parameters: the encoding, the loop's switch, the rate, the timer, the loop's start and its length.
constexpr std::size_t kParamBytes = 12;

// The most words of the start of a loop: its field has 16 bits.
constexpr uint32_t kMostLoopStart = 0xFFFF;

// A wave archive's file: its ID, and the file as ntdlib reads it.
struct FoundWave
{
    uint32_t file_id = 0;
    sdat::WaveArchive parsed;
};

// The file of wave archive `wave_archive` of `archive`, with wave `wave` in it. Throws std::runtime_error with a
// message for the user for a wave archive that is missing from the archive, a file that can't be read, and a wave past
// its waves.
FoundWave FindWave(const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    const std::string name = song::DsWaveArchiveName(archive, wave_archive);
    const sdat::Archive& sdat_archive = archive.archive;
    if (wave_archive >= sdat_archive.wave_archives.size() || !sdat_archive.wave_archives[wave_archive])
    {
        throw std::runtime_error("The archive has no " + name + ".");
    }

    FoundWave found;
    found.file_id = sdat_archive.wave_archives[wave_archive]->file_id & sdat::kFileIdBits;
    try
    {
        found.parsed = sdat::WaveArchive::Parse(sdat_archive.File(found.file_id));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error("Can't read " + name + ": " + error.what() + ".");
    }

    if (wave >= found.parsed.waves.size())
    {
        throw std::runtime_error(name + " has no wave " + std::to_string(wave) + ".");
    }

    return found;
}

// The bytes of each wave of `parsed`, a wave archive whose file is `bytes`: from its offset up to the offset of the
// next wave, or the end of the file.
std::vector<std::vector<uint8_t>> WaveBytesOf(const sdat::WaveArchive& parsed, std::span<const uint8_t> bytes)
{
    std::vector<uint32_t> ordered = parsed.offsets;
    std::sort(ordered.begin(), ordered.end());
    std::vector<std::vector<uint8_t>> waves;
    for (const uint32_t offset : parsed.offsets)
    {
        const auto next = std::upper_bound(ordered.begin(), ordered.end(), offset);
        const std::size_t end = next == ordered.end() ? bytes.size() : *next;
        waves.emplace_back(bytes.begin() + offset, bytes.begin() + static_cast<std::ptrdiff_t>(end));
    }

    return waves;
}

// Writes a wave's rate and timer into its bytes.
void SetRateAndTimer(std::span<uint8_t> wave, uint16_t rate, uint16_t timer)
{
    xsflib::StoreLe16(&wave[2], rate);
    xsflib::StoreLe16(&wave[4], timer);
}

} // namespace

void CheckDsRate(uint32_t rate, std::string_view console)
{
    if (rate < kLeastDsRate || rate > kHighestDsRate)
    {
        throw std::invalid_argument("The sample rate of a " + std::string(console) + " wave must be from " +
                                    std::to_string(kLeastDsRate) + " to " + std::to_string(kHighestDsRate) + " Hz.");
    }
}

std::vector<uint8_t> EncodeDsWave(std::span<const int16_t> samples, uint32_t rate, sdat::WaveEncoding encoding,
                                  std::optional<uint32_t> loop_start)
{
    CheckDsRate(rate, "DS");

    std::vector<uint8_t> bytes =
        sdat::EncodeWave(samples, static_cast<uint16_t>(std::min<uint32_t>(rate, 0xFFFF)), encoding, loop_start);
    if (rate > 0xFFFF)
    {
        SetRateAndTimer(bytes, static_cast<uint16_t>(rate & 0xFFFF), static_cast<uint16_t>(sdat::kTimerClockHz / rate));
    }

    return bytes;
}

bool SameDsTimer(uint32_t first, uint32_t second)
{
    return first != 0 && second != 0 ? sdat::kTimerClockHz / first == sdat::kTimerClockHz / second : first == second;
}

DsWaveValues DsWaveValuesOf(const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    const FoundWave found = FindWave(archive, wave_archive, wave);
    const sdat::Wave& present = found.parsed.waves[wave];
    DsWaveValues values;
    values.rate = song::DsWaveRate(present);
    values.loop = present.loop != 0;
    values.loop_start = values.loop ? present.LoopStartSample() : 0;

    return values;
}

format::File WithDsWave(const format::File& file, const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave,
                        const DsWaveValues& values)
{
    if (!file.native || file.native->engine != format::Engine::kDs)
    {
        throw std::invalid_argument("The file contains no DS sound archive.");
    }

    return WithDsWave(file, archive, wave_archive, wave, values, format::NativePieceStarts(file));
}

format::File WithDsWave(const format::File& file, const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave,
                        const DsWaveValues& values, std::span<const uint64_t> starts)
{
    if (!file.native || file.native->engine != format::Engine::kDs)
    {
        throw std::invalid_argument("The file contains no DS sound archive.");
    }

    FoundWave found;
    try
    {
        found = FindWave(archive, wave_archive, wave);
    }
    catch (const std::runtime_error& error)
    {
        throw std::invalid_argument(error.what());
    }

    const sdat::Wave& present = found.parsed.waves[wave];
    const bool same_rate = values.rate == song::DsWaveRate(present);
    if (!same_rate)
    {
        CheckDsRate(values.rate, "DS");
    }

    // A new encoding makes the wave again from its samples. The same rate keeps the wave's rate and timer.
    if (values.encoding && *values.encoding != present.encoding)
    {
        const std::vector<int16_t> samples = sdat::DecodeWave(present);
        if (values.loop && values.loop_start >= samples.size())
        {
            throw std::invalid_argument("The loop must start before the end of the wave.");
        }

        std::vector<uint8_t> bytes;
        try
        {
            bytes = EncodeDsWave(samples, same_rate ? kLeastDsRate : values.rate, *values.encoding,
                                 values.loop ? std::optional<uint32_t>(values.loop_start) : std::nullopt);
        }
        catch (const std::invalid_argument& error)
        {
            throw std::invalid_argument(std::string("Can't encode the wave for the DS: ") + error.what() + ".");
        }

        if (same_rate)
        {
            SetRateAndTimer(bytes, present.sample_rate, present.timer);
        }

        return WithDsWaveBytes(file, archive, wave_archive, wave, bytes);
    }

    // The loop's start in words. The words of the samples stay, and the loop runs to their end.
    const uint32_t step = sdat::SamplesPerWord(present.encoding);
    const uint32_t header = sdat::HeaderWords(present.encoding);
    const uint64_t words = uint64_t{present.loop_start} + present.loop_length;
    uint32_t start_words = header;
    if (values.loop)
    {
        if (values.loop_start % step != 0)
        {
            throw std::invalid_argument("The loop start of this wave must be a multiple of " + std::to_string(step) +
                                        " samples.");
        }

        start_words = header + values.loop_start / step;
        if (start_words >= words)
        {
            throw std::invalid_argument("The loop must start before the end of the wave.");
        }
        if (start_words > kMostLoopStart)
        {
            throw std::invalid_argument("The loop of a DS wave must start within the first 65,535 words.");
        }
    }
    if (start_words > words)
    {
        throw std::invalid_argument("The wave has no samples.");
    }

    // The parameters, with the encoding as it was. The same rate keeps the wave's rate and timer.
    const uint16_t rate = same_rate ? present.sample_rate : static_cast<uint16_t>(values.rate & 0xFFFF);
    const uint16_t timer = same_rate ? present.timer : static_cast<uint16_t>(sdat::kTimerClockHz / values.rate);
    const auto length = static_cast<uint32_t>(words - start_words);
    const std::array<uint8_t, kParamBytes> bytes = {
        static_cast<uint8_t>(present.encoding),      static_cast<uint8_t>(values.loop ? 1 : 0),
        static_cast<uint8_t>(rate & 0xFF),           static_cast<uint8_t>(rate >> 8),
        static_cast<uint8_t>(timer & 0xFF),          static_cast<uint8_t>(timer >> 8),
        static_cast<uint8_t>(start_words & 0xFF),    static_cast<uint8_t>(start_words >> 8),
        static_cast<uint8_t>(length & 0xFF),         static_cast<uint8_t>((length >> 8) & 0xFF),
        static_cast<uint8_t>((length >> 16) & 0xFF), static_cast<uint8_t>(length >> 24)};

    // The wave archive's file starts at its offset in the archive, and the wave at its offset in the file.
    const uint64_t offset = archive.archive.files[found.file_id].offset + uint64_t{found.parsed.offsets[wave]};
    return WithArchiveBytes(file, starts, offset, bytes, "The wave");
}

namespace
{

// The waves of a wave archive's file, each with its parameters. Only a file that the layout of the tools reproduces can
// accept a wave of another size: the other waves then stay.
struct LaidOutWaves
{
    uint32_t file_id = 0;
    std::vector<std::vector<uint8_t>> waves;
};

LaidOutWaves LaidOutWavesOf(const song::DsArchive& archive, uint32_t wave_archive)
{
    const std::string name = song::DsWaveArchiveName(archive, wave_archive);
    const sdat::Archive& sdat_archive = archive.archive;
    if (wave_archive >= sdat_archive.wave_archives.size() || !sdat_archive.wave_archives[wave_archive])
    {
        throw std::invalid_argument("The archive has no " + name + ".");
    }

    const uint32_t file_id = sdat_archive.wave_archives[wave_archive]->file_id & sdat::kFileIdBits;

    sdat::WaveArchive parsed;
    try
    {
        parsed = sdat::WaveArchive::Parse(sdat_archive.File(file_id));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument("Can't read " + name + ": " + error.what() + ".");
    }

    std::vector<std::vector<uint8_t>> waves = WaveBytesOf(parsed, sdat_archive.File(file_id));
    if (!std::ranges::equal(sdat::WaveArchiveFile(waves), sdat_archive.File(file_id)))
    {
        throw std::invalid_argument(name +
                                    " doesn't use the standard layout. Hokuto can't change the size of its waves.");
    }

    return {file_id, std::move(waves)};
}

} // namespace

format::File WithDsWaveBytes(const format::File& file, const song::DsArchive& archive, uint32_t wave_archive,
                             uint32_t wave, std::span<const uint8_t> bytes)
{
    LaidOutWaves laid_out = LaidOutWavesOf(archive, wave_archive);
    if (wave > laid_out.waves.size())
    {
        throw std::invalid_argument(song::DsWaveArchiveName(archive, wave_archive) + " has no wave " +
                                    std::to_string(wave) + ".");
    }

    if (wave == laid_out.waves.size())
    {
        laid_out.waves.emplace_back(bytes.begin(), bytes.end());
    }
    else
    {
        laid_out.waves[wave].assign(bytes.begin(), bytes.end());
    }

    return WithDsFile(file, laid_out.file_id, sdat::WaveArchiveFile(laid_out.waves));
}

format::File WithDsNewWaves(const format::File& file, const song::DsArchive& archive, uint32_t wave_archive,
                            std::span<const std::vector<uint8_t>> waves)
{
    LaidOutWaves laid_out = LaidOutWavesOf(archive, wave_archive);
    laid_out.waves.insert(laid_out.waves.end(), waves.begin(), waves.end());

    return WithDsFile(file, laid_out.file_id, sdat::WaveArchiveFile(laid_out.waves));
}

bool IsDsWaveFile(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 4 && std::memcmp(bytes.data(), "SWAV", 4) == 0;
}

WaveChanges ChangesOfDsWave(const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    const FoundWave found = FindWave(archive, wave_archive, wave);
    const sdat::Archive& sdat_archive = archive.archive;
    const sdat::Wave& present = found.parsed.waves[wave];
    WaveChanges changes;
    changes.encoding = ntdlib::sar::EncodingOf(present.encoding);
    changes.channels = 1;
    changes.rate = song::DsWaveRate(present);
    changes.loop = present.loop != 0;
    changes.loop_start = changes.loop ? present.LoopStartSample() : 0;
    changes.loop_end = present.Samples();
    changes.size = WaveBytesOf(found.parsed, sdat_archive.File(found.file_id))[wave].size();

    // The banks whose regions play the wave from a slot that names the wave archive.
    for (uint32_t bank = 0; bank < sdat_archive.banks.size(); ++bank)
    {
        const std::optional<sdat::BankInfo>& info = sdat_archive.banks[bank];
        if (!info)
        {
            continue;
        }

        try
        {
            const sdat::Bank file = sdat::Bank::Parse(sdat_archive.File(info->file_id));
            const auto plays = [&](const sdat::Region& region)
            {
                return region.type == sdat::InstrumentType::kPcm && region.wave == wave && region.wave_archive < 4 &&
                       info->wave_archives[region.wave_archive] == wave_archive;
            };
            const bool playing = std::ranges::any_of(
                file.instruments, [&](const sdat::Instrument& instrument)
                { return plays(instrument.region) || std::ranges::any_of(instrument.regions, plays); });
            if (playing)
            {
                changes.banks.push_back(bank);
            }
        }
        catch (const xsflib::FormatError&)
        {
            // A bank that can't be read plays nothing here.
        }
    }

    return changes;
}

} // namespace hokuto::edit
