// SPDX-License-Identifier: MIT

// The comparison of the instruments of two banks of the 3DS, the Wii U or the Switch, as ntdlib reads them. The checks
// of copies of instruments in hokuto_check_layout and hokuto_check_replace use it.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/sar/bank_file.h"

namespace hokuto::tools
{

// Whether region `x` of a bank whose waves are `x_waves` and region `y` of a bank whose waves are `y_waves` hold the
// same values, and waves that name the same wave archive and wave.
inline bool SameRegion(const std::vector<ntdlib::sar::WaveId>& x_waves, const ntdlib::sar::VelocityRegion& x,
                       const std::vector<ntdlib::sar::WaveId>& y_waves, const ntdlib::sar::VelocityRegion& y)
{
    if (x.wave_id_index >= x_waves.size() || y.wave_id_index >= y_waves.size())
    {
        return false;
    }

    const ntdlib::sar::WaveId& x_wave = x_waves[x.wave_id_index];
    const ntdlib::sar::WaveId& y_wave = y_waves[y.wave_id_index];

    const auto adshr = [](const ntdlib::sar::Adshr& a)
    {
        return std::array{a.attack, a.decay, a.sustain, a.hold, a.release};
    };

    return x_wave.wave_archive_item == y_wave.wave_archive_item && x_wave.index == y_wave.index &&
           x.original_key == y.original_key && x.volume == y.volume && x.pan == y.pan &&
           std::bit_cast<uint32_t>(x.pitch) == std::bit_cast<uint32_t>(y.pitch) &&
           x.ignore_note_off == y.ignore_note_off && x.key_group == y.key_group && x.interpolation == y.interpolation &&
           adshr(x.adshr) == adshr(y.adshr);
}

// Whether instrument `x` of bank `a` and instrument `y` of bank `b` hold the same regions, by SameRegion. Two missing
// instruments are the same.
inline bool SameInstrument(const ntdlib::sar::Bank& a, const std::optional<ntdlib::sar::Instrument>& x,
                           const ntdlib::sar::Bank& b, const std::optional<ntdlib::sar::Instrument>& y)
{
    if (!x || !y)
    {
        return !x && !y;
    }

    const auto same_velocity = [&a, &b](const ntdlib::sar::KeyRegion::Vel& v, const ntdlib::sar::KeyRegion::Vel& w)
    {
        return v.lo == w.lo && v.hi == w.hi && v.region.has_value() == w.region.has_value() &&
               (!v.region || SameRegion(a.waves, *v.region, b.waves, *w.region));
    };
    const auto same_key = [&same_velocity](const ntdlib::sar::KeyRegion& k, const ntdlib::sar::KeyRegion& l)
    {
        return k.lo == l.lo && k.hi == l.hi && std::ranges::equal(k.velocities, l.velocities, same_velocity);
    };

    return std::ranges::equal(x->keys, y->keys, same_key);
}

} // namespace hokuto::tools
