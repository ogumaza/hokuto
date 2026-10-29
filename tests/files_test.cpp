// SPDX-License-Identifier: MIT

// Tests the replacement of an archive's banks, wave archives and wave sound files (src/edit/files.h). A small archive
// of the Switch, and the same archive of the Wii U, hold a sequence sound that has the bank in slot 0, two wave sounds
// of one wave sound file, the bank with programs 0 and 2, a second bank of the same file, and two wave archives. The
// first wave archive holds two waves and the second one. The bank plays both waves of the first. The first wave sound
// plays the first wave archive's second wave, and the second wave sound the second wave archive's wave. A group holds a
// copy of the bank with other bytes, and a copy of the first wave archive. In a second archive, the group's copies of
// the bank and the wave sound file name their files' waves at other places. The checks read the exported archive with
// ntdlib. Files of the other console go in converted. A WAV file that seqlib writes takes the place of a wave of the
// first wave archive, as 16-bit PCM and as DSP-ADPCM, and at a lower rate. Hokuto then decodes the wave from the
// exported archive. Small archives of the Wii take files of the Wii in the place of their banks, wave data and waves.
// Files of the 3DS, the Wii U and the Switch go into them converted.

#include "edit/files.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/layout.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/rsar_tools.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/wave/adpcm.h"
#include "seqlib/pcm.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/preview.h"

using hokuto::edit::FileChanges;
using hokuto::edit::FileKind;
using hokuto::edit::IncomingFile;
using hokuto::edit::IncomingWave;
using hokuto::edit::LackedWaves;
using hokuto::edit::ProgramFix;
using hokuto::edit::WaveChanges;
using hokuto::format::File;
using hokuto::song::FileVersions;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The archive's files, sounds, banks and wave archives.
constexpr uint32_t kSequenceFile = 0;
constexpr uint32_t kBankFile = 1;
constexpr uint32_t kFirstWavesFile = 2;
constexpr uint32_t kSecondWavesFile = 3;
constexpr uint32_t kWaveSoundsFile = 4;
constexpr uint32_t kOutsideFile = 5;
constexpr uint32_t kGroupFile = 6;
constexpr uint32_t kSong = 0;
constexpr uint32_t kFirstWaveSound = 1;
constexpr uint32_t kSecondWaveSound = 2;
constexpr uint32_t kBank = 0;
constexpr uint32_t kFirstWaves = 0;
constexpr uint32_t kSecondWaves = 1;

// The item IDs of the wave archives, and of one that the archive lacks.
constexpr uint32_t kFirstWavesId = 0x05000000;
constexpr uint32_t kSecondWavesId = 0x05000001;
constexpr uint32_t kLackedWavesId = 0x05000002;

// A short wave of the console's PCM16 samples.
std::vector<uint8_t> Wave(sar::Flavor flavor, uint8_t value)
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop_end = 4;
    wave.channels = {{value, value, value, value, value, value, value, value}};

    return sar_files::Wave(flavor, wave);
}

// An instrument whose one region plays wave `wave` of its bank.
sar_files::KeyTable Instrument(uint32_t wave)
{
    sar_files::Region region;
    region.wave = wave;
    const sar_files::VelocityTable velocities{sar_files::VelocityTable::kDirect, 0, {{0, region}}};
    return sar_files::KeyTable{sar_files::KeyTable::kDirect, 0, {{0, velocities}}};
}

// A bank that names `waves` and has an instrument at each of `programs`. The instruments play the bank's waves in turn.
std::vector<uint8_t> Bank(sar::Flavor flavor, const std::vector<std::pair<uint32_t, uint32_t>>& waves,
                          const std::vector<uint32_t>& programs)
{
    sar_files::BankSpec bank;
    bank.waves = waves;
    for (std::size_t i = 0; i < programs.size(); i++)
    {
        bank.instruments.resize(std::max<std::size_t>(bank.instruments.size(), programs[i] + 1));
        bank.instruments[programs[i]] = Instrument(static_cast<uint32_t>(i % waves.size()));
    }

    return sar_files::Bank(flavor, bank);
}

// A wave sound file that names `waves`, with a wave sound for each of `plays`: the wave that its one note plays.
std::vector<uint8_t> WaveSounds(sar::Flavor flavor, const std::vector<std::pair<uint32_t, uint32_t>>& waves,
                                const std::vector<uint32_t>& plays)
{
    sar_files::WaveSoundFileSpec file;
    file.waves = waves;
    for (const uint32_t wave : plays)
    {
        sar_files::WaveSoundSpec sound;
        sound.tracks = {{{0.0f, 0.0f, 0}}};
        sar_files::Region note;
        note.wave = wave;
        sound.notes = {note};
        file.sounds.push_back(sound);
    }

    return sar_files::WaveSoundFile(flavor, file);
}

// The song's bytecode: program 0 of the bank in slot 0, and a note.
const std::vector<uint8_t> kSongBytecode = {0xB0, 48, 0x81, 0, 60, 100, 24, 0xFF};

// The test archive. The song plays `song_bytecode` with the banks of `song_banks` in its slots. With `placed`, the
// group's copy of the bank names the bank's waves at other places: the first wave archive's second wave, and the second
// wave archive's wave. The group then also holds a copy of the wave sound file that names the file's two waves in each
// other's places.
std::vector<uint8_t> TestArchive(sar::Flavor flavor, const std::vector<uint8_t>& song_bytecode = kSongBytecode,
                                 const std::vector<uint32_t>& song_banks = {0}, bool placed = false)
{
    sar_files::SoundSpec song;
    song.name = "SEQ_SONG";
    song.banks = song_banks;
    sar_files::SoundSpec first;
    first.name = "WSD_FIRST";
    first.kind = sar_files::SoundSpec::kWaveSound;
    first.file = kWaveSoundsFile;
    sar_files::SoundSpec second = first;
    second.name = "WSD_SECOND";
    second.wave_sound_index = 1;

    sar_files::ArchiveSpec spec;
    spec.sounds = {song, first, second};
    spec.banks = {{"BANK_TEST", kBankFile}, {"BANK_TWIN", kBankFile}};
    spec.wave_archives = {{"WARC_FIRST", kFirstWavesFile}, {"WARC_SECOND", kSecondWavesFile}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.groups = {{"GROUP_TEST", kGroupFile}};
    const std::vector<uint8_t> waves = sar_files::WaveArchive(flavor, {Wave(flavor, 1), Wave(flavor, 2)});
    const std::vector<uint8_t> group_bank = placed ? Bank(flavor, {{kFirstWavesId, 1}, {kSecondWavesId, 0}}, {0, 2})
                                                   : Bank(flavor, {{kSecondWavesId, 0}}, {0, 2});
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> group = {{kBankFile, group_bank}, {kFirstWavesFile, waves}};
    if (placed)
    {
        group.emplace_back(kWaveSoundsFile, WaveSounds(flavor, {{kSecondWavesId, 0}, {kFirstWavesId, 1}}, {0, 1}));
    }
    spec.files = {{.bytes = sar_files::Sequence(flavor, song_bytecode, {{"Song", 0}})},
                  {.bytes = Bank(flavor, {{kFirstWavesId, 0}, {kFirstWavesId, 1}}, {0, 2})},
                  {.bytes = waves},
                  {.bytes = sar_files::WaveArchive(flavor, {Wave(flavor, 3)})},
                  {.bytes = WaveSounds(flavor, {{kFirstWavesId, 1}, {kSecondWavesId, 0}}, {0, 1})},
                  {.external = "stream.bfstm"},
                  {.bytes = sar_files::Group(flavor, group)}};

    return sar_files::Archive(flavor, spec);
}

// The archive `bytes`. The program opens an archive the same way.
std::shared_ptr<const sar::SoundArchive> Read(std::vector<uint8_t> bytes)
{
    return hokuto::song::LoadArchive(std::move(bytes), "test.bfsar");
}

// The bytes of file `id` of `archive`.
std::vector<uint8_t> FileOf(const sar::SoundArchive& archive, uint32_t id)
{
    const std::span<const uint8_t> data = archive.FileData(id);
    return {data.begin(), data.end()};
}

// The test archive with other versions in the second wave archive's file and in its wave: 1.0.1 and 1.1.1.
std::vector<uint8_t> NewerSecondWaves(sar::Flavor flavor)
{
    std::vector<uint8_t> bytes = TestArchive(flavor);
    const std::shared_ptr<const sar::SoundArchive> tables = Read(bytes);
    const std::size_t at = tables->FileOffset(kSecondWavesFile).value_or(0);
    const std::vector<uint8_t> second = FileOf(*tables, kSecondWavesFile);
    const std::span<const uint8_t> wave = sar::WaveArchive::Parse(second, tables->ReadOptions()).waves.at(0);
    const auto store = flavor == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32;
    store(&bytes[at + 0x08], 0x00010001);
    store(&bytes[at + static_cast<std::size_t>(wave.data() - second.data()) + 0x08], 0x00010101);

    return bytes;
}

// A sine of `frequency` cycles a sample, `count` samples long, at `amplitude`.
std::vector<int16_t> Sine(double frequency, double amplitude, std::size_t count)
{
    constexpr double kPi = 3.14159265358979323846;
    std::vector<int16_t> samples(count);
    for (std::size_t i = 0; i < count; i++)
    {
        samples[i] = static_cast<int16_t>(std::lround(amplitude * std::sin(2.0 * kPi * frequency * i)));
    }

    return samples;
}

// Noise uniform over [-amplitude, amplitude], from a linear congruential generator that starts at `seed`.
std::vector<int16_t> Noise(uint32_t seed, int32_t amplitude, std::size_t count)
{
    std::vector<int16_t> samples(count);
    uint32_t state = seed;
    for (int16_t& sample : samples)
    {
        state = state * 1664525 + 1013904223;
        sample = static_cast<int16_t>(static_cast<int32_t>(state >> 16) % (2 * amplitude + 1) - amplitude);
    }

    return samples;
}

// The ratio of the energy of `input` to that of its difference from `decoded`, in decibels. No difference gives 200.
double SignalToNoise(std::span<const int16_t> input, std::span<const int16_t> decoded)
{
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = 0; i < input.size(); i++)
    {
        const double x = input[i];
        const double error = x - decoded[i];
        signal += x * x;
        noise += error * error;
    }

    return noise == 0.0 ? 200.0 : 10.0 * std::log10(signal / noise);
}

// The message of the std::invalid_argument that `call` throws, or nothing when it throws none.
template <class Call>
std::string RefusalOf(const Call& call)
{
    try
    {
        call();
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return "";
}

// The other console of the two: the Wii U for the Switch, and the Switch for the Wii U.
sar::Flavor OtherConsole(sar::Flavor flavor)
{
    return flavor == sar::Flavor::kNx ? sar::Flavor::kCafe : sar::Flavor::kNx;
}

// `archive`, read as `tables`, with the entry of the second wave archive's file in the file table naming the first
// one's bytes. The entry holds a reference: its type, two bytes of padding, the offset from the FILE block's body, then
// the size.
std::vector<uint8_t> SharingBytes(const std::vector<uint8_t>& archive, sar::Flavor flavor,
                                  const sar::SoundArchive& tables)
{
    // The FILE block's body follows its 8-byte header. The archive's table of blocks gives where the block starts.
    const xsflib::Reader r(archive, flavor == sar::Flavor::kCafe);
    std::size_t body = 0;
    for (uint16_t i = 0; i < r.U16(0x10); i++)
    {
        body = r.U16(0x14 + std::size_t{i} * 12) == 0x2002 ? r.U32(0x18 + std::size_t{i} * 12) + 8 : body;
    }

    const std::size_t first = tables.FileOffset(kFirstWavesFile).value_or(0) - body;
    const std::size_t second = tables.FileOffset(kSecondWavesFile).value_or(0) - body;
    const auto store = flavor == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32;
    std::vector<uint8_t> sharing = archive;
    for (std::size_t at = 0; at + 12 <= archive.size(); at += 4)
    {
        if (r.U16(at) == 0x1F00 && r.U32(at + 4) == second && r.U32(at + 8) == tables.FileData(kSecondWavesFile).size())
        {
            store(&sharing[at + 4], static_cast<uint32_t>(first));
            store(&sharing[at + 8], static_cast<uint32_t>(tables.FileData(kFirstWavesFile).size()));
        }
    }

    return sharing;
}

// A bank reads with its version and its bytes. Bytes past the size in its header stay out. Another kind of file, a file
// of another family, a size past the end or inside the header, and files that ntdlib can't read are refused: a bank, a
// wave archive with a wave that isn't one, and a wave sound file.
void TestRead(sar::Flavor flavor)
{
    const std::vector<uint8_t> bank = Bank(flavor, {{kFirstWavesId, 0}}, {0});
    std::vector<uint8_t> longer = bank;
    longer.resize(bank.size() + 7, 0xEE);
    std::vector<uint8_t> shorter = bank;
    shorter.pop_back();
    std::vector<uint8_t> unknown = bank;
    unknown[0] = 'X';
    const auto store = flavor == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32;
    std::vector<uint8_t> tiny = bank;
    store(&tiny[0x0C], 8);

    // A reference in the first block's body that points far past the file's end.
    const auto damage = [&](std::vector<uint8_t> file)
    {
        const std::size_t info = xsflib::Reader(file, flavor == sar::Flavor::kCafe).U32(0x18);
        store(&file[info + 8 + 4], 0x00FFFFF0);

        return file;
    };

    const std::vector<uint8_t> waves = sar_files::WaveArchive(flavor, {std::vector<uint8_t>{1, 2, 3, 4}});
    const xsflib::Reader r(bank, flavor == sar::Flavor::kCafe);

    const IncomingFile read = hokuto::edit::ReadIncomingFile(bank, FileKind::kBank, flavor);
    const IncomingFile trimmed = hokuto::edit::ReadIncomingFile(longer, FileKind::kBank, flavor);

    const auto refusal = [&](const std::vector<uint8_t>& bytes, FileKind kind, sar::Flavor console)
    {
        return RefusalOf([&] { hokuto::edit::ReadIncomingFile(bytes, kind, console); });
    };

    XSFLIB_CHECK(read.kind == FileKind::kBank && read.bytes == bank && read.version == r.U32(0x08));
    XSFLIB_CHECK(trimmed.bytes == bank);
    const std::string console = hokuto::song::ConsoleName(flavor);
    XSFLIB_CHECK(refusal(bank, FileKind::kWaveArchive, flavor) ==
                 "It's a bank file of the " + console + ", not a wave archive file.");
    XSFLIB_CHECK(refusal(sar_files::WaveArchive(flavor, {}), FileKind::kBank, flavor) ==
                 "It's a wave archive file of the " + console + ", not a bank file.");
    XSFLIB_CHECK(refusal(unknown, FileKind::kBank, flavor) == "It isn't a bank file.");
    XSFLIB_CHECK(refusal(shorter, FileKind::kBank, flavor) ==
                 "Its header gives a size of " + std::to_string(bank.size()) + " bytes. The file holds " +
                     std::to_string(shorter.size()) + " bytes.");
    XSFLIB_CHECK(refusal(tiny, FileKind::kBank, flavor) ==
                 "Its header gives a size of 8 bytes. The file holds " + std::to_string(bank.size()) + " bytes.");
    XSFLIB_CHECK(refusal(damage(bank), FileKind::kBank, flavor).starts_with("Hokuto can't read it. "));
    XSFLIB_CHECK(refusal(waves, FileKind::kWaveArchive, flavor).starts_with("Hokuto can't read it. "));
    XSFLIB_CHECK(refusal(WaveSounds(flavor, {}, {}), FileKind::kWaveSoundFile, flavor).empty());
    XSFLIB_CHECK(refusal(damage(WaveSounds(flavor, {{kFirstWavesId, 0}}, {0})), FileKind::kWaveSoundFile, flavor)
                     .starts_with("Hokuto can't read it. "));
}

// A bank, a wave sound file and a wave archive of the other console go in converted for the archive's console. The bank
// and the wave sound file come out as the test builder writes them for that console, with the versions of Export…'s
// files or the given ones. The wave archive's waves decode to the same samples. The incoming file gives the console,
// the version and the size of the file that was read. A bank of the 3DS goes in converted too. A file of the 3DS in the
// Wii U's byte order isn't a file of the kind, and a file that ntdlib can't read for the other console is refused.
void TestConversion(sar::Flavor flavor)
{
    const sar::Flavor other = OtherConsole(flavor);
    const std::vector<uint8_t> bank = Bank(other, {{kFirstWavesId, 0}}, {0, 2});
    const std::vector<uint8_t> sounds = WaveSounds(other, {{kFirstWavesId, 1}}, {0});
    const std::vector<uint8_t> waves = sar_files::WaveArchive(other, {Wave(other, 5), Wave(other, 6)});
    const std::vector<uint8_t> ctr = Bank(sar::Flavor::kCtr, {{kFirstWavesId, 0}}, {0});
    std::vector<uint8_t> older_sounds = WaveSounds(flavor, {{kFirstWavesId, 1}}, {0});
    (flavor == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32)(&older_sounds[0x08], 0x00010000);
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;

    const IncomingFile read_bank = hokuto::edit::ReadIncomingFile(bank, FileKind::kBank, flavor);
    const IncomingFile read_sounds =
        hokuto::edit::ReadIncomingFile(sounds, FileKind::kWaveSoundFile, flavor, FileVersions{0x00010000, 0});
    const IncomingFile sounds_for_export = hokuto::edit::ReadIncomingFile(sounds, FileKind::kWaveSoundFile, flavor);
    const IncomingFile read_waves =
        hokuto::edit::ReadIncomingFile(waves, FileKind::kWaveArchive, flavor, FileVersions{0x00010000, 0x00010100});
    const std::vector<std::span<const uint8_t>> parsed = sar::WaveArchive::Parse(read_waves.bytes, options).waves;
    const IncomingFile read_ctr =
        hokuto::edit::ReadIncomingFile(ctr, FileKind::kBank, flavor, FileVersions{0x00010000, 0});
    std::vector<uint8_t> big_ctr = ctr;
    big_ctr[4] = 0xFE;
    big_ctr[5] = 0xFF;
    const std::string refusal =
        RefusalOf([&] { hokuto::edit::ReadIncomingFile(big_ctr, FileKind::kBank, flavor, FileVersions{}); });
    const IncomingFile newer =
        hokuto::edit::ReadIncomingFile(bank, FileKind::kBank, flavor, FileVersions{0x00010001, 0});
    std::vector<uint8_t> damaged = bank;
    const xsflib::Reader other_reader(bank, other == sar::Flavor::kCafe);
    (other == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32)(&damaged[other_reader.U32(0x18) + 8 + 4],
                                                                          0x00FFFFF0);
    const std::string unreadable = RefusalOf([&] { hokuto::edit::ReadIncomingFile(damaged, FileKind::kBank, flavor); });

    XSFLIB_CHECK(read_bank.bytes == Bank(flavor, {{kFirstWavesId, 0}}, {0, 2}) && read_bank.version == 0x00010000);
    XSFLIB_CHECK(read_bank.converted && read_bank.converted->flavor == other &&
                 read_bank.converted->version == 0x00010000 && read_bank.converted->size == bank.size() &&
                 !read_bank.converted->wave_version);
    XSFLIB_CHECK(read_sounds.bytes == older_sounds && read_sounds.version == 0x00010000);
    XSFLIB_CHECK(read_sounds.converted && read_sounds.converted->version == 0x00010100);
    XSFLIB_CHECK(sounds_for_export.version == (flavor == sar::Flavor::kCafe ? 0x00010000u : 0x00010100u));
    XSFLIB_CHECK(read_waves.version == 0x00010000 && read_waves.converted &&
                 read_waves.converted->wave_version == 0x00010100u && parsed.size() == 2);
    for (std::size_t i = 0; i < parsed.size(); i++)
    {
        const sar::Pcm expected = sar::DecodeWave(Wave(flavor, static_cast<uint8_t>(5 + i)), options);
        const sar::Pcm decoded = sar::DecodeWave(parsed[i], options);
        XSFLIB_CHECK(decoded.channels == expected.channels && decoded.sample_rate == expected.sample_rate);
        XSFLIB_CHECK(xsflib::Reader(parsed[i], flavor == sar::Flavor::kCafe).U32(0x08) == 0x00010100);
    }
    XSFLIB_CHECK(read_ctr.bytes == Bank(flavor, {{kFirstWavesId, 0}}, {0}) && read_ctr.version == 0x00010000);
    XSFLIB_CHECK(read_ctr.converted && read_ctr.converted->flavor == sar::Flavor::kCtr &&
                 read_ctr.converted->size == ctr.size());
    XSFLIB_CHECK(refusal == "It isn't a bank file.");
    XSFLIB_CHECK(newer.version == 0x00010001 &&
                 xsflib::Reader(newer.bytes, flavor == sar::Flavor::kCafe).U32(0x08) == 0x00010001);
    XSFLIB_CHECK(unreadable.starts_with("Hokuto can't read it. "));
}

// A file of the archive's console with a later version than the given versions goes in with them. A bank file and a
// wave sound file keep their bytes, the version aside. A wave archive's waves keep their samples. A file of the given
// versions or of earlier ones, and a file read without versions, go in as they are.
void TestLaterVersions(sar::Flavor flavor)
{
    const auto with_version = [flavor](std::vector<uint8_t> file, uint32_t version)
    {
        (flavor == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32)(&file[0x08], version);

        return file;
    };

    const std::vector<uint8_t> bank = Bank(flavor, {{kFirstWavesId, 0}}, {0, 2});
    const std::vector<uint8_t> sounds = WaveSounds(flavor, {{kFirstWavesId, 1}}, {0});
    const std::vector<uint8_t> waves =
        sar_files::WaveArchive(flavor, {Wave(flavor, 5), with_version(Wave(flavor, 6), 0x00010103)});
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;

    const auto read = [flavor](const std::vector<uint8_t>& bytes, FileKind kind, std::optional<FileVersions> versions)
    {
        return hokuto::edit::ReadIncomingFile(bytes, kind, flavor, versions);
    };

    const IncomingFile later_bank = read(with_version(bank, 0x00010001), FileKind::kBank, FileVersions{0x00010000, 0});
    const IncomingFile later_sounds = read(sounds, FileKind::kWaveSoundFile, FileVersions{0x00010000, 0});
    const IncomingFile same_sounds = read(sounds, FileKind::kWaveSoundFile, FileVersions{0x00010100, 0});
    const IncomingFile earlier_sounds = read(sounds, FileKind::kWaveSoundFile, FileVersions{0x00010200, 0});
    const IncomingFile unversioned = read(sounds, FileKind::kWaveSoundFile, std::nullopt);
    const IncomingFile later_waves = read(waves, FileKind::kWaveArchive, FileVersions{0x00010000, 0x00010100});
    const IncomingFile earlier_waves = read(waves, FileKind::kWaveArchive, FileVersions{0x00010000, 0x00010103});
    const std::vector<std::span<const uint8_t>> parsed = sar::WaveArchive::Parse(later_waves.bytes, options).waves;

    XSFLIB_CHECK(later_bank.bytes == bank && later_bank.version == 0x00010000 && later_bank.converted);
    XSFLIB_CHECK(later_bank.converted && later_bank.converted->flavor == flavor &&
                 later_bank.converted->version == 0x00010001 && !later_bank.converted->wave_version);
    XSFLIB_CHECK(later_sounds.bytes == with_version(sounds, 0x00010000) && later_sounds.version == 0x00010000);
    XSFLIB_CHECK(later_sounds.converted && later_sounds.converted->version == 0x00010100);
    XSFLIB_CHECK(same_sounds.bytes == sounds && !same_sounds.converted);
    XSFLIB_CHECK(earlier_sounds.bytes == sounds && earlier_sounds.version == 0x00010100 && !earlier_sounds.converted);
    XSFLIB_CHECK(unversioned.bytes == sounds && !unversioned.converted);
    XSFLIB_CHECK(later_waves.version == 0x00010000 && later_waves.converted && parsed.size() == 2);
    XSFLIB_CHECK(later_waves.converted && later_waves.converted->flavor == flavor &&
                 later_waves.converted->version == 0x00010000 && later_waves.converted->wave_version == 0x00010100u);
    for (std::size_t i = 0; i < parsed.size(); i++)
    {
        const sar::Pcm expected = sar::DecodeWave(Wave(flavor, static_cast<uint8_t>(5 + i)), options);
        XSFLIB_CHECK(sar::DecodeWave(parsed[i], options).channels == expected.channels);
        XSFLIB_CHECK(xsflib::Reader(parsed[i], flavor == sar::Flavor::kCafe).U32(0x08) == 0x00010100);
    }
    XSFLIB_CHECK(earlier_waves.bytes == waves && !earlier_waves.converted);
}

// A file of the other console takes the versions of the file of the item that it replaces, or of the archive's first
// file of the kind. A wave archive's waves take the version of its first wave. Without such a file, the versions of
// Export…'s files serve.
void TestIncomingVersions(sar::Flavor flavor)
{
    const std::shared_ptr<const sar::SoundArchive> archive = Read(NewerSecondWaves(flavor));
    const FileVersions defaults = hokuto::song::ExportVersions(hokuto::song::ItemKind::kWaveArchive, flavor);

    const FileVersions first = hokuto::edit::IncomingVersions(*archive, FileKind::kWaveArchive);
    const FileVersions replaced = hokuto::edit::IncomingVersions(*archive, FileKind::kWaveArchive, 1);
    const FileVersions past = hokuto::edit::IncomingVersions(*archive, FileKind::kWaveArchive, 9);
    const FileVersions bank = hokuto::edit::IncomingVersions(*archive, FileKind::kBank, kBank);
    const FileVersions sounds = hokuto::edit::IncomingVersions(*archive, FileKind::kWaveSoundFile, 0);
    const FileVersions none = hokuto::edit::IncomingVersions(*Read(sar_files::Archive(flavor, {})), FileKind::kBank);

    XSFLIB_CHECK(first.file == 0x00010000 && first.waves == 0x00010100);
    XSFLIB_CHECK(replaced.file == 0x00010001 && replaced.waves == 0x00010101);
    XSFLIB_CHECK(past.file == 0x00010000 && past.waves == 0x00010100);
    XSFLIB_CHECK(bank.file == 0x00010000 && sounds.file == 0x00010100);
    XSFLIB_CHECK(none.file == 0x00010000 && defaults.file == 0x00010000);
    XSFLIB_CHECK(defaults.waves == (flavor == sar::Flavor::kNx ? 0x00010200u : 0x00010100u));
}

// What a new bank, wave archive and wave sound file change besides their items. Items past the archive's lists, and a
// sound that isn't a wave sound, have no file to replace.
void TestChanges(sar::Flavor flavor)
{
    const std::vector<uint8_t> original = TestArchive(flavor);
    const File file = hokuto::format::ImportArchive(original, flavor);
    const std::shared_ptr<const sar::SoundArchive> archive = Read(original);
    const std::vector<uint8_t> present = FileOf(*archive, kBankFile);

    const auto incoming = [&](FileKind kind, const std::vector<uint8_t>& bytes)
    {
        return hokuto::edit::ReadIncomingFile(bytes, kind, flavor);
    };

    const auto refusal = [&](FileKind kind, uint32_t item)
    {
        return RefusalOf([&] { hokuto::edit::FileIdOf(*archive, kind, item); });
    };

    // The new bank lacks program 2, two waves of the first wave archive, one of them named twice, and a third wave
    // archive.
    const std::vector<std::pair<uint32_t, uint32_t>> kNamed = {
        {kFirstWavesId, 0}, {kFirstWavesId, 5}, {kFirstWavesId, 2}, {kFirstWavesId, 5}, {kLackedWavesId, 0}};
    const FileChanges bank =
        hokuto::edit::ChangesOf(file, *archive, kBank, incoming(FileKind::kBank, Bank(flavor, kNamed, {0})));

    // The new wave archive holds one wave. Both banks and the first wave sound play its second.
    const FileChanges waves =
        hokuto::edit::ChangesOf(file, *archive, kFirstWaves,
                                incoming(FileKind::kWaveArchive, sar_files::WaveArchive(flavor, {Wave(flavor, 4)})));

    // A new wave archive of two waves leaves nothing past its end.
    const FileChanges both = hokuto::edit::ChangesOf(
        file, *archive, kFirstWaves,
        incoming(FileKind::kWaveArchive, sar_files::WaveArchive(flavor, {Wave(flavor, 4), Wave(flavor, 5)})));

    // The new wave sound file holds one wave sound. That wave sound plays a wave past the second wave archive's end.
    const FileChanges sounds =
        hokuto::edit::ChangesOf(file, *archive, kFirstWaveSound,
                                incoming(FileKind::kWaveSoundFile, WaveSounds(flavor, {{kSecondWavesId, 3}}, {0})));

    const xsflib::Reader r(present, flavor == sar::Flavor::kCafe);
    XSFLIB_CHECK(bank.sharing == std::vector<uint32_t>{1} && bank.version == r.U32(0x08) && bank.other_copies == 1);
    XSFLIB_CHECK(bank.placed_copies == 0 && bank.unplaced_waves == 0);
    XSFLIB_CHECK(bank.size == present.size() && bank.programs == std::vector<uint32_t>{2});
    XSFLIB_CHECK(bank.players == std::vector<uint32_t>{kSong} && bank.lacked.size() == 2);
    if (bank.lacked.size() == 2)
    {
        const LackedWaves& wave = bank.lacked[0];
        const LackedWaves& whole = bank.lacked[1];
        XSFLIB_CHECK(wave.wave_archive == 0 && !wave.whole && wave.count == 2 &&
                     (wave.waves == std::vector<uint32_t>{2, 5}));
        XSFLIB_CHECK(whole.wave_archive == 2 && whole.whole && whole.waves.empty());
    }
    XSFLIB_CHECK(waves.count == 1 && waves.sharing.empty() && waves.lacked.empty() && waves.other_copies == 0);
    XSFLIB_CHECK((waves.past_banks == std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK(waves.past_sounds == std::vector<uint32_t>{kFirstWaveSound});
    XSFLIB_CHECK(both.count == 2 && both.past_banks.empty() && both.past_sounds.empty());
    XSFLIB_CHECK(sounds.sharing == std::vector<uint32_t>{kSecondWaveSound} && sounds.count == 1);
    XSFLIB_CHECK(sounds.other_copies == 0);
    XSFLIB_CHECK(sounds.past_sounds == std::vector<uint32_t>{kSecondWaveSound} && sounds.lacked.size() == 1);
    if (sounds.lacked.size() == 1)
    {
        XSFLIB_CHECK(sounds.lacked[0].wave_archive == 1 && sounds.lacked[0].count == 1);
        XSFLIB_CHECK(sounds.lacked[0].waves == std::vector<uint32_t>{3});
    }
    XSFLIB_CHECK(refusal(FileKind::kBank, 2) == "The archive has no bank there.");
    XSFLIB_CHECK(refusal(FileKind::kWaveArchive, 2) == "The archive has no wave archive there.");
    XSFLIB_CHECK(refusal(FileKind::kWaveSoundFile, kSong) == "The archive has no wave sound file there.");
    XSFLIB_CHECK(refusal(FileKind::kWaveSoundFile, 3) == "The archive has no wave sound file there.");
}

// The sounds that play each item, for the Replace window's Play: the song plays the bank in its slot, and with it the
// file of the bank's twin. The song plays the first wave archive's waves through the bank, and so does the first wave
// sound. The second wave sound plays the second wave archive. Both wave sounds play their file.
void TestSoundsPlaying(sar::Flavor flavor)
{
    using hokuto::edit::SoundsPlaying;
    const std::shared_ptr<const sar::SoundArchive> archive = Read(TestArchive(flavor));

    XSFLIB_CHECK(SoundsPlaying(*archive, FileKind::kBank, kBank) == std::vector<uint32_t>{kSong});
    XSFLIB_CHECK(SoundsPlaying(*archive, FileKind::kBank, 1) == std::vector<uint32_t>{kSong});
    XSFLIB_CHECK((SoundsPlaying(*archive, FileKind::kWaveArchive, kFirstWaves) ==
                  std::vector<uint32_t>{kSong, kFirstWaveSound}));
    XSFLIB_CHECK(SoundsPlaying(*archive, FileKind::kWaveArchive, kSecondWaves) ==
                 std::vector<uint32_t>{kSecondWaveSound});
    XSFLIB_CHECK((SoundsPlaying(*archive, FileKind::kWaveSoundFile, kSecondWaveSound) ==
                  std::vector<uint32_t>{kFirstWaveSound, kSecondWaveSound}));
}

// The song selects program 0 of the bank in slot 0, program 2 in slot 1, program 1 of the bank's twin in slot 2, and a
// program from variable 3 there. Slot 1 holds no bank. The twin has the bank's file. The bank's sounds therefore select
// programs 0 and 1, and the twin's too, and one program change is undecided. A new bank with programs 1 and 3 lacks
// program 0. Program 0 then takes the present bank's instrument or the new bank's program 3. The present bank's
// instrument plays the first wave archive's first wave, and program 3 plays that wave archive's second wave. The copy
// reuses the new bank's entry for that wave. The new bank's other instruments stay, and the export holds the new bank.
// A program that the new bank holds, a program that two fixes take and a program without an instrument to copy are
// refused.
void TestProgramFixes(sar::Flavor flavor)
{
    const std::vector<uint8_t> kProgramsBytecode = {0xB0, 48,   0x81, 0,  60,   100, 24,   0xB6, 1,  0x81,
                                                    2,    62,   100,  24, 0xB6, 2,   0x81, 1,    64, 100,
                                                    24,   0xA1, 0x81, 3,  65,   100, 24,   0xFF};
    const std::vector<uint8_t> original = TestArchive(flavor, kProgramsBytecode, {0, 0xFFFFFFFF, 1});
    const File file = hokuto::format::ImportArchive(original, flavor);
    const std::shared_ptr<const sar::SoundArchive> archive = Read(original);
    const std::vector<uint8_t> bytes = hokuto::format::ExportArchive(file);
    const std::vector<std::optional<hokuto::edit::SoundPlace>> places =
        hokuto::edit::SoundPlaces(*archive, hokuto::format::SequencesOfFiles(file, bytes, *archive),
                                  hokuto::format::StartFieldsOfSounds(file, bytes));
    const std::vector<uint8_t> next = Bank(flavor, {{kSecondWavesId, 0}, {kFirstWavesId, 1}}, {1, 3});
    const auto fixed = [&](std::vector<ProgramFix> fixes)
    {
        return sar::Bank::Parse(hokuto::edit::WithProgramFixes(*archive, kBank, next, fixes), archive->ReadOptions());
    };

    const auto wave_of = [](const sar::Bank& bank, uint32_t program)
    {
        const sar::VelocityRegion* region = bank.instruments.at(program)->Find(60, 100);
        const sar::WaveId& wave = bank.waves.at(region->wave_id_index);
        return std::pair<uint32_t, uint32_t>{wave.wave_archive_item, wave.index};
    };

    const auto refusal = [&](std::vector<ProgramFix> fixes)
    {
        return RefusalOf([&] { hokuto::edit::WithProgramFixes(*archive, kBank, next, fixes); });
    };

    const hokuto::edit::BankSelections selections = hokuto::edit::ProgramsOfBank(file, *archive, places, kBank);
    const sar::Bank present = fixed({{0, std::nullopt}});
    const sar::Bank copied = fixed({{0, 3}});
    const std::vector<uint8_t> both = hokuto::edit::WithProgramFixes(*archive, kBank, next, {{{0, 3}, {2, {}}}});
    const std::shared_ptr<const sar::SoundArchive> after =
        Read(hokuto::format::ExportArchive(hokuto::edit::ReplaceFile(file, kBankFile, both)));

    XSFLIB_CHECK((selections.programs == std::vector<uint32_t>{0, 1}) && selections.undecided == 1);
    XSFLIB_CHECK((hokuto::edit::ProgramsOfBank(file, *archive, places, 1).programs == std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK(hokuto::edit::ProgramsOfBank(file, *archive, {}, kBank).programs.empty());
    XSFLIB_CHECK(hokuto::edit::WithProgramFixes(*archive, kBank, next, {}) == next);
    XSFLIB_CHECK(present.instruments.size() == 4 && copied.instruments.size() == 4);
    XSFLIB_CHECK((wave_of(present, 0) == std::pair<uint32_t, uint32_t>{kFirstWavesId, 0}));
    XSFLIB_CHECK((wave_of(copied, 0) == std::pair<uint32_t, uint32_t>{kFirstWavesId, 1}));
    XSFLIB_CHECK(copied.waves.size() == 2 && present.waves.size() == 3);
    for (const sar::Bank& bank : {present, copied})
    {
        XSFLIB_CHECK((wave_of(bank, 1) == std::pair<uint32_t, uint32_t>{kSecondWavesId, 0}));
        XSFLIB_CHECK((wave_of(bank, 3) == std::pair<uint32_t, uint32_t>{kFirstWavesId, 1}));
        XSFLIB_CHECK(!bank.instruments[2]);
    }
    XSFLIB_CHECK(FileOf(*after, kBankFile) == both);
    XSFLIB_CHECK(refusal({{1, std::nullopt}}).starts_with("Hokuto can't copy the instruments into the new bank."));
    XSFLIB_CHECK(refusal({{0, 3}, {0, std::nullopt}}).starts_with("Hokuto can't copy the instruments"));
    XSFLIB_CHECK(refusal({{0, 2}}).starts_with("Hokuto can't copy the instruments"));
    XSFLIB_CHECK(refusal({{1, 1}}).starts_with("Hokuto can't copy the instruments"));
}

// A longer bank and a shorter wave archive go in, in each copy, and the archive's other files keep their bytes. The
// version that the edit starts from keeps its pieces. A file without a native archive, a file that an earlier cut keeps
// among the archive's bytes, a file whose bytes another file ID names, and a file outside the archive are refused.
void TestReplace(sar::Flavor flavor)
{
    const std::vector<uint8_t> original = TestArchive(flavor);
    const File file = hokuto::format::ImportArchive(original, flavor);
    const std::shared_ptr<const sar::SoundArchive> before = Read(original);
    const std::vector<uint8_t> bank = Bank(flavor, {{kFirstWavesId, 0}, {kFirstWavesId, 1}}, {0, 1, 2, 3, 4});
    const std::vector<uint8_t> waves = sar_files::WaveArchive(flavor, {Wave(flavor, 5)});
    File without = file;
    without.native = {};
    File one_run = file;
    one_run.native.Edit(
        [&](hokuto::format::NativeArchive& native)
        {
            native.pieces = std::vector<sar::Piece>{original};
            native.fields.clear();
        });

    // File 3 names the bytes of file 2 too: the offset and the size that its entry in the file table holds.
    const File shared = hokuto::format::ImportArchive(SharingBytes(original, flavor, *before), flavor);

    const File new_bank = hokuto::edit::ReplaceFile(file, kBankFile, bank);
    const File new_waves = hokuto::edit::ReplaceFile(new_bank, kFirstWavesFile, waves);
    const std::shared_ptr<const sar::SoundArchive> after = Read(hokuto::format::ExportArchive(new_waves));

    const auto refusal = [&](const File& from, uint32_t id)
    {
        return RefusalOf([&] { hokuto::edit::ReplaceFile(from, id, bank); });
    };

    const auto copies = [&](uint32_t id)
    {
        const hokuto::format::NativeArchive& native = *new_waves.native;
        return sar::FileCopies(flavor, hokuto::format::CopySequences(new_waves), *native.pieces, native.fields, id);
    };

    XSFLIB_CHECK(FileOf(*after, kBankFile) == bank && FileOf(*after, kFirstWavesFile) == waves);
    XSFLIB_CHECK(copies(kBankFile).size() == 2 && copies(kFirstWavesFile).size() == 2);
    for (const sar::FileCopy& copy : copies(kBankFile))
    {
        XSFLIB_CHECK(copy.bytes == bank);
    }
    for (const sar::FileCopy& copy : copies(kFirstWavesFile))
    {
        XSFLIB_CHECK(copy.bytes == waves);
    }
    for (const uint32_t id : {kSequenceFile, kSecondWavesFile, kWaveSoundsFile})
    {
        XSFLIB_CHECK(FileOf(*after, id) == FileOf(*before, id));
    }
    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == original && *file.native->pieces != *new_bank.native->pieces);
    XSFLIB_CHECK(refusal(without, kBankFile) == "The file holds no archive of a game.");
    XSFLIB_CHECK(refusal(one_run, kBankFile).starts_with("The old file lies among the archive's other bytes."));
    XSFLIB_CHECK(refusal(shared, kFirstWavesFile) ==
                 "Another file of the archive holds the same bytes. That file would change too.");
    XSFLIB_CHECK(refusal(file, kOutsideFile) ==
                 "The old file lies outside the archive. Hokuto can't replace it there.");
}

// The group's copies of the bank and the wave sound file name their files' waves at other places. A new bank and a new
// wave sound file then go into those copies with the copies' places, and each wave that the present file doesn't name
// keeps its place. The archive's copies take the new files as they are. Each copy keeps the bytes of its file outside
// the table of waves. A copy that differs in other bytes too takes the new bank as it is.
void TestCopyPlaces(sar::Flavor flavor)
{
    const std::vector<uint8_t> original = TestArchive(flavor, kSongBytecode, {0}, true);
    const File file = hokuto::format::ImportArchive(original, flavor);
    const std::shared_ptr<const sar::SoundArchive> archive = Read(original);
    const std::vector<std::pair<uint32_t, uint32_t>> kNamed = {
        {kFirstWavesId, 1}, {kSecondWavesId, 0}, {kFirstWavesId, 0}};
    const std::vector<uint8_t> bank = Bank(flavor, kNamed, {0, 1, 2});
    const std::vector<uint8_t> sounds = WaveSounds(flavor, kNamed, {2, 1, 0});
    const FileChanges bank_changes =
        hokuto::edit::ChangesOf(file, *archive, kBank, hokuto::edit::ReadIncomingFile(bank, FileKind::kBank, flavor));
    const FileChanges sound_changes = hokuto::edit::ChangesOf(
        file, *archive, kFirstWaveSound, hokuto::edit::ReadIncomingFile(sounds, FileKind::kWaveSoundFile, flavor));

    const File new_bank = hokuto::edit::ReplaceFile(file, kBankFile, bank);
    const File new_sounds = hokuto::edit::ReplaceFile(new_bank, kWaveSoundsFile, sounds);
    const auto copies = [&](uint32_t id)
    {
        const hokuto::format::NativeArchive& native = *new_sounds.native;
        std::vector<std::vector<uint8_t>> bytes;
        for (const sar::FileCopy& copy :
             sar::FileCopies(flavor, hokuto::format::CopySequences(new_sounds), *native.pieces, native.fields, id))
        {
            bytes.push_back(copy.bytes);
        }

        return bytes;
    };

    const auto placed = [&](const std::vector<uint8_t>& bytes, const std::vector<sar::WaveId>& waves)
    {
        return sar::WithWaveTable(bytes, flavor, waves);
    };

    // The bank's copy names the first wave archive's first wave at the second's place, and its second wave at the
    // second wave archive's. The wave sound file's copy names the file's two waves in each other's places.
    const std::vector<uint8_t> group_bank =
        placed(bank, {{kSecondWavesId, 0}, {kSecondWavesId, 0}, {kFirstWavesId, 1}});
    const std::vector<uint8_t> group_sounds =
        placed(sounds, {{kSecondWavesId, 0}, {kFirstWavesId, 1}, {kFirstWavesId, 0}});
    XSFLIB_CHECK(bank_changes.other_copies == 0 && bank_changes.placed_copies == 1 && bank_changes.unplaced_waves == 1);
    XSFLIB_CHECK(sound_changes.other_copies == 0 && sound_changes.placed_copies == 1 &&
                 sound_changes.unplaced_waves == 1);
    XSFLIB_CHECK((copies(kBankFile) == std::vector<std::vector<uint8_t>>{bank, group_bank}));
    XSFLIB_CHECK((copies(kWaveSoundsFile) == std::vector<std::vector<uint8_t>>{sounds, group_sounds}));
    XSFLIB_CHECK(group_bank.size() == bank.size() && group_sounds.size() == sounds.size());
    const std::shared_ptr<const sar::SoundArchive> after = Read(hokuto::format::ExportArchive(new_sounds));
    XSFLIB_CHECK(FileOf(*after, kBankFile) == bank && FileOf(*after, kWaveSoundsFile) == sounds);

    // A copy that differs in another byte too, here its last, takes the new bank as it is.
    const hokuto::format::NativeArchive& native = *file.native;
    const std::vector<sar::FileCopy> old_copies =
        sar::FileCopies(flavor, hokuto::format::CopySequences(file), *native.pieces, native.fields, kBankFile);
    SEQLIB_REQUIRE(!old_copies.empty());
    std::vector<uint8_t> differing = original;
    differing[old_copies.back().start + old_copies.back().bytes.size() - 1] ^= 1;
    const File other = hokuto::format::ImportArchive(differing, flavor);
    const FileChanges other_changes = hokuto::edit::ChangesOf(
        other, *Read(differing), kBank, hokuto::edit::ReadIncomingFile(bank, FileKind::kBank, flavor));
    const File other_bank = hokuto::edit::ReplaceFile(other, kBankFile, bank);
    const hokuto::format::NativeArchive& replaced = *other_bank.native;
    XSFLIB_CHECK(other_changes.other_copies == 1 && other_changes.placed_copies == 0);
    for (const sar::FileCopy& copy : sar::FileCopies(flavor, hokuto::format::CopySequences(other_bank),
                                                     *replaced.pieces, replaced.fields, kBankFile))
    {
        XSFLIB_CHECK(copy.bytes == bank);
    }
}

// A stereo WAV file with a loop from sample 2 to 5 and one sample after it takes the place of the first wave archive's
// second wave. The wave holds the first 5 samples of each channel as 16-bit PCM in the console's byte order, with the
// loop, the rate and the present wave's version. Both banks name the wave, and the first wave sound plays it. The
// group's copy of the wave archive takes the new wave too, and Hokuto decodes the WAV file's samples from the exported
// archive.
void TestWave(sar::Flavor flavor)
{
    const std::vector<uint8_t> original = TestArchive(flavor);
    const File file = hokuto::format::ImportArchive(original, flavor);
    const std::shared_ptr<const sar::SoundArchive> before = Read(original);
    seqlib::Pcm pcm;
    pcm.channels = {{100, -200, 300, -400, 500, -600}, {1, 2, 3, 4, 5, 6}};
    pcm.loop = true;
    pcm.loop_start = 2;
    pcm.loop_end = 5;
    const std::vector<uint8_t> wav = seqlib::WavFile(pcm, 22050);
    seqlib::Pcm three;
    three.channels = {{1}, {2}, {3}};
    seqlib::Pcm empty;
    empty.channels = {{}};
    sar::Options strict;
    strict.flavor = flavor;
    strict.rules = sar::Rules::kStopAtFirstProblem;

    const IncomingWave incoming =
        hokuto::edit::ReadIncomingWave(wav, *before, kFirstWaves, 1, sar::WaveEncoding::kPcm16);
    const WaveChanges changes = hokuto::edit::ChangesOfWave(*before, kFirstWaves, 1);
    const WaveChanges first = hokuto::edit::ChangesOfWave(*before, kFirstWaves, 0);
    const WaveChanges other = hokuto::edit::ChangesOfWave(*before, 1, 0);
    const std::vector<uint8_t> waves = hokuto::edit::WaveArchiveWith(*before, kFirstWaves, 1, incoming.bytes);
    const File replaced = hokuto::edit::ReplaceFile(file, kFirstWavesFile, waves);
    const std::shared_ptr<const sar::SoundArchive> after = Read(hokuto::format::ExportArchive(replaced));
    const sar::Pcm decoded = hokuto::song::DecodeItemWave(*after, kFirstWaves, 1);

    const auto refusal = [&](const std::vector<uint8_t>& bytes, uint32_t wave_archive, uint32_t wave)
    {
        return RefusalOf([&] { hokuto::edit::ReadIncomingWave(bytes, *before, wave_archive, wave); });
    };

    const sar::Wave wave = sar::Wave::Parse(incoming.bytes, strict);
    const std::vector<uint8_t> first_sample =
        flavor == sar::Flavor::kCafe ? std::vector<uint8_t>{0x00, 0x64} : std::vector<uint8_t>{0x64, 0x00};
    XSFLIB_CHECK(incoming.channels == 2 && incoming.rate == 22050 && incoming.frames == 5 && incoming.loop);
    XSFLIB_CHECK(incoming.encoding == sar::WaveEncoding::kPcm16 && incoming.loop_moved == 0);
    XSFLIB_CHECK(!incoming.signal_to_noise);
    XSFLIB_CHECK(incoming.loop_start == 2 && incoming.after_loop == 1 && incoming.bits == 16 && !incoming.floating);
    XSFLIB_CHECK(incoming.loops == 1 && incoming.loop_forward && incoming.loop_endless && incoming.dropped == 0);
    XSFLIB_CHECK(wave.encoding == sar::WaveEncoding::kPcm16 && wave.loop && wave.loop_start == 2 && wave.loop_end == 5);
    XSFLIB_CHECK(wave.sample_rate == 22050 && wave.channels.size() == 2 && wave.channels[1].data.size() == 10);
    SEQLIB_REQUIRE(!wave.channels.empty() && wave.channels[0].data.size() >= first_sample.size());
    XSFLIB_CHECK(std::equal(first_sample.begin(), first_sample.end(), wave.channels[0].data.begin()));
    XSFLIB_CHECK(sar::WaveFile::Read(incoming.bytes).version == 0x00010100);
    XSFLIB_CHECK(changes.encoding == sar::WaveEncoding::kPcm16 && changes.channels == 1 && changes.rate == 32000);
    XSFLIB_CHECK(!changes.loop && changes.loop_end == 4 && changes.size == Wave(flavor, 2).size());
    XSFLIB_CHECK((changes.banks == std::vector<uint32_t>{0, 1}) &&
                 (changes.sounds == std::vector<uint32_t>{kFirstWaveSound}));
    XSFLIB_CHECK((first.banks == std::vector<uint32_t>{0, 1}) && first.sounds.empty());
    XSFLIB_CHECK(other.banks.empty() && (other.sounds == std::vector<uint32_t>{kSecondWaveSound}));
    SEQLIB_REQUIRE(sar::WaveArchiveFile::Read(waves).waves.size() == 2);
    XSFLIB_CHECK(sar::WaveArchiveFile::Read(waves).waves[0] == Wave(flavor, 1));
    XSFLIB_CHECK(sar::WaveArchiveFile::Read(waves).waves[1] == incoming.bytes);
    XSFLIB_CHECK(FileOf(*after, kFirstWavesFile) == waves && FileOf(*after, kBankFile) == FileOf(*before, kBankFile));
    for (const sar::FileCopy& copy :
         sar::FileCopies(flavor, hokuto::format::CopySequences(replaced), *replaced.native->pieces,
                         replaced.native->fields, kFirstWavesFile))
    {
        XSFLIB_CHECK(copy.bytes == waves);
    }
    XSFLIB_CHECK(decoded.sample_rate == 22050 && decoded.loop && decoded.loop_start == 2 && decoded.loop_end == 5);
    XSFLIB_CHECK((decoded.channels == std::vector<std::vector<int16_t>>{{100, -200, 300, -400, 500}, {1, 2, 3, 4, 5}}));
    XSFLIB_CHECK(refusal({1, 2, 3}, kFirstWaves, 1) == "Hokuto can't read it as a WAV file: not a RIFF WAVE file.");
    XSFLIB_CHECK(refusal(seqlib::WavFile(three, 8000), kFirstWaves, 1) ==
                 "It holds 3 channels. A wave holds one or two.");
    XSFLIB_CHECK(refusal(seqlib::WavFile(empty, 8000), kFirstWaves, 1) == "It holds no samples.");
    XSFLIB_CHECK(refusal(wav, kFirstWaves, 2) == "The wave archive holds 2 waves. It has no wave 2.");
    XSFLIB_CHECK(refusal(wav, 2, 0) == "The archive has no wave archive there.");
    XSFLIB_CHECK(RefusalOf([&] { hokuto::edit::WaveArchiveWith(*before, 1, 1, incoming.bytes); }) ==
                 "The wave archive holds 1 wave. It has no wave 1.");
    XSFLIB_CHECK(
        RefusalOf([&] { hokuto::edit::ReadIncomingWave(wav, *before, kFirstWaves, 1, sar::WaveEncoding::kPcm8); }) ==
        "Hokuto makes a wave in DSP-ADPCM or in 16-bit PCM only.");
}

// A stereo WAV file of a sine and of noise, 1000 samples long, with a loop from sample 101 to 990, takes the place of
// the first wave archive's second wave as DSP-ADPCM. The samples after the loop's end stay out. The loop's start moves
// to sample 112, the start of the next frame of 14 samples. Samples 101 to 111 repeat after the loop's end, and the
// wave holds 1001 samples in 72 frames. A predictor of two samples predicts a sine exactly, and the sine comes back
// with an error more than 40 dB below the signal. Noise that nothing predicts comes back with the error of a 4-bit
// quantizer: more than 20 dB below the signal. A later pass of the loop starts at the loop's frame, from the frame's
// header and the two samples that the wave keeps for the loop's start. It gives the first pass's samples again.
void TestDspAdpcmWave(sar::Flavor flavor)
{
    const std::vector<uint8_t> original = TestArchive(flavor);
    const File file = hokuto::format::ImportArchive(original, flavor);
    const std::shared_ptr<const sar::SoundArchive> before = Read(original);
    seqlib::Pcm pcm;
    pcm.channels = {Sine(0.013, 12000.0, 1000), Noise(7, 9000, 1000)};
    pcm.loop = true;
    pcm.loop_start = 101;
    pcm.loop_end = 990;
    const std::vector<uint8_t> wav = seqlib::WavFile(pcm, 32000);
    std::vector<std::vector<int16_t>> played = pcm.channels;
    for (std::vector<int16_t>& channel : played)
    {
        const std::vector<int16_t> repeated(channel.begin() + 101, channel.begin() + 112);
        channel.resize(990);
        channel.insert(channel.end(), repeated.begin(), repeated.end());
    }

    const IncomingWave incoming =
        hokuto::edit::ReadIncomingWave(wav, *before, kFirstWaves, 1, sar::WaveEncoding::kDspAdpcm);
    const std::vector<uint8_t> waves = hokuto::edit::WaveArchiveWith(*before, kFirstWaves, 1, incoming.bytes);
    const File replaced = hokuto::edit::ReplaceFile(file, kFirstWavesFile, waves);
    const std::shared_ptr<const sar::SoundArchive> after = Read(hokuto::format::ExportArchive(replaced));
    const sar::Pcm decoded = hokuto::song::DecodeItemWave(*after, kFirstWaves, 1);

    const sar::WaveFile wave = sar::WaveFile::Read(incoming.bytes);
    XSFLIB_CHECK(incoming.encoding == sar::WaveEncoding::kDspAdpcm && incoming.frames == 990 && incoming.loop);
    XSFLIB_CHECK(incoming.loop_start == 101 && incoming.loop_moved == 11 && incoming.after_loop == 10);
    XSFLIB_CHECK(incoming.signal_to_noise && *incoming.signal_to_noise > 20.0);
    XSFLIB_CHECK(wave.encoding == sar::WaveEncoding::kDspAdpcm && wave.loop && wave.sample_rate == 32000);
    XSFLIB_CHECK(wave.loop_start == 112 && wave.loop_end == 1001 && wave.original_loop_start == 101);
    XSFLIB_CHECK(wave.version == 0x00010100 && wave.channels.size() == 2 && wave.channels[1].samples.size() == 72 * 8);
    SEQLIB_REQUIRE(sar::WaveArchiveFile::Read(waves).waves.size() > 1);
    XSFLIB_CHECK(sar::WaveArchiveFile::Read(waves).waves[1] == incoming.bytes &&
                 FileOf(*after, kFirstWavesFile) == waves);
    XSFLIB_CHECK(decoded.loop && decoded.loop_start == 112 && decoded.loop_end == 1001);
    SEQLIB_REQUIRE(decoded.channels.size() == 2 && decoded.channels[0].size() == 1001 &&
                   decoded.channels[1].size() == 1001);
    for (std::size_t c = 0; c < wave.channels.size() && c < decoded.channels.size(); c++)
    {
        SEQLIB_REQUIRE(wave.channels[c].adpcm && wave.channels[c].samples.size() > 112 / 14 * 8);
        const sar::DspAdpcmInfo& info = *wave.channels[c].adpcm;
        const std::span<const uint8_t> loop_frames = std::span(wave.channels[c].samples).subspan(112 / 14 * 8);
        const std::vector<int16_t> later = ntdlib::wave::DecodeDspAdpcm(
            loop_frames, 1001 - 112, info.coefs, info.loop_yn1, info.loop_yn2, ntdlib::wave::DspAdpcmSum::kWrap);
        XSFLIB_CHECK(SignalToNoise(played[c], decoded.channels[c]) > (c == 0 ? 40.0 : 20.0));
        XSFLIB_CHECK(info.yn1 == 0 && info.yn2 == 0 && info.pred_scale == wave.channels[c].samples[0]);
        XSFLIB_CHECK(info.loop_pred_scale == loop_frames[0]);
        XSFLIB_CHECK(std::equal(later.begin(), later.end(), decoded.channels[c].begin() + 112));
    }
}

// A mono WAV file without a loop takes the place of the first wave archive's second wave: a sine of 1 kHz at 48 kHz,
// 4,800 samples long. The edit gives it a loop from sample 1,200 to the end, and a rate of 32 kHz. The wave holds 3,200
// samples of 16-bit PCM at 32 kHz, with the loop from sample 800. Hokuto decodes the new samples from the exported
// archive. Past the fade in from the silence before the first sample, each lies within 2 steps of the sine at 32 kHz.
// The loop holds 75 cycles of the sine, and the samples up to its end stay clean. A WAV file that holds a loop keeps
// it. Without a rate, a loop that ends before the last sample leaves the samples after it out. A loop outside the
// samples and a rate that isn't lower than the WAV file's are refused.
void TestResampledWave(sar::Flavor flavor)
{
    const std::vector<uint8_t> original = TestArchive(flavor);
    const File file = hokuto::format::ImportArchive(original, flavor);
    const std::shared_ptr<const sar::SoundArchive> before = Read(original);
    seqlib::Pcm pcm;
    pcm.channels = {Sine(1000.0 / 48000.0, 12000.0, 4800)};
    const std::vector<uint8_t> wav = seqlib::WavFile(pcm, 48000);
    seqlib::Pcm looped;
    looped.channels = {{100, -200, 300, -400, 500, -600}};
    looped.loop = true;
    looped.loop_start = 2;
    looped.loop_end = 5;
    const std::vector<uint8_t> looped_wav = seqlib::WavFile(looped, 48000);
    const std::vector<int16_t> expected = Sine(1000.0 / 32000.0, 12000.0, 3200);

    const auto read =
        [&](const std::vector<uint8_t>& bytes, std::optional<hokuto::edit::WaveLoop> loop, std::optional<uint32_t> rate)
    {
        return hokuto::edit::ReadIncomingWave(bytes, *before, kFirstWaves, 1, sar::WaveEncoding::kPcm16, loop, rate);
    };

    const IncomingWave incoming = read(wav, hokuto::edit::WaveLoop{1200, 4800}, 32000);
    const IncomingWave kept = read(looped_wav, hokuto::edit::WaveLoop{0, 3}, std::nullopt);
    const IncomingWave shorter = read(wav, hokuto::edit::WaveLoop{1200, 4000}, std::nullopt);
    const std::vector<uint8_t> waves = hokuto::edit::WaveArchiveWith(*before, kFirstWaves, 1, incoming.bytes);
    const File replaced = hokuto::edit::ReplaceFile(file, kFirstWavesFile, waves);
    const std::shared_ptr<const sar::SoundArchive> after = Read(hokuto::format::ExportArchive(replaced));
    const sar::Pcm decoded = hokuto::song::DecodeItemWave(*after, kFirstWaves, 1);

    const sar::WaveFile wave = sar::WaveFile::Read(incoming.bytes);
    XSFLIB_CHECK(incoming.rate == 32000 && incoming.frames == 3200 && incoming.loop && incoming.loop_start == 800);
    XSFLIB_CHECK(incoming.wav_rate == 48000 && incoming.wav_frames == 4800 && incoming.wav_loop_start == 1200);
    XSFLIB_CHECK(incoming.loop_from_present && incoming.after_loop == 0 && incoming.loops == 0);
    XSFLIB_CHECK(wave.sample_rate == 32000 && wave.loop && wave.loop_start == 800 && wave.loop_end == 3200);
    XSFLIB_CHECK(wave.original_loop_start == 800 && wave.channels.size() == 1);
    XSFLIB_CHECK(decoded.sample_rate == 32000 && decoded.loop && decoded.loop_start == 800 && decoded.loop_end == 3200);
    SEQLIB_REQUIRE(decoded.channels.size() == 1 && decoded.channels[0].size() == 3200);
    double largest = 0.0;
    for (std::size_t i = 100; i < decoded.channels[0].size() && i < expected.size(); i++)
    {
        largest = std::max(largest, std::abs(static_cast<double>(decoded.channels[0][i]) - expected[i]));
    }
    XSFLIB_CHECK(largest <= 2.0);
    XSFLIB_CHECK(kept.loop && kept.loop_start == 2 && kept.frames == 5 && kept.after_loop == 1);
    XSFLIB_CHECK(!kept.loop_from_present && kept.loops == 1 && kept.rate == 48000);
    XSFLIB_CHECK(shorter.rate == 48000 && shorter.loop_start == 1200 && shorter.frames == 4000);
    XSFLIB_CHECK(shorter.after_loop == 800 && shorter.wav_frames == 4000 && shorter.loop_from_present);
    XSFLIB_CHECK(RefusalOf([&] { read(wav, hokuto::edit::WaveLoop{0, 4801}, std::nullopt); }) ==
                 "The loop from sample 0 to 4801 doesn't lie inside the WAV file's 4800 samples.");
    XSFLIB_CHECK(RefusalOf([&] { read(wav, hokuto::edit::WaveLoop{1200, 1200}, std::nullopt); }) ==
                 "The loop from sample 1200 to 1200 doesn't lie inside the WAV file's 4800 samples.");
    XSFLIB_CHECK(RefusalOf([&] { read(wav, std::nullopt, 48000); }) ==
                 "A wave takes a rate lower than the WAV file's 48000 Hz only.");
    XSFLIB_CHECK(RefusalOf([&] { read(wav, std::nullopt, 0); }) ==
                 "A wave takes a rate lower than the WAV file's 48000 Hz only.");
}

// A mono WAV file goes in as a new wave after the first wave archive's two waves: wave 2, in 16-bit PCM with the
// version of the wave archive's first wave. The group's copy of the wave archive takes the new wave too. The other
// waves and files keep their bytes, and Hokuto decodes the WAV file's samples from the exported archive. A new wave of
// the second wave archive takes the version of that wave archive's wave. The wave archive has to be one of the
// archive's.
void TestNewWave(sar::Flavor flavor)
{
    const std::vector<uint8_t> original = TestArchive(flavor);
    const File file = hokuto::format::ImportArchive(original, flavor);
    const std::shared_ptr<const sar::SoundArchive> before = Read(original);
    const std::shared_ptr<const sar::SoundArchive> newer = Read(NewerSecondWaves(flavor));
    seqlib::Pcm pcm;
    pcm.channels = {{100, -200, 300, -400}};
    const std::vector<uint8_t> wav = seqlib::WavFile(pcm, 16000);

    const IncomingWave incoming =
        hokuto::edit::ReadIncomingWave(wav, *before, kFirstWaves, std::nullopt, sar::WaveEncoding::kPcm16);
    const IncomingWave second =
        hokuto::edit::ReadIncomingWave(wav, *newer, kSecondWaves, std::nullopt, sar::WaveEncoding::kPcm16);
    const std::vector<uint8_t> waves =
        hokuto::edit::WaveArchiveWith(*before, kFirstWaves, std::nullopt, incoming.bytes);
    const File imported = hokuto::edit::ReplaceFile(file, kFirstWavesFile, waves);
    const std::shared_ptr<const sar::SoundArchive> after = Read(hokuto::format::ExportArchive(imported));
    const sar::Pcm decoded = hokuto::song::DecodeItemWave(*after, kFirstWaves, 2);

    const sar::WaveFile wave = sar::WaveFile::Read(incoming.bytes);
    XSFLIB_CHECK(incoming.version == 0x00010100 && wave.version == 0x00010100 && !incoming.converted);
    XSFLIB_CHECK(incoming.channels == 1 && incoming.rate == 16000 && incoming.frames == 4 && !incoming.loop);
    XSFLIB_CHECK(wave.flavor == flavor && wave.encoding == sar::WaveEncoding::kPcm16 && wave.sample_rate == 16000);
    XSFLIB_CHECK(second.version == 0x00010101 && sar::WaveFile::Read(second.bytes).version == 0x00010101);
    XSFLIB_CHECK((sar::WaveArchiveFile::Read(waves).waves ==
                  std::vector<std::vector<uint8_t>>{Wave(flavor, 1), Wave(flavor, 2), incoming.bytes}));
    XSFLIB_CHECK(FileOf(*after, kFirstWavesFile) == waves);
    for (const uint32_t id : {kSequenceFile, kBankFile, kSecondWavesFile, kWaveSoundsFile})
    {
        XSFLIB_CHECK(FileOf(*after, id) == FileOf(*before, id));
    }
    for (const sar::FileCopy& copy :
         sar::FileCopies(flavor, hokuto::format::CopySequences(imported), *imported.native->pieces,
                         imported.native->fields, kFirstWavesFile))
    {
        XSFLIB_CHECK(copy.bytes == waves);
    }
    XSFLIB_CHECK(decoded.sample_rate == 16000 && !decoded.loop);
    XSFLIB_CHECK((decoded.channels == std::vector<std::vector<int16_t>>{{100, -200, 300, -400}}));
    XSFLIB_CHECK(RefusalOf([&] { hokuto::edit::WaveArchiveWith(*before, 2, std::nullopt, incoming.bytes); }) ==
                 "The archive has no wave archive there.");
}

// A looped wave file of the archive's console goes in as it is. It becomes a new wave after the second wave archive's
// wave. Bytes past the size in its header stay out. A wave file of the other console or of the 3DS goes in converted,
// with the version of the wave archive's wave, and its samples decode as that console reads them. Bytes that aren't a
// wave file, a file of another family, a file of the 3DS in the Wii U's byte order, a size past the end or inside the
// header, and a wave that ntdlib can't read or convert are refused.
void TestNewWaveFile(sar::Flavor flavor)
{
    const sar::Flavor other = OtherConsole(flavor);
    const std::vector<uint8_t> original = NewerSecondWaves(flavor);
    const File file = hokuto::format::ImportArchive(original, flavor);
    const std::shared_ptr<const sar::SoundArchive> before = Read(original);
    sar_files::WaveSpec spec;
    spec.encoding = 1;
    spec.loop = true;
    spec.sample_rate = 24000;
    spec.loop_start = 1;
    spec.loop_end = 4;
    spec.channels = {{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}};
    const std::vector<uint8_t> same = sar_files::Wave(flavor, spec);
    const std::vector<uint8_t> converted = sar_files::Wave(other, spec);
    std::vector<uint8_t> longer = same;
    longer.resize(same.size() + 7, 0xEE);
    std::vector<uint8_t> shorter = same;
    shorter.pop_back();
    std::vector<uint8_t> unknown = same;
    unknown[0] = 'X';
    std::vector<uint8_t> tiny = same;
    std::vector<uint8_t> cut = same;
    std::vector<uint8_t> cut_other = converted;
    const auto store = flavor == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32;
    store(&tiny[0x0C], 8);
    store(&cut[0x0C], 0x20);
    (other == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32)(&cut_other[0x0C], 0x20);
    const std::vector<uint8_t> ctr = sar_files::Wave(sar::Flavor::kCtr, spec);
    std::vector<uint8_t> big_ctr = ctr;
    big_ctr[4] = 0xFE;
    big_ctr[5] = 0xFF;
    const std::string console = hokuto::song::ConsoleName(flavor);

    const IncomingWave read = hokuto::edit::ReadIncomingWaveFile(longer, *before, kSecondWaves);
    const IncomingWave from_other = hokuto::edit::ReadIncomingWaveFile(converted, *before, kSecondWaves);
    const IncomingWave from_ctr = hokuto::edit::ReadIncomingWaveFile(ctr, *before, kSecondWaves);
    const std::vector<uint8_t> waves =
        hokuto::edit::WaveArchiveWith(*before, kSecondWaves, std::nullopt, from_other.bytes);
    const File imported = hokuto::edit::ReplaceFile(file, kSecondWavesFile, waves);
    const std::shared_ptr<const sar::SoundArchive> after = Read(hokuto::format::ExportArchive(imported));
    const sar::Pcm decoded = hokuto::song::DecodeItemWave(*after, kSecondWaves, 1);

    const auto refusal = [&](const std::vector<uint8_t>& bytes)
    {
        return RefusalOf([&] { hokuto::edit::ReadIncomingWaveFile(bytes, *before, kSecondWaves); });
    };

    // The other console's 16-bit samples, in its byte order.
    const std::vector<int16_t> samples = other == sar::Flavor::kCafe
                                             ? std::vector<int16_t>{0x0102, 0x0304, 0x0506, 0x0708}
                                             : std::vector<int16_t>{0x0201, 0x0403, 0x0605, 0x0807};
    XSFLIB_CHECK(read.bytes == same && read.version == 0x00010100 && !read.converted);
    XSFLIB_CHECK(read.encoding == sar::WaveEncoding::kPcm16 && read.channels == 1 && read.rate == 24000);
    XSFLIB_CHECK(read.frames == 4 && read.loop && read.loop_start == 1);
    XSFLIB_CHECK(from_other.converted && from_other.converted->flavor == other &&
                 from_other.converted->version == 0x00010100 && from_other.converted->size == converted.size() &&
                 !from_other.converted->wave_version);
    XSFLIB_CHECK(from_other.version == 0x00010101 && sar::WaveFile::Read(from_other.bytes).version == 0x00010101);
    XSFLIB_CHECK(sar::WaveFile::Read(from_other.bytes).flavor == flavor && from_other.frames == 4);
    XSFLIB_CHECK(sar::WaveArchiveFile::Read(waves).waves.size() == 2 &&
                 sar::WaveArchiveFile::Read(waves).waves[1] == from_other.bytes);
    XSFLIB_CHECK(FileOf(*after, kSecondWavesFile) == waves &&
                 FileOf(*after, kFirstWavesFile) == FileOf(*before, kFirstWavesFile));
    XSFLIB_CHECK(decoded.sample_rate == 24000 && decoded.loop && decoded.loop_start == 1 && decoded.loop_end == 4);
    XSFLIB_CHECK((decoded.channels == std::vector<std::vector<int16_t>>{samples}));
    XSFLIB_CHECK(refusal({1, 2, 3}) == "It isn't a wave file.");
    XSFLIB_CHECK(refusal(Bank(flavor, {{kFirstWavesId, 0}}, {0})) ==
                 "It's a bank file of the " + hokuto::song::ConsoleName(flavor) + ", not a wave file.");
    XSFLIB_CHECK(refusal(unknown) == "It isn't a wave file.");
    sar::Options options;
    options.flavor = flavor;
    XSFLIB_CHECK(from_ctr.converted && from_ctr.converted->flavor == sar::Flavor::kCtr &&
                 from_ctr.converted->size == ctr.size() && from_ctr.version == 0x00010101);
    XSFLIB_CHECK((sar::DecodeWave(from_ctr.bytes, options).channels ==
                  std::vector<std::vector<int16_t>>{{0x0201, 0x0403, 0x0605, 0x0807}}));
    XSFLIB_CHECK(refusal(big_ctr) == "It isn't a wave file.");
    XSFLIB_CHECK(refusal(shorter) == "Its header gives a size of " + std::to_string(same.size()) +
                                         " bytes. The file holds " + std::to_string(shorter.size()) + " bytes.");
    XSFLIB_CHECK(refusal(tiny) ==
                 "Its header gives a size of 8 bytes. The file holds " + std::to_string(same.size()) + " bytes.");
    XSFLIB_CHECK(refusal(cut).starts_with("Hokuto can't read the wave. "));
    XSFLIB_CHECK(refusal(cut_other).starts_with("Hokuto can't convert it for the " + console + ". "));
}

// A wave file takes the place of the first wave archive's second wave. A wave file of the archive's console goes in as
// it is. A wave file of the other console goes in converted, with the present wave's version: 1.1.2 here. The wave
// archive's first wave holds 1.1.0. A new wave from the same file takes 1.1.0. A wave file of the archive's console
// with a later version goes in with those versions too, and keeps its samples. Replace… refuses a wave that the archive
// lacks. A WAV file isn't a wave file.
void TestWaveFileInPlace(sar::Flavor flavor)
{
    // The second wave of the first wave archive holds version 1.1.2.
    const sar::Flavor other = OtherConsole(flavor);
    std::vector<uint8_t> bytes = TestArchive(flavor);
    const std::shared_ptr<const sar::SoundArchive> tables = Read(bytes);
    const std::size_t at = tables->FileOffset(kFirstWavesFile).value_or(0);
    const std::vector<uint8_t> first = FileOf(*tables, kFirstWavesFile);
    const std::span<const uint8_t> second = sar::WaveArchive::Parse(first, tables->ReadOptions()).waves.at(1);
    (flavor == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32)(
        &bytes[at + static_cast<std::size_t>(second.data() - first.data()) + 0x08], 0x00010102);
    const std::shared_ptr<const sar::SoundArchive> archive = Read(bytes);
    const std::vector<uint8_t> same = Wave(flavor, 7);
    const std::vector<uint8_t> converted = Wave(other, 7);
    seqlib::Pcm pcm;
    pcm.channels = {{1, 2, 3}};

    const auto refusal = [&](uint32_t wave_archive, uint32_t wave)
    {
        return RefusalOf([&] { hokuto::edit::ReadIncomingWaveFile(same, *archive, wave_archive, wave); });
    };

    const IncomingWave in_place = hokuto::edit::ReadIncomingWaveFile(same, *archive, kFirstWaves, 1);
    const IncomingWave from_other = hokuto::edit::ReadIncomingWaveFile(converted, *archive, kFirstWaves, 1);
    const IncomingWave new_wave = hokuto::edit::ReadIncomingWaveFile(converted, *archive, kFirstWaves);
    std::vector<uint8_t> later = same;
    (flavor == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32)(&later[0x08], 0x00010103);
    const IncomingWave later_in_place = hokuto::edit::ReadIncomingWaveFile(later, *archive, kFirstWaves, 1);
    const IncomingWave later_new = hokuto::edit::ReadIncomingWaveFile(later, *archive, kFirstWaves);
    const std::vector<uint8_t> waves = hokuto::edit::WaveArchiveWith(*archive, kFirstWaves, 1, from_other.bytes);

    const sar::Options options = hokuto::song::ItemOptions(*archive);
    XSFLIB_CHECK(in_place.bytes == same && in_place.version == 0x00010100 && !in_place.converted);
    XSFLIB_CHECK(from_other.converted && from_other.converted->flavor == other && from_other.version == 0x00010102);
    XSFLIB_CHECK(sar::WaveFile::Read(from_other.bytes).version == 0x00010102 && new_wave.version == 0x00010100);
    XSFLIB_CHECK(sar::DecodeWave(from_other.bytes, options).channels == sar::DecodeWave(same, options).channels);
    XSFLIB_CHECK(later_in_place.version == 0x00010102 && later_in_place.converted &&
                 later_in_place.converted->flavor == flavor && later_in_place.converted->version == 0x00010103);
    XSFLIB_CHECK(sar::WaveFile::Read(later_in_place.bytes).version == 0x00010102 &&
                 sar::DecodeWave(later_in_place.bytes, options).channels == sar::DecodeWave(same, options).channels);
    XSFLIB_CHECK(later_new.version == 0x00010100 && later_new.converted &&
                 sar::WaveFile::Read(later_new.bytes).version == 0x00010100);
    XSFLIB_CHECK((sar::WaveArchiveFile::Read(waves).waves ==
                  std::vector<std::vector<uint8_t>>{Wave(flavor, 1), from_other.bytes}));
    XSFLIB_CHECK(refusal(kFirstWaves, 2) == "The wave archive holds 2 waves. It has no wave 2.");
    XSFLIB_CHECK(refusal(2, 0) == "The archive has no wave archive there.");
    XSFLIB_CHECK(hokuto::edit::IsWaveFile(same) && hokuto::edit::IsWaveFile(converted));
    XSFLIB_CHECK(!hokuto::edit::IsWaveFile(Bank(flavor, {{kFirstWavesId, 0}}, {0})));
    XSFLIB_CHECK(!hokuto::edit::IsWaveFile(seqlib::WavFile(pcm, 8000)) &&
                 !hokuto::edit::IsWaveFile(std::span<const uint8_t>(same).first(3)));
}

// The copies of file `id` in the groups of `archive`, an archive of the Wii, and of its wave data with `wave_data`.
std::vector<std::vector<uint8_t>> WiiCopies(std::span<const uint8_t> archive, uint32_t id, bool wave_data)
{
    const ntdlib::rsar::SoundArchive read = ntdlib::rsar::SoundArchive::Load(
        std::vector<uint8_t>(archive.begin(), archive.end()), {.playback_fields = true});
    std::vector<std::vector<uint8_t>> copies;
    for (const ntdlib::rsar::SoundArchive::Group& group : read.Groups())
    {
        for (const ntdlib::rsar::SoundArchive::Group::Item& item : group.items)
        {
            if (item.file_id == id)
            {
                const std::size_t start =
                    wave_data ? group.wave_data_offset + item.wave_data_offset : group.offset + item.offset;
                const std::size_t size = wave_data ? item.wave_data_size : item.size;
                copies.emplace_back(archive.begin() + start, archive.begin() + start + size);
            }
        }
    }

    return copies;
}

// A Wii archive holds a song, and a bank with its wave data in both of its groups. A new bank of 0x50 bytes takes the
// old bank's place in both, with zeros up to 0x60, and keeps the wave data. New wave data then takes the place of the
// bank's, and the bank keeps its bytes. The song stays as it was. A new file in the song's place is refused: the song's
// file lies among the archive's other bytes. Wave data for a file of a later console is refused too.
void TestWiiReplace()
{
    ntdlib::rsar::SequenceFile song;
    song.data = {0xC1, 0x64, 0x3C, 0x64, 0x30, 0x80, 0x30, 0xFF};
    song.labels = {{"SEQ_SONG", 0}};
    const std::vector<uint8_t> kBankBytes(0x40, 0x5A);
    const std::vector<uint8_t> kWaveBytes(0x20, 0xA5);
    const std::vector<uint8_t> original = ntdlib::testing::rsar_tools::MakeArchive(
        0x0104, {{song.Write()}, {kBankBytes, kWaveBytes}}, {{"SEQ_SONG", 0, 0}}, {{0, 1}, {1}});
    const File file = hokuto::format::ImportWiiArchive(original);

    const std::vector<uint8_t> kNewBankBytes(0x50, 0x3C);
    std::vector<uint8_t> padded = kNewBankBytes;
    padded.resize(0x60, 0);
    const std::vector<uint8_t> kNewWaveBytes(0x40, 0x7E);
    const File replaced = hokuto::edit::ReplaceFile(file, 1, kNewBankBytes);
    const File rewaved = hokuto::edit::ReplaceFile(replaced, 1, padded, kNewWaveBytes);
    const std::vector<uint8_t> after = hokuto::format::ExportArchive(replaced);
    const std::vector<uint8_t> after_waves = hokuto::format::ExportArchive(rewaved);

    const auto refusal = [](const File& from, uint32_t id, std::span<const uint8_t> bytes)
    {
        return RefusalOf([&] { hokuto::edit::ReplaceFile(from, id, bytes, bytes); });
    };

    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == original);
    XSFLIB_CHECK((WiiCopies(after, 1, false) == std::vector<std::vector<uint8_t>>(2, padded)));
    XSFLIB_CHECK((WiiCopies(after, 1, true) == std::vector<std::vector<uint8_t>>(2, kWaveBytes)));
    XSFLIB_CHECK((WiiCopies(after_waves, 1, false) == std::vector<std::vector<uint8_t>>(2, padded)));
    XSFLIB_CHECK((WiiCopies(after_waves, 1, true) == std::vector<std::vector<uint8_t>>(2, kNewWaveBytes)));
    XSFLIB_CHECK(after.size() == original.size() + 2 * 0x20 && after_waves.size() == after.size() + 2 * 0x20);
    XSFLIB_CHECK(WiiCopies(after_waves, 0, false) == WiiCopies(original, 0, false));
    XSFLIB_CHECK(refusal(file, 0, kBankBytes).starts_with("The old file lies among the archive's other bytes."));
    XSFLIB_CHECK(refusal(hokuto::format::ImportArchive(TestArchive(sar::Flavor::kNx), sar::Flavor::kNx), kBankFile,
                         kBankBytes) == "Only a file of the Wii has wave data beside it.");
}

// A stereo WAV file with a loop from sample 2 to 5 and one sample after it goes in as a wave of the Wii: 16-bit PCM,
// big-endian, with the loop and the rate, in an RWAV file of the given version. As DSP-ADPCM, the loop moves to the
// next frame of 14 samples, and the samples between repeat after the loop's end. The model keeps the samples before the
// encoding.
void TestWiiWave()
{
    seqlib::Pcm pcm;
    pcm.channels = {{100, -200, 300, -400, 500, -600}, {1, 2, 3, 4, 5, 6}};
    pcm.loop = true;
    pcm.loop_start = 2;
    pcm.loop_end = 5;
    const std::vector<uint8_t> wav = seqlib::WavFile(pcm, 22050);

    const IncomingWave pcm16 = hokuto::edit::ReadIncomingWiiWave(wav, 0x0102, sar::WaveEncoding::kPcm16);
    const ntdlib::rsar::WaveFile read = ntdlib::rsar::WaveFile::Read(pcm16.bytes);
    XSFLIB_CHECK(read.version == 0x0102 && read.encoding == ntdlib::rsar::WaveEncoding::kPcm16 && read.loop);
    XSFLIB_CHECK(read.sample_rate == 22050 && read.loop_start == 2 && read.loop_end == 5 && read.channels.size() == 2);
    XSFLIB_CHECK(read.channels[0].samples.size() >= 10 && read.channels[0].samples[0] == 0x00 &&
                 read.channels[0].samples[1] == 0x64 && !read.channels[0].adpcm);
    XSFLIB_CHECK(
        pcm16.version == 0x0102 && pcm16.frames == 5 && pcm16.after_loop == 1 && pcm16.model &&
        (pcm16.model->samples == std::vector<std::vector<int16_t>>{{100, -200, 300, -400, 500}, {1, 2, 3, 4, 5}}));

    const IncomingWave adpcm = hokuto::edit::ReadIncomingWiiWave(wav, 0x0102);
    const ntdlib::rsar::WaveFile encoded = ntdlib::rsar::WaveFile::Read(adpcm.bytes);
    XSFLIB_CHECK(encoded.encoding == ntdlib::rsar::WaveEncoding::kDspAdpcm && encoded.loop_start == 14 &&
                 adpcm.loop_moved == 12 && encoded.channels.size() == 2 && encoded.channels[1].adpcm);
    XSFLIB_CHECK(adpcm.model && adpcm.model->loop_start == 14 && adpcm.model->length == encoded.loop_end);
    XSFLIB_CHECK(RefusalOf([&] { hokuto::edit::ReadIncomingWiiWave(wav, 0x0102, sar::WaveEncoding::kPcm8); }) ==
                 "Hokuto makes a wave in DSP-ADPCM or in 16-bit PCM only.");
}

// A file's bytes with zeros up to a multiple of 32 bytes. NW4R's tools lay a file out that way in an archive of the
// Wii.
std::vector<uint8_t> PaddedForWii(std::vector<uint8_t> bytes)
{
    bytes.resize((bytes.size() + 31) & ~std::size_t{31}, 0);

    return bytes;
}

// A Wii archive as NW4R's tools lay one out: a song, a bank of version 1.2 with an RWAR of two waves, and a wave sound
// file of version 1.3 with an RWAR of one wave. Two groups hold the bank.
std::vector<uint8_t> WiiItemsArchive()
{
    namespace rsar = ntdlib::rsar;
    namespace rsar_files = ntdlib::testing::rsar_files;
    rsar::SequenceFile song;
    song.data = {0xFF};
    song.labels = {{"SEQ_SONG", 0}};
    rsar_files::WaveSoundSpec spec;
    spec.notes.push_back({});
    rsar::ArchiveModel model;
    model.players = {{"PLAYER_TEST", 1, 0}};
    model.sounds.resize(2);
    model.sounds[0].name = "SEQ_SONG";
    model.sounds[1] = {.name = "WSD_ONE", .type = rsar::SoundType::kWaveSound, .file_id = 2};
    model.banks = {{"BANK_TEST", 1}};
    model.groups = {{"GROUP_A", {0, 1, 2}}, {"GROUP_B", {1}}};
    model.files = {{PaddedForWii(song.Write())},
                   {PaddedForWii(rsar_files::MakeNewBank()), PaddedForWii(rsar_files::MakeRwar())},
                   {PaddedForWii(rsar_files::MakeWaveSoundFile(0x0103, {spec})),
                    PaddedForWii(rsar_files::MakeRwarOf({rsar_files::MakeRwav(0x33)}))}};

    return rsar::BuildArchive(model);
}

// The archive that `from` exports, read as a game of the Wii reads it.
std::shared_ptr<const ntdlib::rsar::SoundArchive> ReadWii(const File& from)
{
    return hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(from), "test.brsar",
                                        ntdlib::rvl::snd::kRsarOptions);
}

// The archive of WiiItemsArchive. The bank's file goes in again, and the archive comes back. A bank of version 1.1
// describes its waves in a WAVE block. Such a bank can't take the place of a bank with an RWAR. An RWAR of one wave
// takes the place of the bank's RWAR in both groups, and the bank names a wave past its end. A bank of the Switch is
// refused, and so is a bank read as a wave sound file.
void TestWiiFiles()
{
    namespace rsar = ntdlib::rsar;
    namespace rsar_files = ntdlib::testing::rsar_files;
    const std::vector<uint8_t> bank = rsar_files::MakeNewBank();
    const std::vector<uint8_t> original = WiiItemsArchive();
    const File file = hokuto::format::ImportWiiArchive(original);

    const std::shared_ptr<const rsar::SoundArchive> before = ReadWii(file);

    // The items' files, and the sounds that play them.
    XSFLIB_CHECK(hokuto::edit::FileIdOf(*before, FileKind::kBank, 0) == 1);
    XSFLIB_CHECK(hokuto::edit::FileIdOf(*before, FileKind::kWaveArchive, 2) == 2);
    XSFLIB_CHECK(hokuto::edit::FileIdOf(*before, FileKind::kWaveSoundFile, 1) == 2);
    XSFLIB_CHECK(RefusalOf([&] { hokuto::edit::FileIdOf(*before, FileKind::kWaveArchive, 0); }) ==
                 "The archive has no wave archive there.");
    XSFLIB_CHECK((hokuto::edit::SoundsPlaying(*before, FileKind::kBank, 0) == std::vector<uint32_t>{0}));
    XSFLIB_CHECK((hokuto::edit::SoundsPlaying(*before, FileKind::kWaveArchive, 1) == std::vector<uint32_t>{0}));
    XSFLIB_CHECK((hokuto::edit::SoundsPlaying(*before, FileKind::kWaveSoundFile, 1) == std::vector<uint32_t>{1}));

    // The same bank, and a bank with a WAVE block.
    const IncomingFile same = hokuto::edit::ReadWiiIncomingFile(bank, FileKind::kBank);
    XSFLIB_CHECK(same.version == 0x0102 && same.bytes == bank);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::ReplaceWiiFile(file, *before, 0, same)) == original);
    const FileChanges unchanged = hokuto::edit::ChangesOf(file, *before, 0, same);
    XSFLIB_CHECK(unchanged.lacked.empty() && unchanged.programs.empty() && unchanged.sharing.empty() &&
                 unchanged.version == 0x0102 && unchanged.size == bank.size());
    const IncomingFile old = hokuto::edit::ReadWiiIncomingFile(rsar_files::MakeOldBank(), FileKind::kBank);
    XSFLIB_CHECK(RefusalOf([&] { hokuto::edit::ReplaceWiiFile(file, *before, 0, old); })
                     .starts_with("The present bank keeps its waves in an RWAR beside it."));

    // An RWAR of one wave beside the bank.
    const std::vector<uint8_t> one = rsar_files::MakeRwarOf({rsar_files::MakeRwav(0x34)});
    const IncomingFile waves = hokuto::edit::ReadWiiIncomingFile(one, FileKind::kWaveArchive);
    const FileChanges changes = hokuto::edit::ChangesOf(file, *before, 1, waves);
    XSFLIB_CHECK(changes.count == 1 && (changes.past_banks == std::vector<uint32_t>{0}) && changes.past_sounds.empty());
    const File rewaved = hokuto::edit::ReplaceWiiFile(file, *before, 1, waves);
    const std::shared_ptr<const rsar::SoundArchive> after = ReadWii(rewaved);
    XSFLIB_CHECK(std::ranges::equal(after->WaveData(1), PaddedForWii(one)));
    XSFLIB_CHECK(std::ranges::equal(after->FileData(1), PaddedForWii(bank)));
    XSFLIB_CHECK(std::ranges::equal(after->WaveData(2), before->WaveData(2)));
    for (const sar::FileCopy& copy :
         rsar::FileCopies(hokuto::format::BuildOfWiiArchive(*rewaved.native), hokuto::format::CopySequences(rewaved),
                          *rewaved.native->pieces, rewaved.native->fields, 1, true))
    {
        XSFLIB_CHECK(copy.bytes == PaddedForWii(one));
    }
    const FileChanges past = hokuto::edit::ChangesOf(rewaved, *after, 0, same);
    SEQLIB_REQUIRE(past.lacked.size() == 1);
    XSFLIB_CHECK((past.lacked[0].waves == std::vector<uint32_t>{1}));

    // Files of another console, and a bank read as a wave sound file.
    const auto read_nx_bank = []
    {
        hokuto::edit::ReadWiiIncomingFile(Bank(sar::Flavor::kNx, {{kFirstWavesId, 0}}, {0}), FileKind::kBank);
    };
    XSFLIB_CHECK(RefusalOf(read_nx_bank).starts_with("It's a bank file of the 3DS, the Wii U or the Switch."));
    XSFLIB_CHECK(RefusalOf([&] { hokuto::edit::ReadWiiIncomingFile(bank, FileKind::kWaveSoundFile); }) ==
                 "It's a bank file of the Wii, not a wave sound file of the Wii.");
}

// A wave of the Wii in 16-bit PCM at `rate`, of one channel with `samples`.
ntdlib::rsar::WaveFile WiiPcm16(const std::vector<int16_t>& samples, uint32_t rate)
{
    ntdlib::rsar::WaveFile made;
    made.encoding = ntdlib::rsar::WaveEncoding::kPcm16;
    made.sample_rate = rate;
    made.loop_end = static_cast<uint32_t>(samples.size());
    ntdlib::rsar::WaveFile::Channel& channel = made.channels.emplace_back();
    for (const int16_t sample : samples)
    {
        channel.samples.push_back(static_cast<uint8_t>(static_cast<uint16_t>(sample) >> 8));
        channel.samples.push_back(static_cast<uint8_t>(sample & 0xFF));
    }

    return made;
}

// An archive of the Wii: a song, a bank with two waves, and a wave sound file with one. Two groups hold the bank. With
// `old`, both describe their waves in WAVE blocks: a bank of version 1.1 and a wave sound file of version 1.2.
// Otherwise they keep their waves in RWARs: a bank of 1.2 and a wave sound file of 1.3. ntdlib lays both out as the
// tools do. `bank` and `wave_sounds` give the two files.
std::vector<uint8_t> ModelWiiItemsArchive(bool old, ntdlib::rsar::BankFile::Files& bank,
                                          ntdlib::rsar::WsdFile::Files& wave_sounds)
{
    namespace rsar = ntdlib::rsar;
    rsar::BankFile::Instrument instrument;
    instrument.key_regions = {{0, 127, {{0, 127, rsar::Region{}}}}};
    const std::vector<std::optional<rsar::BankFile::Instrument>> instruments = {instrument};
    bank = rsar::BankFile::FromInstruments(old ? 0x0101 : 0x0102, instruments,
                                           {WiiPcm16({1, 2, 3}, 32000), WiiPcm16({4, 5}, 32000)})
               .Write();
    rsar::WsdFile sounds;
    sounds.version = old ? 0x0102 : 0x0103;
    rsar::WsdFile::WaveSound& sound = sounds.sounds.emplace_back();
    sound.tracks = {{{{0.0f, 0.0f, 0}}}};
    sound.notes = {{.wave_index = 0}};
    sounds.waves = {WiiPcm16({7, 8}, 32000)};
    wave_sounds = sounds.Write();
    rsar::SequenceFile song;
    song.data = {0xFF};
    song.labels = {{"SEQ_SONG", 0}};
    rsar::ArchiveModel model;
    model.sounds = {{.name = "SEQ_SONG", .file_id = 0},
                    {.name = "WSD_ONE", .type = rsar::SoundType::kWaveSound, .file_id = 2}};
    model.banks = {{.name = "BANK_TEST", .file_id = 1}};
    model.players = {{.name = "PLAYER_TEST"}};
    model.groups = {{.name = "GROUP_A", .file_ids = {0, 1, 2}}, {.name = "GROUP_B", .file_ids = {1}}};
    model.files = {{.bytes = song.Write()},
                   {.bytes = bank.file, .wave_data = bank.wave_data},
                   {.bytes = wave_sounds.file, .wave_data = wave_sounds.wave_data}};

    return rsar::BuildArchive(model);
}

// A wave of the wave data of a file of the Wii takes a wave file of the Wii in its place, and a WAV file's wave goes
// after the waves. The archive's bank keeps two waves in an RWAR, and its program 0 plays the first. Its wave sound
// file keeps the one wave that its wave sound plays. The other waves keep their values, and the bank's file and the
// wave sound file's wave data keep their bytes. The new wave takes the present wave's version. A wave file of another
// console goes in converted. A new wave plays through a preview with its samples.
void TestWiiWaves()
{
    namespace rsar = ntdlib::rsar;
    rsar::BankFile::Files bank;
    rsar::WsdFile::Files wave_sounds;
    const File file = hokuto::format::ImportWiiArchive(ModelWiiItemsArchive(false, bank, wave_sounds));

    const std::shared_ptr<const rsar::SoundArchive> before = ReadWii(file);
    const std::vector<rsar::WaveFile> present = hokuto::edit::WavesOf(*before, 1);

    // The present waves, and what plays them.
    const hokuto::edit::WaveChanges changes = hokuto::edit::ChangesOfWiiWave(*before, 1, 0);
    XSFLIB_CHECK(changes.channels == 1 && changes.rate == 32000 && !changes.loop && changes.loop_end == 3 &&
                 changes.encoding == sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK((changes.banks == std::vector<uint32_t>{0}) && changes.sounds.empty());
    XSFLIB_CHECK(hokuto::edit::ChangesOfWiiWave(*before, 1, 1).banks.empty());
    const hokuto::edit::WaveChanges played = hokuto::edit::ChangesOfWiiWave(*before, 2, 0);
    XSFLIB_CHECK(played.banks.empty() && (played.sounds == std::vector<uint32_t>{1}));

    // A wave file of the Wii in the place of the bank's second wave.
    rsar::WaveFile other = WiiPcm16({9, 10, 11, 12}, 22050);
    other.version = 0x0100;
    const std::vector<uint8_t> rwav = other.Write();
    const hokuto::edit::IncomingWave incoming = hokuto::edit::ReadIncomingWiiWaveFile(rwav);
    XSFLIB_CHECK(incoming.bytes == rwav && incoming.version == 0x0100 && incoming.channels == 1 &&
                 incoming.rate == 22050 && incoming.frames == 4 && !incoming.loop);
    const std::shared_ptr<const rsar::SoundArchive> after =
        ReadWii(hokuto::edit::WithIncomingWiiWave(file, *before, 1, 1, incoming));
    const std::vector<rsar::WaveFile> waves = hokuto::edit::WavesOf(*after, 1);
    SEQLIB_REQUIRE(present.size() > 1);
    other.version = present[1].version;
    XSFLIB_CHECK(waves.size() == 2 && waves[0] == present[0] && waves[1] == other);
    XSFLIB_CHECK(std::ranges::equal(after->FileData(1), before->FileData(1)));
    XSFLIB_CHECK(std::ranges::equal(after->WaveData(2), before->WaveData(2)));

    // A WAV file's wave in 16-bit PCM after the waves.
    seqlib::Pcm pcm;
    pcm.channels = {{100, -200, 300}};
    const hokuto::edit::IncomingWave pcm16 =
        hokuto::edit::ReadIncomingWiiWave(seqlib::WavFile(pcm, 16000), 0x0102, sar::WaveEncoding::kPcm16);
    const std::vector<rsar::WaveFile> three =
        hokuto::edit::WavesOf(*ReadWii(hokuto::edit::WithIncomingWiiWave(file, *before, 1, std::nullopt, pcm16)), 1);
    XSFLIB_CHECK(three.size() == 3 && three[0] == present[0] && three[1] == present[1]);
    XSFLIB_CHECK(three[2].sample_rate == 16000 &&
                 (hokuto::edit::DecodedSamples(three[2], 3) == std::vector<std::vector<int16_t>>{{100, -200, 300}}));

    // The Replace and Import windows' Play plays a new wave of the Wii through a preview: the wave's samples at its
    // rate, up to its length. A preview without the length played nothing.
    const ntdlib::wave::Pcm previewed = hokuto::edit::WavePcm(rsar::WaveFile::Read(pcm16.bytes));
    hokuto::song::WavePreview preview(previewed);
    std::vector<float> out(8 * 2, 1.0f);
    const std::size_t rendered = preview.Render(out.data(), 8);
    XSFLIB_CHECK(preview.SampleRate() == 16000 && previewed.Length() == 3 && !previewed.loop && rendered == 3);
    XSFLIB_CHECK(out[0] == 100.0f / 32768.0f && out[1] == out[0] && out[2] == -200.0f / 32768.0f &&
                 out[4] == 300.0f / 32768.0f && out[5] == out[4]);
    const ntdlib::wave::Pcm replacing = hokuto::edit::WavePcm(rsar::WaveFile::Read(rwav));
    XSFLIB_CHECK(replacing.sample_rate == 22050 &&
                 (replacing.channels == std::vector<std::vector<int16_t>>{{9, 10, 11, 12}}));
    const rsar::WaveFile looped = rsar::WaveFile::Read(ntdlib::testing::rsar_files::MakeRwav(0x31));
    const ntdlib::wave::Pcm round = hokuto::edit::WavePcm(looped);
    const uint32_t length = std::min(looped.loop_end, hokuto::edit::HeldSamples(looped));
    XSFLIB_CHECK(length > 0 && round.Length() == length && round.loop == (looped.loop_start < length) &&
                 round.loop_start == looped.loop_start &&
                 round.channels == hokuto::edit::DecodedSamples(looped, length));

    // Refusals.
    XSFLIB_CHECK(RefusalOf([&] { hokuto::edit::WithIncomingWiiWave(file, *before, 1, 5, incoming); }) ==
                 "The wave data holds no wave 5.");
    XSFLIB_CHECK(RefusalOf([&] { hokuto::edit::ReadIncomingWiiWaveFile(bank.file); }) ==
                 "It's a bank file of the Wii, not a wave file.");

    // A wave file of the Switch goes in converted, with its samples.
    const std::vector<uint8_t> later = Wave(sar::Flavor::kNx, 1);
    const hokuto::edit::IncomingWave converted = hokuto::edit::ReadIncomingWiiWaveFile(later);
    XSFLIB_CHECK(converted.converted && converted.converted->flavor == sar::Flavor::kNx &&
                 converted.converted->size == later.size());
    const rsar::WaveFile wii = rsar::WaveFile::Read(converted.bytes);
    XSFLIB_CHECK(converted.version == wii.version && converted.channels == 1 && converted.frames == 4);
    XSFLIB_CHECK(hokuto::edit::DecodedSamples(wii, 4) == hokuto::edit::DecodedSamples(sar::WaveFile::Read(later), 4));
}

// The copies of file `file_id` of `file`, a file of the Wii, in the groups of the archive that it builds: of its bytes,
// or of its wave data with `wave_data`.
std::vector<std::vector<uint8_t>> WiiCopiesOf(const File& file, uint32_t file_id, bool wave_data)
{
    std::vector<std::vector<uint8_t>> copies;
    for (const sar::FileCopy& copy :
         ntdlib::rsar::FileCopies(hokuto::format::BuildOfWiiArchive(*file.native), hokuto::format::CopySequences(file),
                                  *file.native->pieces, file.native->fields, file_id, wave_data))
    {
        copies.push_back(copy.bytes);
    }

    return copies;
}

// Files of the 3DS, the Wii U and the Switch go into an archive of the Wii converted. Their magic and byte order mark
// tell them from the Wii's files. A wave archive of the Switch goes in as an RWAR of its waves. A bank of the Switch
// names the waves of its wave archives. WiiFileOf gives it the waves of the Wii archive's wave data in their place: in
// an RWAR for a bank of version 1.2, and in a WAVE block for one of 1.1. A wave sound file of the Switch goes in the
// same way. The converted bank takes the place of the archive's bank with its wave data, in both groups. A bank of the
// Wii takes the wave data of a file of the archive. Into an archive whose bank holds version 1.1, a bank of 1.2 goes in
// written again. An RWAR's waves take the place of the waves of a bank with a WAVE block. The RWAR that Export… writes
// for wave data that a WAVE block describes gives the file and its wave data back. A bank of version 1.1 whose WAVE
// block describes other waves than the present wave data holds takes an RWAR of its waves.
void TestWiiLaterFiles()
{
    namespace rsar = ntdlib::rsar;
    namespace rsar_files = ntdlib::testing::rsar_files;

    // The consoles of the files.
    const std::vector<uint8_t> later_bank = Bank(sar::Flavor::kNx, {{kFirstWavesId, 1}, {kSecondWavesId, 0}}, {0, 2});
    XSFLIB_CHECK(hokuto::edit::LaterConsoleOf(later_bank, FileKind::kBank) == sar::Flavor::kNx);
    XSFLIB_CHECK(hokuto::edit::LaterConsoleOf(Bank(sar::Flavor::kCafe, {{kFirstWavesId, 0}}, {0}), FileKind::kBank) ==
                 sar::Flavor::kCafe);
    XSFLIB_CHECK(hokuto::edit::LaterConsoleOf(Bank(sar::Flavor::kCtr, {{kFirstWavesId, 0}}, {0}), FileKind::kBank) ==
                 sar::Flavor::kCtr);
    XSFLIB_CHECK(!hokuto::edit::LaterConsoleOf(later_bank, FileKind::kWaveArchive));
    XSFLIB_CHECK(!hokuto::edit::LaterConsoleOf(rsar_files::MakeNewBank(), FileKind::kBank));

    // A wave archive of the Switch, as an RWAR of its waves.
    const std::vector<uint8_t> later_waves =
        sar_files::WaveArchive(sar::Flavor::kNx, {Wave(sar::Flavor::kNx, 1), Wave(sar::Flavor::kNx, 2)});
    const IncomingFile rwar = hokuto::edit::ReadWiiIncomingFile(later_waves, FileKind::kWaveArchive);
    XSFLIB_CHECK(rwar.kind == FileKind::kWaveArchive && rwar.converted && rwar.converted->flavor == sar::Flavor::kNx &&
                 rwar.converted->size == later_waves.size() && rwar.wave_data.empty());
    const rsar::WaveArchiveFile rwar_file = rsar::WaveArchiveFile::Read(rwar.bytes);
    XSFLIB_CHECK(rwar_file.waves.size() == 2);
    for (std::size_t i = 0; i < rwar_file.waves.size(); i++)
    {
        const std::vector<uint8_t> wave = Wave(sar::Flavor::kNx, static_cast<uint8_t>(i + 1));
        XSFLIB_CHECK(hokuto::edit::DecodedSamples(rsar::WaveFile::Read(rwar_file.waves[i]), 4) ==
                     hokuto::edit::DecodedSamples(sar::WaveFile::Read(wave), 4));
    }

    // The versions of the archive's files.
    rsar::BankFile::Files bank;
    rsar::WsdFile::Files wave_sounds;
    const File file = hokuto::format::ImportWiiArchive(ModelWiiItemsArchive(false, bank, wave_sounds));
    const std::shared_ptr<const rsar::SoundArchive> before = ReadWii(file);
    XSFLIB_CHECK(std::ranges::equal(before->FileData(1), PaddedForWii(bank.file)) &&
                 std::ranges::equal(before->WaveData(2), PaddedForWii(wave_sounds.wave_data)));
    XSFLIB_CHECK(hokuto::edit::NewWiiFileVersion(*before, FileKind::kBank) == 0x0102 &&
                 hokuto::edit::NewWiiFileVersion(*before, FileKind::kWaveSoundFile) == 0x0103 &&
                 hokuto::edit::NewWiiFileVersion(*before, FileKind::kWaveArchive) == 0x0100);
    XSFLIB_CHECK(hokuto::edit::PresentWiiVersion(*before, FileKind::kBank, 0) == 0x0102 &&
                 hokuto::edit::PresentWiiVersion(*before, FileKind::kWaveSoundFile, 1) == 0x0103);

    // The Switch's bank with the second wave of the bank's wave data, and the wave of the wave sound file's.
    const IncomingFile incoming = hokuto::edit::ReadIncomingFile(later_bank, FileKind::kBank, sar::Flavor::kNx);
    const std::map<uint32_t, uint32_t> kMap = {{kFirstWavesId, 1}, {kSecondWavesId, 2}};
    SEQLIB_REQUIRE(hokuto::edit::WavesOf(*before, 1).size() > 1 && !hokuto::edit::WavesOf(*before, 2).empty());
    const std::vector<rsar::WaveFile> expected = {hokuto::edit::WavesOf(*before, 1)[1],
                                                  hokuto::edit::WavesOf(*before, 2)[0]};
    const IncomingFile converted = hokuto::edit::WiiFileOf(incoming, sar::Flavor::kNx, *before, kMap, 0x0102);
    XSFLIB_CHECK(converted.kind == FileKind::kBank && converted.version == 0x0102 && converted.converted &&
                 converted.converted->flavor == sar::Flavor::kNx && converted.converted->size == later_bank.size());
    const rsar::BankFile wii_bank = rsar::BankFile::Read(converted.bytes, converted.wave_data);
    XSFLIB_CHECK(wii_bank.version == 0x0102 && wii_bank.waves == expected && wii_bank.instruments.size() == 3);
    const IncomingFile old = hokuto::edit::WiiFileOf(incoming, sar::Flavor::kNx, *before, kMap, 0x0101);
    XSFLIB_CHECK(old.version == 0x0101 && rsar::BankFile::Read(old.bytes, old.wave_data).waves == expected &&
                 old.wave_data == rsar::RawWaveData(expected));
    XSFLIB_CHECK(
        RefusalOf([&]
                  { hokuto::edit::WiiFileOf(incoming, sar::Flavor::kNx, *before, {{kFirstWavesId, 1}}, 0x0102); }) ==
        "Choose wave data of the archive for wave archive 1.");

    // A wave sound file of the Switch with the same wave of the bank's wave data.
    const IncomingFile later_sounds = hokuto::edit::ReadIncomingFile(
        WaveSounds(sar::Flavor::kNx, {{kFirstWavesId, 1}}, {0}), FileKind::kWaveSoundFile, sar::Flavor::kNx);
    const IncomingFile wsd =
        hokuto::edit::WiiFileOf(later_sounds, sar::Flavor::kNx, *before, {{kFirstWavesId, 1}}, 0x0103);
    const rsar::WsdFile wii_sounds = rsar::WsdFile::Read(wsd.bytes, wsd.wave_data);
    XSFLIB_CHECK(wsd.kind == FileKind::kWaveSoundFile && wii_sounds.version == 0x0103 &&
                 wii_sounds.sounds.size() == 1 && (wii_sounds.waves == std::vector<rsar::WaveFile>{expected[0]}));

    // The converted bank in the place of the archive's bank, with its wave data.
    const File replaced = hokuto::edit::ReplaceWiiFile(file, *before, 0, converted);
    const std::shared_ptr<const rsar::SoundArchive> after = ReadWii(replaced);
    XSFLIB_CHECK(std::ranges::equal(after->FileData(1), PaddedForWii(converted.bytes)) &&
                 std::ranges::equal(after->WaveData(1), PaddedForWii(converted.wave_data)));
    XSFLIB_CHECK(hokuto::edit::WavesOf(*after, 1) == expected);
    XSFLIB_CHECK(std::ranges::equal(after->FileData(2), before->FileData(2)) &&
                 std::ranges::equal(after->WaveData(2), before->WaveData(2)));
    XSFLIB_CHECK(WiiCopiesOf(replaced, 1, false) ==
                 std::vector<std::vector<uint8_t>>(2, PaddedForWii(converted.bytes)));
    XSFLIB_CHECK(WiiCopiesOf(replaced, 1, true) ==
                 std::vector<std::vector<uint8_t>>(2, PaddedForWii(converted.wave_data)));

    // The converted bank as a bank of the Wii, with the bank's wave data and the wave sound file's. The bank names two
    // waves, and the wave sound file's wave data holds one.
    const IncomingFile wii = hokuto::edit::ReadWiiIncomingFile(converted.bytes, FileKind::kBank);
    const IncomingFile again = hokuto::edit::WiiFileWithWaves(wii, hokuto::edit::WiiWaveDataOf(*after, 1), 0x0102);
    XSFLIB_CHECK(again.bytes == converted.bytes && again.wave_data == PaddedForWii(converted.wave_data) &&
                 !again.converted);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::ReplaceWiiFile(replaced, *after, 0, again)) ==
                 hokuto::format::ExportArchive(replaced));
    XSFLIB_CHECK(
        RefusalOf([&] { hokuto::edit::WiiFileWithWaves(wii, hokuto::edit::WiiWaveDataOf(*after, 2), 0x0102); }) ==
        "The bank names wave 1, and its wave data holds 1 wave.");

    // An archive whose bank and wave sound file describe their waves in WAVE blocks.
    rsar::BankFile::Files old_bank;
    rsar::WsdFile::Files old_sounds;
    const std::vector<uint8_t> old_original = ModelWiiItemsArchive(true, old_bank, old_sounds);
    const File old_file = hokuto::format::ImportWiiArchive(old_original);
    const std::shared_ptr<const rsar::SoundArchive> old_before = ReadWii(old_file);
    XSFLIB_CHECK(hokuto::edit::NewWiiFileVersion(*old_before, FileKind::kBank) == 0x0101 &&
                 hokuto::edit::NewWiiFileVersion(*old_before, FileKind::kWaveSoundFile) == 0x0102);

    // The RWARs that Export… writes for the wave data give the files and their wave data back.
    const std::vector<rsar::WaveFile> old_waves = hokuto::edit::WavesOf(*old_before, 1);
    const hokuto::edit::WiiWaveData exported = hokuto::edit::ReadWiiWaveData(
        hokuto::song::ItemBytes(*old_before, {.kind = hokuto::song::ItemKind::kWaveArchive, .index = 1}));
    XSFLIB_CHECK(exported.rwar && old_waves.size() == 2 && exported.waves == old_waves);
    const IncomingFile bank_back = hokuto::edit::WiiFileWithWaves(
        hokuto::edit::ReadWiiIncomingFile(old_bank.file, FileKind::kBank), exported, 0x0101);
    XSFLIB_CHECK(bank_back.bytes == old_bank.file && bank_back.wave_data == old_bank.wave_data && !bank_back.converted);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::ReplaceWiiFile(old_file, *old_before, 0, bank_back)) ==
                 old_original);
    const hokuto::edit::WiiWaveData sounds_exported = hokuto::edit::ReadWiiWaveData(
        hokuto::song::ItemBytes(*old_before, {.kind = hokuto::song::ItemKind::kWaveArchive, .index = 2}));
    const IncomingFile sounds_back = hokuto::edit::WiiFileWithWaves(
        hokuto::edit::ReadWiiIncomingFile(old_sounds.file, FileKind::kWaveSoundFile), sounds_exported, 0x0102);
    XSFLIB_CHECK(sounds_back.bytes == old_sounds.file && sounds_back.wave_data == old_sounds.wave_data);

    // A bank of version 1.2 goes in written again in 1.1, the present bank's version.
    rsar::BankFile model = rsar::BankFile::Read(old_bank.file, old_bank.wave_data);
    model.version = 0x0102;
    const rsar::BankFile::Files newer = model.Write();
    const IncomingFile rewritten = hokuto::edit::WiiFileWithWaves(
        hokuto::edit::ReadWiiIncomingFile(newer.file, FileKind::kBank), hokuto::edit::WiiWaveDataOf(*old_before, 1),
        hokuto::edit::PresentWiiVersion(*old_before, FileKind::kBank, 0));
    XSFLIB_CHECK(rewritten.version == 0x0101 && rewritten.converted && rewritten.converted->wii &&
                 rewritten.converted->version == 0x0102 && rewritten.converted->size == newer.file.size());
    XSFLIB_CHECK(rewritten.bytes == old_bank.file && rewritten.wave_data == old_bank.wave_data);

    // An RWAR of one wave gives the bank with the WAVE block its waves, in both groups.
    const std::vector<uint8_t> one = rsar_files::MakeRwarOf({rsar_files::MakeRwav(0x34)});
    const File rewaved = hokuto::edit::ReplaceWiiFile(old_file, *old_before, 1,
                                                      hokuto::edit::ReadWiiIncomingFile(one, FileKind::kWaveArchive));
    const std::shared_ptr<const rsar::SoundArchive> old_after = ReadWii(rewaved);
    XSFLIB_CHECK((hokuto::edit::WavesOf(*old_after, 1) ==
                  std::vector<rsar::WaveFile>{rsar::WaveFile::Read(rsar_files::MakeRwav(0x34))}));
    XSFLIB_CHECK(hokuto::edit::PresentWiiVersion(*old_after, FileKind::kBank, 0) == 0x0101);
    const std::span<const uint8_t> new_file = old_after->FileData(1);
    const std::span<const uint8_t> new_data = old_after->WaveData(1);
    XSFLIB_CHECK(WiiCopiesOf(rewaved, 1, false) ==
                 std::vector<std::vector<uint8_t>>(2, std::vector<uint8_t>(new_file.begin(), new_file.end())));
    XSFLIB_CHECK(WiiCopiesOf(rewaved, 1, true) ==
                 std::vector<std::vector<uint8_t>>(2, std::vector<uint8_t>(new_data.begin(), new_data.end())));
    XSFLIB_CHECK(std::ranges::equal(old_after->WaveData(2), old_before->WaveData(2)));

    // A bank of version 1.1 alone in the place of the bank keeps the present wave data. Its WAVE block describes longer
    // waves, and Hokuto can't read it with that wave data. An RWAR of its waves then gives it its waves.
    rsar::BankFile::Instrument instrument;
    instrument.key_regions = {{0, 127, {{0, 127, rsar::Region{}}}}};
    std::vector<int16_t> first(64);
    std::vector<int16_t> second(48);
    for (std::size_t i = 0; i < first.size(); i++)
    {
        first[i] = static_cast<int16_t>(i * 100);
        second[i % second.size()] = static_cast<int16_t>(-static_cast<int>(i));
    }
    const std::vector<rsar::WaveFile> longer = {WiiPcm16(first, 32000), WiiPcm16(second, 32000)};
    const std::vector<std::optional<rsar::BankFile::Instrument>> one_instrument = {instrument};
    const rsar::BankFile::Files other = rsar::BankFile::FromInstruments(0x0101, one_instrument, longer).Write();
    const File alone = hokuto::edit::ReplaceWiiFile(old_file, *old_before, 0,
                                                    hokuto::edit::ReadWiiIncomingFile(other.file, FileKind::kBank));
    const std::shared_ptr<const rsar::SoundArchive> alone_after = ReadWii(alone);
    XSFLIB_CHECK(alone_after->FileData(1).size() == other.file.size() &&
                 std::ranges::equal(alone_after->WaveData(1), old_bank.wave_data));
    XSFLIB_CHECK(RefusalOf([&] { hokuto::edit::WavesOf(*alone_after, 1); }).starts_with("Hokuto can't read the bank."));
    std::vector<std::vector<uint8_t>> rwavs;
    for (const rsar::WaveFile& wave : longer)
    {
        rwavs.push_back(wave.Write());
    }
    const File given = hokuto::edit::ReplaceWiiFile(
        alone, *alone_after, 1,
        hokuto::edit::ReadWiiIncomingFile(rsar_files::MakeRwarOf(rwavs), FileKind::kWaveArchive));
    const std::shared_ptr<const rsar::SoundArchive> given_after = ReadWii(given);
    XSFLIB_CHECK(hokuto::edit::WavesOf(*given_after, 1) == longer);
    XSFLIB_CHECK(std::ranges::equal(given_after->FileData(1), other.file) &&
                 std::ranges::equal(given_after->WaveData(1), other.wave_data));
}

} // namespace

int main()
{
    for (const sar::Flavor flavor : {sar::Flavor::kNx, sar::Flavor::kCafe})
    {
        TestRead(flavor);
        TestConversion(flavor);
        TestLaterVersions(flavor);
        TestIncomingVersions(flavor);
        TestChanges(flavor);
        TestSoundsPlaying(flavor);
        TestProgramFixes(flavor);
        TestReplace(flavor);
        TestCopyPlaces(flavor);
        TestWave(flavor);
        TestDspAdpcmWave(flavor);
        TestResampledWave(flavor);
        TestNewWave(flavor);
        TestNewWaveFile(flavor);
        TestWaveFileInPlace(flavor);
    }
    TestWiiReplace();
    TestWiiWave();
    TestWiiFiles();
    TestWiiWaves();
    TestWiiLaterFiles();

    return xsflib::testing::Summary("files");
}
