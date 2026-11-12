// SPDX-License-Identifier: MIT

// The reader of the songs of Quintet's GBA driver.

#include "ntdlib/quintet/commands.h"
#include "ntdlib/quintet/song.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/quintet/performance.h"

#include "format/quintet.h"
#include "song/quintet.h"
#include "song/sequence_parts.h"

namespace hokuto::song
{
namespace
{

namespace export_ = ntdlib::sequence_export;

using sequence_parts::AddTrack;
using sequence_parts::ClipToLoop;
using sequence_parts::ProgramPreset;
using sequence_parts::ReadTrack;
using sequence_parts::SequenceHead;
using sequence_parts::ThrowUnreadable;

// The instrument that each note command or tie of `commands`, a song of Quintet's driver, selects on `track` in each of
// the track's stretches, by the stretch and the item: the last duty (2F) on a square, and the last wave, noise macro or
// sample on the other channels. `items` contains the item at each offset. An instrument stays from one stretch to the
// next.
std::map<std::pair<uint32_t, uint32_t>, uint32_t> QuintetInstruments(const ntdlib::commands::Sequence& commands,
                                                                     const std::map<uint32_t, uint32_t>& items,
                                                                     const Track& track)
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::kOne;
    namespace quintet = ntdlib::quintet;
    constexpr uint8_t kDutyOpcode = 0x2F;
    const int channel = track.number;
    const uint8_t index = channel == quintet::kWave ? 2 : channel == quintet::kNoise ? 1 : 0;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> instruments;
    uint32_t instrument = 0;
    for (std::size_t s = 0; s < track.stretches.size(); s++)
    {
        const auto first = items.find(track.stretches[s].first);
        const auto last = items.find(track.stretches[s].last);
        if (first == items.end() || last == items.end())
        {
            continue;
        }

        for (uint32_t i = first->second; i <= last->second && i < commands.items.size(); i++)
        {
            const ntdlib::commands::Command& command = commands.items[i];
            if (channel < quintet::kWave && command.kind == Kind::kOther && command.encoding.opcode == kDutyOpcode &&
                !command.bytes.empty())
            {
                instrument = command.bytes[0];
            }
            else if (channel >= quintet::kWave && command.kind == Kind::kProgram && command.index == index)
            {
                instrument = static_cast<uint32_t>(command.value.value / kOne);
            }
            else if (command.kind == Kind::kNote || command.kind == Kind::kTie)
            {
                instruments.emplace(std::pair(static_cast<uint32_t>(s), i), instrument);
            }
        }
    }

    return instruments;
}

// The items of a song of Quintet's driver: the item at each offset, and the offset of each item. None for a song that
// can't be decoded.
struct QuintetItems
{
    std::map<uint32_t, uint32_t> items;
    std::vector<uint32_t> offsets;
};

// The items of `decoded`, a song of the driver `info` in `rom`.
QuintetItems ItemsOfSong(const ntdlib::quintet::Rom& rom, const ntdlib::quintet::DriverInfo& info,
                         const ntdlib::quintet::Decoded& decoded)
{
    namespace quintet = ntdlib::quintet;
    QuintetItems found;
    if (!decoded.opaque)
    {
        found.offsets = quintet::ItemOffsets(decoded.sequence, quintet::TraitsOf(rom, info));
        found.items = ItemsByOffset(found.offsets);
    }

    return found;
}

// Ends each stretch of `track` where a file of `decoded` ends it. `items` contains the item at each offset, and
// `offsets` the offset of each item.
void FoldDots(Track& track, const ntdlib::quintet::Decoded& decoded, const std::map<uint32_t, uint32_t>& items,
              const std::vector<uint32_t>& offsets)
{
    // A stretch of ntdlib's performer ends at a note's dot. A file folds the dot into the note before it
    // (format::QuintetFoldsDot), and the stretch ends at that item.
    for (Stretch& stretch : track.stretches)
    {
        const auto last = items.find(stretch.last);
        if (last != items.end() && format::QuintetFoldsDot(decoded.sequence, last->second))
        {
            stretch.last = offsets[last->second - 1];
        }
    }
}

// Sets the instrument of each note of `track` (QuintetInstruments), and the track's program to the program of its first
// note.
void SetQuintetPrograms(Track& track, const ntdlib::quintet::Decoded& decoded,
                        const std::map<uint32_t, uint32_t>& items)
{
    const std::map<std::pair<uint32_t, uint32_t>, uint32_t> instruments =
        QuintetInstruments(decoded.sequence, items, track);
    for (Note& note : track.notes)
    {
        const auto item = items.find(note.command);
        const auto instrument =
            item == items.end() ? instruments.end() : instruments.find(std::pair(note.stretch, item->second));
        note.program = instrument == instruments.end() ? 0 : instrument->second;
        note.bank_slot = 0;
        note.velocity = 127;
        note.midi_key = note.key;
    }

    track.program = track.notes.empty() ? -1 : static_cast<int>(track.notes.front().program);
    track.bank.clear();
}

} // namespace

Sequence ReadSequence(const QuintetRom& rom, uint32_t sound)
{
    namespace quintet = ntdlib::quintet;
    const std::string name = rom.SoundName(sound);
    const auto file = rom.FileOf(static_cast<int>(sound));
    if (!file)
    {
        throw std::runtime_error("The ROM has no song " + std::to_string(sound) + ".");
    }

    // The performer plays one pass of the loop. A song whose header can't be read makes a performance with a warning
    // and no stretches.
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.loops = 1;
    try
    {
        const export_::quintet::Performance performance =
            export_::quintet::PerformSong(rom.rom, *file->first, file->second, settings);
        if (!performance.warnings.empty() &&
            std::ranges::all_of(performance.stretches, [](const auto& stretches) { return stretches.empty(); }))
        {
            throw std::invalid_argument("its header can't be read");
        }

        Sequence sequence = SequenceHead(performance, name, sound);
        sequence.driver = &rom.Rules();

        // The song's commands, for the notes' instruments.
        const quintet::Decoded decoded =
            quintet::DecodeRomSequence(rom.rom, *file->first, quintet::SequenceOfSong(*file->first, file->second));
        const auto [items, offsets] = ItemsOfSong(rom.rom, *file->first, decoded);

        const auto bank_name = [](uint32_t)
        {
            return std::string();
        };
        for (std::size_t i = 0; i < performance.tracks.size() && i < quintet::kChannels; i++)
        {
            Track track = ReadTrack(static_cast<int>(i), performance.tracks[i], performance, ProgramPreset, bank_name,
                                    rom.Rules().StartVolume());
            if (track.notes.empty() && track.stretches.empty())
            {
                continue;
            }

            FoldDots(track, decoded, items, offsets);
            ClipToLoop(sequence, track);
            SetQuintetPrograms(track, decoded, items);
            AddTrack(sequence, std::move(track));
        }

        return sequence;
    }
    catch (const std::invalid_argument& error)
    {
        ThrowUnreadable(name, error);
    }
    catch (const std::out_of_range& error)
    {
        ThrowUnreadable(name, error);
    }
}

} // namespace hokuto::song
