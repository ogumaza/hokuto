// SPDX-License-Identifier: MIT

// The import and the export of the songs of GBA ROMs (format/archive.h).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/layout.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/text.h"

namespace hokuto::format
{

File ImportGbaRom(std::span<const uint8_t> rom)
{
    ntdlib::agb::Rom read;
    read.Assign(std::vector<uint8_t>(rom.begin(), rom.end()));
    ntdlib::mp2k::DriverInfo info;
    std::string error;
    if (!ntdlib::mp2k::DetectDriver(read, {}, info, error))
    {
        throw xsflib::FormatError(error.empty() ? "the ROM doesn't use MusicPlayer2000"
                                                : "the MusicPlayer2000 song table has no song that Hokuto can read");
    }

    ntdlib::mp2k::Layout layout = ntdlib::mp2k::Cut(read, info);
    File file;
    file.engine = Engine::kGba;
    file.sequences = ShareSequences(std::move(layout.sequences));
    file.native = NativeArchive{.engine = Engine::kGba,
                                .pieces = std::move(layout.pieces),
                                .fields = std::move(layout.fields),
                                .game_sizes = GameSizes{rom.size(), {}},
                                .gba_note_arguments = layout.note_arguments};

    return file;
}

bool IsGbaRom(std::span<const uint8_t> bytes)
{
    // A sound archive or a Hokuto file can hold the same byte there. Each starts with its signature.
    const auto starts = [bytes](std::string_view signature)
    {
        return bytes.size() >= signature.size() && std::equal(signature.begin(), signature.end(), bytes.begin());
    };

    return bytes.size() > 0xB2 && bytes[0xB2] == 0x96 && !starts("RSAR") && !starts("CSAR") && !starts("FSAR") &&
           !starts("SDAT") && !IsBinary(bytes) && !IsText(bytes);
}

std::optional<uint32_t> GbaSongSequence(const File& file, uint32_t song_table, int song)
{
    if (!file.native || file.native->engine != Engine::kGba || song < 0)
    {
        return std::nullopt;
    }

    // Each piece's place in the ROM.
    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    const std::vector<uint64_t> starts = NativePieceStarts(file);

    // The song's entry of the song table names the start of the run that contains the song's header. That run follows
    // the song's piece (ntdlib::mp2k::Cut).
    const uint64_t entry = uint64_t{song_table} - ntdlib::agb::kRomBase + 8 * static_cast<uint64_t>(song);
    for (const ntdlib::sar::Field& field : file.native->fields)
    {
        const ntdlib::sar::Place& at = field.at;
        const ntdlib::sar::Place& to = field.to;
        if (at.item || at.sequence || at.piece >= pieces.size() || starts[at.piece] + at.offset != entry || to.item ||
            to.sequence || to.offset != 0 || to.piece == 0 || to.piece > pieces.size())
        {
            continue;
        }

        if (const auto* piece = std::get_if<ntdlib::sar::SequencePiece>(&pieces[to.piece - 1]))
        {
            return piece->sequence;
        }
    }

    return std::nullopt;
}

std::vector<std::vector<uint32_t>> GbaTrackFields(const File& file, uint32_t song_table, std::size_t songs)
{
    std::vector<std::vector<uint32_t>> tracks(file.native && file.native->engine == Engine::kGba ? songs : 0);
    if (tracks.empty())
    {
        return tracks;
    }

    // The place of each piece in the ROM.
    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    const std::vector<uint64_t> starts = NativePieceStarts(file);

    // The run of each song's header, from the song's entry of the song table, and the fields of each run that name an
    // item of a sequence. The fields of a header follow the header's order.
    const uint64_t table = uint64_t{song_table} - ntdlib::agb::kRomBase;
    std::map<uint32_t, std::vector<uint32_t>> fields_in_runs;
    std::vector<std::optional<uint32_t>> runs(songs);
    for (uint32_t index = 0; index < file.native->fields.size(); index++)
    {
        const ntdlib::sar::Place& at = file.native->fields[index].at;
        const ntdlib::sar::Place& to = file.native->fields[index].to;
        if (at.item || at.sequence || at.piece >= pieces.size())
        {
            continue;
        }

        if (to.item)
        {
            fields_in_runs[at.piece].push_back(index);
            continue;
        }

        const uint64_t place = starts[at.piece] + at.offset;
        if (place >= table && (place - table) % 8 == 0 && (place - table) / 8 < songs && !to.sequence && to.offset == 0)
        {
            runs[(place - table) / 8] = to.piece;
        }
    }

    for (std::size_t song = 0; song < songs; song++)
    {
        if (runs[song])
        {
            const auto found = fields_in_runs.find(*runs[song]);
            if (found != fields_in_runs.end())
            {
                tracks[song] = found->second;
            }
        }
    }

    return tracks;
}

std::vector<uint32_t> MovedSongs(const File& file)
{
    std::vector<uint32_t> moved;
    if (const Driver* driver = DriverOfFile(file))
    {
        // A song that outgrows its place moves (format/drivers.h). A driver whose songs are back to back in lists with
        // rooms, such as Quintet's, moves none: a song's place keeps no size, and a longer song moves the songs after
        // it within the list's room. The join refuses a list that outgrows its room.
        if (driver->Facts().rooms)
        {
            return moved;
        }
        for (const ntdlib::sar::Piece& piece : *file.native->pieces)
        {
            const auto* song = std::get_if<ntdlib::sar::SequencePiece>(&piece);
            if (song && song->sequence < file.sequences.size() &&
                driver->ItemOffsets(*file.sequences[song->sequence], file.native->driver->revision).back() > song->room)
            {
                moved.push_back(song->sequence);
            }
        }

        return moved;
    }

    if (!file.native || file.native->engine != Engine::kGba)
    {
        return moved;
    }

    // A song's size doesn't depend on its address: MP2K's addresses take 4 bytes each. A song whose header gains tracks
    // moves too: the bytes after the header hold other data.
    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    const std::vector<uint32_t> tracks = ntdlib::mp2k::HeaderTracks(pieces, file.native->fields);
    uint64_t start = 0;
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&pieces[p]))
        {
            start += bytes->size();
        }
        else if (const auto* song = std::get_if<ntdlib::sar::SequencePiece>(&pieces[p]))
        {
            const auto* header = p + 1 < pieces.size() ? std::get_if<std::vector<uint8_t>>(&pieces[p + 1]) : nullptr;
            const bool grows = header != nullptr && !header->empty() && tracks[p] > (*header)[0];
            if (song->sequence < file.sequences.size() &&
                (grows || ntdlib::mp2k::EncodeSequence(*file.sequences[song->sequence],
                                                       static_cast<uint32_t>(ntdlib::agb::kRomBase + start),
                                                       file.native->gba_note_arguments)
                                  .bytes.size() > song->room))
            {
                moved.push_back(song->sequence);
            }

            start += song->room;
        }
    }

    return moved;
}

bool LacksRoom(const File& file)
{
    if (!file.native || (file.native->engine != Engine::kGba && !file.native->driver) || MovedSongs(file).empty())
    {
        return false;
    }

    if (file.native->driver && !DriverOf(file.native->engine)->Facts().filler_runs)
    {
        // Filler runs add no room for the songs of such a driver.
        return false;
    }

    // ntdlib's join refuses such songs without the filler runs. Its message names the 32 MiB.
    File without = file;
    without.native.Edit([](NativeArchive& native) { native.gba_filler = false; });
    try
    {
        ExportArchive(without);
    }
    catch (const std::invalid_argument& error)
    {
        if (std::string_view(error.what()).find("32 MiB") == std::string_view::npos)
        {
            throw;
        }

        return true;
    }

    return false;
}

namespace
{

// The pieces of `version` whose songs are among `sequences`, each with its sequence back in `kept` from `original`, a
// copy of `version`. A song's piece keeps its place among the pieces in every version. Throws std::invalid_argument
// when the earlier version has another song in the piece's place.
std::vector<std::size_t> RestoredSongPieces(const File& version, const File& original,
                                            std::span<const uint32_t> sequences, File& kept)
{
    const std::vector<ntdlib::sar::Piece>& pieces = *version.native->pieces;
    const std::vector<ntdlib::sar::Piece>& old_pieces = *original.native->pieces;
    const std::set<uint32_t> wanted(sequences.begin(), sequences.end());
    std::vector<std::size_t> restored;
    for (std::size_t p = 0; p < pieces.size(); ++p)
    {
        const auto* song = std::get_if<ntdlib::sar::SequencePiece>(&pieces[p]);
        if (!song || !wanted.contains(song->sequence))
        {
            continue;
        }

        const auto* old_song = std::get_if<ntdlib::sar::SequencePiece>(&old_pieces[p]);
        if (!old_song || old_song->sequence != song->sequence || song->sequence >= original.sequences.size() ||
            song->sequence >= kept.sequences.size())
        {
            throw std::invalid_argument("the earlier version of the ROM has another song in place of sequence " +
                                        std::to_string(song->sequence));
        }

        kept.sequences[song->sequence] = original.sequences[song->sequence];
        restored.push_back(p);
    }

    return restored;
}

// A song's entry of the song table names its piece and items. The fields that lead into the piece of a song
// that comes back take the earlier version's places. A song's piece keeps its place among the pieces in every
// version.
File WithDriverSongsOf(const File& version, const File& original, std::span<const uint32_t> sequences)
{
    const std::vector<ntdlib::sar::Piece>& pieces = *version.native->pieces;
    const std::vector<ntdlib::sar::Piece>& old_pieces = *original.native->pieces;
    if (pieces.size() != old_pieces.size() || version.native->fields.size() != original.native->fields.size())
    {
        throw std::invalid_argument("the earlier version of the ROM has different pieces");
    }

    File kept = version;
    std::set<uint32_t> wanted_pieces;
    for (const std::size_t p : RestoredSongPieces(version, original, sequences, kept))
    {
        wanted_pieces.insert(static_cast<uint32_t>(p));
    }

    const auto restore_fields = [&](NativeArchive& native)
    {
        for (std::size_t f = 0; f < native.fields.size(); ++f)
        {
            if (native.fields[f].to.item && wanted_pieces.contains(native.fields[f].to.piece))
            {
                native.fields[f] = original.native->fields[f];
            }
        }
    };
    kept.native.Edit(restore_fields);

    return kept;
}

// The fields in those headers come from the earlier version, where the version's first such field lay.
std::vector<ntdlib::sar::Field> KeptHeaderFields(const File& version, const File& original,
                                                 const std::map<uint32_t, std::size_t>& headers)
{
    const auto in_header = [&headers](const ntdlib::sar::Field& field)
    {
        const auto found = headers.find(field.at.piece);
        return found != headers.end() && field.at.offset < found->second;
    };
    std::vector<ntdlib::sar::Field> fields;
    std::set<uint32_t> placed;
    for (const ntdlib::sar::Field& field : version.native->fields)
    {
        if (!in_header(field))
        {
            fields.push_back(field);
        }
        else if (placed.insert(field.at.piece).second)
        {
            for (const ntdlib::sar::Field& old : original.native->fields)
            {
                if (in_header(old) && old.at.piece == field.at.piece)
                {
                    fields.push_back(old);
                }
            }
        }
    }

    return fields;
}

} // namespace

File WithSongsOf(const File& version, const File& original, std::span<const uint32_t> sequences)
{
    if (version.native && version.native->driver && original.native && original.native->driver)
    {
        return WithDriverSongsOf(version, original, sequences);
    }

    if (!version.native || version.native->engine != Engine::kGba || !original.native ||
        original.native->engine != Engine::kGba)
    {
        throw std::invalid_argument("only GBA songs can be restored from an earlier version");
    }

    const std::vector<ntdlib::sar::Piece>& pieces = *version.native->pieces;
    const std::vector<ntdlib::sar::Piece>& old_pieces = *original.native->pieces;
    if (pieces.size() != old_pieces.size())
    {
        throw std::invalid_argument("the earlier version of the ROM has different pieces");
    }

    // Each song's sequence, and the header at the start of the run after its piece. The header is 8 bytes long, plus 4
    // for each track. The run's other bytes keep their edits.
    File kept = version;
    std::vector<ntdlib::sar::Piece> kept_pieces = pieces;
    std::map<uint32_t, std::size_t> headers; // the bytes of each header to take, by its run
    for (const std::size_t p : RestoredSongPieces(version, original, sequences, kept))
    {
        const auto& song = std::get<ntdlib::sar::SequencePiece>(pieces[p]);
        auto* run = p + 1 < pieces.size() ? std::get_if<std::vector<uint8_t>>(&kept_pieces[p + 1]) : nullptr;
        const auto* old_run = p + 1 < pieces.size() ? std::get_if<std::vector<uint8_t>>(&old_pieces[p + 1]) : nullptr;
        if (!run || !old_run || run->empty() || old_run->empty() || run->size() != old_run->size())
        {
            throw std::invalid_argument("the earlier version of the ROM has another header after sequence " +
                                        std::to_string(song.sequence));
        }

        const std::size_t size =
            std::min(old_run->size(), 8 + 4 * static_cast<std::size_t>(std::max((*run)[0], (*old_run)[0])));
        std::copy_n(old_run->begin(), size, run->begin());
        headers[static_cast<uint32_t>(p + 1)] = size;
    }

    std::vector<ntdlib::sar::Field> fields = KeptHeaderFields(version, original, headers);

    const auto take_layout = [&](NativeArchive& native)
    {
        native.pieces = std::move(kept_pieces);
        native.fields = std::move(fields);
    };
    kept.native.Edit(take_layout);

    return kept;
}

} // namespace hokuto::format
