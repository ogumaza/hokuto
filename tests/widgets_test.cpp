// SPDX-License-Identifier: MIT

// Tests the text of the program's controls (app/widgets.h) without a display: FitText's cuts, and the tooltip of the
// lines that it cuts, such as the name and the details of a row of the Undo History. Dear ImGui's default font measures
// the text. Each width comes from the text's width in that font. The cuts then don't rest on the font's metrics. Also
// tests Return in the search field of a menu (FilteredCombo). The mouse and the keys reach Dear ImGui through its queue
// of events.
// A field of an info line that the user opened before the program turned busy checks BusyInfoEntry. WaitingValues
// keeps a typed value for its item only, and only while its version exists.

#include "app/widgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "app/theme.h"
#include "format/file.h"
#include "headless_imgui.h"

namespace
{

namespace app = hokuto::app;
namespace theme = hokuto::app::theme;
using app::CutLinesTooltip;
using app::FittedLine;
using app::FitText;
using app::TextSize;

// Dear ImGui without a display, in a frame. Nothing draws the frame.
class Frame
{
public:
    Frame()
    {
        hokuto::testing::CreateHeadlessContext();
        ImGui::NewFrame();
    }

    ~Frame()
    {
        ImGui::EndFrame();
        ImGui::DestroyContext();
    }

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

// A row of the Undo History: the name of an import of a SoundFont, and its details. The row's tooltip stays empty while
// both lines fit. Once either is a pixel too wide, FitText cuts it with an ellipsis, and the tooltip contains both
// lines in full, the name first. Empty details stay out of it.
void TestCutLinesTooltip()
{
    const Frame frame;
    ImFont* font = theme::GetFonts().text;
    const std::string name = "Import SoundFont";
    const std::string details = "tones.sf2 on Voice group 0x08498ED8 (new programs 3 and 5)";
    const float name_width = TextSize(font, theme::kTextSize, name).x;
    const float details_width = TextSize(font, theme::kSmallSize, details).x;
    const auto row = [&](float cut_name, float cut_details)
    {
        return std::array<FittedLine, 2>{FittedLine{font, theme::kTextSize, name, name_width - cut_name},
                                         FittedLine{font, theme::kSmallSize, details, details_width - cut_details}};
    };

    XSFLIB_CHECK(CutLinesTooltip(row(0.0f, 0.0f)).empty());
    XSFLIB_CHECK(FitText(font, theme::kTextSize, name, name_width) == name);
    XSFLIB_CHECK(FitText(font, theme::kSmallSize, details, details_width) == details);

    const std::string cut = FitText(font, theme::kSmallSize, details, details_width - 1.0f);
    XSFLIB_CHECK(cut != details && cut.ends_with("…") && details.starts_with(cut.substr(0, cut.size() - 3)));
    XSFLIB_CHECK(CutLinesTooltip(row(0.0f, 1.0f)) == name + "\n" + details);
    XSFLIB_CHECK(FitText(font, theme::kTextSize, name, name_width - 1.0f) != name);
    XSFLIB_CHECK(CutLinesTooltip(row(1.0f, 0.0f)) == name + "\n" + details);

    const std::array<FittedLine, 2> no_details = {FittedLine{font, theme::kTextSize, name, name_width - 1.0f},
                                                  FittedLine{font, theme::kSmallSize, "", 0.0f}};
    XSFLIB_CHECK(CutLinesTooltip(no_details) == name);
}

// Return in a menu's search field chooses the one item left and closes the menu. A click on the item does the same.
// With more than one item left, Return chooses nothing.
void TestFilteredComboReturn()
{
    hokuto::testing::CreateHeadlessContext();
    const std::vector<std::string> items = {"BANK_PIANO", "BANK_STRINGS", "BANK_BRASS"};
    int chosen = 0;
    std::array<char, 128> filter{};
    ImVec2 field;
    const auto draw = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
        ImGui::Begin("Menu");
        field = ImGui::GetCursorScreenPos();
        const bool changed = app::FilteredCombo("##bank", items, chosen, nullptr, filter, 200.0f);
        ImGui::End();
        hokuto::testing::EndHeadlessFrame();

        return changed;
    };
    const auto frame = [&](const std::function<void()>& event)
    {
        event();
        return draw();
    };
    const auto open = [&]
    {
        ImGuiIO& io = ImGui::GetIO();
        (void)frame([&] { io.AddMousePosEvent(field.x + 20.0f, field.y + ImGui::GetFrameHeight() * 0.5f); });
        (void)frame([&] { io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); });
        (void)frame([&] { io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); });
        (void)draw();
        (void)draw();
    };
    const auto type = [&](const std::string& text)
    {
        for (const char c : text)
        {
            (void)frame([c] { ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(c)); });
        }
    };
    const auto enter = [&]
    {
        const bool pressed = frame([] { ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true); });
        const bool released = frame([] { ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); });

        return pressed || released;
    };
    (void)draw();

    open();
    const bool menu_opened = !ImGui::GetCurrentContext()->OpenPopupStack.empty();
    type("strings");
    const bool chose_one = enter();
    const bool closed_after_one = ImGui::GetCurrentContext()->OpenPopupStack.empty();
    const int chosen_after_one = chosen;

    open();
    type("bank");
    const bool chose_of_three = enter();
    const bool open_after_three = !ImGui::GetCurrentContext()->OpenPopupStack.empty();

    XSFLIB_CHECK(menu_opened);
    XSFLIB_CHECK(chose_one && chosen_after_one == 1 && closed_after_one);
    XSFLIB_CHECK(!chose_of_three && chosen == 1 && open_after_three);
    ImGui::DestroyContext();
}

// A field of an info line that the user opened before the program turned busy stays open. Return ends it, and its text
// waits until the program is free (BusyInfoEntry). With `hover`, the pointer rests on the field before Return.
void TestBusyInfoEntry(bool hover)
{
    hokuto::testing::CreateHeadlessContext();
    app::InfoTyping typing;
    app::WaitingValues<std::string> waiting;
    bool busy = false;
    const auto draw = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(600.0f, 200.0f));
        ImGui::Begin("Line");
        app::InfoLine line = app::BeginInfoLine();
        std::optional<std::string> text =
            app::BusyInfoEntry(line, "VELOCITY", "100", typing, waiting, app::ShownItem{}, busy);
        app::EndInfoLine(line);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::End();
        hokuto::testing::EndHeadlessFrame();

        return text;
    };
    const auto frame = [&](const std::function<void()>& event)
    {
        event();
        return draw();
    };
    ImGuiIO& io = ImGui::GetIO();
    (void)draw();

    // Click each place on the line until the value turns into a text field.
    std::optional<ImVec2> field;
    for (float y = 2.0f; y < 80.0f && !field; y += 4.0f)
    {
        for (float x = 2.0f; x < 600.0f && !field; x += 8.0f)
        {
            (void)frame([&] { io.AddMousePosEvent(x, y); });
            (void)frame([&] { io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); });
            (void)frame([&] { io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); });
            (void)draw();
            if (typing.field == "VELOCITY")
            {
                field = ImVec2(x, y);
            }
        }
    }
    (void)frame([&] { io.AddMousePosEvent(599.0f, 199.0f); });

    // The program turns busy while the user types and ends the field.
    busy = true;
    std::vector<std::optional<std::string>> while_busy;
    while_busy.push_back(frame([&] { io.AddInputCharacter('6'); }));
    while_busy.push_back(frame([&] { io.AddInputCharacter('4'); }));
    if (hover && field)
    {
        while_busy.push_back(frame([&] { io.AddMousePosEvent(field->x, field->y); }));
        while_busy.push_back(draw());
    }
    while_busy.push_back(frame([&] { io.AddKeyEvent(ImGuiKey_Enter, true); }));
    while_busy.push_back(frame([&] { io.AddKeyEvent(ImGuiKey_Enter, false); }));
    while_busy.push_back(draw());
    const bool ended = typing.field.empty();

    // The program is free again.
    busy = false;
    const std::optional<std::string> made = draw();
    const std::optional<std::string> after = draw();

    XSFLIB_CHECK(field.has_value());
    for (const std::optional<std::string>& text : while_busy)
    {
        XSFLIB_CHECK(!text);
    }
    XSFLIB_CHECK(ended);
    XSFLIB_CHECK(made == "64");
    XSFLIB_CHECK(!after);
    ImGui::DestroyContext();
}

// A value that waits for one item drops when the fields show another item, also where the field shows the same text.
// The same item keeps it (WaitingValues). All of it happens in one frame.
void TestWaitingValuesOfAnotherItem()
{
    hokuto::testing::CreateHeadlessContext();
    constexpr ImGuiID kField = 7;
    const app::ShownItem item_1{.shown = 1};
    const app::ShownItem item_2{.shown = 2};
    app::WaitingValues<std::string> waiting;

    // Item 1 receives a value while busy, and passes it on once free.
    const std::optional<std::string> held = waiting.Pass(item_1, kField, "100", "64", true);
    const std::optional<std::string> same = waiting.Pass(item_1, kField, "100", std::nullopt, false);

    // Item 1 receives a value while busy. Item 2 shows before the program is free.
    (void)waiting.Pass(item_1, kField, "100", "64", true);
    const std::string shown = waiting.Shown(item_2, kField, "100");
    const std::optional<std::string> other = waiting.Pass(item_2, kField, "100", std::nullopt, false);

    XSFLIB_CHECK(!held);
    XSFLIB_CHECK(same == "64");
    XSFLIB_CHECK(shown == "100");
    XSFLIB_CHECK(!other);
    ImGui::DestroyContext();
}

// A value typed into the List Editor while the program was busy waits for its cell: the version, the sound, the track
// and the row. Another sound whose track and cell show the same text can't take it. Neither can the same cell after a
// frame in which the editor didn't show, such as after the browser showed another sound and closed the editor. The
// item and the cell keep the value while the editor shows them in each frame.
void TestWaitingValuesOfAnotherSound()
{
    hokuto::testing::CreateHeadlessContext();
    constexpr ImGuiID kCell = 12;
    // Two versions of the file.
    const auto version = std::make_shared<const hokuto::format::File>();
    const auto next_version = std::make_shared<const hokuto::format::File>();
    const app::ShownItem sound_1{.version = version, .shown = 1};
    const app::ShownItem cell_1 = sound_1.At({2, 0, 0});
    const app::ShownItem cell_2 = app::ShownItem{.version = version, .shown = 2}.At({2, 0, 0});
    const app::ShownItem other_version = app::ShownItem{.version = next_version, .shown = 1}.At({2, 0, 0});
    const auto frame = [](const std::function<void()>& draw)
    {
        ImGui::NewFrame();
        draw();
        hokuto::testing::EndHeadlessFrame();
    };
    app::WaitingValues<std::string> waiting;

    // The cell of sound 1 receives a value while busy, then sound 2 shows the same cell.
    std::optional<std::string> to_sound_2;
    frame([&] { (void)waiting.Pass(cell_1, kCell, "C4", "D4", true); });
    frame([&] { to_sound_2 = waiting.Pass(cell_2, kCell, "C4", std::nullopt, false); });

    // The cell receives a value while busy, and the next version shows the same cell.
    std::optional<std::string> to_version;
    frame([&] { (void)waiting.Pass(cell_1, kCell, "C4", "D4", true); });
    frame([&] { to_version = waiting.Pass(other_version, kCell, "C4", std::nullopt, false); });

    // The cell receives a value while busy, and the editor closes for a frame.
    std::optional<std::string> after_gap;
    frame([&] { (void)waiting.Pass(cell_1, kCell, "C4", "D4", true); });
    frame([] {});
    frame([&] { after_gap = waiting.Pass(cell_1, kCell, "C4", std::nullopt, false); });

    // The cell receives a value while busy, shows it in each frame, and passes it on once free.
    std::optional<std::string> kept;
    std::string shown;
    frame([&] { (void)waiting.Pass(cell_1, kCell, "C4", "D4", true); });
    frame([&] { (void)waiting.Pass(cell_1, kCell, "C4", std::nullopt, true); });
    frame(
        [&]
        {
            shown = waiting.Shown(cell_1, kCell, "C4");
            kept = waiting.Pass(cell_1, kCell, "C4", std::nullopt, false);
        });

    XSFLIB_CHECK(!to_sound_2);
    XSFLIB_CHECK(!to_version);
    XSFLIB_CHECK(!after_gap);
    XSFLIB_CHECK(shown == "D4");
    XSFLIB_CHECK(kept == "D4");
    ImGui::DestroyContext();
}

// A value typed while the program was busy refers to its version without keeping it. Once nothing keeps the version,
// the value drops, and no field receives it. The field shows its value again.
void TestWaitingValuesOfAVersionThatIsGone()
{
    hokuto::testing::CreateHeadlessContext();
    constexpr ImGuiID kCell = 12;
    auto version = std::make_shared<const hokuto::format::File>();
    const std::weak_ptr<const hokuto::format::File> watched = version;
    const app::ShownItem cell = app::ShownItem{.version = version, .shown = 1}.At({2, 0, 0});
    const auto frame = [](const std::function<void()>& draw)
    {
        ImGui::NewFrame();
        draw();
        hokuto::testing::EndHeadlessFrame();
    };
    app::WaitingValues<std::string> waiting;

    std::string shown_before;
    frame([&] { (void)waiting.Pass(cell, kCell, "C4", "D4", true); });
    frame([&] { shown_before = waiting.Shown(cell, kCell, "C4"); });
    const bool waits = !waiting.Empty();

    // The last owner of the version lets it go. The item that the value was typed for is still drawn.
    version.reset();
    std::string shown_after;
    std::optional<std::string> taken;
    bool dropped = false;
    frame(
        [&]
        {
            shown_after = waiting.Shown(cell, kCell, "C4");
            waiting.DropStale();
            dropped = waiting.Empty();
            taken = waiting.Pass(cell, kCell, "C4", std::nullopt, false);
        });

    XSFLIB_CHECK(shown_before == "D4");
    XSFLIB_CHECK(waits);
    XSFLIB_CHECK(watched.expired());
    XSFLIB_CHECK(shown_after == "C4");
    XSFLIB_CHECK(dropped);
    XSFLIB_CHECK(!taken);
    ImGui::DestroyContext();
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("widgets",
                                    []
                                    {
                                        TestCutLinesTooltip();
                                        TestFilteredComboReturn();
                                        TestBusyInfoEntry(false);
                                        TestBusyInfoEntry(true);
                                        TestWaitingValuesOfAnotherItem();
                                        TestWaitingValuesOfAnotherSound();
                                        TestWaitingValuesOfAVersionThatIsGone();
                                    });
}
