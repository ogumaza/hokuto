// SPDX-License-Identifier: MIT

// Edits and the Undo History: making an edit, following a move in the history, and the window of an effect on the aux
// buses.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

#include "app/app.h"
#include "app/app_internal.h"
#include "app/system_ui.h"
#include "edit/bytecode.h"
#include "edit/timelines.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/archive.h"

namespace hokuto::app
{
namespace
{

// `selection`, a selection of the commands of a sequence of `edited`, in the version that settles the timelines of
// `edited` (edit::SettleTimelines). Each selected command keeps its place in its settled sequence.
std::optional<KeySelection> SettledSelection(std::optional<KeySelection> selection, const format::File& edited,
                                             const edit::Settled& settled)
{
    if (!selection || !edited.native)
    {
        return selection;
    }

    for (const auto& [sequence, items] : settled.items)
    {
        if (sequence >= edited.sequences.size() || !edited.sequences[sequence].SharesWith(selection->sequence))
        {
            continue;
        }

        const song::Console console = edit::ConsoleOf(edited);
        const std::vector<uint32_t> from = edit::Offsets(*edited.sequences[sequence], console);
        const std::vector<uint32_t> to = edit::Offsets(*settled.file.sequences[sequence], console);
        std::set<uint32_t> commands;
        for (const uint32_t offset : selection->commands)
        {
            const std::optional<uint32_t> item = edit::ItemAtOffset(from, offset);
            if (item && *item < items.size() && items[*item])
            {
                commands.insert(to[*items[*item]]);
            }
        }
        selection->sequence = settled.file.sequences[sequence];
        selection->commands = std::move(commands);
    }

    return selection;
}

} // namespace

const BusEffects* App::Effects() const
{
    const format::File* document = Document();
    return document != nullptr ? &document->aux_effects : nullptr;
}

void App::DrawEffects()
{
    const BusEffects* effects = Effects();
    if (effects == nullptr)
    {
        fx_.clicked.reset();
        effect_window_.reset();
        effect_draft_.reset();
        return;
    }

    // A choice in an empty insert's menu adds the effect, and a click on an effect opens it. Either way, the effect's
    // window opens in place of any other. The menu offers the kinds of the engine that plays the shown sound.
    if (const std::optional<EffectPlace> clicked = std::exchange(fx_.clicked, std::nullopt))
    {
        const EffectPlace place{clicked->channel, std::min(clicked->index, (*effects)[clicked->channel].size())};
        if (place.index == (*effects)[place.channel].size())
        {
            BusEffects added = *effects;
            added[place.channel].push_back(NewEffect(*Document(), PlayedEngine(), fx_.kind));
            ChangeEffects({"Add Effect", EffectTitle(place, added[place.channel].back())}, added);
            effects = Effects();
        }
        effect_window_ = place;
        effect_draft_.reset();
        effect_changing_ = false;
    }

    // The window closes once its effect has gone, and once its channel has gone from the mixer, such as bus C on the
    // 3DS's engine.
    if (!effect_window_ || effect_window_->channel >= fx_.count ||
        effect_window_->index >= (*effects)[effect_window_->channel].size())
    {
        effect_window_.reset();
        effect_draft_.reset();
        return;
    }

    // The draft follows the file, but not while a drag or a typed value is under way. A move in the history to other
    // effects shows in the window even during a drag (FollowHistory).
    const EffectPlace place = *effect_window_;
    const format::Effect& current = (*effects)[place.channel][place.index];
    if (!effect_changing_ || !effect_draft_)
    {
        effect_draft_ = current;
    }
    bool open = true;
    const EffectEdit edit = DrawEffectWindow(place, *effect_draft_, &open);
    effect_changing_ = edit == EffectEdit::kChanging;
    if ((edit == EffectEdit::kChanged || edit == EffectEdit::kPreset) && *effect_draft_ != current)
    {
        BusEffects changed = *effects;
        changed[place.channel][place.index] = *effect_draft_;
        ChangeEffects(EffectChangeNames(place, edit, current, *effect_draft_), changed);
    }
    else if (edit == EffectEdit::kRemoved)
    {
        BusEffects removed = *effects;
        std::vector<format::Effect>& channel = removed[place.channel];
        channel.erase(channel.begin() + static_cast<std::ptrdiff_t>(place.index));
        ChangeEffects({"Remove Effect", EffectTitle(place, current)}, removed);
    }
    if (!open || edit == EffectEdit::kRemoved)
    {
        effect_window_.reset();
        effect_draft_.reset();
    }
}

void App::Do(std::string name, std::string details, format::File file, std::optional<KeySelection> after,
             std::vector<uint32_t> replaced)
{
    // Each timeline that the edit changed takes the converter's layout, and the selections follow its commands. The
    // replaced sequences and the new ones take the mode that the program finds for them.
    const format::File edited = file;
    edit::Settled settled;
    try
    {
        settled = edit::SettleTimelines(*Document(), std::move(file), replaced);
        edit::RefuseLoopsWithoutWaits(*Document(), settled.file);
    }
    catch (const std::invalid_argument& error)
    {
        ShowMessage(error.what());
        return;
    }

    after = SettledSelection(std::move(after), edited, settled);
    list_selection_ = SettledSelection(std::move(list_selection_), edited, settled);
    control_selection_ = SettledSelection(std::move(control_selection_), edited, settled);
    file = settled.replaced.empty() ? std::move(settled.file)
                                    : edit::WithNewModes(std::move(settled.file), settled.replaced);

    // An edit that can change what a sound plays drops the lengths for players. hok lengths measures them again.
    file = format::WithoutStaleLengths(*Document(), std::move(file));

    // The Undo History's rows follow the actions, and so do the editors' selections around them. A new action drops the
    // rows of those that were undone. An edit in the List Editor leaves its selection in `list_selection_`.
    const auto now = std::chrono::system_clock::now();
    std::optional<KeySelection> before;
    if (key_editor_open_)
    {
        before = KeySelection{key_sequence_, key_editor_track_, key_editor_.selected};
    }
    std::optional<KeySelection> list_before;
    if (list_editor_open_)
    {
        list_before = KeySelection{list_sequence_, list_editor_track_, list_editor_.selected};
    }
    history_rows_.resize(history_->Done());
    history_rows_.push_back({name, details, TimeOfDay(now)});
    action_selections_.resize(history_->Done());
    action_selections_.push_back({std::move(before), after, std::move(list_before), list_selection_});
    key_selection_ = std::move(after);
    history_->Do({std::move(name), std::move(details), now, std::make_shared<const format::File>(std::move(file))});
    FollowVersion();
}

void App::ChangeEffects(const ActionNames& names, const BusEffects& effects)
{
    format::File file = *Document();
    file.aux_effects = effects;
    Do(names.name, names.details, std::move(file));
}

void App::FollowVersion()
{
    UpdateTitle();

    // The sequence plays on a new model with the version's effects, in the sound's mixing mode. The new model runs up
    // to the player's frame, and the player then takes it. While playback is stopped, the model runs through the first
    // pass for the next seek.
    const BusEffects* effects = Effects();
    const song::Mixing mixing = ShownMixing();
    if (sequence_ && effects != nullptr && (*effects != model_effects_ || mixing != model_mixing_))
    {
        model_effects_ = *effects;
        model_mixing_ = mixing;
        StartCatchUp();
    }

    // A version whose archive differs from the one that the zones show, or that the reader builds, builds its archive.
    // So does a version that gives the shown sound another engine. A GBA ROM and a DS archive follow too.
    const format::File* document = Document();
    const format::File* target = reading_version_ ? reading_version_.get() : archive_version_.get();
    if (ArchiveOpen() && document != nullptr && target != nullptr &&
        (!SameArchive(*document, *target) || (sequence_ && !EngineServes(*document, sequence_->sound))))
    {
        ReadShown();
    }
}

void App::MoveInHistory(const std::function<void(edit::History&)>& move)
{
    const std::size_t done = history_->Done();
    const std::shared_ptr<const format::File> left = history_->Current();
    move(*history_);
    FollowHistory(done, *left);
}

void App::FollowHistory(std::size_t from, const format::File& left)
{
    // The selections wait for the version, until the editors show it.
    const std::size_t done = history_->Done();
    key_selection_.reset();
    list_selection_.reset();
    if (done < from && done < action_selections_.size())
    {
        key_selection_ = action_selections_[done].before;
        list_selection_ = action_selections_[done].list_before;
    }
    else if (done > from && done <= action_selections_.size())
    {
        key_selection_ = action_selections_[done - 1].after;
        list_selection_ = action_selections_[done - 1].list_after;
    }

    // The drags of the Bank Editor and the properties, the sliders of the Sound sections' wave sounds and the mixer's
    // controls hold values of the version that the history left. A track's edit selects its new track once its version
    // is read. A move to a version whose archive differs from `left`'s leaves those values stale. The new track then
    // belongs to a version that no longer shows. An Undo of an FX fader keeps the archive, and so does the drop of an
    // engine edit whose conversion failed. Such a move leaves them all.
    // The notes of the properties' last edit go with those values.
    const format::File* document = Document();
    if (document != nullptr && !SameArchive(left, *document))
    {
        bank_state_.dragging.reset();
        bank_state_.held.reset();
        bank_state_.gba_held.reset();
        bank_state_.gba_key_held.reset();
        bank_state_.gba_steps.reset();
        bank_state_.ds_held.reset();
        bank_state_.dse_held.reset();
        wave_state_.dragging.reset();
        wave_state_.held.reset();
        wave_state_.notes.clear();
        sound_section_.wave_held.reset();
        browser_sound_.wave_held.reset();
        track_to_select_.reset();
        if (song_)
        {
            for (TrackView& track : song_->tracks)
            {
                track.held.reset();
            }
        }
    }

    // The effect's window keeps its draft while the user drags one of its sliders. After a move to other effects, the
    // draft takes the version's effect. The slider that the user still holds then writes its value into it.
    if (document != nullptr && left.aux_effects != document->aux_effects)
    {
        effect_changing_ = false;
    }

    FollowVersion();

    // The Bank Editor, the properties and the Sound sections show an item's edit before its version's archive is built.
    // Each view is made again when the program's archive changes. An Undo during that build can lead back to the
    // version whose archive the program holds. No build or read then comes, and the views are made again from that
    // archive. A move to another archive builds it, and the views follow once it's built. A move that keeps the archive
    // leaves the views as they are.
    if (document != nullptr && archive_version_ && SameArchive(*document, *archive_version_) &&
        !SameArchive(left, *archive_version_))
    {
        bank_view_version_.reset();
        wave_view_version_.reset();
        sound_section_.wave_version.reset();
        browser_sound_.wave_version.reset();
    }
}

} // namespace hokuto::app
