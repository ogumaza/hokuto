// SPDX-License-Identifier: MIT

// The Export window: the view of each kind of export, with its targets and their lines, and the window that shows it.
// export_warnings.cpp writes the text of the warnings.

#include "app/export_window.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/ascii.h"

#include "app/engine_rows.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "format/engine.h"
#include "format/engines.h"
#include "format/file.h"
#include "song/items.h"
#include "song/streams.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// The ID of the Export window. Its title names the target.
constexpr char kExportWindow[] = "###Export";

// The Export window's margin from the top and the bottom of the main window, in points.
constexpr float kMargin = 32.0f;

// The lines of the targets of a sequence to MIDI.
constexpr char kMidiSoundFontLine[] = "MIDI with SoundFont 2 (one file for all instruments)";
constexpr char kMidiSfzLine[] = "MIDI with SFZ (one file per instrument)";

// The names of an item's kind: in the Export window's title, and as the heading of the item's name.
struct KindNames
{
    const char* title;
    const char* heading;
};

KindNames NamesOf(song::ItemKind kind)
{
    switch (kind)
    {
    case song::ItemKind::kSequence:
        return {"Sequence", "Sequence"};
    case song::ItemKind::kWaveSound:
        return {"Wave Sound", "Wave sound"};
    case song::ItemKind::kWaveSoundFile:
        return {"Wave Sound File", "Wave sound file"};
    case song::ItemKind::kBank:
        return {"Bank", "Bank"};
    case song::ItemKind::kWaveArchive:
        return {"Wave Archive", "Wave archive"};
    case song::ItemKind::kWave:
        return {"Wave", "Wave"};
    case song::ItemKind::kStream:
        return {"Stream", "Stream sound"};
    }

    return {"Item", "Item"};
}

// Whether `name`, a stream's file name, names an AAC file.
bool IsAacFile(const std::string& name)
{
    return name.size() > 4 && xsflib::AsciiLower(name.substr(name.size() - 4)) == ".aac";
}

// The target that writes an item's file for `console`, one of the later consoles.
ExportTarget FileTargetFor(ntdlib::sar::Flavor console)
{
    return RowOf(format::EngineOf(console)).file_target.value_or(ExportTarget::kSwitchFile);
}

} // namespace

ExportView FileExportView(ExportTarget target, const std::string& sound)
{
    constexpr std::array kTitles = {"Export Sound Archive", "Export MIDI and SoundFont", "Export MIDI and SFZ"};
    constexpr std::array kLines = {"Sound archive (rebuilt with edits)", kMidiSoundFontLine, kMidiSfzLine};
    const auto index = static_cast<std::size_t>(target);
    ExportView view;
    view.title = kTitles.at(index);
    view.targets = {target};
    view.lines = {kLines.at(index)};
    if (!sound.empty())
    {
        view.heading = "Sequence";
        view.source = sound;
    }

    return view;
}

ExportView OtherConsoleExportView(format::Engine from, const std::string& sound)
{
    ExportView view;
    view.title = "Export to Another Console";
    const std::optional<ntdlib::sar::Flavor> flavor = format::FlavorOf(from);
    const std::vector<ntdlib::sar::Flavor> consoles =
        flavor ? song::OtherConsoles(*flavor)
               : std::vector<ntdlib::sar::Flavor>{ntdlib::sar::Flavor::kCafe, ntdlib::sar::Flavor::kNx,
                                                  ntdlib::sar::Flavor::kCtr};
    for (const ntdlib::sar::Flavor console : consoles)
    {
        view.targets.push_back(SongTargetFor(format::EngineOf(console)));
        view.lines.push_back("New .hok file for " + song::ConsoleName(console));
    }
    if (from != format::Engine::kWii)
    {
        view.targets.push_back(ExportTarget::kWiiSong);
        view.lines.emplace_back("New .hok file for Wii");
    }
    if (from != format::Engine::kDs)
    {
        view.targets.push_back(ExportTarget::kDsSong);
        view.lines.emplace_back("New .hok file for DS");
    }
    view.heading = "Sequence";
    view.source = sound;

    return view;
}

std::optional<ntdlib::sar::Flavor> ConsoleOf(ExportTarget target)
{
    for (const EngineRow& row : kEngineRows)
    {
        if (row.file_target == target)
        {
            return format::FactsOf(row.engine).flavor;
        }
    }

    return std::nullopt;
}

std::optional<format::Engine> SongEngineOf(ExportTarget target)
{
    for (const EngineRow& row : kEngineRows)
    {
        if (row.song_target == target)
        {
            return row.engine;
        }
    }

    return std::nullopt;
}

// An engine that no song converts to, such as the GBA engine, gets the song target of the Switch.
ExportTarget SongTargetFor(format::Engine engine)
{
    return RowOf(engine).song_target.value_or(ExportTarget::kSwitchSong);
}

ExportView ItemExportView(const ntdlib::sar::SoundArchive& archive, const song::Item& item)
{
    // The item's file, then the same file for the other consoles. MIDI follows for a sequence, WAV for a wave sound, a
    // wave, a wave archive and a stream, and SoundFont 2 and SFZ for a bank.
    const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
    const std::string format = song::ItemFormat(item.kind, flavor);
    const bool aac = item.kind == song::ItemKind::kStream && IsAacFile(song::StreamFileName(archive, item.index));
    std::string first;
    std::vector<std::pair<ExportTarget, std::string>> others;
    switch (item.kind)
    {
    case song::ItemKind::kSequence:
        first = format + " (original file)";
        others = {{ExportTarget::kMidiSoundFont, kMidiSoundFontLine}, {ExportTarget::kMidiSfz, kMidiSfzLine}};
        break;

    case song::ItemKind::kWaveSound:
        first = format + " (original file)";
        others = {{ExportTarget::kWav, "WAV (at the wave sound's pitch)"}};
        break;

    case song::ItemKind::kWaveSoundFile:
        first = format + " (original file, all wave sounds)";
        break;

    case song::ItemKind::kBank:
        first = format + " (original file)";
        others = {{ExportTarget::kSoundFont, "SoundFont 2 (one file for all instruments)"},
                  {ExportTarget::kSfz, "SFZ (one file per instrument, in a folder)"}};
        break;

    case song::ItemKind::kWaveArchive:
        first = format + " (original file)";
        others = {{ExportTarget::kWav, "WAV (16-bit, one file per wave, with loops, in a folder)"}};
        break;

    case song::ItemKind::kWave:
        first = format + " (original file)";
        others = {{ExportTarget::kWav, "WAV (16-bit, with loop)"}};
        break;

    case song::ItemKind::kStream:
        first = (aac ? std::string("AAC") : format) + " (original file)";
        others = {{ExportTarget::kWav, "WAV (16-bit, all channels, with loop)"}};
        break;
    }

    // The same file for each other console.
    ExportView view;
    view.title = std::string("Export ") + NamesOf(item.kind).title;
    view.targets = {ExportTarget::kItemFile};
    view.lines = {first};
    for (const ntdlib::sar::Flavor console : song::OtherConsoles(flavor))
    {
        view.targets.push_back(FileTargetFor(console));
        view.lines.push_back(song::ItemFormat(item.kind, console) + " for " + song::ConsoleName(console) +
                             (aac ? " (DSP-ADPCM)" : ""));
    }

    // The Wii's file. A bank or a wave sound file of the Wii takes its waves along, in an RWAR beside it.
    const std::string wii = song::WiiItemFormat(item.kind);
    view.targets.push_back(ExportTarget::kWiiFile);
    switch (item.kind)
    {
    case song::ItemKind::kSequence:
    case song::ItemKind::kWaveArchive:
    case song::ItemKind::kWave:
        view.lines.push_back(wii + " for Wii");
        break;
    case song::ItemKind::kWaveSound:
    case song::ItemKind::kWaveSoundFile:
    case song::ItemKind::kBank:
        view.lines.push_back(wii + " for Wii (waves in a BRWAR file next to it)");
        break;
    case song::ItemKind::kStream:
        view.lines.push_back(wii + " for Wii" + (aac ? " (DSP-ADPCM)" : ""));
        break;
    }
    for (const auto& [target, line] : others)
    {
        view.targets.push_back(target);
        view.lines.push_back(line);
    }
    view.heading = NamesOf(item.kind).heading;
    view.source = song::ItemName(archive, item);

    return view;
}

namespace
{

// Adds a target to `view` for the file of an item of `kind` for each of the later consoles, the Wii U first: `before`,
// the format's name, `between`, the console's name, and `after`.
void AddLaterConsoles(ExportView& view, song::ItemKind kind, const std::string& before, const std::string& between,
                      const std::string& after = "")
{
    for (const ntdlib::sar::Flavor console :
         {ntdlib::sar::Flavor::kCafe, ntdlib::sar::Flavor::kNx, ntdlib::sar::Flavor::kCtr})
    {
        view.targets.push_back(FileTargetFor(console));
        view.lines.push_back(before + song::ItemFormat(kind, console) + between + song::ConsoleName(console) + after);
    }
}

} // namespace

ExportView ItemExportView(const ntdlib::rsar::SoundArchive& archive, const song::Item& item)
{
    const std::string format = song::WiiItemFormat(item.kind);
    ExportView view;
    view.title = std::string("Export ") + NamesOf(item.kind).title;
    view.targets = {ExportTarget::kItemFile};
    switch (item.kind)
    {
    case song::ItemKind::kSequence:
        view.lines = {format + " (original file)"};
        AddLaterConsoles(view, item.kind, "", " for ");
        view.lines.insert(view.lines.end(), {kMidiSoundFontLine, kMidiSfzLine});
        view.targets.push_back(ExportTarget::kMidiSoundFont);
        view.targets.push_back(ExportTarget::kMidiSfz);
        break;

    // A bank file or a wave sound file of the Wii takes its waves along, in an RWAR beside it. File > Import reads them
    // there.
    case song::ItemKind::kWaveSound:
        view.lines = {format + " (original file, waves in a BRWAR file next to it)", "WAV (at the wave sound's pitch)"};
        view.targets.push_back(ExportTarget::kWav);
        break;

    case song::ItemKind::kWaveSoundFile:
        view.lines = {format + " (original file, all wave sounds, waves in a BRWAR file next to it)"};
        break;

    case song::ItemKind::kBank:
        view.lines = {format + " (original file, waves in a BRWAR file next to it)"};
        AddLaterConsoles(view, item.kind, "", " for ", " (waves in a wave archive file next to it)");
        view.lines.insert(view.lines.end(),
                          {"SoundFont 2 (one file for all instruments)", "SFZ (one file per instrument, in a folder)"});
        view.targets.push_back(ExportTarget::kSoundFont);
        view.targets.push_back(ExportTarget::kSfz);
        break;

    // Wave data that a WAVE block describes goes out as an RWAR of its waves too (song::ItemBytes).
    case song::ItemKind::kWaveArchive:
    case song::ItemKind::kWave:
        {
            const bool wave = item.kind == song::ItemKind::kWave;
            view.lines.push_back(format + " file");
            AddLaterConsoles(view, item.kind, "", " for ");
            view.targets.push_back(ExportTarget::kWav);
            view.lines.push_back(wave ? "WAV (16-bit, with loop)"
                                      : "WAV (16-bit, one file per wave, with loops, in a folder)");
            break;
        }

    // A stream goes to the later consoles too, the Wii U first, and out as a WAV file.
    case song::ItemKind::kStream:
        view.lines = {format + " (original file)"};
        AddLaterConsoles(view, item.kind, "", " for ");
        view.targets.push_back(ExportTarget::kWav);
        view.lines.emplace_back("WAV (16-bit, all channels, with loop)");
        break;
    }
    view.heading = NamesOf(item.kind).heading;
    view.source = song::ItemName(archive, item);

    return view;
}

ExportView InstrumentExportView(const std::string& bank, uint32_t program)
{
    ExportView view;
    view.title = "Export Instrument";
    view.targets = {ExportTarget::kSoundFont, ExportTarget::kSfz};
    view.lines = {"SoundFont 2 (at its program number)", "SFZ (in a folder)"};
    view.heading = "Instrument";
    view.source = bank + ", program " + std::to_string(program);

    return view;
}

ExportView VoiceGroupExportView(const std::string& voice_group, const std::optional<uint32_t>& program, bool sample)
{
    ExportView view;
    view.title = program ? "Export Voice" : "Export Voice Group";
    view.targets = {ExportTarget::kSoundFont};
    view.lines = {program ? "SoundFont 2 (at its program number)"
                          : "SoundFont 2 (the voices that the songs use, at their program numbers)"};
    view.heading = program ? "Voice" : "Voice group";
    view.source = program ? voice_group + ", program " + std::to_string(*program) : voice_group;
    if (!program)
    {
        view.targets.push_back(ExportTarget::kItemFile);
        view.lines.push_back("Assembly (for a GBA project)");
    }
    else if (sample)
    {
        view.targets.push_back(ExportTarget::kItemFile);
        view.lines.push_back("Sample (raw data from the ROM)");
        view.targets.push_back(ExportTarget::kAiff);
        view.lines.push_back("AIFF (sample, for a GBA project)");
    }

    return view;
}

ExportView GbaSongExportView(const std::string& song)
{
    ExportView view = FileExportView(ExportTarget::kMidiSoundFont, song);
    view.title = "Export Song";
    view.targets.push_back(ExportTarget::kItemFile);
    view.lines.push_back("Assembly (for a GBA project)");

    return view;
}

ExportView DriverSongExportView(const std::string& song, bool sfz)
{
    ExportView view = FileExportView(ExportTarget::kMidiSoundFont, song);
    view.title = "Export Song";
    if (sfz)
    {
        view.targets.push_back(ExportTarget::kMidiSfz);
        view.lines.push_back(kMidiSfzLine);
    }

    return view;
}

ExportView DriverSamplesExportView(song::ItemKind kind, const std::string& source, bool sfz)
{
    ExportView view;
    switch (kind)
    {
    case song::ItemKind::kBank:
        view.title = "Export Samples";
        view.targets = {ExportTarget::kSoundFont};
        view.lines = {"SoundFont 2 (the samples that the songs use, at their sample numbers)"};
        if (sfz)
        {
            view.targets.push_back(ExportTarget::kSfz);
            view.lines.emplace_back("SFZ (one file per sample, in a folder)");
        }
        view.heading = "Samples";
        break;

    case song::ItemKind::kWaveArchive:
        view.title = "Export Samples";
        view.targets = {ExportTarget::kWav};
        view.lines = {"WAV (one file per sample, with loops, in a folder)"};
        view.heading = "Samples";
        break;

    default:
        view.title = "Export Sample";
        view.targets = {ExportTarget::kWav, ExportTarget::kSoundFont};
        view.lines = {"WAV (with loop)", "SoundFont 2 (at its sample number)"};
        if (sfz)
        {
            view.targets.push_back(ExportTarget::kSfz);
            view.lines.emplace_back("SFZ (in a folder)");
        }
        view.heading = "Sample";
        break;
    }
    view.source = source;

    return view;
}

ExportView DsSequenceExportView(const std::string& sequence)
{
    ExportView view = FileExportView(ExportTarget::kMidiSoundFont, sequence);
    view.title = "Export Sequence";
    view.targets.push_back(ExportTarget::kMidiSfz);
    view.lines.push_back(kMidiSfzLine);
    view.targets.push_back(ExportTarget::kItemFile);
    view.lines.push_back("SSEQ (original file)");

    return view;
}

ExportView DseSoundExportView(const std::string& sound, bool song)
{
    ExportView view = FileExportView(ExportTarget::kMidiSoundFont, sound);
    view.title = "Export Sequence";
    view.targets.push_back(ExportTarget::kMidiSfz);
    view.lines.push_back(kMidiSfzLine);
    if (song)
    {
        view.targets.push_back(ExportTarget::kItemFile);
        view.lines.push_back("smdl (original file)");
    }

    return view;
}

ExportView DsItemExportView(song::ItemKind kind, const std::string& source)
{
    ExportView view;
    view.title = std::string("Export ") + NamesOf(kind).title;
    view.targets = {ExportTarget::kItemFile};
    switch (kind)
    {
    case song::ItemKind::kBank:
        view.lines = {"SBNK (original file)", "SoundFont 2 (one file for all instruments)",
                      "SFZ (one file per instrument, in a folder)"};
        view.targets.push_back(ExportTarget::kSoundFont);
        view.targets.push_back(ExportTarget::kSfz);
        break;

    case song::ItemKind::kWaveArchive:
        view.lines = {"SWAR (original file)", "WAV (16-bit, one file per wave, with loops, in a folder)"};
        view.targets.push_back(ExportTarget::kWav);
        break;

    case song::ItemKind::kWave:
        view.lines = {"SWAV file", "WAV (16-bit, with loop)"};
        view.targets.push_back(ExportTarget::kWav);
        break;

    default:
        view.lines = {"STRM (original file)", "WAV (16-bit, all channels, with loop)"};
        view.targets.push_back(ExportTarget::kWav);
        break;
    }
    view.heading = NamesOf(kind).heading;
    view.source = source;

    return view;
}

ExportView DseItemExportView(song::ItemKind kind, const std::string& source)
{
    ExportView view;
    view.title = std::string("Export ") + NamesOf(kind).title;
    switch (kind)
    {
    case song::ItemKind::kBank:
        view.targets = {ExportTarget::kItemFile, ExportTarget::kSoundFont, ExportTarget::kSfz};
        view.lines = {"swdl (original file)", "SoundFont 2 (one file for all instruments)",
                      "SFZ (one file per instrument, in a folder)"};
        break;

    case song::ItemKind::kWaveArchive:
        view.targets = {ExportTarget::kItemFile, ExportTarget::kWav};
        view.lines = {"swdl (original file)", "WAV (16-bit, one file per wave, with loops, in a folder)"};
        break;

    default:
        view.targets = {ExportTarget::kWav};
        view.lines = {"WAV (16-bit, with loop)"};
        break;
    }
    view.heading = NamesOf(kind).heading;
    view.source = source;

    return view;
}

namespace
{

// The width of the Export window for the targets of `view`.
float ExportWindowWidth(const ExportView& view)
{
    float width = Px(520.0f);
    if (view.lines.size() > 1)
    {
        float widest = 0.0f;
        for (const std::string& line : view.lines)
        {
            widest = std::max(widest, ImGui::CalcTextSize(line.c_str()).x);
        }
        const float frame = ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
        width = std::max(width, std::ceil(widest + frame + Px(theme::kDialogPadding).x * 2.0f));
        width = std::min(width, std::max(Px(520.0f), ImGui::GetMainViewport()->WorkSize.x - Px(kMargin) * 2.0f));
    }

    return width;
}

// The target and the source of the export, with the menu of targets and the option. A change sets `choice`.
void DrawExportTarget(ImDrawList* draw, ExportView& view, const std::vector<std::string>* warnings,
                      ExportChoice& choice)
{
    DrawDialogHeading(draw, "Target");
    if (view.lines.size() > 1)
    {
        // A target too long for the menu's box ends with an ellipsis before the arrow. The window takes the longest
        // target's width where the main window allows it.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float room = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - style.FramePadding.x -
                           style.ItemInnerSpacing.x;
        const std::string chosen = FittedText(view.lines[view.chosen], room);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##target", chosen.c_str()))
        {
            for (std::size_t i = 0; i < view.lines.size(); i++)
            {
                if (ImGui::Selectable(view.lines[i].c_str(), i == view.chosen) && i != view.chosen)
                {
                    view.chosen = i;
                    choice = ExportChoice::kTarget;
                }
            }
            ImGui::EndCombo();
        }
    }
    else
    {
        ImGui::TextUnformatted(view.lines[view.chosen].c_str());
    }
    if (!view.option.empty())
    {
        ImGui::BeginDisabled(warnings == nullptr);
        if (CheckBox(view.option.c_str(), &view.option_on))
        {
            choice = ExportChoice::kOption;
        }
        ImGui::EndDisabled();
        ItemTooltip(view.option_tooltip);
    }
    if (!view.heading.empty())
    {
        ImGui::Spacing();
        DrawDialogHeading(draw, view.heading.c_str());
        TextWrappedAtSpaces(view.source);
    }
}

// The warnings of the export, or the reason that the target is refused.
void DrawExportWarnings(ImDrawList* draw, const std::vector<std::string>* warnings, const std::string& refused_reason,
                        bool refused, bool appearing)
{
    ImGui::Spacing();
    DrawDialogHeading(draw, refused ? "Can't export" : "Warnings");
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, Px(280.0f)));
    if (appearing)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##warnings", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    ImGui::PushTextWrapPos(0.0f);
    if (refused)
    {
        TextWrappedAtSpaces(refused_reason);
    }
    else if (warnings == nullptr || warnings->empty())
    {
        MutedText(warnings == nullptr ? "Checking…" : "None");
    }
    for (std::size_t i = 0; !refused && warnings != nullptr && i < warnings->size(); i++)
    {
        ImGui::Bullet();
        TextWrappedAtSpaces((*warnings)[i]);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// The measure of the export, with Play buttons under each place that it names. A press sets `choice`.
void DrawExportMeasured(ImDrawList* draw, ExportView& view, const MeasuredView& measured, bool measure_came,
                        ExportChoice& choice)
{
    ImGui::Spacing();
    DrawDialogHeading(draw, "Measured");
    // Below the measure: the spacing before the buttons, the buttons and the window's padding.
    const float below = ImGui::GetStyle().ItemSpacing.y * 3.0f + ImGui::GetFrameHeight() + Px(theme::kDialogPadding.y);
    const float room = ImGui::GetMainViewport()->WorkSize.y - Px(kMargin) * 2.0f - ImGui::GetCursorPosY() - below;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, std::max(Px(120.0f), room)));
    if (measure_came)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##measured", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    ImGui::PushTextWrapPos(0.0f);
    if (!measured.status.empty())
    {
        MutedText(measured.status.c_str());
    }
    for (const std::string& line : measured.lines)
    {
        ImGui::Bullet();
        TextWrappedAtSpaces(line);
    }
    for (std::size_t i = 0; i < measured.places.size(); i++)
    {
        ImGui::PushID(static_cast<int>(i));
        ImGui::Bullet();
        TextWrappedAtSpaces(measured.places[i].line);
        // The buttons start where the line's text starts, after the bullet.
        const float indent = ImGui::GetTreeNodeToLabelSpacing();
        ImGui::Indent(indent);
        if (ImGui::Button("Play Export"))
        {
            view.play = i;
            view.play_hokuto = false;
            choice = ExportChoice::kPlay;
        }
        ImGui::SameLine();
        if (ImGui::Button("Play Hokuto"))
        {
            view.play = i;
            view.play_hokuto = true;
            choice = ExportChoice::kPlay;
        }
        ImGui::Unindent(indent);
        ImGui::PopID();
    }
    if (!measured.closing.empty())
    {
        ImGui::Bullet();
        TextWrappedAtSpaces(measured.closing);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// Cancel and Export… at the bottom right of the window. A press sets `choice`.
void DrawExportButtons(const std::vector<std::string>* warnings, bool refused, ExportChoice& choice)
{
    ImGui::Spacing();
    ImGui::Spacing();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Cancel").x + ImGui::CalcTextSize("Export…").x +
                          style.FramePadding.x * 4.0f + style.ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons);
    if (ImGui::Button("Cancel"))
    {
        ImGui::CloseCurrentPopup();
        choice = ExportChoice::kCancel;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(warnings == nullptr || refused || choice == ExportChoice::kTarget);
    if (PrimaryButton("Export…"))
    {
        choice = ExportChoice::kExport;
    }
    ImGui::EndDisabled();
}

// Opens the modal window `title`, `width` wide, in the style of the dialogs. Returns whether it shows.
bool BeginExportModal(const std::string& title, float width)
{
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    const bool visible = ImGui::BeginPopupModal(title.c_str(), &open, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    return visible;
}

// Whether the measure comes in with this frame of the window: it's `shown` now, and the window is `appearing` or the
// last frame didn't show it. The window's storage keeps whether the last frame showed the measure.
bool MeasureCame(bool shown, bool appearing)
{
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID measure_shown = ImGui::GetID("measure shown");
    const bool came = shown && (appearing || !storage->GetBool(measure_shown, false));
    storage->SetBool(measure_shown, shown);

    return came;
}

} // namespace

ExportChoice DrawExportWindow(ExportView& view, std::optional<ImVec2>& place, const std::vector<std::string>* warnings,
                              const MeasuredView* measured)
{
    if (!ImGui::IsPopupOpen(kExportWindow))
    {
        ImGui::OpenPopup(kExportWindow);
    }

    // A window over the middle of the main window, in the style of Studio Setup. It keeps the clicks from the windows
    // under it while it's open. The user can drag it by its title bar. It then opens where the drag left it until
    // Hokuto quits: `place` keeps its centre. The warnings and the measure make the window taller after it opens. The
    // window keeps its top and grows down. It moves up only to stay inside the main window. The controls under the
    // mouse then stay where they were. While the drag goes on, the drag places the window.
    const std::string title = view.title + kExportWindow;
    const DialogPlace placed = PlaceDialog(title, place, view.top, Px(kMargin));
    // The window takes the width of its longest target where that passes 520 points, up to the main window's width. The
    // menu of targets and the chosen target then stay inside it.
    if (!BeginExportModal(title, ExportWindowWidth(view)))
    {
        return ExportChoice::kCancel;
    }

    // A drag that moves the window gives its new place. A click on the title bar that moves nothing keeps the old one.
    // The window's corner lies on whole pixels. The first frame that shows the window gives its top.
    FollowDialogDrag(placed, place, view.top);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return ExportChoice::kCancel;
    }

    // Each window opens with its warnings at the top, and the measure starts at its top when it comes in. The panes
    // keep their names from one window to the next, and Dear ImGui keeps their scroll with the names. The window's
    // storage says whether the last frame showed the measure.
    const bool appearing = ImGui::IsWindowAppearing();
    const bool measure_came = MeasureCame(measured != nullptr, appearing);

    // What the export writes, from which sequence or item. The headings follow Studio Setup's. The text wraps at the
    // window's edge. A menu offers the targets when there are several. A choice of another target restarts the check.
    ImGui::PushTextWrapPos(0.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    ExportChoice choice = ExportChoice::kNone;
    DrawExportTarget(draw, view, warnings, choice);

    // The warnings scroll once they fill the window's height. A child window wraps its text by itself. A refused target
    // gives its reason there instead.
    const bool refused = !view.refused.empty();
    DrawExportWarnings(draw, warnings, view.refused, refused, appearing);

    // The measure of an export to MIDI comes in after the warnings. Each place that it names has Play buttons under its
    // line: the export's render there, and Hokuto's playback. The measure takes the height that the main window has
    // left, less the buttons and the margin. Its places then show without a scroll where they fit. In a short main
    // window, the measure keeps 120 points.
    if (measured != nullptr)
    {
        DrawExportMeasured(draw, view, *measured, measure_came, choice);
    }
    ImGui::PopTextWrapPos();

    // Cancel and Export… sit at the bottom right, with Export… on the right. macOS places a dialog's buttons the same
    // way.
    DrawExportButtons(warnings, refused, choice);
    ImGui::EndPopup();

    return choice;
}

void CloseExportWindow()
{
    if (ImGui::IsPopupOpen(kExportWindow) && ImGui::BeginPopupModal(kExportWindow))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace hokuto::app
