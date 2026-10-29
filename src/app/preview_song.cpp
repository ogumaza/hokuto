// SPDX-License-Identifier: MIT

#include "app/preview_song.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app
{
namespace
{

constexpr int32_t kQuarter = 48;
constexpr int32_t kBar = kQuarter * 4;
constexpr int kBars = 40;

// The banks that the tracks play, named as a game's sound archive names them.
constexpr char kFieldBank[] = "BANK_FIELD";
constexpr char kDrumBank[] = "BANK_DRUMS";

// The chords' roots, one a bar: C, A minor, F and G. Each is a step of the C major scale.
constexpr std::array<int, 4> kRoots = {0, 5, 3, 4};

// A fixed sequence of numbers. The song comes out the same on every run.
class Numbers
{
public:
    explicit Numbers(uint32_t seed) : state_(seed)
    {
    }

    int Below(int count)
    {
        state_ = state_ * 1664525u + 1013904223u;
        return static_cast<int>((state_ >> 8) % static_cast<uint32_t>(count));
    }

private:
    uint32_t state_;
};

// A part without notes from tick `start`, `length` ticks long.
PartView EmptyPart(int32_t start, int32_t length)
{
    PartView part;
    part.start = start;
    part.length = length;

    return part;
}

// `part` as one of the parts that play the subroutine `name`.
PartView Linked(PartView part, const char* name)
{
    part.name = name;
    part.linked = true;

    return part;
}

// The key of a step of the C major scale above `base`.
int ScaleKey(int step, int base)
{
    constexpr std::array<int, 7> kSemitones = {0, 2, 4, 5, 7, 9, 11};
    const int octave = step >= 0 ? step / 7 : (step - 6) / 7;
    return base + octave * 12 + kSemitones[static_cast<std::size_t>(step - octave * 7)];
}

int RootOf(int bar)
{
    return kRoots[static_cast<std::size_t>(bar % 4)];
}

PartView Melody(int first_bar, int bars, int base, Numbers& numbers)
{
    PartView part = EmptyPart(first_bar * kBar, bars * kBar);
    constexpr std::array<int32_t, 6> kLengths = {kQuarter / 2, kQuarter / 2,     kQuarter,
                                                 kQuarter,     kQuarter * 3 / 2, kQuarter * 2};
    int step = 7 + numbers.Below(4);
    for (int32_t tick = 0; tick < part.length;)
    {
        const int32_t length = std::min(kLengths[static_cast<std::size_t>(numbers.Below(6))], part.length - tick);
        if (numbers.Below(7) != 0)
        {
            part.notes.push_back({tick, length - 4, ScaleKey(step, base)});
        }
        step = std::clamp(step + numbers.Below(5) - 2, 2, 13);
        tick += length;
    }

    return part;
}

PartView Chords(int first_bar, int bars, int base)
{
    PartView part = EmptyPart(first_bar * kBar, bars * kBar);
    for (int bar = 0; bar < bars; bar++)
    {
        const int root = RootOf(first_bar + bar);
        for (const int step : {root, root + 2, root + 4})
        {
            part.notes.push_back({bar * kBar, kBar - 6, ScaleKey(step, base)});
        }
    }

    return part;
}

PartView Arpeggio(int first_bar, int bars, int base)
{
    PartView part = EmptyPart(first_bar * kBar, bars * kBar);
    constexpr std::array<int, 8> kPattern = {0, 2, 4, 7, 9, 7, 4, 2};
    for (int bar = 0; bar < bars; bar++)
    {
        const int root = RootOf(first_bar + bar);
        for (int i = 0; i < 16; i++)
        {
            const int step = root + kPattern[static_cast<std::size_t>(i % 8)];
            part.notes.push_back({bar * kBar + i * kQuarter / 4, kQuarter / 4 - 2, ScaleKey(step, base)});
        }
    }

    return part;
}

PartView Bass(int first_bar, int bars, int base)
{
    PartView part = EmptyPart(first_bar * kBar, bars * kBar);
    for (int bar = 0; bar < bars; bar++)
    {
        const int root = RootOf(first_bar + bar);
        for (int beat = 0; beat < 8; beat++)
        {
            const int key = ScaleKey(root, base) + (beat % 2 == 1 ? 12 : 0);
            part.notes.push_back({bar * kBar + beat * kQuarter / 2, kQuarter / 2 - 6, key});
        }
    }

    return part;
}

PartView Drums(int first_bar, int bars)
{
    PartView part = EmptyPart(first_bar * kBar, bars * kBar);
    for (int bar = 0; bar < bars; bar++)
    {
        const int32_t start = bar * kBar;
        part.notes.push_back({start, kQuarter / 4, 36});
        part.notes.push_back({start + kQuarter, kQuarter / 4, 38});
        part.notes.push_back({start + kQuarter * 2, kQuarter / 4, 36});
        part.notes.push_back({start + kQuarter * 5 / 2, kQuarter / 4, 36});
        part.notes.push_back({start + kQuarter * 3, kQuarter / 4, 38});
        if (bar % 4 == 3)
        {
            for (int i = 0; i < 4; i++)
            {
                part.notes.push_back({start + kQuarter * 3 + i * kQuarter / 4, kQuarter / 8, 45 - i * 2});
            }
        }
    }

    return part;
}

PartView HiHat(int first_bar, int bars)
{
    PartView part = EmptyPart(first_bar * kBar, bars * kBar);
    for (int bar = 0; bar < bars; bar++)
    {
        for (int i = 0; i < 8; i++)
        {
            part.notes.push_back({bar * kBar + i * kQuarter / 2, kQuarter / 4, i == 7 ? 46 : 42});
        }
    }

    return part;
}

PartView Roll(int bar)
{
    PartView part = EmptyPart(bar * kBar, kBar);
    for (int i = 0; i < 16; i++)
    {
        part.notes.push_back({kQuarter * 2 + i * kQuarter / 8, kQuarter / 8, i < 8 ? 43 : 48});
    }
    part.notes.push_back({kBar - kQuarter / 2, kQuarter / 2, 41});

    return part;
}

// A track with a send to FX A at `reverb` dB. The game's reverb sits on bus A.
TrackView Track(const char* name, const char* bank, int program, std::size_t colour, float volume, float pan,
                float reverb)
{
    TrackView track;
    track.name = name;
    track.bank = bank;
    track.program = program;
    track.rainbow_colour = theme::kTrackColours[colour];
    track.colour = track.rainbow_colour;
    track.volume = volume;
    track.pan = pan;
    track.sends[0] = reverb;

    return track;
}

} // namespace

SongView MakePreviewSong()
{
    SongView song;
    song.name = "北斗七星のテーマ";
    song.ticks_per_quarter = kQuarter;
    song.tempo = 132.0;
    song.length = kBars * kBar;
    song.loop_start = 8 * kBar;
    song.loop_end = kBars * kBar;
    Numbers numbers(7);

    TrackView trumpet = Track("Trumpet", kFieldBank, 56, 0, -3.0f, 0.1f, -14.0f);
    for (const int bar : {8, 16, 32})
    {
        trumpet.parts.push_back(Melody(bar, 8, 60, numbers));
    }

    TrackView flute = Track("Flute", kFieldBank, 73, 1, -6.0f, -0.3f, -10.0f);
    flute.parts.push_back(Melody(12, 4, 72, numbers));
    flute.parts.push_back(Melody(24, 8, 72, numbers));

    TrackView strings = Track("Strings", kFieldBank, 48, 8, -8.5f, 0.0f, -8.0f);
    strings.parts.push_back(Chords(0, 8, 55));
    strings.parts.push_back(Chords(8, 16, 55));
    strings.parts.push_back(Chords(24, 16, 55));

    TrackView harp = Track("Harp", kFieldBank, 46, 2, -10.0f, 0.35f, -6.0f);
    harp.parts.push_back(Arpeggio(0, 8, 60));
    harp.parts.push_back(Arpeggio(24, 8, 60));

    TrackView bass = Track("Bass", kFieldBank, 33, 4, -4.0f, 0.0f, kSilence);
    for (int bar = 0; bar < kBars; bar += 8)
    {
        bass.parts.push_back(Linked(Bass(bar, 8, 36), "Bass_Line"));
    }

    TrackView drums = Track("Drums", kDrumBank, 0, 6, -5.0f, 0.0f, -20.0f);
    drums.parts.push_back(Drums(4, 4));
    for (int bar = 8; bar < kBars; bar += 8)
    {
        drums.parts.push_back(Linked(Drums(bar, 8), "Groove"));
    }

    TrackView hats = Track("Hi-hat", kDrumBank, 0, 5, -12.0f, -0.2f, kSilence);
    for (int bar = 8; bar < kBars; bar += 8)
    {
        hats.parts.push_back(HiHat(bar, 8));
    }

    TrackView timpani = Track("Timpani", kFieldBank, 47, 7, -7.0f, 0.0f, -12.0f);
    for (const int bar : {7, 15, 23, 31})
    {
        timpani.parts.push_back(Roll(bar));
    }

    song.tracks = {trumpet, flute, strings, harp, bass, drums, hats, timpani};

    // A variable that picks one of three layers, one that the song has set to 0 at the position, and a global one that
    // a command reads as its argument.
    song.variables = {
        {.number = 0, .choices = {-1, 0, 1, 2}, .start = 1},
        {.number = 1, .choices = {-1, 1}, .written = true, .value = 0},
        {.number = 16},
    };

    // A change can also set variable 5. The song sets it before it reads it.
    song.read_variables = song.variables;
    song.read_variables.insert(song.read_variables.begin() + 2, {.number = 5, .choices = {-1, 0, 1}});

    // The commands that the tracks run where they read the variables or branch. At the loop's start, the trumpet picks
    // a layer. The strings take their volume from the global variable, and the harp plays a fill if variable 1 is set.
    // The trumpet's comparison runs again at each pass of a loop of eight bars.
    song.logic = {
        {.ticks = {8 * kBar, 16 * kBar, 24 * kBar, 32 * kBar},
         .branch = true,
         .variables = {0},
         .commands = {{0, 0x120, "Trumpet: Compare = Variable 0, 1"}, {0, 0x125, "Trumpet: if Jump Layer_B"}}},
        {.ticks = {12 * kBar}, .variables = {16}, .commands = {{2, 0x340, "Strings: Volume Variable 16"}}},
        {.ticks = {20 * kBar},
         .branch = true,
         .variables = {1},
         .commands = {{3, 0x460, "Harp: Compare = Variable 1, 1"}, {3, 0x465, "Harp: if Call Fill"}}},
    };

    return song;
}

} // namespace hokuto::app
