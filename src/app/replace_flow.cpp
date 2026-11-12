// SPDX-License-Identifier: MIT

// The state of Replace… and of the imports: the targets of the file dialog, the end of a replacement, and the edits and
// the versions that the Import window and the Replace window's Play make from the window's choices.

#include "app/replace_flow.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"

#include "app/replace_ds_views.h"
#include "app/replace_sequence_views.h"
#include "app/replace_view.h"
#include "edit/ds_new_items.h"
#include "edit/files.h"
#include "edit/groups.h"
#include "edit/midi_imports.h"
#include "edit/new_items.h"
#include "edit/replace.h"
#include "edit/sound_font_imports.h"
#include "edit/variables.h"
#include "format/file.h"
#include "song/ds.h"

namespace hokuto::app
{

void ReplaceFlow::ForgetTargets()
{
    sound.reset();
    file.reset();
    wave.reset();
    stream.reset();
}

void ReplaceFlow::Drop()
{
    if (shown && shown->pending)
    {
        dropped_edits.push_back(std::move(shown->pending->work));
    }
    shown.reset();
}

void ReplaceFlow::ChangeArchiveSequence()
{
    // The slots stay the sound's until the user chooses a slot's bank.
    const ReplaceView& view = shown->view;
    if (view.slots_changed)
    {
        shown->source.banks = ChosenBanks(view);
    }
    edit::ProgramMap programs = ChosenPrograms(view);
    if (view.slots_changed && !shown->source.midi.empty())
    {
        programs = ProposeMidiMap(shown->source);
    }
    FollowDrumRows(shown->source, view);
    shown->view = MakeReplaceView(shown->source, view.chosen, programs, ChosenVariables(view), view.all_tracks);
}

void ReplaceFlow::DsNewItemPreview(std::function<format::File()>& make, PreviewTarget& target)
{
    // A new item of the DS plays from the version that the import makes.
    const ReplaceSource& source = shown->source;
    make = DsImportEdit();
    const ntdlib::sdat::Archive& tables = source.played.Ds()->archive;
    if (*shown->ds_new == edit::DsNewItem::kSequence)
    {
        target.sound = static_cast<uint32_t>(tables.sequences.size());
    }
    else if (*shown->ds_new == edit::DsNewItem::kBank)
    {
        target.bank = static_cast<uint32_t>(tables.banks.size());
    }
    else
    {
        target.wave_archive = static_cast<uint32_t>(tables.wave_archives.size());
    }
}

void ReplaceFlow::ReplacedFilePreview(std::function<format::File()>& make, PreviewTarget& target)
{
    // A file in the place of another plays the sound chosen in the menu, in its mixing mode. A bank or a wave
    // archive that no sound plays gives a note of its first instrument or its first wave.
    // A sequence of the DS or DSE plays its sound.
    const ReplaceSource& source = shown->source;
    const ReplaceView& view = shown->view;
    make = [result = *view.result]
    {
        return result;
    };
    if (view.play < view.play_sounds.size())
    {
        target.sound = view.play_sounds[view.play];
        target.mixing = format::MixingOf(*source.file, *target.sound);
    }
    else if (shown->file == edit::FileKind::kBank)
    {
        target.bank = source.sound;
    }
    else if (shown->file)
    {
        target.wave_archive = source.sound;
    }
    else
    {
        target.sound = source.sound;
    }
}

void ReplaceFlow::ModelSequencePreview(std::function<format::File()>& make, PreviewTarget& target)
{
    // A new sequence sound plays as its model would with the new commands. The edit puts them in the place of the
    // model's sequence. That builds the archive faster than a new sound.
    const ReplaceSource& source = shown->source;
    const ReplaceView& view = shown->view;
    make = [file = source.file, places = source.places, sound = source.sound, sequence = source.incoming.sequence,
            start = view.starts[view.chosen].item, programs = ChosenPrograms(view), variables = ChosenVariables(view),
            banks = source.banks]
    {
        const edit::MappedVariables mapped = edit::WithVariables(sequence, start, variables);
        const uint32_t tracks = edit::TracksToAdd(*file, places, sound, mapped.sequence, mapped.start);
        return edit::ReplaceSequence(*file, places, sound, mapped.sequence, mapped.start, programs, "Preview", tracks,
                                     banks);
    };
    target.sound = source.sound;
}

std::function<format::File()> ReplaceFlow::ItemImportEdit(bool join) const
{
    // The edit builds the archive again with the new item.
    const ReplaceSource& source = shown->source;
    const ReplaceView& view = shown->view;
    std::map<uint32_t, uint32_t> map;
    const std::vector<int> targets = ChosenWaveArchives(view);
    for (std::size_t k = 0; k < shown->named.size() && k < targets.size(); k++)
    {
        map[shown->named[k].id] = static_cast<uint32_t>(targets[k]);
    }

    // With `join`, the new item, or each new wave sound, joins the chosen groups. A group that loads the sound whose
    // settings new wave sounds copy takes only the files that the sound doesn't require (edit::JoinGroups).
    const edit::IncomingFile& incoming = *shown->new_item;
    std::vector<uint32_t> before;
    if (incoming.kind == edit::FileKind::kWaveSoundFile)
    {
        before = edit::FilesOfItem(*source.played.Sar(), ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, source.sound));
    }

    return [file = source.file, sound = source.sound, name = NameOf(view), incoming, map,
            groups = join ? view.joined : std::vector<uint32_t>(),
            new_groups = join ? view.new_groups : std::vector<std::string>(), files_before = std::move(before)]
    {
        const format::NativeTables& tables = *file->native->tables;
        std::vector<uint32_t> items;
        format::File added;
        if (incoming.kind == edit::FileKind::kBank)
        {
            items.push_back(ntdlib::sar::ItemId(ntdlib::sar::kBankItem, static_cast<uint32_t>(tables.banks.size())));
            added = edit::AddBank(*file, name, incoming.bytes, map);
        }
        else if (incoming.kind == edit::FileKind::kWaveArchive)
        {
            items.push_back(
                ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, static_cast<uint32_t>(tables.wave_archives.size())));
            added = edit::AddWaveArchive(*file, name, incoming.bytes);
        }
        else
        {
            added = edit::AddWaveSounds(*file, sound, name, incoming.bytes, map);
            for (std::size_t s = tables.sounds.size(); s < added.native->tables->sounds.size(); s++)
            {
                items.push_back(ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, static_cast<uint32_t>(s)));
            }
        }

        return edit::JoinGroups(added, groups, items, files_before, new_groups);
    };
}

std::function<format::File()> ReplaceFlow::SoundFontEdit(bool join) const
{
    // The edit builds the archive again with the new banks. With `join`, they join the chosen groups.
    const ReplaceView& view = shown->view;
    const bool later = !shown->source.played.Wii() && !shown->source.played.Ds();
    return [file = shown->source.file, sf2 = shown->sound_font, name = NameOf(view), later,
            groups = join ? view.joined : std::vector<uint32_t>(),
            new_groups = join ? view.new_groups : std::vector<std::string>()]
    {
        if (!later)
        {
            return edit::AddSoundFont(*file, *sf2, name, groups);
        }

        const format::File added = edit::AddSoundFont(*file, *sf2, name);
        std::vector<uint32_t> items;
        const std::size_t first = file->native->tables->banks.size();
        for (std::size_t bank = first; bank < added.native->tables->banks.size(); bank++)
        {
            items.push_back(ntdlib::sar::ItemId(ntdlib::sar::kBankItem, static_cast<uint32_t>(bank)));
        }

        return edit::JoinGroups(added, groups, items, {}, new_groups);
    };
}

std::function<format::File()> ReplaceFlow::DsImportEdit() const
{
    // The edit builds the archive again with the new item.
    const ReplaceSource& source = shown->source;
    const ReplaceView& view = shown->view;
    const edit::DsNewItem kind = *shown->ds_new;
    if (kind == edit::DsNewItem::kStream)
    {
        const edit::DsStreamSettings settings = source.models.empty()
                                                    ? edit::DsStreamSettings()
                                                    : edit::DsStreamSettingsOf(*source.played.Ds(), source.sound);
        return [file = source.file, name = NameOf(view), bytes = view.wave, settings]
        {
            return edit::AddDsStream(*file, name, bytes, settings);
        };
    }

    const std::vector<int> rows = ChosenWaveArchives(view);
    edit::DsSoundValues values;
    std::array<uint16_t, ntdlib::sdat::kWaveArchiveSlots> slots;
    slots.fill(ntdlib::sdat::kNoWaveArchive);
    if (kind == edit::DsNewItem::kSequence)
    {
        if (!source.models.empty())
        {
            values = edit::DsSoundValuesOf(*source.played.Ds(), source.sound);
        }
        const std::vector<uint32_t> banks = song::DsBanks(*source.played.Ds());
        if (!rows.empty() && rows.front() >= 0 && static_cast<std::size_t>(rows.front()) < banks.size())
        {
            values.bank = static_cast<uint16_t>(banks[static_cast<std::size_t>(rows.front())]);
        }
    }
    else if (kind == edit::DsNewItem::kBank)
    {
        slots = ChosenDsSlots(edit::DsBankSlots(shown->wav), rows, song::DsWaveArchives(*source.played.Ds()));
    }

    const std::vector<uint32_t> bank(edit::kMidiBankSlots, values.bank);
    return [file = source.file, kind, name = NameOf(view), bytes = shown->wav, values, slots,
            maps = MidiMapsOf(view, source, bank)]
    {
        format::File made = kind == edit::DsNewItem::kSequence ? edit::AddDsSequenceSound(*file, name, bytes, values)
                            : kind == edit::DsNewItem::kBank   ? edit::AddDsBank(*file, name, bytes, slots)
                                                               : edit::AddDsWaveArchive(*file, name, bytes);
        KeepMidiMaps(made, maps);
        return made;
    };
}

} // namespace hokuto::app
