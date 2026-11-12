// SPDX-License-Identifier: MIT

// The helpers that the checks of hokuto_check_layout share (check_layout.h).

#include "check_layout.h"

namespace hokuto::tools::layout
{

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

void Problem(const std::filesystem::path& path, const std::string& what, Totals& totals)
{
    std::printf("%s: %s\n", xsflib::Utf8(path.filename()).c_str(), what.c_str());
    ++totals.problems;
}

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

bool SameOtherwise(const sar::SoundInfo& a, const sar::SoundInfo& b)
{
    return a.name == b.name && a.file_id == b.file_id && a.type == b.type && a.player_id == b.player_id &&
           a.pan_mode == b.pan_mode && a.pan_curve == b.pan_curve && a.player_priority == b.player_priority &&
           a.actor_player_id == b.actor_player_id && a.banks == b.banks && a.start_offset == b.start_offset &&
           a.channel_priority == b.channel_priority && a.release_priority_fix == b.release_priority_fix &&
           a.wave_sound_index == b.wave_sound_index && a.stream == b.stream;
}

std::vector<uint8_t> Gain(std::size_t size)
{
    return std::vector<uint8_t>(size % 4 * 24 + 8, static_cast<uint8_t>(0xA0 + size % 16));
}

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

bool SameSound(const sar::SoundInfo& a, const sar::SoundInfo& b)
{
    return SameOtherwise(a, b) && a.volume == b.volume && a.allocate_track_flags == b.allocate_track_flags;
}

namespace
{

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

// Whether `y` holds the items of `x`, judged by `same`, and `more` items after them.
template <typename Items, typename Same>
bool Prefix(const Items& x, const Items& y, std::size_t more, const Same& same)
{
    return y.size() == x.size() + more && std::ranges::equal(x, std::span(y).first(x.size()), same);
}

} // namespace

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

bool Extends(const sar::SoundArchive& a, const sar::SoundArchive& b, const Growth& growth)
{
    return Prefix(a.Sounds(), b.Sounds(), growth.sounds, SameSound) && SameOtherItems(a, b, growth);
}

bool SameReading(const sar::SoundArchive& a, const sar::SoundArchive& b)
{
    return Extends(a, b, {});
}

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

std::size_t LeadOf(std::size_t block, sar::Flavor flavor)
{
    const std::size_t alignment = flavor == sar::Flavor::kNx ? 64 : 32;
    return (block + 8 + alignment - 1) / alignment * alignment - block;
}

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

} // namespace hokuto::tools::layout
