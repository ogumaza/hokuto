// SPDX-License-Identifier: MIT

// What the files of the model form share (format/model.h): the codes and the flags of the SNDS and ITMS chunks, the
// helpers that read and write them, and the Wii's model (model_wii.cpp). Only model.cpp, model_chunks.cpp and
// model_wii.cpp include it.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/layout.h"

#include "format/binary.h"
#include "format/file.h"
#include "format/model.h"

namespace hokuto::format::model_parts
{

// The flags of a sound in the SNDS chunk. The flags of the user's parameters follow in a byte of their own.
inline constexpr uint8_t kReleasePriorityFix = 1 << 0;
inline constexpr uint8_t kSinglePlay = 1 << 1;
inline constexpr uint8_t kOption17 = 1 << 2;
inline constexpr uint8_t kStartItem = 1 << 3;
inline constexpr uint8_t kExtension = 1 << 4;
inline constexpr uint8_t kSoundFlags = (1 << 5) - 1;
inline constexpr uint8_t kUserFlags = (1 << 4) - 1;

// The places of a file in the ITMS chunk, and the flags of a file.
inline constexpr uint8_t kBytesPlace = 0;
inline constexpr uint8_t kCopyPlace = 1;
inline constexpr uint8_t kGroupsOnlyPlace = 2;
inline constexpr uint8_t kPathPlace = 3;
inline constexpr uint8_t kSequenceFile = 1 << 0; // the file holds one of the file's sequences
inline constexpr uint8_t kHeldFile = 1 << 1;     // the chunk holds the bytes of a file beside the archive
inline constexpr uint8_t kFileFlags = (1 << 2) - 1;

// The kind of the Wii's archives in the ITMS chunk, the chunk's flags on the Wii, and the flags of a file there.
inline constexpr uint8_t kWiiArchive = 4;
inline constexpr uint8_t kWiiBuild = 1 << 0;     // the file keeps the game's build of NW4R SND
inline constexpr uint8_t kAlignedTrees = 1 << 1; // the trees of the archive's SYMB block start on a multiple of 4 bytes
inline constexpr uint8_t kUnterminatedPaths = 1 << 2; // a path whose length is a multiple of 4 takes no zero after it
inline constexpr uint8_t kWiiItemFlags = (1 << 3) - 1;
inline constexpr uint8_t kOctober2006 = 1 << 0; // the build that comes with AX of October 2006 (bit 0 in BULD too)
inline constexpr uint8_t kWiiPathFile = 1 << 1; // the file lies beside the archive
inline constexpr uint8_t kWiiHeldFile = 1 << 2; // the chunk holds the bytes of the file beside the archive
inline constexpr uint8_t kWiiFileFlags = (1 << 3) - 1;
inline constexpr uint8_t kWiiSoundFlags = kReleasePriorityFix | kStartItem;

// The helpers of model.cpp that model_chunks.cpp and model_wii.cpp share.
ntdlib::sar::KnownSequences KnownOf(const File& file, const std::map<uint32_t, ModelSequence>& sequences);
void RequireWhole(std::span<const uint8_t> bytes, std::size_t at, bool big_endian);
std::map<uint32_t, uint32_t> SequenceVersions(const NativeArchive& native);
std::vector<ntdlib::sar::Piece> Renamed(std::vector<ntdlib::sar::Piece> pieces,
                                        const std::map<uint32_t, uint32_t>& map);
std::vector<ntdlib::sar::Field> Renamed(std::vector<ntdlib::sar::Field> fields,
                                        const std::map<uint32_t, uint32_t>& map);
std::map<uint32_t, uint32_t> CutOrder(const File& file, const File& built, const Model& model,
                                      const std::vector<std::optional<uint32_t>>& cut);
void WriteList(binary::Out& out, const std::vector<uint32_t>& values, const std::string& what);
std::vector<uint32_t> ReadList(binary::In& in);
void WriteBytes(binary::Out& out, std::span<const uint8_t> bytes);
std::vector<uint8_t> ReadBytes(binary::In& in);

// Sets the start of each sequence sound of `archive` that `model` lists. `offsets` holds the offset of each item of
// each sequence. `sequence` is the archive's type of a sequence sound.
template <typename Archive, typename SoundType>
void SetSequenceStarts(Archive& archive, const Model& model, const std::map<uint32_t, std::vector<uint32_t>>& offsets,
                       SoundType sequence)
{
    for (const auto& [s, item] : model.starts)
    {
        const auto held =
            s < archive.sounds.size() ? model.sequences.find(archive.sounds[s].file_id) : model.sequences.end();
        if (held == model.sequences.end() || archive.sounds[s].type != sequence)
        {
            binary::Damaged("sound " + std::to_string(s) +
                            " of the model starts in a sequence without a sequence sound");
        }

        const std::vector<uint32_t>& items = offsets.at(held->second.sequence);
        if (item + 1 >= items.size())
        {
            binary::Damaged("sound " + std::to_string(s) + " of the model starts past the end of its sequence");
        }

        archive.sounds[s].start_offset = items[item];
    }
}

// The reverse of SetSequenceStarts: each sequence sound of `archive` whose file contains a sequence of `model` starts
// at an item of the sequence. Sets the sound's item in `model` and its offset to 0. `plain` contains the sequences.
// `grammar` encodes them. `sequence` is the archive's type of a sequence sound.
template <typename Archive, typename SoundType>
void TakeSequenceStarts(Archive& archive, Model& model, const File& plain, const ntdlib::nwsnd::Grammar& grammar,
                        SoundType sequence_type)
{
    std::map<uint32_t, std::vector<uint32_t>> offsets;
    for (uint32_t s = 0; s < archive.sounds.size(); ++s)
    {
        auto& sound = archive.sounds[s];
        const auto held = model.sequences.find(sound.file_id);
        if (sound.type != sequence_type || held == model.sequences.end())
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
}

// The Wii's model (model_wii.cpp).
Model WiiModelOf(const File& file);
File WithWiiModel(File file, const Model& model);
std::vector<uint8_t> WiiSoundsChunk(const Model& model);
std::vector<uint8_t> WiiItemsChunk(const Model& model);
Model ReadWiiModel(std::span<const uint8_t> sounds, binary::In& in);

} // namespace hokuto::format::model_parts
