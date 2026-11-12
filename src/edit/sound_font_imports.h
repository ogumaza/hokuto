// SPDX-License-Identifier: MIT

// A SoundFont 2 file as new banks of a file's archive, for File > Import > Bank…. Each MIDI bank of the SoundFont that
// has presets becomes a bank, with a wave archive of its samples beside it (edit/sound_font_banks.h). The presets keep
// their programs. On the 3DS, the Wii U and the Switch, the new files go in as Export… writes them for the console. On
// the Wii, the bank goes in with its waves as its wave data (ntdlib::rsar::WiiBankOf), in the version of the archive's
// banks. On the DS, the bank and the wave archive go in as the bank of a song converts to the DS (edit::DsBankFilesOf):
// the bank's first slot names the new wave archive.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "format/file.h"

namespace hokuto::edit
{

// Whether `bytes` start as a SoundFont 2 file does: a RIFF file of the form sfbk.
bool IsSoundFont(std::span<const uint8_t> bytes);

// Whether a file's name `name` ends in .sf2, in any case.
bool IsSoundFontName(std::string_view name);

// What a SoundFont adds to the archive of a file.
struct SoundFontImport
{
    std::vector<int> midi_banks;       // the MIDI banks that hold presets, each a new bank
    std::size_t presets = 0;           // the presets in those MIDI banks
    std::size_t bytes = 0;             // the new files' bytes
    std::vector<std::string> warnings; // what the new banks leave out, in words for the user
};

// What the SoundFont `sf2` adds to the archive of `file`. The waves' bytes count too. The function therefore converts
// the whole SoundFont. Throws std::invalid_argument with a message for the user for a file that Hokuto can't read as a
// SoundFont, one without presets, and an archive of a console that accepts no SoundFont.
SoundFontImport ReadSoundFontImport(const format::File& file, std::span<const uint8_t> sf2);

// The name of the new bank of each of `midi_banks`: `name` alone for one, and otherwise `name`, an underscore and the
// MIDI bank.
std::vector<std::string> SoundFontBankNames(const std::string& name, std::span<const int> midi_banks);

// The name of the wave archive beside bank `name`: the name with WARC_ in the place of BANK_, or else after WARC_.
std::string SoundFontWaveArchiveName(const std::string& name);

// `file` with the new banks of the SoundFont `sf2` under `name` (SoundFontBankNames), each after a new wave archive,
// but on the Wii, where its waves go in as its wave data. On the Wii, `groups` contains the groups with names that the
// new files join. Throws std::invalid_argument with a message for the user as ReadSoundFontImport, edit::AddBank,
// edit::AddWiiBank and edit::AddDsBank do.
format::File AddSoundFont(const format::File& file, std::span<const uint8_t> sf2, const std::string& name,
                          std::span<const uint32_t> groups = {});

} // namespace hokuto::edit
