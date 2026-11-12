// SPDX-License-Identifier: MIT

// SFZ files as a SoundFont 2 file, for File > Import > Bank…. Each SFZ file becomes a preset that plays one instrument,
// at the program of the file. The imports of a SoundFont then take it (edit/sound_font_imports.h and
// edit/gba_sound_fonts.h), and they list what the new banks or voices leave out. The opcodes follow their definitions
// at sfzformat.com.
//
// A region's keys and velocities (key, lokey, hikey, lovel, hivel), root key (pitch_keycenter), tuning (transpose,
// tune, pitch_keytrack), level (volume, amplitude), pan, volume envelope (ampeg_delay to ampeg_release), key group
// (group with off_by) and loop (loop_mode, loop_start, loop_end, or the WAV file's smpl chunk) become the generators of
// its zone. loop_mode=one_shot marks the zone (kPlaysPastRelease): a new bank's region ignores its note-off, and a GBA
// voice gets the slowest release. SFZ's decay and release fall by a factor of e^-8 in their times (69.49 dB), and a
// SoundFont's fall 100 dB. A sample is a WAV file. offset and end cut it. A stereo sample makes two zones, one for each
// channel at its side. A region that a release, a legato note, a later turn of a round robin or a later random choice
// starts stays out. trigger=first stays in. A SoundFont has no level above full. A file with a positive volume is
// therefore imported that much lower as a whole.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hokuto::edit
{

// Whether `name` ends in .sfz, in either case.
bool IsSfzName(std::string_view name);

// An SFZ file and the program that it gets.
struct SfzInstrument
{
    std::filesystem::path path;
    uint32_t program = 0;
};

// The SFZ files that come in with `chosen`. Export… names an instrument's file after its bank and its program, such as
// BANK_GM_005.sfz. Each file of the folder with the same name and another program comes in too, in the order of the
// programs. A file whose name ends in no program gets program 0, alone.
std::vector<SfzInstrument> SfzInstrumentsBeside(const std::filesystem::path& chosen);

// The name of the new bank of `instruments`, chosen as `chosen`: the name without the program, for several.
std::string SfzImportName(std::span<const SfzInstrument> instruments, const std::string& chosen);

// Reads a whole file. Returns false for a file that can't be read.
using SfzReader = std::function<bool(const std::filesystem::path&, std::vector<uint8_t>&)>;

// What SFZ files give the imports of a SoundFont.
struct SfzSoundFont
{
    std::vector<uint8_t> sf2;
    std::size_t regions = 0;           // the regions that went in
    std::vector<std::string> warnings; // what the SoundFont leaves out, in words for the user
};

// The SoundFont of `instruments`. A sample's path counts from the folder of its SFZ file, after default_path. Throws
// std::invalid_argument with a message for the user for a file that `read` can't read, and for files without a region
// that plays a sample that Hokuto can read.
SfzSoundFont SoundFontOfSfz(std::span<const SfzInstrument> instruments, const SfzReader& read);

} // namespace hokuto::edit
