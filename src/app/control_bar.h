// SPDX-License-Identifier: MIT

// The bar across the top of the main window. The buttons at each end show and hide the zones on that side. The
// transport and its display sit in the middle. The output's meter and Studio Setup are on the right.

#pragma once

#include "app/transport.h"
#include "app/views.h"

namespace hokuto::app
{

// The bar's height, before the display's scale.
inline constexpr float kControlBarHeight = 58.0f;

// Draws the bar into the current window. The bar fills the window. The display shows `time` as the position's time, in
// milliseconds from the song's start.
void DrawControlBar(OpenViews& views, Transport& transport, double time, const MasterBus& master,
                    bool& studio_setup_open);

} // namespace hokuto::app
