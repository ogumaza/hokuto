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

#include <numbers>

#include "ntdlib/sar/groups.h"

#include "files_fixtures.h"

namespace
{

// The bytes of file `id` of `archive`.
std::vector<uint8_t> FileOf(const sar::SoundArchive& archive, uint32_t id)
{
    const std::span<const uint8_t> data = archive.FileData(id);
    return {data.begin(), data.end()};
}

// The copies of file `id` in `file`'s exported archive of `flavor`, read from the archive's bytes: the copy that the
// file table names, then each copy that a group file in the archive contains, in the order of the group table. ntdlib
// reads the group table under its strict rules only.
std::vector<std::vector<uint8_t>> CopiesInExport(const File& file, sar::Flavor flavor, uint32_t id)
{
    const sar::SoundArchive archive = sar::SoundArchive::Load(
        hokuto::format::ExportArchive(file), {.flavor = flavor, .rules = sar::Rules::kStopAtFirstProblem});
    std::vector<std::vector<uint8_t>> copies;
    if (!archive.FileData(id).empty())
    {
        copies.push_back(FileOf(archive, id));
    }
    for (const sar::GroupInfo& group : archive.Groups())
    {
        if (group.file_id == sar::kExternalGroupFile)
        {
            continue;
        }
        for (const sar::GroupMember& member : sar::ReadGroupFile(archive.FileData(group.file_id), flavor).files)
        {
            if (member.file == id && member.copy)
            {
                copies.push_back(*member.copy);
            }
        }
    }

    return copies;
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
    constexpr double kPi = std::numbers::pi;
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
        return ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadIncomingFile(bytes, kind, console); });
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
                 "Its header gives a size of " + std::to_string(bank.size()) + " bytes, but the file has " +
                     std::to_string(shorter.size()) + " bytes.");
    XSFLIB_CHECK(refusal(tiny, FileKind::kBank, flavor) ==
                 "Its header gives a size of 8 bytes, but the file has " + std::to_string(bank.size()) + " bytes.");
    XSFLIB_CHECK(refusal(damage(bank), FileKind::kBank, flavor).starts_with("Can't read the file. "));
    XSFLIB_CHECK(refusal(waves, FileKind::kWaveArchive, flavor).starts_with("Can't read the file. "));
    XSFLIB_CHECK(refusal(WaveSounds(flavor, {}, {}), FileKind::kWaveSoundFile, flavor).empty());
    XSFLIB_CHECK(refusal(damage(WaveSounds(flavor, {{kFirstWavesId, 0}}, {0})), FileKind::kWaveSoundFile, flavor)
                     .starts_with("Can't read the file. "));
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
    const std::string refusal = ErrorOf<std::invalid_argument>(
        [&] { hokuto::edit::ReadIncomingFile(big_ctr, FileKind::kBank, flavor, FileVersions{}); });
    const IncomingFile newer =
        hokuto::edit::ReadIncomingFile(bank, FileKind::kBank, flavor, FileVersions{0x00010001, 0});
    std::vector<uint8_t> damaged = bank;
    const xsflib::Reader other_reader(bank, other == sar::Flavor::kCafe);
    (other == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32)(&damaged[other_reader.U32(0x18) + 8 + 4],
                                                                          0x00FFFFF0);
    const std::string unreadable =
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadIncomingFile(damaged, FileKind::kBank, flavor); });

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
    XSFLIB_CHECK(unreadable.starts_with("Can't read the file. "));
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
        return ErrorOf<std::invalid_argument>([&] { hokuto::edit::FileIdOf(*archive, kind, item); });
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
    XSFLIB_CHECK(refusal(FileKind::kBank, 2) == "The archive has no such bank.");
    XSFLIB_CHECK(refusal(FileKind::kWaveArchive, 2) == "The archive has no such wave archive.");
    XSFLIB_CHECK(refusal(FileKind::kWaveSoundFile, kSong) == "The archive has no such wave sound file.");
    XSFLIB_CHECK(refusal(FileKind::kWaveSoundFile, 3) == "The archive has no such wave sound file.");
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
        return ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithProgramFixes(*archive, kBank, next, fixes); });
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
    XSFLIB_CHECK(refusal({{1, std::nullopt}}).starts_with("Can't copy the instruments into the new bank."));
    XSFLIB_CHECK(refusal({{0, 3}, {0, std::nullopt}}).starts_with("Can't copy the instruments"));
    XSFLIB_CHECK(refusal({{0, 2}}).starts_with("Can't copy the instruments"));
    XSFLIB_CHECK(refusal({{1, 1}}).starts_with("Can't copy the instruments"));
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
        return ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReplaceFile(from, id, bank); });
    };

    const auto copies = [&](uint32_t id)
    {
        return CopiesInExport(new_waves, flavor, id);
    };

    XSFLIB_CHECK(FileOf(*after, kBankFile) == bank && FileOf(*after, kFirstWavesFile) == waves);
    XSFLIB_CHECK(copies(kBankFile).size() == 2 && copies(kFirstWavesFile).size() == 2);
    for (const std::vector<uint8_t>& copy : copies(kBankFile))
    {
        XSFLIB_CHECK(copy == bank);
    }
    for (const std::vector<uint8_t>& copy : copies(kFirstWavesFile))
    {
        XSFLIB_CHECK(copy == waves);
    }
    for (const uint32_t id : {kSequenceFile, kSecondWavesFile, kWaveSoundsFile})
    {
        XSFLIB_CHECK(FileOf(*after, id) == FileOf(*before, id));
    }
    XSFLIB_CHECK(hokuto::format::ExportArchive(file) == original && *file.native->pieces != *new_bank.native->pieces);
    XSFLIB_CHECK(refusal(without, kBankFile) == "The file contains no game archive.");
    XSFLIB_CHECK(refusal(one_run, kBankFile).starts_with("The old file is mixed in with other data in the archive."));
    XSFLIB_CHECK(refusal(shared, kFirstWavesFile) ==
                 "Another file in the archive shares the same bytes. That file would change too.");
    XSFLIB_CHECK(refusal(file, kOutsideFile) == "The old file is outside the archive. Can't replace it there.");
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
        return CopiesInExport(new_sounds, flavor, id);
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
    XSFLIB_CHECK(other_changes.other_copies == 1 && other_changes.placed_copies == 0);
    const std::vector<std::vector<uint8_t>> other_copies = CopiesInExport(other_bank, flavor, kBankFile);
    XSFLIB_CHECK(other_copies.size() == 2);
    for (const std::vector<uint8_t>& copy : other_copies)
    {
        XSFLIB_CHECK(copy == bank);
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
        return ErrorOf<std::invalid_argument>([&]
                                              { hokuto::edit::ReadIncomingWave(bytes, *before, wave_archive, wave); });
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
    const std::vector<std::vector<uint8_t>> wave_copies = CopiesInExport(replaced, flavor, kFirstWavesFile);
    XSFLIB_CHECK(wave_copies.size() == 2);
    for (const std::vector<uint8_t>& copy : wave_copies)
    {
        XSFLIB_CHECK(copy == waves);
    }
    XSFLIB_CHECK(decoded.sample_rate == 22050 && decoded.loop && decoded.loop_start == 2 && decoded.loop_end == 5);
    XSFLIB_CHECK((decoded.channels == std::vector<std::vector<int16_t>>{{100, -200, 300, -400, 500}, {1, 2, 3, 4, 5}}));
    XSFLIB_CHECK(refusal({1, 2, 3}, kFirstWaves, 1) == "Can't read it as a WAV file: not a RIFF WAVE file.");
    XSFLIB_CHECK(refusal(seqlib::WavFile(three, 8000), kFirstWaves, 1) ==
                 "It has 3 channels. A wave can have only one or two.");
    XSFLIB_CHECK(refusal(seqlib::WavFile(empty, 8000), kFirstWaves, 1) == "It has no samples.");
    XSFLIB_CHECK(refusal(wav, kFirstWaves, 2) == "The wave archive has 2 waves. There's no wave 2.");
    XSFLIB_CHECK(refusal(wav, 2, 0) == "The archive has no such wave archive.");
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::WaveArchiveWith(*before, 1, 1, incoming.bytes); }) ==
        "The wave archive has 1 wave. There's no wave 1.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::edit::ReadIncomingWave(wav, *before, kFirstWaves, 1, sar::WaveEncoding::kPcm8); }) ==
                 "Hokuto can encode waves only as DSP-ADPCM or 16-bit PCM.");
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
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { read(wav, hokuto::edit::WaveLoop{0, 4801}, std::nullopt); }) ==
                 "The loop from sample 0 to 4801 isn't valid for the 4800 samples of the WAV file.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { read(wav, hokuto::edit::WaveLoop{1200, 1200}, std::nullopt); }) ==
                 "The loop from sample 1200 to 1200 isn't valid for the 4800 samples of the WAV file.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { read(wav, std::nullopt, 48000); }) ==
                 "The new sample rate must be lower than the WAV file's 48000 Hz.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { read(wav, std::nullopt, 0); }) ==
                 "The new sample rate must be lower than the WAV file's 48000 Hz.");
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
    const std::vector<std::vector<uint8_t>> wave_copies = CopiesInExport(imported, flavor, kFirstWavesFile);
    XSFLIB_CHECK(wave_copies.size() == 2);
    for (const std::vector<uint8_t>& copy : wave_copies)
    {
        XSFLIB_CHECK(copy == waves);
    }
    XSFLIB_CHECK(decoded.sample_rate == 16000 && !decoded.loop);
    XSFLIB_CHECK((decoded.channels == std::vector<std::vector<int16_t>>{{100, -200, 300, -400}}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::edit::WaveArchiveWith(*before, 2, std::nullopt, incoming.bytes); }) ==
                 "The archive has no such wave archive.");
}

// The values that the tests of a new wave from a wave file share.
struct NewWaveFileFixture
{
    sar::Flavor other;
    std::shared_ptr<const sar::SoundArchive> before;
    std::vector<uint8_t> same;
    std::vector<uint8_t> converted;
    std::vector<uint8_t> shorter;
    std::vector<uint8_t> unknown;
    std::vector<uint8_t> tiny;
    std::vector<uint8_t> cut;
    std::vector<uint8_t> cut_other;
    std::vector<uint8_t> ctr;
    std::vector<uint8_t> big_ctr;
    std::string console;
    IncomingWave read;
    IncomingWave from_other;
    IncomingWave from_ctr;
    std::vector<uint8_t> waves;
    std::shared_ptr<const sar::SoundArchive> after;
    sar::Pcm decoded;
};

// The fixture of the tests of a new wave from a wave file of `flavor`.
NewWaveFileFixture MakeNewWaveFileFixture(sar::Flavor flavor)
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
    return {other, before,  same,    converted, shorter,    unknown,  tiny,  cut,   cut_other,
            ctr,   big_ctr, console, read,      from_other, from_ctr, waves, after, decoded};
}

// The message with which a new wave from wave file `bytes` is refused in the second wave archive of `before`.
std::string NewWaveFileRefusal(const sar::SoundArchive& before, const std::vector<uint8_t>& bytes)
{
    return ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadIncomingWaveFile(bytes, before, kSecondWaves); });
}

// A looped wave file of the archive's console goes in as it is. Bytes past the size in its header stay out.
void TestNewWaveFileSameConsole(sar::Flavor flavor)
{
    const NewWaveFileFixture fixture = MakeNewWaveFileFixture(flavor);
    const std::vector<uint8_t>& same = fixture.same;
    const IncomingWave& read = fixture.read;
    XSFLIB_CHECK(read.bytes == same && read.version == 0x00010100 && !read.converted);
    XSFLIB_CHECK(read.encoding == sar::WaveEncoding::kPcm16 && read.channels == 1 && read.rate == 24000);
    XSFLIB_CHECK(read.frames == 4 && read.loop && read.loop_start == 1);
}

// A wave file of the other console or of the 3DS goes in converted, with the version of the wave archive's wave, and
// its samples decode as that console reads them. It becomes a new wave after the second wave archive's wave.
void TestNewWaveFileOtherConsole(sar::Flavor flavor)
{
    const NewWaveFileFixture fixture = MakeNewWaveFileFixture(flavor);
    const sar::Flavor other = fixture.other;
    const std::shared_ptr<const sar::SoundArchive>& before = fixture.before;
    const std::vector<uint8_t>& converted = fixture.converted;
    const IncomingWave& from_other = fixture.from_other;
    const std::vector<uint8_t>& waves = fixture.waves;
    const std::shared_ptr<const sar::SoundArchive>& after = fixture.after;
    const sar::Pcm& decoded = fixture.decoded;

    // The other console's 16-bit samples, in its byte order.
    const std::vector<int16_t> samples = other == sar::Flavor::kCafe
                                             ? std::vector<int16_t>{0x0102, 0x0304, 0x0506, 0x0708}
                                             : std::vector<int16_t>{0x0201, 0x0403, 0x0605, 0x0807};
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
}

// Bytes that aren't a wave file are refused. So is a bank file.
void TestNewWaveFileOtherKinds(sar::Flavor flavor)
{
    const NewWaveFileFixture fixture = MakeNewWaveFileFixture(flavor);
    const std::shared_ptr<const sar::SoundArchive>& before = fixture.before;
    const std::vector<uint8_t>& unknown = fixture.unknown;
    const auto refusal = [&](const std::vector<uint8_t>& bytes)
    {
        return NewWaveFileRefusal(*before, bytes);
    };
    XSFLIB_CHECK(refusal({1, 2, 3}) == "It isn't a wave file.");
    XSFLIB_CHECK(refusal(Bank(flavor, {{kFirstWavesId, 0}}, {0})) ==
                 "It's a bank file of the " + hokuto::song::ConsoleName(flavor) + ", not a wave file.");
    XSFLIB_CHECK(refusal(unknown) == "It isn't a wave file.");
}

// A wave file of the 3DS is converted on import. A file of the 3DS in the Wii U's byte order is refused.
void TestNewWaveFileFromCtr(sar::Flavor flavor)
{
    const NewWaveFileFixture fixture = MakeNewWaveFileFixture(flavor);
    const std::shared_ptr<const sar::SoundArchive>& before = fixture.before;
    const std::vector<uint8_t>& ctr = fixture.ctr;
    const std::vector<uint8_t>& big_ctr = fixture.big_ctr;
    const IncomingWave& from_ctr = fixture.from_ctr;
    const auto refusal = [&](const std::vector<uint8_t>& bytes)
    {
        return NewWaveFileRefusal(*before, bytes);
    };
    sar::Options options;
    options.flavor = flavor;
    XSFLIB_CHECK(from_ctr.converted && from_ctr.converted->flavor == sar::Flavor::kCtr &&
                 from_ctr.converted->size == ctr.size() && from_ctr.version == 0x00010101);
    XSFLIB_CHECK((sar::DecodeWave(from_ctr.bytes, options).channels ==
                  std::vector<std::vector<int16_t>>{{0x0201, 0x0403, 0x0605, 0x0807}}));
    XSFLIB_CHECK(refusal(big_ctr) == "It isn't a wave file.");
}

// Bytes that aren't a wave file, a file of another family, a file of the 3DS in the Wii U's byte order, a size past the
// end or inside the header, and a wave that ntdlib can't read or convert are refused.
void TestNewWaveFileDamaged(sar::Flavor flavor)
{
    const NewWaveFileFixture fixture = MakeNewWaveFileFixture(flavor);
    const std::shared_ptr<const sar::SoundArchive>& before = fixture.before;
    const std::vector<uint8_t>& same = fixture.same;
    const std::vector<uint8_t>& shorter = fixture.shorter;
    const std::vector<uint8_t>& tiny = fixture.tiny;
    const std::vector<uint8_t>& cut = fixture.cut;
    const std::vector<uint8_t>& cut_other = fixture.cut_other;
    const std::string& console = fixture.console;
    const auto refusal = [&](const std::vector<uint8_t>& bytes)
    {
        return NewWaveFileRefusal(*before, bytes);
    };
    XSFLIB_CHECK(refusal(shorter) == "Its header gives a size of " + std::to_string(same.size()) +
                                         " bytes, but the file has " + std::to_string(shorter.size()) + " bytes.");
    XSFLIB_CHECK(refusal(tiny) ==
                 "Its header gives a size of 8 bytes, but the file has " + std::to_string(same.size()) + " bytes.");
    XSFLIB_CHECK(refusal(cut).starts_with("Can't read the wave. "));
    XSFLIB_CHECK(refusal(cut_other).starts_with("Can't convert it for the " + console + ". "));
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
        return ErrorOf<std::invalid_argument>(
            [&] { hokuto::edit::ReadIncomingWaveFile(same, *archive, wave_archive, wave); });
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
    XSFLIB_CHECK(refusal(kFirstWaves, 2) == "The wave archive has 2 waves. There's no wave 2.");
    XSFLIB_CHECK(refusal(2, 0) == "The archive has no such wave archive.");
    XSFLIB_CHECK(hokuto::edit::IsWaveFile(same) && hokuto::edit::IsWaveFile(converted));
    XSFLIB_CHECK(!hokuto::edit::IsWaveFile(Bank(flavor, {{kFirstWavesId, 0}}, {0})));
    XSFLIB_CHECK(!hokuto::edit::IsWaveFile(seqlib::WavFile(pcm, 8000)) &&
                 !hokuto::edit::IsWaveFile(std::span<const uint8_t>(same).first(3)));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("files",
                                    []
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
                                            TestNewWaveFileSameConsole(flavor);
                                            TestNewWaveFileOtherConsole(flavor);
                                            TestNewWaveFileOtherKinds(flavor);
                                            TestNewWaveFileFromCtr(flavor);
                                            TestNewWaveFileDamaged(flavor);
                                            TestWaveFileInPlace(flavor);
                                        }
                                    });
}
