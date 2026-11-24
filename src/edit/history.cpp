// SPDX-License-Identifier: MIT

#include "edit/history.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

#include "format/project.h"

namespace hokuto::edit
{

History::History(std::shared_ptr<const format::File> original)
    : original_(std::make_shared<const format::Project>(format::ProjectOf(*original))),
      original_file_(std::move(original)), saved_(original_)
{
    Refresh();
}

History::History(std::shared_ptr<const format::Project> original)
    : original_(std::move(original)), front_(original_->front), saved_(original_)
{
    Refresh();
}

const std::shared_ptr<const format::File>& History::Current() const
{
    return current_;
}

const std::shared_ptr<const format::File>& History::Original() const
{
    return original_front_;
}

format::Project History::CurrentProject() const
{
    format::Project project = *DoneProject();
    project.front = front_;

    return project;
}

const format::Project& History::OriginalProject() const
{
    return *original_;
}

std::size_t History::Archives() const
{
    return original_->archives.size();
}

std::size_t History::Front() const
{
    return front_;
}

void History::SetFront(std::size_t archive)
{
    if (archive >= Archives())
    {
        throw std::invalid_argument("The file has no archive " + std::to_string(archive) + ".");
    }

    front_ = archive;
    Refresh();
}

void History::Do(Action action)
{
    // The archives after the edit: the archive in front from the action, the others with the bytes that it changed.
    action.archive = front_;
    action.project =
        std::make_shared<const format::Project>(format::WithArchive(*DoneProject(), front_, *action.file));

    // WithArchive can give the archive in front another floor of its free space (format::WithFreeFloors). The action
    // then keeps the project's copy. Current() and CurrentProject() give the same archive.
    const format::File& placed = action.project->archives[front_];
    if (placed != *action.file)
    {
        action.file = std::shared_ptr<const format::File>(action.project, &placed);
    }

    actions_.resize(done_);
    actions_.push_back(std::move(action));
    done_ = actions_.size();
    Refresh();
}

std::span<const Action> History::Actions() const
{
    return actions_;
}

std::size_t History::Done() const
{
    return done_;
}

bool History::CanUndo() const
{
    return done_ > 0;
}

bool History::CanRedo() const
{
    return done_ < actions_.size();
}

void History::Undo()
{
    if (CanUndo())
    {
        front_ = actions_[done_ - 1].archive;
        done_--;
        Refresh();
    }
}

void History::Redo()
{
    if (CanRedo())
    {
        done_++;
        front_ = actions_[done_ - 1].archive;
        Refresh();
    }
}

void History::Forget()
{
    actions_.resize(done_);
}

void History::GoTo(std::size_t done)
{
    // The archive in front is the one that Undo or Redo, repeated, would bring to the front: the archive of the last
    // action undone, or of the last action redone.
    const std::size_t target = std::min(done, actions_.size());
    if (target < done_)
    {
        front_ = actions_[target].archive;
    }
    else if (target > done_)
    {
        front_ = actions_[target - 1].archive;
    }

    done_ = target;
    Refresh();
}

void History::MarkSaved()
{
    saved_ = DoneProject();
}

bool History::Modified() const
{
    return DoneProject() != saved_;
}

const std::shared_ptr<const format::Project>& History::DoneProject() const
{
    return done_ == 0 ? original_ : actions_[done_ - 1].project;
}

void History::Refresh()
{
    const std::shared_ptr<const format::Project>& done = DoneProject();
    const std::shared_ptr<const format::File>& kept = done_ == 0 ? original_file_ : actions_[done_ - 1].file;
    const std::size_t kept_archive = done_ == 0 ? 0 : actions_[done_ - 1].archive;
    current_ = kept && kept_archive == front_ ? kept
                                              : std::shared_ptr<const format::File>(done, &done->archives[front_]);
    original_front_ = original_file_ && front_ == 0
                          ? original_file_
                          : std::shared_ptr<const format::File>(original_, &original_->archives[front_]);
}

} // namespace hokuto::edit
