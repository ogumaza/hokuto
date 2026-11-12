// SPDX-License-Identifier: MIT

// This header contains the waves, banks and helpers that the tests of the file replacements share.

#pragma once

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
#include <string_view>
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

#include "edit/files.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/preview.h"

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
using hokuto::edit::FileChanges;
using hokuto::edit::FileKind;
using hokuto::edit::IncomingFile;
using hokuto::edit::IncomingWave;
using hokuto::edit::LackedWaves;
using hokuto::edit::ProgramFix;
using hokuto::edit::WaveChanges;
using hokuto::format::File;
using hokuto::song::FileVersions;
using xsflib::testing::ErrorOf;

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
inline std::vector<uint8_t> Wave(sar::Flavor flavor, uint8_t value)
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop_end = 4;
    wave.channels = {{value, value, value, value, value, value, value, value}};

    return sar_files::Wave(flavor, wave);
}

// An instrument whose one region plays wave `wave` of its bank.
inline sar_files::KeyTable Instrument(uint32_t wave)
{
    sar_files::Region region;
    region.wave = wave;
    const sar_files::VelocityTable velocities{sar_files::VelocityTable::kDirect, 0, {{0, region}}};
    return sar_files::KeyTable{sar_files::KeyTable::kDirect, 0, {{0, velocities}}};
}

// A bank that names `waves` and has an instrument at each of `programs`. The instruments play the bank's waves in turn.
inline std::vector<uint8_t> Bank(sar::Flavor flavor, const std::vector<std::pair<uint32_t, uint32_t>>& waves,
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
inline std::vector<uint8_t> WaveSounds(sar::Flavor flavor, const std::vector<std::pair<uint32_t, uint32_t>>& waves,
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
inline std::vector<uint8_t> TestArchive(sar::Flavor flavor, const std::vector<uint8_t>& song_bytecode = kSongBytecode,
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
inline std::shared_ptr<const sar::SoundArchive> Read(std::vector<uint8_t> bytes)
{
    return hokuto::song::LoadArchive(std::move(bytes), "test.bfsar");
}

} // namespace
