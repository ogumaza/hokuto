// SPDX-License-Identifier: MIT

// Checks the edits of banks, waves, streams and wave sounds through their models (src/edit/banks.h, src/edit/waves.h
// and src/edit/streams.h), on a sound archive of the 3DS, the Wii U or the Switch with the group files and the streams
// beside it. The tool imports the archive as a .hok file. The export has to give the game's archive and group files
// back byte for byte.
//
// The edits follow one another. Each one exports the file, and the next one edits the archive that the export builds.
// Before a bank's or a wave's edits, its file in that archive has to hold the game's bytes: the earlier edits leave it
// alone.
//
// Each bank that ntdlib reads goes through its model twice: with each region's volume halved, and with a copy of each
// instrument at a new program after the bank's programs. The exported bank has to read back as the model. Each program
// with an instrument then plays a note on ntdlib's model of the console for 1.5 s: before the edits, and after each.
// The browser plays a note the same way (song::NotePreview). A copy has to play the instrument's samples. With the
// volumes halved, the level has to fall by the ratio of the volumes of the region that the note plays, within 0.2 dB. A
// program that plays nothing, or plays below -60 dB, counts apart. So does the level of a region above full volume: the
// console limits its gains. The bank keeps its halved volumes for the later edits. A 3DS archive plays its notes on
// ntdlib's model of the DSP's audio firmware. On the 3DS, a note that peaks at half of full scale or more counts apart
// too: the DSP's limiter compresses it (kLimitedPeak). A bank whose file an earlier bank of the archive's list plays
// counts apart: the earlier bank's edits changed the file.
//
// Each wave of each wave archive inside the archive goes through its model three times. First at three quarters of its
// rate, with a loop from the place nearest its middle where the samples stay, up to three quarters of its length. Then
// from that version with its end back at the wave's length. Then from the game's wave in another encoding: PCM16 for a
// wave in DSP-ADPCM, and DSP-ADPCM for one in PCM. ntdlib then decodes each new wave as the console decodes it. The
// first edit has to keep the samples up to the new end, and the second all of them. In PCM16, every sample has to stay.
// In DSP-ADPCM, the tool measures the ratio of signal to noise. Decoded from the loop's context as the console decodes
// it, each later pass of a loop in DSP-ADPCM has to give the first pass's samples. The other encoding has to keep the
// rate, the channels and the loop. A loop in DSP-ADPCM may move forward to the next frame. The wave keeps the second
// edit for the later edits.
//
// Each stream that the file keeps goes through its values twice: at three quarters of its rate with a loop from the
// first block past its middle, and in the other encoding. ntdlib then decodes each new stream. The first edit has to
// keep every sample, and so does 16-bit PCM. The other encoding has to keep the rate, the channels and the loop. Its
// loop may move forward to a block. In DSP-ADPCM, the tool measures the ratio of signal to noise. Each later pass of a
// loop in DSP-ADPCM has to decode to the first pass's samples, after both edits. The file keeps the first edit. The
// properties of a stream refuse an AAC stream of the 3DS, and such a stream counts apart.
//
// Each wave sound whose values ntdlib reads takes a new pan, surround pan, pitch and sends where its file has places
// for them, and the next wave of the archive. ntdlib has to read the new values and the new wave back, and every other
// wave sound of the file has to keep its values.
//
// After the edits, every file of the archive but the banks, the wave archives, the wave sound files and the groups'
// files has to hold the game's bytes. The file with the edits has to come back from its binary and text forms.
//
// The tool prints a line for each problem, then a summary. It returns 1 when it finds a problem, and 2 when the archive
// can't be read.
//
// With `--part k/n`, the tool edits only part k of n of the items: the banks, the waves, the streams and the wave
// sounds whose places in their lists leave k - 1 over when divided by n. The edits of each part follow one another. n
// runs side by side cover a large archive faster than one run. Each run checks the archive's other files and both forms
// of the file too. With `--streams`, the tool edits only the streams, and with `--wave-sounds` only the wave sounds.
//
// An archive of the Wii (RSAR) takes the same edits through the Wii's models (ntdlib's rsar::BankFile and
// rsar::WsdFile). The tool imports it with the game's build of NW4R SND: from the main.dol that `--dol` names, or else
// from the main.dol of the extracted disc that holds the archive. Without either, the archive's version gives the
// build. The notes play on ntdlib's model of NW4R SND and AX with that build. A bank of the Wii keeps its waves in its
// wave data. A wave sound file does too. The waves of each such wave data take the edits of a wave archive's waves. A
// later pass of a loop decodes as the Wii's DSP decodes it (kWiiLoopDecoding). The bank's edits have to keep its waves,
// and the waves' edits the banks' instruments and the wave sounds' values. A bank whose regions hold values that the
// Bank Editor doesn't show, such as a surround pan, counts apart with the reason. Each wave sound also takes a new
// envelope, and its wave is the next wave of its file's wave data. The import reads the streams beside the archive.
// Each one takes the edits of the other consoles' streams through its stream file of the Wii (RSTM). A loop can start
// at the stream's first block. The edits have to keep the stream's tracks. After them, the file table of the exported
// archive has to give each stream's size. A file table that ntdlib's model of the archive can't read counts apart.
//
// With --model, the file goes into the model form after the import (format::InModelForm), and the archive that the
// model builds has to be the game's. Of the files that the edits make, the first 64 and every 64th after them have to
// save in both forms and open again as they were (tools/checked_archive.h). So does the file with all the edits.
//
// usage: hokuto_check_banks [--model] [--streams | --wave-sounds] [--part <k>/<n>] [--dol <main.dol>] <archive>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/wave/adpcm.h"
#include "xsflib/binary.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "edit/banks.h"
#include "edit/streams.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/preview.h"
#include "song/streams.h"

namespace
{

namespace sar = ntdlib::sar;
namespace rsar = ntdlib::rsar;
namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace song = hokuto::song;

// How long a note plays, in seconds: its hold and the start of its release.
constexpr double kNoteSeconds = 1.5;

// How far the level of a note with its volume halved may lie from the ratio of the volumes, in dB, and the level below
// which a note counts apart. On the Wii U, AX ramps a voice's volume across each frame in whole steps of its 1.15
// volume. Each step rounds towards zero, and a ramp stops short of its target by up to 95 of the volume's units. A
// halved volume falls short by twice as much of its level. Where a note's envelope moves, its level can then lie 0.1 dB
// off the ratio.
constexpr double kLevelTolerance = 0.2;
constexpr double kQuietLevel = -60.0;

// A region's full volume. A region above it plays through the console's limits on gains, and its level doesn't follow
// the ratio of the volumes.
constexpr int kFullVolume = 127;

// The peak of a note on the 3DS from which its level doesn't follow the ratio of the volumes. nn::snd turns the DSP's
// limiter on. The limiter takes about 0.9 * a * a dB off the level of a note that peaks at a of full scale without it.
// With its volume halved, a note that peaks at half of full scale then falls by about 0.17 dB less than the ratio, and
// louder notes by more. With the limiter off, the levels follow the ratio.
constexpr float kLimitedPeak = 0.5F;

// The share of a wave's or a stream's rate, length and middle that the edits take.
constexpr double kShare = 0.75;

// The ratio of signal to noise in dB below which a wave in DSP-ADPCM counts apart.
constexpr double kLowSignalToNoise = 20.0;

// A channel's DSP-ADPCM frames and its info, for the check of a loop's later passes.
using AdpcmChannel = std::pair<std::span<const uint8_t>, std::optional<sar::DspAdpcmInfo>>;

// How a console decodes the later passes of a loop in DSP-ADPCM: its decoder's sums, and whether the header of the
// frame at the loop's start comes from the loop's context rather than from the frame.
struct LoopDecoding
{
    ntdlib::wave::DspAdpcmSum sum = ntdlib::wave::DspAdpcmSum::kWrap;
    bool context_header = false;
};

// The Wii's DSP loads the loop's context at the loop's end, and its accelerator reads each frame's header at the
// frame's start (ntdlib's rvl::ax::Renderer). Its sums don't wrap.
constexpr LoopDecoding kWiiLoopDecoding = {ntdlib::wave::DspAdpcmSum::kWide, false};

// What the checks found, for the summary.
struct Totals
{
    std::size_t problems = 0;

    std::size_t banks = 0;
    std::size_t unread_banks = 0;
    std::size_t refused_banks = 0; // banks of the Wii that ntdlib reads and the Bank Editor refuses
    std::size_t shared_banks = 0;  // banks whose file an earlier bank plays
    std::size_t programs = 0;
    std::size_t quiet_programs = 0;
    std::size_t loud_programs = 0;
    std::size_t limited_programs = 0; // notes on the 3DS that the DSP's limiter compresses
    double level_error = 0.0;

    std::size_t waves = 0;
    std::size_t unread_waves = 0;
    std::size_t adpcm_loops = 0;
    std::size_t adpcm_waves = 0;
    std::size_t low_snr_waves = 0;
    double least_snr = std::numeric_limits<double>::infinity();

    std::size_t streams = 0;
    std::size_t aac_streams = 0;
    double least_stream_snr = std::numeric_limits<double>::infinity();

    std::size_t wave_sounds = 0;
    std::size_t unread_wave_sounds = 0;

    std::size_t other_files = 0;
};

// The file that the edits so far have made, and the archive that it builds.
struct Edited
{
    format::File file;
    std::shared_ptr<const sar::SoundArchive> archive;
};

// Prints a problem and counts it.
void Problem(Totals& totals, const std::string& where, const std::string& what)
{
    std::printf("%s: %s\n", where.c_str(), what.c_str());
    totals.problems++;
}

// The archive that `file` builds, with its group files.
std::shared_ptr<const sar::SoundArchive> Built(const format::File& file, const std::string& name)
{
    hokuto::tools::CheckSaved(file, true);

    return song::LoadArchive(format::ExportArchive(file), name, format::ExportGroupFiles(file));
}

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

// The samples of `preview` for up to kNoteSeconds. Two floats for each stereo frame.
std::vector<float> Rendered(song::NotePreview& preview)
{
    const auto frames = static_cast<std::size_t>(kNoteSeconds * preview.SampleRate());
    std::vector<float> samples(frames * 2);
    std::size_t done = 0;
    while (done < frames)
    {
        const std::size_t block = std::min<std::size_t>(1024, frames - done);
        const std::size_t rendered = preview.Render(samples.data() + done * 2, block);
        done += rendered;
        if (rendered < block)
        {
            break;
        }
    }
    samples.resize(done * 2);

    return samples;
}

// The samples of a note of program `program` of bank `bank` of `archive` on ntdlib's model of the console, for up to
// kNoteSeconds. The note plays as the browser plays it.
std::vector<float> Note(const std::shared_ptr<const sar::SoundArchive>& archive, uint32_t bank, uint32_t program)
{
    song::NotePreview preview(archive, bank, program);
    return Rendered(preview);
}

// The same on the Wii's `archive`, on ntdlib's model of NW4R SND and AX with `build`.
std::vector<float> WiiNote(const std::shared_ptr<const rsar::SoundArchive>& archive, uint32_t bank, uint32_t program,
                           const ntdlib::SndBuild& build)
{
    song::NotePreview preview(archive, bank, program, build);
    return Rendered(preview);
}

// The highest magnitude of `samples`, against full scale.
float Peak(std::span<const float> samples)
{
    float peak = 0.0F;
    for (const float sample : samples)
    {
        peak = std::max(peak, std::abs(sample));
    }

    return peak;
}

// The level of `samples` in dB against full scale, or -infinity for silence.
double Level(std::span<const float> samples)
{
    double sum = 0.0;
    for (const float sample : samples)
    {
        sum += static_cast<double>(sample) * sample;
    }

    return sum > 0.0 ? 10.0 * std::log10(sum / static_cast<double>(samples.size())) : -INFINITY;
}

// The ratio of signal to noise in dB of `heard` against `expected`, over the first `length` samples of each channel.
double SignalToNoise(const std::vector<std::vector<int16_t>>& expected, const std::vector<std::vector<int16_t>>& heard,
                     uint32_t length)
{
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t c = 0; c < expected.size() && c < heard.size(); ++c)
    {
        for (uint32_t i = 0; i < length && i < expected[c].size() && i < heard[c].size(); ++i)
        {
            const double difference = static_cast<double>(heard[c][i]) - expected[c][i];
            signal += static_cast<double>(expected[c][i]) * expected[c][i];
            noise += difference * difference;
        }
    }

    return noise > 0.0 ? 10.0 * std::log10(signal / noise) : INFINITY;
}

// Whether the first `length` samples of each channel of `a` and `b` are the same.
bool SameStart(const std::vector<std::vector<int16_t>>& a, const std::vector<std::vector<int16_t>>& b, uint32_t length)
{
    if (a.size() != b.size())
    {
        return false;
    }

    for (std::size_t c = 0; c < a.size(); ++c)
    {
        if (a[c].size() < length || b[c].size() < length ||
            !std::equal(a[c].begin(), a[c].begin() + length, b[c].begin()))
        {
            return false;
        }
    }

    return true;
}

// Whether `after`, a wave or a stream in another encoding, keeps the rate, the channels and the loop of `before`. The
// loop may start up to `slack` - 1 samples later. The samples up to the new start then repeat after the loop's end, and
// the end moves as far as the start.
bool KeepsValues(const ntdlib::wave::Pcm& before, const ntdlib::wave::Pcm& after, uint32_t slack)
{
    if (after.sample_rate != before.sample_rate || after.channels.size() != before.channels.size() ||
        after.loop != before.loop || after.loop_start < before.loop_start)
    {
        return false;
    }

    const uint32_t moved = after.loop_start - before.loop_start;
    return moved < slack && after.loop_end == before.loop_end + moved;
}

// How the console of `flavor` decodes the later passes of a loop in DSP-ADPCM. The Switch and the 3DS read the
// predictor and the scale from the frame's header when the loop starts at a frame's start. The Wii U takes them from
// the loop's context there too. The 3DS's sums don't wrap (ntdlib's sar/decode.h).
LoopDecoding LoopDecodingOf(sar::Flavor flavor)
{
    return {flavor == sar::Flavor::kCtr ? ntdlib::wave::DspAdpcmSum::kWide : ntdlib::wave::DspAdpcmSum::kWrap,
            flavor == sar::Flavor::kCafe};
}

// The samples that the console decodes in a DSP-ADPCM channel's loop from the loop's second pass on, by `decoding`.
// Each such pass starts from the loop's context: the two samples before the loop's start, and a frame's predictor and
// scale. hokuto_check_replace decodes later passes the same way.
std::vector<int16_t> LaterPass(std::span<const uint8_t> samples, const sar::DspAdpcmInfo& adpcm, uint32_t loop_start,
                               uint32_t loop_end, const LoopDecoding& decoding)
{
    const ntdlib::wave::DspAdpcmSum sum = decoding.sum;
    std::vector<int16_t> pass;
    int32_t header = adpcm.loop_pred_scale & 0x7F;
    int32_t yn1 = adpcm.loop_yn1;
    int32_t yn2 = adpcm.loop_yn2;
    for (uint32_t i = loop_start; i < loop_end; i++)
    {
        // Each frame of 8 bytes holds a header and 14 samples of 4 bits.
        const std::size_t frame = std::size_t{i} / 14 * 8;
        const uint32_t place = i % 14;
        if (place == 0 && (i != loop_start || !decoding.context_header))
        {
            header = samples[frame];
        }
        const uint8_t pair = samples[frame + 1 + place / 2];
        const int32_t nibble = place % 2 == 0 ? pair >> 4 : pair & 0xF;
        const int32_t predictor = (header >> 4) & 7;
        const int16_t sample =
            ntdlib::wave::DspAdpcmSample(nibble >= 8 ? nibble - 16 : nibble, header & 0xF, adpcm.coefs[2 * predictor],
                                         adpcm.coefs[2 * predictor + 1], yn1, yn2, sum);
        pass.push_back(sample);
        yn2 = yn1;
        yn1 = sample;
    }

    return pass;
}

// The DSP-ADPCM info of a channel of the 3DS, the Wii U or the Switch.
sar::DspAdpcmInfo InfoOf(const sar::DspAdpcmInfo& info)
{
    return info;
}

// The DSP-ADPCM info of a channel of the Wii, in the terms of the later consoles.
sar::DspAdpcmInfo InfoOf(const rsar::DspAdpcmInfo& info)
{
    return {info.coefs, info.pred_scale, info.yn1, info.yn2, info.loop_pred_scale, info.loop_yn1, info.loop_yn2};
}

// The DSP-ADPCM frames and the info of each of `channels`, a wave's or a stream's.
template <typename Channels>
std::vector<AdpcmChannel> AdpcmChannels(const Channels& channels)
{
    std::vector<AdpcmChannel> result;
    for (const auto& channel : channels)
    {
        result.emplace_back(channel.samples, channel.adpcm ? std::optional(InfoOf(*channel.adpcm)) : std::nullopt);
    }

    return result;
}

// Whether every later pass of the loop of each channel decodes to the first pass's samples, `first`, by `decoding`.
bool LaterPassesMatch(const std::vector<AdpcmChannel>& channels, const std::vector<std::vector<int16_t>>& first,
                      uint32_t loop_start, uint32_t loop_end, const LoopDecoding& decoding)
{
    for (std::size_t c = 0; c < channels.size() && c < first.size(); ++c)
    {
        if (!channels[c].second)
        {
            return false;
        }

        const std::vector<int16_t> later =
            LaterPass(channels[c].first, *channels[c].second, loop_start, loop_end, decoding);
        if (!std::equal(later.begin(), later.end(), first[c].begin() + loop_start))
        {
            return false;
        }
    }

    return true;
}

// `bank` with each region's volume halved, rounded up.
sar::BankFile Halved(sar::BankFile bank)
{
    for (std::optional<sar::BankFile::Instrument>& instrument : bank.instruments)
    {
        if (!instrument)
        {
            continue;
        }

        for (sar::BankFile::KeyRegion& keys : instrument->key_regions)
        {
            for (sar::BankFile::Region& region : keys.regions)
            {
                region.values.volume = static_cast<uint8_t>((region.values.volume + 1) / 2);
            }
        }
    }

    return bank;
}

// `bank` with a copy of each instrument at a new program after the bank's programs, and the programs of the copies by
// those of their instruments.
std::pair<sar::BankFile, std::vector<std::pair<uint32_t, uint32_t>>> WithCopies(sar::BankFile bank)
{
    std::vector<std::pair<uint32_t, uint32_t>> copies;
    const auto programs = static_cast<uint32_t>(bank.instruments.size());
    for (uint32_t program = 0; program < programs; ++program)
    {
        if (bank.instruments[program])
        {
            const auto copy = static_cast<uint32_t>(bank.instruments.size());
            bank = edit::WithInstrumentCopy(std::move(bank), program, copy);
            copies.emplace_back(program, copy);
        }
    }

    return {std::move(bank), copies};
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

// Checks the notes of bank `bank` before its edits, in `before`, and after them, in `halved` and `copied`. `copies`
// gives the programs of the copies by those of their instruments.
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
        totals.level_error = std::max(totals.level_error, error);
        if (error > kLevelTolerance)
        {
            Problem(totals, where,
                    "program " + std::to_string(program) + " with its volume halved lies " + std::to_string(error) +
                        " dB off the ratio of its volumes");
        }
    }
}

// Checks bank `bank` of the game's archive `game` with its volumes halved and with copies of its instruments, from
// `edited`. `edited` then holds the halved volumes.
void CheckBank(Edited& edited, const sar::SoundArchive& game, uint32_t bank, const std::string& name, Totals& totals)
{
    const std::string where = name + ", " + game.Banks()[bank].name;
    for (uint32_t earlier = 0; earlier < bank; ++earlier)
    {
        if (game.Banks()[earlier].file_id == game.Banks()[bank].file_id)
        {
            totals.shared_banks++;
            return;
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
        return;
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
        return;
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
}

// The multiple of `step` nearest `target`.
uint32_t Nearest(double target, uint32_t step)
{
    return static_cast<uint32_t>(std::lround(target / step)) * step;
}

// Checks the edits of a wave that start from `edited`: at a new rate with a new loop, then with its end back. `model`
// and `present` are the wave's model and its decoded samples. Returns the file and the archive with the end back.
Edited CheckLoop(const Edited& edited, uint32_t wave_archive, uint32_t wave, const format::WaveModel& model,
                 const sar::Pcm& present, const std::string& name, const std::string& where, Totals& totals)
{
    // The new rate, the loop from near the middle and the earlier end, then the end back at the wave's length.
    const bool adpcm = model.encoding == sar::WaveEncoding::kDspAdpcm;
    format::WaveModel looped = model;
    looped.rate = std::max<uint32_t>(static_cast<uint32_t>(model.rate * kShare), 1);
    looped.loop = true;
    looped.loop_end = static_cast<uint32_t>(model.length * kShare);
    looped.loop_start = std::min(Nearest(looped.loop_end / 2.0, adpcm ? edit::kDspAdpcmFrame : 1), looped.loop_end - 1);
    looped.loop_start -= adpcm ? looped.loop_start % edit::kDspAdpcmFrame : 0;
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

// Checks the edit of a wave that starts from `edited` and gives it another encoding. `model` and `present` are the
// wave's model and its decoded samples.
void CheckEncoding(const Edited& edited, uint32_t wave_archive, uint32_t wave, const format::WaveModel& model,
                   const sar::Pcm& present, const std::string& name, const std::string& where, Totals& totals)
{
    format::WaveModel encoded = model;
    encoded.encoding =
        model.encoding == sar::WaveEncoding::kDspAdpcm ? sar::WaveEncoding::kPcm16 : sar::WaveEncoding::kDspAdpcm;
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

// Checks wave `wave` of wave archive `wave_archive` of the game's archive `game` from `edited`: at a new rate with a
// new loop, with its end back, and in another encoding. `edited` then holds the wave with its end back.
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

// Checks the stream of file `file_id` that `edited` keeps: at a new rate with a loop at a block, and in the other
// encoding. `edited` then holds the stream with the loop.
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
    const uint32_t block = sar::StreamBlockSamples(values.encoding);
    edit::StreamValues looped = values;
    looped.rate = std::max<uint32_t>(static_cast<uint32_t>(values.rate * kShare), 1);
    looped.loop = true;
    looped.loop_start = (values.length / 2 + block - 1) / block * block;
    if (looped.loop_start >= values.length)
    {
        looped.loop_start = 0;
    }

    // The 3DS's tools never start a loop at the first block, and such a loop is made again. The short stream goes
    // without a loop there.
    if (looped.loop_start == 0 && edited.file.native->flavor == sar::Flavor::kCtr)
    {
        looped.loop = false;
    }
    edit::StreamValues encoded = values;
    encoded.encoding =
        values.encoding == sar::WaveEncoding::kDspAdpcm ? sar::WaveEncoding::kPcm16 : sar::WaveEncoding::kDspAdpcm;
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
                              LoopDecodingOf(edited.file.native->flavor)))
        {
            Problem(totals, where, "a later pass of the new loop decodes otherwise than its first");
        }

        // The other encoding keeps the rate, the channels and the loop. The loop moves forward to a block of the new
        // encoding: on the 3DS to the start of the block after the one that holds it (ntdlib's MakeStream). 16-bit PCM
        // keeps every sample. DSP-ADPCM comes close, and each later pass of the loop plays its first.
        const std::vector<uint8_t> encoded_bytes = edit::StreamWithValues(bytes, encoded);
        const sar::Pcm encoded_pcm = song::DecodeStreamFile(encoded_bytes);
        const bool ctr = edited.file.native->flavor == sar::Flavor::kCtr;
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
                                  encoded_pcm.loop_end, LoopDecodingOf(edited.file.native->flavor)))
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

// Whether two wave sounds' fields hold the same values and wave.
bool SameFields(const edit::WaveSoundFields& a, const edit::WaveSoundFields& b)
{
    return a.values.pan == b.values.pan && a.values.span == b.values.span && a.values.pitch == b.values.pitch &&
           a.values.main_send == b.values.main_send && a.values.aux_sends == b.values.aux_sends &&
           a.values.low_pass == b.values.low_pass && a.values.biquad_type == b.values.biquad_type &&
           a.values.biquad_value == b.values.biquad_value && a.values.adshr == b.values.adshr && a.wave == b.wave;
}

// Checks wave sound `sound` from `edited`: a new pan, surround pan, pitch and sends where its file has places for them,
// and the next of `waves`. `edited` then holds the new values.
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

// Checks that every file of `edited`'s archive that no edit reaches holds the bytes of the game's archive `game`: every
// file but the banks, the wave archives, the wave sound files and the groups' files. A group's file starts with CGRP on
// the 3DS and FGRP on the Wii U and the Switch.
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

// The part of the items that a run edits: part `k` of `n`, counted from 1.
struct Part
{
    uint32_t k = 1;
    uint32_t n = 1;
};

// Whether `part` holds the item at `place` in its list.
bool Holds(const Part& part, std::size_t place)
{
    return place % part.n == part.k - 1;
}

// The part that `text` names, such as "3/8", or nothing for text that names no part.
std::optional<Part> PartOf(std::string_view text)
{
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos)
    {
        return std::nullopt;
    }

    Part part;
    const std::string_view k = text.substr(0, slash);
    const std::string_view n = text.substr(slash + 1);
    const std::from_chars_result k_read = std::from_chars(k.data(), k.data() + k.size(), part.k);
    const std::from_chars_result n_read = std::from_chars(n.data(), n.data() + n.size(), part.n);
    if (k_read.ec != std::errc() || n_read.ec != std::errc() || k_read.ptr != k.data() + k.size() ||
        n_read.ptr != n.data() + n.size() || part.k == 0 || part.k > part.n)
    {
        return std::nullopt;
    }

    return part;
}

// The summary of a run whose part `heading` names, after `seconds`.
void PrintSummary(const std::string& heading, const Totals& totals, double seconds)
{
    const std::string refused =
        totals.refused_banks > 0 ? ", " + std::to_string(totals.refused_banks) + " refused" : std::string();
    std::printf(
        "%s: %zu banks (%zu unread%s, %zu sharing a file), %zu programs (%zu quiet, %zu above full volume, %zu "
        "limited), levels within %.3f dB; %zu waves (%zu unread), %zu loops in DSP-ADPCM, %zu waves to DSP-ADPCM at "
        "%.1f dB or more (%zu below %.0f dB); %zu streams",
        heading.c_str(), totals.banks, totals.unread_banks, refused.c_str(), totals.shared_banks, totals.programs,
        totals.quiet_programs, totals.loud_programs, totals.limited_programs, totals.level_error, totals.waves,
        totals.unread_waves, totals.adpcm_loops, totals.adpcm_waves,
        std::isinf(totals.least_snr) ? 0.0 : totals.least_snr, totals.low_snr_waves, kLowSignalToNoise, totals.streams);
    if (!std::isinf(totals.least_stream_snr))
    {
        std::printf(" (DSP-ADPCM at %.1f dB or more)", totals.least_stream_snr);
    }
    if (totals.aac_streams > 0)
    {
        std::printf(", %zu AAC streams apart", totals.aac_streams);
    }
    std::printf("; %zu wave sounds (%zu unread); %zu other files; %zu problems; %.1f s\n", totals.wave_sounds,
                totals.unread_wave_sounds, totals.other_files, totals.problems, seconds);
}

// The Wii. A bank of the Wii keeps its waves in its wave data, and so does a wave sound file. Hokuto takes such wave
// data for a wave archive, by its file's ID (song::WaveArchivesOf).

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
    format::WaveModel looped = model;
    looped.rate = std::max<uint32_t>(static_cast<uint32_t>(model.rate * kShare), 1);
    looped.loop = true;
    looped.loop_end = static_cast<uint32_t>(model.length * kShare);
    looped.loop_start = std::min(Nearest(looped.loop_end / 2.0, adpcm ? edit::kDspAdpcmFrame : 1), looped.loop_end - 1);
    looped.loop_start -= adpcm ? looped.loop_start % edit::kDspAdpcmFrame : 0;
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
    encoded.encoding =
        model.encoding == sar::WaveEncoding::kDspAdpcm ? sar::WaveEncoding::kPcm16 : sar::WaveEncoding::kDspAdpcm;
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

// `value` moved by `step` within 0 to 127.
uint8_t Moved(uint8_t value, int step)
{
    return static_cast<uint8_t>((value + step) % 128);
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
    const uint32_t block = sar::StreamBlockSamples(values.encoding);
    edit::StreamValues looped = values;
    looped.rate = std::max<uint32_t>(static_cast<uint32_t>(values.rate * kShare), 1);
    looped.loop = true;
    looped.loop_start = (values.length / 2 + block - 1) / block * block;
    if (looped.loop_start >= values.length)
    {
        looped.loop_start = 0;
    }
    edit::StreamValues encoded = values;
    encoded.encoding =
        values.encoding == sar::WaveEncoding::kDspAdpcm ? sar::WaveEncoding::kPcm16 : sar::WaveEncoding::kDspAdpcm;

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

// Checks `bytes`, the archive of the Wii at `path`, with the part `part` of the items. Run checks the other consoles'
// archives in the same manner. The banks and the waves take edits with `items`, the wave sounds with `wave_sounds`, and
// the streams beside the archive with `streams`. The game's build of NW4R SND comes from the main.dol at `dol_path`, or
// else from the main.dol of the extracted disc that holds the archive (song::FindMainDol). Without either, the
// archive's version gives the build.
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
    std::vector<std::pair<uint32_t, sar::BankFile>> halved;
    for (uint32_t bank = 0; bank < game->Banks().size() && items; ++bank)
    {
        if (Holds(part, bank))
        {
            if (std::optional<sar::BankFile> model = CheckWiiBank(edited, *game, bank, build, name, totals))
            {
                halved.emplace_back(bank, std::move(*model));
            }
        }
    }

    // The waves of each bank's and each wave sound file's wave data. They have to keep the values of the wave sounds.
    std::vector<std::pair<uint32_t, edit::WaveSoundFields>> sound_fields;
    for (uint32_t sound = 0; sound < game->Sounds().size(); ++sound)
    {
        if (game->Sounds()[sound].type == rsar::SoundType::kWaveSound)
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
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(*game))
    {
        std::size_t count = 0;
        try
        {
            count = song::WaveCountOf(*game, wave_archive.file_id);
        }
        catch (const std::exception&)
        {
            continue;
        }
        for (uint32_t wave = 0; wave < count; ++wave, ++place)
        {
            if (Holds(part, place) && items)
            {
                CheckWiiWave(edited, *game, wave_archive.file_id, wave, name, totals);
            }
        }
    }

    for (const auto& [sound, fields] : sound_fields)
    {
        if (!SameFields(edit::WaveSoundFieldsOf(*edited.archive, sound), fields))
        {
            Problem(totals, name + ", " + game->Sounds()[sound].name, "the edits of the waves changed its values");
        }
    }

    // The wave sounds and the streams of the run's part. An export writes the streams' sizes into the file table.
    for (uint32_t sound = 0; sound < game->Sounds().size() && wave_sounds; ++sound)
    {
        if (game->Sounds()[sound].type == rsar::SoundType::kWaveSound && Holds(part, sound))
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

// The tool, with the command line's `arguments`. Returns the tool's exit status.
int Run(std::vector<std::string> arguments)
{
    // The options, then the archive.
    hokuto::tools::TakeModelOption(arguments);
    const auto streams_flag = std::find(arguments.begin(), arguments.end(), "--streams");
    const bool streams_only = streams_flag != arguments.end();
    if (streams_only)
    {
        arguments.erase(streams_flag);
    }
    const auto wave_sounds_flag = std::find(arguments.begin(), arguments.end(), "--wave-sounds");
    const bool wave_sounds_only = wave_sounds_flag != arguments.end();
    if (wave_sounds_only)
    {
        arguments.erase(wave_sounds_flag);
    }
    const bool items = !streams_only && !wave_sounds_only;
    std::string dol_path;
    const auto dol_option = std::find(arguments.begin(), arguments.end(), "--dol");
    const bool dol_given = dol_option != arguments.end();
    if (dol_given && dol_option + 1 != arguments.end())
    {
        dol_path = *(dol_option + 1);
        arguments.erase(dol_option, dol_option + 2);
    }
    std::optional<Part> part = Part{};
    std::size_t at = 1;
    if (arguments.size() == 4 && arguments[1] == "--part")
    {
        part = PartOf(arguments[2]);
        at = 3;
    }
    if (arguments.size() != at + 1 || !part || (dol_given && dol_path.empty()))
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_banks [--model] [--streams | --wave-sounds] [--part <k>/<n>] "
                     "[--dol <main.dol>] <archive>\n");
        return 2;
    }

    // The game's archive. An archive of the Wii takes the checks of the Wii. The others are imported with the group
    // files and the streams beside them.
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[at]);
    const std::string name = xsflib::Utf8(path.filename());
    const hokuto::tools::SavesLine saves_line(name);
    const auto began = std::chrono::steady_clock::now();
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        std::fprintf(stderr, "%s can't be read\n", name.c_str());
        return 2;
    }
    if (bytes.size() >= 4 && std::equal(bytes.begin(), bytes.begin() + 4, "RSAR"))
    {
        return RunWii(path, bytes, dol_path, *part, items, !streams_only, !wave_sounds_only);
    }
    if (dol_given)
    {
        std::fprintf(stderr, "%s: --dol takes an archive of the Wii\n", name.c_str());
        return 2;
    }
    std::shared_ptr<const sar::SoundArchive> opened;
    Edited edited;
    try
    {
        opened = song::OpenArchive(path);
        edited.file = format::ImportArchive(bytes, opened->ReadOptions().flavor, opened->GroupFiles(),
                                            song::ReadStreams(*opened, path), opened->GroupFileGroups());
        edited.file = hokuto::tools::CheckedForm(std::move(edited.file), bytes);
        edited.archive = Built(edited.file, name);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    // The unedited archive comes back byte for byte.
    Totals totals;
    const std::shared_ptr<const sar::SoundArchive> game = edited.archive;
    if (format::ExportArchive(edited.file) != bytes || format::ExportGroupFiles(edited.file) != opened->GroupFiles())
    {
        Problem(totals, name, "the unedited archive doesn't come back byte for byte");
    }

    // The banks, the waves of each wave archive inside the archive, the streams and the wave sounds, of the run's part.
    for (uint32_t bank = 0; bank < game->Banks().size() && items; ++bank)
    {
        if (Holds(*part, bank))
        {
            CheckBank(edited, *game, bank, name, totals);
        }
    }
    std::vector<sar::WaveId> waves;
    for (uint32_t wave_archive = 0; wave_archive < game->WaveArchives().size(); ++wave_archive)
    {
        std::size_t count = 0;
        try
        {
            count = song::WaveCountOf(*game, wave_archive);
        }
        catch (const std::exception&)
        {
            continue;
        }
        for (uint32_t wave = 0; wave < count; ++wave)
        {
            waves.push_back({sar::ItemId(sar::kWaveArchiveItem, wave_archive), wave});
            if (Holds(*part, waves.size() - 1) && items)
            {
                CheckWave(edited, *game, wave_archive, wave, name, totals);
            }
        }
    }
    std::vector<uint32_t> streams;
    for (const auto& [file_id, stream] : edited.file.native->streams)
    {
        streams.push_back(file_id);
    }
    for (std::size_t i = 0; i < streams.size() && !wave_sounds_only; ++i)
    {
        if (Holds(*part, i))
        {
            CheckStream(edited, streams[i], name, totals);
        }
    }
    for (uint32_t sound = 0; sound < game->Sounds().size() && !waves.empty() && !streams_only; ++sound)
    {
        if (game->Sounds()[sound].type == sar::SoundType::kWaveSound && Holds(*part, sound))
        {
            CheckWaveSound(edited, sound, waves, name, totals);
        }
    }

    // The other files keep their bytes, and the file comes back from both forms.
    CheckOtherFiles(edited, *game, name, totals);
    if (!hokuto::tools::SavesAndOpens(edited.file))
    {
        Problem(totals, name, "the file with the edits doesn't come back from both forms");
    }

    // The summary, with the part where the run edits one.
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    const std::string heading =
        part->n > 1 ? name + " (part " + std::to_string(part->k) + " of " + std::to_string(part->n) + ")" : name;
    PrintSummary(heading, totals, seconds);

    return totals.problems == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        return Run(xsflib::console::Arguments(argc, argv));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
