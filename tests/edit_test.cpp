// SPDX-License-Identifier: MIT

// Tests the history of a file's versions (src/edit/history.h): undo and redo, a jump to any action, an action after an
// undo, the actions that Forget drops after an undo, and whether the file has changed since it was saved.

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "xsflib/testing/check.h"

#include "edit/history.h"
#include "format/file.h"

using hokuto::edit::Action;
using hokuto::edit::History;
using hokuto::format::File;

namespace
{

// A new version of a file. The history distinguishes versions by their place in memory, not by their values.
std::shared_ptr<const File> Version()
{
    return std::make_shared<const File>();
}

// An action named `name`, done to FX A's reverb now. `file` is the file after it.
Action Make(const std::string& name, std::shared_ptr<const File> file)
{
    return {name, "FX A: Ins. 1 - I3DL2 Reverb", std::chrono::system_clock::now(), std::move(file)};
}

// Undo and redo step through the versions, and each does nothing at its end.
void TestUndoRedo()
{
    const std::shared_ptr<const File> original = Version();
    const std::shared_ptr<const File> first = Version();
    const std::shared_ptr<const File> second = Version();
    History history(original);
    history.Do(Make("First", first));
    history.Do(Make("Second", second));

    history.Undo();
    const bool after_one = history.Current() == first && history.CanUndo() && history.CanRedo();
    history.Undo();
    history.Undo();
    const bool at_start = history.Current() == original && !history.CanUndo() && history.Done() == 0;
    history.Redo();
    history.Redo();
    history.Redo();
    const bool at_end = history.Current() == second && !history.CanRedo() && history.Done() == 2;

    XSFLIB_CHECK(after_one && at_start && at_end);
    XSFLIB_CHECK(history.Actions().size() == 2 && history.Actions()[0].name == "First");
}

// An action after an undo drops the actions that were undone.
void TestBranch()
{
    const std::shared_ptr<const File> third = Version();
    History history(Version());
    history.Do(Make("First", Version()));
    history.Do(Make("Second", Version()));

    history.Undo();
    history.Do(Make("Third", third));

    XSFLIB_CHECK(history.Actions().size() == 2 && history.Actions()[1].name == "Third");
    XSFLIB_CHECK(history.Current() == third && history.Done() == 2 && !history.CanRedo());
}

// Forget drops the actions that were undone, and keeps the others.
void TestForget()
{
    const std::shared_ptr<const File> first = Version();
    History history(Version());
    history.Do(Make("First", first));
    history.Do(Make("Second", Version()));

    history.Undo();
    history.Forget();
    const bool forgot = history.Actions().size() == 1 && history.Current() == first && !history.CanRedo();
    history.Forget();
    const bool kept = history.Actions().size() == 1 && history.Done() == 1 && history.CanUndo();

    XSFLIB_CHECK(forgot && kept);
}

// GoTo moves to the file after any number of actions. A number past the end means the end.
void TestGoTo()
{
    const std::shared_ptr<const File> original = Version();
    const std::shared_ptr<const File> first = Version();
    const std::shared_ptr<const File> second = Version();
    History history(original);
    history.Do(Make("First", first));
    history.Do(Make("Second", second));

    history.GoTo(0);
    const bool at_start = history.Current() == original && history.Done() == 0;
    history.GoTo(1);
    const bool at_first = history.Current() == first && history.CanUndo() && history.CanRedo();
    history.GoTo(9);
    const bool at_end = history.Current() == second && history.Done() == 2;

    XSFLIB_CHECK(at_start && at_first && at_end);
}

// The history is modified while its file isn't the saved one. Undo back to the saved file ends that. Once an action
// drops the saved file, no undo returns to it.
void TestModified()
{
    History history(Version());
    const bool fresh = !history.Modified();

    history.Do(Make("First", Version()));
    const bool edited = history.Modified();
    history.Undo();
    const bool undone = !history.Modified();
    history.Redo();
    history.MarkSaved();
    const bool saved = !history.Modified();
    history.Undo();
    const bool before_save = history.Modified();
    history.Do(Make("Second", Version()));
    history.Undo();
    const bool dropped = history.Modified();

    XSFLIB_CHECK(fresh && edited && undone && saved && before_save && dropped);
}

} // namespace

int main()
{
    TestUndoRedo();
    TestBranch();
    TestForget();
    TestGoTo();
    TestModified();

    return xsflib::testing::Summary("edit");
}
