// SPDX-License-Identifier: MIT

// The tallies of hokuto_check_players and their summaries.

#include "check_players.h"

namespace hokuto::tools::players
{

bool Silent(const sar::Pcm& pcm, const std::vector<std::size_t>& channels)
{
    for (const std::size_t channel : channels)
    {
        for (const int16_t sample : pcm.channels[channel])
        {
            if (sample != 0)
            {
                return false;
            }
        }
    }

    return true;
}

bool Alike(const sar::Pcm& pcm, std::size_t a, std::size_t b)
{
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    const std::size_t length = std::min<std::size_t>({pcm.channels[a].size(), pcm.channels[b].size(), pcm.Length()});
    for (std::size_t i = 0; i < length; i++)
    {
        const double x = pcm.channels[a][i];
        const double y = pcm.channels[b][i];
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }

    return aa > 0.0 && bb > 0.0 && ab >= kMostAlike * std::sqrt(aa * bb);
}

bool WrongLength(const Comparison& comparison)
{
    return comparison.end_frames &&
           (*comparison.end_frames < kLeastEndFrames || *comparison.end_frames > kMostEndFrames);
}

void PrintWaveSounds(const std::string& archive, const WaveSoundTotals& totals)
{
    std::printf(
        "%s: %zu wave sounds, %zu without their files or waves, %zu silent as their waves or settings, %zu that don't "
        "end with their waves; %zu filtered, %zu shaped, %zu panned, %zu with other sends\n",
        archive.c_str(), totals.wave_sounds, totals.missing, totals.silent, totals.endless, totals.filtered,
        totals.shaped, totals.panned, totals.sent);
    std::printf(
        "%s: the WAV export plays another wave in %zu, and the same wave at another pitch in %zu; %zu hold more "
        "than one note, and %zu more than one note event\n",
        archive.c_str(), totals.export_wave, totals.export_pitch, totals.several_notes, totals.several_events);
    if (totals.first_frame > 0)
    {
        std::printf("%s: %zu wave sounds end within their first frame, before the fits begin\n", archive.c_str(),
                    totals.first_frame);
    }
    totals.comparable.Print(archive, "wave sounds without a filter or a shaping envelope");
    totals.apart.Print(archive, "wave sounds counted apart");
    std::printf("%s: %zu wave sounds give no gains: alike channels, or little energy below 1 kHz\n", archive.c_str(),
                totals.without_gains);
    totals.mono.Print(archive, "the gains of a mono wave at the centre, over the sound's volume");
    totals.stereo_direct.Print(archive, "the gains of a stereo wave's channels to their sides");
    totals.stereo_cross.Print(archive, "the gains of a stereo wave's channels to the other side");
}

void PrintStreamSounds(const std::string& archive, const StreamTotals& totals)
{
    std::printf("%s: %zu stream sounds, %zu without their streams, %zu silent, %zu of AAC\n", archive.c_str(),
                totals.sounds, totals.absent, totals.silent, totals.aac);
    if (totals.refused > 0)
    {
        std::printf("%s: the game's build of NW4R SND plays %zu stream sounds no track of their streams\n",
                    archive.c_str(), totals.refused);
    }
    if (totals.clipped > 0)
    {
        std::printf("%s: %zu stream sounds below 20 dB clip at full scale, and count apart\n", archive.c_str(),
                    totals.clipped);
    }
    totals.tally.Print(archive, "stream sounds");
    std::printf("%s: %zu tracks give no gains: alike channels, or little energy below 1 kHz\n", archive.c_str(),
                totals.without_gains);
    totals.mono.Print(archive, "the gains of a mono track at the centre, over the volumes");
    totals.direct.Print(archive, "the gains of a pair's channels to their sides");
    totals.cross.Print(archive, "the gains of a pair's channels to the other side");
}

} // namespace hokuto::tools::players
