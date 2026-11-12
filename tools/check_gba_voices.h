// SPDX-License-Identifier: MIT

// The parts of hokuto_check_gba_voices that its files share: a place of a voice, the totals, a game, the helpers and
// the rounds. check_gba_voices.cpp describes the rounds.

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/archive_player.h"
#include "ntdlib/mp2k/song.h"
#include "seqlib/wav.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "app/item_views.h"
#include "edit/files.h"
#include "edit/gba_tables.h"
#include "edit/gba_voices.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/gba.h"
#include "song/preview.h"

namespace hokuto::tools::gba_voices
{

namespace mp2k = ntdlib::mp2k;
using hokuto::edit::GbaPattern;
using hokuto::format::File;

// A voice that the Bank Editor shows, at the first place that shows it: the program of its voice group, the key that
// the keyboard plays there, the song whose music player plays it, and whether it is a drum kit's voice.
struct Place
{
    std::string name;
    uint32_t program = 0;
    int key = 60;
    int song = -1;
    bool drum = false;
    mp2k::Voice voice;
};

// What the check counted, for the summary.
struct Totals
{
    std::size_t roms = 0;
    std::size_t groups = 0;
    std::size_t voices = 0;
    std::size_t patterns = 0;
    std::size_t edits = 0;
    std::size_t refused = 0;
    std::size_t kept = 0; // values that the edits leave as the ROM stores them, past the PSG's range
    std::size_t levels = 0;
    std::size_t ending = 0; // voices whose samples end
    std::size_t slow = 0;
    std::size_t quiet = 0;
    std::size_t ended = 0; // notes whose output ends before the frames that the level covers
    std::size_t clipped = 0;
    double worst = 0.0; // the largest difference from the ratio of the sustains, in dB
    std::size_t failed = 0;

    // With --new.
    std::size_t kinds_sounded = 0;
    std::size_t kinds_other_rows = 0; // keys that the kinds round's edit of another row sends to another row
    std::size_t new_samples = 0;
    std::size_t compressed = 0;
    std::size_t odd_loops = 0;
    std::size_t odd_rates = 0;
    std::size_t other_ends = 0;
    std::size_t other_rows = 0;
    std::size_t no_room = 0;         // samples without room in the samples round
    std::size_t samples_untried = 0; // samples after the first without room
    std::size_t kinds_no_room = 0;
    std::size_t voices_no_room = 0;
    std::size_t rows_no_room = 0;
    std::size_t in_filler = 0;
    std::size_t notes_compared = 0;
    std::size_t groups_moved = 0;
    std::size_t groups_in_place = 0;
    std::size_t groups_full = 0;
    std::size_t tables_filled = 0;
    std::size_t songs_compared = 0;
    std::size_t copies_compared = 0;
    std::size_t tables_rewritten = 0;
    std::size_t rows_compared = 0;
    std::size_t made_splits = 0;

    // With --songs.
    std::size_t songs_edited = 0;
    std::size_t numbers_read = 0;
    std::size_t reverbs_heard = 0;
    std::size_t reverbs_unheard = 0;
};

// Bytes of a ROM that a voice group plays, and the songs that play the group.
struct Span
{
    uint32_t address = 0;
    uint32_t size = 0;
    const std::vector<int>* songs = nullptr;
};

// The bytes that the voice groups of `rom` play: the voice of each program that the songs select, a key split's key
// table, each voice of its table at every key when it is a key split or a drum kit, and the pattern of each wave voice
// among them. The spans point into `groups`.
std::vector<Span> SpansOf(const hokuto::song::GbaRom& rom, const std::vector<hokuto::song::GbaVoiceGroup>& groups);

// Each voice that the Bank Editor shows of `groups`, the voice groups of `rom`, by its address, and each pattern.
// Unknown voices stay out: the editor shows no fields for them.
std::map<uint32_t, Place> PlacesOf(const hokuto::song::GbaRom& rom,
                                   const std::vector<hokuto::song::GbaVoiceGroup>& groups,
                                   std::map<uint32_t, GbaPattern>& patterns, Totals& totals);

// The round of PSG envelopes.
std::map<uint32_t, mp2k::Voice> PsgEnvelopes(const std::map<uint32_t, Place>& places, Totals& totals);

// The round of PSG settings: the duty, square 1's sweep where no drum kit's pan uses its byte, the noise's mode and the
// length.
std::map<uint32_t, mp2k::Voice> PsgSettings(const std::map<uint32_t, Place>& places, Totals& totals);

// The round of samples' voices: the next sample of the ROM's list, the other pitch mode, and each value of the
// envelope one higher.
std::map<uint32_t, mp2k::Voice> Samples(const std::map<uint32_t, Place>& places, const std::vector<uint32_t>& samples);

// The round of drum kits' voices: a key 5 higher, and the pan on at the centre, or off.
std::map<uint32_t, mp2k::Voice> Drums(const std::map<uint32_t, Place>& places);

// Each pattern upside down.
std::map<uint32_t, GbaPattern> Flipped(const std::map<uint32_t, GbaPattern>& patterns);

// The game's ROM as a file and as bytes, its voice groups, and the bytes that they play.
struct Game
{
    std::string name;
    File file;
    std::vector<uint8_t> bytes;
    std::shared_ptr<const hokuto::song::GbaRom> rom;
    std::vector<hokuto::song::GbaVoiceGroup> groups;
    std::vector<Span> spans;
};

// Makes the edits of a round in a version of the game file, and checks the exported ROM against the ROM of the game.
// The export's list of collisions has to read again the songs whose voice groups play a changed byte, and no others
// (song::GbaSongsWithOtherVoices). Returns the ROM that the version builds, or nothing when the round failed. A voice
// that refuses its edit leaves `voices`. `voices` then contains the edits that the version made.
std::shared_ptr<const hokuto::song::GbaRom> Round(const Game& game, const char* round,
                                                  std::map<uint32_t, mp2k::Voice>& voices,
                                                  const std::map<uint32_t, GbaPattern>& patterns, Totals& totals);

// Round for voices that the caller doesn't read again.
std::shared_ptr<const hokuto::song::GbaRom> Round(const Game& game, const char* round,
                                                  std::map<uint32_t, mp2k::Voice>&& voices,
                                                  const std::map<uint32_t, GbaPattern>& patterns, Totals& totals);

// The round of sustains, and the levels of the held notes before and after it.
void Levels(const Game& game, const std::map<uint32_t, Place>& places, Totals& totals);

// The round of kinds. A new wave plays a new pattern in the free space.
void NewKinds(const Game& game, const std::map<uint32_t, Place>& places, Totals& totals);

// The round of samples through WAV files.
void NewSamples(const Game& game, const std::map<uint32_t, Place>& places, Totals& totals);

// The stereo samples of 3 s of song `song` on ntdlib's player.
std::vector<int16_t> SongOutput(const hokuto::song::GbaRom& rom, int song);

// The round of new voices.
void NewVoices(const Game& game, const std::vector<uint32_t>& samples, Totals& totals);

// The round of the songs' values.
void SongValues(const Game& game, Totals& totals);

} // namespace hokuto::tools::gba_voices
