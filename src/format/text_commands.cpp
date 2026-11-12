// SPDX-License-Identifier: MIT

// The items of a sequence in the text form and back, and the checks of their commands against the console and the
// driver.

#include "format/text_parts.h"

namespace hokuto::format::text_parts
{

namespace
{

// An item's line: its name and the parts that its kind has, then the other parts and the encoder's choices in brackets.
std::string ItemText(const Command& item, const std::vector<std::string>& target_names)
{
    const auto target_text = [&](uint32_t target, bool outside)
    {
        return outside ? "@" + std::to_string(target) : target_names[target];
    };

    std::string text = item.conditional ? "if " : "";
    text += ntdlib::commands::Name(item.kind);
    if (item.kind == Kind::kOther)
    {
        text += " " + Hex(item.encoding.opcode);
    }

    if (HasIndex(item.kind))
    {
        text += " " + std::to_string(item.index);
    }

    if (item.kind == Kind::kNote)
    {
        text += " " + std::to_string(item.key) + " " + Number(item.velocity);
    }

    if (item.kind == Kind::kNoteOff)
    {
        text += " " + std::to_string(item.key);
    }

    if (HasValue(item.kind))
    {
        text += " " + ArgumentText(item.value);
    }

    if (item.time)
    {
        text += " over " + ArgumentText(*item.time);
    }

    if (ntdlib::commands::HasTarget(item.kind))
    {
        text += " " + target_text(item.target, item.target_outside);
    }

    if (item.kind == Kind::kData)
    {
        for (const uint8_t byte : item.bytes)
        {
            text += " " + HexByte(byte);
        }
    }

    // The parts that the kind doesn't have. Only an unusual sequence holds them.
    if (!HasIndex(item.kind) && item.index != 0)
    {
        text += " [index " + std::to_string(item.index) + "]";
    }

    if (!HasKey(item.kind) && item.key != 0)
    {
        text += " [key " + std::to_string(item.key) + "]";
    }

    if (item.kind != Kind::kNote && item.velocity != 0)
    {
        text += " [velocity " + Number(item.velocity) + "]";
    }

    if (!HasValue(item.kind) && item.value != Argument{})
    {
        text += " [value " + ArgumentText(item.value) + "]";
    }

    if (!ntdlib::commands::HasTarget(item.kind) && (item.target != 0 || item.target_outside))
    {
        text += " [target " + (item.target_outside ? "@" : std::string()) + std::to_string(item.target) + "]";
    }

    if (item.kind != Kind::kOther && item.encoding.opcode != 0)
    {
        text += " [opcode " + Hex(item.encoding.opcode) + "]";
    }

    if (item.kind != Kind::kData && !item.bytes.empty())
    {
        text += " [bytes";
        for (const uint8_t byte : item.bytes)
        {
            text += " " + HexByte(byte);
        }

        text += "]";
    }

    if (item.sample)
    {
        text += " [sample " + std::to_string(*item.sample) + "]";
    }

    // The encoder's choices.
    if (item.encoding.value_bytes != 0)
    {
        text += " [value_bytes " + std::to_string(item.encoding.value_bytes) + "]";
    }

    if (item.encoding.unused_time != 0)
    {
        text += " [unused_time " + Hex(item.encoding.unused_time) + "]";
    }

    if (item.encoding.unused_random)
    {
        text += " [unused_random]";
    }

    if (item.encoding.unused_variable)
    {
        text += " [unused_variable]";
    }

    if (item.encoding.repeated)
    {
        text += " [repeated]";
    }

    if (item.encoding.arguments)
    {
        text += " [arguments " + std::to_string(*item.encoding.arguments) + "]";
    }

    if (item.encoding.length_code)
    {
        text += " [length_code " + std::to_string(*item.encoding.length_code) + "]";
    }

    return text;
}

} // namespace

// Writes sequence `number`: the order of its labels when it isn't the order of their names, then each label and item.
// Each target and each item that `named` marks gets a name. One without a label gets a local one, .L1 onwards in the
// order of the items. Returns each item's name: its first label, a local label, or nothing.
std::vector<std::string> WriteSequence(TextOut& out, const Sequence& sequence, std::size_t number,
                                       const std::vector<bool>& named)
{
    const std::size_t count = sequence.items.size();
    std::vector<std::vector<std::string>> labels(count + 1);
    std::set<std::string> names;
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        if (!names.insert(label.name).second)
        {
            throw std::invalid_argument("sequence " + std::to_string(number) + " has two labels named " + label.name);
        }
        if (label.item > count)
        {
            throw std::invalid_argument("label " + label.name + " of sequence " + std::to_string(number) +
                                        " is past its end");
        }

        labels[label.item].push_back(NameText(label.name));
    }

    std::vector<bool> targeted(count + 1);
    for (const Command& item : sequence.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            if (item.target > count)
            {
                throw std::invalid_argument("a command of sequence " + std::to_string(number) + " goes past its end");
            }

            targeted[item.target] = true;
        }
    }

    std::vector<std::string> target_names(count + 1);
    std::vector<std::string> locals(count + 1);
    int next_local = 1;
    for (std::size_t i = 0; i <= count; ++i)
    {
        if (!labels[i].empty())
        {
            target_names[i] = labels[i].front();
        }
        else if (targeted[i] || (i < named.size() && named[i]))
        {
            locals[i] = ".L" + std::to_string(next_local++);
            target_names[i] = locals[i];
        }
    }

    out.Line(0, "sequence " + std::to_string(number));
    std::vector<std::string> order;
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        order.push_back(label.name);
    }
    if (!std::is_sorted(order.begin(), order.end()))
    {
        std::string line = "labels";
        for (const std::string& name : order)
        {
            line += " " + NameText(name);
        }

        out.Line(1, line);
    }

    for (std::size_t i = 0; i <= count; ++i)
    {
        for (const std::string& label : labels[i])
        {
            out.Line(1, label + ":");
        }

        if (!locals[i].empty())
        {
            out.Line(1, locals[i] + ":");
        }

        if (i < count)
        {
            out.Line(2, ItemText(sequence.items[i], target_names));
        }
    }

    return target_names;
}

// The first item of `sequence` whose value falls between two values of the scale that `scale_of` returns for its kind,
// and the message. The message names `whose` values. With `sources`, a variable passes, and both ends of a random range
// have to be on the scale. A kData item, a command of another kind (kOther) and a kind without a unit pass.
template <typename ScaleOf>
std::optional<std::pair<std::size_t, std::string>> OffScale(const ntdlib::commands::Sequence& sequence,
                                                            ScaleOf scale_of, std::string_view whose, bool sources)
{
    for (std::size_t i = 0; i < sequence.items.size(); ++i)
    {
        const Command& command = sequence.items[i];
        const ntdlib::commands::Scale scale = scale_of(command.kind);
        const bool on_scale =
            OnScale(command.value.value, scale) &&
            (!sources || command.value.source != Source::kRandom || OnScale(command.value.high, scale));
        const bool exact = command.kind == Kind::kData || command.kind == Kind::kOther ||
                           !ntdlib::commands::UnitOf(command.kind) ||
                           (sources && command.value.source == Source::kVariable) || on_scale;
        if (!exact)
        {
            return std::pair(i, std::string(ntdlib::commands::Name(command.kind)) + " has to take one of " +
                                    std::string(whose) + " values, not " + ShortArgumentText(command.value));
        }
    }

    return std::nullopt;
}

// Why MP2K's bytes can't store the commands of `sequence` as they are in the text form, where a note reads up to
// `note_arguments` bytes: the item, and the encoder's reason (ntdlib::mp2k::EncodeSequence), or a value between two of
// MP2K's values. Nothing where the bytes hold them. The encoder rounds a value to the nearest value of MP2K. When the
// encoder refuses the sequence, the function encodes each item alone, without its target and without the choices that
// tie it to the items before, to find the item. A refusal that no single item causes is reported at the first item.
std::optional<std::pair<std::size_t, std::string>> Mp2kProblem(const ntdlib::commands::Sequence& sequence,
                                                               int note_arguments)
{
    if (auto off = OffScale(sequence, [](Kind kind) { return ntdlib::mp2k::ScaleOf(kind); }, "MP2K's", true))
    {
        return off;
    }

    try
    {
        ntdlib::mp2k::EncodeSequence(sequence, ntdlib::agb::kRomBase, note_arguments);
        return std::nullopt;
    }
    catch (const std::invalid_argument& error)
    {
        for (std::size_t i = 0; i < sequence.items.size(); ++i)
        {
            Command probe = sequence.items[i];
            if (probe.target)
            {
                probe.target = 0;
                probe.target_outside = false;
            }

            probe.encoding.repeated = false;
            probe.encoding.arguments.reset();
            try
            {
                ntdlib::mp2k::EncodeSequence(ntdlib::commands::Sequence{{probe}, {}}, ntdlib::agb::kRomBase,
                                             note_arguments);
            }
            catch (const std::invalid_argument& alone)
            {
                return std::pair(i, std::string(alone.what()));
            }
        }

        return std::pair(std::size_t{0}, std::string(error.what()));
    }
}

// Why DSE's bytes can't store the commands of `sequence` as they are in the text form: the item, and the encoder's
// reason (ntdlib::dse::EncodeTracks), or a value between two of DSE's values. Nothing where the bytes hold them. When
// the encoder refuses the sequence, the function encodes each item after the first alone to find the item.
std::optional<std::pair<std::size_t, std::string>> DseProblem(const ntdlib::commands::Sequence& sequence)
{
    if (auto off = OffScale(sequence, [](Kind kind) { return ntdlib::dse::ScaleOf(kind); }, "DSE's", false))
    {
        return off;
    }

    try
    {
        ntdlib::dse::EncodeTracks(sequence);
        return std::nullopt;
    }
    catch (const std::invalid_argument& error)
    {
        for (std::size_t i = 0; i < sequence.items.size(); ++i)
        {
            try
            {
                ntdlib::dse::EncodeTracks(ntdlib::commands::Sequence{{sequence.items.front(), sequence.items[i]}, {}});
            }
            catch (const std::invalid_argument& alone)
            {
                return std::pair(i, std::string(alone.what()));
            }
        }

        return std::pair(std::size_t{0}, std::string(error.what()));
    }
}

// Why the bytes of revision `revision` of `driver` can't store the commands of `sequence` as they are in the text form,
// with its tracks at the items `entries`: the item, and the encoder's reason (Driver::Encode), or a value between two
// of the driver's values. Nothing where the bytes hold them. When the encoder refuses the values of an item, the result
// names that item. A refusal that no single item causes is reported at the first item.
std::optional<std::pair<std::size_t, std::string>> DriverProblem(const Driver& driver,
                                                                 const ntdlib::commands::Sequence& sequence,
                                                                 std::span<const uint32_t> entries, uint8_t revision)
{
    if (auto off = OffScale(
            sequence, [&driver, revision](Kind kind) { return driver.ScaleOf(kind, revision); }, "the driver's", false))
    {
        return off;
    }

    for (std::size_t i = 0; i < sequence.items.size(); ++i)
    {
        try
        {
            driver.ItemOffsets(driver.Alone(sequence.items[i]), revision);
        }
        catch (const std::invalid_argument& alone)
        {
            return std::pair(i, std::string(alone.what()));
        }
    }

    try
    {
        driver.Encode(sequence, entries, revision);
        return std::nullopt;
    }
    catch (const std::invalid_argument& error)
    {
        return std::pair(std::size_t{0}, std::string(error.what()));
    }
}

// Why the DS's bytes can't store the commands of `sequence` as they are in the text form: the item, and the encoder's
// reason (ntdlib::ntr::snd::EncodeSequence), or a value between two of the DS's values. Nothing where the bytes hold
// them. The encoder takes a value to the DS's nearest. When the encoder refuses the sequence, the function encodes each
// item alone, without its target, to find the item. A refusal that no single item causes is reported at the first item.
std::optional<std::pair<std::size_t, std::string>> NtrProblem(const ntdlib::commands::Sequence& sequence)
{
    if (auto off = OffScale(sequence, [](Kind kind) { return ntdlib::ntr::snd::ScaleOf(kind); }, "the DS's", true))
    {
        return off;
    }

    try
    {
        ntdlib::ntr::snd::EncodeSequence(sequence);
        return std::nullopt;
    }
    catch (const std::invalid_argument& error)
    {
        for (std::size_t i = 0; i < sequence.items.size(); ++i)
        {
            Command probe = sequence.items[i];
            if (probe.target)
            {
                probe.target = 0;
                probe.target_outside = false;
            }

            try
            {
                ntdlib::ntr::snd::EncodeSequence(ntdlib::commands::Sequence{{probe}, {}});
            }
            catch (const std::invalid_argument& alone)
            {
                return std::pair(i, std::string(alone.what()));
            }
        }

        return std::pair(std::size_t{0}, std::string(error.what()));
    }
}

// Why the console's bytes can't hold the values of `command` as the text form gives them, by the rules of `grammar`: a
// value out of its range, between two values of the console's scale, or from a source that the kind doesn't take. An
// example is "wait has to be a whole number from 0 to 4294967295, not -5". Nothing where the bytes hold them. A kData
// item and a command of another kind (kOther) aren't checked: they give the console's bytes.
std::optional<std::string> CommandProblem(const ntdlib::nwsnd::Grammar& grammar, const Command& command)
{
    using ntdlib::nwsnd::Scale;
    if (command.kind == Kind::kData || command.kind == Kind::kOther)
    {
        return std::nullopt;
    }

    // The encoder takes a value to the console's nearest. A value between two takes another value without a word.
    const Scale value_scale = ntdlib::nwsnd::ScaleOf(command.kind, grammar);

    const auto exact = [](const Argument& argument, const Scale& scale)
    {
        return argument.source == Source::kVariable ||
               (OnScale(argument.value, scale) &&
                (argument.source != Source::kRandom || OnScale(argument.high, scale)));
    };

    const bool note = command.kind == Kind::kNote;
    const bool whole_velocity = !note || OnScale(command.velocity, ntdlib::nwsnd::kLevelScale);
    const bool exact_value = !ntdlib::commands::UnitOf(command.kind) || exact(command.value, value_scale);
    const bool exact_time = !command.time || exact(*command.time, ntdlib::nwsnd::kTicksScale);
    if (whole_velocity && exact_value && exact_time && Holds(grammar, command))
    {
        return std::nullopt;
    }

    // The part that the bytes can't hold, with the values that they hold there.
    const std::string name(ntdlib::commands::Name(command.kind));
    if (note)
    {
        Command probe = command;
        probe.value = Argument{};
        const std::optional<ValueRange> range = HeldVelocities(grammar, probe);
        if (range && (!whole_velocity || command.velocity < range->lowest || command.velocity > range->highest))
        {
            return "a note's velocity has to be " + RangeText(*range, ntdlib::nwsnd::kLevelScale) + ", not " +
                   ShortNumber(command.velocity);
        }
    }

    for (const bool time : {false, true})
    {
        if (time && !command.time)
        {
            continue;
        }

        // The argument's range with a value of 0 in the other argument.
        const Argument& argument = time ? *command.time : command.value;
        Command probe = command;
        if (time)
        {
            probe.value = Argument{};
        }
        else if (probe.time)
        {
            probe.time = Argument{};
        }

        const std::optional<ValueRange> range = HeldRange(grammar, probe, argument.source, time);
        const std::string what = time ? name + "'s time" : note ? "a note's length" : name;
        if (!range)
        {
            // A kind that takes a value, but not from this source, or no time.
            if (time && !HeldRange(grammar, probe, Source::kValue, true))
            {
                return name + " takes no time, not over " + ShortArgumentText(argument);
            }
            if (argument.source != Source::kValue && HeldRange(grammar, probe, Source::kValue, time))
            {
                return what + " takes no " + (argument.source == Source::kRandom ? "random value" : "variable") +
                       ", not " + ShortArgumentText(argument);
            }

            continue;
        }

        const bool variable = argument.source == Source::kVariable;
        const Scale scale = variable ? Scale{1, 0} : time ? ntdlib::nwsnd::kTicksScale : value_scale;
        const auto fits = [&](int64_t value)
        {
            return value >= range->lowest && value <= range->highest && (variable || OnScale(value, scale));
        };
        if (fits(argument.value) && (argument.source != Source::kRandom || fits(argument.high)))
        {
            continue;
        }

        if (variable)
        {
            return what + " has to read a variable from " + std::to_string(range->lowest) + " to " +
                   std::to_string(range->highest) + ", not " + ShortArgumentText(argument);
        }

        return what + " has to be " + RangeText(*range, scale) + ", not " + ShortArgumentText(argument);
    }

    // Another reason, such as a part that the kind doesn't have. The export gives it.
    return std::nullopt;
}

namespace
{

// Reads the bracketed part `part` of a command, with its values `args`, into `item`.
void ParseBracketedPart(const std::string& part, std::span<const Token> args, std::size_t line, Command& item)
{
    const auto one = [&]() -> const Token&
    {
        if (args.size() != 1)
        {
            Fail(line, "[" + part + "] has to have one value");
        }

        return args[0];
    };

    if (part == "index")
    {
        item.index = static_cast<uint8_t>(ParseInteger(one(), 255, line, "an index"));
    }
    else if (part == "key")
    {
        // A note's key holds 0 to 127. Its main parts take the same keys.
        const bool note = HasKey(item.kind);
        item.key = static_cast<uint8_t>(ParseInteger(one(), note ? 127 : 255, line, note ? "a note's key" : "a key"));
    }
    else if (part == "velocity")
    {
        item.velocity = ParseNumber(one(), line);
    }
    else if (part == "value")
    {
        item.value = ParseArgument(one(), line);
    }
    else if (part == "target")
    {
        const Token& target = one();
        item.target_outside = target.text.starts_with("@");
        item.target = static_cast<uint32_t>(ParseInteger(
            Token{item.target_outside ? target.text.substr(1) : target.text, false}, 0xFFFFFFFF, line, "a target"));
    }
    else if (part == "opcode")
    {
        item.encoding.opcode = static_cast<uint16_t>(ParseInteger(one(), 0xFFFF, line, "an opcode"));
    }
    else if (part == "bytes")
    {
        for (const Token& byte : args)
        {
            item.bytes.push_back(ParseHexByte(byte, line));
        }
    }
    else if (part == "value_bytes")
    {
        item.encoding.value_bytes = static_cast<uint8_t>(ParseInteger(one(), 255, line, "value_bytes"));
    }
    else if (part == "unused_time")
    {
        item.encoding.unused_time = static_cast<uint8_t>(ParseInteger(one(), 255, line, "unused_time"));
    }
    else if (part == "unused_random" && args.empty())
    {
        item.encoding.unused_random = true;
    }
    else if (part == "unused_variable" && args.empty())
    {
        item.encoding.unused_variable = true;
    }
    else if (part == "repeated" && args.empty())
    {
        item.encoding.repeated = true;
    }
    else if (part == "arguments")
    {
        item.encoding.arguments = static_cast<uint8_t>(ParseInteger(one(), 255, line, "arguments"));
    }
    else if (part == "length_code")
    {
        item.encoding.length_code = static_cast<uint8_t>(ParseInteger(one(), 254, line, "length_code"));
    }
    else if (part == "sample")
    {
        item.sample = static_cast<uint16_t>(ParseInteger(one(), 0xFFFF, line, "a sample"));
    }
    else
    {
        Fail(line, "no part of a command is written [" + part + (args.empty() ? "" : " ...") + "]");
    }
}

// Reads the target `target` of a command at item `at` into `item`: an offset after an @, or else a name that is added
// to `references`.
void ParseTarget(const Token& target, std::size_t line, uint32_t at, Command& item, std::vector<Reference>& references)
{
    if (!target.quoted && target.text.starts_with("@"))
    {
        item.target_outside = true;
        item.target =
            static_cast<uint32_t>(ParseInteger(Token{target.text.substr(1), false}, 0xFFFFFFFF, line, "an offset"));
    }
    else
    {
        references.push_back({line, at, target});
    }
}

// Reads the bracketed parts of a command, from token `t` of `tokens` to the end, into `item`.
void ParseBracketedParts(const std::vector<Token>& tokens, std::size_t t, std::size_t line, Command& item)
{
    while (t < tokens.size())
    {
        if (!tokens[t].Is("["))
        {
            Fail(line, "unexpected " + tokens[t].text);
        }

        const std::size_t open = ++t;
        while (t < tokens.size() && !tokens[t].Is("]"))
        {
            ++t;
        }
        if (t >= tokens.size() || t == open)
        {
            Fail(line, "a [ has no ] or is empty");
        }

        const std::string& part = tokens[open].text;
        const std::span<const Token> args(tokens.data() + open + 1, t - open - 1);
        ++t;
        ParseBracketedPart(part, args, line, item);
    }
}

} // namespace

// The parts of a command from its tokens. A target by name goes into `references`.
Command ParseCommand(const std::vector<Token>& tokens, std::size_t line, uint32_t at,
                     std::vector<Reference>& references)
{
    Command item;
    std::size_t t = 0;
    if (tokens[t].Is("if"))
    {
        item.conditional = true;
        ++t;
    }
    if (t >= tokens.size())
    {
        Fail(line, "if has no command");
    }

    const std::optional<Kind> kind = tokens[t].quoted ? std::nullopt : ntdlib::commands::KindNamed(tokens[t].text);
    if (!kind)
    {
        Fail(line, "no command is named " + tokens[t].text);
    }

    item.kind = *kind;
    const std::string name = tokens[t++].text;
    const auto next = [&]() -> const Token&
    {
        if (t >= tokens.size() || tokens[t].Is("["))
        {
            Fail(line, name + " is missing a part");
        }

        return tokens[t++];
    };

    if (item.kind == Kind::kOther)
    {
        item.encoding.opcode = static_cast<uint16_t>(ParseInteger(next(), 0xFFFF, line, "an opcode"));
    }

    if (HasIndex(item.kind))
    {
        item.index = static_cast<uint8_t>(ParseInteger(next(), 255, line, "a number"));
    }

    if (item.kind == Kind::kNote)
    {
        // ReadText checks the key once it has read the archive's driver: on some of the GBA's drivers, the key stores
        // the byte of the command.
        item.key = static_cast<uint8_t>(ParseInteger(next(), 255, line, "a note's key"));
        item.velocity = ParseNumber(next(), line);
    }

    if (item.kind == Kind::kNoteOff)
    {
        item.key = static_cast<uint8_t>(ParseInteger(next(), 127, line, "a note's key"));
    }

    if (HasValue(item.kind))
    {
        item.value = ParseArgument(next(), line);
    }

    if (t < tokens.size() && tokens[t].Is("over"))
    {
        ++t;
        item.time = ParseArgument(next(), line);
    }

    if (ntdlib::commands::HasTarget(item.kind))
    {
        ParseTarget(next(), line, at, item, references);
    }

    if (item.kind == Kind::kData)
    {
        while (t < tokens.size() && !tokens[t].Is("["))
        {
            item.bytes.push_back(ParseHexByte(tokens[t++], line));
        }
    }

    ParseBracketedParts(tokens, t, line, item);

    return item;
}

void FinishSequence(SequenceDraft& draft)
{
    Sequence& sequence = draft.sequence;
    for (const Reference& reference : draft.references)
    {
        const bool local = !reference.name.quoted && reference.name.text.starts_with(".");
        const std::map<std::string, uint32_t>& names = local ? draft.locals : draft.labels;
        const auto found = names.find(reference.name.text);
        if (found == names.end())
        {
            Fail(reference.line, "no label is named " + reference.name.text);
        }

        sequence.items[reference.item].target = found->second;
    }

    // The labels in the order that the labels line gives, or else in the order of their names.
    if (draft.order.empty())
    {
        for (const auto& [name, item] : draft.labels)
        {
            sequence.labels.push_back({name, item});
        }

        return;
    }

    if (draft.order.size() != draft.labels.size())
    {
        Fail(draft.order_line, "the labels line has to name each label once");
    }

    for (const std::string& name : draft.order)
    {
        const auto found = draft.labels.find(name);
        if (found == draft.labels.end())
        {
            Fail(draft.order_line, "the labels line names " + name + ", and the sequence has no such label");
        }

        sequence.labels.push_back({name, found->second});
    }
}

} // namespace hokuto::format::text_parts
