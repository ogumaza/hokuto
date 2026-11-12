// SPDX-License-Identifier: MIT

// The replacements of DSE's sounds, banks and waves from files, for Replace…. A file of DSE's files keeps a set of the
// image's files (ntdlib/dse/layout.h). The tracks of a song or a sound effect are a sequence of the set, and its
// settings are in the run before them. A wave bank's file is a run of its own.
//
// - A sound reads its new tracks from a song file (smdl), or from a MIDI file (DseSongOfMidi). The sound keeps its
//   settings, but for the ticks of a quarter note and the counts of tracks and channels. Those come from the song file,
//   or from the new tracks where they hold more. The game finds a song's bank by the song's name. Its tracks select the
//   bank by the id in the bank's header. New tracks that select none of the sound's banks select the sound's first bank
//   in place of each of their bank selects. Tracks that select one of the sound's banks keep their selects: a game's
//   tracks can also select banks that the game doesn't load with the sound.
// - A bank gets a wave bank file (swdl). Every file of the set that contains the same bank gets it. The new bank keeps
//   the current bank's id and name. The tracks and the game then find it. A bank whose waves are in a main bank keeps
//   the current main bank. Its splits then play the main bank's waves with the same numbers.
// - A wave reads its new samples from a WAV file in one of the DS's encodings (ReadIncomingDsWave in edit/files.h). The
//   samples go where the wave's lay, or after the bank's samples when they don't fit there. DSE's driver reads no
//   wave's rate. The tuning of a split sets the pitch, and the tools write it from the rate of the wave. The tuning of
//   each split that plays the wave moves by the new rate, by the rule of edit::WithDseWave. The table of each bank that
//   reads its waves from the bank gets the new values too.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/files.h"
#include "edit/replace.h"
#include "format/file.h"
#include "song/dse.h"

namespace hokuto::edit
{

// The tracks that come into a sound: their commands (ntdlib::dse::DecodeTracks), the ticks of a quarter note, the count
// of tracks and the channels that a song file states, and what the file left out on the way. A song file can state more
// tracks than it contains. A MIDI file states none: the count of its tracks replaces them.
struct DseIncomingSong
{
    ntdlib::commands::Sequence tracks;
    uint16_t ticks_per_quarter = 48;
    uint8_t track_count = 0;
    uint8_t channels = 16;
    std::vector<std::string> warnings;
};

// The tracks of `bytes`, a DSE song file (smdl). Throws std::invalid_argument with a message for the user for bytes
// that aren't one.
DseIncomingSong ReadIncomingDseSong(std::span<const uint8_t> bytes);

// The tracks of the MIDI file `midi` as DSE plays them (edit/midi_imports.h). `file_name` is the name of the file.
// `programs` maps each program of the file to the program that plays it, and `drum_notes` maps the notes of the drum
// channel of MIDI to what plays them. Each MIDI channel becomes a track on a separate channel of the driver, and its
// opening selects a bank. Throws std::invalid_argument with a message for the user.
DseIncomingSong DseSongOfMidi(std::span<const uint8_t> midi, const std::string& file_name,
                              const ProgramMap& programs = {}, const format::MidiDrumNotes& drum_notes = {});

// The programs that the program changes of `song` select, in order.
std::vector<uint32_t> DseSongPrograms(const DseIncomingSong& song);

// The tracks of `song`.
std::size_t DseSongTracks(const DseIncomingSong& song);

// The tracks in a sound effect. A sound effect plays at most 4.
inline constexpr std::size_t kDseEffectTracks = 4;

// `file`, a file of DSE's files, with the tracks of `song` in the place of the tracks of sound `sound` of `set`, the
// set that `file` builds. Each program change gets the program of `programs` (slot 0) where the map has one. Throws
// std::invalid_argument with a message for the user for a sound whose tracks Hokuto can't edit, more tracks than the
// sound accepts, and tracks that the bytes of DSE can't store.
format::File ReplaceDseSound(const format::File& file, const song::DseSet& set, uint32_t sound,
                             const DseIncomingSong& song, const ProgramMap& programs = {});

// What a user should know about `song` in the place of sound `sound`'s tracks, with `programs`: the bank selects that
// select another bank, and the programs that are missing from the sound's banks. Each line is a sentence.
std::vector<std::string> DseSoundWarnings(const song::DseSet& set, uint32_t sound, const DseIncomingSong& song,
                                          const ProgramMap& programs = {});

// The sounds of `set` that play bank `bank`, in order.
std::vector<uint32_t> DseSoundsOfBank(const song::DseSet& set, uint32_t bank);

// The bank of `bytes`, a DSE wave bank file (swdl). Throws std::invalid_argument with a message for the user for bytes
// that aren't one.
ntdlib::dse::WaveBank ReadIncomingDseBank(std::span<const uint8_t> bytes);

// `file` with `incoming` in the place of bank `bank` of `set`, in every file of the set that contains the bank. The new
// bank gets the id and the name of the current bank, and the id of a main bank where its waves are in one. Throws
// std::invalid_argument with a message for the user for a bank that the set doesn't contain, and for a new bank whose
// waves are in a main bank in the place of one that contains its samples.
format::File ReplaceDseBank(const format::File& file, const song::DseSet& set, uint32_t bank,
                            const ntdlib::dse::WaveBank& incoming);

// What a user should know about `incoming` in the place of bank `bank`: the programs that the bank's sounds select and
// the new bank is missing, the waves that the new splits play and the main bank is missing, the main bank that plays
// the waves of a bank without samples, and new samples that the game may not load. Each line is a sentence.
std::vector<std::string> DseBankWarnings(const song::DseSet& set, uint32_t bank, const ntdlib::dse::WaveBank& incoming);

// A wave's new samples, in a DS wave's parameters and samples (ntdlib::sdat::WaveBytes), at `rate`.
struct DseIncomingWave
{
    std::vector<uint8_t> wave;
    uint32_t rate = 0;
};

// The bytes that the samples of `wave`, the parameters and the samples of a DS wave (ntdlib::sdat::WaveBytes), use in a
// bank: the words of the start and the length of its loop. A bank keeps no other bytes of the wave with its samples.
// ChangesOfDseWave computes the size of a wave the same way. Throws std::invalid_argument with a message for the user
// for bytes that aren't such a wave.
std::size_t DseWaveSize(std::span<const uint8_t> wave);

// `file` with `incoming` in the place of wave `wave` of bank `bank` of `set`, a bank that contains samples. Throws
// std::invalid_argument with a message for the user for a wave that the bank doesn't contain, a bank without samples, a
// square wave or noise, a rate that moves the tuning of a split past its 16 bits, and samples past what the fields of
// the bank can store.
format::File ReplaceDseWave(const format::File& file, const song::DseSet& set, uint32_t bank, uint32_t wave,
                            const DseIncomingWave& incoming);

// The banks and the splits that play wave `wave` of bank `bank`: a line for each bank, with the count of its splits.
std::vector<std::string> DseWaveUses(const song::DseSet& set, uint32_t bank, uint32_t wave);

// Wave `wave` of bank `bank` of `set`, a bank that contains samples: its encoding, rate, loop and size. `banks` lists
// the banks whose splits play it, by their first places in the set's banks. Throws std::invalid_argument with a message
// for the user as ReplaceDseWave does.
WaveChanges ChangesOfDseWave(const song::DseSet& set, uint32_t bank, uint32_t wave);

} // namespace hokuto::edit
