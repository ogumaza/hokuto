// SPDX-License-Identifier: MIT

// The patterns of a Krawall module for the edits of edit/krawall.h: the effect of an item, the module header, the
// patterns that each order and each pattern lead to, and the walk that finds whether a channel's next note names no
// instrument.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/krawall/commands.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/sar/layout.h"

#include "edit/bytecode.h"
#include "edit/krawall_parts.h"
#include "format/file.h"

namespace hokuto::edit::krawall_parts
{
namespace
{

using ntdlib::commands::Kind;

// The effects that send the rows elsewhere, by the numbers of ntdlib's player (ntdlib/src/krawall/player.cpp).
constexpr uint8_t kPatternJump = 4;
constexpr uint8_t kPatternBreak = 5;
constexpr uint8_t kPatternLoop = 43;

// Whether `command` sends the rows elsewhere after its row: a jump, a break, or a loop that jumps back.
bool ChangesFlow(const Command& command)
{
    const std::optional<uint8_t> effect = EffectOf(command);
    return effect && (*effect == kPatternJump || *effect == kPatternBreak ||
                      (*effect == kPatternLoop && OperandOf(command) != 0));
}

// The patterns that play from order `order` on of `module`, past the markers between songs (+++). Past the last order,
// or at a marker, the module returns to its restart or to a song's start, by the player's mode (Player::NextRow): the
// set contains every one of them.
std::set<uint32_t> PatternsAtOrder(const ModuleHeader& module, uint32_t order)
{
    std::set<uint32_t> patterns;
    const auto add = [&](uint32_t at)
    {
        while (at < module.order_count && module.orders[at] == ntdlib::krawall::kOrderSkip)
        {
            at++;
        }
        if (at < module.order_count && module.orders[at] < module.patterns.size() && module.patterns[module.orders[at]])
        {
            patterns.insert(module.orders[at]);
        }
    };

    bool marker = false;
    while (order < module.order_count && module.orders[order] == ntdlib::krawall::kOrderSkip)
    {
        order++;
        marker = true;
    }
    if (order < module.order_count)
    {
        add(order);
    }

    if (order >= module.order_count || marker)
    {
        add(module.restart);
        for (const uint8_t start : module.song_starts)
        {
            add(start);
        }
    }

    return patterns;
}

// The patterns that can follow pattern `pattern` of `module` at the end of its rows: the next order after each order
// that plays it.
std::set<uint32_t> NextPatterns(const ModuleHeader& module, uint32_t pattern)
{
    std::set<uint32_t> next;
    for (uint32_t order = 0; order < module.order_count; order++)
    {
        if (module.orders[order] == pattern)
        {
            next.merge(PatternsAtOrder(module, order + 1));
        }
    }

    return next;
}

// The items of pattern `pattern` of `module` in `commands`: from its start up to the next pattern's start.
std::pair<uint32_t, uint32_t> ItemsOfPattern(const Sequence& commands, const ModuleHeader& module, uint32_t pattern)
{
    const uint32_t begin = *module.patterns[pattern];
    uint32_t end = static_cast<uint32_t>(commands.items.size());
    for (const std::optional<uint32_t>& start : module.patterns)
    {
        if (start && *start > begin)
        {
            end = std::min(end, *start);
        }
    }

    return {begin, end};
}

// The walk of NextNoteLacksInstrument through the patterns of `module` that `commands` contains, for the notes of
// channel `channel`.
class PatternWalk
{
public:
    PatternWalk(const Sequence& commands, const ModuleHeader& module, uint8_t channel)
        : commands_(commands), module_(module), channel_(channel)
    {
    }

    // Whether a note of the channel that can play after the note at item `from` names no instrument.
    bool Run(uint32_t from)
    {
        // The pattern of `from`, from the row of `from`.
        std::vector<uint32_t> own;
        for (uint32_t pattern = 0; pattern < module_.patterns.size(); pattern++)
        {
            if (module_.patterns[pattern] && ItemsOfPattern(commands_, module_, pattern).first <= from &&
                from < ItemsOfPattern(commands_, module_, pattern).second)
            {
                own.push_back(pattern);
            }
        }

        // The row of `from` starts after the wait before it.
        uint32_t row = from;
        while (row > 0 && commands_.items[row - 1].kind != Kind::kWait &&
               !std::ranges::any_of(own, [&](uint32_t p) { return *module_.patterns[p] == row; }))
        {
            row--;
        }

        for (const uint32_t pattern : own)
        {
            if (Scan(pattern, row, from))
            {
                return true;
            }
        }

        while (!todo_.empty())
        {
            const auto [pattern, any_row] = todo_.back();
            todo_.pop_back();
            const auto [begin, end] = ItemsOfPattern(commands_, module_, pattern);
            if (!any_row)
            {
                if (Scan(pattern, begin, std::nullopt))
                {
                    return true;
                }

                continue;
            }

            if (ScanAnyRow(pattern, begin, end))
            {
                return true;
            }
        }

        return false;
    }

private:
    // A pattern to look at: from its first row, or from any row.
    void Visit(const std::set<uint32_t>& patterns, bool any_row)
    {
        for (const uint32_t pattern : patterns)
        {
            if (seen_.insert({pattern, any_row}).second)
            {
                todo_.emplace_back(pattern, any_row);
            }
        }
    }

    // The places that the effects of a row send the rows to after it: a jump to the first row of its order, a break to
    // a row of the next order, and a loop back to a row of the pattern or, in the later builds, of the next order.
    void Follow(uint32_t pattern, std::span<const Command* const> effects)
    {
        bool jumps = false;
        bool breaks = false;
        bool first_row = true;
        for (const Command* effect : effects)
        {
            const uint8_t number = *EffectOf(*effect);
            jumps = jumps || number == kPatternJump;
            breaks = breaks || number == kPatternBreak || number == kPatternLoop;
            first_row = first_row && (number == kPatternJump || (number == kPatternBreak && OperandOf(*effect) == 0));
        }

        for (const Command* effect : effects)
        {
            if (*EffectOf(*effect) == kPatternJump)
            {
                Visit(PatternsAtOrder(module_, OperandOf(*effect)), !first_row);
            }

            if (*EffectOf(*effect) == kPatternLoop)
            {
                Visit({pattern}, true);
            }
        }

        if (breaks && !jumps)
        {
            Visit(NextPatterns(module_, pattern), !first_row);
        }
    }

    // Scans the rows of `pattern` from item `begin`, and the places that their effects send the rows to. Returns
    // whether the channel's first note after item `after`, where one is given, names no instrument. The rows that break
    // away before that note continue elsewhere: the walk scans those places too.
    bool Scan(uint32_t pattern, uint32_t begin, std::optional<uint32_t> after)
    {
        const uint32_t end = ItemsOfPattern(commands_, module_, pattern).second;
        std::vector<const Command*> flow;
        for (uint32_t i = begin; i < end; i++)
        {
            const Command& command = commands_.items[i];
            if (command.kind == Kind::kWait)
            {
                if (!flow.empty())
                {
                    Follow(pattern, flow);
                    flow.clear();
                }

                continue;
            }

            if (ChangesFlow(command))
            {
                flow.push_back(&command);
            }

            if (command.kind == Kind::kNote && command.index == channel_ && (!after || i > *after))
            {
                return command.sample.value_or(0) == 0;
            }
        }
        if (!flow.empty())
        {
            Follow(pattern, flow);
        }

        Visit(NextPatterns(module_, pattern), false);

        return false;
    }

    // From any row, each note of the channel can come first, and each row's effects can send the rows elsewhere.
    // ScanAnyRow returns whether such a note names no instrument.
    bool ScanAnyRow(uint32_t pattern, uint32_t begin, uint32_t end)
    {
        std::vector<const Command*> flow;
        for (uint32_t i = begin; i <= end; i++)
        {
            if (i == end || commands_.items[i].kind == Kind::kWait)
            {
                if (!flow.empty())
                {
                    Follow(pattern, flow);
                    flow.clear();
                }

                continue;
            }

            const Command& command = commands_.items[i];
            if (command.kind == Kind::kNote && command.index == channel_ && command.sample.value_or(0) == 0)
            {
                return true;
            }

            if (ChangesFlow(command))
            {
                flow.push_back(&command);
            }
        }

        Visit(NextPatterns(module_, pattern), false);

        return false;
    }

    const Sequence& commands_;
    const ModuleHeader& module_;
    uint8_t channel_;
    std::set<std::pair<uint32_t, bool>> seen_;
    std::vector<std::pair<uint32_t, bool>> todo_;
};

} // namespace

std::optional<uint8_t> EffectOf(const Command& command)
{
    const uint16_t opcode = command.encoding.opcode;
    if (command.kind != Kind::kOther || opcode < ntdlib::krawall::kEffectOpcode ||
        opcode >= ntdlib::krawall::kEffectOpcode + 0x100)
    {
        return std::nullopt;
    }

    return static_cast<uint8_t>(opcode - ntdlib::krawall::kEffectOpcode);
}

uint8_t OperandOf(const Command& command)
{
    return command.bytes.empty() ? 0 : command.bytes.front();
}

std::optional<ModuleHeader> HeaderOf(const format::File& file, uint32_t sequence)
{
    if (!file.native)
    {
        return std::nullopt;
    }

    namespace krawall = ntdlib::krawall;
    const format::NativeArchive& native = *file.native;
    const std::vector<uint8_t>* run = nullptr;
    uint32_t piece = 0;
    uint32_t header = 0;
    std::optional<ModuleHeader> module;
    for (const ntdlib::sar::Field& field : native.fields)
    {
        if (!field.to.item || !NamesItemOf(native, field.to, sequence) || field.at.item)
        {
            continue;
        }

        // The first field is the first entry of the table. The header comes before it.
        if (!module)
        {
            run = field.at.piece < native.pieces->size()
                      ? std::get_if<std::vector<uint8_t>>(&(*native.pieces)[field.at.piece])
                      : nullptr;
            if (run == nullptr || field.at.offset < krawall::kModulePatterns || field.at.offset > run->size())
            {
                return std::nullopt;
            }

            piece = field.at.piece;
            header = field.at.offset - krawall::kModulePatterns;
            const uint8_t* bytes = run->data() + header;
            module.emplace();
            module->instruments = bytes[krawall::kModuleFlags] != 0;
            module->order_count = bytes[krawall::kModuleOrderCount];
            module->restart = bytes[krawall::kModuleRestart];
            std::copy_n(bytes + krawall::kModuleOrders, module->orders.size(), module->orders.begin());
            std::copy_n(bytes + krawall::kModuleSongStarts, module->song_starts.size(), module->song_starts.begin());
        }

        const uint32_t entry = (field.at.offset - header - krawall::kModulePatterns) / 4;
        if (field.at.piece != piece || field.at.offset < header + krawall::kModulePatterns ||
            (field.at.offset - header - krawall::kModulePatterns) % 4 != 0 || entry >= 256)
        {
            continue;
        }

        if (module->patterns.size() <= entry)
        {
            module->patterns.resize(entry + 1);
        }

        module->patterns[entry] = field.to.offset;
    }

    return module;
}

bool NextNoteLacksInstrument(const Sequence& commands, const ModuleHeader& module, uint32_t from, uint8_t channel)
{
    return PatternWalk(commands, module, channel).Run(from);
}

} // namespace hokuto::edit::krawall_parts
