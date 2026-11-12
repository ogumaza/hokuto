// SPDX-License-Identifier: MIT

// The views of a GBA voice group, with the other places that play each voice and the moves of voice groups, and
// of the samples of the GBA's other drivers.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/song.h"

#include "app/item_views.h"
#include "app/item_views_parts.h"
#include "app/key_edits.h"
#include "edit/gba_voices.h"
#include "edit/konami_samples.h"
#include "song/driver.h"
#include "song/gba.h"
#include "song/konami.h"

namespace hokuto::app
{
namespace
{

using item_views_parts::kHighest;

// The places of a ROM's voice groups that play a voice's bytes, by the voice's address.
using GbaPlaces = std::map<uint32_t, std::vector<std::string>>;

// The bytes of a ROM that contain other things than the voices of key splits and drum kits: the voices of the voice
// groups' programs, each key split's key table of 128 bytes, the song table, and the patterns of 16 bytes that the wave
// voices play. A drum kit's table, or a key split's voices, can run on past its voices into them. The voice that such a
// key reads overlaps them, at another place than a program's voice, and isn't one. An edit of it would change those
// bytes.
class GbaOthers
{
public:
    explicit GbaOthers(const song::GbaRom& rom, const std::vector<song::GbaVoiceGroup>& groups)
    {
        constexpr uint32_t kKeyTableBytes = 128;
        constexpr uint32_t kSongBytes = 8;
        std::vector<ntdlib::mp2k::Voice> splits;
        for (const song::GbaVoiceGroup& group : groups)
        {
            for (const int program : group.programs)
            {
                const uint32_t address = group.address + ntdlib::mp2k::kVoiceSize * static_cast<uint32_t>(program);
                ntdlib::mp2k::Voice voice;
                if (ntdlib::mp2k::ReadVoice(rom.rom, address, voice))
                {
                    voices_.insert(address);
                    if ((voice.type & ntdlib::mp2k::kVoiceKeySplit) != 0)
                    {
                        spans_.emplace_back(voice.key_table, voice.key_table + kKeyTableBytes);
                    }
                    if (voice.IsSplit())
                    {
                        splits.push_back(voice);
                    }
                    AddPattern(voice);
                }
            }
        }
        spans_.emplace_back(rom.info.song_table,
                            rom.info.song_table + kSongBytes * static_cast<uint32_t>(rom.info.song_count));

        // The patterns of the wave voices that the keys play. A key whose voice overlaps the other bytes names no
        // pattern.
        std::vector<ntdlib::mp2k::Voice> keys;
        for (const ntdlib::mp2k::Voice& split : splits)
        {
            for (int key = 0; key <= kHighest; key++)
            {
                const uint32_t address = ntdlib::mp2k::SplitVoice(rom.rom, split, key);
                ntdlib::mp2k::Voice voice;
                if (address != 0 && !Across(address) && ntdlib::mp2k::ReadVoice(rom.rom, address, voice))
                {
                    keys.push_back(voice);
                }
            }
        }
        for (const ntdlib::mp2k::Voice& voice : keys)
        {
            AddPattern(voice);
        }
    }

    // Whether the voice at `address` overlaps those bytes.
    bool Across(uint32_t address) const
    {
        const uint32_t end = address + ntdlib::mp2k::kVoiceSize;
        const auto program =
            voices_.lower_bound(address < ntdlib::mp2k::kVoiceSize ? 0 : address - ntdlib::mp2k::kVoiceSize + 1);
        for (auto it = program; it != voices_.end() && *it < end; ++it)
        {
            if (*it != address)
            {
                return true;
            }
        }
        return std::ranges::any_of(spans_, [address, end](const std::pair<uint32_t, uint32_t>& span)
                                   { return address < span.second && span.first < end; });
    }

private:
    static constexpr uint32_t kPatternBytes = 16;

    void AddPattern(const ntdlib::mp2k::Voice& voice)
    {
        if (voice.PsgChannel() == 3)
        {
            spans_.emplace_back(voice.wave, voice.wave + kPatternBytes);
        }
    }

    std::set<uint32_t> voices_;
    std::vector<std::pair<uint32_t, uint32_t>> spans_;
};

// The rows of `split`, a key split or a drum kit: each run of keys of a key split that plays one voice, or each key of
// a drum kit. Each row has a voice that plays something. A voice that overlaps `others` stays out.
std::vector<GbaVoiceRow> GbaRowsOf(const song::GbaRom& rom, const ntdlib::mp2k::Voice& split, const GbaOthers& others)
{
    std::vector<GbaVoiceRow> rows;
    const bool drums = (split.type & ntdlib::mp2k::kVoiceDrumKit) != 0;
    int key = 0;
    while (key <= kHighest)
    {
        const uint32_t address = ntdlib::mp2k::SplitVoice(rom.rom, split, key);
        int last = key;
        while (!drums && last < kHighest && ntdlib::mp2k::SplitVoice(rom.rom, split, last + 1) == address)
        {
            last++;
        }

        ntdlib::mp2k::Voice voice;
        if (address != 0 && !others.Across(address) && ntdlib::mp2k::ReadVoice(rom.rom, address, voice) &&
            !voice.IsSplit() && song::GbaVoiceSounds(rom, voice))
        {
            rows.push_back({key, last, voice, song::GbaVoiceKind(voice), {}});
        }
        key = last + 1;
    }

    return rows;
}

// The name of a row's place in a voice group: the group's name and the program, with the row's keys.
std::string GbaPlaceName(uint32_t group, uint32_t program, const GbaVoiceRow* row)
{
    std::string name = song::GbaVoiceGroupName(group) + ", program " + std::to_string(program);
    if (row == nullptr)
    {
        return name;
    }

    name += KeysDetail(row->first_key, row->last_key);
    return name;
}

// Adds to `places` the places of `groups` that play each voice, to `view.pattern_places` those of each wave's pattern,
// and to `samples` the samples that they play. Adds the programs of the group at `address` to `view`.
void CollectGbaPlaces(const song::GbaRom& rom, const std::vector<song::GbaVoiceGroup>& groups, uint32_t address,
                      GbaBankView& view, GbaPlaces& places, std::set<uint32_t>& samples)
{
    const GbaOthers stray(rom, groups);
    // The word of a key split or a drum kit names its table, not a sample.
    const auto note = [&](const ntdlib::mp2k::Voice& voice, const std::string& place)
    {
        places[voice.address].push_back(place);
        if (voice.IsSplit())
        {
            return;
        }
        if (voice.PsgChannel() == 3)
        {
            view.pattern_places[voice.wave].push_back(place);
        }
        ntdlib::mp2k::Wave wave;
        if (voice.PsgChannel() == 0 && ntdlib::mp2k::ReadWave(rom.rom, voice.wave, wave) && wave.size > 0)
        {
            samples.insert(voice.wave);
        }
    };
    for (const song::GbaVoiceGroup& group : groups)
    {
        const std::vector<int> listed = group.address == address ? song::GbaListedPrograms(rom, group) : group.programs;
        for (const int number : listed)
        {
            const auto program = static_cast<uint32_t>(number);
            GbaProgram shown{program, {}, {}};
            ntdlib::mp2k::Voice voice;
            if (!ntdlib::mp2k::ReadVoice(rom.rom, group.address + ntdlib::mp2k::kVoiceSize * program, voice))
            {
                continue;
            }
            shown.voice = {0, kHighest, voice, song::GbaVoiceKind(voice), {}};
            note(voice, GbaPlaceName(group.address, program, nullptr));
            if (voice.IsSplit())
            {
                shown.rows = GbaRowsOf(rom, voice, stray);
                for (const GbaVoiceRow& row : shown.rows)
                {
                    note(row.voice, GbaPlaceName(group.address, program, &row));
                }
            }
            if (group.address == address)
            {
                view.programs.push_back(std::move(shown));
            }
        }
    }
}

// Sets the other places of each voice of `view`, and reads the patterns of its wave voices.
void FillGbaShared(const song::GbaRom& rom, uint32_t address, GbaPlaces& places, GbaBankView& view)
{
    // A voice's other places, and the patterns of the group's wave voices.
    const auto others = [&places](const ntdlib::mp2k::Voice& voice, const std::string& place)
    {
        std::vector<std::string> shared;
        for (const std::string& other : places[voice.address])
        {
            if (other != place)
            {
                shared.push_back(other);
            }
        }
        return shared;
    };
    for (GbaProgram& program : view.programs)
    {
        program.voice.shared = others(program.voice.voice, GbaPlaceName(address, program.program, nullptr));
        std::vector<const GbaVoiceRow*> voices = {&program.voice};
        for (GbaVoiceRow& row : program.rows)
        {
            row.shared = others(row.voice, GbaPlaceName(address, program.program, &row));
            voices.push_back(&row);
        }
        for (const GbaVoiceRow* row : voices)
        {
            if (row->voice.PsgChannel() == 3 && !view.patterns.contains(row->voice.wave))
            {
                try
                {
                    view.patterns[row->voice.wave] = edit::GbaPatternOf(rom.rom, row->voice.wave);
                }
                catch (const std::invalid_argument&)
                {
                    // The pattern is outside the ROM. The fields show no steps.
                }
            }
        }
    }
}

// Adds the samples at `samples` to `view`.
void AddGbaSamples(const song::GbaRom& rom, const std::set<uint32_t>& samples, GbaBankView& view)
{
    for (const uint32_t sample : samples)
    {
        ntdlib::mp2k::Wave wave;
        ntdlib::mp2k::ReadWave(rom.rom, sample, wave);
        view.samples.push_back({sample, wave.frequency / 1024.0, wave.loops, wave.size});
        view.sample_names.push_back(GbaSampleName(sample));
    }
}

} // namespace

std::string GbaSampleName(uint32_t address)
{
    return "Sample " + ntdlib::agb::Hex(address);
}

GbaVoiceType GbaVoiceTypeOf(const ntdlib::mp2k::Voice& voice)
{
    if ((voice.type & ntdlib::mp2k::kVoiceDrumKit) != 0)
    {
        return GbaVoiceType::kDrumKit;
    }
    if ((voice.type & ntdlib::mp2k::kVoiceKeySplit) != 0)
    {
        return GbaVoiceType::kKeySplit;
    }

    switch (voice.PsgChannel())
    {
    case 0:
        return (voice.type & ntdlib::mp2k::kVoiceFixed) != 0 ? GbaVoiceType::kFixedSample : GbaVoiceType::kSample;
    case 1:
        return GbaVoiceType::kSquare1;
    case 2:
        return GbaVoiceType::kSquare2;
    case 3:
        return GbaVoiceType::kWave;
    case 4:
        return GbaVoiceType::kNoise;
    default:
        return GbaVoiceType::kUnknown;
    }
}

std::vector<edit::GbaKeyRow> KeyRowsOf(const GbaProgram& program)
{
    std::vector<edit::GbaKeyRow> rows;
    rows.reserve(program.rows.size());
    for (const GbaVoiceRow& row : program.rows)
    {
        rows.push_back({row.first_key, row.last_key, row.voice});
    }

    return rows;
}

GbaBankView MakeGbaBankView(const song::GbaRom& rom, uint32_t address)
{
    const std::vector<song::GbaVoiceGroup> groups = song::GbaVoiceGroups(rom);
    const auto found = std::ranges::find(groups, address, &song::GbaVoiceGroup::address);
    if (found == groups.end())
    {
        throw std::invalid_argument("No song in the ROM uses " + song::GbaVoiceGroupName(address) + ".");
    }

    // Each place of each voice group that plays a voice's bytes, a wave's pattern or a sample.
    GbaBankView view;
    GbaPlaces places;
    std::set<uint32_t> samples;
    CollectGbaPlaces(rom, groups, address, view, places, samples);
    FillGbaShared(rom, address, places, view);
    AddGbaSamples(rom, samples, view);

    for (const int song : found->songs)
    {
        view.songs.push_back(song::GbaSongName(rom, song));
    }
    view.address = address;
    view.name = song::GbaVoiceGroupName(address);
    view.song = found->songs.empty() ? -1 : found->songs.front();
    view.free_programs = edit::FreeGbaPrograms(rom, address);

    return view;
}

void RecordGbaGroupMove(GbaGroupMoves& moves, uint32_t from, uint32_t to)
{
    // A move between the same addresses as an earlier one becomes the latest.
    for (const auto& [address, other] : {std::pair(from, to), std::pair(to, from)})
    {
        std::vector<uint32_t>& others = moves[address];
        std::erase(others, other);
        others.push_back(other);
    }
}

uint32_t FollowedGbaGroup(const GbaGroupMoves& moves, uint32_t address, const std::vector<song::GbaVoiceGroup>& groups)
{
    const auto played = [&groups](uint32_t at)
    {
        return std::ranges::find(groups, at, &song::GbaVoiceGroup::address) != groups.end();
    };
    const auto found = moves.find(address);
    if (played(address) || found == moves.end())
    {
        return address;
    }

    // The latest move whose other address a song plays.
    const auto other = std::find_if(found->second.rbegin(), found->second.rend(), played);
    return other != found->second.rend() ? *other : address;
}

DriverSampleSongs SongsOfDriverSamples(const song::DriverRom& rom)
{
    // The songs of each sample, by the notes of the songs' commands.
    DriverSampleSongs songs;
    songs.state = DriverSongsState::kRead;
    for (const uint32_t sound : rom.Sounds())
    {
        for (const int sample : rom.SamplesOfSound(sound))
        {
            songs.songs[sample].push_back(rom.SoundName(sound));
        }
    }

    // The songs whose notes of other samples take each sample's rate, apart from the songs that play it.
    const song::KonamiRom* konami = song::KonamiOf(&rom);
    for (const uint32_t sound : konami != nullptr ? rom.Sounds() : std::vector<uint32_t>())
    {
        for (const int sample : song::KonamiBorrowedRates(*konami, sound))
        {
            const std::vector<std::string>& playing = songs.songs[sample];
            if (std::ranges::find(playing, rom.SoundName(sound)) == playing.end())
            {
                songs.rate_songs[sample].push_back(rom.SoundName(sound));
            }
        }
    }

    return songs;
}

void ShowDriverSampleSongs(DriverBankView& view, const DriverSampleSongs& songs)
{
    for (DriverSampleView& shown : view.samples)
    {
        const auto played = songs.songs.find(static_cast<int>(shown.sample));
        const auto rated = songs.rate_songs.find(static_cast<int>(shown.sample));
        shown.songs = played != songs.songs.end() ? played->second : std::vector<std::string>();
        shown.rate_songs = rated != songs.rate_songs.end() ? rated->second : std::vector<std::string>();
        shown.songs_state = songs.state;
    }
}

DriverBankView MakeDriverBankView(const song::DriverRom& rom, const DriverSampleSongs& songs)
{
    // Konami's driver contains the rates and the loops in its sample table, and the editor edits them.
    const song::KonamiRom* konami = song::KonamiOf(&rom);
    DriverBankView view;
    const std::vector<int> samples = rom.Samples();
    for (const int sample : samples)
    {
        const song::SampleShape shape = rom.ShapeOf(sample);
        DriverSampleView shown;
        shown.sample = static_cast<uint32_t>(sample);
        shown.rate = static_cast<int>(std::lround(rom.SampleRate(sample)));
        shown.length = shape.length;
        shown.loop_start = shape.loop_start;
        if (konami != nullptr)
        {
            const edit::KonamiRates rates = edit::KonamiSampleRates(*konami);
            shown.editable = true;
            shown.least_rate = rates.least;
            shown.most_rate = rates.most;
            shown.every_pitch = rates.every_pitch;
            shown.loops = edit::KonamiSampleLoops(*konami, sample);
            shown.no_loop_reason = edit::KonamiNoLoopReason(*konami, sample);
            for (const int other : edit::KonamiSamplesSharing(*konami, sample, samples))
            {
                shown.sharing.push_back(rom.SampleName(other));
            }
        }
        view.samples.push_back(std::move(shown));
    }

    ShowDriverSampleSongs(view, songs);
    return view;
}

} // namespace hokuto::app
