// SPDX-License-Identifier: MIT

// The tracks' colours and the Sound section of the inspector: a colour pick, the tags and the changes of a sound's
// values.

#include <bit>
#include <cstddef>
#include <exception>
#include <utility>

#include "app/app.h"
#include "app/app_internal.h"
#include "app/theme.h"
#include "edit/ds_tables.h"
#include "edit/gba_tables.h"
#include "edit/tables.h"
#include "format/file.h"

namespace hokuto::app
{
namespace
{

// The names of `change` of the values of `view`, a sound in the tables of the version before the change. Change Sound
// Volume tells the edit apart from a change of a track's volume in the mixer. A switch of the tracks turns one track on
// or off.
ActionNames SoundChangeNames(const SoundView& view, const edit::SoundChange& change)
{
    const uint32_t from = edit::ValueOf(view.values, change.value, change.slot);
    const std::string where = "Sound: " + view.values.name + " - ";
    const std::string from_to =
        "from " + SoundValueText(view, change.value, from) + " to " + SoundValueText(view, change.value, change.to);
    switch (change.value)
    {
    case edit::SoundValue::kVolume:
        return {"Change Sound Volume", where + from_to};

    case edit::SoundValue::kBank:
        return {"Change Bank Slot", where + "Slot " + std::to_string(change.slot) + " " + from_to};

    case edit::SoundValue::kTracks:
        {
            const uint32_t turned = from ^ change.to;
            if (std::has_single_bit(turned))
            {
                const int track = std::countr_zero(turned);
                return {"Change Tracks",
                        where + "Track " + std::to_string(track) + ((change.to & turned) != 0 ? " on" : " off")};
            }
            break;
        }

    default:
        break;
    }

    return {"Change " + edit::NameOf(change.value), where + from_to};
}

} // namespace

void App::ShowTrackColours()
{
    arrangement_.custom_colours = settings_.track_colours == TrackColours::kCustom;
    if (!song_)
    {
        return;
    }

    // A track that the file gives no colour keeps its rainbow colour while the colours are Custom.
    const format::File* document = Document();
    for (std::size_t i = 0; i < song_->tracks.size(); i++)
    {
        TrackView& track = song_->tracks[i];
        track.colour =
            settings_.track_colours == TrackColours::kStandard ? theme::kStandardTrackColour : track.rainbow_colour;
        if (settings_.track_colours != TrackColours::kCustom || document == nullptr || !sequence_ ||
            i >= sequence_->tracks.size())
        {
            continue;
        }

        const format::TrackPlace place{sequence_->sound, static_cast<uint8_t>(sequence_->tracks[i].number)};
        if (const auto found = document->track_colours.find(place); found != document->track_colours.end())
        {
            track.colour = IM_COL32(found->second.red, found->second.green, found->second.blue, 255);
        }
    }

    // The Key Editor shows its track's colour.
    if (sequence_)
    {
        if (const song::Track* track = TrackNumbered(key_editor_track_))
        {
            key_track_.colour = song_->tracks[static_cast<std::size_t>(track - sequence_->tracks.data())].colour;
        }
    }
}

void App::ApplyColourPick()
{
    const std::optional<TrackColourPick> pick = std::exchange(arrangement_.colour_pick, std::nullopt);
    const format::File* document = Document();
    if (!pick || document == nullptr || !sequence_ || !song_ || pick->track < 0 ||
        static_cast<std::size_t>(pick->track) >= sequence_->tracks.size())
    {
        return;
    }

    // The file keeps the colour by the sound and the track's number. Automatic drops it.
    const auto index = static_cast<std::size_t>(pick->track);
    const format::TrackPlace place{sequence_->sound, static_cast<uint8_t>(sequence_->tracks[index].number)};
    format::File file = *document;
    if (pick->colour)
    {
        const ImU32 colour = *pick->colour;
        file.track_colours[place] = {static_cast<uint8_t>(colour >> IM_COL32_R_SHIFT),
                                     static_cast<uint8_t>(colour >> IM_COL32_G_SHIFT),
                                     static_cast<uint8_t>(colour >> IM_COL32_B_SHIFT)};
    }
    else
    {
        file.track_colours.erase(place);
    }
    if (file.track_colours != document->track_colours)
    {
        Do("Change Track Colour", "Sound: " + song_->name + " - " + song_->tracks[index].name, std::move(file));
    }
}

void App::ApplyTagChange(SoundSection& section)
{
    const std::optional<TagChange> change = std::exchange(section.tag_changed, std::nullopt);
    const format::File* document = Document();
    if (!change || document == nullptr || !section.view)
    {
        return;
    }

    Do("Change Tag", "Sound: " + section.view->values.name + " - " + TagLabel(change->key),
       format::WithTag(*document, section.view->sound, change->key, change->value));
}

void App::ApplySoundChange(SoundSection& section)
{
    const std::optional<edit::SoundChange> change = std::exchange(section.changed, std::nullopt);
    const format::File* document = Document();
    if (!change || document == nullptr || !section.view ||
        edit::ValueOf(section.view->values, change->value, change->slot) == change->to)
    {
        return;
    }

    try
    {
        if (played_.Ds() || played_.Gba())
        {
            FollowArchiveVersion();
        }
        format::File file = played_.Gba() ? edit::ChangeGbaSong(*document, *played_.Gba(), section.view->sound, *change)
                            : played_.Ds() ? edit::ChangeDsSound(*document, *played_.Ds(), section.view->sound, *change)
                                           : edit::ChangeSound(*document, section.view->sound, *change);
        const ActionNames names = SoundChangeNames(*section.view, *change);
        Do(names.name, names.details, std::move(file));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ShowSoundIn(SoundSection& section, std::optional<uint32_t> sound)
{
    const std::shared_ptr<const format::File> version = history_ ? history_->Current() : nullptr;
    if ((!played_.Ds() && !played_.Gba()) || !version)
    {
        ShowSound(section, version, sound);
        return;
    }

    // The archive that the program has serves every version that has it, such as one that only changes a track's
    // colour or a sound's engine.
    if (reading_version_ == nullptr)
    {
        FollowArchiveVersion();
    }
    if (archive_version_ && SameArchive(*version, *archive_version_))
    {
        ShowSound(section, version, sound, played_.Ds().get(), played_.Gba().get());
    }
}

} // namespace hokuto::app
