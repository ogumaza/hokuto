// SPDX-License-Identifier: MIT

// The checks of hokuto_check_banks on archives of the Wii.

#include "check_banks.h"

namespace hokuto::tools::banks
{

namespace
{

// The Wii. A bank of the Wii keeps its waves in its wave data, and so does a wave sound file. Hokuto takes such wave
// data for a wave archive, by its file's ID (song::WaveArchivesOf).

// The same on the Wii's `archive`, on ntdlib's model of NW4R SND and AX with `build`.
std::vector<float> WiiNote(const std::shared_ptr<const rsar::SoundArchive>& archive, uint32_t bank, uint32_t program,
                           const ntdlib::SndBuild& build)
{
    song::NotePreview preview(archive, bank, program, build);
    return Rendered(preview);
}

// The file that the edits so far have made from an archive of the Wii, and the archive that it builds, read as ntdlib's
// model of the Wii reads an archive.
struct WiiEdited
{
    format::File file;
    std::shared_ptr<const rsar::SoundArchive> archive;
};

// The archive of the Wii that `file` builds.
std::shared_ptr<const rsar::SoundArchive> BuiltWii(const format::File& file, const std::string& name)
{
    hokuto::tools::CheckSaved(file, true);

    return song::LoadWiiArchive(format::ExportArchive(file), name, ntdlib::rvl::snd::kRsarOptions);
}

// Whether file `file_id` and its wave data hold the same bytes in the Wii's `edited` as in `game`.
bool SameWiiFile(const rsar::SoundArchive& edited, const rsar::SoundArchive& game, uint32_t file_id)
{
    return std::ranges::equal(edited.FileData(file_id), game.FileData(file_id)) &&
           std::ranges::equal(edited.WaveData(file_id), game.WaveData(file_id));
}

// The region that a preview's note plays in program `program` of bank `bank` of the Wii's `archive`, or nothing.
std::optional<rsar::Region> WiiPlayedRegion(const rsar::SoundArchive& archive, uint32_t bank, uint32_t program)
{
    const rsar::Bank parsed = rsar::Bank::Parse(song::ItemFile(archive, {song::ItemKind::kBank, bank, 0}));
    const int key = song::NotePreview::KeyOf(parsed, program);
    const rsar::Region* region = key < 0 ? nullptr : parsed.Find(program, key, song::NotePreview::kVelocity);
    return region != nullptr ? std::optional(*region) : std::nullopt;
}

// Checks the notes of bank `bank` of the Wii as CheckNotes checks those of the other consoles, on ntdlib's model of
// NW4R SND and AX with `build`.
void CheckWiiNotes(const std::shared_ptr<const rsar::SoundArchive>& before,
                   const std::shared_ptr<const rsar::SoundArchive>& halved,
                   const std::shared_ptr<const rsar::SoundArchive>& copied, uint32_t bank,
                   const std::vector<std::pair<uint32_t, uint32_t>>& copies, const ntdlib::SndBuild& build,
                   const std::string& where, Totals& totals)
{
    for (const auto& [program, copy] : copies)
    {
        const std::vector<float> note = WiiNote(before, bank, program, build);
        const double level = Level(note);
        const std::optional<rsar::Region> region = WiiPlayedRegion(*before, bank, program);
        const std::optional<rsar::Region> halved_region = WiiPlayedRegion(*halved, bank, program);
        if (level < kQuietLevel || !region || !halved_region || region->volume == 0)
        {
            totals.quiet_programs++;
            continue;
        }

        totals.programs++;
        if (WiiNote(copied, bank, copy, build) != note)
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

        const double expected = 20.0 * std::log10(static_cast<double>(halved_region->volume) / region->volume);
        const double error = std::abs(Level(WiiNote(halved, bank, program, build)) - level - expected);
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

// Checks bank `bank` of the game's archive `game` of the Wii from `edited` on ntdlib's model of the Wii with `build`.
// CheckBank checks the other consoles' banks alike. The edits have to keep the bank's waves. A bank that ntdlib's model
// reads and the Bank Editor refuses counts apart, with the reason. `edited` then holds the halved volumes. Returns the
// bank's model with them, or nothing when the bank took no edit.
std::optional<sar::BankFile> CheckWiiBank(WiiEdited& edited, const rsar::SoundArchive& game, uint32_t bank,
                                          const ntdlib::SndBuild& build, const std::string& name, Totals& totals)
{
    const std::string where = name + ", " + game.Banks()[bank].name;
    const uint32_t file_id = game.Banks()[bank].file_id;
    for (uint32_t earlier = 0; earlier < bank; ++earlier)
    {
        if (game.Banks()[earlier].file_id == file_id)
        {
            totals.shared_banks++;
            return std::nullopt;
        }
    }

    std::vector<rsar::WaveFile> waves;
    try
    {
        waves = edit::WavesOf(*edited.archive, file_id);
        rsar::Bank::Parse(song::ItemFile(*edited.archive, {song::ItemKind::kBank, bank, 0}));
    }
    catch (const std::exception&)
    {
        totals.unread_banks++;
        return std::nullopt;
    }
    sar::BankFile model;
    try
    {
        model = edit::BankModelOf(*edited.archive, bank);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: refused. %s\n", where.c_str(), error.what());
        totals.refused_banks++;
        return std::nullopt;
    }
    totals.banks++;
    if (!SameWiiFile(*edited.archive, game, file_id))
    {
        Problem(totals, where, "an earlier edit changed the bank's file");
    }

    // The edited versions, and the archives that they build.
    const sar::BankFile halved = Halved(model);
    const std::pair<sar::BankFile, std::vector<std::pair<uint32_t, uint32_t>>> copied = WithCopies(model);
    format::File with_halved;
    std::shared_ptr<const rsar::SoundArchive> halved_archive;
    std::shared_ptr<const rsar::SoundArchive> copied_archive;
    try
    {
        with_halved = edit::WithBankModel(edited.file, *edited.archive, bank, halved);
        halved_archive = BuiltWii(with_halved, name);
        copied_archive = BuiltWii(edit::WithBankModel(edited.file, *edited.archive, bank, copied.first), name);
    }
    catch (const std::exception& error)
    {
        Problem(totals, where, std::string("an edit failed. ") + error.what());
        return std::nullopt;
    }

    // The exported banks read back as their models, with the bank's waves, and play.
    try
    {
        if (edit::BankModelOf(*halved_archive, bank) != halved ||
            edit::BankModelOf(*copied_archive, bank) != copied.first)
        {
            Problem(totals, where, "the exported bank doesn't read back as its model");
        }
        if (edit::WavesOf(*halved_archive, file_id) != waves || edit::WavesOf(*copied_archive, file_id) != waves)
        {
            Problem(totals, where, "the edits changed the bank's waves");
        }
        CheckWiiNotes(edited.archive, halved_archive, copied_archive, bank, copied.second, build, where, totals);
    }
    catch (const std::exception& error)
    {
        Problem(totals, where, std::string("the exported bank can't be read or played. ") + error.what());
    }

    edited = {std::move(with_halved), halved_archive};

    return halved;
}

// Checks the edits of a wave of the Wii that start from `edited`. CheckLoop makes the same checks of the other
// consoles' waves. Returns the file and the archive with the end back.
WiiEdited CheckWiiLoop(const WiiEdited& edited, uint32_t file_id, uint32_t wave, const format::WaveModel& model,
                       const rsar::Pcm& present, const std::string& name, const std::string& where, Totals& totals)
{
    // The new rate, the loop from near the middle and the earlier end, then the end back at the wave's length.
    const bool adpcm = model.encoding == sar::WaveEncoding::kDspAdpcm;
    const format::WaveModel looped = LoopedWaveModel(model);
    format::WaveModel back = looped;
    back.loop_end = model.length;
    const format::File with_loop = edit::WithWaveModel(edited.file, *edited.archive, file_id, wave, looped);
    const std::shared_ptr<const rsar::SoundArchive> loop_archive = BuiltWii(with_loop, name);
    format::File with_back = edit::WithWaveModel(with_loop, *loop_archive, file_id, wave, back);
    const std::shared_ptr<const rsar::SoundArchive> back_archive = BuiltWii(with_back, name);

    // The loop keeps the samples up to its end, and its later passes play its first.
    const rsar::Pcm loop_pcm = song::DecodeItemWave(*loop_archive, file_id, wave);
    if (loop_pcm.sample_rate != looped.rate || !loop_pcm.loop || loop_pcm.loop_start != looped.loop_start ||
        loop_pcm.loop_end != looped.loop_end || !SameStart(loop_pcm.channels, present.channels, looped.loop_end))
    {
        Problem(totals, where, "the new rate and loop don't keep the samples");
    }
    if (adpcm)
    {
        totals.adpcm_loops++;
        const rsar::WaveFile loop_wave = edit::PresentWave(*loop_archive, file_id, wave);
        if (!LaterPassesMatch(AdpcmChannels(loop_wave.channels), loop_pcm.channels, looped.loop_start, looped.loop_end,
                              kWiiLoopDecoding))
        {
            Problem(totals, where, "a later pass of the new loop decodes otherwise than its first");
        }
    }

    // The end back gives every sample again.
    const rsar::Pcm back_pcm = song::DecodeItemWave(*back_archive, file_id, wave);
    if (back_pcm.loop_end != model.length || !SameStart(back_pcm.channels, present.channels, model.length))
    {
        Problem(totals, where, "the end back at the wave's length doesn't give the samples back");
    }

    return {std::move(with_back), back_archive};
}

// The Wii's counterpart of CheckEncoding: checks the edit of a wave of the Wii that starts from `edited` and gives it
// another encoding.
void CheckWiiEncoding(const WiiEdited& edited, uint32_t file_id, uint32_t wave, const format::WaveModel& model,
                      const rsar::Pcm& present, const std::string& name, const std::string& where, Totals& totals)
{
    format::WaveModel encoded = model;
    encoded.encoding = OtherEncoding(model.encoding);
    const std::shared_ptr<const rsar::SoundArchive> archive =
        BuiltWii(edit::WithWaveModel(edited.file, *edited.archive, file_id, wave, encoded), name);
    const rsar::Pcm pcm = song::DecodeItemWave(*archive, file_id, wave);
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
    const rsar::WaveFile wave_file = edit::PresentWave(*archive, file_id, wave);
    if (pcm.loop && !LaterPassesMatch(AdpcmChannels(wave_file.channels), pcm.channels, pcm.loop_start, pcm.loop_end,
                                      kWiiLoopDecoding))
    {
        Problem(totals, where, "a later pass of the loop in DSP-ADPCM decodes otherwise than its first");
    }
}

// Checks wave `wave` of the wave data of file `file_id` of the game's archive `game` of the Wii from `edited`.
// CheckWave makes the same check of the other consoles' waves. Earlier edits have to keep the wave. `edited` then holds
// the wave with its end back.
void CheckWiiWave(WiiEdited& edited, const rsar::SoundArchive& game, uint32_t file_id, uint32_t wave,
                  const std::string& name, Totals& totals)
{
    const std::string where = name + ", " + song::ItemName(game, {song::ItemKind::kWave, file_id, wave});
    format::WaveModel model;
    rsar::Pcm present;
    try
    {
        model = edit::WaveModelOf(edited.file, *edited.archive, file_id, wave);
        present = song::DecodeItemWave(*edited.archive, file_id, wave);
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

    try
    {
        if (edit::PresentWave(*edited.archive, file_id, wave) != edit::PresentWave(game, file_id, wave))
        {
            Problem(totals, where, "an earlier edit changed the wave");
        }
        WiiEdited after = CheckWiiLoop(edited, file_id, wave, model, present, name, where, totals);
        CheckWiiEncoding(edited, file_id, wave, model, present, name, where, totals);
        edited = std::move(after);
    }
    catch (const std::exception& error)
    {
        Problem(totals, where, std::string("an edit failed. ") + error.what());
    }
}

// Checks the Wii's wave sound `sound` from `edited`: a new pan, surround pan, pitch, sends and envelope, and the next
// wave of its file's wave data. ntdlib has to read them back, the file's other wave sounds have to keep their values,
// and the file's waves have to stay. `edited` then holds the new values.
void CheckWiiWaveSound(WiiEdited& edited, uint32_t sound, const std::string& name, Totals& totals)
{
    const rsar::SoundArchive& archive = *edited.archive;
    const uint32_t file_id = archive.Sounds()[sound].file_id;
    const std::string where = name + ", " + archive.Sounds()[sound].name;
    edit::WaveSoundFields fields;
    std::size_t waves = 0;
    try
    {
        fields = edit::WaveSoundFieldsOf(archive, sound);
        waves = song::WaveCountOf(archive, file_id);
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
        const rsar::SoundInfo& info = archive.Sounds()[other];
        if (other == sound || info.type != rsar::SoundType::kWaveSound || info.file_id != file_id)
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
    wanted.values.pan = Moved(fields.values.pan, 32);
    wanted.values.span = Moved(fields.values.span, 32);
    wanted.values.pitch = fields.values.pitch * 1.25f;
    wanted.values.main_send = Moved(fields.values.main_send, 16);
    for (std::size_t bus = 0; bus < fields.places.aux_sends; ++bus)
    {
        wanted.values.aux_sends[bus] = Moved(fields.values.aux_sends[bus], 16);
    }
    wanted.values.adshr.attack = Moved(fields.values.adshr.attack, 16);
    wanted.values.adshr.decay = Moved(fields.values.adshr.decay, 16);
    wanted.values.adshr.sustain = Moved(fields.values.adshr.sustain, 16);
    wanted.values.adshr.hold = Moved(fields.values.adshr.hold, 16);
    wanted.values.adshr.release = Moved(fields.values.adshr.release, 16);
    wanted.wave.index = static_cast<uint32_t>((fields.wave.index + 1) % waves);
    try
    {
        format::File file = edit::WithWaveSoundFields(edited.file, archive, sound, wanted.values, wanted.wave);
        std::shared_ptr<const rsar::SoundArchive> after = BuiltWii(file, name);
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
        if (edit::WavesOf(*after, file_id) != edit::WavesOf(archive, file_id))
        {
            Problem(totals, where, "the edit changed the file's waves");
        }

        edited = {std::move(file), std::move(after)};
    }
    catch (const std::exception& error)
    {
        Problem(totals, where, std::string("the edit failed. ") + error.what());
    }
}

// Checks that every file of the Wii's `edited` archive that no edit reaches holds the bytes of the game's archive
// `game`, with its wave data: every file of the groups but the banks and the wave sound files.
void CheckWiiOtherFiles(const WiiEdited& edited, const rsar::SoundArchive& game, const std::string& name,
                        Totals& totals)
{
    std::set<uint32_t> reached;
    for (const rsar::BankInfo& bank : game.Banks())
    {
        reached.insert(bank.file_id);
    }
    for (const rsar::SoundInfo& sound : game.Sounds())
    {
        if (sound.type == rsar::SoundType::kWaveSound)
        {
            reached.insert(sound.file_id);
        }
    }

    std::set<uint32_t> files;
    for (const rsar::SoundArchive::Group& group : game.Groups())
    {
        for (const rsar::SoundArchive::Group::Item& item : group.items)
        {
            files.insert(item.file_id);
        }
    }
    for (const uint32_t file_id : files)
    {
        if (reached.contains(file_id))
        {
            continue;
        }

        totals.other_files++;
        if (!SameWiiFile(*edited.archive, game, file_id))
        {
            Problem(totals, name, "the edits changed file " + std::to_string(file_id));
        }
    }
}

// The Wii's counterpart of CheckStream: checks the stream of file `file_id` that the Wii's `edited` keeps. The new rate
// and loop, and the other encoding, have to keep the stream's tracks. `edited` then holds the stream with the loop.
void CheckWiiStream(WiiEdited& edited, uint32_t file_id, const std::string& name, Totals& totals)
{
    const std::string where = name + ", stream file " + std::to_string(file_id);
    const std::vector<uint8_t> bytes = *edited.file.native->streams.at(file_id).bytes;
    edit::StreamValues values;
    sar::Pcm present;
    rsar::StreamFile stream;
    try
    {
        values = edit::StreamValuesOf(bytes);
        present = song::DecodeStreamFile(bytes);
        stream = rsar::StreamFile::Read(bytes);
    }
    catch (const std::exception& error)
    {
        Problem(totals, where, std::string("the stream can't be read. ") + error.what());
        return;
    }
    totals.streams++;

    // The new rate with a loop from the first block past the middle, and the other encoding.
    const edit::StreamValues looped = LoopedStreamValues(values);
    edit::StreamValues encoded = values;
    encoded.encoding = OtherEncoding(values.encoding);

    const auto same_tracks = [&stream](const rsar::StreamFile& other)
    {
        return other.tracks == stream.tracks && other.extended_tracks == stream.extended_tracks;
    };
    try
    {
        const std::vector<uint8_t> loop_bytes = edit::StreamWithValues(bytes, looped);
        format::File replaced = edit::ReplaceStream(edited.file, file_id, loop_bytes);
        if (*replaced.native->streams.at(file_id).bytes != loop_bytes)
        {
            Problem(totals, where, "the file doesn't keep the new stream");
        }

        // The loop keeps every sample and the tracks, and its later passes play its first.
        const sar::Pcm loop_pcm = song::DecodeStreamFile(loop_bytes);
        const rsar::StreamFile loop_stream = rsar::StreamFile::Read(loop_bytes);
        if (loop_pcm.sample_rate != looped.rate || loop_pcm.loop != looped.loop ||
            loop_pcm.loop_start != looped.loop_start || !SameStart(loop_pcm.channels, present.channels, values.length))
        {
            Problem(totals, where, "the new rate and loop don't keep the samples");
        }
        if (!same_tracks(loop_stream))
        {
            Problem(totals, where, "the new rate and loop don't keep the tracks");
        }
        if (loop_stream.encoding == rsar::WaveEncoding::kDspAdpcm &&
            !LaterPassesMatch(AdpcmChannels(loop_stream.channels), loop_pcm.channels, looped.loop_start, values.length,
                              kWiiLoopDecoding))
        {
            Problem(totals, where, "a later pass of the new loop decodes otherwise than its first");
        }

        // The other encoding keeps the rate, the channels, the loop and the tracks. The loop moves forward to a block
        // of the new encoding. 16-bit PCM keeps every sample. DSP-ADPCM comes close, and each later pass of the loop
        // plays its first.
        const std::vector<uint8_t> encoded_bytes = edit::StreamWithValues(bytes, encoded);
        const sar::Pcm encoded_pcm = song::DecodeStreamFile(encoded_bytes);
        const rsar::StreamFile encoded_stream = rsar::StreamFile::Read(encoded_bytes);
        if (!KeepsValues(present, encoded_pcm, rsar::StreamBlockSamples(encoded_stream.encoding)))
        {
            Problem(totals, where, "the other encoding doesn't keep the rate, the channels or the loop");
        }
        if (!same_tracks(encoded_stream))
        {
            Problem(totals, where, "the other encoding doesn't keep the tracks");
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
            if (encoded_pcm.loop && !LaterPassesMatch(AdpcmChannels(encoded_stream.channels), encoded_pcm.channels,
                                                      encoded_pcm.loop_start, encoded_pcm.loop_end, kWiiLoopDecoding))
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

// Checks that the file table of the archive that the Wii's `edited` builds gives the size of each stream that the file
// keeps. A file table that ntdlib's model of the archive can't read fails: the sizes can't be checked.
void CheckWiiStreamSizes(const WiiEdited& edited, const std::string& name, Totals& totals)
{
    rsar::ArchiveModel model;
    try
    {
        model = rsar::ReadArchiveModel(format::ExportArchive(edited.file));
    }
    catch (const xsflib::FormatError& error)
    {
        Problem(totals, name,
                std::string("ntdlib's model can't read the exported archive's file table. ") + error.what());
        return;
    }
    for (const auto& [file_id, stream] : edited.file.native->streams)
    {
        if (file_id >= model.files.size() || model.files[file_id].path_size != stream.bytes->size())
        {
            Problem(totals, name + ", stream file " + std::to_string(file_id),
                    "the file table doesn't give the stream's size");
        }
    }
}

// Checks each bank of `game` in the run's `part`, and returns the instruments of each bank whose volumes the check
// halved. `build` is the game's build of NW4R SND.
std::vector<std::pair<uint32_t, sar::BankFile>> CheckWiiBanks(WiiEdited& edited, const rsar::SoundArchive& game,
                                                              const Part& part, const ntdlib::SndBuild& build,
                                                              const std::string& name, Totals& totals)
{
    std::vector<std::pair<uint32_t, sar::BankFile>> halved;
    for (uint32_t bank = 0; bank < game.Banks().size(); ++bank)
    {
        if (Holds(part, bank))
        {
            if (std::optional<sar::BankFile> model = CheckWiiBank(edited, game, bank, build, name, totals))
            {
                halved.emplace_back(bank, std::move(*model));
            }
        }
    }

    return halved;
}

// Checks the waves in the wave data of each bank and each wave sound file in `game`, for part `part` of the run, with
// `items`. The wave sounds have to keep their values.
void CheckWiiWaveData(WiiEdited& edited, const rsar::SoundArchive& game, const Part& part, bool items,
                      const std::string& name, Totals& totals)
{
    std::vector<std::pair<uint32_t, edit::WaveSoundFields>> sound_fields;
    for (uint32_t sound = 0; sound < game.Sounds().size(); ++sound)
    {
        if (game.Sounds()[sound].type == rsar::SoundType::kWaveSound)
        {
            try
            {
                sound_fields.emplace_back(sound, edit::WaveSoundFieldsOf(*edited.archive, sound));
            }
            catch (const std::exception&)
            {
                continue;
            }
        }
    }

    std::size_t place = 0;
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(game))
    {
        std::size_t count = 0;
        try
        {
            count = song::WaveCountOf(game, wave_archive.file_id);
        }
        catch (const std::exception&)
        {
            continue;
        }
        for (uint32_t wave = 0; wave < count; ++wave, ++place)
        {
            if (Holds(part, place) && items)
            {
                CheckWiiWave(edited, game, wave_archive.file_id, wave, name, totals);
            }
        }
    }

    for (const auto& [sound, fields] : sound_fields)
    {
        if (!SameFields(edit::WaveSoundFieldsOf(*edited.archive, sound), fields))
        {
            Problem(totals, name + ", " + game.Sounds()[sound].name, "the edits of the waves changed its values");
        }
    }
}

// Checks the wave sounds of `game` with `wave_sounds` and the streams with `streams`, of the run's `part`. An export
// writes the streams' sizes into the file table.
void CheckWiiWaveSoundsAndStreams(WiiEdited& edited, const rsar::SoundArchive& game, const Part& part, bool wave_sounds,
                                  bool streams, const std::string& name, Totals& totals)
{
    for (uint32_t sound = 0; sound < game.Sounds().size() && wave_sounds; ++sound)
    {
        if (game.Sounds()[sound].type == rsar::SoundType::kWaveSound && Holds(part, sound))
        {
            CheckWiiWaveSound(edited, sound, name, totals);
        }
    }
    std::vector<uint32_t> stream_files;
    for (const auto& [file_id, stream] : edited.file.native->streams)
    {
        stream_files.push_back(file_id);
    }
    for (std::size_t i = 0; i < stream_files.size() && streams; ++i)
    {
        if (Holds(part, i))
        {
            CheckWiiStream(edited, stream_files[i], name, totals);
        }
    }
    if (streams && !stream_files.empty())
    {
        CheckWiiStreamSizes(edited, name, totals);
    }
}

} // namespace

int RunWii(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, const std::string& dol_path,
           const Part& part, bool items, bool wave_sounds, bool streams)
{
    const std::string name = xsflib::Utf8(path.filename());
    const auto began = std::chrono::steady_clock::now();
    WiiEdited edited;
    ntdlib::SndBuild build;
    try
    {
        const std::optional<std::filesystem::path> dol =
            dol_path.empty() ? song::FindMainDol(path) : std::optional(xsflib::PathFromUtf8(dol_path));
        std::optional<ntdlib::SndBuild> game_build;
        if (dol)
        {
            std::vector<uint8_t> dol_bytes;
            game_build = xsflib::ReadFile(*dol, dol_bytes) ? song::ReadWiiBuild(dol_bytes) : std::nullopt;
            if (!game_build)
            {
                std::fprintf(stderr, "%s: the main.dol gives no build of NW4R SND\n", name.c_str());
                return 2;
            }
        }
        const std::shared_ptr<const rsar::SoundArchive> opened =
            song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions);
        edited.file = format::ImportWiiArchive(bytes, game_build, song::ReadStreams(*opened, path));
        edited.file = hokuto::tools::CheckedForm(std::move(edited.file), bytes);
        build = format::BuildOfWiiArchive(*edited.file.native);
        edited.archive = BuiltWii(edited.file, name);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    // The unedited archive comes back byte for byte.
    Totals totals;
    const std::shared_ptr<const rsar::SoundArchive> game = edited.archive;
    if (format::ExportArchive(edited.file) != bytes)
    {
        Problem(totals, name, "the unedited archive doesn't come back byte for byte");
    }

    // The banks of the run's part.
    const std::vector<std::pair<uint32_t, sar::BankFile>> halved =
        items ? CheckWiiBanks(edited, *game, part, build, name, totals)
              : std::vector<std::pair<uint32_t, sar::BankFile>>();

    // The waves of each bank's and each wave sound file's wave data. They have to keep the values of the wave sounds.
    CheckWiiWaveData(edited, *game, part, items, name, totals);

    // The wave sounds and the streams of the run's part. An export writes the streams' sizes into the file table.
    CheckWiiWaveSoundsAndStreams(edited, *game, part, wave_sounds, streams, name, totals);

    // The banks keep their halved volumes, the other files keep their bytes, and the file comes back from both forms.
    for (const auto& [bank, model] : halved)
    {
        if (edit::BankModelOf(*edited.archive, bank) != model)
        {
            Problem(totals, name + ", " + game->Banks()[bank].name, "the later edits changed the bank's instruments");
        }
    }
    CheckWiiOtherFiles(edited, *game, name, totals);
    if (!hokuto::tools::SavesAndOpens(edited.file))
    {
        Problem(totals, name, "the file with the edits doesn't come back from both forms");
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

    // The build's date from the game's banner. A main.dol without one gives 0.
    std::string heading = name + " (" +
                          (build.date != 0 ? "build " + std::to_string(build.date) : "build without a date") +
                          (build.October2006() ? ", AX of October 2006" : "") + ")";
    if (part.n > 1)
    {
        heading += " (part " + std::to_string(part.k) + " of " + std::to_string(part.n) + ")";
    }
    PrintSummary(heading, totals, seconds);

    return totals.problems == 0 ? 0 : 1;
}

} // namespace hokuto::tools::banks
