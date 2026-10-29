// SPDX-License-Identifier: MIT

#include "synth/sfz_synth.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "seqlib/file_name.h"
#include "seqlib/midi/reader.h"
#include "xsflib/files.h"
#include <sfizz.hpp>

namespace hokuto::synth
{
namespace
{

namespace export_ = ntdlib::sequence_export;

// The most samples that sfizz renders at once. A block also ends at each event. Every event then plays at its exact
// sample: sfizz applies some controllers from the start of a block.
constexpr std::size_t kBlock = 1024;

// sfizz's voices for each track.
constexpr int kVoices = 256;

// sfizz's resampling: 2 is Hermite interpolation, its default.
constexpr int kSampleQuality = 2;

// The microseconds of a quarter note until a MIDI file's first tempo change.
constexpr uint32_t kDefaultMicros = 500000;

// The folder of the SFZ files in the temporary folder. The folder of samples lies beside it. Each file's <control>
// header points there.
constexpr char kFilesFolder[] = "sequence";

// The seconds at which each tick of a MIDI file plays.
class TickTimes
{
public:
    explicit TickTimes(const seqlib::midi::File& file);

    double Seconds(uint64_t tick) const;

private:
    // A stretch of ticks at one tempo: its first tick, the seconds there and the seconds of each tick.
    struct Stretch
    {
        uint64_t tick = 0;
        double seconds = 0.0;
        double tick_seconds = 0.0;
    };

    std::vector<Stretch> stretches_;
};

TickTimes::TickTimes(const seqlib::midi::File& file)
{
    // A division with its top bit set gives frames a second and ticks a frame. The tempo then changes nothing.
    if ((file.division & 0x8000) != 0)
    {
        const int frames = -static_cast<int8_t>(file.division >> 8);
        const double rate = frames == 29 ? 30000.0 / 1001.0 : static_cast<double>(frames);
        const int ticks = file.division & 0xFF;
        if (frames <= 0 || ticks == 0)
        {
            throw std::runtime_error("The MIDI file's timing in frames is damaged.");
        }

        stretches_.push_back({0, 0.0, 1.0 / (rate * ticks)});
        return;
    }
    if (file.division == 0)
    {
        throw std::runtime_error("The MIDI file gives no ticks to a quarter note.");
    }

    // The tempo changes of every track, in order of tick. Changes at one tick keep the order of their tracks, and the
    // last one holds.
    std::map<uint64_t, uint32_t> changes;
    for (const seqlib::midi::Track& track : file.tracks)
    {
        for (const seqlib::midi::Event& event : track.events)
        {
            const std::vector<uint8_t>& bytes = event.bytes;
            if (bytes.size() == 6 && bytes[0] == 0xFF && bytes[1] == 0x51 && bytes[2] == 3)
            {
                changes[event.tick] = (uint32_t{bytes[3]} << 16) | (uint32_t{bytes[4]} << 8) | bytes[5];
            }
        }
    }

    const double division = file.division;
    stretches_.push_back({0, 0.0, kDefaultMicros / 1e6 / division});
    for (const auto& [tick, micros] : changes)
    {
        const Stretch& last = stretches_.back();
        const double seconds = last.seconds + static_cast<double>(tick - last.tick) * last.tick_seconds;
        if (tick == last.tick)
        {
            stretches_.back().tick_seconds = micros / 1e6 / division;
            continue;
        }

        stretches_.push_back({tick, seconds, micros / 1e6 / division});
    }
}

double TickTimes::Seconds(uint64_t tick) const
{
    const auto after = std::upper_bound(stretches_.begin(), stretches_.end(), tick,
                                        [](uint64_t t, const Stretch& stretch) { return t < stretch.tick; });
    const Stretch& stretch = *std::prev(after);
    return stretch.seconds + static_cast<double>(tick - stretch.tick) * stretch.tick_seconds;
}

// Reads `midi`, with a message for the user when it can't.
seqlib::midi::File ReadMidi(std::span<const uint8_t> midi)
{
    try
    {
        return seqlib::midi::Read(midi);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(std::string("Hokuto can't read the MIDI file. ") + error.what());
    }
}

// A channel message of a track, and the sample at which it plays.
struct TimedMessage
{
    std::size_t sample = 0;
    std::vector<uint8_t> bytes;
};

// A track that plays: its name, the file that it plays through, and its channel messages up to the render's end.
struct TrackPlay
{
    std::string name;
    const export_::SfzFile* file = nullptr;
    std::vector<TimedMessage> messages;
};

// The file of the MIDI track named `name`, among `files`. A layer such as "Track 3 (layer 2)" plays through the file of
// its track, Track 3.sfz. A file's name is the track's name, made safe for file systems where it has to be.
const export_::SfzFile& FileOf(const std::string& name, const std::vector<export_::SfzFile>& files)
{
    const std::string track = name.substr(0, name.find(" (layer"));
    for (const std::string& candidate : {track + ".sfz", seqlib::SafeFileName(track) + ".sfz"})
    {
        const auto file =
            std::find_if(files.begin(), files.end(), [&](const export_::SfzFile& f) { return f.name == candidate; });
        if (file != files.end())
        {
            return *file;
        }
    }

    throw std::runtime_error("The SFZ files have no file for MIDI track \"" + name + "\".");
}

// The tracks of `file` that play, with their messages before sample `frames` at `rate`. A track plays when a note
// starts before that sample. The others make no sound, and require no file. The converter's first track is one of them:
// it marks the loop's start with CC111.
std::vector<TrackPlay> TracksThatPlay(const seqlib::midi::File& file, const std::vector<export_::SfzFile>& files,
                                      int rate, std::size_t frames)
{
    const TickTimes times(file);
    const std::vector<std::string> names = seqlib::midi::TrackNames(file);
    std::vector<TrackPlay> tracks;
    for (std::size_t t = 0; t < file.tracks.size(); t++)
    {
        TrackPlay play;
        bool notes = false;
        for (const seqlib::midi::Event& event : file.tracks[t].events)
        {
            if (event.bytes.empty() || event.bytes[0] < 0x80 || event.bytes[0] >= 0xF0)
            {
                continue;
            }

            const double at = std::round(times.Seconds(event.tick) * rate);
            if (at < static_cast<double>(frames))
            {
                play.messages.push_back({static_cast<std::size_t>(at), event.bytes});
                notes = notes || ((event.bytes[0] & 0xF0) == 0x90 && event.bytes.size() > 2 && event.bytes[2] > 0);
            }
        }
        if (!notes)
        {
            continue;
        }

        play.name = names[t].empty() ? "#" + std::to_string(t) : names[t];
        play.file = &FileOf(play.name, files);
        tracks.push_back(std::move(play));
    }

    return tracks;
}

// A new folder in the system's temporary folder. The destructor removes it with everything in it.
class TemporaryFolder
{
public:
    TemporaryFolder();
    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;
    ~TemporaryFolder();

    const std::filesystem::path& Path() const;

private:
    std::filesystem::path path_;
};

TemporaryFolder::TemporaryFolder()
{
    static std::atomic<uint64_t> count{0};
    std::random_device random;
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 16; attempt++)
    {
        const uint64_t tag = ((uint64_t{random()} << 32) ^ random()) + count.fetch_add(1);
        path_ = base / ("hokuto-sfz-" + std::to_string(tag));
        if (std::filesystem::create_directory(path_))
        {
            return;
        }
    }

    throw std::runtime_error("Hokuto can't make a temporary folder for sfizz in " + xsflib::Utf8(base) + ".");
}

TemporaryFolder::~TemporaryFolder()
{
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

const std::filesystem::path& TemporaryFolder::Path() const
{
    return path_;
}

// Writes `bytes` to the file `name` in `folder`. `name` has to be a plain file name.
template <typename Bytes>
void WriteFile(const std::filesystem::path& folder, const std::string& name, const Bytes& bytes)
{
    const std::filesystem::path file = xsflib::PathFromUtf8(name);
    if (file.empty() || file.has_parent_path() || file.is_absolute())
    {
        throw std::runtime_error("The SFZ files name a file \"" + name + "\" outside their folders.");
    }

    std::ofstream out(folder / file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
    {
        throw std::runtime_error("Hokuto can't write " + name + " for sfizz in " + xsflib::Utf8(folder) + ".");
    }
}

// The lock that sfizz's synths are made, loaded and freed under. A synth renders without it. sfizz documents which
// threads may use one synth, but not several synths at once. Its synths share a pool of threads that loads files.
std::mutex& SfizzLock()
{
    static std::mutex lock;
    return lock;
}

// Frees a synth under the lock.
struct LockedDelete
{
    void operator()(sfz::Sfizz* synth) const
    {
        std::lock_guard<std::mutex> lock(SfizzLock());
        delete synth;
    }
};

using Synth = std::unique_ptr<sfz::Sfizz, LockedDelete>;

// The <region> headers in SFZ text `text`.
std::size_t RegionsIn(const std::string& text)
{
    std::size_t count = 0;
    for (std::size_t at = text.find("<region>"); at != std::string::npos; at = text.find("<region>", at + 1))
    {
        count++;
    }

    return count;
}

// A synth at `rate` that plays `file`, written at `path`, or none when the file holds no regions. Throws
// std::runtime_error when sfizz leaves out regions, such as those whose samples it can't read.
Synth LoadSynth(const std::filesystem::path& path, const export_::SfzFile& file, int rate)
{
    // Until the synth is ready, an early return or an exception frees it under the lock.
    std::lock_guard<std::mutex> lock(SfizzLock());
    auto synth = std::make_unique<sfz::Sfizz>();
    synth->setSampleRate(static_cast<float>(rate));
    synth->setSamplesPerBlock(static_cast<int>(kBlock));
    synth->setNumVoices(kVoices);
    synth->setVolume(0.0f); // dB: sfizz starts at -7.35
    synth->enableFreeWheeling();
    synth->setSampleQuality(sfz::Sfizz::ProcessFreewheeling, kSampleQuality);
    const bool loaded = synth->loadSfzFile(xsflib::Utf8(path));
    const std::size_t regions = RegionsIn(file.text);
    const std::size_t played = loaded ? static_cast<std::size_t>(synth->getNumRegions()) : 0;
    if (played < regions)
    {
        throw std::runtime_error("sfizz plays " + std::to_string(played) + " of the " + std::to_string(regions) +
                                 " regions of " + file.name + ".");
    }
    if (!loaded)
    {
        return nullptr;
    }

    // sfizz 1.2.3 sets up its filters for 48 kHz while it loads a file.
    synth->setSampleRate(static_cast<float>(rate));

    return Synth(synth.release());
}

// Sends the channel message `bytes` to `synth`, whatever its channel: each SFZ file is one instrument.
void Send(sfz::Sfizz& synth, const std::vector<uint8_t>& bytes)
{
    const int data1 = bytes.size() > 1 ? bytes[1] : 0;
    const int data2 = bytes.size() > 2 ? bytes[2] : 0;
    switch (bytes[0] & 0xF0)
    {
    case 0x80:
        synth.noteOff(0, data1, data2);
        break;

    case 0x90:
        if (data2 > 0)
        {
            synth.noteOn(0, data1, data2);
        }
        else
        {
            synth.noteOff(0, data1, 0);
        }
        break;

    case 0xA0:
        synth.polyAftertouch(0, data1, data2);
        break;

    case 0xB0:
        synth.cc(0, data1, data2);
        break;

    case 0xC0:
        synth.programChange(0, data1);
        break;

    case 0xD0:
        synth.channelAftertouch(0, data1);
        break;

    case 0xE0:
        synth.pitchWheel(0, (data1 | (data2 << 7)) - 8192);
        break;

    default:
        break;
    }
}

// Renders `track` through `synth` and adds it to `mix`, `frames` stereo samples. `advance` gets the samples rendered so
// far and returns false to stop. Returns false when the render stops.
bool RenderTrack(sfz::Sfizz& synth, const TrackPlay& track, std::size_t frames, std::vector<float>& mix,
                 const std::function<bool(std::size_t)>& advance)
{
    std::vector<float> left(kBlock);
    std::vector<float> right(kBlock);
    float* buffers[2] = {left.data(), right.data()};
    std::size_t next = 0;
    for (std::size_t at = 0; at < frames;)
    {
        for (; next < track.messages.size() && track.messages[next].sample <= at; next++)
        {
            Send(synth, track.messages[next].bytes);
        }

        std::size_t count = std::min(kBlock, frames - at);
        if (next < track.messages.size())
        {
            count = std::min(count, track.messages[next].sample - at);
        }
        synth.renderBlock(buffers, count, 1);
        for (std::size_t i = 0; i < count; i++)
        {
            if (!std::isfinite(left[i]) || !std::isfinite(right[i]))
            {
                throw std::runtime_error("sfizz gave samples that aren't finite for MIDI track \"" + track.name +
                                         "\".");
            }

            mix[2 * (at + i)] += left[i];
            mix[2 * (at + i) + 1] += right[i];
        }
        at += count;
        if (advance && !advance(at))
        {
            return false;
        }
    }

    return true;
}

} // namespace

double MidiSeconds(std::span<const uint8_t> midi)
{
    const seqlib::midi::File file = ReadMidi(midi);
    const TickTimes times(file);
    uint64_t end = 0;
    for (const seqlib::midi::Track& track : file.tracks)
    {
        end = std::max(end, track.end_tick);
    }

    return times.Seconds(end);
}

StereoRender RenderSfz(std::span<const uint8_t> midi, const export_::SfzInstruments& instruments, int rate,
                       double seconds, const std::function<bool(double)>& progress)
{
    if (rate <= 0)
    {
        throw std::invalid_argument("RenderSfz: the rate has to be above 0.");
    }

    const std::size_t frames = seconds > 0.0 ? static_cast<std::size_t>(std::llround(seconds * rate)) : 0;
    const std::vector<TrackPlay> tracks = TracksThatPlay(ReadMidi(midi), instruments.files, rate, frames);
    StereoRender render{rate, std::vector<float>(2 * frames, 0.0f)};
    if (tracks.empty() || frames == 0)
    {
        return render;
    }

    // The files go into a temporary folder, laid out as an export lays them out.
    const TemporaryFolder folder;
    const std::filesystem::path files = folder.Path() / kFilesFolder;
    const std::filesystem::path samples = folder.Path() / xsflib::PathFromUtf8(export_::kSfzSamplesFolder);
    std::filesystem::create_directory(files);
    std::filesystem::create_directory(samples);
    for (const export_::SfzFile& file : instruments.files)
    {
        WriteFile(files, file.name, file.text);
    }
    for (const export_::SfzSample& sample : instruments.samples)
    {
        WriteFile(samples, sample.name, sample.wav);
    }

    // Each track plays through a new synth, one track after another. A file without regions plays nothing.
    for (std::size_t t = 0; t < tracks.size(); t++)
    {
        const auto advance = [&](std::size_t at)
        {
            const double done = (static_cast<double>(t) + static_cast<double>(at) / frames) / tracks.size();
            return !progress || progress(done);
        };

        const Synth synth = LoadSynth(files / xsflib::PathFromUtf8(tracks[t].file->name), *tracks[t].file, rate);
        const bool rendered =
            synth == nullptr ? advance(frames) : RenderTrack(*synth, tracks[t], frames, render.samples, advance);
        if (!rendered)
        {
            return {rate, {}};
        }
    }

    return render;
}

} // namespace hokuto::synth
