// SPDX-License-Identifier: MIT

// The checks of hokuto_check_items on the items that go to another console and back.

#include "check_items.h"

namespace hokuto::tools::items
{

namespace
{

// A command that a player reaches, at its byte offset. The command gives its target as an offset too.
struct Reached
{
    friend bool operator==(const Reached&, const Reached&) = default;

    uint32_t offset = 0;
    ntdlib::commands::Command command;
};

// The commands of sequence file `file` of the console of `flavor` that its player can reach from the labels and from
// `starts`, in the order of their bytes. The player goes on after each command but an end, a return and a jump that
// aren't conditional. A jump, a call and a track start reach their targets too. The decoder reads the bytes that no
// command reaches as commands where it can. The byte order can change what it reads there. The list leaves those bytes
// out.
std::vector<Reached> ReachedCommands(std::span<const uint8_t> file, sar::Flavor flavor,
                                     std::span<const uint32_t> starts)
{
    using ntdlib::commands::Kind;
    const sar::SequenceFile read = sar::SequenceFile::Read(file);
    std::vector<ntdlib::nwsnd::LabelOffset> labels;
    for (const sar::Sequence::Label& label : read.labels)
    {
        labels.push_back({label.name, label.offset});
    }
    const ntdlib::nwsnd::Grammar grammar = song::GrammarOf(flavor);
    const ntdlib::nwsnd::Decoded decoded = ntdlib::nwsnd::DecodeSequence(read.data, labels, grammar, starts);
    const std::vector<ntdlib::commands::Command>& items = decoded.sequence.items;
    const std::vector<uint32_t> offsets = ntdlib::nwsnd::EncodeSequence(decoded.sequence, grammar).offsets;

    // The walk from the labels and the starts.
    std::vector<bool> reached(items.size(), false);
    std::vector<uint32_t> next(decoded.entries.begin(), decoded.entries.end());
    for (const ntdlib::commands::Label& label : decoded.sequence.labels)
    {
        next.push_back(label.item);
    }
    while (!next.empty())
    {
        const uint32_t item = next.back();
        next.pop_back();
        if (item >= items.size() || reached[item])
        {
            continue;
        }

        reached[item] = true;
        const ntdlib::commands::Command& command = items[item];
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            next.push_back(command.target);
        }
        const bool stops = command.kind == Kind::kEnd || command.kind == Kind::kReturn || command.kind == Kind::kJump;
        if (!stops || command.conditional)
        {
            next.push_back(item + 1);
        }
    }

    std::vector<Reached> commands;
    for (std::size_t i = 0; i < items.size(); i++)
    {
        if (!reached[i])
        {
            continue;
        }

        Reached command{offsets[i], items[i]};
        if (ntdlib::commands::HasTarget(command.command.kind) && !command.command.target_outside)
        {
            command.command.target = offsets[command.command.target];
        }
        commands.push_back(command);
    }

    return commands;
}

// Whether two sequence files hold the same labels at the same offsets.
bool SameLabels(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
    const std::vector<sar::Sequence::Label> first = sar::SequenceFile::Read(a).labels;
    const std::vector<sar::Sequence::Label> second = sar::SequenceFile::Read(b).labels;
    const auto same = [](const sar::Sequence::Label& x, const sar::Sequence::Label& y)
    {
        return x.name == y.name && x.offset == y.offset;
    };

    return std::equal(first.begin(), first.end(), second.begin(), second.end(), same);
}

// Whether `a` and `b` hold the same commands at the same offsets, but for the types and values of biquad filters
// (song::SameButBiquads).
bool SameButBiquads(std::span<const Reached> a, std::span<const Reached> b)
{
    const auto same = [](const Reached& x, const Reached& y)
    {
        return x.offset == y.offset && song::SameButBiquads(x.command, y.command);
    };

    return std::ranges::equal(a, b, same);
}

// Whether the commands of `file`, a sequence file of the console of `flavor`, overlap or jump into one another as its
// player reads them from its labels and `starts`. ConvertSequenceFile refuses such a file.
bool Overlapping(std::span<const uint8_t> file, sar::Flavor flavor, std::span<const uint32_t> starts)
{
    const sar::SequenceFile read = sar::SequenceFile::Read(file);
    std::vector<ntdlib::nwsnd::LabelOffset> labels;
    for (const sar::Sequence::Label& label : read.labels)
    {
        labels.push_back({label.name, label.offset});
    }

    return ntdlib::nwsnd::DecodeSequence(read.data, labels, song::GrammarOf(flavor), starts).opaque;
}

// Whether the commands of `file`, a sequence file of the console of `flavor`, that its player can reach from `starts`
// include one that the 3DS lacks: each such command alone fails the 3DS's encoder. Commands with targets leave out.
bool ReachesWhatTheCtrLacks(std::span<const uint8_t> file, sar::Flavor flavor, std::span<const uint32_t> starts)
{
    const ntdlib::nwsnd::Grammar grammar = song::GrammarOf(sar::Flavor::kCtr);
    for (const Reached& reached : ReachedCommands(file, flavor, starts))
    {
        if (ntdlib::commands::HasTarget(reached.command.kind))
        {
            continue;
        }

        ntdlib::commands::Sequence alone;
        alone.items = {reached.command};
        try
        {
            ntdlib::nwsnd::EncodeSequence(alone, grammar);
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }
    }

    return false;
}

} // namespace

std::string OtherConsoleProblem(const sar::SoundArchive& built, const sar::SoundArchive& opened, uint32_t sound,
                                sar::Flavor other, Totals& totals)
{
    // The starts of the sounds that play the file.
    const sar::Flavor flavor = opened.ReadOptions().flavor;
    const std::span<const uint8_t> theirs = opened.FileData(opened.Sounds()[sound].file_id);
    std::vector<uint32_t> starts = {opened.Sounds()[sound].start_offset};
    for (const uint32_t sharing : song::SoundsSharingFile(opened, sound))
    {
        starts.push_back(opened.Sounds()[sharing].start_offset);
    }

    // The reads of the file can throw too. The sound then fails like a conversion that throws.
    bool overlapping = false;
    bool lacking = false;
    try
    {
        overlapping = Overlapping(theirs, flavor, starts);
        lacking = !overlapping && other == sar::Flavor::kCtr && ReachesWhatTheCtrLacks(theirs, flavor, starts);
    }
    catch (const std::exception& error)
    {
        return std::string("fails: ") + error.what();
    }

    if (overlapping)
    {
        if (Refuses([&] { song::OtherConsoleSequenceFile(built, sound, other); }))
        {
            totals.overlapping++;
            return {};
        }

        return "goes to another console with commands that overlap";
    }
    if (lacking)
    {
        if (Refuses([&] { song::OtherConsoleSequenceFile(built, sound, other); }))
        {
            totals.refused++;
            return {};
        }

        return "goes to the 3DS with a command that the 3DS lacks";
    }

    try
    {
        const std::vector<uint8_t> ours = song::OtherConsoleSequenceFile(built, sound, other).file;
        sar::Options options = song::ItemOptions(opened);
        options.flavor = other;
        sar::Sequence::Parse(ours, options);
        const std::vector<Reached> reached = ReachedCommands(theirs, flavor, starts);
        const std::vector<Reached> there = ReachedCommands(ours, other, starts);
        if (!SameButBiquads(there, reached))
        {
            return "holds other commands";
        }
        totals.biquads += there == reached ? 0 : 1;

        // Bytes that no command reaches can come back otherwise. The decoder reads what it can of them as commands. The
        // byte order changes what it can read. Bytes 8 to 11 hold the version. A Wii U file of version 2.0.0 comes back
        // as 1.0.0.
        std::vector<uint8_t> back = song::ConvertSequenceFile(ours, other, flavor, starts).file;
        const std::vector<Reached> reached_back = ReachedCommands(back, flavor, starts);
        if (!SameLabels(back, theirs) || !SameButBiquads(reached_back, reached))
        {
            return "doesn't convert back to its commands";
        }
        if (back.size() >= 12 && theirs.size() >= 12 &&
            !std::equal(back.begin() + 8, back.begin() + 12, theirs.begin() + 8))
        {
            std::copy(theirs.begin() + 8, theirs.begin() + 12, back.begin() + 8);
            totals.versions++;
        }
        if (reached_back != reached)
        {
            totals.biquads_back++;
        }
        else if (!std::equal(back.begin(), back.end(), theirs.begin(), theirs.end()))
        {
            totals.unreached++;
        }
    }
    catch (const std::exception& error)
    {
        return std::string("fails: ") + error.what();
    }

    return {};
}

namespace
{

// The version in the header of `file`, a file of the 3DS, the Wii U or the Switch, in the byte order that its mark
// gives. Throws std::invalid_argument for a file without a byte order mark.
uint32_t VersionOf(std::span<const uint8_t> file)
{
    const bool big = file.size() > 5 && file[4] == 0xFE && file[5] == 0xFF;
    const bool little = file.size() > 5 && file[4] == 0xFF && file[5] == 0xFE;
    if (!big && !little)
    {
        throw std::invalid_argument("a file without a byte order mark");
    }

    return xsflib::Reader(file, big).U32(0x08);
}

// Whether two regions hold the same wave, key, volume, pan, pitch, flags and envelope.
bool SameRegion(const sar::VelocityRegion& a, const sar::VelocityRegion& b)
{
    return a.wave_id_index == b.wave_id_index && a.original_key == b.original_key && a.volume == b.volume &&
           a.pan == b.pan && a.pitch == b.pitch && a.ignore_note_off == b.ignore_note_off &&
           a.key_group == b.key_group && a.interpolation == b.interpolation && a.adshr.attack == b.adshr.attack &&
           a.adshr.decay == b.adshr.decay && a.adshr.sustain == b.adshr.sustain && a.adshr.hold == b.adshr.hold &&
           a.adshr.release == b.adshr.release;
}

// Whether two tables of waves name the same waves.
bool SameWaves(const std::vector<sar::WaveId>& a, const std::vector<sar::WaveId>& b)
{
    const auto same = [](const sar::WaveId& x, const sar::WaveId& y)
    {
        return x.wave_archive_item == y.wave_archive_item && x.index == y.index;
    };

    return std::equal(a.begin(), a.end(), b.begin(), b.end(), same);
}

// Whether two banks hold the same waves, and the same regions over the same keys and velocities.
bool SameBank(const sar::Bank& a, const sar::Bank& b)
{
    if (!SameWaves(a.waves, b.waves) || a.instruments.size() != b.instruments.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.instruments.size(); i++)
    {
        if (a.instruments[i].has_value() != b.instruments[i].has_value() ||
            (a.instruments[i] && a.instruments[i]->keys.size() != b.instruments[i]->keys.size()))
        {
            return false;
        }

        for (std::size_t k = 0; a.instruments[i] && k < a.instruments[i]->keys.size(); k++)
        {
            const sar::KeyRegion& x = a.instruments[i]->keys[k];
            const sar::KeyRegion& y = b.instruments[i]->keys[k];
            if (x.lo != y.lo || x.hi != y.hi || x.velocities.size() != y.velocities.size())
            {
                return false;
            }
            for (std::size_t v = 0; v < x.velocities.size(); v++)
            {
                const sar::KeyRegion::Vel& p = x.velocities[v];
                const sar::KeyRegion::Vel& q = y.velocities[v];
                if (p.lo != q.lo || p.hi != q.hi || p.region.has_value() != q.region.has_value() ||
                    (p.region && !SameRegion(*p.region, *q.region)))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

// Whether two wave sound files hold the same waves, and the same wave sounds with the same note events and notes. The
// wave sounds' pans, spans, pitches, envelopes, main sends and first two aux sends have to be the same. So do their
// third aux sends with `third_aux_send`, and their filters with `filters`: the 3DS's reader reads neither, and the
// other consoles' readers read the filters from version 1.1.0 on.
bool SameWaveSounds(const sar::WaveSoundFile& a, const sar::WaveSoundFile& b, bool third_aux_send, bool filters)
{
    if (!SameWaves(a.waves, b.waves) || a.sounds.size() != b.sounds.size())
    {
        return false;
    }

    const auto same_event = [](const sar::WaveSoundFile::NoteEvent& x, const sar::WaveSoundFile::NoteEvent& y)
    {
        return x.position == y.position && x.length == y.length && x.note == y.note;
    };
    for (std::size_t s = 0; s < a.sounds.size(); s++)
    {
        const sar::WaveSoundFile::WaveSound& x = a.sounds[s];
        const sar::WaveSoundFile::WaveSound& y = b.sounds[s];
        const sar::VelocityRegion envelope_x{.adshr = x.adshr};
        const sar::VelocityRegion envelope_y{.adshr = y.adshr};
        const bool same_sends = x.main_send == y.main_send && x.aux_sends[0] == y.aux_sends[0] &&
                                x.aux_sends[1] == y.aux_sends[1] &&
                                (!third_aux_send || x.aux_sends[2] == y.aux_sends[2]);
        const bool same_filters = !filters || (x.low_pass == y.low_pass && x.biquad_type == y.biquad_type &&
                                               x.biquad_value == y.biquad_value);
        if (x.pan != y.pan || x.span != y.span || x.pitch != y.pitch || !SameRegion(envelope_x, envelope_y) ||
            !same_sends || !same_filters || x.tracks.size() != y.tracks.size() ||
            !std::equal(x.notes.begin(), x.notes.end(), y.notes.begin(), y.notes.end(), SameRegion))
        {
            return false;
        }
        for (std::size_t k = 0; k < x.tracks.size(); k++)
        {
            if (!std::equal(x.tracks[k].begin(), x.tracks[k].end(), y.tracks[k].begin(), y.tracks[k].end(), same_event))
            {
                return false;
            }
        }
    }

    return true;
}

// Whether two waves, each read with its options, decode to the same samples at the same rate, with the same loop and
// encoding.
bool SameWave(std::span<const uint8_t> a, const sar::Options& a_options, std::span<const uint8_t> b,
              const sar::Options& b_options)
{
    const sar::Wave x = sar::Wave::Parse(a, a_options);
    const sar::Wave y = sar::Wave::Parse(b, b_options);
    const sar::Pcm p = sar::DecodeWave(a, a_options);
    const sar::Pcm q = sar::DecodeWave(b, b_options);
    return x.encoding == y.encoding && x.loop == y.loop && x.loop_start == y.loop_start && x.loop_end == y.loop_end &&
           p.sample_rate == q.sample_rate && p.loop == q.loop && p.channels == q.channels;
}

// The bytes of each channel of `wave` up to its last sample: whole frames of DSP-ADPCM.
std::size_t SampleBytes(const sar::WaveFile& wave)
{
    switch (wave.encoding)
    {
    case sar::WaveEncoding::kPcm8:
        return wave.loop_end;
    case sar::WaveEncoding::kPcm16:
        return std::size_t{wave.loop_end} * 2;
    default:
        break;
    }

    return (std::size_t{wave.loop_end} + 13) / 14 * 8;
}

// What differs between a wave that went to the other console and back and the archive's wave.
struct WaveBack
{
    bool loop_start = false;
    bool trimmed = false;
    bool other = false;
};

// What differs between `back`, a wave that went to the other console and back, and the archive's wave `theirs`: the
// original loop start, the bytes after the channels' last samples, or anything else.
WaveBack CompareBack(std::span<const uint8_t> back, std::span<const uint8_t> theirs)
{
    WaveBack difference;
    if (std::equal(back.begin(), back.end(), theirs.begin(), theirs.end()))
    {
        return difference;
    }

    // The archive's original loop start and bytes after the last samples put back have to give the archive's bytes. A
    // channel's samples run to the next channel's, and the writer pads them to a boundary with zeros.
    sar::WaveFile patched = sar::WaveFile::Read(back);
    const sar::WaveFile original = sar::WaveFile::Read(theirs);
    difference.loop_start = patched.original_loop_start != original.original_loop_start;
    patched.original_loop_start = original.original_loop_start;
    const auto held = static_cast<std::ptrdiff_t>(SampleBytes(original));
    const auto padding = [](uint8_t byte)
    {
        return byte == 0;
    };
    for (std::size_t c = 0; c < patched.channels.size() && c < original.channels.size(); c++)
    {
        std::vector<uint8_t>& ours = patched.channels[c].samples;
        const std::vector<uint8_t>& archive = original.channels[c].samples;
        if (ours != archive && std::ssize(ours) >= held && std::ssize(archive) >= held &&
            std::equal(ours.begin(), ours.begin() + held, archive.begin()) &&
            std::all_of(ours.begin() + held, ours.end(), padding))
        {
            ours = archive;
            difference.trimmed = true;
        }
    }
    const std::vector<uint8_t> written = patched.Write();
    difference.other = !std::equal(written.begin(), written.end(), theirs.begin(), theirs.end());

    return difference;
}

} // namespace

std::string OtherConsoleFileProblem(const sar::SoundArchive& built, const sar::SoundArchive& opened,
                                    const song::Item& item, sar::Flavor other, Totals& totals)
{
    const sar::Flavor flavor = opened.ReadOptions().flavor;
    const sar::Options options = song::ItemOptions(opened);
    sar::Options other_options = options;
    other_options.flavor = other;
    const song::FileVersions versions = song::ExportVersions(item.kind, other);
    try
    {
        // The new file's version, and the values that the other console's reader reads.
        const std::span<const uint8_t> theirs = song::ItemFile(opened, item);
        const std::vector<uint8_t> ours = song::OtherConsoleFile(built, item, other);
        if (VersionOf(ours) != versions.file)
        {
            return "takes another version";
        }
        totals.file_versions += VersionOf(ours) != VersionOf(theirs) ? 1 : 0;
        totals.converted_files++;
        if (item.kind == song::ItemKind::kBank &&
            !SameBank(sar::Bank::Parse(ours, other_options), sar::Bank::Parse(theirs, options)))
        {
            return "reads otherwise";
        }
        const bool third_aux_send = flavor != sar::Flavor::kCtr && other != sar::Flavor::kCtr;
        const bool filters = third_aux_send && VersionOf(ours) >= 0x00010100 && VersionOf(theirs) >= 0x00010100;
        if (item.kind == song::ItemKind::kWaveSound &&
            !SameWaveSounds(sar::WaveSoundFile::Parse(ours, other_options), sar::WaveSoundFile::Parse(theirs, options),
                            third_aux_send, filters))
        {
            return "reads otherwise";
        }
        if (item.kind != song::ItemKind::kWaveArchive)
        {
            const std::vector<uint8_t> back =
                song::ConvertItemFile(ours, item.kind, other, flavor, {VersionOf(theirs), 0});
            return std::equal(back.begin(), back.end(), theirs.begin(), theirs.end())
                       ? ""
                       : "doesn't convert back to the archive's bytes";
        }

        // Each wave, in the wave archive and alone, then back with the version of the archive's wave.
        const std::vector<std::span<const uint8_t>> our_waves = sar::WaveArchive::Parse(ours, other_options).waves;
        const std::vector<std::span<const uint8_t>> their_waves = sar::WaveArchive::Parse(theirs, options).waves;
        if (our_waves.size() != their_waves.size())
        {
            return "holds another number of waves";
        }
        bool whole = true;
        sar::WaveArchiveFile back_archive;
        back_archive.flavor = flavor;
        back_archive.version = VersionOf(theirs);
        for (uint32_t w = 0; w < our_waves.size(); w++)
        {
            const std::vector<uint8_t> alone =
                song::OtherConsoleFile(built, {song::ItemKind::kWave, item.index, w}, other);
            if (!SameWave(our_waves[w], other_options, their_waves[w], options) ||
                VersionOf(our_waves[w]) != versions.waves ||
                !std::equal(alone.begin(), alone.end(), our_waves[w].begin(), our_waves[w].end()))
            {
                return "plays wave " + std::to_string(w) + " otherwise";
            }

            const std::vector<uint8_t> back = song::ConvertItemFile(our_waves[w], song::ItemKind::kWave, other, flavor,
                                                                    {VersionOf(their_waves[w]), 0});
            const WaveBack difference = CompareBack(back, their_waves[w]);
            if (difference.other)
            {
                return "doesn't convert wave " + std::to_string(w) + " back to the archive's bytes";
            }
            totals.wave_versions += VersionOf(our_waves[w]) != VersionOf(their_waves[w]) ? 1 : 0;
            totals.loop_starts += difference.loop_start ? 1 : 0;
            totals.trimmed += difference.trimmed ? 1 : 0;
            whole = whole && !difference.loop_start && !difference.trimmed;
            back_archive.waves.push_back(back);
            totals.converted_waves++;
        }

        // With every wave back as it was, the wave archive has to come back as it was too.
        const std::vector<uint8_t> written = back_archive.Write();
        if (whole && !std::equal(written.begin(), written.end(), theirs.begin(), theirs.end()))
        {
            return "doesn't convert back to the archive's bytes";
        }
    }
    catch (const std::exception& error)
    {
        return std::string("fails: ") + error.what();
    }

    return {};
}

} // namespace hokuto::tools::items
