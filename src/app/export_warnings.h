// SPDX-License-Identifier: MIT

// The warnings that the Export window lists (export_window.h), and the lines of what the measure of an export
// found.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/rom_writer.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sound_build.h"

#include "app/effect_editor.h"
#include "app/export_window.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/audibility.h"
#include "song/items.h"
#include "song/sequence.h"

namespace hokuto::app
{

// The lines of the Measured section: `renderer` played the export, and the measure compared it with Hokuto's playback
// over `seconds` (song::CompareLevels). `unplayed` names what neither side played, such as the effects on the aux
// buses, and can be empty. `departures` names the renderer's known departures from the format's specification, and can
// be empty. `cut` says that the measure stopped before the end of the export. `random` says that the sound draws random
// numbers (edit::DrawsRandom).
std::vector<std::string> MeasuredLines(const song::LevelComparison& comparison, const std::string& renderer,
                                       const std::string& unplayed, const std::string& departures, double seconds,
                                       bool cut, bool random);

// The line of a place that the measure names: how audible it is, where it lies, and how much louder or quieter the
// export plays there. A run of windows more than 6 dB off, or a second more than 3 dB off, is audible. A second from 1
// to 3 dB off may be audible.
std::string PlaceLine(const song::LevelPlace& place);

// The names of `names` in a list for a warning: "A", "A and B", "A, B and C", or the first five and how many more.
std::string NameList(const std::vector<std::string>& names);

// A size for a warning: bytes up to a kilobyte, then kilobytes or megabytes to a tenth.
std::string SizeText(std::size_t bytes);

// The end of a sentence about one thing that says how its size changes from `before` to `after` bytes: "grows by 2.1
// KB, to 38.5 MB", "shrinks by 96 bytes, to 1.1 MB" or "stays at 1.1 MB". With `plural`, the words follow a plural
// subject, such as "grow by 2.1 KB, to 38.5 MB". The change itself shows where the rounding would hide it.
std::string SizeChangeText(std::size_t before, std::size_t after, bool plural = false);

// The sizes of two files for a sentence that compares them, such as "The present file takes 2.1 KB. The new one takes
// 2.3 KB.": SizeText's, or each size in bytes where two different sizes would read the same in SizeText.
std::pair<std::string, std::string> SizeTexts(std::size_t first, std::size_t second);

// The warnings about an archive that grew in an export: the archive, then the files of each group that grew. A game
// loads a group's files into its sound heap, and no file gives the heap's size. The heap's warning shows when a group
// grew. Where `groups_measured` is false, `growth` contains no groups, and any growth of the archive may come from a
// group.
std::vector<std::string> GrowthWarnings(const format::ArchiveGrowth& growth, bool groups_measured);

// The warning about DSE's files that grew from `before` to `after` bytes in an export. DSE has no groups.
std::vector<std::string> DseGrowthWarnings(std::size_t before, std::size_t after);

// The warnings about a DS cartridge image that grew from `before` to `after` bytes in an export. An image that fills
// its chip grows to the size of the next chip once its data no longer fits.
std::vector<std::string> DsImageWarnings(std::size_t before, std::size_t after);

// Where an export of a GBA ROM puts the songs that outgrow their places.
enum class RomRoom
{
    kFreeSpace, // the free space after the ROM's data
    kFiller,    // the ROM has no room after its data: the songs that don't fit there go into its filler runs
    kLeftOut,   // the ROM has no room after its data: the songs' edits stay out
};

// The warnings about a GBA ROM after an export: the `moved` songs that outgrew their places, where the export put them,
// and how the ROM grew from `before` to `after` bytes. No file of the game says where its free space is, and nothing
// shows that the game's other code doesn't read a filler run. A filler run is 16 KiB of 0xFF bytes for MP2K
// (ntdlib::mp2k::kLeastFillerRun), and 4 KiB of 0xFF or zero bytes for Konami's driver (`konami`).
std::vector<std::string> RomWarnings(std::size_t moved, std::size_t before, std::size_t after,
                                     RomRoom room = RomRoom::kFreeSpace, bool konami = false);

// The warnings about the notes that edits make collide on the GBA's channels: a line for each of `songs`, by its name,
// with the collisions that the edits add (song::AddedGbaCollisions), then where the editor shows them. The first eight
// songs get a line each, and a line counts the rest.
std::vector<std::string> CollisionWarnings(const std::vector<std::pair<std::string, song::Collisions>>& songs,
                                           const std::string& console);

// The notes of `collisions` on `console`, such as "the GBA", for the warnings: "1 note cut short by a later note and 52
// notes that the GBA never plays".
std::string CollisionText(const song::Collisions& collisions, const std::string& console);

// The last line of the warnings about collisions: why notes lose their channel on `console`, and where the editor shows
// them.
std::string ChannelsNote(const std::string& console);

// The note of an export of the archive that writes streams beside it, at `paths`: the streams that the game lacks.
std::string StreamsNote(const std::vector<std::string>& paths);

// The note of an export into a DS image that also runs on the DSi, with the hashes that the export leaves stale
// (ntdlib::ntr::WithFiles). The note counts the hashes of the image's digest: the hashes that the DSi's digest key
// updates. Without such stale hashes, the key wrote them again. With `key_failed`, Studio Setup names a key file that
// Hokuto couldn't use.
std::string DsiNote(const std::vector<ntdlib::ntr::StaleHash>& stale, bool key_failed = false);

// The note of an export into a DS image, with the hashes that the export leaves stale (ntdlib::ntr::WithFiles). It
// lists those that no key updates, such as the header's RSA signature, or returns nothing when there are none. `dsi` is
// set for an image that also runs on the DSi.
std::optional<std::string> SignatureNote(const std::vector<ntdlib::ntr::StaleHash>& stale, bool dsi);

// The note of an export of the archive that writes group files beside it, under `names`: the group files that the edits
// changed. A name that starts with the userManagementFiles folder's name and a slash goes into that folder
// (song::PlaceGroupFiles).
std::string GroupFilesNote(const std::vector<std::string>& names);

// The warning of an export that leaves out group file `index`, counted from 0: an edit changed it, and Hokuto couldn't
// find its group.
std::string UnknownGroupNote(std::size_t index);

// The name of `console` for the middle of a sentence, such as "the Wii U". DSE takes no article.
std::string TheConsole(const std::string& console);

// The note of an export to MIDI of a sound that plays on the engine of `console`, another console than the file's
// `file_console`. The files hold the sound's conversion for that console.
std::string EngineNote(const std::string& console, const std::string& file_console);

// The note of an export of the archive whose sounds `names` play on other consoles' engines in Hokuto. The archive
// keeps them for `file_console`.
std::string EngineSoundsNote(const std::vector<std::string>& names, const std::string& file_console);

// What the MIDI file and the instruments of `target` can't hold of `sequence`, beside what the converter reports: the
// paths that its variables choose, the effects on the aux buses, and the buses' volumes.
std::vector<std::string> MidiWarnings(const song::Sequence& sequence, const BusEffects& effects,
                                      const std::array<float, format::kAuxBuses>& volumes, ExportTarget target);

// The warnings of an export of `item` of `archive` to `target`: the other sounds whose sequences a sequence file holds,
// or whose wave sounds a wave sound file holds, and what a WAV file leaves out: channels past the first two, and a wave
// sound's other notes, its pan and its envelope. A file for another console takes the oldest versions that the
// console's games hold, and a warning gives a version older than the file's. `found` lists what the export's work
// found: each wave of a wave archive that a WAV export leaves out, with the reason, or what the converter reports of a
// bank's instruments.
std::vector<std::string> ItemWarnings(const ntdlib::sar::SoundArchive& archive, const song::Item& item,
                                      ExportTarget target, const std::vector<std::string>& found);

// The warnings of an export of the stream file `stream` as a file of the console of `to`. A stream of a later version
// than the one that goes to that console goes out in that version. A stream that goes to the 3DS from another console
// holds none of the samples that the 3DS's tools encode past a looped stream's end. Before version 2.3.0.0 of a 3DS
// archive, a stream sound plays the tracks of its stream file, and the converted stream may hold none. A stream of the
// Wii keeps its tracks. A stream that ntdlib can't read gives no warnings: the conversion fails with the reason.
std::vector<std::string> StreamConversionWarnings(std::span<const uint8_t> stream, ntdlib::sar::Flavor to);

// The warnings of an export of `stream`, a stream file of the Wii, as a WAV file. `build` of NW4R SND plays it. A build
// before March 2008 plays only the first two channels. A later build plays the stream's tracks with their volumes and
// pans. The WAV file holds every channel as it is.
std::vector<std::string> WiiStreamWavWarnings(std::span<const uint8_t> stream, const ntdlib::SndBuild& build);

// The same for `item` of the Wii's `archive`: the other sounds whose sequences or wave sounds its file holds, and what
// a WAV file leaves out: channels past the first two, and a wave sound's pan and envelope. NW4R SND plays only a wave
// sound's first note.
std::vector<std::string> ItemWarnings(const ntdlib::rsar::SoundArchive& archive, const song::Item& item,
                                      ExportTarget target, const std::vector<std::string>& found);

// The lines that the Export window lists for an export of SFZ files (`sfz`) or SoundFont 2 files: those of a
// conversion's report (song::ConversionReport) or a bank's warnings from ntdlib's converter. A line that names only the
// other format stays out (song::ReportFor), and each line becomes a sentence.
std::vector<std::string> ConverterWarnings(std::vector<std::string> lines, bool sfz);

// The warning of an export of a bank's instruments to SoundFont 2 when they need several files, or nothing. `stem` is
// the name that the dialog offers, without its extension.
std::vector<std::string> SoundFontWarnings(const ntdlib::sequence_export::BankConversion& bank,
                                           const std::string& stem);

} // namespace hokuto::app
