// SPDX-License-Identifier: MIT

#include "format/model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
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
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/units.h"
#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sound_build.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "format/archive.h"
#include "format/binary.h"
#include "format/codes.h"
#include "format/command_values.h"
#include "format/kinds.h"

namespace hokuto::format
{
namespace
{

namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
using binary::Damaged;
using binary::In;
using binary::Newer;
using binary::Out;
using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;

// The flags of a sound in the SNDS chunk. The flags of the user's parameters follow in a byte of their own.
constexpr uint8_t kReleasePriorityFix = 1 << 0;
constexpr uint8_t kSinglePlay = 1 << 1;
constexpr uint8_t kOption17 = 1 << 2;
constexpr uint8_t kStartItem = 1 << 3;
constexpr uint8_t kExtension = 1 << 4;
constexpr uint8_t kSoundFlags = (1 << 5) - 1;
constexpr uint8_t kUserFlags = (1 << 4) - 1;

// The sequences of `file` that the model's sequence files `sequences` hold, by the files' IDs. The cut of the archive
// that the model lays out takes them (ntdlib::sar::Cut). A sequence whose commands run on past a return to commands
// that no label or start leads to then comes back from the cut too.
sar::KnownSequences KnownOf(const File& file, const std::map<uint32_t, ModelSequence>& sequences)
{
    sar::KnownSequences known;
    for (const auto& [id, held] : sequences)
    {
        if (held.sequence < file.sequences.size())
        {
            known[id] = *file.sequences[held.sequence];
        }
    }

    return known;
}

// The fields after a stream sound's sends, by their numbers in the SNDS chunk.
constexpr uint8_t kNoFields = 0;
constexpr uint8_t kExtensionField = 1;
constexpr uint8_t kPrefetchFields = 2;

// The options of a 3DS stream sound of 2.3.0.0: the values of the extension that its entry holds, a bit each.
constexpr uint8_t kStreamOptions = (1 << 3) - 1;

// The places of a file in the ITMS chunk, and the flags of a file.
constexpr uint8_t kBytesPlace = 0;
constexpr uint8_t kCopyPlace = 1;
constexpr uint8_t kGroupsOnlyPlace = 2;
constexpr uint8_t kPathPlace = 3;
constexpr uint8_t kSequenceFile = 1 << 0; // the file holds one of the file's sequences
constexpr uint8_t kHeldFile = 1 << 1;     // the chunk holds the bytes of a file beside the archive
constexpr uint8_t kFileFlags = (1 << 2) - 1;

// The group of a group file in the ITMS chunk where Hokuto couldn't find it.
constexpr uint32_t kNoGroup = 0xFFFFFFFF;

// The oldest versions of the archives that Hokuto lays out: FSAR 2.2.0 on the Switch and 2.0.0 on the Wii U, and CSAR
// 2.0.0.0 on the 3DS.
constexpr uint32_t kOldestSwitchArchive = 0x00020200;
constexpr uint32_t kOldestWiiUArchive = 0x00020000;
constexpr uint32_t kOldest3dsArchive = 0x02000000;

// The kind of the Wii's archives in the ITMS chunk, the chunk's flags on the Wii, and the flags of a file there.
constexpr uint8_t kWiiArchive = 4;
constexpr uint8_t kWiiBuild = 1 << 0;          // the file keeps the game's build of NW4R SND
constexpr uint8_t kAlignedTrees = 1 << 1;      // the trees of the archive's SYMB block start on a multiple of 4 bytes
constexpr uint8_t kUnterminatedPaths = 1 << 2; // a path whose length is a multiple of 4 takes no zero after it
constexpr uint8_t kWiiItemFlags = (1 << 3) - 1;
constexpr uint8_t kOctober2006 = 1 << 0; // the build that comes with AX of October 2006 (bit 0 in BULD too)
constexpr uint8_t kWiiPathFile = 1 << 1; // the file lies beside the archive
constexpr uint8_t kWiiHeldFile = 1 << 2; // the chunk holds the bytes of the file beside the archive
constexpr uint8_t kWiiFileFlags = (1 << 3) - 1;
constexpr uint8_t kWiiSoundFlags = kReleasePriorityFix | kStartItem;

// The newest version of the Wii's archives that Hokuto models: RSAR 1.4.
constexpr uint16_t kNewestWiiArchive = 0x0104;

// The flags of a sound group and of a wave archive.
constexpr uint8_t kWaveArchives = 1 << 0;
constexpr uint8_t kLoadIndividually = 1 << 0;
constexpr uint8_t kWaveCount = 1 << 1;

// The scales of the values that a stream sound's details share with the commands: a pan, a surround pan, a send and a
// low-pass filter. The Wii U and the Switch use the same scales.
const ntdlib::nwsnd::Grammar kGrammar = ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::NxPolicy>();
const ntdlib::nwsnd::Scale kPanScale = ntdlib::nwsnd::ScaleOf(Kind::kPan, kGrammar);
const ntdlib::nwsnd::Scale kSpanScale = ntdlib::nwsnd::ScaleOf(Kind::kSurroundPan, kGrammar);
const ntdlib::nwsnd::Scale kSendScale = ntdlib::nwsnd::ScaleOf(Kind::kSend, kGrammar);
const ntdlib::nwsnd::Scale kLowPassScale = ntdlib::nwsnd::ScaleOf(Kind::kLowPass, kGrammar);

// Throws xsflib::FormatError for `bytes`, an archive whose header gives a larger size than the bytes hold. The tools
// write the size at `at`: at 8 in the Wii's archives, and at 12 in the later consoles'. A reader of the model would
// stop at its first read past the end, with another message on each console.
void RequireWhole(std::span<const uint8_t> bytes, std::size_t at, bool big_endian)
{
    if (bytes.size() < at + 4)
    {
        return;
    }

    const uint32_t size = big_endian ? xsflib::LoadBe32(bytes.data() + at) : xsflib::LoadLe32(bytes.data() + at);
    if (size > bytes.size())
    {
        throw xsflib::FormatError("an archive laid out another way: the archive is cut short");
    }
}

// A byte of the console in its unit, and the byte nearest a value in its unit.
Fixed ToUnit(uint8_t value, const ntdlib::nwsnd::Scale& scale)
{
    return ntdlib::nwsnd::ToUnit(value, scale);
}

uint8_t ByteOf(Fixed unit, const ntdlib::nwsnd::Scale& scale)
{
    return static_cast<uint8_t>(std::clamp<int64_t>(ntdlib::nwsnd::FromUnit(unit, scale), 0, 255));
}

// `sequence` without the encoder's choices. A kOther command keeps its opcode: the opcode is the command.
Sequence Plain(Sequence sequence)
{
    for (ntdlib::commands::Command& item : sequence.items)
    {
        item.encoding = {.opcode = item.encoding.opcode};
    }

    return sequence;
}

// The bytes of a sequence file of `flavor` and `version` that holds `sequence`, and the offset of each of its items in
// the bytecode, then the end. ntdlib's join writes a sequence file the same way.
std::pair<std::vector<uint8_t>, std::vector<uint32_t>> SequenceFileOf(const Sequence& sequence, sar::Flavor flavor,
                                                                      uint32_t version)
{
    ntdlib::nwsnd::Encoded encoded = ntdlib::nwsnd::EncodeSequence(sequence, GrammarOf(flavor));
    sar::SequenceFile file;
    file.flavor = flavor;
    file.version = version;
    file.data = std::move(encoded.bytes);
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        file.labels.push_back({label.name, encoded.offsets.at(label.item)});
    }

    return {file.Write(), std::move(encoded.offsets)};
}

// The version of each sequence of `native`: the version of its first sequence piece in the archive, or else in the
// group files. A sequence file that only group files hold has its pieces there.
std::map<uint32_t, uint32_t> SequenceVersions(const NativeArchive& native)
{
    std::map<uint32_t, uint32_t> versions;
    const auto add = [&versions](const std::vector<sar::Piece>& pieces)
    {
        for (const sar::Piece& piece : pieces)
        {
            if (const auto* sequence = std::get_if<sar::SequencePiece>(&piece))
            {
                versions.emplace(sequence->sequence, sequence->version);
            }
        }
    };
    add(*native.pieces);
    for (const NativeGroupFile& group_file : native.group_files)
    {
        add(*group_file.pieces);
    }

    return versions;
}

// `pieces` with each sequence piece naming the sequence that `map` gives for the one that it names.
std::vector<sar::Piece> Renamed(std::vector<sar::Piece> pieces, const std::map<uint32_t, uint32_t>& map)
{
    for (sar::Piece& piece : pieces)
    {
        if (auto* sequence = std::get_if<sar::SequencePiece>(&piece))
        {
            sequence->sequence = map.at(sequence->sequence);
        }
    }

    return pieces;
}

// `fields` with each place that names a sequence naming the sequence that `map` gives for the one that it names.
std::vector<sar::Field> Renamed(std::vector<sar::Field> fields, const std::map<uint32_t, uint32_t>& map)
{
    for (sar::Field& field : fields)
    {
        for (sar::Place* place : {&field.from, &field.to})
        {
            if (place->sequence)
            {
                place->piece = map.at(place->piece);
            }
        }
    }

    return fields;
}

// `group_file`, a group file of `flavor`, with each copy of a file of `sequence_files` holding the file's bytes. A
// group file whose copies hold them already stays as it is, and so does one that ntdlib can't read. Throws
// xsflib::FormatError for a group file that can't take the bytes.
std::vector<uint8_t> WithSequenceCopies(std::vector<uint8_t> group_file, sar::Flavor flavor,
                                        std::span<const sar::GroupMember> sequence_files)
{
    std::vector<sar::GroupCopy> copies;
    try
    {
        copies = sar::GroupCopies(group_file, flavor);
    }
    catch (const xsflib::FormatError&)
    {
        return group_file;
    }

    std::vector<sar::GroupMember> changed;
    for (const sar::GroupCopy& copy : copies)
    {
        const auto file = std::ranges::find(sequence_files, copy.file, &sar::GroupMember::file);
        if (file != sequence_files.end() &&
            !std::ranges::equal(*file->copy, std::span(group_file).subspan(copy.start, copy.size)))
        {
            changed.push_back(*file);
        }
    }
    if (changed.empty())
    {
        return group_file;
    }

    try
    {
        return sar::GroupWithCopies(group_file, flavor, changed);
    }
    catch (const xsflib::FormatError& error)
    {
        throw xsflib::FormatError(std::string("A group file can't take the bytes of its sequence files: ") +
                                  error.what() + ".");
    }
}

void WriteList(Out& out, const std::vector<uint32_t>& values, const std::string& what)
{
    out.Count(values.size(), what);
    for (const uint32_t value : values)
    {
        out.U32(value);
    }
}

std::vector<uint32_t> ReadList(In& in)
{
    const uint32_t count = in.U32();
    std::vector<uint32_t> values;
    for (uint32_t i = 0; i < count; ++i)
    {
        values.push_back(in.U32());
    }

    return values;
}

// Bytes that start with their 64-bit size.
void WriteBytes(Out& out, std::span<const uint8_t> bytes)
{
    out.U64(bytes.size());
    out.Bytes(bytes);
}

std::vector<uint8_t> ReadBytes(In& in)
{
    const std::span<const uint8_t> bytes = in.Take(in.U64());
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// A main send and the sends to the three aux buses, in Hokuto's units.
void WriteSends(Out& out, uint8_t main, const std::array<uint8_t, 3>& aux)
{
    out.I64(ToUnit(main, kSendScale));
    for (const uint8_t send : aux)
    {
        out.I64(ToUnit(send, kSendScale));
    }
}

void ReadSends(In& in, uint8_t& main, std::array<uint8_t, 3>& aux)
{
    main = ByteOf(in.I64(), kSendScale);
    for (uint8_t& send : aux)
    {
        send = ByteOf(in.I64(), kSendScale);
    }
}

void WriteStream(Out& out, const sar::SoundModel& sound)
{
    const sar::StreamSoundInfo& stream = sound.stream;
    out.U16(stream.valid_tracks);
    out.U16(stream.channel_count);
    out.F32(stream.pitch);
    WriteSends(out, stream.main_send, stream.aux_sends);
    switch (sound.stream_fields)
    {
    case sar::StreamFields::kNone:
        out.U8(kNoFields);
        break;
    case sar::StreamFields::kExtension:
        out.U8(kExtensionField);
        break;
    case sar::StreamFields::kPrefetch:
        out.U8(kPrefetchFields);
        break;
    }
    out.U8(sound.stream_options);
    out.Zeros(2);
    out.U32(sound.prefetch_file_id);
    if (const std::optional<sar::StreamExtension>& extension = sound.stream_extension)
    {
        out.U32(extension->stream_type);
        out.U32(extension->loop_start);
        out.U32(extension->loop_end);
    }

    out.Count(stream.tracks.size(), "tracks of a stream sound");
    for (const sar::StreamTrackInfo& track : stream.tracks)
    {
        out.Count(track.channels.size(), "channels of a stream's track");
        out.Bytes(track.channels);
        out.I64(LevelOfVolume(track.volume));
        out.I64(ToUnit(track.pan, kPanScale));
        out.I64(ToUnit(track.span, kSpanScale));
        out.U8(track.flags);
        out.U8(track.biquad_type);
        out.U8(track.biquad_value);
        out.U8(0);
        WriteSends(out, track.main_send, track.aux_sends);
        out.I64(ToUnit(track.low_pass, kLowPassScale));
    }
}

void ReadStream(In& in, sar::SoundModel& sound, bool extension)
{
    sar::StreamSoundInfo& stream = sound.stream;
    stream.valid_tracks = in.U16();
    stream.channel_count = in.U16();
    stream.pitch = in.F32();
    ReadSends(in, stream.main_send, stream.aux_sends);
    const uint8_t fields = in.U8();
    if (fields > kPrefetchFields)
    {
        Newer("a kind of a stream sound's details (" + std::to_string(fields) + ")");
    }

    sound.stream_fields = fields == kNoFields         ? sar::StreamFields::kNone
                          : fields == kExtensionField ? sar::StreamFields::kExtension
                                                      : sar::StreamFields::kPrefetch;
    sound.stream_options = in.U8();
    if ((sound.stream_options & ~kStreamOptions) != 0)
    {
        Newer("an option of a stream sound");
    }
    in.Take(2);
    sound.prefetch_file_id = in.U32();
    if (extension)
    {
        sar::StreamExtension& read = sound.stream_extension.emplace();
        read.stream_type = in.U32();
        read.loop_start = in.U32();
        read.loop_end = in.U32();
    }

    const uint32_t tracks = in.U32();
    for (uint32_t t = 0; t < tracks; ++t)
    {
        sar::StreamTrackInfo& track = stream.tracks.emplace_back();
        const std::span<const uint8_t> channels = in.Take(in.U32());
        track.channels.assign(channels.begin(), channels.end());
        track.volume = VolumeOfLevel(in.I64());
        track.pan = ByteOf(in.I64(), kPanScale);
        track.span = ByteOf(in.I64(), kSpanScale);
        track.flags = in.U8();
        track.biquad_type = in.U8();
        track.biquad_value = in.U8();
        in.U8();
        ReadSends(in, track.main_send, track.aux_sends);
        track.low_pass = ByteOf(in.I64(), kLowPassScale);
    }
}

// The code of a kind of the Wii's sounds in the SNDS chunk: the code of the later consoles' kind.
uint8_t WiiSoundCode(rsar::SoundType type)
{
    switch (type)
    {
    case rsar::SoundType::kSequence:
        return CodeOf(sar::SoundType::kSequence);
    case rsar::SoundType::kWaveSound:
        return CodeOf(sar::SoundType::kWaveSound);
    case rsar::SoundType::kStream:
        return CodeOf(sar::SoundType::kStream);
    case rsar::SoundType::kUnknown:
        break;
    }

    return 0;
}

std::optional<rsar::SoundType> WiiSoundTypeOfCode(uint8_t code)
{
    for (const rsar::SoundType type :
         {rsar::SoundType::kSequence, rsar::SoundType::kWaveSound, rsar::SoundType::kStream})
    {
        if (WiiSoundCode(type) == code)
        {
            return type;
        }
    }

    return std::nullopt;
}

// The bytes of a sequence file of the Wii of `version` that holds `sequence`, encoded by the rules of NW4R SND's build
// `build`, and the offset of each of its items in the bytecode, then the end. ntdlib's join writes a sequence file of
// the Wii the same way.
std::pair<std::vector<uint8_t>, std::vector<uint32_t>> WiiSequenceFileOf(const Sequence& sequence,
                                                                         const ntdlib::SndBuild& build,
                                                                         uint32_t version)
{
    ntdlib::nwsnd::Encoded encoded =
        ntdlib::nwsnd::EncodeSequence(sequence, ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(build));
    rsar::SequenceFile file;
    file.version = static_cast<uint16_t>(version);
    file.data = std::move(encoded.bytes);
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        file.labels.push_back({label.name, encoded.offsets.at(label.item)});
    }

    return {file.Write(), std::move(encoded.offsets)};
}

// The model of `file`'s archive of the Wii (ModelOf).
Model WiiModelOf(const File& file)
{
    // The archive that the sequences without the encoder's choices build, and its items.
    File plain = file;
    plain.sequences = WithoutChoices(file.sequences);
    const std::vector<uint8_t> bytes = ExportArchive(plain);
    RequireWhole(bytes, 8, true);
    Model model;
    model.wii = rsar::ReadArchiveModel(bytes);
    model.wii_build = file.native->wii_build;
    rsar::ArchiveModel& archive = *model.wii;

    // The sequence that each file holds. Each copy of a sequence file takes its bytes from the sequence.
    const rsar::SoundArchive read = rsar::SoundArchive::Load(bytes, ntdlib::rvl::snd::kRsarOptions);
    const std::vector<std::optional<uint32_t>> sequences = SequencesOfFiles(plain, bytes, read);
    const std::map<uint32_t, uint32_t> versions = SequenceVersions(*file.native);
    for (uint32_t id = 0; id < sequences.size() && id < archive.files.size(); ++id)
    {
        if (sequences[id])
        {
            model.sequences[id] = {*sequences[id], versions.at(*sequences[id])};
            archive.files[id].bytes.clear();
        }
    }

    // Each sequence sound whose file holds a sequence starts at an item of the sequence.
    const ntdlib::nwsnd::Grammar grammar =
        ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(BuildOfWiiArchive(*file.native));
    std::map<uint32_t, std::vector<uint32_t>> offsets;
    for (uint32_t s = 0; s < archive.sounds.size(); ++s)
    {
        rsar::SoundModel& sound = archive.sounds[s];
        const auto held = model.sequences.find(sound.file_id);
        if (sound.type != rsar::SoundType::kSequence || held == model.sequences.end())
        {
            continue;
        }

        const uint32_t sequence = held->second.sequence;
        auto known = offsets.find(sequence);
        if (known == offsets.end())
        {
            known =
                offsets.emplace(sequence, ntdlib::nwsnd::EncodeSequence(*plain.sequences[sequence], grammar).offsets)
                    .first;
        }
        const std::vector<uint32_t>& items = known->second;
        const auto item = std::lower_bound(items.begin(), items.end(), sound.start_offset);
        if (item == items.end() || *item != sound.start_offset)
        {
            throw std::invalid_argument("sound " + std::to_string(s) + " starts inside a command of sequence " +
                                        std::to_string(sequence));
        }

        model.starts[s] = static_cast<uint32_t>(item - items.begin());
        sound.start_offset = 0;
    }

    for (const auto& [id, stream] : file.native->streams)
    {
        model.streams[id] = *stream.bytes;
    }

    return model;
}

// `file` with the archive of `model`, a model of the Wii's archive (WithModel).
File WithWiiModel(File file, const Model& model)
{
    // Each sequence file's bytes, and each sequence sound's start.
    rsar::ArchiveModel archive = *model.wii;
    const ntdlib::SndBuild build = model.wii_build.value_or(BuildOfWiiVersion(archive.version));
    std::map<uint32_t, std::vector<uint32_t>> offsets;
    for (const auto& [id, held] : model.sequences)
    {
        if (id >= archive.files.size() || held.sequence >= file.sequences.size() || archive.files[id].path)
        {
            Damaged("file " + std::to_string(id) + " of the model names no file or no sequence");
        }

        std::pair<std::vector<uint8_t>, std::vector<uint32_t>> written;
        try
        {
            written = WiiSequenceFileOf(*file.sequences[held.sequence], build, held.version);
        }
        catch (const std::exception& error)
        {
            throw xsflib::FormatError("Sequence " + std::to_string(held.sequence) + " can't be written for the Wii. " +
                                      AsSentence(error.what()));
        }
        archive.files[id].bytes = std::move(written.first);
        offsets[held.sequence] = std::move(written.second);
    }
    for (const auto& [s, item] : model.starts)
    {
        const auto held =
            s < archive.sounds.size() ? model.sequences.find(archive.sounds[s].file_id) : model.sequences.end();
        if (held == model.sequences.end() || archive.sounds[s].type != rsar::SoundType::kSequence)
        {
            Damaged("sound " + std::to_string(s) + " of the model starts in a sequence without a sequence sound");
        }

        const std::vector<uint32_t>& items = offsets.at(held->second.sequence);
        if (item + 1 >= items.size())
        {
            Damaged("sound " + std::to_string(s) + " of the model starts past its sequence's items");
        }
        archive.sounds[s].start_offset = items[item];
    }

    // The archive that the model lays out, cut as an import cuts a game's archive.
    std::vector<uint8_t> bytes;
    try
    {
        bytes = rsar::BuildArchive(archive);
    }
    catch (const std::invalid_argument& error)
    {
        throw xsflib::FormatError(std::string("The model's archive can't be laid out: ") + error.what() + ".");
    }
    File built = ImportWiiArchive(bytes, model.wii_build, model.streams, KnownOf(file, model.sequences));

    // The cut's sequences are the file's sequences, in another order.
    const rsar::SoundArchive read = rsar::SoundArchive::Load(bytes, ntdlib::rvl::snd::kRsarOptions);
    const std::vector<std::optional<uint32_t>> cut = SequencesOfFiles(built, bytes, read);
    std::map<uint32_t, uint32_t> map;
    for (const auto& [id, held] : model.sequences)
    {
        const std::optional<uint32_t> found = id < cut.size() ? cut[id] : std::nullopt;
        if (!found || map.emplace(*found, held.sequence).first->second != held.sequence)
        {
            Damaged("the model's file " + std::to_string(id) + " doesn't cut as sequence " +
                    std::to_string(held.sequence));
        }
    }
    for (uint32_t c = 0; c < built.sequences.size(); ++c)
    {
        const auto mapped = map.find(c);
        if (mapped == map.end() || !(*built.sequences[c] == *file.sequences[mapped->second]))
        {
            Damaged("a sequence file of the model doesn't give its sequence back");
        }
    }

    // The native archive names the file's sequences. Its streams are Hokuto's, and it keeps no game's sizes.
    NativeArchive native = *built.native;
    native.pieces = Renamed(*native.pieces, map);
    for (auto& [id, stream] : native.streams)
    {
        stream.from_game = false;
    }
    native.game_sizes.reset();
    file.native = std::move(native);
    file.form = ArchiveForm::kModel;

    return file;
}

// The Wii's form of the SNDS chunk (SoundsChunk).
std::vector<uint8_t> WiiSoundsChunk(const Model& model)
{
    const rsar::ArchiveModel& archive = *model.wii;
    Out out;
    out.Count(archive.sounds.size(), "sounds");
    for (uint32_t s = 0; s < archive.sounds.size(); ++s)
    {
        const rsar::SoundModel& sound = archive.sounds[s];
        const auto start = model.starts.find(s);
        uint8_t flags = 0;
        flags |= sound.release_priority_fix ? kReleasePriorityFix : 0;
        flags |= start != model.starts.end() ? kStartItem : 0;

        out.Name(sound.name, "bytes in a sound's name");
        out.U8(WiiSoundCode(sound.type));
        out.U8(flags);
        out.Zeros(2);
        out.U32(sound.file_id);
        out.U32(sound.player_id);
        out.I64(LevelOfVolume(sound.volume));
        out.U8(sound.remote_filter);
        out.U8(sound.pan_mode);
        out.U8(sound.pan_curve);
        out.U8(sound.player_priority);
        out.U8(sound.actor_player_id);
        out.U8(sound.channel_priority);
        out.Zeros(2);
        out.U32(sound.user_params[0]);
        out.U32(sound.user_params[1]);
        out.U32(sound.settings_3d.flags);
        out.U8(sound.settings_3d.decay_curve);
        out.U8(sound.settings_3d.decay_ratio);
        out.U8(sound.settings_3d.doppler_factor);
        out.U8(0);
        switch (sound.type)
        {
        case rsar::SoundType::kSequence:
            out.U32(start != model.starts.end() ? start->second : sound.start_offset);
            out.U32(sound.bank);
            out.U32(sound.allocate_track_flags);
            break;

        case rsar::SoundType::kWaveSound:
            out.U32(static_cast<uint32_t>(sound.wave_sound_index));
            out.U32(sound.allocate_track_count);
            break;

        case rsar::SoundType::kStream:
            out.U32(sound.stream_start);
            out.U16(sound.allocate_channel_count);
            out.U16(sound.stream_track_flags);
            break;

        case rsar::SoundType::kUnknown:
            throw std::invalid_argument("sound " + std::to_string(s) + " is of an unknown kind");
        }
    }

    return out.Take();
}

// The Wii's form of the ITMS chunk (ItemsChunk).
std::vector<uint8_t> WiiItemsChunk(const Model& model)
{
    const rsar::ArchiveModel& archive = *model.wii;
    Out out;
    out.U8(kWiiArchive);
    uint8_t flags = 0;
    flags |= model.wii_build ? kWiiBuild : 0;
    flags |= archive.aligned_trees ? kAlignedTrees : 0;
    flags |= archive.terminated_paths ? 0 : kUnterminatedPaths;
    out.U8(flags);
    out.Zeros(2);
    out.U32(archive.version);
    out.U32(model.wii_build ? model.wii_build->date : 0);
    out.U8(model.wii_build && model.wii_build->october_2006 ? kOctober2006 : 0);
    out.Zeros(3);
    const rsar::PlayerSettings& settings = archive.player_settings;
    for (const uint16_t count :
         {settings.sequence_sounds, settings.sequence_tracks, settings.stream_sounds, settings.stream_tracks,
          settings.stream_channels, settings.wave_sounds, settings.wave_tracks})
    {
        out.U16(count);
    }
    out.Zeros(2);

    out.Count(archive.banks.size(), "banks");
    for (const rsar::BankModel& bank : archive.banks)
    {
        out.Name(bank.name, "bytes in a bank's name");
        out.U32(bank.file_id);
    }

    out.Count(archive.players.size(), "players");
    for (const rsar::PlayerModel& player : archive.players)
    {
        out.Name(player.name, "bytes in a player's name");
        out.U32(player.playable_sound_count);
        out.U32(player.heap_size);
    }

    out.Count(archive.groups.size(), "groups");
    for (const rsar::GroupModel& group : archive.groups)
    {
        out.Name(group.name, "bytes in a group's name");
        WriteList(out, group.file_ids, "files of a group");
    }

    // The files: each one's flags, then its sequence, its bytes and its wave data, or its path and the bytes of a
    // stream there.
    out.Count(archive.files.size(), "files");
    for (uint32_t id = 0; id < archive.files.size(); ++id)
    {
        const rsar::FileModel& held = archive.files[id];
        const auto sequence = model.sequences.find(id);
        const auto beside = model.streams.find(id);
        const bool has_sequence = sequence != model.sequences.end();
        const bool has_bytes = held.path && beside != model.streams.end();
        out.U8((has_sequence ? kSequenceFile : 0) | (held.path ? kWiiPathFile : 0) | (has_bytes ? kWiiHeldFile : 0));
        out.Zeros(3);
        if (has_sequence)
        {
            out.U32(sequence->second.sequence);
            out.U32(sequence->second.version);
        }
        if (held.path)
        {
            out.Name(*held.path, "bytes in a file's path");
            out.U32(held.path_size);
            if (has_bytes)
            {
                WriteBytes(out, beside->second);
            }
            continue;
        }
        if (!has_sequence)
        {
            WriteBytes(out, held.bytes);
        }
        WriteBytes(out, held.wave_data);
    }

    return out.Take();
}

// The model that the Wii's SNDS chunk `sounds` and ITMS chunk hold. `in` reads the ITMS chunk after its kind
// (ReadModel).
Model ReadWiiModel(std::span<const uint8_t> sounds, In& in)
{
    Model model;
    rsar::ArchiveModel& archive = model.wii.emplace();
    const uint8_t flags = in.U8();
    if ((flags & ~kWiiItemFlags) != 0)
    {
        Newer("a flag of the Wii's archive");
    }
    in.Take(2);
    const uint32_t version = in.U32();
    if (version > 0xFFFF)
    {
        Damaged("the Wii's archive has a version past 0xFFFF");
    }
    archive.version = static_cast<uint16_t>(version);
    const uint32_t date = in.U32();
    const uint8_t build_flags = in.U8();
    if ((build_flags & ~kOctober2006) != 0)
    {
        Newer("a build of the Wii's sound library");
    }
    in.Take(3);
    if ((flags & kWiiBuild) != 0)
    {
        model.wii_build = ntdlib::SndBuild{.date = date, .october_2006 = (build_flags & kOctober2006) != 0};
    }
    archive.aligned_trees = (flags & kAlignedTrees) != 0;
    archive.terminated_paths = (flags & kUnterminatedPaths) == 0;
    rsar::PlayerSettings& settings = archive.player_settings;
    for (uint16_t* count :
         {&settings.sequence_sounds, &settings.sequence_tracks, &settings.stream_sounds, &settings.stream_tracks,
          &settings.stream_channels, &settings.wave_sounds, &settings.wave_tracks})
    {
        *count = in.U16();
    }
    in.Take(2);

    const uint32_t banks = in.U32();
    for (uint32_t i = 0; i < banks; ++i)
    {
        rsar::BankModel& bank = archive.banks.emplace_back();
        bank.name = in.Name();
        bank.file_id = in.U32();
    }

    const uint32_t players = in.U32();
    for (uint32_t i = 0; i < players; ++i)
    {
        rsar::PlayerModel& player = archive.players.emplace_back();
        player.name = in.Name();
        const uint32_t limit = in.U32();
        if (limit > 0xFF)
        {
            Damaged("player " + std::to_string(i) + " plays more than 255 sounds at once");
        }
        player.playable_sound_count = static_cast<uint8_t>(limit);
        player.heap_size = in.U32();
    }

    const uint32_t groups = in.U32();
    for (uint32_t i = 0; i < groups; ++i)
    {
        rsar::GroupModel& group = archive.groups.emplace_back();
        group.name = in.Name();
        group.file_ids = ReadList(in);
    }

    const uint32_t files = in.U32();
    for (uint32_t id = 0; id < files; ++id)
    {
        rsar::FileModel& held = archive.files.emplace_back();
        const uint8_t file_flags = in.U8();
        if ((file_flags & ~kWiiFileFlags) != 0)
        {
            Newer("a flag of a file");
        }
        in.Take(3);
        if ((file_flags & kSequenceFile) != 0)
        {
            ModelSequence& sequence = model.sequences[id];
            sequence.sequence = in.U32();
            sequence.version = in.U32();
        }
        if ((file_flags & kWiiPathFile) != 0)
        {
            if ((file_flags & kSequenceFile) != 0)
            {
                Damaged("file " + std::to_string(id) + " lies beside the archive and holds a sequence");
            }
            held.path = in.Name();
            held.path_size = in.U32();
            if ((file_flags & kWiiHeldFile) != 0)
            {
                model.streams[id] = ReadBytes(in);
            }
            continue;
        }
        if ((file_flags & kWiiHeldFile) != 0)
        {
            Damaged("file " + std::to_string(id) + " holds the bytes of a file beside the archive");
        }
        if ((file_flags & kSequenceFile) == 0)
        {
            held.bytes = ReadBytes(in);
        }
        held.wave_data = ReadBytes(in);
    }

    // The sounds.
    In sound_in(sounds, "the SNDS chunk");
    const uint32_t count = sound_in.U32();
    for (uint32_t s = 0; s < count; ++s)
    {
        rsar::SoundModel& sound = archive.sounds.emplace_back();
        sound.name = sound_in.Name();
        const uint8_t kind = sound_in.U8();
        const std::optional<rsar::SoundType> type = WiiSoundTypeOfCode(kind);
        if (!type)
        {
            Newer("a kind of sound (" + std::to_string(kind) + ")");
        }
        sound.type = *type;
        const uint8_t sound_flags = sound_in.U8();
        if ((sound_flags & ~kWiiSoundFlags) != 0)
        {
            Newer("a flag of a sound");
        }
        sound_in.Take(2);
        sound.release_priority_fix = (sound_flags & kReleasePriorityFix) != 0;
        sound.file_id = sound_in.U32();
        sound.player_id = sound_in.U32();
        sound.volume = VolumeOfLevel(sound_in.I64());
        sound.remote_filter = sound_in.U8();
        sound.pan_mode = sound_in.U8();
        sound.pan_curve = sound_in.U8();
        sound.player_priority = sound_in.U8();
        sound.actor_player_id = sound_in.U8();
        sound.channel_priority = sound_in.U8();
        sound_in.Take(2);
        sound.user_params[0] = sound_in.U32();
        sound.user_params[1] = sound_in.U32();
        sound.settings_3d.flags = sound_in.U32();
        sound.settings_3d.decay_curve = sound_in.U8();
        sound.settings_3d.decay_ratio = sound_in.U8();
        sound.settings_3d.doppler_factor = sound_in.U8();
        sound_in.U8();

        const bool start_item = (sound_flags & kStartItem) != 0;
        if (start_item != (sound.type == rsar::SoundType::kSequence && model.sequences.contains(sound.file_id)))
        {
            Damaged("sound " + std::to_string(s) + " starts at an item only when its file holds a sequence");
        }
        switch (sound.type)
        {
        case rsar::SoundType::kSequence:
            {
                const uint32_t start = sound_in.U32();
                if (start_item)
                {
                    model.starts[s] = start;
                }
                else
                {
                    sound.start_offset = start;
                }
                sound.bank = sound_in.U32();
                sound.allocate_track_flags = sound_in.U32();
                break;
            }

        case rsar::SoundType::kWaveSound:
            sound.wave_sound_index = static_cast<int32_t>(sound_in.U32());
            sound.allocate_track_count = sound_in.U32();
            break;

        default:
            sound.stream_start = sound_in.U32();
            sound.allocate_channel_count = sound_in.U16();
            sound.stream_track_flags = sound_in.U16();
            break;
        }
    }

    return model;
}

} // namespace

Model ModelOf(const File& file)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file holds no archive.");
    }
    if (file.native->wii)
    {
        return WiiModelOf(file);
    }

    // The archive that the sequences without the encoder's choices build, and its items.
    File plain = file;
    plain.sequences = WithoutChoices(file.sequences);
    const std::vector<uint8_t> bytes = ExportArchive(plain);
    const sar::Flavor flavor = file.native->flavor;
    RequireWhole(bytes, 12, flavor == sar::Flavor::kCafe);
    Model model;
    model.archive = sar::ReadArchiveModel(bytes, flavor);

    // The sequence that each file holds. The FILE block's copy of a sequence file takes its bytes from the sequence.
    sar::Options options;
    options.flavor = flavor;
    const sar::SoundArchive read = sar::SoundArchive::Load(bytes, options);
    const std::vector<std::optional<uint32_t>> sequences = SequencesOfFiles(plain, bytes, read);
    const std::map<uint32_t, uint32_t> versions = SequenceVersions(*file.native);
    for (uint32_t id = 0; id < sequences.size() && id < model.archive.files.size(); ++id)
    {
        if (!sequences[id])
        {
            continue;
        }

        model.sequences[id] = {*sequences[id], versions.at(*sequences[id])};
        sar::FileModel& held = model.archive.files[id];
        if (held.place == sar::FilePlace::kBytes)
        {
            held.bytes.clear();
        }
    }

    // Each sequence sound whose file holds a sequence starts at an item of the sequence.
    std::map<uint32_t, std::vector<uint32_t>> offsets;
    for (uint32_t s = 0; s < model.archive.sounds.size(); ++s)
    {
        sar::SoundModel& sound = model.archive.sounds[s];
        const auto held = model.sequences.find(sound.file_id);
        if (sound.type != sar::SoundType::kSequence || held == model.sequences.end())
        {
            continue;
        }

        const uint32_t sequence = held->second.sequence;
        auto known = offsets.find(sequence);
        if (known == offsets.end())
        {
            known = offsets
                        .emplace(sequence,
                                 ntdlib::nwsnd::EncodeSequence(*plain.sequences[sequence], GrammarOf(flavor)).offsets)
                        .first;
        }
        const std::vector<uint32_t>& items = known->second;
        const auto item = std::lower_bound(items.begin(), items.end(), sound.start_offset);
        if (item == items.end() || *item != sound.start_offset)
        {
            throw std::invalid_argument("sound " + std::to_string(s) + " starts inside a command of sequence " +
                                        std::to_string(sequence));
        }

        model.starts[s] = static_cast<uint32_t>(item - items.begin());
        sound.start_offset = 0;
    }

    model.group_files = ExportGroupFiles(plain);
    for (const NativeGroupFile& group_file : file.native->group_files)
    {
        model.group_file_groups.push_back(group_file.group);
    }
    for (const auto& [id, stream] : file.native->streams)
    {
        model.streams[id] = *stream.bytes;
    }

    return model;
}

File WithModel(File file, const Model& model)
{
    if (model.wii)
    {
        return WithWiiModel(std::move(file), model);
    }

    // Each sequence file's bytes, and each sequence sound's start.
    sar::ArchiveModel archive = model.archive;
    const sar::Flavor flavor = archive.flavor;
    std::map<uint32_t, std::vector<uint32_t>> offsets;
    std::vector<sar::GroupMember> sequence_files;
    for (const auto& [id, held] : model.sequences)
    {
        if (id >= archive.files.size() || held.sequence >= file.sequences.size())
        {
            Damaged("file " + std::to_string(id) + " of the model names no file or no sequence");
        }

        std::pair<std::vector<uint8_t>, std::vector<uint32_t>> written;
        try
        {
            written = SequenceFileOf(*file.sequences[held.sequence], flavor, held.version);
        }
        catch (const std::exception& error)
        {
            throw xsflib::FormatError("Sequence " + std::to_string(held.sequence) +
                                      " can't be written for the archive's console. " + AsSentence(error.what()));
        }
        sequence_files.push_back({id, written.first});
        if (archive.files[id].place == sar::FilePlace::kBytes)
        {
            archive.files[id].bytes = std::move(written.first);
        }
        offsets[held.sequence] = std::move(written.second);
    }
    for (const auto& [s, item] : model.starts)
    {
        const auto held =
            s < archive.sounds.size() ? model.sequences.find(archive.sounds[s].file_id) : model.sequences.end();
        if (held == model.sequences.end() || archive.sounds[s].type != sar::SoundType::kSequence)
        {
            Damaged("sound " + std::to_string(s) + " of the model starts in a sequence without a sequence sound");
        }

        const std::vector<uint32_t>& items = offsets.at(held->second.sequence);
        if (item + 1 >= items.size())
        {
            Damaged("sound " + std::to_string(s) + " of the model starts past its sequence's items");
        }
        archive.sounds[s].start_offset = items[item];
    }

    // The archive that the model lays out, cut as an import cuts a game's archive.
    std::vector<uint8_t> bytes;
    try
    {
        bytes = sar::BuildArchive(archive);
    }
    catch (const std::invalid_argument& error)
    {
        throw xsflib::FormatError(std::string("The model's archive can't be laid out: ") + error.what() + ".");
    }

    // The group files' copies of the sequence files take the sequences' bytes too. A sequence file that only group
    // files hold has its copies there.
    std::vector<std::vector<uint8_t>> group_files;
    for (const std::vector<uint8_t>& group_file : model.group_files)
    {
        group_files.push_back(WithSequenceCopies(group_file, flavor, sequence_files));
    }
    std::map<uint32_t, std::vector<uint8_t>> streams = model.streams;
    File built =
        ImportArchive(bytes, flavor, std::move(group_files), std::move(streams), {}, KnownOf(file, model.sequences));

    // The cut's sequences are the file's sequences, in another order.
    sar::Options options;
    options.flavor = flavor;
    const sar::SoundArchive read = sar::SoundArchive::Load(bytes, options);
    const std::vector<std::optional<uint32_t>> cut = SequencesOfFiles(built, bytes, read);
    std::map<uint32_t, uint32_t> map;
    for (const auto& [id, held] : model.sequences)
    {
        const std::optional<uint32_t> found = id < cut.size() ? cut[id] : std::nullopt;
        if (!found || map.emplace(*found, held.sequence).first->second != held.sequence)
        {
            Damaged("the model's file " + std::to_string(id) + " doesn't cut as sequence " +
                    std::to_string(held.sequence));
        }
    }
    for (uint32_t c = 0; c < built.sequences.size(); ++c)
    {
        const auto mapped = map.find(c);
        if (mapped == map.end() || !(*built.sequences[c] == *file.sequences[mapped->second]))
        {
            Damaged("a sequence file of the model doesn't give its sequence back");
        }
    }

    // The native archive names the file's sequences. Its group files and streams are Hokuto's.
    NativeArchive native = *built.native;
    native.pieces = Renamed(*native.pieces, map);
    native.fields = Renamed(std::move(native.fields), map);
    for (std::size_t g = 0; g < native.group_files.size(); ++g)
    {
        NativeGroupFile& group_file = native.group_files[g];
        group_file.pieces = Renamed(*group_file.pieces, map);
        group_file.fields = Renamed(std::move(group_file.fields), map);
        group_file.game_size = 0;
        group_file.game_hash = {};
        if (g < model.group_file_groups.size() && model.group_file_groups[g])
        {
            group_file.group = model.group_file_groups[g];
        }
    }
    for (auto& [id, stream] : native.streams)
    {
        stream.from_game = false;
    }
    native.game_sizes.reset();
    file.native = std::move(native);
    file.form = ArchiveForm::kModel;

    return file;
}

ntdlib::nwsnd::Grammar GrammarOf(const Model& model)
{
    if (model.wii)
    {
        return ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(
            model.wii_build ? *model.wii_build : BuildOfWiiVersion(model.wii->version));
    }

    return GrammarOf(model.archive.flavor);
}

File InModelForm(File file)
{
    const Model model = ModelOf(file);
    file.sequences = WithoutChoices(file.sequences);

    return WithModel(std::move(file), model);
}

File NewFile(Engine engine)
{
    Model model;
    switch (engine)
    {
    case Engine::kWiiU:
        model.archive.flavor = sar::Flavor::kCafe;
        model.archive.version = kOldestWiiUArchive;
        break;

    case Engine::k3ds:
        model.archive.flavor = sar::Flavor::kCtr;
        model.archive.version = kOldest3dsArchive;
        break;

    case Engine::kSwitch:
        model.archive.flavor = sar::Flavor::kNx;
        model.archive.version = kOldestSwitchArchive;
        break;

    case Engine::kWii:
        model.wii.emplace();
        model.wii->version = kNewestWiiArchive;
        model.wii->players.push_back({.name = std::string(kDefaultPlayer), .playable_sound_count = 1});
        break;
    }

    if (!model.wii)
    {
        model.archive.players.push_back({.name = std::string(kDefaultPlayer), .playable_sound_max = 1});
    }

    File file;
    file.engine = engine;

    return WithModel(std::move(file), model);
}

std::vector<Shared<Sequence>> WithoutChoices(const std::vector<Shared<Sequence>>& sequences)
{
    std::vector<Shared<Sequence>> plain;
    for (const Shared<Sequence>& sequence : sequences)
    {
        const std::vector<ntdlib::commands::Command>& items = sequence->items;
        plain.push_back(std::any_of(items.begin(), items.end(), HasChoices) ? Shared<Sequence>(Plain(*sequence))
                                                                            : sequence);
    }

    return plain;
}

std::vector<uint8_t> SoundsChunk(const Model& model)
{
    if (model.wii)
    {
        return WiiSoundsChunk(model);
    }

    Out out;
    out.Count(model.archive.sounds.size(), "sounds");
    for (uint32_t s = 0; s < model.archive.sounds.size(); ++s)
    {
        const sar::SoundModel& sound = model.archive.sounds[s];
        const auto start = model.starts.find(s);
        uint8_t flags = 0;
        flags |= sound.release_priority_fix ? kReleasePriorityFix : 0;
        flags |= sound.single_play ? kSinglePlay : 0;
        flags |= sound.option_17 ? kOption17 : 0;
        flags |= start != model.starts.end() ? kStartItem : 0;
        flags |= sound.stream_extension ? kExtension : 0;
        uint8_t users = 0;
        for (std::size_t i = 0; i < sound.user_options.size(); ++i)
        {
            users |= sound.user_options[i] ? 1 << i : 0;
        }

        out.Name(sound.name, "bytes in a sound's name");
        out.U8(CodeOf(sound.type));
        out.U8(flags);
        out.U8(users);
        out.U8(0);
        out.U32(sound.file_id);
        out.U32(sound.player_id);
        out.I64(LevelOfVolume(sound.volume));
        out.U8(sound.remote_filter);
        out.U8(sound.pan_mode);
        out.U8(sound.pan_curve);
        out.U8(sound.player_priority);
        out.U8(sound.actor_player_id);
        out.U8(sound.channel_priority);
        out.Zeros(2);
        if (sound.single_play)
        {
            out.U32(*sound.single_play);
        }
        if (sound.option_17)
        {
            out.U32(*sound.option_17);
        }
        for (const std::optional<uint32_t>& option : sound.user_options)
        {
            if (option)
            {
                out.U32(*option);
            }
        }
        out.U32(sound.settings_3d.flags);
        out.F32(sound.settings_3d.decay_ratio);
        out.U8(sound.settings_3d.decay_curve);
        out.U8(sound.settings_3d.doppler_factor);
        out.Zeros(2);
        out.U32(sound.after_3d);

        // The details of the sound's kind.
        switch (sound.type)
        {
        case sar::SoundType::kSequence:
            out.U32(start != model.starts.end() ? start->second : sound.start_offset);
            out.U32(sound.allocate_track_flags);
            WriteList(out, sound.banks, "banks of a sound");
            break;

        case sar::SoundType::kWaveSound:
            out.U32(sound.wave_sound_index);
            break;

        case sar::SoundType::kStream:
            WriteStream(out, sound);
            break;

        default:
            throw std::invalid_argument("sound " + std::to_string(s) + " is of an unknown kind");
        }
    }

    return out.Take();
}

std::vector<uint8_t> ItemsChunk(const Model& model)
{
    if (model.wii)
    {
        return WiiItemsChunk(model);
    }

    const sar::ArchiveModel& archive = model.archive;
    Out out;
    out.U8(CodeOf(archive.flavor));
    out.Zeros(3);
    out.U32(archive.version);
    const sar::PlayerSettings& settings = archive.player_settings;
    for (const uint16_t count :
         {settings.sequence_sounds, settings.sequence_tracks, settings.stream_sounds, settings.stream_tracks,
          settings.stream_channels, settings.wave_sounds, settings.wave_tracks})
    {
        out.U16(count);
    }
    out.U8(settings.stream_buffer_times);
    out.U8(0);

    out.Count(archive.sound_groups.size(), "sound groups");
    for (const sar::SoundGroupModel& group : archive.sound_groups)
    {
        out.Name(group.name, "bytes in a sound group's name");
        out.U32(group.first_sound);
        out.U32(group.last_sound);
        WriteList(out, group.file_ids, "files of a sound group");
        out.U8(group.wave_archives ? kWaveArchives : 0);
        out.Zeros(3);
        if (group.wave_archives)
        {
            WriteList(out, *group.wave_archives, "wave archives of a sound group");
        }
    }

    out.Count(archive.banks.size(), "banks");
    for (const sar::BankModel& bank : archive.banks)
    {
        out.Name(bank.name, "bytes in a bank's name");
        out.U32(bank.file_id);
        WriteList(out, bank.wave_archives, "wave archives of a bank");
    }

    out.Count(archive.wave_archives.size(), "wave archives");
    for (const sar::WaveArchiveModel& wave_archive : archive.wave_archives)
    {
        out.Name(wave_archive.name, "bytes in a wave archive's name");
        out.U32(wave_archive.file_id);
        out.U8((wave_archive.load_individually ? kLoadIndividually : 0) | (wave_archive.wave_count ? kWaveCount : 0));
        out.Zeros(3);
        out.U32(wave_archive.wave_count.value_or(0));
    }

    out.Count(archive.groups.size(), "groups");
    for (const sar::GroupModel& group : archive.groups)
    {
        out.Name(group.name, "bytes in a group's name");
        out.U32(group.file_id);
    }

    out.Count(archive.players.size(), "players");
    for (const sar::PlayerModel& player : archive.players)
    {
        out.Name(player.name, "bytes in a player's name");
        out.U32(player.playable_sound_max);
        out.U32(player.heap_size);
    }

    // The files: each one's place and flags, its groups, then its sequence, its bytes, its group or its path.
    out.Count(archive.files.size(), "files");
    for (uint32_t id = 0; id < archive.files.size(); ++id)
    {
        const sar::FileModel& held = archive.files[id];
        const auto sequence = model.sequences.find(id);
        const auto beside = model.streams.find(id);
        const bool has_sequence = sequence != model.sequences.end();
        const bool has_bytes = held.place == sar::FilePlace::kPath && beside != model.streams.end();
        uint8_t place = kBytesPlace;
        switch (held.place)
        {
        case sar::FilePlace::kBytes:
            place = kBytesPlace;
            break;
        case sar::FilePlace::kCopy:
            place = kCopyPlace;
            break;
        case sar::FilePlace::kGroupsOnly:
            place = kGroupsOnlyPlace;
            break;
        case sar::FilePlace::kPath:
            place = kPathPlace;
            break;
        }

        out.U8(place);
        out.U8((has_sequence ? kSequenceFile : 0) | (has_bytes ? kHeldFile : 0));
        out.Zeros(2);
        WriteList(out, held.groups, "groups of a file");
        if (has_sequence)
        {
            out.U32(sequence->second.sequence);
            out.U32(sequence->second.version);
        }
        switch (held.place)
        {
        case sar::FilePlace::kBytes:
            if (!has_sequence)
            {
                WriteBytes(out, held.bytes);
            }
            break;

        case sar::FilePlace::kCopy:
            out.U32(held.copy_group);
            break;

        case sar::FilePlace::kGroupsOnly:
            break;

        case sar::FilePlace::kPath:
            out.Name(held.path, "bytes in a file's path");
            if (has_bytes)
            {
                WriteBytes(out, beside->second);
            }
            break;
        }
    }

    out.Count(model.group_files.size(), "group files");
    for (const std::vector<uint8_t>& group_file : model.group_files)
    {
        WriteBytes(out, group_file);
    }
    for (std::size_t g = 0; g < model.group_files.size(); ++g)
    {
        const bool known = g < model.group_file_groups.size() && model.group_file_groups[g];
        out.U32(known ? *model.group_file_groups[g] : kNoGroup);
    }

    return out.Take();
}

Model ReadModel(std::span<const uint8_t> sounds, std::span<const uint8_t> items)
{
    Model model;
    sar::ArchiveModel& archive = model.archive;
    In in(items, "the ITMS chunk");
    const uint8_t code = in.U8();
    if (code == kWiiArchive)
    {
        return ReadWiiModel(sounds, in);
    }
    const std::optional<sar::Flavor> flavor = FlavorOfCode(code);
    if (!flavor)
    {
        Newer("a kind of archive (" + std::to_string(code) + ")");
    }
    archive.flavor = *flavor;
    in.Take(3);
    archive.version = in.U32();
    sar::PlayerSettings& settings = archive.player_settings;
    for (uint16_t* count :
         {&settings.sequence_sounds, &settings.sequence_tracks, &settings.stream_sounds, &settings.stream_tracks,
          &settings.stream_channels, &settings.wave_sounds, &settings.wave_tracks})
    {
        *count = in.U16();
    }
    settings.stream_buffer_times = in.U8();
    in.U8();

    const uint32_t sound_groups = in.U32();
    for (uint32_t i = 0; i < sound_groups; ++i)
    {
        sar::SoundGroupModel& group = archive.sound_groups.emplace_back();
        group.name = in.Name();
        group.first_sound = in.U32();
        group.last_sound = in.U32();
        group.file_ids = ReadList(in);
        const uint8_t flags = in.U8();
        if ((flags & ~kWaveArchives) != 0)
        {
            Newer("a flag of a sound group");
        }
        in.Take(3);
        if ((flags & kWaveArchives) != 0)
        {
            group.wave_archives = ReadList(in);
        }
    }

    const uint32_t banks = in.U32();
    for (uint32_t i = 0; i < banks; ++i)
    {
        sar::BankModel& bank = archive.banks.emplace_back();
        bank.name = in.Name();
        bank.file_id = in.U32();
        bank.wave_archives = ReadList(in);
    }

    const uint32_t wave_archives = in.U32();
    for (uint32_t i = 0; i < wave_archives; ++i)
    {
        sar::WaveArchiveModel& wave_archive = archive.wave_archives.emplace_back();
        wave_archive.name = in.Name();
        wave_archive.file_id = in.U32();
        const uint8_t flags = in.U8();
        if ((flags & ~(kLoadIndividually | kWaveCount)) != 0)
        {
            Newer("a flag of a wave archive");
        }
        in.Take(3);
        wave_archive.load_individually = (flags & kLoadIndividually) != 0;
        const uint32_t count = in.U32();
        if ((flags & kWaveCount) != 0)
        {
            wave_archive.wave_count = count;
        }
    }

    const uint32_t groups = in.U32();
    for (uint32_t i = 0; i < groups; ++i)
    {
        sar::GroupModel& group = archive.groups.emplace_back();
        group.name = in.Name();
        group.file_id = in.U32();
    }

    const uint32_t players = in.U32();
    for (uint32_t i = 0; i < players; ++i)
    {
        sar::PlayerModel& player = archive.players.emplace_back();
        player.name = in.Name();
        player.playable_sound_max = in.U32();
        player.heap_size = in.U32();
    }

    const uint32_t files = in.U32();
    for (uint32_t id = 0; id < files; ++id)
    {
        sar::FileModel& held = archive.files.emplace_back();
        const uint8_t place = in.U8();
        const uint8_t flags = in.U8();
        if (place > kPathPlace)
        {
            Newer("a place of a file (" + std::to_string(place) + ")");
        }
        if ((flags & ~kFileFlags) != 0)
        {
            Newer("a flag of a file");
        }
        in.Take(2);
        held.groups = ReadList(in);
        if ((flags & kSequenceFile) != 0)
        {
            ModelSequence& sequence = model.sequences[id];
            sequence.sequence = in.U32();
            sequence.version = in.U32();
        }
        switch (place)
        {
        case kBytesPlace:
            held.place = sar::FilePlace::kBytes;
            if ((flags & kSequenceFile) == 0)
            {
                held.bytes = ReadBytes(in);
            }
            break;

        case kCopyPlace:
            held.place = sar::FilePlace::kCopy;
            held.copy_group = in.U32();
            break;

        case kGroupsOnlyPlace:
            held.place = sar::FilePlace::kGroupsOnly;
            break;

        default:
            held.place = sar::FilePlace::kPath;
            held.path = in.Name();
            if ((flags & kHeldFile) != 0)
            {
                model.streams[id] = ReadBytes(in);
            }
            break;
        }
        if ((flags & kHeldFile) != 0 && held.place != sar::FilePlace::kPath)
        {
            Damaged("file " + std::to_string(id) + " holds the bytes of a file beside the archive");
        }
    }

    const uint32_t group_files = in.U32();
    for (uint32_t i = 0; i < group_files; ++i)
    {
        model.group_files.push_back(ReadBytes(in));
    }

    // Each group file's group. A chunk that ends after the group files gives none: the group files then take the groups
    // that their copies give.
    if (in.Left() > 0)
    {
        for (uint32_t i = 0; i < group_files; ++i)
        {
            const uint32_t group = in.U32();
            model.group_file_groups.push_back(group == kNoGroup ? std::nullopt : std::optional<uint32_t>(group));
        }
    }

    // The sounds.
    In sound_in(sounds, "the SNDS chunk");
    const uint32_t count = sound_in.U32();
    for (uint32_t s = 0; s < count; ++s)
    {
        sar::SoundModel& sound = archive.sounds.emplace_back();
        sound.name = sound_in.Name();
        const uint8_t kind = sound_in.U8();
        const std::optional<sar::SoundType> type = SoundTypeOfCode(kind);
        if (!type || *type == sar::SoundType::kUnknown)
        {
            Newer("a kind of sound (" + std::to_string(kind) + ")");
        }

        sound.type = *type;
        const uint8_t flags = sound_in.U8();
        const uint8_t users = sound_in.U8();
        if ((flags & ~kSoundFlags) != 0 || (users & ~kUserFlags) != 0)
        {
            Newer("a flag of a sound");
        }
        sound_in.U8();
        sound.release_priority_fix = (flags & kReleasePriorityFix) != 0;
        sound.file_id = sound_in.U32();
        sound.player_id = sound_in.U32();
        sound.volume = VolumeOfLevel(sound_in.I64());
        sound.remote_filter = sound_in.U8();
        sound.pan_mode = sound_in.U8();
        sound.pan_curve = sound_in.U8();
        sound.player_priority = sound_in.U8();
        sound.actor_player_id = sound_in.U8();
        sound.channel_priority = sound_in.U8();
        sound_in.Take(2);
        if ((flags & kSinglePlay) != 0)
        {
            sound.single_play = sound_in.U32();
        }
        if ((flags & kOption17) != 0)
        {
            sound.option_17 = sound_in.U32();
        }
        for (std::size_t i = 0; i < sound.user_options.size(); ++i)
        {
            if ((users >> i & 1) != 0)
            {
                sound.user_options[i] = sound_in.U32();
            }
        }
        sound.settings_3d.flags = sound_in.U32();
        sound.settings_3d.decay_ratio = sound_in.F32();
        sound.settings_3d.decay_curve = sound_in.U8();
        sound.settings_3d.doppler_factor = sound_in.U8();
        sound_in.Take(2);
        sound.after_3d = sound_in.U32();

        const bool start_item = (flags & kStartItem) != 0;
        if (start_item != (sound.type == sar::SoundType::kSequence && model.sequences.contains(sound.file_id)))
        {
            Damaged("sound " + std::to_string(s) + " starts at an item only when its file holds a sequence");
        }
        if ((flags & kExtension) != 0 && sound.type != sar::SoundType::kStream)
        {
            Damaged("sound " + std::to_string(s) + " holds a stream sound's extension");
        }
        switch (sound.type)
        {
        case sar::SoundType::kSequence:
            {
                const uint32_t start = sound_in.U32();
                if (start_item)
                {
                    model.starts[s] = start;
                }
                else
                {
                    sound.start_offset = start;
                }
                sound.allocate_track_flags = sound_in.U32();
                sound.banks = ReadList(sound_in);
                break;
            }

        case sar::SoundType::kWaveSound:
            sound.wave_sound_index = sound_in.U32();
            break;

        default:
            ReadStream(sound_in, sound, (flags & kExtension) != 0);
            break;
        }
    }

    return model;
}

Fixed LevelOfVolume(uint8_t volume)
{
    return std::llround(std::sqrt(127.0 * volume) * static_cast<double>(kOne));
}

uint8_t VolumeOfLevel(Fixed level)
{
    const double gain = static_cast<double>(level) / static_cast<double>(kOne);
    return static_cast<uint8_t>(std::clamp<long long>(std::llround(gain * gain / 127.0), 0, 255));
}

} // namespace hokuto::format
