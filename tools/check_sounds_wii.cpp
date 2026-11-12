// SPDX-License-Identifier: MIT

// hokuto_check_sounds: the rounds of the Wii's archives.

#include "check_sounds.h"

namespace hokuto::tools::sounds
{

namespace
{

// What the rounds of an archive of the Wii start from: the archive's name and bytes, what ntdlib reads of them for
// playback, the file that they make, and the values that the archive holds.
struct WiiOriginal
{
    std::string name;
    std::vector<uint8_t> bytes;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> archive;
    File file;
    hokuto::edit::HeldValues held;
};

} // namespace

bool Holds(const hokuto::edit::HeldValues& held, const Round& round)
{
    switch (round.value)
    {
    case SoundValue::kActorPlayer:
        return held.actor_player;
    case SoundValue::kPanMode:
    case SoundValue::kPanCurve:
        return held.pan;
    case SoundValue::kReleasePriorityFix:
        return held.release_priority_fix;
    case SoundValue::kBank:
        return round.slot == 0 || held.bank_slots > 1;
    default:
        return true;
    }
}

namespace
{

// The output of sequence sound `sound` of the Wii's `archive` for kLevelSeconds on ntdlib's model of the Wii, with the
// settings of `build`.
std::vector<int16_t> PlayWii(const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive, uint32_t sound,
                             const ntdlib::SndBuild& build)
{
    const auto sequence =
        std::make_shared<const hokuto::song::Sequence>(hokuto::song::ReadSequence(*archive, sound, {}, {}, build));
    hokuto::song::SequenceModel model(archive, sequence);
    const auto samples = static_cast<std::size_t>(kLevelSeconds * model.SampleRate()) * 2;
    std::vector<int16_t> output;
    for (bool more = model.SeekFrame(0); more && output.size() < samples; more = model.RunFrame())
    {
        const std::span<const int16_t> frame = model.Output();
        output.insert(output.end(), frame.begin(), frame.end());
    }

    return output;
}

// The number of bytes in which `exported` differs from `bytes`, outside the INFO block of `bytes`, an archive of the
// Wii. The INFO block's offset and size lie at 0x18 and 0x1C of the header.
std::size_t DifferencesOutsideInfo(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& exported)
{
    const std::size_t info = xsflib::LoadBe32(&bytes[0x18]);
    const std::size_t end = info + xsflib::LoadBe32(&bytes[0x1C]);
    std::size_t differences = 0;
    for (std::size_t i = 0; i < std::min(bytes.size(), exported.size()); ++i)
    {
        differences += bytes[i] != exported[i] && (i < info || i >= end) ? 1 : 0;
    }

    return differences;
}

// The sequences of the files of `bytes`, an archive of the Wii that `file` builds, and the fields of its sounds'
// starts.
Layout WiiLayoutOf(const File& file, const std::vector<uint8_t>& bytes)
{
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> read =
        hokuto::song::LoadWiiArchive(bytes, "export", ntdlib::rvl::snd::kRsarOptions);
    return {hokuto::format::SequencesOfFiles(file, bytes, *read), hokuto::format::StartFieldsOfSounds(file, bytes)};
}

// Checks that `original` refuses each edit of `changes`: the round's value is one that the archive doesn't hold. `what`
// names the value.
void CheckWiiRefusals(const WiiOriginal& original, const NativeTables& tables,
                      const std::vector<std::pair<uint32_t, SoundChange>>& changes, const std::string& what,
                      Totals& totals)
{
    const std::string& name = original.name;
    for (const auto& [sound, change] : changes)
    {
        try
        {
            hokuto::edit::ChangeSound(original.file, sound, change);
            std::printf("%s: %s: the edit of the %s isn't refused.\n", name.c_str(), tables.sounds[sound].name.c_str(),
                        what.c_str());
            ++totals.failures;
        }
        catch (const std::invalid_argument&)
        {
            ++totals.refused;
        }
    }
}

// Checks the archive that `edited`, the file after round `round`, exports: the bytes outside its INFO block, the
// values that it reads back against `expected`, and `layout`. For a round of volumes, sets `after` to the exported
// archive. Returns false when the export fails.
bool CheckWiiExport(const WiiOriginal& original, const Layout& layout, const Round& round, const File& edited,
                    const std::vector<NativeSound>& expected, const std::string& what,
                    std::shared_ptr<const ntdlib::rsar::SoundArchive>& after, Totals& totals)
{
    const std::string& name = original.name;
    std::vector<uint8_t> exported;
    try
    {
        hokuto::tools::CheckSaved(edited);
        exported = hokuto::format::ExportArchive(edited);
        const File read = hokuto::format::ImportWiiArchive(exported);
        if (exported.size() != original.bytes.size() || DifferencesOutsideInfo(original.bytes, exported) != 0)
        {
            std::printf(
                "%s: the export moves bytes, or changes bytes outside its INFO block, after the round of the %s.\n",
                name.c_str(), what.c_str());
            ++totals.differ;
        }
        for (std::size_t sound = 0; sound < expected.size(); ++sound)
        {
            if (!read.native->tables || sound >= read.native->tables->sounds.size() ||
                read.native->tables->sounds[sound] != expected[sound])
            {
                std::printf("%s: %s: the values read back otherwise after the round of the %s.\n", name.c_str(),
                            expected[sound].name.c_str(), what.c_str());
                ++totals.differ;
            }
        }
        ++totals.layouts;
        if (WiiLayoutOf(edited, exported) != layout)
        {
            std::printf("%s: the files' sequences or the starts' fields differ after the round of the %s.\n",
                        name.c_str(), what.c_str());
            ++totals.differ;
        }
        if (round.value == SoundValue::kVolume)
        {
            after = hokuto::song::LoadWiiArchive(exported, name, ntdlib::rvl::snd::kRsarOptions);
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the round of the %s fails: %s\n", name.c_str(), what.c_str(), error.what());
        ++totals.failures;
        return false;
    }

    return true;
}

// Checks that the old values of the edits of `made`, put back in reverse order, give the archive of `original` back.
void CheckWiiEditsBack(const WiiOriginal& original, const NativeTables& tables, const File& edited,
                       const std::vector<std::pair<uint32_t, SoundChange>>& made, const std::string& what,
                       Totals& totals)
{
    const std::string& name = original.name;
    File back = edited;
    try
    {
        for (auto it = made.rbegin(); it != made.rend(); ++it)
        {
            const auto& [sound, change] = *it;
            const uint32_t old = hokuto::edit::ValueOf(tables.sounds[sound], change.value, change.slot);
            back = hokuto::edit::ChangeSound(back, sound, {change.value, change.slot, old});
        }
        if (hokuto::format::ExportArchive(back) != original.bytes)
        {
            std::printf("%s: the old values don't give the archive back after the round of the %s.\n", name.c_str(),
                        what.c_str());
            ++totals.round_trips;
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the edits back of the round of the %s fail: %s\n", name.c_str(), what.c_str(), error.what());
        ++totals.failures;
    }
}

// Checks the levels of the sounds of `made` in `after`, the archive after a round of volumes.
void CheckWiiLevels(const WiiOriginal& original, const std::shared_ptr<const ntdlib::rsar::SoundArchive>& after,
                    const std::vector<std::pair<uint32_t, SoundChange>>& made, Totals& totals)
{
    const std::string& name = original.name;
    const ntdlib::SndBuild build = hokuto::format::BuildOfWiiArchive(*original.file.native);
    const auto at_volume = [&](uint32_t sound, uint32_t volume)
    {
        const File lower = hokuto::edit::ChangeSound(original.file, sound, {SoundValue::kVolume, 0, volume});
        return PlayWii(
            hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(lower), name, ntdlib::rvl::snd::kRsarOptions),
            sound, build);
    };
    CheckLevels(
        name, original.file, [&](uint32_t sound) { return PlayWii(original.archive, sound, build); },
        [&](uint32_t sound) { return PlayWii(after, sound, build); }, at_volume, made, totals);
}

// Makes round `round` on up to kRoundSounds sounds of `original`, an archive of the Wii. A value that the archive
// doesn't hold has to be refused. Otherwise the tool checks the archive that the edits export, and the archive that the
// edits back export. `layout` holds the original's files' sequences and starts' fields. `index` turns the choice of
// sounds from round to round.
void CheckWiiRound(const WiiOriginal& original, const Layout& layout, const Round& round, std::size_t index,
                   Totals& totals)
{
    const std::string& name = original.name;
    const std::string what = hokuto::edit::NameOf(round.value);
    const NativeTables& tables = *original.file.native->tables;
    std::vector<std::pair<uint32_t, SoundChange>> candidates;
    for (uint32_t sound = 0; sound < tables.sounds.size(); ++sound)
    {
        if (const std::optional<SoundChange> change = ChangeOf(tables, tables.sounds[sound], round))
        {
            candidates.emplace_back(sound, *change);
        }
    }
    const std::vector<std::pair<uint32_t, SoundChange>> changes = Spread(candidates, index);

    // A value that the archive doesn't hold.
    if (!Holds(original.held, round))
    {
        CheckWiiRefusals(original, tables, changes, what, totals);
        return;
    }

    // The edits, and the values that the tool works out.
    File edited = original.file;
    std::vector<NativeSound> expected = tables.sounds;
    std::vector<std::pair<uint32_t, SoundChange>> made;
    for (const auto& [sound, change] : changes)
    {
        try
        {
            edited = hokuto::edit::ChangeSound(edited, sound, change);
        }
        catch (const std::invalid_argument& error)
        {
            std::printf("%s: %s: the edit of the %s fails: %s\n", name.c_str(), tables.sounds[sound].name.c_str(),
                        what.c_str(), error.what());
            ++totals.failures;
            continue;
        }

        ++totals.edits;
        expected[sound] = Changed(expected[sound], change);
        made.emplace_back(sound, change);
    }

    // The export holds the new values in place.
    std::shared_ptr<const ntdlib::rsar::SoundArchive> after;
    if (!CheckWiiExport(original, layout, round, edited, expected, what, after, totals))
    {
        return;
    }

    // The old values give the archive back.
    CheckWiiEditsBack(original, tables, edited, made, what, totals);

    if (after)
    {
        CheckWiiLevels(original, after, made, totals);
    }
}

} // namespace

int CheckWiiArchive(const std::string& name, std::vector<uint8_t> bytes)
{
    WiiOriginal original;
    original.name = name;
    original.bytes = std::move(bytes);
    Layout layout;
    try
    {
        original.archive = hokuto::song::LoadWiiArchive(original.bytes, name, ntdlib::rvl::snd::kRsarOptions);
        original.file = hokuto::format::ImportWiiArchive(original.bytes);
        original.file = hokuto::tools::CheckedForm(std::move(original.file), original.bytes);
        if (!original.file.native->tables || hokuto::format::ExportArchive(original.file) != original.bytes)
        {
            throw std::runtime_error("the file holds no tables, or doesn't export the archive");
        }
        original.held = hokuto::edit::HeldValuesOf(original.file);
        layout = WiiLayoutOf(original.file, original.bytes);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    Totals totals;
    totals.sounds = original.file.native->tables->sounds.size();
    for (const NativeSound& sound : original.file.native->tables->sounds)
    {
        totals.sequences += sound.kind == sar::SoundType::kSequence ? 1 : 0;
    }
    for (std::size_t i = 0; i < kRounds.size(); ++i)
    {
        CheckWiiRound(original, layout, kRounds[i], i, totals);
    }

    const uint16_t version = hokuto::format::WiiVersionOf(*original.file.native);
    std::printf(
        "%s: version %d.%d. %zu sounds, %zu of them sequence sounds. %zu edits in %zu rounds, and %zu edits refused of "
        "values that the archive doesn't hold. %zu values read back otherwise, and %zu rounds whose values back give "
        "other bytes. %zu exports checked for the files' sequences and the starts' fields. %zu levels measured, with "
        "the worst %.4f dB from the ratio of the volumes, and %zu further than %.2f dB. Apart: %zu sounds that clip, "
        "%zu too quiet to measure, and %zu whose voices reach the console's limit. %zu failures.\n",
        name.c_str(), version >> 8, version & 0xFF, totals.sounds, totals.sequences, totals.edits, kRounds.size(),
        totals.refused, totals.differ, totals.round_trips, totals.layouts, totals.levels, totals.worst,
        totals.wrong_levels, kTolerance, totals.clipped, totals.quiet, totals.limited, totals.failures);

    return totals.differ == 0 && totals.round_trips == 0 && totals.wrong_levels == 0 && totals.failures == 0 ? 0 : 1;
}

} // namespace hokuto::tools::sounds
