// SPDX-License-Identifier: MIT

// The versions of a Hokuto file that its edits make, for undo and redo. Every edit goes through the history as an
// action: its name, its details, when it happened, and the file after it. Undo and redo switch versions. No action has
// an inverse. The versions share the parts of the file that their edits leave alone (format::Shared). The Undo History
// zone lists the actions.

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "format/file.h"

namespace hokuto::edit
{

// An edit in the history.
struct Action
{
    std::string name;    // what the edit did, such as Remove Effect
    std::string details; // what it did it to, such as FX A: Ins. 1 - I3DL2 Reverb
    std::chrono::system_clock::time_point time;
    std::shared_ptr<const format::File> file; // the file after the edit
};

class History
{
public:
    // A history that starts from `original`: the file as it was opened, imported or made.
    explicit History(std::shared_ptr<const format::File> original);

    // The file after the actions that are done, and the file before any action.
    const std::shared_ptr<const format::File>& Current() const;
    const std::shared_ptr<const format::File>& Original() const;

    // Does `action` after the actions that are done. The actions that undo stepped back over go. Redo can't reach them
    // any more.
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
    std::shared_ptr<const format::File> original_;
    std::vector<Action> actions_;
    std::size_t done_ = 0;
    std::shared_ptr<const format::File> saved_;
};

} // namespace hokuto::edit
