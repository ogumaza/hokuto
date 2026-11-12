// SPDX-License-Identifier: MIT

// Tests the replacements of the files of a DS sound archive (edit/ds_files.h), a DS wave's encoding (edit/ds_waves.h),
// and the WAV files and wave files that come in for a DS wave (edit/files.h). ntdlib's builders make a small archive
// that NITRO-Composer's tools could have laid out: three sequences, two banks and two wave archives. A file whose size
// changes moves the files after it. Each of those keeps its bytes, the FAT follows, and every file starts on a multiple
// of 32 bytes. The old file then restores the game's archive byte for byte.
//
// The expected samples are worked out by hand from the wave formats. PCM8 keeps the high byte of a sample, and 16-bit
// PCM stores a sample in two bytes, low byte first. A rate sets the timer of the channel: 16,756,991 / the rate,
// rounded down. IMA-ADPCM's error has no value by hand, and the test measures it against the samples.

#include "edit/ds_files.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/sdat_files.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "edit/ds_waves.h"
#include "edit/files.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/ds.h"
#include "song/sequence.h"

using hokuto::format::File;
using xsflib::testing::ErrorOf;

namespace
{

namespace files = ntdlib::testing::sdat_files;
namespace sar = ntdlib::sar;
namespace sdat = ntdlib::sdat;

constexpr uint16_t kNone = 0xFFFF;
constexpr uint32_t kBankFile = 3;
constexpr uint32_t kOtherBankFile = 4;
constexpr uint32_t kWavesFile = 5;
constexpr uint32_t kOtherWavesFile = 6;

const std::vector<int16_t> kFirstSamples = {0x4000, 0x4000, -0x4000, -0x4000, 0x4000, 0x4000, -0x4000, -0x4000};

files::Bytes NoteOf(uint8_t key)
{
    return files::Sequence({key, 0x7F, 0x18, 0xFF});
}

// The bank of `waves` programs, each of one region that plays the wave of its number from slot 0.
files::Bytes BankOf(uint16_t waves)
{
    std::vector<files::Instrument> instruments;
    for (uint16_t wave = 0; wave < waves; wave++)
    {
        instruments.push_back({.type = 1, .region = {.wave = wave}});
    }

    return files::Bank(instruments);
}

files::Bytes FirstWaves()
{
    const files::Wave square{.loop = 1, .loop_length = 2, .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    const files::Wave ramp{.loop_length = 2, .data = {0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70}};
    return files::WaveArchive({square, ramp});
}

files::Bytes OtherWaves()
{
    const files::Wave held{.loop_length = 1, .data = {0x10, 0x10, 0x10, 0x10}};
    return files::WaveArchive({held});
}

// Sounds 0 and 1 play sequence files of their own, and sound 2 plays bank 1. Banks 0 and 1 name wave archives 0 and 1.
std::vector<uint8_t> MakeArchive()
{
    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}, sdat::SequenceInfo{1, 0, 127, 64, 64, 0, 0, ""},
                       sdat::SequenceInfo{2, 1, 127, 64, 64, 0, 0, ""}};
    model.banks = {sdat::BankInfo{kBankFile, {0, kNone, kNone, kNone}, ""},
                   sdat::BankInfo{kOtherBankFile, {1, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{kWavesFile, ""}, sdat::WaveArchiveInfo{kOtherWavesFile, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {NoteOf(60), NoteOf(62), NoteOf(64), BankOf(2), BankOf(1), FirstWaves(), OtherWaves()};

    return sdat::BuildArchive(model);
}

// A wave of wave archive 0 of the archive in `bytes`, and the archive that contains its samples.
struct HeldWave
{
    std::shared_ptr<const hokuto::song::DsArchive> archive;
    sdat::Wave wave;
};

HeldWave WaveOf(const std::vector<uint8_t>& bytes, uint32_t wave)
{
    HeldWave held;
    held.archive = hokuto::song::LoadDsArchive(bytes);
    held.wave = sdat::WaveArchive::Parse(held.archive->archive.File(kWavesFile)).waves.at(wave);

    return held;
}

// Checks `exported`: file `changed` contains `bytes`, every other file keeps its bytes, each file starts on a multiple
// of 32 bytes, and the header of the archive records its size.
void CheckFiles(const std::vector<uint8_t>& game, const std::vector<uint8_t>& exported, uint32_t changed,
                std::span<const uint8_t> bytes)
{
    const std::shared_ptr<const hokuto::song::DsArchive> before = hokuto::song::LoadDsArchive(game);
    const std::shared_ptr<const hokuto::song::DsArchive> after = hokuto::song::LoadDsArchive(exported);
    SEQLIB_REQUIRE(after->archive.files.size() == before->archive.files.size());
    for (uint32_t id = 0; id < after->archive.files.size(); id++)
    {
        const std::span<const uint8_t> file = after->archive.File(id);
        XSFLIB_CHECK(id == changed ? std::ranges::equal(file, bytes)
                                   : std::ranges::equal(file, before->archive.File(id)));
        XSFLIB_CHECK(after->archive.files[id].offset % 32 == 0);
    }
    XSFLIB_CHECK(xsflib::LoadLe32(&exported[8]) == exported.size());
}

// A new bank replaces the bank of a DS archive. A larger one moves the files after it, and a smaller one fits. Both
// forms of the file keep it. The warnings name the programs and the regions that the new bank's waves don't cover, and
// the refusals give their messages.
void TestBank()
{
    const std::vector<uint8_t> game = MakeArchive();
    const File file = hokuto::format::ImportDsArchive(game);
    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(game);

    // A larger bank moves the files after it. The old file restores the game's archive.
    const files::Bytes larger = BankOf(40);
    const File grown = hokuto::edit::ReplaceDsBank(file, *archive, 0, larger);
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(grown);
    XSFLIB_CHECK(exported.size() > game.size());
    CheckFiles(game, exported, kBankFile, larger);
    const std::shared_ptr<const hokuto::song::DsArchive> after = hokuto::song::LoadDsArchive(exported);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::ReplaceDsBank(grown, *after, 0, BankOf(2))) == game);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::ReplaceDsBank(file, *archive, 0, BankOf(2))) == game);

    // A smaller bank, and both forms of the file.
    const files::Bytes smaller = BankOf(1);
    const File shrunk = hokuto::edit::ReplaceDsBank(file, *archive, 0, smaller);
    CheckFiles(game, hokuto::format::ExportArchive(shrunk), kBankFile, smaller);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(grown))) == exported);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(grown))) == exported);

    // The current bank contains programs 0 and 1. A bank of one program has no program 1. The slot of the bank contains
    // 2 waves, and 38 regions of the larger bank play waves past them.
    XSFLIB_CHECK((hokuto::edit::DsBankWarnings(*archive, 0, smaller) ==
                  std::vector<std::string>{"Programs missing from the new bank: 1. Their notes will be silent."}));
    XSFLIB_CHECK((hokuto::edit::DsBankWarnings(*archive, 0, larger) ==
                  std::vector<std::string>{"38 regions in the new bank use waves that are missing from its wave "
                                           "archives. They will be silent."}));
    XSFLIB_CHECK(hokuto::edit::DsBankWarnings(*archive, 0, BankOf(2)).empty());
    XSFLIB_CHECK(hokuto::edit::DsBankInstruments(larger) == 40);

    // The refusals.
    const std::vector<uint8_t> junk(64, 0x5A);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReplaceDsBank(file, *archive, 2, larger); }) ==
                 "The archive has no bank 2.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReplaceDsBank(file, *archive, 0, junk); })
                     .starts_with("Can't read the file as an SBNK bank: "));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithDsFile(file, 99, larger); }) ==
                 "Can't replace the file in the archive: the archive holds no file 99.");
}

// A new wave archive replaces a wave archive of a DS archive, with a warning for a program whose wave is missing from
// the new archive. The sounds that play each item come with it.
void TestWaveArchive()
{
    const std::vector<uint8_t> game = MakeArchive();
    const File file = hokuto::format::ImportDsArchive(game);
    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(game);

    // A wave archive of one wave. Program 1 of bank 0 plays wave 1. The new wave archive has no wave 1.
    const files::Bytes one = OtherWaves();
    const File edited = hokuto::edit::ReplaceDsWaveArchive(file, *archive, 0, one);
    CheckFiles(game, hokuto::format::ExportArchive(edited), kWavesFile, one);
    XSFLIB_CHECK((hokuto::edit::DsWaveArchiveWarnings(*archive, 0, one) ==
                  std::vector<std::string>{
                      "The new file has 1 wave. 1 region in the banks uses a higher wave number and will be silent."}));
    XSFLIB_CHECK(hokuto::edit::DsWaveArchiveWarnings(*archive, 0, FirstWaves()).empty());
    XSFLIB_CHECK(hokuto::edit::DsWaveArchiveWaves(one) == 1);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::ReplaceDsWaveArchive(file, *archive, 0, FirstWaves())) ==
                 game);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReplaceDsWaveArchive(file, *archive, 2, one); }) ==
                 "The archive has no wave archive 2.");

    // The sounds that play each item.
    XSFLIB_CHECK((hokuto::edit::DsSoundsOfBank(*archive, 0) == std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK((hokuto::edit::DsSoundsOfBank(*archive, 1) == std::vector<uint32_t>{2}));
    XSFLIB_CHECK((hokuto::edit::DsSoundsOfWaveArchive(*archive, 0) == std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK((hokuto::edit::DsSoundsOfWaveArchive(*archive, 1) == std::vector<uint32_t>{2}));
}

// A new sequence replaces the sequence of a DS sound. The other sounds keep theirs, the old file restores the archive,
// and the refusals give their messages.
void TestSequence()
{
    const std::vector<uint8_t> game = MakeArchive();
    const File file = hokuto::format::ImportDsArchive(game);
    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(game);
    XSFLIB_CHECK((hokuto::edit::DsSoundsOfSequence(file, *archive, 1) == std::vector<uint32_t>{1}));

    // Sound 1 gets a sequence of two notes. The other sounds keep theirs, and the old file restores the archive.
    const files::Bytes longer = files::Sequence({0x47, 0x7F, 0x18, 0x48, 0x7F, 0x18, 0xFF});
    const File edited = hokuto::edit::ReplaceDsSequence(file, *archive, 1, longer);
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    const std::shared_ptr<const hokuto::song::DsArchive> after = hokuto::song::LoadDsArchive(exported);
    const auto keys = [](const hokuto::song::DsArchive& played, uint32_t sound)
    {
        std::vector<int> found;
        for (const hokuto::song::Track& track : hokuto::song::ReadSequence(played, sound).tracks)
        {
            for (const hokuto::song::Note& note : track.notes)
            {
                found.push_back(note.key);
            }
        }

        return found;
    };
    XSFLIB_CHECK((keys(*archive, 1) == std::vector<int>{62}));
    XSFLIB_CHECK((keys(*after, 1) == std::vector<int>{0x47, 0x48}));
    XSFLIB_CHECK((keys(*after, 0) == std::vector<int>{60}));
    XSFLIB_CHECK((keys(*after, 2) == std::vector<int>{64}));
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::ReplaceDsSequence(file, *archive, 1, NoteOf(62))) == game);

    // The refusals.
    const std::vector<uint8_t> junk(64, 0x5A);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReplaceDsSequence(file, *archive, 1, junk); }) ==
                 "Can't read the file as an SSEQ sequence file.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReplaceDsSequence(file, *archive, 3, longer); }) ==
                 hokuto::song::DsSoundName(*archive, 3) +
                     " has no sequence that Hokuto can replace. Its commands are in a sequence archive or among other "
                     "data of the archive.");
}

// A DS wave converts to another encoding or rate: PCM8 to 16-bit PCM exactly, a new rate with the timer of that rate,
// and IMA-ADPCM with its loop on a word. Without a new encoding, the wave keeps its bytes.
void TestEncoding()
{
    using hokuto::edit::DsWaveValues;
    const std::vector<uint8_t> game = MakeArchive();
    const File file = hokuto::format::ImportDsArchive(game);
    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(game);
    const auto exported = [&](uint32_t wave, const DsWaveValues& values)
    {
        return hokuto::format::ExportArchive(hokuto::edit::WithDsWave(file, *archive, 0, wave, values));
    };
    const HeldWave present = WaveOf(game, 0);
    const uint16_t rate = present.wave.sample_rate;

    // PCM8 converts to 16-bit PCM exactly: each sample uses two bytes, and the loop starts at the same sample. The
    // other wave and the other files keep their bytes. The same rate keeps the wave's timer.
    const std::vector<uint8_t> wider = exported(0, {rate, true, 4, sdat::WaveEncoding::kPcm16});
    const HeldWave made = WaveOf(wider, 0);
    XSFLIB_CHECK(made.wave.encoding == sdat::WaveEncoding::kPcm16 && made.wave.timer == present.wave.timer);
    XSFLIB_CHECK(made.wave.loop == 1 && made.wave.sample_rate == rate && made.wave.LoopStartSample() == 4);
    XSFLIB_CHECK(sdat::DecodeWave(made.wave) == kFirstSamples);
    XSFLIB_CHECK(sdat::DecodeWave(WaveOf(wider, 1).wave) == sdat::DecodeWave(WaveOf(game, 1).wave));
    XSFLIB_CHECK(made.archive->archive.File(kWavesFile).size() == present.archive->archive.File(kWavesFile).size() + 8);

    // Another rate sets the timer of the rate.
    const HeldWave slower = WaveOf(exported(0, {16000, false, 0, sdat::WaveEncoding::kPcm16}), 0);
    XSFLIB_CHECK(slower.wave.sample_rate == 16000 && slower.wave.timer == 16756991 / 16000 && slower.wave.loop == 0);

    // IMA-ADPCM stores a word of the decoder state, then 8 samples in a word. A loop from sample 4 moves forward to
    // sample 8, and its 4 samples repeat to fill a word: the wave contains 16 samples.
    const HeldWave adpcm = WaveOf(exported(0, {rate, true, 4, sdat::WaveEncoding::kImaAdpcm}), 0);
    XSFLIB_CHECK(adpcm.wave.encoding == sdat::WaveEncoding::kImaAdpcm && adpcm.wave.Samples() == 16);
    XSFLIB_CHECK(adpcm.wave.LoopStartSample() == 8 && adpcm.wave.loop_start == 2 && adpcm.wave.loop_length == 1);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { exported(0, {rate, true, 8, sdat::WaveEncoding::kPcm16}); }) ==
                 "The loop must start before the end of the wave.");

    // Without an encoding, or with the one that it has, the wave keeps its bytes.
    XSFLIB_CHECK(exported(0, {rate, true, 0, sdat::WaveEncoding::kPcm8}) == game);
    XSFLIB_CHECK(exported(0, {rate, true, 0}) == game);

    // New bytes in a wave's place, and after the waves.
    const std::vector<uint8_t> bytes = sdat::EncodeWave(kFirstSamples, 8000, sdat::WaveEncoding::kPcm16, std::nullopt);
    const std::vector<uint8_t> placed =
        hokuto::format::ExportArchive(hokuto::edit::WithDsWaveBytes(file, *archive, 0, 1, bytes));
    XSFLIB_CHECK(sdat::DecodeWave(WaveOf(placed, 1).wave) == kFirstSamples);
    XSFLIB_CHECK(WaveOf(placed, 1).wave.sample_rate == 8000);
    const std::vector<uint8_t> three =
        hokuto::format::ExportArchive(hokuto::edit::WithDsWaveBytes(file, *archive, 0, 2, bytes));
    XSFLIB_CHECK(sdat::DecodeWave(WaveOf(three, 2).wave) == kFirstSamples);
    XSFLIB_CHECK(sdat::DecodeWave(WaveOf(three, 0).wave) == kFirstSamples);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithDsWaveBytes(file, *archive, 0, 3, bytes); }) ==
                 "WAVEARC_0 has no wave 3.");

    // What the Replace window shows of the current wave: the banks that play it, and its size with the parameters.
    const hokuto::edit::WaveChanges changes = hokuto::edit::ChangesOfDsWave(*archive, 0, 1);
    XSFLIB_CHECK(changes.encoding == sar::WaveEncoding::kPcm8 && changes.channels == 1 && !changes.loop);
    XSFLIB_CHECK(changes.loop_end == 8 && changes.size == 20 && changes.rate == WaveOf(game, 1).wave.sample_rate);
    XSFLIB_CHECK((changes.banks == std::vector<uint32_t>{0}));
}

// A wave archive whose layout differs from the tools' layout, with bytes after its last wave, accepts no wave of
// another size.
void TestUnknownLayout()
{
    files::Bytes waves = FirstWaves();
    waves.insert(waves.end(), 4, 0x11);

    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}};
    model.banks = {sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{2, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {NoteOf(60), BankOf(2), waves};

    const std::vector<uint8_t> game = sdat::BuildArchive(model);
    const File file = hokuto::format::ImportDsArchive(game);
    const std::shared_ptr<const hokuto::song::DsArchive> archive = hokuto::song::LoadDsArchive(game);
    const std::vector<uint8_t> wave_bytes =
        sdat::EncodeWave(kFirstSamples, 8000, sdat::WaveEncoding::kPcm16, std::nullopt);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithDsWaveBytes(file, *archive, 0, 0, wave_bytes); }) ==
        "WAVEARC_0 doesn't use the standard layout. Hokuto can't change the size of its waves.");
}

// A WAV file of `channels` at `rate`, with a loop from `loop_start` to the end when one is given.
std::vector<uint8_t> WavOf(std::vector<std::vector<int16_t>> channels, uint32_t rate,
                           std::optional<uint32_t> loop_start)
{
    seqlib::Pcm pcm;
    pcm.channels = std::move(channels);
    if (loop_start)
    {
        pcm.loop = true;
        pcm.loop_start = *loop_start;
        pcm.loop_end = static_cast<uint32_t>(pcm.channels[0].size());
    }

    return seqlib::WavFile(pcm, rate);
}

// The DS wave of `incoming`, read back.
sdat::Wave Parsed(const hokuto::edit::IncomingWave& incoming)
{
    return sdat::Wave::Parse(xsflib::Reader(incoming.bytes), 0, incoming.bytes.size());
}

// The mono samples of the tests of incoming waves, and the wave that loops from sample 3.
struct IncomingFixture
{
    std::vector<int16_t> samples;
    hokuto::edit::IncomingWave looped;
};

// The fixture of the tests of incoming waves.
IncomingFixture MakeIncomingFixture()
{
    const std::vector<int16_t> samples = {0x1234, -0x1234, 0x0100, -0x0100, 0x2000, 0x3000, -0x2000, -0x3000};
    const hokuto::edit::IncomingWave looped =
        hokuto::edit::ReadIncomingDsWave(WavOf({samples}, 32000, 3), sar::WaveEncoding::kPcm16);

    return {samples, looped};
}

// A WAV file comes in as a DS wave in 16-bit PCM, PCM8 or IMA-ADPCM. Two channels are mixed, a loop starts at a word,
// and each encoding keeps the signal-to-noise ratio that its bits allow.
void TestIncomingPcm()
{
    const IncomingFixture fixture = MakeIncomingFixture();
    const std::vector<int16_t>& samples = fixture.samples;

    // 16-bit PCM stores the samples of a mono file.
    const hokuto::edit::IncomingWave wide =
        hokuto::edit::ReadIncomingDsWave(WavOf({samples}, 22050, std::nullopt), sar::WaveEncoding::kPcm16);
    const sdat::Wave wide_wave = Parsed(wide);
    XSFLIB_CHECK(wide_wave.encoding == sdat::WaveEncoding::kPcm16 && sdat::DecodeWave(wide_wave) == samples);
    XSFLIB_CHECK(wide_wave.sample_rate == 22050 && wide_wave.timer == 16756991 / 22050 && wide_wave.loop == 0);
    XSFLIB_CHECK(wide.encoding == sar::WaveEncoding::kPcm16 && wide.channels == 1 && wide.rate == 22050);
    XSFLIB_CHECK(wide.frames == 8 && !wide.loop && !wide.signal_to_noise);

    // Two channels go in mixed, halfway between them.
    const hokuto::edit::IncomingWave mixed = hokuto::edit::ReadIncomingDsWave(
        WavOf({{1000, -3000, 5, 32767}, {3000, -1000, 6, 32767}}, 32000, std::nullopt), sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK((sdat::DecodeWave(Parsed(mixed)) == std::vector<int16_t>{2000, -2000, 6, 32767}));
    XSFLIB_CHECK(mixed.channels == 2);

    // PCM8 keeps the high byte of each sample. The signal-to-noise ratio comes from the low bytes that it drops.
    const hokuto::edit::IncomingWave narrow = hokuto::edit::ReadIncomingDsWave(
        WavOf({{0x1234, 0x5678, 0x0100, 0x2000}}, 32000, std::nullopt), sar::WaveEncoding::kPcm8);
    const std::vector<int16_t> narrow_samples = sdat::DecodeWave(Parsed(narrow));
    XSFLIB_CHECK((narrow_samples == std::vector<int16_t>{0x1200, 0x5600, 0x0100, 0x2000}));
    const double signal = 4660.0 * 4660.0 + 22136.0 * 22136.0 + 256.0 * 256.0 + 8192.0 * 8192.0;
    const double noise = 52.0 * 52.0 + 120.0 * 120.0;
    SEQLIB_REQUIRE(narrow.signal_to_noise.has_value());
    XSFLIB_CHECK(std::abs(*narrow.signal_to_noise - 10.0 * std::log10(signal / noise)) < 1e-9);
}

// A loop of an incoming wave starts at a word.
void TestIncomingLoop()
{
    const IncomingFixture fixture = MakeIncomingFixture();
    const std::vector<int16_t>& samples = fixture.samples;
    const hokuto::edit::IncomingWave& looped = fixture.looped;

    // A loop starts at a word: at an even sample of 16-bit PCM. Its start moves forward from sample 3 to 4. The loop of
    // 5 samples repeats to fill a word, and plays the same samples as before: the wave contains 14 samples.
    XSFLIB_CHECK(looped.loop && looped.loop_start == 3 && looped.loop_moved == 1);
    XSFLIB_CHECK(Parsed(looped).LoopStartSample() == 4 && Parsed(looped).loop == 1 && Parsed(looped).Samples() == 14);
    const std::vector<int16_t> looped_samples = sdat::DecodeWave(Parsed(looped));
    for (std::size_t i = 8; i < 14; i++)
    {
        XSFLIB_CHECK(looped_samples.at(i) == samples.at(3 + (i - 3) % 5));
    }
}

// A sine comes in as IMA-ADPCM, and at rates past 65,535 Hz and past the highest.
void TestIncomingAdpcmAndRates()
{
    // IMA-ADPCM: the state's word, then 4 bits a sample. The encoder's step follows a sine. A sine therefore comes back
    // at 20 dB or more.
    std::vector<int16_t> sine(4096);
    for (std::size_t i = 0; i < sine.size(); i++)
    {
        sine[i] = static_cast<int16_t>(std::lround(12000.0 * std::sin(static_cast<double>(i) * 0.05)));
    }
    const hokuto::edit::IncomingWave adpcm =
        hokuto::edit::ReadIncomingDsWave(WavOf({sine}, 32000, std::nullopt), sar::WaveEncoding::kImaAdpcm);
    SEQLIB_REQUIRE(adpcm.signal_to_noise.has_value());
    XSFLIB_CHECK(*adpcm.signal_to_noise > 20.0 && Parsed(adpcm).Samples() == sine.size());

    // A rate above 65,535 stays. The wave stores the low 16 bits of its rate, and the timer comes from the rate:
    // 16,756,991 / 96,000 = 174 rounded down, and 96,000 - 65,536 = 30,464. song::DsWaveRate uses the rate of the timer
    // there: 16,756,991 / 174 = 96,304 rounded down.
    const hokuto::edit::IncomingWave wider =
        hokuto::edit::ReadIncomingDsWave(WavOf({sine}, 96000, std::nullopt), sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK(wider.rate == 96000 && Parsed(wider).sample_rate == 30464 && Parsed(wider).timer == 174);
    XSFLIB_CHECK(hokuto::song::DsWaveRate(Parsed(wider)) == 96304);
    XSFLIB_CHECK(sdat::DecodeWave(Parsed(wider)) == sine);

    // A rate past the highest becomes 48 kHz unless the caller passes another. A lower rate requires a file longer than
    // a resampler's filter, and the sine is long enough.
    const hokuto::edit::IncomingWave high =
        hokuto::edit::ReadIncomingDsWave(WavOf({sine}, 200000, std::nullopt), sar::WaveEncoding::kPcm16);
    XSFLIB_CHECK(high.rate == 48000 && high.wav_rate == 200000 && Parsed(high).sample_rate == 48000);
    const hokuto::edit::IncomingWave chosen = hokuto::edit::ReadIncomingDsWave(
        WavOf({sine}, 96000, std::nullopt), sar::WaveEncoding::kPcm16, std::nullopt, 32000);
    XSFLIB_CHECK(chosen.rate == 32000 && Parsed(chosen).sample_rate == 32000);
}

// A WAV file with three channels, a rate too low or an encoding of another console is refused.
void TestIncomingRefusals()
{
    const IncomingFixture fixture = MakeIncomingFixture();
    const std::vector<int16_t>& samples = fixture.samples;

    // The refusals.
    const std::vector<uint8_t> three = WavOf({samples, samples, samples}, 32000, std::nullopt);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadIncomingDsWave(three); }) ==
                 "It has 3 channels. A wave can have only one or two.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::edit::ReadIncomingDsWave(WavOf({samples}, 100, std::nullopt)); }) ==
                 "A DS wave must have a sample rate from 256 to 192000 Hz. This one is 100 Hz.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&]
                     {
                         hokuto::edit::ReadIncomingDsWave(WavOf({samples}, 32000, std::nullopt),
                                                          sar::WaveEncoding::kDspAdpcm);
                     }) == "A DS wave must be 8-bit PCM, 16-bit PCM or IMA-ADPCM.");
}

// A DS wave file comes in with the bytes of its wave.
void TestIncomingWaveFile()
{
    const IncomingFixture fixture = MakeIncomingFixture();
    const hokuto::edit::IncomingWave& looped = fixture.looped;

    // A wave file provides the bytes of its wave, and says what it contains.
    const std::vector<uint8_t> file = sdat::WaveFile(looped.bytes);
    XSFLIB_CHECK(hokuto::edit::IsDsWaveFile(file) && !hokuto::edit::IsDsWaveFile(looped.bytes));
    const hokuto::edit::IncomingWave from_file = hokuto::edit::ReadIncomingDsWaveFile(file);
    XSFLIB_CHECK(from_file.bytes == looped.bytes && from_file.rate == 32000 && from_file.channels == 1);
    XSFLIB_CHECK(from_file.loop && from_file.loop_start == 4 && from_file.frames == 14);
    XSFLIB_CHECK(from_file.encoding == sar::WaveEncoding::kPcm16);
    const std::vector<uint8_t> junk(64, 0x5A);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::ReadIncomingDsWaveFile(junk); })
                     .starts_with("Can't read it as a DS wave file: "));
}

// SameDsTimer returns the timer whose rate is nearest the rate of the wave's WAV file.
void TestSameTimer()
{
    // The channel's clock over 16,000 Hz gives a timer of 1,047. That timer plays at 16,004 Hz, the rate of the wave's
    // WAV file. The next timer down plays at 16,020 Hz.
    XSFLIB_CHECK(hokuto::edit::SameDsTimer(16000, 16004));
    XSFLIB_CHECK(!hokuto::edit::SameDsTimer(16000, 16020));
    XSFLIB_CHECK(hokuto::edit::SameDsTimer(0, 0));
    XSFLIB_CHECK(!hokuto::edit::SameDsTimer(0, 16000));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ds_files",
                                    []
                                    {
                                        TestSameTimer();
                                        TestBank();
                                        TestWaveArchive();
                                        TestSequence();
                                        TestEncoding();
                                        TestUnknownLayout();
                                        TestIncomingPcm();
                                        TestIncomingLoop();
                                        TestIncomingAdpcmAndRates();
                                        TestIncomingRefusals();
                                        TestIncomingWaveFile();
                                    });
}
