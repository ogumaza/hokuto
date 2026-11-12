// SPDX-License-Identifier: MIT

// The checks of hokuto_check_layout on new items: sounds, wave sounds, streams and banks.

#include "ntdlib/sar/archive_model.h"
#include "xsflib/bytes.h"

#include "check_layout.h"

namespace hokuto::tools::layout
{

namespace
{

// The versions of the 3DS whose stream sounds ntdlib lays out: CSAR 2.0.0.0, 2.2.0.0, 2.3.0.0, and 2.3.2.0 and later.
// The check names them itself. It doesn't take them from ntdlib's predicate, the code under test.
constexpr uint32_t kCsar2000 = 0x02000000;
constexpr uint32_t kCsar2200 = 0x02020000;
constexpr uint32_t kCsar2300 = 0x02030000;
constexpr uint32_t kCsar2320 = 0x02030200;

// Checks the names of `after`, the archive `original` of `flavor` with `what`: a string index that named an item in
// order still does, and FindItem finds each name.
void CheckNewNames(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                   const std::vector<uint8_t>& after, sar::Flavor flavor, const std::string& what, Totals& totals)
{
    if (NamesInOrder(original, flavor) && !NamesInOrder(after, flavor))
    {
        Problem(path, "a string index of the archive with " + what + " names another item", totals);
    }
    if (const std::optional<std::span<const uint8_t>> names = StringBlockOf(after, flavor))
    {
        for (const sar::ItemName& item : sar::StringBlock::Read(*names, flavor).names)
        {
            if (sar::FindItem(*names, flavor, item.name) != item.item_id)
            {
                Problem(path, "FindItem doesn't find " + item.name + " in the archive with " + what, totals);
            }
        }
    }
}

// Adds a copy of sound `model` of `before`, the archive `original` of `flavor`, to `added`: a wave sound that plays
// wave `index` of file `file`, with a name that the archive doesn't use. Adds the sound that the reader reads back to
// `expected`.
void AddWaveSound(const sar::SoundArchive& before, const std::vector<uint8_t>& original, sar::Flavor flavor,
                  uint32_t file, uint32_t model, uint32_t index, std::vector<sar::NewWaveSound>& added,
                  std::vector<sar::SoundInfo>& expected)
{
    std::string name = FreeName(original, flavor, "HOKUTO_NEW_WAVE_SOUND");
    name += name.empty() ? "" : std::to_string(added.size());
    added.push_back({.model = model, .name = name, .file = file, .index = index});
    sar::SoundInfo& sound = expected.emplace_back(before.Sounds()[model]);
    sound.name = name.empty() ? "WSD_" + std::to_string(before.Sounds().size() + expected.size() - 1) : name;
    sound.file_id = file;
    sound.wave_sound_index = index;
    if (sound.type == sar::SoundType::kSequence)
    {
        const sar::SoundInfo kPlain;
        sound.type = sar::SoundType::kWaveSound;
        sound.banks = kPlain.banks;
        sound.allocate_track_flags = kPlain.allocate_track_flags;
        sound.start_offset = kPlain.start_offset;
    }
}

} // namespace

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
    CheckNewNames(path, original, after, options.flavor, "a new sound", totals);

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
    for (const uint32_t model : models)
    {
        AddWaveSound(before, original, options.flavor, with.file, model, before.Sounds()[model].wave_sound_index, added,
                     expected);
    }
    if (sequence)
    {
        AddWaveSound(before, original, options.flavor, with.file, *sequence, 0, added, expected);
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
    CheckNewNames(path, original, after, options.flavor, "new wave sounds", totals);

    // The cut gives the same sequences.
    if (sar::Cut(after, options).sequences != layout.sequences)
    {
        Problem(path, "the cut of the archive with new wave sounds gives other sequences", totals);
    }
    totals.new_wave_sounds += static_cast<int>(added.size());
    totals.from_sequences += sequence ? 1 : 0;
    ++totals.wave_sound_archives;
}

namespace
{

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
    if (flavor == sar::Flavor::kCtr && version == sar::kCtrTracksVersion)
    {
        return tracks >= 16 && details + 16 <= entry.size() && r.U32(details + 12) != 0;
    }

    const auto names = [&](std::size_t at)
    {
        return tracks >= at + 4 && details + at + 4 <= entry.size() && r.U32(details + at) != 0xFFFFFFFF;
    };
    return names(28) || names(32);
}

// `entry`, the entry of a stream sound with extras (StreamExtras), as a copy of the sound has to hold it: without the
// reference to the extension and without the prefetch file. In a 3DS archive of CSAR 2.3.0.0, the field of options
// after the reference to the table of tracks contains 0. The values of the options are left out, and the table of
// tracks follows the field.
std::vector<uint8_t> WithoutStreamExtras(std::span<const uint8_t> entry, sar::Flavor flavor, uint32_t version)
{
    const bool big_endian = flavor == sar::Flavor::kCafe;
    std::vector<uint8_t> copy(entry.begin(), entry.end());
    const auto store = [&copy, big_endian](std::size_t at, uint32_t value)
    {
        if (big_endian)
        {
            xsflib::StoreBe32(copy.data() + at, value);
        }
        else
        {
            xsflib::StoreLe32(copy.data() + at, value);
        }
    };

    const xsflib::Reader r(entry, big_endian);
    const std::size_t details = r.U32(16);
    const std::size_t tracks = r.U32(details + 8);
    if (flavor == sar::Flavor::kCtr && version == sar::kCtrTracksVersion)
    {
        if (tracks >= 16 && details + tracks <= copy.size())
        {
            copy.erase(copy.begin() + static_cast<std::ptrdiff_t>(details + 16),
                       copy.begin() + static_cast<std::ptrdiff_t>(details + tracks));
            store(details + 8, 16);
        }
        store(details + 12, 0);
        return copy;
    }
    if (tracks >= 32 && details + 32 <= copy.size())
    {
        // The extension holds the stream's type, and its loop's start and end. Where it ends the entry, the copy ends
        // before it.
        constexpr std::size_t kExtensionSize = 12;
        const uint32_t extension = r.U32(details + 28);
        if (extension != 0xFFFFFFFF && details + extension + kExtensionSize == copy.size())
        {
            copy.resize(details + extension);
        }
        store(details + 24, 0);
        store(details + 28, 0xFFFFFFFF);
    }
    if (tracks >= 36 && details + 36 <= copy.size())
    {
        store(details + 32, 0xFFFFFFFF);
    }

    return copy;
}

} // namespace

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
    // sounds of the versions of the 3DS that kCsar2000 to kCsar2320 name.
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
        !ctr || version == kCsar2000 || version == kCsar2200 || version == kCsar2300 || version >= kCsar2320;
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
        if (entry != model && StreamExtras(model, options.flavor, version) &&
            entry == WithoutStreamExtras(model, options.flavor, version))
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

namespace
{

// The bank of `before` that a check copies, and the wave archives of its waves in the order of their first use: the
// first bank whose waves are all in wave archives with files. Nothing when no bank does.
std::optional<std::pair<uint32_t, std::vector<uint32_t>>> ModelBank(const sar::SoundArchive& before,
                                                                    const sar::Options& options)
{
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
        return std::nullopt;
    }

    return std::pair(*model, named);
}

// The wave archives of `before`, the archive `original`, that `named` lists, with a copy of each after them: the
// archive with the copies, the map from each old item ID to the ID of its copy, and the names of the copies.
struct WaveArchiveCopies
{
    std::vector<uint8_t> archive;
    std::map<uint32_t, uint32_t> map;
    std::vector<std::string> names;
};

WaveArchiveCopies CopyWaveArchives(const sar::SoundArchive& before, const std::vector<uint8_t>& original,
                                   std::span<const uint32_t> named, sar::Flavor flavor)
{
    std::vector<uint8_t> archive = original;
    std::map<uint32_t, uint32_t> map;
    std::vector<std::string> names;
    for (const uint32_t id : named)
    {
        const sar::WaveArchiveInfo& old = before.WaveArchives()[id & 0x00FFFFFF];
        const sar::ArchiveWithFile with = sar::WithNewFile(archive, flavor, before.FileData(old.file_id));
        names.push_back(FreeName(with.archive, flavor, "HOKUTO_NEW_WARC"));
        archive = sar::WithNewWaveArchive(with.archive, flavor, {.name = names.back(), .file = with.file});
        map[id] = sar::ItemId(sar::kWaveArchiveItem, static_cast<uint32_t>(before.WaveArchives().size() + map.size()));
    }

    return {std::move(archive), std::move(map), std::move(names)};
}

// The item IDs of the wave archives that `entry`, a bank's entry in an archive of `flavor`, lists.
std::vector<uint32_t> BankListOf(std::span<const uint8_t> entry, sar::Flavor flavor)
{
    const xsflib::Reader r(entry, flavor == sar::Flavor::kCafe);
    const std::size_t list = r.U32(8);
    std::vector<uint32_t> listed;
    for (uint32_t k = 0; k < r.U32(list); ++k)
    {
        listed.push_back(r.U32(list + 4 + std::size_t{k} * 4));
    }

    return listed;
}

// The item IDs that the entry of the last bank of `after`, an archive of `flavor`, lists.
std::vector<uint32_t> LastBankList(const std::vector<uint8_t>& after, sar::Flavor flavor)
{
    const sar::InfoBlock new_info = sar::InfoBlock::Read(sar::TablesOf(after, flavor).info, flavor);

    return BankListOf(new_info.banks.back(), flavor);
}

} // namespace

void CheckNewBank(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                  const sar::Options& options, Totals& totals)
{
    const sar::SoundArchive before = sar::SoundArchive::Load(original, options);
    if (before.Truncated())
    {
        return;
    }

    // The model, and the wave archives of its waves in the order of their first use.
    const std::optional<std::pair<uint32_t, std::vector<uint32_t>>> found = ModelBank(before, options);
    if (!found)
    {
        ++totals.no_bank;
        return;
    }
    const uint32_t model = found->first;
    const std::vector<uint32_t>& named = found->second;

    // A copy of each wave archive, and the map from the old to the new.
    const auto [archive, map, names] = CopyWaveArchives(before, original, named, options.flavor);

    // The bank's file with the copies, and the new bank. Its entry lists the copies where the model's lists its wave
    // archives.
    const sar::InfoBlock info = sar::InfoBlock::Read(sar::TablesOf(original, options.flavor).info, options.flavor);
    std::vector<uint32_t> listed;
    for (const uint32_t id : BankListOf(info.banks[model], options.flavor))
    {
        listed.push_back(map.contains(id) ? map.at(id) : id);
    }
    const std::span<const uint8_t> file = before.FileData(before.Banks()[model].file_id);
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

    // The new bank's entry lists the copies, read as the model's list is read above.
    const std::vector<uint32_t> new_listed = LastBankList(after, options.flavor);
    if (new_listed != listed)
    {
        Problem(path, "the new bank's entry doesn't list the copies of the wave archives", totals);
    }

    // The names, and the cut.
    CheckNewNames(path, original, after, options.flavor, "a new bank", totals);
    if (sar::Cut(after, options).sequences != layout.sequences)
    {
        Problem(path, "the cut of the archive with a new bank gives other sequences", totals);
    }
    ++totals.new_banks;
    totals.new_wave_archives += static_cast<int>(named.size());
    totals.listed += listed.empty() ? 0 : 1;
}

} // namespace hokuto::tools::layout
