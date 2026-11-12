// SPDX-License-Identifier: MIT

// The browser's previews of an item, on each console.

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "ntdlib/mp2k/song.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "app/export_work.h"
#include "edit/effects.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/aac.h"
#include "song/items.h"
#include "song/preview.h"
#include "song/streams.h"

namespace hokuto::app
{

void App::PlayPreview(const BrowserPlace& place)
{
    if (played_.Wii())
    {
        PlayWiiPreview(place);
        return;
    }
    if (played_.Gba())
    {
        PlayGbaPreview(place);
        return;
    }
    if (played_.Ds())
    {
        PlayDsPreview(place);
        return;
    }
    if (played_.Dse())
    {
        PlayDsePreview(place);
        return;
    }
    if (played_.Driver())
    {
        PlayDriverPreview(place);
        return;
    }

    PlaySarPreview(place);
}

void App::PlaySarPreview(const BrowserPlace& place)
{
    // Only a bank's instruments, a wave archive's waves, the wave sounds and the stream sounds play by themselves. A
    // wave sound file's wave sounds play too.
    const std::optional<ListedPlace> entry = ListedAt(place);
    if (!played_.Sar() || !entry)
    {
        return;
    }

    const std::size_t group = entry->group;
    const bool bank = group == kBanksGroup;
    const bool listed = entry->child.has_value();
    const bool child = (bank || group == kWavesGroup) && listed;
    const uint32_t index = group == kWaveSoundFilesGroup && listed ? *entry->child : entry->number;
    const bool sound = ((group == kSoundsGroup && place.child < 0) || (group == kWaveSoundFilesGroup && listed)) &&
                       index < played_.Sar()->Sounds().size();
    const bool wave_sound = sound && played_.Sar()->Sounds()[index].type == ntdlib::sar::SoundType::kWaveSound;
    const bool stream_sound =
        sound && archive_version_ && played_.Sar()->Sounds()[index].type == ntdlib::sar::SoundType::kStream;
    if (!child && !wave_sound && !stream_sound)
    {
        return;
    }

    // The bank's note, the wave or the sound's files are read before they play. A wave sound and a stream sound play on
    // the console's model. The model plays them as the game's sound library does. The file keeps the stream. An error
    // shows in a message box.
    const auto make = [&]() -> std::unique_ptr<audio::Renderer>
    {
        // A sound plays in its mixing mode, and a bank's note in the file's.
        const format::File* document = Document();
        const format::Mixing file_mixing = FileMixing();
        const song::Mixing sound_mixing =
            edit::ModelMixing(document != nullptr && sound ? format::MixingOf(*document, index) : file_mixing);
        if (wave_sound)
        {
            return std::make_unique<song::SoundPreview>(played_.Sar(), index, std::vector<uint8_t>(), sound_mixing);
        }
        if (stream_sound)
        {
            // ntdlib's players take an AAC stream as a stream file in 16-bit PCM, with the loop of the sound's entry.
            const std::vector<uint8_t>& stream = StreamBytes(*archive_version_, *played_.Sar(), index);
            return std::make_unique<song::SoundPreview>(
                played_.Sar(), index,
                song::IsAdts(stream)
                    ? song::PlayableStream(stream, song::AacStreamOf(*played_.Sar(), index).value_or(song::AacStream()))
                    : stream,
                sound_mixing);
        }
        if (bank)
        {
            return std::make_unique<song::NotePreview>(played_.Sar(), index, *entry->child,
                                                       edit::ModelMixing(file_mixing));
        }

        return std::make_unique<song::WavePreview>(*played_.Sar(), index, *entry->child);
    };
    const char* what = wave_sound     ? "Can't play this wave sound. "
                       : stream_sound ? "Can't play this stream sound. "
                       : bank         ? "Can't play this instrument. "
                                      : "Can't play this wave. ";
    PlayBrowserPreview(place, what, make);
}

void App::PlayGbaPreview(const BrowserPlace& place)
{
    // On the GBA, a voice of a voice group plays a note as the first song that plays the voice group plays it, in the
    // file's mixing mode.
    const std::optional<ListedPlace> entry = ListedAt(place);
    if (place.group != static_cast<int>(kBanksGroup) || !entry || !entry->child)
    {
        return;
    }

    const uint32_t voices = entry->number;
    std::optional<int> first;
    for (const int song : song::GbaSongs(*played_.Gba()))
    {
        ntdlib::mp2k::SongHeader header;
        if (ntdlib::mp2k::ReadSong(played_.Gba()->rom, played_.Gba()->info, song, header) && header.voices == voices)
        {
            first = song;
            break;
        }
    }
    if (!first)
    {
        return;
    }

    const auto make = [&]() -> std::unique_ptr<audio::Renderer>
    {
        return std::make_unique<song::GbaNotePreview>(played_.Gba(), *first, static_cast<int>(*entry->child),
                                                      edit::ModelMixing(FileMixing()));
    };
    PlayBrowserPreview(place, "Can't play this voice. ", make);
}

void App::PlayDriverPreview(const BrowserPlace& place)
{
    // A sample plays a note on the console's model, in the file's mixing mode, at the sample's rate.
    const std::optional<ListedPlace> entry = ListedAt(place);
    if (!entry || (entry->group != kBanksGroup && entry->group != kWavesGroup) || !entry->child)
    {
        return;
    }

    const auto make = [&]() -> std::unique_ptr<audio::Renderer>
    {
        const format::Mixing mixing = FileMixing();
        return std::make_unique<song::DriverNotePreview>(
            played_.Driver(), played_.Driver()->SampleAudition(static_cast<int>(*entry->child), 60, 127), nullptr,
            edit::ModelMixing(mixing));
    };
    PlayBrowserPreview(place, "Can't play this sample. ", make);
}

void App::PlayDsePreview(const BrowserPlace& place)
{
    // On DSE, a bank's program plays a note on the console's model, in the file's mixing mode. A wave plays as it
    // decodes, at its rate.
    const std::optional<ListedPlace> entry = ListedAt(place);
    const bool bank = entry && entry->group == kBanksGroup && entry->child;
    const bool wave = entry && entry->group == kWavesGroup && entry->child;
    if (!bank && !wave)
    {
        return;
    }

    const uint32_t index = entry->number;
    const uint32_t child = *entry->child;
    const auto make = [&]() -> std::unique_ptr<audio::Renderer>
    {
        if (bank)
        {
            const format::Mixing mixing = FileMixing();
            return std::make_unique<song::DseNotePreview>(played_.Dse()->banks.at(index), static_cast<int>(child),
                                                          played_.Dse()->build, edit::ModelMixing(mixing));
        }

        return std::make_unique<song::WavePreview>(song::DseWavePcm(*played_.Dse(), index, child));
    };
    PlayBrowserPreview(place, bank ? "Can't play this instrument. " : "Can't play this wave. ", make);
}

void App::PlayDsPreview(const BrowserPlace& place)
{
    // On the DS, a bank's instrument plays a note on the console's model, in the file's mixing mode. A wave and a
    // stream play as they decode, at the rate of the channel's timer.
    const std::optional<ListedPlace> entry = ListedAt(place);
    const bool bank = entry && entry->group == kBanksGroup && entry->child;
    const bool wave = entry && entry->group == kWavesGroup && entry->child;
    const bool stream = entry && entry->group == kSoundsGroup && place.child < 0;
    if (!bank && !wave && !stream)
    {
        return;
    }

    const uint32_t index = entry->number;
    const auto make = [&]() -> std::unique_ptr<audio::Renderer>
    {
        if (bank)
        {
            const format::Mixing mixing = FileMixing();
            return std::make_unique<song::DsNotePreview>(*played_.Ds(), index, static_cast<int>(*entry->child),
                                                         edit::ModelMixing(mixing));
        }
        if (wave)
        {
            return std::make_unique<song::WavePreview>(song::DsWavePcm(*played_.Ds(), index, *entry->child));
        }

        return std::make_unique<song::WavePreview>(song::DsStreamPcm(*played_.Ds(), index));
    };
    const char* what = bank   ? "Can't play this instrument. "
                       : wave ? "Can't play this wave. "
                              : "Can't play this stream. ";
    PlayBrowserPreview(place, what, make);
}

void App::PlayWiiPreview(const BrowserPlace& place)
{
    // On the Wii, a bank's instruments, a wave archive's waves, the wave sounds and the stream sounds play by
    // themselves. A wave sound file's wave sounds play too. A note, a wave sound and a stream sound play by the rules
    // of the file's build of NW4R SND. The file keeps a stream sound's stream.
    const std::optional<ListedPlace> entry = ListedAt(place);
    if (!entry)
    {
        return;
    }

    const std::size_t group = entry->group;
    const bool listed = entry->child.has_value();
    const bool bank = group == kBanksGroup && listed;
    const bool wave = group == kWavesGroup && listed;
    const uint32_t index = group == kWaveSoundFilesGroup && listed ? *entry->child : entry->number;
    const bool sound = ((group == kSoundsGroup && place.child < 0) || (group == kWaveSoundFilesGroup && listed)) &&
                       index < played_.Wii()->Sounds().size();
    const bool wave_sound = sound && played_.Wii()->Sounds()[index].type == ntdlib::rsar::SoundType::kWaveSound;
    const bool stream_sound =
        sound && archive_version_ && played_.Wii()->Sounds()[index].type == ntdlib::rsar::SoundType::kStream;
    if (!bank && !wave && !wave_sound && !stream_sound)
    {
        return;
    }

    const auto make = [&]() -> std::unique_ptr<audio::Renderer>
    {
        const format::File* document = Document();
        std::optional<ntdlib::SndBuild> build;
        if (document != nullptr && document->native)
        {
            build = format::BuildOfWiiArchive(*document->native);
        }
        if (wave_sound)
        {
            return std::make_unique<song::SoundPreview>(played_.Wii(), index, build);
        }
        if (stream_sound)
        {
            return std::make_unique<song::SoundPreview>(played_.Wii(), index,
                                                        StreamBytes(*archive_version_, *played_.Wii(), index), build);
        }
        if (bank)
        {
            return std::make_unique<song::NotePreview>(played_.Wii(), index, *entry->child, build);
        }

        return std::make_unique<song::WavePreview>(song::DecodeItemWave(*played_.Wii(), index, *entry->child));
    };
    const char* what = wave_sound     ? "Can't play this wave sound. "
                       : stream_sound ? "Can't play this stream sound. "
                       : bank         ? "Can't play this instrument. "
                                      : "Can't play this wave. ";
    PlayBrowserPreview(place, what, make);
}

std::optional<ListedPlace> App::ListedAt(const BrowserPlace& place) const
{
    const auto group = static_cast<std::size_t>(place.group);
    if (place.group < 0 || place.item < 0 || group >= browser_.size() ||
        static_cast<std::size_t>(place.item) >= browser_[group].items.size())
    {
        return std::nullopt;
    }

    const auto item = static_cast<std::size_t>(place.item);
    const std::vector<uint32_t>& children = browser_[group].items[item].child_numbers;
    ListedPlace listed{group, item, browser_[group].items[item].number, std::nullopt};
    if (place.child >= 0 && static_cast<std::size_t>(place.child) < children.size())
    {
        listed.child = children[static_cast<std::size_t>(place.child)];
    }

    return listed;
}

format::Mixing App::FileMixing() const
{
    const format::File* document = Document();
    return document != nullptr ? document->mixing : format::Mixing::kStrict;
}

void App::PlayBrowserPreview(const BrowserPlace& place, const char* what,
                             const std::function<std::unique_ptr<audio::Renderer>()>& make)
{
    std::unique_ptr<audio::Renderer> preview;
    try
    {
        preview = make();
    }
    catch (const std::exception& error)
    {
        ShowMessage(what + format::AsSentence(error.what()));
        return;
    }

    PlayAlone(std::move(preview));
    browser_state_.playing = place;
}

} // namespace hokuto::app
