// SPDX-License-Identifier: MIT

// Tests the windows for a bank of DSE and a wave of DSE (app/bank_editor.h and app/wave_window.h). The test draws them
// without a display. The mouse and the keys reach Dear ImGui through its queue of events. Nothing else on the system
// receives them. The windows' layouts stay out of the test: a fixed series of random clicks, drags and typed values
// runs on a window, and each edit that comes back has to apply to the bank or the wave. A bank of the 3DS checks the
// release of a field's slider while the program is busy.
// It also checks a value typed into a field's slider while the program turns busy. A bank of DSE checks a key typed
// while the program turns busy.

#include "app/bank_editor.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/dse/formats.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/nds_files.h"
#include "seqlib/testing/check.h"
#include "xsflib/testing/check.h"

#include "app/item_views.h"
#include "app/theme.h"
#include "app/wave_window.h"
#include "edit/banks.h"
#include "edit/dse_banks.h"
#include "format/archive.h"
#include "format/file.h"
#include "headless_imgui.h"
#include "song/dse.h"

namespace
{

namespace app = hokuto::app;
namespace dse_files = ntdlib::testing::dse_song_files;
namespace edit = hokuto::edit;
namespace nds_files = ntdlib::testing::nds_files;
namespace song = hokuto::song;
using hokuto::format::File;

// The banks' indices in the set. The main bank comes first, then the bank of the song.
constexpr uint32_t kMainBank = 0;
constexpr uint32_t kBank = 1;

File TestFile()
{
    const dse_files::Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const dse_files::Bytes track = dse_files::Join({{1, 1, 0, 0},
                                                    {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F},
                                                    dse_files::Note(0x60, 0, 0, 0x18, 1),
                                                    {0x92, 0x18, 0x98}});
    nds_files::Image image;
    image.files = {{"SOUND/BGM/bgm.swd", dse_files::MainBankFile()},
                   {"SOUND/BGM/bgm0001.smd", dse_files::SongFile({conductor, track})},
                   {"SOUND/BGM/bgm0001.swd", dse_files::BankFile()}};

    return hokuto::format::ImportDseImage(nds_files::BuildImage(image).first);
}

std::shared_ptr<const song::DseSet> SetOf(const File& file)
{
    return song::LoadDseSet(hokuto::format::ExportArchive(file), ntdlib::dse::Build::kLater);
}

// The bytes of a split that each edit's name changes.
const std::map<std::string, std::set<std::size_t>>& FieldBytes()
{
    static const std::map<std::string, std::set<std::size_t>> kFields = {
        {"Change Keys", {edit::kDseLowKey, edit::kDseHighKey}},
        {"Change Velocities", {edit::kDseLowVelocity, edit::kDseHighVelocity}},
        {"Change Wave", {edit::kDseWave, edit::kDseWave + 1}},
        {"Change Root Key", {edit::kDseCoarseTuning}},
        {"Change Tuning", {edit::kDseTuning, edit::kDseTuning + 1}},
        {"Change Volume", {edit::kDseVolume}},
        {"Change Pan", {edit::kDsePan}},
        {"Change Key Group", {edit::kDseKeyGroup}},
        {"Change Bend Range", {edit::kDseBendRange}},
        {"Change Multiplier", {edit::kDseMultiplier}},
        {"Change Attack Level", {edit::kDseAttackLevel}},
        {"Change Attack", {edit::kDseAttack}},
        {"Change Hold", {edit::kDseHold}},
        {"Change Decay", {edit::kDseDecay}},
        {"Change Sustain", {edit::kDseSustain}},
        {"Change Decay 2", {edit::kDseDecay2}},
        {"Change Release", {edit::kDseRelease}},
    };

    return kFields;
}

// Dear ImGui without a display. Nothing draws its frames, and its textures count as made.
class Screen
{
public:
    Screen()
    {
        hokuto::testing::CreateHeadlessContext(true);
    }

    ~Screen()
    {
        ImGui::DestroyContext();
    }

    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    static app::BankEdit Draw(const app::DseBankView& view, app::BankEditorState& state, bool busy, bool* open)
    {
        ImGui::NewFrame();
        app::BankEdit edit = app::DrawDseBankEditor(view, state, app::ShownItem{}, busy, open);
        hokuto::testing::EndHeadlessFrame();

        return edit;
    }

    static app::BankEdit Draw(const app::BankView& view, app::BankEditorState& state, bool busy, bool* open)
    {
        ImGui::NewFrame();
        app::BankEdit edit = app::DrawBankEditor(view, state, app::ShownItem{}, busy, open);
        hokuto::testing::EndHeadlessFrame();

        return edit;
    }

    static app::WaveEdit Draw(const app::WaveView& view, app::WaveWindowState& state, bool busy, bool* open)
    {
        ImGui::NewFrame();
        app::WaveEdit edit = app::DrawWaveWindow(view, state, app::ShownItem{}, busy, open);
        hokuto::testing::EndHeadlessFrame();

        return edit;
    }
};

// A user at the window: each step clicks, drags, types or scrolls at a random place on it.
class User
{
public:
    User(const ImVec2& window_min, const ImVec2& window_max, uint32_t seed)
        : window_min_(window_min), window_max_(window_max), random_(seed)
    {
    }

    // Queues the next frame's events, and returns whether the step has more frames to come.
    bool Next()
    {
        if (queue_.empty())
        {
            Plan();
        }
        queue_.front()();
        queue_.erase(queue_.begin());

        return !queue_.empty();
    }

private:
    uint32_t Below(uint32_t count)
    {
        return random_() % count;
    }

    // A place inside the window, under its title bar. The window stays where it is and keeps its size.
    ImVec2 Place()
    {
        const auto along = [this](float low, float high)
        {
            return low + static_cast<float>(Below(1000)) / 1000.0f * (high - low);
        };

        return {along(window_min_.x + kMargin, window_max_.x - kMargin),
                along(window_min_.y + kTitleHeight, window_max_.y - kMargin)};
    }

    void Press(ImVec2 at, int button)
    {
        queue_.emplace_back([at] { ImGui::GetIO().AddMousePosEvent(at.x, at.y); });
        queue_.emplace_back([button] { ImGui::GetIO().AddMouseButtonEvent(button, true); });
    }

    void Release(int button)
    {
        queue_.emplace_back([button] { ImGui::GetIO().AddMouseButtonEvent(button, false); });
    }

    void Click(ImVec2 at, int button = ImGuiMouseButton_Left)
    {
        Press(at, button);
        Release(button);
    }

    void Key(ImGuiKey key)
    {
        queue_.emplace_back([key] { ImGui::GetIO().AddKeyEvent(key, true); });
        queue_.emplace_back([key] { ImGui::GetIO().AddKeyEvent(key, false); });
    }

    void Type()
    {
        for (uint32_t digits = 1 + Below(3); digits > 0; digits--)
        {
            const auto digit = static_cast<unsigned int>('0' + Below(10));
            queue_.emplace_back([digit] { ImGui::GetIO().AddInputCharacter(digit); });
        }
        Key(Below(4) == 0 ? ImGuiKey_Tab : ImGuiKey_Enter);
    }

    void Plan()
    {
        const ImVec2 at = Place();
        switch (Below(10))
        {
        case 0:
        case 1:
        case 2:
        case 3:
            Click(at);
            break;
        case 4:
            Click(at);
            Click(at);
            Type();
            break;
        case 5:
        case 6:
            Press(at, ImGuiMouseButton_Left);
            for (int moves = 0; moves < 3; moves++)
            {
                const ImVec2 to = Place();
                const ImVec2 near(at.x + (to.x - at.x) * 0.1f * static_cast<float>(moves + 1), at.y);
                queue_.emplace_back([near] { ImGui::GetIO().AddMousePosEvent(near.x, near.y); });
            }
            Release(ImGuiMouseButton_Left);
            break;
        case 7:
            Click(at);
            Type();
            break;
        case 8:
            queue_.emplace_back([at] { ImGui::GetIO().AddMousePosEvent(at.x, at.y); });
            queue_.emplace_back([this] { ImGui::GetIO().AddMouseWheelEvent(0.0f, Below(2) == 0 ? 1.0f : -1.0f); });
            break;
        default:
            Key(Below(2) == 0 ? ImGuiKey_Escape : ImGuiKey_Tab);
            break;
        }
    }

    static constexpr float kMargin = 12.0f;
    static constexpr float kTitleHeight = 30.0f;

    const ImVec2& window_min_;
    const ImVec2& window_max_;
    std::mt19937 random_;
    std::vector<std::function<void()>> queue_;
};

// The first frame of the bank editor selects the first program and its first row, makes no edit and reports the
// window's bounds. The close button sits at the right end of the title bar.
void TestFirstFrame()
{
    Screen screen;
    const File file = TestFile();
    const std::shared_ptr<const song::DseSet> set = SetOf(file);
    const app::DseBankView view = app::MakeDseBankView(*set, kBank);
    app::BankEditorState state;
    bool open = true;
    const app::BankEdit edit = Screen::Draw(view, state, false, &open);
    XSFLIB_CHECK(open);
    XSFLIB_CHECK(!edit.dse_split && edit.refused.empty());
    SEQLIB_REQUIRE(state.program.has_value());
    XSFLIB_CHECK(*state.program == view.programs.front().program);
    XSFLIB_CHECK(state.dse_row == 0);
    XSFLIB_CHECK(state.window_min.x < state.window_max.x && state.window_min.y < state.window_max.y);
    XSFLIB_CHECK(app::OverBankEditor(state, ImVec2(state.window_min.x + 1.0f, state.window_min.y + 1.0f)));
    XSFLIB_CHECK(!app::OverBankEditor(state, ImVec2(state.window_max.x + 1.0f, state.window_max.y + 1.0f)));

    // The close button is at the right end of the title bar.
    ImGui::GetIO().AddMousePosEvent(state.window_max.x - 19.0f, state.window_min.y + 13.0f);
    (void)Screen::Draw(view, state, false, &open);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    (void)Screen::Draw(view, state, false, &open);
    XSFLIB_CHECK(open);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    (void)Screen::Draw(view, state, false, &open);
    XSFLIB_CHECK(!open);
}

// A bank without programs and a program without splits show their empty states. Nothing comes back from either.
void TestEmptyViews(std::vector<app::DseProgramView> programs, const std::optional<uint32_t>& selected)
{
    Screen screen;
    app::DseBankView view;
    view.name = "empty";
    view.programs = std::move(programs);
    app::BankEditorState state;
    bool open = true;
    User user(state.window_min, state.window_max, 1);
    (void)Screen::Draw(view, state, false, &open);
    XSFLIB_CHECK(state.program == selected);
    for (int step = 0; step < 2000; step++)
    {
        (void)user.Next();
        open = true;
        const app::BankEdit edit = Screen::Draw(view, state, false, &open);
        XSFLIB_CHECK(!edit.dse_split);
    }
    XSFLIB_CHECK(state.sounding == -1 || !view.programs.empty());
}

// Random use of the window: the window reaches each kind of edit, and each edit leaves the bank as the window shows it.
void TestUse()
{
    Screen screen;
    File file = TestFile();
    std::shared_ptr<const song::DseSet> set = SetOf(file);
    app::DseBankView view = app::MakeDseBankView(*set, kBank);
    app::BankEditorState state;
    bool open = true;
    (void)Screen::Draw(view, state, false, &open);
    User user(state.window_min, state.window_max, 0x5D5E);

    std::set<std::string> names;
    std::set<uint32_t> programs;
    std::set<int> keys;
    int edits = 0;
    int refused = 0;
    for (int step = 0; step < 60000; step++)
    {
        (void)user.Next();
        open = true;
        const app::BankEdit made = Screen::Draw(view, state, false, &open);
        programs.insert(state.program.value_or(0));
        keys.insert(state.sounding);
        XSFLIB_CHECK(state.sounding >= -1 && state.sounding <= 127);
        if (!made.refused.empty())
        {
            refused++;
        }
        if (!made.dse_split)
        {
            continue;
        }

        const auto& [place, split] = *made.dse_split;
        XSFLIB_CHECK(place.bank == kBank);
        XSFLIB_CHECK(!made.name.empty() && !made.details.empty());
        names.insert(made.name);

        // An edit changes the value of its name in the row that it comes from.
        const auto split_of = [](const song::DseSet& each, const edit::DseSplitPlace& at) -> const edit::DseSplit&
        {
            return each.banks.at(kBank).bank->instruments->programs.at(at.program)->splits.at(at.split);
        };
        const edit::DseSplit& before = split_of(*set, place);
        XSFLIB_CHECK(before != split);
        const auto field = FieldBytes().find(made.name);
        SEQLIB_REQUIRE(field != FieldBytes().end());
        for (std::size_t i = 0; i < before.size(); i++)
        {
            XSFLIB_CHECK(before[i] == split[i] || field->second.contains(i));
        }
        try
        {
            File next = edit::WithDseSplit(file, *set, place, split);
            file = std::move(next);
        }
        catch (const std::invalid_argument&)
        {
            // Only a typed range can fall outside what the bank accepts. The menu of waves lists the bank's waves.
            XSFLIB_CHECK(made.name == "Change Keys" || made.name == "Change Velocities");
            refused++;
            continue;
        }
        edits++;
        set = SetOf(file);
        view = app::MakeDseBankView(*set, kBank);

        // The bank contains what the window returned. The copies of the ranges follow them, and the root key's byte
        // follows the coarse tuning.
        const edit::DseSplit& held = split_of(*set, place);
        for (std::size_t i = 0; i < held.size(); i++)
        {
            const bool follows = i == edit::kDseLowKey + 2 || i == edit::kDseHighKey + 2 ||
                                 i == edit::kDseLowVelocity + 2 || i == edit::kDseHighVelocity + 2 ||
                                 i == edit::kDseRootKey;
            XSFLIB_CHECK(follows || held[i] == split[i]);
        }
    }

    XSFLIB_CHECK(names.size() == FieldBytes().size());
    XSFLIB_CHECK(edits > 100);
    XSFLIB_CHECK(refused > 0);
    XSFLIB_CHECK(programs.size() == view.programs.size());
    XSFLIB_CHECK(keys.size() > 4);
}

// Random use of a wave's window: each new rate and loop that the window returns is written to the table of the bank. A
// wave's encoding and its end stay. Replace… is enabled.
void TestWaveUse()
{
    Screen screen;
    File file = TestFile();
    std::shared_ptr<const song::DseSet> set = SetOf(file);
    constexpr uint32_t kWave = 0;
    app::WaveView view = app::MakeDseWaveView(*set, kMainBank, kWave);
    const ntdlib::sar::WaveEncoding encoding = view.values.encoding;
    const uint32_t end = view.values.loop_end;
    app::WaveWindowState state;
    bool open = true;
    (void)Screen::Draw(view, state, false, &open);
    const ImGuiWindow* window = ImGui::FindWindowByName("###WaveProperties");
    SEQLIB_REQUIRE(window != nullptr);
    const ImVec2 window_min = window->Pos;
    const ImVec2 window_max(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
    User user(window_min, window_max, 0x5D5F);
    int edits = 0;
    int refused = 0;
    int plays = 0;
    int replaces = 0;
    std::set<std::string> names;
    for (int step = 0; step < 60000; step++)
    {
        (void)user.Next();
        open = true;
        const app::WaveEdit made = Screen::Draw(view, state, false, &open);
        plays += made.play ? 1 : 0;
        refused += made.refused.empty() ? 0 : 1;
        replaces += made.replace ? 1 : 0;
        if (!made.values)
        {
            continue;
        }

        XSFLIB_CHECK(!made.name.empty());
        XSFLIB_CHECK(made.values->encoding == encoding);
        XSFLIB_CHECK(made.values->loop_end == end);
        names.insert(made.name);
        const edit::DseWaveValues values{.rate = made.values->rate,
                                         .loop = made.values->loop,
                                         .loop_start = made.values->loop ? made.values->loop_start : 0};
        try
        {
            File next = edit::WithDseWave(file, *set, kMainBank, kWave, values);
            file = std::move(next);
        }
        catch (const std::invalid_argument&)
        {
            refused++;
            continue;
        }
        edits++;
        set = SetOf(file);
        view = app::MakeDseWaveView(*set, kMainBank, kWave);
        XSFLIB_CHECK(view.values.rate == values.rate);
        XSFLIB_CHECK(view.values.loop == values.loop);
        XSFLIB_CHECK(view.values.loop_start == values.loop_start);
    }
    XSFLIB_CHECK(names.contains("Change Rate") && names.contains("Change Loop") && names.contains("Change Loop Start"));
    XSFLIB_CHECK(refused > 0);
    XSFLIB_CHECK(edits > 10);
    XSFLIB_CHECK(plays > 0);
    XSFLIB_CHECK(replaces > 0);
}

// While the program builds a set, the window accepts no edit and sounds no field's value.
void TestBusy()
{
    Screen screen;
    const File file = TestFile();
    const std::shared_ptr<const song::DseSet> set = SetOf(file);
    const app::DseBankView view = app::MakeDseBankView(*set, kBank);
    app::BankEditorState state;
    bool open = true;
    (void)Screen::Draw(view, state, true, &open);
    User user(state.window_min, state.window_max, 0x5D5E);
    for (int step = 0; step < 20000; step++)
    {
        (void)user.Next();
        open = true;
        const app::BankEdit made = Screen::Draw(view, state, true, &open);
        XSFLIB_CHECK(!made.dse_split);
    }
}

// Opens the wave window's box that shows `value`: clicks each place in the lower half of the window until a box opens
// with that text, and closes any other box with Escape. Returns whether the box opened.
bool OpenWaveBox(const app::WaveView& view, app::WaveWindowState& state, const std::string& value)
{
    const ImGuiWindow* window = ImGui::FindWindowByName("###WaveProperties");
    if (window == nullptr)
    {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    bool open = true;
    const float middle = window->Pos.y + window->Size.y * 0.5f;
    for (float y = middle; y < window->Pos.y + window->Size.y - 12.0f; y += 6.0f)
    {
        for (float x = window->Pos.x + 12.0f; x < window->Pos.x + window->Size.x - 12.0f; x += 12.0f)
        {
            io.AddMousePosEvent(x, y);
            (void)Screen::Draw(view, state, false, &open);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            (void)Screen::Draw(view, state, false, &open);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            (void)Screen::Draw(view, state, false, &open);
            (void)Screen::Draw(view, state, false, &open);
            if (state.typing.box != 0 && state.typing.text == value)
            {
                return true;
            }
            if (state.typing.box != 0)
            {
                io.AddKeyEvent(ImGuiKey_Escape, true);
                (void)Screen::Draw(view, state, false, &open);
                io.AddKeyEvent(ImGuiKey_Escape, false);
                (void)Screen::Draw(view, state, false, &open);
            }
        }
    }

    return false;
}

// A value typed into the wave window's field while the program turns busy waits, and comes back as an edit once the
// program is free. Return ends the field while the program is busy. With `hover`, the pointer rests on the field first,
// and the field stays open. Dear ImGui ignores the keys of a disabled field, and a disabled field under the pointer
// ends.
void TestTypedWhileBusy(bool hover)
{
    Screen screen;
    const File file = TestFile();
    const std::shared_ptr<const song::DseSet> set = SetOf(file);
    const app::WaveView view = app::MakeDseWaveView(*set, kMainBank, 0);
    app::WaveWindowState state;
    bool open = true;
    (void)Screen::Draw(view, state, false, &open);
    const std::string rate = std::to_string(view.values.rate);
    SEQLIB_REQUIRE(OpenWaveBox(view, state, rate));
    const uint32_t typed = view.values.rate == 16000 ? 22050 : 16000;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 field = io.MousePos;
    io.AddMousePosEvent(1.0f, 1.0f);
    (void)Screen::Draw(view, state, false, &open);

    // The program turns busy while the user types and ends the field.
    std::vector<app::WaveEdit> busy_edits;
    const auto busy_frame = [&](const std::function<void()>& event)
    {
        event();
        open = true;
        busy_edits.push_back(Screen::Draw(view, state, true, &open));
    };
    busy_frame([] {});
    for (const char c : std::to_string(typed))
    {
        busy_frame([c, &io] { io.AddInputCharacter(static_cast<unsigned int>(c)); });
    }
    bool open_under_pointer = true;
    if (hover)
    {
        busy_frame([&io, field] { io.AddMousePosEvent(field.x, field.y); });
        busy_frame([] {});
        open_under_pointer = state.typing.box != 0;
    }
    busy_frame([&io] { io.AddKeyEvent(ImGuiKey_Enter, true); });
    busy_frame([&io] { io.AddKeyEvent(ImGuiKey_Enter, false); });
    const bool field_ended = state.typing.box == 0;

    // The program is free again.
    std::optional<app::WaveEdit> made;
    for (int frame = 0; frame < 3 && !made; frame++)
    {
        app::WaveEdit edit = Screen::Draw(view, state, false, &open);
        if (edit.values)
        {
            made = std::move(edit);
        }
    }

    for (const app::WaveEdit& edit : busy_edits)
    {
        XSFLIB_CHECK(!edit.values);
    }
    XSFLIB_CHECK(open_under_pointer);
    XSFLIB_CHECK(field_ended);
    SEQLIB_REQUIRE(made.has_value());
    XSFLIB_CHECK(made->name == "Change Rate" && made->values->rate == typed);
}

// A stream of two channels in DSP-ADPCM, with 120,000 samples of silence, looped from sample 14,336 to its end at
// 100,000, as Stream Properties shows it on the Wii U.
app::WaveView StreamView()
{
    app::WaveView view;
    view.name = "STRM_TEST";
    view.stream = true;
    view.values = {.encoding = ntdlib::sar::WaveEncoding::kDspAdpcm,
                   .rate = 32000,
                   .loop = true,
                   .loop_start = 14336,
                   .loop_end = 100000};
    view.held = 120000;
    view.samples.assign(2, std::vector<int16_t>(view.held));
    view.peaks = app::PeaksOf(view.samples);
    view.encodings = {ntdlib::sar::WaveEncoding::kDspAdpcm, ntdlib::sar::WaveEncoding::kPcm16};

    return view;
}

// Stream Properties says where a loop's start moves, in the Replace window's words. A typed start moves to a block of
// 14,336 samples in DSP-ADPCM (ntdlib's StreamBlockSamples). 16-bit PCM makes the stream again in blocks of 4,096
// samples. The start then moves forward to the next block, and the end moves with it.
void TestStreamLoopNotes()
{
    Screen screen;
    app::WaveView view = StreamView();
    app::WaveWindowState state;
    bool open = true;
    (void)Screen::Draw(view, state, false, &open);
    SEQLIB_REQUIRE(OpenWaveBox(view, state, "14336"));

    // A typed start of 70,000 moves to the fifth block.
    ImGuiIO& io = ImGui::GetIO();
    std::optional<app::WaveEdit> typed;
    for (const char c : std::string("70000"))
    {
        io.AddInputCharacter(static_cast<unsigned int>(c));
        (void)Screen::Draw(view, state, false, &open);
    }
    io.AddKeyEvent(ImGuiKey_Enter, true);
    for (int frame = 0; frame < 3 && !typed; frame++)
    {
        app::WaveEdit edit = Screen::Draw(view, state, false, &open);
        if (edit.values)
        {
            typed = std::move(edit);
        }
        io.AddKeyEvent(ImGuiKey_Enter, false);
    }

    // The program makes the edit, and 16-bit PCM follows.
    SEQLIB_REQUIRE(typed.has_value());
    view.values = *typed->values;
    const std::vector<std::string> encoding_notes =
        app::EncodingNotes(view, view.values, ntdlib::sar::WaveEncoding::kPcm16);

    const std::vector<std::string> kTypedNotes = {
        "Stream loops must start on a block boundary. The loop start moves from sample 70,000 to 71,680."};
    const std::vector<std::string> kEncodingNotes = {
        "Stream loops must start on a block boundary. The loop start moves from sample 71,680 to 73,728. The 2,048 "
        "samples in between are repeated after the loop end. The loop sounds the same.",
        "The end moves from sample 100,000 to 102,048."};
    XSFLIB_CHECK(typed->name == "Change Loop Start" && typed->values->loop_start == 71680);
    XSFLIB_CHECK(typed->notes == kTypedNotes);
    XSFLIB_CHECK(encoding_notes == kEncodingNotes);
}

// A bank of the 3DS with one program, whose one region plays the first wave over every key and velocity.
app::BankView CtrBankView()
{
    app::BankView view;
    view.name = "BANK_TEST";
    view.model.flavor = ntdlib::sar::Flavor::kCtr;
    view.model.waves = {{0, 0}};
    view.model.instruments.resize(1);
    view.model.instruments[0].emplace().key_regions = {{0, 127, {ntdlib::sar::BankFile::Region()}}};
    view.entries = {"WARC_TEST, wave 0"};
    view.waves = {{0, 0}};
    view.wave_names = {"WARC_TEST, wave 0"};
    view.engine = hokuto::format::Engine::k3ds;

    return view;
}

// A field's slider that the user lets go of while the program is busy makes no edit. The fields then show the selected
// region again, as on the GBA, the DS and DSE.
void TestSliderReleasedWhileBusy()
{
    Screen screen;
    const app::BankView view = CtrBankView();
    app::BankEditorState state;
    bool open = true;
    (void)Screen::Draw(view, state, false, &open);
    SEQLIB_REQUIRE(state.program == 0u && state.window_min.x < state.window_max.x);
    state.region = std::pair(0, 0);

    // Press at places in the lower half of the window and drag to the right, until `state.held` has a value.
    ImGuiIO& io = ImGui::GetIO();
    const float middle = (state.window_min.y + state.window_max.y) * 0.5f;
    bool holding = false;
    for (float y = middle; y < state.window_max.y - 12.0f && !holding; y += 6.0f)
    {
        for (float x = state.window_min.x + 12.0f; x < state.window_max.x - 60.0f && !holding; x += 24.0f)
        {
            io.AddMousePosEvent(x, y);
            (void)Screen::Draw(view, state, false, &open);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            (void)Screen::Draw(view, state, false, &open);
            io.AddMousePosEvent(x + 24.0f, y);
            (void)Screen::Draw(view, state, false, &open);
            holding = state.held.has_value();
            if (!holding)
            {
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                (void)Screen::Draw(view, state, false, &open);
            }
        }
    }
    SEQLIB_REQUIRE(holding);

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    const app::BankEdit released = Screen::Draw(view, state, true, &open);
    const app::BankEdit after = Screen::Draw(view, state, true, &open);

    XSFLIB_CHECK(!released.model && !after.model);
    XSFLIB_CHECK(!state.held);
}

// Whether a slider's field is open: Dear ImGui's text input replaces the slider (ValueSlider).
bool SliderFieldOpen()
{
    const ImGuiContext& context = *ImGui::GetCurrentContext();
    return context.TempInputId != 0 && context.ActiveId == context.TempInputId;
}

// Opens the field of a slider in the lower half of the bank editor's window: double-clicks each place until a field
// opens, and closes anything else that opens with Escape. Returns where the field is, if one opened.
std::optional<ImVec2> OpenSliderField(const app::BankView& view, app::BankEditorState& state)
{
    ImGuiIO& io = ImGui::GetIO();
    bool open = true;
    const float middle = (state.window_min.y + state.window_max.y) * 0.5f;
    for (float y = middle; y < state.window_max.y - 12.0f; y += 6.0f)
    {
        for (float x = state.window_min.x + 12.0f; x < state.window_max.x - 12.0f; x += 24.0f)
        {
            io.AddMousePosEvent(x, y);
            (void)Screen::Draw(view, state, false, &open);
            for (int click = 0; click < 2; click++)
            {
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                (void)Screen::Draw(view, state, false, &open);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                (void)Screen::Draw(view, state, false, &open);
            }
            (void)Screen::Draw(view, state, false, &open);
            if (SliderFieldOpen())
            {
                return ImVec2(x, y);
            }

            io.AddKeyEvent(ImGuiKey_Escape, true);
            (void)Screen::Draw(view, state, false, &open);
            io.AddKeyEvent(ImGuiKey_Escape, false);
            (void)Screen::Draw(view, state, false, &open);
            state.held.reset();
        }
    }

    return std::nullopt;
}

// A value typed into a field's slider while the program turns busy waits, and comes back as an edit once the program is
// free, as in the wave window (TestTypedWhileBusy). With `hover`, the pointer rests on the field before Return, and the
// field stays open. With `other`, the instrument has a second region with the same values, and the user selects it
// while the program is busy. The typed value then drops, and no edit comes.
void TestTypedSliderWhileBusy(bool hover, bool other)
{
    Screen screen;
    app::BankView view = CtrBankView();
    if (other)
    {
        view.model.instruments[0]->key_regions = {{0, 63, {ntdlib::sar::BankFile::Region()}},
                                                  {64, 127, {ntdlib::sar::BankFile::Region()}}};
    }
    app::BankEditorState state;
    bool open = true;
    (void)Screen::Draw(view, state, false, &open);
    SEQLIB_REQUIRE(state.program == 0u && state.window_min.x < state.window_max.x);
    state.region = std::pair(0, 0);
    (void)Screen::Draw(view, state, false, &open);
    const std::optional<ImVec2> field = OpenSliderField(view, state);
    SEQLIB_REQUIRE(field.has_value());
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(1.0f, 1.0f);
    (void)Screen::Draw(view, state, false, &open);

    // The program turns busy while the user types and ends the field.
    std::vector<app::BankEdit> busy_edits;
    const auto busy_frame = [&](const std::function<void()>& event)
    {
        event();
        busy_edits.push_back(Screen::Draw(view, state, true, &open));
    };
    busy_frame([] {});
    busy_frame([&io] { io.AddInputCharacter('3'); });
    bool open_under_pointer = true;
    if (hover)
    {
        busy_frame([&io, field] { io.AddMousePosEvent(field->x, field->y); });
        busy_frame([] {});
        open_under_pointer = SliderFieldOpen();
    }
    busy_frame([&io] { io.AddKeyEvent(ImGuiKey_Enter, true); });
    busy_frame([&io] { io.AddKeyEvent(ImGuiKey_Enter, false); });
    busy_frame([] {});
    const bool field_ended = !SliderFieldOpen();
    if (other)
    {
        state.region = std::pair(64, 0);
        busy_frame([] {});
    }

    // The program is free again.
    std::optional<app::BankEdit> made;
    for (int frame = 0; frame < 3 && !made; frame++)
    {
        app::BankEdit edit = Screen::Draw(view, state, false, &open);
        if (edit.model)
        {
            made = std::move(edit);
        }
    }

    for (const app::BankEdit& edit : busy_edits)
    {
        XSFLIB_CHECK(!edit.model);
    }
    XSFLIB_CHECK(open_under_pointer);
    XSFLIB_CHECK(field_ended);
    if (other)
    {
        XSFLIB_CHECK(!made.has_value());
        return;
    }
    SEQLIB_REQUIRE(made.has_value());
    XSFLIB_CHECK(made->name.starts_with("Change "));

    // The edit sets the typed 3 in the field that it names, and changes nothing else.
    SEQLIB_REQUIRE(made->model && made->model->instruments.size() == 1 && made->model->instruments[0]);
    const std::vector<edit::Region> before = edit::RegionsOf(*view.model.instruments[0]);
    const std::vector<edit::Region> after = edit::RegionsOf(*made->model->instruments[0]);
    SEQLIB_REQUIRE(before.size() == 1 && after.size() == 1);
    const std::string field_name = made->name.substr(std::string_view("Change ").size());
    edit::Region expected = before[0];
    ntdlib::sar::VelocityRegion& values = expected.values;
    const std::map<std::string, uint8_t*> bytes = {
        {"Root Key", &values.original_key}, {"Volume", &values.volume},         {"Pan", &values.pan},
        {"Key Group", &values.key_group},   {"Attack", &values.adshr.attack},   {"Hold", &values.adshr.hold},
        {"Decay", &values.adshr.decay},     {"Sustain", &values.adshr.sustain}, {"Release", &values.adshr.release}};
    if (field_name == "Tuning")
    {
        values.pitch = static_cast<float>(std::exp2(3.0 / 1200.0));
    }
    else
    {
        SEQLIB_REQUIRE(bytes.contains(field_name));
        *bytes.at(field_name) = 3;
    }
    XSFLIB_CHECK(after[0] == expected);
}

// Whether a field of the keys or the velocities of a DSE split is open for typing.
bool RangeFieldOpen(const app::BankEditorState& state)
{
    return state.typing_key || state.typing_last || state.typing_velocity || state.typing_top_velocity;
}

// A key or a velocity typed into a field of a DSE split while the program turns busy waits, and comes back as an edit
// once the program is free. With `hover`, the pointer rests on the field before Return, and the field stays open.
// With `other`, the user selects another split with the same values while the program is busy. The typed value then
// drops, and no edit comes.
void TestTypedKeyWhileBusy(bool hover, bool other)
{
    Screen screen;
    const File file = TestFile();
    const std::shared_ptr<const song::DseSet> set = SetOf(file);
    app::DseBankView view = app::MakeDseBankView(*set, kBank);
    app::BankEditorState state;
    bool open = true;
    (void)Screen::Draw(view, state, false, &open);

    // Click each place in the window until a field of the keys or the velocities of the split is open for typing.
    ImGuiIO& io = ImGui::GetIO();
    std::optional<ImVec2> field;
    for (float y = state.window_min.y + 30.0f; y < state.window_max.y - 12.0f && !field; y += 6.0f)
    {
        for (float x = state.window_min.x + 12.0f; x < state.window_max.x - 12.0f && !field; x += 12.0f)
        {
            io.AddMousePosEvent(x, y);
            (void)Screen::Draw(view, state, false, &open);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            (void)Screen::Draw(view, state, false, &open);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            (void)Screen::Draw(view, state, false, &open);
            if (RangeFieldOpen(state))
            {
                field = ImVec2(x, y);
            }
        }
    }
    SEQLIB_REQUIRE(field.has_value());
    io.AddMousePosEvent(1.0f, 1.0f);
    (void)Screen::Draw(view, state, false, &open);

    // The program turns busy while the user types and ends the field. The new value replaces the field's text.
    std::vector<app::BankEdit> busy_edits;
    const auto busy_frame = [&](const std::function<void()>& event)
    {
        event();
        busy_edits.push_back(Screen::Draw(view, state, true, &open));
    };
    busy_frame([] {});
    busy_frame([&io] { io.AddKeyEvent(ImGuiKey_End, true); });
    busy_frame([&io] { io.AddKeyEvent(ImGuiKey_End, false); });
    for (int erased = 0; erased < 3; erased++)
    {
        busy_frame([&io] { io.AddKeyEvent(ImGuiKey_Backspace, true); });
        busy_frame([&io] { io.AddKeyEvent(ImGuiKey_Backspace, false); });
    }
    busy_frame([&io] { io.AddInputCharacter('5'); });
    bool open_under_pointer = true;
    if (hover)
    {
        busy_frame([&io, field] { io.AddMousePosEvent(field->x, field->y); });
        busy_frame([] {});
        open_under_pointer = RangeFieldOpen(state);
    }
    busy_frame([&io] { io.AddKeyEvent(ImGuiKey_Enter, true); });
    busy_frame([&io] { io.AddKeyEvent(ImGuiKey_Enter, false); });
    busy_frame([] {});
    const bool field_ended = !RangeFieldOpen(state);
    if (other)
    {
        // The selected program gets a copy of the selected split, and the user selects the copy.
        SEQLIB_REQUIRE(state.program.has_value() && state.dse_row.has_value());
        const auto program = std::ranges::find(view.programs, *state.program, &app::DseProgramView::program);
        SEQLIB_REQUIRE(program != view.programs.end() && *state.dse_row < program->rows.size());
        app::DseSplitRow copy = program->rows[*state.dse_row];
        copy.split = program->rows.size();
        program->rows.push_back(copy);
        state.dse_row = copy.split;
        busy_frame([] {});
    }

    // The program is free again.
    std::optional<app::BankEdit> made;
    for (int frame = 0; frame < 3 && !made; frame++)
    {
        app::BankEdit edit = Screen::Draw(view, state, false, &open);
        if (edit.dse_split)
        {
            made = std::move(edit);
        }
    }

    for (const app::BankEdit& edit : busy_edits)
    {
        XSFLIB_CHECK(!edit.dse_split);
    }
    XSFLIB_CHECK(open_under_pointer);
    XSFLIB_CHECK(field_ended);
    if (other)
    {
        XSFLIB_CHECK(!made.has_value());
        return;
    }
    SEQLIB_REQUIRE(made.has_value());
    XSFLIB_CHECK(made->name == "Change Keys" || made->name == "Change Velocities");
    const edit::DseSplit& split = made->dse_split->second;
    XSFLIB_CHECK(split[edit::kDseLowKey] == 5 || split[edit::kDseHighKey] == 5 || split[edit::kDseLowVelocity] == 5 ||
                 split[edit::kDseHighVelocity] == 5);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("bank_editor",
                                    []
                                    {
                                        TestFirstFrame();
                                        TestEmptyViews({}, std::nullopt);
                                        TestEmptyViews({{.program = 7, .rows = {}}}, 7);
                                        TestUse();
                                        TestWaveUse();
                                        TestBusy();
                                        TestTypedWhileBusy(false);
                                        TestTypedWhileBusy(true);
                                        TestStreamLoopNotes();
                                        TestSliderReleasedWhileBusy();
                                        TestTypedSliderWhileBusy(false, false);
                                        TestTypedSliderWhileBusy(true, false);
                                        TestTypedSliderWhileBusy(false, true);
                                        TestTypedKeyWhileBusy(false, false);
                                        TestTypedKeyWhileBusy(true, false);
                                        TestTypedKeyWhileBusy(false, true);
                                    });
}
