// SPDX-License-Identifier: MIT

// The previews of a note of a GBA ROM's voice or instrument: on MP2K's player, or on the player of another driver.

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/mp2k/soundfont.h"

#include "song/preview.h"
#include "song/preview_parts.h"

namespace hokuto::song
{
namespace
{

// Whether the voice at `address` of `rom` plays something at `key`: a PSG channel, a synth of Camelot's mixer, a sample
// of 4-bit steps, or a sample that ntdlib reads, with points. A key split or a drum kit plays the voice of the key, if
// it has one.
bool GbaVoicePlays(const GbaRom& rom, uint32_t address, int key)
{
    ntdlib::mp2k::Voice voice;
    if (!ntdlib::mp2k::ReadVoice(rom.rom, address, voice))
    {
        return false;
    }

    if (voice.IsSplit())
    {
        const uint32_t played = ntdlib::mp2k::SplitVoice(rom.rom, voice, key);
        if (played == 0 || !ntdlib::mp2k::ReadVoice(rom.rom, played, voice) || voice.IsSplit())
        {
            return false;
        }
    }

    ntdlib::mp2k::Wave wave;
    return voice.PsgChannel() != 0 ||
           ntdlib::sequence_export::mp2k::SynthOf(rom.rom, rom.info, voice) !=
               ntdlib::sequence_export::mp2k::Synth::kNone ||
           GbaVoicePlaysSteps(rom, voice) || (ntdlib::mp2k::ReadWave(rom.rom, voice.wave, wave) && wave.size > 0);
}

} // namespace

GbaNotePreview::GbaNotePreview(const std::shared_ptr<const GbaRom>& rom, int song, int program, Mixing mixing)
    : GbaNotePreview(rom, song, program, -1, NotePreview::kVelocity, nullptr, mixing)
{
}

GbaNotePreview::GbaNotePreview(const std::shared_ptr<const GbaRom>& rom, int song, int program, int key, int velocity,
                               std::shared_ptr<const std::atomic<bool>> release, Mixing mixing)
    : release_(std::move(release))
{
    // A key of -1 asks for the browser's key. A voice that plays at no key plays nothing.
    ntdlib::mp2k::SongHeader header;
    if (key < 0 && ntdlib::mp2k::ReadSong(rom->rom, rom->info, song, header))
    {
        key = KeyOf(*rom, header.voices, program);
    }
    if (key < 0)
    {
        ended_ = true;
        return;
    }

    rom_ = GbaAuditionRom(*rom, song, program, key, velocity);
    player_ = std::make_unique<ntdlib::mp2k::ArchivePlayer>(rom_->rom, rom_->info);
    ntdlib::mp2k::ArchivePlayerOptions options;
    options.enhanced = mixing == Mixing::kEnhanced;
    try
    {
        player_->Start(0, options);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error(error.what());
    }
}

int GbaNotePreview::KeyOf(const GbaRom& rom, uint32_t voices, int program)
{
    const uint32_t address = voices + ntdlib::mp2k::kVoiceSize * static_cast<uint32_t>(program);
    const auto plays = [&](int key)
    {
        return GbaVoicePlays(rom, address, key);
    };
    return preview_parts::NearestKey(plays);
}

uint64_t GbaNotePreview::HoldFrames()
{
    return FramesIn(NotePreview::kHoldMs, kGbaFrameMs);
}

uint64_t GbaNotePreview::LongestFrames()
{
    return FramesIn(NotePreview::kLongestMs, kGbaFrameMs);
}

int GbaNotePreview::SampleRate() const
{
    return player_ ? player_->Rate() : ntdlib::agb::kBaseOutputRate;
}

std::size_t GbaNotePreview::Render(float* out, std::size_t frames)
{
    return preview_parts::RenderFrames(out, frames, frame_, frame_read_, [this] { return NextFrame(); });
}

bool GbaNotePreview::NextFrame()
{
    // After the hold, or once the note is let go, the track releases the note at its next tick. The preview ends once
    // nothing sounds. Damaged data in the ROM makes the model fail, and ends it too.
    if (ended_ || !player_->Busy())
    {
        ended_ = true;
        return false;
    }

    const bool let_go = release_ ? release_->load(std::memory_order_acquire) : player_->Frames() >= HoldFrames();
    if (let_go && !released_)
    {
        player_->WriteMemory(kAuditionRelease, 1);
        released_ = true;
    }

    try
    {
        if (player_->Enhanced())
        {
            enhanced_samples_.clear();
            player_->RunFrame(enhanced_samples_);
            frame_ = enhanced_samples_;
        }
        else
        {
            samples_.clear();
            player_->RunFrame(samples_);
            preview_parts::SetFrame(frame_, samples_);
        }
    }
    catch (const std::exception&)
    {
        ended_ = true;
        return false;
    }

    // The last frame of a preview that runs to the limit fades out.
    ended_ = player_->Frames() >= LongestFrames();
    if (ended_)
    {
        preview_parts::FadeOut(frame_);
    }

    frame_read_ = 0;

    return true;
}

DriverNotePreview::DriverNotePreview(const std::shared_ptr<const DriverRom>& rom, const DriverAudition& audition,
                                     std::shared_ptr<const std::atomic<bool>> release, Mixing mixing)
    : release_(std::move(release))
{
    try
    {
        rom_ = rom->AuditionRom(audition);
        player_ = rom_->MakePlayer();
        player_->Start(0, mixing == Mixing::kEnhanced);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error(error.what());
    }
}

int DriverNotePreview::SampleRate() const
{
    return player_ ? player_->Rate() : ntdlib::agb::kBaseOutputRate;
}

std::size_t DriverNotePreview::Render(float* out, std::size_t frames)
{
    return preview_parts::RenderFrames(out, frames, frame_, frame_read_, [this] { return NextFrame(); });
}

bool DriverNotePreview::NextFrame()
{
    if (ended_ || !player_ || !player_->Busy())
    {
        ended_ = true;
        return false;
    }

    try
    {
        if (player_->Enhanced())
        {
            enhanced_samples_.clear();
            player_->RunFrame(enhanced_samples_);
            frame_ = enhanced_samples_;
        }
        else
        {
            samples_.clear();
            player_->RunFrame(samples_);
            preview_parts::SetFrame(frame_, samples_);
        }
    }
    catch (const std::exception&)
    {
        ended_ = true;
        return false;
    }

    // The frame that lets the note go, or that reaches the limit, fades out.
    const bool let_go =
        release_ ? release_->load(std::memory_order_acquire) : player_->Frames() >= GbaNotePreview::HoldFrames();
    ended_ = let_go || player_->Frames() >= GbaNotePreview::LongestFrames();
    if (ended_)
    {
        preview_parts::FadeOut(frame_);
    }

    frame_read_ = 0;

    return true;
}

} // namespace hokuto::song
