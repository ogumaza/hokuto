// SPDX-License-Identifier: MIT

// The checks of hokuto_check_banks on archives of the Switch, the Wii U and the 3DS.

#include "check_banks.h"

namespace hokuto::tools::banks
{

std::shared_ptr<const sar::SoundArchive> Built(const format::File& file, const std::string& name)
{
    hokuto::tools::CheckSaved(file, true);

    return song::LoadArchive(format::ExportArchive(file), name, format::ExportGroupFiles(file));
}

namespace
{

// Whether `item`'s file holds the same bytes in `edited` as in `game`.
bool SameFile(const sar::SoundArchive& edited, const sar::SoundArchive& game, const song::Item& item)
{
    try
    {
        return std::ranges::equal(song::ItemFile(edited, item), song::ItemFile(game, item));
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// The samples of a note of program `program` of bank `bank` of `archive` on ntdlib's model of the console, for up to
// kNoteSeconds. The note plays as the browser plays it.
std::vector<float> Note(const std::shared_ptr<const sar::SoundArchive>& archive, uint32_t bank, uint32_t program)
{
    song::NotePreview preview(archive, bank, program);
    return Rendered(preview);
}

// Bank `bank` of `archive` as ntdlib's models read it.
sar::Bank ParsedBank(const sar::SoundArchive& archive, uint32_t bank)
{
    return sar::Bank::Parse(song::ItemFile(archive, {song::ItemKind::kBank, bank, 0}), song::ItemOptions(archive));
}

// The region that a preview's note plays in program `program` of `bank`, or nothing.
const sar::VelocityRegion* PlayedRegion(const sar::Bank& bank, uint32_t program)
{
    if (program >= bank.instruments.size() || !bank.instruments[program])
    {
        return nullptr;
    }

    const int key = song::NotePreview::KeyOf(*bank.instruments[program]);
    return key < 0 ? nullptr : bank.instruments[program]->Find(key, song::NotePreview::kVelocity);
}

} // namespace

void CheckNotes(const std::shared_ptr<const sar::SoundArchive>& before,
                const std::shared_ptr<const sar::SoundArchive>& halved,
                const std::shared_ptr<const sar::SoundArchive>& copied, uint32_t bank,
                const std::vector<std::pair<uint32_t, uint32_t>>& copies, const std::string& where, Totals& totals)
{
    const sar::Bank present = ParsedBank(*before, bank);
    const sar::Bank quieter = ParsedBank(*halved, bank);
    for (const auto& [program, copy] : copies)
    {
        const std::vector<float> note = Note(before, bank, program);
        const double level = Level(note);
        const sar::VelocityRegion* region = PlayedRegion(present, program);
        const sar::VelocityRegion* halved_region = PlayedRegion(quieter, program);
        if (level < kQuietLevel || region == nullptr || halved_region == nullptr || region->volume == 0)
        {
            totals.quiet_programs++;
            continue;
        }

        totals.programs++;
        if (Note(copied, bank, copy) != note)
        {
            Problem(
                totals, where,
                "the copy of program " + std::to_string(program) + " at " + std::to_string(copy) + " plays otherwise");
        }
        if (region->volume > kFullVolume)
        {
            totals.loud_programs++;
            continue;
        }
        if (before->ReadOptions().flavor == sar::Flavor::kCtr && Peak(note) >= kLimitedPeak)
        {
            totals.limited_programs++;
            continue;
        }

        const double expected = 20.0 * std::log10(static_cast<double>(halved_region->volume) / region->volume);
        const double error = std::abs(Level(Note(halved, bank, program)) - level - expected);
        totals.level_programs++;
        totals.level_error = std::max(totals.level_error, error);
        if (error > kLevelTolerance)
        {
            Problem(totals, where,
                    "program " + std::to_string(program) + " with its volume halved lies " + std::to_string(error) +
                        " dB off the ratio of its volumes");
        }
    }
}

std::optional<sar::BankFile> CheckBank(Edited& edited, const sar::SoundArchive& game, uint32_t bank,
                                       const std::string& name, Totals& totals)
{
    const std::string where = name + ", " + game.Banks()[bank].name;
    for (uint32_t earlier = 0; earlier < bank; ++earlier)
    {
        if (game.Banks()[earlier].file_id == game.Banks()[bank].file_id)
        {
            totals.shared_banks++;
            return std::nullopt;
        }
    }

    sar::BankFile model;
    try
    {
        model = edit::BankModelOf(edited.file, *edited.archive, bank);
        ParsedBank(*edited.archive, bank);
    }
    catch (const std::exception&)
    {
        totals.unread_banks++;
        return std::nullopt;
    }
    totals.banks++;
    if (!SameFile(*edited.archive, game, {song::ItemKind::kBank, bank, 0}))
    {
        Problem(totals, where, "an earlier edit changed the bank's file");
    }

    // The edited versions, and the archives that they build.
    const sar::BankFile halved = Halved(model);
    const std::pair<sar::BankFile, std::vector<std::pair<uint32_t, uint32_t>>> copied = WithCopies(model);
    format::File with_halved;
    std::shared_ptr<const sar::SoundArchive> halved_archive;
    std::shared_ptr<const sar::SoundArchive> copied_archive;
    try
    {
        with_halved = edit::WithBankModel(edited.file, *edited.archive, bank, halved);
        halved_archive = Built(with_halved, name);
        copied_archive = Built(edit::WithBankModel(edited.file, *edited.archive, bank, copied.first), name);
    }
    catch (const std::exception& error)
    {
        Problem(totals, where, std::string("an edit failed. ") + error.what());
        return std::nullopt;
    }

    // The exported banks read back as their models, and play.
    const auto read_back = [bank](const sar::SoundArchive& archive)
    {
        return sar::BankFile::Read(song::ItemFile(archive, {song::ItemKind::kBank, bank, 0}));
    };
    if (read_back(*halved_archive) != halved || read_back(*copied_archive) != copied.first)
    {
        Problem(totals, where, "the exported bank doesn't read back as its model");
    }
    try
    {
        CheckNotes(edited.archive, halved_archive, copied_archive, bank, copied.second, where, totals);
    }
    catch (const std::exception& error)
    {
        Problem(totals, where, std::string("a note can't play. ") + error.what());
    }

    edited = {std::move(with_halved), halved_archive};

    return halved;
}

Edited CheckLoop(const Edited& edited, uint32_t wave_archive, uint32_t wave, const format::WaveModel& model,
                 const sar::Pcm& present, const std::string& name, const std::string& where, Totals& totals)
{
    // The new rate, the loop from near the middle and the earlier end, then the end back at the wave's length.
    const bool adpcm = model.encoding == sar::WaveEncoding::kDspAdpcm;
    const format::WaveModel looped = LoopedWaveModel(model);
    format::WaveModel back = looped;
    back.loop_end = model.length;
    const format::File with_loop = edit::WithWaveModel(edited.file, *edited.archive, wave_archive, wave, looped);
    const std::shared_ptr<const sar::SoundArchive> loop_archive = Built(with_loop, name);
    format::File with_back = edit::WithWaveModel(with_loop, *loop_archive, wave_archive, wave, back);
    const std::shared_ptr<const sar::SoundArchive> back_archive = Built(with_back, name);

    // The loop keeps the samples up to its end, and its later passes play its first.
    const sar::Pcm loop_pcm = song::DecodeItemWave(*loop_archive, wave_archive, wave);
    if (loop_pcm.sample_rate != looped.rate || !loop_pcm.loop || loop_pcm.loop_start != looped.loop_start ||
        loop_pcm.loop_end != looped.loop_end || !SameStart(loop_pcm.channels, present.channels, looped.loop_end))
    {
        Problem(totals, where, "the new rate and loop don't keep the samples");
    }
    if (adpcm)
    {
        totals.adpcm_loops++;
        const sar::WaveFile loop_wave =
            sar::WaveFile::Read(song::ItemFile(*loop_archive, {song::ItemKind::kWave, wave_archive, wave}));
        if (!LaterPassesMatch(AdpcmChannels(loop_wave.channels), loop_pcm.channels, looped.loop_start, looped.loop_end,
                              LoopDecodingOf(loop_archive->ReadOptions().flavor)))
        {
            Problem(totals, where, "a later pass of the new loop decodes otherwise than its first");
        }
    }

    // The end back gives every sample again.
    const sar::Pcm back_pcm = song::DecodeItemWave(*back_archive, wave_archive, wave);
    if (back_pcm.loop_end != model.length || !SameStart(back_pcm.channels, present.channels, model.length))
    {
        Problem(totals, where, "the end back at the wave's length doesn't give the samples back");
    }

    return {std::move(with_back), back_archive};
}

void CheckEncoding(const Edited& edited, uint32_t wave_archive, uint32_t wave, const format::WaveModel& model,
                   const sar::Pcm& present, const std::string& name, const std::string& where, Totals& totals)
{
    format::WaveModel encoded = model;
    encoded.encoding = OtherEncoding(model.encoding);
    const std::shared_ptr<const sar::SoundArchive> archive =
        Built(edit::WithWaveModel(edited.file, *edited.archive, wave_archive, wave, encoded), name);
    const sar::Pcm pcm = song::DecodeItemWave(*archive, wave_archive, wave);
    if (!KeepsValues(present, pcm, encoded.encoding == sar::WaveEncoding::kDspAdpcm ? edit::kDspAdpcmFrame : 1))
    {
        Problem(totals, where, "the other encoding doesn't keep the rate, the channels or the loop");
    }

    // PCM16 keeps every sample. DSP-ADPCM comes close, and each later pass of a loop plays its first.
    if (encoded.encoding == sar::WaveEncoding::kPcm16)
    {
        if (!SameStart(pcm.channels, present.channels, model.loop_end))
        {
            Problem(totals, where, "PCM16 doesn't keep the samples");
        }
        return;
    }

    totals.adpcm_waves++;
    const double ratio = SignalToNoise(present.channels, pcm.channels, model.loop_end);
    totals.least_snr = std::min(totals.least_snr, ratio);
    totals.low_snr_waves += ratio < kLowSignalToNoise ? 1 : 0;
    const sar::WaveFile wave_file =
        sar::WaveFile::Read(song::ItemFile(*archive, {song::ItemKind::kWave, wave_archive, wave}));
    if (pcm.loop && !LaterPassesMatch(AdpcmChannels(wave_file.channels), pcm.channels, pcm.loop_start, pcm.loop_end,
                                      LoopDecodingOf(archive->ReadOptions().flavor)))
    {
        Problem(totals, where, "a later pass of the loop in DSP-ADPCM decodes otherwise than its first");
    }
}

void CheckWave(Edited& edited, const sar::SoundArchive& game, uint32_t wave_archive, uint32_t wave,
               const std::string& name, Totals& totals)
{
    const std::string where = name + ", " + song::ItemName(game, {song::ItemKind::kWave, wave_archive, wave});
    format::WaveModel model;
    sar::Pcm present;
    try
    {
        model = edit::WaveModelOf(edited.file, *edited.archive, wave_archive, wave);
        present = song::DecodeItemWave(*edited.archive, wave_archive, wave);
    }
    catch (const std::exception&)
    {
        totals.unread_waves++;
        return;
    }
    if (model.length < 8 || present.channels.empty())
    {
        totals.unread_waves++;
        return;
    }
    totals.waves++;
    if (!SameFile(*edited.archive, game, {song::ItemKind::kWave, wave_archive, wave}))
    {
        Problem(totals, where, "an earlier edit changed the wave");
    }

    try
    {
        Edited after = CheckLoop(edited, wave_archive, wave, model, present, name, where, totals);
        CheckEncoding(edited, wave_archive, wave, model, present, name, where, totals);
        edited = std::move(after);
    }
    catch (const std::exception& error)
    {
        Problem(totals, where, std::string("an edit failed. ") + error.what());
    }
}

void CheckStream(Edited& edited, uint32_t file_id, const std::string& name, Totals& totals)
{
    const std::string where = name + ", stream file " + std::to_string(file_id);
    const std::vector<uint8_t> bytes = *edited.file.native->streams.at(file_id).bytes;
    if (song::IsAdts(bytes))
    {
        totals.aac_streams++;
        return;
    }
    edit::StreamValues values;
    sar::Pcm present;
    try
    {
        values = edit::StreamValuesOf(bytes);
        present = song::DecodeStreamFile(bytes);
    }
    catch (const std::exception& error)
    {
        Problem(totals, where, std::string("the stream can't be read. ") + error.what());
        return;
    }
    totals.streams++;

    // The new rate with a loop from the first block past the middle, and the other encoding.
    edit::StreamValues looped = LoopedStreamValues(values);

    // The 3DS's tools never start a loop at the first block, and such a loop is made again. The short stream goes
    // without a loop there.
    if (looped.loop_start == 0 && FlavorOf(*edited.file.native) == sar::Flavor::kCtr)
    {
        looped.loop = false;
    }
    edit::StreamValues encoded = values;
    encoded.encoding = OtherEncoding(values.encoding);
    try
    {
        const std::vector<uint8_t> loop_bytes = edit::StreamWithValues(bytes, looped);
        format::File replaced = edit::ReplaceStream(edited.file, file_id, loop_bytes);
        if (*replaced.native->streams.at(file_id).bytes != loop_bytes)
        {
            Problem(totals, where, "the file doesn't keep the new stream");
        }

        // The loop keeps every sample, and its later passes play its first.
        const sar::Pcm loop_pcm = song::DecodeStreamFile(loop_bytes);
        if (loop_pcm.sample_rate != looped.rate || loop_pcm.loop != looped.loop ||
            loop_pcm.loop_start != looped.loop_start || !SameStart(loop_pcm.channels, present.channels, values.length))
        {
            Problem(totals, where, "the new rate and loop don't keep the samples");
        }
        const sar::StreamFile loop_stream = sar::StreamFile::Read(loop_bytes);
        if (loop_stream.encoding == sar::WaveEncoding::kDspAdpcm && looped.loop &&
            !LaterPassesMatch(AdpcmChannels(loop_stream.channels), loop_pcm.channels, looped.loop_start, values.length,
                              LoopDecodingOf(FlavorOf(*edited.file.native))))
        {
            Problem(totals, where, "a later pass of the new loop decodes otherwise than its first");
        }

        // The other encoding keeps the rate, the channels and the loop. The loop moves forward to a block of the new
        // encoding: on the 3DS to the start of the block after the one that holds it (ntdlib's MakeStream). 16-bit PCM
        // keeps every sample. DSP-ADPCM comes close, and each later pass of the loop plays its first.
        const std::vector<uint8_t> encoded_bytes = edit::StreamWithValues(bytes, encoded);
        const sar::Pcm encoded_pcm = song::DecodeStreamFile(encoded_bytes);
        const bool ctr = FlavorOf(*edited.file.native) == sar::Flavor::kCtr;
        if (!KeepsValues(present, encoded_pcm, sar::StreamBlockSamples(encoded.encoding) + (ctr ? 1 : 0)))
        {
            Problem(totals, where, "the other encoding doesn't keep the rate, the channels or the loop");
        }
        if (encoded.encoding == sar::WaveEncoding::kPcm16)
        {
            if (!SameStart(encoded_pcm.channels, present.channels, values.length))
            {
                Problem(totals, where, "16-bit PCM doesn't keep the samples");
            }
        }
        else
        {
            totals.least_stream_snr =
                std::min(totals.least_stream_snr, SignalToNoise(present.channels, encoded_pcm.channels, values.length));
            const sar::StreamFile encoded_stream = sar::StreamFile::Read(encoded_bytes);
            if (encoded_pcm.loop &&
                !LaterPassesMatch(AdpcmChannels(encoded_stream.channels), encoded_pcm.channels, encoded_pcm.loop_start,
                                  encoded_pcm.loop_end, LoopDecodingOf(FlavorOf(*edited.file.native))))
            {
                Problem(totals, where, "a later pass of the loop in DSP-ADPCM decodes otherwise than its first");
            }
        }

        edited.file = std::move(replaced);
    }
    catch (const std::exception& error)
    {
        Problem(totals, where, std::string("an edit failed. ") + error.what());
    }
}

void CheckWaveSound(Edited& edited, uint32_t sound, const std::vector<sar::WaveId>& waves, const std::string& name,
                    Totals& totals)
{
    const sar::SoundArchive& archive = *edited.archive;
    const std::string where = name + ", " + archive.Sounds()[sound].name;
    edit::WaveSoundFields fields;
    try
    {
        fields = edit::WaveSoundFieldsOf(archive, sound);
    }
    catch (const std::exception&)
    {
        totals.unread_wave_sounds++;
        return;
    }
    totals.wave_sounds++;

    // The other wave sounds of the file. They have to keep their values.
    std::vector<std::pair<uint32_t, edit::WaveSoundFields>> others;
    for (uint32_t other = 0; other < archive.Sounds().size(); ++other)
    {
        const sar::SoundInfo& info = archive.Sounds()[other];
        if (other == sound || info.type != sar::SoundType::kWaveSound ||
            info.file_id != archive.Sounds()[sound].file_id)
        {
            continue;
        }

        try
        {
            others.emplace_back(other, edit::WaveSoundFieldsOf(archive, other));
        }
        catch (const std::exception&)
        {
            continue;
        }
    }

    // The new values and the next wave.
    edit::WaveSoundFields wanted = fields;
    if (fields.places.pan)
    {
        wanted.values.pan = static_cast<uint8_t>((fields.values.pan + 32) % 128);
        wanted.values.span = static_cast<uint8_t>((fields.values.span + 32) % 128);
    }
    if (fields.places.pitch)
    {
        wanted.values.pitch = fields.values.pitch * 1.25f;
    }
    if (fields.places.sends)
    {
        wanted.values.main_send = static_cast<uint8_t>((fields.values.main_send + 16) % 128);
        for (std::size_t bus = 0; bus < fields.places.aux_sends; ++bus)
        {
            wanted.values.aux_sends[bus] = static_cast<uint8_t>((fields.values.aux_sends[bus] + 16) % 128);
        }
    }
    const auto found = std::ranges::find(waves, fields.wave);
    wanted.wave = found == waves.end() || found + 1 == waves.end() ? waves.front() : *(found + 1);
    try
    {
        format::File file = edit::WithWaveSoundFields(edited.file, archive, sound, wanted.values, wanted.wave);
        std::shared_ptr<const sar::SoundArchive> after = Built(file, name);
        if (!SameFields(edit::WaveSoundFieldsOf(*after, sound), wanted))
        {
            Problem(totals, where, "the new values or wave don't read back");
        }
        for (const auto& [other, before] : others)
        {
            if (!SameFields(edit::WaveSoundFieldsOf(*after, other), before))
            {
                Problem(totals, where, archive.Sounds()[other].name + " lost its values");
            }
        }

        edited = {std::move(file), std::move(after)};
    }
    catch (const std::exception& error)
    {
        Problem(totals, where, std::string("the edit failed. ") + error.what());
    }
}

void CheckOtherFiles(const Edited& edited, const sar::SoundArchive& game, const std::string& name, Totals& totals)
{
    std::set<uint32_t> reached;
    for (const sar::BankInfo& bank : game.Banks())
    {
        reached.insert(bank.file_id);
    }
    for (const sar::WaveArchiveInfo& wave_archive : game.WaveArchives())
    {
        reached.insert(wave_archive.file_id);
    }
    for (const sar::SoundInfo& sound : game.Sounds())
    {
        if (sound.type == sar::SoundType::kWaveSound)
        {
            reached.insert(sound.file_id);
        }
    }

    for (uint32_t file_id = 0; file_id < game.FileCount(); ++file_id)
    {
        const std::span<const uint8_t> before = game.FileData(file_id);
        const std::string kind = before.size() >= 4 ? std::string(before.begin(), before.begin() + 4) : "";
        if (reached.contains(file_id) || kind == "FGRP" || kind == "CGRP")
        {
            continue;
        }

        totals.other_files++;
        const std::span<const uint8_t> after = edited.archive->FileData(file_id);
        if (!std::ranges::equal(after, before))
        {
            Problem(totals, name,
                    "the edits changed file " + std::to_string(file_id) + " (" + kind + ", " +
                        std::to_string(before.size()) + " bytes, then " + std::to_string(after.size()) + ")");
        }
    }
}

} // namespace hokuto::tools::banks
