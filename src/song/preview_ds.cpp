// SPDX-License-Identifier: MIT

// The previews of a note of an instrument of a DS bank: on NITRO-Composer, or on DSE.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "xsflib/binary.h"

#include "song/preview.h"
#include "song/preview_parts.h"

namespace hokuto::song
{
namespace
{

// The length of one of the DS driver's frames: 174,592 of the system's clocks, or 87,296,000 / 16,756,991 ms.
constexpr FrameMs kDsDriverFrameMs{87296000, 16756991};

// A frame of the DSE model lasts two frames of the sound driver. A frame of the DS model does too (kDsFrameMs).
static_assert(kDsFrameMs.numerator == 2 * kDsDriverFrameMs.numerator &&
              kDsFrameMs.denominator == kDsDriverFrameMs.denominator);
static_assert(ntdlib::ntr::snd::kFrameClocks * 1000 * kDsDriverFrameMs.denominator ==
              kDsDriverFrameMs.numerator * uint64_t{ntdlib::ntr::kSystemClockHz});

} // namespace

DsNotePreview::DsNotePreview(const DsArchive& archive, uint32_t bank, int program, Mixing mixing)
    : DsNotePreview(archive, bank, program, KeyOf(archive, bank, program), NotePreview::kVelocity, nullptr, mixing)
{
}

DsNotePreview::DsNotePreview(const DsArchive& archive, uint32_t bank, int program, int key, int velocity,
                             std::shared_ptr<const std::atomic<bool>> release, Mixing mixing)
    : release_(std::move(release)), enhanced_(mixing == Mixing::kEnhanced)
{
    // An instrument that plays at no key plays nothing.
    if (key < 0)
    {
        ended_ = true;
        return;
    }

    try
    {
        // The note plays as the game plays every sequence of its archive, such as through its output effect.
        archive_ = DsAuditionArchive(archive, bank, program, key, velocity);
        ntdlib::ntr::ArchivePlayerOptions options;
        options.build = archive_->build;
        options.enhanced = enhanced_;
        player_ = std::make_unique<ntdlib::ntr::ArchivePlayer>(archive_->archive, options);
        player_->Start(ntdlib::ntr::SequenceRef{0}, DsGameSettingsOf(*archive_));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error(error.what());
    }
}

int DsNotePreview::KeyOf(const DsArchive& archive, uint32_t bank, int program)
{
    const ntdlib::sdat::Archive& source = archive.archive;
    if (bank >= source.banks.size() || !source.banks[bank] || program < 0)
    {
        return -1;
    }

    // The number of waves in the wave archive of each of the bank's slots. A wave archive that can't be read has none.
    const ntdlib::sdat::BankInfo& info = *source.banks[bank];
    ntdlib::sdat::Bank parsed;
    std::array<std::size_t, 4> waves{};
    try
    {
        parsed = ntdlib::sdat::Bank::Parse(source.File(info.file_id));
    }
    catch (const xsflib::FormatError&)
    {
        return -1;
    }

    for (std::size_t slot = 0; slot < waves.size(); slot++)
    {
        const uint16_t index = info.wave_archives[slot];
        if (index >= source.wave_archives.size() || !source.wave_archives[index])
        {
            continue;
        }

        try
        {
            waves[slot] = ntdlib::sdat::WaveArchive::Parse(
                              source.File(source.wave_archives[index]->file_id & ntdlib::sdat::kFileIdBits))
                              .waves.size();
        }
        catch (const xsflib::FormatError&)
        {
        }
    }

    if (static_cast<std::size_t>(program) >= parsed.instruments.size())
    {
        return -1;
    }

    const ntdlib::sdat::Instrument& instrument = parsed.instruments[static_cast<std::size_t>(program)];
    const auto plays = [&instrument, &waves](int key)
    {
        using ntdlib::sdat::InstrumentType;
        const ntdlib::sdat::Region* region = DsRegionAt(instrument, key);
        if (region == nullptr)
        {
            return false;
        }

        switch (region->type)
        {
        case InstrumentType::kPcm:
            return region->wave_archive < waves.size() && region->wave < waves[region->wave_archive];
        case InstrumentType::kPsg:
        case InstrumentType::kNoise:
            return true;
        default:
            return false;
        }
    };

    return preview_parts::NearestKey(plays);
}

uint64_t DsNotePreview::HoldFrames()
{
    return FramesIn(NotePreview::kHoldMs, kDsDriverFrameMs);
}

uint64_t DsNotePreview::LongestFrames()
{
    return FramesIn(NotePreview::kLongestMs, kDsDriverFrameMs);
}

int DsNotePreview::SampleRate() const
{
    return enhanced_ ? static_cast<int>(std::lround(ntdlib::ntr::kEnhancedRate))
                     : static_cast<int>(ntdlib::ntr::kOutputRate);
}

std::size_t DsNotePreview::Render(float* out, std::size_t frames)
{
    return preview_parts::RenderFrames(out, frames, frame_, frame_read_, [this] { return NextFrame(); });
}

bool DsNotePreview::NextFrame()
{
    // After the hold, or once the note is let go, the variable ends the track at its next tick, and the driver releases
    // the note. The preview ends once nothing sounds. Damaged data in the archive makes the model fail, and ends it
    // too.
    if (ended_ || !player_->Busy())
    {
        ended_ = true;
        return false;
    }

    const bool let_go = release_ ? release_->load(std::memory_order_acquire) : frames_ >= HoldFrames();
    if (let_go && !released_)
    {
        ntdlib::ntr::Variables variables;
        variables.local[kDsAuditionRelease] = 1;
        player_->Send(ntdlib::ntr::VariableCommands(variables));
        released_ = true;
    }

    try
    {
        if (enhanced_)
        {
            frame_.clear();
            player_->RunFrame(frame_);
        }
        else
        {
            output_.clear();
            samples_.clear();
            player_->RunFrame(output_);
            ntdlib::ntr::AppendPcm16(output_, samples_);
            preview_parts::SetFrame(frame_, samples_);
        }
    }
    catch (const std::exception&)
    {
        ended_ = true;
        return false;
    }

    // The last frame of a preview that runs to the limit fades out.
    ++frames_;
    ended_ = frames_ >= LongestFrames();
    if (ended_)
    {
        preview_parts::FadeOut(frame_);
    }

    frame_read_ = 0;

    return true;
}

DseNotePreview::DseNotePreview(const DseBank& bank, int program, ntdlib::dse::Build build, Mixing mixing)
    : DseNotePreview(bank, program, KeyOf(bank, program), NotePreview::kVelocity, nullptr, build, mixing)
{
}

DseNotePreview::DseNotePreview(const DseBank& bank, int program, int key, int velocity,
                               std::shared_ptr<const std::atomic<bool>> release, ntdlib::dse::Build build,
                               Mixing mixing)
    : bank_(bank), release_(std::move(release)), enhanced_(mixing == Mixing::kEnhanced)
{
    // A program that plays at no key plays nothing.
    if (key < 0 || !bank_.bank)
    {
        ended_ = true;
        return;
    }

    song_ = NoteSong(*bank_.bank, program, key, velocity);
    player_ = std::make_unique<ntdlib::dse::Player>(ntdlib::dse::PlayerOptions{.build = build, .enhanced = enhanced_});
    const ntdlib::dse::SequenceFiles files{
        .song = &song_, .banks = {bank_.bank.get()}, .main_bank = bank_.main_bank.get()};
    player_->LoadBanks(files);
    player_->Start(files);
}

int DseNotePreview::KeyOf(const DseBank& bank, int program)
{
    if (!bank.bank || program < 0 || program > 0x7FFF)
    {
        return -1;
    }

    const std::vector<ntdlib::sequence_export::dse::PresetZone> zones = ntdlib::sequence_export::dse::PresetZones(
        *bank.bank, {.bank = DseBankId(*bank.bank), .program = static_cast<uint16_t>(program)});
    const auto plays = [&zones](int key)
    {
        const auto holds_key = [key](const ntdlib::sequence_export::dse::PresetZone& zone)
        {
            return zone.low_key <= key && key <= zone.high_key && zone.low_velocity <= NotePreview::kVelocity &&
                   NotePreview::kVelocity <= zone.high_velocity;
        };

        return std::ranges::any_of(zones, holds_key);
    };

    return preview_parts::NearestKey(plays);
}

ntdlib::dse::Song DseNotePreview::NoteSong(const ntdlib::dse::WaveBank& bank, int program, int key, int velocity)
{
    // The events: the fastest tempo, then the bank, the program, the full volume and the octave. The fastest tempo
    // starts the note in the driver's first ticks. The second byte of a note contains the count of the bytes of its
    // length, the change of the octave (2 for none) and the key in the octave. The longest pause keeps the track, and
    // the song, going.
    constexpr uint8_t kTempo = 0xA4;
    constexpr uint8_t kBankHigh = 0xA9;
    constexpr uint8_t kBankLow = 0xAA;
    constexpr uint8_t kProgram = 0xAC;
    constexpr uint8_t kVolume = 0xE0;
    constexpr uint8_t kOctave = 0xA0;
    constexpr uint8_t kLongPause = 0x94;
    constexpr uint8_t kLongestLength = 0xFF;
    constexpr uint8_t kThreeBytesSameOctave = 3 << 6 | 2 << 4;
    constexpr uint8_t kMostValue = 0x7F;
    constexpr uint8_t kFastestTempo = 0xFF;
    constexpr int kKeysInOctave = 12;

    // The settings' bytes that the driver reads, with the values of the games' songs. Entry 2 of the driver's global
    // volumes contains the full level, and entry 0 silence.
    constexpr std::size_t kTicksPerQuarter = 2;
    constexpr std::size_t kTracks = 6;
    constexpr std::size_t kChannels = 7;
    constexpr std::size_t kGlobalVolume = 0x19;
    constexpr std::size_t kPriority = 0x1B;

    ntdlib::dse::Song song;
    std::memcpy(song.header.data(), "smdl", 4);
    song.header[0x0C] = 0x15;
    song.header[0x0D] = 0x04;
    song.header[0x0E] = bank.header[0x0E];
    song.header[0x0F] = bank.header[0x0F];
    song.info.bytes[kTicksPerQuarter] = 48;
    song.info.bytes[kTracks] = 1;
    song.info.bytes[kChannels] = 1;
    song.info.bytes[kGlobalVolume] = 2;
    song.info.bytes[kPriority] = 8;
    ntdlib::dse::Track track;
    track.events = {
        {.code = kTempo, .arguments = {kFastestTempo}},
        {.code = kBankHigh, .arguments = {bank.header[0x0F]}},
        {.code = kBankLow, .arguments = {bank.header[0x0E]}},
        {.code = kProgram, .arguments = {static_cast<uint8_t>(program)}},
        {.code = kVolume, .arguments = {kMostValue}},
        {.code = kOctave, .arguments = {static_cast<uint8_t>(key / kKeysInOctave)}},
        {.code = static_cast<uint8_t>(std::clamp(velocity, 1, static_cast<int>(kMostValue))),
         .arguments = {static_cast<uint8_t>(kThreeBytesSameOctave | key % kKeysInOctave), kLongestLength,
                       kLongestLength, kLongestLength}},
        {.code = kLongPause, .arguments = {kLongestLength, kLongestLength, kLongestLength}},
        {.code = ntdlib::dse::kEndTrack},
    };
    song.tracks.push_back(std::move(track));

    return song;
}

uint64_t DseNotePreview::HoldFrames()
{
    return FramesIn(NotePreview::kHoldMs, kDsFrameMs);
}

uint64_t DseNotePreview::LongestFrames()
{
    return FramesIn(NotePreview::kLongestMs, kDsFrameMs);
}

int DseNotePreview::SampleRate() const
{
    return enhanced_ ? static_cast<int>(std::lround(ntdlib::ntr::kEnhancedRate))
                     : static_cast<int>(ntdlib::ntr::kOutputRate);
}

std::size_t DseNotePreview::Render(float* out, std::size_t frames)
{
    return preview_parts::RenderFrames(out, frames, frame_, frame_read_, [this] { return NextFrame(); });
}

bool DseNotePreview::NextFrame()
{
    // A note of the Key Editor can be released before the driver has started it. The host therefore keeps releasing the
    // notes until the preview ends. The preview ends once a voice has sounded and nothing sounds, or when nothing has
    // started by the time that the note is released. Damaged data makes the model fail, and ends the preview too.
    if (ended_)
    {
        return false;
    }

    const bool let_go = release_ ? release_->load(std::memory_order_acquire) : frames_ >= HoldFrames();
    if (let_go)
    {
        player_->Driver().ReleaseFollowedNotes();
    }

    constexpr uint64_t kFrameClocks = 2 * ntdlib::ntr::snd::kFrameClocks;
    try
    {
        if (enhanced_)
        {
            frame_.clear();
            player_->Run(kFrameClocks, frame_);
        }
        else
        {
            output_.clear();
            samples_.clear();
            player_->Run(kFrameClocks, output_);
            ntdlib::ntr::AppendPcm16(output_, samples_);
            preview_parts::SetFrame(frame_, samples_);
        }
    }
    catch (const std::exception&)
    {
        ended_ = true;
        return false;
    }

    frames_++;
    const bool sounding = player_->BusyChannels() != 0;
    constexpr uint64_t kFramesToStart = 8;
    const bool done = started_ && !sounding;
    started_ = started_ || sounding;
    if (done || (!started_ && frames_ >= kFramesToStart && let_go))
    {
        ended_ = true;
        frame_read_ = 0;
        return true;
    }

    // The last frame of a preview that runs to the limit fades out.
    ended_ = frames_ >= LongestFrames();
    if (ended_)
    {
        preview_parts::FadeOut(frame_);
    }

    frame_read_ = 0;

    return true;
}

} // namespace hokuto::song
