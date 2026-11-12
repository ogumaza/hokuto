// SPDX-License-Identifier: MIT

// The rest of the GBA import (edit/gba_midi.h): the proposed voices, the voice group, the import of assembly, the file
// with the new song, and the collisions of a song on the console's channels.

#include "edit/gba_midi.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/mp2k/commands.h"
#include "xsflib/bytes.h"

#include "edit/gba_tables.h"
#include "format/archive.h"
#include "song/gba.h"
#include "song/gba_assembly.h"
#include "song/sequence.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;

} // namespace

int ProposedGbaVoice(const GbaMidiInstrument& instrument, std::span<const ntdlib::mp2k::Voice> voices)
{
    const auto drum_kit = [](const ntdlib::mp2k::Voice& voice)
    {
        return (voice.type & ntdlib::mp2k::kVoiceDrumKit) != 0;
    };
    if (!instrument.drums || (static_cast<std::size_t>(instrument.program) < voices.size() &&
                              drum_kit(voices[static_cast<std::size_t>(instrument.program)])))
    {
        return instrument.program;
    }

    const auto first = std::find_if(voices.begin(), voices.end(), drum_kit);
    return first != voices.end() ? static_cast<int>(first - voices.begin()) : instrument.program;
}

GbaVoiceMap ProposedGbaVoices(const format::File& file, uint32_t voice_group,
                              std::span<const GbaMidiInstrument> instruments,
                              std::span<const ntdlib::mp2k::Voice> voices)
{
    const auto map = file.instrument_maps.find(voice_group);
    const GbaVoiceMap empty;
    const GbaVoiceMap& known = map != file.instrument_maps.end() ? map->second : empty;
    GbaVoiceMap proposed;
    for (const GbaMidiInstrument& instrument : instruments)
    {
        const auto paired = known.find(instrument);
        proposed[instrument] = paired != known.end() ? paired->second : ProposedGbaVoice(instrument, voices);
    }

    return proposed;
}

std::optional<uint32_t> GbaVoiceGroupOf(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                        uint32_t song)
{
    if (!file.native || file.native->engine != format::Engine::kGba || song >= places.size() || !places[song] ||
        places[song]->track_fields.empty())
    {
        return std::nullopt;
    }

    const std::optional<RunPlace> place = GbaHeaderPlace(*file.native, places[song]->track_fields.front());
    if (!place)
    {
        return std::nullopt;
    }

    const auto& run = std::get<std::vector<uint8_t>>((*file.native->pieces)[place->piece]);

    return xsflib::LoadLe32(&run[place->offset + kGbaHeaderVoices]);
}

bool IsGbaAssembly(std::span<const uint8_t> bytes)
{
    constexpr std::size_t kLookedAt = 4096;
    const auto starts_with = [bytes](std::string_view start)
    {
        return bytes.size() >= start.size() &&
               std::equal(start.begin(), start.end(), bytes.begin(),
                          [](char a, uint8_t b) { return static_cast<uint8_t>(a) == b; });
    };
    return !starts_with("MThd") && !starts_with("RIFF") &&
           std::ranges::find(bytes.first(std::min(bytes.size(), kLookedAt)), uint8_t{0}) ==
               bytes.first(std::min(bytes.size(), kLookedAt)).end();
}

GbaMidiSong GbaSongOfAssembly(std::string_view text, int note_arguments)
{
    const song::GbaSongSource source = song::ReadGbaAssembly(text);
    if (source.entries.empty())
    {
        throw std::invalid_argument("The song in the file has no tracks.");
    }

    ntdlib::mp2k::Decoded decoded =
        ntdlib::mp2k::DecodeSequence(source.bytes, source.base, source.entries, note_arguments);
    if (decoded.opaque)
    {
        throw std::invalid_argument("The commands in the file overlap, or two tracks read them differently.");
    }

    GbaMidiSong imported;
    imported.sequence = std::move(decoded.sequence);
    imported.entries = std::move(decoded.entries);
    imported.priority = source.priority;
    imported.reverb = source.reverb;
    imported.warnings.push_back("The song keeps its current voice group. The voice group in the file is ignored.");

    return imported;
}

format::File WithGbaMidiSong(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t song,
                             const GbaMidiSong& midi)
{
    if (!file.native || file.native->engine != format::Engine::kGba)
    {
        throw std::invalid_argument("The file contains no GBA ROM.");
    }
    if (song >= places.size() || !places[song])
    {
        throw std::invalid_argument(
            "Can't replace this song. Hokuto can't separate its commands from the rest of the ROM.");
    }

    const SoundPlace& place = *places[song];
    const std::vector<uint32_t>& fields = place.track_fields;
    if (fields.size() < midi.entries.size())
    {
        throw std::invalid_argument("The song header has room for " + std::to_string(fields.size()) +
                                    (fields.size() == 1 ? " track" : " tracks") + ", but the new song has " +
                                    std::to_string(midi.entries.size()) + ". Hokuto can't add tracks to a header yet.");
    }

    // The header's other tracks end at once. The cut assigns to the commands of a song the bytes from the lowest track
    // of its header up to the header, clear of the bytes of every other song. Only the fields of the song's header name
    // them.
    ntdlib::commands::Sequence sequence = midi.sequence;
    std::vector<uint32_t> starts = midi.entries;
    if (starts.size() < fields.size())
    {
        Command fine;
        fine.kind = Kind::kEnd;
        starts.resize(fields.size(), static_cast<uint32_t>(sequence.items.size()));
        sequence.items.push_back(fine);
    }

    format::File result = file;
    if (const std::optional<uint32_t> voice_group = GbaVoiceGroupOf(file, places, song))
    {
        for (const auto& [instrument, voice] : midi.voices)
        {
            result.instrument_maps[*voice_group].insert_or_assign(instrument, voice);
        }
    }

    result.sequences[place.sequence] = std::move(sequence);
    const auto place_song = [&](format::NativeArchive& edited)
    {
        for (std::size_t i = 0; i < fields.size(); i++)
        {
            edited.fields[fields[i]].to.offset = starts[i];
        }

        // The header sets the priority and the reverb (GbaHeaderPlace).
        if (!midi.priority && !midi.reverb)
        {
            return;
        }

        const std::optional<RunPlace> header = GbaHeaderPlace(edited, fields.front());
        if (!header)
        {
            throw std::invalid_argument("Can't find the song header.");
        }

        const auto write = [&](std::vector<ntdlib::sar::Piece>& pieces)
        {
            auto& run = std::get<std::vector<uint8_t>>(pieces[header->piece]);
            if (midi.priority)
            {
                run[header->offset + kGbaHeaderPriority] = *midi.priority;
            }

            if (midi.reverb)
            {
                run[header->offset + kGbaHeaderReverb] = *midi.reverb;
            }
        };
        edited.pieces.Edit(write);
    };
    result.native.Edit(place_song);

    return result;
}

std::optional<song::Collisions> GbaSongCollisions(const format::File& file, const song::GbaRom& rom, int song)
{
    // MP2K's detection scans the whole ROM several times, and the Import window makes its view on the program's main
    // thread. The count skips the detection. An export that refuses the commands returns no count. The Play button of
    // the window shows the reason.
    try
    {
        song::GbaRom played;
        played.rom.Assign(format::PlayedBytes(file));
        played.info = rom.info;
        return song::CollisionsOf(song::ReadSequence(played, song));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

} // namespace hokuto::edit
