// SPDX-License-Identifier: MIT

// The properties of a wave or a stream: the window, its edits, the values of wave sounds, and the measure of the
// groups' sizes.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <future>
#include <utility>

#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/encodings.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "app/export_work.h"
#include "edit/ds_streams.h"
#include "edit/ds_waves.h"
#include "edit/dse_banks.h"
#include "edit/streams.h"
#include "edit/waves.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/items.h"
#include "song/preview.h"
#include "song/streams.h"

namespace hokuto::app
{

namespace
{

// The DS encoding that matches `encoding`, the encoding of a sound archive that the properties window shows for a wave
// or a stream.
ntdlib::sdat::WaveEncoding DsEncodingOf(ntdlib::sar::WaveEncoding encoding)
{
    return ntdlib::sdat::EncodingOf(encoding).value_or(ntdlib::sdat::WaveEncoding::kImaAdpcm);
}

} // namespace

void App::OpenWaveProperties(uint32_t wave_archive, uint32_t wave)
{
    const WaveTarget target{wave_archive, wave};
    if (properties_wave_ != target)
    {
        CloseProperties();
        properties_wave_ = target;
    }
    ImGui::SetWindowFocus("###WaveProperties");
}

void App::OpenStreamProperties(uint32_t sound)
{
    if (properties_stream_ != sound)
    {
        CloseProperties();
        properties_stream_ = sound;
    }
    ImGui::SetWindowFocus("###WaveProperties");
}

void App::CloseProperties()
{
    if (wave_state_.playing)
    {
        worker_.Stop();
    }
    properties_wave_.reset();
    properties_stream_.reset();
    wave_view_.reset();
    wave_view_version_.reset();
    wave_state_ = {};
    DropGroupSizes(wave_group_sizes_);
}

void App::UpdateProperties()
{
    if (!properties_wave_ && !properties_stream_)
    {
        return;
    }

    // On the Wii, a wave's wave archive is the wave data of a bank or of a wave sound file, by its file's ID.
    const format::File* document = Document();
    const bool wii = played_.Wii() && properties_wave_;
    const bool wii_stream = played_.Wii() && properties_stream_ && *properties_stream_ < played_.Wii()->Sounds().size();
    const bool ds = played_.Ds() && properties_wave_;
    const bool ds_stream = played_.Ds() && properties_stream_;
    const bool dse = played_.Dse() && properties_wave_;
    const bool listed =
        wii || wii_stream || ds || ds_stream || dse ||
        (played_.Sar() && (properties_wave_ ? properties_wave_->wave_archive < played_.Sar()->WaveArchives().size()
                                            : *properties_stream_ < played_.Sar()->Sounds().size()));
    if (document == nullptr || !archive_version_ || !listed)
    {
        CloseProperties();
        return;
    }

    // The view follows each archive that the program builds, and each move in the history back to the archive that the
    // program holds (FollowHistory). A version without the wave, such as the one before the import that added it,
    // closes the window too. On the Wii, so does a version without the wave's wave data.
    if (wave_view_version_ != archive_version_ && !RebuildWaveView(wii, wii_stream, ds, ds_stream, dse))
    {
        return;
    }

    // While the program builds the archive of another version, or another window waits, the window takes no edits. Play
    // shows as Stop until the item has played to its end.
    const bool busy = EditorsWait();
    if (wave_state_.playing && !worker_.Playing() && ring_.Available() == 0)
    {
        wave_state_.playing = false;
    }
    TakeGroupSizes(wave_group_sizes_, wave_view_->sizes);
    bool open = true;
    // The item of the window's fields: a wave by its wave archive and its index, or else a stream sound.
    const ShownItem item = properties_wave_
                               ? ItemShown(0).At({properties_wave_->wave_archive, properties_wave_->wave, 0})
                               : ItemShown(1).At({properties_stream_.value_or(0), 0, 0});
    const WaveEdit wave_edit = DrawWaveWindow(*wave_view_, wave_state_, item, busy, &open);
    if (!open)
    {
        CloseProperties();
        return;
    }

    ApplyWaveEdit(wave_edit);
}

bool App::RebuildWaveView(bool wii, bool wii_stream, bool ds, bool ds_stream, bool dse)
{
    try
    {
        // On the DS and on DSE, a version without the wave or the stream closes the window.
        if (dse || ds_stream || ds)
        {
            return RebuildDsWaveView(ds_stream, dse);
        }

        return RebuildSarWaveView(wii, wii_stream);
    }
    catch (const std::exception& error)
    {
        ShowMessage(std::string(properties_wave_ ? "Can't show this wave. " : "Can't show this stream. ") +
                    format::AsSentence(error.what()));
        CloseProperties();
        return false;
    }
}

bool App::RebuildDsWaveView(bool ds_stream, bool dse)
{
    if (dse)
    {
        const std::vector<uint32_t> waves =
            properties_wave_->wave_archive < played_.Dse()->banks.size()
                ? song::DseBankWaves(*played_.Dse()->banks[properties_wave_->wave_archive].bank)
                : std::vector<uint32_t>();
        if (std::ranges::find(waves, properties_wave_->wave) == waves.end())
        {
            CloseProperties();
            return false;
        }
        wave_view_ = MakeDseWaveView(*played_.Dse(), properties_wave_->wave_archive, properties_wave_->wave);
        wave_view_version_ = archive_version_;
    }
    else if (ds_stream)
    {
        const auto& streams = played_.Ds()->archive.streams;
        if (*properties_stream_ >= streams.size() || !streams[*properties_stream_])
        {
            CloseProperties();
            return false;
        }
        wave_view_ = MakeDsStreamView(*played_.Ds(), *properties_stream_);
        wave_view_version_ = archive_version_;
    }
    else
    {
        if (properties_wave_->wave >= song::DsWaveCount(*played_.Ds(), properties_wave_->wave_archive))
        {
            CloseProperties();
            return false;
        }
        wave_view_ = MakeDsWaveView(*played_.Ds(), properties_wave_->wave_archive, properties_wave_->wave);
        wave_view_version_ = archive_version_;
    }

    return true;
}

bool App::RebuildSarWaveView(bool wii, bool wii_stream)
{
    if (wii && std::ranges::none_of(song::WaveArchivesOf(*played_.Wii()), [this](const song::WiiWaveArchive& each)
                                    { return each.file_id == properties_wave_->wave_archive; }))
    {
        CloseProperties();
        return false;
    }
    if (properties_wave_ &&
        properties_wave_->wave >= (wii ? song::WaveCountOf(*played_.Wii(), properties_wave_->wave_archive)
                                       : song::WaveCountOf(*played_.Sar(), properties_wave_->wave_archive)))
    {
        CloseProperties();
        return false;
    }

    wave_view_ =
        wii ? MakeWaveView(*archive_version_, *played_.Wii(), properties_wave_->wave_archive, properties_wave_->wave)
        : wii_stream ? MakeStreamView(*archive_version_, *played_.Wii(), *properties_stream_)
        : properties_wave_
            ? MakeWaveView(*archive_version_, *played_.Sar(), properties_wave_->wave_archive, properties_wave_->wave)
            : MakeStreamView(*archive_version_, *played_.Sar(), *properties_stream_);
    wave_view_version_ = archive_version_;
    if (wii)
    {
        MeasureGroups(wave_group_sizes_, properties_wave_->wave_archive);
    }
    else if (properties_wave_)
    {
        MeasureGroups(wave_group_sizes_, played_.Sar()->WaveArchives()[properties_wave_->wave_archive].file_id);
    }

    return true;
}

void App::ApplyWaveEdit(const WaveEdit& wave_edit)
{
    if (!wave_edit.refused.empty())
    {
        ShowMessage(wave_edit.refused);
    }
    if (wave_edit.play)
    {
        PlayProperties();
    }
    if (wave_edit.stop)
    {
        worker_.Stop();
        wave_state_.playing = false;
    }
    if (wave_edit.replace || wave_edit.export_item)
    {
        if (const std::optional<BrowserPlace> place = PropertiesPlace())
        {
            wave_edit.replace ? StartReplace(*place) : StartItemExport(*place);
        }
    }
    if (!wave_edit.values || !wave_view_)
    {
        return;
    }

    const WaveValues& values = *wave_edit.values;
    try
    {
        const format::File& document = *Document();
        format::File file = FileWithWaveValues(document, values);

        // The window shows the new values at once. The view follows the version's archive once the program has built
        // it. The notes of the edit replace those of the last edit.
        const format::File* before = Document();
        Do(wave_edit.name, wave_view_->name, std::move(file));
        if (Document() != before)
        {
            wave_view_->values = values;
            wave_state_.notes = wave_edit.notes;
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

format::File App::FileWithWaveValues(const format::File& document, const WaveValues& values)
{
    // A wave takes the values through its model, and a stream through its file. Without a loop, the loop starts at 0.
    format::File file;
    if (properties_stream_ && played_.Ds())
    {
        // A DS stream's rate and loop change in its HEAD block. Another encoding makes the stream again.
        FollowArchiveVersion();
        edit::DsStreamValues ds;
        ds.rate = values.rate;
        ds.loop = values.loop;
        ds.loop_start = values.loop ? values.loop_start : 0;
        ds.encoding = DsEncodingOf(values.encoding);
        file = edit::WithDsStream(document, *played_.Ds(), *properties_stream_, ds);
    }
    else if (properties_wave_ && played_.Dse())
    {
        // A DSE wave's parameters change where the set contains them. Its encoding stays.
        FollowArchiveVersion();
        edit::DseWaveValues dse;
        dse.rate = values.rate;
        dse.loop = values.loop;
        dse.loop_start = values.loop ? values.loop_start : 0;
        file = edit::WithDseWave(document, *played_.Dse(), properties_wave_->wave_archive, properties_wave_->wave, dse);
    }
    else if (properties_wave_ && played_.Ds())
    {
        // A DS wave's parameters change where the archive contains them, and its end stays. Another encoding makes the
        // wave again, and the archive's files move.
        FollowArchiveVersion();
        edit::DsWaveValues ds;
        ds.rate = values.rate;
        ds.loop = values.loop;
        ds.loop_start = values.loop ? values.loop_start : 0;
        ds.encoding = DsEncodingOf(values.encoding);
        file = edit::WithDsWave(document, *played_.Ds(), properties_wave_->wave_archive, properties_wave_->wave, ds);
    }
    else if (properties_wave_)
    {
        const uint32_t wave_archive = properties_wave_->wave_archive;
        const uint32_t wave = properties_wave_->wave;
        format::WaveModel model = played_.Wii() ? edit::WaveModelOf(document, *played_.Wii(), wave_archive, wave)
                                                : edit::WaveModelOf(document, *played_.Sar(), wave_archive, wave);
        model.encoding = values.encoding;
        model.rate = values.rate;
        model.loop = values.loop;
        model.loop_start = values.loop ? values.loop_start : 0;
        model.loop_end = values.loop_end;
        file = played_.Wii() ? edit::WithWaveModel(document, *played_.Wii(), wave_archive, wave, std::move(model))
                             : edit::WithWaveModel(document, *played_.Sar(), wave_archive, wave, std::move(model));
    }
    else
    {
        const uint32_t sound = *properties_stream_;
        const std::vector<uint8_t>& bytes =
            played_.Wii() ? StreamBytes(document, *played_.Wii(), sound) : StreamBytes(document, *played_.Sar(), sound);
        edit::StreamValues stream = edit::StreamValuesOf(bytes);
        stream.encoding = values.encoding;
        stream.rate = values.rate;
        stream.loop = values.loop;
        stream.loop_start = values.loop ? values.loop_start : 0;
        stream.length = values.loop_end;
        std::vector<uint8_t> made = edit::StreamWithValues(bytes, stream);
        const uint32_t id =
            played_.Wii() ? *song::StreamFileOf(*played_.Wii(), sound) : *song::StreamFileOf(*played_.Sar(), sound);
        file = edit::ReplaceStream(document, id, std::move(made));
    }

    return file;
}

void App::PlayProperties()
{
    if (!wave_view_ || wave_view_->samples.empty())
    {
        return;
    }

    // The item plays as the window shows it: up to its end, then round its loop until Stop.
    ntdlib::sar::Pcm pcm;
    pcm.sample_rate = wave_view_->values.rate;
    pcm.loop = wave_view_->values.loop;
    pcm.loop_start = wave_view_->values.loop_start;
    pcm.loop_end = std::min(wave_view_->values.loop_end, wave_view_->held);
    pcm.channels = wave_view_->samples;
    try
    {
        PlayAlone(std::make_unique<song::WavePreview>(std::move(pcm), true));
        wave_state_.playing = true;
    }
    catch (const std::exception& error)
    {
        ShowMessage(std::string(wave_view_->stream ? "Can't play this stream. " : "Can't play this wave. ") +
                    format::AsSentence(error.what()));
    }
}

void App::ApplyWaveSoundChange(SoundSection& section)
{
    const std::optional<WaveSoundChange> change = std::exchange(section.wave_changed, std::nullopt);
    const format::File* document = Document();
    if (!change || document == nullptr || !section.wave_sound || !section.wave_sound_of || EditorsWait())
    {
        return;
    }

    // The section shows the new values at once. The view follows the version's archive once the program has built it.
    try
    {
        format::File file = played_.Wii() ? edit::WithWaveSoundFields(*document, *played_.Wii(), *section.wave_sound_of,
                                                                      change->values, change->wave)
                                          : edit::WithWaveSoundFields(*document, *played_.Sar(), *section.wave_sound_of,
                                                                      change->values, change->wave);
        Do(change->name, change->details, std::move(file));
        if (Document() != document)
        {
            section.wave_sound->fields.values = change->values;
            section.wave_sound->fields.wave = change->wave;
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ShowWaveSoundIn(SoundSection& section)
{
    // For a new view, a thread measures the groups that load the wave sound file. Their lines join the view once
    // they're ready.
    if (played_.Wii())
    {
        if (ShowWaveSound(section, archive_version_, archive_version_ ? played_.Wii().get() : nullptr, EditorsWait()) &&
            section.wave_sound)
        {
            MeasureGroups(section.wave_group_sizes, played_.Wii()->Sounds()[*section.wave_sound_of].file_id);
        }
    }
    else if (ShowWaveSound(section, archive_version_, archive_version_ ? played_.Sar().get() : nullptr,
                           EditorsWait()) &&
             section.wave_sound)
    {
        MeasureGroups(section.wave_group_sizes, played_.Sar()->Sounds()[*section.wave_sound_of].file_id);
    }
    if (section.wave_sound)
    {
        TakeGroupSizes(section.wave_group_sizes, section.wave_sound->sizes);
    }
    else
    {
        // A section without a wave sound's view, such as one that shows a sound of another kind, drops the measure of
        // its last wave sound.
        DropGroupSizes(section.wave_group_sizes);
    }
}

bool App::EditorsWait() const
{
    const format::File* document = Document();
    return document == nullptr || !ArchiveOpen() || !archive_version_ || reading_version_ != nullptr ||
           !SameArchive(*document, *archive_version_) || replace_.shown || export_.has_value();
}

void App::MeasureGroups(std::future<std::vector<std::string>>& sizes, uint32_t file_id)
{
    // The version and its archive stay as they are. A group that can't be measured gives no line.
    const auto measure = [version = archive_version_, archive = played_.Sar(), wii = played_.Wii() != nullptr, file_id]
    {
        try
        {
            return wii ? WiiGroupSizeLines(*version, file_id) : GroupSizeLines(*version, *archive, file_id);
        }
        catch (const std::exception&)
        {
            return std::vector<std::string>();
        }
    };
    DropGroupSizes(sizes);
    sizes = std::async(std::launch::async, measure);
}

void App::DropGroupSizes(std::future<std::vector<std::string>>& sizes)
{
    if (sizes.valid() && !Done(sizes))
    {
        dropped_group_sizes_.push_back(std::move(sizes));
    }
    sizes = {};
}

std::optional<BrowserPlace> App::PropertiesPlace() const
{
    // A wave's row lies under its wave archive's, and a stream sound's among the sounds.
    const std::size_t group = properties_wave_ ? kWavesGroup : kSoundsGroup;
    const uint32_t number = properties_wave_ ? properties_wave_->wave_archive : properties_stream_.value_or(0);
    const std::vector<BrowserItem>& items = browser_[group].items;
    for (std::size_t item = 0; item < items.size(); ++item)
    {
        if (items[item].number != number)
        {
            continue;
        }

        if (!properties_wave_)
        {
            return BrowserPlace{static_cast<int>(group), static_cast<int>(item), -1};
        }

        const std::vector<uint32_t>& children = items[item].child_numbers;
        const auto found = std::ranges::find(children, properties_wave_->wave);
        if (found != children.end())
        {
            return BrowserPlace{static_cast<int>(group), static_cast<int>(item),
                                static_cast<int>(found - children.begin())};
        }
    }

    return std::nullopt;
}

} // namespace hokuto::app
