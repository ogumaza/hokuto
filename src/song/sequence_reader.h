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

#include "song/played.h"
#include "song/sequence.h"

namespace hokuto::song
{

class SequenceReader
{
public:
    // A sound converted for another console: the archive of a file of that console that holds it, its index there, and
    // what changed on the way, in words for the user. The archive is an archive of a later console, the Wii or the DS.
    // A sound converted for the Wii plays by the rules of `wii_build` of NW4R SND. Where the banks of several slots
    // went in as the one bank of the Wii or the DS, a note of slot s and program p plays program slot_programs[s] + p
    // there (edit::OtherConsoleSong).
    struct EngineSound
    {
        Played played;
        uint32_t sound = 0;
        std::vector<std::string> warnings;
        std::optional<ntdlib::SndBuild> wii_build;
        std::map<int, uint32_t> slot_programs;
    };

    // A read's outcome: the archive that the sequence came from, and the sequence, or the message of the error that
    // stopped the read. A sequence of the Wii comes from `wii` in place of `archive`, a song of the GBA from `gba`, a
    // sequence of the DS from `ds`, a song or a sound effect of DSE from `dse`, and a song of one of the GBA's other
    // drivers, such as Konami's driver, from `driver`. A build that failed leaves no archive. A read with a conversion
    // also gives the converted sound and its sequence from the other console's performer, or the message of the error
    // that stopped the conversion. The conversion's sequence contains the map of the arrangement's ticks to its ticks
    // (Sequence::arrangement_ticks). For a song of the GBA's other drivers, it ends at the first pass of its loop
    // (FirstLoopOfConversion).
    struct Result
    {
        uint32_t sound = 0;
        std::shared_ptr<const ntdlib::sar::SoundArchive> archive;
        std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
        std::shared_ptr<const GbaRom> gba;
        std::shared_ptr<const DsArchive> ds;
        std::shared_ptr<const DseSet> dse;
        std::shared_ptr<const DriverRom> driver;
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

    // Makes a GBA ROM. It throws to give an error.
    using GbaBuild = std::function<std::shared_ptr<const GbaRom>()>;

    // Makes a GBA ROM with one of the GBA's other drivers (song/driver.h). It throws to give an error.
    using DriverBuild = std::function<std::shared_ptr<const DriverRom>()>;

    // Makes a sound archive of the DS. It throws to give an error.
    using DsBuild = std::function<std::shared_ptr<const DsArchive>()>;

    // Reads the DSE sounds of a DS cartridge image. It throws to give an error.
    using DseBuild = std::function<std::shared_ptr<const DseSet>()>;

    SequenceReader() = default;
    SequenceReader(const SequenceReader&) = delete;
    SequenceReader& operator=(const SequenceReader&) = delete;

    // Waits for the reads that are still going.
    ~SequenceReader();

    // Starts to read sequence sound `sound` of `archive` on a thread of its own, with the variables' starting values
    // `values` and the game's `changes`. The second form first builds the archive with `build` on that thread. After
    // the read, `convert` converts the sound for another console, if it's set, and the read of the converted sound
    // takes the same values and changes. A read that an earlier request started is abandoned if it's still going.
    // At most one read runs at a time. A request that comes while a read runs waits for that read to end, in the place
    // of the request that waited before it.
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

    // The same for song `song` of the GBA's `rom`, and of the ROM that `build` makes. A GBA song has no variables. The
    // conversion of `convert` gives a sound of another console.
    void Request(std::shared_ptr<const GbaRom> rom, uint32_t song, Convert convert = {});
    void Request(GbaBuild build, uint32_t song, Convert convert = {});

    // The same for sound `sound` of `rom`, a GBA ROM with one of the GBA's other drivers, and of the ROM that `build`
    // makes.
    void Request(std::shared_ptr<const DriverRom> rom, uint32_t sound, Convert convert = {});
    void Request(DriverBuild build, uint32_t sound, Convert convert = {});

    // The same for sound `sound` of the DS's `archive`, and of the archive that `build` makes, with the variables'
    // starting values `values` and the changes `changes`. The conversion of `convert` gives a sound of another console.
    // The read of the converted sound takes no changes: a conversion of a DS sequence reads no variables.
    void Request(std::shared_ptr<const DsArchive> archive, uint32_t sound, StartValues values = {},
                 Changes changes = {}, Convert convert = {});
    void Request(DsBuild build, uint32_t sound, StartValues values = {}, Changes changes = {}, Convert convert = {});

    // The same for sound `sound` of `set`, the DSE sounds of a cartridge image, and of the set that `build` reads. DSE
    // has no variables. The conversion of `convert` gives a sound of another console.
    void Request(std::shared_ptr<const DseSet> set, uint32_t sound, Convert convert = {});
    void Request(DseBuild build, uint32_t sound, Convert convert = {});

    // Abandons the latest request's read. Nothing comes of it.
    void Abandon();

    // The latest request's result, once its read is done. Each result comes once.
    std::optional<Result> Take();

    // Whether the latest request's read is still going.
    bool Reading() const;

private:
    // Abandons the read of the request before. Starts `read` once no read runs.
    void Start(std::function<Result()> read);

    // Lets go of a read that is done and whose result no request uses. Then starts the waiting request if no read runs.
    void Advance();

    std::future<Result> running_;     // the read that runs, or the latest request's result until Take
    bool wanted_ = false;             // whether running_ is the latest request's read
    std::function<Result()> waiting_; // the latest request, while an abandoned read still runs
};

} // namespace hokuto::song
