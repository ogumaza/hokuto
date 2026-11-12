// SPDX-License-Identifier: MIT

// The checks of hokuto_check_layout on the names, the INFO block, the tables and the values of an archive.

#include "check_layout.h"

namespace hokuto::tools::layout
{

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

namespace
{

// The sequence piece of `layout` that starts at `offset` in the archive, or nothing. `starts` maps the pieces' starts
// to their indices (PieceStarts).
const sar::SequencePiece* SequenceAt(const sar::Layout& layout, const std::map<std::size_t, std::size_t>& starts,
                                     std::optional<std::size_t> offset)
{
    const auto found = offset ? starts.find(*offset) : starts.end();
    return found == starts.end() ? nullptr : std::get_if<sar::SequencePiece>(&layout.pieces[found->second]);
}

// Checks each file of `after`, the grown archive of `before`: a sequence's file as the writer writes the grown commands
// of `grown`, or the same bytes.
void CheckFiles(const std::filesystem::path& path, const sar::SoundArchive& before, const sar::SoundArchive& after,
                const sar::Layout& layout, const sar::Layout& grown, const std::map<std::size_t, std::size_t>& starts,
                Totals& totals)
{
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
        if (const sar::SequencePiece* piece = SequenceAt(layout, starts, offset))
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
}

// Counts and prints the sequence files of `before` that the cut of `layout` leaves in the bytes.
void CheckCutLeftovers(const std::filesystem::path& path, const sar::SoundArchive& before, const sar::Layout& layout,
                       const std::map<std::size_t, std::size_t>& starts, Totals& totals)
{
    std::set<std::size_t> left;
    for (const sar::SoundInfo& sound : before.Sounds())
    {
        const std::optional<std::size_t> offset = before.FileOffset(sound.file_id);
        if (sound.type == sar::SoundType::kSequence && offset && !before.FileData(sound.file_id).empty() &&
            !SequenceAt(layout, starts, offset))
        {
            left.insert(*offset);
        }
    }

    totals.left += static_cast<int>(left.size());
    if (!left.empty())
    {
        std::printf("%s: %zu sequence files stay in the bytes\n", xsflib::Utf8(path.filename()).c_str(), left.size());
    }
}

// Checks that each sequence sound of `after`, the grown archive of `before`, starts at its item. An item past the first
// of a sequence moves by the items that `added` lists for the sequence.
void CheckSequenceStarts(const std::filesystem::path& path, const sar::SoundArchive& before,
                         const sar::SoundArchive& after, const sar::Layout& layout, const sar::Layout& grown,
                         const std::map<std::size_t, std::size_t>& starts, const std::vector<uint32_t>& added,
                         Totals& totals)
{
    for (std::size_t i = 0; i < before.Sounds().size(); ++i)
    {
        const sar::SoundInfo& sound = before.Sounds()[i];
        const sar::SequencePiece* piece = sound.type == sar::SoundType::kSequence
                                              ? SequenceAt(layout, starts, before.FileOffset(sound.file_id))
                                              : nullptr;
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
}

// Checks that the strict rules read `joined`, the grown archive, as `after` reads, where they read `original`.
void CheckStrictRules(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                      const std::vector<uint8_t>& joined, const sar::SoundArchive& after, const sar::Options& options,
                      Totals& totals)
{
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

} // namespace

void CheckTables(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                 const std::vector<uint8_t>& joined, const sar::Layout& layout, const sar::Layout& grown,
                 const std::vector<uint32_t>& added, const sar::Options& options, Totals& totals)
{
    const sar::SoundArchive before = sar::SoundArchive::Load(original, options);
    const sar::SoundArchive after = sar::SoundArchive::Load(joined, options);
    if (after.GroupErrors().size() != before.GroupErrors().size() || after.Truncated() != before.Truncated() ||
        after.FileCount() != before.FileCount() || after.Sounds().size() != before.Sounds().size())
    {
        Problem(path, "the grown archive's tables read otherwise", totals);
        return;
    }

    // Each file: a sequence's file as the writer writes the grown commands, or the same bytes.
    const std::map<std::size_t, std::size_t> starts = PieceStarts(layout);
    CheckFiles(path, before, after, layout, grown, starts, totals);

    // The sequence files that the cut leaves in the bytes. A change that breaks the cut can leave every one there.
    CheckCutLeftovers(path, before, layout, starts, totals);

    // Each sequence sound starts at its item.
    CheckSequenceStarts(path, before, after, layout, grown, starts, added, totals);

    // The strict rules, where they read the original.
    CheckStrictRules(path, original, joined, after, options, totals);
}

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

} // namespace hokuto::tools::layout
