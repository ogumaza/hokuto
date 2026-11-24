// SPDX-License-Identifier: MIT

// What the exports of a file's sounds share: the tags of a sound's MIDI file, and the batch exports of the Export
// Audio Mixdown window, hok mixdown and hok midi. A batch reads each sound's sequence, names its files by a naming
// scheme, and renders them through the playback engine or converts them to MIDI.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sound_build.h"

#include "format/file.h"
#include "song/batch.h"
#include "song/midi_export.h"
#include "song/mixdown_files.h"
#include "song/played.h"
#include "song/sequence.h"
#include "song/sequence_model.h"
#include "song/sequence_reader.h"

namespace hokuto::edit
{

// The tags of the MIDI file of sound `sound` of `file`: the sound's title, and the sound's copyright or else the
// file's. The file's title names the whole file. Without a title of the sound, the MIDI file keeps the sound's name.
song::MidiTags MidiTagsOf(const format::File& file, uint32_t sound);

// What a batch exports from: what plays the sounds, the Wii's build, the Hokuto file when there's one, the variables'
// starting values and changes of each sound, by its number, and how the game starts each sound (format::File::starts).
// A sound without starting values or changes takes those of its start. The file sets the effects on the aux buses, each
// sound's mixing mode and engine, and the MIDI files' tags. `name` names the file in messages.
struct BatchSource
{
    song::Played played;
    std::optional<ntdlib::SndBuild> build;
    std::shared_ptr<const format::File> file;
    std::map<uint32_t, song::StartValues> start_values;
    std::map<uint32_t, song::Changes> changes;
    std::vector<format::SoundStart> starts;
    std::string name;
};

// A sound's sequence, or why it can't be read. A sound that plays on another console's engine also has its conversion
// (song::SequenceReader::EngineSound) and the conversion's sequence. A conversion that fails leaves the sound on the
// file's engine. Playback does the same.
struct BatchSequence
{
    std::shared_ptr<const song::Sequence> sequence;
    std::string error;
    std::optional<song::SequenceReader::EngineSound> engine;
    std::shared_ptr<const song::Sequence> engine_sequence;
};

// Sequence sound `sound` of `file` converted for the console of `engine` the way playback converts it (ForEngine).
// `name` names the file in messages. With `fold`, the conversion's timelines fold first. A saved file contains them
// folded, and the MIDI export converts that file. Throws what ForEngine and the archives' readers throw.
song::SequenceReader::EngineSound EngineSoundOf(const format::File& file, uint32_t sound, format::Engine engine,
                                                const std::string& name, bool fold = false);

// The engine on which sound `sound` of `file` plays when it isn't the file's engine, or nothing. An engine that
// EngineRefused refuses plays nothing, and the sound stays on the file's engine.
std::optional<format::Engine> OtherEngineOf(const format::File& file, uint32_t sound);

// The sequences of `sounds`, read as the program reads them (song::ReadBatchSequence), on up to `threads` threads, with
// each sound's conversion for the engine that the file sets. Once `stop` is set, the sounds left stay unread.
std::vector<BatchSequence> ReadBatchSequences(const BatchSource& source, std::span<const song::BatchSound> sounds,
                                              std::size_t threads, const std::atomic<bool>* stop = nullptr);

// How the program plays sound `sound` of `source`: with the file's effects and in the sound's mixing mode, or strictly
// without effects for an archive. `mixing` sets the mode of every sound.
song::ModelOptions BatchModelOptions(const BatchSource& source, uint32_t sound, std::optional<song::Mixing> mixing);

// The WAV files of each of `sounds`, named by `scheme`: the whole mix with `mix`, and the stem of each track that plays
// notes with `stems` (song::StemTracks). A track that `muted` mutes for the sound has no stem. A stem's Track token is
// the track's name. `project` fills the Project token. The names are unique in the batch and against `taken`
// (song::BatchFileNames). A sound whose sequence is missing has no files.
std::vector<std::vector<song::MixdownFile>> MixdownFileNames(std::span<const song::BatchSound> sounds,
                                                             std::span<const BatchSequence> sequences,
                                                             const song::NamingScheme& scheme,
                                                             const std::string& project, bool mix, bool stems,
                                                             const std::function<bool(const std::string&)>& taken,
                                                             const std::map<uint32_t, uint32_t>& muted = {});

// The MIDI files of `sounds`, named by `scheme`, unique in the batch and against `taken`.
std::vector<std::string> MidiFileNames(std::span<const song::BatchSound> sounds, const song::NamingScheme& scheme,
                                       const std::string& project,
                                       const std::function<bool(const std::string&)>& taken);

// Renders sound `sound` of `source`, read into `read`, into `files` in `folder` with `settings`. The render plays what
// the program plays: the conversion on another console's engine, or else the sequence. `mixing` sets the mode.
// `written` is called after each file is written. Throws std::runtime_error with a message for the user when a file
// can't be written.
song::SoundFiles WriteMixdownSound(const BatchSource& source, uint32_t sound, const BatchSequence& read,
                                   std::span<const song::MixdownFile> files, const song::MixdownSettings& settings,
                                   std::optional<song::Mixing> mixing, const std::filesystem::path& folder,
                                   const song::MixdownProgress& progress = {},
                                   const std::function<void()>& written = {});

// Converts sound `sound` of `source` to MIDI with its instruments on the file's console, and writes the file `midi`
// with a SoundFont 2 file beside it, or with `sfz`, SFZ files (song::WriteMidiAndSoundFont, WriteMidiAndSfz). A file's
// sound keeps its tags. Throws what the conversion and the writers throw, song::EmptyConversion included.
void WriteBatchMidi(const BatchSource& source, uint32_t sound, const std::filesystem::path& midi, bool sfz);

// Runs `work` for each index from 0 to `count`, on up to `threads` threads. Each thread runs the next index that's
// left.
void ForEachOnThreads(std::size_t count, std::size_t threads, const std::function<void(std::size_t)>& work);

} // namespace hokuto::edit
