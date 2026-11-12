// SPDX-License-Identifier: MIT

#include "edit/rd2.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rd2/commands.h"
#include "ntdlib/rd2/sequencer.h"

#include "edit/bytecode.h"
#include "edit/drivers.h"
#include "edit/notes.h"
#include "format/archive.h"
#include "format/drivers.h"

namespace hokuto::edit
{

namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Scale;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The longest length of a note or a wait, in ticks: a length uses 15 bits in two bytes.
constexpr int64_t kLongestLength = 0x7FFF;

// The highest key that the byte of a note can store.
constexpr uint8_t kHighestKey = 0x5F;

// How many rounds Rd2Writable runs before it gives up. Each round stores the values of at least one more note or wait
// in its bytes. In practice one or two rounds settle an edit.
constexpr int kMostRounds = 64;

// Whether the driver's reading of `read` yields the values of `wanted`, a note or a wait.
bool SameValues(const Command& wanted, const Command& read)
{
    return read.kind == wanted.kind && read.value.value == wanted.value.value &&
           (wanted.kind != Kind::kNote || read.velocity == wanted.velocity);
}

// `edited`, an edit of sequence `sequence`, with bytes that the driver reads as its items say. A note or a wait of one
// byte that would read other values from the track's last note or wait stores its values in its bytes.
Edited Rd2Writable(const format::File&, Edited edited, uint32_t sequence)
{
    const ntdlib::rd2::Revision revision = *Rd2Of(ConsoleOf(edited.file));
    Sequence changed = *edited.file.sequences[sequence];
    const bool effects = IsRd2EffectSequence(changed);
    for (int round = 0;; round++)
    {
        ntdlib::rd2::Encoded encoded;
        ntdlib::rd2::Decoded decoded;
        const auto encode = [&]
        {
            encoded = ntdlib::rd2::EncodeSequence(changed, revision);
            decoded = ntdlib::rd2::DecodeSequence(encoded.bytes, revision, effects);
        };
        CheckEncodes("Nintendo R&D2's driver can't hold this edit: ", encode);
        if (decoded.opaque)
        {
            throw std::invalid_argument(
                "Nintendo R&D2's driver would read the edited commands otherwise: " + decoded.reason + ".");
        }

        std::map<uint32_t, uint32_t> read;
        const std::vector<uint32_t> offsets = ntdlib::rd2::ItemOffsets(decoded.sequence, revision);
        for (std::size_t item = 0; item + 1 < offsets.size(); item++)
        {
            read.emplace(offsets[item], static_cast<uint32_t>(item));
        }

        bool settled = true;
        for (std::size_t item = 0; item < changed.items.size(); item++)
        {
            Command& command = changed.items[item];
            if ((command.kind != Kind::kNote && command.kind != Kind::kWait) || !command.encoding.repeated)
            {
                continue;
            }

            const auto found = read.find(encoded.offsets[item]);
            if (found == read.end() || !SameValues(command, decoded.sequence.items[found->second]))
            {
                command.encoding.repeated = false;
                command.bytes.clear();
                settled = false;
            }
        }
        if (settled)
        {
            break;
        }
        if (round == kMostRounds)
        {
            throw std::invalid_argument("Hokuto can't settle the bytes of this edit's notes and waits.");
        }
    }

    edited.file.sequences[sequence] = std::move(changed);

    return edited;
}

// The values that the driver's bytes hold in the value of `command` (ntdlib/rd2/commands.h). A note's length, a wait
// and a tempo of the revisions that give it a length take 15 bits. A pan, a bend and a transpose take a byte about the
// zero of their scale, and the flags of a tie and the note wait 0 or 1. The other commands take a byte. A jump, a call,
// a track start, a return, an end and a portamento's end hold no value. No command takes a time, a random value or a
// variable.
std::optional<ValueRange> Rd2Range(const Command& command, Source source, bool time, ntdlib::rd2::Revision revision)
{
    if (time || source != Source::kValue)
    {
        return std::nullopt;
    }

    int64_t lowest = 0;
    int64_t highest = 0xFF;
    switch (command.kind)
    {
    case Kind::kJump:
    case Kind::kCall:
    case Kind::kOpenTrack:
    case Kind::kReturn:
    case Kind::kEnd:
    case Kind::kPortamento:
    case Kind::kOther:
    case Kind::kData:
        return std::nullopt;
    case Kind::kNote:
    case Kind::kWait:
        highest = kLongestLength;
        break;
    case Kind::kTempo:
        highest = ntdlib::rd2::TraitsOf(revision).byte_tempo ? 0xFF : kLongestLength;
        break;
    case Kind::kPan:
        lowest = -64;
        highest = 0xFF - 64;
        break;
    case Kind::kPitchBend:
    case Kind::kTranspose:
        lowest = -0x80;
        highest = 0x7F;
        break;
    case Kind::kTie:
    case Kind::kNoteWait:
        highest = 1;
        break;
    default:
        break;
    }

    const Scale scale = ntdlib::rd2::ScaleOf(command.kind);
    const auto unit = [&scale](int64_t value)
    {
        return ntdlib::commands::ToUnit(value, scale);
    };

    return ValueRange{unit(lowest), unit(highest)};
}

// The edits on R&D2's driver. Where the defaults of DriverEdits apply:
// - The driver's bytes keep nothing else around an edit.
// - The driver has no loops.
class Rd2DriverEdits final : public DriverEdits
{
public:
    Edited Writable(const format::File& before, Edited edited, uint32_t sequence) const override
    {
        return Rd2Writable(before, std::move(edited), sequence);
    }

    Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                     std::span<const NewNote> additions) const override
    {
        for (const NewNote& addition : additions)
        {
            if (addition.key < 0 || addition.key > kHighestKey)
            {
                throw std::invalid_argument("A note of Nintendo R&D2's driver plays a key from 0 to " +
                                            std::to_string(kHighestKey) + ".");
            }
            if (addition.length > kLongestLength)
            {
                throw std::invalid_argument("A note of Nintendo R&D2's driver lasts " + std::to_string(kLongestLength) +
                                            " ticks at most.");
            }
        }

        return EditNotesWithLengths(file, sequence, edits, additions);
    }

    std::optional<ValueRange> Range(const song::Console& console, int /*track*/, const Command& command, Source source,
                                    bool time) const override
    {
        return Rd2Range(command, source, time, static_cast<ntdlib::rd2::Revision>(console.driver->Revision()));
    }

    // As on MusicPlayer2000, the tracks loop through jumps. A note waits only with the note wait on.
    std::size_t LoopsWithoutWaits(const Sequence& sequence, std::span<const uint32_t> entries) const override
    {
        return CommandsWithoutWayOut(sequence, song::Console{}, entries);
    }

    // A sound effect's track opens with the note wait on (ntdlib's sequencer), and a song's with it off.
    bool NoteWaitOpens(const Sequence& sequence) const override
    {
        return IsRd2EffectSequence(sequence);
    }

    // A song's table follows the count of its tracks, and a sound effects' sequence starts with its table. An entry
    // of 0 starts no track.
    std::vector<uint32_t> EntryStarts(const Sequence& sequence, std::span<const int> entries) const override
    {
        std::vector<uint32_t> starts;
        for (std::size_t item = IsRd2EffectSequence(sequence) ? 0 : 1; item < sequence.items.size(); item++)
        {
            const Command& entry = sequence.items[item];
            if (entry.kind != Kind::kOpenTrack || entry.encoding.opcode != ntdlib::rd2::kHeaderEntry)
            {
                break;
            }

            if (!entry.target_outside && std::ranges::find(entries, entry.index) != entries.end())
            {
                starts.push_back(entry.target);
            }
        }

        return starts;
    }

    // A song's table opens each track, and F8 opens one. A sound effect plays one track, track 0, from any entry of the
    // table of its sequence.
    bool OpensTrack(const Sequence& sequence, const Command& item, int track) const override
    {
        return item.kind == Kind::kOpenTrack &&
               (item.index == track ||
                (track == 0 && item.encoding.opcode == ntdlib::rd2::kHeaderEntry && IsRd2EffectSequence(sequence)));
    }

    // F8 copies the settings of the starting track to the track that it starts (ntdlib's sequencer).
    bool StartedTracksTakeSettings() const override
    {
        return true;
    }

    // A sound effect's track can run on into the next effect's track (command F0 jumps).
    bool JumpsPastOpenings() const override
    {
        return true;
    }

    bool IsTableEntry(const Command& item) const override
    {
        return item.kind == Kind::kOpenTrack && item.encoding.opcode == ntdlib::rd2::kHeaderEntry;
    }

    TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                       const song::Sequence& song, std::optional<Program> instrument) const override
    {
        return AddRd2Track(file, places, sound, song, instrument);
    }

    TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        return DuplicateRd2Track(file, places, sound, track);
    }

    CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const song::Sequence&, int track) const override
    {
        return CopyRd2Track(file, places, sound, track);
    }

    TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence&, const CopiedTrack& copied) const override
    {
        return PasteRd2Track(file, places, sound, copied);
    }

    format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        return RemoveRd2Track(file, places, sound, track);
    }

    // A note of a square or of the wave shows at the pitch that its region sets (Rd2CommandKey).
    int FirstNoteKey(const format::File& file, uint32_t sequence, int /*track*/, const DrawnKeyNote& drawn,
                     int transpose, int key) const override
    {
        return Rd2CommandKey(file, sequence, drawn.bank_slot, drawn.program, transpose, drawn.key).value_or(key);
    }

    // A bank select also selects the first instrument of its bank, program 0 (ntdlib's sequencer, SetBank).
    bool BankSelectsProgram() const override
    {
        return true;
    }
};

// The ROM that a file builds, with the driver's tables.
struct BuiltRom
{
    ntdlib::rd2::Rom rom;
    ntdlib::rd2::DriverInfo info;
};

// The ROM that `file` builds. Throws std::invalid_argument with a message for the user when Hokuto can't read the
// driver's tables there.
BuiltRom BuiltRomOf(const format::File& file)
{
    BuiltRom built;
    built.rom.Assign(format::ExportArchive(file));
    std::string error;
    if (!ntdlib::rd2::DetectDriver(built.rom, {}, built.info, error))
    {
        throw std::invalid_argument("Hokuto can't read the driver's tables of the ROM that the file builds.");
    }

    return built;
}

// The address of the list of banks of sequence `sequence` of `file` in `built`. Throws std::invalid_argument with a
// message for the user when Hokuto can't find the list.
uint32_t BankListOf(const BuiltRom& built, const format::File& file, uint32_t sequence)
{
    const ntdlib::rd2::DriverInfo& info = built.info;
    const auto music = static_cast<int>(info.sequence_addresses.size());
    for (int number = 0; number < ntdlib::rd2::SequenceCount(info); number++)
    {
        const bool effects = ntdlib::rd2::IsEffectSequence(info, number);
        const uint32_t entry = effects ? info.effects + 4 * static_cast<uint32_t>(number - music)
                                       : info.sequences + 4 * static_cast<uint32_t>(number);
        if (format::SequenceNamedAt(file, entry - ntdlib::agb::kRomBase) == sequence)
        {
            return effects ? info.effect_bank_list_addresses[static_cast<std::size_t>(number - music)]
                           : info.bank_list_addresses[static_cast<std::size_t>(number)];
        }
    }

    throw std::invalid_argument("Hokuto finds no list of banks of the song.");
}

// The key that ntdlib's converter assigns to a note of `note`, the driver's note after the track's transpose, on
// instrument `instrument` of the driver's bank `bank`, or nothing when the bank has no region for the note. The notes
// of a square and of the wave get the pitch that the tuning of the region sets. The sample of a square plays middle C
// on key 60, and the sample of the wave plays C3 on key 48. A drum or an instrument with a sample for each key plays
// the region's pitch. A sample's note and the noise's keep the note.
std::optional<int> ShownKeyOf(const BuiltRom& built, uint16_t bank, uint16_t instrument, uint8_t note)
{
    const ntdlib::rd2::Lookup lookup = ntdlib::rd2::LookUpInstrument(built.rom, built.info, bank, instrument, note);
    if (!lookup.found || lookup.missing)
    {
        return std::nullopt;
    }

    const uint8_t type = ntdlib::rd2::RegionType(built.rom, lookup.region);
    const bool fixed = lookup.drum || lookup.key_sample;
    const int index = ntdlib::rd2::PitchIndex(fixed ? ntdlib::rd2::kFixedNote : note,
                                              ntdlib::rd2::RegionTuning(built.rom, lookup.region));
    switch (type)
    {
    case ntdlib::rd2::kSquare1Type:
    case ntdlib::rd2::kSquare2Type:
        return index + 12;
    case ntdlib::rd2::kWaveType:
        return index;
    default:
        return note;
    }
}

// The bank in slot `slot` of the list of banks of sequence `sequence` of `file`, or nothing past the ROM's end.
std::optional<uint16_t> SlotBankOf(const BuiltRom& built, const format::File& file, uint32_t sequence, int slot)
{
    const uint32_t place = BankListOf(built, file, sequence) + 2 * static_cast<uint32_t>(std::max(slot, 0));
    if (!built.rom.Contains(place, 2))
    {
        return std::nullopt;
    }

    return built.rom.U16(place);
}

// Of the keys of a note command on instrument `instrument` of bank `bank`, on a track that transposes its notes by
// `transpose`, the key whose note shows at `shown` and is nearest `shown` less the transpose. Nothing when no key shows
// there. The driver adds the transpose to the command's key in a byte (ntdlib's sequencer).
std::optional<int> NearestKey(const BuiltRom& built, uint16_t bank, uint16_t instrument, int transpose, int shown)
{
    const int plain = shown - transpose;
    std::optional<int> nearest;
    for (int key = 0; key < 128; key++)
    {
        const auto note = static_cast<uint8_t>(key + transpose);
        const std::optional<int> key_shown = ShownKeyOf(built, bank, instrument, note);
        if (key_shown == shown && (!nearest || std::abs(key - plain) < std::abs(*nearest - plain)))
        {
            nearest = key;
        }
    }

    return nearest;
}

} // namespace

const DriverEdits& Rd2Edits()
{
    static const Rd2DriverEdits edits;
    return edits;
}

std::optional<ntdlib::rd2::Revision> Rd2Of(const song::Console& console)
{
    if (!RunsDriver(console, format::Engine::kRd2))
    {
        return std::nullopt;
    }

    return static_cast<ntdlib::rd2::Revision>(console.driver->Revision());
}

bool IsRd2EffectSequence(const Sequence& sequence)
{
    return !sequence.items.empty() && sequence.items[0].kind == Kind::kOpenTrack &&
           sequence.items[0].encoding.opcode == ntdlib::rd2::kHeaderEntry;
}

std::vector<uint32_t> Rd2BanksOf(const format::File& file, uint32_t sequence, int last)
{
    const BuiltRom built = BuiltRomOf(file);
    const uint32_t list = BankListOf(built, file, sequence);
    std::vector<uint32_t> banks;
    for (int slot = 0; slot <= last; slot++)
    {
        const uint32_t place = list + 2 * static_cast<uint32_t>(slot);
        banks.push_back(built.rom.Contains(place, 2) ? built.rom.U16(place) : 0xFFFFFFFF);
    }

    return banks;
}

std::optional<int> Rd2CommandKey(const format::File& file, uint32_t sequence, int slot, uint32_t program, int transpose,
                                 int shown)
{
    const BuiltRom built = BuiltRomOf(file);
    const std::optional<uint16_t> bank = SlotBankOf(built, file, sequence, slot);
    if (!bank || program > 0xFFFF)
    {
        return std::nullopt;
    }

    return NearestKey(built, *bank, static_cast<uint16_t>(program), transpose, shown);
}

std::optional<uint8_t> Rd2VoiceTypeOf(const format::File& file, uint32_t sequence, int slot, uint32_t program,
                                      int shown)
{
    const BuiltRom built = BuiltRomOf(file);
    const std::optional<uint16_t> bank = SlotBankOf(built, file, sequence, slot);
    if (!bank || program > 0xFFFF)
    {
        return std::nullopt;
    }

    const std::optional<int> key = NearestKey(built, *bank, static_cast<uint16_t>(program), 0, shown);
    if (!key)
    {
        return std::nullopt;
    }

    const ntdlib::rd2::Lookup lookup = ntdlib::rd2::LookUpInstrument(
        built.rom, built.info, *bank, static_cast<uint16_t>(program), static_cast<uint8_t>(*key));
    return lookup.region != 0 ? ntdlib::rd2::RegionType(built.rom, lookup.region) : ntdlib::rd2::kSampleType;
}

} // namespace hokuto::edit
