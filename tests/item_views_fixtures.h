// SPDX-License-Identifier: MIT

// This header contains the archives and helpers that the tests of the item views share.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/mp2k/song.h"
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
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "ntdlib/wave/encode.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "app/item_views.h"
#include "app/key_edits.h"
#include "cart_bytes.h"
#include "edit/banks.h"
#include "edit/ds_banks.h"
#include "edit/ds_waves.h"
#include "edit/gba_voices.h"
#include "edit/groups.h"
#include "edit/new_items.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/ds.h"
#include "song/gba.h"

namespace
{

namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
namespace wave = ntdlib::wave;
namespace sar_files = ntdlib::testing::sar_files;
using hokuto::app::DroppedWaves;
using hokuto::app::LoopMarkerPlace;
using hokuto::app::WaveValues;
using hokuto::app::WaveView;
using hokuto::format::File;
using xsflib::testing::ErrorOf;

// A run's least and most sample.
using Peak = std::pair<int16_t, int16_t>;

// A cartridge whose song 0 selects the eight programs of its voice group, and the voice group's place. Program 0 plays
// a sample, and program 1 square 1 with a sweep, a length past the PSG's 63 and an envelope. Programs 2 and 5 play the
// same pattern on the wave channel. Program 3 is a drum kit: key 36 plays noise panned left, and key 38 the sample at a
// fixed pitch. Program 4 is a key split: keys up to 59 play square 2 at a duty of 50 %, and the others the sample.
// Program 6 is a drum kit whose table reaches the key split's key table at key 50. From there, its keys read the key
// table and the voice group as voices, and some of those bytes read as square 1. Program 7 is a drum kit whose table
// starts at the pattern. Its key 0 reads the pattern as square 1. Without `strays`, the voice group and the song leave
// programs 6 and 7 out.
struct GbaCart
{
    std::vector<uint8_t> bytes;
    uint32_t voices = 0;
    uint32_t sample = 0;
    uint32_t pattern = 0;
    uint32_t header = 0;
    uint32_t key_table = 0;
};

inline GbaCart MakeGbaCart(bool strays = true)
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    namespace mp2k = ntdlib::mp2k;
    using mp2k_cart::Cart;
    using enum mp2k::Command;

    Cart cart;
    GbaCart made;
    std::vector<int8_t> saw(64);
    for (std::size_t i = 0; i < saw.size(); ++i)
    {
        saw[i] = static_cast<int8_t>(static_cast<int>(i) * 4 - 128);
    }
    made.sample = cart.Wave(saw, 13379, 0);
    made.pattern =
        cart.Place({0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10});
    const uint32_t drums = cart.VoiceGroup(
        {{36, Cart::Voice(4, 0, 60, {0, 1, 8, 2}, 0xA0)}, {38, Cart::Voice(mp2k::kVoiceFixed, made.sample, 60)}});
    const uint32_t split =
        cart.VoiceGroup({{0, Cart::Voice(2, 2, 60, {0, 0, 15, 0})}, {1, Cart::Voice(0, made.sample, 60)}});
    const std::vector<uint8_t> key_split = cart.KeySplit(split, 59);
    made.key_table = xsflib::LoadLe32(&key_split[8]);
    std::map<int, std::vector<uint8_t>> voices = {{0, Cart::Voice(0, made.sample, 60, {255, 250, 200, 165})},
                                                  {1, Cart::Voice(1, 2, 60, {1, 2, 9, 3}, 0x26, 100)},
                                                  {2, Cart::Voice(3, made.pattern, 60, {0, 0, 15, 0})},
                                                  {3, Cart::Voice(mp2k::kVoiceDrumKit, drums)},
                                                  {4, key_split},
                                                  {5, Cart::Voice(3, made.pattern, 72, {0, 0, 15, 0})}};
    if (strays)
    {
        voices.emplace(6, Cart::Voice(mp2k::kVoiceDrumKit, made.key_table + 60 - 12 * 50));
        voices.emplace(7, Cart::Voice(mp2k::kVoiceDrumKit, made.pattern));
    }
    made.voices = cart.VoiceGroup(voices);
    mp2k_cart::Track track;
    track.Cmd(kCmdTempo, 75);
    for (int program = 0; program < static_cast<int>(voices.size()); ++program)
    {
        track.Cmd(kCmdVoice, program).Note(24, program == 3 ? 36 : 60, 100).Wait(24);
    }
    track.Fine();
    made.header = cart.Song({track}, made.voices);

    const ntdlib::agb::Rom rom = cart.ToRom();
    made.bytes = hokuto::testing::RomBytes(rom);
    return made;
}

} // namespace
