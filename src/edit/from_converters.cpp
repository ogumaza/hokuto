// SPDX-License-Identifier: MIT

// A sound of the GBA, the DS or DSE as a new file of the Wii U, through the MIDI file and the SoundFont that a
// converter makes of it (edit/other_console.h).

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/formats.h"

#include "edit/converter.h"
#include "edit/ds_files.h"
#include "edit/midi_songs.h"
#include "edit/new_items.h"
#include "edit/other_console.h"
#include "edit/other_console_parts.h"
#include "edit/replace.h"
#include "edit/sound_font_banks.h"
#include "edit/tables.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/model.h"
#include "format/shared.h"
#include "song/driver.h"
#include "song/ds.h"
#include "song/gba.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
using other_console_parts::CopyParts;
using other_console_parts::Settle;
using other_console_parts::Song;

// The MIDI file and the SoundFont of `conversion` as a new file of the Wii U. `conversion` contains the files that a
// converter made of sequence `sound` of `file`. The MIDI file is imported as a song of NintendoWare's MIDI converter,
// with its subroutines, and the file keeps it as a timeline (SongOfMidi in edit/midi_songs.h). A MIDI file of more than
// 16 channels joins the layers that the converter gave a track into that track. The song leaves out the events from the
// loop's end on, such as a second pass of the loop. A note that sounds over the loop's end keeps its length, and sounds
// on after the jump. The song's tempo moves between whole tempos from bar to bar and keeps the MIDI file's time. The
// SoundFont is imported as a bank after a wave archive of its samples (BankOfSoundFont in edit/sound_font_banks.h). Its
// MIDI banks 0 to 127 go in, and control change 0 selects among them, for a source's bank of more than 128 programs.
// The sound, the bank and the wave archive take `name` after SEQ_, BANK_ and WARC_. The sound uses Hokuto's settings
// and the sine and cosine pan curve. The pans of the MIDI file follow that law. The reverb of the song sends to bus A,
// and the new file has no effect there. The warnings name no console but the source console: the file can be converted
// on to another console. With `to_ds`, the file is converted on to the DS. The DS has no buses. The warnings then give
// no advice about bus A, and the warning about whole tempos names the DS.
OtherConsoleSong FromConversion(const format::File& file, uint32_t sound,
                                const ntdlib::sequence_export::Conversion& conversion, const std::string& name,
                                bool to_ds)
{
    const MidiSong midi = SongOfMidi(conversion.midi, name,
                                     {.from_tick_0 = true,
                                      .keeps_time = true,
                                      .ends_at_loop = true,
                                      .joins_layers = true,
                                      .selects_banks = true,
                                      .engine = to_ds ? "DS" : "NintendoWare"});
    const SoundFontBank bank = BankOfSoundFont(conversion.sf2, sar::Flavor::kCafe, kEveryMidiBank);

    Song song;
    song.result.file = format::NewFile(format::Engine::kWiiU);
    song.result.file = AddWaveArchive(song.result.file, "WARC_" + name, bank.wave_archive);
    song.result.file = AddBank(song.result.file, "BANK_" + name, bank.bank, {{kSoundFontWaves, 0}});
    const uint32_t bank_id = sar::ItemId(sar::kBankItem, 0);
    const ntdlib::commands::Sequence sequence = LayOutConverted(midi.timeline, midi.hints, true);
    const auto start =
        std::ranges::find(sequence.labels, SongLabelPrefix(name) + "Start", &ntdlib::commands::Label::name);
    song.result.file = AddSequenceSound(song.result.file, std::nullopt, "SEQ_" + name, sequence, start->item, {},
                                        std::vector<uint32_t>{bank_id});
    const auto new_sequence = static_cast<uint32_t>(song.result.file.sequences.size() - 1);
    const auto added = static_cast<uint32_t>(song.result.file.native->tables->sounds.size() - 1);
    song.result.file = ChangeSound(song.result.file, added, {.value = SoundValue::kPanCurve, .slot = 0, .to = 3});
    Settle(song, added, std::nullopt);
    CopyParts(song, file, sound, added, std::nullopt, std::nullopt);
    if (!song.result.file.timelines)
    {
        song.result.file.timelines.emplace();
    }

    (*song.result.file.timelines)[new_sequence] = midi.hints;

    // What changed on the way.
    std::vector<std::string>& warnings = song.result.warnings;
    for (const std::string& line : song::ConversionReport(conversion))
    {
        warnings.push_back(line);
    }

    warnings.insert(warnings.end(), midi.warnings.begin(), midi.warnings.end());
    warnings.insert(warnings.end(), bank.warnings.begin(), bank.warnings.end());
    const bool reverb = std::ranges::any_of(
        sequence.items, [](const ntdlib::commands::Command& command)
        { return command.kind == ntdlib::commands::Kind::kSend && command.index == 0 && command.value.value != 0; });
    if (reverb && !to_ds)
    {
        warnings.emplace_back(
            "The song sends reverb to bus A, but the new file has no effect on bus A. Add one in FX channel A of the "
            "mixer.");
    }

    return std::move(song.result);
}

// The conversion that `convert` makes. Throws std::invalid_argument with the message of a std::runtime_error that the
// converter throws.
template <class Convert>
ntdlib::sequence_export::Conversion ConversionOf(Convert convert)
{
    try
    {
        return convert();
    }
    catch (const std::runtime_error& error)
    {
        throw std::invalid_argument(error.what());
    }
}

// `name` with an underscore in place of each space, for the labels of the new items.
std::string Underscored(std::string name)
{
    std::ranges::replace(name, ' ', '_');

    return name;
}

// Song `song` of `file` as a new file of the Wii U (FromConversion). `file` is a file of the GBA. song::ConvertToMidi
// makes a MIDI file of two passes of the song's loop and a SoundFont of the voices that it plays. In the second pass, a
// note that sounds over the end of the loop gets the length that it has on the GBA. MP2K steps at the GBA's 59.73 Hz,
// and plays its tempos 0.45% slower than they read. The time of the MIDI file follows the time of the GBA. Throws
// std::invalid_argument with a message for the user for a song that Hokuto can't send, and the exceptions of the
// imports. FromConversion takes `to_ds`.
OtherConsoleSong FromGba(const format::File& file, uint32_t sound, bool to_ds)
{
    const std::shared_ptr<const song::GbaRom> rom = song::LoadGbaRom(format::ExportArchive(file));
    const ntdlib::sequence_export::Conversion conversion =
        ConversionOf([&] { return song::ConvertToMidi(*rom, static_cast<int>(sound), "Hokuto"); });
    const std::string name = Underscored(song::GbaSongName(*rom, static_cast<int>(sound)));

    return FromConversion(file, sound, conversion, name, to_ds);
}

// Sequence `sound` of `file` as a new file of the Wii U (FromConversion). `file` is a file of the DS. ntdlib's
// converter of the DS makes a MIDI file of the intro and one pass of the loop, and a SoundFont of the instruments that
// it plays (song::ConvertToMidi). The conversion uses the build of the driver in the cartridge image when the file
// keeps the image, and the sound plays with the profile of its game (song::DsProfileOf). The sequence's name loses its
// prefix SEQ_: the new sound, bank and wave archive take SEQ_, BANK_ and WARC_ before it. Throws std::invalid_argument
// with a message for the user for a sequence that Hokuto can't send, and the exceptions of the imports.
OtherConsoleSong FromDs(const format::File& file, uint32_t sound)
{
    const std::shared_ptr<const song::DsArchive> archive = song::LoadDsArchive(
        format::ExportArchive(file),
        file.native->cartridge ? song::DsBuildOf(*file.native->cartridge->image) : ntdlib::ntr::snd::kDefaultBuild,
        DsGameOfFile(file));
    const ntdlib::sequence_export::Conversion conversion =
        ConversionOf([&] { return song::ConvertToMidi(*archive, sound, {}, "Hokuto"); });
    std::string name = song::DsSoundName(*archive, sound);
    if (name.starts_with("SEQ_") && name.size() > 4)
    {
        name.erase(0, 4);
    }

    return FromConversion(file, sound, conversion, Underscored(std::move(name)), false);
}

// Sound `sound` of `file` as a new file of the Wii U (FromConversion). `file` is a file of DSE's files. ntdlib's
// converter of DSE makes a MIDI file of the intro and one pass of the loop, and a SoundFont of the instruments that it
// plays (song::ConvertToMidi). The conversion uses the build of the driver in the cartridge image when the file keeps
// the image. Throws std::invalid_argument with a message for the user for a sound that Hokuto can't send, and the
// exceptions of the imports. FromConversion takes `to_ds`.
OtherConsoleSong FromDse(const format::File& file, uint32_t sound, bool to_ds)
{
    const std::shared_ptr<const song::DseSet> set = song::LoadDseSet(
        format::ExportArchive(file),
        file.native->cartridge ? song::DseBuildOf(*file.native->cartridge->image) : ntdlib::dse::Build::kLater);
    const ntdlib::sequence_export::Conversion conversion =
        ConversionOf([&] { return song::ConvertToMidi(*set, sound, "Hokuto"); });
    const std::string name = Underscored(song::DseSoundName(*set, sound));

    return FromConversion(file, sound, conversion, name, to_ds);
}

// Sound `sound` of `file` as a new file of the Wii U (FromConversion). `file` is a file of a ROM with one of the GBA's
// other drivers, such as Konami's driver. song::DriverRom::ConvertToMidi makes a MIDI file of two passes of the song's
// loop and a SoundFont of what the song plays. The samples of the driver are converted as they sound at the rate of its
// mixer, and the MIDI file has none of the echo of the driver. Throws std::invalid_argument with a message for the user
// for a song that Hokuto can't send, and the exceptions of the imports. FromConversion takes `to_ds`.
OtherConsoleSong FromDriver(const format::File& file, uint32_t sound, bool to_ds)
{
    const std::shared_ptr<const song::DriverRom> rom =
        song::LoadDriverRom(file.native->engine, format::ExportArchive(file));
    const ntdlib::sequence_export::Conversion conversion =
        ConversionOf([&] { return rom->ConvertToMidi(sound, "Hokuto"); });
    std::string name = Underscored(rom->SoundName(sound));
    std::erase_if(name, [](char c) { return c == '(' || c == ')'; });

    return FromConversion(file, sound, conversion, name, to_ds);
}

} // namespace

namespace other_console_parts
{

OtherConsoleSong FromConverter(const format::File& file, uint32_t sound, bool to_ds)
{
    return file.native->engine == format::Engine::kGba  ? FromGba(file, sound, to_ds)
           : file.native->driver                        ? FromDriver(file, sound, to_ds)
           : file.native->engine == format::Engine::kDs ? FromDs(file, sound)
                                                        : FromDse(file, sound, to_ds);
}

} // namespace other_console_parts

} // namespace hokuto::edit
