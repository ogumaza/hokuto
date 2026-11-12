// SPDX-License-Identifier: MIT

// Checks ntdlib's players of wave sounds and of stream sounds on a sound archive of the Switch, the Wii U, the 3DS or
// the Wii. The browser's previews play through those players (src/song/preview.h). The tool opens the archive with the
// group files and the streams beside it. Each wave sound, and each stream sound whose stream lies beside the archive,
// plays by itself on the console's model without effects. The tool starts each one on ntdlib's engines itself, with
// helpers that the preview uses too, such as song::WaveSoundNoteOf. The check never runs song::SoundPreview. A wave
// sound without a loop plays until it ends, and one with a loop for 30 s. A stream with a loop plays to 5 s past the
// end of its first pass. One without a loop plays to 1 s past its end. Neither plays for more than 6 minutes. A stream
// without a loop that the limit cuts short doesn't have to end.
//
// The reference doesn't come from the model's voices. ntdlib's readers decode the wave of the wave sound's first note,
// or each channel of the stream's tracks. The exports decode them that way too. A wave or a stream that loops repeats
// its loop. The tool resamples the reference with a windowed sinc. It finds the output sample where the reference
// starts, and then the place in the reference and the step that the console's resampler takes, from the sound's pitch.
// It fits a gain from each channel of the reference to each side of the output, and measures the output's
// signal-to-noise ratio against the fitted reference: the ratio as heard. The consoles' resamplers interpolate with 4
// taps. The 4 taps dull the highest frequencies. The tool therefore also fits a filter of 13 taps from each channel to
// each side, and measures the ratio against that: the ratio beyond a fixed filter. It then measures that ratio in the
// band below 7 kHz, or below 0.45 times the rate at which the sound plays when that's lower: the band holds no images
// of the resampling. A sound whose wave ends has to end with it. The gains that the tool reports come from a fit of the
// reference and the output below 1 kHz. A stereo wave whose channels hold nearly the same samples, and a sound with
// little energy there, give no gains.
//
// A sound fails below 20 dB in the band. The fits leave out the voice's first frame. The band's filter spans samples on
// each side of each sample that it measures. A sound that plays for less than twice the filter's length after its first
// frame is judged by the ratio beyond a fixed filter instead. A wave that ends within its first frame counts apart. At
// a step above 1, the console's resampler puts images of a sound's higher frequencies into the band too. When the band
// holds less than a tenth of such a sound's energy, the better of the two ratios judges it. A sound also passes when
// the residual where it plays lies 80 dB or more below full scale. The output holds 16-bit samples. Their rounding
// alone can keep a quiet sound's ratio below 20 dB. A tone that swells can fit best whole periods early. When a sound
// fits below 20 dB from the start that the search finds, a start at the output's onset takes its place if it fits
// better. On every console, a stream sound below 20 dB whose output reaches full scale counts apart when it passes with
// the residual at full scale taken as zero, and its end is checked: the mix of a stream's tracks can clip, and the fit
// can't follow it.
//
// A wave sound counts apart when its settings change it beyond a gain: a filter, or an envelope that fades it in or
// holds it below full level. The tool also counts the wave sounds that the WAV export plays otherwise than the game.
// The export plays note 0 at the sound's pitch times the note's. The game plays note 0 at the sound's pitch.
//
// On the 3DS, each sound plays on ntdlib's archive mode of the 3DS, on its model of the DSP's audio firmware, in strict
// mixing (song/ctr_model.h). The DSP's output runs at about 32,728.5 Hz. nn::snd turns a pitch into the DSP's step at
// 32,728 Hz, and the tool takes that rate. The output follows the engine by four frames: three of silence and one of
// the DSP. The tool takes a sound's end four frames after the frame where the engine ends it. The 3DS's players read no
// filters, no sends of a stream's tracks and no pitch of a stream sound. A stream sound plays up to 8 channels: no more
// than its entry gives. A stream's voices keep the pan curve 0. An AAC stream plays as the preview plays it: the
// system's decoder decodes it into a stream file of 16-bit PCM, with the loop that the sound's entry gives
// (song::PlayableStream). The reference is that file's samples. The AAC test checks the decoding.
//
// On the Wii, the tool takes the game's build of NW4R SND from the main.dol of the extracted disc that holds the
// archive, or else the build that the archive's version gives. An import does the same. Each wave sound plays on
// ntdlib's model of NW4R SND and AX, with ntdlib's model of AX's microcode in place of the DSP. The model's output runs
// at AX's 32 kHz. NW4R SND plays note 0 of a wave sound, and reads only its wave and its envelope. The reference is
// that wave at the sound's pitch. A wave sound of the Wii has no filter. A stream sound plays on ntdlib's model of NW4R
// SND's stream player, at the stream's rate: the Wii's stream sounds have no pitch. The build's player selects the
// tracks (rvl::snd::StrmPlayer::TracksOf), and each track's voice plays the track's first two channels. The builds
// before March 2008 play the stream's first two channels at full volume and in the centre. A stream sound that the
// build's player can't play counts apart. The model plays every track at the stream file's volume, and the mix of a
// stream's layers can clip (see above). A game's code sets the tracks' volumes as it plays the stream.
//
// The tool prints a line for each stream sound, and for each wave sound that fails by its ratio or ends at another
// time. It then prints a summary. It returns 1 when such a sound doesn't count apart, and 2 when the archive can't be
// read. With a sound's name, it checks that sound alone, and prints the ratios in each half second.
//
// usage: hokuto_check_players <archive> [sound]

#include "check_players.h"

namespace hokuto::tools::players
{

namespace
{

// Runs the console's model with the sound that `start` starts, until nothing sounds or for `limit` frames.
template <class ArchiveModel, class Start>
Run RunModel(const sar::SoundArchive& archive, std::size_t limit, const Start& start)
{
    Run run;
    ArchiveModel model(archive);
    run.rate = ArchiveModel::kSampleRate;
    run.frame_samples = ArchiveModel::kFrameSamples;
    if (!start(*model.engine))
    {
        return run;
    }
    run.started = true;
    while (run.frames < limit && model.engine->IsBusy())
    {
        model.engine->RunFrame();
        std::span<const int16_t> output;
        if constexpr (std::is_same_v<ArchiveModel, cafe::ArchiveModel>)
        {
            output = model.system.Output();
        }
        else
        {
            output = model.renderer.DeviceOutput();
        }
        for (const int16_t sample : output)
        {
            run.output.push_back(static_cast<float>(sample) / 32768.0f);
        }
        run.frames++;
    }
    run.ended = !model.engine->IsBusy();

    return run;
}

// Runs ntdlib's archive mode of the 3DS in strict mixing with the sound that `start` starts, until nothing sounds or
// for `limit` frames. The model runs kTailFrames frames past the engine's end, more than the output's lag. The output
// of each frame counts. The run's frames end with the output's lag after the engine's end.
template <class Start>
Run RunCtrModel(const sar::SoundArchive& archive, std::size_t limit, const Start& start)
{
    using hokuto::song::CtrModel;
    constexpr std::size_t kLagFrames = CtrModel::kSilence / CtrModel::kFrameSamples + 1;

    Run run;
    CtrModel model;
    run.rate = CtrModel::kSampleRate;
    run.frame_samples = CtrModel::kFrameSamples;
    if (!model.Start(archive) || !start(model.Engine()))
    {
        return run;
    }
    run.started = true;
    std::size_t frames = 0;
    std::optional<std::size_t> engine_end;
    while (frames < limit && model.IsBusy())
    {
        if (!model.RunFrame())
        {
            break;
        }

        for (const int16_t sample : model.Output())
        {
            run.output.push_back(static_cast<float>(sample) / 32768.0f);
        }
        frames++;
        if (!engine_end && !model.Engine().IsBusy())
        {
            engine_end = frames;
        }
    }
    run.ended = !model.IsBusy();
    run.frames = engine_end ? std::min(frames, *engine_end + kLagFrames) : frames;

    return run;
}

// Runs ntdlib's model of NW4R SND and AX for the Wii's `archive`, on `build` of NW4R SND, with the sound that `start`
// starts, until nothing sounds or for `limit` frames.
template <class Start>
Run RunWiiModel(const rsar::SoundArchive& archive, const ntdlib::SndBuild& build, std::size_t limit, const Start& start)
{
    Run run;
    rvl::ArchiveModel model;
    model.build = build;
    model.StartWithRenderer(archive);
    run.rate = ntdlib::rvl::ax::kSampleRate;
    run.frame_samples = static_cast<std::size_t>(ntdlib::rvl::ax::kSamplesPerFrame);
    if (!start(*model.engine))
    {
        return run;
    }
    run.started = true;
    std::vector<int16_t> output;
    while (run.frames < limit && model.engine->IsBusy())
    {
        output.clear();
        if (!model.RunFrame(output))
        {
            break;
        }
        for (const int16_t sample : output)
        {
            run.output.push_back(static_cast<float>(sample) / 32768.0f);
        }
        run.frames++;
    }
    run.ended = !model.engine->IsBusy();

    return run;
}

// What a wave sound plays, and how its WAV export differs from the game.
struct WaveSoundFacts
{
    std::string name;
    uint32_t wave_archive = 0;
    uint32_t wave = 0;
    float pitch = 1.0f;
    bool filtered = false;     // a low-pass or a biquad filter
    bool shaped = false;       // an envelope that fades it in or holds it below full level
    bool panned = false;       // a pan, a surround pan, or a pan curve other than the default
    bool sent = false;         // sends other than the main bus at full level
    bool muted = false;        // a main send of 0
    bool export_wave = false;  // the WAV export plays another wave
    bool export_pitch = false; // the WAV export plays at another pitch
    std::size_t notes = 0;
    std::size_t events = 0;
};

// The facts of wave sound `sound`. Throws std::runtime_error when its file or its wave is missing.
WaveSoundFacts FactsOf(const sar::SoundArchive& archive, uint32_t sound)
{
    const sar::SoundInfo& info = archive.Sounds()[sound];
    const std::span<const uint8_t> file =
        hokuto::song::ItemFile(archive, {hokuto::song::ItemKind::kWaveSound, sound, 0});
    const sar::WaveSoundFile parsed = sar::WaveSoundFile::Parse(file, hokuto::song::ItemOptions(archive));
    if (info.wave_sound_index >= parsed.sounds.size())
    {
        throw std::runtime_error("the file lacks the wave sound");
    }
    const sar::WaveSoundFile::WaveSound& wave_sound = parsed.sounds[info.wave_sound_index];
    if (wave_sound.notes.empty() || wave_sound.notes.front().wave_id_index >= parsed.waves.size())
    {
        throw std::runtime_error("the sound plays no wave");
    }
    const sar::WaveId& id = parsed.waves[wave_sound.notes.front().wave_id_index];
    const std::optional<std::size_t> wave_archive =
        sar::ItemIndex(id.wave_archive_item, sar::kWaveArchiveItem, archive.WaveArchives().size());
    if (!wave_archive)
    {
        throw std::runtime_error("the archive lacks the wave archive");
    }

    WaveSoundFacts facts;
    facts.name = info.name;
    facts.wave_archive = static_cast<uint32_t>(*wave_archive);
    facts.wave = id.index;
    facts.pitch = wave_sound.pitch;
    facts.filtered = wave_sound.low_pass != 64 || wave_sound.biquad_type != 0;
    facts.shaped = wave_sound.adshr.attack < 127 || wave_sound.adshr.sustain < 127;
    facts.panned = wave_sound.pan != 64 || wave_sound.span != 0 || info.pan_curve != 0;
    facts.sent = wave_sound.main_send != 127 || wave_sound.aux_sends != std::array<uint8_t, 3>{};
    facts.muted = wave_sound.main_send == 0;
    facts.notes = wave_sound.notes.size();

    // The WAV export's note.
    try
    {
        const hokuto::song::WaveSoundNote note = hokuto::song::WaveSoundNoteOf(archive, sound);
        facts.export_wave = note.wave_archive != facts.wave_archive || note.wave != facts.wave;
        facts.export_pitch = note.pitch != facts.pitch;
        facts.events = note.events;
    }
    catch (const std::exception&)
    {
        facts.export_wave = true;
    }

    return facts;
}

// The facts of the Wii's wave sound `sound`, read with ntdlib alone. NW4R SND plays note 0 of the sound, at the sound's
// pitch, and reads only the note's wave and envelope. The wave lies in the wave data of the sound's file, by the file's
// ID (song::WaveArchivesOf). AX mixes an aux bus back into the output only through its effect, and the preview plays
// none. Only a main send below full level counts as another send. Throws std::runtime_error when the file or its wave
// is missing.
WaveSoundFacts FactsOf(const rsar::SoundArchive& archive, uint32_t sound)
{
    const rsar::SoundInfo& info = archive.Sounds()[sound];
    const rsar::WaveSoundFile parsed =
        rsar::WaveSoundFile::Parse(archive.FileData(info.file_id), ntdlib::rvl::snd::kRsarOptions);
    if (info.sub_index < 0 || static_cast<std::size_t>(info.sub_index) >= parsed.Sounds().size())
    {
        throw std::runtime_error("the file lacks the wave sound");
    }
    const rsar::WaveSoundFile::WaveSound& wave_sound = parsed.Sounds()[static_cast<std::size_t>(info.sub_index)];
    if (wave_sound.notes.empty() || wave_sound.notes.front().wave_index < 0)
    {
        throw std::runtime_error("the sound plays no wave");
    }
    const rsar::WaveSoundFile::Note& note = wave_sound.notes.front();

    WaveSoundFacts facts;
    facts.name = info.name;
    facts.wave_archive = info.file_id;
    facts.wave = static_cast<uint32_t>(note.wave_index);
    facts.pitch = wave_sound.pitch;
    facts.shaped = note.attack < 127 || note.sustain < 127;
    facts.panned = wave_sound.pan != 64 || wave_sound.surround_pan != 0 || info.pan_curve != 0;
    facts.sent = wave_sound.main_send != 127;
    facts.muted = wave_sound.main_send == 0;
    facts.notes = wave_sound.notes.size();

    // The WAV export's note.
    try
    {
        const hokuto::song::WaveSoundNote exported = hokuto::song::WaveSoundNoteOf(archive, sound);
        facts.export_wave = exported.wave_archive != facts.wave_archive || exported.wave != facts.wave;
        facts.export_pitch = exported.pitch != facts.pitch;
        facts.events = exported.events;
    }
    catch (const std::exception&)
    {
        facts.export_wave = true;
    }

    return facts;
}

// Plays a wave sound of `facts` that plays `pcm` at `volume`, and compares the output with the wave. `run_for` runs the
// console's model with the sound for the seconds that it takes. The model's output runs at `rate`. `archive` names the
// archive in the lines that the check prints. With `detail`, the comparison prints the ratios in each half second.
template <class RunFor>
void CheckWaveSound(const std::string& archive, const WaveSoundFacts& facts, const sar::Pcm& pcm, uint8_t volume,
                    double rate, const RunFor& run_for, bool detail, WaveSoundTotals& totals)
{
    totals.export_wave += facts.export_wave ? 1 : 0;
    totals.export_pitch += !facts.export_wave && facts.export_pitch ? 1 : 0;
    totals.several_notes += facts.notes > 1 ? 1 : 0;
    totals.several_events += facts.events > 1 ? 1 : 0;
    totals.filtered += facts.filtered ? 1 : 0;
    totals.shaped += facts.shaped ? 1 : 0;
    totals.panned += facts.panned ? 1 : 0;
    totals.sent += facts.sent ? 1 : 0;

    // The console reads the wave at its rate times the sound's pitch.
    const double step = static_cast<double>(pcm.sample_rate) * static_cast<double>(facts.pitch) / rate;
    const bool loops = pcm.loop && pcm.loop_start < pcm.loop_end;
    const double seconds =
        loops ? kLongestSeconds
              : std::min(kLongestStreamSeconds, static_cast<double>(pcm.Length()) / (step * rate) + kPastEndSeconds);
    const Run run = run_for(seconds);
    if (!run.started)
    {
        std::printf("%s: %s doesn't start\n", archive.c_str(), facts.name.c_str());
        totals.failed++;
        return;
    }
    if (!loops && !run.ended)
    {
        std::printf("%s: %s doesn't end with its wave\n", archive.c_str(), facts.name.c_str());
        totals.endless++;
        totals.failed++;
        return;
    }
    std::vector<std::size_t> channels;
    for (std::size_t c = 0; c < std::min<std::size_t>(pcm.channels.size(), 2); c++)
    {
        channels.push_back(c);
    }
    const Reference reference(pcm, channels);
    if (detail)
    {
        std::printf(
            "  wave %u of wave archive %u: %zu channels, %zu samples at %u Hz%s; the pitch %.6f, the step %.6f\n",
            facts.wave, facts.wave_archive, pcm.channels.size(), static_cast<std::size_t>(pcm.Length()),
            pcm.sample_rate, loops ? ", looped" : "", static_cast<double>(facts.pitch), step);
    }
    const Comparison comparison = Compare(run, reference, step, detail);
    if (detail)
    {
        std::printf("  the start at %zu, the place %.2f, the step %.6f\n", comparison.start, comparison.place,
                    comparison.step);
    }
    if (comparison.energy == 0.0)
    {
        if (Silent(pcm, channels) || volume == 0 || facts.muted)
        {
            totals.silent++;
        }
        else if (static_cast<double>(pcm.Length()) / step <= static_cast<double>(run.frame_samples) &&
                 Onset(run.output))
        {
            totals.first_frame++;
        }
        else
        {
            std::printf("%s: %s plays nothing\n", archive.c_str(), facts.name.c_str());
            totals.failed++;
        }
        return;
    }

    const bool counted_apart = facts.filtered || facts.shaped;
    (counted_apart ? totals.apart : totals.comparable).Add(facts.name, comparison, step);
    if (!Passes(comparison) || WrongLength(comparison))
    {
        char band[64];
        if (comparison.band_measured)
        {
            std::snprintf(band, sizeof(band), "%.1f dB below %.0f Hz", comparison.band_snr, comparison.band);
        }
        else
        {
            std::snprintf(band, sizeof(band), "too short for the band");
        }
        std::printf(
            "%s: %s: %.1f dB as heard, %.1f dB beyond a fixed filter, %s, the residual at %.1f dB, ends %s frames "
            "after its wave%s\n",
            archive.c_str(), facts.name.c_str(), comparison.snr, comparison.filtered_snr, band,
            comparison.residual_level, comparison.end_frames ? std::to_string(*comparison.end_frames).c_str() : "-",
            counted_apart ? " (counted apart)" : "");
        totals.failed += counted_apart ? 0 : 1;
    }

    // The gains over the sound's volume, with the default pan curve: of a mono wave at the centre, and of each channel
    // of a stereo wave. Either pan mode keeps a stereo wave's channels on their sides there.
    if (!facts.panned && !facts.sent && !counted_apart && volume > 0)
    {
        const double share = volume / 127.0;
        if (comparison.low_share < kLeastLowShare ||
            (comparison.gains.size() == 2 && Alike(pcm, channels[0], channels[1])))
        {
            totals.without_gains++;
        }
        else if (comparison.gains.size() == 1)
        {
            totals.mono.Add(comparison.gains[0][0] / share);
            totals.mono.Add(comparison.gains[0][1] / share);
        }
        else
        {
            totals.stereo_direct.Add(comparison.gains[0][0] / share);
            totals.stereo_direct.Add(comparison.gains[1][1] / share);
            totals.stereo_cross.Add(comparison.gains[0][1] / share);
            totals.stereo_cross.Add(comparison.gains[1][0] / share);
        }
    }
}

// Checks stream sound `sound` of `archive` against `pcm`, its stream of `stream_rate` decoded, played at `pitch` (see
// the top of the file). The channels of `tracks` play, each once, up to `most_channels`. `run_for` runs the console's
// model with the sound for a number of seconds at most. The tracks at the centre give gains where `gains` holds: on the
// Switch and the Wii U, with the default pan curve only.
template <class RunFor>
void CheckStreamSound(const std::string& archive, const std::string& sound, const sar::Pcm& pcm, uint32_t stream_rate,
                      uint8_t volume, double pitch, const std::vector<PlayedTrack>& tracks, std::size_t most_channels,
                      bool gains, double rate, const RunFor& run_for, bool detail, StreamTotals& totals)
{
    std::vector<std::size_t> channels;
    for (const PlayedTrack& track : tracks)
    {
        for (const uint8_t channel : track.channels)
        {
            if (channel < pcm.channels.size() && channel < most_channels &&
                std::find(channels.begin(), channels.end(), channel) == channels.end())
            {
                channels.push_back(channel);
            }
        }
    }
    if (channels.empty())
    {
        totals.silent++;
        return;
    }

    const double step = static_cast<double>(stream_rate) * pitch / rate;
    const double first_pass = static_cast<double>(pcm.Length()) / (step * rate);
    const double seconds =
        std::min(kLongestStreamSeconds, first_pass + (pcm.loop ? kPastLoopSeconds : kPastEndSeconds));
    const bool cut_short = !pcm.loop && first_pass + kPastEndSeconds > kLongestStreamSeconds;
    const Run run = run_for(seconds);
    if (!run.started)
    {
        std::printf("%s: %s doesn't start\n", archive.c_str(), sound.c_str());
        totals.failed++;
        return;
    }
    const Reference reference(pcm, channels);
    const Comparison comparison = Compare(run, reference, step, detail);
    if (comparison.energy == 0.0)
    {
        if (Silent(pcm, channels) || volume == 0)
        {
            totals.silent++;
        }
        else
        {
            std::printf("%s: %s plays nothing\n", archive.c_str(), sound.c_str());
            totals.failed++;
        }
        return;
    }
    totals.tally.Add(sound, comparison, step);
    std::printf(
        "%s: %s: %zu channels at %u Hz, %s, %.1f s; %.1f dB as heard, %.1f dB beyond a fixed filter, %.1f "
        "dB below %.0f Hz, the step %.2g off the pitch's, the start at %zu, the gains",
        archive.c_str(), sound.c_str(), channels.size(), stream_rate, pcm.loop ? "looped" : "once", first_pass,
        comparison.snr, comparison.filtered_snr, comparison.band_snr, comparison.band, comparison.step / step - 1.0,
        comparison.start);
    for (const std::array<double, 2>& gain : comparison.gains)
    {
        std::printf(" %.4f/%.4f", gain[0], gain[1]);
    }
    if (comparison.end_frames)
    {
        std::printf(", ends %lld frames after its stream", *comparison.end_frames);
    }
    if (Judged(comparison) < kLeastSnr)
    {
        std::printf(", the residual at %.1f dB", comparison.residual_level);
    }

    // The output clips where the mix of the tracks reaches full scale. The fit then can't follow it. A sound that fails
    // the fit counts apart when it passes with the residual at those samples taken as zero. A wrong length or an end
    // that never comes still fails.
    const auto full_scale = static_cast<std::size_t>(std::count_if(
        run.output.begin(), run.output.end(), [](float sample) { return std::abs(sample) >= kFullScale; }));
    bool clipped = false;
    if (full_scale > 0)
    {
        std::printf(", %zu samples at full scale", full_scale);
        if (!Passes(comparison))
        {
            const Comparison unclipped = WithoutFullScale(run, reference, step, comparison);
            std::printf(", %.1f dB without them", Judged(unclipped));
            clipped = Passes(unclipped);
        }
    }
    std::printf("\n");
    if (WrongLength(comparison) || (!pcm.loop && !cut_short && !run.ended))
    {
        totals.failed++;
    }
    else if (!Passes(comparison))
    {
        (clipped ? totals.clipped : totals.failed)++;
    }

    // The gains over the volumes: of each track at the centre that plays one channel or a pair. Either pan mode keeps a
    // pair's channels on their sides there. A track that plays a channel the run left out (`most_channels`) has no
    // gains.
    const auto index = [&](uint8_t channel)
    {
        return static_cast<std::size_t>(std::find(channels.begin(), channels.end(), channel) - channels.begin());
    };
    const auto fitted = [&](uint8_t channel)
    {
        return channel < pcm.channels.size() && index(channel) < channels.size();
    };
    for (const PlayedTrack& track : tracks)
    {
        if (track.pan != 64 || track.span != 0 || !gains || track.volume == 0 || volume == 0 ||
            track.channels.empty() || !fitted(track.channels[0]) ||
            (track.channels.size() > 1 && track.channels[1] < pcm.channels.size() && !fitted(track.channels[1])))
        {
            continue;
        }

        const double gain_of_volumes = volume / 127.0 * track.volume / 127.0;
        if (comparison.low_share < kLeastLowShare ||
            (track.channels.size() > 1 && track.channels[1] < pcm.channels.size() &&
             Alike(pcm, track.channels[0], track.channels[1])))
        {
            totals.without_gains++;
        }
        else if (track.channels.size() == 1)
        {
            const std::array<double, 2>& gain = comparison.gains[index(track.channels[0])];
            totals.mono.Add(gain[0] / gain_of_volumes);
            totals.mono.Add(gain[1] / gain_of_volumes);
        }
        else if (track.channels[1] < pcm.channels.size())
        {
            const std::array<double, 2>& left = comparison.gains[index(track.channels[0])];
            const std::array<double, 2>& right = comparison.gains[index(track.channels[1])];
            totals.direct.Add(left[0] / gain_of_volumes);
            totals.direct.Add(right[1] / gain_of_volumes);
            totals.cross.Add(left[1] / gain_of_volumes);
            totals.cross.Add(right[0] / gain_of_volumes);
        }
    }
}

// The rate at which the Wii mixes, and the samples of an AX frame.
const double kRate = ntdlib::rvl::ax::kSampleRate;
const double kAxFrameSamples = ntdlib::rvl::ax::kSamplesPerFrame;

// Checks stream sound `sound` of `archive`, with the entry `info`, against the decoding of its file in `streams`, as
// the game's `build` of NW4R SND plays it. `name` is the archive's name, and `only` the sound that the run checks, if
// it checks one. Counts the sound in `stream_totals`.
void CheckWiiStreamSound(const std::shared_ptr<const rsar::SoundArchive>& archive, const ntdlib::SndBuild& build,
                         const std::map<uint32_t, std::vector<uint8_t>>& streams, uint32_t sound,
                         const rsar::SoundInfo& info, const std::string& name, const std::string& only,
                         StreamTotals& stream_totals)
{
    stream_totals.sounds++;
    const std::optional<uint32_t> file = hokuto::song::StreamFileOf(*archive, sound);
    const auto found = file ? streams.find(*file) : streams.end();
    if (found == streams.end())
    {
        stream_totals.absent++;
        return;
    }

    rsar::StreamFile stream;
    sar::Pcm pcm;
    try
    {
        stream = rsar::StreamFile::Read(found->second);
        pcm = rsar::DecodeStream(stream);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s: %s\n", name.c_str(), info.name.c_str(), error.what());
        stream_totals.failed++;
        return;
    }

    // The tracks that the build's player plays. A track's voice plays the track's first two channels. The Wii's
    // stream sounds have no pitch.
    const std::optional<std::vector<rvl::StrmPlayer::Track>> tracks =
        rvl::StrmPlayer::TracksOf(build, info, archive->Version(), stream);
    if (!tracks)
    {
        std::printf("%s: %s: the game's build of NW4R SND plays no track of the stream\n", name.c_str(),
                    info.name.c_str());
        stream_totals.refused++;
        return;
    }
    std::vector<PlayedTrack> played;
    for (const rvl::StrmPlayer::Track& track : *tracks)
    {
        if (track.active)
        {
            const std::size_t count = std::min<std::size_t>(track.channels.size(), 2);
            played.push_back({{track.channels.begin(), track.channels.begin() + static_cast<std::ptrdiff_t>(count)},
                              track.volume,
                              track.pan,
                              0});
        }
    }
    const std::vector<uint8_t>& bytes = found->second;
    const auto run_for = [&](double seconds)
    {
        const auto limit = static_cast<std::size_t>(seconds * kRate / kAxFrameSamples) + 8;
        const auto start_stream = [sound, &bytes](rvl::Engine& engine)
        {
            return engine.StartStreamSound(sound, bytes) != nullptr;
        };
        return RunWiiModel(*archive, build, limit, start_stream);
    };
    CheckStreamSound(name, info.name, pcm, stream.sample_rate, info.volume, 1.0, played, rvl::StrmPlayer::kMaxChannels,
                     true, kRate, run_for, !only.empty(), stream_totals);
}

// Checks the wave sounds and the stream sounds of the Wii's archive at `path`, in `bytes`, or only the sound `only`
// (see the top of the file).
int CheckWii(const std::filesystem::path& path, std::vector<uint8_t> bytes, const std::string& only)
{
    const std::string name = xsflib::Utf8(path.filename());
    std::shared_ptr<const rsar::SoundArchive> archive;
    std::map<uint32_t, std::vector<uint8_t>> streams;
    std::optional<ntdlib::SndBuild> read;
    ntdlib::SndBuild build;
    try
    {
        archive = hokuto::song::LoadWiiArchive(std::move(bytes), name, ntdlib::rvl::snd::kRsarOptions);
        streams = hokuto::song::ReadStreams(*archive, path);
        read = GameBuildOf(path);
        build = read.value_or(ntdlib::sequence_export::BuildOfRsarVersion(archive->Version()));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    WaveSoundTotals totals;
    StreamTotals stream_totals;
    for (uint32_t sound = 0; sound < archive->Sounds().size(); sound++)
    {
        const rsar::SoundInfo& info = archive->Sounds()[sound];
        if (!only.empty() && info.name != only)
        {
            continue;
        }
        if (info.type == rsar::SoundType::kStream)
        {
            CheckWiiStreamSound(archive, build, streams, sound, info, name, only, stream_totals);
            continue;
        }
        if (info.type != rsar::SoundType::kWaveSound)
        {
            continue;
        }

        totals.wave_sounds++;
        WaveSoundFacts facts;
        sar::Pcm pcm;
        try
        {
            facts = FactsOf(*archive, sound);
            pcm = hokuto::song::DecodeItemWave(*archive, facts.wave_archive, facts.wave);
        }
        catch (const std::exception&)
        {
            totals.missing++;
            continue;
        }
        const auto run_for = [&](double seconds)
        {
            const auto limit = static_cast<std::size_t>(seconds * kRate / kAxFrameSamples) + 8;
            return RunWiiModel(*archive, build, limit,
                               [sound](rvl::Engine& engine) { return engine.StartWaveSound(sound) != nullptr; });
        };
        CheckWaveSound(name, facts, pcm, info.volume, kRate, run_for, !only.empty(), totals);
    }

    const std::string build_text = BuildText(read);
    std::printf("%s: NW4R SND of %s, mixed at %.0f Hz\n", name.c_str(), build_text.c_str(), kRate);
    PrintWaveSounds(name, totals);
    PrintStreamSounds(name, stream_totals);

    return totals.failed + stream_totals.failed > 0 ? 1 : 0;
}

} // namespace

} // namespace hokuto::tools::players

namespace
{

namespace atk = ntdlib::nx::atk;
namespace cafe = ntdlib::cafe::snd;
namespace rsar = ntdlib::rsar;
namespace rvl = ntdlib::rvl::snd;
namespace sar = ntdlib::sar;
using hokuto::tools::IsWiiArchive;
using hokuto::tools::players::CheckStreamSound;
using hokuto::tools::players::CheckWaveSound;
using hokuto::tools::players::CheckWii;
using hokuto::tools::players::FactsOf;
using hokuto::tools::players::PlayedTrack;
using hokuto::tools::players::PrintStreamSounds;
using hokuto::tools::players::PrintWaveSounds;
using hokuto::tools::players::RunCtrModel;
using hokuto::tools::players::RunModel;
using hokuto::tools::players::StreamTotals;
using hokuto::tools::players::WaveSoundFacts;
using hokuto::tools::players::WaveSoundTotals;

// Checks stream sound `sound` of `archive`, with the entry `info`, against the decoding of its file in `streams`.
// `run_model` runs the console's model. The model mixes at `rate`. `ctr` says whether the console is the 3DS. `name` is
// the archive's name, and `only` the sound that the run checks, if it checks one. Counts the sound in `stream_totals`.
template <class RunModelFn>
void CheckStream(const std::shared_ptr<const sar::SoundArchive>& archive,
                 const std::map<uint32_t, std::vector<uint8_t>>& streams, uint32_t sound, const sar::SoundInfo& info,
                 const std::string& name, const std::string& only, bool ctr, double rate, const RunModelFn& run_model,
                 StreamTotals& stream_totals)
{
    stream_totals.sounds++;
    const std::optional<uint32_t> file = hokuto::song::StreamFileOf(*archive, sound);
    const auto found = file ? streams.find(*file) : streams.end();
    if (found == streams.end())
    {
        stream_totals.absent++;
        return;
    }

    std::vector<uint8_t> bytes;
    sar::StreamFile stream;
    sar::Pcm pcm;
    try
    {
        const bool aac = hokuto::song::IsAdts(found->second);
        bytes = aac ? hokuto::song::PlayableStream(
                          found->second, hokuto::song::AacStreamOf(*archive, sound).value_or(hokuto::song::AacStream()))
                    : found->second;
        stream = sar::StreamFile::Read(bytes);
        pcm = sar::DecodeStream(stream);
        stream_totals.aac += aac ? 1 : 0;
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s: %s\n", name.c_str(), info.name.c_str(), error.what());
        stream_totals.failed++;
        return;
    }

    // The tracks that play. The 3DS plays 8 channels at most, and the other consoles 16.
    ntdlib::nwsnd::StreamTracks tracks;
    if (ctr)
    {
        const ntdlib::ctr::snd::StreamTracks played = ntdlib::ctr::snd::StreamTracksOf(info.stream, stream);
        tracks.valid = played.valid;
        tracks.tracks = played.tracks;
    }
    else
    {
        tracks = ntdlib::nwsnd::StreamTracksOf(info.stream, stream);
    }
    std::vector<PlayedTrack> played;
    for (std::size_t t = 0; t < tracks.tracks.size(); t++)
    {
        if ((tracks.valid >> t & 1) != 0)
        {
            const sar::StreamTrackInfo& track = tracks.tracks[t];
            played.push_back({track.channels, track.volume, track.pan, track.span});
        }
    }
    const std::size_t most_channels =
        ctr ? std::min<std::size_t>(info.stream.channel_count, ntdlib::ctr::snd::StreamSoundPlayer::kMaxChannels) : 16;
    const auto run_for = [&](double seconds)
    {
        const auto start_stream = [sound, &bytes](auto& engine)
        {
            return engine.StartStreamSound(sound, bytes) != nullptr;
        };
        return run_model(seconds, start_stream);
    };
    CheckStreamSound(name, info.name, pcm, stream.sample_rate, info.volume,
                     ctr ? 1.0 : static_cast<double>(info.stream.pitch), played, most_channels,
                     ctr || info.pan_curve == 0, rate, run_for, !only.empty(), stream_totals);
}

// Checks wave sound `sound` of `archive`, with the entry `info`, against the decoding of its wave. `run_model` runs the
// console's model. The model mixes at `rate`. `name` is the archive's name, and `only` the sound that the run checks, if it
// checks one. Counts the sound in `waves`.
template <class RunModelFn>
void CheckWave(const sar::SoundArchive& archive, uint32_t sound, const sar::SoundInfo& info, const std::string& name,
               const std::string& only, double rate, const RunModelFn& run_model, WaveSoundTotals& waves)
{
    waves.wave_sounds++;
    WaveSoundFacts facts;
    sar::Pcm pcm;
    try
    {
        facts = FactsOf(archive, sound);
        pcm = hokuto::song::DecodeItemWave(archive, facts.wave_archive, facts.wave);
    }
    catch (const std::exception&)
    {
        waves.missing++;
        return;
    }
    const auto run_for = [&](double seconds)
    {
        return run_model(seconds, [sound](auto& engine) { return engine.StartWaveSound(sound) != nullptr; });
    };
    CheckWaveSound(name, facts, pcm, info.volume, rate, run_for, !only.empty(), waves);
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        std::fprintf(stderr, "usage: hokuto_check_players <archive> [sound]\n");
        return 2;
    }
    const std::string only = arguments.size() > 2 ? arguments[2] : std::string();

    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    std::vector<uint8_t> bytes;
    if (xsflib::ReadFile(path, bytes) && IsWiiArchive(bytes))
    {
        return CheckWii(path, std::move(bytes), only);
    }
    const std::string name = xsflib::Utf8(path.filename());
    std::shared_ptr<const sar::SoundArchive> archive;
    std::map<uint32_t, std::vector<uint8_t>> streams;
    try
    {
        archive = hokuto::song::OpenArchive(path);
        streams = hokuto::song::ReadStreams(*archive, path);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    const bool wii_u = archive->ReadOptions().flavor == sar::Flavor::kCafe;
    const bool ctr = archive->ReadOptions().flavor == sar::Flavor::kCtr;
    const double rate = ctr     ? static_cast<double>(hokuto::song::CtrModel::kSampleRate)
                        : wii_u ? cafe::ArchiveModel::kSampleRate
                                : atk::ArchiveModel::kSampleRate;
    const double frame_samples = ctr     ? static_cast<double>(hokuto::song::CtrModel::kFrameSamples)
                                 : wii_u ? cafe::ArchiveModel::kFrameSamples
                                         : atk::ArchiveModel::kFrameSamples;

    // Runs the console's model for `seconds` at most with the sound that `start` starts.
    const auto run_model = [&](double seconds, const auto& start)
    {
        const auto limit = static_cast<std::size_t>(seconds * rate / frame_samples) + 8;
        if (ctr)
        {
            return RunCtrModel(*archive, limit, start);
        }

        return wii_u ? RunModel<cafe::ArchiveModel>(*archive, limit, start)
                     : RunModel<atk::ArchiveModel>(*archive, limit, start);
    };

    WaveSoundTotals waves;
    StreamTotals stream_totals;

    for (uint32_t sound = 0; sound < archive->Sounds().size(); sound++)
    {
        const sar::SoundInfo& info = archive->Sounds()[sound];
        if (!only.empty() && info.name != only)
        {
            continue;
        }

        if (info.type == sar::SoundType::kWaveSound)
        {
            CheckWave(*archive, sound, info, name, only, rate, run_model, waves);
        }
        else if (info.type == sar::SoundType::kStream)
        {
            CheckStream(archive, streams, sound, info, name, only, ctr, rate, run_model, stream_totals);
        }
    }

    PrintWaveSounds(name, waves);
    PrintStreamSounds(name, stream_totals);

    return stream_totals.failed + waves.failed > 0 ? 1 : 0;
}
