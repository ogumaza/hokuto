// SPDX-License-Identifier: MIT

// The round Streams of hokuto_check_ds_replace.

#include "check_ds_replace.h"

namespace hokuto::tools::ds_replace
{

namespace
{

// Whether `made`, a stream that came from `pcm`, contains its samples: the same up to the old end, then the loop's
// samples again. 16-bit PCM stores them exactly. In another encoding, the samples up to the old end and those after it
// are each `floor` dB or more above their noise. `lowest` receives the lower ratio. The loop starts at a block, at or
// after the old start.
bool HoldsStream(const sdat::Stream& made, const ntdlib::wave::Pcm& pcm, double floor, double& lowest)
{
    if (made.channels != pcm.channels.size() || made.sample_rate != pcm.sample_rate)
    {
        return false;
    }
    const std::size_t length = pcm.channels[0].size();
    if (pcm.loop)
    {
        if (made.loop == 0 || pcm.loop_start >= length || made.loop_start < pcm.loop_start ||
            made.loop_start % made.block_samples != 0 || made.samples != made.loop_start + (length - pcm.loop_start))
        {
            return false;
        }
    }
    else if (made.loop != 0 || made.samples != length)
    {
        return false;
    }

    for (uint32_t channel = 0; channel < made.channels; channel++)
    {
        const std::vector<int16_t> decoded = sdat::DecodeStream(made, channel);
        const std::span<const int16_t> old_samples = pcm.channels[channel];
        if (old_samples.size() != length || decoded.size() < length)
        {
            return false;
        }

        // The samples after the old end repeat the samples of the loop, round the loop when the loop is shorter than
        // they are.
        std::vector<int16_t> repeated;
        for (std::size_t i = length; pcm.loop && i < decoded.size(); i++)
        {
            repeated.push_back(old_samples[pcm.loop_start + (i - length) % (length - pcm.loop_start)]);
        }
        const std::span<const int16_t> head = std::span<const int16_t>(decoded).first(length);
        const std::span<const int16_t> tail = std::span<const int16_t>(decoded).subspan(length, repeated.size());
        if (made.encoding == sdat::WaveEncoding::kPcm16)
        {
            if (!std::ranges::equal(head, old_samples) || !std::ranges::equal(tail, repeated))
            {
                return false;
            }
            continue;
        }

        for (const double ratio :
             {Ratio(old_samples, head).value_or(kExactRatio), Ratio(repeated, tail).value_or(kExactRatio)})
        {
            lowest = std::min(lowest, ratio);
            if (ratio < floor)
            {
                return false;
            }
        }
    }

    return true;
}

// Checks that `edited`, the file with the edit `name`, exports `exported` again after both forms.
void CheckForms(const Game& game, const File& edited, const std::vector<uint8_t>& exported, const std::string& name,
                Totals& totals)
{
    if (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(edited))) != exported ||
        hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(edited))) != exported)
    {
        Fail(game, "streams", name + ": the file comes back otherwise from a form", totals);
    }
}

// Checks that each file of `after`, the archive with the edit `name`, but file `file_id` keeps its bytes.
void CheckOtherFilesStay(const Game& game, const sdat::Archive& after, uint32_t file_id, const std::string& name,
                         Totals& totals)
{
    const sdat::Archive& before = game.archive->archive;
    for (uint32_t id = 0; id < before.files.size(); id++)
    {
        if (id != file_id && !std::ranges::equal(before.File(id), after.File(id)))
        {
            Fail(game, "streams", name + ": file " + std::to_string(id) + " changed", totals);
        }
    }
}

// The message with which edit::WithDsFile (src/edit/ds_files.cpp) refuses an edit that changes the size of an archive
// that shares its file in the image with other data. No constant contains it. This copy has to follow that one, and a
// change of the refusal fails the check.
constexpr std::string_view kSharedFileRefusal =
    "Can't change the size of the sound archive. It shares its file in the image with other data, and Hokuto can't "
    "move that data.";

// Whether `error`, from an edit that changes the size of the game's archive, is the refusal of a new size that the
// archive makes when it shares its file in the image.
bool RefusesNewSize(const Game& game, const std::exception& error)
{
    return game.shares_file && std::string_view(error.what()) == kSharedFileRefusal;
}

constexpr double kFloorDb = 15.0;

// Checks a new rate and a loop from the first block past the middle of stream `stream`, whose file `file_id` contains
// `copy`, parsed as `parsed`. The other bytes of the file stay.
void CheckNewRateAndLoop(const Game& game, uint32_t stream, uint32_t file_id, const sdat::Stream& parsed,
                         const std::vector<uint8_t>& copy, const std::string& name, Totals& totals)
{
    hokuto::edit::DsStreamValues values = hokuto::edit::DsStreamValuesOf(*game.archive, stream);
    values.rate = values.rate * 3 / 4;
    values.loop = true;
    values.loop_start = (parsed.samples / 2 + parsed.block_samples) / parsed.block_samples * parsed.block_samples;
    const bool room = values.loop_start < parsed.samples;
    if (room)
    {
        const File edited = hokuto::edit::WithDsStream(game.file, *game.archive, stream, values);
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
        CheckForms(game, edited, exported, name, totals);
        const std::shared_ptr<const song::DsArchive> after = song::LoadDsArchive(exported, game.archive->build);
        CheckOtherFilesStay(game, after->archive, file_id, name, totals);
        const std::span<const uint8_t> made = after->archive.File(file_id);
        bool same = made.size() == copy.size();
        for (std::size_t i = 0; same && i < made.size(); i++)
        {
            const bool field = (i >= 0x19 && i < 0x1A) || (i >= 0x1C && i < 0x24);
            same = field || made[i] == copy[i];
        }
        const sdat::Stream back = sdat::Stream::Parse(made);
        if (!same || back.sample_rate != values.rate || back.loop == 0 || back.loop_start != values.loop_start)
        {
            Fail(game, "streams", name + ": the new rate and loop change other bytes", totals);
        }
    }
}

// Checks stream `stream` in the other encoding: the file `file_id` contains samples close to `pcm`.
void CheckOtherEncoding(const Game& game, uint32_t stream, uint32_t file_id, const sdat::Stream& parsed,
                        const ntdlib::wave::Pcm& pcm, const std::string& name, Totals& totals)
{
    hokuto::edit::DsStreamValues encoded = hokuto::edit::DsStreamValuesOf(*game.archive, stream);
    encoded.encoding =
        parsed.encoding == sdat::WaveEncoding::kImaAdpcm ? sdat::WaveEncoding::kPcm16 : sdat::WaveEncoding::kImaAdpcm;
    const File reencoded = hokuto::edit::WithDsStream(game.file, *game.archive, stream, encoded);
    const std::vector<uint8_t> reencoded_bytes = hokuto::format::ExportArchive(reencoded);
    CheckForms(game, reencoded, reencoded_bytes, name, totals);
    const std::shared_ptr<const song::DsArchive> reread = song::LoadDsArchive(reencoded_bytes, game.archive->build);
    CheckOtherFilesStay(game, reread->archive, file_id, name, totals);
    if (!HoldsStream(sdat::Stream::Parse(reread->archive.File(file_id)), pcm, kFloorDb, totals.lowest_stream_ratio))
    {
        Fail(game, "streams", name + ": the other encoding holds other samples", totals);
    }
}

// Checks the WAV file of `pcm`, the samples of stream `stream`, in both encodings: the stream that it makes and the
// archive that contains it in file `file_id`. The refusal of a new size is passed on to the caller. A stream of the
// file's size keeps the archive's size, and any refusal of it fails.
void CheckWavFile(const Game& game, uint32_t stream, uint32_t file_id, const ntdlib::wave::Pcm& pcm,
                  const std::string& name, Totals& totals)
{
    const std::size_t old_size = game.archive->archive.File(file_id).size();
    const std::vector<uint8_t> wav = song::PcmAsWav(pcm, name);
    for (const ntdlib::sar::WaveEncoding encoding :
         {ntdlib::sar::WaveEncoding::kPcm16, ntdlib::sar::WaveEncoding::kImaAdpcm})
    {
        const hokuto::edit::IncomingWave incoming = hokuto::edit::ReadIncomingDsStream(wav, encoding);
        const sdat::Stream made = sdat::Stream::Parse(incoming.bytes);
        if (!HoldsStream(made, pcm, kFloorDb, totals.lowest_stream_ratio))
        {
            Fail(game, "streams", name + ": its WAV file makes other samples", totals);
        }
        if (pcm.loop)
        {
            totals.stream_loops++;
            totals.stream_loops_moved += made.loop_start != pcm.loop_start ? 1 : 0;
        }
        std::optional<File> edited;
        try
        {
            edited = hokuto::edit::WithDsStreamBytes(game.file, *game.archive, stream, incoming.bytes);
        }
        catch (const std::exception& error)
        {
            if (incoming.bytes.size() != old_size && RefusesNewSize(game, error))
            {
                throw;
            }
            Fail(game, "streams", name + ": its WAV file is refused. " + error.what(), totals);
            continue;
        }
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(*edited);
        const std::shared_ptr<const song::DsArchive> after = song::LoadDsArchive(exported, game.archive->build);
        CheckOtherFilesStay(game, after->archive, file_id, name, totals);
        if (!std::ranges::equal(after->archive.File(file_id), incoming.bytes))
        {
            Fail(game, "streams", name + ": the archive doesn't hold the WAV file's stream", totals);
        }
    }
}

// Checks a new stream with the settings of stream `stream` and its file `copy`: its values, its samples and its name.
void CheckNewStream(const Game& game, uint32_t stream, const std::vector<uint8_t>& copy, const ntdlib::wave::Pcm& pcm,
                    const std::string& name, Totals& totals)
{
    const hokuto::edit::DsStreamSettings settings = hokuto::edit::DsStreamSettingsOf(*game.archive, stream);
    const File added = hokuto::edit::AddDsStream(game.file, name + "_NEW", copy, settings);
    const std::vector<uint8_t> added_bytes = hokuto::format::ExportArchive(added);
    CheckForms(game, added, added_bytes, name, totals);
    const std::shared_ptr<const song::DsArchive> with_new = song::LoadDsArchive(added_bytes, game.archive->build);
    CheckOtherFilesStay(game, with_new->archive, static_cast<uint32_t>(game.archive->archive.files.size()), name,
                        totals);
    const auto new_number = static_cast<uint32_t>(game.archive->archive.streams.size());
    if (with_new->archive.streams.size() != game.archive->archive.streams.size() + 1 ||
        hokuto::edit::DsStreamSettingsOf(*with_new, new_number) != settings ||
        !std::ranges::equal(song::DsStreamPcm(*with_new, new_number).channels, pcm.channels))
    {
        Fail(game, "streams", name + ": the new stream holds other values", totals);
    }
    if (game.archive->archive.has_symbols && song::DsStreamName(*with_new, new_number) != name + "_NEW")
    {
        Fail(game, "streams", name + ": the new stream holds another name", totals);
    }
}

// Checks stream `stream` of the game: its bytes from its fields and in its place, a new rate and loop, the other
// encoding, its WAV file and a new stream with its settings and its file. An archive that shares its file in the image
// with other data refuses a new size of the archive. The other encoding, a WAV file of another size and a new stream
// change the size. The stream counts apart when one of them is refused so. The other steps keep the size, and any
// refusal of them fails.
void CheckStream(const Game& game, uint32_t stream, Totals& totals)
{
    const std::string name = song::DsStreamName(*game.archive, stream);
    uint32_t file_id = 0;
    std::vector<uint8_t> copy;
    sdat::Stream parsed;
    ntdlib::wave::Pcm pcm;
    try
    {
        file_id = hokuto::edit::DsStreamFileId(*game.archive, stream);
        const std::span<const uint8_t> bytes = game.archive->archive.File(file_id);
        copy.assign(bytes.begin(), bytes.end());
        parsed = sdat::Stream::Parse(copy);
        if (sdat::StreamFile(parsed) != copy)
        {
            Fail(game, "streams", name + ": the file doesn't come back from its fields", totals);
        }

        // The samples as the browser plays them, at the field's rate.
        pcm = song::DsStreamPcm(*game.archive, stream);
        if (pcm.sample_rate != parsed.sample_rate)
        {
            Fail(game, "streams", name + ": the browser plays another rate", totals);
        }
        if (hokuto::format::ExportArchive(hokuto::edit::WithDsStreamBytes(game.file, *game.archive, stream, copy)) !=
            game.bytes)
        {
            Fail(game, "streams", name + ": its bytes in its place change the archive", totals);
        }

        // A new rate and loop.
        CheckNewRateAndLoop(game, stream, file_id, parsed, copy, name, totals);
    }
    catch (const std::exception& error)
    {
        Fail(game, "streams", name + ": " + error.what(), totals);
        return;
    }

    try
    {
        // The other encoding.
        CheckOtherEncoding(game, stream, file_id, parsed, pcm, name, totals);

        // Its WAV file, in both encodings.
        CheckWavFile(game, stream, file_id, pcm, name, totals);

        // A new stream with its settings and its file.
        CheckNewStream(game, stream, copy, pcm, name, totals);
        totals.streams++;
    }
    catch (const std::exception& error)
    {
        if (RefusesNewSize(game, error))
        {
            std::printf("%s: streams: %s stays apart: %s\n", game.name.c_str(), name.c_str(), error.what());
            totals.streams_apart++;
        }
        else
        {
            Fail(game, "streams", name + ": " + error.what(), totals);
        }
    }
}

} // namespace

void Streams(const Game& game, Totals& totals)
{
    for (const uint32_t stream : song::DsStreams(*game.archive))
    {
        CheckStream(game, stream, totals);
    }
}

} // namespace hokuto::tools::ds_replace
