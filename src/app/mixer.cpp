// SPDX-License-Identifier: MIT

// The mixer: the tracks' strips, the output, the FX channels with their inserts, and the switches of the mixing
// mode and the engine.

#include "app/mixer.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "app/arrangement.h"
#include "app/channel_strip.h"
#include "app/engine_names.h"
#include "app/engine_rows.h"
#include "app/export_warnings.h"
#include "app/icons.h"
#include "app/song_view.h"
#include "app/theme.h"
#include "app/views.h"
#include "app/widgets.h"
#include "app/zone_parts.h"
#include "format/engine.h"
#include "format/engines.h"
#include "format/kinds.h"

namespace hokuto::app
{
namespace
{

using channel_strip::DrawFaderMeters;
using channel_strip::DrawLevels;
using channel_strip::DrawStrip;
using channel_strip::kLevelsTop;
using channel_strip::OutputStrip;
using channel_strip::SongTrackStrip;
using channel_strip::Strip;
using theme::Px;
using zone_parts::BeginZone;

// The mixer's sizes, before the display's scale.
constexpr float kStripWidth = 84.0f;
constexpr float kMasterWidth = 96.0f;
constexpr float kFxStripWidth = 104.0f;
constexpr float kInsertHeight = 24.0f;

// The mixing switch in the mixer's header, before the display's scale: its size, and its gap after the title. The
// header is 32 high.
constexpr float kMixingSwitchWidth = 172.0f;
constexpr float kMixingSwitchHeight = 24.0f;
constexpr float kMixingSwitchGap = 18.0f;
constexpr float kEngineSwitchWidth = 236.0f;
constexpr float kHeaderHeight = 32.0f;

// Draws an insert of an FX channel from `min` to `max`, with the name of its effect. The insert of the open effect is
// lit. The name of an idle effect shows grey.
void DrawInsert(ImDrawList* draw, ImVec2 min, ImVec2 max, const std::string& name, bool open, bool hovered, bool idle)
{
    const ImU32 fill = open      ? theme::Mix(theme::kControl, theme::kAccent, 0.3f)
                       : hovered ? theme::kControlHover
                                 : theme::kControl;
    draw->AddRectFilled(min, max, fill, Px(4.0f));
    if (open)
    {
        draw->AddRect(min, max, theme::kAccent, Px(4.0f), Px(1.0f));
    }

    ImFont* text = theme::GetFonts().text;
    const std::string shown = FitText(text, theme::kSmallSize, name, max.x - min.x - Px(14.0f));
    const ImVec2 size = TextSize(text, theme::kSmallSize, shown);
    DrawText(draw, text, theme::kSmallSize, ImVec2(min.x + Px(8.0f), (min.y + max.y - size.y) * 0.5f),
             idle ? theme::kTextMuted : theme::kText, shown);
}

// Draws an empty insert from `min` to `max`, with a plus in the middle.
void DrawEmptyInsert(ImDrawList* draw, ImVec2 min, ImVec2 max, bool hovered)
{
    draw->AddRect(min, max, hovered ? theme::kBorderStrong : theme::kBorder, Px(4.0f), Px(1.0f));
    const ImVec2 centre(std::floor((min.x + max.x) * 0.5f), std::floor((min.y + max.y) * 0.5f));
    const float arm = Px(4.5f);
    const ImU32 colour = hovered ? theme::kText : theme::kTextMuted;
    draw->AddLine(ImVec2(centre.x - arm, centre.y), ImVec2(centre.x + arm, centre.y), colour, Px(1.5f));
    draw->AddLine(ImVec2(centre.x, centre.y - arm), ImVec2(centre.x, centre.y + arm), colour, Px(1.5f));
}

// The menu of the kinds of effect that an empty insert at `place` opens. A choice sets `fx.clicked` and `fx.kind`.
void DrawEffectKindsMenu(FxChannels& fx, const EffectPlace& place)
{
    if (ImGui::BeginPopup("##kinds"))
    {
        PushMenuSpacing();
        for (std::size_t kind = 0; kind < fx.kinds.size(); kind++)
        {
            if (ImGui::MenuItem(fx.kinds[kind].c_str()))
            {
                fx.clicked = place;
                fx.kind = kind;
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

// Draws the inserts of FX channel `channel` in the strip from `pos` to `end`. Returns the top of the space under them.
float DrawInserts(ImDrawList* draw, std::size_t channel, FxChannels& fx, ImVec2 pos, ImVec2 end)
{
    DrawCaps(draw, ImVec2(pos.x + Px(8.0f), pos.y + Px(12.0f)), theme::kTextMuted, "Inserts");
    float y = pos.y + Px(12.0f) + CapsSize("Inserts").y + Px(8.0f);
    const std::vector<std::string>& effects = fx.effects[channel];
    for (std::size_t i = 0; i <= effects.size(); i++)
    {
        const EffectPlace place{channel, i};
        const ImVec2 min(pos.x + Px(6.0f), y);
        const ImVec2 max(end.x - Px(6.0f), y + Px(kInsertHeight));
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorScreenPos(min);
        const bool clicked = ImGui::InvisibleButton("##insert", ImVec2(max.x - min.x, max.y - min.y));
        const bool hovered = ImGui::IsItemHovered();
        if (i < effects.size())
        {
            const bool idle = i < fx.idle[channel].size() && fx.idle[channel][i];
            DrawInsert(draw, min, max, effects[i], fx.open == place, hovered, idle);
            ItemTooltip(idle ? effects[i] + "\n" + fx.idle_note : effects[i]);
            if (clicked)
            {
                fx.clicked = place;
            }
        }
        else
        {
            // A click opens the menu of the kinds of effect.
            DrawEmptyInsert(draw, min, max, hovered);
            ItemTooltip("Add an effect");
            if (clicked)
            {
                ImGui::OpenPopup("##kinds");
            }
            DrawEffectKindsMenu(fx, place);
        }
        ImGui::PopID();
        y += Px(kInsertHeight) + Px(4.0f);
    }

    return y;
}

// Draws the fader of FX channel `channel` and its meters from `y` down to the name's box above `end`, left of
// `centre`. A hold of the fader sets `fx.held`, and its end with a new level sets `fx.moved`.
void DrawFxFader(ImDrawList* draw, std::size_t channel, FxChannels& fx, float centre, float y, ImVec2 end)
{
    const float fader_height = std::max(end.y - Px(28.0f) - Px(8.0f) - y, Px(48.0f));
    float& volume = fx.volumes[channel];
    const float before = volume;
    ImGui::SetCursorScreenPos(ImVec2(centre - Px(34.0f), y));
    Fader("##fader", &volume, ImVec2(Px(30.0f), fader_height));
    if (ImGui::IsItemActivated())
    {
        fx.held = FxMove{channel, before, before};
    }
    if (ImGui::IsItemDeactivated() && fx.held && fx.held->channel == channel)
    {
        if (volume != fx.held->from)
        {
            fx.moved = FxMove{channel, fx.held->from, volume};
        }
        fx.held.reset();
    }
    DrawFaderMeters(draw, centre, y, fader_height, fx.meters[channel]);
}

// Draws the name of FX channel `channel` in a box at the bottom of the strip from `pos` to `end`.
void DrawFxName(ImDrawList* draw, std::size_t channel, ImVec2 pos, ImVec2 end)
{
    const std::string name = FxName(channel);
    const ImU32 kColour = theme::Mix(theme::kAccent, theme::kRaised, 0.45f);
    const ImVec2 name_min(pos.x + Px(5.0f), end.y - Px(28.0f));
    const ImVec2 name_max(end.x - Px(5.0f), end.y - Px(5.0f));
    draw->AddRectFilled(name_min, name_max, kColour, Px(4.0f));
    ImFont* text = theme::GetFonts().text;
    const ImVec2 name_size = TextSize(text, theme::kSmallSize, name);
    DrawText(draw, text, theme::kSmallSize,
             ImVec2(std::floor((name_min.x + name_max.x - name_size.x) * 0.5f),
                    name_min.y + (name_max.y - name_min.y - name_size.y) * 0.5f),
             theme::TextOn(kColour), name);
}

// Draws the strip of FX channel `channel` at the cursor: its inserts at the top, the level and the peak, the fader with
// its scale and meters, and its name at the bottom. A click on an effect, or a choice in an empty insert's menu, sets
// `fx.clicked`. A hold of the fader that ends with a new level sets `fx.moved`.
void DrawFxStrip(std::size_t channel, FxChannels& fx, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    const float centre = std::floor(pos.x + size.x * 0.5f);
    draw->AddRectFilled(pos, end, theme::kRaised, Px(8.0f));
    draw->AddRect(pos, end, theme::kBorder, Px(8.0f), Px(1.0f));

    // An insert for each effect, then an empty one.
    float y = DrawInserts(draw, channel, fx, pos, end);

    // The level and the peak, then the fader, in line with the tracks' strips. More inserts than fit push them down.
    y = std::max(pos.y + Px(kLevelsTop), y + Px(2.0f));
    DrawLevels(draw, centre, y, fx.volumes[channel], &fx.peaks[channel]);
    y += Px(27.0f);
    DrawFxFader(draw, channel, fx, centre, y, end);

    // The name, in a box at the bottom.
    DrawFxName(draw, channel, pos, end);

    // The strip takes its place in the layout.
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(size);
}

// Draws the switch between the shown sound's two mixing modes in the mixer's header at `header`, after the title. A
// click on the other mode sets the song's `mixing_request`. Each half's tooltip explains its mode. A sound that can't
// take enhanced mixing has that half disabled, and its tooltip says why. The cursor stays where it was.
void DrawMixingSwitch(ImVec2 header, SongView& song, bool busy)
{
    const std::array<std::string, 2> kLabels = {"Strict", "Enhanced"};
    const std::array<uint8_t, 2> lit = {static_cast<uint8_t>(song.enhanced ? 0 : 1),
                                        static_cast<uint8_t>(song.enhanced ? 1 : 0)};
    const std::array<uint8_t, 2> disabled = {0, static_cast<uint8_t>(song.enhanced_refused.empty() ? 0 : 1)};
    const float title = TextSize(theme::GetFonts().strong, theme::kTextSize, "Mixer").x;
    const ImVec2 size(Px(kMixingSwitchWidth), Px(kMixingSwitchHeight));
    const ImVec2 pos(std::floor(header.x + Px(14.0f) + title + Px(kMixingSwitchGap)),
                     std::floor(header.y + (Px(kHeaderHeight) - size.y) * 0.5f));
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(pos);
    const int clicked = SegmentedSwitch("##mixing", kLabels, lit, size, disabled);
    ImGui::SetCursorScreenPos(cursor);

    // The half under the mouse explains its mode, or why the sound can't take it.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool over = ImGui::IsWindowHovered() && mouse.y >= pos.y && mouse.y < pos.y + size.y && mouse.x >= pos.x &&
                      mouse.x < pos.x + size.x;
    if (over && !ImGui::IsAnyMouseDown())
    {
        WrappedTooltip(mouse.x < pos.x + size.x * 0.5f
                           ? "Mix the sound as the console does, with its voice limit, resampling and rounding. This "
                             "is how the sound plays in the game."
                       : song.enhanced_refused.empty()
                           ? "Mix the sound without the console limits: up to 1,024 voices, 48 kHz floating-point "
                             "mixing and long windowed-sinc resampling. Notes, instruments and effects play as on the "
                             "console."
                           : song.enhanced_refused);
    }
    if (!busy && clicked == 0 && song.enhanced)
    {
        song.mixing_request = false;
    }
    else if (!busy && clicked == 1 && !song.enhanced && song.enhanced_refused.empty())
    {
        song.mixing_request = true;
    }
}

// A segment of the mixer's engine switch: a console, and the engines of the console that can play the shown sound. A
// segment with more than one engine opens a menu of their drivers.
struct EngineSegment
{
    format::Platform console;
    std::vector<int> engines;
};

// The segments of the mixer's engine switch, in the order of format::Platform: the consoles whose engines play the
// sounds of other consoles, and the console of the file. A segment holds those engines of its console, and the file's
// engine first. A file of DSE gives the DS's segment DSE and NITRO-Composer. A file of another driver of the GBA gives
// the GBA's segment that driver alone. No other sound plays on DSE or on those drivers.
std::vector<EngineSegment> SwitchSegments(const SongView& song)
{
    std::vector<EngineSegment> segments;
    for (const format::EngineFacts& facts : format::kEngineFacts)
    {
        const int engine = static_cast<int>(facts.engine);
        if (!facts.hosts_other_consoles && engine != song.file_engine)
        {
            continue;
        }
        auto segment = std::ranges::find(segments, facts.platform, &EngineSegment::console);
        if (segment == segments.end())
        {
            segment = segments.insert(segments.end(), EngineSegment{.console = facts.platform, .engines = {}});
        }
        segment->engines.insert(engine == song.file_engine ? segment->engines.begin() : segment->engines.end(), engine);
    }
    std::ranges::sort(segments, {}, &EngineSegment::console);

    return segments;
}

// What the tooltip of engine `engine` of the mixer's switch says.
std::string EngineTip(const SongView& song, int engine)
{
    const auto index = static_cast<std::size_t>(engine);
    const auto shown = static_cast<format::Engine>(engine);
    const auto file = static_cast<format::Engine>(song.file_engine);
    const std::string library = RowOf(shown).library;
    if (!song.engine_refused[index].empty())
    {
        return song.engine_refused[index];
    }
    if (engine == song.file_engine)
    {
        return "Play the sound on its original engine: " + library + ". It plays as in the game.";
    }

    std::string tip = "Play the sound on " + TheEngine(shown) + ": " + library +
                      ". Hokuto plays the conversion that File > Export > Another Console… writes. The file keeps the "
                      "sound for " +
                      TheConsole(EngineName(file)) + ".";
    if (engine == song.failed_engine && !song.engine_error.empty())
    {
        tip += "\n\nCan't convert the sound. " + format::AsSentence(song.engine_error) + " It plays on " +
               TheEngine(file) + ".";
    }
    else if (engine == song.engine && !song.engine_notes.empty())
    {
        for (const std::string& note : song.engine_notes)
        {
            tip += "\n\n" + note;
        }
    }

    return tip;
}

// What the tooltip of segment `segment` of the mixer's switch says. A segment of one engine gives the engine's tooltip.
// A segment with a menu says what the menu chooses, and gives the tooltip of its engine that plays the sound.
std::string SegmentTip(const SongView& song, const EngineSegment& segment)
{
    if (segment.engines.size() == 1)
    {
        return EngineTip(song, segment.engines.front());
    }

    std::string tip = std::string("Choose the driver that plays the sound on the ") +
                      ConsoleName(static_cast<format::Engine>(segment.engines.front())) + ".";
    if (std::ranges::find(segment.engines, song.engine) != segment.engines.end())
    {
        tip += "\n\n" + EngineTip(song, song.engine);
    }

    return tip;
}

// The menu of the drivers of segment `segment`. It opens under the segment's switch at `below`. A choice of another
// engine that can play the sound sets the song's `engine_request`. Each driver's tooltip says how it plays the sound,
// or why it can't.
void DrawDriverMenu(SongView& song, const EngineSegment& segment, ImVec2 below, bool busy)
{
    ImGui::SetNextWindowPos(below, ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##drivers"))
    {
        return;
    }

    PushMenuSpacing();
    for (const int engine : segment.engines)
    {
        const bool refused = !song.engine_refused[static_cast<std::size_t>(engine)].empty();
        if (ImGui::MenuItem(DriverName(static_cast<format::Engine>(engine)), nullptr, engine == song.engine,
                            !refused) &&
            !busy && engine != song.engine)
        {
            song.engine_request = engine;
        }
        ItemTooltip(EngineTip(song, engine));
    }
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

// Draws the switch between the consoles whose engines can play the shown sound in the mixer's header at `header`,
// after the switch of the mixing modes. A click on a console of one engine sets the song's `engine_request`. A console
// of several engines shows a chevron, and a click opens the menu of its drivers. Each segment's tooltip says how its
// engine plays the sound, or why it can't. The cursor stays where it was.
void DrawEngineSwitch(ImVec2 header, SongView& song, bool busy)
{
    const std::vector<EngineSegment> segments = SwitchSegments(song);
    std::vector<std::string> labels;
    std::vector<uint8_t> lit;
    std::vector<uint8_t> disabled;
    for (const EngineSegment& segment : segments)
    {
        labels.emplace_back(ConsoleName(static_cast<format::Engine>(segment.engines.front())));
        lit.push_back(std::ranges::find(segment.engines, song.engine) != segment.engines.end() ? 1 : 0);
        disabled.push_back(
            std::ranges::all_of(segment.engines, [&song](int engine)
                                { return !song.engine_refused[static_cast<std::size_t>(engine)].empty(); })
                ? 1
                : 0);
    }
    const float title = TextSize(theme::GetFonts().strong, theme::kTextSize, "Mixer").x;
    const float width = kEngineSwitchWidth * static_cast<float>(segments.size()) / 4.0f;
    const ImVec2 size(Px(width), Px(kMixingSwitchHeight));
    const ImVec2 pos(
        std::floor(header.x + Px(14.0f) + title + Px(kMixingSwitchGap) + Px(kMixingSwitchWidth) + Px(kMixingSwitchGap)),
        std::floor(header.y + (Px(kHeaderHeight) - size.y) * 0.5f));
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(pos);
    const int clicked = SegmentedSwitch("##engine", labels, lit, size, disabled);
    ImGui::SetCursorScreenPos(cursor);

    // A segment with a menu shows a chevron at its right, like a menu's button. SegmentedSwitch divides the strip
    // inside its 2-point inset evenly.
    const float inset = Px(2.0f);
    const float segment_width = (size.x - inset * 2.0f) / static_cast<float>(segments.size());
    const float chevron = Px(8.0f);
    for (std::size_t i = 0; i < segments.size(); i++)
    {
        if (segments[i].engines.size() > 1)
        {
            const float right = std::floor(pos.x + inset + segment_width * static_cast<float>(i + 1));
            DrawIcon(ImGui::GetWindowDrawList(), Icon::kChevronDown,
                     ImVec2(right - Px(6.0f) - chevron * 0.5f, pos.y + size.y * 0.5f), chevron,
                     disabled[i] != 0 ? theme::kTextDim : theme::kTextMuted);
        }
    }

    // The segment under the mouse says how its engine plays the sound, or why it can't.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool over = ImGui::IsWindowHovered() && mouse.y >= pos.y && mouse.y < pos.y + size.y && mouse.x >= pos.x &&
                      mouse.x < pos.x + size.x;
    if (over && !ImGui::IsAnyMouseDown() && !ImGui::IsPopupOpen("##drivers"))
    {
        const auto under = static_cast<int>((mouse.x - pos.x) / size.x * static_cast<float>(segments.size()));
        const int place = std::clamp(under, 0, static_cast<int>(segments.size()) - 1);
        WrappedTooltip(SegmentTip(song, segments[static_cast<std::size_t>(place)]));
    }

    // The window's storage keeps the console whose menu is open. Another file can show other segments. A console that
    // no longer has a menu draws none.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID menu_console = ImGui::GetID("##drivers console");
    if (clicked >= 0)
    {
        const EngineSegment& segment = segments[static_cast<std::size_t>(clicked)];
        if (segment.engines.size() > 1)
        {
            storage->SetInt(menu_console, static_cast<int>(segment.console));
            ImGui::OpenPopup("##drivers");
        }
        else if (!busy && segment.engines.front() != song.engine &&
                 song.engine_refused[static_cast<std::size_t>(segment.engines.front())].empty())
        {
            song.engine_request = segment.engines.front();
        }
    }
    const auto open =
        std::ranges::find_if(segments, [&](const EngineSegment& segment)
                             { return static_cast<int>(segment.console) == storage->GetInt(menu_console, -1); });
    if (open != segments.end() && open->engines.size() > 1)
    {
        const float left =
            std::floor(pos.x + inset + segment_width * static_cast<float>(std::distance(segments.begin(), open)));
        DrawDriverMenu(song, *open, ImVec2(left, pos.y + size.y + Px(2.0f)), busy);
    }
}

// The channel strips of the tracks of `song`, `height` tall. A click on a strip selects its track.
void DrawTrackChannels(SongView* song, std::size_t channels, ArrangementState& state, bool busy, float height)
{
    for (std::size_t i = 0; i < channels; i++)
    {
        ImGui::PushID(static_cast<int>(i));
        const Strip strip = SongTrackStrip(*song, song->tracks[i], state.selected_track == static_cast<int>(i), busy);
        if (DrawStrip(strip, ImVec2(Px(kStripWidth), height)))
        {
            state.selected_track = static_cast<int>(i);
        }
        ImGui::SameLine();
        ImGui::PopID();
    }
}

// The note that fills the width that the FX channels leave in a mixer without tracks.
void DrawNoChannels(const FxChannels* fx, float height)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const std::size_t fx_count = fx != nullptr ? std::min(fx->count, kFxChannels) : 0;
    const float fx_width =
        fx_count > 0 ? Px(14.0f) + static_cast<float>(fx_count) * (Px(kFxStripWidth) + Px(6.0f)) : 0.0f;
    const float avail_width = ImGui::GetContentRegionAvail().x;
    const float width = fx != nullptr ? std::max(avail_width - fx_width, Px(320.0f)) : avail_width;
    EmptyState(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + width, pos.y + height), "No channels",
               "Each track of the sequence appears here as a channel.");
    ImGui::Dummy(ImVec2(width, height));
    ImGui::SameLine();
}

// The FX channels after the tracks' channels, past a line.
void DrawFxChannels(FxChannels& fx, float height)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x + Px(3.0f), at.y + Px(10.0f)),
                                              ImVec2(at.x + Px(4.0f), at.y + height - Px(10.0f)), theme::kBorder);
    ImGui::SetCursorScreenPos(ImVec2(at.x + Px(14.0f), at.y));
    ImGui::PushID("##fx");
    for (std::size_t channel = 0; channel < std::min(fx.count, kFxChannels); channel++)
    {
        ImGui::PushID(static_cast<int>(channel));
        DrawFxStrip(channel, fx, ImVec2(Px(kFxStripWidth), height));
        ImGui::SameLine();
        ImGui::PopID();
    }
    ImGui::PopID();
}

} // namespace

void DrawMixer(SongView* song, MasterBus& master, ArrangementState& state, FxChannels* fx, bool busy, bool* open)
{
    if (!*open)
    {
        return;
    }

    if (BeginZone(kMixer, open))
    {
        const std::size_t channels = song != nullptr ? song->tracks.size() : 0;
        const ImVec2 header = ImGui::GetCursorScreenPos();
        ZoneHeader("Mixer", channels == 0   ? std::string()
                            : channels == 1 ? std::string("1 channel")
                                            : std::to_string(channels) + " channels");
        if (song != nullptr)
        {
            DrawMixingSwitch(header, *song, busy);
            if (song->engines)
            {
                DrawEngineSwitch(header, *song, busy);
            }
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float output_width = Px(kMasterWidth) + Px(20.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(10.0f), Px(10.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(6.0f), 0.0f));

        // The tracks' channels scroll sideways.
        ImGui::BeginChild("##channels", ImVec2(avail.x - output_width, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_HorizontalScrollbar);
        const float height = ImGui::GetContentRegionAvail().y;
        DrawTrackChannels(song, channels, state, busy, height);

        // Without tracks, a note fills the width that the FX channels leave.
        if (channels == 0)
        {
            DrawNoChannels(fx, height);
        }

        // The FX channels follow the tracks' channels, past a line. The GBA and the DS have none.
        if (fx != nullptr && fx->count > 0)
        {
            DrawFxChannels(*fx, height);
        }
        ImGui::EndChild();

        // The output's channel stays at the right.
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("##output", ImVec2(output_width, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        const ImVec2 pos = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(pos.x, pos.y + Px(10.0f)), ImVec2(pos.x + Px(1.0f), pos.y + avail.y - Px(10.0f)), theme::kBorder);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(4.0f));
        DrawStrip(OutputStrip(master), ImVec2(Px(kMasterWidth), ImGui::GetContentRegionAvail().y));
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
}

} // namespace hokuto::app
