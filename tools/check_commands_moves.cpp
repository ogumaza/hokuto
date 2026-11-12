// SPDX-License-Identifier: MIT

// The round of hokuto_check_commands that moves the settings of the rows, and the helpers for Krawall's rows that it
// uses.

#include "check_commands.h"

namespace hokuto::tools::commands
{

namespace
{

// On Krawall, the rows from `stretch` among `rows` to the row that plays at `stretch`'s tick and `shift`: the last row
// that starts by then. Nothing where `stretch` is missing from `rows`.
std::optional<int64_t> KrawallRowsTo(std::span<const hokuto::song::Stretch> rows, const hokuto::song::Stretch& stretch,
                                     int64_t shift)
{
    const auto from = std::find(rows.begin(), rows.end(), stretch);
    if (from == rows.end())
    {
        return std::nullopt;
    }

    const int64_t tick = int64_t{stretch.tick} + shift;
    auto to = from;
    while (shift > 0 && to + 1 != rows.end() && int64_t{(to + 1)->tick} <= tick)
    {
        ++to;
    }
    while (shift < 0 && to != rows.begin() && int64_t{to->tick} > tick)
    {
        --to;
    }

    return to - from;
}

// On Krawall, the tick of the row `count` rows on from row `from` of `rows`, where each row between follows on from the
// row before it in its pattern. Nothing where the rows don't follow on.
std::optional<uint32_t> KrawallTickOfRow(std::span<const hokuto::song::Stretch> rows, std::size_t from, int64_t count)
{
    const int64_t to = static_cast<int64_t>(from) + count;
    if (to < 0 || to >= static_cast<int64_t>(rows.size()))
    {
        return std::nullopt;
    }
    for (int64_t row = std::min<int64_t>(static_cast<int64_t>(from), to) + 1;
         row <= std::max<int64_t>(static_cast<int64_t>(from), to); ++row)
    {
        if (rows[static_cast<std::size_t>(row)].entry != ntdlib::sequence_export::Entry::kOpen)
        {
            return std::nullopt;
        }
    }

    return rows[static_cast<std::size_t>(to)].tick;
}

// On Krawall, the field of a row's entry that `command` fills: 0 for the note or the instrument, 1 for the volume and 2
// for the effect. Nothing for an item that no entry contains, such as a wait.
std::optional<int> KrawallField(const Command& command)
{
    switch (command.kind)
    {
    case Kind::kNote:
    case Kind::kProgram:
    case Kind::kNoteOff:
        return 0;
    case Kind::kVolume:
        return 1;
    case Kind::kOther:
        if (command.encoding.opcode >= ntdlib::krawall::kEffectOpcode &&
            command.encoding.opcode < ntdlib::krawall::kEffectOpcode + 0x100)
        {
            return 2;
        }
        break;
    default:
        break;
    }

    return std::nullopt;
}

// On Krawall, whether the channel of item `item` of `commands` has one entry in the item's row: the channel's items of
// the row are adjacent, in the order note, volume and effect.
bool OneEntry(const ntdlib::commands::Sequence& commands, uint32_t item)
{
    uint32_t begin = item;
    uint32_t end = item + 1;
    while (begin > 0 && KrawallField(commands.items[begin - 1]))
    {
        --begin;
    }
    while (end < commands.items.size() && KrawallField(commands.items[end]))
    {
        ++end;
    }

    const uint8_t channel = commands.items[item].index;
    std::optional<uint32_t> last;
    for (uint32_t at = begin; at < end; ++at)
    {
        if (commands.items[at].index != channel)
        {
            continue;
        }
        if (last && (*last + 1 != at || *KrawallField(commands.items[*last]) >= *KrawallField(commands.items[at])))
        {
            return false;
        }
        last = at;
    }

    return true;
}

// On Krawall, the move of the setting at `place` half as many rows as `range` allows, later or earlier, among `rows`
// (song::Track::stretches). Only the rows that follow on from the setting's row count. The move targets the tick of a
// row whose entry of the setting's channel has no item of the setting's field, and that no other move of the sequence
// uses for that channel and field (`targets`). The row nearest the half way wins. Nothing where the range has no such
// row.
std::optional<int32_t> KrawallMove(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                                   std::span<const hokuto::song::Stretch> rows, const hokuto::edit::NotePlace& place,
                                   const hokuto::edit::ShiftRange& range, bool later,
                                   std::set<std::tuple<uint32_t, uint8_t, int>>& targets)
{
    const auto from = std::find(rows.begin(), rows.end(), place.stretch);
    const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, place.command);
    if (from == rows.end() || !item || !KrawallField(commands.items.at(*item)))
    {
        return std::nullopt;
    }
    const Command& setting = commands.items[*item];
    const int field = *KrawallField(setting);

    // The rows of the run in the move's direction, nearest first.
    std::vector<const hokuto::song::Stretch*> run;
    if (later)
    {
        for (auto row = from + 1; row != rows.end() && row->entry == ntdlib::sequence_export::Entry::kOpen &&
                                  int64_t{row->tick} - from->tick <= int64_t{range.later};
             ++row)
        {
            run.push_back(&*row);
        }
    }
    else
    {
        for (auto row = from; row != rows.begin() && row->entry == ntdlib::sequence_export::Entry::kOpen; --row)
        {
            if (int64_t{from->tick} - (row - 1)->tick > int64_t{range.earlier})
            {
                break;
            }
            run.push_back(&*(row - 1));
        }
    }

    // From the row half way, outwards.
    const int64_t half = (static_cast<int64_t>(run.size()) + 1) / 2 - 1;
    for (int64_t step = 0; step < 2 * static_cast<int64_t>(run.size()); ++step)
    {
        const int64_t at = half + (step % 2 == 0 ? step / 2 : -(step + 1) / 2);
        if (at < 0 || at >= static_cast<int64_t>(run.size()))
        {
            continue;
        }

        const hokuto::song::Stretch& row = *run[static_cast<std::size_t>(at)];
        const std::optional<uint32_t> first = hokuto::edit::ItemAtOffset(offsets, row.first);
        const std::optional<uint32_t> last = hokuto::edit::ItemAtOffset(offsets, row.last);
        if (!first || !last)
        {
            continue;
        }
        bool held = false;
        for (uint32_t other = *first; other <= *last && other < commands.items.size(); ++other)
        {
            held =
                held || (commands.items[other].index == setting.index && KrawallField(commands.items[other]) == field);
        }
        if (!held && targets.emplace(row.first, setting.index, field).second)
        {
            return static_cast<int32_t>(int64_t{row.tick} - from->tick);
        }
    }

    return std::nullopt;
}

// Whether a move of `command` can change the notes that a sound plays: a transpose changes their keys, and a velocity
// range their velocities. A program change and a bank select change their instrument, and a note without a region in
// the instrument doesn't sound. A tie and a mute change which notes start. A note-off ends MusicPlayer2000's tied note:
// it changes how long the note keeps its channel, and a later note can find no channel. With `voices`, on
// MusicPlayer2000 and on Nintendo R&D2's driver, a priority decides which notes take the voices too. A tempo moves the
// frames that random draws follow, and a random value changes the numbers that later draws give. With `dungeon`, the
// Dungeon Dice Monsters revision of Konami's driver: a volume there starts the sound of a note again, and a release
// ends one. In the Eternal Duelist's layout of that driver, a volume of index 2 sets the next track's volume too. A
// note at a volume of 0 doesn't sound there. With `vibrato`, a vibrato's depth decides how a note starts: in the
// Eternal Duelist revision, the flag of the vibrato replaces the start of a note, and in Dungeon Dice Monsters, the
// vibrato moves the key that a note's start reads.
bool ChangesNotes(const Command& command, bool voices, bool dungeon, bool vibrato)
{
    const bool random =
        command.value.source == Source::kRandom || (command.time && command.time->source == Source::kRandom);
    return random || command.kind == Kind::kTranspose || command.kind == Kind::kVelocityRange ||
           command.kind == Kind::kProgram || command.kind == Kind::kBankSelect || command.kind == Kind::kTie ||
           command.kind == Kind::kNoteOff || command.kind == Kind::kMute || command.kind == Kind::kTempo ||
           (voices && command.kind == Kind::kPriority) ||
           (dungeon && (command.kind == Kind::kVolume || command.kind == Kind::kRelease)) ||
           (command.kind == Kind::kVolume && command.index == 2) || (vibrato && command.kind == Kind::kLfoDepth);
}

// On Krawall, whether a move of `command` can change the notes or the rows' ticks: an effect that sets the speed or the
// tempo, or sends the rows elsewhere, or one that starts a note again, delays it, cuts it or slides to it in place of
// starting it. The numbers are those of ntdlib's player (ntdlib/src/krawall/player.cpp).
bool KrawallChangesNotes(const Command& command)
{
    if (KrawallField(command) != 2)
    {
        return false;
    }

    switch (command.encoding.opcode - ntdlib::krawall::kEffectOpcode)
    {
    case 1:  // the speed
    case 2:  // the tempo
    case 3:  // the speed and the tempo
    case 4:  // a jump
    case 5:  // a break
    case 19: // a slide to a note
    case 24: // a volume slide with a slide to a note
    case 27: // an offset
    case 29: // a retrigger
    case 43: // a loop
    case 44: // a note cut
    case 45: // a note delay
        return true;
    default:
        return false;
    }
}

// Whether `command` can turn on the tie or the monophonic mode. A note then can continue the channel of the note before
// it, and a setting that moves between them can change whether the console continues the channel or starts another.
bool Legato(const Command& command)
{
    const bool off = command.value.source == Source::kValue && command.value.value == 0;
    return (command.kind == Kind::kTie || command.kind == Kind::kMonophonic) && !off;
}

// The commands of `rows` that set a volume of 0, or a volume where the volume was 0. A GBA track starts at volume 0,
// and a note at volume 0 doesn't sound. A move of such a command can therefore change which notes sound. A command that
// runs more than once counts when any run does. `commands` and `offsets` hold the sequence of the rows.
std::set<uint32_t> Silencing(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                             std::span<const hokuto::edit::CommandRow> rows)
{
    std::set<uint32_t> silencing;
    bool silent = true;
    for (const hokuto::edit::CommandRow& row : rows)
    {
        const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, row.command);
        if (!item || commands.items.at(*item).kind != Kind::kVolume)
        {
            continue;
        }

        const ntdlib::commands::Argument& volume = commands.items.at(*item).value;
        const bool zero = volume.source == Source::kValue && volume.value == 0;
        if (silent || zero)
        {
            silencing.insert(row.command);
        }
        silent = zero;
    }

    return silencing;
}

// The seventh round's moves on the tracks of `song`, the first sound of sequence `sequence` of `file`. Each track moves
// up to kMostMoves commands that MoveSettings can move, spread over the commands that it ran, each once. A command
// whose move can change the notes stays out (ChangesNotes), and so does a track that runs a command of Legato. On the
// GBA, so does a volume that Silencing returns. On Krawall, so does an effect for which KrawallChangesNotes is true.
// Each moves half as far as LimitsOf allows: later and earlier in turn, or the other way where it has no room. On
// Krawall, each moves to a row whose entry of its channel has no item of its field (KrawallMove). `levels` receives the
// tracks whose volume or pan moves.
std::vector<hokuto::edit::SettingMove> MovesOf(const File& file, uint32_t sequence, const hokuto::song::Sequence& song,
                                               std::set<int>& levels)
{
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    const bool gba = hokuto::edit::ConsoleOf(file).engine == hokuto::format::Engine::kGba;
    const bool dungeon =
        hokuto::edit::KonamiOf(hokuto::edit::ConsoleOf(file)) == ntdlib::konami::Revision::kDungeonDiceMonsters;
    const bool vibrato =
        dungeon || hokuto::edit::KonamiOf(hokuto::edit::ConsoleOf(file)) == ntdlib::konami::Revision::kEternalDuelist;
    const bool voices = gba || hokuto::edit::Rd2Of(hokuto::edit::ConsoleOf(file)).has_value();
    const bool krawall = hokuto::edit::IsKrawall(hokuto::edit::ConsoleOf(file));
    std::vector<hokuto::edit::SettingMove> moves;
    std::set<uint32_t> taken;
    std::set<std::tuple<uint32_t, uint8_t, int>> targets; // on Krawall, the rows, channels and fields that moves take
    for (const hokuto::song::Track& track : song.tracks)
    {
        const std::vector<hokuto::edit::CommandRow> rows = RowsOf(file, sequence, song, track.number);
        const auto legato = [&commands, &offsets](const hokuto::edit::CommandRow& row)
        {
            const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, row.command);
            return item && Legato(commands.items.at(*item));
        };
        if (std::any_of(rows.begin(), rows.end(), legato))
        {
            continue;
        }

        const std::set<uint32_t> silencing = gba ? Silencing(commands, offsets, rows) : std::set<uint32_t>();
        std::vector<hokuto::edit::NotePlace> places;
        for (const hokuto::edit::CommandRow& row : rows)
        {
            const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, row.command);
            if (row.tick && item && !hokuto::edit::WhyFixedInTime(commands.items.at(*item)) &&
                !ChangesNotes(commands.items.at(*item), voices, dungeon, vibrato) &&
                !(krawall && KrawallChangesNotes(commands.items.at(*item))) && !silencing.contains(row.command) &&
                taken.insert(row.command).second)
            {
                places.push_back({.command = row.command, .stretch = track.stretches.at(row.stretch)});
            }
        }
        if (places.empty())
        {
            continue;
        }

        std::vector<hokuto::edit::NotePlace> chosen;
        for (const std::size_t index : SpreadIndices(places.size(), kMostMoves))
        {
            chosen.push_back(places[index]);
        }
        const std::vector<hokuto::edit::NoteLimits> limits =
            hokuto::edit::LimitsOf(file, sequence, chosen, std::nullopt, track.stretches);
        for (std::size_t i = 0; i < chosen.size(); ++i)
        {
            const std::optional<hokuto::edit::ShiftRange>& range = limits[i].shift;
            if (!range || (range->earlier == 0 && range->later == 0))
            {
                continue;
            }

            const bool later = range->earlier == 0 || (range->later > 0 && moves.size() % 2 == 0);
            int64_t shift = later ? (int64_t{range->later} + 1) / 2 : -(int64_t{range->earlier} + 1) / 2;
            if (krawall)
            {
                const std::optional<int32_t> row_shift =
                    KrawallMove(commands, offsets, track.stretches, chosen[i], *range, later, targets);
                if (!row_shift)
                {
                    continue;
                }
                shift = *row_shift;
            }
            moves.push_back({chosen[i].command, chosen[i].stretch, static_cast<int32_t>(shift)});
            const Kind kind = hokuto::edit::CommandAt(file, sequence, chosen[i].command).kind;
            if (kind == Kind::kVolume || kind == Kind::kPan)
            {
                levels.insert(track.number);
            }
        }
    }

    return moves;
}

} // namespace

std::size_t CheckMovedRows(const std::string& name, const File& file, const File& edited, uint32_t sequence,
                           const hokuto::song::Sequence& before, const hokuto::song::Sequence& after,
                           const Moved& moved)
{
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    const bool krawall = hokuto::edit::IsKrawall(console);
    const ntdlib::commands::Sequence& old_commands = *file.sequences[sequence];
    const std::vector<uint32_t> old_offsets = hokuto::edit::Offsets(old_commands, console);
    const ntdlib::commands::Sequence& commands = *edited.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, console);
    const uint32_t end = after.loop ? after.loop->end : std::numeric_limits<uint32_t>::max();
    std::size_t problems = 0;
    for (const hokuto::song::Track& track : before.tracks)
    {
        const std::vector<hokuto::edit::CommandRow> rows = RowsOf(edited, sequence, after, track.number);
        std::multiset<std::pair<uint32_t, uint32_t>> ran;
        for (const hokuto::edit::CommandRow& row : rows)
        {
            if (row.tick)
            {
                ran.emplace(row.command, *row.tick);
            }
        }
        for (const hokuto::edit::CommandRow& row : RowsOf(file, sequence, before, track.number))
        {
            const std::optional<uint32_t> old_item = hokuto::edit::ItemAtOffset(old_offsets, row.command);
            if (!row.tick || !old_item || old_commands.items.at(*old_item).kind == Kind::kWait)
            {
                continue;
            }

            // On Krawall, a moved command runs as many rows on in each pass of its row.
            const auto shift = moved.shifts.find(row.command);
            int64_t tick = int64_t{*row.tick} + (shift != moved.shifts.end() ? shift->second : 0);
            if (const auto rows_on = moved.rows.find(row.command); rows_on != moved.rows.end())
            {
                const std::optional<uint32_t> moved_tick =
                    KrawallTickOfRow(track.stretches, row.stretch, rows_on->second);
                if (!moved_tick)
                {
                    continue;
                }
                tick = *moved_tick;
            }
            const std::optional<uint32_t> item = moved.items.at(*old_item);
            if (*row.tick >= end || tick >= end || tick >= after.length || !item)
            {
                continue;
            }

            const uint32_t offset = offsets.at(*item);
            const auto found = ran.find({offset, static_cast<uint32_t>(tick)});
            if (found == ran.end())
            {
                std::printf("%s: sequence %u: the command at %u of track %d doesn't run at tick %lld after the moves\n",
                            name.c_str(), sequence, row.command, track.number, static_cast<long long>(tick));
                ++problems;
                break;
            }
            ran.erase(found);
            if (shift == moved.shifts.end())
            {
                continue;
            }

            // On Krawall, a moved command joins the entry of its channel in its new row.
            if (krawall)
            {
                if (!OneEntry(commands, *item))
                {
                    std::printf(
                        "%s: sequence %u: the moved command at %u of track %d makes a second entry of its "
                        "channel at tick %lld\n",
                        name.c_str(), sequence, row.command, track.number, static_cast<long long>(tick));
                    ++problems;
                }
                continue;
            }

            // A moved command runs before the notes that its stretch starts at its new tick.
            const auto at = [offset, tick](const hokuto::edit::CommandRow& other)
            {
                return other.command == offset && other.tick == static_cast<uint32_t>(tick);
            };
            const auto run = std::find_if(rows.begin(), rows.end(), at);
            for (auto earlier = rows.begin(); earlier != run; ++earlier)
            {
                const std::optional<uint32_t> other = hokuto::edit::ItemAtOffset(offsets, earlier->command);
                if (earlier->stretch == run->stretch && earlier->tick == run->tick && other &&
                    commands.items.at(*other).kind == Kind::kNote)
                {
                    std::printf("%s: sequence %u: the moved command at %u of track %d runs after a note at tick %lld\n",
                                name.c_str(), sequence, row.command, track.number, static_cast<long long>(tick));
                    ++problems;
                    break;
                }
            }
        }
    }

    return problems;
}

namespace
{

// Tries `move` of a setting of `sequence` on its own, with the sound's `rows`. A move that the edit accepts is added to
// `moves`, with its shift and, on Krawall, its rows in `place`. A refusal counts in `totals`.
void TrySettingMove(const std::string& name, const File& file, const CheckedArchive& archive, uint32_t sequence,
                    const hokuto::edit::SettingMove& move, std::span<const hokuto::song::Stretch> rows,
                    std::vector<hokuto::edit::SettingMove>& moves, Moved& place, Totals& totals)
{
    try
    {
        hokuto::edit::MoveSettings(file, sequence, {&move, 1}, rows);
        moves.push_back(move);
        place.shifts.emplace(move.command, move.shift);
        if (archive.Krawall())
        {
            if (const std::optional<int64_t> count = KrawallRowsTo(rows, move.stretch, move.shift))
            {
                place.rows.emplace(move.command, *count);
            }
        }
    }
    catch (const std::invalid_argument& error)
    {
        // On Konami's driver, the last command of a call decides how long the call plays. The delay after the
        // call makes up for a move, unless other code enters that delay too (edit::KonamiKeepCallLengths).
        if (archive.Konami() && std::string_view(error.what()).find("how long a call") != std::string_view::npos)
        {
            ++totals.move_calls;
            return;
        }
        // A moved volume command of Konami's driver mustn't start the note of another note command
        // (edit::CheckKonamiVolumeStarts). The limits of a move don't include that check.
        const std::string_view why = error.what();
        if (archive.Konami() && why.find("start another note again") != std::string_view::npos)
        {
            ++totals.move_starts;
            return;
        }
        // The same check refuses every move that changes the order or the waits of the commands in a song whose loop
        // cuts a track short.
        if (archive.Konami() && why.find("loops before one of its tracks") != std::string_view::npos)
        {
            ++totals.move_cut_songs;
            return;
        }
        std::printf("%s: sequence %u: the command at %u can't move by %d ticks inside its limits: %s\n", name.c_str(),
                    sequence, move.command, move.shift, error.what());
        ++totals.problems;
    }
}

// Checks that each sound of the sequences in `moved` plays its notes again from `edited`, the file with the moves, as
// it played `songs` from `file`.
void CheckMovedSounds(const std::string& name, const File& file, const File& edited, const CheckedArchive& archive,
                      const std::vector<std::optional<uint32_t>>& sequences,
                      const std::map<uint32_t, hokuto::song::Sequence>& songs, const std::map<uint32_t, Moved>& moved,
                      Totals& totals)
{
    // Each sound of those sequences plays its notes again from the archive with the moves.
    CheckedArchive after;
    try
    {
        hokuto::tools::CheckSaved(edited);
        after = hokuto::tools::BuildChecked(edited, name);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file with the moved settings can't be exported: %s\n", name.c_str(), error.what());
        ++totals.problems;
        return;
    }
    for (const auto& [sound, before] : songs)
    {
        const CheckedSound info = archive.Info(sound);
        const uint32_t sequence = *sequences[info.file_id];
        const auto place = moved.find(sequence);
        if (place == moved.end())
        {
            continue;
        }

        ++totals.move_sounds;
        try
        {
            const hokuto::song::Sequence read = after.Read(sound);
            totals.move_notes += NotesOf(before).size();
            bool loop_moved = false;
            if (!SamePlaying(before, read, loop_moved) && WaitsForNotes(file, sequence, before))
            {
                ++totals.move_waits;
                continue;
            }
            if (!SamePlaying(before, read, loop_moved) && archive.dse && ReadsData(file, sequence, before))
            {
                ++totals.move_past_end;
                continue;
            }
            if (!SamePlaying(before, read, loop_moved))
            {
                // As in the fifth round, a volume or a pan that moves on the GBA changes the level of a PSG channel's
                // notes.
                // As in the fifth round, a setting that moves on the DS changes which channel is the quietest.
                // On Konami's driver, a pan decides which PSG channel plays a note (tools::KonamiPsgNotesApart). On
                // Nintendo R&D2's driver, a PSG note ends once its level falls to 0 (tools::Rd2PsgNotesApart). On
                // Brownie Brown's driver, a volume or a pan can silence a PSG note (tools::BrowniePsgNotesApart).
                const hokuto::tools::NotesApart psg =
                    archive.gba         ? hokuto::tools::PsgNotesApart(*archive.gba, sound, place->second.levels)
                    : archive.Konami()  ? hokuto::tools::KonamiPsgNotesApart(archive.Konami()->info.revision)
                    : archive.Rd2()     ? hokuto::tools::Rd2PsgNotesApart(*archive.Rd2(), sound, place->second.levels)
                    : archive.Brownie() ? hokuto::tools::BrowniePsgNotesApart(place->second.levels)
                                        : hokuto::tools::NotesApart();
                if (archive.ds && SameNoteCommands(before, read, loop_moved))
                {
                    ++totals.move_channels;
                }
                else if (archive.Rare() && FramesDecide(before, read, loop_moved, totals.frames_alone))
                {
                    ++totals.move_frames;
                }
                else if (archive.Konami() && (!psg || !SamePlaying(before, read, loop_moved, 0, psg)) &&
                         CutByVolume(edited, sequence, before, read, psg))
                {
                    ++totals.move_cuts;
                }
                else if (psg && SamePlaying(before, read, loop_moved, 0, psg))
                {
                    ++totals.move_psg;
                }
                else if (const std::optional<RareRepeats> rare = RareRepeatsOf(archive, after, sound, before, read);
                         SameOverLoops(before, read, psg, rare ? &*rare : nullptr, &totals.frames_alone))
                {
                    ++totals.move_repeats;
                    loop_moved = false;
                }
                else
                {
                    std::printf("%s: %s plays other notes after the moves: %zu notes before, %zu after\n", name.c_str(),
                                info.name.c_str(), NotesOf(before).size(), NotesOf(read).size());
                    ++totals.problems;
                    continue;
                }
            }

            totals.move_loops += loop_moved && !PartPass(before, read) ? 1 : 0;
            totals.move_part_passes += PartPass(before, read) ? 1 : 0;
            if (place->second.sound == sound)
            {
                totals.problems += CheckMovedRows(name, file, edited, sequence, before, read, place->second);
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the moves: %s\n", name.c_str(), info.name.c_str(), error.what());
            ++totals.problems;
        }
    }
}

} // namespace

void SettingMoves(const std::string& name, const File& file, const CheckedArchive& archive,
                  const std::vector<std::optional<uint32_t>>& sequences,
                  const std::map<uint32_t, hokuto::song::Sequence>& songs, Totals& totals)
{
    File edited = file;
    std::map<uint32_t, Moved> moved;
    std::set<uint32_t> tried;
    for (const auto& [sound, song] : songs)
    {
        const uint32_t sequence = *sequences[archive.FileOf(sound)];
        if (!tried.insert(sequence).second)
        {
            continue;
        }

        std::vector<hokuto::edit::SettingMove> moves;
        Moved place{.sound = sound};
        const std::span<const hokuto::song::Stretch> rows = RowsOfSound(song);
        for (const hokuto::edit::SettingMove& move : MovesOf(file, sequence, song, place.levels))
        {
            TrySettingMove(name, file, archive, sequence, move, rows, moves, place, totals);
        }
        if (moves.empty())
        {
            continue;
        }

        try
        {
            hokuto::edit::Edited result = hokuto::edit::MoveSettings(edited, sequence, moves, rows);
            if (hokuto::tools::Outgrows(archive, result.file, sequence))
            {
                ++totals.outgrown;
                continue;
            }
            edited = std::move(result.file);
            place.items = std::move(result.items);
            totals.moves += moves.size();
            ++totals.move_sequences;
            moved.emplace(sequence, std::move(place));
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u: %zu settings move one at a time but not together: %s\n", name.c_str(),
                        sequence, moves.size(), error.what());
            ++totals.problems;
        }
    }

    CheckMovedSounds(name, file, edited, archive, sequences, songs, moved, totals);
}

void PrintMoveTotals(const std::string& name, const Totals& totals, bool gba)
{
    std::printf(
        "%s: %zu settings moved in %zu sequences. %zu sounds played their %zu notes again. In %zu of them, the loop "
        "moved by a whole number of passes. %zu sounds with notes that wait for the track's notes to end played other "
        "notes. %zu sounds started their loops elsewhere, and played their notes over both loops.\n",
        name.c_str(), totals.moves, totals.move_sequences, totals.move_sounds, totals.move_notes, totals.move_loops,
        totals.move_waits, totals.move_repeats);
    if (gba)
    {
        std::printf("%s: apart on the GBA: %zu sounds whose notes of a PSG channel differ after the moves.\n",
                    name.c_str(), totals.move_psg);
    }
}

} // namespace hokuto::tools::commands
