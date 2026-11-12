// SPDX-License-Identifier: MIT

#include "edit/gba_tables.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/ascii.h"

#include "edit/gba_voices.h"
#include "format/archive.h"

namespace hokuto::edit
{

namespace mp2k = ntdlib::mp2k;

namespace
{

// A song table's entry: the address of the song's header, then the numbers of its two music players. The song plays on
// the first. Games give the second the same number.
constexpr uint32_t kEntryBytes = 8;
constexpr uint32_t kEntryFirstPlayer = 4;
constexpr uint32_t kEntrySecondPlayer = 6;

mp2k::SongHeader HeaderOf(const song::GbaRom& rom, uint32_t song)
{
    mp2k::SongHeader header;
    if (song >= static_cast<uint32_t>(rom.info.song_count) ||
        !mp2k::ReadSong(rom.rom, rom.info, static_cast<int>(song), header))
    {
        throw std::invalid_argument("Can't read song " + std::to_string(song) + " of the ROM.");
    }

    return header;
}

// `file` with `byte` at `offset` of the song's header. The fields of a cut song state the place of the header in the
// runs of the file, where an export finds it, whichever place the song has in the ROM. Any other song's header stays at
// its address.
format::File WithHeaderByte(const format::File& file, const song::GbaRom& rom, uint32_t song,
                            const mp2k::SongHeader& header, uint32_t offset, uint8_t byte)
{
    const std::vector<std::vector<uint32_t>> fields =
        format::GbaTrackFields(file, rom.info.song_table, static_cast<std::size_t>(rom.info.song_count));
    if (song >= fields.size() || fields[song].empty())
    {
        const std::array<uint8_t, 1> bytes = {byte};
        return WithGbaBytes(file, header.address + offset, bytes);
    }

    format::File result = file;
    const auto write_header = [&](format::NativeArchive& edited)
    {
        const std::optional<RunPlace> place = GbaHeaderPlace(edited, fields[song].front());
        if (!place)
        {
            throw std::invalid_argument("Can't find the song header.");
        }

        const auto write = [&](std::vector<ntdlib::sar::Piece>& pieces)
        {
            std::get<std::vector<uint8_t>>(pieces[place->piece])[place->offset + offset] = byte;
        };
        edited.pieces.Edit(write);
    };
    result.native.Edit(write_header);

    return result;
}

} // namespace

std::optional<RunPlace> GbaHeaderPlace(const format::NativeArchive& native, uint32_t first_field)
{
    const ntdlib::sar::Place at = native.fields[first_field].at;
    const auto* run =
        at.piece < native.pieces->size() ? std::get_if<std::vector<uint8_t>>(&(*native.pieces)[at.piece]) : nullptr;
    if (run == nullptr || at.offset < kGbaHeaderSize || at.offset > run->size())
    {
        return std::nullopt;
    }

    return RunPlace{at.piece, at.offset - kGbaHeaderSize};
}

HeldValues GbaHeldValues()
{
    HeldValues held;
    held.bank_slots = 0;
    held.actor_player = false;
    held.pan = false;
    held.release_priority_fix = false;
    held.tracks = false;
    held.highest_volume = 0;
    held.highest_priority = 255;

    return held;
}

format::NativeSound GbaNativeSoundOf(const song::GbaRom& rom, uint32_t song)
{
    const mp2k::SongHeader header = HeaderOf(rom, song);
    const uint32_t entry = rom.info.song_table + kEntryBytes * song;
    if (!rom.rom.Contains(entry, kEntryBytes))
    {
        throw std::invalid_argument("Can't read song " + std::to_string(song) + " of the ROM.");
    }

    format::NativeSound values;
    values.name = song::GbaSongName(rom, static_cast<int>(song));
    values.kind = ntdlib::sar::SoundType::kSequence;
    values.player = ntdlib::sar::ItemId(ntdlib::sar::kPlayerItem, rom.rom.U16(entry + kEntryFirstPlayer));
    values.player_priority = header.priority;
    values.reverb = header.reverb;

    return values;
}

std::vector<std::string> GbaPlayerNames(const song::GbaRom& rom)
{
    std::vector<std::string> names;
    names.reserve(rom.info.players.size());
    for (std::size_t player = 0; player < rom.info.players.size(); player++)
    {
        const int tracks = rom.info.players[player].track_count;
        names.push_back(std::to_string(player) + " (" + std::to_string(tracks) +
                        (tracks == 1 ? " track)" : " tracks)"));
    }

    return names;
}

format::File ChangeGbaSong(const format::File& file, const song::GbaRom& rom, uint32_t song, const SoundChange& change)
{
    if (!file.native || file.native->engine != format::Engine::kGba)
    {
        throw std::invalid_argument("The file contains no GBA ROM.");
    }

    const mp2k::SongHeader header = HeaderOf(rom, song);
    switch (change.value)
    {
    case SoundValue::kPlayer:
        {
            const uint32_t player = ntdlib::sar::ItemNumber(change.to);
            if (ntdlib::sar::ItemType(change.to) != ntdlib::sar::kPlayerItem || player >= rom.info.players.size())
            {
                throw std::invalid_argument("The driver has no such music player.");
            }
            if (rom.info.players[player].track_count <= 0)
            {
                throw std::invalid_argument("Music player " + std::to_string(player) + " has no tracks.");
            }

            const uint32_t entry = rom.info.song_table + kEntryBytes * song;
            const uint32_t first = rom.rom.U16(entry + kEntryFirstPlayer);
            const uint32_t second = rom.rom.U16(entry + kEntrySecondPlayer);
            const auto low = static_cast<uint8_t>(player);
            const auto high = static_cast<uint8_t>(player >> 8);
            const std::array<uint8_t, 4> same = {low, high, low, high};
            const std::array<uint8_t, 2> alone = {low, high};
            return first == second ? WithGbaBytes(file, entry + kEntryFirstPlayer, same)
                                   : WithGbaBytes(file, entry + kEntryFirstPlayer, alone);
        }

    case SoundValue::kPlayerPriority:
    case SoundValue::kReverb:
        if (change.to > 255)
        {
            throw std::invalid_argument("The " + xsflib::AsciiLower(NameOf(change.value)) +
                                        " of a GBA song must be from 0 to 255.");
        }

        return WithHeaderByte(file, rom, song, header,
                              change.value == SoundValue::kReverb ? kGbaHeaderReverb : kGbaHeaderPriority,
                              static_cast<uint8_t>(change.to));

    default:
        break;
    }

    throw std::invalid_argument("GBA songs have no " + xsflib::AsciiLower(NameOf(change.value)) + " setting.");
}

} // namespace hokuto::edit
