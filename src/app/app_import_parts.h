// SPDX-License-Identifier: MIT

// The parts of the imports that their files share (app_import.cpp, app_import_items.cpp and app_import_waves.cpp). The
// names are internal to those files.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hokuto::app::import_parts
{

// The first name of a bank that Project > Add Bank… adds.
inline constexpr char kNewBankName[] = "BANK_NEW";

// Adds to `models` the index of each sound of `sounds` whose type is `type`, in order. With `of_type` false, it adds
// those of every other type instead.
template <typename Sounds, typename Type>
void AddSoundsOfType(std::vector<uint32_t>& models, const Sounds& sounds, Type type, bool of_type = true)
{
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        if ((sounds[i].type == type) == of_type)
        {
            models.push_back(static_cast<uint32_t>(i));
        }
    }
}

} // namespace hokuto::app::import_parts
