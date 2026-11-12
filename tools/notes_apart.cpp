// SPDX-License-Identifier: MIT

// The rules by which a check tells notes apart on each driver (tools/notes_apart.h).

#include "notes_apart.h"

namespace hokuto::tools
{

int GbaPsgChannel(const ntdlib::agb::Rom& rom, uint32_t voices, uint32_t program, int key)
{
    ntdlib::mp2k::Voice voice;
    if (!ntdlib::mp2k::ReadVoice(rom, voices + 12 * program, voice))
    {
        return 0;
    }
    if (voice.IsSplit())
    {
        const uint32_t address = ntdlib::mp2k::SplitVoice(rom, voice, key);
        if (address == 0 || !ntdlib::mp2k::ReadVoice(rom, address, voice))
        {
            return 0;
        }
    }

    return voice.PsgChannel();
}

bool Roomless(const song::GbaRom& rom)
{
    constexpr uint32_t kMargin = ntdlib::mp2k::kFreeSpaceMargin;
    const auto size = static_cast<uint32_t>(rom.rom.Size());
    if (size < ntdlib::mp2k::kLargestRom)
    {
        return false;
    }

    // A run longer than two margins always leaves free space.
    uint32_t run = 0;
    while (run < size && run <= 2 * kMargin && rom.rom.U8(rom.rom.End() - 1 - run) == 0xFF)
    {
        ++run;
    }
    const uint32_t free = run == 0 ? size : (size - run + 2 * kMargin - 1) / kMargin * kMargin;
    return free >= size;
}

bool ExportOutgrows(const format::File& edited)
{
    try
    {
        format::ExportArchive(edited);
    }
    catch (const std::invalid_argument& error)
    {
        const std::string_view message = error.what();
        if (message.find("32 MiB") == std::string_view::npos &&
            message.find("outgrows its room") == std::string_view::npos)
        {
            throw std::runtime_error(std::string("the export refuses the edited file: ") + error.what());
        }
        return true;
    }

    return false;
}

bool Outgrows(const CheckedArchive& archive, const format::File& edited, uint32_t sequence)
{
    if (archive.driver)
    {
        return ExportOutgrows(edited);
    }

    if (!archive.gba || !Roomless(*archive.gba))
    {
        return false;
    }

    const std::vector<uint32_t> moved = format::MovedSongs(edited);
    if (std::find(moved.begin(), moved.end(), sequence) == moved.end())
    {
        return false;
    }
    if (!edited.native->gba_filler)
    {
        return true;
    }

    return ExportOutgrows(edited);
}

std::set<std::pair<int, uint32_t>> NoteCommandTicks(const format::File& file, uint32_t sequence,
                                                    const song::Sequence& song)
{
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    const std::vector<uint32_t> offsets = edit::Offsets(commands, edit::ConsoleOf(file));
    std::map<uint32_t, std::size_t> items;
    for (std::size_t i = 0; i < std::min(offsets.size(), commands.items.size()); ++i)
    {
        items.emplace(offsets[i], i);
    }

    std::set<std::pair<int, uint32_t>> ticks;
    for (const song::Track& track : song.tracks)
    {
        for (const song::Stretch& stretch : track.stretches)
        {
            const auto first = items.find(stretch.first);
            const auto last = items.find(stretch.last);
            if (first == items.end() || last == items.end())
            {
                continue;
            }

            uint64_t tick = stretch.tick;
            for (std::size_t i = first->second; i <= last->second; ++i)
            {
                const ntdlib::commands::Command& command = commands.items[i];
                if (command.kind == ntdlib::commands::Kind::kWait)
                {
                    tick += static_cast<uint64_t>(command.value.value / ntdlib::commands::kOne);
                }
                else if (command.kind == ntdlib::commands::Kind::kNote && tick <= UINT32_MAX)
                {
                    ticks.emplace(track.number, static_cast<uint32_t>(tick));
                }
            }
        }
    }

    return ticks;
}

NotesApart PsgNotesApart(const song::GbaRom& rom, uint32_t song, const std::set<int>& changed)
{
    ntdlib::mp2k::SongHeader header;
    if (!ntdlib::mp2k::ReadSong(rom.rom, rom.info, static_cast<int>(song), header))
    {
        return {};
    }

    return [&rom, voices = header.voices, changed](const song::Track& track, const song::Note& note)
    {
        return changed.contains(track.number) && GbaPsgChannel(rom.rom, voices, note.program, note.key) != 0;
    };
}

NotesApart BrowniePsgNotesApart(const std::set<int>& changed)
{
    return [changed](const song::Track& track, const song::Note&)
    {
        return changed.contains(track.number) && track.number < ntdlib::brownie::kFirstSampleChannel;
    };
}

NotesApart Rd2PsgNotesApart(const song::Rd2Rom& rom, uint32_t sound, const std::set<int>& changed)
{
    const std::vector<uint16_t> banks = rom.BankListOf(sound);
    return [&rom, banks, changed](const song::Track& track, const song::Note& note)
    {
        if (!changed.contains(track.number) || note.bank_slot >= banks.size())
        {
            return false;
        }
        for (int key = 0; key < 128; key++)
        {
            const ntdlib::rd2::Lookup lookup =
                ntdlib::rd2::LookUpInstrument(rom.rom, rom.info, banks[note.bank_slot],
                                              static_cast<uint16_t>(note.program), static_cast<uint8_t>(key));
            const uint8_t type = lookup.found && !lookup.missing && lookup.region != 0
                                     ? ntdlib::rd2::RegionType(rom.rom, lookup.region)
                                     : ntdlib::rd2::kSampleType;
            if (type != ntdlib::rd2::kSampleType && type <= ntdlib::rd2::kNoiseType)
            {
                return true;
            }
        }

        return false;
    };
}

NotesApart KonamiPsgNotesApart(ntdlib::konami::Revision revision)
{
    if (!ntdlib::konami::RecordHasPan(revision) && !ntdlib::konami::EternalDuelistLayout(revision))
    {
        return {};
    }

    return [](const song::Track& track, const song::Note&)
    {
        return ntdlib::konami::IsPsgTrack(track.number);
    };
}

bool HoldsSequence(const CheckedArchive& archive, const ntdlib::commands::Sequence& want,
                   const ntdlib::commands::Sequence& got)
{
    if (const song::BrownieRom* brownie = archive.Brownie())
    {
        // The bytes as the export places them, at the address that keeps the sequence's first word on a multiple of 4.
        const auto bytes = [brownie](const ntdlib::commands::Sequence& sequence)
        {
            const std::vector<uint32_t> offsets = format::BrownieItemOffsets(sequence, brownie->info.revision);
            uint32_t remainder = 0;
            for (std::size_t item = 0; item < sequence.items.size(); item++)
            {
                const ntdlib::commands::Command& command = sequence.items[item];
                if (command.kind == ntdlib::commands::Kind::kOpenTrack ||
                    (command.kind == ntdlib::commands::Kind::kJump &&
                     command.encoding.opcode == ntdlib::brownie::kAddressWord) ||
                    (command.kind == ntdlib::commands::Kind::kEnd && command.index == 1))
                {
                    remainder = (4 - offsets[item] % 4) % 4;
                    break;
                }
            }
            return ntdlib::brownie::EncodeSequence(sequence, brownie->info.revision, ntdlib::agb::kRomBase + remainder)
                .bytes;
        };
        // The data after the last command can come back as other bytes.
        std::size_t end = want.items.size();
        while (end > 0 && want.items[end - 1].kind == ntdlib::commands::Kind::kData)
        {
            --end;
        }
        const std::size_t size = format::BrownieItemOffsets(want, brownie->info.revision)[end];
        const std::vector<uint8_t> wanted = bytes(want);
        const std::vector<uint8_t> held = bytes(got);
        return wanted.size() >= size && held.size() >= size &&
               std::equal(wanted.begin(), wanted.begin() + static_cast<std::ptrdiff_t>(size), held.begin());
    }
    if (archive.dse)
    {
        return ntdlib::dse::EncodeTracks(want).bytes == ntdlib::dse::EncodeTracks(got).bytes;
    }
    if (archive.driver)
    {
        const song::KonamiRom* konami = archive.Konami();
        const auto same = [konami](const ntdlib::commands::Command& a, const ntdlib::commands::Command& b)
        {
            // On Konami's driver, a new pan without a level uses the level of the driver (ntdlib/konami/commands.h). A
            // pan's value comes back as the nearest position that the revision's bytes hold. A delay that a new value
            // leaves in another form keeps its value. On Nintendo R&D2's driver, a note or a wait of one byte keeps the
            // values that the track's last note or wait gave.
            ntdlib::commands::Command without = a;
            without.encoding = b.encoding;
            without.bytes = a.kind == ntdlib::commands::Kind::kWait || !konami ? b.bytes : a.bytes;
            if (konami && a.kind == ntdlib::commands::Kind::kPan)
            {
                const ntdlib::commands::Scale scale = ntdlib::konami::ScaleOf(a.kind, konami->info.revision);
                without.velocity = a.velocity == 0 ? b.velocity : a.velocity;
                without.value.value =
                    ntdlib::commands::FromUnit(a.value.value, scale) == ntdlib::commands::FromUnit(b.value.value, scale)
                        ? b.value.value
                        : a.value.value;
            }
            return without == b;
        };
        auto end = want.items.end();
        while (end != want.items.begin() && std::prev(end)->kind == ntdlib::commands::Kind::kData)
        {
            --end;
        }
        return got.items.size() >= static_cast<std::size_t>(end - want.items.begin()) &&
               std::equal(want.items.begin(), end, got.items.begin(), same);
    }
    if (!archive.gba)
    {
        return got == want;
    }

    const int note_arguments = ntdlib::mp2k::NoteArguments(archive.gba->info);
    const std::vector<uint8_t> wanted = ntdlib::mp2k::EncodeSequence(want, ntdlib::agb::kRomBase, note_arguments).bytes;
    const std::vector<uint8_t> held = ntdlib::mp2k::EncodeSequence(got, ntdlib::agb::kRomBase, note_arguments).bytes;
    return held.size() >= wanted.size() && std::equal(wanted.begin(), wanted.end(), held.begin()) &&
           std::all_of(held.begin() + static_cast<std::ptrdiff_t>(wanted.size()), held.end(),
                       [](uint8_t b) { return b == 0; });
}

} // namespace hokuto::tools
