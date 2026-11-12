// SPDX-License-Identifier: MIT

#include "edit/native_runs.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "ntdlib/sar/layout.h"

#include "format/file.h"

namespace hokuto::edit
{
namespace
{

// The field's bytes in a run. Join writes these 4 bytes over the run's bytes.
constexpr uint64_t kFieldBytes = 4;

} // namespace

std::variant<RunPlace, RunMiss> RunOf(const format::NativeArchive& native, std::span<const uint64_t> starts,
                                      uint64_t offset, uint64_t size)
{
    const std::vector<ntdlib::sar::Piece>& pieces = *native.pieces;
    if (starts.size() != pieces.size() + 1)
    {
        throw std::invalid_argument("the layout's starts don't match its pieces");
    }

    for (std::size_t p = 0; p < pieces.size(); ++p)
    {
        const uint64_t start = starts[p];
        if (offset >= starts[p + 1])
        {
            continue;
        }

        const auto* run = std::get_if<std::vector<uint8_t>>(&pieces[p]);
        if (run == nullptr || offset + size > starts[p + 1])
        {
            return RunMiss::kSequence;
        }

        for (const ntdlib::sar::Field& field : native.fields)
        {
            if (!field.at.item && !field.at.sequence && field.at.piece == p &&
                field.at.offset < offset - start + size && offset - start < field.at.offset + kFieldBytes)
            {
                return RunMiss::kField;
            }
        }

        return RunPlace{p, static_cast<std::size_t>(offset - start)};
    }

    return RunMiss::kPastEnd;
}

format::File WithRunBytes(const format::File& file, const RunPlace& place, std::span<const uint8_t> bytes)
{
    format::File result = file;
    const auto write = [&](std::vector<ntdlib::sar::Piece>& pieces)
    {
        auto& run = std::get<std::vector<uint8_t>>(pieces[place.piece]);
        std::ranges::copy(bytes, run.begin() + static_cast<std::ptrdiff_t>(place.offset));
    };
    result.native.Edit([&write](format::NativeArchive& native) { native.pieces.Edit(write); });

    return result;
}

namespace
{

// Throws std::invalid_argument with a message for the user that says why no run contains some bytes of `item`.
[[noreturn]] void ThrowMiss(RunMiss miss, const std::string& item)
{
    switch (miss)
    {
    case RunMiss::kSequence:
        throw std::invalid_argument(item + " shares bytes with a sequence. Hokuto can't change it here.");
    case RunMiss::kField:
        throw std::invalid_argument(item + " shares bytes with the archive tables. Hokuto can't change it.");
    case RunMiss::kPastEnd:
        break;
    }

    throw std::invalid_argument(item + " is past the end of the archive.");
}

} // namespace

format::File WithArchiveBytes(const format::File& file, std::span<const uint64_t> starts, uint64_t offset,
                              std::span<const uint8_t> bytes, const std::string& item)
{
    const std::variant<RunPlace, RunMiss> found = RunOf(*file.native, starts, offset, bytes.size());
    if (const auto* place = std::get_if<RunPlace>(&found))
    {
        return WithRunBytes(file, *place, bytes);
    }

    ThrowMiss(std::get<RunMiss>(found), item);
}

format::File WithArchiveBytes(const format::File& file, std::span<const uint64_t> starts,
                              std::span<const ArchiveBytes> changes, const std::string& item)
{
    std::vector<RunPlace> places;
    places.reserve(changes.size());
    for (const ArchiveBytes& change : changes)
    {
        const std::variant<RunPlace, RunMiss> found = RunOf(*file.native, starts, change.offset, change.bytes.size());
        if (const auto* place = std::get_if<RunPlace>(&found))
        {
            places.push_back(*place);
            continue;
        }

        ThrowMiss(std::get<RunMiss>(found), item);
    }

    format::File result = file;
    const auto write = [&](std::vector<ntdlib::sar::Piece>& pieces)
    {
        for (std::size_t i = 0; i < changes.size(); ++i)
        {
            auto& run = std::get<std::vector<uint8_t>>(pieces[places[i].piece]);
            std::ranges::copy(changes[i].bytes, run.begin() + static_cast<std::ptrdiff_t>(places[i].offset));
        }
    };
    result.native.Edit([&write](format::NativeArchive& native) { native.pieces.Edit(write); });

    return result;
}

} // namespace hokuto::edit
