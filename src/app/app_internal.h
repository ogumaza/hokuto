// SPDX-License-Identifier: MIT

// Constants and helpers that several of the files app_*.cpp use: the sizes of the auditions' worker, the browser's
// groups, and small functions about files, archives and the arrangement.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/dse/driver.h"
#include "ntdlib/ntr/snd/driver.h"

#include "app/replace_view.h"
#include "app/song_view.h"
#include "app/views.h"
#include "edit/files.h"
#include "format/file.h"
#include "song/ds.h"
#include "song/played.h"
#include "song/sequence_model.h"

namespace hokuto::app
{

// The auditions' worker keeps a lead of two of the device's buffers in its ring, and 10 ms at least. The ring then
// holds a buffer's frames whenever the device takes them. A note's release comes up to a lead late.
inline constexpr std::chrono::milliseconds kLeastAuditionLead{10};
inline constexpr std::size_t kAuditionBlockFrames = 64;

// The rate of the ring's frames until a renderer sets another, and the test tone's rate. SDL3 converts it to the
// device's rate.
inline constexpr int kDefaultRate = 48'000;

// The name of a new file before its first save.
inline constexpr char kUntitled[] = "Untitled";

// The tracks' colours in Preferences and in the settings' file, in the order of TrackColours (app_work.h).
inline constexpr std::array<const char*, 3> kTrackColourNames = {"Rainbow", "Standard", "Custom"};

// The browser's groups, in order. Sounds holds the archive's stream sounds and wave sounds. Sequences holds its
// sequence sounds. Waves holds its wave archives. Wave Sound Files holds its wave sound files, each with its wave
// sounds.
inline constexpr std::size_t kSoundsGroup = 0;
inline constexpr std::size_t kSequencesGroup = 1;
inline constexpr std::size_t kBanksGroup = 2;
inline constexpr std::size_t kWavesGroup = 3;
inline constexpr std::size_t kWaveSoundFilesGroup = 4;

// The title of Replace…'s dialog for a file of `kind`, and the name of its action in the Undo History.
std::string ReplaceAction(edit::FileKind kind);

// Adds the lines of `sizes` to `lines` once the thread has measured them.
void TakeGroupSizes(std::future<std::vector<std::string>>& sizes, std::vector<std::string>& lines);

// Whether the work that `work` waits for is done. The future of std::async blocks in its destructor until then.
template <typename T>
bool Done(const std::future<T>& work)
{
    return work.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// The part that the arrangement shows as selected, or nullptr.
const PartView* SelectedPart(const std::optional<SongView>& song, const ArrangementState& state);

// Whether versions `a` and `b` of a file build the same archive. The versions share the sequences and the native
// archive that their edits leave alone.
bool SameArchive(const format::File& a, const format::File& b);

// The mixing mode in which sound `sound` of `file` plays: the file's mode for the sound, or strict mixing on an engine
// that takes no other (format::MixingRefused), such as the Wii's. The mode that the file keeps for the sound stays.
song::Mixing PlayedMixing(const format::File& file, uint32_t sound);

// Whether `archive` holds a sound of one of `types`.
template <class Archive, class Type>
bool HoldsSoundOf(const Archive& archive, std::initializer_list<Type> types)
{
    return std::ranges::any_of(
        archive.Sounds(), [types](const auto& sound) { return std::ranges::find(types, sound.type) != types.end(); });
}

// The field of `document`'s native layout that holds each sound's start (format::StartFieldsOfSounds), or nothing when
// the archive's tables can't be read. `bytes` are the archive that the layout builds. Without the fields, Replace…
// stays disabled.
std::optional<std::vector<std::optional<uint32_t>>> StartFieldsOf(const format::File& document,
                                                                  std::span<const uint8_t> bytes);

// The build of the driver that plays the DSE files of `document`: the build in the ARM9 binary of the cartridge image
// that the file keeps, or else the later build (song::DseBuildOf).
ntdlib::dse::Build DseBuildOfFile(const format::File& document);

// The build of the driver that plays the DS's archive of `document`: the build in the ARM7 binary of the cartridge
// image that the file keeps, or else the default build (song::DsBuildOf).
ntdlib::ntr::snd::Build DsBuildOfFile(const format::File& document);

// The game of `archive`, a DS archive that the program opened, or nothing. A cartridge image whose last file ends in
// 0xFF bytes requires a copy to find the game (edit::DsGameOfFile). The program therefore works the game out as the
// file opens, and the later archives of the file take the game of the archive that opened.
std::optional<song::DsGame> GameOf(const std::shared_ptr<const song::DsArchive>& archive);

// Reads `bytes`, a sequence file or a MIDI file named `name`, as the sequence for `file`, by the rules of the
// console of `file`. `source` receives the sequence, and the bytes and drum notes of a MIDI file. Throws
// std::invalid_argument with the reason for bytes that the console can't take.
void ReadIncomingSequence(ReplaceSource& source, std::vector<uint8_t> bytes, const std::string& name,
                          const format::File& file);

// The source of a replacement or an import of `sound` from the file `file_name`, into `file` as `played` plays it.
ReplaceSource SourceAt(std::shared_ptr<const format::File> file, song::Played played, uint32_t sound,
                       std::string file_name);

// The edit that the Replace window or the Import window offers: the new version, or else why the edit can't be made.
struct EditAttempt
{
    std::optional<format::File> result;
    std::string error;
};

// Makes the edit of `edit()`. The message of a std::invalid_argument from it becomes the reason.
template <typename Edit>
EditAttempt TryEdit(const Edit& edit)
{
    EditAttempt attempt;
    try
    {
        attempt.result = edit();
    }
    catch (const std::invalid_argument& failure)
    {
        attempt.error = failure.what();
    }

    return attempt;
}

// The archive, the ROM or the set that `version` builds, for each kind. The DS keeps the game of the archive that
// opened, and the archives of the Wii and the later consoles take the file's `name`.
std::shared_ptr<const song::GbaRom> GbaRomOf(const format::File& version);
std::shared_ptr<const song::DriverRom> DriverRomOf(const format::File& version);
std::shared_ptr<const song::DsArchive> DsArchiveOf(const format::File& version, std::optional<song::DsGame> game);
std::shared_ptr<const song::DseSet> DseSetOf(const format::File& version);
std::shared_ptr<const ntdlib::rsar::SoundArchive> WiiArchiveOf(const format::File& version, const std::string& name);
std::shared_ptr<const ntdlib::sar::SoundArchive> SarArchiveOf(const format::File& version, const std::string& name);

} // namespace hokuto::app
