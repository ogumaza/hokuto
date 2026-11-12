// SPDX-License-Identifier: MIT

// The round of hokuto_check_tracks that pastes tracks into other sounds.

#include "check_tracks.h"

namespace hokuto::tools::tracks
{

namespace
{

// A paste of a track into a sound: the sound that the track came from, its number there, its number in the sound, and
// whether the tool counts it apart.
struct Paste
{
    uint32_t from = 0;
    int original = 0;
    int track = 0;
    bool apart = false;
};

// The voice group that GBA song `sound` uses, from its header, or nothing.
std::optional<uint32_t> VoicesOf(const Archive& archive, uint32_t sound)
{
    ntdlib::mp2k::SongHeader header;
    if (!ntdlib::mp2k::ReadSong(archive.archive.gba->rom, archive.archive.gba->info, static_cast<int>(sound), header))
    {
        return std::nullopt;
    }

    return header.voices;
}

// The bank in each slot of sound `sound` of `file`: on the GBA, the song's voice group. A DS sound uses the one bank of
// its entry in the archive. A DS file keeps no tables, and the edits of tracks never change the bank. A DSE sound names
// its banks by their ids. A song of Nintendo R&D2's driver uses the banks in the bank list of its sequence, by their
// numbers among the banks of the driver. The list is in the ROM's tables, and the edits of tracks never change it. A
// paste selects only slots that the song already selects. Throws std::runtime_error for a file whose slots the tool
// can't read.
std::vector<uint32_t> SlotsOf(const Archive& archive, const File& file, uint32_t sound)
{
    if (archive.archive.dse)
    {
        const std::vector<uint16_t> ids = edit::DseBankIds(file, archive.places[sound]->sequence);
        return {ids.begin(), ids.end()};
    }
    if (archive.archive.Konami())
    {
        return {};
    }
    if (archive.archive.gba)
    {
        return {VoicesOf(archive, sound).value_or(sar::kNoItem)};
    }
    if (archive.archive.ds)
    {
        return {song::DsBankOf(*archive.archive.ds, sound).value_or(sar::kNoItem)};
    }
    if (const song::Rd2Rom* rd2 = song::Rd2Of(archive.archive.driver.get()))
    {
        const std::vector<uint16_t> banks = rd2->BankListOf(sound);
        return {banks.begin(), banks.end()};
    }
    if (!file.native || !file.native->tables || sound >= file.native->tables->sounds.size())
    {
        throw std::runtime_error("the tool reads no bank slots of sound " + std::to_string(sound) + " of this file");
    }

    return file.native->tables->sounds[sound].banks;
}

// The sound of `chosen` that gives sound `chosen[index]` a track: the next one that plays another sequence file, or
// else the next one. Nothing when `chosen` holds no other sound. A sound of the Wii plays one bank, and takes a track
// only from a sound of the same bank: the next one that plays another sequence file first. A GBA song uses one voice
// group in the same way. A DSE sound accepts a track from a sound that names no other banks than it does, `dse_banks`.
std::optional<uint32_t> SourceOf(const Archive& archive, const std::vector<uint32_t>& chosen, std::size_t index,
                                 const std::map<uint32_t, std::vector<uint32_t>>& dse_banks)
{
    const uint32_t sound = chosen[index];
    const uint32_t sequence = archive.places[sound]->sequence;

    const auto takes = [&archive, &dse_banks, sound](uint32_t other)
    {
        if (archive.archive.dse)
        {
            const std::vector<uint32_t>& banks = dse_banks.at(sound);
            const std::vector<uint32_t>& others = dse_banks.at(other);
            const auto held = [&banks](uint32_t bank)
            {
                return std::ranges::find(banks, bank) != banks.end();
            };
            return !others.empty() && std::ranges::all_of(others, held) &&
                   archive.songs.at(other).timebase == archive.songs.at(sound).timebase;
        }
        if (archive.archive.gba)
        {
            return VoicesOf(archive, other) == VoicesOf(archive, sound);
        }
        return !archive.archive.wii || archive.archive.BanksOf(other) == archive.archive.BanksOf(sound);
    };
    for (const bool same_file : {false, true})
    {
        for (std::size_t step = 1; step < chosen.size(); step++)
        {
            const uint32_t other = chosen[(index + step) % chosen.size()];
            if ((archive.places[other]->sequence == sequence) == same_file && takes(other))
            {
                return other;
            }
        }
    }

    return chosen.size() > 1 ? std::optional<uint32_t>(chosen[(index + 1) % chosen.size()]) : std::nullopt;
}

// The bank of each note of track `number` of `sound` that starts before tick `end` and lasts `shortest` ticks or more,
// from the sound's slots `banks`, with the note's program.
std::vector<std::pair<uint32_t, uint32_t>> InstrumentsOf(const song::Sequence& sound, int number, uint32_t end,
                                                         const std::vector<uint32_t>& banks, uint32_t shortest = 0)
{
    std::vector<std::pair<uint32_t, uint32_t>> instruments;
    for (const song::Track& track : sound.tracks)
    {
        for (const song::Note& note : CommandNotes(sound, track))
        {
            if (track.number == number && note.start < end && note.length >= shortest)
            {
                const uint32_t bank = note.bank_slot < banks.size() ? banks[note.bank_slot] : sar::kNoItem;
                instruments.emplace_back(bank, note.program);
            }
        }
    }

    return instruments;
}

// Whether the notes of `pairing`, of two lists whose instruments `first` and `second` give (InstrumentsOf), play the
// same instruments. A GBA note that the driver never started matches any instrument (kNoProgram).
bool SameInstruments(const Pairing& pairing, const std::vector<std::pair<uint32_t, uint32_t>>& first,
                     const std::vector<std::pair<uint32_t, uint32_t>>& second)
{
    return !pairing.failed && std::ranges::all_of(pairing.pairs,
                                                  [&first, &second](const std::pair<std::size_t, std::size_t>& pair)
                                                  {
                                                      const auto& one = first[pair.first];
                                                      const auto& other = second[pair.second];
                                                      return one.second == kNoProgram || other.second == kNoProgram ||
                                                             one == other;
                                                  });
}

// `sound` with its notes' times, its length and its loop in ticks of a quarter note of `to` ticks.
song::Sequence InTicksOf(song::Sequence sound, int to)
{
    const int from = sound.timebase > 0 ? sound.timebase : to;
    const auto scale = [from, to](uint32_t& ticks)
    {
        ticks = static_cast<uint32_t>(uint64_t{ticks} * static_cast<uint64_t>(to) / static_cast<uint64_t>(from));
    };
    for (song::Track& track : sound.tracks)
    {
        for (std::vector<song::Note>* notes : {&track.notes, &track.unstarted})
        {
            for (song::Note& note : *notes)
            {
                scale(note.start);
                scale(note.length);
            }
        }
    }
    scale(sound.length);
    if (sound.loop)
    {
        scale(sound.loop->start);
        scale(sound.loop->end);
    }
    sound.timebase = to;

    return sound;
}

// Whether two sounds each keep one tempo, the same. The engine sets some lengths, such as a one-shot wave's. Such a
// length lasts other ticks at another tempo.
bool SameTempo(const song::Sequence& first, const song::Sequence& second)
{
    const auto bpm = [](const song::Sequence& sound)
    {
        return sound.tempo.empty() ? 120.0 : sound.tempo.front().bpm;
    };
    const auto steady = [&bpm](const song::Sequence& sound)
    {
        const auto same = [&bpm, &sound](const song::Tempo& tempo)
        {
            return tempo.bpm == bpm(sound);
        };
        return std::ranges::all_of(sound.tempo, same);
    };

    return steady(first) && steady(second) && bpm(first) == bpm(second);
}

// The first track of `before`, a sound before its paste, whose notes `now`, the sound with the paste, plays otherwise
// up to `earlier_end`, with what differs, or nothing when every track plays as before.
std::optional<std::string> OtherNotes(const Archive& archive, const song::Sequence& before, const song::Sequence& now,
                                      const Paste& paste, uint32_t earlier_end)
{
    for (const int number : NumbersOf(before))
    {
        // A song of Konami's driver keeps all its tracks, and a paste is placed on a track without notes.
        if (archive.archive.Konami() && number == paste.track)
        {
            continue;
        }
        if (!SameNotes(NotesOf(before, number, earlier_end), NotesOf(now, number, earlier_end), earlier_end, true,
                       FrameTicks(before, now)))
        {
            return "other notes on track " + std::to_string(number) + ": " +
                   NoteDifference(NotesOf(before, number, earlier_end), NotesOf(now, number, earlier_end));
        }
    }

    return std::nullopt;
}

// The starts and the ends of the loops of `counted`, `before` and `now`.
std::set<uint32_t> LoopEdges(const song::Sequence& counted, const song::Sequence& before, const song::Sequence& now)
{
    std::set<uint32_t> ends;
    for (const song::Sequence* song : std::array<const song::Sequence*, 3>{&counted, &before, &now})
    {
        if (song->loop)
        {
            ends.insert(song->loop->start);
            ends.insert(song->loop->end);
        }
    }

    return ends;
}

// Pairs the notes of `paste`'s track in `counted` and in `now` up to `shared` without the quiet notes of the PSG
// channels (WithoutQuietPsgNotes). Where they pair, `pairing` receives the pairing, `counted` and `now` lose those
// notes, and the function returns true.
bool PairLoudNotes(const Paste& paste, uint32_t shared, bool lengths, uint32_t frame, song::Sequence& counted,
                   song::Sequence& now, Pairing& pairing)
{
    song::Sequence loud_original = WithoutQuietPsgNotes(counted);
    song::Sequence loud_now = WithoutQuietPsgNotes(now);
    const Pairing loud = PairNotes(NotesOf(loud_original, paste.original, shared),
                                   NotesOf(loud_now, paste.track, shared), shared, lengths, frame);
    if (loud.failed)
    {
        return false;
    }

    pairing = loud;
    counted = std::move(loud_original);
    now = std::move(loud_now);

    return true;
}

// Checks that sound `sound` plays its tracks as before with the pasted track of `paste`. `read` is the sound in the
// reads of `version`, the file with the pastes.
void CheckPaste(const Archive& archive, const File& version, uint32_t sound, const Paste& paste,
                std::map<uint32_t, song::Sequence>::const_iterator read, Totals& totals)
{
    // The sound plays its tracks as before, up to the earlier end. A track that ran longer than the others or
    // looped sets where the sound ends or loops, and the pasted track can. A note that lasts to the end of the
    // performance then lasts otherwise.
    const song::Sequence before = ComparableRead(archive, archive.file, sound, archive.songs.at(sound));
    const song::Sequence source = ComparableRead(archive, archive.file, paste.from, archive.songs.at(paste.from));
    song::Sequence now = ComparableRead(archive, version, sound, read->second);
    std::set<int> numbers = NumbersOf(before);
    numbers.insert(paste.track);
    std::optional<std::string> difference;
    if (ComparedNumbersOf(before, now, numbers) != numbers)
    {
        difference = "tracks " + NumbersText(NumbersOf(now)) + " in place of " + NumbersText(numbers);
    }
    const uint32_t earlier_end = std::min(EndOf(before), EndOf(now));
    if (!difference)
    {
        difference = OtherNotes(archive, before, now, paste, earlier_end);
    }

    // The pasted track plays the notes of the other sound's track in the sound's ticks, up to the earlier end, with its
    // banks and programs. Its lengths count where both sounds keep one tempo, the same, and count the same ticks to a
    // quarter note. The engine sets some lengths, such as a one-shot wave's, in frames. A tick of Konami's driver lasts
    // a frame in every song, and the timebase only draws the ruler. A pasted track has no loop point there, and returns
    // to its start when the song loops. It compares up to that loop's end. The lengths and levels of its notes count
    // less there. A song's end cuts a note short. In the Eternal Duelist's layout, the track before a PSG track sets
    // its volume. The performer can mark a song's second pass as its loop. Such a read continues past the first pass,
    // where the pasted track has gone back to its start or stopped. It compares up to the latest end of a marked loop
    // or its start that produces the same notes then.
    song::Sequence counted = archive.archive.Konami() ? source : InTicksOf(source, before.timebase);
    uint32_t shared = archive.archive.Konami() ? std::min({EndOf(counted), EndOf(now), EndOf(before)})
                                               : std::min(EndOf(counted), EndOf(now));
    const bool levels = !archive.archive.Konami() ||
                        !ntdlib::konami::EternalDuelistLayout(archive.archive.Konami()->info.revision) ||
                        !ntdlib::konami::IsPsgTrack(paste.track);
    const auto notes_of = [levels, &paste, &counted, &now](uint32_t end, bool pasted)
    {
        std::vector<TrackNote> notes = pasted ? NotesOf(now, paste.track, end) : NotesOf(counted, paste.original, end);
        for (TrackNote& note : notes)
        {
            std::get<3>(note) = levels ? std::get<3>(note) : uint8_t{0};
        }

        return notes;
    };
    std::vector<TrackNote> original = notes_of(shared, false);
    std::vector<TrackNote> copy = notes_of(shared, true);
    const bool lengths = !archive.archive.Konami() && SameTempo(source, before) && source.timebase == before.timebase;
    const uint32_t frame = FrameTicks(counted, now);
    Pairing pairing = PairNotes(original, copy, shared, lengths, frame);
    if (archive.archive.Konami() && pairing.failed)
    {
        const std::set<uint32_t> ends = LoopEdges(counted, before, now);
        for (auto end = ends.rbegin(); end != ends.rend() && pairing.failed; ++end)
        {
            const std::vector<TrackNote> first_original = notes_of(*end, false);
            const std::vector<TrackNote> first_copy = notes_of(*end, true);
            const Pairing first_pairing = PairNotes(first_original, first_copy, *end, lengths, frame);
            if (*end < shared && !first_original.empty() && !first_pairing.failed)
            {
                shared = *end;
                original = first_original;
                copy = first_copy;
                pairing = first_pairing;
                ++totals.pasted_first_pass;
            }
        }
    }

    // On the GBA, the pasted track plays beside other tracks than the original did, and their notes come before
    // its notes on the PSG channels. Its quiet notes there can then come or go (WithoutQuietPsgNotes). A paste that
    // plays otherwise only in such notes counts apart.
    const bool quiet =
        archive.archive.gba && pairing.failed && PairLoudNotes(paste, shared, lengths, frame, counted, now, pairing);
    std::optional<Pairing> ticks;
    if (!difference && pairing.failed)
    {
        ticks = PairNotes(NotesOf(counted, paste.original, shared, kTickNotes),
                          NotesOf(now, paste.track, shared, kTickNotes), shared, lengths, frame);
        difference = "other notes on the pasted track " + std::to_string(paste.track) + " from " + source.name +
                     "'s track " + std::to_string(paste.original) + ": " + NoteDifference(original, copy);
        if (archive.archive.Konami())
        {
            *difference += " (loops " + LoopText(counted.loop) + " there, " + LoopText(before.loop) + " before and " +
                           LoopText(now.loop) + " after)";
        }
    }
    const std::vector<uint32_t> banks = SlotsOf(archive, version, sound);
    const std::vector<uint32_t> source_banks = SlotsOf(archive, archive.file, paste.from);
    if (!difference && !SameInstruments(pairing, InstrumentsOf(counted, paste.original, shared, source_banks),
                                        InstrumentsOf(now, paste.track, shared, banks)))
    {
        difference = "other banks or programs on the pasted track " + std::to_string(paste.track);
    }

    totals.pasted_ending += now.loop != before.loop || now.length != before.length ? 1 : 0;
    if (paste.apart)
    {
        ++totals.pasted_apart;
        totals.pasted_differing += difference ? 1 : 0;
    }
    else if (ticks && SameInstruments(*ticks, InstrumentsOf(counted, paste.original, shared, source_banks, kTickNotes),
                                      InstrumentsOf(now, paste.track, shared, banks, kTickNotes)))
    {
        ++totals.tick_notes;
    }
    else if (difference && NumbersOf(now) == numbers &&
             VoicesDecide(
                 [&]
                 {
                     const Pairing pasted =
                         PairNotes(notes_of(shared, false), notes_of(shared, true), shared, lengths, frame);
                     return !OtherNotes(archive, before, now, paste, earlier_end) && !pasted.failed &&
                            SameInstruments(pasted, InstrumentsOf(counted, paste.original, shared, source_banks),
                                            InstrumentsOf(now, paste.track, shared, banks));
                 }))
    {
        ++totals.voice_notes;
    }
    else if (difference)
    {
        std::printf("%s: %s plays %s with a paste of %s's track %d\n", archive.name.c_str(), before.name.c_str(),
                    difference->c_str(), source.name.c_str(), paste.original);
        ++totals.problems;
    }
    else if (quiet)
    {
        ++totals.quiet_pastes;
    }
}

// The file with the pastes, and the paste in each sound that took one.
struct Pasted
{
    File version;
    std::map<uint32_t, Paste> pastes;
};

// Pastes the busiest track of another sound of `chosen` into each sound of `chosen` (SourceOf), all in one version.
Pasted PasteChosen(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals)
{
    File version = archive.file;
    std::map<uint32_t, Paste> pastes;
    std::map<uint32_t, std::vector<uint32_t>> dse_banks;
    for (const uint32_t sound : chosen)
    {
        if (archive.archive.dse)
        {
            dse_banks[sound] = SlotsOf(archive, archive.file, sound);
        }
    }

    for (std::size_t index = 0; index < chosen.size(); index++)
    {
        const uint32_t sound = chosen[index];
        const std::optional<uint32_t> from = SourceOf(archive, chosen, index, dse_banks);
        const std::optional<int> track = from ? BusiestTrack(archive.songs.at(*from)) : std::nullopt;
        if (!track)
        {
            continue;
        }

        // A track whose commands draw random numbers, use variables, run the game's code or run only if a comparison
        // held is counted apart. So is a track that its sound starts more than once: the paste plays it once. So is a
        // track with a note of length 0. Its wave sets how long the note lasts, and with the note wait, when the track
        // goes on. Its ticks then follow the tempo.
        const song::Sequence& source = archive.songs.at(*from);
        const auto numbered = [&track](const song::Track& candidate)
        {
            return candidate.number == *track;
        };
        const song::Track& played = *std::ranges::find_if(source.tracks, numbered);
        const auto opens = [](const song::Stretch& stretch)
        {
            return stretch.entry == ntdlib::sequence_export::Entry::kOpen;
        };
        try
        {
            const edit::CopiedTrack copied = edit::CopyTrack(archive.file, archive.places, *from, source, *track);
            edit::TrackEdit edited = edit::PasteTrack(version, archive.places, sound, archive.songs.at(sound), copied);
            if (hokuto::tools::Outgrows(archive.archive, edited.file, archive.places[sound]->sequence))
            {
                ++totals.outgrown;
                continue;
            }
            const bool slots = SlotsOf(archive, edited.file, sound) != SlotsOf(archive, version, sound);
            version = std::move(edited.file);
            const auto waits = [](const Command& command)
            {
                return command.kind == Kind::kNote && command.value.value == 0;
            };
            // A note of Konami's driver has no length, and gets no wait from its wave.
            const bool apart = std::ranges::any_of(copied.commands, Chancy) ||
                               std::ranges::count_if(played.stretches, opens) > 1 ||
                               (!archive.archive.Konami() && std::ranges::any_of(copied.commands, waits)) ||
                               KonamiPsgMate(archive, *track);
            pastes.emplace(sound, Paste{*from, *track, edited.track, apart});
            totals.pasted_late += copied.tick > 0 ? 1 : 0;
            totals.pasted_slots += slots ? 1 : 0;
            ++totals.pasted;
        }
        catch (const std::invalid_argument& error)
        {
            ++totals.refusals[std::string("paste: ") + error.what()];
        }
        catch (const std::runtime_error& error)
        {
            std::printf("%s: %s: paste: %s\n", archive.name.c_str(), archive.songs.at(sound).name.c_str(),
                        error.what());
            ++totals.problems;
        }
    }

    return {std::move(version), std::move(pastes)};
}

} // namespace

void PasteTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals)
{
    auto [version, pastes] = PasteChosen(archive, chosen, totals);

    // A sound that shares the file with other banks plays the pasted notes on them, and counts apart.
    for (const auto& [sharer, sound] : Sharers(archive, pastes))
    {
        Paste paste = pastes.at(sound);
        paste.apart = paste.apart || SlotsOf(archive, version, sharer) != SlotsOf(archive, version, sound);
        pastes.emplace(sharer, paste);
    }
    const std::map<uint32_t, song::Sequence> after = ReadAll(archive, version, "pastes", totals);
    std::set<uint32_t> edited;
    for (const auto& [sound, paste] : pastes)
    {
        edited.insert(sound);
        const auto read = after.find(sound);
        if (read == after.end())
        {
            continue;
        }

        CheckPaste(archive, version, sound, paste, read, totals);
    }
    CheckOthers(archive, after, edited, "pastes", totals);
}

} // namespace hokuto::tools::tracks
