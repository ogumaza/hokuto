// SPDX-License-Identifier: MIT

// The rounds Back and Next of hokuto_check_ds_replace.

#include "check_ds_replace.h"

namespace hokuto::tools::ds_replace
{

void Back(const Game& game, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    File edited = game.file;
    for (const uint32_t sound : SequenceSounds(game))
    {
        try
        {
            const std::vector<uint8_t> bytes = song::DsItemBytes(*game.archive, {song::ItemKind::kSequence, sound});
            edited = hokuto::edit::ReplaceDsSequence(edited, *game.archive, sound, bytes);
            totals.sequences++;
        }
        catch (const std::exception& error)
        {
            Fail(game, "back", song::DsSoundName(*game.archive, sound) + ": " + error.what(), totals);
        }
    }
    for (const auto& [bank, id] : BanksOf(game))
    {
        try
        {
            const std::vector<uint8_t> bytes = song::DsItemBytes(*game.archive, {song::ItemKind::kBank, bank});
            edited = hokuto::edit::ReplaceDsBank(edited, *game.archive, bank, bytes);
            totals.banks++;
        }
        catch (const std::exception& error)
        {
            // A file that ntdlib can't read can't go back through the edit, and neither can one that shares bytes
            // with another file.
            const std::string name = song::DsBankName(*game.archive, bank);
            if (MayRefuse<sdat::Bank>(game, id, tables.File(id)))
            {
                std::printf("%s: back: %s stays apart: %s\n", game.name.c_str(), name.c_str(), error.what());
                totals.back_apart++;
            }
            else
            {
                Fail(game, "back", name + ": " + error.what(), totals);
            }
        }
    }
    for (const auto& [wave_archive, id] : WaveArchivesOf(game))
    {
        try
        {
            const std::vector<uint8_t> bytes =
                song::DsItemBytes(*game.archive, {song::ItemKind::kWaveArchive, wave_archive});
            edited = hokuto::edit::ReplaceDsWaveArchive(edited, *game.archive, wave_archive, bytes);
            totals.wave_archives++;
        }
        catch (const std::exception& error)
        {
            const std::string name = song::DsWaveArchiveName(*game.archive, wave_archive);
            if (MayRefuse<sdat::WaveArchive>(game, id, tables.File(id)))
            {
                std::printf("%s: back: %s stays apart: %s\n", game.name.c_str(), name.c_str(), error.what());
                totals.back_apart++;
            }
            else
            {
                Fail(game, "back", name + ": " + error.what(), totals);
            }
        }
    }
    if (hokuto::format::ExportArchive(edited) != game.bytes)
    {
        Fail(game, "back", "the export differs from the game's archive", totals);
    }
}

namespace
{

// A copy of `bytes`.
std::vector<uint8_t> CopyBytes(std::span<const uint8_t> bytes)
{
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// Replaces each bank of `items`, or each wave archive, with the file of the next in `edited`. `expected` receives the
// new bytes of each file.
void ReplaceFiles(const Game& game, const std::vector<std::pair<uint32_t, uint32_t>>& items, bool bank, File& edited,
                  std::map<uint32_t, std::vector<uint8_t>>& expected, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    for (std::size_t i = 0; i + 1 < items.size(); i++)
    {
        const uint32_t id = items[i].second;
        const std::vector<uint8_t> bytes = CopyBytes(tables.File(items[i + 1].second));
        try
        {
            edited = bank ? hokuto::edit::ReplaceDsBank(edited, *game.archive, items[i].first, bytes)
                          : hokuto::edit::ReplaceDsWaveArchive(edited, *game.archive, items[i].first, bytes);
            expected[id] = bytes;
            totals.next_files++;
        }
        catch (const std::exception& error)
        {
            const bool may_refuse =
                bank ? MayRefuse<sdat::Bank>(game, id, bytes) : MayRefuse<sdat::WaveArchive>(game, id, bytes);
            if (may_refuse)
            {
                std::printf("%s: next: file %u stays apart: %s\n", game.name.c_str(), id, error.what());
                totals.next_apart++;
            }
            else
            {
                Fail(game, "next", "file " + std::to_string(id) + " is refused: " + error.what(), totals);
            }
        }
    }
}

// Checks the archive size in the header of `exported`, and the size of the FILE block where the sizes in the game file
// reach the end of the archive.
void CheckSizes(const Game& game, const std::vector<uint8_t>& exported, Totals& totals)
{
    if (xsflib::LoadLe32(&exported[8]) != exported.size())
    {
        Fail(game, "next", "the archive's header gives another size", totals);
    }

    // The size of the FILE block in the archive header and in the block, where the sizes in the game file reach the end
    // of the archive (ntdlib::sdat::Cut).
    const uint32_t file_block = xsflib::LoadLe32(&game.bytes[0x28]);
    if (std::size_t{file_block} + 8 <= std::min(game.bytes.size(), exported.size()))
    {
        for (const std::size_t at : {std::size_t{0x2C}, std::size_t{file_block} + 4})
        {
            const bool reaches = file_block + std::size_t{xsflib::LoadLe32(&game.bytes[at])} == game.bytes.size();
            if (reaches && file_block + std::size_t{xsflib::LoadLe32(&exported[at])} != exported.size())
            {
                Fail(game, "next",
                     "the FILE block's size at " + std::to_string(at) + " doesn't reach the archive's end", totals);
            }
        }
    }
}

// Checks that each file of `new_tables` contains its bytes from `expected`, or else its bytes before, and that no two
// files overlap.
void CheckFileBytes(const Game& game, const sdat::Archive& new_tables,
                    const std::map<uint32_t, std::vector<uint8_t>>& expected, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    std::vector<std::pair<uint64_t, uint64_t>> places;
    for (uint32_t id = 0; id < new_tables.files.size(); id++)
    {
        const std::span<const uint8_t> present = new_tables.File(id);
        const auto found = expected.find(id);
        const std::span<const uint8_t> wanted =
            found != expected.end() ? std::span<const uint8_t>(found->second) : tables.File(id);
        if (!std::ranges::equal(present, wanted))
        {
            Fail(game, "next",
                 "file " + std::to_string(id) + " holds other bytes: " + std::to_string(present.size()) +
                     " in place of " + std::to_string(wanted.size()),
                 totals);
        }
        if (new_tables.files[id].size > 0)
        {
            places.emplace_back(new_tables.files[id].offset, new_tables.files[id].size);
        }
    }
    std::ranges::sort(places);
    for (std::size_t i = 1; i < places.size(); i++)
    {
        if (places[i - 1].first + places[i - 1].second > places[i].first && places[i - 1].first != places[i].first)
        {
            Fail(game, "next", "two files overlap at " + std::to_string(places[i].first), totals);
        }
    }
}

// Checks that the files of `new_tables` that hold bytes keep their order in the archive.
void CheckFileOrder(const Game& game, const sdat::Archive& new_tables, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    std::vector<uint32_t> in_order;
    for (uint32_t id = 0; id < tables.files.size(); id++)
    {
        if (tables.files[id].size > 0)
        {
            in_order.push_back(id);
        }
    }
    std::ranges::stable_sort(in_order, {}, [&tables](uint32_t id) { return tables.files[id].offset; });
    for (std::size_t i = 1; i < in_order.size(); i++)
    {
        if (new_tables.files[in_order[i]].offset < new_tables.files[in_order[i - 1]].offset)
        {
            Fail(game, "next",
                 "file " + std::to_string(in_order[i]) + " moves before file " + std::to_string(in_order[i - 1]),
                 totals);
        }
    }
}

// Checks that ntdlib's readers read each new file of `new_tables` that `expected` lists.
void CheckNewFilesRead(const Game& game, const sdat::Archive& new_tables,
                       const std::vector<std::pair<uint32_t, uint32_t>>& banks,
                       const std::vector<std::pair<uint32_t, uint32_t>>& wave_archives,
                       const std::map<uint32_t, std::vector<uint8_t>>& expected, Totals& totals)
{
    try
    {
        for (const auto& [bank, id] : banks)
        {
            if (expected.contains(id))
            {
                sdat::Bank::Parse(new_tables.File(id));
            }
        }
        for (const auto& [wave_archive, id] : wave_archives)
        {
            if (expected.contains(id))
            {
                sdat::WaveArchive::Parse(new_tables.File(id));
            }
        }
    }
    catch (const std::exception& error)
    {
        Fail(game, "next", std::string("a new file can't be read: ") + error.what(), totals);
    }
}

// Checks that the old files of `banks`, `wave_archives` and the sounds of `replaced` give the game's archive back from
// `edited`, the file that exports `after`.
void CheckPutBack(const Game& game, const File& edited, const std::shared_ptr<const song::DsArchive>& after,
                  const std::vector<std::pair<uint32_t, uint32_t>>& banks,
                  const std::vector<std::pair<uint32_t, uint32_t>>& wave_archives,
                  const std::vector<uint32_t>& replaced, const std::map<uint32_t, std::vector<uint8_t>>& expected,
                  Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    File back = edited;
    const auto put_back = [&](const std::vector<std::pair<uint32_t, uint32_t>>& items)
    {
        for (const auto& [item, id] : items)
        {
            if (expected.contains(id))
            {
                back = hokuto::edit::WithDsFile(back, id, tables.File(id));
            }
        }
    };
    put_back(banks);
    put_back(wave_archives);
    std::shared_ptr<const song::DsArchive> current = after;
    for (const uint32_t sound : replaced)
    {
        back = hokuto::edit::ReplaceDsSequence(back, *current, sound, tables.File(tables.sequences.at(sound)->file_id));
        current = song::LoadDsArchive(hokuto::format::ExportArchive(back), game.archive->build);
    }
    if (hokuto::format::ExportArchive(back) != game.bytes)
    {
        Fail(game, "next", "the old files don't give the game's archive back", totals);
    }
}

} // namespace

void Next(const Game& game, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;

    // A sequence is read through the FAT of the version: as in the program, each edit reads the archive that the last
    // one built.
    File edited = game.file;
    std::map<uint32_t, std::vector<uint8_t>> expected;
    const std::vector<uint32_t> sounds = SequenceSounds(game);
    std::vector<uint32_t> replaced;
    std::shared_ptr<const song::DsArchive> current = game.archive;
    for (std::size_t i = 0; i + 1 < sounds.size(); i++)
    {
        const uint32_t id = tables.sequences.at(sounds[i])->file_id;
        const std::vector<uint8_t> bytes = CopyBytes(tables.File(tables.sequences.at(sounds[i + 1])->file_id));
        try
        {
            edited = hokuto::edit::ReplaceDsSequence(edited, *current, sounds[i], bytes);
            current = song::LoadDsArchive(hokuto::format::ExportArchive(edited), game.archive->build);
            expected[id] = bytes;
            replaced.push_back(sounds[i]);
            totals.next_files++;
        }
        catch (const std::exception& error)
        {
            Fail(game, "next", song::DsSoundName(*game.archive, sounds[i]) + ": " + error.what(), totals);
        }
    }

    const std::vector<std::pair<uint32_t, uint32_t>> banks = BanksOf(game);
    const std::vector<std::pair<uint32_t, uint32_t>> wave_archives = WaveArchivesOf(game);
    ReplaceFiles(game, banks, true, edited, expected, totals);
    ReplaceFiles(game, wave_archives, false, edited, expected, totals);

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    const std::shared_ptr<const song::DsArchive> after = song::LoadDsArchive(exported, game.archive->build);
    const sdat::Archive& new_tables = after->archive;
    if (new_tables.files.size() != tables.files.size())
    {
        Fail(game, "next", "the FAT has another length", totals);
        return;
    }
    CheckSizes(game, exported, totals);

    CheckFileBytes(game, new_tables, expected, totals);

    // The files that hold bytes keep their order in the archive.
    CheckFileOrder(game, new_tables, totals);

    // ntdlib's readers read each new file.
    CheckNewFilesRead(game, new_tables, banks, wave_archives, expected, totals);

    // Both forms of the file give the same archive.
    if (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(edited))) != exported ||
        hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(edited))) != exported)
    {
        Fail(game, "next", "a saved file gives another archive", totals);
    }

    // The old files give the game's archive back.
    CheckPutBack(game, edited, after, banks, wave_archives, replaced, expected, totals);
}

} // namespace hokuto::tools::ds_replace
