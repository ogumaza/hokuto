// SPDX-License-Identifier: MIT

// Checks the walk that the edits of Konami's driver use against the read of each song of a GBA ROM.
//
// On Konami's driver, a volume command (C0-CF) starts a PSG channel again at the key of the last note command that the
// track ran. The reader finds that command from a run of ntdlib's model of the driver (song::KonamiVolumeStarts). An
// edit of notes or settings has no run. The check of an edit walks each track's commands in the driver's order instead
// (edit::CheckKonamiVolumeStarts). Two ways to the same order can disagree. For each sound that plays a sequence, this
// tool goes through the notes of the read that a volume command starts, and looks for the same volume command with the
// same note command in the walk (edit::KonamiWalkOf). A note that the walk has with another note command, or doesn't
// have, is a difference.
//
// A song that the walk doesn't follow counts apart: the check refuses every edit of it that adds, removes or moves a
// command. The tool prints the reason. One reason counts on its own: the song's loop cuts a track short before a note
// command or a loop point of it. The songs where only a loop after the first does that count on their own too. The tool
// also counts the songs in which a track that the loop starts again ends at another time than the song loops. The walk
// follows those.
//
// The summary lists the counts of songs, of notes compared and of differences. The tool returns 0 when no note differs
// and every song reads.
//
// usage: hokuto_check_konami_starts <rom.gba>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "edit/konami.h"
#include "song/konami.h"

namespace
{

using hokuto::song::KonamiVolumeStart;
using hokuto::tools::CheckedArchive;

struct Totals
{
    std::size_t songs = 0;
    std::size_t unread = 0;
    std::size_t unfollowed = 0;
    std::size_t cut = 0;
    std::size_t later_cut = 0;
    std::size_t apart = 0;
    std::size_t compared = 0;
    std::size_t differences = 0;
};

// Compares the notes of sound `sound` of `archive` that a volume command starts with the walk of sequence `sequence` of
// `file`. Prints each difference under the ROM's `name`.
void CompareSong(const std::string& name, const hokuto::format::File& file, const CheckedArchive& archive,
                 uint32_t sound, uint32_t sequence, Totals& totals)
{
    const std::vector<KonamiVolumeStart> read = hokuto::song::KonamiVolumeStarts(*archive.Konami(), sound);
    const hokuto::edit::KonamiWalked walked = hokuto::edit::KonamiWalkOf(file, sequence);
    totals.apart += walked.ends_apart ? 1 : 0;
    if (walked.stopped)
    {
        std::printf("%s: %s isn't followed, with %zu notes that a volume command starts: %s\n", name.c_str(),
                    archive.NameOf(sound).c_str(), read.size(), walked.stopped->c_str());
        ++(walked.later_loop_cuts ? totals.later_cut : walked.loop_cuts ? totals.cut : totals.unfollowed);
        return;
    }

    for (const KonamiVolumeStart& start : read)
    {
        ++totals.compared;
        if (!std::ranges::binary_search(walked.starts, start))
        {
            std::printf(
                "%s: %s: the volume command at %u starts a note of the command at %lld%s, and the walk "
                "doesn't give that\n",
                name.c_str(), archive.NameOf(sound).c_str(), start.volume,
                start.note ? static_cast<long long>(*start.note) : -1LL, start.on_next ? " on the next track" : "");
            ++totals.differences;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    if (arguments.size() != 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_konami_starts <rom.gba>\n");
        return 2;
    }

    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    const std::string name = xsflib::Utf8(path.filename());
    Totals totals;
    try
    {
        std::vector<uint8_t> bytes;
        xsflib::ReadFile(path, bytes);
        const CheckedArchive archive = hokuto::tools::OpenChecked(path, bytes);
        if (!archive.Konami())
        {
            std::fprintf(stderr, "%s: the ROM doesn't hold Konami's driver\n", name.c_str());
            return 2;
        }

        const hokuto::format::File file = hokuto::tools::ImportChecked(bytes, archive);
        const std::vector<std::optional<uint32_t>> sequences = hokuto::tools::SequencesOfChecked(file, bytes, archive);
        for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
        {
            if (!archive.IsSequence(sound) || !sequences[sound])
            {
                continue;
            }

            ++totals.songs;
            try
            {
                CompareSong(name, file, archive, sound, *sequences[sound], totals);
            }
            catch (const std::exception& error)
            {
                std::printf("%s: %s can't be read: %s\n", name.c_str(), archive.NameOf(sound).c_str(), error.what());
                ++totals.unread;
            }
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s\n", name.c_str(), error.what());
        ++totals.unread;
    }

    std::printf(
        "%s: %zu songs, %zu that can't be read. The walk doesn't follow %zu, %zu more whose first loop cuts a "
        "track short and %zu more where only a later loop does. In %zu songs, a track ends at another time than "
        "the song loops. %zu notes that a volume command starts were compared, and %zu differ.\n",
        name.c_str(), totals.songs, totals.unread, totals.unfollowed, totals.cut, totals.later_cut, totals.apart,
        totals.compared, totals.differences);

    return totals.differences == 0 && totals.unread == 0 ? 0 : 1;
}
