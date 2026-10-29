// SPDX-License-Identifier: MIT

// Tests what the Bank Editor and the properties of a wave or a stream show of their items, the waves that a drop places
// on a bank, and the places of a loop's markers (src/app/item_views.h). A small archive of the Switch, and the same
// archive of the Wii U, hold a sequence sound that plays the bank, a wave sound, the bank, a wave archive of two waves
// and a wave sound file. The bank's program 0 plays the first wave over every key, and its program 2 the second wave
// over keys 60 to 127. The first wave is a sine of 280 samples at 32 kHz in DSP-ADPCM without a loop, and the second a
// ramp of 100 samples at 48 kHz in PCM16 with a loop from 10. The wave sound plays the second wave. A copy of the
// archive adds a sound that plays the same wave sound. Another archive names two stream files beside it. Two stream
// sounds play the first, and a third the second. The test's import lacks the second. An archive of the Wii names a
// stream file beside it, and three stream sounds play it with different tracks. The Bank Editor shows the bank of the
// same archive of the 3DS too. Another archive of the Wii holds the sine and the ramp in the wave data of its bank and
// of a wave sound file. The views show that archive's bank, waves and wave sounds by the rules of the file's build of
// NW4R SND.

#include "app/item_views.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
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
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/wave/encode.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "app/key_edits.h"
#include "edit/banks.h"
#include "edit/groups.h"
#include "edit/new_items.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"

using hokuto::app::DroppedWaves;
using hokuto::app::LoopMarkerPlace;
using hokuto::app::WaveValues;
using hokuto::app::WaveView;
using hokuto::format::File;
using xsflib::testing::ErrorOf;

namespace
{

namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
namespace wave = ntdlib::wave;
namespace sar_files = ntdlib::testing::sar_files;

// The archive's items and files.
constexpr uint32_t kBank = 0;
constexpr uint32_t kWaves = 0;
constexpr uint32_t kBankFile = 1;
constexpr uint32_t kWavesFile = 2;
constexpr uint32_t kWaveSoundsFile = 3;
constexpr uint32_t kWavesId = 0x05000000;

// The waves.
constexpr uint32_t kSine = 0;
constexpr uint32_t kRamp = 1;
constexpr uint32_t kSineLength = 280;
constexpr uint32_t kRampLength = 100;

// The stream sounds, and the samples in each channel of their stream.
constexpr uint32_t kStereoSound = 0;
constexpr uint32_t kMonoSound = 1;
constexpr uint32_t kMissingSound = 2;
constexpr uint32_t kStreamFile = 0;
constexpr std::size_t kStreamLength = 20000;

// A run's least and most sample.
using Peak = std::pair<int16_t, int16_t>;

// The bank: program 0 plays the sine over every key, and program 2 the ramp over keys 60 to 127.
sar::BankFile TestBank(sar::Flavor flavor)
{
    sar::BankFile bank;
    bank.flavor = flavor;
    bank.waves = {{kWavesId, kSine}, {kWavesId, kRamp}};
    bank.instruments.resize(3);
    sar::BankFile::Region ramp;
    ramp.values.wave_id_index = 1;
    bank.instruments[0].emplace().key_regions = {{0, 127, {sar::BankFile::Region()}}};
    bank.instruments[2].emplace().key_regions = {{60, 127, {ramp}}};

    return bank;
}

// The sine of the wave archive.
std::vector<int16_t> SineSamples()
{
    std::vector<int16_t> samples(kSineLength);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        samples[i] = static_cast<int16_t>(std::lround(8000.0 * std::sin(static_cast<double>(i) * 0.3)));
    }

    return samples;
}

// The ramp of the wave archive.
std::vector<int16_t> RampSamples()
{
    std::vector<int16_t> samples(kRampLength);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        samples[i] = static_cast<int16_t>(i * 100);
    }

    return samples;
}

// The wave archive: the sine in DSP-ADPCM, and the ramp in PCM16 with a loop from 10.
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

    sar::WaveFile ramp;
    ramp.flavor = flavor;
    ramp.loop = true;
    ramp.loop_start = 10;
    ramp.original_loop_start = 10;
    ramp.loop_end = kRampLength;
    const std::vector<int16_t> ramp_samples = RampSamples();
    std::vector<uint8_t> bytes(kRampLength * 2);
    for (std::size_t i = 0; i < kRampLength; ++i)
    {
        const auto sample = static_cast<uint16_t>(ramp_samples[i]);
        flavor == sar::Flavor::kCafe ? xsflib::StoreBe16(&bytes[i * 2], sample)
                                     : xsflib::StoreLe16(&bytes[i * 2], sample);
    }
    ramp.channels.push_back({std::move(bytes), std::nullopt});

    sar::WaveArchiveFile archive;
    archive.flavor = flavor;
    archive.waves = {sine.Write(), ramp.Write()};

    return archive.Write();
}

// The archive. With `twin`, a third sound plays the same wave sound of the wave sound file.
std::vector<uint8_t> TestArchive(sar::Flavor flavor, bool twin = false)
{
    sar::ArchiveModel model;
    model.flavor = flavor;
    model.version = flavor == sar::Flavor::kNx ? 0x00020500 : flavor == sar::Flavor::kCtr ? 0x02000000 : 0x00020200;
    sar::SoundModel song;
    song.name = "SEQ_TEST";
    song.banks = {sar::ItemId(sar::kBankItem, kBank)};
    sar::SoundModel wave_sound;
    wave_sound.name = "WSD_TEST";
    wave_sound.type = sar::SoundType::kWaveSound;
    wave_sound.file_id = kWaveSoundsFile;
    model.sounds = {song, wave_sound};
    if (twin)
    {
        wave_sound.name = "WSD_TWIN";
        model.sounds.push_back(wave_sound);
    }
    model.banks = {{.name = "BANK_TEST", .file_id = kBankFile}};
    model.wave_archives = {{.name = "WARC_TEST", .file_id = kWavesFile}};
    model.players = {{.name = "PLAYER_TEST", .playable_sound_max = 1}};

    // The wave sound plays the ramp, with a pan.
    sar_files::WaveSoundSpec sound;
    sound.pan = 20;
    sound.tracks = {{{0.0f, 1.0f, 0}}};
    sound.notes.resize(1);
    sar_files::WaveSoundFileSpec sounds;
    sounds.waves = {{kWavesId, kRamp}};
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

// A number with a comma between each group of three digits.
std::string Grouped(uint64_t number)
{
    std::string digits = std::to_string(number);
    for (std::size_t at = digits.size(); at > 3; at -= 3)
    {
        digits.insert(at - 3, ",");
    }

    return digits;
}

// The stream's two channels: a ramp, and a square wave.
std::vector<std::vector<int16_t>> StreamChannels()
{
    std::vector<std::vector<int16_t>> channels(2, std::vector<int16_t>(kStreamLength));
    for (std::size_t i = 0; i < kStreamLength; ++i)
    {
        channels[0][i] = static_cast<int16_t>(static_cast<int>(i % 4000) * 8 - 16000);
        channels[1][i] = static_cast<int16_t>(i / 50 % 2 == 0 ? 6000 : -6000);
    }

    return channels;
}

// A stream sound's details: the channels that each track plays, and each track's pan. Each track's volume is 100. The
// tracks in `starting` play when the sound starts.
sar::StreamSoundInfo Details(std::vector<std::vector<uint8_t>> tracks, std::vector<uint8_t> pans, uint16_t starting)
{
    sar::StreamSoundInfo info;
    info.channel_count = 2;
    info.valid_tracks = starting;
    for (std::size_t t = 0; t < tracks.size(); t++)
    {
        sar::StreamTrackInfo track;
        track.channels = std::move(tracks[t]);
        track.pan = pans[t];
        track.volume = 100;
        info.tracks.push_back(std::move(track));
    }

    return info;
}

// The archive of streams: two stream sounds play the stream file beside the archive, one with a stereo track and one
// with two mono tracks. A third plays a stream file that the import doesn't find.
std::vector<uint8_t> StreamArchive(sar::Flavor flavor)
{
    sar_files::ArchiveSpec spec;
    spec.files = {{{}, "stream/STRM_SONG.bfstm"}, {{}, "stream/STRM_GONE.bfstm"}};
    spec.players = {{"PLAYER", 1}};
    spec.sounds = {
        {.name = "STRM_SONG",
         .kind = sar_files::SoundSpec::kStream,
         .file = kStreamFile,
         .stream = Details({{0, 1}}, {64}, 1)},
        {.name = "STRM_SONG_MONO",
         .kind = sar_files::SoundSpec::kStream,
         .file = kStreamFile,
         .stream = Details({{0}, {1}}, {0, 127}, 1)},
        {.name = "STRM_GONE", .kind = sar_files::SoundSpec::kStream, .file = 1, .stream = Details({{0, 1}}, {64}, 1)}};

    return sar_files::Archive(flavor, spec);
}

// The files and wave sounds of the Wii's archive.
constexpr uint32_t kWiiBankFile = 1;
constexpr uint32_t kWiiWaveSoundsFile = 2;
constexpr uint32_t kWiiRampSound = 1;
constexpr uint32_t kWiiSineSound = 2;

// A mono wave of the Wii of `samples` in `encoding` at 32 kHz, with a loop from `loop_start` to its end when it has
// one.
rsar::WaveFile WiiWave(rsar::WaveEncoding encoding, const std::vector<int16_t>& samples,
                       std::optional<uint32_t> loop_start)
{
    rsar::WaveFile made;
    made.encoding = encoding;
    made.loop = loop_start.has_value();
    made.loop_start = loop_start.value_or(0);
    made.loop_end = static_cast<uint32_t>(samples.size());
    rsar::WaveFile::Channel& channel = made.channels.emplace_back();
    if (encoding == rsar::WaveEncoding::kDspAdpcm)
    {
        const wave::DspAdpcmChannel encoded = wave::EncodeDspAdpcm(samples, made.loop_start, wave::DspAdpcmSum::kWide);
        rsar::DspAdpcmInfo info;
        info.coefs = encoded.coefs;
        info.pred_scale = encoded.pred_scale;
        info.loop_pred_scale = encoded.loop_pred_scale;
        info.loop_yn1 = encoded.loop_yn1;
        info.loop_yn2 = encoded.loop_yn2;
        channel.samples = encoded.data;
        channel.adpcm = info;
        return made;
    }

    channel.samples.resize(samples.size() * 2);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        xsflib::StoreBe16(&channel.samples[i * 2], static_cast<uint16_t>(samples[i]));
    }

    return made;
}

// The Wii's archive: a sequence sound that plays the bank, the bank with the sine and the ramp in its wave data, and a
// wave sound file whose two wave sounds play the ramp and the sine of its wave data. ntdlib lays the archive out from
// its model as NintendoWare for Revolution's tools do. Two groups hold the bank, and BuildArchive adds a third.
std::vector<uint8_t> WiiArchive()
{
    rsar::Region ramp;
    ramp.wave_index = 1;
    std::vector<std::optional<rsar::BankFile::Instrument>> instruments(3);
    instruments[0].emplace().key_regions = {{0, 127, {{0, 127, rsar::Region()}}}};
    instruments[2].emplace().key_regions = {{60, 127, {{0, 127, ramp}}}};
    const rsar::BankFile::Files bank =
        rsar::BankFile::FromInstruments(0x0102, instruments,
                                        {WiiWave(rsar::WaveEncoding::kDspAdpcm, SineSamples(), std::nullopt),
                                         WiiWave(rsar::WaveEncoding::kPcm16, RampSamples(), 10)})
            .Write();

    rsar::WsdFile sounds;
    for (const int32_t wave : {0, 1})
    {
        rsar::WsdFile::WaveSound& sound = sounds.sounds.emplace_back();
        sound.pan = 20;
        sound.tracks = {{{{0.0f, 0.0f, 0}}}};
        sound.notes = {{.wave_index = wave}};
    }
    sounds.waves = {WiiWave(rsar::WaveEncoding::kPcm16, RampSamples(), 10),
                    WiiWave(rsar::WaveEncoding::kDspAdpcm, SineSamples(), std::nullopt)};
    const rsar::WsdFile::Files wave_sounds = sounds.Write();

    rsar::SequenceFile song;
    song.data = {0xC1, 0x64, 0x3C, 0x64, 0x30, 0x80, 0x30, 0xFF};
    song.labels = {{"SEQ_TEST", 0}};
    rsar::ArchiveModel model;
    model.sounds = {{.name = "SEQ_TEST", .file_id = 0},
                    {.name = "WSD_RAMP", .type = rsar::SoundType::kWaveSound, .file_id = kWiiWaveSoundsFile},
                    {.name = "WSD_SINE",
                     .type = rsar::SoundType::kWaveSound,
                     .file_id = kWiiWaveSoundsFile,
                     .wave_sound_index = 1}};
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

// The first program from a program up that holds no instrument.
void TestFreeProgram()
{
    sar::BankFile full;
    full.instruments.resize(hokuto::edit::kHighestProgram + 1, sar::BankFile::Instrument());

    const sar::BankFile bank = TestBank(sar::Flavor::kNx);

    XSFLIB_CHECK(hokuto::app::FreeProgram(bank, 0) == 1u && hokuto::app::FreeProgram(bank, 2) == 3u);
    XSFLIB_CHECK(hokuto::app::FreeProgram(bank, 500) == 500u);
    XSFLIB_CHECK(!hokuto::app::FreeProgram(full, 0) && !hokuto::app::FreeProgram(bank, 0x10000));
}

// Waves dropped on the list of programs, on a map without an instrument, on a region and on free keys.
void TestDroppedWaves()
{
    const sar::BankFile bank = TestBank(sar::Flavor::kNx);
    const sar::WaveId kFirst{kWavesId + 1, 4};
    const sar::WaveId kSecond{kWavesId + 1, 5};
    sar::BankFile low = bank;
    low.instruments.resize(5);
    low.instruments[4].emplace().key_regions = {{0, 10, {sar::BankFile::Region()}}};
    sar::BankFile full;
    full.instruments.resize(hokuto::edit::kHighestProgram + 1, sar::BankFile::Instrument());

    const DroppedWaves listed = hokuto::app::WithDroppedWaves(bank, {}, {kFirst, kSecond});
    const DroppedWaves free = hokuto::app::WithDroppedWaves(bank, {.program = 5}, {kFirst});
    const DroppedWaves changed = hokuto::app::WithDroppedWaves(bank, {.program = 2, .key = 70}, {kFirst});
    const DroppedWaves keys = hokuto::app::WithDroppedWaves(bank, {.program = 2, .key = 40}, {kFirst, kSecond});

    // On the list, each wave makes an instrument at the next free program, over every key and velocity.
    XSFLIB_CHECK(listed.name == "Add Instruments");
    SEQLIB_REQUIRE(listed.bank.instruments.size() == 4 && listed.bank.instruments[1] && listed.bank.instruments[3]);
    XSFLIB_CHECK(listed.programs == std::vector<uint32_t>({1, 3}) && listed.region == std::pair(0, 0));
    XSFLIB_CHECK(hokuto::edit::RegionsOf(*listed.bank.instruments[1]).size() == 1 &&
                 hokuto::edit::RegionsOf(*listed.bank.instruments[3]).size() == 1);
    XSFLIB_CHECK(listed.bank.waves.size() == 4 && listed.bank.waves[2] == kFirst && listed.bank.waves[3] == kSecond);
    XSFLIB_CHECK(free.name == "Add Instrument" && free.bank.instruments.size() == 6 && free.bank.instruments[5]);
    XSFLIB_CHECK(free.programs == std::vector<uint32_t>({5}));

    // One wave on a region gives the region the wave. The table of waves loses the entry that no region plays.
    SEQLIB_REQUIRE(changed.bank.instruments.size() > 2 && changed.bank.instruments[2] &&
                   !changed.bank.instruments[2]->key_regions.empty() &&
                   !changed.bank.instruments[2]->key_regions[0].regions.empty());
    XSFLIB_CHECK(changed.name == "Change Wave" &&
                 changed.bank.instruments[2]->key_regions[0].regions[0].values.wave_id_index == 1 &&
                 changed.bank.waves.size() == 2 && changed.bank.waves[1] == kFirst);
    XSFLIB_CHECK(changed.programs == std::vector<uint32_t>({2}) && changed.region == std::pair(60, 0));

    // Waves on free keys make a region of one key each, in the order of the waves. Each region plays its wave at its
    // key.
    SEQLIB_REQUIRE(keys.bank.instruments.size() > 2 && keys.bank.instruments[2]);
    const std::vector<hokuto::edit::Region> regions = hokuto::edit::RegionsOf(*keys.bank.instruments[2]);

    const auto wave_of = [&keys](const hokuto::edit::Region& region)
    {
        const auto index = static_cast<std::size_t>(region.values.wave_id_index);
        return index < keys.bank.waves.size() ? std::optional(keys.bank.waves[index]) : std::nullopt;
    };

    SEQLIB_REQUIRE(keys.name == "Add Regions" && regions.size() == 3);
    XSFLIB_CHECK(regions[0].first_key == 40 && regions[0].last_key == 40 && regions[0].values.original_key == 40);
    XSFLIB_CHECK(regions[1].first_key == 41 && regions[1].values.original_key == 41 && regions[1].first_velocity == 0 &&
                 regions[1].last_velocity == 127);
    XSFLIB_CHECK(wave_of(regions[0]) == kFirst && wave_of(regions[1]) == kSecond);
    XSFLIB_CHECK(keys.programs == std::vector<uint32_t>({2}) && keys.region == std::pair(40, 0));

    // The Undo History names the programs that a drop changes.
    XSFLIB_CHECK(hokuto::app::ProgramsText({9}) == "program 9");
    XSFLIB_CHECK(hokuto::app::ProgramsText({1, 3}) == "programs 1 and 3");
    XSFLIB_CHECK(hokuto::app::ProgramsText({14, 15}) == "programs 14 and 15");
    XSFLIB_CHECK(hokuto::app::ProgramsText({14, 15, 16}) == "programs 14 to 16");
    XSFLIB_CHECK(hokuto::app::ProgramsText({1, 3, 5}) == "programs 1, 3 and 5");

    // Keys that a region covers, keys past 127, and programs past the highest refuse the drop.
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::app::WithDroppedWaves(bank, {.program = 2, .key = 59}, {kFirst, kSecond}); }) ==
                 "Key " + hokuto::app::KeyName(60) +
                     " has a region already. Drop one wave on a region to give it the wave, or drop waves on "
                     "free keys.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::app::WithDroppedWaves(low, {.program = 4, .key = 127}, {kFirst, kSecond}); }) ==
                 "The waves run past key 127. Drop them on a lower key.");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::app::WithDroppedWaves(full, {}, {kFirst}); }) ==
                 "The bank has no free program for another instrument.");
}

// The least and the most sample of each run of a channel.
void TestPeaks()
{
    std::vector<int16_t> rising(600);
    for (std::size_t i = 0; i < rising.size(); ++i)
    {
        rising[i] = static_cast<int16_t>(static_cast<int>(i) - 300);
    }
    const std::vector<std::vector<int16_t>> channels = {rising, std::vector<int16_t>(3, 7)};

    const hokuto::app::Peaks peaks = hokuto::app::PeaksOf(channels);

    SEQLIB_REQUIRE(peaks.size() == 2 && peaks[0].size() == 3 && peaks[1].size() == 1);
    XSFLIB_CHECK(peaks[0][0] == Peak(-300, -45) && peaks[0][2] == Peak(212, 299));
    XSFLIB_CHECK(peaks[1][0] == Peak(7, 7));
}

// The steps of a loop's start: a frame of DSP-ADPCM in a wave, and a block in a stream.
void TestLoopSteps()
{
    XSFLIB_CHECK(hokuto::app::LoopStep(sar::WaveEncoding::kDspAdpcm, false) == 14);
    XSFLIB_CHECK(hokuto::app::LoopStep(sar::WaveEncoding::kPcm16, false) == 1);
    XSFLIB_CHECK(hokuto::app::LoopStep(sar::WaveEncoding::kPcm8, false) == 1);
    XSFLIB_CHECK(hokuto::app::LoopStep(sar::WaveEncoding::kDspAdpcm, true) == 14336);
    XSFLIB_CHECK(hokuto::app::LoopStep(sar::WaveEncoding::kPcm16, true) == 4096);
}

// The places of a marker: the nearest step, within the least and the most, or with the snap the quietest place within
// reach and the nearest of equals.
void TestMarkerPlaces()
{
    std::vector<int16_t> channel(100, 1000);
    channel[30] = 0;
    channel[36] = 0;
    channel[60] = -10;
    const std::vector<std::vector<int16_t>> mono = {channel};
    std::vector<int16_t> other(100, 1000);
    other[30] = 2000;
    const std::vector<std::vector<int16_t>> stereo = {channel, other};

    // Without the snap, the nearest step within the least and the most.
    XSFLIB_CHECK(LoopMarkerPlace(mono, 31.4, 1, 0, 99, false, 0.0) == 31u);
    XSFLIB_CHECK(LoopMarkerPlace(mono, 31.6, 1, 0, 99, false, 0.0) == 32u);
    XSFLIB_CHECK(LoopMarkerPlace(mono, 20.0, 14, 0, 99, false, 0.0) == 14u);
    XSFLIB_CHECK(LoopMarkerPlace(mono, 22.0, 14, 0, 99, false, 0.0) == 28u);
    XSFLIB_CHECK(LoopMarkerPlace(mono, 5.0, 1, 10, 99, false, 0.0) == 10u);
    XSFLIB_CHECK(LoopMarkerPlace(mono, 5.0, 14, 10, 99, false, 0.0) == 14u);
    XSFLIB_CHECK(LoopMarkerPlace(mono, 200.0, 14, 0, 99, false, 0.0) == 98u);

    // With it, the quietest place within reach, and the nearer of two zeros. Each channel counts.
    XSFLIB_CHECK(LoopMarkerPlace(mono, 33.0, 1, 0, 99, true, 5.0) == 30u);
    XSFLIB_CHECK(LoopMarkerPlace(mono, 34.0, 1, 0, 99, true, 5.0) == 36u);
    XSFLIB_CHECK(LoopMarkerPlace(mono, 50.0, 1, 0, 99, true, 5.0) == 50u);
    XSFLIB_CHECK(LoopMarkerPlace(mono, 57.0, 1, 0, 99, true, 5.0) == 60u);
    XSFLIB_CHECK(LoopMarkerPlace(stereo, 33.0, 1, 0, 99, true, 5.0) == 36u);
    XSFLIB_CHECK(LoopMarkerPlace(mono, 31.0, 1, 0, 32, true, 0.0) == 31u);

    // A place past the last sample counts as the last sample. Without a place, nothing.
    XSFLIB_CHECK(LoopMarkerPlace(mono, 104.0, 1, 0, 120, true, 3.0) == 104u);
    XSFLIB_CHECK(!LoopMarkerPlace(mono, 20.0, 0, 0, 99, false, 0.0));
    XSFLIB_CHECK(!LoopMarkerPlace(mono, 20.0, 14, 15, 27, false, 0.0));
    XSFLIB_CHECK(!LoopMarkerPlace(mono, 20.0, 1, 50, 40, true, 3.0));
}

// The loop's switch. A loop that goes off starts at 0. A loop that comes on starts where the loop that went off
// started, at a frame in DSP-ADPCM and before the end, or else at 0.
void TestLoopSwitch()
{
    WaveView view;
    view.values = {sar::WaveEncoding::kDspAdpcm, 32000, true, 70, 250};
    view.held = 280;
    view.samples = {std::vector<int16_t>(280, 1000)};
    const WaveValues off = hokuto::app::WithLoopSwitched(view, false, 70);
    const WaveValues kExpectedOff{sar::WaveEncoding::kDspAdpcm, 32000, false, 0, 250};
    XSFLIB_CHECK(off == kExpectedOff);

    view.values = off;
    const WaveValues kExpectedOn{sar::WaveEncoding::kDspAdpcm, 32000, true, 70, 250};
    XSFLIB_CHECK(hokuto::app::WithLoopSwitched(view, true, 70) == kExpectedOn);
    XSFLIB_CHECK(hokuto::app::WithLoopSwitched(view, true, std::nullopt).loop_start == 0);
    XSFLIB_CHECK(hokuto::app::WithLoopSwitched(view, true, 66).loop_start == 70);
    XSFLIB_CHECK(hokuto::app::WithLoopSwitched(view, true, 300).loop_start == 238);
    view.values.encoding = sar::WaveEncoding::kPcm16;
    XSFLIB_CHECK(hokuto::app::WithLoopSwitched(view, true, 66).loop_start == 66);
    XSFLIB_CHECK(hokuto::app::WithLoopSwitched(view, true, 300).loop_start == 249);
}

// The Bank Editor's view of the bank. The Wii U and the 3DS read a region's interpolation, and the Switch doesn't.
void TestBankView(sar::Flavor flavor)
{
    const File file = hokuto::format::ImportArchive(TestArchive(flavor), flavor);
    const std::shared_ptr<const sar::SoundArchive> archive = Load(file);

    const hokuto::app::BankView view = hokuto::app::MakeBankView(file, *archive, kBank);

    XSFLIB_CHECK(view.bank == kBank && view.name == "BANK_TEST" && view.model == TestBank(flavor));
    XSFLIB_CHECK(view.entries == std::vector<std::string>({"WARC_TEST, wave 0", "WARC_TEST, wave 1"}));
    XSFLIB_CHECK(view.waves == std::vector<sar::WaveId>({{kWavesId, kSine}, {kWavesId, kRamp}}) &&
                 view.wave_names.size() == 2);
    XSFLIB_CHECK(view.interpolation == (flavor != sar::Flavor::kNx) && view.ctr == (flavor == sar::Flavor::kCtr));
    XSFLIB_CHECK(view.sounds == std::vector<std::string>({"SEQ_TEST"}) && view.lacking.empty());
    const std::size_t size = archive->FileData(kBankFile).size();
    XSFLIB_CHECK(view.sizes == std::vector<std::string>({"The bank's file takes " + Grouped(size) + " bytes."}));
}

// The sizes of the groups that load the bank. A group that an edit added grows from nothing, and the game's group loads
// as many bytes as it did. No group loads the sequence file. The sizes are those of the archive that the file exports.
// The archive that plays can hold more bytes: its timelines lie there with every bar in their tracks. Here an archive
// whose bank holds another instrument stands for it.
void TestGroupSizes(sar::Flavor flavor)
{
    File file = hokuto::format::ImportArchive(TestArchive(flavor), flavor);
    file = hokuto::edit::AddGroups(file, std::vector<std::string>{"GROUP_TEST"});
    const std::vector<uint32_t> banks = {sar::ItemId(sar::kBankItem, kBank)};
    file = hokuto::edit::JoinGroups(file, std::vector<uint32_t>{0}, banks, {});
    hokuto::format::NativeArchive native = *file.native;
    native.game_sizes =
        hokuto::format::SizesOf(hokuto::format::ExportArchive(file), flavor, hokuto::format::GroupFilesBeside(file));
    File measured = file;
    measured.native = std::move(native);
    const std::shared_ptr<const sar::SoundArchive> archive = Load(file);

    const std::vector<std::string> added = hokuto::app::GroupSizeLines(file, *archive, kBankFile);
    const std::vector<std::string> same = hokuto::app::GroupSizeLines(measured, *archive, kBankFile);
    const std::vector<std::string> waves = hokuto::app::GroupSizeLines(measured, *archive, kWavesFile);
    const File grown = hokuto::edit::WithBankModel(
        measured, *archive, kBank,
        hokuto::edit::WithNewInstrument(hokuto::edit::BankModelOf(measured, *archive, kBank), 9, std::nullopt));
    const std::shared_ptr<const sar::SoundArchive> playing = Load(grown);
    const std::vector<std::string> exported = hokuto::app::GroupSizeLines(measured, *playing, kBankFile);

    const uint64_t size = measured.native->game_sizes->groups.at(0);
    const std::string loads = "Group GROUP_TEST loads " + Grouped(size) + " bytes";
    XSFLIB_CHECK(size == archive->FileData(kBankFile).size() + archive->FileData(kWavesFile).size());
    XSFLIB_CHECK(added == std::vector<std::string>({loads + ", " + Grouped(size) + " more than in the game."}));
    XSFLIB_CHECK(same == std::vector<std::string>({loads + ", as in the game."}) && waves == same);
    XSFLIB_CHECK(playing->FileData(kBankFile).size() > archive->FileData(kBankFile).size() && exported == same);
    XSFLIB_CHECK(hokuto::app::GroupSizeLines(file, *archive, 0).empty());
}

// The Sound section's view of the wave sound's values, with what else an edit changes and the file's size. Another
// sound that plays the same wave sound changes with it. A sequence sound has no view.
void TestWaveSoundView(sar::Flavor flavor)
{
    const File file = hokuto::format::ImportArchive(TestArchive(flavor), flavor);
    const std::shared_ptr<const sar::SoundArchive> archive = Load(file);
    const File twins = hokuto::format::ImportArchive(TestArchive(flavor, true), flavor);
    const std::shared_ptr<const sar::SoundArchive> twin_archive = Load(twins);

    const std::optional<hokuto::app::WaveSoundView> view = hokuto::app::MakeWaveSoundView(*archive, 1);
    const std::optional<hokuto::app::WaveSoundView> twin = hokuto::app::MakeWaveSoundView(*twin_archive, 1);

    SEQLIB_REQUIRE(view.has_value());
    XSFLIB_CHECK(view->fields.wave == sar::WaveId({kWavesId, kRamp}) && view->fields.values.pan == 20 &&
                 view->fields.places.pan);
    XSFLIB_CHECK(view->waves == std::vector<sar::WaveId>({{kWavesId, kSine}, {kWavesId, kRamp}}) &&
                 view->wave_names.size() == 2);
    const std::string size = Grouped(archive->FileData(kWaveSoundsFile).size());
    XSFLIB_CHECK(view->reach == "No other sound plays the wave sound." &&
                 view->sizes == std::vector<std::string>({"The wave sound file takes " + size + " bytes."}));
    XSFLIB_CHECK(twin && twin->reach == "An edit changes the wave sound of 1 other sound too: WSD_TWIN.");
    XSFLIB_CHECK(!hokuto::app::MakeWaveSoundView(*archive, 0) && !hokuto::app::MakeWaveSoundView(*archive, 5));
}

// The properties of a wave: its values, its samples and what else an edit changes. An end moved earlier can move back
// over the samples after it.
void TestWaveView(sar::Flavor flavor)
{
    const File file = hokuto::format::ImportArchive(TestArchive(flavor), flavor);
    const std::shared_ptr<const sar::SoundArchive> archive = Load(file);
    hokuto::format::WaveModel shorter = hokuto::edit::WaveModelOf(file, *archive, kWaves, kSine);
    shorter.loop_end = 200;
    const File edited = hokuto::edit::WithWaveModel(file, *archive, kWaves, kSine, shorter);

    const WaveView sine = hokuto::app::MakeWaveView(file, *archive, kWaves, kSine);
    const WaveView ramp = hokuto::app::MakeWaveView(file, *archive, kWaves, kRamp);
    const WaveView ended = hokuto::app::MakeWaveView(edited, *Load(edited), kWaves, kSine);

    // The sine's values and samples as the console decodes them.
    const sar::WaveFile present = hokuto::edit::PresentWave(*archive, kWaves, kSine);
    XSFLIB_CHECK(!sine.stream && sine.values == WaveValues({sar::WaveEncoding::kDspAdpcm, 32000, false, 0, 280}));
    XSFLIB_CHECK(sine.held == kSineLength && sine.samples == hokuto::edit::DecodedSamples(present, kSineLength));
    XSFLIB_CHECK(sine.peaks.size() == 1 && sine.peaks[0].size() == 2 && sine.encodings.size() == 3);
    XSFLIB_CHECK(ramp.values == WaveValues({sar::WaveEncoding::kPcm16, 48000, true, 10, kRampLength}));
    SEQLIB_REQUIRE(ramp.samples.size() == 1 && ramp.samples[0].size() > 99);
    XSFLIB_CHECK(ramp.samples[0][99] == 9900);

    // What else plays each wave, and the sizes.
    XSFLIB_CHECK(sine.reach == "An edit changes the wave in 1 bank (BANK_TEST).");
    XSFLIB_CHECK(ramp.reach == "An edit changes the wave in 1 bank (BANK_TEST) and 1 wave sound (WSD_TEST).");
    const std::string archive_size = Grouped(archive->FileData(kWavesFile).size());
    XSFLIB_CHECK(sine.sizes.size() == 2 && sine.sizes[0].starts_with("The wave takes ") &&
                 sine.sizes[1] == "The wave archive's file takes " + archive_size + " bytes.");

    // The edited wave plays up to its new end, and keeps the samples after it.
    XSFLIB_CHECK(ended.values.loop_end == 200 && ended.held == kSineLength);
}

// The properties of a stream: its values, its samples, the sound's tracks and the other stream sounds that play it. A
// sound whose stream the file lacks has none.
void TestStreamView(sar::Flavor flavor)
{
    const std::vector<uint8_t> stream =
        sar::MakeStream(flavor, 0x00040000, sar::WaveEncoding::kPcm16, 32000, StreamChannels(), 0).Write();
    const std::map<uint32_t, std::vector<uint8_t>> streams = {{kStreamFile, stream}};
    const File file = hokuto::format::ImportArchive(StreamArchive(flavor), flavor, {}, streams);
    const std::shared_ptr<const sar::SoundArchive> archive = Load(file);

    const WaveView stereo = hokuto::app::MakeStreamView(file, *archive, kStereoSound);
    const WaveView mono = hokuto::app::MakeStreamView(file, *archive, kMonoSound);

    XSFLIB_CHECK(stereo.stream && stereo.name == "STRM_SONG");
    XSFLIB_CHECK(stereo.values == WaveValues({sar::WaveEncoding::kPcm16, 32000, true, 0, kStreamLength}));
    XSFLIB_CHECK(stereo.held == kStreamLength && stereo.samples == StreamChannels());
    XSFLIB_CHECK(stereo.encodings ==
                 std::vector<sar::WaveEncoding>({sar::WaveEncoding::kDspAdpcm, sar::WaveEncoding::kPcm16}));
    XSFLIB_CHECK(stereo.tracks == std::vector<std::string>({"Track 1: channels 1 and 2, volume 100, pan the centre"}));
    XSFLIB_CHECK(mono.tracks == std::vector<std::string>({"Track 1: channel 1, volume 100, pan 64 left",
                                                          "Track 2: channel 2, volume 100, pan 63 right. It doesn't "
                                                          "play when the sound starts."}));
    XSFLIB_CHECK(stereo.reach == "An edit changes the stream of 1 other stream sound too: STRM_SONG_MONO.");
    XSFLIB_CHECK(stereo.sizes ==
                 std::vector<std::string>({"The stream file takes " + Grouped(stream.size()) + " bytes."}));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { hokuto::app::MakeStreamView(file, *archive, kMissingSound); }) ==
                 "The file holds no stream of STRM_GONE. Its stream, STRM_GONE.bfstm, wasn't beside the archive when "
                 "the archive was imported. Import the archive again with the stream beside it.");
}

// The views of the Wii's bank, waves and wave sounds. The Wii reads no interpolation. A build before March 2008 reads
// no hold, key group or note-off. A wave's reach names the bank of its wave data, or the wave sounds whose notes play
// it. Each group that holds the bank loads its files and their wave data.
void TestWiiViews()
{
    const std::vector<uint8_t> bytes = WiiArchive();
    const File file = hokuto::format::ImportWiiArchive(bytes);
    const File old_build = hokuto::format::ImportWiiArchive(bytes, ntdlib::SndBuild{.date = 20070608});
    const std::shared_ptr<const rsar::SoundArchive> archive = LoadWii(file);

    // The bank.
    const hokuto::app::BankView bank = hokuto::app::MakeBankView(file, *archive, kBank);
    const uint32_t kBankWaves = sar::ItemId(sar::kWaveArchiveItem, kWiiBankFile);
    const uint32_t kSoundWaves = sar::ItemId(sar::kWaveArchiveItem, kWiiWaveSoundsFile);
    XSFLIB_CHECK(bank.name == "BANK_TEST" && bank.model == hokuto::edit::BankModelOf(*archive, kBank));
    XSFLIB_CHECK(bank.entries == std::vector<std::string>({"BANK_TEST, wave 0", "BANK_TEST, wave 1"}));
    XSFLIB_CHECK(bank.waves ==
                     std::vector<sar::WaveId>({{kBankWaves, 0}, {kBankWaves, 1}, {kSoundWaves, 0}, {kSoundWaves, 1}}) &&
                 bank.wave_names.size() == 4);
    XSFLIB_CHECK(bank.wii && !bank.interpolation && bank.hold_and_groups);
    XSFLIB_CHECK(!hokuto::app::MakeBankView(old_build, *archive, kBank).hold_and_groups);
    XSFLIB_CHECK(bank.sounds == std::vector<std::string>({"SEQ_TEST"}));
    XSFLIB_CHECK(bank.sizes ==
                 std::vector<std::string>(
                     {"The bank's file takes " + Grouped(archive->FileData(kWiiBankFile).size()) + " bytes.",
                      "Its wave data takes " + Grouped(archive->WaveData(kWiiBankFile).size()) + " bytes."}));

    // The groups that hold the bank.
    std::vector<std::string> loads;
    for (std::size_t g = 0; g < archive->Groups().size(); ++g)
    {
        const rsar::SoundArchive::Group& group = archive->Groups()[g];
        for (const rsar::SoundArchive::Group::Item& item : group.items)
        {
            if (item.file_id == kWiiBankFile)
            {
                loads.push_back("Group " + std::to_string(g) + " loads " +
                                Grouped(uint64_t{group.size} + group.wave_data_size) + " bytes.");
                break;
            }
        }
    }
    XSFLIB_CHECK(loads.size() >= 2 && hokuto::app::WiiGroupSizeLines(file, kWiiBankFile) == loads);

    // The waves of the bank's wave data and of the wave sound file's.
    const WaveView sine = hokuto::app::MakeWaveView(file, *archive, kWiiBankFile, kSine);
    const WaveView ramp = hokuto::app::MakeWaveView(file, *archive, kWiiBankFile, kRamp);
    const WaveView sound_sine = hokuto::app::MakeWaveView(file, *archive, kWiiWaveSoundsFile, 1);
    XSFLIB_CHECK(sine.values == WaveValues({sar::WaveEncoding::kDspAdpcm, 32000, false, 0, kSineLength}));
    XSFLIB_CHECK(sine.held == kSineLength && sine.encodings.size() == 3);
    XSFLIB_CHECK(ramp.values == WaveValues({sar::WaveEncoding::kPcm16, 32000, true, 10, kRampLength}));
    XSFLIB_CHECK(ramp.samples == std::vector<std::vector<int16_t>>({RampSamples()}));
    XSFLIB_CHECK(sine.reach == "An edit changes the wave in 1 bank (BANK_TEST).");
    XSFLIB_CHECK(sound_sine.reach == "An edit changes the wave in 1 wave sound (WSD_SINE).");
    SEQLIB_REQUIRE(bank.sizes.size() > 1);
    XSFLIB_CHECK(ramp.sizes.size() == 3 && ramp.sizes[0] == "The wave's samples take 200 bytes in the wave data." &&
                 ramp.sizes[1] == bank.sizes[0] && ramp.sizes[2] == bank.sizes[1]);
    XSFLIB_CHECK(sound_sine.sizes.size() == 3 && sound_sine.sizes[1].starts_with("The wave sound file takes "));

    // The wave sounds. A sequence sound has no view.
    const std::optional<hokuto::app::WaveSoundView> wave_sound =
        hokuto::app::MakeWaveSoundView(file, *archive, kWiiRampSound);
    SEQLIB_REQUIRE(wave_sound.has_value());
    XSFLIB_CHECK(wave_sound->fields.wave == sar::WaveId({kSoundWaves, 0}) && wave_sound->fields.values.pan == 20 &&
                 !wave_sound->fields.places.filters);
    XSFLIB_CHECK(wave_sound->waves == std::vector<sar::WaveId>({{kSoundWaves, 0}, {kSoundWaves, 1}}) &&
                 wave_sound->hold && wave_sound->reach == "No other sound plays the wave sound.");
    XSFLIB_CHECK(wave_sound->sizes.size() == 2 && wave_sound->sizes[0].starts_with("The wave sound file takes "));
    const std::optional<hokuto::app::WaveSoundView> old_sine =
        hokuto::app::MakeWaveSoundView(old_build, *archive, kWiiSineSound);
    SEQLIB_REQUIRE(old_sine.has_value());
    XSFLIB_CHECK(!old_sine->hold);
    XSFLIB_CHECK(!hokuto::app::MakeWaveSoundView(file, *archive, 0));
}

// The properties of a Wii stream: its values, its samples, the tracks that each sound plays and the other stream sounds
// that play it. The view offers DSP-ADPCM and 16-bit PCM. A build before March 2008 plays the stream's first two
// channels at full volume and in the centre. A sound whose entry names a track that the stream lacks plays none.
void TestWiiStreamView()
{
    rsar::StreamFile stream = rsar::MakeStream(rsar::WaveEncoding::kPcm16, 32000, StreamChannels(), 0);
    stream.tracks = {{100, 64, {0, 1}}, {90, 0, {1}}};
    const std::vector<uint8_t> bytes = stream.Write();
    rsar::ArchiveModel model;
    model.version = 0x0104;
    model.players = {{.name = "PLAYER_TEST"}};
    model.files = {{.path = "stream/SONG.brstm", .path_size = static_cast<uint32_t>(bytes.size())}};
    model.sounds = {{.name = "STRM_SONG", .type = rsar::SoundType::kStream},
                    {.name = "STRM_BOTH", .type = rsar::SoundType::kStream, .stream_track_flags = 3},
                    {.name = "STRM_THIRD", .type = rsar::SoundType::kStream, .stream_track_flags = 4}};
    const std::vector<uint8_t> archive_bytes = rsar::BuildArchive(model);
    const File file = hokuto::format::ImportWiiArchive(archive_bytes, std::nullopt, {{kStreamFile, bytes}});
    const File old_build =
        hokuto::format::ImportWiiArchive(archive_bytes, ntdlib::SndBuild{.date = 20070608}, {{kStreamFile, bytes}});
    const std::shared_ptr<const rsar::SoundArchive> archive = LoadWii(file);

    const WaveView song = hokuto::app::MakeStreamView(file, *archive, 0);
    XSFLIB_CHECK(song.stream && song.name == "STRM_SONG");
    XSFLIB_CHECK(song.values == WaveValues({sar::WaveEncoding::kPcm16, 32000, true, 0, kStreamLength}));
    XSFLIB_CHECK(song.held == kStreamLength && song.samples == StreamChannels());
    XSFLIB_CHECK(song.encodings ==
                 std::vector<sar::WaveEncoding>({sar::WaveEncoding::kDspAdpcm, sar::WaveEncoding::kPcm16}));
    XSFLIB_CHECK(song.tracks == std::vector<std::string>({"Track 1: channels 1 and 2, volume 100, pan the centre",
                                                          "Track 2: channel 2, volume 90, pan 64 left. The sound "
                                                          "doesn't play it."}));
    XSFLIB_CHECK(hokuto::app::MakeStreamView(file, *archive, 1).tracks ==
                 std::vector<std::string>({"Track 1: channels 1 and 2, volume 100, pan the centre",
                                           "Track 2: channel 2, volume 90, pan 64 left"}));
    XSFLIB_CHECK(hokuto::app::MakeStreamView(file, *archive, 2).tracks.back() ==
                 "The sound plays none of the tracks: its entry names a track or a channel that the stream lacks.");
    XSFLIB_CHECK(song.reach == "An edit changes the stream of 2 other stream sounds too: STRM_BOTH and STRM_THIRD.");
    XSFLIB_CHECK(song.sizes ==
                 std::vector<std::string>({"The stream file takes " + Grouped(bytes.size()) + " bytes."}));
    XSFLIB_CHECK(hokuto::app::MakeStreamView(old_build, *archive, 0).tracks ==
                 std::vector<std::string>({"The game's build of NW4R SND plays channels 1 and 2 at full volume and in "
                                           "the centre, whatever the stream's tracks."}));
}

} // namespace

int main()
{
    TestFreeProgram();
    TestDroppedWaves();
    TestPeaks();
    TestLoopSteps();
    TestMarkerPlaces();
    TestLoopSwitch();
    TestBankView(sar::Flavor::kCtr);
    for (const sar::Flavor flavor : {sar::Flavor::kNx, sar::Flavor::kCafe})
    {
        TestBankView(flavor);
        TestGroupSizes(flavor);
        TestWaveSoundView(flavor);
        TestWaveView(flavor);
        TestStreamView(flavor);
    }
    TestWiiViews();
    TestWiiStreamView();

    return xsflib::testing::Summary("item_views");
}
