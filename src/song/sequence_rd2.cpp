// SPDX-License-Identifier: MIT

// The reader of the songs of R&D2's GBA driver.

#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/rd2/performance.h"

#include "song/rd2.h"
#include "song/sequence_parts.h"

namespace hokuto::song
{
namespace
{

namespace export_ = ntdlib::sequence_export;

using sequence_parts::AddTrack;
using sequence_parts::ClipToLoop;
using sequence_parts::ReadTrack;
using sequence_parts::SequenceHead;
using sequence_parts::ThrowUnreadable;

} // namespace

Sequence ReadSequence(const Rd2Rom& rom, uint32_t sound)
{
    const std::string name = rom.SoundName(sound);
    const int song = rom.SongOf(sound);
    if (song < 0 || song >= ntdlib::rd2::SongCount(rom.info))
    {
        throw std::runtime_error("The ROM has no song " + std::to_string(sound) + ".");
    }

    // The performer plays one pass of the loop.
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.loops = 1;
    settings.version = rom.VersionOf(sound);
    try
    {
        const export_::rd2::Performance performance = export_::rd2::PerformSong(rom.rom, rom.info, song, settings);

        Sequence sequence = SequenceHead(performance, name, sound);
        sequence.driver = &rom.Rules();

        // The tracks and the length follow the same rules as those of MP2K. A preset is 128 times the note's bank plus
        // its instrument. The note's bank slot is the first slot of the sound's list of banks that contains the bank.
        const std::vector<uint16_t> banks = rom.BankListOf(sound);
        const auto preset_of = [&banks](uint32_t preset)
        {
            export_::PresetKey key;
            const auto slot = std::ranges::find(banks, preset / 128);
            key.bank_slot = slot == banks.end() ? 0 : static_cast<uint8_t>(slot - banks.begin());
            key.program = preset % 128;
            return std::optional(key);
        };
        const auto bank_name = [&banks](uint32_t slot)
        {
            return slot < banks.size() ? Rd2BankName(banks[slot]) : std::string();
        };
        for (std::size_t i = 0; i < performance.tracks.size(); i++)
        {
            Track track = ReadTrack(static_cast<int>(i), performance.tracks[i], performance, preset_of, bank_name,
                                    rom.Rules().StartVolume());
            if (track.notes.empty() && track.stretches.empty())
            {
                continue;
            }

            ClipToLoop(sequence, track);
            for (Note& note : track.notes)
            {
                note.velocity = static_cast<uint8_t>(std::max(Rd2VelocityOf(note.velocity), 1));
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
