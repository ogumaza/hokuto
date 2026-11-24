// SPDX-License-Identifier: MIT

// Tests the browser of a file with two archives (app/views.h) without a display. A click on any row of the archive that
// isn't in front brings that archive to the front, the row that the archive kept selected included. Only the archive in
// front shows a row as selected. Nothing draws the frames.

#include "app/browser.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "seqlib/testing/check.h"
#include "xsflib/testing/check.h"

#include "app/sound_section.h"
#include "app/theme.h"
#include "app/views.h"
#include "headless_imgui.h"

namespace
{

namespace app = hokuto::app;

// The browser's left edge and width, and its height on the display of 1280 by 860 pixels.
constexpr float kLeft = 20.0f;
constexpr float kWidth = 360.0f;
constexpr float kHeight = 820.0f;

// A group of sequences with `count` songs, named "Song 1" and on.
std::vector<app::BrowserGroup> SongGroups(int count)
{
    app::BrowserGroup group{.name = "Sequences", .icon = app::Icon::kSequence, .colour = app::theme::kSky};
    for (int i = 0; i < count; i++)
    {
        group.items.push_back(app::BrowserItem{.name = "Song " + std::to_string(i + 1)});
    }

    return {group};
}

// Dear ImGui without a display, with a file of two archives in the browser. Archive 0 is in front. Nothing draws the
// frames.
class Browser
{
public:
    Browser() : front_groups_(SongGroups(3)), back_groups_(SongGroups(3))
    {
        hokuto::testing::CreateHeadlessContext();
    }

    ~Browser()
    {
        ImGui::DestroyContext();
    }

    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;

    app::BrowserState& Front()
    {
        return front_state_;
    }

    app::BrowserState& Back()
    {
        return back_state_;
    }

    // Draws a frame with the pointer at (`x`, `y`), and the left button down while `down` is set.
    void Draw(float x, float y, bool down)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ImGui::NewFrame();
        const std::vector<app::BrowserArchive> archives = {
            app::BrowserArchive{.name = "MusicPlayer2000", .sounds = 3},
            app::BrowserArchive{.name = "Nintendo R&D2", .sounds = 3, .groups = &back_groups_, .state = &back_state_}};
        ImGui::SetNextWindowPos(ImVec2(kLeft, 20.0f));
        ImGui::SetNextWindowSize(ImVec2(kWidth, kHeight));
        bool open = true;
        app::DrawBrowser(front_groups_, archives, 0, "AZLE", front_state_, sound_, &open);
        hokuto::testing::EndHeadlessFrame();
    }

    // Draws frames with the pointer away from the browser.
    void DrawAway()
    {
        Draw(1200.0f, 840.0f, false);
        Draw(1200.0f, 840.0f, false);
    }

    // The height of the pointer over the row of item `item` of the archive that isn't in front, found by hovering, or
    // nothing.
    std::optional<float> BackRowHeight(int item)
    {
        DrawAway();
        ImGuiWindow* tree = TreeWindow();
        XSFLIB_CHECK(tree != nullptr);
        if (tree == nullptr)
        {
            return std::nullopt;
        }
        // The ID of the row's button: archive 1, group 0, the item and no child (DrawBrowser and DrawGroupRow).
        ImGuiID id = ImHashStr("archive", 0, tree->ID);
        const int archive = 1;
        const int group = 0;
        const int child = -1;
        id = ImHashData(&archive, sizeof(archive), id);
        id = ImHashData(&group, sizeof(group), id);
        id = ImHashData(&item, sizeof(item), id);
        id = ImHashData(&child, sizeof(child), id);
        id = ImHashStr("##item", 0, id);
        const float x = kLeft + kWidth * 0.5f;
        for (float y = tree->Pos.y; y < tree->Pos.y + tree->Size.y; y += 3.0f)
        {
            Draw(x, y, false);
            if (ImGui::GetCurrentContext()->HoveredId == id)
            {
                return y;
            }
        }

        return std::nullopt;
    }

    // Clicks at height `y` in the middle of the browser. Returns the archive that the click brings to the front.
    std::optional<std::size_t> Click(float y)
    {
        const float x = kLeft + kWidth * 0.5f;
        Draw(x, y, false);
        Draw(x, y, true);
        Draw(x, y, false);
        std::optional<std::size_t> archive = front_state_.archive;
        front_state_.archive.reset();

        return archive;
    }

    // The number of vertices of the tree in the colour of a selected row's fill. The count pills of the headers share
    // the colour.
    static std::size_t SelectedFillVertices()
    {
        ImGuiWindow* tree = TreeWindow();
        XSFLIB_CHECK(tree != nullptr);
        std::size_t count = 0;
        if (tree == nullptr)
        {
            return count;
        }
        for (const ImDrawVert& vertex : tree->DrawList->VtxBuffer)
        {
            count += vertex.col == app::theme::kControl ? 1 : 0;
        }

        return count;
    }

private:
    // The child window of the tree, or nullptr.
    static ImGuiWindow* TreeWindow()
    {
        for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
        {
            if (std::string_view(window->Name).find("##tree") != std::string_view::npos)
            {
                return window;
            }
        }

        return nullptr;
    }

    std::vector<app::BrowserGroup> front_groups_;
    std::vector<app::BrowserGroup> back_groups_;
    app::BrowserState front_state_;
    app::BrowserState back_state_;
    app::SoundSection sound_;
};

// A click on the row that the archive kept selected brings the archive to the front. A click on another row does the
// same, and the archive selects that row.
void TestClickBringsToFront()
{
    Browser browser;
    browser.Front().selected = app::BrowserPlace{.group = 0, .item = 0};
    browser.Back().selected = app::BrowserPlace{.group = 0, .item = 1};

    const std::optional<float> kept = browser.BackRowHeight(1);
    SEQLIB_REQUIRE(kept.has_value());
    XSFLIB_CHECK(browser.Click(*kept) == std::optional<std::size_t>(1));
    XSFLIB_CHECK(browser.Back().selected == (app::BrowserPlace{.group = 0, .item = 1}));
    XSFLIB_CHECK(browser.Front().selected == (app::BrowserPlace{.group = 0, .item = 0}));

    const std::optional<float> other = browser.BackRowHeight(2);
    SEQLIB_REQUIRE(other.has_value());
    XSFLIB_CHECK(browser.Click(*other) == std::optional<std::size_t>(1));
    XSFLIB_CHECK(browser.Back().selected == (app::BrowserPlace{.group = 0, .item = 2}));

    // Frames with the pointer away bring nothing to the front.
    browser.DrawAway();
    XSFLIB_CHECK(!browser.Front().archive.has_value());
}

// The archive that isn't in front shows no row as selected. The archive in front does.
void TestOnlyFrontShowsSelected()
{
    Browser browser;
    browser.DrawAway();
    const std::size_t none = Browser::SelectedFillVertices();

    browser.Back().selected = app::BrowserPlace{.group = 0, .item = 1};
    browser.DrawAway();
    XSFLIB_CHECK(Browser::SelectedFillVertices() == none);

    browser.Front().selected = app::BrowserPlace{.group = 0, .item = 1};
    browser.DrawAway();
    XSFLIB_CHECK(Browser::SelectedFillVertices() > none);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("browser",
                                    []
                                    {
                                        TestClickBringsToFront();
                                        TestOnlyFrontShowsSelected();
                                    });
}
