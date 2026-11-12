// SPDX-License-Identifier: MIT

// The checks of hokuto_check_layout on archives of the Wii.

#include "check_layout.h"

namespace hokuto::tools::layout
{

namespace
{

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

} // namespace

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

} // namespace hokuto::tools::layout
