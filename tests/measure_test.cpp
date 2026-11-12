// SPDX-License-Identifier: MIT

// Tests the measure of an export to MIDI (src/measure/measure.h). A small archive made for the test holds two sequences
// on an instrument that plays a looped sine. One plays four notes of half a second, one after another. The other rests
// in the place of the third note. The export of the first sequence, rendered by Hokuto's SoundFont 2 renderer or by
// sfizz, has to measure without a finding against the model's render of the same sequence. The export of the second has
// to measure against the first sequence with a run of windows where the third note is missing. Its clips run from a
// second before the run to a second after, and the export's clip is silent where the note is missing. A measure that
// starts cancelled returns nothing, and a MIDI file that can't be read throws. The first sequence, sent to the Wii U as
// the same archive in the Wii U's byte order, has to measure without a finding against the Switch's model too. The same
// notes in a Wii archive, exported and measured at the Wii's 32 kHz, have to measure without a finding against the
// Wii's model. Sent to the Wii U, they have to measure without a finding against the Wii's model as well. Sent back to
// the Wii from the Wii U, they have to measure without a finding against the Wii U's model. Two renders of that song on
// the Wii's model have to give the same samples. A GBA song of four notes renders dry without the reverb of MP2K's
// mixer. Its MIDI export, and the song sent to each other console, have to measure without a finding against that
// render. A DS sequence of four notes is exported as MIDI with SoundFont 2 and SFZ. In each mixing mode, both exports
// have to measure without a finding against the DS's model.

#include "measure/measure.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numbers>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/krawall_cart.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "edit/other_console.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "played_fixtures.h"
#include "song/archive.h"
#include "song/audibility.h"
#include "song/dry_render.h"
#include "song/ds.h"
#include "song/gba.h"
#include "song/krawall.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace
{

namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace krawall_cart = ntdlib::testing::krawall_cart;
namespace measure = hokuto::measure;
namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace song = hokuto::song;

using hokuto::testing::DseMeasureImage;
using hokuto::testing::DsMeasureArchive;
using hokuto::testing::GbaMeasureCart;
using hokuto::testing::KrawallMeasureRom;

// The test archive's sounds.
constexpr uint32_t kNotesSound = 0;
constexpr uint32_t kFewerSound = 1;

// At timebase 48 and tempo 120, a sequence plays 96 ticks a second. Keys 60, 64, 67 and 72 play for 48 ticks each, half
// a second, one after another. A note waits for its length. Then the sequence ends. The Switch's sequences give
// arguments in little-endian order.
const std::vector<uint8_t> kNotesBytecode = {0xB0, 48, 0xE1, 120, 0x00, 60, 100, 48, 64,
                                             100,  48, 67,   100, 48,   72, 100, 48, 0xFF};

// The notes' sequence as the Wii U holds it: the tempo's argument is big-endian.
const std::vector<uint8_t> kWiiUNotesBytecode = {0xB0, 48, 0xE1, 0x00, 120, 60, 100, 48, 64,
                                                 100,  48, 67,   100,  48,  72, 100, 48, 0xFF};

// The same, with a rest of 48 ticks (0x80) in the place of key 67.
const std::vector<uint8_t> kFewerBytecode = {0xB0, 48, 0xE1, 120, 0x00, 60,  100, 48,  64,
                                             100,  48, 0x80, 48,  72,   100, 48,  0xFF};

// The instrument's wave: one period of a sine at half of full scale, 480 samples at 48 kHz, looped. It plays at 100 Hz
// at its original key. The samples are PCM16 in the Switch's byte order or, with `big_endian`, the Wii U's.
std::vector<uint8_t> SineWave(bool big_endian)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 480; i++)
    {
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) / 480.0;
        const auto sample = static_cast<uint16_t>(static_cast<int16_t>(std::lround(16384.0 * std::sin(phase))));
        const auto high = static_cast<uint8_t>(sample >> 8);
        const auto low = static_cast<uint8_t>(sample);
        bytes.push_back(big_endian ? high : low);
        bytes.push_back(big_endian ? low : high);
    }

    return bytes;
}

// The test archive: both sequences on a bank of one instrument, program 0. The instrument plays the sine for every key
// at original key 60. The tables and files are in the Switch's byte order or, with `big_endian`, the Wii U's. The Wii
// U's archive holds the notes' sequence as the Wii U holds it, and the Switch's other sequence unchanged.
std::vector<uint8_t> MeasureArchiveBytes(bool big_endian = false)
{
    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop = true;
    wave.sample_rate = 48000;
    wave.loop_end = 480;
    wave.channels = {SineWave(big_endian)};

    sar_files::SoundSpec notes;
    notes.name = "SEQ_NOTES";
    notes.volume = 127;
    notes.banks = {0};
    sar_files::SoundSpec fewer = notes;
    fewer.name = "SEQ_FEWER";
    fewer.file = 3;

    sar_files::ArchiveSpec spec;
    spec.sounds = {notes, fewer};
    spec.banks = {{"BANK_TEST", 1}};
    spec.wave_archives = {{"WARC_TEST", 2}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {
        {sar_files::Sequence(big_endian, big_endian ? kWiiUNotesBytecode : kNotesBytecode, {{"Notes", 0}}), "", false},
        {sar_files::Bank(big_endian, bank), "", false},
        {sar_files::WaveArchive(big_endian, {sar_files::Wave(big_endian, wave)}), "", false},
        {sar_files::Sequence(big_endian, kFewerBytecode, {{"Fewer", 0}}), "", false}};

    return sar_files::Archive(big_endian, spec);
}

// The test archive, opened from a file the way the program opens one. The file goes again once it's open.
std::shared_ptr<const sar::SoundArchive> OpenMeasureArchive(bool big_endian = false)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("hokuto_measure_test_" + std::to_string(std::random_device{}()) + ".bfsar");
    xsflib::WriteFile(path, MeasureArchiveBytes(big_endian));
    std::shared_ptr<const sar::SoundArchive> archive = song::OpenArchive(path);
    std::error_code error;
    std::filesystem::remove(path, error);

    return archive;
}

// The sequence of `sound` of `archive` as the program reads it.
std::shared_ptr<const song::Sequence> SequenceOf(const sar::SoundArchive& archive, uint32_t sound)
{
    return std::make_shared<const song::Sequence>(song::ReadSequence(archive, sound));
}

// The bytes of a Wii archive whose sequence plays the four notes of kWiiUNotesBytecode on the first instrument of
// ntdlib's test bank. ntdlib's test archive holds silent waves of DSP-ADPCM. Here each frame of the waves gives the
// samples of its nibbles at a scale of 2^11, and the waves loop. With `sine`, each wave holds PCM16 in place of
// DSP-ADPCM: a sine whose period is the loop's 29 samples, from sample 2 to sample 30. The test bank's regions decay
// and release at the slowest rates, over many seconds. Here each region holds its level, and its release ends at once:
// decay, sustain and release at 127.
std::vector<uint8_t> WiiMeasureBytes(bool sine = false)
{
    namespace rsar_files = ntdlib::testing::rsar_files;
    std::vector<uint8_t> bytes = rsar_files::MakeArchive(rsar_files::MakeSequence(kWiiUNotesBytecode), 0, 1);
    constexpr std::array<uint8_t, 7> kNibbles = {0x7F, 0x19, 0xE3, 0x5A, 0xC6, 0x2D, 0x91};
    constexpr std::array<uint8_t, 8> kEnvelope = {127, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<uint8_t, 4> kTune = {0x3F, 0x80, 0x00, 0x00};
    for (std::size_t at = 0; at + 0x14 <= bytes.size(); at++)
    {
        // A region: its wave's index, then the envelope from byte 4, and at byte 0x10 its tuning of 1.0.
        if (std::memcmp(&bytes[at + 4], kEnvelope.data(), kEnvelope.size()) == 0 && bytes[at + 0xD] == 100 &&
            std::memcmp(&bytes[at + 0x10], kTune.data(), kTune.size()) == 0)
        {
            bytes[at + 5] = 127;
            bytes[at + 6] = 127;
            bytes[at + 7] = 127;
        }
    }
    for (std::size_t at = 0; at + 4 <= bytes.size(); at++)
    {
        if (std::memcmp(&bytes[at], "RWAV", 4) != 0)
        {
            continue;
        }

        // The wave's DATA block holds 0x40 bytes of samples after its header of 8 bytes. Its INFO block starts with the
        // wave's format after a header of 8 bytes: 1 for PCM16.
        const uint32_t data = xsflib::LoadBe32(&bytes[at + 0x18]);
        if (sine)
        {
            bytes[at + xsflib::LoadBe32(&bytes[at + 0x10]) + 8] = 1;
            for (std::size_t sample = 0; sample < 0x20; sample++)
            {
                const double phase = 2.0 * std::numbers::pi * (static_cast<double>(sample) - 2.0) / 29.0;
                xsflib::StoreBe16(&bytes[at + data + 8 + sample * 2],
                                  static_cast<uint16_t>(std::lround(12000.0 * std::sin(phase))));
            }
            continue;
        }
        for (std::size_t frame = 0; frame < 0x40; frame += 8)
        {
            uint8_t* samples = &bytes[at + data + 8 + frame];
            samples[0] = 0x0B;
            std::copy(kNibbles.begin(), kNibbles.end(), samples + 1);
        }
    }

    return bytes;
}

// The Wii archive of WiiMeasureBytes.
std::shared_ptr<const ntdlib::rsar::SoundArchive> OpenWiiMeasureArchive(bool sine = false)
{
    return song::LoadWiiArchive(WiiMeasureBytes(sine), "SOUND.brsar", ntdlib::rvl::snd::kRsarOptions);
}

// The root mean square of the frames of `clip`, interleaved stereo at 48 kHz, from `from` to `to` seconds.
double Rms(std::span<const float> clip, double from, double to)
{
    const auto first = static_cast<std::size_t>(from * 48000.0) * 2;
    const auto last = static_cast<std::size_t>(to * 48000.0) * 2;
    double sum = 0.0;
    for (std::size_t i = first; i < last && i < clip.size(); i++)
    {
        sum += static_cast<double>(clip[i]) * clip[i];
    }

    return std::sqrt(sum / static_cast<double>(last - first));
}

// The export of SEQ_NOTES measures without a finding against the model's render of SEQ_NOTES. The renders line up
// within a few milliseconds. The measure covers the four notes, 2 s, and stops where the model's output ends.
void TestFaithfulExport(measure::Instruments instruments)
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenMeasureArchive();
    const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*archive, kNotesSound, {}, {}, "Hokuto");
    const std::atomic<bool> kGo{false};
    const measure::Measure found = measure::MeasureExport(
        conversion, instruments, song::DryOf(archive, SequenceOf(*archive, kNotesSound), song::Mixing::kStrict), kGo);

    SEQLIB_CHECK_EQ(found.rate, 48000);
    SEQLIB_CHECK(found.comparison.windows >= 38);
    SEQLIB_CHECK(found.comparison.places.empty());
    SEQLIB_CHECK(found.places.empty());
    SEQLIB_CHECK(std::abs(found.comparison.lag) <= 0.005);
    SEQLIB_CHECK(found.seconds >= 2.0 && found.seconds < 4.0);
    SEQLIB_CHECK(!found.cut);
}

// The export of SEQ_FEWER lacks the third note of SEQ_NOTES. Against SEQ_NOTES, the measure finds a run of windows from
// 1 s to 1.5 s. The export plays far quieter there. That run lies furthest off. Its clips run from the start, a second
// before the run, to a second after it: 2.5 s. Where the note is missing, the export's clip is silent and Hokuto's
// isn't. Everywhere else, both play.
void TestMissingNote(measure::Instruments instruments)
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenMeasureArchive();
    const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*archive, kFewerSound, {}, {}, "Hokuto");
    const std::atomic<bool> kGo{false};
    const measure::Measure found = measure::MeasureExport(
        conversion, instruments, song::DryOf(archive, SequenceOf(*archive, kNotesSound), song::Mixing::kStrict), kGo);

    SEQLIB_REQUIRE(!found.places.empty());
    SEQLIB_CHECK(found.places.size() <= 3);
    const measure::PlaceClips& worst = found.places[0];
    SEQLIB_CHECK(worst.place.run);
    SEQLIB_CHECK(worst.place.db < -20.0);
    SEQLIB_CHECK(std::abs(worst.place.start - 1.0) < 0.06);
    SEQLIB_CHECK(std::abs(worst.place.seconds - 0.5) < 0.11);
    SEQLIB_CHECK_EQ(worst.export_clip.size(), worst.hokuto_clip.size());
    SEQLIB_CHECK(std::abs(static_cast<double>(worst.hokuto_clip.size()) -
                          2.0 * 48000.0 * (worst.place.start + worst.place.seconds + 1.0)) <= 2.0);
    const double missing = Rms(worst.export_clip, 1.05, 1.45);
    const double played = Rms(worst.hokuto_clip, 1.05, 1.45);
    SEQLIB_CHECK(played > 0.01);
    SEQLIB_CHECK(missing < played / 100.0);
    SEQLIB_CHECK(Rms(worst.export_clip, 0.05, 0.95) > 0.01);
    SEQLIB_CHECK(Rms(worst.hokuto_clip, 0.05, 0.95) > 0.01);
}

// SEQ_NOTES on the Wii U's model measures without a finding against the Switch's model. The two consoles mix at other
// rates and with other resamplers, but play the same notes at the same levels. A run whose flag is set from the start
// returns nothing.
void TestOtherConsole()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenMeasureArchive();
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenMeasureArchive(true);
    const std::atomic<bool> kGo{false};
    const std::atomic<bool> kStop{true};
    const measure::Renders renders =
        measure::RenderOtherConsole(song::DryOf(archive, SequenceOf(*archive, kNotesSound), song::Mixing::kStrict),
                                    song::DryOf(wii_u, SequenceOf(*wii_u, kNotesSound), song::Mixing::kStrict), kGo);
    const measure::Measure found = measure::MeasureRenders(renders);

    SEQLIB_CHECK(!renders.cut);
    SEQLIB_CHECK(found.comparison.windows >= 38);
    SEQLIB_CHECK(found.comparison.places.empty());
    SEQLIB_CHECK(std::abs(found.comparison.gain_db) < 1.0);
    SEQLIB_CHECK(std::abs(found.comparison.lag) <= 0.005);
    SEQLIB_CHECK(
        measure::RenderOtherConsole(song::DryOf(archive, SequenceOf(*archive, kNotesSound), song::Mixing::kStrict),
                                    song::DryOf(wii_u, SequenceOf(*wii_u, kNotesSound), song::Mixing::kStrict), kStop)
            .hokuto.empty());
}

// The Wii's export of the four notes measures without a finding against the Wii's model, at 32 kHz. A measure whose
// flag is set from the start returns nothing.
void TestWiiExport(measure::Instruments instruments)
{
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiMeasureArchive();
    const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, 0));
    const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*archive, 0, {}, {}, "Hokuto");
    const std::atomic<bool> kGo{false};
    const std::atomic<bool> kStop{true};
    const measure::Measure found = measure::MeasureExport(conversion, instruments, song::DryOf(archive, sequence), kGo);
    const measure::Measure stopped =
        measure::MeasureExport(conversion, instruments, song::DryOf(archive, sequence), kStop);

    SEQLIB_CHECK_EQ(found.rate, 32000);
    SEQLIB_CHECK(found.comparison.windows >= 38);
    SEQLIB_CHECK(found.comparison.places.empty());
    SEQLIB_CHECK(found.places.empty());
    SEQLIB_CHECK(std::abs(found.comparison.lag) <= 0.005);
    SEQLIB_CHECK(found.seconds >= 2.0 && found.seconds < 4.0);
    SEQLIB_CHECK(!found.cut);
    SEQLIB_CHECK(stopped.rate == 0 && stopped.places.empty());
}

// The four notes of the Wii archive with sine waves, sent to the Wii U, measure without a finding against the Wii's
// model. The Wii's model renders at 32 kHz and the Wii U's at 48 kHz. The renders compare at 48 kHz. The Wii U's model
// starts the notes 6 ms later than the Wii's: two of AX's frames. A run whose flag is set from the start returns
// nothing. The test bank's waves of DSP-ADPCM wouldn't do here. Their samples start 4 bytes past a multiple of 8 in the
// wave archive, and the Wii's accelerator reads a frame's header in the middle of each frame. The Wii U's wave file
// that the conversion writes holds the frames in place. The sine waves of PCM16 have no headers.
void TestWiiOtherConsole()
{
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii = OpenWiiMeasureArchive(true);
    const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*wii, 0));
    const edit::OtherConsoleSong sent =
        edit::ForOtherConsole(format::ImportWiiArchive(WiiMeasureBytes(true)), 0, sar::Flavor::kCafe);
    const std::shared_ptr<const sar::SoundArchive> wii_u =
        song::LoadArchive(format::ExportArchive(sent.file), "SOUND.bfsar", format::ExportGroupFiles(sent.file));
    const std::atomic<bool> kGo{false};
    const std::atomic<bool> kStop{true};
    const measure::Renders renders = measure::RenderOtherConsole(
        song::DryOf(wii, sequence), song::DryOf(wii_u, SequenceOf(*wii_u, 0), song::Mixing::kStrict), kGo);
    const measure::Measure found = measure::MeasureRenders(renders);

    SEQLIB_CHECK_EQ(found.rate, 48000);
    SEQLIB_CHECK(!renders.cut);
    SEQLIB_CHECK(found.comparison.windows >= 38);
    SEQLIB_CHECK(found.comparison.places.empty());
    SEQLIB_CHECK(std::abs(found.comparison.gain_db) < 0.2);
    SEQLIB_CHECK(std::abs(found.comparison.lag - 0.006) <= 0.001);
    SEQLIB_CHECK(measure::RenderOtherConsole(song::DryOf(wii, sequence),
                                             song::DryOf(wii_u, SequenceOf(*wii_u, 0), song::Mixing::kStrict), kStop)
                     .hokuto.empty());
}

// The four notes of the Wii archive with sine waves, sent to the Wii U and back to the Wii, measure without a finding
// against the Wii U's model. The Wii's model starts the notes 6 ms earlier than the Wii U's. On the Wii's engine, the
// Wii U's sound plays the song that goes to the Wii (edit::ForEngine). Two renders of that song on the Wii's model give
// the same samples, and measure without a finding. A run whose flag is set from the start returns nothing.
void TestToWii()
{
    const edit::OtherConsoleSong sent =
        edit::ForOtherConsole(format::ImportWiiArchive(WiiMeasureBytes(true)), 0, sar::Flavor::kCafe);
    const std::shared_ptr<const sar::SoundArchive> wii_u =
        song::LoadArchive(format::ExportArchive(sent.file), "SOUND.bfsar", format::ExportGroupFiles(sent.file));
    const edit::OtherConsoleSong back = edit::ForWii(sent.file, 0);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii =
        song::LoadWiiArchive(format::ExportArchive(back.file), "SOUND.brsar", ntdlib::rvl::snd::kRsarOptions);
    const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*wii, 0));
    const std::atomic<bool> kGo{false};
    const std::atomic<bool> kStop{true};
    const measure::Renders renders = measure::RenderOtherConsole(
        song::DryOf(wii_u, SequenceOf(*wii_u, 0), song::Mixing::kStrict), song::DryOf(wii, sequence), kGo);
    const measure::Measure found = measure::MeasureRenders(renders);
    const measure::Renders same =
        measure::RenderOtherConsole(song::DryOf(wii, sequence), song::DryOf(wii, sequence), kGo);
    const measure::Measure none = measure::MeasureRenders(same);

    SEQLIB_CHECK_EQ(found.rate, 48000);
    SEQLIB_CHECK(!renders.cut);
    SEQLIB_CHECK(found.comparison.windows >= 38);
    SEQLIB_CHECK(found.comparison.places.empty());
    SEQLIB_CHECK(std::abs(found.comparison.gain_db) < 0.2);
    SEQLIB_CHECK(std::abs(found.comparison.lag + 0.006) <= 0.001);
    SEQLIB_CHECK_EQ(none.rate, 32000);
    SEQLIB_CHECK(same.hokuto == same.exported && none.comparison.places.empty());
    SEQLIB_CHECK(measure::RenderOtherConsole(song::DryOf(wii_u, SequenceOf(*wii_u, 0), song::Mixing::kStrict),
                                             song::DryOf(wii, sequence), kStop)
                     .hokuto.empty());
    SEQLIB_CHECK(
        measure::RenderOtherConsole(song::DryOf(wii, sequence), song::DryOf(wii, sequence), kStop).hokuto.empty());
}

// The render of `sequence` of `rom` on the GBA's model with the reverb, for 2.5 s.
std::vector<float> GbaRender(std::shared_ptr<const song::GbaRom> rom, std::shared_ptr<const song::Sequence> sequence)
{
    song::SequenceModel model(std::move(rom), std::move(sequence));
    std::vector<float> samples;
    for (bool plays = model.SeekFrame(0); plays && samples.size() < 5.0 * model.SampleRate(); plays = model.RunFrame())
    {
        const std::span<const float> output = model.FloatOutput();
        samples.insert(samples.end(), output.begin(), output.end());
    }

    return samples;
}

// The GBA's dry render leaves out the reverb of MP2K's mixer: a song whose header sets the reverb renders as the song
// without it. The model plays the reverb otherwise. The song's MIDI export (song::ConvertToMidi) measures without a
// finding against the dry render. So does the song sent to the Wii U, the Switch, the 3DS and the Wii, against the same
// render.
void TestGba()
{
    const std::shared_ptr<const song::GbaRom> wet = song::LoadGbaRom(GbaMeasureCart(0x80 | 100));
    const std::shared_ptr<const song::GbaRom> dry = song::LoadGbaRom(GbaMeasureCart(0x80));
    const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*wet, 0));
    const std::atomic<bool> kGo{false};
    const song::DryRender wet_render = song::RenderDry(wet, sequence, song::Mixing::kStrict, 2.5, kGo);
    const song::DryRender dry_render = song::RenderDry(dry, sequence, song::Mixing::kStrict, 2.5, kGo);
    SEQLIB_CHECK(!wet_render.samples.empty() && wet_render.samples == dry_render.samples);
    SEQLIB_CHECK_EQ(wet_render.rate, song::GbaRate(*wet));
    SEQLIB_CHECK(GbaRender(wet, sequence) != GbaRender(dry, sequence));

    const measure::Measure exported =
        measure::MeasureExport(song::ConvertToMidi(*wet, 0, "Hokuto"), measure::Instruments::kSoundFont,
                               song::DryOf(wet, sequence, song::Mixing::kStrict), kGo);
    SEQLIB_CHECK_EQ(exported.rate, song::GbaRate(*wet));
    SEQLIB_CHECK(exported.comparison.windows >= 30);
    SEQLIB_CHECK(exported.comparison.places.empty());

    const format::File file = format::ImportGbaRom(GbaMeasureCart(0x80 | 100));
    for (const sar::Flavor to : {sar::Flavor::kCafe, sar::Flavor::kNx, sar::Flavor::kCtr})
    {
        const edit::OtherConsoleSong sent = edit::ForOtherConsole(file, 0, to);
        const std::shared_ptr<const sar::SoundArchive> target =
            song::LoadArchive(format::ExportArchive(edit::FoldTimelines(sent.file)), "SOUND");
        const measure::Measure found = measure::MeasureRenders(
            measure::RenderOtherConsole(song::DryOf(wet, sequence, song::Mixing::kStrict),
                                        song::DryOf(target, SequenceOf(*target, 0), song::Mixing::kStrict), kGo));
        SEQLIB_CHECK(found.comparison.windows >= 30);
        SEQLIB_CHECK(found.comparison.places.empty());
    }
    const edit::OtherConsoleSong wii = edit::ForWii(file, 0);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii_archive = song::LoadWiiArchive(
        format::ExportArchive(edit::FoldTimelines(wii.file)), "SOUND.brsar", ntdlib::rvl::snd::kRsarOptions);
    const measure::Measure found = measure::MeasureRenders(measure::RenderOtherConsole(
        song::DryOf(wet, sequence, song::Mixing::kStrict),
        song::DryOf(wii_archive, std::make_shared<const song::Sequence>(song::ReadSequence(*wii_archive, 0))), kGo));
    SEQLIB_CHECK(found.comparison.windows >= 30);
    SEQLIB_CHECK(found.comparison.places.empty());

    const std::shared_ptr<const song::DsArchive> ds =
        song::LoadDsArchive(format::ExportArchive(edit::ForDs(file, 0).file));
    const measure::Renders on_ds = measure::RenderOtherConsole(
        song::DryOf(wet, sequence, song::Mixing::kStrict),
        song::DryOf(ds, std::make_shared<const song::Sequence>(song::ReadSequence(*ds, 0)), song::Mixing::kStrict),
        kGo);
    SEQLIB_CHECK(!on_ds.exported.empty() && !on_ds.hokuto.empty());
    SEQLIB_CHECK(measure::MeasureRenders(on_ds).comparison.windows >= 30);
}

// The DS's export measures without a finding against the DS's model, at the model's rate: 32,728 Hz in strict mixing,
// and 47,989 Hz in enhanced mixing.
void TestDs()
{
    const std::shared_ptr<const song::DsArchive> archive = DsMeasureArchive();
    const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, 0));
    const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*archive, 0, {}, "Hokuto");
    const std::atomic<bool> kGo{false};
    for (const song::Mixing mixing : {song::Mixing::kStrict, song::Mixing::kEnhanced})
    {
        for (const measure::Instruments instruments : {measure::Instruments::kSoundFont, measure::Instruments::kSfz})
        {
            const measure::Measure found =
                measure::MeasureExport(conversion, instruments, song::DryOf(archive, sequence, mixing), kGo);
            SEQLIB_CHECK_EQ(found.rate, mixing == song::Mixing::kStrict ? 32728 : 47989);
            SEQLIB_CHECK(found.comparison.windows >= 30);
            SEQLIB_CHECK(found.comparison.places.empty());
            SEQLIB_CHECK(found.places.empty());
            SEQLIB_CHECK(found.seconds >= 2.0 && found.seconds < 4.0);
        }
    }
}

// DSE's export measures without a finding against the DS's model, at the model's rate: 32,728 Hz in strict mixing, and
// 47,989 Hz in enhanced mixing.
void TestDse()
{
    const std::vector<uint8_t> image = DseMeasureImage();
    const std::shared_ptr<const song::DseSet> set = song::LoadDseSet(image);
    const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*set, 0));
    const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*set, 0, "Hokuto");
    const std::atomic<bool> kGo{false};
    for (const song::Mixing mixing : {song::Mixing::kStrict, song::Mixing::kEnhanced})
    {
        for (const measure::Instruments instruments : {measure::Instruments::kSoundFont, measure::Instruments::kSfz})
        {
            const measure::Measure found =
                measure::MeasureExport(conversion, instruments, song::DryOf(set, sequence, mixing), kGo);
            SEQLIB_CHECK_EQ(found.rate, mixing == song::Mixing::kStrict ? 32728 : 47989);
            SEQLIB_CHECK(found.comparison.windows >= 30);
            SEQLIB_CHECK(found.comparison.places.empty());
            SEQLIB_CHECK(found.places.empty());
            SEQLIB_CHECK(found.seconds >= 1.0 && found.seconds < 4.0);
        }
    }

    // The sound sent to the DS measures without a finding against DSE's model. Both play on the DS's model.
    const std::shared_ptr<const song::DsArchive> on_ds =
        song::LoadDsArchive(format::ExportArchive(edit::ForDs(format::ImportDseImage(image), 0).file));
    const measure::Measure to_ds = measure::MeasureRenders(measure::RenderOtherConsole(
        song::DryOf(set, sequence, song::Mixing::kStrict),
        song::DryOf(on_ds, std::make_shared<const song::Sequence>(song::ReadSequence(*on_ds, 0)),
                    song::Mixing::kStrict),
        kGo));
    SEQLIB_CHECK_EQ(to_ds.rate, 32728);
    SEQLIB_CHECK(to_ds.comparison.windows >= 30);
    SEQLIB_CHECK(to_ds.comparison.places.empty());
}

// A sound of the Switch and a sound of the Wii sent to the DS measure without a finding against the models of their
// consoles. A DS render uses the model's rate of 32,728 Hz. The renders compare at the higher of the two rates. A
// measure whose flag is set from the start returns nothing.
void TestToDs()
{
    const std::atomic<bool> kGo{false};
    const std::atomic<bool> kStop{true};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenMeasureArchive();
    const format::File file = format::ImportArchive(MeasureArchiveBytes(false), sar::Flavor::kNx);
    const std::shared_ptr<const song::DsArchive> from_switch =
        song::LoadDsArchive(format::ExportArchive(edit::ForDs(file, kNotesSound).file));
    const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*from_switch, 0));

    const measure::Renders renders =
        measure::RenderOtherConsole(song::DryOf(archive, SequenceOf(*archive, kNotesSound), song::Mixing::kStrict),
                                    song::DryOf(from_switch, sequence, song::Mixing::kStrict), kGo);
    const measure::Measure found = measure::MeasureRenders(renders);

    SEQLIB_CHECK_EQ(found.rate, 48000);
    SEQLIB_CHECK(!renders.cut);
    SEQLIB_CHECK(found.comparison.windows >= 38);
    SEQLIB_CHECK(found.comparison.places.empty());

    // A sound that plays its conversion on the DS's engine measures against the same conversion without a finding.
    const measure::Measure same = measure::MeasureRenders(
        measure::RenderOtherConsole(song::DryOf(from_switch, sequence, song::Mixing::kStrict),
                                    song::DryOf(from_switch, sequence, song::Mixing::kStrict), kGo));
    SEQLIB_CHECK_EQ(same.rate, 32728);
    SEQLIB_CHECK(same.comparison.windows >= 30);
    SEQLIB_CHECK(same.comparison.places.empty());

    SEQLIB_CHECK(
        measure::RenderOtherConsole(song::DryOf(archive, SequenceOf(*archive, kNotesSound), song::Mixing::kStrict),
                                    song::DryOf(from_switch, sequence, song::Mixing::kStrict), kStop)
            .hokuto.empty());

    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii = OpenWiiMeasureArchive(true);
    const std::shared_ptr<const song::DsArchive> from_wii = song::LoadDsArchive(
        format::ExportArchive(edit::ForDs(format::ImportWiiArchive(WiiMeasureBytes(true)), 0).file));
    const auto wii_sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*wii, 0));
    const measure::Measure wii_found = measure::MeasureRenders(measure::RenderOtherConsole(
        song::DryOf(wii, wii_sequence),
        song::DryOf(from_wii, std::make_shared<const song::Sequence>(song::ReadSequence(*from_wii, 0)),
                    song::Mixing::kStrict),
        kGo));
    SEQLIB_CHECK_EQ(wii_found.rate, 32728);
    SEQLIB_CHECK(wii_found.comparison.windows >= 30);
    SEQLIB_CHECK(wii_found.comparison.places.empty());
}

// A measure whose flag is set from the start returns nothing.
void TestCancel()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenMeasureArchive();
    const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*archive, kNotesSound, {}, {}, "Hokuto");
    const std::atomic<bool> kStop{true};

    for (const measure::Instruments instruments : {measure::Instruments::kSoundFont, measure::Instruments::kSfz})
    {
        const measure::Measure found = measure::MeasureExport(
            conversion, instruments, song::DryOf(archive, SequenceOf(*archive, kNotesSound), song::Mixing::kStrict),
            kStop);
        SEQLIB_CHECK_EQ(found.rate, 0);
        SEQLIB_CHECK_EQ(found.comparison.windows, std::size_t{0});
        SEQLIB_CHECK(found.places.empty());
    }
}

// The DrySource of `sequence` of `played` in `mixing` keeps the sequence. It renders the samples, at the rate, that
// RenderDry renders from the same arguments. A render whose flag is set from the start returns nothing.
void CheckDryOf(const song::Played& played, const std::shared_ptr<const song::Sequence>& sequence,
                song::Mixing mixing = song::Mixing::kStrict)
{
    const std::atomic<bool> kGo{false};
    const std::atomic<bool> kStop{true};
    const song::DrySource source = song::DryOf(played, sequence, mixing);
    const song::DryRender render = source.render(1.5, kGo);
    const song::DryRender expected = song::RenderDry(played, sequence, mixing, 1.5, kGo);
    SEQLIB_CHECK(source.sequence == sequence);
    SEQLIB_CHECK(!render.samples.empty());
    SEQLIB_CHECK_EQ(render.rate, expected.rate);
    SEQLIB_CHECK(render.samples == expected.samples);
    SEQLIB_CHECK(source.render(1.5, kStop).samples.empty());
}

// The DrySource of each kind of archive, ROM or set renders as RenderDry does: a sequence of the Switch, the Wii, the
// GBA, the DS, DSE and one of the GBA's other drivers, in each mixing mode that the console plays.
void TestDryOf()
{
    for (const song::Mixing mixing : {song::Mixing::kStrict, song::Mixing::kEnhanced})
    {
        const std::shared_ptr<const sar::SoundArchive> archive = OpenMeasureArchive();
        CheckDryOf(archive, SequenceOf(*archive, kNotesSound), mixing);

        const std::shared_ptr<const song::GbaRom> gba = song::LoadGbaRom(GbaMeasureCart(0x80 | 100));
        CheckDryOf(gba, std::make_shared<const song::Sequence>(song::ReadSequence(*gba, 0)), mixing);

        const std::shared_ptr<const song::DsArchive> ds = DsMeasureArchive();
        CheckDryOf(ds, std::make_shared<const song::Sequence>(song::ReadSequence(*ds, 0)), mixing);

        const std::shared_ptr<const song::DseSet> dse = song::LoadDseSet(DseMeasureImage());
        CheckDryOf(dse, std::make_shared<const song::Sequence>(song::ReadSequence(*dse, 0)), mixing);

        const std::shared_ptr<const song::DriverRom> driver = KrawallMeasureRom();
        CheckDryOf(driver, std::make_shared<const song::Sequence>(driver->ReadSequence(0)), mixing);
    }

    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii = OpenWiiMeasureArchive(true);
    CheckDryOf(wii, std::make_shared<const song::Sequence>(song::ReadSequence(*wii, 0)));
}

// A MIDI file that can't be read throws std::runtime_error, the error that the Export window shows.
void TestUnreadableMidi()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenMeasureArchive();
    ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*archive, kNotesSound, {}, {}, "Hokuto");
    conversion.midi = {0x4D, 0x54, 0x68, 0x64, 0x00};
    const std::atomic<bool> kGo{false};

    for (const measure::Instruments instruments : {measure::Instruments::kSoundFont, measure::Instruments::kSfz})
    {
        SEQLIB_CHECK(!xsflib::testing::ErrorOf<std::runtime_error>(
                          [&]
                          {
                              measure::MeasureExport(
                                  conversion, instruments,
                                  song::DryOf(archive, SequenceOf(*archive, kNotesSound), song::Mixing::kStrict), kGo);
                          })
                          .empty());
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("measure",
                                    []
                                    {
                                        TestFaithfulExport(measure::Instruments::kSoundFont);
                                        TestFaithfulExport(measure::Instruments::kSfz);
                                        TestMissingNote(measure::Instruments::kSoundFont);
                                        TestMissingNote(measure::Instruments::kSfz);
                                        TestOtherConsole();
                                        TestWiiExport(measure::Instruments::kSoundFont);
                                        TestWiiExport(measure::Instruments::kSfz);
                                        TestWiiOtherConsole();
                                        TestToWii();
                                        TestGba();
                                        TestDs();
                                        TestDse();
                                        TestToDs();
                                        TestCancel();
                                        TestDryOf();
                                        TestUnreadableMidi();
                                    });
}
