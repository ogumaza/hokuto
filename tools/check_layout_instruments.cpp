// SPDX-License-Identifier: MIT

// The checks of hokuto_check_layout on the copies of instruments and their places in banks.

#include "check_layout.h"

namespace hokuto::tools::layout
{

namespace
{

// Whether region `x` of a bank whose waves are `x_waves` and region `y` of a bank whose waves are `y_waves` hold the
// same values, and waves that name the same wave archive and wave.
bool SameRegion(const std::vector<sar::WaveId>& x_waves, const sar::VelocityRegion& x,
                const std::vector<sar::WaveId>& y_waves, const sar::VelocityRegion& y)
{
    if (x.wave_id_index >= x_waves.size() || y.wave_id_index >= y_waves.size())
    {
        return false;
    }

    const sar::WaveId& x_wave = x_waves[x.wave_id_index];
    const sar::WaveId& y_wave = y_waves[y.wave_id_index];

    const auto adshr = [](const sar::Adshr& a)
    {
        return std::array{a.attack, a.decay, a.sustain, a.hold, a.release};
    };

    return x_wave.wave_archive_item == y_wave.wave_archive_item && x_wave.index == y_wave.index &&
           x.original_key == y.original_key && x.volume == y.volume && x.pan == y.pan &&
           std::bit_cast<uint32_t>(x.pitch) == std::bit_cast<uint32_t>(y.pitch) &&
           x.ignore_note_off == y.ignore_note_off && x.key_group == y.key_group && x.interpolation == y.interpolation &&
           adshr(x.adshr) == adshr(y.adshr);
}

// Whether instrument `x` of bank `a` and instrument `y` of bank `b` hold the same regions, by SameRegion.
bool SameInstrument(const sar::Bank& a, const std::optional<sar::Instrument>& x, const sar::Bank& b,
                    const std::optional<sar::Instrument>& y)
{
    if (!x || !y)
    {
        return !x && !y;
    }

    const auto same_velocity = [&a, &b](const sar::KeyRegion::Vel& v, const sar::KeyRegion::Vel& w)
    {
        return v.lo == w.lo && v.hi == w.hi && v.region.has_value() == w.region.has_value() &&
               (!v.region || SameRegion(a.waves, *v.region, b.waves, *w.region));
    };
    const auto same_key = [&same_velocity](const sar::KeyRegion& k, const sar::KeyRegion& l)
    {
        return k.lo == l.lo && k.hi == l.hi && std::ranges::equal(k.velocities, l.velocities, same_velocity);
    };

    return std::ranges::equal(x->keys, y->keys, same_key);
}

// The place of each program's instrument in `bank`, a bank file of `flavor`. Nothing for a program without one.
std::vector<std::optional<std::size_t>> InstrumentPlaces(std::span<const uint8_t> bank, sar::Flavor flavor)
{
    const xsflib::Reader r(bank, flavor == sar::Flavor::kCafe);
    const std::size_t body = r.U32(0x18) + 8;
    const std::size_t table = body + static_cast<std::size_t>(r.S32(r.U16(body) == 0x0101 ? body + 4 : body + 12));
    std::vector<std::optional<std::size_t>> places;
    for (uint32_t i = 0; i < r.U32(table); ++i)
    {
        const std::size_t entry = table + 4 + std::size_t{i} * 8;
        places.push_back(r.U16(entry) == 0x5900 ? std::optional(table + static_cast<std::size_t>(r.S32(entry + 4)))
                                                : std::nullopt);
    }

    return places;
}

// A bank of an archive with instruments, read by the strict rules where they read it.
struct ReadBank
{
    std::span<const uint8_t> file;
    sar::Options options;
    sar::Bank bank;
};

// Checks the copies of each instrument of `source` into `target`, at the target's first free programs.
void CheckCopies(const std::filesystem::path& path, const ReadBank& target, const ReadBank& source, bool same_bank,
                 Totals& totals)
{
    // The copies: the target's empty programs first, then the programs past its table.
    const sar::Flavor flavor = target.options.flavor;
    std::vector<sar::InstrumentCopy> copies;
    uint32_t free = 0;
    for (uint32_t program = 0; program < source.bank.instruments.size(); ++program)
    {
        while (free < target.bank.instruments.size() && target.bank.instruments[free])
        {
            ++free;
        }
        if (source.bank.instruments[program])
        {
            copies.push_back({.from = source.file, .program = program, .to = free++});
        }
    }

    const std::vector<uint8_t> bank = sar::WithInstruments(target.file, flavor, copies);

    // The bank's instruments as before, each copy as its source, and nothing at the other new programs. All three banks
    // are read by the strict rules where they read both banks.
    sar::Options rules = target.options;
    rules.rules = target.options.rules == source.options.rules ? rules.rules : sar::Rules::kReadPastDamage;
    const sar::Bank before = sar::Bank::Parse(target.file, rules);
    const sar::Bank from = sar::Bank::Parse(source.file, rules);
    const sar::Bank read = sar::Bank::Parse(bank, rules);
    const std::size_t count = std::max<std::size_t>(before.instruments.size(), copies.back().to + 1);
    const auto same_wave = [](const sar::WaveId& a, const sar::WaveId& b)
    {
        return a.wave_archive_item == b.wave_archive_item && a.index == b.index;
    };
    bool same = read.instruments.size() == count && read.waves.size() >= before.waves.size() &&
                std::equal(before.waves.begin(), before.waves.end(), read.waves.begin(), same_wave);
    std::vector<bool> copied(count);
    for (const sar::InstrumentCopy& copy : copies)
    {
        copied[copy.to] = true;
        same = same && SameInstrument(from, from.instruments[copy.program], read, read.instruments[copy.to]);
    }
    for (std::size_t program = 0; same && program < count; ++program)
    {
        const bool old = program < before.instruments.size();
        same = copied[program] || SameInstrument(before, old ? before.instruments[program] : std::nullopt, read,
                                                 read.instruments[program]);
    }
    if (!same)
    {
        Problem(path,
                std::string("the reader reads a bank with copies of ") +
                    (same_bank ? "the same bank's" : "another bank's") + " instruments otherwise",
                totals);
        return;
    }

    // The sizes in the header, and the other byte order.
    const xsflib::Reader r(bank, flavor == sar::Flavor::kCafe);
    const xsflib::Reader old(target.file, flavor == sar::Flavor::kCafe);
    const std::size_t grown = bank.size() - target.file.size();
    if (r.U32(0x0C) != old.U32(0x0C) + grown || r.U32(0x1C) != old.U32(0x1C) + grown ||
        r.U32(r.U32(0x18) + 4) != old.U32(old.U32(0x18) + 4) + grown)
    {
        Problem(path, "a bank with copies of instruments holds other sizes", totals);
    }
    if (flavor != sar::Flavor::kCtr)
    {
        const uint32_t version = r.U32(8);
        if (sar::WithOtherByteOrder(sar::WithOtherByteOrder(bank, version), version) != bank)
        {
            Problem(path, "a bank with copies of instruments doesn't come back from the other byte order", totals);
        }
        totals.swapped_banks += same_bank ? 1 : 0;
    }

    // A copy of an instrument of the same bank holds the instrument's bytes: the copies lie one after another, before
    // the bank's other bytes. A wave of the copy takes the first of the bank's entries for the wave. Where the bank
    // names a wave twice, a word of the copy can hold the first entry's index in place of a later one's.
    if (same_bank)
    {
        const std::vector<std::optional<std::size_t>> sources = InstrumentPlaces(target.file, flavor);
        const std::vector<std::optional<std::size_t>> to = InstrumentPlaces(bank, flavor);
        const std::size_t entries = (count - before.instruments.size()) * 8;

        const auto first_entry = [&before](uint32_t index)
        {
            const auto same_wave = [&before, index](const sar::WaveId& wave)
            {
                return wave.wave_archive_item == before.waves[index].wave_archive_item &&
                       wave.index == before.waves[index].index;
            };
            return static_cast<uint32_t>(std::ranges::find_if(before.waves, same_wave) - before.waves.begin());
        };

        std::size_t end = *to[copies.front().to] + grown - entries;
        for (std::size_t k = copies.size(); k-- > 0;)
        {
            const std::size_t start = *to[copies[k].to];
            const std::size_t size = end - start;
            const std::span<const uint8_t> original = target.file.subspan(*sources[copies[k].program]);
            bool equal = size <= original.size() && size % 4 == 0;
            bool first = false;
            for (std::size_t at = 0; equal && at < size; at += 4)
            {
                const uint32_t a = old.U32(*sources[copies[k].program] + at);
                const uint32_t b = r.U32(start + at);
                first = first || a != b;
                equal = a == b || (a < before.waves.size() && b == first_entry(a));
            }
            totals.same_runs += equal && !first ? 1 : 0;
            totals.first_wave_runs += equal && first ? 1 : 0;
            if (!equal)
            {
                Problem(path, "a copy of an instrument of the same bank holds other bytes", totals);
            }
            end = start;
        }
    }

    totals.copies += static_cast<int>(copies.size());
    totals.new_waves += static_cast<int>(read.waves.size() - before.waves.size());
    for (const sar::InstrumentCopy& copy : copies)
    {
        totals.gap_copies += copy.to < before.instruments.size() ? 1 : 0;
    }
}

} // namespace

void CheckInstruments(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                      const sar::Options& options, Totals& totals)
{
    const sar::SoundArchive archive = sar::SoundArchive::Load(original, options);
    sar::Options strict = options;
    strict.rules = sar::Rules::kStopAtFirstProblem;
    std::vector<ReadBank> banks;
    for (const sar::BankInfo& info : archive.Banks())
    {
        const std::span<const uint8_t> file = archive.FileData(info.file_id);
        std::optional<ReadBank> bank;
        for (const sar::Options& rules : {strict, options})
        {
            try
            {
                bank = ReadBank{file, rules, sar::Bank::Parse(file, rules)};
                break;
            }
            catch (const std::exception&)
            {
                // The next rules try the bank.
            }
        }
        if (!bank)
        {
            ++totals.unread_banks;
            continue;
        }
        if (bank->options.rules != strict.rules)
        {
            ++totals.loose_banks;
        }
        if (std::ranges::none_of(bank->bank.instruments, [](const auto& instrument) { return instrument.has_value(); }))
        {
            ++totals.empty_banks;
            continue;
        }

        banks.push_back(*bank);
    }

    for (std::size_t i = 0; i < banks.size(); ++i)
    {
        try
        {
            if (sar::WithInstruments(banks[i].file, options.flavor, {}) !=
                std::vector(banks[i].file.begin(), banks[i].file.end()))
            {
                Problem(path, "a bank doesn't come back without copies", totals);
            }
            CheckCopies(path, banks[i], banks[(i + 1) % banks.size()], false, totals);
            CheckCopies(path, banks[i], banks[i], true, totals);
            ++totals.instrument_banks;
        }
        catch (const std::exception& error)
        {
            Problem(path, std::string("bank ") + std::to_string(i) + ": " + error.what(), totals);
        }
    }
}

} // namespace hokuto::tools::layout
