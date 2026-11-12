// SPDX-License-Identifier: MIT

#include "edit/midi_events.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "format/kinds.h"

namespace hokuto::edit
{

seqlib::midi::File ReadMidiToImport(std::span<const uint8_t> midi)
{
    seqlib::midi::File file;
    try
    {
        file = seqlib::midi::Read(midi);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("Can't read the MIDI file. " + format::AsSentence(error.what()));
    }

    if ((file.division & 0x8000) != 0 || file.division == 0)
    {
        throw std::invalid_argument("The MIDI file uses SMPTE time. Hokuto supports only ticks per quarter note.");
    }

    return file;
}

std::pair<uint8_t, std::vector<uint8_t>> MetaOf(const seqlib::midi::Event& event)
{
    return {event.bytes[1], seqlib::midi::MetaData(event)};
}

bool NamesLoop(const std::vector<uint8_t>& text, bool end)
{
    std::string name;
    for (const uint8_t c : text)
    {
        if (c != '_' && c != ' ')
        {
            name.push_back(static_cast<char>(std::tolower(c)));
        }
    }

    return name == (end ? "loopend" : "loopstart");
}

std::string LeftOut(uint8_t status, uint8_t data)
{
    switch (status & 0xF0)
    {
    case 0xA0:
        return "key aftertouch";
    case 0xB0:
        return "control change " + std::to_string(data);
    case 0xD0:
        return "channel aftertouch";
    default:
        return "other MIDI messages";
    }
}

void AddLeftOutWarnings(const std::map<std::string, std::size_t>& left_out, std::vector<std::string>& warnings)
{
    for (const auto& [what, count] : left_out)
    {
        warnings.push_back(std::to_string(count) + (count == 1 ? " event left out: " : " events left out: ") + what +
                           ".");
    }
}

} // namespace hokuto::edit
