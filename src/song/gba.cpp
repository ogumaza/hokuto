// SPDX-License-Identifier: MIT

#include "song/gba.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/apu.h"
#include "agblib/mp2k/commands.h"
#include "agblib/mp2k/song.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/mp2k/performance.h"
#include "agblib/sequence_export/mp2k/soundfont.h"
#include "ntdlib/commands/units.h"
#include "seqlib/midi/performance.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "song/gba_bytes.h"

namespace hokuto::song
{

std::shared_ptr<const GbaRom> LoadGbaRom(std::vector<uint8_t> bytes)
{
    auto rom = std::make_shared<GbaRom>();
    rom->rom.Assign(std::move(bytes));
    std::string error;
    if (!agblib::mp2k::DetectDriver(rom->rom, {}, rom->info, error))
    {
        throw xsflib::FormatError(error.empty() ? "the ROM doesn't use MusicPlayer2000"
                                                : "the MusicPlayer2000 song table has no song that Hokuto can read");
    }
    if (GbaSongs(*rom).empty())
    {
        throw xsflib::FormatError("MusicPlayer2000 plays none of the songs in the song table");
    }

    return rom;
}

std::vector<int> GbaSongs(const GbaRom& rom)
{
    std::vector<int> songs;
    for (int song = 0; song < rom.info.song_count; song++)
    {
        agblib::mp2k::SongHeader header;
        if (agblib::mp2k::ReadSong(rom.rom, rom.info, song, header) &&
            agblib::mp2k::PlayedTracks(rom.rom, rom.info, song, header) > 0)
        {
            songs.push_back(song);
        }
    }

    return songs;
}

std::string GbaSongName(const GbaRom& rom, int song)
{
    return "Song " + agblib::sequence_export::agb::SongNumber(song, rom.info.song_count);
}

std::vector<uint32_t> GbaPlayerTracks(const GbaRom& rom, std::size_t songs)
{
    std::vector<uint32_t> tracks;
    for (std::size_t song = 0; song < songs; song++)
    {
        const int most = agblib::mp2k::SongPlayer(rom.rom, rom.info, static_cast<int>(song)).track_count;
        tracks.push_back(static_cast<uint32_t>(std::max(most, 0)));
    }

    return tracks;
}

int GbaRate(const GbaRom& rom)
{
    // The resolution is bits 6 and 7 of the sound bias register's high byte.
    return (rom.info.dac_bits >> 6 & 3) == 0 ? agblib::agb::kBaseOutputRate : agblib::agb::kMaxOutputRate;
}

namespace
{

constexpr uint32_t kVoiceBytes = agblib::mp2k::kVoiceSize;
constexpr uint32_t kVoiceWave = 4;
constexpr uint32_t kPatternBytes = 16;

// The programs that song `song`, whose header is `header`, selects: from its program commands, or from the notes that
// sound where the decoder can't read the song.
std::set<int> ProgramsOfSong(const GbaRom& rom, int song, const agblib::mp2k::SongHeader& header)
{
    std::set<int> selected;
    bool decoded = false;
    try
    {
        const agblib::mp2k::Decoded decoding =
            agblib::mp2k::DecodeSong(rom.rom, header, agblib::mp2k::NoteArguments(rom.info));
        decoded = !decoding.opaque;
        for (const ntdlib::commands::Command& command : decoding.sequence.items)
        {
            if (decoded && command.kind == ntdlib::commands::Kind::kProgram &&
                command.value.source == ntdlib::commands::Source::kValue)
            {
                selected.insert(static_cast<int>(command.value.value / ntdlib::commands::kOne));
            }
        }
    }
    catch (const std::invalid_argument&)
    {
        // The tracks aren't before the header. The performer reads them.
    }

    if (!decoded)
    {
        const agblib::sequence_export::mp2k::Performance performance =
            agblib::sequence_export::mp2k::PerformSong(rom.rom, rom.info, song, {});
        for (const std::vector<seqlib::midi::TrackEvent>& events : performance.tracks)
        {
            for (const seqlib::midi::TrackEvent& event : events)
            {
                if (event.kind == seqlib::midi::EventKind::kPreset)
                {
                    selected.insert(static_cast<int>(event.preset));
                }
            }
        }
    }

    return selected;
}

// The words of the songs' headers that name a voice group that moved.
std::set<uint32_t> MovedVoiceWords(const GbaRom& before, const GbaRom& after, const std::vector<GbaVoiceGroup>& groups)
{
    constexpr uint32_t kHeaderVoices = 4;
    std::set<uint32_t> moved_words;
    for (const GbaVoiceGroup& group : groups)
    {
        for (const int song : group.songs)
        {
            agblib::mp2k::SongHeader old_header;
            agblib::mp2k::SongHeader new_header;
            if (agblib::mp2k::ReadSong(before.rom, before.info, song, old_header) &&
                agblib::mp2k::ReadSong(after.rom, after.info, song, new_header) &&
                old_header.voices != new_header.voices)
            {
                moved_words.insert(old_header.address + kHeaderVoices);
                moved_words.insert(new_header.address + kHeaderVoices);
            }
        }
    }

    return moved_words;
}

// What the voice at `address` of `rom` plays: its bytes, and a wave voice's pattern. Nothing outside the ROM.
// `moved_words` read as 0. A wave voice's pattern counts only within `before`.
std::vector<uint8_t> VoicePlayed(const GbaRom& rom, uint32_t address, const std::set<uint32_t>& moved_words,
                                 const GbaRom& before)
{
    std::vector<uint8_t> bytes;
    const auto append = [&](uint32_t from, uint32_t size)
    {
        for (uint32_t at = from; at < from + size; at++)
        {
            bytes.push_back(moved_words.contains(at & ~3U) ? uint8_t{0} : *rom.rom.Ptr(at));
        }
    };
    if (address == 0 || !rom.rom.Contains(address, kVoiceBytes))
    {
        return bytes;
    }

    append(address, kVoiceBytes);
    agblib::mp2k::Voice voice;
    if (agblib::mp2k::ReadVoice(rom.rom, address, voice) && voice.PsgChannel() == 3 &&
        !moved_words.contains(address + kVoiceWave) && rom.rom.Contains(voice.wave, kPatternBytes) &&
        voice.wave + kPatternBytes <= before.rom.End())
    {
        append(voice.wave, kPatternBytes);
    }

    return bytes;
}

// Compares what the programs of the voice groups play before the edits and after them.
class VoiceComparison
{
public:
    VoiceComparison(const GbaRom& before, const GbaRom& after, const std::vector<GbaVoiceGroup>& groups)
        : before_(before), after_(after), moved_words_(MovedVoiceWords(before, after, groups))
    {
        // The programs that each group's songs select before the edits. For a song that the decoder can't read, the
        // performer finds the programs from the notes that sound. An edit can silence a voice there, and its program
        // then counts only before the edit.
        for (GbaVoiceGroup& group : GbaVoiceGroups(before))
        {
            programs_before_.emplace(group.address, std::move(group.programs));
        }
    }

    // Whether a program that the songs of `group` select plays otherwise, for songs that played the group at
    // `old_group` before the edits.
    bool ChangedFrom(const GbaVoiceGroup& group, uint32_t old_group) const
    {
        const auto program_changed = [&](int program)
        {
            return ProgramChanged(group.address, old_group, program);
        };

        std::set<int> programs(group.programs.begin(), group.programs.end());
        if (const auto found = programs_before_.find(old_group); found != programs_before_.end())
        {
            programs.insert(found->second.begin(), found->second.end());
        }

        return std::ranges::any_of(programs, program_changed);
    }

private:
    // Whether program `program` of the group at `group_address` plays otherwise than it did in the group at
    // `old_group`.
    bool ProgramChanged(uint32_t group_address, uint32_t old_group, int program) const
    {
        const uint32_t offset = kVoiceBytes * static_cast<uint32_t>(program);
        const uint32_t address = group_address + offset;
        if (Played(before_, old_group + offset) != Played(after_, address))
        {
            return true;
        }

        agblib::mp2k::Voice voice;
        if (!agblib::mp2k::ReadVoice(after_.rom, address, voice) || !voice.IsSplit())
        {
            return false;
        }

        for (int key = 0; key <= 127; key++)
        {
            if (Played(before_, agblib::mp2k::SplitVoice(before_.rom, voice, key)) !=
                Played(after_, agblib::mp2k::SplitVoice(after_.rom, voice, key)))
            {
                return true;
            }
        }

        return false;
    }

    // What the voice at `address` of `rom` plays (VoicePlayed).
    std::vector<uint8_t> Played(const GbaRom& rom, uint32_t address) const
    {
        return VoicePlayed(rom, address, moved_words_, before_);
    }

    const GbaRom& before_;
    const GbaRom& after_;
    const std::set<uint32_t> moved_words_;
    std::map<uint32_t, std::vector<int>> programs_before_;
};

} // namespace

std::vector<GbaVoiceGroup> GbaVoiceGroups(const GbaRom& rom)
{
    std::map<uint32_t, std::set<int>> programs;
    std::map<uint32_t, std::vector<int>> songs;
    for (const int song : GbaSongs(rom))
    {
        agblib::mp2k::SongHeader header;
        if (!agblib::mp2k::ReadSong(rom.rom, rom.info, song, header))
        {
            continue;
        }

        songs[header.voices].push_back(song);
        const std::set<int> selected = ProgramsOfSong(rom, song, header);
        programs[header.voices].insert(selected.begin(), selected.end());
    }

    std::vector<GbaVoiceGroup> groups;
    for (const auto& [address, named] : songs)
    {
        const std::set<int>& selected = programs[address];
        groups.push_back({address, std::vector<int>(selected.begin(), selected.end()), named});
    }

    return groups;
}

std::vector<int> GbaSongsWithOtherVoices(const GbaRom& before, const GbaRom& after)
{
    const std::vector<GbaVoiceGroup> groups = GbaVoiceGroups(after);
    const VoiceComparison comparison(before, after, groups);

    // A program plays otherwise when its voice does, or when a key of its table plays another voice or the voice
    // otherwise. A key split's key table can change too. A voice group that moved has its voices at other addresses:
    // each song's header names the group that it played before.
    std::set<int> songs;
    for (const GbaVoiceGroup& group : groups)
    {
        std::map<uint32_t, bool> changed;
        for (const int song : group.songs)
        {
            agblib::mp2k::SongHeader header;
            const uint32_t old_group =
                agblib::mp2k::ReadSong(before.rom, before.info, song, header) ? header.voices : group.address;
            const auto [known, added] = changed.try_emplace(old_group, false);
            if (added)
            {
                known->second = comparison.ChangedFrom(group, old_group);
            }
            if (known->second)
            {
                songs.insert(song);
            }
        }
    }

    return {songs.begin(), songs.end()};
}

bool GbaVoicePlaysSteps(const GbaRom& rom, const agblib::mp2k::Voice& voice)
{
    agblib::mp2k::Wave wave;
    agblib::mp2k::ReadWave(rom.rom, voice.wave, wave);

    return rom.info.camelot_adpcm && voice.PsgChannel() == 0 && !voice.IsSplit() &&
           (voice.type & agblib::mp2k::kVoiceFixed) != 0 && static_cast<int32_t>(wave.size) < 0;
}

bool GbaVoiceSounds(const GbaRom& rom, const agblib::mp2k::Voice& voice)
{
    const int psg = voice.PsgChannel();
    if (psg != 0)
    {
        return psg <= 4;
    }

    agblib::mp2k::Wave wave;
    return agblib::sequence_export::mp2k::SynthOf(rom.rom, rom.info, voice) !=
               agblib::sequence_export::mp2k::Synth::kNone ||
           GbaVoicePlaysSteps(rom, voice) || (agblib::mp2k::ReadWave(rom.rom, voice.wave, wave) && wave.size > 0);
}

bool GbaTableHoldsVoices(const GbaRom& rom, uint32_t address, std::span<const int> selected)
{
    for (uint32_t program = 0; program < 128; program++)
    {
        const uint32_t entry = address + kVoiceBytes * program;
        agblib::mp2k::Voice voice;
        if (!rom.rom.Contains(entry, kVoiceBytes) || !agblib::mp2k::ReadVoice(rom.rom, entry, voice))
        {
            return false;
        }
        if (std::ranges::find(selected, static_cast<int>(program)) != selected.end())
        {
            continue;
        }

        bool empty = true;
        for (uint32_t i = 0; i < 12; i++)
        {
            empty = empty && rom.rom.U8(entry + i) == 0;
        }

        if (!empty && !voice.IsSplit() && !GbaVoiceSounds(rom, voice))
        {
            return false;
        }
    }

    return true;
}

std::vector<int> GbaListedPrograms(const GbaRom& rom, const GbaVoiceGroup& group)
{
    std::set<int> listed(group.programs.begin(), group.programs.end());
    if (GbaTableHoldsVoices(rom, group.address, group.programs))
    {
        for (int program = 0; program < 128; program++)
        {
            agblib::mp2k::Voice voice;
            if (agblib::mp2k::ReadVoice(rom.rom, group.address + kVoiceBytes * static_cast<uint32_t>(program), voice) &&
                (voice.IsSplit() || GbaVoiceSounds(rom, voice)))
            {
                listed.insert(program);
            }
        }
    }

    return {listed.begin(), listed.end()};
}

std::string GbaVoiceKind(const agblib::mp2k::Voice& voice)
{
    if ((voice.type & agblib::mp2k::kVoiceDrumKit) != 0)
    {
        return "Drum kit";
    }
    if ((voice.type & agblib::mp2k::kVoiceKeySplit) != 0)
    {
        return "Key split";
    }

    switch (voice.PsgChannel())
    {
    case 0:
        return (voice.type & agblib::mp2k::kVoiceFixed) != 0 ? "Fixed sample" : "Sample";
    case 1:
        return "Square 1";
    case 2:
        return "Square 2";
    case 3:
        return "Wave";
    case 4:
        return "Noise";
    default:
        return "Unknown";
    }
}

std::shared_ptr<const GbaRom> GbaAuditionRom(const GbaRom& rom, int song, int program, int key, int velocity)
{
    agblib::mp2k::SongHeader header;
    if (!agblib::mp2k::ReadSong(rom.rom, rom.info, song, header))
    {
        throw std::runtime_error("Can't read the header of " + GbaSongName(rom, song) + ".");
    }

    const auto byte = [](int value)
    {
        return static_cast<uint8_t>(std::clamp(value, 0, 127));
    };
    std::vector<uint8_t> bytes = PaddedRomCopy(rom.rom);
    const auto address = [&bytes]
    {
        return EndAddress(bytes);
    };

    // The header: one track, no blocks, the song's priority and reverb, its voice group, and the track after it.
    const uint32_t song_header = address();
    bytes.insert(bytes.end(), {1, 0, header.priority, header.reverb});
    xsflib::AppendLe32(bytes, header.voices);
    xsflib::AppendLe32(bytes, song_header + 12);

    // VOICE, VOL 127, PAN in the centre and TIE. Each tick, a memacc jumps to the release once the byte is 1, and GOTO
    // waits for the next tick otherwise. EOT releases the note, and FINE ends the track.
    const uint32_t track = address();
    const uint32_t wait = track + 9;
    const uint32_t release = track + 23;
    bytes.insert(bytes.end(),
                 {agblib::mp2k::kCmdVoice, byte(program), agblib::mp2k::kCmdVol, 127, agblib::mp2k::kCmdPan, 0x40,
                  agblib::mp2k::kCmdTie, byte(key), byte(velocity), agblib::mp2k::kCmdWait + 1,
                  agblib::mp2k::kCmdMemAcc, 6, static_cast<uint8_t>(kAuditionRelease), 1});
    xsflib::AppendLe32(bytes, release);
    bytes.push_back(agblib::mp2k::kCmdGoto);
    xsflib::AppendLe32(bytes, wait);
    bytes.insert(bytes.end(), {agblib::mp2k::kCmdEot, byte(key), agblib::mp2k::kCmdFine});

    // The song table's one entry names the header, with the song's music player.
    PadToWord(bytes);
    const uint32_t table = address();
    xsflib::AppendLe32(bytes, song_header);
    const uint32_t entry = rom.info.song_table + 8 * static_cast<uint32_t>(song);
    xsflib::AppendLe32(bytes, rom.rom.U32(entry + 4));

    auto audition = std::make_shared<GbaRom>();
    audition->rom.Assign(std::move(bytes));
    audition->info = rom.info;
    audition->info.song_table = table;
    audition->info.song_count = 1;

    // The game's code sets up some songs as it starts them: the reverb, the volume of the music player and the
    // DirectSound channels (agblib::mp2k::SongSetting and SongChannels). Song 0 of the audition takes the set-up of
    // song `song`.
    audition->info.song_settings.clear();
    for (agblib::mp2k::SongSetting setting : rom.info.song_settings)
    {
        if (setting.song == song)
        {
            setting.song = 0;
            audition->info.song_settings.push_back(setting);
        }
    }
    audition->info.song_channels.channels = agblib::mp2k::SongChannels(rom.rom, rom.info, song);
    audition->info.song_channels.songs = {0};

    return audition;
}

std::string GbaVoiceGroupName(uint32_t address)
{
    return "Voice group " + agblib::agb::Hex(address);
}

} // namespace hokuto::song
