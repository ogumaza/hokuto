// SPDX-License-Identifier: MIT

#include "song/gba_samples.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

#include "ntdlib/mp2k/song.h"
#include "seqlib/pcm.h"
#include "seqlib/wav.h"
#include "xsflib/bytes.h"

namespace hokuto::song
{
namespace
{

namespace mp2k = ntdlib::mp2k;

constexpr std::size_t kHeaderBytes = 16;
constexpr uint8_t kLoopFlag = 0x40;
constexpr uint32_t kFrequencyUnits = 1024; // a sample's frequency field contains 1024 times its rate
constexpr uint8_t kBaseNote = 60;
constexpr int kExponentBias = 16383;
constexpr uint16_t kLoopStartMarker = 1;
constexpr uint16_t kLoopEndMarker = 2;

void AppendBe16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t value)
{
    AppendBe16(out, static_cast<uint16_t>(value >> 16));
    AppendBe16(out, static_cast<uint16_t>(value));
}

void AppendChunk(std::vector<uint8_t>& out, std::string_view id, std::span<const uint8_t> body)
{
    out.insert(out.end(), id.begin(), id.end());
    AppendBe32(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() % 2 != 0)
    {
        out.push_back(0);
    }
}

// The sample at `address`, read as MP2K's mixer reads it. Throws for a sample that the file's forms can't hold.
mp2k::Wave ReadPcmSample(const GbaRom& rom, uint32_t address)
{
    mp2k::Wave wave;
    if (!mp2k::ReadWave(rom.rom, address, wave))
    {
        throw std::invalid_argument("Can't read the sample.");
    }
    if (wave.type != 0)
    {
        throw std::invalid_argument("The sample is compressed. Hokuto can't export compressed samples.");
    }

    return wave;
}

// The number of data bytes of `wave` in the ROM: its samples and the one past the last, where the ROM contains it.
uint32_t DataBytes(const GbaRom& rom, const mp2k::Wave& wave)
{
    return wave.size + (rom.rom.Contains(wave.Data(), wave.size + 1) ? 1 : 0);
}

// An IEEE 754 number of 80 bits for `units`, a whole number of 1024ths.
std::array<uint8_t, 10> ExtendedOfUnits(uint32_t units)
{
    std::array<uint8_t, 10> bytes{};
    if (units == 0)
    {
        return bytes;
    }

    const auto mantissa = static_cast<uint64_t>(units) << std::countl_zero(uint64_t{units});
    const int exponent = 63 - std::countl_zero(uint64_t{units}) - std::countr_zero(kFrequencyUnits) + kExponentBias;
    bytes[0] = static_cast<uint8_t>(exponent >> 8);
    bytes[1] = static_cast<uint8_t>(exponent);
    for (std::size_t i = 0; i < 8; i++)
    {
        bytes[2 + i] = static_cast<uint8_t>(mantissa >> (56 - 8 * i));
    }

    return bytes;
}

// The whole number of 1024ths nearest the IEEE 754 number of 80 bits at `bytes`. Nothing for a number that a sample's
// frequency field can't hold.
std::optional<uint32_t> UnitsOfExtended(std::span<const uint8_t> bytes)
{
    const int exponent = (bytes[0] & 0x7F) << 8 | bytes[1];
    uint64_t mantissa = 0;
    for (std::size_t i = 0; i < 8; i++)
    {
        mantissa = mantissa << 8 | bytes[2 + i];
    }
    if ((bytes[0] & 0x80) != 0 || mantissa == 0)
    {
        return std::nullopt;
    }

    // The mantissa's top bit stands for 2^(exponent - bias), and the units are 1024 times the number.
    const int shift = exponent - kExponentBias + std::countr_zero(kFrequencyUnits) - 63;
    if (shift >= 0 || shift < -63)
    {
        return std::nullopt;
    }

    const uint64_t units = (mantissa + (uint64_t{1} << (-shift - 1))) >> -shift;
    if (units == 0 || units > UINT32_MAX)
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(units);
}

bool LooksLikeBin(std::span<const uint8_t> bytes)
{
    if (bytes.size() <= kHeaderBytes || bytes[0] != 0 || bytes[1] != 0 || bytes[2] != 0 ||
        (bytes[3] != 0 && bytes[3] != kLoopFlag))
    {
        return false;
    }

    const uint32_t frequency = xsflib::LoadLe32(bytes.data() + 4);
    const uint32_t loop_start = xsflib::LoadLe32(bytes.data() + 8);
    const uint32_t size = xsflib::LoadLe32(bytes.data() + 12);

    return frequency != 0 && size <= bytes.size() - kHeaderBytes && (bytes[3] == 0 || loop_start <= size);
}

bool IsAiff(std::span<const uint8_t> bytes)
{
    const auto is = [bytes](std::size_t at, std::string_view id)
    {
        return std::equal(id.begin(), id.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
    };
    return bytes.size() >= 12 && is(0, "FORM") && is(8, "AIFF");
}

// The bodies of the chunks of an AIFF file that a GBA sample uses.
struct AiffChunkBodies
{
    std::optional<std::span<const uint8_t>> common;
    std::optional<std::span<const uint8_t>> sound;
    std::optional<std::span<const uint8_t>> markers;
    std::optional<std::span<const uint8_t>> instrument;
};

AiffChunkBodies AiffChunks(std::span<const uint8_t> bytes)
{
    AiffChunkBodies chunks;
    for (std::size_t at = 12; at + 8 <= bytes.size();)
    {
        const std::string_view id(reinterpret_cast<const char*>(bytes.data() + at), 4);
        const std::size_t size = std::min<std::size_t>(xsflib::LoadBe32(bytes.data() + at + 4), bytes.size() - at - 8);
        const std::span<const uint8_t> body = bytes.subspan(at + 8, size);
        if (id == "COMM")
        {
            chunks.common = body;
        }
        else if (id == "SSND")
        {
            chunks.sound = body;
        }
        else if (id == "MARK")
        {
            chunks.markers = body;
        }
        else if (id == "INST")
        {
            chunks.instrument = body;
        }

        at += 8 + size + size % 2;
    }

    return chunks;
}

// The sustain loop's markers give the loop. The end marker counts the frames before it.
std::optional<uint32_t> AiffLoopStart(const std::optional<std::span<const uint8_t>>& markers,
                                      const std::optional<std::span<const uint8_t>>& instrument, std::size_t& frames)
{
    std::optional<uint32_t> loop_start;
    if (markers && instrument && instrument->size() >= 14 && xsflib::LoadBe16(instrument->data() + 8) != 0 &&
        markers->size() >= 2)
    {
        const uint16_t start_id = xsflib::LoadBe16(instrument->data() + 10);
        const uint16_t end_id = xsflib::LoadBe16(instrument->data() + 12);
        std::optional<uint32_t> start;
        std::optional<uint32_t> end;
        std::size_t at = 2;
        for (uint16_t i = 0; i < xsflib::LoadBe16(markers->data()) && at + 7 <= markers->size(); i++)
        {
            const uint16_t id = xsflib::LoadBe16(markers->data() + at);
            const uint32_t position = xsflib::LoadBe32(markers->data() + at + 2);
            const std::size_t name = (*markers)[at + 6];
            start = id == start_id ? std::optional<uint32_t>(position) : start;
            end = id == end_id ? std::optional<uint32_t>(position) : end;
            at += 6 + 1 + name + ((name + 1) % 2);
        }
        if (start && end && *start < *end && *end <= frames)
        {
            loop_start = start;
            frames = *end;
        }
    }

    return loop_start;
}

std::vector<uint8_t> SampleOfAiff(std::span<const uint8_t> bytes)
{
    const auto [common, sound, markers, instrument] = AiffChunks(bytes);

    if (!common || common->size() < 18 || !sound || sound->size() < 8)
    {
        throw std::invalid_argument("The AIFF file has no COMM or SSND chunk.");
    }
    if (xsflib::LoadBe16(common->data()) != 1 || xsflib::LoadBe16(common->data() + 6) != 8)
    {
        throw std::invalid_argument(
            "The AIFF file of a GBA sample must be 8-bit mono. To convert other audio, drop a WAV file "
            "instead.");
    }

    const std::optional<uint32_t> units = UnitsOfExtended(common->subspan(8, 10));
    if (!units)
    {
        throw std::invalid_argument("A GBA sample can't use the sample rate of this AIFF file.");
    }

    const uint32_t offset = xsflib::LoadBe32(sound->data());
    std::size_t frames = std::min<std::size_t>(xsflib::LoadBe32(common->data() + 2),
                                               sound->size() - std::min<std::size_t>(8 + offset, sound->size()));
    if (frames < 2)
    {
        throw std::invalid_argument("The AIFF file has too few samples for a GBA sample.");
    }

    const std::optional<uint32_t> loop_start = AiffLoopStart(markers, instrument, frames);

    std::vector<uint8_t> out;
    out.reserve(kHeaderBytes + frames);
    xsflib::AppendLe32(out, loop_start ? uint32_t{kLoopFlag} << 24 : 0);
    xsflib::AppendLe32(out, *units);
    xsflib::AppendLe32(out, loop_start.value_or(0));
    xsflib::AppendLe32(out, static_cast<uint32_t>(frames - 1));
    const std::span<const uint8_t> samples = sound->subspan(8 + offset, frames);
    out.insert(out.end(), samples.begin(), samples.end());

    return out;
}

} // namespace

std::optional<uint32_t> GbaVoiceSample(const GbaRom& rom, const mp2k::Voice& voice)
{
    mp2k::Wave wave;
    if (voice.PsgChannel() != 0 || voice.IsSplit() || (voice.type & mp2k::kVoiceCompressed) != 0 ||
        !mp2k::ReadWave(rom.rom, voice.wave, wave) || wave.type != 0 || wave.size == 0)
    {
        return std::nullopt;
    }

    return voice.wave;
}

std::vector<uint32_t> GbaVoiceGroupSamples(const GbaRom& rom)
{
    std::set<uint32_t> samples;
    const auto add = [&rom, &samples](const mp2k::Voice& voice)
    {
        if (const std::optional<uint32_t> sample = GbaVoiceSample(rom, voice))
        {
            samples.insert(*sample);
        }
    };
    for (const GbaVoiceGroup& group : GbaVoiceGroups(rom))
    {
        for (const int program : GbaListedPrograms(rom, group))
        {
            mp2k::Voice voice;
            if (!mp2k::ReadVoice(rom.rom, group.address + mp2k::kVoiceSize * static_cast<uint32_t>(program), voice))
            {
                continue;
            }

            if (!voice.IsSplit())
            {
                add(voice);
                continue;
            }

            for (int key = 0; key < 128; key++)
            {
                mp2k::Voice inner;
                const uint32_t place = mp2k::SplitVoice(rom.rom, voice, key);
                if (place != 0 && mp2k::ReadVoice(rom.rom, place, inner))
                {
                    add(inner);
                }
            }
        }
    }

    return std::vector<uint32_t>(samples.begin(), samples.end());
}

std::vector<uint8_t> GbaSampleBin(const GbaRom& rom, uint32_t address)
{
    const mp2k::Wave wave = ReadPcmSample(rom, address);
    const uint8_t* const start = rom.rom.Ptr(address);
    return std::vector<uint8_t>(start, start + kHeaderBytes + DataBytes(rom, wave));
}

std::vector<uint8_t> GbaSampleWav(const GbaRom& rom, uint32_t address)
{
    const mp2k::Wave wave = ReadPcmSample(rom, address);
    if (wave.size == 0)
    {
        throw std::invalid_argument("The sample is empty.");
    }

    seqlib::Pcm pcm;
    pcm.channels.emplace_back(wave.size);
    const uint8_t* const data = rom.rom.Ptr(wave.Data());
    for (uint32_t i = 0; i < wave.size; i++)
    {
        pcm.channels[0][i] = static_cast<int16_t>(static_cast<int8_t>(data[i]) * 256);
    }

    if (wave.loops && wave.loop_start < wave.size)
    {
        pcm.loop = true;
        pcm.loop_start = wave.loop_start;
        pcm.loop_end = wave.size;
    }

    return seqlib::WavFile(pcm, std::max<uint32_t>((wave.frequency + 512) / 1024, 1));
}

std::vector<uint8_t> GbaSampleAiff(const GbaRom& rom, uint32_t address)
{
    const mp2k::Wave wave = ReadPcmSample(rom, address);
    const uint32_t frames = DataBytes(rom, wave);
    if (frames == 0)
    {
        throw std::invalid_argument("The sample is empty.");
    }

    std::vector<uint8_t> body;
    body.insert(body.end(), {'A', 'I', 'F', 'F'});

    std::vector<uint8_t> common;
    AppendBe16(common, 1);
    AppendBe32(common, frames);
    AppendBe16(common, 8);
    const std::array<uint8_t, 10> rate = ExtendedOfUnits(wave.frequency);
    common.insert(common.end(), rate.begin(), rate.end());
    AppendChunk(body, "COMM", common);

    if (wave.loops)
    {
        // A marker's name is a counted string that fills a whole number of 16-bit words.
        std::vector<uint8_t> markers;
        AppendBe16(markers, 2);
        AppendBe16(markers, kLoopStartMarker);
        AppendBe32(markers, wave.loop_start);
        markers.insert(markers.end(), {5, 'S', 'T', 'A', 'R', 'T'});
        AppendBe16(markers, kLoopEndMarker);
        AppendBe32(markers, frames);
        markers.insert(markers.end(), {3, 'E', 'N', 'D'});
        AppendChunk(body, "MARK", markers);
    }

    // The base note, no detune, every note and velocity, no gain. A loop is a forward sustain loop.
    std::vector<uint8_t> instrument = {kBaseNote, 0, 0, 127, 1, 127, 0, 0};
    AppendBe16(instrument, wave.loops ? 1 : 0);
    AppendBe16(instrument, wave.loops ? kLoopStartMarker : 0);
    AppendBe16(instrument, wave.loops ? kLoopEndMarker : 0);
    AppendBe16(instrument, 0);
    AppendBe16(instrument, 0);
    AppendBe16(instrument, 0);
    AppendChunk(body, "INST", instrument);

    std::vector<uint8_t> sound(8, 0);
    const uint8_t* const data = rom.rom.Ptr(wave.Data());
    sound.insert(sound.end(), data, data + frames);
    AppendChunk(body, "SSND", sound);

    std::vector<uint8_t> out = {'F', 'O', 'R', 'M'};
    AppendBe32(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());

    return out;
}

bool IsGbaSampleFile(std::span<const uint8_t> bytes)
{
    return IsAiff(bytes) || LooksLikeBin(bytes);
}

std::vector<uint8_t> GbaSampleOfFile(std::span<const uint8_t> bytes)
{
    if (IsAiff(bytes))
    {
        return SampleOfAiff(bytes);
    }
    if (!LooksLikeBin(bytes))
    {
        throw std::invalid_argument("The file is neither an AIFF file nor a raw GBA sample.");
    }

    // The bytes after the samples and the one past them are padding. A file without that one sample gets the sample
    // that the mixer would read: the loop's first, or else the last.
    const uint32_t size = xsflib::LoadLe32(bytes.data() + 12);
    const std::size_t data = std::min<std::size_t>(size + std::size_t{1}, bytes.size() - kHeaderBytes);
    std::vector<uint8_t> out(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + data));
    if (data == size && size > 0)
    {
        const std::size_t repeated = bytes[3] != 0 ? xsflib::LoadLe32(bytes.data() + 8) : size - 1;
        out.push_back(out[kHeaderBytes + std::min<std::size_t>(repeated, size - 1)]);
    }

    return out;
}

} // namespace hokuto::song
