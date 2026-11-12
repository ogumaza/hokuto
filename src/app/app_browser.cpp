// SPDX-License-Identifier: MIT

// The browser: the lists of each console, the slots of a sound, opening the chosen item, reading the shown sequence,
// and following a new version. app_browser_gba.cpp contains the lists of the GBA, and app_browser_ds.cpp those of the
// DS and DSE.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <map>
#include <numeric>
#include <utility>

#include "app/app.h"
#include "app/app_browser_parts.h"
#include "app/app_internal.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/items.h"
#include "song/streams.h"

namespace hokuto::app
{

using browser_parts::ChildName;
using browser_parts::kOpenBankEditor;
using browser_parts::kStreamProperties;
using browser_parts::kWaveProperties;

namespace
{

// What the browser lists under bank or wave archive `index` of `archive`, by the item's group: a bank's programs, or a
// wave archive's waves. A bank or a wave archive that can't be read lists none.
std::vector<uint32_t> ChildrenOf(const ntdlib::sar::SoundArchive& archive, std::size_t group, std::size_t index)
{
    std::vector<uint32_t> children;
    try
    {
        if (group == kBanksGroup)
        {
            children = song::ProgramsOf(archive, index);
        }
        else
        {
            children.resize(song::WaveCountOf(archive, index));
            std::iota(children.begin(), children.end(), 0u);
        }
    }
    catch (const std::exception&)
    {
        children.clear();
    }

    return children;
}

// Why a stream sound doesn't export: the file holds no stream for it. `name` is the stream file's name.
std::string MissingStreamNote(const std::string& name)
{
    return "The file has no stream for this sound. The stream file " + name +
           " wasn't next to the archive when it was imported. Import the archive again with the stream file next to "
           "it.";
}

// The item `item_name` of a file of the Wii or of a later console, number `index` of `group`, with `children` named
// `child_names`.
BrowserItem BrowserItemOf(std::size_t group, const std::string& item_name, std::size_t index,
                          std::vector<uint32_t> children, std::vector<std::string> child_names, bool exports)
{
    BrowserItem item;
    item.name = item_name;
    item.children = std::move(child_names);
    item.exports = exports;
    item.children_export = group == kBanksGroup || group == kWavesGroup || group == kWaveSoundFilesGroup;
    item.number = static_cast<uint32_t>(index);
    item.child_numbers = std::move(children);

    return item;
}

// The names of `children`, the children of an item of `group`.
std::vector<std::string> ChildNames(std::size_t group, const std::vector<uint32_t>& children)
{
    std::vector<std::string> child_names;
    for (const uint32_t child : children)
    {
        child_names.push_back(ChildName(group, child));
    }

    return child_names;
}

// Lists the banks of `archive` in `browser`.
void ListWiiBanks(const ntdlib::rsar::SoundArchive& archive, bool replaces, std::vector<BrowserGroup>& browser)
{
    for (std::size_t i = 0; i < archive.Banks().size(); i++)
    {
        std::vector<uint32_t> programs;
        try
        {
            programs = song::ProgramsOf(archive, i);
        }
        catch (const std::exception&)
        {
            programs.clear();
        }
        browser[kBanksGroup].items.push_back(
            BrowserItemOf(kBanksGroup, archive.Banks()[i].name, i, programs, ChildNames(kBanksGroup, programs), true));
        browser[kBanksGroup].items.back().replaces = replaces;
    }
}

// Lists the wave data of the banks and of the wave sound files of `archive` in `browser`, as wave archives.
void ListWiiWaveArchives(const ntdlib::rsar::SoundArchive& archive, bool replaces, std::vector<BrowserGroup>& browser)
{
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(archive))
    {
        std::vector<uint32_t> waves;
        try
        {
            waves.resize(song::WaveCountOf(archive, wave_archive.file_id));
            std::iota(waves.begin(), waves.end(), 0u);
        }
        catch (const std::exception&)
        {
            waves.clear();
        }
        browser[kWavesGroup].items.push_back(BrowserItemOf(kWavesGroup, wave_archive.name, wave_archive.file_id, waves,
                                                           ChildNames(kWavesGroup, waves), true));
        browser[kWavesGroup].items.back().replaces = replaces;
        browser[kWavesGroup].items.back().children_replace = replaces;
    }
}

// Lists the wave sound files of `archive` in `browser`.
void ListWiiWaveSoundFiles(const ntdlib::rsar::SoundArchive& archive, bool replaces, std::vector<BrowserGroup>& browser)
{
    for (const song::WaveSoundSet& set : song::WaveSoundSets(archive))
    {
        std::vector<std::string> sounds;
        for (const uint32_t sound : set.sounds)
        {
            sounds.push_back(archive.Sounds()[sound].name);
        }
        browser[kWaveSoundFilesGroup].items.push_back(
            BrowserItemOf(kWaveSoundFilesGroup, set.name, set.sounds.front(), set.sounds, std::move(sounds), true));
        browser[kWaveSoundFilesGroup].items.back().replaces = replaces;
        browser[kWaveSoundFilesGroup].items.back().children_replace = replaces;
        browser[kWaveSoundFilesGroup].items.back().children_found = true;
    }
}

} // namespace

namespace browser_parts
{

std::string ChildName(std::size_t group, uint32_t child)
{
    return (group == kBanksGroup ? "Program " : "Wave ") + std::to_string(child);
}

} // namespace browser_parts

void App::OpenBrowsed()
{
    // A click on a bank's instrument, a wave archive's wave, a wave sound or a stream sound plays it, or stops it while
    // it plays. A preview that has played to its end no longer shows as playing.
    if (std::exchange(browser_state_.clicked, false))
    {
        if (browser_state_.playing == browser_state_.selected)
        {
            worker_.Stop();
            browser_state_.playing.reset();
        }
        else
        {
            PlayPreview(browser_state_.selected);
        }
    }
    if (browser_state_.playing && !worker_.Playing() && ring_.Available() == 0)
    {
        browser_state_.playing.reset();
    }

    // A sequence chosen in the browser opens. The other items don't show in the zones yet.
    if (browser_state_.selected.group == shown_group_ && browser_state_.selected.item == shown_item_)
    {
        return;
    }

    // The zones go on showing the last sequence until the next one has been read. Another choice abandons the read.
    shown_group_ = browser_state_.selected.group;
    shown_item_ = browser_state_.selected.item;
    ReadShown();
}

void App::ReadShown()
{
    // A request without a conversion converts nothing (ConvertFor).
    converting_engine_.reset();
    converting_version_.reset();
    const auto& sequences = browser_[kSequencesGroup].items;
    const std::shared_ptr<const format::File> version = history_ ? history_->Current() : nullptr;
    const bool browsed = shown_group_ == static_cast<int>(kSequencesGroup) && shown_item_ >= 0 &&
                         shown_item_ < static_cast<int>(sequences.size());
    const bool readable = version && version->native && archive_version_ && (browsed || sequence_);

    // A version without the sound, such as the one before the import that added it, closes the zones' sequence, on the
    // Wii too. The browser then lists the version's items.
    if ((played_.Sar() || played_.Wii()) && readable && version->native->tables)
    {
        if (ShownSequenceSound(browsed) >= version->native->tables->sounds.size())
        {
            CloseSequence();
            BuildArchive(version);
            return;
        }
    }

    // A song of the GBA reads from the open ROM when it serves the version, and from the version's ROM otherwise. A
    // song has no variables. On another console's engine, it plays its conversion for that console. The reader makes
    // the conversion too.
    if (played_.Gba() && readable)
    {
        ReadShownGba(version, ShownSequenceSound(browsed));
        return;
    }

    // A song of the GBA's other drivers does the same.
    if (played_.Driver() && readable)
    {
        ReadShownDriver(version, ShownSequenceSound(browsed));
        return;
    }

    // A sequence of the DS reads from the open archive when it serves the version, and from the version's archive
    // otherwise, with the variables' starting values and changes that the user chose. On another console's engine, it
    // plays its conversion for that console. The reader makes the conversion too.
    if (played_.Ds() && readable)
    {
        ReadShownDs(version, ShownSequenceSound(browsed));
        return;
    }

    // A sound of DSE reads from the open set when it serves the version, and from the version's set otherwise. It uses
    // no starting values and no changes. On another console's engine, it plays its conversion for that console. The
    // reader makes the conversion too.
    if (played_.Dse() && readable)
    {
        ReadShownDse(version, ShownSequenceSound(browsed));
        return;
    }

    // A sequence of the Wii reads from the open archive when it serves the version, and from the version's archive
    // otherwise. A sound on another console's engine plays its conversion for that console. The reader makes the
    // conversion too.
    if (played_.Wii() && readable)
    {
        ReadShownWii(version, ShownSequenceSound(browsed));
        return;
    }

    if (!played_.Sar() || !version || !archive_version_ || (!browsed && !sequence_))
    {
        reader_.Abandon();
        reading_version_.reset();
        if (ArchiveOpen() && version && version->native && archive_version_ &&
            !SameArchive(*version, *archive_version_))
        {
            BuildArchive(version);
        }
        return;
    }

    // The open archive serves the version that it came from. While the browser shows another kind of item, the zones go
    // on showing their sequence. The variables start with the values that the user chose, and change as the user asked.
    ReadShownSar(version, ShownSequenceSound(browsed));
}

uint32_t App::ShownSequenceSound(bool browsed) const
{
    return browsed ? browser_[kSequencesGroup].items[static_cast<std::size_t>(shown_item_)].number : sequence_->sound;
}

void App::ReadShownGba(const std::shared_ptr<const format::File>& version, uint32_t sound)
{
    if (!SameArchive(*version, *archive_version_))
    {
        reading_version_ = version;
        reader_.Request(song::SequenceReader::GbaBuild([version] { return GbaRomOf(*version); }), sound,
                        ConvertFor(version, sound));
        return;
    }

    reading_version_.reset();
    if (sequence_ && sequence_->sound == sound && EngineServes(*version, sound))
    {
        reader_.Abandon();
    }
    else
    {
        reader_.Request(played_.Gba(), sound, ConvertFor(version, sound));
    }
}

void App::ReadShownDriver(const std::shared_ptr<const format::File>& version, uint32_t sound)
{
    if (!SameArchive(*version, *archive_version_))
    {
        reading_version_ = version;
        reader_.Request(song::SequenceReader::DriverBuild([version] { return DriverRomOf(*version); }), sound,
                        ConvertFor(version, sound));
        return;
    }

    reading_version_.reset();
    if (sequence_ && sequence_->sound == sound && EngineServes(*version, sound))
    {
        reader_.Abandon();
    }
    else
    {
        reader_.Request(played_.Driver(), sound, ConvertFor(version, sound));
    }
}

void App::ReadShownDs(const std::shared_ptr<const format::File>& version, uint32_t sound)
{
    song::StartValues values = StartValuesOf(sound);
    song::Changes changes = ChangesOf(sound);
    if (!SameArchive(*version, *archive_version_))
    {
        reading_version_ = version;
        const auto build = [version, game = GameOf(played_.Ds())]
        {
            return DsArchiveOf(*version, game);
        };
        reader_.Request(song::SequenceReader::DsBuild(build), sound, std::move(values), std::move(changes),
                        ConvertFor(version, sound));
        return;
    }

    reading_version_.reset();
    if (sequence_ && sequence_->sound == sound && sequence_->start_values == values && sequence_->changes == changes &&
        EngineServes(*version, sound))
    {
        reader_.Abandon();
    }
    else
    {
        reader_.Request(played_.Ds(), sound, std::move(values), std::move(changes), ConvertFor(version, sound));
    }
}

void App::ReadShownDse(const std::shared_ptr<const format::File>& version, uint32_t sound)
{
    if (!SameArchive(*version, *archive_version_))
    {
        reading_version_ = version;
        const auto build = [version]
        {
            return DseSetOf(*version);
        };
        reader_.Request(song::SequenceReader::DseBuild(build), sound, ConvertFor(version, sound));
        return;
    }

    reading_version_.reset();
    if (sequence_ && sequence_->sound == sound && EngineServes(*version, sound))
    {
        reader_.Abandon();
    }
    else
    {
        reader_.Request(played_.Dse(), sound, ConvertFor(version, sound));
    }
}

void App::ReadShownWii(const std::shared_ptr<const format::File>& version, uint32_t sound)
{
    song::StartValues values = StartValuesOf(sound);
    song::Changes changes = ChangesOf(sound);
    const ntdlib::SndBuild snd_build = format::BuildOfWiiArchive(*version->native);
    if (!SameArchive(*version, *archive_version_))
    {
        reading_version_ = version;
        const auto build = [version, name = file_name_]
        {
            return WiiArchiveOf(*version, name);
        };
        reader_.Request(song::SequenceReader::WiiBuild(build), sound, std::move(values), std::move(changes), snd_build,
                        ConvertFor(version, sound));
        return;
    }

    reading_version_.reset();
    if (sequence_ && sequence_->sound == sound && sequence_->start_values == values && sequence_->changes == changes &&
        sequence_->wii_build == snd_build && EngineServes(*version, sound))
    {
        reader_.Abandon();
    }
    else
    {
        reader_.Request(played_.Wii(), sound, std::move(values), std::move(changes), snd_build,
                        ConvertFor(version, sound));
    }
}

void App::ReadShownSar(const std::shared_ptr<const format::File>& version, uint32_t sound)
{
    song::StartValues values = StartValuesOf(sound);
    song::Changes changes = ChangesOf(sound);

    // A sound on another console's engine plays its conversion for that console. The reader makes the conversion too.
    if (SameArchive(*version, *archive_version_))
    {
        reading_version_.reset();
        if (sequence_ && sequence_->sound == sound && sequence_->start_values == values &&
            sequence_->changes == changes && EngineServes(*version, sound))
        {
            reader_.Abandon();
        }
        else
        {
            reader_.Request(played_.Sar(), sound, std::move(values), std::move(changes), ConvertFor(version, sound));
        }
        return;
    }

    // Another version builds its archive, with the group files that the file keeps.
    reading_version_ = version;
    const std::string name = file_name_;
    const auto build = [version, name]
    {
        return SarArchiveOf(*version, name);
    };
    reader_.Request(build, sound, std::move(values), std::move(changes), ConvertFor(version, sound));
}

void App::BuildArchive(const std::shared_ptr<const format::File>& version)
{
    try
    {
        played_ = played_.Gba()      ? song::Played(GbaRomOf(*version))
                  : played_.Driver() ? song::Played(DriverRomOf(*version))
                  : played_.Ds()     ? song::Played(DsArchiveOf(*version, played_.Ds()->game))
                  : played_.Dse()    ? song::Played(DseSetOf(*version))
                  : played_.Wii()    ? song::Played(WiiArchiveOf(*version, file_name_))
                                     : song::Played(SarArchiveOf(*version, file_name_));
        archive_version_ = version;
        FollowItems();
    }
    catch (const std::exception& error)
    {
        ShowMessage(std::string("Can't build the archive for this version. ") + format::AsSentence(error.what()));
    }
}

void App::ListBrowser()
{
    for (std::size_t group = 0; group < browser_.size(); group++)
    {
        browser_[group].items.clear();
    }

    // The DS, DSE and the GBA's drivers keep no wave sound files.
    browser_[kWaveSoundFilesGroup].hidden =
        played_.Ds() != nullptr || played_.Dse() != nullptr || played_.Gba() != nullptr || played_.Driver() != nullptr;

    // Replace… replaces a whole bank. The Bank Editor edits one of its programs.
    browser_[kBanksGroup].children_replace_note =
        played_.Gba() ? "Replace takes the whole voice group. Edit this voice in the Bank Editor."
                      : "Replace takes the whole bank. Edit this instrument in the Bank Editor.";

    if (played_.Wii())
    {
        ListWiiBrowser();
        return;
    }
    if (played_.Gba())
    {
        ListGbaBrowser();
        return;
    }
    if (played_.Ds())
    {
        ListDsBrowser();
        return;
    }
    if (played_.Dse())
    {
        ListDseBrowser();
        return;
    }
    if (played_.Driver())
    {
        ListDriverBrowser();
        return;
    }

    ListSarBrowser();
}

void App::ListSarBrowser()
{
    const format::File& version = *history_->Current();
    const bool native = static_cast<bool>(version.native);

    ListSarSounds(version);

    // A bank lists its instruments by their programs, and a wave archive its waves. A bank or a wave archive that can't
    // be read lists none.
    for (std::size_t i = 0; i < played_.Sar()->Banks().size(); i++)
    {
        AddSarBrowserItem(kBanksGroup, played_.Sar()->Banks()[i].name, i, ChildrenOf(*played_.Sar(), kBanksGroup, i),
                          true);
        browser_[kBanksGroup].items.back().replaces = native;
        if (native)
        {
            browser_[kBanksGroup].items.back().edit = kOpenBankEditor;
            browser_[kBanksGroup].items.back().children_edit = kOpenBankEditor;
        }
    }
    for (std::size_t i = 0; i < played_.Sar()->WaveArchives().size(); i++)
    {
        AddSarBrowserItem(kWavesGroup, played_.Sar()->WaveArchives()[i].name, i,
                          ChildrenOf(*played_.Sar(), kWavesGroup, i), true);
        browser_[kWavesGroup].items.back().replaces = native;
        browser_[kWavesGroup].items.back().children_replace = native;
        if (native)
        {
            browser_[kWavesGroup].items.back().children_edit = kWaveProperties;
            browser_[kWavesGroup].items.back().children_drag = true;
        }
    }

    // A wave sound file lists its wave sounds, and goes by its first. A file can take its place.
    for (const song::WaveSoundSet& set : song::WaveSoundSets(*played_.Sar()))
    {
        AddSarBrowserItem(kWaveSoundFilesGroup, set.name, set.sounds.front(), set.sounds, true);
        browser_[kWaveSoundFilesGroup].items.back().replaces = native;
        browser_[kWaveSoundFilesGroup].items.back().children_replace = native;
        browser_[kWaveSoundFilesGroup].items.back().children_found = true;
    }

    MarkMissingGroupItems();
}

void App::AddSarBrowserItem(std::size_t group, const std::string& item_name, std::size_t index,
                            std::vector<uint32_t> children, bool exports)
{
    std::vector<std::string> child_names;
    for (const uint32_t child : children)
    {
        child_names.push_back(group == kWaveSoundFilesGroup ? played_.Sar()->Sounds()[child].name
                                                            : ChildName(group, child));
    }
    browser_[group].items.push_back(
        BrowserItemOf(group, item_name, index, std::move(children), std::move(child_names), exports));
}

void App::ListSarSounds(const format::File& version)
{
    // A sequence sound's sequence can be replaced when its file holds one of the file's sequences, and the tables say
    // where the sound starts. A wave sound plays on a click. So does a stream sound: a message says why one whose
    // stream the file lacks can't play. A file can take the place of a bank's, a wave archive's or a wave sound's file,
    // and a WAV file the place of a wave. A stream file or a WAV file can take the place of a stream sound's stream, a
    // file beside the archive.
    const bool native = static_cast<bool>(version.native);
    const std::vector<std::optional<edit::SoundPlace>> places =
        start_fields_ ? edit::SoundPlaces(*played_.Sar(), file_sequences_, *start_fields_)
                      : std::vector<std::optional<edit::SoundPlace>>{};
    for (std::size_t i = 0; i < played_.Sar()->Sounds().size(); i++)
    {
        const ntdlib::sar::SoundInfo& sound = played_.Sar()->Sounds()[i];
        const bool sequence = sound.type == ntdlib::sar::SoundType::kSequence;
        const bool wave_sound = sound.type == ntdlib::sar::SoundType::kWaveSound;
        const std::optional<uint32_t> stream =
            native ? song::StreamFileOf(*played_.Sar(), static_cast<uint32_t>(i)) : std::nullopt;
        const bool kept = stream && version.native->streams.contains(*stream);
        const bool aac = kept && song::IsAdts(*version.native->streams.at(*stream).bytes);
        const std::size_t group = sequence ? kSequencesGroup : kSoundsGroup;
        AddSarBrowserItem(group, sound.name, i, {}, sequence || wave_sound || kept);
        BrowserItem& item = browser_[group].items.back();
        item.replaces = native && ((i < places.size() && places[i]) || wave_sound || stream.has_value());
        item.plays = wave_sound || (native && sound.type == ntdlib::sar::SoundType::kStream);
        if (kept)
        {
            item.edit = kStreamProperties;
            item.edits = !aac;
            item.edit_note = aac ? "Hokuto doesn't show properties for AAC streams. To change the loop, use Replace… "
                                   "with a WAV file that has loop points."
                                 : "";
        }
        if (sound.type == ntdlib::sar::SoundType::kStream && !aac)
        {
            item.export_note = MissingStreamNote(song::StreamFileName(*played_.Sar(), static_cast<uint32_t>(i)));
        }
        if (sound.type == ntdlib::sar::SoundType::kStream && native && !stream)
        {
            item.replace_note =
                "The archive has no path for this stream in its folder. Hokuto can replace only streams stored there.";
        }
    }
}

void App::MarkMissingGroupItems()
{
    // An item whose file lies in a group file that the Hokuto file lacks shows grey, and says why. Its commands then
    // wait for a new import of the archive with the group file beside it.
    const std::map<uint32_t, std::vector<std::string>> missing = song::FilesInMissingGroups(*played_.Sar());
    if (missing.empty())
    {
        return;
    }

    const auto file_of = [this](std::size_t group, uint32_t index) -> std::optional<uint32_t>
    {
        if (group == kBanksGroup)
        {
            return played_.Sar()->Banks()[index].file_id;
        }
        if (group == kWavesGroup)
        {
            return played_.Sar()->WaveArchives()[index].file_id;
        }

        const ntdlib::sar::SoundInfo& sound = played_.Sar()->Sounds()[index];
        return sound.type == ntdlib::sar::SoundType::kStream ? std::nullopt : std::optional<uint32_t>(sound.file_id);
    };
    for (const std::size_t group : {kSequencesGroup, kSoundsGroup, kBanksGroup, kWavesGroup, kWaveSoundFilesGroup})
    {
        for (std::size_t i = 0; i < browser_[group].items.size(); i++)
        {
            const std::optional<uint32_t> file = file_of(group, browser_[group].items[i].number);
            const auto found = file ? missing.find(*file) : missing.end();
            if (found != missing.end())
            {
                BrowserItem& item = browser_[group].items[i];
                item.missing = song::MissingGroupNote(item.name, found->second);
                item.plays = false;
            }
        }
    }
}

void App::ListWiiBrowser()
{
    // The wave data of the banks and of the wave sound files lists as wave archives, by their files' IDs
    // (song::WaveArchivesOf). A bank or a wave archive that can't be read lists no children. Every item exports but a
    // stream sound whose stream the file lacks, and Export… says why. A wave sound file's wave sounds export too, and
    // so do a wave archive's waves. A wave sound and a stream sound play on a click, and a stream sound's stream opens
    // in its properties. A sequence sound's sequence, a bank's file, a wave sound's file and a wave archive that is an
    // RWAR take a file of the Wii in their place. A wave takes a WAV file or a wave file of the Wii, and a stream
    // sound's stream a stream file or a WAV file.
    const ntdlib::rsar::SoundArchive& archive = *played_.Wii();
    const format::File& version = *history_->Current();
    const bool replaces = static_cast<bool>(version.native);
    const std::vector<std::optional<edit::SoundPlace>> places =
        start_fields_ ? edit::SoundPlaces(archive, file_sequences_, *start_fields_)
                      : std::vector<std::optional<edit::SoundPlace>>{};

    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const ntdlib::rsar::SoundInfo& sound = archive.Sounds()[i];
        const bool sequence = sound.type == ntdlib::rsar::SoundType::kSequence;
        const bool wave_sound = sound.type == ntdlib::rsar::SoundType::kWaveSound;
        const std::optional<uint32_t> stream =
            version.native ? song::StreamFileOf(archive, static_cast<uint32_t>(i)) : std::nullopt;
        const bool kept = stream && version.native->streams.contains(*stream);
        const std::size_t group = sequence ? kSequencesGroup : kSoundsGroup;
        browser_[group].items.push_back(BrowserItemOf(group, sound.name, i, {}, {}, sequence || wave_sound || kept));
        BrowserItem& item = browser_[group].items.back();
        item.replaces = replaces && ((i < places.size() && places[i]) || wave_sound || stream.has_value());
        item.plays = wave_sound || (version.native && sound.type == ntdlib::rsar::SoundType::kStream);
        if (kept)
        {
            item.edit = kStreamProperties;
        }
        else if (sound.type == ntdlib::rsar::SoundType::kStream)
        {
            item.export_note = MissingStreamNote(song::StreamFileName(archive, static_cast<uint32_t>(i)));
        }
    }
    ListWiiBanks(archive, replaces, browser_);
    ListWiiWaveArchives(archive, replaces, browser_);
    ListWiiWaveSoundFiles(archive, replaces, browser_);

    // A bank opens in the Bank Editor, and a wave archive's wave in its properties. A wave drags onto a bank.
    for (BrowserItem& item : browser_[kBanksGroup].items)
    {
        item.edit = kOpenBankEditor;
        item.children_edit = kOpenBankEditor;
    }
    for (BrowserItem& item : browser_[kWavesGroup].items)
    {
        item.children_edit = kWaveProperties;
        item.children_drag = true;
    }
}

void App::FollowItems()
{
    if (!ArchiveOpen())
    {
        return;
    }
    if (!archive_version_)
    {
        ListChildren();
        return;
    }

    // A ROM's edits keep its songs, and DSE's edits keep its sounds. The DS's edits can add sounds, and each version
    // contains the sequences of its sounds.
    const std::size_t sounds = browser_[kSoundsGroup].items.size() + browser_[kSequencesGroup].items.size();
    const bool same =
        played_.Gba() || played_.Dse() || played_.Driver() ||
        (played_.Ds()    ? false
         : played_.Wii() ? sounds == played_.Wii()->Sounds().size() &&
                               browser_[kBanksGroup].items.size() == played_.Wii()->Banks().size() &&
                               browser_[kWavesGroup].items.size() == song::WaveArchivesOf(*played_.Wii()).size()
                         : sounds == played_.Sar()->Sounds().size() &&
                               browser_[kBanksGroup].items.size() == played_.Sar()->Banks().size() &&
                               browser_[kWavesGroup].items.size() == played_.Sar()->WaveArchives().size());

    // The places of the sequences and of the starts follow the version that the archive came from.
    if (!same)
    {
        ReadFileSequences();
    }

    // The browser lists the items again. Its choices of items that went go too. Items go and come only at the ends of
    // the lists, and the others keep their places.
    ListBrowser();
    DropUnlistedChoices();

    // The browser selects the sound that an import added, and scrolls to it. The zones then show it.
    if (const std::optional<uint32_t> sound = std::exchange(select_sound_, std::nullopt))
    {
        const std::vector<BrowserItem>& sequences = browser_[kSequencesGroup].items;
        const auto found = std::ranges::find(sequences, *sound, &BrowserItem::number);
        if (found != sequences.end())
        {
            browser_state_.selected = {static_cast<int>(kSequencesGroup), static_cast<int>(found - sequences.begin())};
            browser_state_.reveal = true;
        }
    }
}

void App::ReadFileSequences()
{
    try
    {
        if (played_.Ds())
        {
            file_sequences_ = format::DsSoundSequences(*archive_version_, played_.Ds()->bytes);
        }
        else
        {
            const std::vector<uint8_t> bytes = format::ExportArchive(*archive_version_);
            file_sequences_ = played_.Wii() ? format::SequencesOfFiles(*archive_version_, bytes, *played_.Wii())
                                            : format::SequencesOfFiles(*archive_version_, bytes, *played_.Sar());
            start_fields_ = StartFieldsOf(*archive_version_, bytes);
        }
    }
    catch (const std::exception&)
    {
        file_sequences_.clear();
        start_fields_.reset();
    }
}

void App::DropUnlistedChoices()
{
    const auto listed = [this](int group, int item)
    {
        return group >= 0 && item >= 0 && static_cast<std::size_t>(group) < browser_.size() &&
               static_cast<std::size_t>(item) < browser_[static_cast<std::size_t>(group)].items.size();
    };
    if (!listed(browser_state_.selected.group, browser_state_.selected.item))
    {
        browser_state_.selected = {};
    }
    if (browser_state_.playing && !listed(browser_state_.playing->group, browser_state_.playing->item))
    {
        browser_state_.playing.reset();
    }
    std::erase_if(browser_state_.open,
                  [&listed](const std::pair<int, int>& open) { return !listed(open.first, open.second); });
    if (!listed(shown_group_, shown_item_))
    {
        shown_group_ = -1;
        shown_item_ = -1;
    }
}

void App::ListChildren()
{
    // On the Wii, a bank's children are the programs that play a region, and a wave archive's the waves of its file's
    // wave data. A bank or a wave archive that can't be read lists none.
    if (!played_.Sar() && !played_.Wii())
    {
        return;
    }

    const auto children_of = [this](std::size_t group, uint32_t index)
    {
        if (!played_.Wii())
        {
            return ChildrenOf(*played_.Sar(), group, index);
        }

        std::vector<uint32_t> children;
        try
        {
            if (group == kBanksGroup)
            {
                children = song::ProgramsOf(*played_.Wii(), index);
            }
            else
            {
                children.resize(song::WaveCountOf(*played_.Wii(), index));
                std::iota(children.begin(), children.end(), 0u);
            }
        }
        catch (const std::exception&)
        {
            children.clear();
        }

        return children;
    };
    for (const std::size_t group : {kBanksGroup, kWavesGroup})
    {
        for (std::size_t i = 0; i < browser_[group].items.size(); i++)
        {
            std::vector<uint32_t> children = children_of(group, browser_[group].items[i].number);
            std::vector<std::string>& names = browser_[group].items[i].children;
            names.clear();
            for (const uint32_t child : children)
            {
                names.push_back(ChildName(group, child));
            }
            browser_[group].items[i].child_numbers = std::move(children);
        }
    }
}

std::optional<uint32_t> App::BrowsedSound() const
{
    // A wave sound file's wave sound shows too. On the DS, the sounds' list contains streams, by stream numbers: the
    // section edits sequences.
    const BrowserPlace& place = browser_state_.selected;
    const bool sound = (place.group == static_cast<int>(kSoundsGroup) && !played_.Ds()) ||
                       place.group == static_cast<int>(kSequencesGroup);
    const bool listed = place.group == static_cast<int>(kWaveSoundFilesGroup) && place.child >= 0 && place.item >= 0;
    if (listed)
    {
        const std::vector<BrowserItem>& items = browser_[kWaveSoundFilesGroup].items;
        const auto item = static_cast<std::size_t>(place.item);
        const auto child = static_cast<std::size_t>(place.child);
        return item < items.size() && child < items[item].child_numbers.size()
                   ? std::optional<uint32_t>(items[item].child_numbers[child])
                   : std::nullopt;
    }
    if (!sound || place.child >= 0 || place.item < 0)
    {
        return std::nullopt;
    }

    const std::vector<BrowserItem>& items = browser_[static_cast<std::size_t>(place.group)].items;
    const auto item = static_cast<std::size_t>(place.item);
    return item < items.size() ? std::optional<uint32_t>(items[item].number) : std::nullopt;
}

} // namespace hokuto::app
