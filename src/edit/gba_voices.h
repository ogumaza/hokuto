// SPDX-License-Identifier: MIT

// The edits of a GBA ROM's voices: a voice's 12 bytes, and the 16 bytes of a programmable wave's pattern. MP2K's voice
// groups, voices and patterns sit at addresses in the ROM, and code and tables name those addresses. An edit therefore
// changes the bytes in place, in the run of the ROM that contains them (ntdlib/mp2k/layout.h). The ROM keeps its size.
//
// New samples, new patterns and voice groups with new programs are written into the free space of the ROM, where a song
// that outgrows its place moves too (ntdlib::mp2k::Join). They are in the last run of the file's ROM, and hold their
// addresses. A song that moves is written after them.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/song.h"

#include "format/file.h"
#include "song/gba.h"

namespace hokuto::edit
{

// The bytes of a voice, and the steps of a programmable wave's pattern. A pattern stores two 4-bit steps in each byte,
// the high one first.
inline constexpr std::size_t kGbaVoiceBytes = ntdlib::mp2k::kVoiceSize;
inline constexpr std::size_t kGbaPatternSteps = 32;
using GbaPattern = std::array<uint8_t, kGbaPatternSteps>;

// The highest values that MP2K's PSG voices take. A PSG voice's attack, decay and release count frames for each step of
// the PSG's 16 levels, and its sustain is one of those levels. A square wave's duty is one of four, and the noise's
// mode one of two. The length and the sweep go into the PSG's registers: the length into NRx1's 6 bits, and the sweep
// into NR10's 7.
inline constexpr int kHighestPsgStep = 7;
inline constexpr int kHighestPsgSustain = 15;
inline constexpr int kHighestDuty = 3;
inline constexpr int kHighestNoiseMode = 1;
inline constexpr int kHighestPsgLength = 63;
inline constexpr int kHighestSweep = 0x7F;

// The 12 bytes of `voice` as ntdlib::mp2k::ReadVoice reads them.
std::array<uint8_t, kGbaVoiceBytes> GbaVoiceBytes(const ntdlib::mp2k::Voice& voice);

// The steps of the pattern at `address` of `rom`. Throws std::invalid_argument when the pattern isn't in the ROM.
GbaPattern GbaPatternOf(const ntdlib::agb::Rom& rom, uint32_t address);

// `file`, a GBA file, with `bytes` at ROM address `address`. Throws std::invalid_argument with a message for the user
// when the bytes don't fall within one run of the file's ROM: a song's bytes and the addresses in the songs' headers
// and the song table hold them.
format::File WithGbaBytes(const format::File& file, uint32_t address, std::span<const uint8_t> bytes);

// `file`, a GBA file, with `voice` at its address. A key split or a drum kit keeps its tables. Another voice can take
// another kind (GbaVoiceOfKind). Throws std::invalid_argument with a message for the user for a value that MP2K's voice
// doesn't take, such as a PSG voice's attack past kHighestPsgStep, and as WithGbaBytes does. A value that the edit
// leaves unchanged isn't checked, in a voice that stays a PSG voice.
format::File WithGbaVoice(const format::File& file, const ntdlib::mp2k::Voice& voice);

// `file`, a GBA file, with `steps` in the pattern at `address`. Each step ranges from 0 to 15. Throws as WithGbaBytes
// does, and std::invalid_argument for a step past 15.
format::File WithGbaPattern(const format::File& file, uint32_t address, const GbaPattern& steps);

// The 16 bytes of `steps`. Throws std::invalid_argument for a step past 15.
std::array<uint8_t, kGbaPatternSteps / 2> GbaPatternBytes(const GbaPattern& steps);

// The pattern of a new wave voice: a triangle over its 32 steps.
GbaPattern NewGbaPattern();

// The bytes of a sample of 8-bit PCM from 16-bit `samples`, each rounded to 8 bits: MP2K's header, the samples, then
// one more that the mixer reads when it interpolates past the last one. `rate` is the rate at which key 60 plays it. A
// loop runs from `loop_start` to the sample's end. Throws std::invalid_argument with a message for the user for no
// samples, a rate of 0 or past 4 MHz, and a loop that starts at the end or past it.
std::vector<uint8_t> EncodeGbaSample(std::span<const int16_t> samples, uint32_t rate,
                                     std::optional<uint32_t> loop_start);

// `file`, a GBA file, with `bytes` at offset `offset` of its last run. The run grows, in `padding`, up to them.
format::File WithLastRunBytes(const format::File& file, std::size_t offset, std::span<const uint8_t> bytes,
                              uint8_t padding);

// A version of a file with new bytes, and the bytes' ROM address.
struct PlacedGbaBytes
{
    format::File file;
    uint32_t address = 0;
};

// `file`, a GBA file, with `bytes` in the ROM's free space: as ntdlib::mp2k::Join finds it, from a multiple of 256
// bytes in the run of 0xFF bytes at the ROM's end, or else on a multiple of 4 bytes after the ROM's end. Zeros follow
// them up to a multiple of 4 bytes, at least one: bytes that end in 0xFF stay out of the run. Past the 32 MiB that the
// GBA maps, the bytes go into the ROM's first filler run with room, where the file allows it
// (NativeArchive::gba_filler). Throws std::invalid_argument with a message for the user for a ROM without room.
PlacedGbaBytes WithGbaNewBytes(const format::File& file, std::span<const uint8_t> bytes);

// `voice` as a voice of `psg`, the PSG's channel from 1 to 4, or of a sample for 0. `wave` is the sample's address for
// a sample, and the pattern's for channel 3. The voice keeps its address, its key, a drum kit's pan and whether its
// pitch is fixed. Among the PSG's channels, it keeps its length and its envelope, and a square wave its duty. A new PSG
// voice starts at full level with a duty of 50 %, and a new sample with the envelope of a sample that holds. A voice
// that's a sample and stays one changes only its sample address. The new address is `wave`.
ntdlib::mp2k::Voice GbaVoiceOfKind(const ntdlib::mp2k::Voice& voice, int psg, uint32_t wave);

// A new voice that plays the sample at `sample`, at key 60.
ntdlib::mp2k::Voice NewGbaSampleVoice(uint32_t sample);

// A new voice of square wave 2.
ntdlib::mp2k::Voice NewGbaSquareVoice();

// The programs of the voice group at `group` of `rom` that a new voice can take: no song selects them, and their
// entries play nothing. `rom` is the ROM that the file builds.
std::vector<uint32_t> FreeGbaPrograms(const song::GbaRom& rom, uint32_t group);

// The programs of the voice group at `group` of `rom` that hold voices and that no song selects. A game's table can
// fill its programs with such voices.
std::vector<uint32_t> UnplayedGbaPrograms(const song::GbaRom& rom, uint32_t group);

// The programs of a voice group that a new voice can use (FreeGbaPrograms), and those with voices that no song
// selects (UnplayedGbaPrograms).
struct GbaOpenPrograms
{
    std::vector<uint32_t> free;
    std::vector<uint32_t> unplayed;
};

// The open programs of the voice group at `group` of `rom`, from one pass over the songs. `rom` is the ROM that the
// file builds.
GbaOpenPrograms GbaOpenProgramsOf(const song::GbaRom& rom, uint32_t group);

// A version of a file, and the address of the voice group that it edited.
struct GbaGroupVersion
{
    format::File file;
    uint32_t group = 0;
};

// `file`, a GBA file, with each voice of `voices` at its program of the voice group at `group`. `rom` is the ROM that
// the file builds. A table that is in the last run of the ROM and has only voices (song::GbaTableHoldsVoices) gets them
// in place. Any other table can end before the program, with other data after it. It is written into the free space
// first, with the entries that the songs select and those that play something, and zeros in the others. Each song's
// header then names the new table, and so does the file's instrument map of the group. Code that names the old table
// keeps it. Each voice gets its new address. Throws std::invalid_argument with a message for the user for a program
// past 127, a value that MP2K's voice doesn't take, and as WithGbaNewBytes does.
GbaGroupVersion WithGbaVoicesAt(const format::File& file, const song::GbaRom& rom, uint32_t group,
                                std::span<const std::pair<uint32_t, ntdlib::mp2k::Voice>> voices);

// `file`, a GBA file, with the voices of the file of assembly `text` (song::ReadGbaVoiceGroup) at their programs of the
// voice group at `group`. A table, a key table or a pattern of the file that contains the bytes of the ROM at the
// address that its label ends in stays there, and any other is written into the free space. For an unchanged file, the
// function therefore returns `file`. `rom` is the ROM that the file builds. Throws std::invalid_argument with a message
// for the user as song::ReadGbaVoiceGroup, WithGbaNewBytes and WithGbaVoicesAt do.
GbaGroupVersion WithGbaVoiceGroupFile(const format::File& file, const song::GbaRom& rom, uint32_t group,
                                      std::string_view text);

// `file`, a GBA file, with zeros in the 12 bytes of the voice at `address`. The program plays nothing then. Throws as
// WithGbaBytes does.
format::File WithoutGbaVoice(const format::File& file, uint32_t address);

// A run of keys of a key split or a drum kit, and the voice that plays them. A run of a drum kit has one key.
struct GbaKeyRow
{
    int first_key = 0;
    int last_key = 127;
    ntdlib::mp2k::Voice voice;
};

// `voice` as a key split or a drum kit, by `type` (ntdlib::mp2k::kVoiceKeySplit or kVoiceDrumKit), at the voice's
// address. Its tables come from WithGbaKeyRows.
ntdlib::mp2k::Voice GbaSplitOf(const ntdlib::mp2k::Voice& voice, uint8_t type);

// `rows` of a key split or a drum kit with one more. A new row of a drum kit gets the first key after `selected` that
// plays nothing, or else the first such key, and a copy of the voice of the selected row. A new row of a key split gets
// the upper half of the keys of the selected row, and a copy of its voice. Without rows, a new row is a square wave:
// every key of a key split, or key 60 of a drum kit. Throws std::invalid_argument with a message for the user for a
// drum kit whose keys all play, and for a selected row of a key split with one key.
std::vector<GbaKeyRow> GbaRowsWithNew(std::span<const GbaKeyRow> rows, bool drums, std::optional<int> selected);

// The rows of a key split without the row that starts at `first_key`. The row before it receives its keys, or else the
// row after it. Throws std::invalid_argument with a message for the user for the only row.
std::vector<GbaKeyRow> GbaSplitRowsWithout(std::span<const GbaKeyRow> rows, int first_key);

// The rows with the row that starts at `first_key` over the keys from `new_first` to `new_last`. A drum kit's row moves
// to one key. Throws std::invalid_argument with a message for the user for a key that another row of the drum kit
// plays. A key split's other rows give way to the row. A key that a row gave up plays nothing.
std::vector<GbaKeyRow> GbaRowsWithKeys(std::span<const GbaKeyRow> rows, bool drums, int first_key, int new_first,
                                       int new_last);

// `file`, a GBA file, with the voice at `split`'s address as `split`, with new tables in the ROM's free space that hold
// `rows`. For a key split, the new tables are a table of the voices of the rows, with an empty voice after them for the
// keys that no row plays, and a key table of 128 bytes. For a drum kit, the new table has 128 voices: the voice of each
// row at its key, and zeros at the others. The old tables stay: another voice or code can name them. The values of the
// rows are written as they are: a row can have any value that a voice of a game has. Throws std::invalid_argument with
// a message for the user for rows that overlap or extend past key 127, a drum kit's row of more than one key, a key
// split without rows, a row that plays a key split or a drum kit, and as WithGbaNewBytes does.
format::File WithGbaKeyRows(const format::File& file, const ntdlib::mp2k::Voice& split,
                            std::span<const GbaKeyRow> rows);

// `file`, a GBA file, with `voice` at `address`, whatever kind of item is at the address. A key split or a drum kit
// that a single voice replaces leaves its tables behind. Throws as WithGbaBytes does.
format::File WithGbaProgramVoice(const format::File& file, uint32_t address, const ntdlib::mp2k::Voice& voice);

} // namespace hokuto::edit
