// SPDX-License-Identifier: MIT

// hokuto_check_other_console: the choices of the sounds to check on each console.

#include "check_other_console.h"

namespace hokuto::tools::other_console
{

bool Draws(const ntdlib::commands::Command& command)
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;

    return command.kind == Kind::kRandomVariable || command.value.source == Source::kRandom ||
           (command.time && command.time->source == Source::kRandom);
}

bool SetsBiquad(const ntdlib::commands::Command& command)
{
    using ntdlib::commands::Kind;

    return command.kind == Kind::kBiquadType || command.kind == Kind::kBiquadValue;
}

namespace
{

// Whether a command that the player reaches from item `start` of `sequence` passes `test`. The player's paths lead
// through the track starts too.
bool Reaches(const ntdlib::commands::Sequence& sequence, uint32_t start, const Test& test)
{
    const std::vector<bool> reached = edit::Reached(sequence, start, true);
    for (std::size_t item = 0; item < sequence.items.size(); item++)
    {
        if (reached[item] && test(sequence.items[item]))
        {
            return true;
        }
    }

    return false;
}

} // namespace

bool SoundReaches(const sar::SoundArchive* game, const format::File& file, const std::optional<edit::SoundPlace>& where,
                  uint32_t sound, const Test& test)
{
    if (where)
    {
        const uint32_t start = where->start_field ? file.native->fields.at(*where->start_field).to.offset : 0;
        return Reaches(*file.sequences.at(where->sequence), start, test);
    }
    if (!game)
    {
        return false;
    }

    const edit::Incoming incoming =
        edit::ReadIncoming(song::ItemFile(*game, {.kind = song::ItemKind::kSequence, .index = sound}));
    const std::optional<uint32_t> start = edit::ItemAtOffset(
        edit::Offsets(incoming.sequence, song::Console{.engine = hokuto::format::EngineOf(incoming.flavor)}),
        game->Sounds()[sound].start_offset);
    return start && Reaches(incoming.sequence, *start, test);
}

namespace
{

// Up to `most` of `sequences`, spread evenly, or all of them with 0 or with `named`.
std::vector<uint32_t> Spread(const std::vector<uint32_t>& sequences, std::size_t most, bool named)
{
    if (named || most == 0 || sequences.size() <= most)
    {
        return sequences;
    }

    std::vector<uint32_t> chosen;
    for (const std::size_t index : SpreadIndices(sequences.size(), most))
    {
        chosen.push_back(sequences[index]);
    }

    return chosen;
}

} // namespace

std::vector<uint32_t> Chosen(const format::File& file, std::size_t most, const std::set<std::string>& names,
                             const std::function<bool(uint32_t)>& keep)
{
    std::vector<uint32_t> sequences;
    for (uint32_t sound = 0; sound < file.native->tables->sounds.size(); sound++)
    {
        const auto& entry = file.native->tables->sounds[sound];
        if (entry.kind == sar::SoundType::kSequence && (names.empty() || names.contains(entry.name)) &&
            (!keep || keep(sound)))
        {
            sequences.push_back(sound);
        }
    }

    return Spread(sequences, most, !names.empty());
}

std::vector<uint32_t> GbaChosen(const song::GbaRom& rom, std::size_t most, const std::set<std::string>& names)
{
    std::vector<uint32_t> songs;
    for (const int song : song::GbaSongs(rom))
    {
        if (names.empty() || names.contains(song::GbaSongName(rom, song)))
        {
            songs.push_back(static_cast<uint32_t>(song));
        }
    }

    return Spread(songs, most, !names.empty());
}

std::vector<uint32_t> DriverChosen(const song::DriverRom& rom, std::size_t most, const std::set<std::string>& names)
{
    std::vector<uint32_t> sounds;
    for (const uint32_t sound : rom.Sounds())
    {
        if (names.empty() || names.contains(rom.SoundName(sound)))
        {
            sounds.push_back(sound);
        }
    }

    return Spread(sounds, most, !names.empty());
}

std::vector<uint32_t> DsChosen(const song::DsArchive& archive, std::size_t most, const std::set<std::string>& names)
{
    std::vector<uint32_t> sounds;
    for (const uint32_t sound : song::DsSounds(archive))
    {
        if (names.empty() || names.contains(song::DsSoundName(archive, sound)))
        {
            sounds.push_back(sound);
        }
    }

    return Spread(sounds, most, !names.empty());
}

std::vector<uint32_t> DseChosen(const song::DseSet& set, std::size_t most, const std::set<std::string>& names)
{
    std::vector<uint32_t> sounds;
    for (const uint32_t sound : song::DseSounds(set))
    {
        if (names.empty() || names.contains(song::DseSoundName(set, sound)))
        {
            sounds.push_back(sound);
        }
    }

    return Spread(sounds, most, !names.empty());
}

} // namespace hokuto::tools::other_console
