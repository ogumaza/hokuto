// SPDX-License-Identifier: MIT

// The parts of the binary form that its files share: the flags and constants of the chunks, and the writers and
// readers of the chunks that Write and Read call. Only file.cpp, file_sequences.cpp, file_chunks.cpp and
// file_checks.cpp include it.

#pragma once

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
#include "format/file.h"
#include "format/model.h"

namespace hokuto::format::file_parts
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The byte of the view that stands for no track.
constexpr uint8_t kNoTrack = 0xFF;

// The highest program of a bank that a bank's instrument map names.
constexpr uint32_t kMostBankMapProgram = 0xFFFF;

// The flag of the build of NW4R SND that comes with AX of October 2006, in the BULD chunk.
constexpr uint8_t kOctober2006 = 1;

// The flags of a GBA ROM in the NATV chunk: songs that can go into its filler runs, and notes of MusicPlayer2000 that
// read two bytes after their code (NativeArchive::gba_note_arguments).
constexpr uint8_t kGbaFiller = 1;

constexpr uint8_t kGbaTwoByteNotes = 2;

// The flags of a sound's lengths in the LNTH chunk: a sound that never makes a sound, and the ticks of a loop.
constexpr uint8_t kSilentSound = 1;

constexpr uint8_t kLoopTicks = 2;

// The flags of an item. The flags say which of the item's parts follow its index.
constexpr uint16_t kConditional = 1 << 0;

constexpr uint16_t kHasValue = 1 << 1;

constexpr int kValueSourceShift = 2; // two bits: the value's Source

constexpr uint16_t kHasTime = 1 << 4;

constexpr int kTimeSourceShift = 5; // two bits: the time's Source

constexpr uint16_t kHasNote = 1 << 7;

constexpr uint16_t kHasTarget = 1 << 8;

constexpr uint16_t kTargetOutside = 1 << 9;

constexpr uint16_t kHasOpcode = 1 << 10;

constexpr uint16_t kHasBytes = 1 << 11;

constexpr uint16_t kHasSample = 1 << 12;

constexpr uint16_t kItemFlags = (1 << 13) - 1;

// The types of the native archive's pieces.
constexpr uint8_t kBytesPiece = 0;

constexpr uint8_t kSequencePiece = 1;

constexpr uint8_t kPaddingPiece = 2;

// The flags of a place in the native archive.
constexpr uint8_t kItemPlace = 1 << 0;

constexpr uint8_t kSequencePlace = 1 << 1;

constexpr uint8_t kTwoByteField = 1 << 2; // on the place of a field: the field is 16 bits wide

// The flags of an item's encoding.
constexpr uint8_t kUnusedRandom = 1 << 0;

constexpr uint8_t kUnusedVariable = 1 << 1;

// The flags of an item's encoding in a GBA ROM or a set of DSE's files.
constexpr uint8_t kRepeated = 1 << 0;

// The flags of a sound in the native archive's tables.
constexpr uint8_t kReleasePriorityFix = 1 << 0;

// The group of a group file whose group Hokuto couldn't find.
constexpr uint32_t kNoGroup = 0xFFFFFFFF;

using binary::Damaged;
using binary::In;
using binary::Newer;
using binary::Out;

// The chunks of the sequences and of the native archive (file_sequences.cpp).
std::vector<uint8_t> SequencesChunk(const File& file);
std::vector<Sequence> ReadSequences(std::span<const uint8_t> chunk);
std::vector<uint8_t> NativeChunk(const File& file);
NativeArchive ReadNative(std::span<const uint8_t> chunk, std::vector<Sequence>& sequences);

// The other chunks (file_chunks.cpp).
std::vector<uint8_t> TablesChunk(const NativeTables& tables);
NativeTables ReadTables(std::span<const uint8_t> chunk);
std::vector<uint8_t> BuildChunk(const ntdlib::SndBuild& build);
ntdlib::SndBuild ReadBuild(std::span<const uint8_t> chunk);
std::vector<uint8_t> SizesChunk(const GameSizes& sizes);
GameSizes ReadSizes(std::span<const uint8_t> chunk);
bool NotUnity(float volume);
std::vector<uint8_t> MixerChunk(const File& file);
void ReadMixer(std::span<const uint8_t> chunk, File& file);
std::vector<uint8_t> ScenesChunk(const File& file);
void ReadScenes(std::span<const uint8_t> chunk, File& file);
std::vector<uint8_t> TrackColoursChunk(const File& file);
void ReadTrackColours(std::span<const uint8_t> chunk, File& file);
std::vector<uint8_t> InstrumentMapsChunk(const File& file);
void ReadInstrumentMaps(std::span<const uint8_t> chunk, File& file);
std::vector<uint8_t> BankMapsChunk(const File& file);
void ReadBankMaps(std::span<const uint8_t> chunk, File& file);
std::vector<uint8_t> EditorChunk(const EditorView& view);
void ReadEditor(std::span<const uint8_t> chunk, File& file);
std::vector<uint8_t> TagsChunk(const File& file);
void ReadTags(std::span<const uint8_t> chunk, File& file);
std::vector<uint8_t> LengthsChunk(const File& file);
void ReadLengths(std::span<const uint8_t> chunk, File& file);
std::vector<uint8_t> TimelinesChunk(const File& file);
void ReadTimelines(std::span<const uint8_t> chunk, File& file);
std::vector<uint8_t> EngineChunk(const File& file);
void ReadEngine(std::span<const uint8_t> chunk, File& file);

} // namespace hokuto::format::file_parts
