// SPDX-License-Identifier: MIT

// The Sound section of the inspector and the browser: a sound's values in the archive's tables, after the properties of
// a sound in NintendoWare's sound tools. The player, the player priority, the actor player, the volume and the pan show
// for every sound. The channel priority, the release priority fix, the tracks and the bank slots show for a sequence
// sound. Each value has a menu, a slider or switches. A value that the archive doesn't hold shows greyed, and a sound
// of the Wii shows one bank slot (edit::HeldValuesOf). The program makes the change once the user lets go of the
// control (edit/tables.h). A wave sound's values in its wave sound file follow: its wave, its pitch, its pans, its
// sends, its filters and its envelope (edit/waves.h). The sound's tags follow, a box of text for each key that the
// program offers (format::kTagKeys).

#pragma once

#include <imgui.h>

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/item_views.h"
#include "app/widgets.h"
#include "edit/tables.h"
#include "format/file.h"

namespace hokuto::app
{

// What the Sound section shows of a sound.
struct SoundView
{
    uint32_t sound = 0;               // the sound's place in the archive's sound table
    format::NativeSound values;       // the sound's values in the tables
    std::vector<std::string> players; // the names of the archive's players, by their indices
    std::vector<std::string> banks;   // the names of the archive's banks, by their indices
    format::Tags tags;                // the sound's tags
    edit::HeldValues held;            // the values that the archive holds
};

// A tag that the user typed: its key, and the new value. An empty value removes the tag.
struct TagChange
{
    std::string key;
    std::string value;
};

// A wave sound's new values and wave in its wave sound file, with the action's name and details for the history.
struct WaveSoundChange
{
    ntdlib::sar::WaveSoundFile::WaveSound values;
    ntdlib::sar::WaveId wave;
    std::string name;
    std::string details;
};

// The Sound section between frames: the view of the sound that it shows, the version and the sound that the view comes
// from, the value of the slider that the user holds, and a change that the user made. The program takes the change, and
// clears it. A tag that the user typed waits in `tag_changed` the same way. A wave sound's values in its wave sound
// file have a view of their own, from the archive of `wave_version`, with the slider that the user holds by its row and
// its value, and the change in `wave_changed`. While `wave_busy`, those rows take no edits. `wave_group_sizes` measures
// the groups that load the wave sound file. `filter` holds the text of the menus' search fields, and `typing` the text
// of the tag that the user types. The inspector and the browser each have a section.
struct SoundSection
{
    std::optional<SoundView> view;
    std::shared_ptr<const format::File> version;
    std::optional<uint32_t> sound;
    std::optional<edit::SoundChange> held;
    std::optional<edit::SoundChange> changed;
    std::optional<TagChange> tag_changed;
    std::optional<WaveSoundView> wave_sound;
    std::shared_ptr<const format::File> wave_version;
    std::optional<uint32_t> wave_sound_of;
    std::optional<std::pair<int, float>> wave_held;
    std::optional<WaveSoundChange> wave_changed;
    bool wave_busy = false;
    std::future<std::vector<std::string>> wave_group_sizes;
    std::array<char, 128> filter{};
    TextTyping typing;
};

// Shows sound `sound` of `version` in `section`. A new version or another sound makes the view again, and drops the
// value of a slider that the user holds. Another sound drops the text of a tag that the user types too. Nothing shows
// without a version or a sound, or for a sound past the tables.
void ShowSound(SoundSection& section, std::shared_ptr<const format::File> version, std::optional<uint32_t> sound);

// Shows the values of the section's wave sound in its wave sound file, from `archive`, the archive that `version`
// builds. A new version or another sound makes the view again, and drops the value of a slider that the user holds.
// Nothing shows for a sound of another kind, or without an archive. While `busy`, the rows take no edits. Returns true
// when it makes the view again.
bool ShowWaveSound(SoundSection& section, std::shared_ptr<const format::File> version,
                   const ntdlib::sar::SoundArchive* archive, bool busy);

// The same for a wave sound of the Wii's `archive`.
bool ShowWaveSound(SoundSection& section, std::shared_ptr<const format::File> version,
                   const ntdlib::rsar::SoundArchive* archive, bool busy);

// The label of the tag of `key`, such as Title for title, and what the tag holds. A key that the program doesn't offer
// is its label, and has no tip.
std::string TagLabel(std::string_view key);
std::string TagTip(std::string_view key);

// Draws the rows of the values of `section`'s sound from `pos`, `width` wide. Returns the bottom of the last row.
float DrawSoundValues(ImDrawList* draw, ImVec2 pos, float width, SoundSection& section);

// The text of value `value` of a sound of `view`'s archive: a player's or a bank's name, a pan mode's or a pan curve's
// name, the tracks' numbers, Off or On, or a number. `value_of` is in a SoundChange's terms.
std::string SoundValueText(const SoundView& view, edit::SoundValue value, uint32_t value_of);

} // namespace hokuto::app
