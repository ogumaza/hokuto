// SPDX-License-Identifier: MIT

// The tests of the edits share these helpers: the places of the sounds of a Switch archive, and the sequences that
// they play.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "ntdlib/sar/formats.h"

#include "edit/replace.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace hokuto::testing
{

// Where each sound of `file`, a file of a Switch archive, plays. The program finds the places the same way.
inline std::vector<std::optional<edit::SoundPlace>> PlacesOf(const format::File& file)
{
    const std::vector<uint8_t> built = format::ExportArchive(file);
    const std::shared_ptr<const ntdlib::sar::SoundArchive> archive = song::LoadArchive(built, "test.bfsar");
    return edit::SoundPlaces(*archive, format::SequencesOfFiles(file, built, *archive),
                             format::StartFieldsOfSounds(file, built));
}

// Sound `sound` as ntdlib's performer plays it from the Switch archive that `file` exports.
inline song::Sequence PlayedSequence(const format::File& file, uint32_t sound)
{
    return song::ReadSequence(*song::LoadArchive(format::ExportArchive(file), "test.bfsar"), sound);
}

} // namespace hokuto::testing
