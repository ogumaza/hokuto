// SPDX-License-Identifier: MIT

// Tests the lengths for players (src/song/lengths.h and src/edit/lengths.h). The sequences play at timebase 48 and
// tempo 120: 96 ticks a second. The loop's sequence plays its intro for 1 s, then its loop twice, 1 s a pass, before
// the fade. Its length is 3 s, within two frames of the console's model, on the Switch, the Wii U, the 3DS and the Wii.
// The arrangement's loop runs from tick 96 to 192. The note's sequence plays key 60 for 0.25 s and ends. Its release
// and 0.5 s of silence follow, without a fade. A sound past the table gets no lengths. A file takes the lengths of its
// sequence sounds in place of the lengths that it held, a file of the Wii included. A measure that its progress stops
// gives nothing.

#include "edit/lengths.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/lengths.h"

namespace
{

namespace sar_files = ntdlib::testing::sar_files;

// The test archive's sounds.
constexpr uint32_t kLoopSound = 0;
constexpr uint32_t kNoteSound = 1;

// The intro plays key 60 for 48 ticks and rests for 48. The loop, from tick 96 to 192, plays key 64 for 48 ticks, rests
// for 48 and jumps back to byte 10. The Switch's sequences give arguments in little-endian order, and those of the Wii
// U and the 3DS in big-endian order.
const std::vector<uint8_t> kLoopBytecode = {0xB0, 48,  0xE1, 120,  0x00, 60,   100, 48,   0x80, 48,
                                            64,   100, 48,   0x80, 48,   0x89, 10,  0x00, 0x00, 0xFF};
const std::vector<uint8_t> kWiiULoopBytecode = {0xB0, 48,  0xE1, 0x00, 120, 60,   100,  48,   0x80, 48,
                                                64,   100, 48,   0x80, 48,  0x89, 0x00, 0x00, 10,   0xFF};

// Key 60 for 24 ticks, then the end.
const std::vector<uint8_t> kNoteBytecode = {0xB0, 48, 0xE1, 120, 0x00, 60, 100, 24, 0xFF};
const std::vector<uint8_t> kWiiUNoteBytecode = {0xB0, 48, 0xE1, 0x00, 120, 60, 100, 24, 0xFF};

// The same for the Wii, in big-endian order. The test bank's regions decay and release over many seconds. Each sequence
// therefore first sets a full sustain and the fastest release (D1 7F D2 7F D3 7F), and the loop jumps back to byte 16.
const std::vector<uint8_t> kWiiLoopBytecode = {0xB0, 48,   0xE1, 0x00, 120,  0xD1, 0x7F, 0xD2, 0x7F,
                                               0xD3, 0x7F, 60,   100,  48,   0x80, 48,   64,   100,
                                               48,   0x80, 48,   0x89, 0x00, 0x00, 16,   0xFF};
const std::vector<uint8_t> kWiiNoteBytecode = {0xB0, 48,   0xE1, 0x00, 120, 0xD1, 0x7F, 0xD2,
                                               0x7F, 0xD3, 0x7F, 60,   100, 24,   0xFF};

// A build of NW4R SND before March 2008. It plays as Wii Sports' build.
constexpr ntdlib::SndBuild kWiiBuild{20070227, false};

// The length of the loop's sequence in milliseconds: its intro and two passes of its loop, 288 ticks at 96 ticks a
// second.
constexpr uint32_t kLoopLength = 3000;

// A looped saw of 480 samples at 48 kHz, as PCM16 in the byte order of `flavor`'s console.
std::vector<uint8_t> SawWave(ntdlib::sar::Flavor flavor)
{
    const bool big_endian = flavor == ntdlib::sar::Flavor::kCafe;
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop = true;
    wave.sample_rate = 48000;
    wave.loop_end = 480;
    std::vector<uint8_t> samples;
    for (int i = 0; i < 480; i++)
    {
        const auto sample = static_cast<uint16_t>(static_cast<int16_t>(i * 40 - 9600));
        samples.push_back(static_cast<uint8_t>(big_endian ? sample >> 8 : sample));
        samples.push_back(static_cast<uint8_t>(big_endian ? sample : sample >> 8));
    }
    wave.channels = {samples};

    return sar_files::Wave(flavor, wave);
}

// The test archive of `flavor`'s console: the loop's sequence and the note's, on a bank whose instrument plays the saw
// for every key, at original key 60.
std::shared_ptr<const ntdlib::sar::SoundArchive> TestArchive(ntdlib::sar::Flavor flavor)
{
    const bool big_arguments = flavor != ntdlib::sar::Flavor::kNx;
    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    sar_files::SoundSpec loop_sound;
    loop_sound.name = "SEQ_LOOP";
    loop_sound.volume = 127;
    loop_sound.banks = {0};
    sar_files::SoundSpec note_sound = loop_sound;
    note_sound.name = "SEQ_NOTE";
    note_sound.file = 1;

    sar_files::ArchiveSpec spec;
    spec.sounds = {loop_sound, note_sound};
    spec.banks = {{"BANK_TEST", 2}};
    spec.wave_archives = {{"WARC_TEST", 3}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {
        {sar_files::Sequence(flavor, big_arguments ? kWiiULoopBytecode : kLoopBytecode, {{"Loop", 0}}), "", false},
        {sar_files::Sequence(flavor, big_arguments ? kWiiUNoteBytecode : kNoteBytecode, {{"Note", 0}}), "", false},
        {sar_files::Bank(flavor, bank), "", false},
        {sar_files::WaveArchive(flavor, {SawWave(flavor)}), "", false}};

    return hokuto::song::LoadArchive(sar_files::Archive(flavor, spec),
                                     flavor == ntdlib::sar::Flavor::kCtr ? "test.bcsar" : "test.bfsar");
}

// The bytes of a Wii archive whose one sequence sound plays `bytecode` on ntdlib's test bank. The test archive's waves
// are silent: each frame of their DSP-ADPCM holds only its header. Here each frame gives the samples of its nibbles at
// a scale of 2^11.
std::vector<uint8_t> WiiArchiveBytes(const std::vector<uint8_t>& bytecode)
{
    namespace rsar_files = ntdlib::testing::rsar_files;
    std::vector<uint8_t> bytes = rsar_files::MakeArchive(rsar_files::MakeSequence(bytecode), 0, 1);
    constexpr uint8_t kNibbles[] = {0x7F, 0x19, 0xE3, 0x5A, 0xC6, 0x2D, 0x91};
    for (std::size_t at = 0; at + 4 <= bytes.size(); at++)
    {
        if (std::memcmp(&bytes[at], "RWAV", 4) != 0)
        {
            continue;
        }

        // The wave's DATA block holds 0x40 bytes of samples after its header of 8 bytes.
        const uint32_t data = xsflib::LoadBe32(&bytes[at + 0x18]);
        for (std::size_t frame = 0; frame < 0x40; frame += 8)
        {
            uint8_t* samples = &bytes[at + data + 8 + frame];
            samples[0] = 0x0B;
            std::memcpy(samples + 1, kNibbles, sizeof(kNibbles));
        }
    }

    return bytes;
}

// Whether `ms` lies within two frames of `expected`, on a console whose frames last `frame_ms`.
bool Near(uint32_t ms, uint32_t expected, uint32_t frame_ms)
{
    return ms + 2 * frame_ms >= expected && ms <= expected + 2 * frame_ms;
}

// The length of a sound that loops and of a sound that ends, on each console. A sound past the archive's sounds has no
// length.
void TestMeasure()
{
    for (const ntdlib::sar::Flavor flavor :
         {ntdlib::sar::Flavor::kNx, ntdlib::sar::Flavor::kCafe, ntdlib::sar::Flavor::kCtr})
    {
        const auto archive = TestArchive(flavor);
        const uint32_t frame_ms = flavor == ntdlib::sar::Flavor::kCafe ? 3 : 5;
        const std::optional<hokuto::song::Length> loop = hokuto::song::MeasureLength(*archive, kLoopSound);
        XSFLIB_CHECK(loop && Near(loop->length, kLoopLength, frame_ms) && loop->fade == 10000 && loop->loops == 2 &&
                     !loop->silent && (loop->loop == hokuto::song::Loop{96, 192}));

        // The note sounds for 250 ms. Its release and 500 ms of silence follow.
        const std::optional<hokuto::song::Length> note = hokuto::song::MeasureLength(*archive, kNoteSound);
        XSFLIB_CHECK(note && note->length >= 750 && note->length < kLoopLength && note->fade == 0 && note->loops == 0 &&
                     !note->silent && !note->loop);
        XSFLIB_CHECK(!hokuto::song::MeasureLength(*archive, 2));
    }

    // The Wii's archives hold one sequence sound each. Its frames last 3 ms.
    const auto wii_loop =
        hokuto::song::LoadWiiArchive(WiiArchiveBytes(kWiiLoopBytecode), "loop.brsar", ntdlib::rvl::snd::kRsarOptions);
    const std::optional<hokuto::song::Length> loop = hokuto::song::MeasureLength(*wii_loop, kWiiBuild, 0);
    XSFLIB_CHECK(loop && Near(loop->length, kLoopLength, 3) && loop->fade == 10000 && loop->loops == 2 &&
                 !loop->silent && (loop->loop == hokuto::song::Loop{96, 192}));
    XSFLIB_CHECK(!hokuto::song::MeasureLength(*wii_loop, kWiiBuild, 1));

    const auto wii_note =
        hokuto::song::LoadWiiArchive(WiiArchiveBytes(kWiiNoteBytecode), "note.brsar", ntdlib::rvl::snd::kRsarOptions);
    const std::optional<hokuto::song::Length> note = hokuto::song::MeasureLength(*wii_note, kWiiBuild, 0);
    XSFLIB_CHECK(note && note->length >= 750 && note->length < kLoopLength && note->fade == 0 && note->loops == 0 &&
                 !note->silent && !note->loop);
}

// A file takes the lengths of its sequence sounds, and drops the lengths of other sounds. A progress that returns
// false stops the measure.
void TestFileLengths()
{
    const auto archive = TestArchive(ntdlib::sar::Flavor::kNx);
    hokuto::format::File file;
    file.lengths[7] = {.length = 1};
    std::size_t calls = 0;
    const auto progress = [&calls](std::size_t measured, std::size_t sounds)
    {
        calls++;

        return measured <= sounds;
    };
    const std::optional<hokuto::format::File> measured = hokuto::edit::WithLengths(file, *archive, 2, progress);
    XSFLIB_CHECK(measured && measured->lengths.size() == 2 && calls == 2);
    if (measured && measured->lengths.size() == 2)
    {
        const hokuto::format::SoundLength& loop = measured->lengths.at(kLoopSound);
        XSFLIB_CHECK(Near(loop.length, kLoopLength, 5) && loop.fade == 10000 && loop.loops == 2 &&
                     (loop.loop == hokuto::format::LoopTicks{96, 192}));
        XSFLIB_CHECK(measured->lengths.at(kNoteSound).fade == 0 && !measured->lengths.at(kNoteSound).loop);
    }

    // A progress that stops after the first sound stops the measure.
    const auto stop = [](std::size_t, std::size_t)
    {
        return false;
    };
    XSFLIB_CHECK(!hokuto::edit::WithLengths(file, *archive, 1, stop));

    // A file of the Wii takes the lengths of its sequence sound.
    const std::vector<uint8_t> wii_bytes = WiiArchiveBytes(kWiiLoopBytecode);
    const hokuto::format::File wii_file = hokuto::format::ImportWiiArchive(wii_bytes, kWiiBuild);
    const auto wii_archive = hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(wii_file), "loop.brsar",
                                                          ntdlib::rvl::snd::kRsarOptions);
    const std::optional<hokuto::format::File> wii_measured =
        hokuto::edit::WithLengths(wii_file, *wii_archive, hokuto::format::BuildOfWiiArchive(*wii_file.native), 2);
    XSFLIB_CHECK(wii_measured && wii_measured->lengths.size() == 1);
    if (wii_measured && wii_measured->lengths.size() == 1)
    {
        const hokuto::format::SoundLength& loop = wii_measured->lengths.at(0);
        XSFLIB_CHECK(Near(loop.length, kLoopLength, 3) && loop.fade == 10000 && loop.loops == 2 &&
                     (loop.loop == hokuto::format::LoopTicks{96, 192}));
    }
}

} // namespace

int main()
{
    TestMeasure();
    TestFileLengths();

    return xsflib::testing::Summary("lengths");
}
