// SPDX-License-Identifier: MIT

// The sounds of a batch export: the sequence sounds of what plays, the sequence and the MIDI conversion of each, and
// the names of their files. The Export Audio Mixdown window, hok mixdown and hok midi export a batch
// (edit/sound_exports.h).

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sound_build.h"

#include "song/played.h"
#include "song/sequence.h"

namespace hokuto::song
{

// A sequence sound of a batch, by its number in what plays, with its name in the browser.
struct BatchSound
{
    friend bool operator==(const BatchSound&, const BatchSound&) = default;

    uint32_t sound = 0;
    std::string name;
};

// The sequence sounds of `played`, in the browser's order: an archive's sequence sounds, a GBA ROM's songs that play,
// a DS archive's sequences and those of its sequence archives, DSE's songs and sound effects, and the sounds of another
// GBA driver.
std::vector<BatchSound> BatchSounds(const Played& played);

// The sequence of sound `sound` of `played`, read as the program reads it, with the variables' starting values
// `values` and their changes `changes`. The GBA's drivers and DSE read no variables, and the DS reads no changes. A
// sound of the Wii plays by `wii_build` of NW4R SND, or by the build that its archive names. Throws what ReadSequence
// throws.
Sequence ReadBatchSequence(const Played& played, uint32_t sound,
                           const std::optional<ntdlib::SndBuild>& wii_build = std::nullopt,
                           const StartValues& values = {}, const Changes& changes = {});

// Sound `sound` of `played` converted to MIDI with its instruments on the sound's console. The program's export
// converts it the same way (ConvertToMidi). `program` names the program in the SoundFont and the SFZ files. Throws what
// ConvertToMidi throws, EmptyConversion included. `values` and `changes` set the variables as for ReadBatchSequence.
ntdlib::sequence_export::Conversion ConvertBatchSound(const Played& played, uint32_t sound,
                                                      const std::optional<ntdlib::SndBuild>& wii_build,
                                                      const std::string& program, const StartValues& values = {},
                                                      const Changes& changes = {});

// The parts of a file's name in a naming scheme, in the Export Audio Mixdown window's terms.
enum class NameToken
{
    kProject, // the .hok file's name, or the archive's or the ROM's
    kSong,    // the sound's name in the browser
    kNumber,  // the sound's number in the browser, in three digits at least
    kTrack,   // the stem's track by its name, such as "Track 3"
    kText,    // the text set in the scheme
};

// A naming scheme: the tokens of each file's name, in order, between separators.
struct NamingScheme
{
    friend bool operator==(const NamingScheme&, const NamingScheme&) = default;

    std::vector<NameToken> tokens = {NameToken::kSong};
    std::string text;
    std::string separator = " - ";
};

// What a file's name can contain: the project, the song and its number, and a stem's track.
struct NameParts
{
    std::string project;
    std::string song;
    uint32_t number = 0;
    std::string track; // empty for the whole mix
};

// The name of a file by `scheme`, without its extension. A token that comes out empty leaves out its separator too. A
// stem whose scheme has no track token gets the track at the end: stems of one song would share a name otherwise.
std::string SchemeName(const NamingScheme& scheme, const NameParts& parts);

// The names of the files of `names`, the names of a batch of files: each name made safe for every system
// (seqlib::SafeFileName), with `extension`, such as ".wav". A name that an earlier name uses, ignoring case, or that
// `taken` reports as taken, such as a file that's there already, gets " 2", " 3" and so on before the extension. An
// empty name becomes "Sound" and its place in the batch, from 1.
std::vector<std::string> BatchFileNames(std::span<const std::string> names, std::string_view extension,
                                        const std::function<bool(const std::string&)>& taken = {});

} // namespace hokuto::song
