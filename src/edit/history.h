// SPDX-License-Identifier: MIT

// The versions of a Hokuto file that its edits make, for undo and redo. Every edit goes through the history as an
// action: its name, its details, when it happened, and the file after it. Undo and redo switch versions. No action has
// an inverse. The versions share the parts of the file that their edits leave alone (format::Shared). The Undo History
// zone lists the actions.
//
// A file can contain more than one archive (format/project.h). The history then keeps the project, and one archive is
// in front. An edit changes the archive in front, and the action stores every archive as it was after the edit. An undo
// or a redo brings the archive that the action changed to the front. With one archive, the project is that archive.

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "format/file.h"
#include "format/project.h"

namespace hokuto::edit
{

// An edit in the history.
struct Action
{
    std::string name;    // what the edit did, such as Remove Effect
    std::string details; // what it did it to, such as FX A: Ins. 1 - I3DL2 Reverb
    std::chrono::system_clock::time_point time;
    std::shared_ptr<const format::File> file; // the file after the edit: the archive in front

    // The archives after the edit, and the archive that it changed. History::Do sets them.
    std::shared_ptr<const format::Project> project{};
    std::size_t archive = 0;
};

// The actions of one file, and how many of them are done.
class History
{
public:
    // A history that starts from `original`: the file as it was opened, imported or made. The project's archive in
    // front comes to the front.
    explicit History(std::shared_ptr<const format::File> original);
    explicit History(std::shared_ptr<const format::Project> original);

    // The archive in front after the actions that are done, and the same archive before any action.
    const std::shared_ptr<const format::File>& Current() const;
    const std::shared_ptr<const format::File>& Original() const;

    // The archives after the actions that are done, with the archive in front. A save writes it.
    format::Project CurrentProject() const;

    // The archives as they were before any action.
    const format::Project& OriginalProject() const;

    // The number of archives, and the archive in front. SetFront brings archive `archive` to the front without an
    // action.
    std::size_t Archives() const;
    std::size_t Front() const;
    void SetFront(std::size_t archive);

    // Does `action` after the actions that are done. Its file is the archive in front after the edit. The other
    // archives take the bytes that the edit changed (format::WithArchive). The actions that undo stepped back over go.
    // Redo can't reach them any more. Throws std::invalid_argument as WithArchive does, and then does nothing.
    void Do(Action action);

    // The actions, oldest first, and how many of them are done. The rest have been undone.
    std::span<const Action> Actions() const;
    std::size_t Done() const;

    bool CanUndo() const;
    bool CanRedo() const;

    // Steps back over the last action that's done, or forward over the next one. Each does nothing at the end of the
    // history.
    void Undo();
    void Redo();

    // Drops the actions that undo stepped back over. A new action drops them too. Redo can't reach them any more.
    void Forget();

    // Moves to the file after the first `done` actions. A number past the last action means the last action.
    void GoTo(std::size_t done);

    // The current file becomes the saved one. The history is modified while its current file isn't the saved one. At
    // first, the original file counts as saved.
    void MarkSaved();
    bool Modified() const;

private:
    // The archives after the actions that are done.
    const std::shared_ptr<const format::Project>& DoneProject() const;

    // Points current_ and original_front_ at the archive in front. A version keeps the file that its action gave, or
    // that the history started from, as it was: the program tells versions apart by their places in memory.
    void Refresh();

    std::shared_ptr<const format::Project> original_;
    // The file that the history started from, if it started from one.
    std::shared_ptr<const format::File> original_file_;
    std::vector<Action> actions_;
    std::size_t done_ = 0;
    std::size_t front_ = 0;
    std::shared_ptr<const format::Project> saved_;
    std::shared_ptr<const format::File> current_;
    std::shared_ptr<const format::File> original_front_;
};

} // namespace hokuto::edit
