// SPDX-License-Identifier: MIT

// What the imports of MIDI files share: the read of the file, the data of a meta event, the names of a loop's markers,
// and the warnings for the events that an import leaves out.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"

namespace hokuto::edit
{

// The MIDI file `midi`, read for an import. Throws std::invalid_argument with a message for the user for a file that
// seqlib can't read, and for a file in SMPTE time.
seqlib::midi::File ReadMidiToImport(std::span<const uint8_t> midi);

// The type and the data of a meta event, as seqlib::midi::Read reads it. The event's bytes are FF, the type, the length
// and the data.
std::pair<uint8_t, std::vector<uint8_t>> MetaOf(const seqlib::midi::Event& event);

// Whether `text` names the loop's start (or its end with `end`): loopStart, loop_start or Loop Start, in any case and
// with any underscores and spaces.
bool NamesLoop(const std::vector<uint8_t>& text, bool end);

// The description of a channel message that the song leaves out, for the warnings.
std::string LeftOut(uint8_t status, uint8_t data);

// Adds to `warnings` one warning for each description in `left_out` (LeftOut), with the number of events.
void AddLeftOutWarnings(const std::map<std::string, std::size_t>& left_out, std::vector<std::string>& warnings);

} // namespace hokuto::edit
