// SPDX-License-Identifier: MIT

// The checks that a file saves and opens again as it was (tools/checked_saves.h).

#include "checked_saves.h"

namespace hokuto::tools
{

std::string PartsThatDiffer(const format::File& a, const format::File& b)
{
    std::string parts;
    const auto add = [&parts](bool same, const std::string& part)
    {
        if (!same)
        {
            parts += (parts.empty() ? " " : "; ") + part;
        }
    };

    add(a.engine == b.engine && a.sound_engines == b.sound_engines, "the engines");
    add(a.mixing == b.mixing && a.sound_mixing == b.sound_mixing, "the mixing modes");
    add(a.aux_effects == b.aux_effects && a.aux_volumes == b.aux_volumes, "the effects");
    add(a.scenes == b.scenes, "the scenes");
    add(a.track_colours == b.track_colours, "the colours");
    add(a.instrument_maps == b.instrument_maps && a.bank_maps == b.bank_maps, "the instrument maps");
    add(a.tags == b.tags && a.sound_tags == b.sound_tags, "the tags");
    add(a.lengths == b.lengths, "the lengths");
    add(a.form == b.form, "the form");
    add(a.other_chunks == b.other_chunks, "the other chunks");
    add(a.view == b.view, "the view");
    add(a.banks == b.banks, "the banks");
    add(a.waves == b.waves, "the waves");
    add(a.timelines == b.timelines, "the timelines");
    add(a.sequences == b.sequences, "the sequences, " + FirstDifference(a.sequences, b.sequences));
    if (!a.native || !b.native)
    {
        add(!a.native == !b.native, "the native archive");
        return parts;
    }

    const format::NativeArchive& native_a = *a.native;
    const format::NativeArchive& native_b = *b.native;
    add(native_a.engine == native_b.engine, "the archive's kind");
    add(native_a.pieces == native_b.pieces, "the pieces, " + FirstDifference(*native_a.pieces, *native_b.pieces));
    add(native_a.fields == native_b.fields, "the fields, " + FirstDifference(native_a.fields, native_b.fields));
    add(native_a.group_files == native_b.group_files,
        "the group files, " + FirstDifference(native_a.group_files, native_b.group_files));
    if (native_a.tables && native_b.tables)
    {
        add(native_a.tables->sounds == native_b.tables->sounds,
            "the sounds' values, " + FirstDifference(native_a.tables->sounds, native_b.tables->sounds));
        add(native_a.tables->banks == native_b.tables->banks &&
                native_a.tables->wave_archives == native_b.tables->wave_archives &&
                native_a.tables->players == native_b.tables->players &&
                native_a.tables->groups == native_b.tables->groups,
            "the other tables");
    }
    else
    {
        add(!native_a.tables == !native_b.tables, "the tables");
    }
    add(native_a.game_sizes == native_b.game_sizes, "the game's sizes");
    add(native_a.streams == native_b.streams, "the streams");
    add(native_a.wii_build == native_b.wii_build, "the build");
    add(native_a.gba_filler == native_b.gba_filler, "the choice of filler runs");
    add(native_a.cartridge == native_b.cartridge, "the cartridge");
    add(native_a.driver == native_b.driver, "the driver's values");

    return parts;
}

bool CheckSaved(const format::File& file, bool sample)
{
    if (!model_form)
    {
        return false;
    }
    if (sample && ++sampled_saves > kAllSampledSaves && sampled_saves % kSampleStep != 0)
    {
        ++passed_saves;
        return false;
    }

    // The opened file cuts the archive that the model builds. The opened file's pieces hold the tables' values. An edit
    // leaves the old values in the pieces. An edit that grows the tables also leaves the fields at their old places.
    // The archives that the pieces build have to be the same.
    const std::vector<uint8_t> binary = format::Write(file);
    const format::File opened = format::Read(binary);
    format::File compared = opened;
    if (opened.native && file.native)
    {
        format::NativeArchive native = *opened.native;
        native.pieces = file.native->pieces;
        native.fields = file.native->fields;
        for (std::size_t i = 0; i < native.group_files.size() && i < file.native->group_files.size(); ++i)
        {
            native.group_files[i].pieces = file.native->group_files[i].pieces;
            native.group_files[i].fields = file.native->group_files[i].fields;
        }
        compared.native = std::move(native);
    }
    if (!(compared == file))
    {
        throw std::runtime_error("the saved file opens otherwise from the binary form:" +
                                 PartsThatDiffer(compared, file));
    }
    if (format::ExportArchive(opened) != format::ExportArchive(file) ||
        format::ExportGroupFiles(opened) != format::ExportGroupFiles(file))
    {
        throw std::runtime_error("the saved file opens from the binary form with another archive");
    }
    const format::File from_text = format::ReadText(format::WriteText(file));
    if (!(from_text == opened))
    {
        throw std::runtime_error("the saved file opens otherwise from the text form:" +
                                 PartsThatDiffer(from_text, opened));
    }
    if (format::Write(opened) != binary)
    {
        throw std::runtime_error("a save of the opened file gives other bytes");
    }
    ++checked_saves;

    return true;
}

void PrintSaves(const std::string& name)
{
    if (!model_form)
    {
        return;
    }

    std::printf("%s: %zu files saved in the model form opened again in both forms.", name.c_str(), checked_saves);
    if (passed_saves > 0)
    {
        std::printf(" %zu saves between the samples went unchecked.", passed_saves);
    }
    std::printf("\n");
}

bool SavesAndOpens(const format::File& file, bool sample)
{
    if (model_form)
    {
        CheckSaved(file, sample);
        return true;
    }

    return RoundTrips(file);
}

bool RoundTrips(const format::File& file)
{
    return format::Read(format::Write(file)) == file && format::ReadText(format::WriteText(file)) == file;
}

} // namespace hokuto::tools
