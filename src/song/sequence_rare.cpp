// SPDX-License-Identifier: MIT

// The reader of the songs of Rare's GBA driver.

#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/rare/performance.h"

#include "song/rare.h"
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

} // namespace

Sequence ReadSequence(const RareRom& rom, uint32_t sound)
{
    const std::string name = rom.SoundName(sound);
    const int song = rom.SongOf(sound);
    if (song < 0 || song >= ntdlib::rare::SongCount(rom.info))
    {
        throw std::runtime_error("The ROM has no song " + std::to_string(sound) + ".");
    }

    // The performer plays one pass of the loop.
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.loops = 1;
    settings.version = rom.VersionOf(sound);
    try
    {
        const export_::rare::Performance performance = export_::rare::PerformSong(rom.rom, rom.info, song, settings);

        Sequence sequence = SequenceHead(performance, name, sound);
        sequence.driver = &rom.Rules();

        // The tracks and the length follow the same rules as those of MP2K. A preset is a program of the tune's program
        // map, its one bank.
        const auto bank_name = [&name](uint32_t)
        {
            return name;
        };
        for (std::size_t i = 0; i < performance.tracks.size(); i++)
        {
            Track track = ReadTrack(static_cast<int>(i), performance.tracks[i], performance, ProgramPreset, bank_name,
                                    rom.Rules().StartVolume());
            if (track.notes.empty() && track.stretches.empty())
            {
                continue;
            }

            ClipToLoop(sequence, track);
            for (Note& note : track.notes)
            {
                note.velocity = static_cast<uint8_t>(std::clamp<int>(note.velocity, 1, 127));
                note.midi_key = note.key;
            }

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
