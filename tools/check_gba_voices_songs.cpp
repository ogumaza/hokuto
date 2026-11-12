// SPDX-License-Identifier: MIT

// The output of the songs of hokuto_check_gba_voices, and the round of the songs' values.

#include "check_gba_voices.h"

namespace hokuto::tools::gba_voices
{

std::vector<int16_t> SongOutput(const hokuto::song::GbaRom& rom, int song)
{
    mp2k::ArchivePlayer player(rom.rom, rom.info);
    player.Start(song, mp2k::ArchivePlayerOptions{});
    const std::size_t samples = static_cast<std::size_t>(hokuto::song::GbaRate(rom)) * 3 * 2;
    std::vector<int16_t> out;
    while (out.size() < samples && player.Busy())
    {
        player.RunFrame(out);
    }

    return out;
}

namespace
{

using hokuto::edit::SoundChange;
using hokuto::edit::SoundValue;
namespace sar = ntdlib::sar;

// Prints the round's problem `what` with the game's name, and counts it.
void Fail(const Game& game, Totals& totals, const std::string& what)
{
    std::printf("%s: songs: %s\n", game.name.c_str(), what.c_str());
    totals.failed++;
}

// The values of song `song` of `rom`.
hokuto::format::NativeSound ValuesOf(const hokuto::song::GbaRom& rom, int song)
{
    return hokuto::edit::GbaNativeSoundOf(rom, static_cast<uint32_t>(song));
}

// The round's edits of the songs' headers.
struct SongEdits
{
    std::map<uint32_t, int> edited_headers; // each edited header's address, with the song that edits it
    File edited;                            // the file with the new values
    File back;                              // the file with the new values, then the old ones again
    File reverb_only;                       // the file with the new reverbs alone
    std::vector<std::size_t> allowed;       // the offsets in the ROM of the values' bytes
    std::map<int, uint32_t> new_players;    // the new music player of each song that the round edits
};

// Edits every `every`th song of `songs` that has a header of its own: a new music player, priority and reverb.
SongEdits EditSongHeaders(const Game& game, const std::vector<int>& songs, std::size_t every, Totals& totals)
{
    const hokuto::song::GbaRom& rom = *game.rom;
    std::map<uint32_t, int> edited_headers;
    File edited = game.file;
    File back = game.file;
    File reverb_only = game.file;
    std::vector<std::size_t> allowed;
    std::map<int, uint32_t> new_players;
    for (std::size_t i = 0; i < songs.size(); i += every)
    {
        mp2k::SongHeader header;
        if (!mp2k::ReadSong(rom.rom, rom.info, songs[i], header) || edited_headers.contains(header.address))
        {
            continue;
        }
        edited_headers.emplace(header.address, songs[i]);
        const hokuto::format::NativeSound values = ValuesOf(rom, songs[i]);
        const uint32_t player = sar::ItemNumber(values.player);
        uint32_t next = player;
        for (std::size_t step = 1; step < rom.info.players.size(); step++)
        {
            const auto other = static_cast<uint32_t>((player + step) % rom.info.players.size());
            if (rom.info.players[other].track_count > 0)
            {
                next = other;
                break;
            }
        }
        const uint32_t new_player = sar::ItemId(sar::kPlayerItem, next);
        const uint32_t level = (values.reverb & 0x7FU) == 64 ? 32 : 64;
        new_players.emplace(songs[i], new_player);

        const auto number = static_cast<uint32_t>(songs[i]);
        const auto change = [&](const File& file, SoundValue value, uint32_t to)
        {
            return hokuto::edit::ChangeGbaSong(file, rom, number, SoundChange{value, 0, to});
        };
        edited = change(edited, SoundValue::kPlayer, new_player);
        edited = change(edited, SoundValue::kPlayerPriority, (values.player_priority + 1U) & 0xFFU);
        edited = change(edited, SoundValue::kReverb, hokuto::edit::kGbaReverbOn | level);
        back = change(back, SoundValue::kPlayer, new_player);
        back = change(back, SoundValue::kPlayerPriority, (values.player_priority + 1U) & 0xFFU);
        back = change(back, SoundValue::kReverb, hokuto::edit::kGbaReverbOn | level);
        back = change(back, SoundValue::kPlayer, values.player);
        back = change(back, SoundValue::kPlayerPriority, values.player_priority);
        back = change(back, SoundValue::kReverb, values.reverb);
        reverb_only = change(reverb_only, SoundValue::kReverb, hokuto::edit::kGbaReverbOn | level);
        totals.songs_edited++;

        const std::size_t entry = rom.info.song_table + 8U * number - ntdlib::agb::kRomBase;
        for (std::size_t offset = 4; offset < 8; offset++)
        {
            allowed.push_back(entry + offset);
        }
        allowed.push_back(header.address - ntdlib::agb::kRomBase + 2);
        allowed.push_back(header.address - ntdlib::agb::kRomBase + 3);
    }

    return {std::move(edited_headers), std::move(edited),  std::move(back),
            std::move(reverb_only),    std::move(allowed), std::move(new_players)};
}

// Checks that only the values' bytes at the offsets of `allowed` differ in the export of `edited`, and that `back`
// reproduces the ROM. Returns the export, or nothing when its size differs.
std::optional<std::vector<uint8_t>> CheckBytesAndPutBack(const Game& game, const File& edited, const File& back,
                                                         const std::vector<std::size_t>& allowed, Totals& totals)
{
    // Only the values' bytes differ, and the old values give the ROM back.
    if (hokuto::format::ExportArchive(back) != game.bytes)
    {
        Fail(game, totals, "the old values don't give the ROM back");
    }
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    if (exported.size() != game.bytes.size())
    {
        Fail(game, totals, "the ROM's size changed");
        return std::nullopt;
    }
    for (std::size_t i = 0; i < exported.size(); i++)
    {
        if (exported[i] != game.bytes[i] && !std::ranges::binary_search(allowed, i))
        {
            Fail(game, totals,
                 "a byte differs at " + ntdlib::agb::Hex(static_cast<uint32_t>(i + ntdlib::agb::kRomBase)));
            break;
        }
    }

    return exported;
}

// Checks that each song of `songs` reads its values from `built`, the ROM with the edits: the new music player of
// `new_players`, and the new priority and reverb of a header of `edited_headers`.
void CheckReadValues(const Game& game, const hokuto::song::GbaRom& built, const std::vector<int>& songs,
                     const std::map<uint32_t, int>& edited_headers, const std::map<int, uint32_t>& new_players,
                     Totals& totals)
{
    const hokuto::song::GbaRom& rom = *game.rom;
    for (const int song : songs)
    {
        mp2k::SongHeader header;
        if (!mp2k::ReadSong(rom.rom, rom.info, song, header))
        {
            continue;
        }
        const hokuto::format::NativeSound before = ValuesOf(rom, song);
        const hokuto::format::NativeSound after = hokuto::edit::GbaNativeSoundOf(built, static_cast<uint32_t>(song));
        const bool edited_header = edited_headers.contains(header.address);
        const auto player = new_players.find(song);
        totals.numbers_read++;
        if (after.player != (player != new_players.end() ? player->second : before.player))
        {
            Fail(game, totals, "song " + std::to_string(song) + " reads another music player");
        }
        if (edited_header != (after.player_priority != before.player_priority) ||
            edited_header != (after.reverb != before.reverb))
        {
            Fail(game, totals, "song " + std::to_string(song) + " reads other values of its header");
        }
    }
}

// Counts the songs of up to 8 edited headers whose output differs in `built`, the ROM with the new reverbs alone.
void CheckReverbHeard(const hokuto::song::GbaRom& rom, const hokuto::song::GbaRom& built,
                      const std::map<uint32_t, int>& edited_headers, Totals& totals)
{
    std::size_t played = 0;
    for (const auto& [address, song] : edited_headers)
    {
        if (played++ >= 8)
        {
            break;
        }
        (SongOutput(rom, song) != SongOutput(built, song) ? totals.reverbs_heard : totals.reverbs_unheard)++;
    }
}

} // namespace

void SongValues(const Game& game, Totals& totals)
{
    // The numbers that the round edits, each with another header.
    const hokuto::song::GbaRom& rom = *game.rom;
    const std::vector<int> songs = hokuto::song::GbaSongs(rom);
    const std::size_t every = std::max<std::size_t>(songs.size() / 64, 1);
    SongEdits edits = EditSongHeaders(game, songs, every, totals);
    if (edits.edited_headers.empty())
    {
        return;
    }
    std::ranges::sort(edits.allowed);

    // Only the values' bytes differ, and the old values give the ROM back.
    const std::optional<std::vector<uint8_t>> exported =
        CheckBytesAndPutBack(game, edits.edited, edits.back, edits.allowed, totals);
    if (!exported)
    {
        return;
    }

    // Each number reads what it should.
    const std::shared_ptr<const hokuto::song::GbaRom> built = hokuto::song::LoadGbaRom(*exported);
    CheckReadValues(game, *built, songs, edits.edited_headers, edits.new_players, totals);

    if (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(edits.edited))) != *exported ||
        hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(edits.edited))) != *exported)
    {
        Fail(game, totals, "the file doesn't come back from its forms");
    }

    // A reverb is heard in a song with samples. The music player and the priority stay as they were.
    const std::shared_ptr<const hokuto::song::GbaRom> reverb_rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(edits.reverb_only));
    CheckReverbHeard(rom, *reverb_rom, edits.edited_headers, totals);
}

} // namespace hokuto::tools::gba_voices
