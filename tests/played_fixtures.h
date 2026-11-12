// SPDX-License-Identifier: MIT

// Fixtures that play: a GBA cartridge of one MP2K song, a DS sound archive and a DS image with DSE's files, each of
// four notes of a sine, a ROM of Krawall with one note, and a 3DS archive of one note. The tests of the measure, the
// 3DS and the sequence model share them.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/krawall_cart.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sdat_files.h"

#include "cart_bytes.h"
#include "song/ds.h"
#include "song/krawall.h"

namespace hokuto::testing
{

// A cartridge of one MP2K song: keys 60, 64, 67 and 72 for a quarter note each at 150 beats a minute, on a looped sine.
// The song's header sets the reverb to `reverb`, or leaves the reverb of the driver.
inline std::vector<uint8_t> GbaMeasureCart(uint8_t reverb)
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    std::vector<int8_t> sine(64);
    for (std::size_t i = 0; i < sine.size(); ++i)
    {
        sine[i] = static_cast<int8_t>(std::lround(100.0 * std::sin(2.0 * std::numbers::pi * i / 64.0)));
    }
    const uint32_t voices =
        cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(sine, 13379, 0), 60, {255, 0, 255, 200})}});
    mp2k_cart::Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    for (const int key : {60, 64, 67, 72})
    {
        track.Note(24, key, 100).Wait(24);
    }
    track.Fine();
    cart.Song({track}, voices, 0, 0, reverb);

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// A DS sound archive of one sequence: keys 60, 64, 67 and 72 for a quarter note each at the driver's tempo of 120, on a
// looped sine of 64 samples in PCM16 at half of full scale. A note waits for its length.
inline std::shared_ptr<const song::DsArchive> DsMeasureArchive()
{
    namespace sdat = ntdlib::sdat;
    namespace sdat_files = ntdlib::testing::sdat_files;

    sdat_files::Wave sine{.encoding = 1, .loop = 1, .loop_length = 32};
    for (int i = 0; i < 64; i++)
    {
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) / 64.0;
        const auto sample = static_cast<uint16_t>(static_cast<int16_t>(std::lround(16384.0 * std::sin(phase))));
        sine.data.push_back(static_cast<uint8_t>(sample));
        sine.data.push_back(static_cast<uint8_t>(sample >> 8));
    }
    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}};
    model.banks = {sdat::BankInfo{1, {0, 0xFFFF, 0xFFFF, 0xFFFF}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{2, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {sdat_files::Sequence({0x3C, 100, 0x30, 0x40, 100, 0x30, 0x43, 100, 0x30, 0x48, 100, 0x30, 0xFF}),
                   sdat_files::Bank({{.type = 1, .region = {.wave = 0}}}), sdat_files::WaveArchive({sine})};

    return song::LoadDsArchive(sdat::BuildArchive(model));
}

// A DS image with DSE's files: one song that plays four notes of a sine, each for half a second at a tempo of 120. The
// wave is in the main bank, in PCM16.
inline std::vector<uint8_t> DseMeasureImage()
{
    namespace dse_files = ntdlib::testing::dse_song_files;

    dse_files::Builder samples;
    for (int i = 0; i < 64; i++)
    {
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) / 64.0;
        const auto sample = static_cast<uint16_t>(static_cast<int16_t>(std::lround(16384.0 * std::sin(phase))));
        samples.U16(sample);
    }
    const dse_files::Bytes pcmd = samples.Take();
    const dse_files::Bytes wavi = dse_files::Table(1, {{0, dse_files::WaveEntry(0, 1, true, 32000, 0, 0, 32)}}, 0xAA);
    dse_files::Builder main;
    dse_files::HeaderStart(main, "swdl", 0x0000);
    dse_files::BankHeader(main, static_cast<uint32_t>(pcmd.size()), 1, 0, 1, static_cast<uint32_t>(wavi.size()));
    dse_files::BankChunk(main, "wavi", static_cast<uint32_t>(wavi.size())).Append(wavi);
    dse_files::BankChunk(main, "pcmd", static_cast<uint32_t>(pcmd.size())).Append(pcmd);
    dse_files::BankChunk(main, "eod ", 0);
    main.Put32(0x08, static_cast<uint32_t>(main.Size()));

    const dse_files::Bytes prgi = dse_files::Table(128, {{0, dse_files::Program(0, 0x7F, {}, {{.wave = 0}})}}, 0);
    dse_files::Builder bank;
    dse_files::HeaderStart(bank, "swdl", dse_files::kBankId);
    dse_files::BankHeader(bank, ntdlib::dse::WaveBank::kNoSamples, 1, 1, 2, static_cast<uint32_t>(wavi.size()));
    dse_files::BankChunk(bank, "wavi", static_cast<uint32_t>(wavi.size())).Append(wavi);
    dse_files::BankChunk(bank, "prgi", static_cast<uint32_t>(prgi.size())).Append(prgi);
    dse_files::BankChunk(bank, "kgrp", 8).Append({0, 0, 0xFF, 8, 0, 0xFF, 0, 0}).Fill(8, 0);
    dse_files::BankChunk(bank, "eod ", 0);
    bank.Put32(0x08, static_cast<uint32_t>(bank.Size()));

    const dse_files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const dse_files::Bytes track = dse_files::Join({{1, 1, 0, 0},
                                                    {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F},
                                                    dse_files::Note(0x64, 0, 0, 0x30, 1),
                                                    {0x92, 0x30},
                                                    dse_files::Note(0x64, 2, 0, 0x30, 1),
                                                    {0x92, 0x30},
                                                    dse_files::Note(0x64, 4, 0, 0x30, 1),
                                                    {0x92, 0x30},
                                                    dse_files::Note(0x64, 7, 0, 0x30, 1),
                                                    {0x92, 0x30, 0x98}});
    ntdlib::testing::nds_files::Image image;
    image.files = {
        {"SOUND/BGM/bgm.swd", main.Take()},
        {"SOUND/BGM/bgm0001.smd", dse_files::SongFile({conductor, track})},
        {"SOUND/BGM/bgm0001.swd", bank.Take()},
    };

    return ntdlib::testing::nds_files::BuildImage(image).first;
}

// A ROM of Krawall from ntdlib's test cartridge, with one module of 1 channel and 4 rows: C-4 of the first instrument.
inline std::shared_ptr<const song::DriverRom> KrawallMeasureRom()
{
    ntdlib::testing::krawall_cart::Cart image;
    image.Module(1, 4, {{.row = 0, .channel = 0, .note = 49, .instrument = 1}});
    const ntdlib::agb::Rom rom = image.ToRom();
    return song::LoadKrawallRom(hokuto::testing::RomBytes(rom));
}

// The archive's bytes: sequence sound 0 plays key 60 at velocity 127 for 96 ticks on program 0, waits 96 ticks and
// ends. Program 0 plays a constant wave of 2 s. With `sends`, the track first sends to buses A and B at full level.
inline std::vector<uint8_t> CtrNoteArchiveBytes(bool sends = false)
{
    namespace sar_files = ntdlib::testing::sar_files;
    constexpr ntdlib::sar::Flavor kCtr = ntdlib::sar::Flavor::kCtr;
    std::vector<uint8_t> commands = {0x3C, 0x7F, 0x60, 0x80, 0x60, 0xFF};
    if (sends)
    {
        commands.insert(commands.begin(), {0xD9, 0x7F, 0xDA, 0x7F});
    }
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_NOTE", .file = 0, .banks = {0}}};
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Sequence(kCtr, commands, {{"SEQ_NOTE", 0}})},
                  {sar_files::Bank(kCtr, sar_files::OneRegionBank())},
                  {sar_files::WaveArchive(kCtr, {sar_files::Wave(kCtr, sar_files::ConstantWave(false, 64000))})}};

    return sar_files::Archive(kCtr, spec);
}

} // namespace hokuto::testing
