// SPDX-License-Identifier: MIT

// The numbers, names and records of the binary form, little-endian, for the readers and writers of its chunks. A read
// past the end of a chunk throws xsflib::FormatError for a damaged file.

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace hokuto::format::binary
{

// Little-endian numbers and bytes, appended.
class Out
{
public:
    void U8(uint8_t value)
    {
        bytes_.push_back(value);
    }

    void U16(uint16_t value)
    {
        std::array<uint8_t, 2> b{};
        xsflib::StoreLe16(b.data(), value);
        bytes_.insert(bytes_.end(), b.begin(), b.end());
    }

    void U32(uint32_t value)
    {
        std::array<uint8_t, 4> b{};
        xsflib::StoreLe32(b.data(), value);
        bytes_.insert(bytes_.end(), b.begin(), b.end());
    }

    void U64(uint64_t value)
    {
        std::array<uint8_t, 8> b{};
        xsflib::StoreLe64(b.data(), value);
        bytes_.insert(bytes_.end(), b.begin(), b.end());
    }

    void I64(int64_t value)
    {
        U64(static_cast<uint64_t>(value));
    }

    // A float as IEEE 754's 32 bits.
    void F32(float value)
    {
        U32(std::bit_cast<uint32_t>(value));
    }

    void Zeros(std::size_t count)
    {
        bytes_.resize(bytes_.size() + count);
    }

    void Bytes(std::span<const uint8_t> bytes)
    {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    // A count or a size that the format stores in 32 bits.
    void Count(std::size_t count, const std::string& what)
    {
        if (count > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("too many " + what + " for a .hok file");
        }

        U32(static_cast<uint32_t>(count));
    }

    // A name: its length in bytes, then its bytes.
    void Name(std::string_view name, const std::string& what)
    {
        Count(name.size(), what);
        Bytes(std::span(reinterpret_cast<const uint8_t*>(name.data()), name.size()));
    }

    // Room for a 64-bit size that Patch64 fills in.
    std::size_t Reserve64()
    {
        const std::size_t at = bytes_.size();
        Zeros(8);

        return at;
    }

    void Patch64(std::size_t at, uint64_t value)
    {
        xsflib::StoreLe64(&bytes_[at], value);
    }

    std::size_t Size() const
    {
        return bytes_.size();
    }

    std::vector<uint8_t> Take()
    {
        return std::move(bytes_);
    }

private:
    std::vector<uint8_t> bytes_;
};

// Little-endian numbers and bytes, read in order. A read past the end throws FormatError for a damaged file.
class In
{
public:
    In(std::span<const uint8_t> bytes, std::string what) : bytes_(bytes), what_(std::move(what))
    {
    }

    uint8_t U8()
    {
        return Take(1)[0];
    }

    uint16_t U16()
    {
        return xsflib::LoadLe16(Take(2).data());
    }

    uint32_t U32()
    {
        return xsflib::LoadLe32(Take(4).data());
    }

    uint64_t U64()
    {
        return xsflib::LoadLe64(Take(8).data());
    }

    int64_t I64()
    {
        return static_cast<int64_t>(U64());
    }

    float F32()
    {
        return std::bit_cast<float>(U32());
    }

    // A name that Out::Name wrote.
    std::string Name()
    {
        const std::span<const uint8_t> name = Take(U32());
        return std::string(reinterpret_cast<const char*>(name.data()), name.size());
    }

    std::span<const uint8_t> Take(uint64_t count)
    {
        if (count > bytes_.size() - at_)
        {
            throw xsflib::FormatError("The file is damaged: " + what_ + " ends early.");
        }

        const std::span<const uint8_t> taken = bytes_.subspan(at_, static_cast<std::size_t>(count));
        at_ += static_cast<std::size_t>(count);

        return taken;
    }

    // A record that starts with its 32-bit size, and the bytes after the size.
    In Record(const std::string& what)
    {
        const uint32_t size = U32();
        return In(Take(size), what);
    }

    std::size_t Left() const
    {
        return bytes_.size() - at_;
    }

private:
    std::span<const uint8_t> bytes_;
    std::size_t at_ = 0;
    std::string what_;
};

// Throws xsflib::FormatError for a damaged file, with `why` in the message.
[[noreturn]] inline void Damaged(const std::string& why)
{
    throw xsflib::FormatError("The file is damaged: " + why + ".");
}

// Throws xsflib::FormatError for a file that holds `what`. A later version of Hokuto made such a file.
[[noreturn]] inline void Newer(const std::string& what)
{
    throw xsflib::FormatError("The file holds " + what +
                              " that this version of Hokuto doesn't know. A later version made it.");
}

} // namespace hokuto::format::binary
