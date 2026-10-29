// SPDX-License-Identifier: MIT

// A value that copies share. A copy costs a pointer, however big the value. The value never changes. An edit makes a
// new value and puts it in the place of the old one. The versions of a Hokuto file share the parts that their edits
// leave alone: an edit of one sequence copies that sequence, and the other sequences and the native archive stay
// shared.

#pragma once

#include <memory>
#include <utility>

namespace hokuto::format
{

// A shared value of type T, or no value. Two Shared values are equal when their values are equal.
template <typename T>
class Shared
{
public:
    // No value.
    Shared() = default;

    // Takes `value`. The conversion isn't explicit. A T can stand wherever a Shared<T> goes. std::optional works the
    // same way.
    Shared(T value) : value_(std::make_shared<const T>(std::move(value)))
    {
    }

    explicit operator bool() const
    {
        return value_ != nullptr;
    }

    const T& operator*() const
    {
        return *value_;
    }

    const T* operator->() const
    {
        return value_.get();
    }

    // Whether this and `other` share one value, or both have none. Shared values that don't share can still be equal.
    bool SharesWith(const Shared& other) const
    {
        return value_ == other.value_;
    }

    // Copies the value, lets `change` change the copy, and puts the copy in place of the value. Other copies of this
    // Shared keep the old value. Without a value, `change` gets a value-initialised T.
    template <typename Change>
    void Edit(Change change)
    {
        T value = value_ ? *value_ : T{};
        change(value);
        value_ = std::make_shared<const T>(std::move(value));
    }

    friend bool operator==(const Shared& a, const Shared& b)
    {
        return a.value_ == b.value_ || (a.value_ && b.value_ && *a.value_ == *b.value_);
    }

private:
    std::shared_ptr<const T> value_;
};

} // namespace hokuto::format
