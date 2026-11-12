// SPDX-License-Identifier: MIT

// Replace… of the banks, wave archives, wave sound files and waves of the 3DS, the Wii U and the Switch (edit/files.h).
// The Wii's are in wii_files.cpp, and the incoming waves in wav_waves.cpp.

#include "edit/files.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/bank_edit.h"
#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/wave_file.h"
#include "xsflib/binary.h"

#include "edit/bytecode.h"
#include "edit/files_internal.h"
#include "edit/files_parts.h"
#include "format/archive.h"
#include "format/kinds.h"
#include "song/archive.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
using files_parts::BanksNaming;
using files_parts::BytesOfCopy;
using files_parts::CopyBytes;
using files_parts::Names;
using files_parts::WaveSoundsPlaying;

// The letters after the console's letter in the magic of a file of `kind`.
std::string MagicOf(FileKind kind)
{
    switch (kind)
    {
    case FileKind::kBank:
        return "BNK";
    case FileKind::kWaveArchive:
        return "WAR";
    case FileKind::kWaveSoundFile:
        break;
    }

    return "WSD";
}

// The waves of each of the archive's wave archives, or nothing for one that ntdlib can't read.
std::vector<std::optional<std::size_t>> WaveCounts(const sar::SoundArchive& archive)
{
    std::vector<std::optional<std::size_t>> counts;
    for (const sar::WaveArchiveInfo& wave_archive : archive.WaveArchives())
    {
        std::optional<std::size_t>& count = counts.emplace_back();
        try
        {
            count = sar::WaveArchive::Parse(archive.FileData(wave_archive.file_id), archive.ReadOptions()).waves.size();
        }
        catch (const std::exception&)
        {
            // The archive lacks the file, or ntdlib can't read it.
        }
    }

    return counts;
}

// The waves of `waves` that the archive lacks, by wave archive in order. `counts` gives the waves of each of the
// archive's wave archives (WaveCounts). A wave that names an item of another kind counts as naming a wave archive that
// the archive lacks.
std::vector<LackedWaves> Lacked(std::span<const sar::WaveId> waves,
                                const std::vector<std::optional<std::size_t>>& counts)
{
    std::map<uint32_t, LackedWaves> lacked;
    for (const sar::WaveId& wave : waves)
    {
        const uint32_t index = sar::ItemNumber(wave.wave_archive_item);
        const bool known =
            sar::ItemType(wave.wave_archive_item) == sar::kWaveArchiveItem && index < counts.size() && counts[index];
        if (known && wave.index < *counts[index])
        {
            continue;
        }

        LackedWaves& entry = lacked[index];
        entry.wave_archive = index;
        entry.whole = entry.whole || !known;
        entry.count = known ? *counts[index] : 0;
        if (known)
        {
            entry.waves.push_back(wave.index);
        }
    }

    std::vector<LackedWaves> list;
    for (auto& [index, entry] : lacked)
    {
        std::ranges::sort(entry.waves);
        entry.waves.erase(std::ranges::unique(entry.waves).begin(), entry.waves.end());
        list.push_back(std::move(entry));
    }

    return list;
}

// The kind of item whose file is a file of `kind`.
song::ItemKind ItemKindOf(FileKind kind)
{
    switch (kind)
    {
    case FileKind::kBank:
        return song::ItemKind::kBank;
    case FileKind::kWaveArchive:
        return song::ItemKind::kWaveArchive;
    case FileKind::kWaveSoundFile:
        break;
    }

    return song::ItemKind::kWaveSound;
}

// Reads `bytes`, a file of `kind` on the console of `flavor`, by ntdlib's strict rules. Each wave of a wave archive has
// to read too. Throws std::invalid_argument with a message for the user when one doesn't.
void ReadStrictly(std::span<const uint8_t> bytes, FileKind kind, sar::Flavor flavor)
{
    const sar::Options options = StrictOptions(flavor);
    try
    {
        if (kind == FileKind::kBank)
        {
            sar::Bank::Parse(bytes, options);
        }
        else if (kind == FileKind::kWaveArchive)
        {
            for (const std::span<const uint8_t> wave : sar::WaveArchive::Parse(bytes, options).waves)
            {
                sar::Wave::Parse(wave, options);
            }
        }
        else
        {
            sar::WaveSoundFile::Parse(bytes, options);
        }
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the file. ") + format::AsSentence(error.what()));
    }
}

// The latest version among the waves of `file`, a wave archive of `flavor`, or nothing for a wave archive without
// waves. ntdlib has to read the file by the strict rules (ReadStrictly).
std::optional<uint32_t> LatestWaveVersion(std::span<const uint8_t> file, sar::Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    std::optional<uint32_t> latest;
    for (const std::span<const uint8_t> wave : sar::WaveArchive::Parse(file, options).waves)
    {
        if (wave.size() >= kHeaderSize)
        {
            const uint32_t version = xsflib::Reader(wave, flavor == sar::Flavor::kCafe).U32(kVersionField);
            latest = std::max(latest.value_or(0), version);
        }
    }

    return latest;
}

// `bytes`, a file of `kind` on the console of `from`, as a file of `to` with `versions`. A file of another console
// converts as Export… converts it (song::ConvertItemFile). A file of the same console keeps its byte order. A wave
// archive is written again with its waves. A bank file and a wave sound file take the version in their header, and
// their structures keep their places.
std::vector<uint8_t> WithVersions(std::span<const uint8_t> bytes, FileKind kind, sar::Flavor from, sar::Flavor to,
                                  const song::FileVersions& versions)
{
    if (from != to)
    {
        return song::ConvertItemFile(bytes, ItemKindOf(kind), from, to, versions);
    }
    if (kind == FileKind::kWaveArchive)
    {
        return sar::ConvertWaveArchive(sar::WaveArchiveFile::Read(bytes), to, versions.file, versions.waves).Write();
    }

    return sar::WithConsole(bytes, to, versions.file);
}

// The version of the first wave of `file`, a wave archive that ntdlib reads with `options`, or nothing for a wave
// archive without waves or one that ntdlib can't read.
std::optional<uint32_t> FirstWaveVersion(std::span<const uint8_t> file, const sar::Options& options)
{
    try
    {
        const std::vector<std::span<const uint8_t>> waves = sar::WaveArchive::Parse(file, options).waves;
        if (!waves.empty() && waves[0].size() >= kHeaderSize)
        {
            return xsflib::Reader(waves[0], options.flavor == sar::Flavor::kCafe).U32(kVersionField);
        }
    }
    catch (const std::exception&)
    {
        // Another wave archive may give the version.
    }

    return std::nullopt;
}

// A wave in a table of waves: its wave archive's item ID and its index.
using WaveKey = std::pair<uint32_t, uint32_t>;

// The places at which `copy`, a group's copy of `present`, names the waves of `present`'s table. `present` is a bank
// file or a wave sound file of `flavor`. A group's copy can name the same waves in a wave archive of the group's, at
// other indices. The map gives each wave of `present` its place in the copy. Nothing when the copy differs from
// `present` in other bytes than those of its table, when a wave of `present` has two places in the copy, or when ntdlib
// can't read either table.
std::optional<std::map<WaveKey, sar::WaveId>> PlacesInCopy(std::span<const uint8_t> present,
                                                           std::span<const uint8_t> copy, sar::Flavor flavor)
{
    try
    {
        const std::vector<sar::WaveId> waves = sar::WaveTable(present, flavor);
        const std::vector<sar::WaveId> places = sar::WaveTable(copy, flavor);
        if (waves.size() != places.size() || !std::ranges::equal(sar::WithWaveTable(present, flavor, places), copy))
        {
            return std::nullopt;
        }

        std::map<WaveKey, sar::WaveId> result;
        for (std::size_t i = 0; i < waves.size(); i++)
        {
            const auto [place, added] = result.try_emplace({waves[i].wave_archive_item, waves[i].index}, places[i]);
            if (!added && place->second != places[i])
            {
                return std::nullopt;
            }
        }

        return result;
    }
    catch (const std::exception&)
    {
        // A wave archive, or a file whose table ntdlib can't read.
        return std::nullopt;
    }
}

} // namespace

namespace files_parts
{

bool Names(const sar::WaveId& wave, uint32_t wave_archive)
{
    return sar::ItemType(wave.wave_archive_item) == sar::kWaveArchiveItem &&
           sar::ItemNumber(wave.wave_archive_item) == wave_archive;
}

std::vector<sar::WaveId> WavesOf(const sar::WaveSoundFile& file, uint32_t sound)
{
    std::vector<sar::WaveId> waves;
    if (sound >= file.sounds.size())
    {
        return waves;
    }

    for (const sar::VelocityRegion& note : file.sounds[sound].notes)
    {
        if (note.wave_id_index < file.waves.size())
        {
            waves.push_back(file.waves[note.wave_id_index]);
        }
    }

    return waves;
}

CopyBytes BytesOfCopy(std::span<const uint8_t> copy, std::span<const uint8_t> present,
                      std::span<const uint8_t> incoming, sar::Flavor flavor)
{
    CopyBytes result{.bytes = std::vector<uint8_t>(incoming.begin(), incoming.end())};
    if (std::ranges::equal(copy, present))
    {
        return result;
    }

    const std::optional<std::map<WaveKey, sar::WaveId>> places = PlacesInCopy(present, copy, flavor);
    std::vector<sar::WaveId> waves;
    try
    {
        waves = places ? sar::WaveTable(incoming, flavor) : std::vector<sar::WaveId>();
    }
    catch (const std::exception&)
    {
        // A new file without a table that ntdlib reads goes in as it is.
        return result;
    }

    if (!places)
    {
        return result;
    }

    for (sar::WaveId& wave : waves)
    {
        const auto place = places->find({wave.wave_archive_item, wave.index});
        if (place != places->end())
        {
            wave = place->second;
        }
        else
        {
            result.unplaced++;
        }
    }

    result.bytes = sar::WithWaveTable(incoming, flavor, waves);
    result.placed = true;

    return result;
}

} // namespace files_parts

std::string NameOf(FileKind kind)
{
    switch (kind)
    {
    case FileKind::kBank:
        return "bank";
    case FileKind::kWaveArchive:
        return "wave archive";
    case FileKind::kWaveSoundFile:
        break;
    }

    return "wave sound file";
}

IncomingFile ReadIncomingFile(std::span<const uint8_t> bytes, FileKind kind, sar::Flavor flavor,
                              const std::optional<song::FileVersions>& versions)
{
    // The magic gives the kind and the console's letter. The byte order mark tells the Wii U from the Switch.
    const std::string magic = MagicOf(kind);
    const bool big_endian = bytes.size() >= kHeaderSize && MarksBigEndian(bytes);
    const bool little_endian = bytes.size() >= kHeaderSize && MarksLittleEndian(bytes);
    if ((!big_endian && !little_endian) || (bytes[0] != 'C' && bytes[0] != 'F') ||
        !std::equal(magic.begin(), magic.end(), bytes.begin() + 1))
    {
        throw std::invalid_argument(format::NotKindText(bytes, "a " + FileText(kind)));
    }

    // The 3DS's files are little-endian. So are the Switch's.
    const sar::Flavor console = bytes[0] == 'C' ? sar::Flavor::kCtr
                                : big_endian    ? sar::Flavor::kCafe
                                                : sar::Flavor::kNx;
    if (console == sar::Flavor::kCtr && big_endian)
    {
        throw std::invalid_argument(format::NotKindText(bytes, "a " + FileText(kind)));
    }

    // The header's size.
    const xsflib::Reader r(bytes, big_endian);
    const uint32_t size = r.U32(kSizeField);
    if (size < kHeaderSize || size > bytes.size())
    {
        throw std::invalid_argument("Its header gives a size of " + std::to_string(size) + " bytes, but the file has " +
                                    std::to_string(bytes.size()) + " bytes.");
    }

    // A file of another console goes in converted, with the versions of the archive's files. With `versions`, so does a
    // file of the archive's console that holds a later version, or a wave archive with a wave of a later version. A
    // game may refuse a later version than its files hold.
    IncomingFile incoming{
        kind, r.U32(kVersionField), std::vector<uint8_t>(bytes.begin(), bytes.begin() + size), {}, {}};
    ReadStrictly(incoming.bytes, kind, console);
    const song::FileVersions chosen = versions.value_or(song::ExportVersions(ItemKindOf(kind), flavor));
    const bool later =
        versions && (incoming.version > chosen.file ||
                     (kind == FileKind::kWaveArchive && LatestWaveVersion(incoming.bytes, console) > chosen.waves));
    if (console != flavor || later)
    {
        ConvertedFile converted{console, incoming.version, incoming.bytes.size(), std::nullopt};
        if (kind == FileKind::kWaveArchive)
        {
            converted.wave_version = chosen.waves;
        }

        try
        {
            incoming.bytes = WithVersions(incoming.bytes, kind, console, flavor, chosen);
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument("Can't convert it for the " + song::ConsoleName(flavor) + ". " +
                                        format::AsSentence(error.what()));
        }

        incoming.version = chosen.file;
        incoming.converted = converted;
        ReadStrictly(incoming.bytes, kind, flavor);
    }

    return incoming;
}

song::FileVersions IncomingVersions(const sar::SoundArchive& archive, FileKind kind, std::optional<uint32_t> item)
{
    // The item's file, then the archive's files of the kind in the order of their items.
    std::vector<uint32_t> files;
    if (item)
    {
        try
        {
            files.push_back(FileIdOf(archive, kind, *item));
        }
        catch (const std::invalid_argument&)
        {
            // A new item takes the versions of the archive's files.
        }
    }

    if (kind == FileKind::kBank)
    {
        for (const sar::BankInfo& bank : archive.Banks())
        {
            files.push_back(bank.file_id);
        }
    }
    else if (kind == FileKind::kWaveArchive)
    {
        for (const sar::WaveArchiveInfo& wave_archive : archive.WaveArchives())
        {
            files.push_back(wave_archive.file_id);
        }
    }
    else
    {
        for (const sar::SoundInfo& sound : archive.Sounds())
        {
            if (sound.type == sar::SoundType::kWaveSound)
            {
                files.push_back(sound.file_id);
            }
        }
    }

    // The first file of the kind gives the file's version, and the first wave the waves' version.
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const std::string magic = MagicOf(kind);
    std::optional<uint32_t> file_version;
    std::optional<uint32_t> wave_version;
    for (const uint32_t file_id : files)
    {
        const std::span<const uint8_t> file = archive.FileData(file_id);
        if (file.size() < kHeaderSize || !std::equal(magic.begin(), magic.end(), file.begin() + 1))
        {
            continue;
        }

        if (!file_version)
        {
            file_version = xsflib::Reader(file, flavor == sar::Flavor::kCafe).U32(kVersionField);
        }

        if (kind == FileKind::kWaveArchive && !wave_version)
        {
            wave_version = FirstWaveVersion(file, archive.ReadOptions());
        }
    }

    // The versions of Export…'s files serve for the rest.
    song::FileVersions versions = song::ExportVersions(ItemKindOf(kind), flavor);
    versions.file = file_version.value_or(versions.file);
    versions.waves = wave_version.value_or(versions.waves);

    return versions;
}

uint32_t FileIdOf(const sar::SoundArchive& archive, FileKind kind, uint32_t item)
{
    switch (kind)
    {
    case FileKind::kBank:
        if (item < archive.Banks().size())
        {
            return archive.Banks()[item].file_id;
        }
        break;

    case FileKind::kWaveArchive:
        if (item < archive.WaveArchives().size())
        {
            return archive.WaveArchives()[item].file_id;
        }
        break;

    case FileKind::kWaveSoundFile:
        if (item < archive.Sounds().size() && archive.Sounds()[item].type == sar::SoundType::kWaveSound)
        {
            return archive.Sounds()[item].file_id;
        }
        break;
    }

    throw std::invalid_argument("The archive has no such " + NameOf(kind) + ".");
}

namespace
{

// Counts in `changes` the copies of file `file_id` of `file` in groups that contain other bytes than `present`, the
// current file, and how each accepts `incoming` (ChangesOf).
void CountGroupCopies(const format::File& file, uint32_t file_id, std::span<const uint8_t> present,
                      const IncomingFile& incoming, FileChanges& changes)
{
    const format::NativeArchive& native = *file.native;
    std::vector<sar::FileCopy> copies;
    try
    {
        copies = sar::FileCopies(FlavorOf(native), format::CopySequences(file), *native.pieces, native.fields,
                                 format::GroupFileLayouts(native), file_id);
    }
    catch (const std::exception&)
    {
        // The replacement can't read the archive's tables either, and says so.
    }

    for (const sar::FileCopy& copy : copies)
    {
        if (std::ranges::equal(copy.bytes, present))
        {
            continue;
        }

        const CopyBytes bytes = BytesOfCopy(copy.bytes, present, incoming.bytes, FlavorOf(native));
        changes.placed_copies += bytes.placed ? 1 : 0;
        changes.unplaced_waves = std::max(changes.unplaced_waves, bytes.unplaced);
        changes.other_copies += bytes.placed ? 0 : 1;
    }
}

// Stores in `changes` what `incoming`, a bank in the place of bank `item` of `archive` whose file is `present`,
// changes: the waves that it names and that are missing from the archive, by `counts`, the programs that it drops and
// the sounds that play the bank (ChangesOf).
void BankChanges(const sar::SoundArchive& archive, uint32_t item, const IncomingFile& incoming,
                 std::span<const uint8_t> present, const std::vector<std::optional<std::size_t>>& counts,
                 FileChanges& changes)
{
    const sar::Options& options = archive.ReadOptions();
    const sar::Bank bank = sar::Bank::Parse(incoming.bytes, options);
    changes.lacked = Lacked(bank.waves, counts);
    std::optional<sar::Bank> old;
    try
    {
        old = sar::Bank::Parse(present, options);
    }
    catch (const std::exception&)
    {
        // The present bank holds no programs that Hokuto can read.
    }

    for (std::size_t program = 0; old && program < old->instruments.size(); program++)
    {
        const bool held = program < bank.instruments.size() && bank.instruments[program].has_value();
        if (old->instruments[program] && !held)
        {
            changes.programs.push_back(static_cast<uint32_t>(program));
        }
    }

    const uint32_t id = sar::ItemId(sar::kBankItem, item);
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        if (std::ranges::find(archive.Sounds()[i].banks, id) != archive.Sounds()[i].banks.end())
        {
            changes.players.push_back(static_cast<uint32_t>(i));
        }
    }
}

// Stores in `changes` what `incoming`, a wave archive in the place of wave archive `item` of `archive`, changes: its
// count of waves, and the banks and wave sounds that name waves past it (ChangesOf).
void WaveArchiveChanges(const sar::SoundArchive& archive, uint32_t item, const IncomingFile& incoming,
                        FileChanges& changes)
{
    const sar::Options& options = archive.ReadOptions();
    changes.count = sar::WaveArchive::Parse(incoming.bytes, options).waves.size();
    const auto past = [&](const sar::WaveId& wave)
    {
        return Names(wave, item) && wave.index >= changes.count;
    };
    changes.past_banks = BanksNaming(archive, past);
    changes.past_sounds = WaveSoundsPlaying(archive, past);
}

// Stores in `changes` what `incoming`, a wave sound file in the place of file `file_id` of `archive`, changes: the
// waves that it names and that are missing from the archive, by `counts`, its count of sounds, and the wave sounds past
// it (ChangesOf).
void WaveSoundFileChanges(const sar::SoundArchive& archive, uint32_t file_id, const IncomingFile& incoming,
                          const std::vector<std::optional<std::size_t>>& counts, FileChanges& changes)
{
    const sar::Options& options = archive.ReadOptions();
    const sar::WaveSoundFile sounds = sar::WaveSoundFile::Parse(incoming.bytes, options);
    changes.lacked = Lacked(sounds.waves, counts);
    changes.count = sounds.sounds.size();
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = archive.Sounds()[i];
        if (sound.type == sar::SoundType::kWaveSound && sound.file_id == file_id &&
            sound.wave_sound_index >= sounds.sounds.size())
        {
            changes.past_sounds.push_back(static_cast<uint32_t>(i));
        }
    }
}

} // namespace

FileChanges ChangesOf(const format::File& file, const sar::SoundArchive& archive, uint32_t item,
                      const IncomingFile& incoming)
{
    // The present file, and the other items whose file it is.
    FileChanges changes;
    const uint32_t file_id = FileIdOf(archive, incoming.kind, item);
    const std::span<const uint8_t> present = archive.FileData(file_id);
    const sar::Options& options = archive.ReadOptions();
    changes.size = present.size();
    if (present.size() >= kHeaderSize)
    {
        changes.version = xsflib::Reader(present, options.flavor == sar::Flavor::kCafe).U32(kVersionField);
    }

    const auto share = [&](std::size_t count, const auto& file_of)
    {
        for (std::size_t i = 0; i < count; i++)
        {
            if (i != item && file_of(i))
            {
                changes.sharing.push_back(static_cast<uint32_t>(i));
            }
        }
    };
    if (incoming.kind == FileKind::kBank)
    {
        share(archive.Banks().size(), [&](std::size_t i) { return archive.Banks()[i].file_id == file_id; });
    }
    else if (incoming.kind == FileKind::kWaveArchive)
    {
        share(archive.WaveArchives().size(),
              [&](std::size_t i) { return archive.WaveArchives()[i].file_id == file_id; });
    }
    else
    {
        const auto same_file = [&](std::size_t i)
        {
            const sar::SoundInfo& sound = archive.Sounds()[i];
            return sound.type == sar::SoundType::kWaveSound && sound.file_id == file_id;
        };
        share(archive.Sounds().size(), same_file);
    }

    // The copies in groups that hold other bytes. Only a group's copy can differ from the present file. A copy that
    // names the present file's waves at other places takes the new file with those places.
    if (file.native)
    {
        CountGroupCopies(file, file_id, present, incoming, changes);
    }

    // What the new file names, and what names the new wave archive's waves.
    const std::vector<std::optional<std::size_t>> counts = WaveCounts(archive);
    switch (incoming.kind)
    {
    case FileKind::kBank:
        BankChanges(archive, item, incoming, present, counts, changes);
        break;

    case FileKind::kWaveArchive:
        WaveArchiveChanges(archive, item, incoming, changes);
        break;

    case FileKind::kWaveSoundFile:
        WaveSoundFileChanges(archive, file_id, incoming, counts, changes);
        break;
    }

    return changes;
}

std::vector<uint32_t> SoundsPlaying(const sar::SoundArchive& archive, FileKind kind, uint32_t item)
{
    // The banks that a sequence sound plays the item through, and the wave sounds that play it.
    std::vector<uint32_t> banks;
    std::vector<uint32_t> wave_sounds;
    if (kind == FileKind::kBank)
    {
        // Banks can share a file. The sounds of each of them play it.
        const uint32_t file_id = FileIdOf(archive, kind, item);
        for (std::size_t b = 0; b < archive.Banks().size(); b++)
        {
            if (archive.Banks()[b].file_id == file_id)
            {
                banks.push_back(static_cast<uint32_t>(b));
            }
        }
    }
    else if (kind == FileKind::kWaveArchive)
    {
        const auto named = [item](const sar::WaveId& wave)
        {
            return Names(wave, item);
        };
        banks = BanksNaming(archive, named);
        wave_sounds = WaveSoundsPlaying(archive, named);
    }
    else
    {
        const uint32_t file_id = FileIdOf(archive, kind, item);
        for (std::size_t i = 0; i < archive.Sounds().size(); i++)
        {
            const sar::SoundInfo& sound = archive.Sounds()[i];
            if (sound.type == sar::SoundType::kWaveSound && sound.file_id == file_id)
            {
                wave_sounds.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    std::vector<uint32_t> sounds;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = archive.Sounds()[i];
        const auto in_slot = [&banks](uint32_t slot)
        {
            return sar::ItemType(slot) == sar::kBankItem &&
                   std::ranges::find(banks, sar::ItemNumber(slot)) != banks.end();
        };
        const bool sequence = sound.type == sar::SoundType::kSequence && std::ranges::any_of(sound.banks, in_slot);
        if (sequence || std::ranges::find(wave_sounds, static_cast<uint32_t>(i)) != wave_sounds.end())
        {
            sounds.push_back(static_cast<uint32_t>(i));
        }
    }

    return sounds;
}

BankSelections ProgramsOfBank(const format::File& file, const sar::SoundArchive& archive,
                              std::span<const std::optional<SoundPlace>> places, uint32_t bank)
{
    // The slots that hold the bank or a bank of the same file.
    const uint32_t file_id = FileIdOf(archive, FileKind::kBank, bank);
    const auto same_file = [&archive, file_id](uint32_t slot)
    {
        const uint32_t index = sar::ItemNumber(slot);
        return sar::ItemType(slot) == sar::kBankItem && index < archive.Banks().size() &&
               archive.Banks()[index].file_id == file_id;
    };

    // The programs that each sound selects in those slots. Sounds that start at the same item of a sequence select the
    // same programs.
    std::map<std::pair<uint32_t, uint32_t>, Selections> selected;
    std::set<uint32_t> programs;
    BankSelections selections;
    for (std::size_t i = 0; i < archive.Sounds().size() && i < places.size(); i++)
    {
        const std::vector<uint32_t>& slots = archive.Sounds()[i].banks;
        if (!places[i] || std::ranges::none_of(slots, same_file))
        {
            continue;
        }

        const SoundPlace& place = *places[i];
        const uint32_t start = place.start_field ? file.native->fields.at(*place.start_field).to.offset : 0;
        const auto [found, added] = selected.try_emplace({place.sequence, start});
        if (added)
        {
            found->second = ProgramsSelected(SequenceToEdit(file, place.sequence), start);
        }

        for (const Program& program : found->second.programs)
        {
            const auto slot = static_cast<std::size_t>(program.slot);
            if (slot < slots.size() && same_file(slots[slot]))
            {
                programs.insert(static_cast<uint32_t>(program.number));
            }
        }

        selections.undecided += found->second.undecided;
    }

    selections.programs.assign(programs.begin(), programs.end());

    return selections;
}

std::optional<sar::Flavor> LaterConsoleOf(std::span<const uint8_t> bytes, FileKind kind)
{
    // The magic gives the kind and the console's letter. The byte order mark tells the Wii U from the Switch. The 3DS's
    // files are little-endian.
    const std::string magic = MagicOf(kind);
    const bool big_endian = bytes.size() >= kHeaderSize && MarksBigEndian(bytes);
    const bool little_endian = bytes.size() >= kHeaderSize && MarksLittleEndian(bytes);
    if ((!big_endian && !little_endian) || (bytes[0] != 'C' && bytes[0] != 'F') ||
        !std::equal(magic.begin(), magic.end(), bytes.begin() + 1))
    {
        return std::nullopt;
    }

    if (bytes[0] == 'C')
    {
        return little_endian ? std::optional(sar::Flavor::kCtr) : std::nullopt;
    }

    return big_endian ? sar::Flavor::kCafe : sar::Flavor::kNx;
}

std::vector<uint8_t> WithProgramFixes(const sar::SoundArchive& archive, uint32_t bank, std::span<const uint8_t> bytes,
                                      std::span<const ProgramFix> fixes)
{
    // The present bank's instrument comes from the archive's file, and another from the new bank.
    const std::span<const uint8_t> present = archive.FileData(FileIdOf(archive, FileKind::kBank, bank));
    std::vector<sar::InstrumentCopy> copies;
    for (const ProgramFix& fix : fixes)
    {
        copies.push_back({fix.from ? bytes : present, fix.from.value_or(fix.program), fix.program});
    }

    try
    {
        return sar::WithInstruments(bytes, archive.ReadOptions().flavor, copies);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't copy the instruments into the new bank. ") +
                                    format::AsSentence(error.what()));
    }
}

} // namespace hokuto::edit
