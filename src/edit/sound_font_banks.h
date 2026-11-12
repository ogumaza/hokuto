// SPDX-License-Identifier: MIT

// A SoundFont 2 file's presets as a bank of the 3DS, the Wii U or the Switch, with a wave archive of its samples. The
// presets of one MIDI bank become the bank's instruments, each at its program. Hokuto's renderer reads the SoundFont
// (synth/sound_font.h), and each key and velocity of a preset gets the region of the voice that the renderer starts
// there. A region of NintendoWare's banks plays one wave. Where a preset layers voices, the region uses the first. The
// two voices of a stereo sample take one region, of a stereo wave at the centre (StereoVoicesOf).
//
// A sample goes into the wave archive as a wave of 16-bit PCM at the sample's rate, once for each way that the voices
// play it: from their start to their end, or up to the end of their loop with the loop. A loop that plays on to the
// sample's end after the release loops for ever there. The original key of the region is the root key of the voice, and
// its pitch is the tuning of the voice in cents. A voice whose scale tuning isn't 100 cents a key gets a region for
// each key, with the pitch of that key. The volume is set from the attenuation of the voice: 0.4 of the zones'
// initialAttenuation, and the constant modulators' amounts. The SoundFont players apply 0.4 of initialAttenuation too.
// The pan is set from the pan of the voice, and a key group from its exclusive class. The envelope uses the values of
// NintendoWare's envelope whose SoundFont envelope comes nearest (ntdlib::sequence_export::ConvertEnvelope): nearest in
// time for the attack, the hold, the decay and the release, and in level for the sustain. The warnings name what the
// bank leaves out, such as the filters, the LFOs, the modulation envelope and the volume envelope's delay.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace hokuto::edit
{

// The item ID by which a SoundFont's bank names its wave archive in its table of waves. AddBank maps it to the
// archive's place for the wave archive (edit/new_items.h).
inline constexpr uint32_t kSoundFontWaves = ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, 0);

// The bank file of a SoundFont's presets and the file of the wave archive of their waves, of one console, with the
// versions that Export… writes for the console (song::ExportVersions). The warnings say what the bank leaves out, in
// words for the user.
struct SoundFontBank
{
    std::vector<uint8_t> bank;
    std::vector<uint8_t> wave_archive;
    std::vector<std::string> warnings;

    // What the warnings count: the keys where the SoundFont layers voices, and those of each thing that the bank leaves
    // out. Files of several banks add them up for SoundFontWarnings.
    std::size_t layered = 0;
    std::map<std::string, std::size_t> left_out;
};

// The warnings for `layered` keys where a SoundFont layers voices, and for what `left_out` counts.
std::vector<std::string> SoundFontWarnings(std::size_t layered, const std::map<std::string, std::size_t>& left_out);

// The MIDI bank of BankOfSoundFont that stands for MIDI banks 0 to 127.
inline constexpr int kEveryMidiBank = -1;

// The presets of MIDI bank `midi_bank` of the SoundFont 2 file `sf2` as a bank of the console of `flavor`: the 3DS, the
// Wii U or the Switch. With kEveryMidiBank, the presets of MIDI banks 0 to 127 go in, preset p of bank b at program 128
// b + p. The SoundFont of a converter stores a program past 127 of the source bank so. Throws std::invalid_argument
// with a message for the user for a file that Hokuto can't read as a SoundFont, and for a bank without presets.
SoundFontBank BankOfSoundFont(std::span<const uint8_t> sf2, ntdlib::sar::Flavor flavor, int midi_bank = 0);

} // namespace hokuto::edit
