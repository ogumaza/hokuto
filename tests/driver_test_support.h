// SPDX-License-Identifier: MIT

// The helpers that the tests of the drivers share: the tests of the GBA's drivers and those of the DS and DSE. Each
// test includes this header and names the helpers that it uses with using-declarations. The tests of the songs take
// HasSound from it too.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"

#include "cart_bytes.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/driver.h"
#include "song/sequence.h"
#include "song/sequence_model.h"
#include "song/sequence_reader.h"

namespace hokuto::testing
{

// The track of `sequence` with the number `number`, or nullptr for a track that doesn't play.
inline const song::Track* TrackOf(const song::Sequence& sequence, int number)
{
    const auto found = std::ranges::find(sequence.tracks, number, &song::Track::number);
    return found == sequence.tracks.end() ? nullptr : &*found;
}

// The ticks at which a track's notes start. Nothing for a track that doesn't play.
inline std::vector<uint32_t> StartsOf(const song::Sequence& sequence, int number)
{
    std::vector<uint32_t> starts;
    if (const song::Track* track = TrackOf(sequence, number))
    {
        for (const song::Note& note : track->notes)
        {
            starts.push_back(note.start);
        }
    }

    return starts;
}

// Whether any of `samples` isn't silent. A model makes 16-bit samples in strict mixing and floats in enhanced mixing.
template <typename Sample>
bool HasSound(std::span<const Sample> samples)
{
    return std::ranges::any_of(samples, [](Sample sample) { return sample != Sample{}; });
}

// The keys of a track's notes.
inline std::vector<int> KeysOf(const song::Sequence& sequence, int number)
{
    std::vector<int> keys;
    if (const song::Track* track = TrackOf(sequence, number))
    {
        for (const song::Note& note : track->notes)
        {
            keys.push_back(note.key);
        }
    }

    return keys;
}

// The List Editor's place of the first command of `kind` in sequence `sequence` of `file`.
inline edit::CommandPlace PlaceOfKind(const format::File& file, uint32_t sequence, ntdlib::commands::Kind kind)
{
    const ntdlib::commands::Sequence& commands = *file.sequences.at(sequence);
    const std::vector<uint32_t> offsets = edit::Offsets(commands, edit::ConsoleOf(file));
    const auto found = std::ranges::find(commands.items, kind, &ntdlib::commands::Command::kind);
    if (found == commands.items.end())
    {
        throw std::logic_error("The sequence has no command of that kind.");
    }

    return edit::CommandPlace{offsets.at(static_cast<std::size_t>(found - commands.items.begin())), {}};
}

// The result of the last request to `reader`, polled every millisecond for up to 30 seconds. Nothing when no result
// comes by then.
inline std::optional<song::SequenceReader::Result> TakeRead(song::SequenceReader& reader)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::optional<song::SequenceReader::Result> read = reader.Take();
    while (!read && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        read = reader.Take();
    }

    return read;
}

// The output of a model: its 16-bit samples, and its floats in enhanced mixing.
struct Played
{
    std::vector<int16_t> samples;
    std::vector<float> floats;
};

// The output of `model` from frame 0, for up to `frames` frames or until the model ends.
inline Played Play(song::SequenceModel& model, int frames)
{
    Played played;
    int frame = 0;
    for (bool more = model.SeekFrame(0); more && frame < frames; more = model.RunFrame(), frame++)
    {
        played.samples.insert(played.samples.end(), model.Output().begin(), model.Output().end());
        const std::span<const float> floats = model.FloatOutput();
        played.floats.insert(played.floats.end(), floats.begin(), floats.end());
    }

    return played;
}

// Whether the model plays sound `sound` of `rom` for `frames` frames as the reference does, sample for sample, and
// makes a sound. `direct(samples)` returns at least `samples` stereo samples of the reference, in strict mixing. The
// reference runs 2,048 stereo samples past the model's output.
template <class Rom, class Direct>
bool ModelPlaysAsReference(const std::shared_ptr<const Rom>& rom, uint32_t sound, const Direct& direct, int frames = 60)
{
    const auto sequence = std::make_shared<const song::Sequence>(rom->ReadSequence(sound));
    song::SequenceModel model(rom, sequence);
    const Played played = Play(model, frames);
    const std::vector<int16_t> reference = direct(played.samples.size() / 2 + 2048);
    return !played.samples.empty() && HasSound(std::span<const int16_t>(played.samples)) &&
           std::equal(played.samples.begin(), played.samples.end(), reference.begin());
}

// How far the clock of the model of a GBA ROM's driver is from the time that it played (`drift`): the arrangement's
// time at DriverPlayer::Tick() after sound `sound` of `rom` plays `frames` frames, or up to its end (`played`), less
// the time that those frames take, in frames. The playhead and the transport's time follow the clock. A clock at 60
// frames a second in place of the GBA's 59.73, 0.46 % off, drifts 2 frames only after about 440 frames: a sound that
// ends sooner checks only the gross rate. Past the loop's end, the time goes on at the last tempo (song::MsOfTick): a
// sound whose loop changes the tempo can't be checked there.
struct ClockRun
{
    double drift = 0.0;
    int played = 0;
};

template <class Rom>
ClockRun ClockDrift(const std::shared_ptr<const Rom>& rom, uint32_t sound, int frames)
{
    const song::Sequence sequence = rom->ReadSequence(sound);
    const std::unique_ptr<song::DriverPlayer> player = rom->MakePlayer();
    player->Start(sound, false);
    player->SetArrangement(sequence);
    std::vector<int16_t> out;
    int played = 0;
    while (played < frames && !player->Finished())
    {
        player->RunFrame(out);
        played++;
    }

    const double frame_ms = ntdlib::agb::FrameSeconds(1.0) * 1000.0;
    return {.drift = song::MsOfTick(sequence, player->Tick()) / frame_ms - played, .played = played};
}

// The reference of the tests of the GBA's drivers: the samples of `player`, an ntdlib player that has started a song,
// run frame by frame up to `samples` stereo samples or a few more. Sample is int16_t in strict mixing and float in
// enhanced mixing.
template <typename Sample, class Player>
std::vector<Sample> RunDirect(Player& player, std::size_t samples)
{
    std::vector<Sample> out;
    while (out.size() < samples * 2)
    {
        player.RunFrame(out);
    }

    return out;
}

// Whether the sequence loops over the range from `start` to `end`, or over a later pass of it. ntdlib's performer marks
// the second pass where the first pass plays unlike the second. It can also mark a later pass of a short loop.
inline bool LoopsOver(const song::Sequence& sequence, uint32_t start, uint32_t end)
{
    return sequence.loop && sequence.loop->end - sequence.loop->start == end - start && sequence.loop->start >= start &&
           (sequence.loop->start - start) % (end - start) == 0;
}

// Where each sound of `file` plays. The file is the file of a ROM of another of the GBA's drivers. The program finds
// each place the same way (edit::DriverSoundPlaces): the field of the sound's sequence names the sequence.
inline std::vector<std::optional<edit::SoundPlace>> PlacesOf(const format::File& file)
{
    const std::shared_ptr<const song::DriverRom> rom = song::LoadDriverRom(file.engine, format::ExportArchive(file));
    std::vector<std::optional<uint32_t>> sequences(rom->SoundCount());
    for (const uint32_t sound : rom->Sounds())
    {
        sequences[sound] = format::SequenceNamedAt(file, *rom->FieldsOf(sound).sequence);
    }

    return edit::DriverSoundPlaces(file, *rom, sequences);
}

// The sequence that sound `sound` plays, from the places of a file's sounds. Throws std::logic_error for a sound
// without a place.
inline uint32_t SequenceAt(const std::vector<std::optional<edit::SoundPlace>>& places, uint32_t sound)
{
    if (sound >= places.size() || !places[sound])
    {
        throw std::logic_error("Sound " + std::to_string(sound) + " has no place.");
    }

    return places[sound]->sequence;
}

// The sequence of `file` that sound `sound` plays. The file is the file of a ROM of another of the GBA's drivers.
inline uint32_t SequenceOf(const format::File& file, uint32_t sound)
{
    return SequenceAt(PlacesOf(file), sound);
}

} // namespace hokuto::testing
