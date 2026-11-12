// SPDX-License-Identifier: MIT

// The browser on the DS: the lists of the DS's archives and of DSE's sets, and the slots of their sounds.

#include <cstddef>
#include <exception>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "app/app_browser_parts.h"
#include "app/app_internal.h"

namespace hokuto::app
{

using browser_parts::ChildName;
using browser_parts::kOpenBankEditor;
using browser_parts::kStreamProperties;
using browser_parts::kWaveProperties;

namespace
{

// The item `name` of the DS or of DSE, number `index` of `group`, with `children`. It is exported as a file, and its
// banks and waves too.
BrowserItem DsBrowserItem(std::size_t group, std::string name, uint32_t index, std::vector<uint32_t> children)
{
    BrowserItem item;
    item.name = std::move(name);
    for (const uint32_t child : children)
    {
        item.children.push_back(ChildName(group, child));
    }
    item.exports = true;
    item.children_export = group == kBanksGroup || group == kWavesGroup;
    item.number = index;
    item.child_numbers = std::move(children);

    return item;
}

} // namespace

void App::ListDseBrowser()
{
    // The songs, then the sound effects, by their numbers (song/dse.h). Each plays in the zones, and is exported as
    // MIDI. Replace… puts a song file or a MIDI file in the place of a sound's tracks.
    const song::DseSet& set = *played_.Dse();
    const auto add = [this](std::size_t group, std::string name, uint32_t index, std::vector<uint32_t> children)
    {
        BrowserItem item = DsBrowserItem(group, std::move(name), index, std::move(children));
        item.replaces = true;
        browser_[group].items.push_back(std::move(item));
    };
    for (const uint32_t sound : song::DseSounds(set))
    {
        add(kSequencesGroup, song::DseSoundName(set, sound), sound, {});
    }

    // A bank lists the programs that contain an instrument, and a click on one plays a note of it on the console's
    // model. A bank with samples lists its waves, and a click on one plays it. Each of these is exported as its file. A
    // bank's instruments go out as SoundFont 2 and SFZ, and the waves as WAV files. Replace… puts a wave bank file in a
    // bank's place, and a WAV file in a wave's place.
    for (const uint32_t bank : song::DseProgramBanks(set))
    {
        add(kBanksGroup, song::DseBankName(*set.banks[bank].bank), bank, song::DseBankPrograms(*set.banks[bank].bank));
        browser_[kBanksGroup].items.back().edit = kOpenBankEditor;
        browser_[kBanksGroup].items.back().children_edit = kOpenBankEditor;
    }
    for (const uint32_t bank : song::DseSampleBanks(set))
    {
        add(kWavesGroup, song::DseBankName(*set.banks[bank].bank), bank, song::DseBankWaves(*set.banks[bank].bank));
        browser_[kWavesGroup].items.back().children_edit = kWaveProperties;
        browser_[kWavesGroup].items.back().children_replace = true;
    }
}

void App::ListDsBrowser()
{
    // The archive's sequences, then those of its sequence archives, by their numbers (song/ds.h). A sequence plays in
    // the zones, and is exported as MIDI.
    const song::DsArchive& archive = *played_.Ds();
    const auto add = [this](std::size_t group, std::string name, uint32_t index, std::vector<uint32_t> children)
    {
        browser_[group].items.push_back(DsBrowserItem(group, std::move(name), index, std::move(children)));
    };
    for (const uint32_t sound : song::DsSounds(archive))
    {
        add(kSequencesGroup, song::DsSoundName(archive, sound), sound, {});
        BrowserItem& item = browser_[kSequencesGroup].items.back();
        item.replaces = sound < archive.archive.sequences.size();
        if (!item.replaces)
        {
            item.replace_note = "This sound is in a sequence archive. Hokuto doesn't support replacing it yet.";
        }
    }

    // The streams play on a click. A bank lists the programs that contain an instrument, and a click on one plays a
    // note of it on the console's model. A wave archive lists its waves, and a click on one plays it. A bank or a wave
    // archive that can't be read lists none. Each of these items is exported as its file. A bank's instruments go out
    // as SoundFont 2 and SFZ, and the waves and the streams as WAV files. A bank opens in the Bank Editor.
    for (const uint32_t stream : song::DsStreams(archive))
    {
        add(kSoundsGroup, song::DsStreamName(archive, stream), stream, {});
        browser_[kSoundsGroup].items.back().plays = true;
        browser_[kSoundsGroup].items.back().replaces = true;
        browser_[kSoundsGroup].items.back().edit = kStreamProperties;
    }
    for (const uint32_t bank : song::DsBanks(archive))
    {
        add(kBanksGroup, song::DsBankName(archive, bank), bank, song::DsBankPrograms(archive, bank));
        browser_[kBanksGroup].items.back().edit = kOpenBankEditor;
        browser_[kBanksGroup].items.back().children_edit = kOpenBankEditor;
        browser_[kBanksGroup].items.back().replaces = true;
    }
    for (const uint32_t wave_archive : song::DsWaveArchives(archive))
    {
        std::vector<uint32_t> waves(song::DsWaveCount(archive, wave_archive));
        std::iota(waves.begin(), waves.end(), 0u);
        add(kWavesGroup, song::DsWaveArchiveName(archive, wave_archive), wave_archive, std::move(waves));
        browser_[kWavesGroup].items.back().replaces = true;
        browser_[kWavesGroup].items.back().children_replace = true;
        browser_[kWavesGroup].items.back().children_edit = kWaveProperties;
        browser_[kWavesGroup].items.back().children_drag = true;
    }
}

std::vector<SlotView> App::DseSlotsOf(uint32_t sound) const
{
    std::vector<SlotView> slots;
    if (!played_.Dse() || sound >= played_.Dse()->sounds.size())
    {
        return slots;
    }

    for (const std::shared_ptr<const ntdlib::dse::WaveBank>& bank : played_.Dse()->sounds[sound].banks)
    {
        SlotView slot;
        slot.bank = song::DseBankName(*bank);
        slot.programs = song::DseBankPrograms(*bank);
        slots.push_back(std::move(slot));
    }

    return slots;
}

std::vector<SlotView> App::DsSlotsOf(uint32_t sound) const
{
    std::optional<uint32_t> bank;
    try
    {
        bank = played_.Ds() ? song::DsBankOf(*played_.Ds(), sound) : std::nullopt;
    }
    catch (const std::exception&)
    {
    }
    if (!bank)
    {
        return {};
    }

    SlotView slot;
    slot.bank = song::DsBankName(*played_.Ds(), *bank);
    slot.programs = song::DsBankPrograms(*played_.Ds(), *bank);

    return {slot};
}

} // namespace hokuto::app
