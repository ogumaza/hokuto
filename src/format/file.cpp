// SPDX-License-Identifier: MIT

#include "format/file.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "format/binary.h"
#include "format/codes.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/file_parts.h"
#include "format/model.h"

namespace hokuto::format
{
namespace
{

using file_parts::BankMapsChunk;
using file_parts::BuildChunk;
using file_parts::Damaged;
using file_parts::EditorChunk;
using file_parts::EngineChunk;
using file_parts::In;
using file_parts::InstrumentMapsChunk;
using file_parts::LengthsChunk;
using file_parts::MixerChunk;
using file_parts::NativeChunk;
using file_parts::Newer;
using file_parts::NotUnity;
using file_parts::Out;
using file_parts::ReadBankMaps;
using file_parts::ReadBuild;
using file_parts::ReadEditor;
using file_parts::ReadEngine;
using file_parts::ReadInstrumentMaps;
using file_parts::ReadLengths;
using file_parts::ReadMixer;
using file_parts::ReadNative;
using file_parts::ReadScenes;
using file_parts::ReadSequences;
using file_parts::ReadSizes;
using file_parts::ReadTables;
using file_parts::ReadTags;
using file_parts::ReadTimelines;
using file_parts::ReadTrackColours;
using file_parts::ScenesChunk;
using file_parts::Sequence;
using file_parts::SequencesChunk;
using file_parts::SizesChunk;
using file_parts::TablesChunk;
using file_parts::TagsChunk;
using file_parts::TimelinesChunk;
using file_parts::TrackColoursChunk;

constexpr std::string_view kEngineChunk = "ENGN";

constexpr std::string_view kMixerChunk = "MIXR";

constexpr std::string_view kSequencesChunk = "SEQS";

constexpr std::string_view kNativeChunk = "NATV";

constexpr std::string_view kBuildChunk = "BULD";

constexpr std::string_view kTablesChunk = "TABL";

constexpr std::string_view kScenesChunk = "SCEN";

constexpr std::string_view kTrackColoursChunk = "TCOL";

constexpr std::string_view kInstrumentMapsChunk = "IMAP";

constexpr std::string_view kBankMapsChunk = "BMAP";

constexpr std::string_view kEditorChunk = "EDTR";

constexpr std::string_view kTagsChunk = "TAGS";

constexpr std::string_view kLengthsChunk = "LNTH";

constexpr std::string_view kSizesChunk = "SIZE";

constexpr std::string_view kTimelinesChunk = "TMLN";

// The flag of a stream that the game lacks, in the STRM chunk.
constexpr uint32_t kHokutoStream = 1;

// The flags of a wave in the WAVE chunk: a wave that loops, and one whose samples follow. A wave holds one or two
// channels.
constexpr uint8_t kLoopingWave = 1;

constexpr uint8_t kWaveSamples = 2;

constexpr std::size_t kMostWaveChannels = 2;

// The header: the signature, the version, the flags and the number of chunks, then an entry for each chunk: its ID, its
// CRC-32, and where it lies. Each chunk starts on a multiple of 8 bytes.
constexpr std::size_t kHeaderSize = 24;

constexpr std::size_t kEntrySize = 24;

constexpr std::size_t kChunkAlignment = 8;

} // namespace

bool IsHokutoChunk(std::string_view id)
{
    return id == kEngineChunk || id == kMixerChunk || id == kSequencesChunk || id == kNativeChunk ||
           id == kBuildChunk || id == kTablesChunk || id == kSizesChunk || id == kGroupFilesChunk ||
           id == kStreamsChunk || id == kScenesChunk || id == kTrackColoursChunk || id == kEditorChunk ||
           id == kTagsChunk || id == kLengthsChunk || id == kTimelinesChunk || id == kSoundsChunk ||
           id == kItemsChunk || id == kBanksChunk || id == kWavesChunk || id == kCartridgeChunk ||
           id == kInstrumentMapsChunk || id == kBankMapsChunk;
}

std::vector<uint8_t> StreamsChunk(const NativeArchive& native)
{
    Out out;
    out.Count(native.streams.size(), "streams");
    for (const auto& [id, stream] : native.streams)
    {
        out.U32(id);
        out.U32(stream.from_game ? 0 : kHokutoStream);
        out.U64(stream.bytes->size());
        out.Bytes(*stream.bytes);
    }

    return out.Take();
}

std::vector<uint8_t> CartridgeChunk(const Cartridge& cartridge)
{
    if (cartridge.image->size() > cartridge.size)
    {
        throw std::invalid_argument("a cartridge image holds more bytes than its size");
    }

    Out out;
    out.U32(cartridge.file);
    out.U32(cartridge.offset);
    out.U32(cartridge.size);
    out.U32(0);
    out.Bytes(*cartridge.image);

    return out.Take();
}

Cartridge ReadCartridge(std::span<const uint8_t> chunk)
{
    In in(chunk, "the CART chunk");
    Cartridge cartridge;
    cartridge.file = in.U32();
    cartridge.offset = in.U32();
    cartridge.size = in.U32();
    if (in.U32() != 0)
    {
        Newer("a cartridge image with a flag");
    }

    const std::span<const uint8_t> image = in.Take(in.Left());
    if (image.size() > cartridge.size)
    {
        Damaged("its cartridge image is larger than its stated size");
    }

    if (!image.empty() && image.back() == 0xFF)
    {
        Damaged("its cartridge image still has the 0xFF bytes at its end");
    }

    cartridge.image = std::vector<uint8_t>(image.begin(), image.end());

    return cartridge;
}

std::vector<uint8_t> BanksChunk(const File& file)
{
    Out out;
    out.Count(file.banks.size(), "banks");
    for (const auto& [id, bank] : file.banks)
    {
        // The header's values, the table of waves and the order of the layout.
        out.U32(id);
        out.U8(CodeOf(bank->flavor));
        out.Zeros(3);
        out.U32(bank->version);
        out.Count(bank->waves.size(), "waves in a bank");
        for (const ntdlib::sar::WaveId& wave : bank->waves)
        {
            out.U32(wave.wave_archive_item);
            out.U32(wave.index);
        }

        out.Count(bank->instruments.size(), "programs in a bank");
        out.Count(bank->order.size(), "programs in a bank");
        for (const uint32_t program : bank->order)
        {
            out.U32(program);
        }

        // Each program's instrument, with its key regions and their velocity regions.
        for (const std::optional<ntdlib::sar::BankFile::Instrument>& instrument : bank->instruments)
        {
            out.U8(instrument ? 1 : 0);
            out.Zeros(3);
            if (!instrument)
            {
                continue;
            }

            out.Count(instrument->key_regions.size(), "key regions");
            for (const ntdlib::sar::BankFile::KeyRegion& keys : instrument->key_regions)
            {
                out.U8(keys.first_key);
                out.U8(keys.last_key);
                out.U16(0);
                out.Count(keys.regions.size(), "velocity regions");
                for (const ntdlib::sar::BankFile::Region& region : keys.regions)
                {
                    const ntdlib::sar::VelocityRegion& values = region.values;
                    out.U8(region.first_velocity);
                    out.U8(region.last_velocity);
                    out.U16(0);
                    out.U32(values.wave_id_index);
                    out.U8(values.original_key);
                    out.U8(values.volume);
                    out.U8(values.pan);
                    out.U8(values.ignore_note_off ? 1 : 0);
                    out.F32(values.pitch);
                    out.U8(values.key_group);
                    out.U8(values.interpolation);
                    out.U16(0);
                    out.U8(values.adshr.attack);
                    out.U8(values.adshr.decay);
                    out.U8(values.adshr.sustain);
                    out.U8(values.adshr.hold);
                    out.U8(values.adshr.release);
                    out.Zeros(3);
                }
            }
        }
    }

    return out.Take();
}

namespace
{

// A key region of an instrument of the BANK chunk.
ntdlib::sar::BankFile::KeyRegion ReadKeyRegion(In& in)
{
    ntdlib::sar::BankFile::KeyRegion keys;
    keys.first_key = in.U8();
    keys.last_key = in.U8();
    in.U16();
    const uint32_t regions = in.U32();
    for (uint32_t r = 0; r < regions; ++r)
    {
        ntdlib::sar::BankFile::Region region;
        ntdlib::sar::VelocityRegion& values = region.values;
        region.first_velocity = in.U8();
        region.last_velocity = in.U8();
        in.U16();
        values.wave_id_index = in.U32();
        values.original_key = in.U8();
        values.volume = in.U8();
        values.pan = in.U8();
        const uint8_t region_flags = in.U8();
        if ((region_flags & ~1u) != 0)
        {
            Newer("a region with a flag");
        }

        values.ignore_note_off = region_flags != 0;
        values.pitch = in.F32();
        values.key_group = in.U8();
        values.interpolation = in.U8();
        in.U16();
        values.adshr = {in.U8(), in.U8(), in.U8(), in.U8(), in.U8()};
        in.Take(3);
        keys.regions.push_back(region);
    }

    return keys;
}

// Bank `id` of the BANK chunk, after its ID.
ntdlib::sar::BankFile ReadBank(In& in, uint32_t id)
{
    const std::optional<ntdlib::sar::Flavor> flavor = FlavorOfCode(in.U8());
    in.Take(3);
    if (!flavor)
    {
        Newer("a bank for a console");
    }

    ntdlib::sar::BankFile bank;
    bank.flavor = *flavor;
    bank.version = in.U32();
    const uint32_t waves = in.U32();
    for (uint32_t w = 0; w < waves; ++w)
    {
        const uint32_t archive = in.U32();
        bank.waves.push_back({archive, in.U32()});
    }

    // Each program takes 4 bytes at least. The chunk has to hold them before they take memory.
    const uint32_t programs = in.U32();
    if (programs > in.Left() / 4)
    {
        Damaged("bank " + std::to_string(id) + " has more programs than its chunk can contain");
    }

    bank.instruments.resize(programs);
    const uint32_t laid = in.U32();
    for (uint32_t p = 0; p < laid; ++p)
    {
        bank.order.push_back(in.U32());
    }

    for (std::optional<ntdlib::sar::BankFile::Instrument>& instrument : bank.instruments)
    {
        const uint8_t flags = in.U8();
        in.Take(3);
        if ((flags & ~1u) != 0)
        {
            Newer("an instrument with a flag");
        }

        if (flags == 0)
        {
            continue;
        }

        instrument.emplace();
        const uint32_t key_regions = in.U32();
        for (uint32_t k = 0; k < key_regions; ++k)
        {
            instrument->key_regions.push_back(ReadKeyRegion(in));
        }
    }

    // The model has to lay a bank out.
    try
    {
        bank.Write();
    }
    catch (const std::invalid_argument& error)
    {
        Damaged("bank " + std::to_string(id) + " can't be laid out: " + error.what());
    }

    return bank;
}

} // namespace

std::map<uint32_t, Shared<ntdlib::sar::BankFile>> ReadBanks(std::span<const uint8_t> chunk)
{
    In in(chunk, "the BANK chunk");
    const uint32_t count = in.U32();
    std::map<uint32_t, Shared<ntdlib::sar::BankFile>> banks;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t id = in.U32();
        if (!banks.empty() && id <= banks.rbegin()->first)
        {
            Damaged("the file IDs of its banks don't rise");
        }

        banks.emplace(id, ReadBank(in, id));
    }

    return banks;
}

std::vector<uint8_t> WavesChunk(const File& file)
{
    Out out;
    out.Count(file.waves.size(), "waves");
    for (const auto& [place, wave] : file.waves)
    {
        if (const std::optional<std::string> problem = WaveModelProblem(*wave))
        {
            throw std::invalid_argument("wave " + std::to_string(place.index) + " of file " +
                                        std::to_string(place.file) + " " + *problem);
        }

        out.U32(place.file);
        out.U32(place.index);
        out.U8(static_cast<uint8_t>(wave->encoding));
        out.U8(static_cast<uint8_t>((wave->loop ? kLoopingWave : 0) | (wave->samples.empty() ? 0 : kWaveSamples)));
        out.U16(0);
        out.U32(wave->rate);
        out.U32(wave->loop_start);
        out.U32(wave->loop_end);
        out.U32(wave->length);
        if (!wave->samples.empty())
        {
            out.U32(static_cast<uint32_t>(wave->samples.size()));
            for (const std::vector<int16_t>& channel : wave->samples)
            {
                for (const int16_t sample : channel)
                {
                    out.U16(static_cast<uint16_t>(sample));
                }
            }
        }
    }

    return out.Take();
}

std::map<WavePlace, Shared<WaveModel>> ReadWaves(std::span<const uint8_t> chunk)
{
    In in(chunk, "the WAVE chunk");
    const uint32_t count = in.U32();
    std::map<WavePlace, Shared<WaveModel>> waves;
    for (uint32_t i = 0; i < count; ++i)
    {
        WavePlace place;
        place.file = in.U32();
        place.index = in.U32();
        if (!waves.empty() && place <= waves.rbegin()->first)
        {
            Damaged("the places of its waves don't rise");
        }

        WaveModel wave;
        const uint8_t encoding = in.U8();
        const uint8_t flags = in.U8();
        in.U16();
        if (encoding > static_cast<uint8_t>(ntdlib::sar::WaveEncoding::kDspAdpcm))
        {
            Newer("a wave in an encoding");
        }

        if ((flags & ~(kLoopingWave | kWaveSamples)) != 0)
        {
            Newer("a wave with a flag");
        }

        wave.encoding = static_cast<ntdlib::sar::WaveEncoding>(encoding);
        wave.loop = (flags & kLoopingWave) != 0;
        wave.rate = in.U32();
        wave.loop_start = in.U32();
        wave.loop_end = in.U32();
        wave.length = in.U32();

        // The PCM16 samples, channel after channel. The chunk has to hold them before they take memory.
        if ((flags & kWaveSamples) != 0)
        {
            const uint32_t channels = in.U32();
            if (channels == 0 || channels > kMostWaveChannels || wave.length > in.Left() / 2 / channels)
            {
                Damaged("wave " + std::to_string(place.index) + " of file " + std::to_string(place.file) +
                        " has more samples than its chunk can contain");
            }

            wave.samples.assign(channels, std::vector<int16_t>(wave.length));
            for (std::vector<int16_t>& channel : wave.samples)
            {
                for (int16_t& sample : channel)
                {
                    sample = static_cast<int16_t>(in.U16());
                }
            }
        }

        if (const std::optional<std::string> problem = WaveModelProblem(wave))
        {
            Damaged("wave " + std::to_string(place.index) + " of file " + std::to_string(place.file) + " " + *problem);
        }

        waves.emplace(place, std::move(wave));
    }

    return waves;
}

std::optional<std::string> WaveModelProblem(const WaveModel& wave)
{
    if (wave.encoding > ntdlib::sar::WaveEncoding::kDspAdpcm)
    {
        return std::string("is in IMA-ADPCM or another encoding that Hokuto doesn't write");
    }
    if (wave.rate == 0)
    {
        return std::string("plays at a rate of 0");
    }
    if (wave.loop_end == 0)
    {
        return std::string("plays no samples");
    }
    if (wave.loop_end > wave.length)
    {
        return std::string("ends its loop after its last sample");
    }
    if (wave.loop ? wave.loop_start >= wave.loop_end : wave.loop_start != 0)
    {
        return std::string("has a loop that doesn't end after its start");
    }
    if (wave.samples.empty())
    {
        return std::nullopt;
    }

    const auto whole = [&wave](const std::vector<int16_t>& channel)
    {
        return channel.size() == wave.length;
    };
    if (wave.samples.size() > kMostWaveChannels || !std::all_of(wave.samples.begin(), wave.samples.end(), whole))
    {
        return std::string("doesn't have one or two channels of its length");
    }

    return std::nullopt;
}

std::map<uint32_t, NativeStream> ReadStreams(std::span<const uint8_t> chunk)
{
    In in(chunk, "the STRM chunk");
    const uint32_t count = in.U32();
    std::map<uint32_t, NativeStream> streams;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t id = in.U32();
        const uint32_t flags = in.U32();
        if (!streams.empty() && id <= streams.rbegin()->first)
        {
            Damaged("the file IDs of its streams don't rise");
        }

        if ((flags & ~kHokutoStream) != 0)
        {
            Newer("a stream with a flag");
        }

        const std::span<const uint8_t> bytes = in.Take(in.U64());
        streams[id] = NativeStream{std::vector<uint8_t>(bytes.begin(), bytes.end()), (flags & kHokutoStream) == 0};
    }

    return streams;
}

namespace
{

// Adds to `chunks` the chunks of the archive of `file`.
void AddArchiveChunks(const File& file, std::vector<std::pair<std::string, std::vector<uint8_t>>>& chunks)
{
    // The archive in its form: the model's sounds and items, or the native layout.
    const NativeArchive* native = file.native && file.form == ArchiveForm::kNative ? &*file.native : nullptr;
    if (file.native && file.form == ArchiveForm::kModel)
    {
        const Model model = ModelOf(file);
        chunks.emplace_back(kSoundsChunk, SoundsChunk(model));
        chunks.emplace_back(kItemsChunk, ItemsChunk(model));
    }

    if (native)
    {
        chunks.emplace_back(kNativeChunk, NativeChunk(file));
    }

    if (native && native->wii_build)
    {
        if (native->engine != Engine::kWii)
        {
            throw std::invalid_argument("a build of the Wii's sound library belongs to an archive of the Wii");
        }

        chunks.emplace_back(kBuildChunk, BuildChunk(*native->wii_build));
    }

    if (native && native->tables)
    {
        chunks.emplace_back(kTablesChunk, TablesChunk(*native->tables));
    }

    if (native && native->game_sizes)
    {
        if (const std::optional<std::string> problem = GameSizesProblem(*native))
        {
            throw std::invalid_argument("the game's sizes " + *problem);
        }

        chunks.emplace_back(kSizesChunk, SizesChunk(*native->game_sizes));
    }

    if (native && !native->group_files.empty())
    {
        chunks.emplace_back(kGroupFilesChunk, GroupFilesChunk(file));
    }

    if (native && !native->streams.empty())
    {
        chunks.emplace_back(kStreamsChunk, StreamsChunk(*native));
    }

    if (native && native->cartridge)
    {
        if (native->engine != Engine::kDs && native->engine != Engine::kDse)
        {
            throw std::invalid_argument("a cartridge image belongs to a sound archive or a set of files of the DS");
        }

        chunks.emplace_back(kCartridgeChunk, CartridgeChunk(*native->cartridge));
    }
}

// The bytes of a file of `chunks`: the header, the table of chunks, then the chunks.
std::vector<uint8_t> LaidOut(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& chunks)
{
    // Where each chunk starts: after the header and the table of chunks, on a multiple of 8 bytes.
    std::vector<std::size_t> offsets;
    std::size_t end = kHeaderSize + chunks.size() * kEntrySize;
    for (const auto& [id, bytes] : chunks)
    {
        end = (end + kChunkAlignment - 1) / kChunkAlignment * kChunkAlignment;
        offsets.push_back(end);
        end += bytes.size();
    }

    Out out;
    out.Bytes(kSignature);
    out.U16(kMajorVersion);
    out.U16(kMinorVersion);
    out.U32(0);
    out.Count(chunks.size(), "chunks");
    out.U32(0);
    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        const auto& [id, bytes] = chunks[i];
        out.Bytes(std::span(reinterpret_cast<const uint8_t*>(id.data()), 4));
        out.U32(Crc32(bytes));
        out.U64(offsets[i]);
        out.U64(bytes.size());
    }

    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        out.Zeros(offsets[i] - out.Size());
        out.Bytes(chunks[i].second);
    }

    return out.Take();
}

// The chunks of the file that Hokuto reads, by their IDs, from the `count` entries of the table of chunks in `header`.
// The other chunks go into `file`.
std::map<std::string, std::span<const uint8_t>> KnownChunks(std::span<const uint8_t> bytes, In& header, uint32_t count,
                                                            File& file)
{
    std::map<std::string, std::span<const uint8_t>> known;
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::span<const uint8_t> entry = header.Take(kEntrySize);
        const std::string id(reinterpret_cast<const char*>(entry.data()), 4);
        const uint32_t crc = xsflib::LoadLe32(&entry[4]);
        const uint64_t offset = xsflib::LoadLe64(&entry[8]);
        const uint64_t size = xsflib::LoadLe64(&entry[16]);
        if (offset > bytes.size() || size > bytes.size() - offset)
        {
            Damaged("chunk " + id + " is past the end of the file");
        }

        const std::span<const uint8_t> chunk =
            bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
        if (Crc32(chunk) != crc)
        {
            Damaged("chunk " + id + " doesn't match its CRC-32");
        }

        if (IsHokutoChunk(id))
        {
            if (!known.emplace(id, chunk).second)
            {
                Damaged("it contains two " + id + " chunks");
            }
        }
        else
        {
            file.other_chunks.push_back({id, std::vector<uint8_t>(chunk.begin(), chunk.end())});
        }
    }

    return known;
}

// The native archive of the NATV chunk among `known`, with its build, tables, sizes, group files, streams and
// cartridge. Its sequence files name `sequences`.
std::optional<NativeArchive> ReadNativeChunks(const std::map<std::string, std::span<const uint8_t>>& known,
                                              std::vector<Sequence>& sequences)
{
    std::optional<NativeArchive> native;
    if (const auto chunk = known.find(std::string(kNativeChunk)); chunk != known.end())
    {
        native = ReadNative(chunk->second, sequences);
    }

    if (const auto build = known.find(std::string(kBuildChunk)); build != known.end())
    {
        if (!native || native->engine != Engine::kWii)
        {
            Damaged("it contains a build of the Wii sound library, but no native Wii archive");
        }

        native->wii_build = ReadBuild(build->second);
    }

    if (const auto tables = known.find(std::string(kTablesChunk)); tables != known.end())
    {
        if (!native)
        {
            Damaged("it contains tables, but no native archive");
        }

        native->tables = ReadTables(tables->second);
    }

    if (const auto sizes = known.find(std::string(kSizesChunk)); sizes != known.end())
    {
        if (!native)
        {
            Damaged("it contains the game sizes, but no native archive");
        }

        native->game_sizes = ReadSizes(sizes->second);
        if (const std::optional<std::string> problem = GameSizesProblem(*native))
        {
            Damaged("the game's sizes " + *problem);
        }
    }

    if (const auto group_files = known.find(std::string(kGroupFilesChunk)); group_files != known.end())
    {
        if (!native)
        {
            Damaged("it contains group files, but no native archive");
        }

        native->group_files = ReadGroupFiles(group_files->second, sequences);
    }

    if (const auto streams = known.find(std::string(kStreamsChunk)); streams != known.end())
    {
        if (!native)
        {
            Damaged("it contains streams, but no native archive");
        }

        native->streams = ReadStreams(streams->second);
    }

    if (const auto cartridge = known.find(std::string(kCartridgeChunk)); cartridge != known.end())
    {
        if (!native || (native->engine != Engine::kDs && native->engine != Engine::kDse))
        {
            Damaged("it contains a cartridge image, but no native DS archive");
        }

        native->cartridge = ReadCartridge(cartridge->second);
    }

    return native;
}

// The model of the SNDS and ITMS chunks among `known`, or nothing. A file with a native archive can't have them.
std::optional<Model> ReadModelChunks(const std::map<std::string, std::span<const uint8_t>>& known, bool has_native)
{
    const auto sounds = known.find(std::string(kSoundsChunk));
    const auto items = known.find(std::string(kItemsChunk));
    std::optional<Model> model;
    if (sounds != known.end() || items != known.end())
    {
        if (sounds == known.end() || items == known.end())
        {
            Damaged("it contains only one of the SNDS and ITMS chunks of the model form");
        }

        if (has_native)
        {
            Damaged("it contains its archive in both forms");
        }

        model = ReadModel(sounds->second, items->second);
    }

    return model;
}

// Reads the models of the edited banks and waves among `known` into `file`. They name the archive's files.
void ReadEditedModels(const std::map<std::string, std::span<const uint8_t>>& known, File& file)
{
    const auto banks = known.find(std::string(kBanksChunk));
    const auto waves = known.find(std::string(kWavesChunk));
    if (!file.native && (banks != known.end() || waves != known.end()))
    {
        Damaged("it contains banks or waves, but no archive");
    }

    if (banks != known.end())
    {
        file.banks = ReadBanks(banks->second);
    }

    if (waves != known.end())
    {
        file.waves = ReadWaves(waves->second);
    }
}

// Whether `file` or one of its sounds has a tag.
bool HasTags(const File& file)
{
    const auto tagged = [](const auto& entry)
    {
        return !entry.second.empty();
    };

    return !file.tags.empty() || std::any_of(file.sound_tags.begin(), file.sound_tags.end(), tagged);
}

// A chunk of the project's settings: its ID, whether a file has it, its writer and its reader.
struct ProjectChunk
{
    std::string_view id;
    bool (*present)(const File& file);
    std::vector<uint8_t> (*write)(const File& file);
    void (*read)(std::span<const uint8_t> chunk, File& file);
};

// The chunks of the project's settings that need nothing else of the file, in the order that Write writes them and
// Read reads them.
constexpr std::array<ProjectChunk, 7> kProjectChunks = {{
    {kScenesChunk, [](const File& file) { return !file.scenes.empty(); }, ScenesChunk, ReadScenes},
    {kTrackColoursChunk, [](const File& file) { return !file.track_colours.empty(); }, TrackColoursChunk,
     ReadTrackColours},
    {kInstrumentMapsChunk, [](const File& file) { return !file.instrument_maps.empty(); }, InstrumentMapsChunk,
     ReadInstrumentMaps},
    {kBankMapsChunk, [](const File& file) { return !file.bank_maps.empty(); }, BankMapsChunk, ReadBankMaps},
    {kEditorChunk, [](const File& file) { return file.view.has_value(); },
     [](const File& file) { return EditorChunk(*file.view); }, ReadEditor},
    {kTagsChunk, HasTags, TagsChunk, ReadTags},
    {kLengthsChunk, [](const File& file) { return !file.lengths.empty(); }, LengthsChunk, ReadLengths},
}};

} // namespace

std::vector<uint8_t> Write(const File& file)
{
    if (const std::optional<std::size_t> sequence = UnfiledChoices(file))
    {
        throw std::invalid_argument(
            "sequence " + std::to_string(*sequence) +
            " holds the encoder's choices, and no sequence file of the native archive or its group files names it");
    }
    if (const std::optional<std::size_t> sequence = ForeignChoices(file))
    {
        throw std::invalid_argument("sequence " + std::to_string(*sequence) +
                                    " holds another console's choices of the encoder than the native archive's");
    }

    // The chunks in order: Hokuto's chunks, then the chunks that Hokuto doesn't read.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> chunks;
    chunks.emplace_back(kEngineChunk, EngineChunk(file));
    const auto has_effects = [](const std::vector<Effect>& effects)
    {
        return !effects.empty();
    };
    if (std::any_of(file.aux_effects.begin(), file.aux_effects.end(), has_effects) ||
        std::any_of(file.aux_volumes.begin(), file.aux_volumes.end(), NotUnity))
    {
        chunks.emplace_back(kMixerChunk, MixerChunk(file));
    }

    chunks.emplace_back(kSequencesChunk, SequencesChunk(file));

    AddArchiveChunks(file, chunks);
    if (!file.banks.empty())
    {
        chunks.emplace_back(kBanksChunk, BanksChunk(file));
    }

    if (!file.waves.empty())
    {
        chunks.emplace_back(kWavesChunk, WavesChunk(file));
    }

    for (const ProjectChunk& chunk : kProjectChunks)
    {
        if (chunk.present(file))
        {
            chunks.emplace_back(chunk.id, chunk.write(file));
        }
    }

    if (file.timelines)
    {
        chunks.emplace_back(kTimelinesChunk, TimelinesChunk(file));
    }

    for (const OtherChunk& chunk : file.other_chunks)
    {
        if (chunk.id.size() != 4 || IsHokutoChunk(chunk.id))
        {
            throw std::invalid_argument("a chunk that Hokuto doesn't read can't have the ID " + chunk.id);
        }

        chunks.emplace_back(chunk.id, chunk.bytes);
    }

    return LaidOut(chunks);
}

File Read(std::span<const uint8_t> bytes)
{
    if (!IsBinary(bytes))
    {
        throw xsflib::FormatError("It isn't a Hokuto file.");
    }

    In header(bytes.subspan(kSignature.size()), "the header");
    const uint16_t major = header.U16();
    header.U16();
    header.U32();
    const uint32_t count = header.U32();
    header.U32();
    if (major != kMajorVersion)
    {
        throw xsflib::FormatError("A later version of Hokuto saved the file, in version " + std::to_string(major) +
                                  " of the format. This version reads only version " + std::to_string(kMajorVersion) +
                                  ".");
    }

    // The chunks, checked against their CRC-32s.
    File file;
    const std::map<std::string, std::span<const uint8_t>> known = KnownChunks(bytes, header, count, file);

    // The engine, its mixer, the scenes and the tracks' colours, then the sequences, the native archive that names
    // them, its tables and its group files.
    if (!known.contains(std::string(kEngineChunk)))
    {
        Damaged("it has no engine");
    }

    ReadEngine(known.at(std::string(kEngineChunk)), file);
    if (const auto mixer = known.find(std::string(kMixerChunk)); mixer != known.end())
    {
        ReadMixer(mixer->second, file);
    }

    for (const ProjectChunk& project : kProjectChunks)
    {
        if (const auto chunk = known.find(std::string(project.id)); chunk != known.end())
        {
            project.read(chunk->second, file);
        }
    }

    std::vector<Sequence> sequences;
    if (const auto chunk = known.find(std::string(kSequencesChunk)); chunk != known.end())
    {
        sequences = ReadSequences(chunk->second);
    }

    std::optional<NativeArchive> native = ReadNativeChunks(known, sequences);
    if (native)
    {
        CheckNativeEngine(file.engine, *native);
    }

    // A file in the model form holds the model's sounds and items, and no native layout.
    const std::optional<Model> model = ReadModelChunks(known, native.has_value());

    // Reading the native archive fills in the sequences' choices. The file shares the parts once they're whole. The
    // model builds the native archive from the sequences.
    file.sequences = ShareSequences(std::move(sequences));
    if (native)
    {
        file.native = std::move(*native);
    }

    if (model)
    {
        file = WithModel(std::move(file), *model);
    }

    if (const auto timelines = known.find(std::string(kTimelinesChunk)); timelines != known.end())
    {
        ReadTimelines(timelines->second, file);
    }

    ReadEditedModels(known, file);

    return file;
}

bool IsBinary(std::span<const uint8_t> bytes)
{
    return bytes.size() >= kHeaderSize && std::equal(kSignature.begin(), kSignature.end(), bytes.begin());
}

} // namespace hokuto::format
