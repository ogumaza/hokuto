// SPDX-License-Identifier: MIT

// Makes the files of the conformance suite (tests/conformance/README.md), and the archives that they hold.
//
// A new file of the Switch holds a wave archive of two looped waves, a bank of two instruments that play them, and a
// sequence sound. Its song plays two tracks round a loop, with the commands of the mix, an LFO, a pitch bend, a release
// and sends to aux bus A. The song goes to the Wii U and the 3DS through File > Export > Another Console…'s edit, and
// to the Wii through the same edit for the Wii. Each file gets an effect of its console on bus A at the values that a
// new effect starts from: on the Switch, nn::audio's I3DL2 preset 1, the reverb of hokuto_check_playback --reverb 1.
// Each file then goes out in strict mixing. A file of each console but the Wii also goes out in enhanced mixing. The
// tool writes each file in the text form to the output folder, with the archive that the file holds beside it. It also
// writes hashes.txt: a line for each sequence sound of each file, with the file's name, the sound's index, the seconds
// of the render and the render's SHA-256 (tests/conformance_render.h).
//
// The hashes are the reference only once hokuto_check_playback has checked Hokuto's player against ntdlib's model of
// each console, run directly, on each archive, with the file's effect and mixing mode. The suite's README gives the
// runs. hokuto_check_playback gives the player its effect from an option. The tool checks that each file's effect plays
// the same as that option's.
//
// usage: hokuto_make_conformance <output folder>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/files.h"

#include "conformance_render.h"
#include "edit/bytecode.h"
#include "edit/effects.h"
#include "edit/new_items.h"
#include "edit/other_console.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/sequence_model.h"

namespace
{

namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace song = hokuto::song;

// The seconds of each render. The song's loop lasts 1.5 s.
constexpr double kSeconds = 8.0;

// The song's bytecode, in the Switch's byte order. Track 0 picks program 0, sets its volume, pan and send, and plays
// four notes with an LFO, a pitch bend and a release, then a rest. Track 1 plays program 1 on the other side, with less
// send. Each track loops after 144 ticks: three beats at 120 beats a minute.
const std::vector<uint8_t> kBytecode = {
    0xFE, 0x03, 0x00,             // 0: allocate tracks 0 and 1
    0x88, 0x01, 0x31, 0x00, 0x00, // 3: open track 1 at offset 49
    0xE1, 0x78, 0x00,             // 8: tempo 120
    0xD9, 0x50,                   // 11: send A 80
    0x81, 0x00,                   // 13: program 0
    0xC1, 0x64,                   // 15: volume 100, the loop's start
    0xC0, 0x30,                   // 17: pan 48
    0x3C, 0x64, 0x18,             // 19: key 60 for 24 ticks
    0xCA, 0x30,                   // 22: LFO depth 48
    0xCB, 0x20,                   // 24: LFO speed 32
    0x40, 0x5A, 0x18,             // 26: key 64 for 24 ticks
    0xC4, 0x20,                   // 29: pitch bend +32
    0x43, 0x50, 0x30,             // 31: key 67 for 48 ticks
    0xC4, 0x00,                   // 34: pitch bend 0
    0xCA, 0x00,                   // 36: LFO depth 0
    0xD3, 0x64,                   // 38: release 100
    0x48, 0x64, 0x18,             // 40: key 72 for 24 ticks
    0x80, 0x18,                   // 43: wait 24 ticks
    0x89, 0x0F, 0x00, 0x00,       // 45: jump to offset 15
    0xD9, 0x28,                   // 49: track 1, send A 40
    0x81, 0x01,                   // 51: program 1
    0xC0, 0x58,                   // 53: pan 88
    0xC1, 0x50,                   // 55: volume 80, the loop's start
    0x30, 0x6E, 0x30,             // 57: key 48 for 48 ticks
    0x37, 0x64, 0x30,             // 60: key 55 for 48 ticks
    0x34, 0x64, 0x30,             // 63: key 52 for 48 ticks
    0x89, 0x37, 0x00, 0x00,       // 66: jump to offset 55
};

// A looped wave of 16-bit PCM at 32 kHz: one period of a saw or of a square, 64 samples long.
std::vector<uint8_t> LoopedWave(bool square)
{
    constexpr uint32_t kLength = 64;
    std::vector<uint8_t> samples;
    for (uint32_t i = 0; i < kLength; i++)
    {
        const int value = square ? (i < kLength / 2 ? 9000 : -9000) : static_cast<int>(i) * 400 - 12800;
        const auto bits = static_cast<uint16_t>(static_cast<int16_t>(value));
        samples.push_back(static_cast<uint8_t>(bits & 0xFF));
        samples.push_back(static_cast<uint8_t>(bits >> 8));
    }

    sar_files::WaveSpec spec;
    spec.encoding = 1;
    spec.loop = true;
    spec.loop_end = kLength;
    spec.channels = {samples};

    return sar_files::Wave(sar::Flavor::kNx, spec);
}

// An instrument that plays wave `wave` of the bank's waves at every key and velocity, from key 60, with envelope
// `adshr`.
sar_files::KeyTable Instrument(uint32_t wave, std::array<uint8_t, 5> adshr)
{
    sar_files::Region region;
    region.wave = wave;
    region.original_key = 60;
    region.adshr = adshr;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};

    return keys;
}

// The file of the Switch with the song.
format::File SwitchSong()
{
    const format::File empty = format::NewFile(format::Engine::kSwitch);
    const format::File waves = edit::AddWaveArchive(
        empty, "", sar_files::WaveArchive(sar::Flavor::kNx, {LoopedWave(false), LoopedWave(true)}));
    sar_files::BankSpec bank;
    bank.waves = {{sar::ItemId(sar::kWaveArchiveItem, 0), 0}, {sar::ItemId(sar::kWaveArchiveItem, 0), 1}};
    bank.instruments = {Instrument(0, {120, 110, 100, 0, 110}), Instrument(1, {127, 127, 127, 0, 100})};
    const format::File banked = edit::AddBank(waves, "BANK_CONFORMANCE", sar_files::Bank(sar::Flavor::kNx, bank),
                                              {{sar::ItemId(sar::kWaveArchiveItem, 0), 0}});

    const std::vector<ntdlib::nwsnd::LabelOffset> kLabels = {{"SEQ_CONFORMANCE", 0}};
    const std::vector<uint32_t> kEntries = {0};
    const ntdlib::nwsnd::Decoded decoded =
        ntdlib::nwsnd::DecodeSequence(kBytecode, kLabels, song::GrammarOf(edit::ConsoleOf(banked)), kEntries);
    return edit::AddSequenceSound(banked, std::nullopt, "SEQ_CONFORMANCE", decoded.sequence, decoded.entries.at(0), {});
}

// `file` with an effect of `kind` with `parameters` on aux bus A.
format::File WithEffect(format::File file, format::EffectKind kind, std::vector<float> parameters)
{
    file.aux_effects[0] = {format::Effect{kind, std::move(parameters)}};

    return file;
}

// The extension of the archive that `file` holds.
std::string ArchiveExtension(const format::File& file)
{
    if (file.native->wii)
    {
        return ".brsar";
    }

    return file.native->flavor == sar::Flavor::kCtr ? ".bcsar" : ".bfsar";
}

// Writes `bytes` to `path`. Prints the error and returns false when the write fails.
bool Write(const std::filesystem::path& path, std::span<const uint8_t> bytes)
{
    if (const std::optional<std::string> error = xsflib::WriteFile(path, bytes))
    {
        std::fprintf(stderr, "%s\n", error->c_str());
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: hokuto_make_conformance <output folder>\n");
        return 2;
    }

    const std::filesystem::path out = xsflib::PathFromUtf8(argv[1]);
    std::vector<std::pair<std::string, format::File>> files;
    try
    {
        const format::File nx = SwitchSong();
        const format::File cafe = edit::ForOtherConsole(nx, 0, sar::Flavor::kCafe).file;
        const format::File ctr = edit::ForOtherConsole(nx, 0, sar::Flavor::kCtr).file;
        const format::File wii = edit::ForWii(nx, 0).file;

        const auto i3dl2 = song::I3dl2Parameters(ntdlib::nx::audio::kI3dl2ReverbPresets[1]);
        const format::File nx_reverb = WithEffect(nx, format::EffectKind::kI3dl2Reverb, {i3dl2.begin(), i3dl2.end()});
        const format::File cafe_reverb =
            WithEffect(cafe, format::EffectKind::kReverbHi,
                       song::WiiUStartingParameters(*edit::WiiUTypeOf(format::EffectKind::kReverbHi)));
        const format::File ctr_reverb = WithEffect(ctr, format::EffectKind::kCtrReverb,
                                                   edit::CtrStartingParameters(format::EffectKind::kCtrReverb));
        const format::File wii_reverb =
            WithEffect(wii, format::EffectKind::kWiiReverbHi2008,
                       song::WiiStartingParameters(*edit::WiiTypeOf(format::EffectKind::kWiiReverbHi2008)));

        // The effects as hokuto_check_playback's options give them: --reverb 1 on the Switch, and --effect with
        // reverb_hi, 3ds_reverb and wii_reverb_hi_2008.
        song::AuxEffects nx_option;
        nx_option[0] = {ntdlib::nx::audio::kI3dl2ReverbPresets[1]};
        const ntdlib::cafe::snd::EffectType cafe_type = *edit::WiiUTypeOf(format::EffectKind::kReverbHi);
        song::AuxEffects cafe_option;
        cafe_option[0] = {song::WiiUEffectOf(cafe_type, song::WiiUStartingParameters(cafe_type))};
        song::AuxEffects ctr_option;
        ctr_option[0] = {song::CtrReverb{ntdlib::ctr_fx::Library::kNwSnd, {}}};
        const ntdlib::rvl::snd::EffectType wii_type = *edit::WiiTypeOf(format::EffectKind::kWiiReverbHi2008);
        song::AuxEffects wii_option;
        wii_option[0] = {song::WiiEffectOf(wii_type, song::WiiStartingParameters(wii_type))};

        for (const auto& [name, file, option] :
             {std::tuple{"switch", nx_reverb, nx_option}, std::tuple{"wii-u", cafe_reverb, cafe_option},
              std::tuple{"3ds", ctr_reverb, ctr_option}, std::tuple{"wii", wii_reverb, wii_option}})
        {
            for (const format::Mixing mixing : {format::Mixing::kStrict, format::Mixing::kEnhanced})
            {
                if (mixing == format::Mixing::kEnhanced && file.native->wii)
                {
                    continue;
                }

                const format::File mixed = format::WithMixing(file, mixing);
                if (hokuto::conformance::Render(mixed, 0, kSeconds) !=
                    hokuto::conformance::Render(mixed, 0, kSeconds, &option))
                {
                    std::fprintf(stderr, "%s: the file's effect plays otherwise than the check's\n", name);
                    return 1;
                }
                files.emplace_back(std::string(name) + (mixing == format::Mixing::kStrict ? "-strict" : "-enhanced"),
                                   mixed);
            }
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "can't make the files: %s\n", error.what());
        return 1;
    }

    std::string hashes;
    for (const auto& [name, file] : files)
    {
        const std::string text = format::WriteText(file);
        const std::vector<uint8_t> archive = format::ExportArchive(file);
        if (!Write(out / (name + ".hok"), std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size())) ||
            !Write(out / (name + ArchiveExtension(file)), archive))
        {
            return 1;
        }

        // The hashes come from the file read back as a player reads it.
        const format::File read =
            format::ReadEitherForm(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
        for (uint32_t sound = 0; sound < read.native->tables->sounds.size(); sound++)
        {
            if (read.native->tables->sounds[sound].kind != sar::SoundType::kSequence)
            {
                continue;
            }

            const std::vector<uint8_t> render = hokuto::conformance::Render(read, sound, kSeconds);
            char line[160];
            std::snprintf(line, sizeof(line), "%s.hok %u %.1f %s\n", name.c_str(), sound, kSeconds,
                          hokuto::conformance::Sha256(render).c_str());
            hashes += line;

            // The render's peak, and whether the effect changes it.
            int peak = 0;
            for (std::size_t i = 0; i + 1 < render.size(); i += 2)
            {
                const auto sample = static_cast<int16_t>(static_cast<uint16_t>(render[i] | (render[i + 1] << 8)));
                peak = std::max(peak, std::abs(static_cast<int>(sample)));
            }
            const hokuto::song::AuxEffects dry;
            const bool wet = hokuto::conformance::Render(read, sound, kSeconds, &dry) != render;
            std::printf("%s: sound %u, %zu frames, peak %d, %s\n", name.c_str(), sound, render.size() / 4, peak,
                        wet ? "the effect sounds" : "the effect changes nothing");
        }
    }

    return Write(out / "hashes.txt", std::span(reinterpret_cast<const uint8_t*>(hashes.data()), hashes.size())) ? 0 : 1;
}
