// SPDX-License-Identifier: MIT

#include "song/played.h"

#include <array>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

#include "song/driver.h"

namespace hokuto::song
{

namespace
{

// `pointer` as the value of a Played: nothing when it's null.
template <typename Value>
Played::Alternatives ValueOf(std::shared_ptr<const Value> pointer)
{
    if (!pointer)
    {
        return std::monostate{};
    }

    return pointer;
}

// The pointer of kind `Value` that `value` holds, or an empty one.
template <typename Value>
const std::shared_ptr<const Value>& PointerOf(const Played::Alternatives& value)
{
    static const std::shared_ptr<const Value> kNone;

    const auto* held = std::get_if<std::shared_ptr<const Value>>(&value);
    return held != nullptr ? *held : kNone;
}

// The family of each kind of Played::Alternatives after std::monostate, in the variant's order.
constexpr std::array<format::Family, 6> kFamilies = {
    format::Family::kNintendoWare,  format::Family::kNw4r, format::Family::kMp2k,
    format::Family::kNitroComposer, format::Family::kDse,  format::Family::kGbaDriver,
};
static_assert(std::variant_size_v<Played::Alternatives> == kFamilies.size() + 1, "one family for each kind");

// The engine of an archive of `flavor` of a later console. format::EngineOf returns the same engine. The song layer
// doesn't link the format layer. The row is therefore looked up here.
format::Engine EngineOfFlavor(ntdlib::sar::Flavor flavor)
{
    for (const format::EngineFacts& facts : format::kEngineFacts)
    {
        if (facts.flavor == flavor)
        {
            return facts.engine;
        }
    }

    return format::Engine::kSwitch;
}

} // namespace

Played::Played(std::shared_ptr<const ntdlib::sar::SoundArchive> archive) : value_(ValueOf(std::move(archive)))
{
}

Played::Played(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive) : value_(ValueOf(std::move(archive)))
{
}

Played::Played(std::shared_ptr<const GbaRom> rom) : value_(ValueOf(std::move(rom)))
{
}

Played::Played(std::shared_ptr<const DsArchive> archive) : value_(ValueOf(std::move(archive)))
{
}

Played::Played(std::shared_ptr<const DseSet> set) : value_(ValueOf(std::move(set)))
{
}

Played::Played(std::shared_ptr<const DriverRom> rom) : value_(ValueOf(std::move(rom)))
{
}

Played::operator bool() const
{
    return !std::holds_alternative<std::monostate>(value_);
}

format::Family Played::Family() const
{
    if (!*this)
    {
        throw std::logic_error("Played::Family requires an open archive, ROM or set");
    }

    return kFamilies[value_.index() - 1];
}

format::Engine Played::Engine() const
{
    if (const auto* archive = std::get_if<std::shared_ptr<const ntdlib::sar::SoundArchive>>(&value_))
    {
        return EngineOfFlavor((*archive)->ReadOptions().flavor);
    }
    if (const auto* rom = std::get_if<std::shared_ptr<const DriverRom>>(&value_))
    {
        return (*rom)->Rules().DriverEngine();
    }

    // Each of the other families has one engine.
    const format::Family family = Family();
    for (const format::EngineFacts& facts : format::kEngineFacts)
    {
        if (facts.family == family)
        {
            return facts.engine;
        }
    }

    throw std::logic_error("kEngineFacts has a row for each family");
}

const std::shared_ptr<const ntdlib::sar::SoundArchive>& Played::Sar() const
{
    return PointerOf<ntdlib::sar::SoundArchive>(value_);
}

const std::shared_ptr<const ntdlib::rsar::SoundArchive>& Played::Wii() const
{
    return PointerOf<ntdlib::rsar::SoundArchive>(value_);
}

const std::shared_ptr<const GbaRom>& Played::Gba() const
{
    return PointerOf<GbaRom>(value_);
}

const std::shared_ptr<const DsArchive>& Played::Ds() const
{
    return PointerOf<DsArchive>(value_);
}

const std::shared_ptr<const DseSet>& Played::Dse() const
{
    return PointerOf<DseSet>(value_);
}

const std::shared_ptr<const DriverRom>& Played::Driver() const
{
    return PointerOf<DriverRom>(value_);
}

bool OnGbaOrDs(const Played& played)
{
    if (!played)
    {
        return false;
    }

    const format::Family family = played.Family();
    return family != format::Family::kNintendoWare && family != format::Family::kNw4r;
}

} // namespace hokuto::song
