// SPDX-License-Identifier: MIT

// The checks of banks, wave archives and wave sound files in the places of others of hokuto_check_replace.

#include "check_replace.h"

namespace hokuto::tools::replace
{

namespace
{

// The instrument at program `program` of `bank`, or nothing.
const sar::Instrument* InstrumentAt(const sar::Bank& bank, uint32_t program)
{
    return program < bank.instruments.size() && bank.instruments[program] ? &*bank.instruments[program] : nullptr;
}

// Whether ntdlib reads the same instrument at program `p` of `a` as at program `q` of `b`, or neither: the same key and
// velocity regions with the same values, and each region's wave by its wave archive and index.
bool SameInstrument(const sar::Bank& a, uint32_t p, const sar::Bank& b, uint32_t q)
{
    const sar::Instrument* x = InstrumentAt(a, p);
    const sar::Instrument* y = InstrumentAt(b, q);
    if (!x || !y)
    {
        return !x && !y;
    }

    const auto same_region = [&a, &b](const sar::VelocityRegion& r, const sar::VelocityRegion& s)
    {
        const bool waves = r.wave_id_index < a.waves.size() && s.wave_id_index < b.waves.size() &&
                           a.waves[r.wave_id_index].wave_archive_item == b.waves[s.wave_id_index].wave_archive_item &&
                           a.waves[r.wave_id_index].index == b.waves[s.wave_id_index].index;

        const auto envelope = [](const sar::Adshr& e)
        {
            return std::array<uint8_t, 5>{e.attack, e.decay, e.sustain, e.hold, e.release};
        };

        return waves && r.original_key == s.original_key && r.volume == s.volume && r.pan == s.pan &&
               r.pitch == s.pitch && r.ignore_note_off == s.ignore_note_off && r.key_group == s.key_group &&
               r.interpolation == s.interpolation && envelope(r.adshr) == envelope(s.adshr);
    };

    if (x->keys.size() != y->keys.size())
    {
        return false;
    }
    for (std::size_t k = 0; k < x->keys.size(); k++)
    {
        const sar::KeyRegion& i = x->keys[k];
        const sar::KeyRegion& j = y->keys[k];
        if (i.lo != j.lo || i.hi != j.hi || i.velocities.size() != j.velocities.size())
        {
            return false;
        }
        for (std::size_t v = 0; v < i.velocities.size(); v++)
        {
            const sar::KeyRegion::Vel& m = i.velocities[v];
            const sar::KeyRegion::Vel& n = j.velocities[v];
            if (m.lo != n.lo || m.hi != n.hi || m.region.has_value() != n.region.has_value() ||
                (m.region && !same_region(*m.region, *n.region)))
            {
                return false;
            }
        }
    }

    return true;
}

// What is wrong when bank `item` takes `bytes`, the next bank, with copies of instruments at the programs that the
// sounds select and the next bank lacks: first the present bank's instruments, then the next bank's first instrument.
// `places` gives where each sound plays. A bank whose sounds select no such program gives nothing.
std::string FixesProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                         std::span<const std::optional<edit::SoundPlace>> places, const FileItem& item,
                         std::span<const uint8_t> bytes, Totals& totals)
{
    // The programs that the sounds select and the next bank lacks.
    const sar::Options& options = built.ReadOptions();
    const edit::IncomingFile incoming =
        edit::ReadIncomingFile(bytes, item.kind, options.flavor, edit::IncomingVersions(built, item.kind, item.item));
    const sar::Bank next = sar::Bank::Parse(incoming.bytes, options);
    const sar::Bank present = sar::Bank::Parse(built.FileData(item.file_id), options);
    const edit::BankSelections selections = edit::ProgramsOfBank(file, built, places, item.item);
    std::vector<uint32_t> lacked;
    for (const uint32_t program : selections.programs)
    {
        if (!InstrumentAt(next, program))
        {
            lacked.push_back(program);
        }
    }
    totals.selected += selections.programs.size();
    totals.undecided += selections.undecided;
    if (lacked.empty())
    {
        return {};
    }
    totals.fixed_banks++;
    totals.fixed_programs += lacked.size();

    // The present bank's instruments where it holds them, then the next bank's first instrument.
    std::optional<uint32_t> first;
    for (uint32_t program = 0; program < next.instruments.size() && !first; program++)
    {
        first = next.instruments[program] ? std::optional<uint32_t>(program) : std::nullopt;
    }
    for (const bool from_present : {true, false})
    {
        std::vector<edit::ProgramFix> fixes;
        for (const uint32_t program : lacked)
        {
            if (from_present && InstrumentAt(present, program))
            {
                fixes.push_back({program, std::nullopt});
            }
            else if (!from_present && first)
            {
                fixes.push_back({program, *first});
            }
        }
        if (fixes.empty())
        {
            continue;
        }

        // The copies pass the checks of the next file. Each reads as its instrument, and the others as before.
        const std::vector<uint8_t> fixed = edit::WithProgramFixes(built, item.item, incoming.bytes, fixes);
        Archive after;
        const std::string problem =
            ReplacedProblem(built, groups, item, fixed, edit::ReplaceFile(file, item.file_id, fixed), after);
        if (!problem.empty())
        {
            return problem + " with copies of instruments";
        }
        const sar::Bank read = sar::Bank::Parse(after->FileData(item.file_id), options);
        for (uint32_t program = 0; program < std::max(read.instruments.size(), next.instruments.size()); program++)
        {
            const auto fix = std::ranges::find(fixes, program, &edit::ProgramFix::program);
            const bool same = fix == fixes.end() ? SameInstrument(read, program, next, program)
                              : fix->from        ? SameInstrument(read, program, next, *fix->from)
                                                 : SameInstrument(read, program, present, program);
            if (!same)
            {
                return "reads program " + std::to_string(program) + " otherwise after the copies of instruments";
            }
        }
        (from_present ? totals.fixes_present : totals.fixes_next) += fixes.size();
    }

    return {};
}

// Whether two wave archives that ntdlib reads with `options` hold waves that decode to the same samples at the same
// rate, with the same loop.
bool SameWaveArchive(std::span<const uint8_t> a, std::span<const uint8_t> b, const sar::Options& options)
{
    const std::vector<std::span<const uint8_t>> x = sar::WaveArchive::Parse(a, options).waves;
    const std::vector<std::span<const uint8_t>> y = sar::WaveArchive::Parse(b, options).waves;
    if (x.size() != y.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < x.size(); i++)
    {
        const sar::Pcm p = sar::DecodeWave(x[i], options);
        const sar::Pcm q = sar::DecodeWave(y[i], options);
        if (p.sample_rate != q.sample_rate || p.loop != q.loop || p.loop_start != q.loop_start ||
            p.loop_end != q.loop_end || p.channels != q.channels)
        {
            return false;
        }
    }

    return true;
}

// What is wrong when `item` of `built`, the archive that `file` builds, receives `other_bytes`, the file of another
// item of its kind, or nothing (FileProblem, and FixesProblem for a bank). The item then joins groups
// (JoinAndNewGroupCheck). `versions` are the versions of the item's file. Counts the files crossed and written.
std::string CrossAndJoinFile(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                             std::span<const edit::Group> edit_groups,
                             std::span<const std::optional<edit::SoundPlace>> places, const FileItem& item,
                             std::span<const uint8_t> other_bytes, const song::FileVersions& versions, Joins& joins,
                             Totals& totals)
{
    Archive after;
    std::string problem = FileProblem(file, built, groups, item, other_bytes, after, &totals.files_placed_copies);
    const edit::IncomingFile read =
        edit::ReadIncomingFile(other_bytes, item.kind, built.ReadOptions().flavor, versions);
    totals.files_written += read.converted ? 1 : 0;
    if (problem.empty() && item.kind == edit::FileKind::kBank)
    {
        problem = FixesProblem(file, built, groups, places, item, other_bytes, totals);
    }
    totals.files_crossed++;

    // The groups that load the item and lack a wave archive that the new file names take it. The first item
    // also joins a new group.
    if (problem.empty() && JoinsGroups(joins, edit_groups))
    {
        const uint32_t id = ItemIdOf(item);
        const edit::IncomingFile incoming =
            edit::ReadIncomingFile(other_bytes, item.kind, built.ReadOptions().flavor, versions);
        const std::vector<uint32_t> files_before = edit::FilesOfItem(built, id);
        const auto files_after = [&]
        {
            return edit::FilesOfItem(built, id, {.file = incoming.bytes});
        };
        std::optional<format::File> replaced;
        const auto added = [&]() -> const format::File&
        {
            replaced = edit::ReplaceFile(file, item.file_id, incoming.bytes);
            return *replaced;
        };
        problem = JoinAndNewGroupCheck(file, built, edit_groups, std::span(&id, 1), files_before, files_after, added,
                                       joins, totals);
    }

    return problem;
}

// What is wrong with file `items[i]` of `built`, the archive that `file` builds, or nothing: when it is imported again,
// when its files for the other consoles are imported, and when it receives the next file of its kind
// (CrossAndJoinFile).
std::string ItemFileProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                            std::span<const edit::Group> edit_groups,
                            std::span<const std::optional<edit::SoundPlace>> places, std::span<const FileItem> items,
                            std::size_t i, Joins& joins, Totals& totals)
{
    const FileItem& item = items[i];
    const std::span<const uint8_t> bytes = built.FileData(item.file_id);
    std::string problem;

    // A group's copy with other bytes takes the file's bytes. That copy changes.
    const song::FileVersions versions = edit::IncomingVersions(built, item.kind, item.item);
    const edit::IncomingFile same = edit::ReadIncomingFile(bytes, item.kind, built.ReadOptions().flavor, versions);
    const edit::FileChanges changes = edit::ChangesOf(file, built, item.item, same);
    totals.files_other_copies += changes.other_copies > 0 ? 1 : 0;
    totals.files_placed += changes.placed_copies > 0 ? 1 : 0;
    if (edit::ReplaceFile(file, item.file_id, same.bytes) != file && changes.other_copies == 0)
    {
        problem = "doesn't come back from its file";
    }

    // The files that Export… writes for the other consoles. Each goes in converted, with the versions of the
    // present file. The file has to come back unchanged too, but for a wave archive whose waves come back with
    // other bytes. Those waves have to decode as before.
    for (const sar::Flavor console : song::OtherConsoles(built.ReadOptions().flavor))
    {
        const std::vector<uint8_t> other_file = song::OtherConsoleFile(built, SongItemOf(item), console);
        const edit::IncomingFile converted =
            edit::ReadIncomingFile(other_file, item.kind, built.ReadOptions().flavor, versions);
        edit::ChangesOf(file, built, item.item, converted);
        const bool same_bytes = std::ranges::equal(converted.bytes, bytes);
        if (!same_bytes && (item.kind != edit::FileKind::kWaveArchive ||
                            !SameWaveArchive(converted.bytes, bytes, built.ReadOptions())))
        {
            problem = "doesn't come back from the " + song::ConsoleName(console);
        }
        if (same_bytes && edit::ReplaceFile(file, item.file_id, converted.bytes) != file && changes.other_copies == 0)
        {
            problem = "doesn't come back from the " + song::ConsoleName(console);
        }
        totals.files_converted_otherwise += same_bytes ? 0 : 1;
        totals.files_converted++;
    }

    // The next file of the same kind that the archive or its group files hold.
    for (std::size_t step = 1; step < items.size() && problem.empty(); step++)
    {
        const FileItem& other = items[(i + step) % items.size()];
        const std::span<const uint8_t> other_bytes = built.FileData(other.file_id);
        if (other.kind != item.kind || other_bytes.empty())
        {
            continue;
        }

        problem =
            CrossAndJoinFile(file, built, groups, edit_groups, places, item, other_bytes, versions, joins, totals);
        break;
    }

    return problem;
}

} // namespace

void CheckFiles(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                std::span<const edit::Group> edit_groups, std::span<const std::optional<edit::SoundPlace>> places,
                const std::string& name, Totals& totals)
{
    const std::vector<FileItem> items = FileItemsOf(built);
    Joins joins;
    for (std::size_t i = 0; i < items.size(); i++)
    {
        // A file that neither the archive nor its group files hold, and a file that two file IDs name, count apart.
        const FileItem& item = items[i];
        if (Outside(built, item.file_id))
        {
            totals.files_outside++;
            continue;
        }
        if (SharedFile(built, item.file_id))
        {
            totals.files_shared++;
            continue;
        }
        totals.files_beside += Beside(built, item.file_id) ? 1 : 0;

        std::string problem;
        try
        {
            problem = ItemFileProblem(file, built, groups, edit_groups, places, items, i, joins, totals);
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), NameOf(built, item).c_str(), problem.c_str());
            totals.files_failed++;
        }
        totals.files++;
    }
}

} // namespace hokuto::tools::replace
