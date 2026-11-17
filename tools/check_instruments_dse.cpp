// SPDX-License-Identifier: MIT

// hokuto_check_instruments: the settings of a DSE channel at each note of an edited track. A selection of a program
// resets the channel's bend range, its LFOs and its envelope (ntdlib's Driver::State::SelectProgram), and a bank
// select selects the channel's program again. An edit of the instrument mustn't change those settings at a note.

#include "ntdlib/dse/player.h"

#include "check_instruments.h"

namespace hokuto::tools::instruments
{

namespace
{

// The driver's ticks that a run plays at most: 10 minutes at 10 ms a tick.
constexpr uint32_t kMostTicks = 60'000;

// The settings at each note of track `track` of sound `sound` of `set`, in the order that ntdlib's DSE driver plays
// them, until the track has played `notes` notes or the run reaches kMostTicks.
std::vector<DseNoteSettings> TrackSettings(const song::DseSet& set, uint32_t sound, int track, std::size_t notes)
{
    ntdlib::dse::Player player({.build = set.build});
    std::vector<DseNoteSettings> settings;
    ntdlib::dse::SequenceHooks hooks;
    hooks.on_note = [&player, &settings, track](const ntdlib::dse::NoteEvent& note)
    {
        if (note.track == track)
        {
            const ntdlib::dse::ChannelView channel = player.Driver().GetChannel(note.channel);
            // An LFO of type 0 is off. A selection of a program that has no such LFO sets the type alone.
            std::array<ntdlib::dse::LfoValues, 4> lfos = channel.lfos;
            for (ntdlib::dse::LfoValues& lfo : lfos)
            {
                lfo = lfo.type == 0 ? ntdlib::dse::LfoValues() : lfo;
            }
            settings.push_back({.bank = channel.bank,
                                .program = channel.program,
                                .has_program = channel.has_program,
                                .bend_range = channel.bend_range,
                                .lfos = lfos,
                                .envelope = channel.envelope});
        }
    };
    player.Driver().SetHooks(std::move(hooks));
    const ntdlib::dse::SequenceFiles files = set.sounds.at(sound).Files();
    player.LoadBanks(files);
    if (!player.Start(files))
    {
        return settings;
    }

    for (uint32_t tick = 0; tick < kMostTicks && settings.size() < notes; tick++)
    {
        player.RunTick();
    }

    return settings;
}

// The LFOs that program `program` of the bank with the id `bank` of sound `sound` of `set` gives its channel, or
// nothing where the sound has no such program.
std::optional<std::vector<std::array<uint8_t, 0x10>>> ProgramLfos(const song::DseSet& set, uint32_t sound,
                                                                  uint16_t bank, uint16_t program)
{
    for (const std::shared_ptr<const ntdlib::dse::WaveBank>& held : set.sounds.at(sound).banks)
    {
        if (song::DseBankId(*held) != bank || !held->instruments)
        {
            continue;
        }

        const auto& programs = held->instruments->programs;
        if (program < programs.size() && programs[program])
        {
            return programs[program]->lfos;
        }
    }

    return std::nullopt;
}

} // namespace

DseSettingsChange DseSettingsOf(const song::DseSet& before, const song::DseSet& after, uint32_t sound, int track,
                                std::size_t notes)
{
    const std::vector<DseNoteSettings> old_settings = TrackSettings(before, sound, track, notes);
    const std::vector<DseNoteSettings> new_settings = TrackSettings(after, sound, track, notes);
    DseSettingsChange change;
    for (std::size_t i = 0; i < std::min(old_settings.size(), new_settings.size()); i++)
    {
        const DseNoteSettings& old_note = old_settings[i];
        const DseNoteSettings& new_note = new_settings[i];
        const bool other = old_note.bank != new_note.bank || old_note.program != new_note.program;
        bool lfos = old_note.has_program && new_note.has_program;
        if (other && lfos)
        {
            const auto old_lfos = ProgramLfos(before, sound, old_note.bank, old_note.program);
            const auto new_lfos = ProgramLfos(after, sound, new_note.bank, new_note.program);
            lfos = old_lfos && new_lfos && *old_lfos == *new_lfos;
        }
        change.lfos += lfos ? 0 : 1;

        const char* differs = old_note.bend_range != new_note.bend_range ? "bend range"
                              : old_note.envelope != new_note.envelope   ? "envelope"
                              : lfos && old_note.lfos != new_note.lfos   ? "LFOs"
                                                                         : nullptr;
        if (differs != nullptr && change.change.empty())
        {
            change.change = "note " + std::to_string(i) + " of the track, its " + differs + " (program " +
                            std::to_string(old_note.program) + " of bank " + std::to_string(old_note.bank) +
                            ", then program " + std::to_string(new_note.program) + " of bank " +
                            std::to_string(new_note.bank) + ")";
        }
    }

    return change;
}

} // namespace hokuto::tools::instruments
