// SPDX-License-Identifier: MIT

// What the item files of song/items.h share. Only items.cpp and items_wii.cpp include it.

#pragma once

#include <string>
#include <string_view>

#include "ntdlib/wave/decode.h"

namespace hokuto::song::items_parts
{

// The sentence that starts what the program says of a file that runs past the end of an archive that is cut short.
inline constexpr std::string_view kCutShort = "The archive is cut short. ";

// A wave sound's wave at the rate of its pitch (items.cpp).
ntdlib::wave::Pcm AtPitch(ntdlib::wave::Pcm pcm, float pitch, const std::string& name);

} // namespace hokuto::song::items_parts
