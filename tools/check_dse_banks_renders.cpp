// SPDX-License-Identifier: MIT

// The renders of hokuto_check_dse_banks: the level and the pitch of a note after an edit.

#include "check_dse_banks.h"

namespace hokuto::tools::dse_banks
{

namespace
{

std::vector<float> RenderNote(const DseSet& set, uint32_t bank, uint32_t program, int key)
{
    auto release = std::make_shared<std::atomic<bool>>(false);
    hokuto::song::DseNotePreview preview(set.banks.at(bank), static_cast<int>(program), key, kNoteVelocity, release,
                                         set.build);
    const auto frames = static_cast<std::size_t>(preview.SampleRate() * kRenderSeconds);
    std::vector<float> out(frames * 2);
    const std::size_t rendered = preview.Render(out.data(), frames);
    out.resize(rendered * 2);

    return out;
}

// The rate of a render. Each preview of the model plays at the rate of the sound unit's mix.
int RateOf(const DseSet& set, uint32_t bank, uint32_t program, int key)
{
    auto release = std::make_shared<std::atomic<bool>>(false);

    return hokuto::song::DseNotePreview(set.banks.at(bank), static_cast<int>(program), key, kNoteVelocity, release,
                                        set.build)
        .SampleRate();
}

// The stereo frames up to the last one that sounds.
std::size_t SoundingFrames(std::span<const float> render)
{
    std::size_t frames = render.size() / 2;
    while (frames > 0 && std::abs(render[frames * 2 - 1]) < 1e-5f && std::abs(render[frames * 2 - 2]) < 1e-5f)
    {
        frames--;
    }

    return frames;
}

double LevelDb(std::span<const float> render, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t i = from * 2; i < to * 2; ++i)
    {
        sum += static_cast<double>(render[i]) * render[i];
    }

    return 10.0 * std::log10(std::max(sum / static_cast<double>((to - from) * 2), 1e-20));
}

std::size_t Crossings(std::span<const float> render, std::size_t from, std::size_t to)
{
    std::size_t count = 0;
    double last = 0.0;
    for (std::size_t frame = from; frame < to; ++frame)
    {
        const double mono = static_cast<double>(render[frame * 2]) + render[frame * 2 + 1];
        if (mono != 0.0)
        {
            count += last * mono < 0.0 ? 1 : 0;
            last = mono;
        }
    }

    return count;
}

std::optional<Note> NoteOf(const DseSet& set, uint32_t bank, uint32_t program)
{
    const dse::Program& held = *set.banks[bank].bank->instruments->programs[program];
    for (std::size_t split = 0; split < held.splits.size(); ++split)
    {
        const DseSplit& values = held.splits[split];
        if (values[edit::kDseHighVelocity] < kNoteVelocity)
        {
            continue;
        }

        const int key = std::clamp(60, int{values[edit::kDseLowKey]}, int{values[edit::kDseHighKey]});
        const auto plays = [key](const DseSplit& other)
        {
            return other[edit::kDseLowKey] <= key && key <= other[edit::kDseHighKey] &&
                   other[edit::kDseLowVelocity] <= kNoteVelocity && kNoteVelocity <= other[edit::kDseHighVelocity];
        };
        if (std::ranges::count_if(held.splits, plays) == 1)
        {
            return Note{bank, program, split, key};
        }
    }

    return std::nullopt;
}

// Checks that `after` plays the samples of `before` at half the speed. A stretch of `before` has to contain the
// crossings of the stretch of `after` that plays the same samples: twice as late and twice as long.
void CheckOctaveDown(const Game& game, const Note& note, std::span<const float> before, std::span<const float> after,
                     int rate, const char* what, std::size_t& counted, Totals& totals)
{
    const auto from = static_cast<std::size_t>(kMeasureFrom * rate);
    const std::size_t to = std::min(SoundingFrames(before), SoundingFrames(after) / 2);
    if (to < from + static_cast<std::size_t>(kShortestMeasure * rate))
    {
        totals.pitches_apart++;
        return;
    }

    const std::size_t first = Crossings(before, from, to);
    const std::size_t second = Crossings(after, 2 * from, 2 * to);
    if (first < kFewestCrossings)
    {
        totals.pitches_apart++;
        return;
    }

    counted++;
    const double ratio = static_cast<double>(second) / static_cast<double>(first);
    totals.lowest_ratio = std::min(totals.lowest_ratio, ratio);
    totals.highest_ratio = std::max(totals.highest_ratio, ratio);
    if (ratio < kLeastOctaveRatio || ratio > kMostOctaveRatio)
    {
        Fail(game, totals,
             std::string(what) + ": program " + std::to_string(note.program) + " of " +
                 hokuto::song::DseBankName(*game.set->banks[note.bank].bank) + " at key " + std::to_string(note.key) +
                 " holds " + std::to_string(second) + " crossings in place of " + std::to_string(first));
    }
}

// A note of each program of the banks of `set`. A program without one counts apart for a level and for a pitch.
std::vector<Note> CollectNotes(const DseSet& set, Totals& totals)
{
    std::vector<Note> notes;
    for (const uint32_t bank : DistinctBanks(set))
    {
        const dse::WaveBank& held = *set.banks[bank].bank;
        if (!held.instruments)
        {
            continue;
        }

        for (uint32_t program = 0; program < held.instruments->programs.size(); ++program)
        {
            if (!held.instruments->programs[program])
            {
                continue;
            }
            if (const std::optional<Note> note = NoteOf(set, bank, program))
            {
                notes.push_back(*note);
            }
            else
            {
                totals.levels_apart++;
                totals.pitches_apart++;
            }
        }
    }

    return notes;
}

// Checks that `note` falls by the square of the ratio of its levels with half the volume of its split. `before` is its
// render at `rate`, measured from sample `from`.
void CheckHalvedVolume(const Game& game, const Note& note, std::span<const float> before, int rate, std::size_t from,
                       Totals& totals)
{
    const DseSet& set = *game.set;
    const dse::WaveBank& held = *set.banks[note.bank].bank;
    const dse::Program& program = *held.instruments->programs[note.program];
    const DseSplit& old = program.splits[note.split];
    // The driver keeps a level made of the velocity, the volume of the program and the volume of the split, and squares
    // it.
    const bool moved_by_lfo = std::ranges::any_of(
        program.lfos, [](const auto& lfo) { return lfo[kLfoType] != 0 && lfo[kLfoOutput] == kLevelOutput; });
    const int old_level = kNoteVelocity * program.header[4] * old[edit::kDseVolume] / (127 * 127);
    const std::size_t to = SoundingFrames(before);
    const bool loud_enough =
        to > from + static_cast<std::size_t>(kShortestMeasure * rate) && LevelDb(before, from, to) >= kQuietestDb;
    const bool full = std::ranges::any_of(before, [](float sample) { return std::abs(sample) >= 0.99f; });
    const int volume = old[edit::kDseVolume] / 2;
    const int new_level = kNoteVelocity * program.header[4] * volume / (127 * 127);
    if (moved_by_lfo || !loud_enough || full || new_level == 0)
    {
        totals.levels_apart++;
    }
    else
    {
        DseSplit quieter = old;
        quieter[edit::kDseVolume] = static_cast<uint8_t>(volume);
        const File edited =
            edit::WithDseSplit(game.file, set, {note.bank, note.program, note.split}, quieter, game.starts);
        const std::shared_ptr<const DseSet> read =
            hokuto::song::LoadDseSet(hokuto::format::ExportArchive(edited), game.build);
        const std::vector<float> after = RenderNote(*read, note.bank, note.program, note.key);
        const std::size_t end = std::min({to, before.size() / 2, after.size() / 2});
        const double fell = LevelDb(after, from, end) - LevelDb(before, from, end);
        const double want = 40.0 * std::log10(static_cast<double>(new_level) / old_level);
        totals.levels++;
        if (std::abs(fell - want) > kLevelTolerance)
        {
            Fail(game, totals,
                 "program " + std::to_string(note.program) + " of " + hokuto::song::DseBankName(held) + " at " +
                     std::to_string(LevelDb(before, from, end)) + " dB falls by " + std::to_string(-fell) +
                     " dB and should fall by " + std::to_string(-want));
        }
    }
}

// Renders `note` with half the volume of its split, then an octave down by its root key and by half the rate of its
// wave.
void CheckNote(const Game& game, const Note& note, Totals& totals)
{
    const DseSet& set = *game.set;
    const dse::WaveBank& held = *set.banks[note.bank].bank;
    const dse::Program& program = *held.instruments->programs[note.program];
    const DseSplit& old = program.splits[note.split];
    const std::vector<float> before = RenderNote(set, note.bank, note.program, note.key);
    const int rate = RateOf(set, note.bank, note.program, note.key);
    const auto from = static_cast<std::size_t>(kMeasureFrom * rate);

    CheckHalvedVolume(game, note, before, rate, from, totals);

    const uint32_t wave = Get16(old, edit::kDseWave);
    const bool has_wave = wave < held.waves.size() && held.waves[wave];
    if (!has_wave || held.waves[wave]->bytes[kWaveFormat] == kNoise)
    {
        totals.pitches_apart++;
        return;
    }

    // A note above the mix's rate drops samples, and the dropped samples' crossings with them. Key 60 at no tuning
    // plays at the mix's rate.
    const int root = edit::DseRootKey(old);
    const int key = std::clamp(root - 12, int{old[edit::kDseLowKey]}, int{old[edit::kDseHighKey]});
    const auto plays_key = [key](const DseSplit& other)
    {
        return other[edit::kDseLowKey] <= key && key <= other[edit::kDseHighKey] &&
               other[edit::kDseLowVelocity] <= kNoteVelocity && kNoteVelocity <= other[edit::kDseHighVelocity];
    };
    const int pitch = (key + static_cast<int8_t>(old[edit::kDseCoarseTuning])) * 256 + edit::DseTuning(old);
    if (pitch > 60 * 256 || std::ranges::count_if(program.splits, plays_key) != 1)
    {
        totals.pitches_apart++;
        return;
    }

    // An envelope acts in time and not in samples. The renders of the pitches therefore hold a level.
    const edit::DseSplitPlace place{note.bank, note.program, note.split};
    DseSplit steady = old;
    steady[edit::kDseAttackLevel] = kHighest;
    steady[edit::kDseAttack] = 0;
    steady[edit::kDseDecay] = 0;
    steady[edit::kDseSustain] = kHighest;
    steady[edit::kDseHold] = 0;
    steady[edit::kDseDecay2] = kHighest;
    const File held_file = edit::WithDseSplit(game.file, set, place, steady, game.starts);
    const std::shared_ptr<const DseSet> held_set =
        hokuto::song::LoadDseSet(hokuto::format::ExportArchive(held_file), game.build);
    const Note low_note{note.bank, note.program, note.split, key};
    const std::vector<float> low = RenderNote(*held_set, note.bank, note.program, key);
    if (root + 12 <= kHighest && root >= 0)
    {
        DseSplit lower = steady;
        edit::SetDseRootKey(lower, root + 12);
        const File edited = edit::WithDseSplit(held_file, *held_set, place, lower, game.starts);
        const std::shared_ptr<const DseSet> read =
            hokuto::song::LoadDseSet(hokuto::format::ExportArchive(edited), game.build);
        const std::vector<float> after = RenderNote(*read, note.bank, note.program, key);
        CheckOctaveDown(game, low_note, low, after, rate, "a root key an octave higher", totals.root_pitches, totals);
    }
    else
    {
        totals.pitch_checks_left_out++;
    }

    const uint8_t format = held.waves[wave]->bytes[kWaveFormat];
    if (format != kPcm8 && format != kPcm16 && format != kImaAdpcm)
    {
        totals.pitch_checks_left_out++;
        return;
    }

    // The edit applies to the wave of the bank that contains the samples. Each table moves the tunings of its splits by
    // the semitones between the rate that it held and the new one. The table of the note's bank can give the wave
    // another rate than the table of the bank with the samples. The new rate is half that rate.
    const auto is_main = [&](const hokuto::song::DseBank& other)
    {
        return other.bank == set.banks[note.bank].main_bank;
    };
    const uint32_t source = set.banks[note.bank].main_bank
                                ? static_cast<uint32_t>(std::ranges::find_if(set.banks, is_main) - set.banks.begin())
                                : note.bank;
    edit::DseWaveValues slower = edit::DseWaveValuesOf(set, source, wave);
    const uint32_t half = held.waves[wave]->SampleRate() / 2;
    if (half < edit::kLeastDseRate || half > edit::kHighestDseRate || half == slower.rate)
    {
        totals.pitches_apart++;
        return;
    }

    slower.rate = half;
    const File edited = edit::WithDseWave(held_file, *held_set, source, wave, slower, game.starts);
    const std::shared_ptr<const DseSet> read =
        hokuto::song::LoadDseSet(hokuto::format::ExportArchive(edited), game.build);
    const std::vector<float> after = RenderNote(*read, note.bank, note.program, key);
    CheckOctaveDown(game, low_note, low, after, rate, "half a wave's rate", totals.rate_pitches, totals);
}

} // namespace

void Renders(const Game& game, Totals& totals)
{
    const DseSet& set = *game.set;
    const std::vector<Note> notes = CollectNotes(set, totals);

    // Every nth note, with n the count of notes divided by kMostPrograms and rounded down: every note of up to 255, and
    // 128 to 192 of more.
    const std::size_t step = std::max<std::size_t>(notes.size() / kMostPrograms, 1);
    for (std::size_t i = 0; i < notes.size(); i += step)
    {
        CheckNote(game, notes[i], totals);
    }
    std::fflush(stdout);
}

} // namespace hokuto::tools::dse_banks
