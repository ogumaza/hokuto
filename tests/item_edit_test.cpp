// SPDX-License-Identifier: MIT

// The work of an item's edit away from the program's thread: a call that asks for the outcome never waits for the
// work unless it says so, the work runs again from a newer version, and a dropped edit lets go at once.

#include "app/item_edit.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

#include "xsflib/testing/check.h"

#include "format/file.h"
#include "song/played.h"

namespace hokuto
{
namespace
{

using app::ItemEdit;

// A version with `volume` as the gain of its first aux bus.
std::shared_ptr<const format::File> VersionWith(float volume)
{
    format::File file;
    file.aux_volumes[0] = volume;
    return std::make_shared<const format::File>(std::move(file));
}

// An edit that sets the gain of the second aux bus to 0.5, counts its runs in `runs`, and waits for `go` first.
ItemEdit::Make EditThatWaits(std::shared_ptr<std::atomic<bool>> go, std::shared_ptr<std::atomic<int>> runs)
{
    return [go, runs](const format::File& file, const song::Played&)
    {
        while (!go->load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++*runs;
        format::File edited = file;
        edited.aux_volumes[1] = 0.5f;
        return edited;
    };
}

// The outcome of `edit` for `version`, asked for as the program asks in each frame, for 10 s at most.
std::optional<ItemEdit::Outcome> TakeInFrames(ItemEdit& edit, const std::shared_ptr<const format::File>& version)
{
    for (int frame = 0; frame < 10000; ++frame)
    {
        if (std::optional<ItemEdit::Outcome> outcome = edit.Take(version, song::Played()))
        {
            return outcome;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return std::nullopt;
}

// While the work runs, a frame gets nothing and doesn't wait for it. The window draws on. Once the work is done, the
// frame gets the new version, and the edit is over.
void TestFramesDontWait()
{
    const auto go = std::make_shared<std::atomic<bool>>(false);
    const auto runs = std::make_shared<std::atomic<int>>(0);
    const std::shared_ptr<const format::File> version = VersionWith(0.25f);
    ItemEdit edit;
    XSFLIB_CHECK(!edit.Waiting());
    XSFLIB_CHECK(!edit.Take(version, song::Played()));

    edit.Start(EditThatWaits(go, runs), version, song::Played());
    XSFLIB_CHECK(edit.Waiting());
    XSFLIB_CHECK(edit.From() == version);
    for (int frame = 0; frame < 20; ++frame)
    {
        XSFLIB_CHECK(!edit.Take(version, song::Played()));
        XSFLIB_CHECK(edit.Waiting());
    }
    XSFLIB_CHECK(runs->load() == 0);

    go->store(true);
    const std::optional<ItemEdit::Outcome> outcome = TakeInFrames(edit, version);
    XSFLIB_CHECK(outcome && outcome->file && outcome->error.empty());
    if (outcome && outcome->file)
    {
        XSFLIB_CHECK(outcome->file->aux_volumes[0] == 0.25f && outcome->file->aux_volumes[1] == 0.5f);
    }
    XSFLIB_CHECK(runs->load() == 1);
    XSFLIB_CHECK(!edit.Waiting());
    XSFLIB_CHECK(!edit.Take(version, song::Played()));
}

// Another edit enters the history while the work runs. The work then runs again from that edit's version, and the
// new version keeps the other edit's change.
void TestRunsAgainFromNewerVersion()
{
    const auto go = std::make_shared<std::atomic<bool>>(true);
    const auto runs = std::make_shared<std::atomic<int>>(0);
    const std::shared_ptr<const format::File> version = VersionWith(0.25f);
    const std::shared_ptr<const format::File> newer = VersionWith(0.75f);
    ItemEdit edit;
    edit.Start(EditThatWaits(go, runs), version, song::Played());

    const std::optional<ItemEdit::Outcome> outcome = TakeInFrames(edit, newer);
    XSFLIB_CHECK(outcome && outcome->file);
    if (outcome && outcome->file)
    {
        XSFLIB_CHECK(outcome->file->aux_volumes[0] == 0.75f && outcome->file->aux_volumes[1] == 0.5f);
    }
    XSFLIB_CHECK(runs->load() == 2);
    XSFLIB_CHECK(!edit.Waiting());
}

// A call that waits gets the outcome at once: of the version that the work started from, or of a newer one, which
// the call makes itself.
void TestWaitingCall()
{
    const auto go = std::make_shared<std::atomic<bool>>(true);
    const auto runs = std::make_shared<std::atomic<int>>(0);
    const std::shared_ptr<const format::File> version = VersionWith(0.25f);
    const std::shared_ptr<const format::File> newer = VersionWith(0.75f);
    ItemEdit edit;
    edit.Start(EditThatWaits(go, runs), version, song::Played());
    std::optional<ItemEdit::Outcome> outcome = edit.Take(version, song::Played(), true);
    XSFLIB_CHECK(outcome && outcome->file && outcome->file->aux_volumes[0] == 0.25f);
    XSFLIB_CHECK(runs->load() == 1);
    XSFLIB_CHECK(!edit.Waiting());

    edit.Start(EditThatWaits(go, runs), version, song::Played());
    outcome = edit.Take(newer, song::Played(), true);
    XSFLIB_CHECK(outcome && outcome->file && outcome->file->aux_volumes[0] == 0.75f &&
                 outcome->file->aux_volumes[1] == 0.5f);
    XSFLIB_CHECK(runs->load() == 3);
    XSFLIB_CHECK(!edit.Waiting());
}

// An edit that can't be made gives its reason and no version.
void TestRefusal()
{
    const std::shared_ptr<const format::File> version = VersionWith(0.25f);
    ItemEdit edit;
    edit.Start([](const format::File&, const song::Played&) -> format::File
               { throw std::invalid_argument("Can't write the bank file."); }, version, song::Played());
    const std::optional<ItemEdit::Outcome> outcome = TakeInFrames(edit, version);
    XSFLIB_CHECK(outcome && !outcome->file && outcome->error == "Can't write the bank file.");
    XSFLIB_CHECK(!edit.Waiting());
}

// A dropped edit lets go at once, while its work still runs, and gives no outcome. A new edit can start meanwhile.
void TestDrop()
{
    const auto go = std::make_shared<std::atomic<bool>>(false);
    const auto runs = std::make_shared<std::atomic<int>>(0);
    const std::shared_ptr<const format::File> version = VersionWith(0.25f);
    {
        ItemEdit edit;
        edit.Start(EditThatWaits(go, runs), version, song::Played());
        edit.Drop();
        XSFLIB_CHECK(!edit.Waiting());
        XSFLIB_CHECK(!edit.From());
        XSFLIB_CHECK(!edit.Take(version, song::Played()));
        XSFLIB_CHECK(!edit.Take(version, song::Played(), true));
        XSFLIB_CHECK(runs->load() == 0);

        // The second edit's work runs beside the dropped one.
        const auto go_now = std::make_shared<std::atomic<bool>>(true);
        edit.Start(EditThatWaits(go_now, runs), version, song::Played());
        const std::optional<ItemEdit::Outcome> outcome = TakeInFrames(edit, version);
        XSFLIB_CHECK(outcome && outcome->file);
        XSFLIB_CHECK(runs->load() == 1);

        // The dropped work ends before the edit goes: its future waits for it.
        go->store(true);
    }
    XSFLIB_CHECK(runs->load() == 2);
}

} // namespace
} // namespace hokuto

int main()
{
    return xsflib::testing::RunTest("item_edit",
                                    []
                                    {
                                        hokuto::TestFramesDontWait();
                                        hokuto::TestRunsAgainFromNewerVersion();
                                        hokuto::TestWaitingCall();
                                        hokuto::TestRefusal();
                                        hokuto::TestDrop();
                                    });
}
