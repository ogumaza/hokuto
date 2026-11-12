// SPDX-License-Identifier: MIT

// A song of MusicPlayer2000 as assembly: the form in which GBA projects keep their songs. The file contains the song's
// tracks, each command as a line of `.byte` with the names of the SDK's definitions (MPlayDef.s), each jump's target as
// a `.word` of a label, and then the song's header. The GNU assembler and LLVM's read it, with those definitions. The
// bytes come out as the ROM stores them. Only the addresses of the file's labels depend on where the linker puts them.
//
// The song's values stand as the usual files give them: a track's volume is `value*song_mvl/mxv`, a tempo is
// `beats*song_tbs/2`, a key shift is `song_key+value`, a pan, a bend and a tune are `c_v+value`. The file's `.equ`
// lines set song_mvl to 127, song_tbs to 1 and song_key to 0. A user can change them there. Users of the usual files do
// the same.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "song/gba.h"

namespace hokuto::song
{

// The address at which ReadGbaAssembly lays a file's bytes out by default. The address is outside every ROM. An address
// that the file writes as a number, such as a pattern of another song, stays outside the song's bytes.
inline constexpr uint32_t kGbaAssemblyBase = 0x0E000000;

// The assembly of song `song` of `rom`, with its labels from `name`: a character other than a letter, a digit or an
// underscore becomes an underscore. A target outside the song's bytes stays an address. Throws std::invalid_argument
// with a message for the user for a song that can't be read, whose tracks aren't before its header, and whose commands
// overlap or read otherwise from two tracks.
std::string GbaSongAssembly(const GbaRom& rom, int song, std::string_view name);

// What a file of assembly contains.
struct GbaSongSource
{
    std::string name;              // the header's label
    std::vector<uint8_t> bytes;    // from the lowest track's start up to the header: none without tracks
    uint32_t base = 0;             // the address of the first of `bytes`
    std::vector<uint32_t> entries; // the address of each track of the header, in the header's order
    uint8_t blocks = 0;            // the header's count of blocks
    uint8_t priority = 0;
    uint8_t reverb = 0;
    std::optional<uint32_t> voices; // the voice group's address, where the file writes a number
    std::string voices_symbol;      // the voice group's name, where the file uses a name that it doesn't define
};

// Reads a file of assembly, with its first byte at address `base`. The file's header is the label that its first
// `.global` names, or else its last label. The reader supports the directives that such files use: `.byte`, `.2byte`,
// `.hword`, `.word`, `.4byte`, `.long`, `.int`, `.equ`, `.set`, `.align`, `.balign`, `.global`, `.globl`, `.section`,
// `.text`, `.data`, `.include` and `.end`, with the arithmetic of + - * / and parentheses, and the names of the SDK's
// definitions. `.align` counts from the file's start. Throws std::invalid_argument with a message for the user, with
// the line's number, for a directive or a name that the reader doesn't support, and for a file without a header that it
// can read.
GbaSongSource ReadGbaAssembly(std::string_view text, uint32_t base = kGbaAssemblyBase);

// The assembly of voice group `group` of `rom`, in `.byte` and `.4byte` lines, without a project's macros: the group's
// table, from program 0 up to its last listed program (GbaListedPrograms), and then the tables of its key splits and
// drum kits, their key tables and its wave patterns. A word that names a sample, or anything else that the file leaves
// out, becomes a name that ends in its address, or else the ROM's number. Throws std::invalid_argument with a message
// for the user for a table past the ROM's end.
std::string GbaVoiceGroupAssembly(const GbaRom& rom, const GbaVoiceGroup& group);

// A label of a voice group's file other than the label of the table, with the bytes up to the next label.
struct GbaVoiceGroupPart
{
    std::string label;
    std::size_t offset = 0; // in GbaVoiceGroupSource::bytes
    std::size_t size = 0;
};

// What a file of a voice group contains.
struct GbaVoiceGroupSource
{
    std::string name;
    std::vector<uint8_t> bytes;
    std::size_t table = 0;  // the offset of the group's table in `bytes`
    std::size_t voices = 0; // the voices of the group's table: its programs, from 0
    std::vector<GbaVoiceGroupPart> parts;
};

// Reads a file of a voice group, with its first byte at address kGbaAssemblyBase, by the rules of ReadGbaAssembly. The
// group's table is the label that the first `.global` names, or else the file's first label, and it runs to the next
// label. A label in `placed` stands for that address instead of its place in the file. A name that the file doesn't
// define has to end in an underscore and 8 hexadecimal digits: that address. Throws std::invalid_argument with a
// message for the user as ReadGbaAssembly does, and for another name, such as a project's label or a macro, and a table
// that doesn't hold a whole number of voices, up to 128.
GbaVoiceGroupSource ReadGbaVoiceGroup(std::string_view text, const std::map<std::string, uint32_t>& placed = {});

// The name that GbaVoiceGroupAssembly gives the sample at `address`.
std::string GbaSampleName(uint32_t address);

// The address that the 8 hexadecimal digits after the last underscore of a name give. GbaVoiceGroupAssembly writes the
// address in that form.
std::optional<uint32_t> GbaAddressOfName(std::string_view name);

// The SDK's definitions as lines of `.equ`, with the names that GbaSongAssembly writes and ReadGbaAssembly reads. They
// are facts of the driver's commands. A project keeps them in its MPlayDef.s.
std::string GbaAssemblyDefinitions();

} // namespace hokuto::song
