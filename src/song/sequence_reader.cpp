// SPDX-License-Identifier: MIT

#include "song/sequence_reader.h"

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include "song/driver.h"
#include "song/ds.h"
#include "song/dse.h"

namespace hokuto::song
{
namespace
{

// Whether the read of `future` is done.
bool IsDone(const std::future<SequenceReader::Result>& future)
{
    return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// Gives `result` the sound that `convert` converts for another console, read with `values` and `changes`, or the
// message of the error that stopped the conversion. A conversion that fails leaves the sound on the file's engine.
void ReadConversion(SequenceReader::Result& result, const SequenceReader::Convert& convert, const StartValues& values,
                    const Changes& changes)
{
    if (!convert)
    {
        return;
    }

    try
    {
        SequenceReader::EngineSound converted = convert();
        Sequence played =
            converted.played.Ds() ? ReadSequence(*converted.played.Ds(), converted.sound, values, changes)
            : converted.played.Wii()
                ? ReadSequence(*converted.played.Wii(), converted.sound, values, changes, converted.wii_build)
                : ReadSequence(*converted.played.Sar(), converted.sound, values, changes);

        // Like the song, a conversion of a song of the GBA's other drivers shows its first pass. The performer can find
        // the conversion's loop a pass or more late.
        if (result.sequence->driver != nullptr)
        {
            if (const std::optional<Loop> first = FirstLoopOfConversion(*result.sequence, played))
            {
                played = FirstPass(std::move(played), *first);
            }
        }

        played.arrangement_ticks = TickMap(*result.sequence, played);
        result.engine_sequence = std::make_shared<const Sequence>(std::move(played));
        result.engine = std::move(converted);
    }
    catch (const std::exception& error)
    {
        result.engine_error = error.what();
    }
}

// The outcome of a read of sound `sound`. `make` builds what plays it into `result.*source`, and `read` reads the
// sound's sequence from that. Then `convert` converts the sound for another console, read with `values` and `changes`
// (ReadConversion).
template <typename Source, typename Make, typename Read>
SequenceReader::Result ReadResult(uint32_t sound, std::shared_ptr<const Source> SequenceReader::Result::* source,
                                  const Make& make, const Read& read, const SequenceReader::Convert& convert,
                                  const StartValues& values, const Changes& changes)
{
    SequenceReader::Result result;
    result.sound = sound;
    try
    {
        result.*source = make();
        result.sequence = std::make_shared<const Sequence>(read(*(result.*source)));
    }
    catch (const std::exception& error)
    {
        result.error = error.what();
        return result;
    }

    ReadConversion(result, convert, values, changes);

    return result;
}

} // namespace

// The futures of std::async wait for their reads as they're destroyed. At most one read runs. A request that waits for
// it never starts.
SequenceReader::~SequenceReader() = default;

void SequenceReader::Request(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, uint32_t sound,
                             StartValues values, Changes changes, Convert convert)
{
    // The read keeps the archive for as long as it takes.
    Request([archive] { return archive; }, sound, std::move(values), std::move(changes), std::move(convert));
}

void SequenceReader::Request(Build build, uint32_t sound, StartValues values, Changes changes, Convert convert)
{
    const auto read = [make = std::move(build), sound, start = std::move(values), changed = std::move(changes),
                       other = std::move(convert)]
    {
        const auto read_played = [&](const ntdlib::sar::SoundArchive& played)
        {
            return ReadSequence(played, sound, start, changed);
        };

        return ReadResult(sound, &Result::archive, make, read_played, other, start, changed);
    };
    Start(read);
}

void SequenceReader::Request(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, uint32_t sound,
                             StartValues values, Changes changes, std::optional<ntdlib::SndBuild> snd_build,
                             Convert convert)
{
    Request(WiiBuild([archive] { return archive; }), sound, std::move(values), std::move(changes), snd_build,
            std::move(convert));
}

void SequenceReader::Request(WiiBuild build, uint32_t sound, StartValues values, Changes changes,
                             std::optional<ntdlib::SndBuild> snd_build, Convert convert)
{
    const auto read = [make = std::move(build), sound, start = std::move(values), changed = std::move(changes),
                       snd_build, other = std::move(convert)]
    {
        const auto read_played = [&](const ntdlib::rsar::SoundArchive& played)
        {
            return ReadSequence(played, sound, start, changed, snd_build);
        };

        return ReadResult(sound, &Result::wii, make, read_played, other, start, changed);
    };
    Start(read);
}

void SequenceReader::Request(std::shared_ptr<const GbaRom> rom, uint32_t song, Convert convert)
{
    Request(GbaBuild([rom] { return rom; }), song, std::move(convert));
}

void SequenceReader::Request(GbaBuild build, uint32_t song, Convert convert)
{
    const auto read = [make = std::move(build), song, other = std::move(convert)]
    {
        const auto read_played = [&](const GbaRom& played)
        {
            return ReadSequence(played, static_cast<int>(song));
        };

        return ReadResult(song, &Result::gba, make, read_played, other, {}, {});
    };
    Start(read);
}

void SequenceReader::Request(std::shared_ptr<const DriverRom> rom, uint32_t sound, Convert convert)
{
    Request(DriverBuild([rom] { return rom; }), sound, std::move(convert));
}

void SequenceReader::Request(DriverBuild build, uint32_t sound, Convert convert)
{
    const auto read = [make = std::move(build), sound, other = std::move(convert)]
    {
        const auto read_played = [&](const DriverRom& played)
        {
            return played.ReadSequence(sound);
        };

        return ReadResult(sound, &Result::driver, make, read_played, other, {}, {});
    };
    Start(read);
}

void SequenceReader::Request(std::shared_ptr<const DsArchive> archive, uint32_t sound, StartValues values,
                             Changes changes, Convert convert)
{
    Request(DsBuild([archive] { return archive; }), sound, std::move(values), std::move(changes), std::move(convert));
}

void SequenceReader::Request(DsBuild build, uint32_t sound, StartValues values, Changes changes, Convert convert)
{
    const auto read = [make = std::move(build), sound, values = std::move(values), changed = std::move(changes),
                       other = std::move(convert)]
    {
        const auto read_played = [&](const DsArchive& played)
        {
            return ReadSequence(played, sound, values, changed);
        };

        return ReadResult(sound, &Result::ds, make, read_played, other, values, {});
    };
    Start(read);
}

void SequenceReader::Request(std::shared_ptr<const DseSet> set, uint32_t sound, Convert convert)
{
    Request(DseBuild([set] { return set; }), sound, std::move(convert));
}

void SequenceReader::Request(DseBuild build, uint32_t sound, Convert convert)
{
    const auto read = [make = std::move(build), sound, other = std::move(convert)]
    {
        const auto read_played = [&](const DseSet& played)
        {
            return ReadSequence(played, sound);
        };

        return ReadResult(sound, &Result::dse, make, read_played, other, {}, {});
    };
    Start(read);
}

void SequenceReader::Abandon()
{
    wanted_ = false;
    waiting_ = nullptr;
    Advance();
}

std::optional<SequenceReader::Result> SequenceReader::Take()
{
    Advance();

    if (!wanted_ || !running_.valid() || !IsDone(running_))
    {
        return std::nullopt;
    }

    wanted_ = false;
    return running_.get();
}

bool SequenceReader::Reading() const
{
    return waiting_ != nullptr || (wanted_ && running_.valid() && !IsDone(running_));
}

void SequenceReader::Start(std::function<Result()> read)
{
    Abandon();
    waiting_ = std::move(read);
    Advance();
}

void SequenceReader::Advance()
{
    if (running_.valid() && !wanted_ && IsDone(running_))
    {
        running_ = {};
    }

    if (!running_.valid() && waiting_ != nullptr)
    {
        running_ = std::async(std::launch::async, std::move(waiting_));
        waiting_ = nullptr;
        wanted_ = true;
    }
}

} // namespace hokuto::song
