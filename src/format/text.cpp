// SPDX-License-Identifier: MIT

// Writes a file of Hokuto in the text form.

#include "format/text.h"

#include <algorithm>
#include <bit>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/dse/commands.h"
#include "ntdlib/nwsnd/units.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/binary.h"
#include "xsflib/text.h"
#include "xsflib/utf8.h"

#include "format/codes.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text_parts.h"

namespace hokuto::format
{
namespace
{

using text_parts::BusVolumeText;
using text_parts::EffectText;
using text_parts::Hex;
using text_parts::LengthText;
using text_parts::NameText;
using text_parts::SoundEngineText;
using text_parts::SoundMixingText;
using text_parts::TagText;
using text_parts::TextOut;
using text_parts::TrackColourText;
using text_parts::WriteNative;
using text_parts::WriteScene;
using text_parts::WriteSequence;
using text_parts::WriteTimelines;
using text_parts::WriteView;

// Writes the lines of the instrument maps and the bank maps of `file`.
void WriteMapLines(TextOut& out, const File& file)
{
    if (!file.instrument_maps.empty() || !file.bank_maps.empty())
    {
        out.Line(0, "");
    }

    for (const auto& [voices, map] : file.instrument_maps)
    {
        for (const auto& [instrument, voice] : map)
        {
            out.Line(0, "instrument_map " + Hex(voices) + (instrument.drums ? " drums " : " program ") +
                            std::to_string(instrument.program) + " voice " + std::to_string(voice));
        }
    }

    for (const auto& [bank, map] : file.bank_maps)
    {
        for (const auto& [program, target] : map.programs)
        {
            out.Line(0, "bank_map " + std::to_string(bank) + " program " + std::to_string(program) + " instrument " +
                            std::to_string(target));
        }

        for (const auto& [note, target] : map.drum_notes)
        {
            out.Line(0, "bank_map " + std::to_string(bank) + " drum_note " + std::to_string(note) + " instrument " +
                            std::to_string(target.program) + " key " + std::to_string(target.key));
        }
    }
}

// Writes the lines of the settings of `file` that come before its sequences: the mixing, the engines, the tags, the
// lengths, the effects, the scenes, the colours, the maps, the view and the timelines.
void WriteSettings(TextOut& out, const File& file)
{
    const NativeTables* tables = file.native && file.native->tables ? &*file.native->tables : nullptr;
    for (const auto& [sound, mixing] : file.sound_mixing)
    {
        out.Line(0, SoundMixingText(sound, mixing, tables));
    }

    for (const auto& [sound, engine] : file.sound_engines)
    {
        out.Line(0, SoundEngineText(sound, engine, tables));
    }

    for (const auto& [key, value] : file.tags)
    {
        out.Line(0, TagText(std::nullopt, key, value, tables));
    }

    for (const auto& [sound, tags] : file.sound_tags)
    {
        for (const auto& [key, value] : tags)
        {
            out.Line(0, TagText(sound, key, value, tables));
        }
    }

    for (const auto& [sound, length] : file.lengths)
    {
        out.Line(0, LengthText(sound, length, tables));
    }

    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        for (const Effect& effect : file.aux_effects[bus])
        {
            out.Line(0, EffectText(bus, effect));
        }
    }

    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        if (file.aux_volumes[bus] != 1.0f)
        {
            out.Line(0, BusVolumeText(bus, file.aux_volumes[bus]));
        }
    }

    for (std::size_t s = 0; s < file.scenes.size(); ++s)
    {
        out.Line(0, "");
        WriteScene(out, s, file.scenes[s], tables);
    }

    if (!file.track_colours.empty())
    {
        out.Line(0, "");
    }

    for (const auto& [place, colour] : file.track_colours)
    {
        out.Line(0, TrackColourText(place, colour, tables));
    }

    WriteMapLines(out, file);
    if (file.view)
    {
        out.Line(0, "");
        WriteView(out, *file.view, tables);
    }

    if (file.timelines)
    {
        out.Line(0, "");
        WriteTimelines(out, file);
    }
}

// The items of each sequence of `file` that the fields of `native` name by a label, by their places in the sequence.
std::vector<std::vector<bool>> LabelledItems(const File& file, const NativeArchive* native)
{
    std::vector<std::vector<bool>> named(file.sequences.size());
    if (!native)
    {
        return named;
    }

    for (const ntdlib::sar::Field& field : native->fields)
    {
        for (const ntdlib::sar::Place& place : {field.from, field.to})
        {
            const std::vector<ntdlib::sar::Piece>& pieces = *native->pieces;
            const auto* piece = !place.sequence && place.piece < pieces.size()
                                    ? std::get_if<ntdlib::sar::SequencePiece>(&pieces[place.piece])
                                    : nullptr;
            const std::size_t sequence = place.sequence ? place.piece : piece ? piece->sequence : named.size();
            if (place.item && place.offset > 0 && sequence < named.size())
            {
                std::vector<bool>& items = named[sequence];
                items.resize(std::max<std::size_t>(items.size(), std::size_t{place.offset} + 1));
                items[place.offset] = true;
            }
        }
    }

    return named;
}

// Writes the chunks of `file` that the text form keeps as bytes.
void WriteChunks(TextOut& out, const File& file, const NativeArchive* native)
{
    const auto write_chunk = [&](std::string_view id, const std::vector<uint8_t>& chunk)
    {
        out.Line(0, "");
        out.Line(0, "chunk " + std::string(id) + " " + std::to_string(chunk.size()));
        out.Bytes(1, chunk);
    };

    if (native && !native->group_files.empty())
    {
        write_chunk(kGroupFilesChunk, GroupFilesChunk(file));
    }

    if (native && !native->streams.empty())
    {
        write_chunk(kStreamsChunk, StreamsChunk(*native));
    }

    if (native && native->cartridge)
    {
        if (native->engine != Engine::kDs && native->engine != Engine::kDse)
        {
            throw std::invalid_argument("a cartridge image belongs to a sound archive or a set of files of the DS");
        }

        write_chunk(kCartridgeChunk, CartridgeChunk(*native->cartridge));
    }

    if (file.native && file.form == ArchiveForm::kModel)
    {
        const Model model = ModelOf(file);
        write_chunk(kSoundsChunk, SoundsChunk(model));
        write_chunk(kItemsChunk, ItemsChunk(model));
    }

    if (!file.banks.empty())
    {
        write_chunk(kBanksChunk, BanksChunk(file));
    }

    if (!file.waves.empty())
    {
        write_chunk(kWavesChunk, WavesChunk(file));
    }

    for (const OtherChunk& chunk : file.other_chunks)
    {
        out.Line(0, "");
        out.Line(0, "chunk " + NameText(chunk.id) + " " + std::to_string(chunk.bytes.size()));
        out.Bytes(1, chunk.bytes);
    }
}

} // namespace

std::string WriteText(const File& file)
{
    if (const std::optional<std::size_t> sequence = UnfiledChoices(file))
    {
        throw std::invalid_argument(
            "sequence " + std::to_string(*sequence) +
            " holds the encoder's choices, and no sequence file of the native archive or its group files names it");
    }
    if (const std::optional<std::size_t> sequence = ForeignChoices(file))
    {
        throw std::invalid_argument("sequence " + std::to_string(*sequence) +
                                    " holds another console's choices of the encoder than the native archive's");
    }

    TextOut out;
    out.Line(0, "hok " + std::to_string(kMajorVersion) + "." + std::to_string(kMinorVersion));
    out.Line(0, "engine " + std::string(NameOf(file.engine)));
    out.Line(0, "mixing " + std::string(NameOf(file.mixing)));
    WriteSettings(out, file);

    // The items that the native archive's fields name by a label: each item but a bytecode's first. A file in the model
    // form keeps no fields.
    const NativeArchive* native = file.native && file.form == ArchiveForm::kNative ? &*file.native : nullptr;
    const std::vector<std::vector<bool>> named = LabelledItems(file, native);

    std::vector<std::vector<std::string>> names;
    for (std::size_t s = 0; s < file.sequences.size(); ++s)
    {
        out.Line(0, "");
        names.push_back(WriteSequence(out, *file.sequences[s], s, named[s]));
    }

    if (native)
    {
        out.Line(0, "");
        WriteNative(out, *native, names);
    }

    WriteChunks(out, file, native);

    return out.Take();
}

} // namespace hokuto::format
