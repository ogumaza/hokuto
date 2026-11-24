// SPDX-License-Identifier: MIT

// The starts of a file's sounds in the text form: a start's line, then a line for each of its fields and variables.

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "format/text_parts.h"

namespace hokuto::format::text_parts
{
namespace
{

// A variable's line in a start: variable, its number and its value, with track and the track before them for a
// variable of a single track, and at and a time before them for a change after the start.
std::string StartVariableText(const StartVariable& variable)
{
    std::string text;
    if (variable.time != 0)
    {
        text += "at " + TimeText(variable.time) + " ";
    }
    if (variable.track)
    {
        text += "track " + std::to_string(*variable.track) + " ";
    }

    return text + "variable " + std::to_string(variable.variable) + " " + std::to_string(variable.value);
}

// A track's number in a start's line: 0 to 31.
uint8_t ParseStartTrack(const Token& token, std::size_t line)
{
    return static_cast<uint8_t>(ParseInteger(token, kStartTracks - 1, line, "a track's number"));
}

// A start's variable: [at time] [track N] variable number value.
StartVariable ParseStartVariable(const std::vector<Token>& tokens, std::size_t line)
{
    StartVariable variable;
    std::size_t t = 0;
    if (tokens[t].Is("at") && tokens.size() > 1)
    {
        variable.time = ParseTime(tokens[1], line);
        t = 2;
    }
    if (t + 1 < tokens.size() && tokens[t].Is("track"))
    {
        variable.track = ParseStartTrack(tokens[t + 1], line);
        t += 2;
    }
    if (tokens.size() != t + 3 || !tokens[t].Is("variable"))
    {
        Fail(line,
             "a start's variable is variable, its number and its value, with at and a time, and track and a "
             "track, before them");
    }

    variable.variable =
        static_cast<uint8_t>(ParseInteger(tokens[t + 1], kStartVariables - 1, line, "a variable's number"));
    variable.value = ParseVariableValue(tokens[t + 2], line);

    return variable;
}

// The value of a line of one number: tempo_ratio, pitch, volume or low_pass, and the value.
float ParseRatio(const std::vector<Token>& tokens, std::size_t line)
{
    if (tokens.size() != 2)
    {
        Fail(line, "a start's " + tokens[0].text + " line is " + tokens[0].text + " and a number");
    }

    return ParseFloat(tokens[1], line);
}

// A start's banks: banks and four item IDs or nones.
StartBanks ParseStartBanks(const std::vector<Token>& tokens, std::size_t line)
{
    if (tokens.size() != 5)
    {
        Fail(line, "a start's banks are banks and four banks' item IDs, with none for an empty slot");
    }

    StartBanks banks{};
    for (std::size_t i = 0; i < banks.size(); ++i)
    {
        const Token& bank = tokens[i + 1];
        banks[i] = bank.Is("none") ? kNoStartBank
                                   : static_cast<uint32_t>(ParseInteger(bank, 0xFFFFFFFF, line, "a bank's item ID"));
    }

    return banks;
}

} // namespace

// The lines of `start`: start, its sound and its name, then a line for each field that it sets and for each variable.
// Where `tables` name the sound, a comment gives its name.
void WriteStart(TextOut& out, const SoundStart& start, const NativeTables* tables)
{
    if (const std::optional<std::string> problem = StartProblem(start))
    {
        throw std::invalid_argument("sound " + std::to_string(start.sound) + "'s start " + *problem);
    }

    out.Line(
        0, "start " + std::to_string(start.sound) + " " + NameText(start.name) + SoundNameComment(start.sound, tables));
    if (start.muted != 0)
    {
        out.Line(1, "muted " + Hex(start.muted));
    }
    if (start.silenced != 0)
    {
        out.Line(1, "silenced " + Hex(start.silenced));
    }

    for (const auto& [track, volume] : start.track_volumes)
    {
        out.Line(1, "track_volume " + std::to_string(track) + " " + FloatText(volume));
    }

    const std::pair<const std::optional<float>*, const char*> kRatios[] = {
        {&start.tempo_ratio, "tempo_ratio"}, {&start.pitch, "pitch"}, {&start.volume, "volume"}};
    for (const auto& [ratio, word] : kRatios)
    {
        if (*ratio)
        {
            out.Line(1, std::string(word) + " " + FloatText(**ratio));
        }
    }

    if (start.sends)
    {
        out.Line(1, "sends " + FloatText((*start.sends)[0]) + " " + FloatText((*start.sends)[1]) + " " +
                        FloatText((*start.sends)[2]));
    }
    if (start.banks)
    {
        std::string line = "banks";
        for (const uint32_t bank : *start.banks)
        {
            line += bank == kNoStartBank ? " none" : " " + Hex(bank);
        }
        out.Line(1, line);
    }
    if (start.seed)
    {
        out.Line(1, "seed " + Hex(*start.seed));
    }
    if (start.low_pass)
    {
        out.Line(1, "low_pass " + FloatText(*start.low_pass));
    }
    if (start.dse_level)
    {
        out.Line(1, "dse_level " + std::to_string(*start.dse_level));
    }

    for (const StartVariable& variable : start.variables)
    {
        out.Line(1, StartVariableText(variable));
    }
}

// Whether `word` starts a line of a start's fields or variables.
bool IsStartWord(const Token& word)
{
    constexpr std::array<std::string_view, 14> kWords = {
        "muted", "silenced", "track_volume", "tempo_ratio", "pitch", "volume",   "sends",
        "banks", "seed",     "low_pass",     "track",       "at",    "variable", "dse_level"};
    return std::any_of(kWords.begin(), kWords.end(), [&word](std::string_view text) { return word.Is(text); });
}

// A start's line: start, its sound's number and its name.
SoundStart ParseStartLine(const std::vector<Token>& tokens, std::size_t line)
{
    if (tokens.size() != 3)
    {
        Fail(line, "a start's line is start, its sound's number and its name");
    }

    SoundStart start;
    start.sound = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, line, "a sound's number"));
    start.name = tokens[2].text;

    return start;
}

// A line of a start's fields: muted or silenced and a mask, track_volume, a track and its volume, tempo_ratio, pitch or
// volume and a number, sends and three numbers, banks and four banks, seed and a number, low_pass and a number,
// dse_level and a level, or a variable. A start gives each field once.
void ParseStartValue(const std::vector<Token>& tokens, std::size_t line, SoundStart& start)
{
    const Token& word = tokens[0];
    const auto once = [&word, line](bool given)
    {
        if (given)
        {
            Fail(line, "the start gives its " + word.text + " twice");
        }
    };
    if (word.Is("muted") || word.Is("silenced"))
    {
        if (tokens.size() != 2)
        {
            Fail(line, "a start's " + word.text + " line is " + word.text + " and a mask of tracks");
        }

        uint32_t& mask = word.Is("muted") ? start.muted : start.silenced;
        once(mask != 0);
        mask = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, line, "a mask of tracks"));
    }
    else if (word.Is("track_volume"))
    {
        if (tokens.size() != 3)
        {
            Fail(line, "a track's volume is track_volume, the track's number and the volume");
        }

        const uint8_t track = ParseStartTrack(tokens[1], line);
        if (!start.track_volumes.emplace(track, ParseFloat(tokens[2], line)).second)
        {
            Fail(line, "the start gives track " + std::to_string(track) + " a volume twice");
        }
    }
    else if (word.Is("tempo_ratio") || word.Is("pitch") || word.Is("volume"))
    {
        std::optional<float>& ratio =
            word.Is("tempo_ratio") ? start.tempo_ratio : (word.Is("pitch") ? start.pitch : start.volume);
        once(ratio.has_value());
        ratio = ParseRatio(tokens, line);
    }
    else if (word.Is("sends"))
    {
        if (tokens.size() != 4)
        {
            Fail(line, "a start's sends are sends and the sends on buses A, B and C");
        }

        once(start.sends.has_value());
        start.sends = StartSends{ParseFloat(tokens[1], line), ParseFloat(tokens[2], line), ParseFloat(tokens[3], line)};
    }
    else if (word.Is("banks"))
    {
        once(start.banks.has_value());
        start.banks = ParseStartBanks(tokens, line);
    }
    else if (word.Is("seed"))
    {
        if (tokens.size() != 2)
        {
            Fail(line, "a start's seed is seed and a number");
        }

        once(start.seed.has_value());
        start.seed = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, line, "a seed"));
    }
    else if (word.Is("low_pass"))
    {
        once(start.low_pass.has_value());
        start.low_pass = ParseRatio(tokens, line);
    }
    else if (word.Is("dse_level"))
    {
        if (tokens.size() != 2)
        {
            Fail(line,
                 "a start's level of DSE is dse_level and a whole number from 0 to " + std::to_string(kFullDseLevel));
        }

        once(start.dse_level.has_value());
        start.dse_level =
            static_cast<int>(ParseInteger(tokens[1], static_cast<uint64_t>(kFullDseLevel), line, "a level of DSE"));
    }
    else
    {
        start.variables.push_back(ParseStartVariable(tokens, line));
    }
}

// Checks the start that the lines up to `line` give.
void FinishStart(const SoundStart& start, std::size_t line)
{
    if (HasTimedTrackVariable(start))
    {
        Fail(line,
             "a later version of Hokuto saved the file: the start changes a variable of a single track after the "
             "sound's start");
    }
    if (const std::optional<std::string> problem = StartProblem(start))
    {
        Fail(line, "the start " + *problem);
    }
}

} // namespace hokuto::format::text_parts
