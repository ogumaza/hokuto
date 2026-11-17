// SPDX-License-Identifier: MIT

// hokuto_check_driver_playback: the reference (the driver's ArchivePlayer in ntdlib, run directly) and the commands
// that the driver decodes.

#include "check_driver_playback.h"

namespace hokuto::tools::driver_playback
{

namespace
{

// An ArchivePlayer of ntdlib, `Player`, and `start`, the function that starts it.
template <typename Player, typename Starter>
class AnyReference final : public ReferencePlayer
{
public:
    template <typename Rom, typename Info>
    AnyReference(const Rom& rom, const Info& info, Starter start) : player_(rom, info), start_(std::move(start))
    {
    }

    void Start(uint32_t sound, bool enhanced) override
    {
        start_(player_, sound, enhanced);
    }

    void MuteTrack(int track) override
    {
        player_.MuteTrack(track, true);
    }

    void MeterTracks() override
    {
        player_.MeterTracks(true);
    }

    void RunFrame(std::vector<int16_t>& out) override
    {
        player_.RunFrame(out);
    }

    void RunFrame(std::vector<float>& out) override
    {
        player_.RunFrame(out);
    }

    std::array<int32_t, 2> TrackPeaks(int track) const override
    {
        return player_.TrackPeaks(track);
    }

private:
    Player player_;
    Starter start_;
};

} // namespace

std::unique_ptr<ReferencePlayer> MakeReference(const song::DriverRom& rom)
{
    if (const song::KonamiRom* konami = song::KonamiOf(&rom))
    {
        const auto start = [konami](agblib::konami::ArchivePlayer& player, uint32_t sound, bool enhanced)
        {
            player.Start(song::KonamiSongOf(*konami, sound),
                         agblib::konami::ArchivePlayerOptions{.enhanced = enhanced,
                                                              .live = song::KonamiSoundIsLive(*konami, sound)});
        };
        return std::make_unique<AnyReference<agblib::konami::ArchivePlayer, std::decay_t<decltype(start)>>>(
            konami->rom, konami->info, start);
    }
    if (const song::Rd2Rom* rd2 = song::Rd2Of(&rom))
    {
        const auto start = [rd2](agblib::rd2::ArchivePlayer& player, uint32_t sound, bool enhanced)
        {
            player.Start(rd2->SongOf(sound),
                         agblib::rd2::ArchivePlayerOptions{.enhanced = enhanced, .version = rd2->VersionOf(sound)});
        };
        return std::make_unique<AnyReference<agblib::rd2::ArchivePlayer, std::decay_t<decltype(start)>>>(
            rd2->rom, rd2->info, start);
    }
    if (const song::BrownieRom* brownie = song::BrownieOf(&rom))
    {
        const auto start = [brownie](agblib::brownie::ArchivePlayer& player, uint32_t sound, bool enhanced)
        {
            player.Start(brownie->SongOf(sound), agblib::brownie::ArchivePlayerOptions{.enhanced = enhanced});
        };
        return std::make_unique<AnyReference<agblib::brownie::ArchivePlayer, std::decay_t<decltype(start)>>>(
            brownie->rom, brownie->info, start);
    }
    if (const song::UbiMilanRom* milan = song::UbiMilanOf(&rom))
    {
        const auto start = [](agblib::ubimilan::ArchivePlayer& player, uint32_t sound, bool enhanced)
        {
            player.Start(static_cast<int>(sound), agblib::ubimilan::ArchivePlayerOptions{.enhanced = enhanced});
        };
        return std::make_unique<AnyReference<agblib::ubimilan::ArchivePlayer, std::decay_t<decltype(start)>>>(
            milan->rom, milan->info, start);
    }
    if (const song::RareRom* rare = song::RareOf(&rom))
    {
        const auto start = [rare](agblib::rare::ArchivePlayer& player, uint32_t sound, bool enhanced)
        {
            player.Start(rare->SongOf(sound), agblib::rare::ArchivePlayerOptions{
                                                  .enhanced = enhanced, .under_water = rare->VersionOf(sound) == 1});
        };
        return std::make_unique<AnyReference<agblib::rare::ArchivePlayer, std::decay_t<decltype(start)>>>(
            rare->rom, rare->info, start);
    }
    if (const song::QuintetRom* quintet = song::QuintetOf(&rom))
    {
        const auto start = [](agblib::quintet::ArchivePlayer& player, uint32_t sound, bool enhanced)
        {
            player.Start(static_cast<int>(sound), agblib::quintet::ArchivePlayerOptions{.enhanced = enhanced});
        };
        return std::make_unique<AnyReference<agblib::quintet::ArchivePlayer, std::decay_t<decltype(start)>>>(
            quintet->rom, quintet->files, start);
    }
    if (const song::KrawallRom* krawall = song::KrawallOf(&rom))
    {
        const auto start = [](agblib::krawall::ArchivePlayer& player, uint32_t sound, bool enhanced)
        {
            player.Start(static_cast<int>(sound), agblib::krawall::ArchivePlayerOptions{.enhanced = enhanced});
        };
        return std::make_unique<AnyReference<agblib::krawall::ArchivePlayer, std::decay_t<decltype(start)>>>(
            krawall->rom, krawall->info, start);
    }

    throw std::invalid_argument("The check has no reference of " + std::string(rom.Rules().Title()) + ".");
}

std::pair<ntdlib::commands::Sequence, std::vector<uint32_t>> DecodedCommands(const song::DriverRom& rom, uint32_t sound)
{
    if (const song::KonamiRom* konami = song::KonamiOf(&rom))
    {
        agblib::konami::Decoded decoded =
            agblib::konami::DecodeSong(konami->rom, konami->info, song::KonamiSongOf(*konami, sound));
        std::vector<uint32_t> offsets = agblib::konami::ItemOffsets(decoded.sequence, konami->info.revision);
        return {std::move(decoded.sequence), std::move(offsets)};
    }
    if (const song::Rd2Rom* rd2 = song::Rd2Of(&rom))
    {
        agblib::rd2::Decoded decoded = agblib::rd2::DecodeRomSequence(
            rd2->rom, rd2->info, agblib::rd2::SequenceOfSong(rd2->info, rd2->SongOf(sound)));
        std::vector<uint32_t> offsets = agblib::rd2::ItemOffsets(decoded.sequence, rd2->info.revision);
        return {std::move(decoded.sequence), std::move(offsets)};
    }
    if (const song::KrawallRom* krawall = song::KrawallOf(&rom))
    {
        agblib::krawall::Decoded decoded = agblib::krawall::DecodeRomSequence(
            krawall->rom, krawall->info, agblib::krawall::SequenceOfSong(krawall->info, static_cast<int>(sound)));
        std::vector<uint32_t> offsets =
            agblib::krawall::ItemOffsets(decoded.sequence, agblib::krawall::TraitsOf(krawall->info));
        return {std::move(decoded.sequence), std::move(offsets)};
    }
    if (const song::BrownieRom* brownie = song::BrownieOf(&rom))
    {
        // The region that contains the sound, with the frames that each command that waits waits there.
        agblib::brownie::Decoded decoded =
            agblib::brownie::DecodeSong(brownie->rom, brownie->info, brownie->SongOf(sound));
        const agblib::brownie::Revision revision = brownie->info.revision;
        hokuto::format::FillBrownieWaits(decoded.sequence, revision,
                                         [brownie](uint8_t byte, int set)
                                         {
                                             const uint8_t frames =
                                                 agblib::brownie::NoteLength(brownie->rom, brownie->info, byte, set);
                                             return frames != 0 ? uint32_t{frames} : 256u;
                                         });
        std::vector<uint32_t> offsets = hokuto::format::BrownieItemOffsets(decoded.sequence, revision);
        return {std::move(decoded.sequence), std::move(offsets)};
    }
    if (const song::UbiMilanRom* milan = song::UbiMilanOf(&rom))
    {
        // The piece's first sequence, with the length in ticks of each wait (format/ubimilan.h).
        agblib::ubimilan::MusicPiece piece;
        if (!agblib::ubimilan::ReadMusic(milan->rom, milan->info, static_cast<int>(sound), piece) ||
            piece.sequences.empty())
        {
            throw std::invalid_argument("The ROM has no piece " + std::to_string(sound) + ".");
        }
        agblib::ubimilan::Decoded decoded =
            agblib::ubimilan::DecodeResource(milan->rom, milan->info, piece.sequences.front());
        std::vector<uint32_t> offsets = agblib::ubimilan::EncodeSequence(decoded.sequence).offsets;
        return {hokuto::format::UbiMilanTicks(std::move(decoded.sequence)), std::move(offsets)};
    }

    if (const song::RareRom* rare = song::RareOf(&rom))
    {
        agblib::rare::Decoded decoded = agblib::rare::DecodeRomSequence(
            rare->rom, rare->info, agblib::rare::SequenceOfSong(rare->info, rare->SongOf(sound)));
        std::vector<uint32_t> offsets = agblib::rare::ItemOffsets(decoded.sequence, agblib::rare::TraitsOf(rare->info));
        return {std::move(decoded.sequence), std::move(offsets)};
    }
    if (const song::QuintetRom* quintet = song::QuintetOf(&rom))
    {
        // The lengths in the performer's ticks as a file stores them (format/quintet.h).
        const auto file = quintet->FileOf(static_cast<int>(sound));
        if (!file)
        {
            throw std::invalid_argument("The ROM has no song " + std::to_string(sound) + ".");
        }
        agblib::quintet::Decoded decoded = agblib::quintet::DecodeRomSequence(
            quintet->rom, *file->first, agblib::quintet::SequenceOfSong(*file->first, file->second));
        // A dot's byte belongs to the item that folds it.
        const std::vector<uint32_t> offsets =
            agblib::quintet::ItemOffsets(decoded.sequence, agblib::quintet::TraitsOf(quintet->rom, *file->first));
        const std::vector<uint32_t> folded = hokuto::format::QuintetFoldedItems(decoded.sequence);
        std::vector<uint32_t> folded_offsets(folded.back() + 1);
        for (std::size_t item = offsets.size(); item-- > 0;)
        {
            folded_offsets[folded[item]] = offsets[item];
        }
        return {hokuto::format::QuintetTicks(std::move(decoded.sequence)), std::move(folded_offsets)};
    }

    throw std::invalid_argument("The check reads no commands of " + std::string(rom.Rules().Title()) + ".");
}

Reference RunReference(const song::DriverRom& rom, uint32_t sound, bool enhanced, std::size_t samples, int muted)
{
    const std::unique_ptr<ReferencePlayer> player = MakeReference(rom);
    Reference reference;
    player->Start(sound, enhanced);
    if (muted >= 0)
    {
        player->MuteTrack(muted);
    }
    std::vector<int16_t> strict;
    while (reference.samples.size() < samples * 2)
    {
        if (enhanced)
        {
            player->RunFrame(reference.samples);
        }
        else
        {
            strict.clear();
            player->RunFrame(strict);
            for (const int16_t sample : strict)
            {
                reference.samples.push_back(static_cast<float>(sample) / 32768.0f);
            }
        }
    }

    return reference;
}

} // namespace hokuto::tools::driver_playback
