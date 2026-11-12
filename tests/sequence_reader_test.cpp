// SPDX-License-Identifier: MIT

// Tests the reader of sequences on its threads (src/song/sequence_reader.cpp).

#include "song_runs.h"

namespace
{

// The reader gives the sequence that ReadSequence gives, once. For a sound past the archive's sounds, it gives a
// message and no sequence.
void TestReader()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const Sequence direct = ReadSequence(*archive, kLoopSound);
    SequenceReader reader;

    reader.Request(archive, kLoopSound);
    const std::optional<SequenceReader::Result> read = TakeRead(reader);
    const bool again = reader.Take().has_value();
    reader.Request(archive, kMissingSound);
    const std::optional<SequenceReader::Result> missing = TakeRead(reader);

    SEQLIB_CHECK(read && read->sound == kLoopSound && read->sequence && *read->sequence == direct);
    SEQLIB_CHECK(read && read->error.empty() && !again && !reader.Reading());
    SEQLIB_CHECK(missing && missing->sound == kMissingSound && !missing->sequence && !missing->error.empty());
}

// A request abandons the read that the request before it started. Only the latest request's result comes. Abandon drops
// that one too.
void TestReaderAbandons()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    SequenceReader reader;

    reader.Request(archive, kLoopSound);
    reader.Request(archive, kNoteSound);
    const std::optional<SequenceReader::Result> read = TakeRead(reader);
    reader.Request(archive, kReleaseSound);
    reader.Abandon();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SEQLIB_CHECK(read && read->sound == kNoteSound && read->sequence && read->sequence->sound == kNoteSound);
    SEQLIB_CHECK(!reader.Reading() && !reader.Take());
}

// At most one read runs. A request that comes while a read runs waits, and a later request replaces it: the
// request in between never runs. The latest request's read starts once the abandoned one ends.
void TestReaderWaits()
{
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::atomic<int> firsts{0};
    std::atomic<int> seconds{0};
    std::atomic<int> thirds{0};
    SequenceReader reader;

    reader.Request(SequenceReader::Build(
                       [&]() -> std::shared_ptr<const sar::SoundArchive>
                       {
                           firsts++;
                           released.wait();
                           throw std::invalid_argument("First.");
                       }),
                   kLoopSound);
    reader.Request(SequenceReader::Build(
                       [&]() -> std::shared_ptr<const sar::SoundArchive>
                       {
                           seconds++;
                           throw std::invalid_argument("Second.");
                       }),
                   kLoopSound);
    reader.Request(SequenceReader::Build(
                       [&]() -> std::shared_ptr<const sar::SoundArchive>
                       {
                           thirds++;
                           throw std::invalid_argument("Third.");
                       }),
                   kLoopSound);
    const bool reading = reader.Reading();
    const bool early = reader.Take().has_value();
    release.set_value();
    const std::optional<SequenceReader::Result> read = TakeRead(reader);

    SEQLIB_CHECK(reading && !early);
    SEQLIB_CHECK(read && read->error == "Third.");
    SEQLIB_CHECK(firsts == 1 && seconds == 0 && thirds == 1);
    SEQLIB_CHECK(!reader.Reading() && !reader.Take());
}

// A read can build its archive first. The result holds the archive that the build made, and the sequence of it. A build
// that throws gives its message, with no archive and no sequence.
void TestReaderBuilds()
{
    const std::vector<uint8_t> bytes = TestArchiveBytes();
    const Sequence direct = ReadSequence(*LoadArchive(bytes, "test.bfsar"), kLoopSound);
    std::shared_ptr<const sar::SoundArchive> built;
    const auto build = [&bytes, &built]
    {
        built = LoadArchive(bytes, "test.bfsar");
        return built;
    };
    const auto fail = []() -> std::shared_ptr<const sar::SoundArchive>
    {
        throw std::invalid_argument("No archive.");
    };
    SequenceReader reader;

    reader.Request(build, kLoopSound);
    const std::optional<SequenceReader::Result> read = TakeRead(reader);
    reader.Request(fail, kLoopSound);
    const std::optional<SequenceReader::Result> failed = TakeRead(reader);

    SEQLIB_CHECK(read && built && read->archive == built && read->sequence && *read->sequence == direct);
    SEQLIB_CHECK(failed && !failed->archive && !failed->sequence && failed->error == "No archive.");
}

// After the read, a conversion gives the converted sound, and the read of it takes the same starting values. A
// conversion that throws gives its message and leaves the sequence. A read that fails converts nothing.
void TestReaderConverts()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const StartValues kValues = {{0, 1}};
    const Sequence direct = ReadSequence(*archive, kLoopSound, kValues);
    const Sequence converted_direct = ReadSequence(*archive, kNoteSound, kValues);
    int conversions = 0;
    const auto convert = [&archive, &conversions]
    {
        conversions++;

        return SequenceReader::EngineSound{.played = archive, .sound = kNoteSound, .warnings = {"A warning."}};
    };
    const auto fail = []() -> SequenceReader::EngineSound
    {
        throw std::invalid_argument("No conversion.");
    };
    SequenceReader reader;

    reader.Request(archive, kLoopSound, kValues, {}, convert);
    const std::optional<SequenceReader::Result> read = TakeRead(reader);
    reader.Request(archive, kLoopSound, kValues, {}, fail);
    const std::optional<SequenceReader::Result> failed = TakeRead(reader);
    reader.Request(archive, kMissingSound, kValues, {}, convert);
    const std::optional<SequenceReader::Result> missing = TakeRead(reader);
    reader.Request(archive, kLoopSound, kValues);
    const std::optional<SequenceReader::Result> alone = TakeRead(reader);

    SEQLIB_CHECK(read && read->sequence && *read->sequence == direct && read->engine && read->engine_sequence);
    SEQLIB_CHECK(read && read->engine && read->engine->played.Sar() == archive && read->engine->sound == kNoteSound);
    SEQLIB_CHECK(read && read->engine && read->engine->warnings == std::vector<std::string>{"A warning."});
    SEQLIB_CHECK(read && read->engine_sequence && ConvertedAs(*read->engine_sequence, converted_direct));
    SEQLIB_CHECK(read && read->engine_error.empty());
    SEQLIB_CHECK(failed && failed->sequence && *failed->sequence == direct && !failed->engine);
    SEQLIB_CHECK(failed && !failed->engine_sequence && failed->engine_error == "No conversion.");
    SEQLIB_CHECK(missing && !missing->sequence && !missing->engine && missing->engine_error.empty());
    SEQLIB_CHECK(alone && alone->sequence && !alone->engine && !alone->engine_sequence);
    SEQLIB_CHECK(conversions == 1);
}

// A conversion for the Wii gives the sound in the Wii's archive, and the read of the converted sound follows the
// conversion's build of NW4R SND with the same starting values.
void TestReaderConvertsForWii()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii = OpenWiiArchive(kWiiChangesBytecode);
    const ntdlib::SndBuild kBuild{.date = 20080620};
    const StartValues kValues = {{0, 1}};
    const auto convert = [&wii, &kBuild]
    {
        return SequenceReader::EngineSound{.played = wii, .sound = 0, .warnings = {"A warning."}, .wii_build = kBuild};
    };
    SequenceReader reader;
    reader.Request(archive, kLoopSound, kValues, {}, convert);
    const std::optional<SequenceReader::Result> read = TakeRead(reader);

    SEQLIB_CHECK(read && read->sequence && *read->sequence == ReadSequence(*archive, kLoopSound, kValues));
    SEQLIB_CHECK(read && read->engine && read->engine->played.Wii() == wii && !read->engine->played.Sar());
    SEQLIB_CHECK(read && read->engine && read->engine->warnings == std::vector<std::string>{"A warning."});
    SEQLIB_CHECK(read && read->engine_sequence &&
                 ConvertedAs(*read->engine_sequence, ReadSequence(*wii, 0, kValues, {}, kBuild)));
    SEQLIB_CHECK(read && read->engine_error.empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sequence_reader",
                                    []
                                    {
                                        TestReaderConvertsForWii();
                                        TestReader();
                                        TestReaderAbandons();
                                        TestReaderWaits();
                                        TestReaderBuilds();
                                        TestReaderConverts();
                                    });
}
