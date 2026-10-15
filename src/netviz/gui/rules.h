#pragma once

// Style rules for the schematic: how signals (nets) and cells are drawn,
// chosen by name patterns. Ordered lists; the first enabled rule that matches
// wins. Part of the user's preferences (settings.json). No ImGui or SDL
// dependency, so it can be tested on its own.

#include "netviz/core/netlist.h"
#include "netviz/view/display_graph.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace netviz {

// RGB color as 0xRRGGBB.
using RuleColor = uint32_t;

enum class LineStyle : uint8_t { Solid, Dashed };
enum class PinMarker : uint8_t { None, Clock }; // Clock: the edge-triggered ">" at input pins
enum class NetPlacement : uint8_t { Routed, Label };
enum class BorderStyle : uint8_t { Solid, Dashed, Dotted, None };

// A cell pin: cell type pattern and pin name pattern, e.g. "*dff*" / "CLK".
struct CellPinPattern {
    std::string cell;
    std::string pin;
    bool operator==(const CellPinPattern&) const = default;
};

// An attribute: name pattern and value pattern, e.g. "keep" / "1", or
// "clock_signal" / "*" for any value. Tools and RTL "(* ... *)" annotations
// set attributes; numeric values also match as decimal numbers.
struct AttributePattern {
    std::string name;
    std::string value = "*";
    bool operator==(const AttributePattern&) const = default;
};

// How matching nets are drawn. A net matches if one of its wires matches by
// name or attribute, or if it connects to a matching cell pin.
struct SignalRule {
    std::string name;
    bool enabled = true;
    std::vector<std::string> nets;            // wire name patterns
    std::vector<AttributePattern> attributes; // of wires
    std::vector<CellPinPattern> cell_pins;
    RuleColor color = 0x808080;
    float width = 1.0f; // line width multiplier
    LineStyle line = LineStyle::Solid;
    PinMarker marker = PinMarker::None;
    // Label: no wire; a short stub with the net's name at each pin. For nets
    // that go everywhere (clocks, resets), which clutter a drawing.
    NetPlacement placement = NetPlacement::Routed;
    bool operator==(const SignalRule&) const = default;
};

// How matching cells are drawn (not submodule instances). A cell matches by
// type, instance name or attribute.
struct CellRule {
    std::string name;
    bool enabled = true;
    std::vector<std::string> types;
    std::vector<std::string> names;
    std::vector<AttributePattern> attributes;
    RuleColor color = 0x808080; // box tint and border
    BorderStyle border = BorderStyle::Solid;
    float border_width = 1.0f;
    bool show_type = true; // the type label under the name
    bool operator==(const CellRule&) const = default;
};

std::vector<SignalRule> default_signal_rules();
std::vector<CellRule> default_cell_rules();

// Case-insensitive glob: '*' any run of characters, '?' one character.
bool glob_match(std::string_view pattern, std::string_view text);

// Which rule applies to each net and cell of a display graph.
struct RuleMatches {
    static constexpr uint16_t kNone = UINT16_MAX;
    std::vector<uint16_t> net_rule;    // per display net: signal rule index, or kNone
    std::vector<uint16_t> node_rule;   // per display node: cell rule index (cells only), or kNone
    std::vector<std::string> net_name; // per display net: a wire name (for labels), or empty
};

RuleMatches match_rules(const Design& design, const DisplayGraph& graph,
                        const std::vector<SignalRule>& signals, const std::vector<CellRule>& cells);

} // namespace netviz
