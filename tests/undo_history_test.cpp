// SPDX-License-Identifier: MIT

// Tests the Undo History (app/undo_history.h) without a display. The zone keeps the number of done actions and of rows
// that it last showed, and a change of either scrolls the line into view. Nothing draws the frames.

#include "app/undo_history.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cfloat>
#include <cstddef>
#include <span>
#include <string_view>

#include "seqlib/testing/check.h"
#include "xsflib/testing/check.h"

#include "headless_imgui.h"

namespace
{

namespace app = hokuto::app;

// Dear ImGui without a display. Nothing draws its frames, and its textures count as made.
class Screen
{
public:
    Screen()
    {
        hokuto::testing::CreateHeadlessContext();
    }

    ~Screen()
    {
        ImGui::DestroyContext();
    }

    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    static void Draw(std::span<const app::HistoryRow> rows, std::size_t done)
    {
        ImGui::NewFrame();
        bool open = true;
        (void)app::DrawUndoHistory(rows, done, &open);
        hokuto::testing::EndHeadlessFrame();
    }
};

// The child window of the rows, or nullptr.
ImGuiWindow* RowsWindow()
{
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
    {
        if (std::string_view(window->Name).find("##rows") != std::string_view::npos)
        {
            return window;
        }
    }

    return nullptr;
}

// The first frame with rows keeps both numbers that it shows. The next frame then finds them unchanged, and asks for no
// scroll.
void TestShownNumbers()
{
    Screen screen;
    const std::array<app::HistoryRow, 3> rows = {app::HistoryRow{"Change Volume", "Track 1", "10:00"},
                                                 app::HistoryRow{"Move Notes", "Track 2", "10:01"},
                                                 app::HistoryRow{"Change Pan", "Track 1", "10:02"}};

    Screen::Draw(rows, 2);

    ImGuiWindow* window = RowsWindow();
    SEQLIB_REQUIRE(window != nullptr);
    XSFLIB_CHECK(window->StateStorage.GetInt(window->GetID("##done"), -1) == 2);
    XSFLIB_CHECK(window->StateStorage.GetInt(window->GetID("##count"), -1) == 3);

    // The second frame shows the same rows.
    Screen::Draw(rows, 2);

    SEQLIB_REQUIRE(RowsWindow() == window);
    XSFLIB_CHECK(window->StateStorage.GetInt(window->GetID("##done"), -1) == 2);
    XSFLIB_CHECK(window->StateStorage.GetInt(window->GetID("##count"), -1) == 3);
    XSFLIB_CHECK(window->ScrollTarget.y == FLT_MAX);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("undo_history", [] { TestShownNumbers(); });
}
