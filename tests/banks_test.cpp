// SPDX-License-Identifier: MIT

// Tests the edits of banks, waves and wave sounds through their models (src/edit/banks.h and src/edit/waves.h). A small
// archive of the Switch, and the same archive of the Wii U, hold a sequence sound that plays the bank, a wave sound,
// the bank, a wave archive of two waves and a wave sound file. ntdlib lays the archive out from its model as
// NintendoWare's tools do. The bank's program 0 plays the first wave over every key, and its program 2 the second wave
// over keys 60 to 127. The first wave is a sine of 280 samples in DSP-ADPCM without a loop, and the second two ramps of
// 100 samples in PCM16 with a loop from 10. The wave sound plays the second wave. The checks read the exported archive
// with ntdlib. Waves of two channels of each console, the 3DS's included, also go in as waves of one. An archive of the
// Wii holds the same waves in the wave data of its bank and of its wave sound file (see WiiArchive).

#include "edit/banks.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/encode.h"
#include "seqlib/wav.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "edit/files.h"
#include "edit/groups.h"
#include "edit/new_items.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/items.h"

using hokuto::edit::Region;
using hokuto::format::File;
using hokuto::format::WaveModel;
using xsflib::testing::ErrorOf;

namespace
{

namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
namespace wave = ntdlib::wave;
namespace sar_files = ntdlib::testing::sar_files;

// The archive's items and files.
constexpr uint32_t kWaveSound = 1;
constexpr uint32_t kBank = 0;
constexpr uint32_t kWaves = 0;
constexpr uint32_t kBankFile = 1;
constexpr uint32_t kWavesFile = 2;
constexpr uint32_t kWaveSoundsFile = 3;
constexpr uint32_t kWavesId = 0x05000000;

// The waves.
constexpr uint32_t kSine = 0;
constexpr uint32_t kRamps = 1;
constexpr uint32_t kSineLength = 280;
constexpr uint32_t kRampLength = 100;

// The bank: program 0 plays the sine over every key, and program 2 the ramps over keys 60 to 127.
sar::BankFile TestBank(sar::Flavor flavor)
{
    sar::BankFile bank;
    bank.flavor = flavor;
    bank.waves = {{kWavesId, kSine}, {kWavesId, kRamps}};
    bank.instruments.resize(3);
    sar::BankFile::Region ramps;
    ramps.values.wave_id_index = 1;
    bank.instruments[0].emplace().key_regions = {{0, 127, {sar::BankFile::Region()}}};
    bank.instruments[2].emplace().key_regions = {{60, 127, {ramps}}};

    return bank;
}

// The sine's samples: kSineLength samples at a peak of 8,000, 0.3 radians apart.
std::vector<int16_t> SineSamples()
{
    std::vector<int16_t> samples(kSineLength);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        samples[i] = static_cast<int16_t>(std::lround(8000.0 * std::sin(static_cast<double>(i) * 0.3)));
    }

    return samples;
}

// A ramp of the second wave: up in the first channel, down in the second.
int16_t RampSample(std::size_t channel, std::size_t i)
{
    return static_cast<int16_t>(channel == 0 ? i * 100 : -static_cast<int>(i) * 100);
}

// The wave archive: the sine in DSP-ADPCM, and the ramps in PCM16 with a loop from 10.
std::vector<uint8_t> TestWaves(sar::Flavor flavor)
{
    sar::WaveFile sine;
    sine.flavor = flavor;
    sine.encoding = sar::WaveEncoding::kDspAdpcm;
    sine.sample_rate = 32000;
    sine.loop_end = kSineLength;
    const wave::DspAdpcmChannel encoded = wave::EncodeDspAdpcm(SineSamples(), 0, wave::DspAdpcmSum::kWrap);
    sar::DspAdpcmInfo info;
    info.coefs = encoded.coefs;
    info.pred_scale = encoded.pred_scale;
    sine.channels = {{encoded.data, info}};

    sar::WaveFile ramps;
    ramps.flavor = flavor;
    ramps.loop = true;
    ramps.loop_start = 10;
    ramps.original_loop_start = 10;
    ramps.loop_end = kRampLength;
    for (std::size_t c = 0; c < 2; ++c)
    {
        std::vector<uint8_t> samples(kRampLength * 2);
        for (std::size_t i = 0; i < kRampLength; ++i)
        {
            const auto sample = static_cast<uint16_t>(RampSample(c, i));
            flavor == sar::Flavor::kCafe ? xsflib::StoreBe16(&samples[i * 2], sample)
                                         : xsflib::StoreLe16(&samples[i * 2], sample);
        }
        ramps.channels.push_back({std::move(samples), std::nullopt});
    }

    sar::WaveArchiveFile archive;
    archive.flavor = flavor;
    archive.waves = {sine.Write(), ramps.Write()};

    return archive.Write();
}

// The archive of `flavor`, laid out from its model: a sequence sound that plays the bank, and a wave sound that plays
// the ramps with a pan of 20 and a pitch of 1.5. The bank and the wave sound file take their waves from the wave
// archive.
std::vector<uint8_t> TestArchive(sar::Flavor flavor)
{
    sar::ArchiveModel model;
    model.flavor = flavor;
    model.version = flavor == sar::Flavor::kNx ? 0x00020500 : 0x00020200;
    sar::SoundModel song;
    song.name = "SEQ_TEST";
    song.banks = {sar::ItemId(sar::kBankItem, kBank)};
    sar::SoundModel wave_sound;
    wave_sound.name = "WSD_TEST";
    wave_sound.type = sar::SoundType::kWaveSound;
    wave_sound.file_id = kWaveSoundsFile;
    model.sounds = {song, wave_sound};
    model.banks = {{.name = "BANK_TEST", .file_id = kBankFile}};
    model.wave_archives = {{.name = "WARC_TEST", .file_id = kWavesFile}};
    model.players = {{.name = "PLAYER_TEST", .playable_sound_max = 1}};

    // The wave sound plays the ramps, with a pan and a pitch.
    sar_files::WaveSoundSpec sound;
    sound.pan = 20;
    sound.pitch = 1.5f;
    sound.tracks = {{{0.0f, 1.0f, 0}}};
    sound.notes.resize(1);
    sar_files::WaveSoundFileSpec sounds;
    sounds.waves = {{kWavesId, kRamps}};
    sounds.sounds = {sound};
    const std::vector<uint8_t> kBytecode = {0xB0, 48, 0x81, 0, 60, 100, 24, 0xFF};
    model.files = {{.bytes = sar_files::Sequence(flavor, kBytecode, {{"Test", 0}})},
                   {.bytes = TestBank(flavor).Write()},
                   {.bytes = TestWaves(flavor)},
                   {.bytes = sar_files::WaveSoundFile(flavor, sounds)}};

    return sar::BuildArchive(model);
}

// The archive that `file` exports. The program opens an archive the same way.
std::shared_ptr<const sar::SoundArchive> Load(const File& file)
{
    return hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "test.bfsar");
}

// Whether `file` comes back from both forms unchanged.
bool ComesBack(const File& file)
{
    return hokuto::format::Read(hokuto::format::Write(file)) == file &&
           hokuto::format::ReadText(hokuto::format::WriteText(file)) == file;
}

// The wave of `archive` as its file holds it.
sar::WaveFile WaveOf(const sar::SoundArchive& archive, uint32_t index)
{
    return sar::WaveFile::Read(hokuto::song::ItemFile(archive, {hokuto::song::ItemKind::kWave, kWaves, index}));
}

// A wave of `channels` in PCM16 or DSP-ADPCM at 32 kHz, with a loop from `loop_start` to its end when it has one.
// ntdlib's encoder encodes DSP-ADPCM.
sar::WaveFile WaveOfSamples(sar::Flavor flavor, sar::WaveEncoding encoding,
                            const std::vector<std::vector<int16_t>>& channels, std::optional<uint32_t> loop_start)
{
    sar::WaveFile made;
    made.flavor = flavor;
    made.encoding = encoding;
    made.sample_rate = 32000;
    made.loop = loop_start.has_value();
    made.loop_start = loop_start.value_or(0);
    made.original_loop_start = made.loop_start;
    made.loop_end = static_cast<uint32_t>(channels[0].size());
    for (const std::vector<int16_t>& samples : channels)
    {
        if (encoding == sar::WaveEncoding::kDspAdpcm)
        {
            const wave::DspAdpcmChannel encoded =
                wave::EncodeDspAdpcm(samples, made.loop_start, wave::DspAdpcmSum::kWrap);
            sar::DspAdpcmInfo info;
            info.coefs = encoded.coefs;
            info.pred_scale = encoded.pred_scale;
            info.loop_pred_scale = encoded.loop_pred_scale;
            info.loop_yn1 = encoded.loop_yn1;
            info.loop_yn2 = encoded.loop_yn2;
            made.channels.push_back({encoded.data, info});
            continue;
        }

        std::vector<uint8_t> bytes(samples.size() * 2);
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            const auto sample = static_cast<uint16_t>(samples[i]);
            flavor == sar::Flavor::kCafe ? xsflib::StoreBe16(&bytes[i * 2], sample)
                                         : xsflib::StoreLe16(&bytes[i * 2], sample);
        }
        made.channels.push_back({std::move(bytes), std::nullopt});
    }

    return made;
}

// A wave of two channels of `flavor` goes in as a wave of one (edit::MonoWaveOf). Each sample is the mean of the two,
// rounded half away from zero. The rate and the loop stay, and a wave in PCM16 keeps the means exactly. With a gain of
// 2, the means double, held to 16 bits. A loop in DSP-ADPCM from sample 20 moves to the next frame, at 28, and the wave
// ends 8 samples later. MonoWaveBytes gives the size of the new wave's file. MonoLevelOf gives each channel's mean
// power, the mean of the products of the two channels' samples, and the peak of the mean. A wave of one channel is
// refused.
void TestMonoWave(sar::Flavor flavor)
{
    const std::vector<int16_t> kLeft = {3, -3, 100, -100, 32767, -32768, 7, 0};
    const std::vector<int16_t> kRight = {0, 0, 101, -101, 32767, -32768, -8, 0};
    const sar::WaveFile pcm = WaveOfSamples(flavor, sar::WaveEncoding::kPcm16, {kLeft, kRight}, 2);
    const hokuto::edit::MonoWave mono = hokuto::edit::MonoWaveOf(pcm);
    XSFLIB_CHECK(mono.wave.channels.size() == 1 && !mono.signal_to_noise);
    XSFLIB_CHECK(mono.wave.sample_rate == 32000 && mono.wave.loop && mono.wave.loop_start == 2 &&
                 mono.wave.loop_end == 8);
    XSFLIB_CHECK(hokuto::edit::DecodedSamples(mono.wave, 8).at(0) ==
                 (std::vector<int16_t>{2, -2, 101, -101, 32767, -32768, -1, 0}));
    XSFLIB_CHECK(hokuto::edit::DecodedSamples(hokuto::edit::MonoWaveOf(pcm, 2.0).wave, 8).at(0) ==
                 (std::vector<int16_t>{3, -3, 201, -201, 32767, -32768, -1, 0}));
    SEQLIB_CHECK_EQ(hokuto::edit::MonoWaveBytes(pcm), mono.wave.Write().size());
    SEQLIB_CHECK_EQ(hokuto::edit::MonoLevelOf(pcm).peak, 32768);

    const std::vector<int16_t> sine = SineSamples();
    std::vector<int16_t> half(sine.size());
    std::vector<int16_t> opposite(sine.size());
    for (std::size_t i = 0; i < sine.size(); ++i)
    {
        half[i] = static_cast<int16_t>(sine[i] / 2);
        opposite[i] = static_cast<int16_t>(-sine[i] * 9 / 10);
    }
    const sar::WaveFile looped = WaveOfSamples(flavor, sar::WaveEncoding::kDspAdpcm, {sine, half}, 20);
    const hokuto::edit::MonoWave moved = hokuto::edit::MonoWaveOf(looped);
    XSFLIB_CHECK(moved.wave.channels.size() == 1 && moved.wave.loop && moved.wave.loop_start == 28 &&
                 moved.wave.loop_end == kSineLength + 8);
    XSFLIB_CHECK(moved.signal_to_noise && *moved.signal_to_noise > 20.0);
    SEQLIB_CHECK_EQ(hokuto::edit::MonoWaveBytes(looped), moved.wave.Write().size());
    const sar::WaveFile once = WaveOfSamples(flavor, sar::WaveEncoding::kDspAdpcm, {sine, half}, std::nullopt);
    const hokuto::edit::MonoWave unlooped = hokuto::edit::MonoWaveOf(once);
    XSFLIB_CHECK(!unlooped.wave.loop && unlooped.wave.loop_end == kSineLength);
    SEQLIB_CHECK_EQ(hokuto::edit::MonoWaveBytes(once), unlooped.wave.Write().size());

    const std::vector<int16_t> silence(sine.size());

    const auto level = [&](const std::vector<int16_t>& first, const std::vector<int16_t>& second)
    {
        return hokuto::edit::MonoLevelOf(WaveOfSamples(flavor, sar::WaveEncoding::kPcm16, {first, second}, {}));
    };

    const auto mean = [](const std::vector<int16_t>& first, const std::vector<int16_t>& second)
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < first.size(); ++i)
        {
            sum += static_cast<double>(first[i]) * static_cast<double>(second[i]);
        }

        return sum / static_cast<double>(first.size());
    };

    const hokuto::edit::MonoLevel equal = level(sine, sine);
    const double power = mean(sine, sine);
    XSFLIB_CHECK(power > 0.0 && equal.left == power && equal.right == power && equal.both == power &&
                 equal.peak == 8000);
    const hokuto::edit::MonoLevel one_side = level(sine, silence);
    XSFLIB_CHECK(one_side.left == power && one_side.right == 0.0 && one_side.both == 0.0 && one_side.peak == 4000);
    const hokuto::edit::MonoLevel opposed = level(sine, opposite);
    XSFLIB_CHECK(opposed.right == mean(opposite, opposite) && opposed.both == mean(sine, opposite) &&
                 opposed.both < 0.0);
    const hokuto::edit::MonoLevel quiet = level(silence, silence);
    XSFLIB_CHECK(quiet.left == 0.0 && quiet.right == 0.0 && quiet.both == 0.0 && quiet.peak == 0);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::edit::MonoWaveOf(WaveOfSamples(flavor, sar::WaveEncoding::kPcm16, {sine}, {})); }) ==
                 "The wave holds 1 channel, not 2.");
}

// The regions of an instrument's model, and their refusals.
void TestRegions()
{
    Region low;
    low.last_key = 59;
    Region soft;
    soft.first_key = 60;
    soft.last_velocity = 63;
    Region loud = soft;
    loud.first_velocity = 64;
    loud.last_velocity = 127;
    loud.values.original_key = 72;

    const sar::BankFile::Instrument instrument = hokuto::edit::InstrumentOf({loud, low, soft});

    XSFLIB_CHECK(instrument.key_regions.size() == 2 && instrument.key_regions[1].regions.size() == 2);
    XSFLIB_CHECK(hokuto::edit::RegionsOf(instrument) == std::vector<Region>({low, soft, loud}));
    XSFLIB_CHECK(hokuto::edit::RegionAt(instrument, 61, 100) == 2 && hokuto::edit::RegionAt(instrument, 10, 0) == 0);

    // Keys that overlap without being the same, velocities that overlap, and runs that end before they start or past
    // 127.
    Region across = low;
    across.first_key = 50;
    across.last_key = 70;
    Region shared = loud;
    shared.first_velocity = 63;
    Region backwards = low;
    backwards.first_key = 80;
    Region loudest = low;
    loudest.last_velocity = 128;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::InstrumentOf({low, across}); }) ==
                 "Regions over keys 0 to 59 and keys 50 to 70 overlap. Regions over other keys can't overlap.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::InstrumentOf({soft, shared}); }) ==
                 "Two regions over keys 60 to 127 share velocity 63.");
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { hokuto::edit::InstrumentOf({backwards}); }).empty());
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { hokuto::edit::InstrumentOf({loudest}); }).empty());
}

// The instruments of a bank's model: new ones, copies, moves and removals.
void TestInstruments()
{
    const sar::BankFile bank = TestBank(sar::Flavor::kNx);
    const sar::WaveId kOther{kWavesId + 1, 4};

    const sar::BankFile added = hokuto::edit::WithNewInstrument(bank, 5, kOther);
    const sar::BankFile empty = hokuto::edit::WithNewInstrument(bank, 1, std::nullopt);
    const sar::BankFile copied = hokuto::edit::WithInstrumentCopy(bank, 2, 7);
    const sar::BankFile moved = hokuto::edit::WithInstrumentMoved(bank, 2, 1);
    const sar::BankFile removed = hokuto::edit::WithoutInstrument(bank, 2);

    // A new instrument plays its wave over every key, through a new entry of the table of waves.
    XSFLIB_CHECK(added.instruments.size() == 6 && added.waves.size() == 3 && added.waves[2] == kOther);
    XSFLIB_CHECK(hokuto::edit::RegionsOf(*added.instruments[5]).size() == 1 &&
                 added.instruments[5]->key_regions[0].regions[0].values.wave_id_index == 2);
    XSFLIB_CHECK(empty.instruments[1] && empty.instruments[1]->key_regions.empty());
    XSFLIB_CHECK(copied.instruments.size() == 8 && copied.instruments[7] == bank.instruments[2]);
    XSFLIB_CHECK(moved.instruments.size() == 2 && moved.instruments[1] == bank.instruments[2]);
    XSFLIB_CHECK(removed.instruments.size() == 1 && removed.instruments[0] == bank.instruments[0]);

    // A layout in another order than the programs lists the new programs last.
    sar::BankFile ordered = bank;
    ordered.order = {2, 0};
    XSFLIB_CHECK(hokuto::edit::WithInstrumentCopy(ordered, 0, 3).order == std::vector<uint32_t>({2, 0, 3}));
    XSFLIB_CHECK(hokuto::edit::WithInstrumentMoved(ordered, 2, 4).order == std::vector<uint32_t>({4, 0}));
    XSFLIB_CHECK(hokuto::edit::WithoutInstrument(ordered, 2).order == std::vector<uint32_t>({0}));

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithNewInstrument(bank, 0, std::nullopt); }) ==
                 "Program 0 holds an instrument already.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithoutInstrument(bank, 1); }) ==
                 "Program 1 holds no instrument.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithInstrumentCopy(bank, 0, 0x10000); }) ==
                 "A program goes up to 65535.");
}

// A region's new wave goes into the table of waves, and a wave that no region plays leaves it.
void TestWaveTable()
{
    sar::BankFile bank = TestBank(sar::Flavor::kNx);
    std::vector<Region> regions = hokuto::edit::RegionsOf(*bank.instruments[2]);
    regions[0].values.wave_id_index = hokuto::edit::WaveEntry(bank, {kWavesId, 7});

    const sar::BankFile edited = hokuto::edit::WithRegions(bank, 2, regions);

    XSFLIB_CHECK(edited.waves == std::vector<sar::WaveId>({{kWavesId, kSine}, {kWavesId, 7}}));
    XSFLIB_CHECK(edited.instruments[2]->key_regions[0].regions[0].values.wave_id_index == 1);
    XSFLIB_CHECK(edited.instruments[0] == bank.instruments[0]);
    regions[0].values.wave_id_index = 5;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithRegions(bank, 2, regions); }) ==
                 "A region plays entry 5 of the bank's table of waves. The table holds 3 entries.");
}

// An edited bank goes into the archive, and the file keeps its model.
void TestBankEdits(sar::Flavor flavor)
{
    const File file = hokuto::format::ImportArchive(TestArchive(flavor), flavor);
    const std::shared_ptr<const sar::SoundArchive> archive = Load(file);
    const sar::BankFile model = hokuto::edit::BankModelOf(file, *archive, kBank);
    SEQLIB_REQUIRE(!model.instruments.empty() && model.instruments[0]);
    std::vector<Region> regions = hokuto::edit::RegionsOf(*model.instruments[0]);
    SEQLIB_REQUIRE(!regions.empty());
    regions[0].last_key = 59;
    regions.push_back(regions[0]);
    regions[1].first_key = 60;
    regions[1].last_key = 127;
    regions[1].values.volume = 90;
    regions[1].values.adshr = {120, 110, 100, 90, 80};

    const File edited =
        hokuto::edit::WithBankModel(file, *archive, kBank, hokuto::edit::WithRegions(model, 0, regions));
    const std::shared_ptr<const sar::SoundArchive> after = Load(edited);

    // The archive's bank lays out the new model, and the file keeps it.
    XSFLIB_CHECK(model == TestBank(flavor) && edited.banks.size() == 1 && edited.banks.count(kBankFile) == 1);
    const sar::BankFile read =
        sar::BankFile::Read(hokuto::song::ItemFile(*after, {hokuto::song::ItemKind::kBank, kBank, 0}));
    XSFLIB_CHECK(read == *edited.banks.at(kBankFile) && hokuto::edit::RegionsOf(*read.instruments[0]) == regions);
    XSFLIB_CHECK(hokuto::edit::BankModelOf(edited, *after, kBank) == read);
    XSFLIB_CHECK(ComesBack(edited));

    // A file in the bank's place drops the model. So does an empty bank.
    const File replaced = hokuto::edit::ReplaceFile(edited, kBankFile, hokuto::edit::EmptyBank(flavor));
    XSFLIB_CHECK(replaced.banks.empty());
    const sar::BankFile empty = hokuto::edit::BankModelOf(replaced, *Load(replaced), kBank);
    XSFLIB_CHECK(empty.instruments.empty() && empty.waves.empty() && empty.flavor == flavor);
    XSFLIB_CHECK(hokuto::edit::WaveEntryName(*archive, model, 1) == "WARC_TEST, wave 1");
    XSFLIB_CHECK(hokuto::edit::WaveEntryName(*archive, model, 5) == "entry 5");
    sar::BankFile other = model;
    other.flavor = flavor == sar::Flavor::kNx ? sar::Flavor::kCafe : sar::Flavor::kNx;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithBankModel(file, *archive, kBank, other); }) ==
                 "The bank is a bank of another console than the archive's.");
}

// A wave's loop, rate and encoding.
void TestWaveEdits(sar::Flavor flavor)
{
    const File file = hokuto::format::ImportArchive(TestArchive(flavor), flavor);
    const std::shared_ptr<const sar::SoundArchive> archive = Load(file);
    const WaveModel sine = hokuto::edit::WaveModelOf(file, *archive, kWaves, kSine);
    const WaveModel kExpected{sar::WaveEncoding::kDspAdpcm, 32000, false, 0, kSineLength, kSineLength, {}};
    XSFLIB_CHECK(sine == kExpected);

    // A loop from frame 5 and a new rate keep the encoded samples. The loop starts again with the decoder's state.
    WaveModel looped = sine;
    looped.loop = true;
    looped.loop_start = 70;
    looped.loop_end = 250;
    looped.rate = 24000;
    const File with_loop = hokuto::edit::WithWaveModel(file, *archive, kWaves, kSine, looped);
    const std::shared_ptr<const sar::SoundArchive> after_loop = Load(with_loop);
    const sar::WaveFile present = WaveOf(*archive, kSine);
    const sar::WaveFile made = WaveOf(*after_loop, kSine);
    XSFLIB_CHECK(made.loop && made.loop_start == 70 && made.loop_end == 250 && made.sample_rate == 24000);
    SEQLIB_REQUIRE(!made.channels.empty() && !present.channels.empty() && made.channels[0].adpcm &&
                   present.channels[0].adpcm);
    XSFLIB_CHECK(made.channels[0].samples == present.channels[0].samples);
    const wave::DspAdpcmState state = wave::DspAdpcmStateAt(
        present.channels[0].samples, 70, present.channels[0].adpcm->coefs, 0, 0, wave::DspAdpcmSum::kWrap);
    XSFLIB_CHECK(made.channels[0].adpcm->loop_pred_scale == state.pred_scale &&
                 made.channels[0].adpcm->loop_yn1 == state.yn1 && made.channels[0].adpcm->loop_yn2 == state.yn2);
    XSFLIB_CHECK(*with_loop.waves.at({kWavesFile, kSine}) == looped && ComesBack(with_loop));

    // The loop can't start between frames while the samples stay, nor end past them.
    WaveModel between = looped;
    between.loop_start = 71;
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithWaveModel(file, *archive, kWaves, kSine, between); }) ==
        "A loop in DSP-ADPCM starts at a frame of 14 samples.");
    WaveModel past = looped;
    past.loop_end = kSineLength + 1;
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithWaveModel(file, *archive, kWaves, kSine, past); }) ==
        "The wave ends its loop after its last sample.");

    // A loop from the wave's end to its end holds no samples. The refusal says how to change it.
    WaveModel empty = sine;
    empty.loop = true;
    empty.loop_start = kSineLength;
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithWaveModel(file, *archive, kWaves, kSine, empty); }) ==
        "The wave has a loop that doesn't end after its start. Turn the loop off, or move its start before its end, "
        "first.");

    // PCM16 holds the decoded samples, and the model keeps them. The ramps' model stays unedited.
    WaveModel pcm = sine;
    pcm.encoding = sar::WaveEncoding::kPcm16;
    const File with_pcm = hokuto::edit::WithWaveModel(file, *archive, kWaves, kSine, pcm);
    const std::vector<std::vector<int16_t>> decoded = hokuto::edit::DecodedSamples(present, kSineLength);
    const sar::Pcm read = hokuto::song::DecodeItemWave(*Load(with_pcm), kWaves, kSine);
    XSFLIB_CHECK(read.channels == decoded && with_pcm.waves.at({kWavesFile, kSine})->samples == decoded);
    XSFLIB_CHECK(with_pcm.waves.size() == 1 && ComesBack(with_pcm));

    // From PCM16 to DSP-ADPCM, a loop between frames moves to the next frame, and its samples repeat after its end.
    const std::shared_ptr<const sar::SoundArchive> pcm_archive = Load(with_pcm);
    WaveModel back = *with_pcm.waves.at({kWavesFile, kSine});
    back.encoding = sar::WaveEncoding::kDspAdpcm;
    back.loop = true;
    back.loop_start = 20;
    back.loop_end = 200;
    const File with_back = hokuto::edit::WithWaveModel(with_pcm, *pcm_archive, kWaves, kSine, back);
    const WaveModel& aligned = *with_back.waves.at({kWavesFile, kSine});
    XSFLIB_CHECK(aligned.loop_start == 28 && aligned.loop_end == 208 && aligned.length == 208);
    const sar::Pcm adpcm = hokuto::song::DecodeItemWave(*Load(with_back), kWaves, kSine);
    XSFLIB_CHECK(adpcm.loop && adpcm.loop_start == 28 && adpcm.loop_end == 208);

    // PCM8 holds the nearest 8-bit samples.
    const WaveModel ramps = hokuto::edit::WaveModelOf(file, *archive, kWaves, kRamps);
    WaveModel pcm8 = ramps;
    pcm8.encoding = sar::WaveEncoding::kPcm8;
    const File with_pcm8 = hokuto::edit::WithWaveModel(file, *archive, kWaves, kRamps, pcm8);
    const sar::Pcm eight = hokuto::song::DecodeItemWave(*Load(with_pcm8), kWaves, kRamps);
    XSFLIB_CHECK(eight.loop && eight.loop_start == 10);
    SEQLIB_REQUIRE(eight.channels.size() == 2 && eight.channels[0].size() > 99 && eight.channels[1].size() > 99);
    XSFLIB_CHECK(eight.channels[0][99] == 9984 && eight.channels[1][99] == -9984);

    // A wave of the edited wave archive keeps its model when another wave changes.
    const File both = hokuto::edit::WithWaveModel(with_pcm, *pcm_archive, kWaves, kRamps, pcm8);
    XSFLIB_CHECK(both.waves.size() == 2 &&
                 both.waves.at({kWavesFile, kSine}) == with_pcm.waves.at({kWavesFile, kSine}));
}

// A wave sound's values and its wave.
void TestWaveSoundEdits(sar::Flavor flavor)
{
    const File file = hokuto::format::ImportArchive(TestArchive(flavor), flavor);
    const std::shared_ptr<const sar::SoundArchive> archive = Load(file);
    const hokuto::edit::WaveSoundFields fields = hokuto::edit::WaveSoundFieldsOf(*archive, kWaveSound);
    XSFLIB_CHECK(fields.values.pan == 20 && fields.values.pitch == 1.5f && fields.places.pan && fields.places.pitch);
    XSFLIB_CHECK(!fields.places.envelope && fields.wave == sar::WaveId({kWavesId, kRamps}));
    sar::WaveSoundFile::WaveSound values = fields.values;
    values.pan = 100;
    values.pitch = 0.75f;

    const File edited = hokuto::edit::WithWaveSoundFields(file, *archive, kWaveSound, values, {kWavesId, kSine});

    const hokuto::edit::WaveSoundFields after = hokuto::edit::WaveSoundFieldsOf(*Load(edited), kWaveSound);
    XSFLIB_CHECK(after.values.pan == 100 && after.values.pitch == 0.75f &&
                 after.wave == sar::WaveId({kWavesId, kSine}));
    values.adshr.release = 1;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::edit::WithWaveSoundFields(file, *archive, kWaveSound, values, fields.wave); })
                     .starts_with("The wave sound file can't take the values."));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WaveSoundFieldsOf(*archive, 0); }) ==
                 "The archive has no wave sound file there.");
}

// New waves of a bank from WAV files: after the waves of the bank's wave archive, or in a new wave archive, each with
// its model. The group that loads the bank takes the new wave archive, and a wave archive that a region comes to play.
void TestBankWaves(sar::Flavor flavor)
{
    // A group loads the test bank and a bank without waves. Another wave archive lies outside the group.
    File file = hokuto::format::ImportArchive(TestArchive(flavor), flavor);
    file = hokuto::edit::AddBank(file, "BANK_EMPTY", hokuto::edit::EmptyBank(flavor), {});
    file = hokuto::edit::AddWaveArchive(file, "WARC_OTHER", TestWaves(flavor));
    file = hokuto::edit::AddGroups(file, std::vector<std::string>{"GROUP_TEST"});
    const std::vector<uint32_t> banks = {sar::ItemId(sar::kBankItem, kBank), sar::ItemId(sar::kBankItem, 1)};
    file = hokuto::edit::JoinGroups(file, std::vector<uint32_t>{0}, banks, {});
    const std::shared_ptr<const sar::SoundArchive> archive = Load(file);
    const std::vector<uint8_t> wav = seqlib::WavFile(SineSamples(), 22050);
    const hokuto::edit::IncomingWave incoming = hokuto::edit::ReadIncomingWave(wav, *archive, kWaves, std::nullopt);
    const sar::BankFile test = hokuto::edit::BankModelOf(file, *archive, kBank);
    const sar::BankFile empty = hokuto::edit::BankModelOf(file, *archive, 1);
    sar::BankFile other = test;
    other.waves[1] = {sar::ItemId(sar::kWaveArchiveItem, 1), kRamps};
    const File unjoined = hokuto::edit::ReplaceFile(file, kBankFile, other.Write());

    const hokuto::edit::BankWaves after =
        hokuto::edit::WithBankWaves(file, *archive, kBank, test, {incoming, incoming});
    const hokuto::edit::BankWaves fresh = hokuto::edit::WithBankWaves(file, *archive, 1, empty, {incoming});
    const File played = hokuto::edit::WithBankModel(fresh.file, *archive, 1,
                                                    hokuto::edit::WithNewInstrument(empty, 0, fresh.waves.at(0)));
    const File joined = hokuto::edit::WithBankModel(file, *archive, kBank, other);

    // The test bank's waves go after the wave archive's two, with their models.
    XSFLIB_CHECK(after.waves == std::vector<sar::WaveId>({{kWavesId, 2}, {kWavesId, 3}}));
    XSFLIB_CHECK(hokuto::song::WaveCountOf(*Load(after.file), kWaves) == 4 && after.file.waves.size() == 2);
    XSFLIB_CHECK(*after.file.waves.at({kWavesFile, 3}) == *incoming.model && incoming.model->rate == 22050);
    XSFLIB_CHECK(incoming.model->samples == std::vector<std::vector<int16_t>>({SineSamples()}));

    // The empty bank's wave goes into a new wave archive, WARC_EMPTY. The group loads the new wave archive.
    const std::shared_ptr<const sar::SoundArchive> built = Load(played);
    XSFLIB_CHECK(built->WaveArchives().size() == 3 && built->WaveArchives()[2].name == "WARC_EMPTY");
    XSFLIB_CHECK(fresh.waves == std::vector<sar::WaveId>({{sar::ItemId(sar::kWaveArchiveItem, 2), 0}}));
    XSFLIB_CHECK(played.waves.size() == 1 && played.waves.count({built->WaveArchives()[2].file_id, 0}) == 1);
    XSFLIB_CHECK(hokuto::edit::GroupsLackingWaves(played, *built, 1).empty());
    SEQLIB_REQUIRE(!played.banks.empty() && played.banks.begin()->second);
    XSFLIB_CHECK(hokuto::edit::WaveEntryName(*built, *played.banks.begin()->second, 0) == "WARC_EMPTY, wave 0");
    XSFLIB_CHECK(ComesBack(played));

    // A region that plays a wave of the other wave archive leaves the group without it, until the bank's edit.
    XSFLIB_CHECK(hokuto::edit::GroupsLackingWaves(unjoined, *Load(unjoined), kBank) == std::vector<uint32_t>({0}));
    XSFLIB_CHECK(hokuto::edit::GroupsLackingWaves(joined, *Load(joined), kBank).empty());
}

// The items and files of the Wii's archive.
constexpr uint32_t kWiiBankFile = 1;
constexpr uint32_t kWiiWaveSoundsFile = 2;
constexpr uint32_t kWiiWaveSound = 1;
constexpr uint32_t kWiiBankWaves = sar::ItemId(sar::kWaveArchiveItem, kWiiBankFile);
constexpr uint32_t kWiiSoundWaves = sar::ItemId(sar::kWaveArchiveItem, kWiiWaveSoundsFile);

// A wave of the Wii of `channels` in `encoding` at 32 kHz, with a loop from `loop_start` to its end when it has one.
rsar::WaveFile WiiWave(rsar::WaveEncoding encoding, const std::vector<std::vector<int16_t>>& channels,
                       std::optional<uint32_t> loop_start)
{
    rsar::WaveFile made;
    made.encoding = encoding;
    made.loop = loop_start.has_value();
    made.loop_start = loop_start.value_or(0);
    made.loop_end = static_cast<uint32_t>(channels[0].size());
    for (const std::vector<int16_t>& samples : channels)
    {
        rsar::WaveFile::Channel& channel = made.channels.emplace_back();
        if (encoding == rsar::WaveEncoding::kDspAdpcm)
        {
            const wave::DspAdpcmChannel encoded =
                wave::EncodeDspAdpcm(samples, made.loop_start, wave::DspAdpcmSum::kWide);
            rsar::DspAdpcmInfo info;
            info.coefs = encoded.coefs;
            info.pred_scale = encoded.pred_scale;
            info.loop_pred_scale = encoded.loop_pred_scale;
            info.loop_yn1 = encoded.loop_yn1;
            info.loop_yn2 = encoded.loop_yn2;
            channel.samples = encoded.data;
            channel.adpcm = info;
            continue;
        }
        channel.samples.resize(samples.size() * 2);
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            xsflib::StoreBe16(&channel.samples[i * 2], static_cast<uint16_t>(samples[i]));
        }
    }

    return made;
}

// The ramps' two channels: up in the first and down in the second, kRampLength samples each.
std::vector<std::vector<int16_t>> RampSamples()
{
    std::vector<std::vector<int16_t>> channels(2, std::vector<int16_t>(kRampLength));
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < kRampLength; ++i)
        {
            channels[c][i] = RampSample(c, i);
        }
    }

    return channels;
}

// An instrument of the Wii with one region from `first_key` up, over every velocity.
rsar::BankFile::Instrument WiiInstrument(uint8_t first_key, const rsar::Region& region)
{
    rsar::BankFile::Instrument instrument;
    instrument.key_regions = {{first_key, 127, {{0, 127, region}}}};

    return instrument;
}

// The Wii's archive: a sequence sound that plays the bank, a wave sound, the bank with its two waves in its wave data,
// and a wave sound file with its two waves. ntdlib lays the archive out from its model as NintendoWare for Revolution's
// tools do. Two groups hold the bank. The bank's program 0 plays the sine over every key, with a tune of 1.5, a hold of
// 5, key group 2 and its note-offs ignored, and its program 2 plays the ramps over keys 60 to 127. The wave sound file
// holds the ramps in DSP-ADPCM, with a loop from sample 10, and the sine. Its wave sound plays the sine, with an attack
// of 100. The sine's region holds `surround_pan`.
std::vector<uint8_t> WiiArchive(uint8_t surround_pan = 0)
{
    rsar::Region sine;
    sine.tune = 1.5f;
    sine.hold = 5;
    sine.key_group = 2;
    sine.note_off_type = 1;
    sine.surround_pan = surround_pan;
    rsar::Region ramps;
    ramps.wave_index = 1;
    std::vector<std::optional<rsar::BankFile::Instrument>> instruments(3);
    instruments[0] = WiiInstrument(0, sine);
    instruments[2] = WiiInstrument(60, ramps);
    const rsar::BankFile::Files bank =
        rsar::BankFile::FromInstruments(0x0102, instruments,
                                        {WiiWave(rsar::WaveEncoding::kDspAdpcm, {SineSamples()}, std::nullopt),
                                         WiiWave(rsar::WaveEncoding::kPcm16, RampSamples(), 10)})
            .Write();

    rsar::WsdFile sounds;
    rsar::WsdFile::WaveSound& sound = sounds.sounds.emplace_back();
    sound.pan = 20;
    sound.pitch = 1.5f;
    sound.fx_sends = {10, 0, 0};
    sound.tracks = {{{{0.0f, 0.0f, 0}}}};
    sound.notes = {{.wave_index = 1, .attack = 100}};
    sounds.waves = {WiiWave(rsar::WaveEncoding::kDspAdpcm, RampSamples(), 10),
                    WiiWave(rsar::WaveEncoding::kDspAdpcm, {SineSamples()}, std::nullopt)};
    const rsar::WsdFile::Files wave_sounds = sounds.Write();

    rsar::SequenceFile song;
    song.data = {0xC1, 0x64, 0x3C, 0x64, 0x30, 0x80, 0x30, 0xFF};
    song.labels = {{"SEQ_TEST", 0}};
    rsar::ArchiveModel model;
    model.sounds = {{.name = "SEQ_TEST", .file_id = 0},
                    {.name = "WSD_TEST", .type = rsar::SoundType::kWaveSound, .file_id = kWiiWaveSoundsFile}};
    model.banks = {{.name = "BANK_TEST", .file_id = kWiiBankFile}};
    model.players = {{.name = "PLAYER_TEST"}};
    model.groups = {{.name = "GROUP_ALL", .file_ids = {0, kWiiBankFile, kWiiWaveSoundsFile}},
                    {.name = "GROUP_BANK", .file_ids = {kWiiBankFile}}};
    model.files = {{.bytes = song.Write()},
                   {.bytes = bank.file, .wave_data = bank.wave_data},
                   {.bytes = wave_sounds.file, .wave_data = wave_sounds.wave_data}};

    return rsar::BuildArchive(model);
}

// The archive of the Wii that `file` exports. The program opens an archive the same way.
std::shared_ptr<const rsar::SoundArchive> LoadWii(const File& file)
{
    return hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(file), "test.brsar",
                                        ntdlib::rvl::snd::kRsarOptions);
}

// The copies of file `id`, or of its wave data, in each group of the Wii's archive that `file` builds.
std::vector<std::vector<uint8_t>> WiiCopies(const File& file, uint32_t id, bool wave_data)
{
    const std::vector<uint8_t> bytes = hokuto::format::ExportArchive(file);
    const std::shared_ptr<const rsar::SoundArchive> archive = LoadWii(file);
    std::vector<std::vector<uint8_t>> copies;
    for (const rsar::SoundArchive::Group& group : archive->Groups())
    {
        for (const rsar::SoundArchive::Group::Item& item : group.items)
        {
            if (item.file_id == id)
            {
                const std::size_t start =
                    wave_data ? group.wave_data_offset + item.wave_data_offset : group.offset + item.offset;
                const std::size_t size = wave_data ? item.wave_data_size : item.size;
                copies.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                    bytes.begin() + static_cast<std::ptrdiff_t>(start + size));
            }
        }
    }

    return copies;
}

// A bank of the Wii in the other consoles' model: its waves name the bank's wave data, and the regions keep their
// values. A new volume and a new instrument reach the bank in both groups, and the archive gives the edited model back.
// A region that plays a wave of the wave sound file's wave data takes a copy of the wave, and new waves go after the
// bank's. The waves keep their models through the bank's edits. A region with a surround pan is refused. An empty bank
// takes the version of the archive's bank. An empty bank with waves holds them.
void TestWiiBankEdits()
{
    const File file = hokuto::format::ImportWiiArchive(WiiArchive());
    const std::shared_ptr<const rsar::SoundArchive> archive = LoadWii(file);
    const sar::BankFile model = hokuto::edit::BankModelOf(*archive, kBank);
    XSFLIB_CHECK(model.version == 0x0102 &&
                 model.waves == std::vector<sar::WaveId>({{kWiiBankWaves, kSine}, {kWiiBankWaves, kRamps}}));
    SEQLIB_REQUIRE(model.instruments.size() == 3 && model.instruments[0] && !model.instruments[1] &&
                   model.instruments[2]);
    const std::vector<Region> regions = hokuto::edit::RegionsOf(*model.instruments[0]);
    SEQLIB_REQUIRE(regions.size() == 1);
    XSFLIB_CHECK(regions[0].values.wave_id_index == 0 && regions[0].values.pitch == 1.5f);
    XSFLIB_CHECK(regions[0].values.adshr.hold == 5 && regions[0].values.key_group == 2 &&
                 regions[0].values.ignore_note_off && regions[0].values.adshr.attack == 127);
    XSFLIB_CHECK(hokuto::edit::RegionsOf(*model.instruments[2]).at(0).first_key == 60 &&
                 hokuto::edit::RegionsOf(*model.instruments[2]).at(0).values.wave_id_index == 1);
    XSFLIB_CHECK(hokuto::edit::WaveEntryName(*archive, model, 1) == "BANK_TEST, wave 1");

    // A new volume, and a new instrument at program 3 that plays the ramps.
    std::vector<Region> louder = regions;
    louder[0].values.volume = 100;
    sar::BankFile edited = hokuto::edit::WithRegions(model, 0, louder);
    edited = hokuto::edit::WithNewInstrument(std::move(edited), 3, sar::WaveId{kWiiBankWaves, kRamps});
    const File after = hokuto::edit::WithBankModel(file, *archive, kBank, edited);
    XSFLIB_CHECK(hokuto::edit::BankModelOf(*LoadWii(after), kBank) == edited);
    const std::vector<std::vector<uint8_t>> copies = WiiCopies(after, kWiiBankFile, false);
    SEQLIB_REQUIRE(!WiiCopies(file, kWiiBankFile, false).empty());
    XSFLIB_CHECK(copies.size() == 2 && copies[0] == copies[1] && copies[0] != WiiCopies(file, kWiiBankFile, false)[0]);
    XSFLIB_CHECK(WiiCopies(after, kWiiBankFile, true) == WiiCopies(file, kWiiBankFile, true));
    XSFLIB_CHECK(WiiCopies(after, kWiiWaveSoundsFile, false) == WiiCopies(file, kWiiWaveSoundsFile, false));
    XSFLIB_CHECK(ComesBack(after));

    // A region of program 0 that plays the wave sound file's ramps takes a copy of them after the bank's waves.
    std::vector<Region> copied = regions;
    sar::BankFile copying = model;
    copied[0].values.wave_id_index = hokuto::edit::WaveEntry(copying, {kWiiSoundWaves, 0});
    copying = hokuto::edit::WithRegions(std::move(copying), 0, copied);
    const std::shared_ptr<const rsar::SoundArchive> with_copy =
        LoadWii(hokuto::edit::WithBankModel(file, *archive, kBank, copying));
    const std::vector<rsar::WaveFile> bank_waves = hokuto::edit::WavesOf(*with_copy, kWiiBankFile);
    XSFLIB_CHECK(bank_waves.size() == 3 && bank_waves[2] == hokuto::edit::WavesOf(*archive, kWiiWaveSoundsFile)[0]);
    const sar::BankFile read_copy = hokuto::edit::BankModelOf(*with_copy, kBank);
    XSFLIB_CHECK(hokuto::edit::RegionsOf(*read_copy.instruments[0]).at(0).values.wave_id_index == 2);

    // NewWaveIds names a new wave after the bank's waves.
    const std::vector<sar::WaveId> ids = hokuto::edit::NewWaveIds(*archive, kBank, 1);
    XSFLIB_CHECK(ids == std::vector<sar::WaveId>({{kWiiBankWaves, 2}}));
    const rsar::WaveFile added = WiiWave(rsar::WaveEncoding::kPcm16, {{1, 2, 3, 4}}, std::nullopt);
    const File with_new =
        hokuto::edit::WithBankModel(file, *archive, kBank, hokuto::edit::WithNewInstrument(model, 4, ids[0]), {added});
    const std::shared_ptr<const rsar::SoundArchive> with_new_archive = LoadWii(with_new);
    XSFLIB_CHECK(hokuto::edit::WavesOf(*with_new_archive, kWiiBankFile).size() == 3 &&
                 hokuto::edit::PresentWave(*with_new_archive, kWiiBankFile, 2).loop_end == 4);
    XSFLIB_CHECK(hokuto::edit::RegionsOf(*hokuto::edit::BankModelOf(*with_new_archive, kBank).instruments.at(4))
                     .at(0)
                     .values.wave_id_index == 2);

    // The ramps' model stays through an edit of the instruments.
    WaveModel slower = hokuto::edit::WaveModelOf(file, *archive, kWiiBankFile, kRamps);
    slower.rate = 16000;
    const File with_rate = hokuto::edit::WithWaveModel(file, *archive, kWiiBankFile, kRamps, slower);
    const std::shared_ptr<const rsar::SoundArchive> rate_archive = LoadWii(with_rate);
    const File both = hokuto::edit::WithBankModel(
        with_rate, *rate_archive, kBank,
        hokuto::edit::WithRegions(hokuto::edit::BankModelOf(*rate_archive, kBank), 0, louder));
    XSFLIB_CHECK(both.waves.size() == 1 && *both.waves.at({kWiiBankFile, kRamps}) == slower);
    XSFLIB_CHECK(hokuto::edit::PresentWave(*LoadWii(both), kWiiBankFile, kRamps).sample_rate == 16000);

    // A surround pan is refused. The empty bank takes version 1.2, without instruments or waves.
    const std::shared_ptr<const rsar::SoundArchive> surround = LoadWii(hokuto::format::ImportWiiArchive(WiiArchive(1)));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::BankModelOf(*surround, kBank); })
                     .starts_with("Hokuto can't edit this bank. A region of program 0 holds a surround pan"));
    const rsar::BankFile::Files empty = hokuto::edit::EmptyBank(*archive);
    const rsar::BankFile empty_bank = rsar::BankFile::Read(empty.file, empty.wave_data);
    XSFLIB_CHECK(empty_bank.version == 0x0102 && empty_bank.instruments.empty() && empty_bank.waves.empty());
    const std::vector<rsar::WaveFile> held = hokuto::edit::WavesOf(*archive, kWiiWaveSoundsFile);
    const rsar::BankFile::Files holding = hokuto::edit::EmptyBank(*archive, held);
    const rsar::BankFile holding_bank = rsar::BankFile::Read(holding.file, holding.wave_data);
    XSFLIB_CHECK(holding_bank.version == 0x0102 && holding_bank.instruments.empty() && holding_bank.waves == held);
}

// A wave of the Wii's bank: a loop and a rate keep its samples, and the loop starts with the decoder's state. 16-bit
// PCM holds the decoded samples. The bank's other wave and its instruments stay. A wave of the wave sound file whose
// loop starts between frames keeps its loop through a new rate. A loop that moves there has to start at a frame. PCM8
// holds the nearest 8-bit samples, and the wave sound keeps its values.
void TestWiiWaveEdits()
{
    const File file = hokuto::format::ImportWiiArchive(WiiArchive());
    const std::shared_ptr<const rsar::SoundArchive> archive = LoadWii(file);
    const WaveModel sine = hokuto::edit::WaveModelOf(file, *archive, kWiiBankFile, kSine);
    const WaveModel kExpected{sar::WaveEncoding::kDspAdpcm, 32000, false, 0, kSineLength, kSineLength, {}};
    XSFLIB_CHECK(sine == kExpected);

    WaveModel looped = sine;
    looped.loop = true;
    looped.loop_start = 70;
    looped.loop_end = 250;
    looped.rate = 24000;
    const File with_loop = hokuto::edit::WithWaveModel(file, *archive, kWiiBankFile, kSine, looped);
    const std::shared_ptr<const rsar::SoundArchive> after_loop = LoadWii(with_loop);
    const rsar::WaveFile present = hokuto::edit::PresentWave(*archive, kWiiBankFile, kSine);
    const rsar::WaveFile made = hokuto::edit::PresentWave(*after_loop, kWiiBankFile, kSine);
    XSFLIB_CHECK(made.loop && made.loop_start == 70 && made.loop_end == 250 && made.sample_rate == 24000);

    // The made wave ends at its loop's end, sample 250. The reader still takes the DATA block's bytes for samples: the
    // present wave's samples and the zeros after them.
    SEQLIB_REQUIRE(!made.channels.empty() && !present.channels.empty() && made.channels[0].adpcm &&
                   present.channels[0].adpcm);
    const std::vector<uint8_t>& held = made.channels[0].samples;
    const std::vector<uint8_t>& samples = present.channels[0].samples;
    XSFLIB_CHECK(held.size() >= samples.size() && std::equal(samples.begin(), samples.end(), held.begin()) &&
                 std::all_of(held.begin() + static_cast<std::ptrdiff_t>(samples.size()), held.end(),
                             [](uint8_t byte) { return byte == 0; }));
    const wave::DspAdpcmState state = wave::DspAdpcmStateAt(
        present.channels[0].samples, 70, present.channels[0].adpcm->coefs, 0, 0, wave::DspAdpcmSum::kWide);
    XSFLIB_CHECK(made.channels[0].adpcm->loop_pred_scale == state.pred_scale &&
                 made.channels[0].adpcm->loop_yn1 == state.yn1 && made.channels[0].adpcm->loop_yn2 == state.yn2);
    XSFLIB_CHECK(*with_loop.waves.at({kWiiBankFile, kSine}) == looped && ComesBack(with_loop));
    XSFLIB_CHECK(hokuto::edit::PresentWave(*after_loop, kWiiBankFile, kRamps) ==
                 hokuto::edit::PresentWave(*archive, kWiiBankFile, kRamps));
    XSFLIB_CHECK(hokuto::edit::BankModelOf(*after_loop, kBank) == hokuto::edit::BankModelOf(*archive, kBank));
    const std::vector<std::vector<uint8_t>> copies = WiiCopies(with_loop, kWiiBankFile, true);
    XSFLIB_CHECK(copies.size() == 2 && copies[0] == copies[1]);

    // 16-bit PCM holds the decoded samples, and the model keeps them.
    WaveModel pcm = sine;
    pcm.encoding = sar::WaveEncoding::kPcm16;
    const File with_pcm = hokuto::edit::WithWaveModel(file, *archive, kWiiBankFile, kSine, pcm);
    const std::vector<std::vector<int16_t>> decoded = hokuto::edit::DecodedSamples(present, kSineLength);
    const rsar::Pcm read = hokuto::song::DecodeItemWave(*LoadWii(with_pcm), kWiiBankFile, kSine);
    XSFLIB_CHECK(read.channels == decoded && with_pcm.waves.at({kWiiBankFile, kSine})->samples == decoded);
    XSFLIB_CHECK(hokuto::song::DecodeItemWave(*archive, kWiiBankFile, kSine).channels == decoded);

    // The wave sound file's ramps loop from sample 10, between frames. A new rate keeps the loop and its state.
    WaveModel ramps = hokuto::edit::WaveModelOf(file, *archive, kWiiWaveSoundsFile, 0);
    XSFLIB_CHECK(ramps.loop && ramps.loop_start == 10 && ramps.encoding == sar::WaveEncoding::kDspAdpcm);
    ramps.rate = 44100;
    const File with_ramps = hokuto::edit::WithWaveModel(file, *archive, kWiiWaveSoundsFile, 0, ramps);
    rsar::WaveFile ramps_after = hokuto::edit::PresentWave(*LoadWii(with_ramps), kWiiWaveSoundsFile, 0);
    XSFLIB_CHECK(ramps_after.sample_rate == 44100);
    ramps_after.sample_rate = 32000;
    XSFLIB_CHECK(ramps_after == hokuto::edit::PresentWave(*archive, kWiiWaveSoundsFile, 0));
    WaveModel moved = ramps;
    moved.loop_start = 11;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::edit::WithWaveModel(file, *archive, kWiiWaveSoundsFile, 0, moved); }) ==
                 "A loop in DSP-ADPCM starts at a frame of 14 samples.");

    // PCM8 holds the nearest 8-bit samples. The wave sound keeps its values.
    WaveModel pcm8 = hokuto::edit::WaveModelOf(file, *archive, kWiiBankFile, kRamps);
    pcm8.encoding = sar::WaveEncoding::kPcm8;
    const File with_pcm8 = hokuto::edit::WithWaveModel(file, *archive, kWiiBankFile, kRamps, pcm8);
    const rsar::Pcm eight = hokuto::song::DecodeItemWave(*LoadWii(with_pcm8), kWiiBankFile, kRamps);
    XSFLIB_CHECK(eight.loop && eight.loop_start == 10);
    SEQLIB_REQUIRE(eight.channels.size() == 2 && eight.channels[0].size() > 99 && eight.channels[1].size() > 99);
    XSFLIB_CHECK(eight.channels[0][99] == 9984 && eight.channels[1][99] == -9984);
    WaveModel sound_pcm8 = hokuto::edit::WaveModelOf(file, *archive, kWiiWaveSoundsFile, 1);
    sound_pcm8.encoding = sar::WaveEncoding::kPcm8;
    const File with_sound_pcm8 = hokuto::edit::WithWaveModel(file, *archive, kWiiWaveSoundsFile, 1, sound_pcm8);
    XSFLIB_CHECK(hokuto::edit::WaveSoundFieldsOf(*LoadWii(with_sound_pcm8), kWiiWaveSound).values.pan == 20);
}

// A wave sound of the Wii in the other consoles' terms: its settings and its note's envelope and wave, with places for
// all but the filters. New values and the other wave reach the file, and the waves keep their models. Filters, a wave
// of the bank and a sound of another kind are refused.
void TestWiiWaveSoundEdits()
{
    const File file = hokuto::format::ImportWiiArchive(WiiArchive());
    const std::shared_ptr<const rsar::SoundArchive> archive = LoadWii(file);
    const hokuto::edit::WaveSoundFields fields = hokuto::edit::WaveSoundFieldsOf(*archive, kWiiWaveSound);
    XSFLIB_CHECK(fields.values.pan == 20 && fields.values.pitch == 1.5f && fields.values.aux_sends[0] == 10);
    XSFLIB_CHECK(fields.values.adshr.attack == 100 && fields.values.notes.size() == 1 &&
                 fields.values.notes[0].original_key == 60);
    XSFLIB_CHECK(fields.places.pan && fields.places.pitch && fields.places.sends && fields.places.aux_sends == 3 &&
                 fields.places.envelope && !fields.places.filters);
    XSFLIB_CHECK(fields.wave == sar::WaveId({kWiiSoundWaves, 1}));

    // New values, and the ramps. The sine keeps its model.
    WaveModel slower = hokuto::edit::WaveModelOf(file, *archive, kWiiWaveSoundsFile, 1);
    slower.rate = 16000;
    const File with_rate = hokuto::edit::WithWaveModel(file, *archive, kWiiWaveSoundsFile, 1, slower);
    const std::shared_ptr<const rsar::SoundArchive> rate_archive = LoadWii(with_rate);
    sar::WaveSoundFile::WaveSound values = fields.values;
    values.pan = 100;
    values.pitch = 0.75f;
    values.adshr.release = 1;
    values.aux_sends[2] = 30;
    const File edited =
        hokuto::edit::WithWaveSoundFields(with_rate, *rate_archive, kWiiWaveSound, values, {kWiiSoundWaves, 0});
    const hokuto::edit::WaveSoundFields after = hokuto::edit::WaveSoundFieldsOf(*LoadWii(edited), kWiiWaveSound);
    XSFLIB_CHECK(after.values.pan == 100 && after.values.pitch == 0.75f && after.values.adshr.release == 1);
    XSFLIB_CHECK(after.values.aux_sends[2] == 30 && after.wave == sar::WaveId({kWiiSoundWaves, 0}));
    XSFLIB_CHECK(edited.waves.size() == 1 && *edited.waves.at({kWiiWaveSoundsFile, 1}) == slower && ComesBack(edited));

    // The refusals.
    sar::WaveSoundFile::WaveSound filtered = fields.values;
    filtered.low_pass = 10;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&]
                     { hokuto::edit::WithWaveSoundFields(file, *archive, kWiiWaveSound, filtered, fields.wave); }) ==
                 "A wave sound of the Wii has no filters.");
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>(
            [&]
            { hokuto::edit::WithWaveSoundFields(file, *archive, kWiiWaveSound, fields.values, {kWiiBankWaves, 0}); }) ==
        "A wave sound of the Wii plays a wave of its wave sound file's wave data.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::edit::WaveSoundFieldsOf(*archive, 0); }) ==
                 "The sound isn't a wave sound.");
}

} // namespace

int main()
{
    TestRegions();
    TestInstruments();
    TestWaveTable();
    for (const sar::Flavor flavor : {sar::Flavor::kNx, sar::Flavor::kCafe})
    {
        TestBankEdits(flavor);
        TestWaveEdits(flavor);
        TestWaveSoundEdits(flavor);
        TestBankWaves(flavor);
    }
    TestWiiBankEdits();
    TestWiiWaveEdits();
    TestWiiWaveSoundEdits();
    for (const sar::Flavor flavor : {sar::Flavor::kNx, sar::Flavor::kCafe, sar::Flavor::kCtr})
    {
        TestMonoWave(flavor);
    }

    return xsflib::testing::Summary("banks");
}
