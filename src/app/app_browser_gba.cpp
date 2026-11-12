// SPDX-License-Identifier: MIT

// The browser on the GBA: the lists of MusicPlayer2000's songs and voice groups and of the other GBA drivers, and the
// slots of their songs.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/mp2k/song.h"

#include "app/app.h"
#include "app/app_browser_parts.h"
#include "app/app_internal.h"
#include "song/driver.h"

namespace hokuto::app
{

using browser_parts::ChildName;
using browser_parts::kOpenBankEditor;

void App::ListGbaBrowser()
{
    // The songs of the song table that play, by their numbers. MP2K's songs have no names: a song is named by its
    // number (song::GbaSongName).
    for (const int song : song::GbaSongs(*played_.Gba()))
    {
        BrowserItem item;
        item.name = song::GbaSongName(*played_.Gba(), song);
        item.exports = true;
        item.replaces = true;
        item.number = static_cast<uint32_t>(song);
        browser_[kSequencesGroup].items.push_back(std::move(item));
    }

    // The voice groups that the songs play, by their addresses, with the programs that the songs select. A voice group
    // is exported as a SoundFont 2 file, and so is each voice. A click on a voice plays a note of it. A double-click
    // opens the voice group in the Bank Editor.
    for (const song::GbaVoiceGroup& group : GbaGroups())
    {
        BrowserItem item;
        item.name = song::GbaVoiceGroupName(group.address);
        std::vector<uint32_t> programs;
        for (const int program : group.programs)
        {
            item.children.push_back(ChildName(kBanksGroup, static_cast<uint32_t>(program)));
            programs.push_back(static_cast<uint32_t>(program));
        }
        item.exports = true;
        item.children_export = true;
        item.replaces = true;
        item.edit = kOpenBankEditor;
        item.children_edit = kOpenBankEditor;
        item.number = group.address;
        item.child_numbers = std::move(programs);
        browser_[kBanksGroup].items.push_back(std::move(item));
    }
}

void App::ListDriverBrowser()
{
    // The songs of the song table that play, by their numbers. Each plays in the zones, and is exported as MIDI with
    // SoundFont 2.
    const song::DriverRom& rom = *played_.Driver();
    for (const uint32_t sound : rom.Sounds())
    {
        BrowserItem item;
        item.name = rom.SoundName(sound);
        item.exports = true;
        item.replace_note = "Hokuto doesn't support replacing songs of " + std::string(rom.Rules().Title()) + " yet.";
        item.number = sound;
        browser_[kSequencesGroup].items.push_back(std::move(item));
    }

    // The samples that the songs play, once as the driver's instruments and once as waves. A click on a sample plays a
    // note of it on the console's model, and a double-click among the instruments opens them in the Bank Editor.
    const std::vector<int>& samples = DriverSampleList();
    if (samples.empty())
    {
        return;
    }

    std::vector<uint32_t> children(samples.begin(), samples.end());
    std::vector<std::string> names;
    for (const int sample : samples)
    {
        names.push_back(played_.Driver()->SampleName(sample));
    }
    for (const std::size_t group : {std::size_t{kBanksGroup}, std::size_t{kWavesGroup}})
    {
        BrowserItem item;
        item.name = "Samples";
        item.children = names;
        item.exports = true;
        item.children_export = true;
        const bool replaces = played_.Driver()->EditsSamples();
        item.replace_note = !replaces              ? "Hokuto doesn't support replacing samples of " +
                                                         std::string(played_.Driver()->Rules().Title()) + " yet."
                            : group == kWavesGroup ? "Replace one sample at a time."
                                                   : "To replace a sample, choose Replace… on it under Waves.";
        item.children_replace = replaces && group == kWavesGroup;
        if (group == kBanksGroup)
        {
            item.edit = kOpenBankEditor;
            item.children_edit = kOpenBankEditor;
        }
        item.number = 0;
        item.child_numbers = children;
        browser_[group].items.push_back(std::move(item));
    }
}

const std::vector<int>& App::DriverSampleList()
{
    if (driver_samples_rom_.lock() != played_.Driver())
    {
        driver_samples_ = played_.Driver() ? played_.Driver()->Samples() : std::vector<int>();
        driver_psg_programs_.clear();
        for (int track = 0; played_.Driver() && track < song::kMostDriverTracks; track++)
        {
            if (played_.Driver()->Rules().PsgTrack(track))
            {
                driver_psg_programs_[track] = played_.Driver()->TrackPrograms(track);
            }
        }
        driver_samples_rom_ = played_.Driver();
    }

    return driver_samples_;
}

std::vector<SlotView> App::DriverSlots()
{
    // A driver with banks returns the banks of the sound's slots. Otherwise the driver's samples are the programs.
    const std::vector<int>& samples = DriverSampleList();
    if (sequence_)
    {
        std::vector<SlotView> banks;
        for (song::DriverBankSlot& bank : played_.Driver()->BankSlots(sequence_->sound))
        {
            banks.push_back({std::move(bank.bank), std::move(bank.programs)});
        }
        if (!banks.empty())
        {
            return banks;
        }
    }
    SlotView slot;
    slot.bank = std::string(played_.Driver()->Rules().Title());
    slot.programs.assign(samples.begin(), samples.end());

    return {slot};
}

void App::ShowDriverPrograms()
{
    if (!played_.Driver() || !song_ || !sequence_)
    {
        return;
    }

    // A PSG track selects a duty or a wave, and the noise channel neither. The other tracks' notes name their samples,
    // or the tracks select one (song::DriverRules::NotesNameSamples).
    DriverSampleList();
    for (std::size_t i = 0; i < song_->tracks.size() && i < sequence_->tracks.size(); i++)
    {
        const int number = sequence_->tracks[i].number;
        if (!played_.Driver()->Rules().PsgTrack(number))
        {
            continue;
        }
        const auto programs = driver_psg_programs_.find(number);
        if (programs != driver_psg_programs_.end() && !programs->second.empty())
        {
            song_->tracks[i].programs = programs->second;
        }
        else
        {
            song_->tracks[i].slot = -1;
        }
    }
}

const std::vector<song::GbaVoiceGroup>& App::GbaGroups()
{
    if (gba_groups_rom_.lock() != played_.Gba())
    {
        gba_groups_ = played_.Gba() ? song::GbaVoiceGroups(*played_.Gba()) : std::vector<song::GbaVoiceGroup>();
        gba_groups_rom_ = played_.Gba();
    }

    return gba_groups_;
}

std::vector<SlotView> App::GbaSlotsOf(uint32_t song)
{
    ntdlib::mp2k::SongHeader header;
    if (!played_.Gba() ||
        !ntdlib::mp2k::ReadSong(played_.Gba()->rom, played_.Gba()->info, static_cast<int>(song), header))
    {
        return {};
    }

    SlotView slot;
    slot.bank = song::GbaVoiceGroupName(header.voices);
    for (const song::GbaVoiceGroup& group : GbaGroups())
    {
        if (group.address == header.voices)
        {
            const std::vector<int> listed = song::GbaListedPrograms(*played_.Gba(), group);
            slot.programs.assign(listed.begin(), listed.end());
        }
    }

    return {slot};
}

} // namespace hokuto::app
