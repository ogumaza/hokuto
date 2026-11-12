// SPDX-License-Identifier: MIT

#include "edit/midi_imports.h"

#include <stdexcept>
#include <utility>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/sdat/files.h"
#include "xsflib/files.h"

#include "edit/converter.h"
#include "edit/ds_conversion.h"
#include "edit/other_console.h"
#include "format/kinds.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
namespace sar = ntdlib::sar;

} // namespace

std::string StemOf(const std::string& file_name)
{
    return xsflib::Utf8(xsflib::PathFromUtf8(file_name).stem());
}

bool IsMidi(std::span<const uint8_t> bytes)
{
    return format::IsInputKind(bytes, format::InputKind::kMidi);
}

MidiSong ImportedSong(std::span<const uint8_t> midi, const std::string& name, uint32_t most_timebase,
                      const format::MidiDrumNotes& drum_notes, std::string_view engine)
{
    MidiOptions options{.timebase = 48,
                        .from_tick_0 = true,
                        .keeps_time = true,
                        .ends_at_loop = true,
                        .joins_layers = true,
                        .imports = true,
                        .engine = engine,
                        .drum_notes = &drum_notes};
    MidiSong song = SongOfMidi(midi, name, options);
    if (song.moved > 0 && most_timebase >= 96)
    {
        options.timebase = 96;
        MidiSong finer = SongOfMidi(midi, name, options);
        if (finer.moved < song.moved)
        {
            return finer;
        }
    }

    return song;
}

Incoming ReadIncomingMidi(std::span<const uint8_t> midi, const std::string& file_name, const song::Console& into,
                          const format::MidiDrumNotes& drum_notes)
{
    const MidiSong song = ImportedSong(midi, StemOf(file_name), 96, drum_notes);
    Sequence sequence = LayOutConverted(song.timeline, song.hints, true);

    Incoming incoming;
    incoming.midi = true;
    incoming.size = midi.size();
    incoming.warnings = song.warnings;
    if (!into.wii)
    {
        incoming.flavor = FlavorOf(into);
        incoming.sequence = std::move(sequence);
        return incoming;
    }

    // The Wii's commands read as a later console's do, and the song starts from the Switch's units.
    CommandsForWii converted = WiiCommandsOf(sequence, sar::Flavor::kNx, *into.wii);
    incoming.flavor = sar::Flavor::kNx;
    incoming.sequence = std::move(converted.sequence);
    incoming.wii = true;
    incoming.converted = true;
    incoming.warnings.insert(incoming.warnings.end(), converted.warnings.begin(), converted.warnings.end());

    return incoming;
}

DsMidiFile DsFileOfMidi(std::span<const uint8_t> midi, const std::string& file_name, const ProgramMap& programs,
                        const format::MidiDrumNotes& drum_notes)
{
    const MidiSong song = ImportedSong(midi, StemOf(file_name), 48, drum_notes, "DS");
    Sequence sequence = LayOutConverted(song.timeline, song.hints, true);

    // The song's tracks, and the commands as the DS reads them. A song of 48 ticks a quarter note requires no scaling.
    const auto tracks = static_cast<uint32_t>(sequence.items.at(0).value.value / kOne);
    const song::Console reads{.engine = format::Engine::kSwitch};
    const DsCommands commands = AsDs(WithPrograms(sequence, kEmptySongStart, programs), kEmptySongStart, tracks, reads);

    DsMidiFile result;
    result.song = std::move(sequence);
    try
    {
        result.file = ntdlib::sdat::SequenceFile(ntdlib::ntr::snd::EncodeSequence(commands.sequence).bytes);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't write the song commands for the DS. ") +
                                    format::AsSentence(error.what()));
    }

    result.warnings = song.warnings;
    AddDsCommandWarnings(commands, reads, result.warnings);

    return result;
}

} // namespace hokuto::edit
