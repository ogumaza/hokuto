// SPDX-License-Identifier: MIT

// The Key Editor's window, drawn without a display.

#include "app/key_editor.h"

#include <imgui.h>

#include <cstdint>
#include <optional>

#include "xsflib/testing/check.h"

#include "app/widgets.h"
#include "headless_imgui.h"

namespace hokuto
{
namespace
{

using app::KeyEdit;

// Draws one frame of the Key Editor over the whole display. Returns the edit that the frame ends.
std::optional<KeyEdit> Frame(const app::KeyTrack& track, app::KeyEditorState& state, app::Transport& transport)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    bool open = true;
    std::optional<KeyEdit> edit = app::DrawKeyEditor(track, state, transport, app::ShownItem(), false, &open);
    testing::EndHeadlessFrame();
    return edit;
}

// A press of the Draw tool on a note removes the note at the press. Every key has a note over the whole song. A press
// anywhere among the notes is therefore on one.
void TestDrawPressRemoves()
{
    testing::CreateHeadlessContext();
    app::KeyTrack track;
    track.name = "Track 0";
    track.song_length = 48 * 64;
    for (int key = 0; key < 128; key++)
    {
        track.notes.push_back({.start = 0, .length = track.song_length, .key = key, .command = 1000u + key});
    }
    app::KeyEditorState state;
    state.tool = app::KeyTool::kDraw;
    app::Transport transport;
    for (int frame = 0; frame < 3; frame++)
    {
        (void)Frame(track, state, transport);
    }

    ImGuiIO& io = ImGui::GetIO();
    bool removed = false;
    for (float y = 300.0f; y < 640.0f && !removed; y += 20.0f)
    {
        io.AddMousePosEvent(640.0f, y);
        (void)Frame(track, state, transport);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        const std::optional<KeyEdit> pressed = Frame(track, state, transport);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        (void)Frame(track, state, transport);
        removed = pressed.has_value() && pressed->changes.size() == 1 && pressed->changes.front().remove &&
                  pressed->changes.front().command >= 1000u;
    }
    XSFLIB_CHECK(removed);
    ImGui::DestroyContext();
}

} // namespace
} // namespace hokuto

int main()
{
    return xsflib::testing::RunTest("key_editor", [] { hokuto::TestDrawPressRemoves(); });
}
