// SPDX-License-Identifier: MIT

// Checks ntdlib's cutting and joining of sound archives (ntdlib/sar/layout.h) on archives of the 3DS, the Wii U and the
// Switch, and of the Wii (ntdlib/rsar/layout.h). For an archive of the Wii, it checks two things:
//
// - Hokuto's import and export give the archive back byte for byte. The import takes the game's build of NW4R SND, as
//   the program's import does.
// - After every sequence grows as below, ntdlib's reader finds each file where it moved. A file that holds no sequence
//   keeps its bytes, and so does each file's wave data. A sequence's file holds the grown commands as the writer writes
//   them. Each group holds its files at multiples of 32 bytes, one after another, then their wave data. This is the
//   layout of NW4R's tools. The cut of the grown archive gives the grown commands, the same copies, and the fields at
//   the same places.
//
// For each other archive, it checks sixteen things:
//
// - Cut and Join give the archive back byte for byte.
// - ntdlib's StringBlock writes the archive's STRG block again from its names and their items, byte for byte
//   (ntdlib/sar/string_block.h). FindItem finds each name's item. The reader gives each sound, bank, wave archive,
//   player and group the same name.
// - ntdlib's InfoBlock writes the archive's INFO block again from its entries, byte for byte
//   (ntdlib/sar/info_block.h). WithTables gives the archive back with its STRG and INFO blocks (ntdlib/sar/tables.h).
//   Then each entry of the INFO block grows by 4 bytes, and WithTables puts the block in the archive. The reader reads
//   the same items, names and files from that archive as from the original, by the default rules and by the strict ones
//   where they read the original. Its cut gives the same sequences. An archive cut short counts apart.
// - ntdlib's WithNewFile adds a copy of the file of the first sequence sound whose file the reader reads. Then
//   WithNewSequenceSound adds a copy of that sound. The copy plays the new file. The reader reads every other item as
//   before, the new file with the copied bytes, and the new sound with the model's values. Each entry's string index
//   names its item, and the string table keeps the names in the order of the tables. FindItem finds each name. The cut
//   gives the same sequences, and the model's sequence again for the new file when the model's file is a piece.
// - ntdlib's WithNewFile and WithNewWaveArchive copy each wave archive of the first bank whose file and wave archives
//   the reader reads. WithWaveArchives points a copy of the bank's file at the new wave archives. WithNewBank adds that
//   file as a new bank. The reader reads every other item as before, the new wave archives with the old files' bytes,
//   and the new bank with the new file. The new bank's waves lie in the new wave archives at the model's indices. Each
//   entry's string index names its item. FindItem finds each name. The cut gives the same sequences.
// - ntdlib's WithBankLists gives each sequence sound with fewer than four banks a list one slot longer, with an empty
//   slot at its end. Then it gives each sequence sound with banks a list one bank shorter. The reader reads each
//   sound's banks from the new lists, and every other value, item and file as before. The cut gives the same
//   sequences. The old lists then give the archive back byte for byte.
// - ntdlib's WithNewFile adds a copy of the first wave sound file that the reader reads. WithNewWaveSounds then adds a
//   copy of each wave sound that plays the file, at its index, and a copy of the first sequence sound as a wave sound
//   at index 0. The reader reads every other item as before, and the new sounds with their models' values. Each entry's
//   string index names its item. FindItem finds each name. The cut gives the same sequences.
// - ntdlib's WithNewExternalFile adds each file that the file table names outside the archive again, at its path. The
//   new entry has to hold the bytes of the file's entry. WithNewStreamSound then adds a copy of each stream sound, with
//   the sound's file and details. The new entry has to hold the sound's entry, the string index aside. A copy leaves
//   out an extension and a prefetch file that the sound's details name, and the values of the field of options in a
//   3DS archive of CSAR 2.3.0.0. Such a copy counts apart. The reader has to read every other item as before, and the
//   copy with the sound's values. A copy of the archive's first sound of another kind, with the same file and details,
//   counts apart where its details hold the bytes of the sound's details. ntdlib lays out no stream sound of a 3DS
//   archive of CSAR 2.1.0.0 or 2.3.1.0. A copy there has to be refused.
// - After every sequence grows, ntdlib's reader finds each file and each sound's start where they moved. Sequence s
//   gains (s % 5) * 9 waits after its first item: 0 to 72 bytes. A sequence with a jump past the end of its bytes
//   doesn't grow. A file that holds no sequence keeps its bytes. A sequence's file holds the grown commands as the
//   writer writes them, and each sound starts at the item it started at. Where the strict rules read the archive, they
//   read the grown one as the default rules do.
// - The cut of the grown archive gives the grown commands, the same copies, and the fields at the same places. A
//   sequence with bytes that nothing reaches can decode otherwise once the code around them moves. Such sequences count
//   apart.
// - Where the strict rules read the archive, its tables' values written back give it back (ntdlib's WriteTableValues).
//   A new volume for every sound and new flags for every sequence sound's tracks then read back, and nothing else
//   moves.
// - Where the strict rules read the archive, ntdlib's TableValuePlaces finds the places of the sounds' optional values
//   in the archive's first piece as in the whole archive. A new pan curve, player priority and channel priority for
//   every sound then read back where the sound has a place for them, and nowhere else.
// - Where the strict rules read the archive and it isn't cut short, every option of a pan, of the priorities and of a
//   channel priority leaves the sounds' entries (ntdlib's sar_tools.h). The reader reads the values that it gives
//   without the options, and every other value, item and file as before. The cut gives the same sequences. ntdlib's
//   WithValuePlaces then gives the entries the options back, and the archive's values written again give the archive
//   back byte for byte. The archive's first piece gives the fields of the sounds' starts as the whole archive does.
// - After every file that the cut makes a run of its own grows, ntdlib's reader finds each file where it moved. A run
//   that padding follows holds a file. A file of n bytes gains n % 4 * 24 + 8 bytes. Each copy of the file in a group
//   then gains the same bytes. Each sound starts where it started. The cut of the archive with longer files gives the
//   same pieces, the longer files and the fields at the same places. Then ReplaceFile gives the first, the middle and
//   the last of the files in runs 40 more bytes, in every copy, and the reader finds them.
// - GroupWith gives the file of each group in the archive back byte for byte (ntdlib/sar/groups.h). The tool finds the
//   group files beside the archive as the program does (song::OpenArchive). The cut takes them, and leaves the
//   archive's pieces, sequences and fields as without them. A sequence file that only group files hold becomes a
//   sequence after the archive's, and only the fields of its sounds' starts name items of it. Each group file joins
//   back byte for byte. The import finds a group
//   beside the archive for it (format::ImportArchive): the group whose copies it holds, or on the 3DS the group whose
//   name the file's name gives. GroupWith gives it back byte for byte. Then each file whose copy a group file holds
//   takes 40 more bytes through ReplaceFile. Every copy has to hold them, and the reader reads them from the archive
//   and its group files. A copy of a sequence file that is a piece of the archive's sequence counts apart: ReplaceFile
//   refuses it, and the edits of the sequence reach it. Each SARC archive beside the archive comes back byte for byte
//   with its files (ntdlib's WithSarcFile). After each ReplaceFile, the group files that changed go out as an export
//   writes them (format::GroupFileOutputs). A SARC archive that holds the game's copy of a group file has to hold the
//   new group file in the copy's place, and keep its other files. Another group file goes out by itself, under the
//   name that the tool found it by.
// - ntdlib's WithInstruments gives each bank with instruments whose file the reader reads copies of instruments
//   (ntdlib/sar/bank_edit.h). Without copies, the bank comes back byte for byte. Each instrument of the archive's next
//   such bank goes in at the bank's first free program. Then each instrument of the bank itself does. The reader
//   reads the bank's instruments as before, and each copy as its source, with waves that name the same wave archive
//   and wave. A copy of an instrument of the same bank holds the instrument's bytes. A bank of the Wii U or the Switch
//   comes back byte for byte from the other byte order (ntdlib/sar/byte_order.h).
//
// A group that holds a copy of a sequence file changes with it. Its bytes aren't compared. The cut of the grown archive
// checks its copies. The tool reads each archive's kind from its header. It prints a line for each problem and a
// summary, and returns 1 for any problem.
//
// usage: hokuto_check_layout [--instruments] <archive>...
// --instruments checks only the copies of instruments. An archive of the Wii takes its full check with it too.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/layout.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/sar/bank_edit.h"
#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/sarc.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/testing/sar_tools.h"
#include "seqlib/file_name.h"
#include "xsflib/binary.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "format/archive.h"
#include "song/archive.h"

namespace
{

namespace commands = ntdlib::commands;
namespace nwsnd = ntdlib::nwsnd;
namespace sar = ntdlib::sar;

// What the check counted, for the summary.
struct Totals
{
    int archives = 0;
    int problems = 0;
    int sequences = 0;
    int pieces = 0;
    int fields = 0;
    int files = 0;
    int groups = 0;
    int starts = 0;
    int strict = 0;
    int same = 0;
    int unreached = 0;
    int left = 0;
    int values = 0;
    int runs = 0;
    int run_files = 0;
    int run_groups = 0;
    int replaced = 0;
    int name_blocks = 0;
    int names = 0;
    int nameless = 0;
    int info_blocks = 0;
    int entries = 0;
    int moved = 0;
    int new_lead = 0;
    int cut_short = 0;
    int new_sounds = 0;
    int new_pieces = 0;
    int no_model = 0;
    int new_banks = 0;
    int new_wave_archives = 0;
    int listed = 0;
    int no_bank = 0;
    int bank_lists = 0;
    int longer_lists = 0;
    int shorter_lists = 0;
    int new_wave_sounds = 0;
    int wave_sound_archives = 0;
    int from_sequences = 0;
    int no_wave_sound = 0;
    int external_files = 0;
    int new_streams = 0;
    int stream_archives = 0;
    int stream_extras = 0;  // copies of stream sounds whose details name an extension or a prefetch file
    int stream_refused = 0; // copies in 3DS archives of versions whose stream sounds ntdlib doesn't lay out
    std::map<std::string, std::pair<int, int>> stream_layouts; // the copies of other kinds with the stream sound's
                                                               // details and all of them, by the archive's version
    int place_archives = 0;
    int place_sounds = 0;
    int pan_places = 0;
    int priority_places = 0;
    int channel_places = 0;
    int option_archives = 0;
    int pan_options = 0;
    int priority_options = 0;
    int channel_options = 0;
    std::map<std::string, std::pair<int, int>> kinds; // the files in runs and all files, by the magic at their starts
    int groups_inside = 0;                            // groups' files in the archives that GroupWith gave back
    int group_archives = 0;                           // archives with group files beside them
    int group_files = 0;                              // group files beside the archives
    int group_replaced = 0;                           // files in the group files that took 40 more bytes
    int group_sequences = 0;                          // copies of sequence files in the group files
    int group_only_sequences = 0;                     // sequences of the sequence files that only the group files hold
    int group_only_starts = 0;                        // the fields of their sounds' starts
    int group_copies = 0;   // the copies that took the bytes, in the archives and the group files
    int sarcs = 0;          // SARC archives beside the archives
    int sarc_writes = 0;    // SARC archives that took new group files after ReplaceFile
    int managed_writes = 0; // new group files that went into a userManagementFiles folder after ReplaceFile
    int instrument_banks = 0;
    int unread_banks = 0;
    int empty_banks = 0;
    int loose_banks = 0;
    int copies = 0;
    int gap_copies = 0;
    int new_waves = 0;
    int same_runs = 0;
    int first_wave_runs = 0;
    int swapped_banks = 0;
    int wii_archives = 0;
    int wii_sequences = 0;
    int wii_pieces = 0;
    int wii_fields = 0;
    int wii_files = 0; // sequence files read back after the growth
    int wii_left = 0;  // sequence files that stay in the bytes
    int64_t wii_bytes = 0;
};

// The grammar of the sequences of an archive of `flavor`.
nwsnd::Grammar GrammarFor(sar::Flavor flavor)
{
    switch (flavor)
    {
    case sar::Flavor::kCtr:
        return nwsnd::GrammarOf<nwsnd::CtrPolicy>();
    case sar::Flavor::kCafe:
        return nwsnd::GrammarOf<nwsnd::CafePolicy>();
    case sar::Flavor::kNx:
        break;
    }

    return nwsnd::GrammarOf<nwsnd::NxPolicy>();
}

// The file of `sequence` as the writer writes it.
std::vector<uint8_t> FileOf(const commands::Sequence& sequence, uint32_t version, sar::Flavor flavor)
{
    const nwsnd::Encoded encoded = nwsnd::EncodeSequence(sequence, GrammarFor(flavor));
    sar::SequenceFile file;
    file.flavor = flavor;
    file.version = version;
    file.data = encoded.bytes;
    for (const commands::Label& label : sequence.labels)
    {
        file.labels.push_back({label.name, encoded.offsets[label.item]});
    }

    return file.Write();
}

// The sequence of each sequence piece, in order.
std::vector<uint32_t> SequencesOfPieces(const sar::Layout& layout)
{
    std::vector<uint32_t> sequences;
    for (const sar::Piece& piece : layout.pieces)
    {
        if (const auto* file = std::get_if<sar::SequencePiece>(&piece))
        {
            sequences.push_back(file->sequence);
        }
    }

    return sequences;
}

// Whether the fields of `a` and `b` lie at the same places, with items compared by their pieces only.
bool SamePlaces(const sar::Layout& a, const sar::Layout& b)
{
    if (a.fields.size() != b.fields.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.fields.size(); ++i)
    {
        sar::Field x = a.fields[i];
        sar::Field y = b.fields[i];
        for (sar::Place* place : {&x.from, &x.to, &y.from, &y.to})
        {
            place->offset = place->item ? 0 : place->offset;
        }
        if (!(x == y))
        {
            return false;
        }
    }

    return true;
}

// `layout` with waits after the first item of each sequence, and the labels, targets and places moved. `added` gets the
// number of waits for each sequence.
sar::Layout Grow(const sar::Layout& layout, std::vector<uint32_t>& added)
{
    sar::Layout grown = layout;
    added.assign(grown.sequences.size(), 0);
    for (std::size_t s = 0; s < grown.sequences.size(); ++s)
    {
        commands::Sequence& sequence = grown.sequences[s];
        auto outside = [](const commands::Command& item)
        {
            return item.target_outside;
        };
        if (sequence.items.empty() || std::ranges::any_of(sequence.items, outside))
        {
            continue;
        }

        commands::Command wait;
        wait.kind = commands::Kind::kWait;
        wait.value.value = commands::kOne;
        added[s] = static_cast<uint32_t>(s % 5 * 9);
        sequence.items.insert(sequence.items.begin() + 1, added[s], wait);
        for (commands::Label& label : sequence.labels)
        {
            label.item += label.item >= 1 ? added[s] : 0;
        }
        for (commands::Command& item : sequence.items)
        {
            if (commands::HasTarget(item.kind) && !item.target_outside && item.target >= 1)
            {
                item.target += added[s];
            }
        }
    }

    for (sar::Field& field : grown.fields)
    {
        for (sar::Place* place : {&field.from, &field.to})
        {
            if (place->item && place->offset >= 1)
            {
                const uint32_t sequence =
                    place->sequence ? place->piece : std::get<sar::SequencePiece>(grown.pieces[place->piece]).sequence;
                place->offset += added[sequence];
            }
        }
    }

    return grown;
}

// Where each of the original's pieces starts, for the sequence piece at a file's start.
std::map<std::size_t, std::size_t> PieceStarts(const sar::Layout& layout)
{
    std::map<std::size_t, std::size_t> starts;
    std::size_t position = 0;
    for (std::size_t i = 0; i < layout.pieces.size(); ++i)
    {
        starts[position] = i;
        const sar::Piece& piece = layout.pieces[i];
        if (const auto* run = std::get_if<std::vector<uint8_t>>(&piece))
        {
            position += run->size();
        }
        else if (const auto* file = std::get_if<sar::SequencePiece>(&piece))
        {
            position += FileOf(layout.sequences[file->sequence], file->version, layout.flavor).size();
        }
        else
        {
            const uint32_t alignment = std::get<sar::Padding>(piece).alignment;
            position = (position + alignment - 1) / alignment * alignment;
        }
    }

    return starts;
}

// Prints problem `what` of the archive at `path`, and counts it.
void Problem(const std::filesystem::path& path, const std::string& what, Totals& totals)
{
    std::printf("%s: %s\n", xsflib::Utf8(path.filename()).c_str(), what.c_str());
    ++totals.problems;
}

// The STRG block of `archive`, from the header's table of blocks. Nothing for an archive without one.
std::optional<std::span<const uint8_t>> StringBlockOf(std::span<const uint8_t> archive, sar::Flavor flavor)
{
    const xsflib::Reader r(archive, flavor == sar::Flavor::kCafe);
    for (uint16_t i = 0; i < r.U16(0x10); ++i)
    {
        const std::size_t entry = 0x14 + std::size_t{i} * 12;
        if (r.U16(entry) == 0x2000 && r.U32(entry + 4) != 0xFFFFFFFF)
        {
            return archive.subspan(r.U32(entry + 4), r.U32(entry + 8));
        }
    }

    return std::nullopt;
}

// The name that the reader gives item `item_id` of `archive`, for the types whose lists it reads. Nothing for another
// type.
std::optional<std::string> ReaderNameOf(const sar::SoundArchive& archive, uint32_t item_id)
{
    const uint32_t index = sar::ItemNumber(item_id);
    auto name_in = [index](const auto& list) -> std::optional<std::string>
    {
        return index < list.size() ? std::optional(list[index].name) : std::nullopt;
    };
    switch (sar::ItemType(item_id))
    {
    case sar::kSoundItem:
        return name_in(archive.Sounds());
    case sar::kBankItem:
        return name_in(archive.Banks());
    case sar::kWaveArchiveItem:
        return name_in(archive.WaveArchives());
    case sar::kPlayerItem:
        return name_in(archive.Players());
    case sar::kGroupItem:
        return name_in(archive.Groups());
    default:
        return std::nullopt;
    }
}

// Checks that ntdlib's StringBlock writes the STRG block of `original` again from its names, byte for byte, that
// FindItem finds each name's item, and that the reader gives the items the same names.
void CheckNames(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Options& options,
                Totals& totals)
{
    const std::optional<std::span<const uint8_t>> block = StringBlockOf(original, options.flavor);
    if (!block)
    {
        ++totals.nameless;
        return;
    }

    const sar::StringBlock names = sar::StringBlock::Read(*block, options.flavor);
    if (!std::ranges::equal(names.Write(), *block))
    {
        Problem(path, "the STRG block written again differs", totals);
    }
    const sar::SoundArchive archive = sar::SoundArchive::Load(original, options);
    for (const sar::ItemName& name : names.names)
    {
        if (sar::FindItem(*block, options.flavor, name.name) != name.item_id)
        {
            Problem(path, "FindItem doesn't find " + name.name, totals);
        }
        const std::optional<std::string> read = ReaderNameOf(archive, name.item_id);
        if (read && *read != name.name)
        {
            Problem(path, "the reader names " + name.name + " " + *read, totals);
        }
    }

    ++totals.name_blocks;
    totals.names += static_cast<int>(names.names.size());
}

// Checks the files and the sounds' starts of `grown`, the bytes of `layout` grown, with the reader.
void CheckTables(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                 const std::vector<uint8_t>& joined, const sar::Layout& layout, const sar::Layout& grown,
                 const std::vector<uint32_t>& added, const sar::Options& options, Totals& totals)
{
    const sar::SoundArchive before = sar::SoundArchive::Load(original, options);
    const sar::SoundArchive after = sar::SoundArchive::Load(joined, options);
    if (after.GroupErrors().size() != before.GroupErrors().size() || after.Truncated() != before.Truncated() ||
        after.FileCount() != before.FileCount())
    {
        Problem(path, "the grown archive's tables read otherwise", totals);
    }

    // Each file: a sequence's file as the writer writes the grown commands, or the same bytes.
    const std::map<std::size_t, std::size_t> starts = PieceStarts(layout);
    auto sequence_at = [&](std::optional<std::size_t> offset) -> const sar::SequencePiece*
    {
        const auto found = offset ? starts.find(*offset) : starts.end();
        return found == starts.end() ? nullptr : std::get_if<sar::SequencePiece>(&layout.pieces[found->second]);
    };

    for (uint32_t id = 0; id < before.FileCount(); ++id)
    {
        const std::span<const uint8_t> data = before.FileData(id);
        const std::optional<std::size_t> offset = before.FileOffset(id);
        if (data.empty() || !offset)
        {
            continue;
        }

        // A file with a sequence piece inside it is a group that holds a copy.
        auto copy = [&](const auto& start)
        {
            return std::holds_alternative<sar::SequencePiece>(layout.pieces[start.second]);
        };
        const auto first = starts.upper_bound(*offset);
        const auto last = starts.lower_bound(*offset + data.size());
        if (std::any_of(first, last, copy))
        {
            ++totals.groups;
            continue;
        }

        std::vector<uint8_t> expected(data.begin(), data.end());
        if (const sar::SequencePiece* piece = sequence_at(offset))
        {
            expected = FileOf(grown.sequences[piece->sequence], piece->version, grown.flavor);
        }

        const std::span<const uint8_t> now = after.FileData(id);
        ++totals.files;
        if (!std::ranges::equal(now, expected))
        {
            Problem(path, "file " + std::to_string(id) + " differs", totals);
        }
    }

    // The sequence files that the cut leaves in the bytes. A change that breaks the cut can leave every one there.
    std::set<std::size_t> left;
    for (const sar::SoundInfo& sound : before.Sounds())
    {
        const std::optional<std::size_t> offset = before.FileOffset(sound.file_id);
        if (sound.type == sar::SoundType::kSequence && offset && !before.FileData(sound.file_id).empty() &&
            !sequence_at(offset))
        {
            left.insert(*offset);
        }
    }

    totals.left += static_cast<int>(left.size());
    if (!left.empty())
    {
        std::printf("%s: %zu sequence files stay in the bytes\n", xsflib::Utf8(path.filename()).c_str(), left.size());
    }

    // Each sequence sound starts at its item.
    for (std::size_t i = 0; i < before.Sounds().size(); ++i)
    {
        const sar::SoundInfo& sound = before.Sounds()[i];
        const sar::SequencePiece* piece =
            sound.type == sar::SoundType::kSequence ? sequence_at(before.FileOffset(sound.file_id)) : nullptr;
        if (!piece)
        {
            continue;
        }

        const nwsnd::Grammar grammar = GrammarFor(layout.flavor);
        const std::vector<uint32_t> old_offsets =
            nwsnd::EncodeSequence(layout.sequences[piece->sequence], grammar).offsets;
        const std::vector<uint32_t> new_offsets =
            nwsnd::EncodeSequence(grown.sequences[piece->sequence], grammar).offsets;
        const auto item = std::ranges::find(old_offsets, sound.start_offset);
        ++totals.starts;
        if (item == old_offsets.end())
        {
            Problem(path, "sound " + std::to_string(i) + " starts at no item", totals);
            continue;
        }

        const auto index = static_cast<std::size_t>(item - old_offsets.begin());
        const std::size_t moved = index >= 1 ? index + added[piece->sequence] : index;
        if (after.Sounds()[i].start_offset != new_offsets[moved])
        {
            Problem(path,
                    "sound " + std::to_string(i) + " starts at " + std::to_string(after.Sounds()[i].start_offset) +
                        ", not " + std::to_string(new_offsets[moved]),
                    totals);
        }
    }

    // The strict rules, where they read the original.
    sar::Options strict = options;
    strict.rules = sar::Rules::kStopAtFirstProblem;
    try
    {
        sar::SoundArchive::Load(original, strict);
    }
    catch (const std::exception&)
    {
        return;
    }

    const sar::SoundArchive strict_after = sar::SoundArchive::Load(joined, strict);
    bool same = strict_after.Sounds().size() == after.Sounds().size();
    for (std::size_t i = 0; same && i < after.Sounds().size(); ++i)
    {
        same = strict_after.Sounds()[i].start_offset == after.Sounds()[i].start_offset;
    }
    for (uint32_t id = 0; same && id < after.FileCount(); ++id)
    {
        same = std::ranges::equal(strict_after.FileData(id), after.FileData(id));
    }

    ++totals.strict;
    if (!same)
    {
        Problem(path, "the strict rules read the grown archive otherwise", totals);
    }
}

// Whether `a` and `b` hold the same values, but for the volume and the tracks' flags.
bool SameOtherwise(const sar::SoundInfo& a, const sar::SoundInfo& b)
{
    return a.name == b.name && a.file_id == b.file_id && a.type == b.type && a.player_id == b.player_id &&
           a.pan_mode == b.pan_mode && a.pan_curve == b.pan_curve && a.player_priority == b.player_priority &&
           a.actor_player_id == b.actor_player_id && a.banks == b.banks && a.start_offset == b.start_offset &&
           a.channel_priority == b.channel_priority && a.release_priority_fix == b.release_priority_fix &&
           a.wave_sound_index == b.wave_sound_index && a.stream == b.stream;
}

// The bytes that a file of `size` bytes gains: 8 to 80.
std::vector<uint8_t> Gain(std::size_t size)
{
    return std::vector<uint8_t>(size % 4 * 24 + 8, static_cast<uint8_t>(0xA0 + size % 16));
}

// The magic at the start of `file`, or "other" when it isn't four letters.
std::string KindOf(std::span<const uint8_t> file)
{
    const auto letter = [](uint8_t byte)
    {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
    };
    if (file.size() < 4 || !std::all_of(file.begin(), file.begin() + 4, letter))
    {
        return "other";
    }

    return std::string(file.begin(), file.begin() + 4);
}

// Checks that the group files in `changed` go out as an export writes them beside `archive`, an archive of `flavor` at
// `path` (format::GroupFileOutputs and song::PlaceGroupFiles). `sarcs` are the SARC archives beside the archive, and
// `sarc_of` gives the one that holds the game's copy of each group file in `group_files`, if any. That SARC archive has
// to hold the new group file in the copy's place, and keep its other files and their names. Another group file goes out
// by itself. One that the tool found by its group's label in `labels` goes out under that name
// (ntdlib::sar::SoundArchive::GroupFileGroups). When the archive's folder holds the game's file of that name only in
// its userManagementFiles folder, the new file goes there.
void CheckSarcs(const std::filesystem::path& path, const std::vector<hokuto::format::ChangedGroupFile>& changed,
                std::span<const uint8_t> archive, sar::Flavor flavor,
                const std::vector<hokuto::format::FileBeside>& sarcs,
                const std::vector<std::optional<std::size_t>>& sarc_of,
                const std::vector<std::vector<uint8_t>>& group_files, const std::vector<std::string>& labels,
                Totals& totals)
{
    const std::array<std::filesystem::path, 1> folder = {path.parent_path()};
    const std::vector<hokuto::format::FileBeside> outputs =
        hokuto::song::PlaceGroupFiles(hokuto::format::GroupFileOutputs(changed, archive, flavor, sarcs), folder);
    for (const hokuto::format::ChangedGroupFile& group_file : changed)
    {
        const std::string name = "group file " + std::to_string(group_file.index);
        const std::optional<std::size_t> s = sarc_of[group_file.index];
        const auto by_name = [&](const hokuto::format::FileBeside& output)
        {
            return s && output.first == sarcs[*s].first;
        };
        if (!s && std::ranges::none_of(outputs, [&](const auto& output) { return output.second == group_file.bytes; }))
        {
            Problem(path, "the export leaves " + name + " out", totals);
        }
        const std::string& label = labels.at(group_file.index);
        std::string file_name = sar::GroupFileName(seqlib::SafeFileName(label), flavor);
        const std::filesystem::path file = xsflib::PathFromUtf8(file_name);
        std::error_code error;
        const bool managed = !std::filesystem::exists(folder[0] / file, error) &&
                             std::filesystem::exists(folder[0] / hokuto::song::kUserManagementFolder / file, error);
        if (managed)
        {
            file_name = std::string(hokuto::song::kUserManagementFolder) + "/" + file_name;
        }
        const auto named = [&](const hokuto::format::FileBeside& output)
        {
            return output.first == file_name && output.second == group_file.bytes;
        };
        if (!s && !label.empty() && std::ranges::none_of(outputs, named))
        {
            Problem(path, "the export doesn't write " + name + " as " + file_name, totals);
        }
        else if (!s && !label.empty() && managed)
        {
            ++totals.managed_writes;
        }
        if (s && std::ranges::none_of(outputs, by_name))
        {
            Problem(path, "the export leaves " + name + " out of " + sarcs[*s].first, totals);
        }
    }

    // Each SARC archive that the export writes: the copies of the changed group files take their new bytes.
    for (const hokuto::format::FileBeside& output : outputs)
    {
        const auto sarc = std::ranges::find_if(sarcs, [&](const auto& each) { return each.first == output.first; });
        if (sarc == sarcs.end())
        {
            continue;
        }

        const std::vector<sar::SarcFile> was = sar::SarcFiles(sarc->second);
        const std::vector<sar::SarcFile> now = sar::SarcFiles(output.second);
        bool same = was.size() == now.size();
        for (std::size_t i = 0; same && i < was.size(); ++i)
        {
            std::span<const uint8_t> expected = was[i].data;
            for (const hokuto::format::ChangedGroupFile& group_file : changed)
            {
                const std::optional<std::size_t> s = sarc_of[group_file.index];
                if (s && sarcs[*s].first == output.first &&
                    std::ranges::equal(was[i].data, group_files[group_file.index]))
                {
                    expected = group_file.bytes;
                }
            }
            same = was[i].name == now[i].name && std::ranges::equal(now[i].data, expected);
        }
        if (!same)
        {
            Problem(path, output.first + " doesn't hold the new group files in the place of the game's copies", totals);
        }
        ++totals.sarc_writes;
    }
}

// Checks the groups of `original`, an archive at `path`, and the group files beside it. `layout` is the cut of the
// archive alone. The 3DS's file table holds no lists of groups. There, the name under which the tool finds a group file
// beside the archive gives its group (ntdlib::sar::SoundArchive::GroupFileGroups).
void CheckGroupFiles(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                     const sar::Options& options, Totals& totals)
{
    const sar::Flavor flavor = options.flavor;

    // Each group's file in the archive, at the size that the header gives. A group's file that ends the archive can end
    // without the zeros that pad INFX.
    sar::Options strict = options;
    strict.rules = sar::Rules::kStopAtFirstProblem;
    std::optional<sar::SoundArchive> read;
    try
    {
        read = sar::SoundArchive::Load(original, strict);
    }
    catch (const std::exception&)
    {
        read.reset();
    }
    std::set<uint32_t> sequence_files;
    for (std::size_t i = 0; read && i < read->Sounds().size(); ++i)
    {
        if (read->Sounds()[i].type == sar::SoundType::kSequence)
        {
            sequence_files.insert(read->Sounds()[i].file_id);
        }
    }
    const xsflib::Reader r(original, flavor == sar::Flavor::kCafe);
    for (std::size_t g = 0; read && g < read->Groups().size(); ++g)
    {
        const uint32_t file = read->Groups()[g].file_id;
        const std::optional<std::size_t> offset =
            file == sar::kExternalGroupFile ? std::nullopt : read->FileOffset(file);
        if (!offset)
        {
            continue;
        }

        const std::size_t size = r.U32(*offset + 0x0C);
        const std::span<const uint8_t> bytes =
            std::span(original).subspan(*offset, std::min(size, original.size() - *offset));
        if (!std::ranges::equal(sar::GroupWith(bytes, flavor, {}, {}), bytes))
        {
            Problem(path, "GroupWith doesn't give the file of group " + read->Groups()[g].name + " back", totals);
        }
        ++totals.groups_inside;
    }

    // The group files beside the archive, cut with it.
    const std::shared_ptr<const sar::SoundArchive> opened = hokuto::song::OpenArchive(path);
    const std::vector<std::vector<uint8_t>>& group_files = opened->GroupFiles();
    if (group_files.empty())
    {
        return;
    }
    ++totals.group_archives;

    // A sequence file that only the group files hold becomes a sequence after the archive's. The fields of its sounds'
    // starts name items of it.
    const sar::Layout cut = sar::Cut(original, group_files, options);
    const auto group_only = [&layout](const sar::Field& field)
    {
        return field.from.sequence && field.to.sequence && field.from.piece >= layout.sequences.size();
    };
    std::vector<sar::Field> archive_fields;
    std::ranges::copy_if(cut.fields, std::back_inserter(archive_fields), std::not_fn(group_only));
    const bool prefix = cut.sequences.size() >= layout.sequences.size() &&
                        std::equal(layout.sequences.begin(), layout.sequences.end(), cut.sequences.begin());
    if (cut.pieces != layout.pieces || archive_fields != layout.fields || !prefix)
    {
        Problem(path, "the cut with the group files gives the archive other pieces, sequences or fields", totals);
        return;
    }
    totals.group_only_sequences += static_cast<int>(cut.sequences.size() - layout.sequences.size());
    totals.group_only_starts += static_cast<int>(cut.fields.size() - archive_fields.size());

    // The SARC archives beside the archive, and the one that holds each group file. Each SARC archive comes back with
    // its files.
    const std::array<std::filesystem::path, 1> folder = {path.parent_path()};
    const std::vector<hokuto::format::FileBeside> sarcs = hokuto::song::SarcArchivesIn(folder);
    std::vector<std::optional<std::size_t>> sarc_of(group_files.size());
    for (std::size_t s = 0; s < sarcs.size(); ++s)
    {
        for (const sar::SarcFile& file : sar::SarcFiles(sarcs[s].second))
        {
            const std::vector<uint8_t> same(file.data.begin(), file.data.end());
            if (sar::WithSarcFile(sarcs[s].second, file.data, same) != sarcs[s].second)
            {
                Problem(path, sarcs[s].first + " doesn't come back with its files", totals);
            }
            for (std::size_t k = 0; k < group_files.size(); ++k)
            {
                if (!sarc_of[k] && std::ranges::equal(file.data, group_files[k]))
                {
                    sarc_of[k] = s;
                }
            }
        }
        ++totals.sarcs;
    }

    // Each group file's group as the program's import finds it, and the group's name.
    const std::vector<std::string>& labels = opened->GroupFileGroups();
    const hokuto::format::File imported = hokuto::format::ImportArchive(original, flavor, group_files, {}, labels);
    std::vector<std::optional<uint32_t>> group_of(group_files.size());
    std::vector<std::string> group_name(group_files.size());
    for (std::size_t k = 0; k < group_files.size(); ++k)
    {
        group_of[k] = imported.native->group_files.at(k).group;
        if (group_of[k] && read)
        {
            group_name[k] = read->Groups().at(*group_of[k]).name;
        }
    }

    for (std::size_t i = 0; i < group_files.size(); ++i)
    {
        const std::string name = "group file " + std::to_string(i);
        const sar::GroupFileLayout& group = cut.group_files.at(i);
        if (sar::Join(flavor, cut.sequences, group.pieces, group.fields) != group_files[i])
        {
            Problem(path, name + " doesn't join back", totals);
            continue;
        }
        const std::optional<uint32_t> found = group_of[i];
        if (!found || (read && read->Groups().at(*found).file_id != sar::kExternalGroupFile))
        {
            Problem(path, "the import finds no group beside the archive for " + name, totals);
        }
        if (sar::GroupWith(group_files[i], flavor, {}, {}) != group_files[i])
        {
            Problem(path, "GroupWith doesn't give " + name + " back", totals);
        }
        ++totals.group_files;

        // Each file whose copy the group file holds takes 40 more bytes, in every copy.
        for (const sar::GroupMember& member : sar::ReadGroupFile(group_files[i], flavor).files)
        {
            if (!member.copy)
            {
                continue;
            }

            // A copy that is a piece of the archive's sequence isn't a run.
            const std::string what = "file " + std::to_string(member.file) + " of " + name;
            std::vector<uint8_t> longer = *member.copy;
            longer.resize(longer.size() + 40, 0x5A);
            sar::ReplacedPieces replaced;
            try
            {
                replaced = sar::ReplaceFile(flavor, cut.sequences, cut.pieces, cut.fields, cut.group_files, member.file,
                                            longer);
            }
            catch (const std::invalid_argument& error)
            {
                if (!sequence_files.contains(member.file) ||
                    !std::string_view(error.what()).ends_with("isn't a piece of its own"))
                {
                    throw;
                }
                ++totals.group_sequences;
                continue;
            }
            std::vector<sar::GroupFileLayout> after = cut.group_files;
            const std::vector<uint8_t> joined = sar::Join(flavor, cut.sequences, replaced.pieces, cut.fields);
            sar::SoundArchive check = sar::SoundArchive::Load(joined, options);
            std::vector<hokuto::format::ChangedGroupFile> changed;
            for (std::size_t k = 0; k < after.size(); ++k)
            {
                after[k].pieces = replaced.group_files.at(k);
                std::vector<uint8_t> group_file = sar::Join(flavor, cut.sequences, after[k].pieces, after[k].fields);
                if (group_file != group_files[k])
                {
                    changed.push_back({k, group_of[k], group_name[k], group_file});
                }
                check.AddGroupFile(std::move(group_file));
            }
            CheckSarcs(path, changed, joined, flavor, sarcs, sarc_of, group_files, labels, totals);
            if (!std::ranges::equal(check.FileData(member.file), longer))
            {
                Problem(path, what + " doesn't read back with longer bytes", totals);
            }
            for (const sar::FileCopy& copy :
                 sar::FileCopies(flavor, cut.sequences, replaced.pieces, cut.fields, after, member.file))
            {
                if (copy.bytes != longer)
                {
                    Problem(path, what + " keeps a copy without the longer bytes", totals);
                }
                ++totals.group_copies;
            }
            ++totals.group_replaced;
        }
    }
}

// Checks the runs of files that `layout`, the cut of `original`, holds, after each one grows.
void CheckRuns(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
               const sar::Options& options, Totals& totals)
{
    // A run that padding follows holds a file.
    sar::Layout grown = layout;
    std::set<std::size_t> runs;
    for (std::size_t i = 0; i + 1 < grown.pieces.size(); ++i)
    {
        auto* run = std::get_if<std::vector<uint8_t>>(&grown.pieces[i]);
        if (run && std::holds_alternative<sar::Padding>(grown.pieces[i + 1]))
        {
            const std::vector<uint8_t> gain = Gain(run->size());
            run->insert(run->end(), gain.begin(), gain.end());
            runs.insert(i);
        }
    }
    totals.runs += static_cast<int>(runs.size());
    const std::vector<uint8_t> joined = sar::Join(grown.flavor, grown.sequences, grown.pieces, grown.fields);
    const sar::SoundArchive before = sar::SoundArchive::Load(original, options);
    const sar::SoundArchive after = sar::SoundArchive::Load(joined, options);
    if (after.GroupErrors().size() != before.GroupErrors().size() || after.Truncated() != before.Truncated() ||
        after.FileCount() != before.FileCount() || after.Sounds().size() != before.Sounds().size())
    {
        Problem(path, "the archive with longer files reads otherwise", totals);
        return;
    }

    // Each file in a run gained its bytes. Every other file keeps its bytes, but for a group that holds a run.
    const std::map<std::size_t, std::size_t> starts = PieceStarts(layout);
    std::vector<uint32_t> in_runs;
    for (uint32_t id = 0; id < before.FileCount(); ++id)
    {
        const std::span<const uint8_t> data = before.FileData(id);
        const std::optional<std::size_t> offset = before.FileOffset(id);
        if (data.empty() || !offset)
        {
            continue;
        }

        const auto found = starts.find(*offset);
        const bool run = found != starts.end() && runs.contains(found->second);
        auto holds_run = [&](const auto& start)
        {
            return runs.contains(start.second);
        };
        if (!run && std::any_of(starts.upper_bound(*offset), starts.lower_bound(*offset + data.size()), holds_run))
        {
            ++totals.run_groups;
            continue;
        }

        std::vector<uint8_t> expected(data.begin(), data.end());
        std::pair<int, int>& kind = totals.kinds[KindOf(data)];
        ++kind.second;
        if (run)
        {
            const std::vector<uint8_t> gain = Gain(data.size());
            expected.insert(expected.end(), gain.begin(), gain.end());
            in_runs.push_back(id);
            ++totals.run_files;
            ++kind.first;
        }
        if (!std::ranges::equal(after.FileData(id), expected))
        {
            Problem(path, "file " + std::to_string(id) + " differs after the files grow", totals);
        }
    }

    for (std::size_t i = 0; i < before.Sounds().size(); ++i)
    {
        if (after.Sounds()[i].start_offset != before.Sounds()[i].start_offset)
        {
            Problem(path, "sound " + std::to_string(i) + " moved its start after the files grow", totals);
        }
    }

    // The cut of the archive with longer files. Its runs between the files hold the fields' new values.
    const sar::Layout cut = sar::Cut(joined, options);
    bool same = cut.pieces.size() == grown.pieces.size() && cut.fields == grown.fields;
    for (std::size_t i = 0; same && i < cut.pieces.size(); ++i)
    {
        same =
            cut.pieces[i].index() == grown.pieces[i].index() && (!runs.contains(i) || cut.pieces[i] == grown.pieces[i]);
    }
    if (!same)
    {
        Problem(path, "the cut of the archive with longer files has other pieces or fields", totals);
    }

    // ReplaceFile, in every copy of the first, the middle and the last file in a run.
    if (in_runs.empty())
    {
        return;
    }

    for (const uint32_t id : std::set<uint32_t>{in_runs.front(), in_runs[in_runs.size() / 2], in_runs.back()})
    {
        const std::span<const uint8_t> data = before.FileData(id);
        std::vector<uint8_t> bytes(data.begin(), data.end());
        bytes.resize(bytes.size() + 40, 0x5A);
        const std::vector<sar::Piece> pieces =
            sar::ReplaceFile(layout.flavor, layout.sequences, layout.pieces, layout.fields, id, bytes);
        const sar::SoundArchive replaced =
            sar::SoundArchive::Load(sar::Join(layout.flavor, layout.sequences, pieces, layout.fields), options);
        ++totals.replaced;
        if (!std::ranges::equal(replaced.FileData(id), bytes))
        {
            Problem(path, "file " + std::to_string(id) + " lacks its new bytes", totals);
        }
    }
}

// Checks the writing of the tables' values, where the strict rules read the archive.
void CheckValues(const std::filesystem::path& path, const std::vector<uint8_t>& original, sar::Options options,
                 Totals& totals)
{
    options.rules = sar::Rules::kStopAtFirstProblem;
    std::optional<sar::SoundArchive> before;
    try
    {
        before.emplace(sar::SoundArchive::Load(original, options));
    }
    catch (const std::exception&)
    {
        return;
    }

    // The values that the archive holds give it back.
    std::vector<uint8_t> written = original;
    sar::WriteTableValues(written, options.flavor, before->Sounds(), before->Players());
    ++totals.values;
    if (written != original)
    {
        Problem(path, "the tables' values, written back, change the archive", totals);
        return;
    }

    // A new volume for every sound and new flags for every sequence sound's tracks read back, and nothing else moves.
    std::vector<sar::SoundInfo> sounds = before->Sounds();
    for (sar::SoundInfo& sound : sounds)
    {
        sound.volume = static_cast<uint8_t>(sound.volume ^ 1);
        sound.allocate_track_flags ^= sound.type == sar::SoundType::kSequence ? 0x8000 : 0;
    }
    sar::WriteTableValues(written, options.flavor, sounds, before->Players());
    const sar::SoundArchive after = sar::SoundArchive::Load(written, options);
    bool same = after.Sounds().size() == sounds.size() && after.Players().size() == before->Players().size();
    for (std::size_t i = 0; same && i < sounds.size(); ++i)
    {
        const sar::SoundInfo& read = after.Sounds()[i];
        same = read.volume == sounds[i].volume && read.allocate_track_flags == sounds[i].allocate_track_flags &&
               SameOtherwise(read, sounds[i]);
    }
    for (std::size_t i = 0; same && i < after.Players().size(); ++i)
    {
        same = after.Players()[i].playable_sound_max == before->Players()[i].playable_sound_max;
    }
    for (uint32_t id = 0; same && id < after.FileCount(); ++id)
    {
        same = std::ranges::equal(after.FileData(id), before->FileData(id));
    }

    if (!same)
    {
        Problem(path, "new volumes and tracks' flags read back otherwise", totals);
    }
}

// Checks the places of the sounds' optional values, where the strict rules read the archive.
void CheckPlaces(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                 sar::Options options, Totals& totals)
{
    options.rules = sar::Rules::kStopAtFirstProblem;
    std::optional<sar::SoundArchive> before;
    try
    {
        before.emplace(sar::SoundArchive::Load(original, options));
    }
    catch (const std::exception&)
    {
        return;
    }

    // The archive's first piece gives the places by itself.
    const std::vector<sar::ValuePlaces> places = sar::TableValuePlaces(original, options.flavor);
    const auto* head = layout.pieces.empty() ? nullptr : std::get_if<std::vector<uint8_t>>(&layout.pieces.front());
    ++totals.place_archives;
    if (head == nullptr || sar::TableValuePlaces(*head, options.flavor) != places)
    {
        Problem(path, "the first piece gives other places of the optional values", totals);
    }

    // A new pan curve, player priority and channel priority read back where the sound has places for them.
    std::vector<sar::SoundInfo> sounds = before->Sounds();
    for (sar::SoundInfo& sound : sounds)
    {
        sound.pan_curve = static_cast<uint8_t>(sound.pan_curve ^ 1);
        sound.player_priority = static_cast<uint8_t>(sound.player_priority ^ 1);
        sound.channel_priority = static_cast<uint8_t>(sound.channel_priority ^ 1);
    }
    std::vector<uint8_t> written = original;
    sar::WriteTableValues(written, options.flavor, sounds, before->Players());
    const sar::SoundArchive after = sar::SoundArchive::Load(written, options);
    const std::vector<sar::SoundInfo>& read = after.Sounds();
    if (read.size() != places.size() || before->Sounds().size() != places.size())
    {
        Problem(path, "TableValuePlaces gives places for another number of sounds", totals);
        return;
    }

    for (std::size_t i = 0; i < places.size(); ++i)
    {
        const sar::SoundInfo& old = before->Sounds()[i];
        const sar::ValuePlaces changed{read[i].pan_curve != old.pan_curve,
                                       read[i].player_priority != old.player_priority,
                                       read[i].channel_priority != old.channel_priority};
        if (changed != places[i])
        {
            Problem(path, "sound " + std::to_string(i) + "'s new values read back otherwise than its places", totals);
        }
        ++totals.place_sounds;
        totals.pan_places += places[i].pan ? 1 : 0;
        totals.priority_places += places[i].priority ? 1 : 0;
        totals.channel_places += places[i].channel ? 1 : 0;
    }
}

// Whether `a` and `b` hold the same values.
bool SameSound(const sar::SoundInfo& a, const sar::SoundInfo& b)
{
    return SameOtherwise(a, b) && a.volume == b.volume && a.allocate_track_flags == b.allocate_track_flags;
}

// The files that group `group` of `archive` loads. Nothing for a group whose file can't be read.
std::optional<std::vector<uint32_t>> GroupIdsOf(const sar::SoundArchive& archive, std::size_t group)
{
    try
    {
        return archive.GroupFileIds(group);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

// How many items of each kind an archive gains.
struct Growth
{
    std::size_t sounds = 0;
    std::size_t banks = 0;
    std::size_t wave_archives = 0;
    std::size_t files = 0;
};

// Whether `y` holds the items of `x`, judged by `same`, and `more` items after them.
template <typename Items, typename Same>
bool Prefix(const Items& x, const Items& y, std::size_t more, const Same& same)
{
    return y.size() == x.size() + more && std::ranges::equal(x, std::span(y).first(x.size()), same);
}

// Whether `b` reads the items of `a` other than its sounds the same, with the same names, and the same files. `b` holds
// the banks, wave archives and files of `growth` after them.
bool SameOtherItems(const sar::SoundArchive& a, const sar::SoundArchive& b, const Growth& growth)
{
    auto same_item = [](const auto& x, const auto& y)
    {
        return x.name == y.name && x.file_id == y.file_id;
    };
    auto same_player = [](const sar::PlayerInfo& x, const sar::PlayerInfo& y)
    {
        return x.name == y.name && x.playable_sound_max == y.playable_sound_max;
    };
    if (b.FileCount() != a.FileCount() + growth.files || !Prefix(a.Banks(), b.Banks(), growth.banks, same_item) ||
        !Prefix(a.WaveArchives(), b.WaveArchives(), growth.wave_archives, same_item) ||
        !std::ranges::equal(a.Groups(), b.Groups(), same_item) ||
        !std::ranges::equal(a.Players(), b.Players(), same_player) || a.ExternalGroups() != b.ExternalGroups() ||
        a.GroupErrors().size() != b.GroupErrors().size() || a.NamesDamaged() != b.NamesDamaged())
    {
        return false;
    }

    for (uint32_t id = 0; id < a.FileCount(); ++id)
    {
        if (!std::ranges::equal(a.FileData(id), b.FileData(id)) || a.ExternalPath(id) != b.ExternalPath(id) ||
            a.FileMissing(id) != b.FileMissing(id) || a.FileOffset(id).has_value() != b.FileOffset(id).has_value())
        {
            return false;
        }
    }
    for (std::size_t group = 0; group < a.Groups().size(); ++group)
    {
        if (GroupIdsOf(a, group) != GroupIdsOf(b, group))
        {
            return false;
        }
    }

    return true;
}

// Whether `b` reads the items of `a` the same, with the same names, and the same files. `b` holds the items of `growth`
// after them.
bool Extends(const sar::SoundArchive& a, const sar::SoundArchive& b, const Growth& growth)
{
    return Prefix(a.Sounds(), b.Sounds(), growth.sounds, SameSound) && SameOtherItems(a, b, growth);
}

// Whether the reader reads the same items from `a` and `b`, with the same names, and the same files.
bool SameReading(const sar::SoundArchive& a, const sar::SoundArchive& b)
{
    return Extends(a, b, {});
}

// The offset of the FILE block of `archive`, from the header's table of blocks.
std::size_t FileBlockOf(std::span<const uint8_t> archive, sar::Flavor flavor)
{
    const xsflib::Reader r(archive, flavor == sar::Flavor::kCafe);
    for (uint16_t i = 0; i < r.U16(0x10); ++i)
    {
        const std::size_t entry = 0x14 + std::size_t{i} * 12;
        if (r.U16(entry) == 0x2002)
        {
            return r.U32(entry + 4);
        }
    }

    throw xsflib::FormatError("an archive without a FILE block");
}

// The distance from `block`, the start of a FILE block in an archive of `flavor`, to the block's first file.
std::size_t LeadOf(std::size_t block, sar::Flavor flavor)
{
    const std::size_t alignment = flavor == sar::Flavor::kNx ? 64 : 32;
    return (block + 8 + alignment - 1) / alignment * alignment - block;
}

// Checks ntdlib's writing of the INFO block of `original` and its replacement. `layout` is the cut of `original`.
void CheckInfo(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
               const sar::Options& options, Totals& totals)
{
    const sar::Tables tables = sar::TablesOf(original, options.flavor);
    const sar::InfoBlock info = sar::InfoBlock::Read(tables.info, options.flavor);
    if (!std::ranges::equal(info.Write(), tables.info))
    {
        Problem(path, "the INFO block written again differs", totals);
        return;
    }
    ++totals.info_blocks;

    const sar::SoundArchive before = sar::SoundArchive::Load(original, options);
    if (before.Truncated())
    {
        ++totals.cut_short;
        return;
    }
    std::optional<sar::StringBlock> strings;
    if (tables.strings)
    {
        strings = sar::StringBlock::Read(*tables.strings, options.flavor);
    }
    if (sar::WithTables(original, options.flavor, strings, info) != original)
    {
        Problem(path, "WithTables changes the archive with its tables", totals);
        return;
    }

    // Each entry grows by 4 bytes.
    sar::InfoBlock grown = info;
    for (std::vector<std::vector<uint8_t>>* entries :
         {&grown.sounds, &grown.sound_groups, &grown.banks, &grown.wave_archives, &grown.groups, &grown.players,
          &grown.files})
    {
        for (std::vector<uint8_t>& entry : *entries)
        {
            entry.insert(entry.end(), {0xA5, 0x5A, 0xC3, 0x3C});
            ++totals.entries;
        }
    }
    const std::vector<uint8_t> moved = sar::WithTables(original, options.flavor, strings, grown);
    ++totals.moved;
    if (LeadOf(FileBlockOf(moved, options.flavor), options.flavor) !=
        LeadOf(FileBlockOf(original, options.flavor), options.flavor))
    {
        ++totals.new_lead;
    }
    if (!SameReading(before, sar::SoundArchive::Load(moved, options)))
    {
        Problem(path, "the reader reads the archive with longer entries otherwise", totals);
    }

    // The strict rules, where they read the original.
    sar::Options strict = options;
    strict.rules = sar::Rules::kStopAtFirstProblem;
    std::optional<sar::SoundArchive> strict_before;
    try
    {
        strict_before.emplace(sar::SoundArchive::Load(original, strict));
    }
    catch (const std::exception&)
    {
        // The strict rules don't read the original, and the check leaves them out.
    }
    if (strict_before && !SameReading(*strict_before, sar::SoundArchive::Load(moved, strict)))
    {
        Problem(path, "the strict rules read the archive with longer entries otherwise", totals);
    }

    if (sar::Cut(moved, options).sequences != layout.sequences)
    {
        Problem(path, "the cut of the archive with longer entries gives other sequences", totals);
    }
}

// Whether each entry of `archive`'s INFO block that holds a string index names its item there, and whether the string
// table holds the names in the order of the tables. The options of an entry start 20 bytes in for a sound, 24 for a
// sound group, 12 for a bank, 8 for a wave archive, and 4 for a group and a player. Bit 0 gives the string index.
bool NamesInOrder(std::span<const uint8_t> archive, sar::Flavor flavor)
{
    const sar::Tables tables = sar::TablesOf(archive, flavor);
    if (!tables.strings)
    {
        return true;
    }

    const sar::StringBlock strings = sar::StringBlock::Read(*tables.strings, flavor);
    const sar::InfoBlock info = sar::InfoBlock::Read(tables.info, flavor);
    const std::array lists = {&info.sounds,        &info.sound_groups, &info.banks,
                              &info.wave_archives, &info.groups,       &info.players};
    const std::array kTypes = {sar::kSoundItem,       sar::kSoundGroupItem, sar::kBankItem,
                               sar::kWaveArchiveItem, sar::kGroupItem,      sar::kPlayerItem};
    const std::array<std::size_t, 6> kOptions = {20, 24, 12, 8, 4, 4};
    uint32_t next = 0;
    for (std::size_t k = 0; k < lists.size(); ++k)
    {
        for (std::size_t i = 0; i < lists[k]->size(); ++i)
        {
            const xsflib::Reader r((*lists[k])[i], flavor == sar::Flavor::kCafe);
            if ((r.U32(kOptions[k]) & 1) == 0)
            {
                continue;
            }

            const uint32_t index = r.U32(kOptions[k] + 4);
            if (index != next || index >= strings.names.size() ||
                strings.names[index].item_id != sar::ItemId(kTypes[k], static_cast<uint32_t>(i)))
            {
                return false;
            }
            ++next;
        }
    }

    return next == strings.names.size();
}

// A name that `archive`'s STRG block lacks, from `stem`, or none for an archive without one.
std::string FreeName(std::span<const uint8_t> archive, sar::Flavor flavor, const std::string& stem)
{
    const std::optional<std::span<const uint8_t>> block = StringBlockOf(archive, flavor);
    if (!block)
    {
        return "";
    }

    std::string name = stem;
    while (sar::FindItem(*block, flavor, name))
    {
        name += "_";
    }

    return name;
}

// Checks ntdlib's new file and new sequence sound in `original`, with its cut `layout`. The model is the first sequence
// sound whose file the reader reads.
void CheckNewItems(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                   const sar::Options& options, Totals& totals)
{
    const sar::SoundArchive before = sar::SoundArchive::Load(original, options);
    std::optional<uint32_t> model;
    for (uint32_t i = 0; i < before.Sounds().size() && !model; ++i)
    {
        const sar::SoundInfo& sound = before.Sounds()[i];
        if (sound.type == sar::SoundType::kSequence && !before.FileData(sound.file_id).empty())
        {
            model = i;
        }
    }
    if (before.Truncated() || !model)
    {
        totals.no_model += before.Truncated() ? 0 : 1;
        return;
    }

    // The new file.
    const sar::SoundInfo& sound = before.Sounds()[*model];
    const std::span<const uint8_t> bytes = before.FileData(sound.file_id);
    const sar::ArchiveWithFile with = sar::WithNewFile(original, options.flavor, bytes);
    const sar::SoundArchive with_file = sar::SoundArchive::Load(with.archive, options);
    const std::size_t alignment = options.flavor == sar::Flavor::kNx ? 64 : 32;
    const std::size_t start = with_file.FileOffset(with.file).value_or(1);
    if (with.file != before.FileCount() || !Extends(before, with_file, {.files = 1}) ||
        !std::ranges::equal(with_file.FileData(with.file), bytes) || start % alignment != 0 ||
        with.archive.size() != start + bytes.size())
    {
        Problem(path, "the reader reads the archive with a new file otherwise", totals);
        return;
    }

    // The new sound, with a name that the archive lacks.
    const std::string name = FreeName(original, options.flavor, "HOKUTO_NEW_SOUND");
    const sar::NewSequenceSound added{
        .model = *model, .name = name, .file = with.file, .start = sound.start_offset, .banks = sound.banks};
    const std::vector<uint8_t> after = sar::WithNewSequenceSound(with.archive, options.flavor, added);
    const sar::SoundArchive read = sar::SoundArchive::Load(after, options);
    sar::SoundInfo expected = sound;
    expected.name = name.empty() ? "SEQ_" + std::to_string(before.Sounds().size()) : name;
    expected.file_id = with.file;
    if (!Extends(with_file, read, {.sounds = 1}) || !SameSound(read.Sounds().back(), expected))
    {
        Problem(path, "the reader reads the archive with a new sound otherwise", totals);
        return;
    }

    // The names.
    if (NamesInOrder(original, options.flavor) && !NamesInOrder(after, options.flavor))
    {
        Problem(path, "a string index of the archive with a new sound names another item", totals);
    }
    if (const std::optional<std::span<const uint8_t>> names = StringBlockOf(after, options.flavor))
    {
        for (const sar::ItemName& item : sar::StringBlock::Read(*names, options.flavor).names)
        {
            if (sar::FindItem(*names, options.flavor, item.name) != item.item_id)
            {
                Problem(path, "FindItem doesn't find " + item.name + " in the archive with a new sound", totals);
            }
        }
    }

    // The cut: the same sequences, and the model's again when its file is a piece.
    const sar::Layout cut = sar::Cut(after, options);
    const std::vector<std::size_t> starts = sar::Starts(original, options.flavor, layout.pieces);
    std::optional<uint32_t> piece;
    for (std::size_t i = 0; i < layout.pieces.size(); ++i)
    {
        const auto* file = std::get_if<sar::SequencePiece>(&layout.pieces[i]);
        if (file && starts[i] == before.FileOffset(sound.file_id))
        {
            piece = file->sequence;
        }
    }
    const std::size_t count = layout.sequences.size() + (piece ? 1 : 0);
    if (cut.sequences.size() != count ||
        !std::ranges::equal(std::span(cut.sequences).first(layout.sequences.size()), layout.sequences) ||
        (piece && cut.sequences.back() != layout.sequences[*piece]))
    {
        Problem(path, "the cut of the archive with a new sound gives other sequences", totals);
    }
    ++totals.new_sounds;
    totals.new_pieces += piece ? 1 : 0;
}

// Checks ntdlib's new wave sounds in `original`, with its cut `layout`. A copy of the first wave sound file that the
// reader reads takes a copy of each wave sound that the file holds, at its index. A wave sound copied from the first
// sequence sound follows, at index 0.
void CheckNewWaveSounds(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                        const sar::Layout& layout, const sar::Options& options, Totals& totals)
{
    // The models: the wave sounds of the file, and the sequence sound.
    const sar::SoundArchive before = sar::SoundArchive::Load(original, options);
    std::optional<uint32_t> file;
    std::vector<uint32_t> models;
    std::optional<uint32_t> sequence;
    for (uint32_t i = 0; i < before.Sounds().size(); ++i)
    {
        const sar::SoundInfo& sound = before.Sounds()[i];
        if (sound.type == sar::SoundType::kWaveSound && !before.FileData(sound.file_id).empty() &&
            file.value_or(sound.file_id) == sound.file_id)
        {
            file = sound.file_id;
            models.push_back(i);
        }
        if (sound.type == sar::SoundType::kSequence && !sequence)
        {
            sequence = i;
        }
    }
    if (before.Truncated() || !file)
    {
        totals.no_wave_sound += before.Truncated() ? 0 : 1;
        return;
    }

    // The new file, and the new sounds with names that the archive lacks. A wave sound has no banks, tracks or start. A
    // copy of a sequence sound keeps the model's channel priority and release priority fix in its details.
    const sar::ArchiveWithFile with = sar::WithNewFile(original, options.flavor, before.FileData(*file));
    std::vector<sar::NewWaveSound> added;
    std::vector<sar::SoundInfo> expected;
    const auto add = [&](uint32_t model, uint32_t index)
    {
        std::string name = FreeName(original, options.flavor, "HOKUTO_NEW_WAVE_SOUND");
        name += name.empty() ? "" : std::to_string(added.size());
        added.push_back({.model = model, .name = name, .file = with.file, .index = index});
        sar::SoundInfo& sound = expected.emplace_back(before.Sounds()[model]);
        sound.name = name.empty() ? "WSD_" + std::to_string(before.Sounds().size() + expected.size() - 1) : name;
        sound.file_id = with.file;
        sound.wave_sound_index = index;
        if (sound.type == sar::SoundType::kSequence)
        {
            const sar::SoundInfo kPlain;
            sound.type = sar::SoundType::kWaveSound;
            sound.banks = kPlain.banks;
            sound.allocate_track_flags = kPlain.allocate_track_flags;
            sound.start_offset = kPlain.start_offset;
        }
    };
    for (const uint32_t model : models)
    {
        add(model, before.Sounds()[model].wave_sound_index);
    }
    if (sequence)
    {
        add(*sequence, 0);
    }
    const std::vector<uint8_t> after = sar::WithNewWaveSounds(with.archive, options.flavor, added);

    // The reader reads every other item as before, and the new sounds with their models' values.
    const sar::SoundArchive with_file = sar::SoundArchive::Load(with.archive, options);
    const sar::SoundArchive read = sar::SoundArchive::Load(after, options);
    if (!Extends(with_file, read, {.sounds = added.size()}) ||
        !std::ranges::equal(std::span(read.Sounds()).last(added.size()), expected, SameSound))
    {
        Problem(path, "the reader reads the archive with new wave sounds otherwise", totals);
        return;
    }

    // The names.
    if (NamesInOrder(original, options.flavor) && !NamesInOrder(after, options.flavor))
    {
        Problem(path, "a string index of the archive with new wave sounds names another item", totals);
    }
    if (const std::optional<std::span<const uint8_t>> names = StringBlockOf(after, options.flavor))
    {
        for (const sar::ItemName& item : sar::StringBlock::Read(*names, options.flavor).names)
        {
            if (sar::FindItem(*names, options.flavor, item.name) != item.item_id)
            {
                Problem(path, "FindItem doesn't find " + item.name + " in the archive with new wave sounds", totals);
            }
        }
    }

    // The cut gives the same sequences.
    if (sar::Cut(after, options).sequences != layout.sequences)
    {
        Problem(path, "the cut of the archive with new wave sounds gives other sequences", totals);
    }
    totals.new_wave_sounds += static_cast<int>(added.size());
    totals.from_sequences += sequence ? 1 : 0;
    ++totals.wave_sound_archives;
}

// Whether the details of `entry`, a stream sound's entry in an archive of `flavor` and `version`, lead to an extension
// or name a prefetch file. A copy leaves both out: they describe the model's stream. In a 3DS archive of CSAR 2.3.0.0,
// the field of options after the reference to the table of tracks can hold values of the same kind: probably the
// stream's type, and its loop's start and end.
bool StreamExtras(std::span<const uint8_t> entry, sar::Flavor flavor, uint32_t version)
{
    const xsflib::Reader r(entry, flavor == sar::Flavor::kCafe);
    if (entry.size() < 20 || std::size_t{r.U32(16)} + 12 > entry.size())
    {
        return false;
    }

    const std::size_t details = r.U32(16);
    const std::size_t tracks = r.U32(details + 8);
    if (flavor == sar::Flavor::kCtr && version == 0x02030000)
    {
        return tracks >= 16 && details + 16 <= entry.size() && r.U32(details + 12) != 0;
    }

    const auto names = [&](std::size_t at)
    {
        return tracks >= at + 4 && details + at + 4 <= entry.size() && r.U32(details + at) != 0xFFFFFFFF;
    };
    return names(28) || names(32);
}

// Checks ntdlib's new files outside the archive and new stream sounds in `original`.
void CheckNewStreams(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                     const sar::Options& options, Totals& totals)
{
    const sar::SoundArchive before = sar::SoundArchive::Load(original, options);
    if (before.Truncated())
    {
        return;
    }
    const bool big_endian = options.flavor == sar::Flavor::kCafe;
    const sar::InfoBlock info = sar::InfoBlock::Read(sar::TablesOf(original, options.flavor).info, options.flavor);

    const auto last_entry = [&](std::span<const uint8_t> archive, bool sound)
    {
        sar::InfoBlock after = sar::InfoBlock::Read(sar::TablesOf(archive, options.flavor).info, options.flavor);
        return sound ? std::move(after.sounds.back()) : std::move(after.files.back());
    };

    // Each file outside the archive, again at its path.
    for (std::size_t i = 0; i < info.files.size(); ++i)
    {
        const std::vector<uint8_t>& entry = info.files[i];
        const xsflib::Reader r(entry, big_endian);
        if (entry.size() < 12 || r.U16(0) != 0x220D || r.U32(4) >= entry.size())
        {
            continue;
        }

        const std::string file_path = r.CString(r.U32(4), entry.size() - r.U32(4));
        const sar::ArchiveWithFile with = sar::WithNewExternalFile(original, options.flavor, file_path);
        if (with.file != info.files.size() || last_entry(with.archive, false) != entry)
        {
            Problem(path, "the new entry of " + file_path + " holds other bytes than the file's entry", totals);
        }
        ++totals.external_files;
    }

    // A copy of each stream sound, from the sound and from the first sound of another kind. ntdlib lays out the stream
    // sounds of every version of the 3DS from CSAR 2.0.0.0 on but 2.1.0.0 and 2.3.1.0.
    if (before.Sounds().size() != info.sounds.size())
    {
        return;
    }
    const auto other = std::ranges::find_if(
        before.Sounds(), [](const sar::SoundInfo& sound) { return sound.type != sar::SoundType::kStream; });
    const std::string name = FreeName(original, options.flavor, "HOKUTO_NEW_STREAM_SOUND");
    const uint32_t version = xsflib::Reader(original, big_endian).U32(8);
    const bool ctr = options.flavor == sar::Flavor::kCtr;
    const bool laid_out =
        !ctr || version == 0x02000000 || version == 0x02020000 || version == 0x02030000 || version >= 0x02030200;
    bool any = false;
    for (uint32_t i = 0; i < before.Sounds().size(); ++i)
    {
        const sar::SoundInfo& sound = before.Sounds()[i];
        if (sound.type != sar::SoundType::kStream)
        {
            continue;
        }

        // The copy's entry holds the sound's, the string index aside.
        const sar::NewStreamSound copy{.model = i, .name = name, .file = sound.file_id, .stream = sound.stream};
        if (!laid_out)
        {
            try
            {
                sar::WithNewStreamSound(original, options.flavor, copy);
                Problem(path,
                        "takes a copy of " + sound.name + " in a version whose stream sounds ntdlib doesn't lay out",
                        totals);
            }
            catch (const std::invalid_argument&)
            {
                ++totals.stream_refused;
            }
            continue;
        }
        const std::vector<uint8_t> after = sar::WithNewStreamSound(original, options.flavor, copy);
        std::vector<uint8_t> entry = last_entry(after, true);
        const std::vector<uint8_t>& model = info.sounds[i];
        const bool named = model.size() >= 28 && (xsflib::Reader(model, big_endian).U32(20) & 1) != 0;
        if (named && entry.size() >= 28)
        {
            std::copy(model.begin() + 24, model.begin() + 28, entry.begin() + 24);
        }
        if (entry != model && StreamExtras(model, options.flavor, version))
        {
            ++totals.stream_extras;
        }
        else if (entry != model)
        {
            Problem(path, "the copy of " + sound.name + " holds another entry", totals);
        }

        // The reader reads the copy with the sound's values.
        const sar::SoundArchive read = sar::SoundArchive::Load(after, options);
        sar::SoundInfo expected = sound;
        expected.name = name.empty() ? "STRM_" + std::to_string(before.Sounds().size()) : name;
        if (!Extends(before, read, {.sounds = 1}) || !SameSound(read.Sounds().back(), expected))
        {
            Problem(path, "the reader reads the archive with a copy of " + sound.name + " otherwise", totals);
        }

        // A copy of a sound of another kind, with the sound's details.
        if (other != before.Sounds().end())
        {
            const sar::NewStreamSound from_other{.model = static_cast<uint32_t>(other - before.Sounds().begin()),
                                                 .name = name,
                                                 .file = sound.file_id,
                                                 .stream = sound.stream};
            const std::vector<uint8_t> other_entry =
                last_entry(sar::WithNewStreamSound(original, options.flavor, from_other), true);
            const xsflib::Reader m(model, big_endian);
            const xsflib::Reader o(other_entry, big_endian);
            const bool same =
                std::ranges::equal(std::span(model).subspan(m.U32(16)), std::span(other_entry).subspan(o.U32(16)));

            const auto part = [version](int shift)
            {
                return std::to_string(version >> shift & 0xFF);
            };
            const std::string text = ctr ? "CSAR " + part(24) + "." + part(16) + "." + part(8) + "." + part(0)
                                         : "FSAR " + part(16) + "." + part(8) + "." + part(0);
            totals.stream_layouts[text].first += same ? 1 : 0;
            ++totals.stream_layouts[text].second;
        }
        ++totals.new_streams;
        any = true;
    }
    totals.stream_archives += any ? 1 : 0;
}

// Checks ntdlib's new wave archives and new bank in `original`, with its cut `layout`. The model is the first bank
// whose file the reader reads, with waves in wave archives whose files the reader reads. Each of those wave archives
// gets a copy, with a name where the archive has names. The model's file then names the copies (WithWaveArchives) and
// goes in as a new bank. Its entry lists the copies where the model's entry lists wave archives.
void CheckNewBank(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                  const sar::Options& options, Totals& totals)
{
    const sar::SoundArchive before = sar::SoundArchive::Load(original, options);
    if (before.Truncated())
    {
        return;
    }

    // The model, and the wave archives of its waves in the order of their first use.
    const auto readable = [&before](uint32_t id)
    {
        const uint32_t index = id & 0x00FFFFFF;
        return id >> 24 == sar::kWaveArchiveItem && index < before.WaveArchives().size() &&
               !before.FileData(before.WaveArchives()[index].file_id).empty();
    };
    std::optional<uint32_t> model;
    std::vector<uint32_t> named;
    for (uint32_t i = 0; i < before.Banks().size() && !model; ++i)
    {
        std::vector<uint32_t> ids;
        try
        {
            for (const sar::WaveId& wave : sar::Bank::Parse(before.FileData(before.Banks()[i].file_id), options).waves)
            {
                if (std::ranges::find(ids, wave.wave_archive_item) == ids.end())
                {
                    ids.push_back(wave.wave_archive_item);
                }
            }
        }
        catch (const std::exception&)
        {
            continue;
        }

        if (!ids.empty() && std::ranges::all_of(ids, readable))
        {
            model = i;
            named = ids;
        }
    }
    if (!model)
    {
        ++totals.no_bank;
        return;
    }

    // A copy of each wave archive, and the map from the old to the new.
    std::vector<uint8_t> archive = original;
    std::map<uint32_t, uint32_t> map;
    std::vector<std::string> names;
    for (const uint32_t id : named)
    {
        const sar::WaveArchiveInfo& old = before.WaveArchives()[id & 0x00FFFFFF];
        const sar::ArchiveWithFile with = sar::WithNewFile(archive, options.flavor, before.FileData(old.file_id));
        names.push_back(FreeName(with.archive, options.flavor, "HOKUTO_NEW_WARC"));
        archive = sar::WithNewWaveArchive(with.archive, options.flavor, {.name = names.back(), .file = with.file});
        map[id] = sar::ItemId(sar::kWaveArchiveItem, static_cast<uint32_t>(before.WaveArchives().size() + map.size()));
    }

    // The bank's file with the copies, and the new bank. Its entry lists the copies where the model's lists its wave
    // archives.
    const sar::InfoBlock info = sar::InfoBlock::Read(sar::TablesOf(original, options.flavor).info, options.flavor);
    const xsflib::Reader entry(info.banks[*model], options.flavor == sar::Flavor::kCafe);
    const std::size_t list = entry.U32(8);
    std::vector<uint32_t> listed;
    for (uint32_t k = 0; k < entry.U32(list); ++k)
    {
        const uint32_t id = entry.U32(list + 4 + std::size_t{k} * 4);
        listed.push_back(map.contains(id) ? map.at(id) : id);
    }
    const std::span<const uint8_t> file = before.FileData(before.Banks()[*model].file_id);
    const std::vector<uint8_t> bytes = sar::WithWaveArchives(file, options.flavor, map);
    const sar::ArchiveWithFile with = sar::WithNewFile(archive, options.flavor, bytes);
    const std::string name = FreeName(with.archive, options.flavor, "HOKUTO_NEW_BANK");
    const std::vector<uint8_t> after =
        sar::WithNewBank(with.archive, options.flavor, {.name = name, .file = with.file, .wave_archives = listed});

    // The reader reads every other item as before, the copies with the wave archives' files, and the new bank with the
    // bank's file and the copies' waves.
    const sar::SoundArchive read = sar::SoundArchive::Load(after, options);
    bool same = Extends(before, read, {.banks = 1, .wave_archives = named.size(), .files = named.size() + 1});
    for (std::size_t k = 0; same && k < named.size(); ++k)
    {
        const sar::WaveArchiveInfo& copy = read.WaveArchives()[before.WaveArchives().size() + k];
        const sar::WaveArchiveInfo& old = before.WaveArchives()[named[k] & 0x00FFFFFF];
        same = (names[k].empty() || copy.name == names[k]) &&
               std::ranges::equal(read.FileData(copy.file_id), before.FileData(old.file_id));
    }
    const auto mapped = [&map](const sar::WaveId& a, const sar::WaveId& b)
    {
        return map.at(a.wave_archive_item) == b.wave_archive_item && a.index == b.index;
    };
    if (!same || (!name.empty() && read.Banks().back().name != name) || read.Banks().back().file_id != with.file ||
        !std::ranges::equal(read.FileData(with.file), bytes) ||
        !std::ranges::equal(sar::Bank::Parse(file, options).waves, sar::Bank::Parse(bytes, options).waves, mapped))
    {
        Problem(path, "the reader reads the archive with a new bank otherwise", totals);
        return;
    }

    // The names, and the cut.
    if (NamesInOrder(original, options.flavor) && !NamesInOrder(after, options.flavor))
    {
        Problem(path, "a string index of the archive with a new bank names another item", totals);
    }
    if (const std::optional<std::span<const uint8_t>> block = StringBlockOf(after, options.flavor))
    {
        for (const sar::ItemName& item : sar::StringBlock::Read(*block, options.flavor).names)
        {
            if (sar::FindItem(*block, options.flavor, item.name) != item.item_id)
            {
                Problem(path, "FindItem doesn't find " + item.name + " in the archive with a new bank", totals);
            }
        }
    }
    if (sar::Cut(after, options).sequences != layout.sequences)
    {
        Problem(path, "the cut of the archive with a new bank gives other sequences", totals);
    }
    ++totals.new_banks;
    totals.new_wave_archives += static_cast<int>(named.size());
    totals.listed += listed.empty() ? 0 : 1;
}

// Checks ntdlib's WithValuePlaces on `original`, with its cut `layout`, where the strict rules read the archive and it
// isn't cut short. Every option of a pan, of the priorities and of a channel priority leaves the sounds' entries, and
// WithValuePlaces gives them back.
void CheckValuePlaces(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                      const sar::Layout& layout, sar::Options options, Totals& totals)
{
    options.rules = sar::Rules::kStopAtFirstProblem;
    std::optional<sar::SoundArchive> before;
    try
    {
        before.emplace(sar::SoundArchive::Load(original, options));
    }
    catch (const std::exception&)
    {
        return;
    }
    if (before->Truncated())
    {
        return;
    }

    // The tables in the first piece give the fields of the starts by themselves.
    const auto* head = layout.pieces.empty() ? nullptr : std::get_if<std::vector<uint8_t>>(&layout.pieces.front());
    if (head == nullptr || sar::StartFields(*head, options.flavor) != sar::StartFields(original, options.flavor))
    {
        Problem(path, "the first piece gives other fields of the sounds' starts", totals);
    }

    // Every option of the values leaves the entries.
    const std::vector<sar::ValuePlaces> places = sar::TableValuePlaces(original, options.flavor);
    std::map<uint32_t, sar::ValuePlaces> held;
    for (uint32_t i = 0; i < places.size(); ++i)
    {
        if (places[i] != sar::ValuePlaces{})
        {
            held[i] = places[i];
        }
    }
    if (held.empty() || places.size() != before->Sounds().size())
    {
        return;
    }

    // The reader reads the values that it gives without the options, and everything else as before.
    const std::vector<uint8_t> bare = ntdlib::testing::sar_tools::WithoutValuePlaces(original, options.flavor, held);
    const sar::SoundArchive after = sar::SoundArchive::Load(bare, options);
    const sar::SoundInfo kUnset;
    bool same = after.Sounds().size() == places.size() && SameOtherItems(*before, after, {}) &&
                sar::TableValuePlaces(bare, options.flavor) == std::vector<sar::ValuePlaces>(places.size());
    for (std::size_t i = 0; same && i < places.size(); ++i)
    {
        sar::SoundInfo expected = before->Sounds()[i];
        if (places[i].pan)
        {
            expected.pan_mode = kUnset.pan_mode;
            expected.pan_curve = kUnset.pan_curve;
        }
        if (places[i].priority)
        {
            expected.player_priority = kUnset.player_priority;
            expected.actor_player_id = kUnset.actor_player_id;
        }
        if (places[i].channel)
        {
            expected.channel_priority = kUnset.channel_priority;
            expected.release_priority_fix = kUnset.release_priority_fix;
        }
        same = SameSound(expected, after.Sounds()[i]);
    }
    if (!same)
    {
        Problem(path, "the reader reads other values from the archive without the options", totals);
    }
    if (sar::Cut(bare, options).sequences != layout.sequences)
    {
        Problem(path, "the cut of the archive without the options gives other sequences", totals);
    }

    // The options back, with the values written again, give the archive back. Options that the entries hold stay.
    std::vector<uint8_t> placed = sar::WithValuePlaces(bare, options.flavor, held);
    sar::WriteTableValues(placed, options.flavor, before->Sounds(), before->Players());
    if (placed != original)
    {
        Problem(path, "the options and the values back don't give the archive back", totals);
    }
    if (sar::WithValuePlaces(original, options.flavor, held) != original)
    {
        Problem(path, "the options that the entries hold don't give the archive back", totals);
    }

    ++totals.option_archives;
    for (const auto& [sound, values] : held)
    {
        totals.pan_options += values.pan ? 1 : 0;
        totals.priority_options += values.priority ? 1 : 0;
        totals.channel_options += values.channel ? 1 : 0;
    }
}

// Checks ntdlib's lists of banks in `original`, with its cut `layout`. Each sequence sound with fewer than four banks
// takes a list one slot longer, with an empty slot at its end. Then each sequence sound with banks takes a list one
// bank shorter.
void CheckBankLists(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                    const sar::Options& options, Totals& totals)
{
    const sar::SoundArchive before = sar::SoundArchive::Load(original, options);
    if (before.Truncated())
    {
        return;
    }

    // The old lists, the longer ones and the shorter ones.
    std::map<uint32_t, std::vector<uint32_t>> old;
    std::map<uint32_t, std::vector<uint32_t>> longer;
    std::map<uint32_t, std::vector<uint32_t>> shorter;
    for (uint32_t i = 0; i < before.Sounds().size(); ++i)
    {
        const std::vector<uint32_t>& banks = before.Sounds()[i].banks;
        if (before.Sounds()[i].type != sar::SoundType::kSequence)
        {
            continue;
        }

        old[i] = banks;
        if (banks.size() < 4)
        {
            longer[i] = banks;
            longer[i].push_back(sar::kNoItem);
        }
        if (!banks.empty())
        {
            shorter[i].assign(banks.begin(), banks.end() - 1);
        }
    }
    if (old.empty())
    {
        return;
    }

    // Each sound reads its new list, and everything else reads as before.
    for (const std::map<uint32_t, std::vector<uint32_t>>* lists : {&longer, &shorter})
    {
        const std::vector<uint8_t> changed = sar::WithBankLists(original, options.flavor, *lists);
        const sar::SoundArchive after = sar::SoundArchive::Load(changed, options);
        bool same = after.Sounds().size() == before.Sounds().size() && SameOtherItems(before, after, {});
        for (uint32_t i = 0; same && i < before.Sounds().size(); ++i)
        {
            sar::SoundInfo expected = before.Sounds()[i];
            expected.banks = lists->contains(i) ? lists->at(i) : expected.banks;
            same = SameSound(expected, after.Sounds()[i]);
        }
        if (!same)
        {
            Problem(path, "the reader reads other values from the archive with other lists of banks", totals);
        }
        if (sar::Cut(changed, options).sequences != layout.sequences)
        {
            Problem(path, "the cut of the archive with other lists of banks gives other sequences", totals);
        }
        if (sar::WithBankLists(changed, options.flavor, old) != original)
        {
            Problem(path, "the old lists of banks don't give the archive back", totals);
        }
    }

    ++totals.bank_lists;
    totals.longer_lists += static_cast<int>(longer.size());
    totals.shorter_lists += static_cast<int>(shorter.size());
}

// Whether region `x` of a bank whose waves are `x_waves` and region `y` of a bank whose waves are `y_waves` hold the
// same values, and waves that name the same wave archive and wave.
bool SameRegion(const std::vector<sar::WaveId>& x_waves, const sar::VelocityRegion& x,
                const std::vector<sar::WaveId>& y_waves, const sar::VelocityRegion& y)
{
    if (x.wave_id_index >= x_waves.size() || y.wave_id_index >= y_waves.size())
    {
        return false;
    }

    const sar::WaveId& x_wave = x_waves[x.wave_id_index];
    const sar::WaveId& y_wave = y_waves[y.wave_id_index];

    const auto adshr = [](const sar::Adshr& a)
    {
        return std::array{a.attack, a.decay, a.sustain, a.hold, a.release};
    };

    return x_wave.wave_archive_item == y_wave.wave_archive_item && x_wave.index == y_wave.index &&
           x.original_key == y.original_key && x.volume == y.volume && x.pan == y.pan &&
           std::bit_cast<uint32_t>(x.pitch) == std::bit_cast<uint32_t>(y.pitch) &&
           x.ignore_note_off == y.ignore_note_off && x.key_group == y.key_group && x.interpolation == y.interpolation &&
           adshr(x.adshr) == adshr(y.adshr);
}

// Whether instrument `x` of bank `a` and instrument `y` of bank `b` hold the same regions, by SameRegion.
bool SameInstrument(const sar::Bank& a, const std::optional<sar::Instrument>& x, const sar::Bank& b,
                    const std::optional<sar::Instrument>& y)
{
    if (!x || !y)
    {
        return !x && !y;
    }

    const auto same_velocity = [&a, &b](const sar::KeyRegion::Vel& v, const sar::KeyRegion::Vel& w)
    {
        return v.lo == w.lo && v.hi == w.hi && v.region.has_value() == w.region.has_value() &&
               (!v.region || SameRegion(a.waves, *v.region, b.waves, *w.region));
    };
    const auto same_key = [&same_velocity](const sar::KeyRegion& k, const sar::KeyRegion& l)
    {
        return k.lo == l.lo && k.hi == l.hi && std::ranges::equal(k.velocities, l.velocities, same_velocity);
    };

    return std::ranges::equal(x->keys, y->keys, same_key);
}

// The place of each program's instrument in `bank`, a bank file of `flavor`. Nothing for a program without one.
std::vector<std::optional<std::size_t>> InstrumentPlaces(std::span<const uint8_t> bank, sar::Flavor flavor)
{
    const xsflib::Reader r(bank, flavor == sar::Flavor::kCafe);
    const std::size_t body = r.U32(0x18) + 8;
    const std::size_t table = body + static_cast<std::size_t>(r.S32(r.U16(body) == 0x0101 ? body + 4 : body + 12));
    std::vector<std::optional<std::size_t>> places;
    for (uint32_t i = 0; i < r.U32(table); ++i)
    {
        const std::size_t entry = table + 4 + std::size_t{i} * 8;
        places.push_back(r.U16(entry) == 0x5900 ? std::optional(table + static_cast<std::size_t>(r.S32(entry + 4)))
                                                : std::nullopt);
    }

    return places;
}

// A bank of an archive with instruments, read by the strict rules where they read it.
struct ReadBank
{
    std::span<const uint8_t> file;
    sar::Options options;
    sar::Bank bank;
};

// Checks the copies of each instrument of `source` into `target`, at the target's first free programs.
void CheckCopies(const std::filesystem::path& path, const ReadBank& target, const ReadBank& source, bool same_bank,
                 Totals& totals)
{
    // The copies: the target's empty programs first, then the programs past its table.
    const sar::Flavor flavor = target.options.flavor;
    std::vector<sar::InstrumentCopy> copies;
    uint32_t free = 0;
    for (uint32_t program = 0; program < source.bank.instruments.size(); ++program)
    {
        while (free < target.bank.instruments.size() && target.bank.instruments[free])
        {
            ++free;
        }
        if (source.bank.instruments[program])
        {
            copies.push_back({.from = source.file, .program = program, .to = free++});
        }
    }

    const std::vector<uint8_t> bank = sar::WithInstruments(target.file, flavor, copies);

    // The bank's instruments as before, each copy as its source, and nothing at the other new programs. All three banks
    // are read by the strict rules where they read both banks.
    sar::Options rules = target.options;
    rules.rules = target.options.rules == source.options.rules ? rules.rules : sar::Rules::kReadPastDamage;
    const sar::Bank before = sar::Bank::Parse(target.file, rules);
    const sar::Bank from = sar::Bank::Parse(source.file, rules);
    const sar::Bank read = sar::Bank::Parse(bank, rules);
    const std::size_t count = std::max<std::size_t>(before.instruments.size(), copies.back().to + 1);
    const auto same_wave = [](const sar::WaveId& a, const sar::WaveId& b)
    {
        return a.wave_archive_item == b.wave_archive_item && a.index == b.index;
    };
    bool same = read.instruments.size() == count && read.waves.size() >= before.waves.size() &&
                std::equal(before.waves.begin(), before.waves.end(), read.waves.begin(), same_wave);
    std::vector<bool> copied(count);
    for (const sar::InstrumentCopy& copy : copies)
    {
        copied[copy.to] = true;
        same = same && SameInstrument(from, from.instruments[copy.program], read, read.instruments[copy.to]);
    }
    for (std::size_t program = 0; same && program < count; ++program)
    {
        const bool old = program < before.instruments.size();
        same = copied[program] || SameInstrument(before, old ? before.instruments[program] : std::nullopt, read,
                                                 read.instruments[program]);
    }
    if (!same)
    {
        Problem(path,
                std::string("the reader reads a bank with copies of ") +
                    (same_bank ? "the same bank's" : "another bank's") + " instruments otherwise",
                totals);
        return;
    }

    // The sizes in the header, and the other byte order.
    const xsflib::Reader r(bank, flavor == sar::Flavor::kCafe);
    const xsflib::Reader old(target.file, flavor == sar::Flavor::kCafe);
    const std::size_t grown = bank.size() - target.file.size();
    if (r.U32(0x0C) != old.U32(0x0C) + grown || r.U32(0x1C) != old.U32(0x1C) + grown ||
        r.U32(r.U32(0x18) + 4) != old.U32(old.U32(0x18) + 4) + grown)
    {
        Problem(path, "a bank with copies of instruments holds other sizes", totals);
    }
    if (flavor != sar::Flavor::kCtr)
    {
        const uint32_t version = r.U32(8);
        if (sar::WithOtherByteOrder(sar::WithOtherByteOrder(bank, version), version) != bank)
        {
            Problem(path, "a bank with copies of instruments doesn't come back from the other byte order", totals);
        }
        totals.swapped_banks += same_bank ? 1 : 0;
    }

    // A copy of an instrument of the same bank holds the instrument's bytes: the copies lie one after another, before
    // the bank's other bytes. A wave of the copy takes the first of the bank's entries for the wave. Where the bank
    // names a wave twice, a word of the copy can hold the first entry's index in place of a later one's.
    if (same_bank)
    {
        const std::vector<std::optional<std::size_t>> sources = InstrumentPlaces(target.file, flavor);
        const std::vector<std::optional<std::size_t>> to = InstrumentPlaces(bank, flavor);
        const std::size_t entries = (count - before.instruments.size()) * 8;

        const auto first_entry = [&before](uint32_t index)
        {
            const auto same_wave = [&before, index](const sar::WaveId& wave)
            {
                return wave.wave_archive_item == before.waves[index].wave_archive_item &&
                       wave.index == before.waves[index].index;
            };
            return static_cast<uint32_t>(std::ranges::find_if(before.waves, same_wave) - before.waves.begin());
        };

        std::size_t end = *to[copies.front().to] + grown - entries;
        for (std::size_t k = copies.size(); k-- > 0;)
        {
            const std::size_t start = *to[copies[k].to];
            const std::size_t size = end - start;
            const std::span<const uint8_t> original = target.file.subspan(*sources[copies[k].program]);
            bool equal = size <= original.size() && size % 4 == 0;
            bool first = false;
            for (std::size_t at = 0; equal && at < size; at += 4)
            {
                const uint32_t a = old.U32(*sources[copies[k].program] + at);
                const uint32_t b = r.U32(start + at);
                first = first || a != b;
                equal = a == b || (a < before.waves.size() && b == first_entry(a));
            }
            totals.same_runs += equal && !first ? 1 : 0;
            totals.first_wave_runs += equal && first ? 1 : 0;
            if (!equal)
            {
                Problem(path, "a copy of an instrument of the same bank holds other bytes", totals);
            }
            end = start;
        }
    }

    totals.copies += static_cast<int>(copies.size());
    totals.new_waves += static_cast<int>(read.waves.size() - before.waves.size());
    for (const sar::InstrumentCopy& copy : copies)
    {
        totals.gap_copies += copy.to < before.instruments.size() ? 1 : 0;
    }
}

// Checks ntdlib's WithInstruments on each bank of `original` with instruments whose file the reader reads.
void CheckInstruments(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                      const sar::Options& options, Totals& totals)
{
    const sar::SoundArchive archive = sar::SoundArchive::Load(original, options);
    sar::Options strict = options;
    strict.rules = sar::Rules::kStopAtFirstProblem;
    std::vector<ReadBank> banks;
    for (const sar::BankInfo& info : archive.Banks())
    {
        const std::span<const uint8_t> file = archive.FileData(info.file_id);
        std::optional<ReadBank> bank;
        for (const sar::Options& rules : {strict, options})
        {
            try
            {
                bank = ReadBank{file, rules, sar::Bank::Parse(file, rules)};
                break;
            }
            catch (const std::exception&)
            {
                // The next rules try the bank.
            }
        }
        if (!bank)
        {
            ++totals.unread_banks;
            continue;
        }
        if (bank->options.rules != strict.rules)
        {
            ++totals.loose_banks;
        }
        if (std::ranges::none_of(bank->bank.instruments, [](const auto& instrument) { return instrument.has_value(); }))
        {
            ++totals.empty_banks;
            continue;
        }

        banks.push_back(*bank);
    }

    for (std::size_t i = 0; i < banks.size(); ++i)
    {
        try
        {
            if (sar::WithInstruments(banks[i].file, options.flavor, {}) !=
                std::vector(banks[i].file.begin(), banks[i].file.end()))
            {
                Problem(path, "a bank doesn't come back without copies", totals);
            }
            CheckCopies(path, banks[i], banks[(i + 1) % banks.size()], false, totals);
            CheckCopies(path, banks[i], banks[i], true, totals);
            ++totals.instrument_banks;
        }
        catch (const std::exception& error)
        {
            Problem(path, std::string("bank ") + std::to_string(i) + ": " + error.what(), totals);
        }
    }
}

// The file of `sequence` as the Wii's writer writes it, with the grammar of `build`.
std::vector<uint8_t> WiiFileOf(const commands::Sequence& sequence, uint32_t version, const ntdlib::SndBuild& build)
{
    const nwsnd::Encoded encoded = nwsnd::EncodeSequence(sequence, nwsnd::GrammarOf<nwsnd::RevolutionPolicy>(build));
    ntdlib::rsar::SequenceFile file;
    file.version = static_cast<uint16_t>(version);
    file.data = encoded.bytes;
    for (const commands::Label& label : sequence.labels)
    {
        file.labels.push_back({label.name, encoded.offsets[label.item]});
    }

    return file.Write();
}

// Whether each internal group of `archive` holds its files in the layout of NW4R's tools: at multiples of 32 bytes, one
// after another, and then their wave data.
bool LaidOutAsTools(const ntdlib::rsar::SoundArchive& archive)
{
    for (const ntdlib::rsar::SoundArchive::Group& group : archive.Groups())
    {
        uint32_t files = 0;
        uint32_t waves = 0;
        for (const ntdlib::rsar::SoundArchive::Group::Item& item : group.items)
        {
            if (!group.internal)
            {
                continue;
            }
            if (item.offset != files || (group.offset + item.offset) % 32 != 0 ||
                (item.wave_data_size > 0 && item.wave_data_offset != waves))
            {
                return false;
            }

            files += item.size;
            waves += item.wave_data_size;
        }
        if (group.internal && (files != group.size || group.wave_data_offset != group.offset + group.size))
        {
            return false;
        }
    }

    return true;
}

// Checks an archive of the Wii (RSAR) through Hokuto's import and ntdlib's rsar module: the round trip, then every
// sequence grown as Grow grows it. ntdlib's reader finds each file where it moved. A file that holds no sequence keeps
// its bytes, and so does each file's wave data. A sequence's file holds the grown commands as the writer writes them.
// Each group holds its files as NW4R's tools lay them out. The cut of the grown archive gives the grown commands and
// the fields at the same places. As in the program, the import takes the game's build of NW4R SND from the main.dol of
// the extracted disc that holds the archive. Without it, the archive's version gives the build.
void CheckWii(const std::filesystem::path& path, const std::vector<uint8_t>& original, Totals& totals)
{
    std::vector<uint8_t> dol;
    const std::optional<std::filesystem::path> main_dol = hokuto::song::FindMainDol(path);
    const hokuto::format::File file = hokuto::format::ImportWiiArchive(
        original, main_dol && xsflib::ReadFile(*main_dol, dol) ? hokuto::song::ReadWiiBuild(dol) : std::nullopt);
    if (hokuto::format::ExportArchive(file) != original)
    {
        Problem(path, "the exported archive differs", totals);
        return;
    }

    const ntdlib::SndBuild build = hokuto::format::BuildOfWiiArchive(*file.native);
    sar::Layout layout{.sequences = {}, .pieces = *file.native->pieces, .fields = file.native->fields};
    for (const auto& sequence : file.sequences)
    {
        layout.sequences.push_back(*sequence);
    }
    totals.wii_sequences += static_cast<int>(layout.sequences.size());
    totals.wii_fields += static_cast<int>(layout.fields.size());

    // The file at each sequence piece's start.
    const ntdlib::rsar::SoundArchive before = ntdlib::rsar::SoundArchive::Load(original, {.playback_fields = true});
    const std::vector<std::size_t> starts = ntdlib::rsar::Starts(original, layout.pieces);
    std::map<std::size_t, std::size_t> piece_at;
    for (std::size_t i = 0; i < layout.pieces.size(); ++i)
    {
        if (std::holds_alternative<sar::SequencePiece>(layout.pieces[i]))
        {
            piece_at[starts[i]] = i;
            ++totals.wii_pieces;
        }
    }

    const auto piece_of = [&](const ntdlib::rsar::SoundArchive& archive, uint32_t id) -> std::optional<std::size_t>
    {
        const std::span<const uint8_t> data = archive.FileData(id);
        const auto found = data.empty() ? piece_at.end()
                                        : piece_at.find(static_cast<std::size_t>(data.data() - archive.Bytes().data()));
        return found != piece_at.end() ? std::optional(found->second) : std::nullopt;
    };

    // Every sequence grows, and the archive keeps its other files where they moved.
    std::vector<uint32_t> added;
    const sar::Layout grown = Grow(layout, added);
    const std::vector<uint8_t> bigger = ntdlib::rsar::Join(build, grown.sequences, grown.pieces, grown.fields);
    const ntdlib::rsar::SoundArchive after = ntdlib::rsar::SoundArchive::Load(bigger, {.playback_fields = true});
    if (after.Truncated() != before.Truncated() || after.Sounds().size() != before.Sounds().size())
    {
        Problem(path, "the grown archive reads otherwise", totals);
    }
    if (!before.Truncated() && !LaidOutAsTools(after))
    {
        Problem(path, "a group of the grown archive isn't laid out as NW4R's tools lay one out", totals);
    }

    std::set<uint32_t> ids;
    for (const ntdlib::rsar::SoundArchive::Group& group : before.Groups())
    {
        for (const ntdlib::rsar::SoundArchive::Group::Item& item : group.items)
        {
            ids.insert(item.file_id);
        }
    }
    for (const uint32_t id : ids)
    {
        const std::span<const uint8_t> old_file = before.FileData(id);
        const std::span<const uint8_t> new_file = after.FileData(id);
        const std::span<const uint8_t> old_waves = before.WaveData(id);
        const std::span<const uint8_t> new_waves = after.WaveData(id);
        if (!std::ranges::equal(old_waves, new_waves))
        {
            Problem(path, "the wave data of file " + std::to_string(id) + " moved otherwise", totals);
        }
        const std::optional<std::size_t> piece = piece_of(before, id);
        if (!piece)
        {
            totals.wii_left += old_file.size() >= 4 && std::memcmp(old_file.data(), "RSEQ", 4) == 0 ? 1 : 0;
            if (!std::ranges::equal(old_file, new_file))
            {
                Problem(path, "file " + std::to_string(id) + " moved otherwise", totals);
            }
            continue;
        }

        const auto& sequence = std::get<sar::SequencePiece>(grown.pieces[*piece]);
        if (!std::ranges::equal(new_file, WiiFileOf(grown.sequences[sequence.sequence], sequence.version, build)))
        {
            Problem(path, "sequence file " + std::to_string(id) + " holds other bytes than its grown commands", totals);
        }
        ++totals.wii_files;
    }

    // The cut of the grown archive gives the grown commands, the same copies and the fields at the same places.
    const ntdlib::rsar::Layout cut = ntdlib::rsar::Cut(bigger, build);
    const sar::Layout recut{.sequences = cut.sequences, .pieces = cut.pieces, .fields = cut.fields};
    if (recut.sequences != grown.sequences || SequencesOfPieces(recut) != SequencesOfPieces(grown) ||
        !SamePlaces(recut, grown))
    {
        Problem(path, "the cut of the grown archive differs", totals);
    }
    totals.wii_bytes += static_cast<int64_t>(bigger.size()) - static_cast<int64_t>(original.size());
}

// Checks the archive at `path` (see the top of the file). With `instruments_only`, an archive of the 3DS, the Wii U or
// the Switch takes only the copies of instruments.
void Check(const std::filesystem::path& path, bool instruments_only, Totals& totals)
{
    std::vector<uint8_t> original;
    if (!xsflib::ReadFile(path, original) || original.size() < 8)
    {
        Problem(path, "can't be read", totals);
        return;
    }

    // The kind from the magic and the byte order mark. The Wii's archives take other checks.
    if (std::memcmp(original.data(), "RSAR", 4) == 0)
    {
        ++totals.wii_archives;
        try
        {
            CheckWii(path, original, totals);
        }
        catch (const std::exception& error)
        {
            Problem(path, error.what(), totals);
        }
        return;
    }

    sar::Options options;
    options.flavor = original[0] == 'C'    ? sar::Flavor::kCtr
                     : original[4] == 0xFE ? sar::Flavor::kCafe
                                           : sar::Flavor::kNx;
    ++totals.archives;

    try
    {
        if (instruments_only)
        {
            CheckInstruments(path, original, options, totals);
            return;
        }

        const sar::Layout layout = sar::Cut(original, options);
        if (sar::Join(layout.flavor, layout.sequences, layout.pieces, layout.fields) != original)
        {
            Problem(path, "the joined pieces differ", totals);
            return;
        }

        CheckNames(path, original, options, totals);
        CheckInfo(path, original, layout, options, totals);
        CheckNewItems(path, original, layout, options, totals);
        CheckNewWaveSounds(path, original, layout, options, totals);
        CheckNewStreams(path, original, options, totals);
        CheckNewBank(path, original, layout, options, totals);
        CheckBankLists(path, original, layout, options, totals);
        CheckValues(path, original, options, totals);
        CheckPlaces(path, original, layout, options, totals);
        CheckValuePlaces(path, original, layout, options, totals);
        CheckRuns(path, original, layout, options, totals);
        CheckGroupFiles(path, original, layout, options, totals);
        CheckInstruments(path, original, options, totals);
        totals.sequences += static_cast<int>(layout.sequences.size());
        totals.pieces += static_cast<int>(SequencesOfPieces(layout).size());
        totals.fields += static_cast<int>(layout.fields.size());
        std::vector<uint32_t> added;
        const sar::Layout grown = Grow(layout, added);
        const std::vector<uint8_t> joined = sar::Join(grown.flavor, grown.sequences, grown.pieces, grown.fields);
        CheckTables(path, original, joined, layout, grown, added, options, totals);

        // The cut of the grown archive.
        const sar::Layout cut = sar::Cut(joined, options);
        if (cut.sequences.size() != grown.sequences.size() || SequencesOfPieces(cut) != SequencesOfPieces(grown) ||
            !SamePlaces(cut, grown))
        {
            Problem(path, "the cut of the grown archive has other pieces or fields", totals);
            return;
        }

        for (std::size_t s = 0; s < grown.sequences.size(); ++s)
        {
            const std::vector<commands::Command>& items = grown.sequences[s].items;
            auto unreached = [](const commands::Command& item)
            {
                return item.kind == commands::Kind::kData;
            };
            if (cut.sequences[s] == grown.sequences[s])
            {
                ++totals.same;
            }
            else if (std::ranges::any_of(items, unreached))
            {
                ++totals.unreached;
            }
            else
            {
                Problem(path, "the cut of the grown archive gives sequence " + std::to_string(s) + " otherwise",
                        totals);
            }
        }
    }
    catch (const std::exception& error)
    {
        Problem(path, error.what(), totals);
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    const bool instruments_only = arguments.size() > 1 && arguments[1] == "--instruments";
    const std::size_t first = instruments_only ? 2 : 1;
    if (arguments.size() <= first)
    {
        std::fprintf(stderr, "usage: hokuto_check_layout [--instruments] <archive>...\n");
        return 2;
    }

    Totals totals;
    for (std::size_t i = first; i < arguments.size(); ++i)
    {
        Check(xsflib::PathFromUtf8(arguments[i]), instruments_only, totals);
    }

    std::printf(
        "%d archives: %d sequences in %d pieces, %d fields. Checked %d files (%d groups with copies apart), %d starts, "
        "%d archives by the strict rules. %d grown sequences cut the same, %d with unreached bytes otherwise. %d "
        "sequence files stay in the bytes. Wrote the tables' values of %d archives. %d runs of files grew: %d files "
        "(%d groups with runs apart). Replaced %d files. Wrote %d STRG blocks again, with %d names. %d archives have "
        "no STRG block. Wrote %d INFO blocks again. %d entries of %d archives grew, and the zeros before the first "
        "file changed in %d of them. %d archives are cut short. Added a file and a sequence sound to %d archives, %d "
        "of whose new files are pieces. %d archives have no sequence sound with a file to copy. Added %d banks with %d "
        "wave archives. The entries of %d of them list their wave archives. %d archives have no bank to copy. Gave %d "
        "sounds of %d archives longer lists of banks, and %d sounds shorter ones. Added %d wave sounds to %d archives, "
        "%d of them copied from a sequence sound. %d archives have no wave sound with a file to copy. Added %d files "
        "outside the archives again. Added copies of %d stream sounds of %d archives. Found the places of the optional "
        "values of %d sounds in %d archives: %d pans, %d priorities and %d channel priorities. Took the options of %d "
        "pans, %d priorities and %d channel priorities out of the entries of %d archives, and gave them back.\n",
        totals.archives, totals.sequences, totals.pieces, totals.fields, totals.files, totals.groups, totals.starts,
        totals.strict, totals.same, totals.unreached, totals.left, totals.values, totals.runs, totals.run_files,
        totals.run_groups, totals.replaced, totals.name_blocks, totals.names, totals.nameless, totals.info_blocks,
        totals.entries, totals.moved, totals.new_lead, totals.cut_short, totals.new_sounds, totals.new_pieces,
        totals.no_model, totals.new_banks, totals.new_wave_archives, totals.listed, totals.no_bank, totals.longer_lists,
        totals.bank_lists, totals.shorter_lists, totals.new_wave_sounds, totals.wave_sound_archives,
        totals.from_sequences, totals.no_wave_sound, totals.external_files, totals.new_streams, totals.stream_archives,
        totals.place_sounds, totals.place_archives, totals.pan_places, totals.priority_places, totals.channel_places,
        totals.pan_options, totals.priority_options, totals.channel_options, totals.option_archives);
    for (const auto& [kind, counts] : totals.kinds)
    {
        std::printf("  %s: %d of %d files in runs\n", kind.c_str(), counts.first, counts.second);
    }
    std::printf(
        "Copied %d instruments into %d banks, with %d new waves. %d of the copies went into empty programs. %d copies "
        "of instruments into the same bank held the instruments' bytes. %d more held them but for the indices of "
        "waves that the bank's table names more than once. A copy takes the first entry of such a wave. %d banks of "
        "the Wii U and the Switch came back from the other byte order with each kind of copy. %d banks hold no "
        "instruments. The reader doesn't read %d banks, and reads %d more only by the default rules.\n",
        totals.copies, totals.instrument_banks, totals.new_waves, totals.gap_copies, totals.same_runs,
        totals.first_wave_runs, totals.swapped_banks, totals.empty_banks, totals.unread_banks, totals.loose_banks);
    std::printf(
        "%d copies of stream sounds left out an extension or a prefetch file. ntdlib refused %d copies in 3DS "
        "archives of versions whose stream sounds it doesn't lay out.\n",
        totals.stream_extras, totals.stream_refused);
    for (const auto& [version, counts] : totals.stream_layouts)
    {
        std::printf("  %s: %d of %d copies of other kinds of sound hold the stream sound's details\n", version.c_str(),
                    counts.first, counts.second);
    }
    std::printf(
        "GroupWith gave the files of %d groups in the archives back. %d group files lie beside %d archives: each "
        "joined back. %d files of them took 40 more bytes, in %d copies, and %d copies of sequence files count apart. "
        "%d sequence files that only the group files hold became sequences, with %d fields of their sounds' starts.\n",
        totals.groups_inside, totals.group_files, totals.group_archives, totals.group_replaced, totals.group_copies,
        totals.group_sequences, totals.group_only_sequences, totals.group_only_starts);
    std::printf(
        "%d SARC archives lie beside the archives. Each came back with its files. After ReplaceFile, they took "
        "new group files %d times. %d new group files went into the userManagementFiles folders of their games' "
        "files.\n",
        totals.sarcs, totals.sarc_writes, totals.managed_writes);
    std::printf(
        "%d archives of the Wii: %d sequences in %d pieces, %d fields. After the growth, %d sequence files held "
        "their grown commands, and the archives grew by %lld bytes. %d sequence files stay in the bytes.\n",
        totals.wii_archives, totals.wii_sequences, totals.wii_pieces, totals.wii_fields, totals.wii_files,
        static_cast<long long>(totals.wii_bytes), totals.wii_left);
    std::printf("%d problems.\n", totals.problems);

    return totals.problems == 0 ? 0 : 1;
}
