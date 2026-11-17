// SPDX-License-Identifier: MIT

// The work of an item's edit away from the program's thread.

#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "format/file.h"
#include "song/played.h"

namespace hokuto::app
{

// An item's edit while its work runs. Laying a bank out from its model joins the whole archive several times
// (edit::WithBankModel), and that takes seconds on a large archive. The work runs on a thread of its own, and the
// window draws on meanwhile. The edit becomes an action in the history once its work is done (App::TakeItemEdit).
class ItemEdit
{
public:
    // Builds the new version from a version and what plays it. Throws with a message for the user.
    using Make = std::function<format::File(const format::File&, const song::Played&)>;

    // What the work made: the new version, or why the edit can't be made.
    struct Outcome
    {
        std::optional<format::File> file;
        std::string error;
    };

    // Whether an edit is under way.
    bool Waiting() const
    {
        return make_ != nullptr;
    }

    // The version that the work of the edit under way started from, or nothing.
    const std::shared_ptr<const format::File>& From() const
    {
        return from_;
    }

    // Starts `make` on a thread of its own, from `version` and from `played`, which plays it. The work keeps both for
    // as long as it takes. An edit under way is dropped first.
    void Start(Make make, std::shared_ptr<const format::File> version, song::Played played)
    {
        Drop();
        make_ = std::move(make);
        from_ = std::move(version);
        work_ = std::async(std::launch::async, [make = make_, from = from_, played = std::move(played)]
                           { return Run(make, *from, played); });
    }

    // The outcome of the edit under way as an edit of `version`, which `played` plays. Nothing while the work runs,
    // and nothing without an edit. Another edit can enter the history while the work runs, such as an effect's. The
    // work then started from an older version than `version`. It runs again from `version`, and Take gives nothing
    // until that's done. With `wait`, Take waits for the work instead, and makes the edit of a newer version itself.
    std::optional<Outcome> Take(const std::shared_ptr<const format::File>& version, const song::Played& played,
                                bool wait = false)
    {
        std::erase_if(dropped_, IsDone);
        if (!Waiting() || (!wait && !IsDone(work_)))
        {
            return std::nullopt;
        }

        Outcome outcome = work_.get();
        if (from_ != version && !wait)
        {
            Start(std::move(make_), version, played);
            return std::nullopt;
        }
        if (from_ != version)
        {
            outcome = Run(make_, *version, played);
        }
        make_ = nullptr;
        from_.reset();

        return outcome;
    }

    // Drops the edit under way. Its work goes on to its end on its thread: the future of std::async would wait for it
    // in its destructor. The dropped work goes in the first Take or Drop after its end.
    void Drop()
    {
        std::erase_if(dropped_, IsDone);
        if (work_.valid() && !IsDone(work_))
        {
            dropped_.push_back(std::move(work_));
        }
        work_ = {};
        make_ = nullptr;
        from_.reset();
    }

private:
    static bool IsDone(const std::future<Outcome>& work)
    {
        return work.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    // What `make` makes of `version`, or the message of the exception that it throws.
    static Outcome Run(const Make& make, const format::File& version, const song::Played& played)
    {
        Outcome outcome;
        try
        {
            outcome.file = make(version, played);
        }
        catch (const std::exception& error)
        {
            outcome.error = error.what();
        }

        return outcome;
    }

    Make make_;
    std::shared_ptr<const format::File> from_;
    std::future<Outcome> work_;
    std::vector<std::future<Outcome>> dropped_;
};

} // namespace hokuto::app
