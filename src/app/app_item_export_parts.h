// SPDX-License-Identifier: MIT

// The parts of the item export that its files share (app_item_export.cpp and app_item_export_ds_gba.cpp). The names
// are internal to those files.

#pragma once

#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/sequence_export/convert.h"

#include "app/app_work.h"
#include "app/export_window.h"
#include "format/kinds.h"

namespace hokuto::app::item_export_parts
{

// `name` without its extension: the part from its last dot on. A name without a dot stays as it is.
std::string WithoutExtension(const std::string& name);

// The file name of the export of `job` to `target`, from the item's file name `name`. A SoundFont or SFZ export uses
// the name without its extension, with the program of an instrument that is exported by itself (ProgramSuffix) and
// `.sf2` for a SoundFont. Other targets keep `name`.
std::string BankFileName(const std::string& name, const Export& job, ExportTarget target);

// Makes `job` the export of instrument `program` of its bank, whose last program is `last`.
void ForInstrument(Export& job, uint32_t program, uint32_t last);

// The rest of a SoundFont or SFZ export to `file_name`, once `bank` contains the conversion: the name of the SFZ file
// of an instrument that is exported by itself, at `program` (NameInstrumentSfz), and the warnings of the conversion.
std::vector<std::string> BankExportWarnings(ntdlib::sequence_export::BankConversion& bank, ExportTarget target,
                                            const std::optional<uint32_t>& program, const std::string& file_name);

// The WAV files of a folder export: `add(wave)` adds the file of each wave of `waves` to the work. A wave whose `add`
// throws stays out of the folder. Returns a warning for each of them.
template <typename Waves, typename Add>
std::vector<std::string> AddWavFiles(const Waves& waves, Add add)
{
    std::vector<std::string> found;
    for (const auto wave : waves)
    {
        try
        {
            add(wave);
        }
        catch (const std::exception& error)
        {
            found.push_back(format::AsSentence(error.what()) + " It's left out of the folder.");
        }
    }

    return found;
}

// The error of a WAV export of item `name` that makes no file: the item has no waves (`none`), or no wave decodes.
std::runtime_error NoWavFiles(const std::string& name, bool none);

} // namespace hokuto::app::item_export_parts
