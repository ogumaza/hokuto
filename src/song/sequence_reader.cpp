// SPDX-License-Identifier: MIT

#include "song/sequence_reader.h"

#include <chrono>
#include <exception>
#include <memory>
#include <utility>

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
        result.engine_sequence = std::make_shared<const Sequence>(
            converted.wii ? ReadSequence(*converted.wii, converted.sound, values, changes, converted.wii_build)
                          : ReadSequence(*converted.archive, converted.sound, values, changes));
        result.engine = std::move(converted);
    }
    catch (const std::exception& error)
    {
        result.engine_error = error.what();
    }
}

} // namespace

// The futures of std::async wait for their reads as they're destroyed.
SequenceReader::~SequenceReader() = default;

void SequenceReader::Request(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, uint32_t sound,
                             StartValues values, Changes changes, Convert convert)
{
    // The read keeps the archive for as long as it takes.
    Request([archive] { return archive; }, sound, std::move(values), std::move(changes), std::move(convert));
}

void SequenceReader::Request(Build build, uint32_t sound, StartValues values, Changes changes, Convert convert)
{
    Abandon();

    const auto read = [make = std::move(build), sound, start = std::move(values), changed = std::move(changes),
                       other = std::move(convert)]
    {
        Result result;
        result.sound = sound;
        try
        {
            result.archive = make();
            result.sequence = std::make_shared<const Sequence>(ReadSequence(*result.archive, sound, start, changed));
        }
        catch (const std::exception& error)
        {
            result.error = error.what();
            return result;
        }
        ReadConversion(result, other, start, changed);

        return result;
    };
    latest_ = std::async(std::launch::async, read);
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
    Abandon();

    const auto read = [make = std::move(build), sound, start = std::move(values), changed = std::move(changes),
                       snd_build, other = std::move(convert)]
    {
        Result result;
        result.sound = sound;
        try
        {
            result.wii = make();
            result.sequence =
                std::make_shared<const Sequence>(ReadSequence(*result.wii, sound, start, changed, snd_build));
        }
        catch (const std::exception& error)
        {
            result.error = error.what();
            return result;
        }
        ReadConversion(result, other, start, changed);

        return result;
    };
    latest_ = std::async(std::launch::async, read);
}

void SequenceReader::Abandon()
{
    if (latest_.valid())
    {
        abandoned_.push_back(std::move(latest_));
    }

    Prune();
}

std::optional<SequenceReader::Result> SequenceReader::Take()
{
    Prune();

    if (!latest_.valid() || !IsDone(latest_))
    {
        return std::nullopt;
    }

    return latest_.get();
}

bool SequenceReader::Reading() const
{
    return latest_.valid() && !IsDone(latest_);
}

void SequenceReader::Prune()
{
    std::erase_if(abandoned_, [](const std::future<Result>& read) { return IsDone(read); });
}

} // namespace hokuto::song
