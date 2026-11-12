// SPDX-License-Identifier: MIT

// The writers of hok that write an archive's items as files, its waves as WAV files and its instruments as SoundFont 2
// or SFZ files, for each console and driver.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/ntr/rom_writer.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "seqlib/file_name.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/cli/tool.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "cli/hok_parts.h"
#include "edit/ds_files.h"
#include "edit/lengths.h"
#include "edit/other_console.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/codes.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/text.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/gba_assembly.h"
#include "song/gba_samples.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/played.h"
#include "song/streams.h"

namespace hokuto::cli::hok_parts
{

namespace
{

// The stream sounds of `archive` whose streams `input` holds, once for each stream file: the first sound that plays the
// file.
template <class Archive>
std::vector<uint32_t> StreamSounds(const ArchiveInput& input, const Archive& archive)
{
    std::vector<uint32_t> sounds;
    std::set<uint32_t> files;
    for (uint32_t sound = 0; sound < archive.Sounds().size(); sound++)
    {
        const std::optional<uint32_t> file = hokuto::song::StreamFileOf(archive, sound);
        if (file && input.streams.contains(*file) && files.insert(*file).second)
        {
            sounds.push_back(sound);
        }
    }

    return sounds;
}

// Writes `bytes` into `folder` as `name`, and counts it in `written`. Two items whose names differ only in case would
// share a file on macOS and Windows. The second one then takes `index` after its name: its index in the archive's
// table, or its wave archive's index for a wave.
void WriteItem(const std::filesystem::path& folder, std::string name, std::span<const uint8_t> bytes,
               std::set<std::string>& written, uint32_t index)
{
    if (written.contains(xsflib::AsciiLower(name)))
    {
        const std::size_t dot = name.rfind('.');
        name.insert(dot == std::string::npos ? name.size() : dot, "_" + std::to_string(index));
    }

    written.insert(xsflib::AsciiLower(name));
    WriteOutput(xsflib::Utf8(folder / xsflib::PathFromUtf8(name)), bytes);
}

// Writes items into one folder and counts the items that fail. Two items whose names differ only in case would share a
// file on macOS and Windows. The second one then gets an index after its name (WriteItem).
class ItemWriter
{
public:
    explicit ItemWriter(std::filesystem::path folder) : folder_(std::move(folder))
    {
    }

    // Writes `bytes` into the folder as `name`. `index` is added after the name when an earlier item has the name
    // already.
    void Write(const std::string& name, std::span<const uint8_t> bytes, uint32_t index)
    {
        WriteItem(folder_, name, bytes, written_, index);
    }

    // Runs `step`. An exception from it is reported and counts as an item that failed.
    template <typename Step>
    void Try(Step step)
    {
        try
        {
            step();
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed_++;
        }
    }

    std::size_t Failed() const
    {
        return failed_;
    }

private:
    std::filesystem::path folder_;
    std::set<std::string> written_;
    std::size_t failed_ = 0;
};

// The wave archives of `archive`, by their indexes.
std::vector<uint32_t> WaveArchiveIds(const ntdlib::sar::SoundArchive& archive)
{
    std::vector<uint32_t> ids(archive.WaveArchives().size());
    std::iota(ids.begin(), ids.end(), 0u);

    return ids;
}

// The wave archives of the Wii's `archive`, by the IDs of the files that give their wave data (song::WaveArchivesOf).
std::vector<uint32_t> WaveArchiveIds(const ntdlib::rsar::SoundArchive& archive)
{
    std::vector<uint32_t> ids;
    for (const hokuto::song::WiiWaveArchive& wave_archive : hokuto::song::WaveArchivesOf(archive))
    {
        ids.push_back(wave_archive.file_id);
    }

    return ids;
}

// Writes the items of `archive`, the archive of `input`, into `folder`: WriteItems for the 3DS, the Wii U and the
// Switch, and WriteWiiItems for the Wii. The Wii's items differ in three ways. A wave sound's item is its file. Wave
// data that a WAVE block describes goes out as an RWAR (song::ItemBytes). A wave archive is the wave data of a file.
template <class Archive>
std::size_t WriteArchiveItems(const ArchiveInput& input, const Archive& archive, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    constexpr bool kWii = std::is_same_v<Archive, ntdlib::rsar::SoundArchive>;
    constexpr song::ItemKind kWaveSoundKind = kWii ? song::ItemKind::kWaveSoundFile : song::ItemKind::kWaveSound;
    ItemWriter writer(folder);
    std::set<uint32_t> files;
    std::map<song::ItemKind, std::size_t> counts;
    const auto write = [&](const song::Item& item, std::size_t waves)
    {
        const auto step = [&]
        {
            if constexpr (kWii)
            {
                writer.Write(song::ItemFileName(archive, item, waves), song::ItemBytes(archive, item), item.index);
            }
            else
            {
                writer.Write(song::ItemFileName(archive, item, waves), song::ItemFile(archive, item), item.index);
            }

            counts[item.kind]++;
        };
        writer.Try(step);
    };

    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const auto& sound = archive.Sounds()[i];
        using SoundType = std::remove_cvref_t<decltype(sound.type)>;
        const bool sequence = sound.type == SoundType::kSequence;
        if ((sequence || sound.type == SoundType::kWaveSound) && files.insert(sound.file_id).second)
        {
            write({sequence ? song::ItemKind::kSequence : kWaveSoundKind, static_cast<uint32_t>(i), 0}, 0);
        }
    }

    for (std::size_t i = 0; i < archive.Banks().size(); i++)
    {
        write({song::ItemKind::kBank, static_cast<uint32_t>(i), 0}, 0);
    }

    for (const uint32_t wave_archive : WaveArchiveIds(archive))
    {
        write({song::ItemKind::kWaveArchive, wave_archive, 0}, 0);
        std::size_t waves = 0;
        writer.Try([&] { waves = song::WaveCountOf(archive, wave_archive); });
        for (std::size_t wave = 0; wave < waves; wave++)
        {
            write({song::ItemKind::kWave, wave_archive, static_cast<uint32_t>(wave)}, waves);
        }
    }

    for (const uint32_t sound : StreamSounds(input, archive))
    {
        const uint32_t file = archive.Sounds()[sound].file_id;
        writer.Write(song::StreamFileName(archive, sound), input.streams.at(file), sound);
        counts[song::ItemKind::kStream]++;
    }

    std::printf("Wrote %s, %s, %s, %s, %s and %s.\n",
                Counted(counts[song::ItemKind::kSequence], "sequence file", "sequence files").c_str(),
                Counted(counts[kWaveSoundKind], "wave sound file", "wave sound files").c_str(),
                Counted(counts[song::ItemKind::kBank], "bank", "banks").c_str(),
                Counted(counts[song::ItemKind::kWaveArchive], "wave archive", "wave archives").c_str(),
                Counted(counts[song::ItemKind::kWave], "wave", "waves").c_str(),
                Counted(counts[song::ItemKind::kStream], "stream", "streams").c_str());

    return writer.Failed();
}

// Writes each wave of `archive`, the archive of `input`, into `folder` as a WAV file, then each wave sound and each
// stream: WriteWavs for the 3DS, the Wii U and the Switch, and WriteWiiWavs for the Wii.
template <class Archive>
std::size_t WriteArchiveWavs(const ArchiveInput& input, const Archive& archive, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    constexpr bool kWii = std::is_same_v<Archive, ntdlib::rsar::SoundArchive>;
    ItemWriter writer(folder);
    std::size_t count = 0;
    for (const uint32_t wave_archive : WaveArchiveIds(archive))
    {
        const auto write_waves = [&]
        {
            const std::size_t waves = song::WaveCountOf(archive, wave_archive);
            for (std::size_t wave = 0; wave < waves; wave++)
            {
                const auto write_wave = [&]
                {
                    const auto number = static_cast<uint32_t>(wave);
                    writer.Write(song::WavFileName(archive, wave_archive, number, waves),
                                 song::WaveAsWav(archive, wave_archive, number), wave_archive);
                    count++;
                };
                writer.Try(write_wave);
            }
        };
        writer.Try(write_waves);
    }

    std::size_t wave_sounds = 0;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        using SoundType = std::remove_cvref_t<decltype(archive.Sounds()[i].type)>;
        if (archive.Sounds()[i].type != SoundType::kWaveSound)
        {
            continue;
        }

        const auto sound = static_cast<uint32_t>(i);
        const auto write_wave_sound = [&]
        {
            writer.Write(song::WaveSoundWavFileName(archive, sound), song::WaveSoundAsWav(archive, sound), sound);
            wave_sounds++;
        };
        writer.Try(write_wave_sound);
    }

    std::size_t streams = 0;
    for (const uint32_t sound : StreamSounds(input, archive))
    {
        const auto write_stream = [&]
        {
            std::string name = song::StreamFileName(archive, sound);
            name = name.substr(0, name.rfind('.')) + ".wav";
            const std::vector<uint8_t>& bytes = input.streams.at(archive.Sounds()[sound].file_id);
            if constexpr (kWii)
            {
                writer.Write(name, song::StreamAsWav(bytes), sound);
            }
            else
            {
                // An AAC stream takes the loop that the sound's entry gives.
                const song::AacStream aac = song::IsAdts(bytes)
                                                ? song::AacStreamOf(archive, sound).value_or(song::AacStream())
                                                : song::AacStream();
                writer.Write(name, song::StreamAsWav(bytes, aac), sound);
            }

            streams++;
        };
        writer.Try(write_stream);
    }

    std::printf("Wrote WAV files of %s, %s and %s.\n", Counted(count, "wave", "waves").c_str(),
                Counted(wave_sounds, "wave sound", "wave sounds").c_str(),
                Counted(streams, "stream", "streams").c_str());

    return writer.Failed();
}

} // namespace

// Writes the items of `input` into `folder`: a sequence file and a wave sound file once each, under the name of the
// first sound that plays it, then the banks, the wave archives and each wave, and last each stream under the name of
// its file. An item that can't be read is reported and left out. Returns the number of items left out.
std::size_t WriteItems(const ArchiveInput& input, const std::filesystem::path& folder)
{
    return WriteArchiveItems(input, *input.played.Sar(), folder);
}

// Writes each wave of `input` into `folder` as a WAV file, then each wave sound under the sound's name, and each stream
// under the name of its file. A wave, a wave sound or a stream that can't be decoded is reported and left out. Returns
// the number left out.
std::size_t WriteWavs(const ArchiveInput& input, const std::filesystem::path& folder)
{
    return WriteArchiveWavs(input, *input.played.Sar(), folder);
}

// Writes the items of the Wii's archive of `input` into `folder` as WriteItems writes those of the other consoles: a
// sequence file and a wave sound file once each, then the banks, then the wave data of each bank and wave sound file as
// an RWAR with each of its waves, and last each stream under the name of its file. Wave data that a WAVE block
// describes goes out as an RWAR of the same waves (song::ItemBytes). A wave sound file takes the browser's name,
// WSD_FILE_ and its file ID. Its RWAR takes the same name. File > Import > Wave Sound File… then finds the RWAR beside
// the file. Returns the number of items left out.
std::size_t WriteWiiItems(const ArchiveInput& input, const std::filesystem::path& folder)
{
    return WriteArchiveItems(input, *input.played.Wii(), folder);
}

// Writes each wave of each wave archive of the Wii's archive of `input` into `folder` as a WAV file: the waves of an
// RWAR, and those that a WAVE block describes. Each wave sound then goes out at the rate at which it plays its wave,
// and each stream under the name of its file. A wave, a wave sound or a stream that can't be decoded is reported and
// left out. Returns the number left out.
std::size_t WriteWiiWavs(const ArchiveInput& input, const std::filesystem::path& folder)
{
    return WriteArchiveWavs(input, *input.played.Wii(), folder);
}

namespace
{

// The indexes of the banks of `input` that WriteBanks converts.
std::vector<uint32_t> BankIndexes(const ArchiveInput& input)
{
    namespace song = hokuto::song;
    std::vector<uint32_t> indexes;
    if (input.played.Ds())
    {
        indexes = song::DsBanks(*input.played.Ds());
    }
    else if (input.played.Dse())
    {
        indexes = song::DseProgramBanks(*input.played.Dse());
    }
    else
    {
        indexes.resize(input.played.Wii() ? input.played.Wii()->Banks().size() : input.played.Sar()->Banks().size());
        std::iota(indexes.begin(), indexes.end(), 0u);
    }

    return indexes;
}

// The folder of the SFZ files of bank `index` of `input`, named `name`, in `folder`. `bank_ids` contains the first bank
// of each ID. Samples are named after the IDs of their banks. Two banks of DSE's files can share an ID, and the later
// one then gets a folder with its samples.
std::filesystem::path SfzFolder(const ArchiveInput& input, uint32_t index, const std::string& name,
                                const std::filesystem::path& folder,
                                std::map<uint16_t, std::shared_ptr<const ntdlib::dse::WaveBank>>& bank_ids)
{
    namespace song = hokuto::song;
    std::filesystem::path place = folder;
    if (input.played.Dse())
    {
        const std::shared_ptr<const ntdlib::dse::WaveBank>& found = input.played.Dse()->banks[index].bank;
        const auto [first, fresh] = bank_ids.try_emplace(song::DseBankId(*found), found);
        if (!fresh && first->second->Write() != found->Write())
        {
            place /= xsflib::PathFromUtf8(name);
        }
    }

    return place;
}

} // namespace

// Writes the instruments of each bank of `input` into `folder`: a SoundFont for each bank, or with `sfz`, a folder of
// SFZ files for each bank, with the samples in the folder samples. The banks share the samples. The files take the
// bank's name, with its index after it when the name differs only in case from an earlier bank's. A bank past the
// SoundFont format's limits takes several SoundFonts, named after their programs (song::SoundFontNames). A bank that
// can't be converted is reported and left out. A bank without instruments is reported too, and gives no file. What the
// converter reports of a bank goes to stderr too. A bank of the Wii plays by the rules of the game's build. Returns the
// number of banks that couldn't be converted.
std::size_t WriteBanks(const ArchiveInput& input, const std::filesystem::path& folder, bool sfz)
{
    namespace song = hokuto::song;
    std::set<std::string> written;
    std::map<uint16_t, std::shared_ptr<const ntdlib::dse::WaveBank>> bank_ids;
    std::size_t count = 0;
    std::size_t empty = 0;
    std::size_t failed = 0;
    for (const uint32_t index : BankIndexes(input))
    {
        try
        {
            const song::Item item = {song::ItemKind::kBank, index, 0};
            const ntdlib::sequence_export::BankConversion bank =
                input.played.Dse() ? song::ConvertDseBank(*input.played.Dse(), index,
                                                          song::DseBankPrograms(*input.played.Dse()->banks[index].bank))
                : input.played.Ds()
                    ? song::ConvertDsBank(*input.played.Ds(), index, song::DsBankPrograms(*input.played.Ds(), index))
                : input.played.Wii() ? song::ConvertBank(*input.played.Wii(), index, "Hokuto", input.build)
                                     : song::ConvertBank(*input.played.Sar(), index, "Hokuto");
            std::string name = input.played.Dse()   ? song::DseItemFileName(*input.played.Dse(), item)
                               : input.played.Ds()  ? song::DsItemFileName(*input.played.Ds(), item)
                               : input.played.Wii() ? song::ItemFileName(*input.played.Wii(), item)
                                                    : song::ItemFileName(*input.played.Sar(), item);
            name.erase(name.rfind('.'));
            if (!sfz)
            {
                const std::vector<std::string> names = song::SoundFontNames(bank, name);
                for (std::size_t k = 0; k < names.size(); k++)
                {
                    WriteItem(folder, names[k], bank.sound_fonts[k].sf2, written, index);
                }
            }
            else
            {
                if (written.contains(xsflib::AsciiLower(name)))
                {
                    name += "_" + std::to_string(index);
                }

                written.insert(xsflib::AsciiLower(name));
                const auto replace = [](const std::vector<std::string>&)
                {
                    return true;
                };

                const std::filesystem::path place = SfzFolder(input, index, name, folder, bank_ids);
                song::WriteBankSfz(bank, place, name, replace);
            }

            for (const std::string& warning : bank.warnings)
            {
                std::fprintf(stderr, "%s: %s\n", name.c_str(), warning.c_str());
            }

            count++;
        }
        catch (const song::BankWithoutInstruments& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            empty++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
    }

    std::printf("Converted %s.", Counted(count, "bank", "banks").c_str());
    if (empty > 0)
    {
        std::printf(" Skipped %s without instruments.", Counted(empty, "bank", "banks").c_str());
    }

    std::printf("\n");

    return failed;
}

// Writes the items of DSE's files of `input` into `folder`: the banks with programs, then each bank with samples and
// each of its waves, each under its name (song::DseItemFileName). An item that can't be read is reported and left out.
// Returns the number of items left out.
std::size_t WriteDseItems(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    const song::DseSet& set = *input.played.Dse();
    ItemWriter writer(folder);
    std::map<song::ItemKind, std::size_t> counts;
    const auto write = [&](const song::Item& item, std::size_t waves)
    {
        const auto step = [&]
        {
            writer.Write(song::DseItemFileName(set, item, waves), song::DseItemBytes(set, item), item.index);
            counts[item.kind]++;
        };
        writer.Try(step);
    };

    for (const uint32_t bank : song::DseProgramBanks(set))
    {
        write({.kind = song::ItemKind::kBank, .index = bank}, 0);
    }

    for (const uint32_t bank : song::DseSampleBanks(set))
    {
        write({.kind = song::ItemKind::kWaveArchive, .index = bank}, 0);
        for (const uint32_t wave : song::DseBankWaves(*set.banks[bank].bank))
        {
            write({.kind = song::ItemKind::kWave, .index = bank, .wave = wave}, set.banks[bank].bank->waves.size());
        }
    }

    std::printf("Wrote %s, %s and %s.\n", Counted(counts[song::ItemKind::kBank], "bank", "banks").c_str(),
                Counted(counts[song::ItemKind::kWaveArchive], "bank with samples", "banks with samples").c_str(),
                Counted(counts[song::ItemKind::kWave], "wave", "waves").c_str());

    return writer.Failed();
}

// Writes each wave of DSE's files of `input` into `folder` as a WAV file, under its name. A wave that can't be decoded
// is reported and left out. Returns the number left out.
std::size_t WriteDseWavs(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    const song::DseSet& set = *input.played.Dse();
    ItemWriter writer(folder);
    std::size_t waves_written = 0;
    for (const uint32_t bank : song::DseSampleBanks(set))
    {
        for (const uint32_t wave : song::DseBankWaves(*set.banks[bank].bank))
        {
            const song::Item item{.kind = song::ItemKind::kWave, .index = bank, .wave = wave};
            const auto step = [&]
            {
                writer.Write(song::DseItemFileName(set, item, set.banks[bank].bank->waves.size()),
                             song::DseItemBytes(set, item), bank);
                waves_written++;
            };
            writer.Try(step);
        }
    }

    std::printf("Wrote %s.\n", Counted(waves_written, "wave", "waves").c_str());

    return writer.Failed();
}

// Writes the items of the DS's archive of `input` into `folder`: the sequence files, the banks, then each wave archive
// with each of its waves, and last the streams, each under its name (song::DsItemFileName). An item that can't be read
// is reported and left out. Returns the number of items left out.
std::size_t WriteDsItems(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    const song::DsArchive& archive = *input.played.Ds();
    ItemWriter writer(folder);
    std::map<song::ItemKind, std::size_t> counts;
    const auto write = [&](const song::Item& item, std::size_t waves)
    {
        const auto step = [&]
        {
            writer.Write(song::DsItemFileName(archive, item, waves), song::DsItemBytes(archive, item), item.index);
            counts[item.kind]++;
        };
        writer.Try(step);
    };

    for (const uint32_t sound : song::DsSounds(archive))
    {
        // A sequence archive's sounds share its file.
        if (sound < archive.archive.sequences.size())
        {
            write({song::ItemKind::kSequence, sound, 0}, 0);
        }
    }

    for (const uint32_t bank : song::DsBanks(archive))
    {
        write({song::ItemKind::kBank, bank, 0}, 0);
    }

    for (const uint32_t wave_archive : song::DsWaveArchives(archive))
    {
        write({song::ItemKind::kWaveArchive, wave_archive, 0}, 0);
        const std::size_t waves = song::DsWaveCount(archive, wave_archive);
        for (std::size_t wave = 0; wave < waves; wave++)
        {
            write({song::ItemKind::kWave, wave_archive, static_cast<uint32_t>(wave)}, waves);
        }
    }

    for (const uint32_t stream : song::DsStreams(archive))
    {
        write({song::ItemKind::kStream, stream, 0}, 0);
    }

    std::printf("Wrote %s, %s, %s, %s and %s.\n",
                Counted(counts[song::ItemKind::kSequence], "sequence", "sequences").c_str(),
                Counted(counts[song::ItemKind::kBank], "bank", "banks").c_str(),
                Counted(counts[song::ItemKind::kWaveArchive], "wave archive", "wave archives").c_str(),
                Counted(counts[song::ItemKind::kWave], "wave", "waves").c_str(),
                Counted(counts[song::ItemKind::kStream], "stream", "streams").c_str());

    return writer.Failed();
}

// Writes each wave of the DS's archive of `input` into `folder` as a WAV file, then each stream, each under the name of
// its file with .wav. A wave or a stream that can't be decoded is reported and left out. Returns the number left out.
std::size_t WriteDsWavs(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    const song::DsArchive& archive = *input.played.Ds();
    ItemWriter writer(folder);
    std::size_t waves_written = 0;
    std::size_t streams = 0;
    const auto wav_name = [&archive](const song::Item& item, std::size_t waves)
    {
        std::string name = song::DsItemFileName(archive, item, waves);
        return name.substr(0, name.rfind('.')) + ".wav";
    };
    for (const uint32_t wave_archive : song::DsWaveArchives(archive))
    {
        const std::size_t waves = song::DsWaveCount(archive, wave_archive);
        for (std::size_t i = 0; i < waves; i++)
        {
            const song::Item item{song::ItemKind::kWave, wave_archive, static_cast<uint32_t>(i)};
            const auto step = [&]
            {
                writer.Write(
                    wav_name(item, waves),
                    song::PcmAsWav(song::DsWavePcm(archive, wave_archive, item.wave), song::DsItemName(archive, item)),
                    wave_archive);
                waves_written++;
            };
            writer.Try(step);
        }
    }

    for (const uint32_t stream : song::DsStreams(archive))
    {
        const song::Item item{song::ItemKind::kStream, stream, 0};
        const auto step = [&]
        {
            writer.Write(wav_name(item, 0),
                         song::PcmAsWav(song::DsStreamPcm(archive, stream), song::DsItemName(archive, item)), stream);
            streams++;
        };
        writer.Try(step);
    }

    std::printf("Wrote WAV files of %s and %s.\n", Counted(waves_written, "wave", "waves").c_str(),
                Counted(streams, "stream", "streams").c_str());

    return writer.Failed();
}

// Writes each song that plays of the GBA ROM of `input` and each voice group of its songs into `folder` as assembly,
// and each sample that the voice groups play as the ROM stores it. An item that can't go out is reported and left out.
// Returns the number of items left out.
std::size_t WriteGbaItems(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    const song::GbaRom& rom = *input.played.Gba();
    std::set<std::string> written;
    std::size_t failed = 0;
    const auto write = [&](const std::string& name, const auto& make, uint32_t index, std::size_t& count)
    {
        try
        {
            const auto bytes = make();
            WriteItem(folder, name, std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), written,
                      index);
            count++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
            failed++;
        }
    };

    std::size_t songs = 0;
    for (const int number : song::GbaSongs(rom))
    {
        const std::string name = song::GbaSongName(rom, number);
        const auto assembly = [&]
        {
            return song::GbaSongAssembly(rom, number, name);
        };
        write(seqlib::SafeFileName(name) + ".s", assembly, static_cast<uint32_t>(number), songs);
    }

    std::size_t groups = 0;
    for (const song::GbaVoiceGroup& group : song::GbaVoiceGroups(rom))
    {
        const auto assembly = [&]
        {
            return song::GbaVoiceGroupAssembly(rom, group);
        };
        write(seqlib::SafeFileName(song::GbaVoiceGroupName(group.address)) + ".s", assembly, group.address, groups);
    }

    std::size_t samples = 0;
    for (const uint32_t sample : song::GbaVoiceGroupSamples(rom))
    {
        write(song::GbaSampleName(sample) + ".bin", [&] { return song::GbaSampleBin(rom, sample); }, sample, samples);
    }

    std::printf("Wrote %s, %s and %s.\n", Counted(songs, "song", "songs").c_str(),
                Counted(groups, "voice group", "voice groups").c_str(), Counted(samples, "sample", "samples").c_str());

    return failed;
}

// Writes each sample that the voice groups of the GBA ROM of `input` play into `folder` as a WAV file. A sample that
// can't go out is reported and left out. Returns the number of samples left out.
std::size_t WriteGbaWavs(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    const song::GbaRom& rom = *input.played.Gba();
    std::set<std::string> written;
    std::size_t count = 0;
    std::size_t failed = 0;
    for (const uint32_t sample : song::GbaVoiceGroupSamples(rom))
    {
        const std::string name = song::GbaSampleName(sample) + ".wav";
        try
        {
            WriteItem(folder, name, song::GbaSampleWav(rom, sample), written, sample);
            count++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
            failed++;
        }
    }

    std::printf("Wrote %s.\n", Counted(count, "WAV file", "WAV files").c_str());

    return failed;
}

// Writes the voices that the songs of the GBA ROM of `input` select into `folder`: a SoundFont 2 file for each voice
// group that the songs play, under its name (song::GbaVoiceGroupName). A voice group whose voices play nothing is
// reported and left out. Returns the number of voice groups that failed.
std::size_t WriteVoiceGroups(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    std::set<std::string> written;
    std::size_t count = 0;
    std::size_t empty = 0;
    std::size_t failed = 0;
    for (const song::GbaVoiceGroup& group : song::GbaVoiceGroups(*input.played.Gba()))
    {
        const std::string name = seqlib::SafeFileName(song::GbaVoiceGroupName(group.address));
        try
        {
            const ntdlib::sequence_export::BankConversion conversion =
                song::ConvertVoiceGroup(*input.played.Gba(), group.address, group.programs, "Hokuto");
            WriteItem(folder, name + ".sf2", conversion.sound_fonts.at(0).sf2, written, group.address);
            for (const std::string& warning : conversion.warnings)
            {
                std::fprintf(stderr, "%s: %s\n", name.c_str(), warning.c_str());
            }

            count++;
        }
        catch (const song::BankWithoutInstruments& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            empty++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
            failed++;
        }
    }

    std::printf("Converted %s.", Counted(count, "voice group", "voice groups").c_str());
    if (empty > 0)
    {
        std::printf(" Skipped %s without a voice that makes a sound.",
                    Counted(empty, "voice group", "voice groups").c_str());
    }

    std::printf("\n");

    return failed;
}

// Writes each sample that the songs of the ROM of `input` play, with another of the GBA's drivers, into `folder` as a
// WAV file. A sample that can't be read is reported and left out. Returns how many failed.
std::size_t WriteDriverWavs(const ArchiveInput& input, const std::filesystem::path& folder)
{
    std::set<std::string> written;
    std::size_t count = 0;
    std::size_t failed = 0;
    for (const int sample : input.played.Driver()->Samples())
    {
        const std::string name = input.played.Driver()->SampleName(sample);
        try
        {
            WriteItem(folder, name + ".wav", input.played.Driver()->SampleWav(sample), written,
                      static_cast<uint32_t>(sample));
            count++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
            failed++;
        }
    }

    std::printf("Wrote %s.\n", Counted(count, "sample", "samples").c_str());

    return failed;
}

// Writes into `folder` a SoundFont of the samples that the songs of the ROM of `input` play, and one for each song with
// what the song plays. Each file has the name of its song. With `sfz`, each song gets a folder of SFZ files of that
// name instead, and the folders share the folder of samples. The samples' folder of SFZ files is "All Samples". A song
// that plays nothing has no file. Returns how many failed.
std::size_t WriteDriverSoundFonts(const ArchiveInput& input, const std::filesystem::path& folder, bool sfz)
{
    namespace song = hokuto::song;
    std::set<std::string> written;
    std::size_t count = 0;
    std::size_t silent = 0;
    std::size_t failed = 0;
    const auto replace = [](const std::vector<std::string>&)
    {
        return true;
    };
    try
    {
        const ntdlib::sequence_export::BankConversion samples =
            input.played.Driver()->ConvertSamples(input.played.Driver()->Samples(), "Hokuto");
        // The folder of samples is "samples", and file systems that ignore case would take "Samples" for it.
        if (sfz)
        {
            song::WriteBankSfz(samples, folder, "All Samples", replace);
        }
        else
        {
            WriteItem(folder, "Samples.sf2", samples.sound_fonts.at(0).sf2, written, 0);
        }

        count++;
    }
    catch (const song::BankWithoutInstruments& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Samples: %s\n", error.what());
        failed++;
    }

    for (const uint32_t sound : input.played.Driver()->Sounds())
    {
        const std::string name = seqlib::SafeFileName(input.played.Driver()->SoundName(sound));
        try
        {
            ntdlib::sequence_export::Conversion conversion = input.played.Driver()->ConvertToMidi(sound, "Hokuto");
            if (sfz)
            {
                ntdlib::sequence_export::BankConversion files;
                files.sfz = std::move(conversion.sfz);
                song::WriteBankSfz(files, folder, name, replace);
            }
            else
            {
                WriteItem(folder, name + ".sf2", conversion.sf2, written, sound);
            }

            count++;
        }
        catch (const song::EmptyConversion&)
        {
            silent++;
            continue;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
            failed++;
        }
    }

    std::printf("Wrote %s. Skipped %s.\n",
                sfz ? Counted(count, "folder of SFZ files", "folders of SFZ files").c_str()
                    : Counted(count, "SoundFont", "SoundFonts").c_str(),
                Counted(silent, "silent song", "silent songs").c_str());

    return failed;
}

// Writes each song that plays of the ROM of `input`, with another of the GBA's drivers, into `folder` as a MIDI file
// with its SoundFont, and each sample as a WAV file. A song that can't be converted is reported and left out. Returns
// how many failed.
std::size_t WriteDriverItems(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    std::set<std::string> written;
    std::size_t count = 0;
    std::size_t silent = 0;
    std::size_t failed = 0;
    for (const uint32_t sound : input.played.Driver()->Sounds())
    {
        const std::string name = seqlib::SafeFileName(input.played.Driver()->SoundName(sound));
        try
        {
            const ntdlib::sequence_export::Conversion conversion =
                input.played.Driver()->ConvertToMidi(sound, "Hokuto");
            WriteItem(folder, name + ".mid", conversion.midi, written, sound);
            WriteItem(folder, name + ".sf2", conversion.sf2, written, sound);
            count++;
        }
        catch (const song::EmptyConversion&)
        {
            silent++;
            continue;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
            failed++;
        }
    }

    std::printf("Wrote %s. Skipped %s.\n", Counted(count, "song", "songs").c_str(),
                Counted(silent, "silent song", "silent songs").c_str());

    return failed + WriteDriverWavs(input, folder);
}

} // namespace hokuto::cli::hok_parts
