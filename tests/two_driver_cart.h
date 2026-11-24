// SPDX-License-Identifier: MIT

// A made-up GBA ROM with two drivers, for the tests of projects (format/project.h): agblib's test cartridge of Nintendo
// R&D2's driver with two sequences, and an MP2K test cartridge past it with two songs and a voice group
// (agblib/testing/two_drivers_cart.h). The ROM contains no game data.

#pragma once

#include <cstdint>
#include <vector>

#include "agblib/mp2k/song.h"
#include "agblib/testing/mp2k_cart.h"
#include "agblib/testing/rd2_cart.h"
#include "agblib/testing/two_drivers_cart.h"

namespace hokuto::testing
{

// Where the MP2K cartridge starts in the ROM.
inline constexpr uint32_t kTwoDriverMp2kOrigin = agblib::testing::two_drivers_cart::kMp2kOrigin;

// The ROM, and the ROM address of the MP2K songs' voice group. Program 0 of the group plays a sample.
struct TwoDriverRom
{
    std::vector<uint8_t> rom;
    uint32_t voices = 0;
};

inline TwoDriverRom TwoDriverCart()
{
    namespace mp2k_cart = agblib::testing::mp2k_cart;
    namespace rd2_cart = agblib::testing::rd2_cart;
    rd2_cart::Cart rd2;
    rd2.Sequences({{rd2_cart::TrackData().Wait(4).End()},
                   {rd2_cart::TrackData().Note(60, 24, 100).Wait(24).End(), rd2_cart::TrackData().End()}});

    mp2k_cart::Cart mp2k(mp2k_cart::kMode, {10, 3}, true, true, kTwoDriverMp2kOrigin);
    TwoDriverRom made;
    made.voices = mp2k.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, mp2k.Wave(mp2k_cart::Saw(), 13379, 0))}});
    mp2k_cart::Track first;
    first.Cmd(agblib::mp2k::kCmdVoice, 0).Note(24, 60, 100).Wait(24).Fine();
    mp2k_cart::Track second;
    second.Cmd(agblib::mp2k::kCmdVoice, 0).Note(12, 64, 90).Wait(12).Fine();
    mp2k.Song({first}, made.voices);
    mp2k.Song({second}, made.voices);
    made.rom = agblib::testing::two_drivers_cart::TwoDrivers(rd2, mp2k);

    return made;
}

} // namespace hokuto::testing
