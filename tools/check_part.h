// SPDX-License-Identifier: MIT

// The part of the items that a run of a check tool checks, as the option `--part k/n` sets it. A long check splits its
// items into n parts, and each run checks one of them.

#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace hokuto::tools
{

// The part of the items that a run checks: part `k` of `n`, counted from 1.
struct Part
{
    uint32_t k = 1;
    uint32_t n = 1;
};

// The part that `text` names, such as "3/8", or nothing for text that names no part.
inline std::optional<Part> PartOf(std::string_view text)
{
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos)
    {
        return std::nullopt;
    }

    Part part;
    const std::string_view k = text.substr(0, slash);
    const std::string_view n = text.substr(slash + 1);
    const std::from_chars_result k_read = std::from_chars(k.data(), k.data() + k.size(), part.k);
    const std::from_chars_result n_read = std::from_chars(n.data(), n.data() + n.size(), part.n);
    if (k_read.ec != std::errc() || n_read.ec != std::errc() || k_read.ptr != k.data() + k.size() ||
        n_read.ptr != n.data() + n.size() || part.k == 0 || part.k > part.n)
    {
        return std::nullopt;
    }

    return part;
}

} // namespace hokuto::tools
