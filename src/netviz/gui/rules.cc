#include "netviz/gui/rules.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace netviz {

std::vector<SignalRule> default_signal_rules() {
    SignalRule clock;
    clock.name = "Clock";
    clock.nets = {"*clk*", "*clock*"};
    // Clock pins of Yosys flip-flops ($dff, $adff, ... CLK; $_DFF_*_ C) and of
    // common library flip-flops (CK, CLK).
    clock.cell_pins = {{"*dff*", "C"}, {"*dff*", "CLK"}, {"*dff*", "CK"}};
    clock.color = 0xD070E0;
    clock.width = 1.5f;
    clock.marker = PinMarker::Clock;

    SignalRule reset;
    reset.name = "Reset";
    reset.nets = {"*rst*", "*reset*"};
    reset.cell_pins = {{"*dff*", "ARST"}, {"*dff*", "SRST"}, {"*dff*", "R"},
                       {"*dff*", "CLR"},  {"*dff*", "RN"},   {"*dff*", "RESET"}};
    reset.color = 0xF06040;
    reset.line = LineStyle::Dashed;
    return {clock, reset};
}

std::vector<CellRule> default_cell_rules() {
    auto rule = [](const char* name, std::vector<std::string> types, RuleColor color) {
        CellRule r;
        r.name = name;
        r.types = std::move(types);
        r.color = color;
        return r;
    };
    // The same groups netviz used before rules existed; edit or reorder freely.
    return {
        rule("Flip-flops and latches", {"*dff*", "*latch*", "*flop*"}, 0x8052B8),
        rule("Multiplexers", {"*mux*"}, 0xB88020),
        rule("Arithmetic and compare",
             {"$add",   "$sub", "$mul", "$div*", "$mod*", "$neg",    "$pow",    "$alu",   "$lcu",
              "$macc*", "$shl", "$shr", "$sshl", "$sshr", "$shift*", "$lt",     "$le",    "$gt",
              "$ge",    "$eq",  "$ne",  "$eqx",  "$nex",  "$fa",     "$_*ADD*", "*adder*"},
             0xB84664),
        rule("Logic gates",
             {"*and*", "*or*", "*xor*", "*xnor*", "*not*", "*buf*", "*inv*", "*aoi*", "*oai*",
              "$reduce_*", "$logic_*"},
             0x208080),
    };
}

bool glob_match(std::string_view pattern, std::string_view text) {
    // Iterative matcher with backtracking to the last '*'.
    size_t p = 0, t = 0, star = std::string_view::npos, resume = 0;
    auto same = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || same(pattern[p], text[t]))) {
            p++;
            t++;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        p++;
    return p == pattern.size();
}

namespace {

bool any_match(const std::vector<std::string>& patterns, std::string_view text) {
    for (const std::string& p : patterns)
        if (glob_match(p, text))
            return true;
    return false;
}

bool any_attribute(const std::vector<AttributePattern>& patterns,
                   const std::vector<NamedValue>& attributes, const StringPool& names) {
    for (const AttributePattern& p : patterns)
        for (const NamedValue& a : attributes) {
            if (!glob_match(p.name, names.str(a.name)))
                continue;
            if (glob_match(p.value, a.value.text))
                return true;
            if (std::optional<uint64_t> n = as_int(a.value);
                n && glob_match(p.value, std::to_string(*n)))
                return true;
        }
    return false;
}

} // namespace

RuleMatches match_rules(const Design& design, const DisplayGraph& graph,
                        const std::vector<SignalRule>& signals,
                        const std::vector<CellRule>& cells) {
    const Module& module = design.module(graph.module());
    const StringPool& names = design.names();
    RuleMatches out;
    out.net_rule.assign(graph.nets().size(), RuleMatches::kNone);
    out.node_rule.assign(graph.nodes().size(), RuleMatches::kNone);
    out.net_name.assign(graph.nets().size(), "");

    // Per signal bit: the first signal rule matching one of its wires' names,
    // and a wire naming it (a visible name if there is one).
    struct Name {
        uint32_t wire = UINT32_MAX;
        uint32_t offset = 0;
    };
    std::vector<uint16_t> bit_rule(module.signal_count(), RuleMatches::kNone);
    std::vector<Name> bit_name(module.signal_count());
    std::span<const Wire> wires = module.wires();
    for (uint32_t w = 0; w < wires.size(); w++) {
        const Wire& wire = wires[w];
        const std::string& name = names.str(wire.name);
        uint16_t rule = RuleMatches::kNone;
        for (size_t r = 0; r < signals.size() && rule == RuleMatches::kNone; r++)
            if (signals[r].enabled &&
                (any_match(signals[r].nets, name) ||
                 any_attribute(signals[r].attributes, wire.attributes, names)))
                rule = static_cast<uint16_t>(r);
        std::span<const SigBit> bits = module.bits(wire);
        for (uint32_t i = 0; i < bits.size(); i++) {
            if (bits[i].is_constant())
                continue;
            uint32_t b = bits[i].signal().index;
            bit_rule[b] = std::min(bit_rule[b], rule);
            Name& n = bit_name[b];
            if (n.wire == UINT32_MAX || (wires[n.wire].hidden && !wire.hidden))
                n = {w, i};
        }
    }

    for (uint32_t n = 0; n < graph.nets().size(); n++) {
        std::span<const SignalBitId> bits = graph.bits(graph.nets()[n]);
        for (SignalBitId b : bits)
            out.net_rule[n] = std::min(out.net_rule[n], bit_rule[b.index]);
        // Label text: the wire of the first bit, with the bits' range if partial.
        if (bits.empty() || bit_name[bits[0].index].wire == UINT32_MAX)
            continue;
        Name first = bit_name[bits[0].index];
        const Wire& wire = wires[first.wire];
        bool consecutive = true;
        for (size_t i = 1; i < bits.size() && consecutive; i++) {
            Name next = bit_name[bits[i].index];
            consecutive = next.wire == first.wire && next.offset == first.offset + i;
        }
        std::string text = names.str(wire.name);
        if (wire.width > 1 && !(consecutive && bits.size() == wire.width)) {
            int64_t lo = bit_index(wire, first.offset);
            if (bits.size() == 1)
                text += std::format("[{}]", lo);
            else if (consecutive)
                text += std::format("[{}:{}]", bit_index(wire, first.offset + bits.size() - 1), lo);
            else
                text += "[...]";
        }
        out.net_name[n] = std::move(text);
    }

    // Cell pins, and cell rules.
    std::vector<uint32_t> pin_net(graph.pins().size(), UINT32_MAX);
    for (uint32_t n = 0; n < graph.nets().size(); n++) {
        for (DisplayPinId p : graph.drivers(graph.nets()[n]))
            pin_net[p.index] = n;
        for (DisplayPinId p : graph.loads(graph.nets()[n]))
            pin_net[p.index] = n;
    }
    for (uint32_t v = 0; v < graph.nodes().size(); v++) {
        const DisplayNode& node = graph.nodes()[v];
        if (node.kind != NodeKind::Cell)
            continue;
        const std::string& type = names.str(node.type);
        const Cell& cell = module.cell(node.cell);
        for (size_t r = 0; r < cells.size(); r++)
            if (cells[r].enabled && (any_match(cells[r].types, type) ||
                                     any_match(cells[r].names, names.str(cell.name)) ||
                                     any_attribute(cells[r].attributes, cell.attributes, names))) {
                out.node_rule[v] = static_cast<uint16_t>(r);
                break;
            }
        for (size_t r = 0; r < signals.size(); r++) {
            if (!signals[r].enabled)
                continue;
            for (const CellPinPattern& cp : signals[r].cell_pins) {
                if (!glob_match(cp.cell, type))
                    continue;
                std::span<const DisplayPin> pins = graph.pins_of(node);
                for (size_t i = 0; i < pins.size(); i++) {
                    uint32_t net = pin_net[node.pins_begin + i];
                    if (net != UINT32_MAX && pins[i].name.valid() &&
                        glob_match(cp.pin, names.str(pins[i].name)))
                        out.net_rule[net] = std::min(out.net_rule[net], static_cast<uint16_t>(r));
                }
            }
        }
    }
    return out;
}

} // namespace netviz
