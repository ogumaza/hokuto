// SPDX-License-Identifier: MIT

// A sequence sound as MIDI, SoundFont 2 and SFZ files, and a bank's instruments as SoundFont 2 and SFZ files. ntdlib's
// converter makes them with the settings of the archive's console. hacsf2's, cafesf2's, citrusf2's and revosf2's files
// come from the same converter. MIDI holds no variables or conditions. The files hold the path that the variables'
// starting values and their changes give.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"

#include "song/sequence.h"

namespace hokuto::song
{

// Converts sequence sound `sound` of `archive`, with the variables' starting values `values` and the game's `changes`.
// `program` names the program in the SoundFont and the SFZ files. Throws std::runtime_error with a message for the user
// if it isn't a sequence sound or its data is damaged.
ntdlib::sequence_export::Conversion ConvertToMidi(const ntdlib::sar::SoundArchive& archive, uint32_t sound,
                                                  const StartValues& values, const Changes& changes,
                                                  const std::string& program);

// The same for sequence sound `sound` of the Wii archive `archive`, with the settings of `build` of NW4R SND, the
// game's build, or else of the build that the archive's version gives (SettingsOf). revosf2 converts the sound the same
// way with the game's main.dol, and without it.
ntdlib::sequence_export::Conversion ConvertToMidi(const ntdlib::rsar::SoundArchive& archive, uint32_t sound,
                                                  const StartValues& values, const Changes& changes,
                                                  const std::string& program,
                                                  const std::optional<ntdlib::SndBuild>& build = {});

// What the files can't hold, from what hacsf2 reports: each approximation with its count, then each warning.
std::vector<std::string> ConversionReport(const ntdlib::sequence_export::Conversion& conversion);

// The lines of `report` that concern an export's SFZ files (`sfz`) or its SoundFont 2 files. ntdlib's converter writes
// both formats. A line that concerns one format alone names it, such as "two-pole in the SoundFont" or "SFZ supports
// one type per track". A line that names only the other format stays out.
std::vector<std::string> ReportFor(std::vector<std::string> report, bool sfz);

// The tags that a sequence's MIDI file holds: its title and its copyright notice. An empty one leaves the file as it
// is.
struct MidiTags
{
    std::string title;
    std::string copyright;
};

// `midi`, a MIDI file from ntdlib's converter, with `tags`. The converter names the sequence in the first event of the
// first track: a meta event of type 3 at tick 0. The title takes the place of that name. The copyright notice, a meta
// event of type 2, goes in before it. Standard MIDI Files 1.0 asks for the notice as the first event of the first
// track, at tick 0. The text stays UTF-8. Throws std::runtime_error with a message for the user when `midi` doesn't
// start that way.
std::vector<uint8_t> WithMidiTags(std::span<const uint8_t> midi, const MidiTags& tags);

// Writes the MIDI file of `conversion` at `midi`, and its SoundFont beside it. The SoundFont takes the MIDI file's name
// with the extension .sf2. hacsf2 names its files the same way. Throws std::runtime_error with a message for the user
// if a file can't be written.
void WriteMidiAndSoundFont(const ntdlib::sequence_export::Conversion& conversion, const std::filesystem::path& midi);

// Writes the MIDI file of `conversion` at `midi`, and its SFZ files in the folder sfz beside it. The SFZ files go in a
// folder there with the MIDI file's name, and the samples in the folder samples. hacsf2 lays out its files the same
// way. A sequence that plays no notes has no SFZ files. A sample that the folder of samples holds already stays as it
// is. Throws std::runtime_error with a message for the user if a file can't be written. If the folder of samples holds
// a sample of the same name with other bytes, it throws before it writes anything.
void WriteMidiAndSfz(const ntdlib::sequence_export::Conversion& conversion, const std::filesystem::path& midi);

// Throws std::runtime_error with WriteMidiAndSfz's message for the user if the folder of samples beside the MIDI file
// `midi` holds a sample of `conversion` with the same name and other bytes. The export checks this before it asks
// whether to replace files.
void CheckSfzSamples(const ntdlib::sequence_export::Conversion& conversion, const std::filesystem::path& midi);

// The error of ConvertBank for a bank without instruments: it gives no SoundFont and no SFZ files.
class BankWithoutInstruments : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Converts the instruments of bank `bank` of `archive`. Each instrument plays as a track plays it before the sequence
// sets anything. `program` names the program in the SoundFont and the SFZ files. Throws std::runtime_error with a
// message for the user when the archive lacks the bank or its file and when the bank can't be read. Throws
// BankWithoutInstruments when the bank holds no instruments.
ntdlib::sequence_export::BankConversion ConvertBank(const ntdlib::sar::SoundArchive& archive, uint32_t bank,
                                                    const std::string& program);

// The same for bank `bank` of the Wii archive `archive`, with the settings of `build` of NW4R SND, the game's build, or
// else of the build that the archive's version gives (SettingsOf). A stereo wave plays in the pan mode of the first
// sequence sound that plays the bank. Without one, it plays in balance mode: NW4R SND's only mode before RSAR 1.2.
ntdlib::sequence_export::BankConversion ConvertBank(const ntdlib::rsar::SoundArchive& archive, uint32_t bank,
                                                    const std::string& program,
                                                    const std::optional<ntdlib::SndBuild>& build = {});

// The names of the SoundFonts of bank conversion `conversion`, in an export called `stem`: the stem with .sf2 when one
// SoundFont holds every instrument. The instruments of a bank past the format's limits go into several SoundFonts. Each
// of those takes the stem and its programs, such as BANK_GM_000-048.sf2, with the digits of the last program.
std::vector<std::string> SoundFontNames(const ntdlib::sequence_export::BankConversion& conversion,
                                        const std::string& stem);

// Writes the SFZ files of bank conversion `conversion` into the folder `folder`: each instrument's file into the folder
// `name` there, and the samples into the folder samples beside it. The files find their samples there. A sequence's SFZ
// files share a folder of samples the same way (WriteMidiAndSfz). When the folder `name` holds some of the instruments'
// files already, `replace` gets their names and decides whether to write over them. Returns false when it doesn't. A
// sample that the folder of samples holds already stays as it is. Throws std::runtime_error with a message for the user
// if a file can't be written. If the folder of samples holds a sample of the same name with other bytes, it throws
// before it writes anything.
bool WriteBankSfz(const ntdlib::sequence_export::BankConversion& conversion, const std::filesystem::path& folder,
                  const std::string& name, const std::function<bool(const std::vector<std::string>&)>& replace);

} // namespace hokuto::song
