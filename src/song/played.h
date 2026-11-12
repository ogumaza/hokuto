// SPDX-License-Identifier: MIT

#pragma once

// The archive, the ROM or the set of sounds that plays: one value in place of six pointers held side by side. The sound
// archive of a later console or of the Wii, a GBA ROM with MusicPlayer2000, a DS archive, the DSE sounds of a DS image,
// or a GBA ROM with one of the other drivers. Played also returns the family and the engine of what it holds.

#include <concepts>
#include <memory>
#include <utility>
#include <variant>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "format/engines.h"

namespace hokuto::song
{

class DriverRom;
struct DsArchive;
struct DseSet;
struct GbaRom;

// One of the six kinds of archive, ROM or set, or nothing. A null pointer makes an empty value.
class Played
{
public:
    // The kinds that Played holds, in the order of format::Family's first six values.
    using Alternatives =
        std::variant<std::monostate, std::shared_ptr<const ntdlib::sar::SoundArchive>,
                     std::shared_ptr<const ntdlib::rsar::SoundArchive>, std::shared_ptr<const GbaRom>,
                     std::shared_ptr<const DsArchive>, std::shared_ptr<const DseSet>, std::shared_ptr<const DriverRom>>;

    // Nothing open.
    Played() = default;

    // `archive`, `rom` or `set` as the value. The constructors convert implicitly on purpose: a Played is one of these
    // pointers.
    Played(std::shared_ptr<const ntdlib::sar::SoundArchive> archive);
    Played(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive);
    Played(std::shared_ptr<const GbaRom> rom);
    Played(std::shared_ptr<const DsArchive> archive);
    Played(std::shared_ptr<const DseSet> set);
    Played(std::shared_ptr<const DriverRom> rom);

    // The ROM of a driver's class, such as KonamiRom, as the driver's ROM.
    template <std::derived_from<DriverRom> Rom>
    Played(std::shared_ptr<const Rom> rom) : Played(std::shared_ptr<const DriverRom>(std::move(rom)))
    {
    }

    // Whether an archive, a ROM or a set is open.
    explicit operator bool() const;

    // The family of what is open: kNintendoWare for an archive of a later console, kNw4r for an archive of the Wii, and
    // so on. Requires a value.
    format::Family Family() const;

    // The engine of what is open: the engine of a later console by the flavor of the archive, and the engine of a
    // driver by its rules. Requires a value.
    format::Engine Engine() const;

    // The pointer of each kind. Each one is empty unless Played holds that kind.
    const std::shared_ptr<const ntdlib::sar::SoundArchive>& Sar() const;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive>& Wii() const;
    const std::shared_ptr<const GbaRom>& Gba() const;
    const std::shared_ptr<const DsArchive>& Ds() const;
    const std::shared_ptr<const DseSet>& Dse() const;
    const std::shared_ptr<const DriverRom>& Driver() const;

    // `visitor` called with the pointer that Played holds, or with std::monostate when nothing is open.
    template <typename Visitor>
    decltype(auto) Visit(Visitor&& visitor) const
    {
        return std::visit(std::forward<Visitor>(visitor), value_);
    }

private:
    Alternatives value_;
};

// Whether `played` holds a ROM of the GBA or the sounds of the DS: the sounds of MusicPlayer2000, another GBA driver,
// NITRO-Composer or DSE. Returns false when nothing is open.
bool OnGbaOrDs(const Played& played);

} // namespace hokuto::song
