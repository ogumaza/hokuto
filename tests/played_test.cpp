// SPDX-License-Identifier: MIT

// Tests the value that contains the open archive, ROM or set (song/played.h): each kind, its family and its engine
// against the table of the engines, and an empty value.

#include "song/played.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "ntdlib/rvl/snd/rsar_options.h"
#include "xsflib/testing/check.h"

#include "format/archive.h"
#include "format/engines.h"
#include "format/model.h"
#include "played_fixtures.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/krawall.h"

namespace
{

namespace format = hokuto::format;
namespace song = hokuto::song;

using format::Engine;
using format::Family;
using song::Played;

// Which of the six pointers in `played` is set: the index of the one that isn't empty, from 1, or 0 for none. Fails
// when more than one isn't empty.
int PointerIndex(const Played& played)
{
    const std::vector<bool> held = {played.Sar() != nullptr, played.Wii() != nullptr, played.Gba() != nullptr,
                                    played.Ds() != nullptr,  played.Dse() != nullptr, played.Driver() != nullptr};
    int index = 0;
    for (std::size_t i = 0; i < held.size(); ++i)
    {
        if (held[i])
        {
            XSFLIB_CHECK(index == 0);
            index = static_cast<int>(i) + 1;
        }
    }

    return index;
}

// `played` contains one value of the kind at `index`, of `engine`, in the family of the engine's row of kEngineFacts.
void CheckPlayed(const Played& played, int index, Engine engine)
{
    XSFLIB_CHECK(static_cast<bool>(played));
    XSFLIB_CHECK(PointerIndex(played) == index);
    XSFLIB_CHECK(played.Engine() == engine);
    XSFLIB_CHECK(played.Family() == format::FactsOf(engine).family);
    XSFLIB_CHECK(song::OnGbaOrDs(played) == (index >= 3));
    XSFLIB_CHECK(
        played.Visit([](const auto& value) { return !std::is_same_v<decltype(value), const std::monostate&>; }));
}

// The archive that a new file of `engine` exports, opened as a later console's archive.
std::shared_ptr<const ntdlib::sar::SoundArchive> NewArchive(Engine engine)
{
    return song::LoadArchive(format::ExportArchive(format::NewFile(engine)), "test");
}

// Each kind of archive, ROM or set returns its pointer alone, and the family and the engine of kEngineFacts.
void TestEachKind()
{
    for (const Engine engine : {Engine::kSwitch, Engine::kWiiU, Engine::k3ds})
    {
        CheckPlayed(NewArchive(engine), 1, engine);
    }

    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii = song::LoadWiiArchive(
        format::ExportArchive(format::NewFile(Engine::kWii)), "test.brsar", ntdlib::rvl::snd::kRsarOptions);
    CheckPlayed(wii, 2, Engine::kWii);
    CheckPlayed(std::make_shared<const song::GbaRom>(), 3, Engine::kGba);
    CheckPlayed(std::make_shared<const song::DsArchive>(), 4, Engine::kDs);
    CheckPlayed(std::make_shared<const song::DseSet>(), 5, Engine::kDse);
    CheckPlayed(hokuto::testing::KrawallMeasureRom(), 6, Engine::kKrawall);
}

// A value made by default or from a null pointer contains nothing: no pointer, and no family or engine.
void TestEmpty()
{
    for (const Played& played : {Played(), Played(std::shared_ptr<const song::DsArchive>()),
                                 Played(std::shared_ptr<const ntdlib::sar::SoundArchive>())})
    {
        XSFLIB_CHECK(!played);
        XSFLIB_CHECK(PointerIndex(played) == 0);
        XSFLIB_CHECK(!song::OnGbaOrDs(played));
        XSFLIB_CHECK(!xsflib::testing::ErrorOf<std::logic_error>([&] { played.Family(); }).empty());
        XSFLIB_CHECK(!xsflib::testing::ErrorOf<std::logic_error>([&] { played.Engine(); }).empty());
        XSFLIB_CHECK(
            played.Visit([](const auto& value) { return std::is_same_v<decltype(value), const std::monostate&>; }));
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("played",
                                    []
                                    {
                                        TestEachKind();
                                        TestEmpty();
                                    });
}
