// SPDX-License-Identifier: MIT

// This header contains the archives, banks and helpers that the tests of the conversions to other consoles share.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "binary_form_testing.h"
#include "cart_bytes.h"
#include "edit/ds_new_items.h"
#include "edit/effects.h"
#include "edit/other_console.h"
#include "edit/replace.h"
#include "edit/tables.h"
#include "edit/timelines.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/biquads.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/sequence.h"
#include "song/sequence_reader.h"

namespace
{

namespace format = hokuto::format;
namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace song = hokuto::song;

using format::EffectKind;
using format::File;
using hokuto::testing::ComesBack;
using xsflib::testing::ErrorOf;

// The archive's sounds, banks and wave archives.
constexpr uint32_t kSong = 0;
constexpr uint32_t kOther = 1;
constexpr uint32_t kWavesA = 0;
constexpr uint32_t kWavesB = 1;
constexpr uint32_t kWavesAId = 0x05000000;
constexpr uint32_t kWavesBId = 0x05000001;
constexpr uint32_t kLackedWavesId = 0x05000007;

// The sequence file. The song picks program 0 and plays a note, then program 1 and another. The other sound starts at
// offset 13 and plays a note of program 0.
const std::vector<uint8_t> kBytecode = {
    0xB0, 48,      // 0: timebase 48
    0x81, 0,       // 2: program 0
    60,   100, 24, // 4: key 60 for 24 ticks
    0x81, 1,       // 7: program 1
    62,   90,  12, // 9: key 62 for 12 ticks
    0xFF,          // 12: the end
    0x81, 0,       // 13: program 0
    64,   80,  24, // 15: key 64 for 24 ticks
    0xFF,          // 18: the end
};

// The version of a wave of `flavor` without the original start of its loop.
inline uint32_t OldWaveVersion(sar::Flavor flavor)
{
    return flavor == sar::Flavor::kCtr ? 0x02010000 : 0x00010100;
}

// The version of a wave of `flavor` with the original start of its loop.
inline uint32_t NewWaveVersion(sar::Flavor flavor)
{
    return flavor == sar::Flavor::kCtr ? 0x02010100 : 0x00010200;
}

// A looped wave of the console's 16-bit PCM samples with the version `version`. The loop lets a note last its length on
// both consoles. A wave that ends sooner ends the note in another tick on each.
inline std::vector<uint8_t> PcmWave(sar::Flavor flavor, uint8_t value, uint32_t version)
{
    sar_files::WaveSpec spec;
    spec.encoding = 1;
    spec.loop = true;
    spec.loop_end = 4;
    spec.channels = {{value, value, 0, value, value, 0, 0, value}};
    std::vector<uint8_t> wave = sar_files::Wave(flavor, spec);
    (flavor == sar::Flavor::kCafe ? xsflib::StoreBe32 : xsflib::StoreLe32)(&wave[0x08], version);

    return wave;
}

// A looped wave of 8-bit PCM samples.
inline std::vector<uint8_t> Pcm8Wave(sar::Flavor flavor)
{
    sar_files::WaveSpec spec;
    spec.encoding = 0;
    spec.loop = true;
    spec.loop_end = 8;
    spec.channels = {{0x10, 0x20, 0x7F, 0x80, 0xF0, 0x00, 0x01, 0xFF}};

    return sar_files::Wave(flavor, spec);
}

// A DSP-ADPCM wave of two frames, 28 samples, with a loop from sample 14.
inline std::vector<uint8_t> AdpcmWave(sar::Flavor flavor)
{
    sar_files::WaveSpec spec;
    spec.encoding = 2;
    spec.loop = true;
    spec.loop_start = 14;
    spec.loop_end = 28;
    spec.channels = {{0x21, 0x13, 0x57, 0x9B, 0xDF, 0x02, 0x46, 0x8A, 0x12, 0xF1, 0xE2, 0xD3, 0xC4, 0xB5, 0xA6, 0x97}};

    return sar_files::Wave(flavor, spec);
}

// An instrument whose one region plays wave `wave` of its bank.
inline sar_files::KeyTable Instrument(uint32_t wave)
{
    sar_files::Region region;
    region.wave = wave;
    region.original_key = 60;
    region.adshr = {127, 127, 127, 0, 127};
    const sar_files::VelocityTable velocities{sar_files::VelocityTable::kDirect, 0, {{0, region}}};
    return sar_files::KeyTable{sar_files::KeyTable::kDirect, 0, {{0, velocities}}};
}

// A bank that names `waves`, with an instrument for each wave in turn.
inline std::vector<uint8_t> Bank(sar::Flavor flavor, const std::vector<std::pair<uint32_t, uint32_t>>& waves)
{
    sar_files::BankSpec bank;
    bank.waves = waves;
    for (uint32_t i = 0; i < waves.size(); ++i)
    {
        bank.instruments.push_back(Instrument(i));
    }

    return sar_files::Bank(flavor, bank);
}

// The test archive of `flavor`, laid out as the tools lay one out, with `bytecode` in its sequence file. The song plays
// banks A and B. Bank A names both waves of wave archive A. Bank B names wave archive B's first wave and wave archive
// A's second wave. Wave archive B's first wave holds the original start of its loop, and its second 8-bit PCM. The
// other sound starts at offset `other_start` and plays bank C. Bank C names a wave archive that the archive lacks.
inline std::vector<uint8_t> TestArchive(sar::Flavor flavor, const std::vector<uint8_t>& bytecode = kBytecode,
                                        uint32_t other_start = 13)
{
    sar::ArchiveModel model;
    model.flavor = flavor;
    model.version = flavor == sar::Flavor::kCtr ? 0x02030000 : 0x00020200;
    model.players = {{.name = "PLAYER_TEST", .playable_sound_max = 1}};

    sar::SoundModel song;
    song.name = "SEQ_SONG";
    song.volume = 90;
    song.pan_mode = 1;
    song.pan_curve = 2;
    song.player_priority = 48;
    song.actor_player_id = 2;
    song.single_play = 0x00010005;
    song.settings_3d = {.flags = 0x7, .decay_ratio = 0.25f, .decay_curve = 2, .doppler_factor = 3};
    song.user_options = {0x12345678, std::nullopt, std::nullopt, std::nullopt};
    song.banks = {sar::ItemId(sar::kBankItem, 0), sar::ItemId(sar::kBankItem, 1)};
    song.allocate_track_flags = 3;
    song.channel_priority = 40;
    song.release_priority_fix = true;
    sar::SoundModel other;
    other.name = "SEQ_OTHER";
    other.banks = {sar::ItemId(sar::kBankItem, 2)};
    other.start_offset = other_start;
    model.sounds = {song, other};
    model.banks = {
        {.name = "BANK_A", .file_id = 1}, {.name = "BANK_B", .file_id = 2}, {.name = "BANK_C", .file_id = 3}};
    model.wave_archives = {{.name = "WARC_A", .file_id = 4}, {.name = "WARC_B", .file_id = 5}};
    model.files = {
        {.bytes = sar_files::Sequence(flavor, bytecode, {{"Song", 0}, {"Other", other_start}})},
        {.bytes = Bank(flavor, {{kWavesAId, 0}, {kWavesAId, 1}})},
        {.bytes = Bank(flavor, {{kWavesBId, 0}, {kWavesAId, 1}})},
        {.bytes = Bank(flavor, {{kLackedWavesId, 0}})},
        {.bytes = sar_files::WaveArchive(flavor, {PcmWave(flavor, 0x30, OldWaveVersion(flavor)), AdpcmWave(flavor)})},
        {.bytes = sar_files::WaveArchive(flavor, {PcmWave(flavor, 0x50, NewWaveVersion(flavor)), Pcm8Wave(flavor)})},
    };

    return sar::BuildArchive(model);
}

// The test file of `flavor` with `bytecode` in its sequence file, and with its tags, scenes, colours and buses'
// volumes. A file of the Switch or the Wii U holds the enhanced mixing for the song, an I3DL2 reverb of the Switch on
// bus A and a chorus of the Wii U on bus B. Each file holds a reverb of the 3DS on bus A and an AXFX chorus of the Wii
// on bus B. Bus C holds none: the 3DS lacks it.
inline File TestFile(sar::Flavor flavor, const std::vector<uint8_t>& bytecode = kBytecode, uint32_t other_start = 13)
{
    File file = format::ImportArchive(TestArchive(flavor, bytecode, other_start), flavor);
    file.engine = format::EngineOf(flavor);
    file.tags = {{"title", "Test songs"}};
    file.sound_tags = {{kSong, {{"title", "The song"}}}, {kOther, {{"title", "The other"}}}};
    file.scenes = {{.sound = kSong, .name = "Loud", .values = {{3, 5}}, .changes = {{.time = 96, .variable = 3}}},
                   {.sound = kOther, .name = "Quiet", .values = {{1, 0}}, .changes = {}}};
    file.track_colours = {{{kSong, 0}, {200, 100, 50}}, {{kOther, 0}, {1, 2, 3}}};
    file.aux_volumes = {0.5f, 1.0f, 1.5f};
    if (flavor != sar::Flavor::kCtr)
    {
        file.sound_mixing = {{kSong, format::Mixing::kEnhanced}};
        file.aux_effects[0] = {{EffectKind::kI3dl2Reverb, std::vector<float>(12, 0.0f)}};
        file.aux_effects[1] = {{EffectKind::kChorus, {0.1f, 0.2f, 0.3f, 0.4f, 0.5f}}};
    }
    file.aux_effects[0].push_back(
        {EffectKind::kCtrReverb, hokuto::edit::CtrStartingParameters(EffectKind::kCtrReverb)});
    file.aux_effects[1].push_back({EffectKind::kWiiChorus, {10.0f, 0.5f, 1.0f}});

    return file;
}

// The archive that `file` exports, opened.
inline std::shared_ptr<const sar::SoundArchive> ArchiveOf(const File& file)
{
    return song::LoadArchive(format::ExportArchive(file), "test", format::ExportGroupFiles(file));
}

// The notes that ntdlib's performer plays of sound `sound` of `archive`, track by track. The archive is a later
// console's (sar::SoundArchive), the Wii's (ntdlib::rsar::SoundArchive) or the DS's (song::DsArchive).
template <typename Archive>
std::vector<std::vector<song::Note>> NotesOf(const Archive& archive, uint32_t sound)
{
    std::vector<std::vector<song::Note>> notes;
    for (const song::Track& track : song::ReadSequence(archive, sound).tracks)
    {
        notes.push_back(track.notes);
    }

    return notes;
}

// `notes` without the offsets of their note commands. The offsets move when commands leave the sequence.
inline std::vector<std::vector<song::Note>> WithoutOffsets(std::vector<std::vector<song::Note>> notes)
{
    for (std::vector<song::Note>& track : notes)
    {
        for (song::Note& note : track)
        {
            note.command = 0;
        }
    }

    return notes;
}

// Whether `warnings` hold `warning`.
inline bool Holds(const std::vector<std::string>& warnings, const std::string& warning)
{
    return std::ranges::find(warnings, warning) != warnings.end();
}

// The test sequence with a byte that no command reaches at its end, after `extra` at its start.
inline std::vector<uint8_t> FixedBytecode(const std::vector<uint8_t>& extra = {})
{
    std::vector<uint8_t> bytecode = extra;
    bytecode.insert(bytecode.end(), kBytecode.begin(), kBytecode.end());
    bytecode.push_back(0x55);

    return bytecode;
}

// The warning about the biquad filters between the Switch and `other`, after the sentences that name the rates.
inline std::string BiquadText(const std::string& other, const std::string& changes)
{
    return "The Switch runs biquad filter types 1–5 at 48 kHz, and the " + other + " runs them at " +
           (other == "3DS" ? "32,728 Hz" : "32 kHz") +
           ". Their cutoffs are about 1.5 times higher on the Switch. Switch types 6–10 are the same filters "
           "designed for 48 kHz. " +
           changes;
}

// A sequence file with a send to bus C and the settings of two LFOs. The song sends to bus C, calls a subroutine at
// offset 20 and plays key 62. The subroutine sets LFO 0's curve, plays key 60 and sets LFO 1's depth. The 3DS's player
// skips the three settings. The 3DS reads LFO 1's depth without its byte. The other sound starts at offset 14. The
// Switch gives the call's offset in little-endian order, and the Wii U and the 3DS in big-endian order.
inline std::vector<uint8_t> SkippedBytecode(sar::Flavor flavor)
{
    const bool little = flavor == sar::Flavor::kNx;
    const uint8_t low = little ? 20 : 0;
    const uint8_t high = little ? 0 : 20;
    std::vector<uint8_t> bytecode = {
        0xB0, 48,             // 0: timebase 48
        0xDE, 20,             // 2: send C 20
        0x8A, low,  0,  high, // 4: call 20
        0x81, 1,              // 8: program 1
        62,   90,   12,       // 10: key 62 for 12 ticks
        0xFF,                 // 13: the end
        0x81, 0,              // 14: program 0
        64,   80,   24,       // 16: key 64 for 24 ticks
        0xFF,                 // 19: the end
        0xBE, 1,              // 20: LFO 0's curve 1
        0x81, 0,              // 22: program 0
        60,   100,  24,       // 24: key 60 for 24 ticks
        0xF0, 0xA2,           // 27: LFO 1's depth
    };
    if (flavor != sar::Flavor::kCtr)
    {
        bytecode.push_back(5);
    }
    bytecode.push_back(0xFD); // the return

    return bytecode;
}

// The sequence file of the Wii's test archive. The archive's version, 1.4, gives NW4R SND's build of June 2008. The
// song picks program 0, selects bank 1 and sets a low-pass of byte 34. The Wii's player skips the bank select. The song
// then picks program 1 if global variable 20 holds 0, and plays key 60. NW4R SND starts the global variables at 0.
// Volume2 then goes to 90 over 48 ticks. NW4R SND sets it at once. The song transposes by variable 3 behind a random
// prefix, and plays key 62. The prefix gives nothing on the Wii. An end after a comparison ends the track on the Wii
// whatever the comparison gave: key 64 never plays.
const std::vector<uint8_t> kWiiBytecode = {
    0xB0, 0x30,                   // 0: timebase 48
    0x81, 0x00,                   // 2: program 0
    0xB6, 0x01,                   // 4: bank select 1
    0xC1, 0x64,                   // 6: volume 100
    0xD8, 0x22,                   // 8: low-pass 34
    0xF0, 0x90, 0x14, 0x00, 0x00, // 10: whether variable 20 holds 0
    0xA2, 0x81, 0x01,             // 15: if so, program 1
    0x3C, 0x64, 0x18,             // 18: key 60 for 24 ticks
    0xA3, 0xD5, 0x5A, 0x00, 0x30, // 21: volume2 90 over 48 ticks
    0xA0, 0xA1, 0xC3, 0x03,       // 26: transpose by variable 3, behind a random prefix
    0x3E, 0x5A, 0x0C,             // 30: key 62 for 12 ticks
    0xF0, 0x90, 0x03, 0x00, 0x05, // 33: whether variable 3 holds 5
    0xA2, 0xFF,                   // 38: if so, the end
    0x40, 0x50, 0x0C,             // 40: key 64 for 12 ticks
    0xFF,                         // 43: the end
};

// A file of the Wii whose one sound plays `bytecode` by the rules of `build`, or else of the build that the archive's
// version gives. The sound's bank plays two waves of an RWAR.
inline File WiiFile(const std::vector<uint8_t>& bytecode = kWiiBytecode,
                    const std::optional<ntdlib::SndBuild>& build = {})
{
    ntdlib::rsar::SequenceFile sequence;
    sequence.data = bytecode;
    sequence.labels = {{"SEQ_TEST", 0}};

    return format::ImportWiiArchive(ntdlib::testing::rsar_files::MakeArchive(sequence.Write(), 0, 0b1), build);
}

// The Wii archive that `file` exports, opened.
inline std::shared_ptr<const ntdlib::rsar::SoundArchive> WiiArchiveOf(const File& file)
{
    return song::LoadWiiArchive(format::ExportArchive(file), "test.brsar", ntdlib::rvl::snd::kRsarOptions);
}

// The warnings of a song of WiiFile on its way to each later console.
const std::vector<std::string> kWiiWarnings = {
    "The new file leaves out 1 command: bank select (1). The Wii skipped it.",
    "The sound reads global variable 20 before setting it. NW4R SND starts global variables at 0. Later consoles start "
    "them at -1. New commands at the start of the sound set it to 0 if it is -1.",
};

// What a listener hears of `notes`: when each note starts, how long it lasts, its key, its velocity and its program.
inline std::vector<std::vector<std::array<uint32_t, 5>>> Heard(const std::vector<std::vector<song::Note>>& notes)
{
    std::vector<std::vector<std::array<uint32_t, 5>>> heard;
    for (const std::vector<song::Note>& track : notes)
    {
        std::vector<std::array<uint32_t, 5>>& out = heard.emplace_back();
        for (const song::Note& note : track)
        {
            out.push_back({note.start, note.length, note.key, note.velocity, note.program});
        }
    }

    return heard;
}

// A song that plays both of its banks for the Wii. Key 60 plays program 1 of slot 0. A bank select of slot 1 follows,
// and key 62 plays program 1 there. Key 64 plays program 0 of slot 1, and key 65 program 0 of slot 0 after a bank
// select of slot 0. The other sound starts at offset 23 and plays key 64 for 24 ticks.
const std::vector<uint8_t> kTwoBanksBytecode = {
    0xB0, 48,      // 0: timebase 48
    0x81, 1,       // 2: program 1
    60,   100, 24, // 4: key 60 for 24 ticks
    0xB6, 1,       // 7: bank select 1
    62,   90,  12, // 9: key 62 for 12 ticks
    0x81, 0,       // 12: program 0
    64,   80,  12, // 14: key 64 for 12 ticks
    0xB6, 0,       // 17: bank select 0
    65,   80,  12, // 19: key 65 for 12 ticks
    0xFF,          // 22: the end
    0x81, 0,       // 23: program 0
    64,   80,  24, // 25: key 64 for 24 ticks
    0xFF,          // 28: the end
};

// A song for the Wii that plays program 1 of slot 0 alone. The other sound starts at offset 8.
const std::vector<uint8_t> kOneProgramBytecode = {
    0xB0, 48,      // 0: timebase 48
    0x81, 1,       // 2: program 1
    60,   100, 24, // 4: key 60 for 24 ticks
    0xFF,          // 7: the end
    0x81, 0,       // 8: program 0
    64,   80,  24, // 10: key 64 for 24 ticks
    0xFF,          // 13: the end
};

// A cartridge of MP2K songs on a looped saw. Song 0 plays at 150 beats a minute as MP2K reads it. Its track 0 plays a
// note in its first bar, then loops over a bar of two notes. Track 1 plays a note on each beat of the loop, panned to
// the left. Song 0's header sets `reverb`. Song 1 plays no notes.
inline std::vector<uint8_t> GbaCart(uint8_t reverb = 0)
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    mp2k_cart::Track melody;
    melody.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(96);
    const std::size_t loop = melody.Here();
    melody.Note(24, 64, 90).Wait(48).Note(48, 67, 80).Wait(48).Goto(loop);
    mp2k_cart::Track beats;
    beats.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 80).Cmd(kCmdPan, 0x20).Wait(96);
    const std::size_t beat = beats.Here();
    beats.Note(12, 48, 110).Wait(24).Note(12, 48, 70).Wait(24).Note(12, 48, 110).Wait(24).Note(12, 48, 70).Wait(24);
    beats.Goto(beat);
    cart.Song({melody, beats}, voices, 0, 0, reverb);
    mp2k_cart::Track silent;
    silent.Cmd(kCmdVoice, 0).Wait(24).Fine();
    cart.Song({silent}, voices);

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// The notes that `sequence` plays, in the order of their times: the time of each in milliseconds, its key and its
// velocity.
inline std::vector<std::tuple<double, uint8_t, uint8_t>> TimedNotes(const song::Sequence& sequence)
{
    std::vector<std::tuple<double, uint8_t, uint8_t>> notes;
    for (const song::Track& track : sequence.tracks)
    {
        for (const song::Note& note : track.notes)
        {
            notes.emplace_back(song::MsOfTick(sequence, note.start), note.key, note.velocity);
        }
    }
    std::ranges::sort(notes);

    return notes;
}

// Whether `sent` plays the notes of the GBA's `gba` with their keys, each within 10 ms of its time, and loops within 10
// ms of the GBA's loop. MP2K's velocity scales a note's level, and NintendoWare's engines scale it by the square of the
// velocity. Each note of `sent` has to use the velocity whose square comes nearest the level on the GBA.
inline bool PlaysAsGba(const song::Sequence& gba, const song::Sequence& sent)
{
    constexpr double kNear = 10.0;
    const auto gba_notes = TimedNotes(gba);
    const auto sent_notes = TimedNotes(sent);
    bool same = !gba_notes.empty() && gba_notes.size() == sent_notes.size() && gba.loop && sent.loop;
    for (std::size_t i = 0; same && i < gba_notes.size(); ++i)
    {
        const auto& [gba_ms, gba_key, gba_velocity] = gba_notes[i];
        const auto& [sent_ms, sent_key, sent_velocity] = sent_notes[i];
        same = std::abs(gba_ms - sent_ms) <= kNear && gba_key == sent_key &&
               std::lround(127.0 * std::sqrt(gba_velocity / 127.0)) == sent_velocity;
    }

    return same && std::abs(song::MsOfTick(gba, gba.loop->start) - song::MsOfTick(sent, sent.loop->start)) <= kNear &&
           std::abs(song::MsOfTick(gba, gba.loop->end) - song::MsOfTick(sent, sent.loop->end)) <= kNear;
}

} // namespace
