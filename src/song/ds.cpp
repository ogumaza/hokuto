// SPDX-License-Identifier: MIT

#include "song/ds.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/ntr/game_profiles.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/sequence_export/ntr/convert.h"
#include "seqlib/file_name.h"
#include "seqlib/midi/encoder.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "song/midi_export.h"
#include "song/sequence.h"

namespace hokuto::song
{

namespace
{

// The places in a cartridge image's header of the ARM7 binary's offset and size.
constexpr std::size_t kArm7Offset = 0x30;
constexpr std::size_t kArm7Size = 0x3C;

// The commands of an audition's sequence (snd/commands.h).
constexpr uint8_t kWait = 0x80;
constexpr uint8_t kProgram = 0x81;
constexpr uint8_t kJump = 0x94;
constexpr uint8_t kIf = 0xA2;
constexpr uint8_t kCompareEqual = 0xB8;
constexpr uint8_t kPan = 0xC0;
constexpr uint8_t kVolume = 0xC1;
constexpr uint8_t kTie = 0xC8;
constexpr uint8_t kEnd = 0xFF;

// The bytecode of an audition's sequence (DsAuditionArchive). A program change stores its number in groups of 7 bits,
// the highest first, with bit 7 set in each byte but the last: a variable-length number of MIDI. A jump stores its
// target in 3 bytes, the lowest first. The note waits for its length of a tick.
std::vector<uint8_t> AuditionBytecode(int program, int key, int velocity)
{
    const auto byte = [](int value)
    {
        return static_cast<uint8_t>(std::clamp(value, 0, 127));
    };
    const auto put24 = [](std::vector<uint8_t>& bytes, uint32_t value)
    {
        for (int shift = 0; shift < 24; shift += 8)
        {
            bytes.push_back(static_cast<uint8_t>(value >> shift));
        }
    };

    std::vector<uint8_t> bytes{kProgram};
    const auto number = static_cast<uint32_t>(std::max(program, 0));
    seqlib::midi::AppendVlq(bytes, number);
    bytes.insert(bytes.end(), {kVolume, 127, kPan, 64, kTie, 1, byte(key), byte(velocity), 1});

    // Each tick: a wait, a comparison of the variable with 1, a jump to the end if it held, and a jump back. The wait,
    // the comparison and the two jumps take 15 bytes.
    const auto hold = static_cast<uint32_t>(bytes.size());
    bytes.insert(bytes.end(), {kWait, 1, kCompareEqual, static_cast<uint8_t>(kDsAuditionRelease), 1, 0, kIf, kJump});
    put24(bytes, hold + 15);
    bytes.push_back(kJump);
    put24(bytes, hold);
    bytes.push_back(kEnd);

    return bytes;
}

} // namespace

std::shared_ptr<const DsArchive> LoadDsArchive(std::vector<uint8_t> bytes, const ntdlib::ntr::snd::Build& build,
                                               std::optional<DsGame> game)
{
    // The archive points into the bytes. The bytes stay in place from here on.
    auto loaded = std::make_shared<DsArchive>();
    loaded->bytes = std::move(bytes);
    loaded->archive = ntdlib::sdat::Archive::Parse(loaded->bytes);
    loaded->build = build;
    loaded->game = std::move(game);

    const ntdlib::sdat::Archive& archive = loaded->archive;
    for (std::size_t i = 0; i < archive.sequences.size(); i++)
    {
        loaded->sounds.push_back(
            archive.sequences[i] ? std::optional(ntdlib::ntr::SequenceRef{static_cast<uint32_t>(i)}) : std::nullopt);
    }
    if (!archive.sequence_archives.empty() && loaded->sounds.size() < kFirstDsArchivedSound)
    {
        loaded->sounds.resize(kFirstDsArchivedSound);
    }

    for (std::size_t i = 0; i < archive.sequence_archives.size(); i++)
    {
        const std::optional<ntdlib::sdat::SequenceArchiveInfo>& info = archive.sequence_archives[i];
        if (!info || info->file_id >= archive.files.size())
        {
            continue;
        }

        try
        {
            const ntdlib::sdat::SequenceArchive parsed =
                ntdlib::sdat::SequenceArchive::Parse(archive.File(info->file_id));
            for (std::size_t j = 0; j < parsed.sequences.size(); j++)
            {
                const bool held = parsed.sequences[j].offset != kNoSsarSequence;
                loaded->sounds.push_back(
                    held ? std::optional(ntdlib::ntr::SequenceRef{static_cast<uint32_t>(i), static_cast<uint32_t>(j)})
                         : std::nullopt);
            }
        }
        catch (const xsflib::FormatError&)
        {
        }
    }

    return loaded;
}

ntdlib::ntr::snd::Build DsBuildOf(std::span<const uint8_t> image)
{
    if (image.size() < kArm7Size + 4)
    {
        return ntdlib::ntr::snd::kDefaultBuild;
    }

    const uint32_t offset = xsflib::LoadLe32(&image[kArm7Offset]);
    const uint32_t size = xsflib::LoadLe32(&image[kArm7Size]);
    if (offset > image.size() || size > image.size() - offset)
    {
        return ntdlib::ntr::snd::kDefaultBuild;
    }

    try
    {
        return ntdlib::ntr::snd::BuildOf(image.subspan(offset, size));
    }
    catch (const std::runtime_error&)
    {
        return ntdlib::ntr::snd::kDefaultBuild;
    }
}

std::optional<DsGame> DsGameOf(std::span<const uint8_t> image, uint32_t size, uint32_t file, uint32_t offset)
{
    const auto game_of = [file, offset](std::span<const uint8_t> whole) -> std::optional<DsGame>
    {
        const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(whole);
        if (file >= rom.Files().size())
        {
            return std::nullopt;
        }

        // The archives that start before the offset come before the archive in the file.
        const std::span<const uint8_t> data = rom.FileData(file);
        const std::size_t index =
            ntdlib::sdat::FindArchives(data.first(std::min<std::size_t>(offset, data.size()))).size();
        return DsGame{rom.Header().game_code, rom.Files()[file].path, static_cast<uint32_t>(index)};
    };

    try
    {
        return game_of(image);
    }
    catch (const xsflib::FormatError&)
    {
        if (image.size() >= size)
        {
            return std::nullopt;
        }
    }

    // The last file can end in 0xFF bytes that the image left out.
    std::vector<uint8_t> whole(size, 0xFF);
    std::ranges::copy(image, whole.begin());
    try
    {
        return game_of(whole);
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
}

DsProfile DsProfileOf(const DsArchive& archive, uint32_t sound)
{
    if (!archive.game || sound >= archive.sounds.size() || !archive.sounds[sound])
    {
        return {};
    }

    ntdlib::ntr::GameProfile profile = ntdlib::ntr::GameProfileOf(archive.game->code, archive.game->path,
                                                                  archive.game->index, DsSoundName(archive, sound));
    return {std::move(profile.settings), std::move(profile.commands)};
}

ntdlib::ntr::GameSettings DsGameSettingsOf(const DsArchive& archive)
{
    // No row of the profiles names a sequence by an empty label. The profile of that label contains only what the game
    // sets for every sequence of the archive.
    if (!archive.game)
    {
        return {};
    }

    return ntdlib::ntr::GameProfileOf(archive.game->code, archive.game->path, archive.game->index, "").settings;
}

DsProfile DryDsProfile(DsProfile profile)
{
    profile.settings.reverb.reset();
    profile.settings.effect.reset();

    return profile;
}

ntdlib::sequence_export::ntr::PerformOptions DsPerformOptions(const DsArchive& archive, uint32_t sound,
                                                              const std::map<int, int16_t>& values)
{
    DsProfile profile = DsProfileOf(archive, sound);
    ntdlib::sequence_export::ntr::PerformOptions options;
    options.build = archive.build;
    options.player_channels = std::move(profile.settings.channels);
    options.track_channels = std::move(profile.settings.track_channels);
    options.reverb = profile.settings.reverb;
    options.commands = std::move(profile.commands);

    // The performer numbers the player's variables 0 to 15, and the global variables 16 to 31.
    const ntdlib::ntr::Variables& variables = profile.settings.variables;
    for (std::size_t i = 0; i < variables.local.size(); i++)
    {
        if (variables.local[i])
        {
            options.variables[static_cast<uint8_t>(i)] = *variables.local[i];
        }
        if (variables.global[i])
        {
            options.variables[static_cast<uint8_t>(variables.local.size() + i)] = *variables.global[i];
        }
    }

    for (const auto& [number, value] : values)
    {
        if (number >= 0 && number < kGameVariables)
        {
            options.variables[static_cast<uint8_t>(number)] = value;
        }
    }

    return options;
}

std::vector<uint32_t> DsSounds(const DsArchive& archive)
{
    std::vector<uint32_t> sounds;
    for (std::size_t i = 0; i < archive.sounds.size(); i++)
    {
        if (archive.sounds[i])
        {
            sounds.push_back(static_cast<uint32_t>(i));
        }
    }

    return sounds;
}

std::vector<uint32_t> DsSoundsThatChange(const DsArchive& before, const DsArchive& after)
{
    const ntdlib::sdat::Archive& then = before.archive;
    const ntdlib::sdat::Archive& now = after.archive;
    std::map<uint32_t, bool> files;
    const auto same_file = [&](uint32_t file)
    {
        const auto [found, added] = files.try_emplace(file, false);
        if (added)
        {
            found->second = file < then.files.size() && file < now.files.size() &&
                            std::ranges::equal(then.File(file), now.File(file));
        }

        return found->second;
    };

    std::map<uint32_t, bool> banks;
    const auto same_bank = [&](uint32_t bank)
    {
        const auto [found, added] = banks.try_emplace(bank, false);
        if (!added)
        {
            return found->second;
        }

        if (bank >= then.banks.size() || bank >= now.banks.size() || !then.banks[bank] || !now.banks[bank] ||
            then.banks[bank]->file_id != now.banks[bank]->file_id || !same_file(then.banks[bank]->file_id) ||
            then.banks[bank]->wave_archives != now.banks[bank]->wave_archives)
        {
            return false;
        }

        for (const uint16_t wave_archive : then.banks[bank]->wave_archives)
        {
            if (wave_archive < then.wave_archives.size() && wave_archive < now.wave_archives.size() &&
                then.wave_archives[wave_archive] && now.wave_archives[wave_archive])
            {
                const uint32_t file = then.wave_archives[wave_archive]->file_id;
                if (file != now.wave_archives[wave_archive]->file_id || !same_file(file & ntdlib::sdat::kFileIdBits))
                {
                    return false;
                }
            }
        }

        found->second = true;
        return true;
    };

    std::map<uint32_t, std::pair<ntdlib::sdat::SequenceArchive, ntdlib::sdat::SequenceArchive>> parsed;
    std::vector<uint32_t> changed;
    for (const uint32_t sound : DsSounds(after))
    {
        if (sound >= before.sounds.size() || !before.sounds[sound])
        {
            continue;
        }

        const ntdlib::ntr::SequenceRef& ref = *after.sounds[sound];
        if (!ref.archived)
        {
            const ntdlib::sdat::SequenceInfo& old_info = *then.sequences.at(ref.index);
            const ntdlib::sdat::SequenceInfo& info = *now.sequences.at(ref.index);
            if (old_info.file_id != info.file_id || !same_file(info.file_id) || old_info.bank != info.bank ||
                old_info.channel_priority != info.channel_priority || old_info.player != info.player ||
                !same_bank(info.bank))
            {
                changed.push_back(sound);
            }

            continue;
        }

        auto found = parsed.find(ref.index);
        if (found == parsed.end())
        {
            found = parsed
                        .emplace(ref.index, std::pair(ntdlib::sdat::SequenceArchive::Parse(
                                                          then.File(then.sequence_archives.at(ref.index)->file_id)),
                                                      ntdlib::sdat::SequenceArchive::Parse(
                                                          now.File(now.sequence_archives.at(ref.index)->file_id))))
                        .first;
        }

        const auto& [old_file, file] = found->second;
        const ntdlib::sdat::ArchivedSequence& old_record = old_file.sequences.at(*ref.archived);
        const ntdlib::sdat::ArchivedSequence& record = file.sequences.at(*ref.archived);
        if (!std::ranges::equal(old_file.bytecode, file.bytecode) || old_record.offset != record.offset ||
            old_record.bank != record.bank || old_record.channel_priority != record.channel_priority ||
            old_record.player != record.player || !same_bank(record.bank))
        {
            changed.push_back(sound);
        }
    }

    return changed;
}

std::string DsSoundName(const DsArchive& archive, uint32_t sound)
{
    if (sound >= archive.sounds.size() || !archive.sounds[sound])
    {
        return "Sound " + std::to_string(sound);
    }

    return ntdlib::ntr::Label(archive.archive, *archive.sounds[sound]);
}

std::optional<uint32_t> DsBankOf(const DsArchive& archive, uint32_t sound)
{
    if (sound >= archive.sounds.size() || !archive.sounds[sound])
    {
        return std::nullopt;
    }

    // A sequence of a sequence archive names its bank in the archive's record of it.
    const ntdlib::ntr::SequenceRef& ref = *archive.sounds[sound];
    if (!ref.archived)
    {
        return archive.archive.sequences.at(ref.index)->bank;
    }

    const ntdlib::sdat::SequenceArchiveInfo& info = *archive.archive.sequence_archives.at(ref.index);
    return ntdlib::sdat::SequenceArchive::Parse(archive.archive.File(info.file_id)).sequences.at(*ref.archived).bank;
}

std::string DsBankName(const DsArchive& archive, uint32_t bank)
{
    const std::optional<ntdlib::sdat::BankInfo>& info =
        bank < archive.archive.banks.size() ? archive.archive.banks[bank] : std::nullopt;

    return info && !info->name.empty() ? info->name : "BANK_" + std::to_string(bank);
}

std::vector<uint32_t> DsBankPrograms(const DsArchive& archive, uint32_t bank)
{
    std::vector<uint32_t> programs;
    if (bank >= archive.archive.banks.size() || !archive.archive.banks[bank])
    {
        return programs;
    }

    try
    {
        const ntdlib::sdat::Bank parsed =
            ntdlib::sdat::Bank::Parse(archive.archive.File(archive.archive.banks[bank]->file_id));
        for (std::size_t program = 0; program < parsed.instruments.size(); program++)
        {
            if (parsed.instruments[program].type != ntdlib::sdat::InstrumentType::kNone)
            {
                programs.push_back(static_cast<uint32_t>(program));
            }
        }
    }
    catch (const xsflib::FormatError&)
    {
    }

    return programs;
}

std::shared_ptr<const DsArchive> DsAuditionArchive(const DsArchive& archive, uint32_t bank, int program, int key,
                                                   int velocity)
{
    const ntdlib::sdat::Archive& source = archive.archive;
    if (bank >= source.banks.size() || !source.banks[bank])
    {
        throw std::runtime_error("The archive has no " + DsBankName(archive, bank) + ".");
    }

    // The files: the sequence, the bank, then the wave archive of each slot that names one that the archive contains.
    // Bits 24 to 31 of a wave archive's file ID hold its flags, and they stay.
    const ntdlib::sdat::BankInfo& info = *source.banks[bank];
    ntdlib::sdat::ArchiveModel model;
    model.version = source.header.version;
    model.files.push_back(ntdlib::sdat::SequenceFile(AuditionBytecode(program, key, velocity)));
    const std::span<const uint8_t> bank_file = source.File(info.file_id);
    model.files.emplace_back(bank_file.begin(), bank_file.end());

    ntdlib::sdat::BankInfo copy{1, {}, ""};
    copy.wave_archives.fill(0xFFFF);
    for (std::size_t slot = 0; slot < info.wave_archives.size(); slot++)
    {
        const uint16_t index = info.wave_archives[slot];
        if (index >= source.wave_archives.size() || !source.wave_archives[index])
        {
            continue;
        }

        const ntdlib::sdat::WaveArchiveInfo& wave_archive = *source.wave_archives[index];
        const std::span<const uint8_t> file = source.File(wave_archive.file_id & ntdlib::sdat::kFileIdBits);
        copy.wave_archives[slot] = static_cast<uint16_t>(model.wave_archives.size());
        model.wave_archives.push_back(ntdlib::sdat::WaveArchiveInfo{
            (wave_archive.file_id & 0xFF000000) | static_cast<uint32_t>(model.files.size()), ""});
        model.files.emplace_back(file.begin(), file.end());
    }

    model.banks.push_back(copy);
    model.players.push_back(ntdlib::sdat::PlayerInfo{1, 0, 0, 0, ""});
    model.sequences.push_back(ntdlib::sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""});

    return LoadDsArchive(ntdlib::sdat::BuildArchive(model), archive.build, archive.game);
}

namespace
{

// The indexes of the entries of `table`.
template <typename Info>
std::vector<uint32_t> Entries(const std::vector<std::optional<Info>>& table)
{
    std::vector<uint32_t> indexes;
    for (std::size_t i = 0; i < table.size(); i++)
    {
        if (table[i])
        {
            indexes.push_back(static_cast<uint32_t>(i));
        }
    }

    return indexes;
}

// The rate at which the DS's channel plays a wave of `timer` at its root key, or `sample_rate` without a timer. From a
// rate rounded down, the rule of the tools gets the timer back: the clock over the rate, rounded down.
uint32_t TimerRate(uint16_t timer, uint16_t sample_rate)
{
    return timer != 0 ? ntdlib::sdat::kTimerClockHz / timer : sample_rate;
}

// The file of wave archive `wave_archive`. Throws std::runtime_error for one that is missing from the archive.
std::span<const uint8_t> WaveArchiveFile(const DsArchive& archive, uint32_t wave_archive)
{
    if (wave_archive >= archive.archive.wave_archives.size() || !archive.archive.wave_archives[wave_archive])
    {
        throw std::runtime_error("The archive has no " + DsWaveArchiveName(archive, wave_archive) + ".");
    }

    return archive.archive.File(archive.archive.wave_archives[wave_archive]->file_id & ntdlib::sdat::kFileIdBits);
}

// Wave `wave` of the wave archive `parsed`, named `name` in the error. Throws std::runtime_error for a wave past its
// waves.
const ntdlib::sdat::Wave& WaveOf(const ntdlib::sdat::WaveArchive& parsed, uint32_t wave, const std::string& name)
{
    if (wave >= parsed.waves.size())
    {
        throw std::runtime_error(name + " has no wave " + std::to_string(wave) + ".");
    }

    return parsed.waves[wave];
}

// The file of stream `stream`. Throws std::runtime_error for one that is missing from the archive.
std::span<const uint8_t> StreamFile(const DsArchive& archive, uint32_t stream)
{
    if (stream >= archive.archive.streams.size() || !archive.archive.streams[stream])
    {
        throw std::runtime_error("The archive has no " + DsStreamName(archive, stream) + ".");
    }

    return archive.archive.File(archive.archive.streams[stream]->file_id & ntdlib::sdat::kFileIdBits);
}

// A message for the user that names `name` and contains `what`, an item's error from ntdlib.
std::runtime_error Unreadable(const std::string& name, const std::exception& what)
{
    return std::runtime_error("Can't read " + name + ": " + what.what() + ".");
}

} // namespace

std::vector<uint32_t> DsBanks(const DsArchive& archive)
{
    return Entries(archive.archive.banks);
}

std::vector<uint32_t> DsWaveArchives(const DsArchive& archive)
{
    return Entries(archive.archive.wave_archives);
}

std::vector<uint32_t> DsStreams(const DsArchive& archive)
{
    return Entries(archive.archive.streams);
}

std::string DsWaveArchiveName(const DsArchive& archive, uint32_t wave_archive)
{
    const auto& table = archive.archive.wave_archives;
    return wave_archive < table.size() && table[wave_archive] && !table[wave_archive]->name.empty()
               ? table[wave_archive]->name
               : "WAVEARC_" + std::to_string(wave_archive);
}

std::string DsStreamName(const DsArchive& archive, uint32_t stream)
{
    const auto& table = archive.archive.streams;
    return stream < table.size() && table[stream] && !table[stream]->name.empty() ? table[stream]->name
                                                                                  : "STRM_" + std::to_string(stream);
}

std::size_t DsWaveCount(const DsArchive& archive, uint32_t wave_archive)
{
    try
    {
        return ntdlib::sdat::WaveArchive::Parse(WaveArchiveFile(archive, wave_archive)).waves.size();
    }
    catch (const std::exception&)
    {
        return 0;
    }
}

namespace
{

// The samples of `wave`, decoded to 16-bit PCM with its loop, at the rate of the channel's timer.
ntdlib::wave::Pcm PcmOfWave(const ntdlib::sdat::Wave& wave)
{
    ntdlib::wave::Pcm pcm;
    pcm.channels.push_back(ntdlib::sdat::DecodeWave(wave));
    pcm.sample_rate = TimerRate(wave.timer, wave.sample_rate);
    const auto length = static_cast<uint32_t>(pcm.channels[0].size());
    pcm.loop_end = length;
    if (wave.loop == 1 && wave.LoopStartSample() < length)
    {
        pcm.loop = true;
        pcm.loop_start = wave.LoopStartSample();
    }

    return pcm;
}

} // namespace

uint32_t DsWaveRate(const ntdlib::sdat::Wave& wave)
{
    const uint32_t played = TimerRate(wave.timer, wave.sample_rate);
    const uint32_t held = wave.sample_rate;
    return std::max(played, held) - std::min(played, held) <= played / 100 ? held : played;
}

ntdlib::wave::Pcm DsWavePcm(const DsArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    const std::string name = DsItemName(archive, {ItemKind::kWave, wave_archive, wave});
    try
    {
        const ntdlib::sdat::WaveArchive parsed =
            ntdlib::sdat::WaveArchive::Parse(WaveArchiveFile(archive, wave_archive));
        return PcmOfWave(WaveOf(parsed, wave, DsWaveArchiveName(archive, wave_archive)));
    }
    catch (const xsflib::FormatError& error)
    {
        throw Unreadable(name, error);
    }
}

ntdlib::wave::Pcm DsWavePcmOf(std::span<const uint8_t> bytes)
{
    return PcmOfWave(ntdlib::sdat::Wave::Parse(xsflib::Reader(bytes), 0, bytes.size()));
}

ntdlib::wave::Pcm DsStreamPcmOf(std::span<const uint8_t> bytes)
{
    // The timer of a stream is a 32nd of the timer of a wave: 16 at 32,728 Hz. Its field of the rate contains the rate.
    const ntdlib::sdat::Stream parsed = ntdlib::sdat::Stream::Parse(bytes);
    ntdlib::wave::Pcm pcm;
    for (uint32_t channel = 0; channel < parsed.channels; channel++)
    {
        pcm.channels.push_back(ntdlib::sdat::DecodeStream(parsed, channel));
    }

    pcm.sample_rate = parsed.sample_rate;
    const auto length = pcm.channels.empty() ? 0u : static_cast<uint32_t>(pcm.channels[0].size());
    pcm.loop_end = length;
    if (parsed.loop != 0 && parsed.loop_start < length)
    {
        pcm.loop = true;
        pcm.loop_start = parsed.loop_start;
    }

    return pcm;
}

ntdlib::wave::Pcm DsStreamPcm(const DsArchive& archive, uint32_t stream)
{
    const std::string name = DsStreamName(archive, stream);
    try
    {
        return DsStreamPcmOf(StreamFile(archive, stream));
    }
    catch (const xsflib::FormatError& error)
    {
        throw Unreadable(name, error);
    }
}

std::string DsItemName(const DsArchive& archive, const Item& item)
{
    switch (item.kind)
    {
    case ItemKind::kBank:
        return DsBankName(archive, item.index);

    case ItemKind::kWaveArchive:
        return DsWaveArchiveName(archive, item.index);

    case ItemKind::kWave:
        return DsWaveArchiveName(archive, item.index) + ", wave " + std::to_string(item.wave);

    case ItemKind::kStream:
        return DsStreamName(archive, item.index);

    default:
        return DsSoundName(archive, item.index);
    }
}

std::string DsItemFileName(const DsArchive& archive, const Item& item, std::size_t waves)
{
    switch (item.kind)
    {
    case ItemKind::kBank:
        return seqlib::SafeFileName(DsBankName(archive, item.index)) + ".sbnk";

    case ItemKind::kWaveArchive:
        return seqlib::SafeFileName(DsWaveArchiveName(archive, item.index)) + ".swar";

    // The index takes as many digits as the wave archive's last index.
    case ItemKind::kWave:
        {
            return seqlib::SafeFileName(DsWaveArchiveName(archive, item.index) + "_" + PaddedIndex(item.wave, waves)) +
                   ".swav";
        }

    case ItemKind::kStream:
        return seqlib::SafeFileName(DsStreamName(archive, item.index)) + ".strm";

    default:
        return seqlib::SafeFileName(DsSoundName(archive, item.index)) + ".sseq";
    }
}

std::vector<uint8_t> DsItemBytes(const DsArchive& archive, const Item& item)
{
    const std::string name = DsItemName(archive, item);
    try
    {
        std::span<const uint8_t> file;
        switch (item.kind)
        {
        case ItemKind::kSequence:
            if (item.index >= archive.archive.sequences.size() || !archive.archive.sequences[item.index])
            {
                throw std::runtime_error(name + " is in a sequence archive. Hokuto can't export it as a file yet.");
            }

            file = archive.archive.File(archive.archive.sequences[item.index]->file_id);
            break;

        case ItemKind::kBank:
            if (item.index >= archive.archive.banks.size() || !archive.archive.banks[item.index])
            {
                throw std::runtime_error("The archive has no " + name + ".");
            }

            file = archive.archive.File(archive.archive.banks[item.index]->file_id);
            break;

        case ItemKind::kWaveArchive:
            file = WaveArchiveFile(archive, item.index);
            break;

        case ItemKind::kWave:
            {
                const ntdlib::sdat::WaveArchive parsed =
                    ntdlib::sdat::WaveArchive::Parse(WaveArchiveFile(archive, item.index));
                return ntdlib::sdat::WaveFile(
                    ntdlib::sdat::WaveBytes(WaveOf(parsed, item.wave, DsWaveArchiveName(archive, item.index))));
            }

        case ItemKind::kStream:
            file = StreamFile(archive, item.index);
            break;

        default:
            throw std::runtime_error("Hokuto can't export " + name + " as a file.");
        }

        return {file.begin(), file.end()};
    }
    catch (const xsflib::FormatError& error)
    {
        throw Unreadable(name, error);
    }
}

ntdlib::sequence_export::BankConversion ConvertDsBank(const DsArchive& archive, uint32_t bank,
                                                      std::span<const uint32_t> programs)
{
    if (bank >= archive.archive.banks.size() || !archive.archive.banks[bank])
    {
        throw std::runtime_error("The archive has no " + DsBankName(archive, bank) + ".");
    }
    if (programs.empty())
    {
        throw BankWithoutInstruments(DsBankName(archive, bank) + " has no instruments.");
    }

    return ntdlib::sequence_export::ntr::ConvertBank(archive.archive, static_cast<uint16_t>(bank), programs,
                                                     archive.build, "Hokuto");
}

const ntdlib::sdat::Region* DsRegionAt(const ntdlib::sdat::Instrument& instrument, int key)
{
    using ntdlib::sdat::InstrumentType;
    switch (instrument.type)
    {
    case InstrumentType::kNone:
        return nullptr;

    case InstrumentType::kDrumSet:
        {
            if (key < instrument.min_key || key > instrument.max_key)
            {
                return nullptr;
            }

            const auto index = static_cast<std::size_t>(key - instrument.min_key);
            return index < instrument.regions.size() ? &instrument.regions[index] : nullptr;
        }

    case InstrumentType::kKeySplit:
        for (std::size_t i = 0; i < instrument.split_keys.size(); i++)
        {
            if (key <= instrument.split_keys[i])
            {
                return i < instrument.regions.size() ? &instrument.regions[i] : nullptr;
            }
        }

        return nullptr;

    default:
        return &instrument.region;
    }
}

} // namespace hokuto::song
