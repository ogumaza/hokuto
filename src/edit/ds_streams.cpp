// SPDX-License-Identifier: MIT

#include "edit/ds_streams.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/sar/encodings.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "edit/ds_files.h"
#include "edit/ds_waves.h"

namespace hokuto::edit
{
namespace
{

namespace sdat = ntdlib::sdat;

// The fields of the HEAD block that an edit of the rate or the loop changes, from the file's start.
constexpr std::size_t kLoopField = 0x19;
constexpr std::size_t kRateField = 0x1C;
constexpr std::size_t kTimerField = 0x1E;
constexpr std::size_t kLoopStartField = 0x20;

void CheckRate(uint32_t rate)
{
    if (rate < kLeastDsRate || rate > kHighestDsStreamRate)
    {
        throw std::invalid_argument("The sample rate of a DS stream must be from 256 to 65,535 Hz.");
    }
}

sdat::Stream Parsed(const song::DsArchive& archive, uint32_t stream)
{
    const uint32_t file_id = DsStreamFileId(archive, stream);
    try
    {
        return sdat::Stream::Parse(archive.archive.File(file_id));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument("Can't read " + song::DsStreamName(archive, stream) + ": " + error.what() + ".");
    }
}

} // namespace

std::vector<uint8_t> EncodeDsStream(std::span<const std::vector<int16_t>> channels, uint32_t rate,
                                    sdat::WaveEncoding encoding, std::optional<uint32_t> loop_start)
{
    CheckRate(rate);
    if (channels.empty() || channels.size() > 2)
    {
        throw std::invalid_argument("A DS stream must have one or two channels.");
    }

    const std::size_t length = channels[0].size();
    if (length == 0 || std::ranges::any_of(channels, [length](const auto& each) { return each.size() != length; }))
    {
        throw std::invalid_argument("The channels of a DS stream must have the same length, with at least one sample.");
    }
    if (loop_start && *loop_start >= length)
    {
        throw std::invalid_argument("The loop must start before the end of the stream.");
    }

    // A loop moves forward to a block. The samples between repeat after the end, and the loop keeps its length.
    const uint32_t block_samples = sdat::StreamBlockSamples(encoding);
    std::size_t total = length;
    if (loop_start)
    {
        const uint32_t start = (*loop_start + block_samples - 1) / block_samples * block_samples;
        total = start + (length - *loop_start);
    }
    if (total > 0xFFFFFFFFu)
    {
        throw std::invalid_argument("Too many samples for a DS stream.");
    }

    return sdat::EncodeStream(channels, static_cast<uint16_t>(rate), encoding, loop_start);
}

uint32_t DsStreamFileId(const song::DsArchive& archive, uint32_t stream)
{
    const auto& streams = archive.archive.streams;
    CheckDsRecord(streams, stream, "stream");

    return streams[stream]->file_id & sdat::kFileIdBits;
}

DsStreamValues DsStreamValuesOf(const song::DsArchive& archive, uint32_t stream)
{
    const sdat::Stream parsed = Parsed(archive, stream);
    DsStreamValues values;
    values.rate = parsed.sample_rate;
    values.loop = parsed.loop != 0 && parsed.loop_start < parsed.samples;
    values.loop_start = values.loop ? parsed.loop_start : 0;

    return values;
}

format::File WithDsStream(const format::File& file, const song::DsArchive& archive, uint32_t stream,
                          const DsStreamValues& values)
{
    const sdat::Stream parsed = Parsed(archive, stream);
    const uint32_t file_id = DsStreamFileId(archive, stream);
    if (values.rate != parsed.sample_rate)
    {
        CheckRate(values.rate);
    }

    // Another encoding makes the stream again from its samples.
    if (values.encoding && *values.encoding != parsed.encoding)
    {
        std::vector<std::vector<int16_t>> channels;
        for (uint32_t channel = 0; channel < parsed.channels; channel++)
        {
            channels.push_back(sdat::DecodeStream(parsed, channel));
        }

        return WithDsFile(file, file_id,
                          EncodeDsStream(channels, values.rate, *values.encoding,
                                         values.loop ? std::optional(values.loop_start) : std::nullopt));
    }

    if (values.loop)
    {
        if (parsed.block_samples == 0 || values.loop_start % parsed.block_samples != 0)
        {
            throw std::invalid_argument("The loop start of this stream must be a multiple of " +
                                        std::to_string(parsed.block_samples) + " samples.");
        }
        if (values.loop_start >= parsed.samples)
        {
            throw std::invalid_argument("The loop must start before the end of the stream.");
        }
    }

    const std::span<const uint8_t> present = archive.archive.File(file_id);
    std::vector<uint8_t> bytes(present.begin(), present.end());
    bytes[kLoopField] = values.loop ? 1 : 0;
    xsflib::StoreLe32(&bytes[kLoopStartField], values.loop ? values.loop_start : 0);
    if (values.rate != parsed.sample_rate)
    {
        xsflib::StoreLe16(&bytes[kRateField], static_cast<uint16_t>(values.rate));
        xsflib::StoreLe16(&bytes[kTimerField],
                          static_cast<uint16_t>(sdat::kTimerClockHz / values.rate / sdat::kStreamTimerDivider));
    }

    return WithDsFile(file, file_id, bytes);
}

format::File WithDsStreamBytes(const format::File& file, const song::DsArchive& archive, uint32_t stream,
                               std::span<const uint8_t> bytes)
{
    ReadDsStream(bytes);

    return WithDsFile(file, DsStreamFileId(archive, stream), bytes);
}

WaveChanges ChangesOfDsStream(const song::DsArchive& archive, uint32_t stream)
{
    const sdat::Stream parsed = Parsed(archive, stream);
    WaveChanges changes;
    changes.encoding = ntdlib::sar::EncodingOf(parsed.encoding);
    changes.channels = parsed.channels;
    changes.rate = parsed.sample_rate;
    changes.loop = parsed.loop != 0;
    changes.loop_start = changes.loop ? parsed.loop_start : 0;
    changes.loop_end = parsed.samples;
    changes.size = archive.archive.File(DsStreamFileId(archive, stream)).size();

    return changes;
}

bool IsDsStreamFile(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 4 && std::memcmp(bytes.data(), "STRM", 4) == 0;
}

} // namespace hokuto::edit
