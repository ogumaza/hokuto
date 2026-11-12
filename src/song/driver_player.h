// SPDX-License-Identifier: MIT

// The part of a GBA driver's player (DriverPlayer) that passes straight to ntdlib's ArchivePlayer of the driver. Each
// driver's player derives from it and adds how a sound starts, the tracks, the tick and the end.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "song/driver.h"

namespace hokuto::song
{

// A DriverPlayer around `Player`, one of ntdlib's ArchivePlayer classes.
template <typename Player>
class ArchiveDriverPlayer : public DriverPlayer
{
public:
    int Rate() const override
    {
        return player_.Rate();
    }

    bool Enhanced() const override
    {
        return player_.Enhanced();
    }

    bool Busy() const override
    {
        return player_.Busy();
    }

    uint64_t Frames() const override
    {
        return player_.Frames();
    }

    void RunFrame(std::vector<int16_t>& out) override
    {
        player_.RunFrame(out);
    }

    void RunFrame(std::vector<float>& out) override
    {
        player_.RunFrame(out);
    }

    void MuteTrack(int track, bool mute) override
    {
        player_.MuteTrack(track, mute);
    }

    void MeterTracks(bool meter) override
    {
        player_.MeterTracks(meter);
    }

    std::array<int32_t, 2> TrackPeaks(int track) const override
    {
        return player_.TrackPeaks(track);
    }

    std::shared_ptr<const Snapshot> Save(const Snapshot* previous) const override
    {
        const auto* earlier = dynamic_cast<const Saved*>(previous);
        return std::make_shared<const Saved>(player_.Save(earlier != nullptr ? &earlier->snapshot : nullptr));
    }

    void Restore(const Snapshot& snapshot) override
    {
        player_.Restore(dynamic_cast<const Saved&>(snapshot).snapshot);
    }

protected:
    // Makes the player from `args`, the arguments of the constructor of `Player`.
    template <typename... Args>
    explicit ArchiveDriverPlayer(Args&&... args) : player_(std::forward<Args>(args)...)
    {
    }

    Player player_;

private:
    struct Saved final : Snapshot
    {
        explicit Saved(typename Player::Snapshot saved) : snapshot(std::move(saved))
        {
        }

        typename Player::Snapshot snapshot;
    };
};

} // namespace hokuto::song
