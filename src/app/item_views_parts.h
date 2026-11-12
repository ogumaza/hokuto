// SPDX-License-Identifier: MIT

// The text helpers that the views of the consoles share (item_views.cpp). The names are internal to the views.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace hokuto::app::item_views_parts
{

// The highest key and the highest velocity.
inline constexpr int kHighest = 127;

// `names` as a sentence lists them: "A", "A and B", or "A, B and C". A list of more than kListedNames ends with the
// number of the others.
std::string ListOf(const std::vector<std::string>& names);

} // namespace hokuto::app::item_views_parts
