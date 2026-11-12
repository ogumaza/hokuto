// SPDX-License-Identifier: MIT

// The channel strips that the inspector and the mixer draw. The names are internal to those zones.

#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>

#include "app/mixer.h"
#include "app/song_view.h"
#include "app/widgets.h"

namespace hokuto::app::channel_strip
{

// Where a strip's level and peak sit below its top, under the sends, the pan, and M and S. The fader follows them.
inline constexpr float kLevelsTop = 151.0f;

// What a channel strip shows. The output's strip has no sends, pan, mute or solo. A track's strip records the user's
// moves of its controls in the track's view (TrackView::held and moved). While `busy`, the controls take no moves.
struct Strip
{
    const char* name = "";
    ImU32 colour = 0;
    float* volume = nullptr;
    float highest = kFaderTop; // the level in dB where the fader stops
    float* pan = nullptr;
    std::array<float, kSendCount>* sends = nullptr;
    std::size_t buses = kSendCount; // the sends that the console's tracks have
    const char* sends_missing = ""; // why the console's tracks have no other sends
    bool* mute = nullptr;
    bool* solo = nullptr;
    std::array<float, 2> meter = {kSilence, kSilence};
    float* peak = nullptr;
    TrackView* track = nullptr;
    bool selected = false;
    bool busy = false;
};

// Draws the M and S buttons side by side, from `pos`. A click on either one switches its flag.
void DrawMuteSolo(ImDrawList* draw, ImVec2 pos, ImVec2 button, float gap, bool* mute, bool* solo);

// Draws a strip's level and its loudest peak since a click on the peak cleared it, in boxes either side of `centre` at
// `y`. The peak turns red above 0 dB. A strip without a peak shows silence there.
void DrawLevels(ImDrawList* draw, float centre, float y, float level, float* peak);

// Draws a strip's meters, left and right, and the scale that they share with the fader. The fader stands at `y`,
// `height` high, left of `centre`, and stops at `highest`.
void DrawFaderMeters(ImDrawList* draw, float centre, float y, float height, const std::array<float, 2>& meter,
                     float highest = kFaderTop);

// Draws a channel strip at the cursor. The strip holds the sends, the pan, M and S, the level and the peak, the fader
// with its scale and meters, and the name at the bottom in the track's colour. A track whose mix other tracks or sounds
// share shows an equals sign after its name. Returns true when the strip is clicked.
bool DrawStrip(const Strip& strip, ImVec2 size);

// The channel strip of `track`. `selected` marks it, and while `busy` it accepts no moves.
Strip TrackStrip(TrackView& track, bool selected, bool busy);

// The channel strip of `track` of `song`, with the highest level of the faders and the sends of the song's console.
Strip SongTrackStrip(const SongView& song, TrackView& track, bool selected, bool busy);

// The channel strip of the output, Stereo Out.
Strip OutputStrip(MasterBus& master);

} // namespace hokuto::app::channel_strip
