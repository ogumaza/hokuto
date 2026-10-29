// SPDX-License-Identifier: MIT

#include "edit/history.h"

#include <algorithm>
#include <utility>

namespace hokuto::edit
{

History::History(std::shared_ptr<const format::File> original) : original_(std::move(original)), saved_(original_)
{
}

const std::shared_ptr<const format::File>& History::Current() const
{
    return done_ == 0 ? original_ : actions_[done_ - 1].file;
}

const std::shared_ptr<const format::File>& History::Original() const
{
    return original_;
}

void History::Do(Action action)
{
    actions_.resize(done_);
    actions_.push_back(std::move(action));
    done_ = actions_.size();
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
        done_--;
    }
}

void History::Redo()
{
    if (CanRedo())
    {
        done_++;
    }
}

void History::Forget()
{
    actions_.resize(done_);
}

void History::GoTo(std::size_t done)
{
    done_ = std::min(done, actions_.size());
}

void History::MarkSaved()
{
    saved_ = Current();
}

bool History::Modified() const
{
    return Current() != saved_;
}

} // namespace hokuto::edit
