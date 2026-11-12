// SPDX-License-Identifier: MIT

// The imports from a donor of hokuto_check_replace on archives of the Wii.
//
// The imports and replacements of a later console's files in an archive of the Wii. The donor is an archive of the 3DS,
// the Wii U or the Switch.

#include "check_replace.h"

namespace hokuto::tools::replace
{

namespace
{

// The Wii's file after an edit, the archive that it builds, and the archive's model.
struct WiiState
{
    format::File file;
    std::vector<uint8_t> bytes;
    WiiArchive archive;
    ntdlib::rsar::ArchiveModel model;
};

// `file` with the archive that it builds and the archive's model.
WiiState WiiStateOf(format::File file)
{
    WiiState state{std::move(file), {}, {}, {}};
    hokuto::tools::CheckSaved(state.file, true);
    state.bytes = format::ExportArchive(state.file);
    state.archive = song::LoadWiiArchive(state.bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    state.model = ntdlib::rsar::ReadArchiveModel(state.bytes);

    return state;
}

// What the checks with a donor counted.
struct WiiDonorTotals
{
    std::size_t imports = 0;
    std::size_t failed = 0;
    std::size_t sounds = 0;                     // sequence sounds that came in with their banks
    std::size_t same_samples = 0;               // of them, those that play the samples of Another Console's conversion
    std::size_t sounding = 0;                   // of them, those whose renders hold sound
    std::size_t wave_archives = 0;              // wave archives that came in as banks that hold their waves
    std::size_t waves = 0;                      // waves of those wave archives, and new waves
    std::size_t banks = 0;                      // banks that came in, or took a bank's place
    std::size_t wave_sounds = 0;                // new wave sounds
    std::size_t files = 0;                      // files of the Wii archive that took a donor's file in their place
    std::size_t copies = 0;                     // copies of those files in the groups
    std::size_t unlabelled = 0;                 // donor's sounds that start where their files have no label
    std::size_t without_bank = 0;               // donor's sounds without a bank in their first slot
    std::size_t lacked = 0;                     // donor's items whose files the donor lacks
    std::size_t defaults = 0;                   // conversions that take the default pan mode and curve before 1.2
    std::size_t several_slots = 0;              // donor's sounds whose conversions take the banks of several slots
    std::size_t trimmed = 0;                    // donor's sounds whose conversions keep only the selected instruments
    std::map<std::string, std::size_t> refused; // refusals, by their reasons
};

// Whether `wii`, a wave of the Wii that a wave of a later console became, keeps `later`'s rate, its loop and the
// samples of its first two channels.
bool KeepsWave(const ntdlib::rsar::WaveFile& wii, const sar::WaveFile& later)
{
    const std::size_t channels = std::min<std::size_t>(later.channels.size(), 2);
    if (wii.sample_rate != later.sample_rate || wii.loop != later.loop || wii.loop_end != later.loop_end ||
        (later.loop && wii.loop_start != later.loop_start) || wii.channels.size() != channels)
    {
        return false;
    }
    std::vector<std::vector<int16_t>> samples = edit::DecodedSamples(later, later.loop_end);
    samples.resize(channels);

    return edit::DecodedSamples(wii, wii.loop_end) == samples;
}

// Whether `wii`, the samples of a wave sound of the Wii that a wave sound of a later console became, keeps `later`'s
// rate, its loop and its first two channels.
bool KeepsPcm(const sar::Pcm& wii, sar::Pcm later)
{
    later.channels.resize(std::min<std::size_t>(later.channels.size(), 2));

    return wii == later;
}

// The bytes of `item` of the donor, or nothing when the donor lacks its file.
std::optional<std::vector<uint8_t>> DonorBytes(const sar::SoundArchive& donor, const song::Item& item)
{
    try
    {
        const std::span<const uint8_t> bytes = song::ItemFile(donor, item);
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    }
    catch (const std::runtime_error&)
    {
        return std::nullopt;
    }
}

// Wave archive `id` of the donor, an item ID, in the Wii's `state` as a bank without instruments that holds its waves:
// File > Import > Wave Archive… on the Wii. The waves have to keep the donor's samples. Gives the new bank's file ID.
// Throws a Refusal for a wave archive that the Wii can't take, a LeftOut for a wave archive that the donor lacks, and
// std::runtime_error with a problem.
uint32_t ImportDonorWaves(WiiState& state, const sar::SoundArchive& donor, uint32_t id, WiiDonorTotals& totals)
{
    const std::optional<std::vector<uint8_t>> bytes =
        DonorBytes(donor, {.kind = song::ItemKind::kWaveArchive, .index = id & 0xFFFFFF});
    if (!bytes)
    {
        throw LeftOut("lacks wave archive " + std::to_string(id & 0xFFFFFF));
    }
    const edit::IncomingFile incoming =
        Refusable([&] { return edit::ReadWiiIncomingFile(*bytes, edit::FileKind::kWaveArchive); });
    std::vector<ntdlib::rsar::WaveFile> waves;
    for (const std::vector<uint8_t>& wave : ntdlib::rsar::WaveArchiveFile::Read(incoming.bytes).waves)
    {
        waves.push_back(ntdlib::rsar::WaveFile::Read(wave));
    }
    const sar::WaveArchiveFile later = sar::WaveArchiveFile::Read(*bytes);
    if (waves.size() != later.waves.size())
    {
        throw std::runtime_error("wave archive " + std::to_string(id & 0xFFFFFF) + " keeps another number of waves");
    }
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        if (!KeepsWave(waves[i], sar::WaveFile::Read(later.waves[i])))
        {
            throw std::runtime_error("wave " + std::to_string(i) + " of wave archive " + std::to_string(id & 0xFFFFFF) +
                                     " changes");
        }
    }

    const ntdlib::rsar::BankFile::Files files = Refusable([&] { return edit::EmptyBank(*state.archive, waves); });
    const std::size_t before = state.model.files.size();
    const auto add_bank = [&]
    {
        return edit::AddWiiBank(state.file, "HOKUTO_CHECK_WARC_" + std::to_string(id & 0xFFFFFF), files.file,
                                files.wave_data, {});
    };
    state = WiiStateOf(Refusable(add_bank));
    totals.imports++;
    totals.wave_archives++;
    totals.waves += waves.size();
    const uint32_t file_id = state.archive->Banks().back().file_id;
    if (state.model.files.size() != before + 1 || file_id != before || edit::WavesOf(*state.archive, file_id) != waves)
    {
        throw std::runtime_error("wave archive " + std::to_string(id & 0xFFFFFF) + " doesn't come in with its waves");
    }

    return file_id;
}

// The wave archives that `bytes`, a bank file or a wave sound file of the donor, names, in the Wii's `state` as banks
// that hold their waves (ImportDonorWaves). Gives the file of each by the wave archive's item ID.
std::map<uint32_t, uint32_t> ImportNamedWaves(WiiState& state, const sar::SoundArchive& donor,
                                              std::span<const uint8_t> bytes, WiiDonorTotals& totals)
{
    std::map<uint32_t, uint32_t> map;
    const std::vector<edit::NamedWaveArchive> names =
        Refusable([&] { return edit::WaveArchivesNamed(bytes, donor.ReadOptions().flavor); });
    for (const edit::NamedWaveArchive& named : names)
    {
        map[named.id] = ImportDonorWaves(state, donor, named.id, totals);
    }

    return map;
}

// The reasons why the check leaves out a donor's sound whose conversion File > Import can't match: the import brings in
// the bank of one slot, whole.
constexpr std::string_view kSeveralSlots = "plays the banks of several slots";
constexpr std::string_view kTrimmed = "keeps only the instruments that it selects";

// What is wrong when sequence sound `sound` of the donor comes into the Wii's `file` with the bank of its first slot
// and the wave archives that the bank names, or nothing. The new sound copies the values of the archive's first
// sequence sound `model`, and then takes the volume, the pan mode and the pan curve of the sound that Another Console
// makes for `build` (edit::ForWii). It has to play that sound's samples for `seconds` by the rules of `build`. An
// archive before version 1.2 holds no pan mode and no pan curve. NW4R SND then pans in balance mode, with the
// square-root curve (ntdlib::rsar::SoundInfo). A conversion with other values takes those for the reference, and
// `totals` counts it. A conversion that takes the banks of several slots, or keeps only the instruments that the sound
// selects, has no import to match. Throws a Refusal for an import that Hokuto refuses, and a LeftOut for a sound that
// the check leaves out.
std::string DonorSoundProblem(const format::File& file, const sar::SoundArchive& donor, const format::File& donor_file,
                              uint32_t sound, uint32_t model, const ntdlib::SndBuild& build, double seconds,
                              WiiDonorTotals& totals)
{
    const sar::SoundInfo& info = donor.Sounds()[sound];
    if (info.banks.empty() || info.banks[0] == sar::kNoItem)
    {
        throw LeftOut("without a bank in its first slot");
    }
    const std::optional<std::vector<uint8_t>> bank_bytes =
        DonorBytes(donor, {.kind = song::ItemKind::kBank, .index = info.banks[0] & 0xFFFFFF});
    const std::optional<std::vector<uint8_t>> sequence_bytes =
        DonorBytes(donor, {.kind = song::ItemKind::kSequence, .index = sound});
    if (!bank_bytes || !sequence_bytes)
    {
        throw LeftOut("lacks its files");
    }

    // The sound's start: a label of its file, or the file's first command when the file has no label.
    const edit::Incoming read = Refusable([&] { return edit::ReadIncoming(*sequence_bytes); });
    const std::optional<uint32_t> item =
        edit::ItemAtOffset(edit::Offsets(read.sequence, song::Console{.engine = hokuto::format::EngineOf(read.flavor)}),
                           info.start_offset);
    std::optional<std::string> label;
    for (const ntdlib::commands::Label& named : read.sequence.labels)
    {
        label = item && named.item == *item ? std::optional(named.name) : label;
    }
    if (!label && !(read.sequence.labels.empty() && info.start_offset == 0))
    {
        throw LeftOut("starts where its file has no label");
    }

    // The reference: the sound that Another Console makes for the archive's build.
    edit::OtherConsoleSong other = Refusable([&] { return edit::ForWii(donor_file, sound, build); });
    if (!other.slot_programs.empty())
    {
        throw LeftOut(std::string(kSeveralSlots));
    }
    const auto kept_only = [](const std::string& warning)
    {
        return warning.starts_with("The bank keeps only the instruments");
    };
    if (std::ranges::any_of(other.warnings, kept_only))
    {
        throw LeftOut(std::string(kTrimmed));
    }
    const ntdlib::rsar::SoundModel converted =
        ntdlib::rsar::ReadArchiveModel(format::ExportArchive(other.file)).sounds.at(0);

    // The wave archives, the bank and the sound.
    WiiState state = WiiStateOf(file);
    const std::map<uint32_t, uint32_t> map = ImportNamedWaves(state, donor, *bank_bytes, totals);
    const sar::Flavor flavor = donor.ReadOptions().flavor;
    const auto convert_bank = [&]
    {
        return edit::WiiFileOf(edit::ReadIncomingFile(*bank_bytes, edit::FileKind::kBank, flavor), flavor,
                               *state.archive, map, edit::NewWiiFileVersion(*state.archive, edit::FileKind::kBank));
    };
    const edit::IncomingFile wii_bank = Refusable(convert_bank);
    const auto add_bank = [&]
    {
        return edit::AddWiiBank(state.file, "HOKUTO_CHECK_BANK", wii_bank.bytes, wii_bank.wave_data, {});
    };
    state = WiiStateOf(Refusable(add_bank));
    totals.imports++;
    totals.banks++;
    const auto bank = static_cast<uint32_t>(state.archive->Banks().size() - 1);
    edit::Incoming incoming = Refusable(
        [&]
        {
            return edit::ReadIncoming(*sequence_bytes,
                                      song::Console{.engine = hokuto::format::Engine::kWii, .wii = build});
        });
    uint32_t start = 0;
    for (const ntdlib::commands::Label& named : incoming.sequence.labels)
    {
        start = label && named.name == *label ? named.item : start;
    }
    start = Refusable([&] { return edit::WithLaterGlobals(incoming.sequence, start).first; });
    const std::size_t sounds = state.model.sounds.size();
    const auto add_sound = [&]
    {
        return edit::AddWiiSequenceSound(state.file, model, "HOKUTO_CHECK_SEQ", incoming.sequence, start, {},
                                         std::vector<uint32_t>{sar::ItemId(sar::kBankItem, bank)}, {});
    };
    state = WiiStateOf(Refusable(add_sound));
    totals.imports++;
    const auto added = static_cast<uint32_t>(sounds);
    if (state.model.sounds.size() != sounds + 1 || state.model.sounds.back().bank != bank)
    {
        return "doesn't add the sound with its bank";
    }

    // The conversion's values. Before version 1.2, the conversion takes the balance mode and the square-root curve.
    format::File valued = edit::ChangeSound(state.file, added, {edit::SoundValue::kVolume, 0, converted.volume});
    if (edit::HeldValuesOf(valued).pan)
    {
        valued = edit::ChangeSound(valued, added, {edit::SoundValue::kPanMode, 0, converted.pan_mode});
        valued = edit::ChangeSound(valued, added, {edit::SoundValue::kPanCurve, 0, converted.pan_curve});
    }
    else if (converted.pan_mode != 1 || converted.pan_curve != 0)
    {
        other.file = edit::ChangeSound(other.file, 0, {edit::SoundValue::kPanMode, 0, 1});
        other.file = edit::ChangeSound(other.file, 0, {edit::SoundValue::kPanCurve, 0, 0});
        totals.defaults++;
    }
    state = WiiStateOf(std::move(valued));
    totals.sounds++;
    const WiiArchive reference =
        song::LoadWiiArchive(format::ExportArchive(other.file), "other.brsar", ntdlib::rvl::snd::kRsarOptions);

    // Both play by the rules of the archive's build.
    const std::atomic<bool> kCancel{false};
    const auto render = [&build, &kCancel, seconds](const WiiArchive& archive, uint32_t which)
    {
        const auto sequence =
            std::make_shared<const song::Sequence>(song::ReadSequence(*archive, which, {}, {}, build));
        return song::RenderDry(archive, sequence, song::Mixing::kStrict, seconds, kCancel).samples;
    };
    const std::vector<float> expected = render(reference, 0);
    const std::vector<float> samples = render(state.archive, added);
    totals.sounding += std::ranges::any_of(expected, [](float sample) { return sample != 0.0f; }) ? 1 : 0;
    if (samples != expected)
    {
        return "doesn't play the samples of Another Console's conversion";
    }
    totals.same_samples++;

    return {};
}

// What is wrong when wave sound file `set` of the donor comes into the Wii's `file` with the wave archives that its
// waves name, or nothing. The new wave sounds copy the values of the Wii's sound `model`. Each has to play the wave of
// the donor's wave sound at the same rate: the first two channels of its samples, with its loop. Throws a Refusal for
// an import that Hokuto refuses, and a LeftOut for a file that the check leaves out.
std::string DonorWaveSoundsProblem(const format::File& file, const sar::SoundArchive& donor,
                                   const song::WaveSoundSet& set, uint32_t model, WiiDonorTotals& totals)
{
    const std::span<const uint8_t> data = donor.FileData(set.file_id);
    if (data.empty())
    {
        throw LeftOut("lacks its file");
    }
    const std::vector<uint8_t> bytes(data.begin(), data.end());
    WiiState state = WiiStateOf(file);
    const std::map<uint32_t, uint32_t> map = ImportNamedWaves(state, donor, bytes, totals);
    const sar::Flavor flavor = donor.ReadOptions().flavor;
    const auto convert_file = [&]
    {
        return edit::WiiFileOf(edit::ReadIncomingFile(bytes, edit::FileKind::kWaveSoundFile, flavor), flavor,
                               *state.archive, map,
                               edit::NewWiiFileVersion(*state.archive, edit::FileKind::kWaveSoundFile));
    };
    const edit::IncomingFile wii = Refusable(convert_file);
    const std::size_t sounds = state.model.sounds.size();
    const auto add_wave_sounds = [&]
    {
        return edit::AddWiiWaveSounds(state.file, model, "HOKUTO_CHECK_WSD", wii.bytes, wii.wave_data, {});
    };
    state = WiiStateOf(Refusable(add_wave_sounds));
    totals.imports++;
    for (const uint32_t sound : set.sounds)
    {
        const auto placed = static_cast<uint32_t>(sounds + donor.Sounds()[sound].wave_sound_index);
        if (placed >= state.model.sounds.size() ||
            state.model.sounds[placed].type != ntdlib::rsar::SoundType::kWaveSound)
        {
            return "doesn't add wave sound " + donor.Sounds()[sound].name;
        }
        if (!KeepsPcm(song::DecodeWaveSound(*state.archive, placed), song::DecodeWaveSound(donor, sound)))
        {
            return "doesn't play the wave of " + donor.Sounds()[sound].name;
        }
        totals.wave_sounds++;
    }

    return {};
}

// What is wrong when the first wave of the donor's wave archive `index` comes into the Wii's `file` after the waves of
// the wave data of file `id`, and in the place of its first wave, or nothing. The new wave has to keep the donor's
// rate, loop and samples, and the other waves their values. Throws a Refusal for a wave that Hokuto refuses, and a
// LeftOut for a wave that the check leaves out.
std::string DonorWaveProblem(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                             const sar::SoundArchive& donor, uint32_t index, uint32_t id, WiiDonorTotals& totals)
{
    const std::optional<std::vector<uint8_t>> bytes =
        DonorBytes(donor, {.kind = song::ItemKind::kWave, .index = index, .wave = 0});
    if (!bytes)
    {
        throw LeftOut("lacks its file");
    }
    const sar::WaveFile later = sar::WaveFile::Read(*bytes);
    const edit::IncomingWave incoming = Refusable([&] { return edit::ReadIncomingWiiWaveFile(*bytes); });
    if (!incoming.converted || incoming.converted->flavor != donor.ReadOptions().flavor)
    {
        return "doesn't go in converted";
    }
    const std::vector<ntdlib::rsar::WaveFile> present = Refusable([&] { return edit::WavesOf(*built, id); });
    for (const std::optional<uint32_t> wave : {std::optional<uint32_t>(), std::optional<uint32_t>(0)})
    {
        if (wave && present.empty())
        {
            continue;
        }

        const format::File replaced =
            Refusable([&] { return edit::WithIncomingWiiWave(file, *built, id, wave, incoming); });
        hokuto::tools::CheckSaved(replaced, true);
        totals.imports++;
        const std::vector<uint8_t> after_bytes = format::ExportArchive(replaced);
        const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
        const std::vector<ntdlib::rsar::WaveFile> waves = edit::WavesOf(*after, id);
        const std::size_t at = wave ? 0 : present.size();
        if (waves.size() != present.size() + (wave ? 0 : 1) || !KeepsWave(waves[at], later))
        {
            return wave ? "doesn't take the wave in the place of the first" : "doesn't take the wave after the waves";
        }
        for (std::size_t i = 0; i < present.size(); i++)
        {
            if (i != at && waves[i] != present[i])
            {
                return "changes wave " + std::to_string(i);
            }
        }
        if (std::string problem = OtherWiiFilesProblem(built_bytes, *built, after_bytes, *after, id, true, true);
            !problem.empty())
        {
            return problem;
        }
        totals.waves++;
    }

    return {};
}

// What is wrong when the file of the Wii's bank or wave sound `item`, of file `id`, takes `bytes`, a file of `kind` of
// the donor, in its place, or nothing. Each wave archive that the donor's file names maps to the present file's wave
// data. That is the Replace window's first proposal. Every copy of the file and of its wave data has to take the new
// bytes, and every other file has to keep its bytes. Throws a Refusal for a replacement that Hokuto refuses.
std::string DonorFileProblem(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                             edit::FileKind kind, uint32_t item, uint32_t id, std::span<const uint8_t> bytes,
                             sar::Flavor flavor, WiiDonorTotals& totals)
{
    std::map<uint32_t, uint32_t> map;
    for (const edit::NamedWaveArchive& named : Refusable([&] { return edit::WaveArchivesNamed(bytes, flavor); }))
    {
        map[named.id] = id;
    }
    const auto convert_file = [&]
    {
        return edit::WiiFileOf(edit::ReadIncomingFile(bytes, kind, flavor), flavor, *built, map,
                               edit::PresentWiiVersion(*built, kind, item));
    };
    const edit::IncomingFile wii = Refusable(convert_file);
    const format::File replaced = Refusable([&] { return edit::ReplaceWiiFile(file, *built, item, wii); });
    hokuto::tools::CheckSaved(replaced, true);
    totals.imports++;
    const std::vector<uint8_t> after_bytes = format::ExportArchive(replaced);
    const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    for (const bool wave_data : {false, true})
    {
        const std::vector<std::vector<uint8_t>> copies = WiiCopies(after_bytes, *after, id, wave_data);
        if (copies.size() != WiiCopies(built_bytes, *built, id, wave_data).size())
        {
            return "changes the number of copies";
        }
        for (const std::vector<uint8_t>& copy : copies)
        {
            if (copy != PaddedForWii(wave_data ? wii.wave_data : wii.bytes))
            {
                return wave_data ? "leaves a copy with other wave data" : "leaves a copy with other bytes";
            }

            totals.copies += wave_data ? 0 : 1;
        }
    }
    if (std::string problem = OtherWiiFilesProblem(built_bytes, *built, after_bytes, *after, id, true, true);
        !problem.empty())
    {
        return problem;
    }
    totals.files++;
    totals.banks += kind == edit::FileKind::kBank ? 1 : 0;

    return {};
}

// Runs `check` of `item` in archive `name`, and counts the outcome in `totals`.
// An attempt gives the reason why the check leaves the item out, or nothing.
template <typename Check>
std::string Attempt(const std::string& name, const std::string& item, const Check& check, WiiDonorTotals& totals)
{
    try
    {
        if (const std::string problem = check(); !problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), item.c_str(), problem.c_str());
            totals.failed++;
        }
    }
    catch (const Refusal& error)
    {
        totals.refused[error.what()]++;
    }
    catch (const LeftOut& error)
    {
        const std::string why = error.what();
        totals.unlabelled += why == "starts where its file has no label" ? 1 : 0;
        totals.without_bank += why == "without a bank in its first slot" ? 1 : 0;
        totals.lacked += why.starts_with("lacks") ? 1 : 0;
        totals.several_slots += why == kSeveralSlots ? 1 : 0;
        totals.trimmed += why == kTrimmed ? 1 : 0;
        return why;
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s fails: %s\n", name.c_str(), item.c_str(), error.what());
        totals.failed++;
    }

    return {};
}

// Up to 8 of the donor's sounds, wave sound files or wave archives, spread over the donor's tables.
std::vector<uint32_t> Spread(std::size_t count)
{
    std::vector<uint32_t> chosen;
    for (const std::size_t index : SpreadIndices(count, 8))
    {
        chosen.push_back(static_cast<uint32_t>(index));
    }

    return chosen;
}

// The donor's sequence sounds as new sounds of `file`, with the first sequence sound of `sequences` as the model.
// `build` is the build of NW4R SND of the archive that `file` builds, and `donor_file` the file of `donor`. Counts the
// sounds in `totals`.
void ImportDonorSounds(const format::File& file, const Archive& donor, const format::File& donor_file,
                       const std::vector<uint32_t>& sequences, const ntdlib::SndBuild& build, const std::string& name,
                       WiiDonorTotals& totals)
{
    std::vector<uint32_t> donor_sounds;
    for (uint32_t s = 0; s < donor->Sounds().size(); s++)
    {
        donor_sounds.push_back(donor->Sounds()[s].type == sar::SoundType::kSequence ? s : sar::kNoItem);
    }
    std::erase(donor_sounds, sar::kNoItem);

    // A sound whose conversion File > Import can't match gives its place to the next of the donor's sounds, up to
    // the next sound that the spread gives.
    const std::vector<uint32_t> chosen = Spread(donor_sounds.size());
    for (std::size_t c = 0; c < chosen.size() && !sequences.empty(); c++)
    {
        const std::size_t end = c + 1 < chosen.size() ? chosen[c + 1] : donor_sounds.size();
        for (std::size_t k = chosen[c]; k < end; k++)
        {
            const uint32_t sound = donor_sounds[k];
            const auto check_sound = [&]
            {
                return DonorSoundProblem(file, *donor, donor_file, sound, sequences[0], build, 10.0, totals);
            };
            const std::string left_out = Attempt(name, donor->Sounds()[sound].name, check_sound, totals);
            if (left_out != kSeveralSlots && left_out != kTrimmed)
            {
                break;
            }
        }
    }
}

// The donor's wave sound files as new wave sounds of `file`, with the first of `wave_sounds`, or else of `sequences`,
// as the model. Counts them in `totals`.
void ImportDonorWaveSounds(const format::File& file, const Archive& donor, const std::vector<uint32_t>& sequences,
                           const std::vector<uint32_t>& wave_sounds, const std::string& name, WiiDonorTotals& totals)
{
    const std::vector<song::WaveSoundSet> sets = song::WaveSoundSets(*donor);
    const std::vector<uint32_t> models = wave_sounds.empty() ? sequences : wave_sounds;
    for (const uint32_t k : Spread(sets.size()))
    {
        if (!models.empty())
        {
            const auto check_sounds = [&]
            {
                return DonorWaveSoundsProblem(file, *donor, sets[k], models[0], totals);
            };
            Attempt(name, sets[k].name, check_sounds, totals);
        }
    }
}

// The donor's waves as new waves of the first wave data of `built`, the archive that `file` builds. `built_bytes` are
// the bytes of `built`. Counts them in `totals`.
void ImportDonorWaves(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                      const Archive& donor, const std::string& name, WiiDonorTotals& totals)
{
    const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(*built);
    for (const uint32_t k : Spread(donor->WaveArchives().size()))
    {
        if (!wave_data.empty())
        {
            const auto check_wave = [&]
            {
                return DonorWaveProblem(file, built, built_bytes, *donor, k, wave_data[0].file_id, totals);
            };
            Attempt(name, donor->WaveArchives()[k].name, check_wave, totals);
        }
    }
}

// The first file of `kind` that the donor contains, or nothing.
std::optional<std::vector<uint8_t>> FirstFileOfKind(const Archive& donor, edit::FileKind kind)
{
    const std::size_t count = kind == edit::FileKind::kBank ? donor->Banks().size() : donor->Sounds().size();
    for (uint32_t i = 0; i < count; i++)
    {
        if (kind == edit::FileKind::kWaveSoundFile && donor->Sounds()[i].type != sar::SoundType::kWaveSound)
        {
            continue;
        }

        const std::span<const uint8_t> data =
            donor->FileData(kind == edit::FileKind::kBank ? donor->Banks()[i].file_id : donor->Sounds()[i].file_id);
        if (!data.empty())
        {
            return std::vector<uint8_t>(data.begin(), data.end());
        }
    }

    return std::nullopt;
}

// The donor's files in the places of the banks and wave sound files of `built`, the archive that `file` builds.
// `built_bytes` are the bytes of `built`. Counts them in `totals`.
void ImportDonorFiles(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                      const Archive& donor, const std::string& name, WiiDonorTotals& totals)
{
    // Each of up to 8 banks and 8 wave sound files of the Wii takes the donor's first file of its kind that the
    // donor holds.
    const sar::Flavor flavor = donor->ReadOptions().flavor;
    for (const edit::FileKind kind : {edit::FileKind::kBank, edit::FileKind::kWaveSoundFile})
    {
        const std::optional<std::vector<uint8_t>> bytes = FirstFileOfKind(donor, kind);
        const ntdlib::rsar::SoundType type =
            kind == edit::FileKind::kBank ? ntdlib::rsar::SoundType::kSequence : ntdlib::rsar::SoundType::kWaveSound;
        std::set<uint32_t> done;
        for (const uint32_t sound : WiiModels(*built, type, 64))
        {
            const ntdlib::rsar::SoundInfo& info = built->Sounds()[sound];
            const uint32_t item = kind == edit::FileKind::kBank ? info.bank : sound;
            if (!bytes || done.size() == 8 || (kind == edit::FileKind::kBank && item >= built->Banks().size()))
            {
                continue;
            }
            const uint32_t id = kind == edit::FileKind::kBank ? built->Banks()[item].file_id : info.file_id;
            if (!done.insert(id).second || built->FileData(id).empty())
            {
                continue;
            }
            const auto check_file = [&]
            {
                return DonorFileProblem(file, built, built_bytes, kind, item, id, *bytes, flavor, totals);
            };
            Attempt(name, (kind == edit::FileKind::kBank ? "bank " : "wave sound file ") + std::to_string(item),
                    check_file, totals);
        }
    }
}

// Prints the summary of the imports of `mode` from donor `donor_name` into archive `name`.
void PrintDonorSummary(const std::string& name, const std::string& donor_name, const std::string& mode,
                       const WiiDonorTotals& totals)
{
    std::printf("%s: %zu imports from %s (%s). %zu fail.", name.c_str(), totals.imports, donor_name.c_str(),
                mode.c_str(), totals.failed);
    if (mode == "--new")
    {
        std::printf(
            " %zu sequence sounds came in with their banks. %zu play the samples of Another Console's "
            "conversion, and the renders of %zu hold sound. %zu conversions took the balance mode and the "
            "square-root curve. %zu wave archives came in as banks with %zu waves. Of the donor's sounds, %zu "
            "start where their files have no label, %zu have no bank in their first slot and %zu lack their files. "
            "%zu conversions take the banks of several slots, and %zu keep only the instruments that their sounds "
            "select: File > Import can't match them, and the next sounds took their places.",
            totals.sounds, totals.same_samples, totals.sounding, totals.defaults, totals.wave_archives, totals.waves,
            totals.unlabelled, totals.without_bank, totals.lacked, totals.several_slots, totals.trimmed);
    }
    else if (mode == "--wave-sounds")
    {
        std::printf(
            " %zu new wave sounds play the donor's waves. %zu wave archives came in as banks with %zu waves. "
            "%zu of the donor's wave sound files lack their files or their wave archives.",
            totals.wave_sounds, totals.wave_archives, totals.waves, totals.lacked);
    }
    else if (mode == "--new-waves")
    {
        std::printf(
            " %zu new waves keep the donor's samples, after the waves and in a wave's place. %zu of the "
            "donor's waves lack their files.",
            totals.waves, totals.lacked);
    }
    else
    {
        std::printf(" %zu files took a donor's file in their place, %zu of them banks, in %zu copies.", totals.files,
                    totals.banks, totals.copies);
    }
    std::printf("\n");
    for (const auto& [reason, count] : totals.refused)
    {
        std::printf("%s: refused %zu times: %s\n", name.c_str(), count, reason.c_str());
    }
}

} // namespace

int CheckWiiDonor(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                  const ntdlib::SndBuild& build, const std::string& name, const std::string& mode,
                  const std::filesystem::path& donor_path)
{
    // The donor and its file.
    const std::string donor_name = xsflib::Utf8(donor_path.filename());
    Archive donor;
    format::File donor_file;
    try
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(donor_path, bytes))
        {
            throw std::runtime_error("can't be read");
        }
        donor = song::OpenArchive(donor_path);
        donor_file = format::ImportArchive(bytes, donor->ReadOptions().flavor, donor->GroupFiles(),
                                           song::ReadStreams(*donor, donor_path), donor->GroupFileGroups());
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", donor_name.c_str(), error.what());
        return 2;
    }

    // An archive that the tools didn't lay out takes no new item.
    try
    {
        ntdlib::rsar::ReadArchiveModel(built_bytes);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: takes no new item: %s\n", name.c_str(), error.what());
        return 0;
    }

    WiiDonorTotals totals;

    const std::vector<uint32_t> sequences = WiiModels(*built, ntdlib::rsar::SoundType::kSequence, 1);
    const std::vector<uint32_t> wave_sounds = WiiModels(*built, ntdlib::rsar::SoundType::kWaveSound, 1);
    if (mode == "--new")
    {
        ImportDonorSounds(file, donor, donor_file, sequences, build, name, totals);
    }
    else if (mode == "--wave-sounds")
    {
        ImportDonorWaveSounds(file, donor, sequences, wave_sounds, name, totals);
    }
    else if (mode == "--new-waves")
    {
        ImportDonorWaves(file, built, built_bytes, donor, name, totals);
    }
    else
    {
        ImportDonorFiles(file, built, built_bytes, donor, name, totals);
    }

    PrintDonorSummary(name, donor_name, mode, totals);

    return totals.failed == 0 ? 0 : 1;
}

} // namespace hokuto::tools::replace
