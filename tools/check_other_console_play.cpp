// SPDX-License-Identifier: MIT

// hokuto_check_other_console: the plays of a sound on each console, and the comparisons of their notes.

#include "check_other_console.h"

namespace hokuto::tools::other_console
{

Played Play(const std::shared_ptr<const sar::SoundArchive>& archive, uint32_t sound, double seconds)
{
    const std::atomic<bool> kCancel = false;
    Played played{std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound)), {}};
    played.render = song::RenderDry(archive, played.sequence, song::Mixing::kStrict, seconds, kCancel);

    return played;
}

Played PlayWii(const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive, uint32_t sound, double seconds,
               const ntdlib::SndBuild& build)
{
    const std::atomic<bool> kCancel = false;
    Played played{std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound, {}, {}, build)), {}};
    played.render = song::RenderDry(archive, played.sequence, song::Mixing::kStrict, seconds, kCancel);

    return played;
}

Played PlayDs(const std::shared_ptr<const song::DsArchive>& archive, uint32_t sound, double seconds,
              song::Mixing mixing)
{
    const std::atomic<bool> kCancel = false;
    Played played{std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound)), {}};
    played.render = song::RenderDry(archive, played.sequence, mixing, seconds, kCancel);

    return played;
}

Played PlayDse(const std::shared_ptr<const song::DseSet>& set, uint32_t sound, double seconds)
{
    const std::atomic<bool> kCancel = false;
    Played played{std::make_shared<const song::Sequence>(song::ReadSequence(*set, sound)), {}};
    played.render = song::RenderDry(set, played.sequence, song::Mixing::kStrict, seconds, kCancel);

    return played;
}

Played PlayGba(const std::shared_ptr<const song::GbaRom>& rom, uint32_t number, double seconds)
{
    const std::atomic<bool> kCancel = false;
    Played played{std::make_shared<const song::Sequence>(song::ReadSequence(*rom, static_cast<int>(number))), {}};
    played.render = song::RenderDry(rom, played.sequence, song::Mixing::kStrict, seconds, kCancel);

    return played;
}

Played PlayDriver(const std::shared_ptr<const song::DriverRom>& rom, uint32_t sound, double seconds)
{
    const std::atomic<bool> kCancel = false;
    Played played{std::make_shared<const song::Sequence>(rom->ReadSequence(sound)), {}};
    played.render = song::RenderDry(rom, played.sequence, song::Mixing::kStrict, seconds, kCancel);

    return played;
}

std::optional<double> GbaNotesOff(const song::Sequence& gba, const song::Sequence& sent, double seconds, bool squared)
{
    const auto timed = [](const song::Sequence& sequence, bool nearest_square)
    {
        std::vector<std::array<double, 3>> notes;
        for (const song::Track& track : sequence.tracks)
        {
            for (const song::Note& note : track.notes)
            {
                const double velocity =
                    nearest_square ? std::round(127.0 * std::sqrt(note.velocity / 127.0)) : note.velocity;
                // On Konami's driver, the key of a note shows the pitch of its sample. Its MIDI file contains the key
                // of the driver.
                notes.push_back(
                    {song::MsOfTick(sequence, note.start),
                     static_cast<double>(song::KonamiRevisionOf(sequence.driver) ? note.midi_key : note.key),
                     velocity});
            }
        }
        std::ranges::sort(notes);

        return notes;
    };

    // The MIDI file of Konami's driver keeps the levels of a song in its controllers, and each note has the highest
    // velocity.
    const bool levels = !song::KonamiRevisionOf(gba.driver);
    std::vector<std::array<double, 3>> before = timed(gba, squared);
    std::vector<std::array<double, 3>> after = timed(sent, false);
    std::erase_if(before, [seconds](const std::array<double, 3>& note) { return note[0] >= seconds * 1000.0; });
    if (after.size() < before.size())
    {
        return std::nullopt;
    }
    after.resize(before.size());
    double off = 0.0;
    for (std::size_t i = 0; i < before.size(); i++)
    {
        if (before[i][1] != after[i][1] || (levels && before[i][2] != after[i][2]))
        {
            return std::nullopt;
        }
        off = std::max(off, std::abs(before[i][0] - after[i][0]));
    }

    return off;
}

bool SameNotes(const song::Sequence& a, const song::Sequence& b)
{
    const auto starts = [](const song::Sequence& sequence)
    {
        std::vector<std::vector<std::array<uint32_t, 5>>> tracks;
        for (const song::Track& track : sequence.tracks)
        {
            std::vector<std::array<uint32_t, 5>>& notes = tracks.emplace_back();
            for (const song::Note& note : track.notes)
            {
                notes.push_back({note.start, note.key, note.velocity, note.bank_slot, note.program});
            }
        }

        return tracks;
    };

    return starts(a) == starts(b);
}

song::Sequence InOneBank(song::Sequence sequence, const std::map<int, uint32_t>& slot_programs)
{
    for (song::Track& track : sequence.tracks)
    {
        for (song::Note& note : track.notes)
        {
            if (const auto range = slot_programs.find(note.bank_slot); range != slot_programs.end())
            {
                note.bank_slot = 0;
                note.program += range->second;
            }
        }
    }

    return sequence;
}

} // namespace hokuto::tools::other_console
