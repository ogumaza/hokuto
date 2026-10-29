// SPDX-License-Identifier: MIT

// Reads sequences away from the program's thread. A read runs ntdlib's performer over the whole sequence. A long
// sequence takes about half a second. After an edit, a read can build the archive of the new version first. For a sound
// on another console's engine, a read also converts the sound for that console and reads the converted sound.

#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "song/sequence.h"

namespace hokuto::song
{

class SequenceReader
{
public:
    // A sound converted for another console: the archive of a file of that console that holds it, its index there, and
    // what changed on the way, in words for the user. A sound converted for the Wii comes in `wii` in place of
    // `archive`, and plays by the rules of `wii_build` of NW4R SND. Where the banks of several slots went in as the
    // Wii's one bank, a note of slot s and program p plays program slot_programs[s] + p there (edit::OtherConsoleSong).
    struct EngineSound
    {
        std::shared_ptr<const ntdlib::sar::SoundArchive> archive;
        uint32_t sound = 0;
        std::vector<std::string> warnings;
        std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
        std::optional<ntdlib::SndBuild> wii_build;
        std::map<int, uint32_t> slot_programs;
    };

    // A read's outcome: the archive that the sequence came from, and the sequence, or the message of the error that
    // stopped the read. A sequence of the Wii comes from `wii` in place of `archive`. A build that failed leaves no
    // archive. A read with a conversion also gives the converted sound and its sequence from the other console's
    // performer, or the message of the error that stopped the conversion.
    struct Result
    {
        uint32_t sound = 0;
        std::shared_ptr<const ntdlib::sar::SoundArchive> archive;
        std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
        std::shared_ptr<const Sequence> sequence;
        std::string error;
        std::optional<EngineSound> engine;
        std::shared_ptr<const Sequence> engine_sequence;
        std::string engine_error;
    };

    // Makes an archive. It throws to give an error.
    using Build = std::function<std::shared_ptr<const ntdlib::sar::SoundArchive>()>;

    // Converts the sound for another console. It throws to give an error.
    using Convert = std::function<EngineSound()>;

    // Makes an archive of the Wii. It throws to give an error.
    using WiiBuild = std::function<std::shared_ptr<const ntdlib::rsar::SoundArchive>()>;

    SequenceReader() = default;
    SequenceReader(const SequenceReader&) = delete;
    SequenceReader& operator=(const SequenceReader&) = delete;

    // Waits for the reads that are still going.
    ~SequenceReader();

    // Starts to read sequence sound `sound` of `archive` on a thread of its own, with the variables' starting values
    // `values` and the game's `changes`. The second form first builds the archive with `build` on that thread. After
    // the read, `convert` converts the sound for another console, if it's set, and the read of the converted sound
    // takes the same values and changes. A read that an earlier request started is abandoned if it's still going.
    void Request(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, uint32_t sound, StartValues values = {},
                 Changes changes = {}, Convert convert = {});
    void Request(Build build, uint32_t sound, StartValues values = {}, Changes changes = {}, Convert convert = {});

    // The same for sequence sound `sound` of the Wii's `archive`, and of the Wii's archive that `build` makes, by the
    // rules of `snd_build` of NW4R SND, the game's build, or else of the build that the archive's version gives. The
    // conversion of `convert` gives a sound of another console.
    void Request(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, uint32_t sound, StartValues values = {},
                 Changes changes = {}, std::optional<ntdlib::SndBuild> snd_build = {}, Convert convert = {});
    void Request(WiiBuild build, uint32_t sound, StartValues values = {}, Changes changes = {},
                 std::optional<ntdlib::SndBuild> snd_build = {}, Convert convert = {});

    // Abandons the latest request's read. Nothing comes of it.
    void Abandon();

    // The latest request's result, once its read is done. Each result comes once.
    std::optional<Result> Take();

    // Whether the latest request's read is still going.
    bool Reading() const;

private:
    // Lets go of the abandoned reads that are done.
    void Prune();

    std::future<Result> latest_;
    std::vector<std::future<Result>> abandoned_;
};

} // namespace hokuto::song
