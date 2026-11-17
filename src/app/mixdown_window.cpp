// SPDX-License-Identifier: MIT

#include "app/mixdown_window.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

#include "seqlib/file_name.h"

#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// The names of the tokens of a naming scheme, in NameToken's order, and the choice that adds none.
constexpr std::array<const char*, 5> kTokenNames = {"Project", "Song", "Number", "Track", "Text"};

// The names of the file types, in MixdownFileType's order.
constexpr std::array<const char*, 3> kTypeNames = {"Wave", "MIDI and SoundFont 2", "MIDI and SFZ"};

// The names of what a stem keeps, in StemEffects's order.
constexpr std::array<const char*, 3> kStemEffectNames = {"Dry", "With Effects", "Through Stereo Out"};

// The names of the sample formats, in SampleFormat's order.
constexpr std::array<const char*, 3> kFormatNames = {"16-bit", "24-bit", "32-bit float"};

// The rate of `hertz` as the window shows it: "48 kHz", "44.1 kHz", "32.728 kHz".
std::string RateText(int hertz)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%g kHz", hertz / 1000.0);
    return text;
}

// Grows a string that a text field edits: Dear ImGui asks for more room as the user types.
int ResizeString(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = text->data();
    }

    return 0;
}

// A text field that edits `text`. Returns true when the text changed.
bool StringInput(const char* id, std::string* text)
{
    return ImGui::InputText(id, text->data(), text->capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeString,
                            text);
}

// A combo of the names in `names`, with `*index` chosen. Returns true when the user chose another.
template <std::size_t N>
bool NamesCombo(const char* id, const std::array<const char*, N>& names, int* index)
{
    bool changed = false;
    if (ImGui::BeginCombo(id, names[static_cast<std::size_t>(*index)]))
    {
        for (std::size_t i = 0; i < N; i++)
        {
            if (ImGui::Selectable(names[i], static_cast<int>(i) == *index))
            {
                changed = static_cast<int>(i) != *index;
                *index = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }

    return changed;
}

// Begins a table of settings with the labels in the left column.
bool BeginSettings(const char* id)
{
    if (!ImGui::BeginTable(id, 2))
    {
        return false;
    }

    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(110.0f));
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// The list of songs, with a search field, check boxes, All and None, and each song's length and loop. All and None
// change the songs that the search shows.
void DrawSongs(MixdownView& view, float height)
{
    DrawDialogHeading(ImGui::GetWindowDrawList(), "Songs");
    std::vector<std::size_t> shown;
    for (std::size_t i = 0; i < view.songs.size(); i++)
    {
        if (MatchesFilter(view.songs[i], view.filter))
        {
            shown.push_back(i);
        }
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Search", view.filter.data(), view.filter.capacity() + 1,
                             ImGuiInputTextFlags_CallbackResize, ResizeString, &view.filter);
    if (ImGui::Button("All"))
    {
        for (const std::size_t i : shown)
        {
            view.songs[i].chosen = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("None"))
    {
        for (const std::size_t i : shown)
        {
            view.songs[i].chosen = false;
        }
    }
    ImGui::SameLine();
    const auto chosen = std::ranges::count_if(view.songs, [](const MixdownSong& song) { return song.chosen; });
    const auto looped = std::ranges::count_if(
        view.songs, [](const MixdownSong& song) { return song.chosen && song.seconds && song.loops; });
    MutedText((std::to_string(chosen) + " of " + std::to_string(view.songs.size()) + " chosen, " +
               std::to_string(looped) + " with a loop")
                  .c_str());

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter;
    if (!ImGui::BeginTable("##songs", 3, kFlags, ImVec2(0.0f, height)))
    {
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, Px(80.0f));
    ImGui::TableSetupColumn("Loop", ImGuiTableColumnFlags_WidthFixed, Px(40.0f));
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
        {
            const std::size_t index = shown[static_cast<std::size_t>(row)];
            MixdownSong& song = view.songs[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox(song.name.c_str(), &song.chosen);
            ImGui::TableSetColumnIndex(1);
            if (!song.error.empty())
            {
                MutedText("Can't read");
                ItemTooltip(song.error);
            }
            else if (song.seconds)
            {
                ImGui::TextUnformatted(MixdownLengthText(*song.seconds).c_str());
                if (*song.seconds >= song::kMixdownLimitSeconds)
                {
                    ItemTooltip("Plays on past 10 minutes, and fades there.");
                }
                ImGui::TableSetColumnIndex(2);
                if (song.loops)
                {
                    ImGui::TextUnformatted("Yes");
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

// The Channels section: the whole mix, a stem for each track, and what the stems keep.
bool DrawChannels(MixdownSetup& setup)
{
    bool changed = false;
    DrawDialogHeading(ImGui::GetWindowDrawList(), "Channels");
    if (!BeginSettings("##channels"))
    {
        return false;
    }

    SettingLabel("Export");
    ImGui::BeginDisabled(setup.type != MixdownFileType::kWave);
    changed |= CheckBox("Mix", &setup.mix);
    ItemTooltip("The whole song through Stereo Out, as Hokuto plays it.");
    ImGui::SameLine();
    changed |= CheckBox("Stems", &setup.stems);
    ItemTooltip("A file for each track that plays notes, with the other tracks muted.");
    SettingLabel("Stems");
    ImGui::BeginDisabled(!setup.stems);
    int effects = static_cast<int>(setup.settings.stem_effects);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (NamesCombo("##stem effects", kStemEffectNames, &effects))
    {
        setup.settings.stem_effects = static_cast<song::StemEffects>(effects);
        changed = true;
    }
    ItemTooltip("What each stem keeps: no effects, the track's sends, or the sends and the Stereo Out fader.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::EndTable();

    return changed;
}

// The Range section: the passes of the loop, the fade, the tails' limit and the seamless loop.
bool DrawRange(MixdownSetup& setup, const MixdownView& view)
{
    bool changed = false;
    song::MixdownSettings& settings = setup.settings;
    DrawDialogHeading(ImGui::GetWindowDrawList(), "Range");
    if (!BeginSettings("##range"))
    {
        return false;
    }

    ImGui::BeginDisabled(setup.type != MixdownFileType::kWave);
    SettingLabel("Loops");
    ImGui::BeginDisabled(settings.seamless);
    ImGui::SetNextItemWidth(Px(120.0f));
    changed |= ImGui::InputInt("##loops", &settings.loops);
    settings.loops = std::clamp(settings.loops, 1, 99);
    ItemTooltip("Times a song that loops plays its loop before the fade. A song without a loop plays once.");
    SettingLabel("Fade Out");
    auto fade = static_cast<float>(settings.fade_seconds);
    ImGui::SetNextItemWidth(Px(120.0f));
    if (ImGui::InputFloat("##fade", &fade, 1.0f, 5.0f, "%.1f s"))
    {
        settings.fade_seconds = std::clamp(static_cast<double>(fade), 0.0, 60.0);
        changed = true;
    }
    ItemTooltip("Length of the fade after the last loop. A song without a loop ends at its tail limit.");
    ImGui::EndDisabled();
    SettingLabel("Tail Limit");
    auto tail = static_cast<float>(settings.tail_seconds);
    ImGui::SetNextItemWidth(Px(120.0f));
    if (ImGui::InputFloat("##tail", &tail, 1.0f, 5.0f, "%.1f s"))
    {
        settings.tail_seconds = std::clamp(static_cast<double>(tail), 0.0, 60.0);
        changed = true;
    }
    ItemTooltip(
        "A song that ends plays until it's silent, for at most this long after its end. Tails that still "
        "sound there fade over the last second.");
    SettingLabel("");
    changed |= CheckBox("Seamless Loop", &settings.seamless);
    ItemTooltip("Renders the loop twice without a fade, and marks the second pass as a loop in the file.");
    ImGui::EndDisabled();
    SettingLabel("Mixing");
    int mixing = setup.mixing ? (*setup.mixing == song::Mixing::kStrict ? 1 : 2) : 0;
    constexpr std::array<const char*, 3> kMixings = {"As Set for Each Song", "Strict", "Enhanced"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##mixing", kMixings[static_cast<std::size_t>(mixing)]))
    {
        for (int i = 0; i < 3; i++)
        {
            const bool refused = i == 2 && !view.enhanced;
            if (ImGui::Selectable(kMixings[static_cast<std::size_t>(i)], i == mixing,
                                  refused ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None))
            {
                changed |= i != mixing;
                setup.mixing = i == 0   ? std::nullopt
                               : i == 1 ? std::optional(song::Mixing::kStrict)
                                        : std::optional(song::Mixing::kEnhanced);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndTable();

    return changed;
}

// A combo of a naming scheme's token at `index` of `scheme`, with a choice that removes it. Returns whether the
// scheme changed. A removed token leaves `index` past the scheme's end.
bool TokenCombo(song::NamingScheme& scheme, std::size_t index)
{
    const bool added = index >= scheme.tokens.size();
    const char* shown = added ? "Add…" : kTokenNames[static_cast<std::size_t>(scheme.tokens[index])];
    bool changed = false;
    ImGui::PushID(static_cast<int>(index));
    ImGui::SetNextItemWidth(Px(90.0f));
    if (ImGui::BeginCombo("##token", shown))
    {
        for (std::size_t i = 0; i < kTokenNames.size(); i++)
        {
            if (ImGui::Selectable(kTokenNames[i], !added && static_cast<std::size_t>(scheme.tokens[index]) == i))
            {
                if (added)
                {
                    scheme.tokens.push_back(static_cast<song::NameToken>(i));
                }
                else
                {
                    scheme.tokens[index] = static_cast<song::NameToken>(i);
                }
                changed = true;
            }
        }
        if (!added && ImGui::Selectable("Remove"))
        {
            scheme.tokens.erase(scheme.tokens.begin() + static_cast<std::ptrdiff_t>(index));
            changed = true;
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();

    return changed;
}

// The File Location section: the naming scheme with its example, the folder, and what happens to a file that's there
// already.
MixdownChoice DrawLocation(MixdownView& view)
{
    MixdownChoice choice = MixdownChoice::kNone;
    MixdownSetup& setup = view.setup;
    DrawDialogHeading(ImGui::GetWindowDrawList(), "File Location");
    if (!BeginSettings("##location"))
    {
        return choice;
    }

    SettingLabel("Name");
    song::NamingScheme& scheme = setup.naming;
    for (std::size_t i = 0; i <= scheme.tokens.size() && i < 6; i++)
    {
        TokenCombo(scheme, i);
        ImGui::SameLine();
    }
    ImGui::NewLine();
    if (std::ranges::find(scheme.tokens, song::NameToken::kText) != scheme.tokens.end())
    {
        SettingLabel("Text");
        ImGui::SetNextItemWidth(-FLT_MIN);
        StringInput("##text", &scheme.text);
    }
    SettingLabel("Separator");
    ImGui::SetNextItemWidth(Px(90.0f));
    StringInput("##separator", &scheme.separator);
    SettingLabel("Example");
    MutedText(MixdownPreview(view).c_str());
    if (setup.type == MixdownFileType::kWave && setup.stems &&
        std::ranges::find(scheme.tokens, song::NameToken::kTrack) == scheme.tokens.end())
    {
        ItemTooltip("Stems add the track to the end of the name.");
    }

    SettingLabel("Path");
    const float button = ImGui::CalcTextSize("Choose…").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-(button + ImGui::GetStyle().ItemSpacing.x));
    StringInput("##folder", &setup.folder);
    ImGui::SameLine();
    if (ImGui::Button("Choose…"))
    {
        choice = MixdownChoice::kChooseFolder;
    }
    SettingLabel("Name Conflicts");
    int conflicts = setup.overwrite ? 1 : 0;
    constexpr std::array<const char*, 2> kConflicts = {"Create Unique File Name", "Always Overwrite"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (NamesCombo("##conflicts", kConflicts, &conflicts))
    {
        setup.overwrite = conflicts == 1;
    }
    ImGui::EndTable();

    return choice;
}

// The File Format section: the file type, and for WAV files, the rate, the bit depth, normalising and dither.
bool DrawFormat(MixdownView& view)
{
    bool changed = false;
    MixdownSetup& setup = view.setup;
    song::WavForm& form = setup.settings.form;
    DrawDialogHeading(ImGui::GetWindowDrawList(), "File Format");
    if (!BeginSettings("##format"))
    {
        return false;
    }

    SettingLabel("File Type");
    int type = static_cast<int>(setup.type);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##type", kTypeNames[static_cast<std::size_t>(type)]))
    {
        for (int i = 0; i < 3; i++)
        {
            const bool refused = i == 2 && !view.sfz;
            if (ImGui::Selectable(kTypeNames[static_cast<std::size_t>(i)], i == type,
                                  refused ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None))
            {
                changed |= i != type;
                setup.type = static_cast<MixdownFileType>(i);
            }
        }
        ImGui::EndCombo();
    }

    ImGui::BeginDisabled(setup.type != MixdownFileType::kWave);
    SettingLabel("Sample Rate");
    const std::string console = view.console_rate > 0 ? RateText(view.console_rate) + " (console)" : "Console Rate";
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##rate", form.rate == 0 ? console.c_str() : RateText(form.rate).c_str()))
    {
        if (ImGui::Selectable(console.c_str(), form.rate == 0))
        {
            form.rate = 0;
        }
        for (const int rate : kMixdownRates)
        {
            if (ImGui::Selectable(RateText(rate).c_str(), form.rate == rate))
            {
                form.rate = rate;
            }
        }
        ImGui::EndCombo();
    }
    ItemTooltip(
        "The console's rate keeps every sample as the mixer makes it. Another rate goes through Hokuto's "
        "resampler.");
    SettingLabel("Bit Depth");
    int format = static_cast<int>(form.format);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (NamesCombo("##bits", kFormatNames, &format))
    {
        form.format = static_cast<song::SampleFormat>(format);
    }
    SettingLabel("");
    CheckBox("Normalise", &form.normalise);
    ItemTooltip("Scales each file so that its peak reaches 0 dBFS.");
    ImGui::SameLine();
    ImGui::BeginDisabled(form.format != song::SampleFormat::kPcm16);
    CheckBox("Dither", &form.dither);
    ItemTooltip("Adds TPDF dither to 16-bit files.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::EndTable();

    return changed;
}

// The Export Queue: a row for each export, with its progress, and the queue's buttons.
MixdownChoice DrawQueue(MixdownView& view, int console_rate)
{
    MixdownChoice choice = MixdownChoice::kNone;
    DrawDialogHeading(ImGui::GetWindowDrawList(), "Export Queue");
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##queue", 5, kFlags, ImVec2(0.0f, Px(110.0f))))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Songs", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, Px(170.0f));
        ImGui::TableSetupColumn("Channels", ImGuiTableColumnFlags_WidthFixed, Px(170.0f));
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, Px(130.0f));
        ImGui::TableHeadersRow();
        for (const MixdownJob& job : view.queue)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%zu", job.songs.size());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(job.setup.folder.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(MixdownFormatText(job.setup, console_rate).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(MixdownChannelsText(job.setup).c_str());
            ImGui::TableSetColumnIndex(4);
            if (!job.status.empty())
            {
                ImGui::TextUnformatted(job.status.c_str());
                ItemTooltip(job.status);
            }
            else
            {
                ImGui::ProgressBar(MixdownJobDone(job), ImVec2(-FLT_MIN, 0.0f), job.running ? nullptr : "Waiting");
            }
        }
        ImGui::EndTable();
    }

    const bool running = view.running;
    const MixdownActions actions = QueueActions(view);
    ImGui::BeginDisabled(!actions.add);
    if (ImGui::Button("Add to Queue"))
    {
        choice = MixdownChoice::kAddToQueue;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!actions.remove);
    if (ImGui::Button("Remove All"))
    {
        choice = MixdownChoice::kRemoveAll;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    CheckBox("Keep Dialog Open", &view.keep_open);

    // The buttons at the bottom right: Close, then the main action.
    const char* start = actions.waiting ? "Start Queue Export" : "Export Audio";
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::CalcTextSize(start).x + ImGui::CalcTextSize("Close").x + style.FramePadding.x * 4.0f +
                        style.ItemSpacing.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
    if (running)
    {
        if (ImGui::Button("Stop"))
        {
            choice = MixdownChoice::kStop;
        }
    }
    else if (ImGui::Button("Close"))
    {
        choice = MixdownChoice::kClose;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!actions.start);
    if (PrimaryButton(start))
    {
        choice = MixdownChoice::kStart;
    }
    ImGui::EndDisabled();

    return choice;
}

} // namespace

void FollowSongs(MixdownView& view, const std::vector<song::BatchSound>& sounds, std::optional<uint32_t> shown)
{
    const bool first = view.songs.empty();
    std::vector<MixdownSong> songs;
    for (const song::BatchSound& sound : sounds)
    {
        const auto kept = std::ranges::find(view.songs, sound.sound, &MixdownSong::sound);
        const bool chosen = kept != view.songs.end() ? kept->chosen : first && shown == sound.sound;
        songs.push_back({.sound = sound.sound, .name = sound.name, .chosen = chosen});
    }
    view.songs = std::move(songs);
}

bool MatchesFilter(const MixdownSong& song, const std::string& filter)
{
    const auto lower = [](char c)
    {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    return filter.empty() || !std::ranges::search(song.name, filter, {}, lower, lower).empty();
}

float MixdownJobDone(const MixdownJob& job)
{
    if (job.files > 0)
    {
        return static_cast<float>(std::min(job.files_done, job.files)) / static_cast<float>(job.files);
    }

    return job.songs.empty() ? 0.0f : static_cast<float>(job.done + job.failed) / static_cast<float>(job.songs.size());
}

MixdownActions QueueActions(const MixdownView& view)
{
    const bool chosen = std::ranges::any_of(view.songs, [](const MixdownSong& song) { return song.chosen; });
    const bool ready = chosen && !view.setup.folder.empty() &&
                       (view.setup.type != MixdownFileType::kWave || view.setup.mix || view.setup.stems);
    const bool waiting = std::ranges::any_of(view.queue, [](const MixdownJob& job) { return job.status.empty(); });

    return {.add = ready && !view.running && !view.settling,
            .remove = !view.queue.empty() && !view.running,
            .start = !view.running && !view.settling && (waiting || ready),
            .waiting = waiting && !view.running};
}

std::string MixdownPreview(const MixdownView& view)
{
    const auto first = std::ranges::find_if(view.songs, [](const MixdownSong& song) { return song.chosen; });
    const MixdownSong* song = first != view.songs.end() ? &*first : view.songs.empty() ? nullptr : &view.songs[0];
    const bool wave = view.setup.type == MixdownFileType::kWave;
    song::NameParts parts = {.project = view.project,
                             .song = song != nullptr ? song->name : "",
                             .number = song != nullptr ? song->sound : 0};
    const auto file = [&]
    {
        const std::string name = song::SchemeName(view.setup.naming, parts);
        return seqlib::SafeFileName(name.empty() ? "Sound 1" : name) + (wave ? ".wav" : ".mid");
    };

    std::string example = !wave || view.setup.mix || !view.setup.stems ? file() : "";
    if (wave && view.setup.stems)
    {
        parts.track = "Track 0";
        example += (example.empty() ? "" : ", ") + file();
    }

    return example;
}

std::string MixdownFormatText(const MixdownSetup& setup, int console_rate)
{
    if (setup.type != MixdownFileType::kWave)
    {
        return kTypeNames[static_cast<std::size_t>(setup.type)];
    }

    const song::WavForm& form = setup.settings.form;
    const int rate = form.rate > 0 ? form.rate : console_rate;
    std::string text = "Wave, " + (rate > 0 ? RateText(rate) : std::string("console rate")) + ", " +
                       kFormatNames[static_cast<std::size_t>(form.format)];
    if (form.normalise)
    {
        text += ", normalised";
    }
    if (form.dither && form.format == song::SampleFormat::kPcm16)
    {
        text += ", dither";
    }

    return text;
}

std::string MixdownChannelsText(const MixdownSetup& setup)
{
    if (setup.type != MixdownFileType::kWave)
    {
        return "All tracks";
    }

    const std::string effects = kStemEffectNames[static_cast<std::size_t>(setup.settings.stem_effects)];
    if (setup.mix && setup.stems)
    {
        return "Mix and Stems (" + effects + ")";
    }

    return setup.stems ? "Stems (" + effects + ")" : "Mix";
}

std::string MixdownLengthText(double seconds)
{
    const auto tenths = static_cast<long long>(std::llround(std::max(seconds, 0.0) * 10.0));
    char text[32];
    std::snprintf(text, sizeof(text), "%lld:%02lld.%lld", tenths / 600, tenths / 10 % 60, tenths % 10);
    return text;
}

MixdownChoice DrawMixdownWindow(MixdownView& view, bool* open)
{
    // The window opens again at the size and the place that it had.
    constexpr const char* kTitle = "Export Audio Mixdown";
    const ImGuiWindow* last = ImGui::FindWindowByName(kTitle);
    const bool reopened = last != nullptr && last->LastFrameActive < ImGui::GetFrameCount() - 1;
    const ImVec2 place = last != nullptr ? last->Pos : ImVec2();
    ImGui::SetNextWindowSize(Px(ImVec2(900.0f, 640.0f)), ImGuiCond_FirstUseEver);
    if (!BeginDialogWindow(kTitle, open, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return *open ? MixdownChoice::kNone : MixdownChoice::kClose;
    }
    if (reopened)
    {
        ImGui::SetWindowPos(place);
    }

    MixdownChoice choice = MixdownChoice::kNone;
    const bool running = view.running;
    const float queue = Px(180.0f);
    const float height = std::max(Px(200.0f), ImGui::GetContentRegionAvail().y - queue);

    // The songs on the left, the settings on the right, and the queue below both.
    ImGui::BeginDisabled(running);
    if (ImGui::BeginTable("##columns", 2, ImGuiTableFlags_None, ImVec2(0.0f, height)))
    {
        ImGui::TableSetupColumn("##songs", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("##settings", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawSongs(view, height - Px(70.0f) - ImGui::GetFrameHeightWithSpacing());
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##settings", ImVec2(0.0f, height - ImGui::GetStyle().ItemSpacing.y));
        bool changed = DrawChannels(view.setup);
        changed |= DrawRange(view.setup, view);
        const MixdownChoice located = DrawLocation(view);
        changed |= DrawFormat(view);
        ImGui::EndChild();
        choice = located != MixdownChoice::kNone ? located : changed ? MixdownChoice::kChanged : choice;
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    const MixdownChoice queued = DrawQueue(view, view.console_rate);
    if (queued != MixdownChoice::kNone)
    {
        choice = queued;
    }
    ImGui::End();

    if (!*open)
    {
        return MixdownChoice::kClose;
    }

    return choice;
}

} // namespace hokuto::app
