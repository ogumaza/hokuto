// SPDX-License-Identifier: MIT

// The parts of the export that its files share (app_export.cpp and app_export_midi.cpp). The names are internal to
// those files.

#pragma once

#include <cstdint>

#include "format/file.h"

namespace hokuto::app::export_parts
{

// Whether sound `sound` of `file` is exported as its conversion for another console's engine (edit::ForEngine). The
// sound then plays on that engine. The measure plays it the same way.
bool Converts(const format::File& file, uint32_t sound);

} // namespace hokuto::app::export_parts
