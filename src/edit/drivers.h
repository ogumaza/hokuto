// SPDX-License-Identifier: MIT

// The edits of the songs of the GBA's drivers other than MusicPlayer2000, such as Konami's (song/driver.h). The command
// layer's functions, such as EditNotes, AddTrack and Writable, pass the edits of such a file to the class of its
// driver. The file of a driver in this folder contains its class, and one row of kEditsRows in drivers.cpp names each
// driver.
//
// A new driver adds its row in kEditsRows and its class in edit/<driver>.cpp, beside its files in the other layers
// (song/driver.h lists them). The class overrides every pure virtual function. The others, such as CheckMixable and
// PlaceDrawnNote, refuse and change nothing by default, or give the answer of most drivers, and the class overrides the
// ones where the driver differs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/engine_edits.h"
#include "edit/notes.h"
#include "edit/tracks.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// A note that the Key Editor draws on a track: its tick, length, key and velocity, its instrument, and on Konami's
// driver the semitones of the sample's note that it copies (song::Note::sample_semitones).
struct DrawnKeyNote
{
    uint32_t start = 0;
    uint32_t length = 0;
    int key = 0;
    int velocity = 0;
    int bank_slot = 0;
    uint32_t program = 0;
    std::optional<int8_t> sample_semitones;
};

// The edits that follow a driver's rules. Each method takes a file of the driver, and throws std::invalid_argument with
// a message for the user for an edit that the bytes of the driver can't store. The edits of tracks come from
// EngineEdits. Placed, StartedTracksTakeSettings and IsTableEntry keep the defaults of EngineEdits.
class DriverEdits : public EngineEdits
{
public:
    virtual ~DriverEdits() = default;

    // `edited` as the driver's bytes can hold it. `edited` is an edit of sequence `sequence` of `before`.
    // `Edited::items` and `Edited::added` follow the new items.
    Edited Writable(const format::File& before, Edited edited, uint32_t sequence) const override = 0;

    // EditNotes on the driver, Writable included.
    Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                     std::span<const NewNote> additions) const override = 0;

    // ArgumentRange on the driver, by the rules of `console`.
    std::optional<ValueRange> Range(const song::Console& console, int track, const ntdlib::commands::Command& command,
                                    ntdlib::commands::Source source, bool time) const override = 0;

    // How many of the places where a loop sends a track back reach another loop without a wait (LoopsWithoutWaits).
    // `entries` contains the items where the tracks start.
    std::size_t LoopsWithoutWaits(const ntdlib::commands::Sequence& sequence,
                                  std::span<const uint32_t> entries) const override = 0;

    // Whether the tracks of `sequence` open with the note wait on: their notes then wait for their lengths. The
    // default says no.
    bool NoteWaitOpens(const ntdlib::commands::Sequence& sequence) const override;

    // The items where the entries `entries` of the table of tracks at the start of `sequence` start their tracks, in
    // their order, without an entry that starts none (song::SoundFields::entries). A driver whose song table starts the
    // tracks of its songs returns none. So does the default.
    virtual std::vector<uint32_t> EntryStarts(const ntdlib::commands::Sequence& sequence,
                                              std::span<const int> entries) const;

    // Whether `item`, an item of `sequence`, opens track `track` of a sound that plays the sequence: an entry of the
    // table of tracks at the sequence's start, or a command that starts the track. The default says no.
    bool OpensTrack(const ntdlib::commands::Sequence& sequence, const ntdlib::commands::Command& item,
                    int track) const override;

    // Whether new commands at a track's opening take a jump past them where the item before them runs into them. The
    // other track that runs into the opening then skips them. A call of Konami's driver counts its commands, and the
    // driver has no jump that a track can take. The default says no.
    bool JumpsPastOpenings() const override;

    // Whether a loop of the driver is a counted repeat: the track runs the commands from the loop's start (kLoopStart)
    // to its end (kLoopEnd) a number of times, and then runs on. A removal then deletes both the start and the end, or
    // neither. Otherwise the end of the loop sends the track back to the start for ever, like the loop of a song, and
    // the track reads no further. The end then stays (RemoveCommands). The default says no.
    virtual bool CountsRepeats() const;

    // The mixer's edits on the driver (EditMix). CheckMixable throws std::invalid_argument with a message for the user
    // where the mixer can't change `setting` of track `track` on `console`, before the check of the commands that the
    // driver runs. RefusedPan returns the message where the driver runs no pan command. NotesSetVolume says whether the
    // notes of a sample track set its volume. The mixer then edits their velocities. CheckMixEdit throws where the
    // mixer can't change `setting` of the track that ran `stretches` of `commands`. `run_empty` says whether the track
    // runs no command or note that contains the setting. IndexOpening sets the index of `command`, a new command at the
    // track's opening, at item `first` of `commands`. FollowPan adds to `command`, a pan with a new value, what else
    // the driver requires of it. The defaults refuse nothing and change nothing.
    virtual void CheckMixable(const song::Console& console, int track, song::Mix setting) const;
    virtual std::string RefusedPan(const song::DriverRules& rules) const;
    virtual bool NotesSetVolume() const;
    virtual void CheckMixEdit(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                              std::span<const song::Stretch> stretches, song::Mix setting, bool run_empty) const;
    virtual void IndexOpening(const ntdlib::commands::Sequence& commands, uint32_t first,
                              ntdlib::commands::Command& command) const;
    virtual void FollowPan(const song::Console& console, ntdlib::commands::Command& command) const;

    // The Key Editor's new notes on the driver. PlaceDrawnNote returns the note to add for `drawn` on `track`, in
    // sequence `sequence` of `file`, where the driver places it itself, or nothing to place it as on the other
    // consoles. `stretch` is the stretch of the track's commands that started at or before the note's tick, if any.
    // FirstNoteKey returns the key of the command of `drawn` on a track without notes, from the track's transpose
    // `transpose` and `key`, the drawn key without the transpose. NewNoteIndex returns the index of the command of a
    // new note on track `track`, or nothing to keep it. The defaults place nothing and change nothing.
    virtual std::optional<NewNote> PlaceDrawnNote(const format::File& file, uint32_t sequence, const song::Track& track,
                                                  const std::optional<song::Stretch>& stretch,
                                                  const DrawnKeyNote& drawn) const;
    virtual int FirstNoteKey(const format::File& file, uint32_t sequence, int track, const DrawnKeyNote& drawn,
                             int transpose, int key) const;
    virtual std::optional<uint8_t> NewNoteIndex(int track) const;

    // The edits of settings and the limits of moves (AddSettings, MoveSettings and LimitsOf). AddSettings, MoveSettings
    // and LimitsOf give the driver's result, or nothing to work as on the other consoles. ShiftOfSetting returns the
    // shift in ticks of `move`, where `rows` contains the stretches of the rows of the arrangement. FitLimits changes
    // `limits`, the limits of `notes`, where `kinds` contains the kind of the command of each one. The defaults change
    // nothing.
    virtual std::optional<Edited> AddSettings(const format::File& file, uint32_t sequence,
                                              std::span<const NewSetting> settings) const;
    virtual std::optional<Edited> MoveSettings(const format::File& file, uint32_t sequence,
                                               std::span<const SettingMove> moves) const;
    virtual std::optional<std::vector<NoteLimits>> LimitsOf(const format::File& file, uint32_t sequence,
                                                            std::span<const NotePlace> notes) const;
    virtual int32_t ShiftOfSetting(std::span<const song::Stretch> rows, const SettingMove& move) const;
    virtual void FitLimits(std::span<const NotePlace> notes, std::span<const ntdlib::commands::Kind> kinds,
                           std::span<const song::Stretch> rows, std::vector<NoteLimits>& limits) const;

    // The List Editor's edits of commands (InsertCommand and ChangeCommand). PrepareInsert throws std::invalid_argument
    // with a message for the user where `command` can't be inserted before item `at` of `commands`, sequence `sequence`
    // of `file`, and adds to it what the driver requires there. NotesHoldLengths says whether the command of a note
    // contains its length: a note edit then changes it as the Key Editor does. SampleTranspose returns the transpose of
    // a note of a sample from `before` to `command`, where the driver stores it in another form. RowTicks returns how
    // far `command` moves its track on in the rows of commands (CommandRows), in ticks. `ticks` is that count by the
    // rules of the other consoles. The defaults refuse nothing and change nothing.
    virtual void PrepareInsert(const format::File& file, uint32_t sequence, const ntdlib::commands::Sequence& commands,
                               uint32_t at, ntdlib::commands::Command& command) const;
    virtual bool NotesHoldLengths() const;
    virtual std::optional<int> SampleTranspose(const ntdlib::commands::Command& before,
                                               const ntdlib::commands::Command& command) const;
    virtual std::optional<uint32_t> RowTicks(const ntdlib::commands::Command& command,
                                             std::optional<uint32_t> ticks) const;

    // What an edit reaches (ReachOf). ListedStarts returns the items where the channels of the list at item `list` of
    // `sequence` start, by their channels, where a sound starts at such a list. NotesPlayOnNextTrack says whether a
    // note of a track can play on the next track. The defaults give no list and no such note.
    virtual std::vector<std::pair<int, uint32_t>> ListedStarts(const ntdlib::commands::Sequence& sequence,
                                                               uint32_t list) const;
    virtual bool NotesPlayOnNextTrack() const;

    // The edits of instruments (EditInstrument, EditOpeningInstrument, EditNotesWithInstrument and InstrumentSetters).
    // CheckInstruments throws std::invalid_argument with a message for the user where the driver's tracks select no
    // instrument that these edits can change. CheckTrackInstrument throws where track `track` of `console` plays no
    // instrument. CheckOpeningInstrument throws where the opening of track `track` can't select one before the track
    // has notes. RefusedBankSelect returns the message where the driver runs no bank select, or nothing for the message
    // of the Wii. SelectsProgram says whether `command`, a program change of track `track` of `console`, selects the
    // track's program. A track of -1 uses the rules of the other consoles. BankSelectsProgram says whether a bank
    // select also selects program 0 of its bank. NotesSelectSamples says whether a note that names its sample sets its
    // instrument alone. PassesTime says whether time passes at `command`, beside a wait, a note and an end.
    // ProgramIndex returns the index of a new program change of track `track` of `console`. ChannelsHoldPrograms says
    // whether a program belongs to a channel, and ProgramChannel then returns the channel of the track that ran
    // `stretches` of `commands`, or nothing. The defaults refuse nothing and follow the other consoles.
    virtual void CheckInstruments() const;
    virtual void CheckTrackInstrument(const song::Console& console, int track) const;
    virtual void CheckOpeningInstrument(const song::Console& console, int track) const;
    virtual std::optional<std::string> RefusedBankSelect() const;
    virtual bool SelectsProgram(const song::Console& console, int track,
                                const ntdlib::commands::Command& command) const;
    virtual bool BankSelectsProgram() const;
    virtual bool NotesSelectSamples() const;
    virtual bool PassesTime(const ntdlib::commands::Command& command) const;
    virtual uint8_t ProgramIndex(const song::Console& console, int track) const;
    virtual bool ChannelsHoldPrograms() const;
    virtual std::optional<uint8_t> ProgramChannel(const ntdlib::commands::Sequence& commands,
                                                  const std::vector<uint32_t>& offsets,
                                                  std::span<const song::Stretch> stretches) const;
};

// The edits of a driver whose songs keep their count of tracks. AddTrack, DuplicateTrack and PasteTrack throw
// std::invalid_argument with the message `added`, and CopyTrack with `pasted`. The driver class defines RemoveTrack.
class FixedTracksEdits : public DriverEdits
{
public:
    FixedTracksEdits(const char* added, const char* pasted);

    TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                       const song::Sequence& song, std::optional<Program> instrument) const override;
    TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override;
    CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const song::Sequence& song, int track) const override;
    TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence& song, const CopiedTrack& copied) const override;

private:
    const char* added_;
    const char* pasted_;
};

// Where sound `sound` plays, from `places` (SoundPlaces). Throws std::invalid_argument with a message for the user for
// a sound without a sequence in the file.
const SoundPlace& PlaceToEdit(std::span<const std::optional<SoundPlace>> places, uint32_t sound);

// Calls `encode`, a function that encodes commands in a driver's bytes. Throws std::invalid_argument with `prefix`, the
// message of the encoder and a full stop where the encoder throws std::invalid_argument.
template <typename Encode>
void CheckEncodes(std::string_view prefix, Encode encode)
{
    try
    {
        encode();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string(prefix) + error.what() + ".");
    }
}

// `after` as the edit of sequence `sequence` of `file`, with every item where it was.
Edited InPlace(const format::File& file, uint32_t sequence, ntdlib::commands::Sequence after);

// `edited` once Offsets can lay out sequence `sequence` of its file. Throws std::invalid_argument with the reason for
// an item that the driver's bytes can't hold.
Edited WritableByOffsets(const format::File& before, Edited edited, uint32_t sequence);

// Whether `console` runs the GBA's other driver of `engine`.
bool RunsDriver(const song::Console& console, format::Engine engine);

// The edits of the driver whose rules `rules` contains.
const DriverEdits& EditsOf(const song::DriverRules& rules);

} // namespace hokuto::edit
