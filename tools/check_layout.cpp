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

#include "check_layout.h"

namespace hokuto::tools::layout
{

namespace
{

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

} // namespace hokuto::tools::layout

namespace
{

using hokuto::tools::layout::Check;
using hokuto::tools::layout::Totals;

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
