#include "netviz/gui/rules.h"
#include "netviz/gui/settings_file.h"
#include "netviz/io/yosys_json.h"

#include <doctest/doctest.h>

#include <filesystem>

using namespace netviz;

namespace {

// The display net a port's bit is on.
uint32_t net_of_port(const DisplayGraph& g, const Module& m, const char* name,
                     const StringPool& names) {
    for (uint32_t v = 0; v < g.nodes().size(); v++) {
        const DisplayNode& node = g.nodes()[v];
        if (node.kind != NodeKind::Cell && node.kind != NodeKind::Instance &&
            names.str(node.name) == name) {
            uint32_t pin = node.pins_begin;
            for (uint32_t n = 0; n < g.nets().size(); n++)
                for (auto list : {g.drivers(g.nets()[n]), g.loads(g.nets()[n])})
                    for (DisplayPinId p : list)
                        if (p.index == pin)
                            return n;
        }
    }
    (void)m;
    return UINT32_MAX;
}

} // namespace

TEST_CASE("rules: glob matching") {
    CHECK(glob_match("*clk*", "sys_CLK_div"));
    CHECK(glob_match("clk", "CLK"));
    CHECK_FALSE(glob_match("clk", "clk2"));
    CHECK(glob_match("clk?", "clk2"));
    CHECK(glob_match("*", ""));
    CHECK(glob_match("$_DFF_*_", "$_DFF_PN0_"));
    CHECK_FALSE(glob_match("$_DFF_*_", "$_DFF_PN0"));
    CHECK(glob_match("a*b*c", "aXXbYYbZc"));
    CHECK_FALSE(glob_match("a*b*c", "aXXbYY"));
}

TEST_CASE("rules: default rules find clocks and flip-flops in an example") {
    ImportResult r = import_yosys_json(std::filesystem::path(NETVIZ_EXAMPLES_DIR) / "03_regfile" /
                                       "regfile.json");
    REQUIRE(r.design);
    const Design& d = *r.design;
    ModuleId id = *d.find_module(*d.names().find("dffe"));
    DisplayGraph g = DisplayGraph::build(d, id);
    std::vector<SignalRule> signals = default_signal_rules();
    std::vector<CellRule> cells = default_cell_rules();
    RuleMatches m = match_rules(d, g, signals, cells);

    uint32_t clk = net_of_port(g, d.module(id), "clk", d.names());
    REQUIRE(clk != UINT32_MAX);
    CHECK(signals[m.net_rule[clk]].name == "Clock");
    CHECK(m.net_name[clk] == "clk");

    bool found_dff = false;
    for (uint32_t v = 0; v < g.nodes().size(); v++)
        if (g.nodes()[v].kind == NodeKind::Cell && d.names().str(g.nodes()[v].type) == "DFF") {
            found_dff = true;
            REQUIRE(m.node_rule[v] != RuleMatches::kNone);
            CHECK(cells[m.node_rule[v]].name == "Flip-flops and latches");
        }
    CHECK(found_dff);

    // Disabled rules do not match.
    signals[0].enabled = false;
    RuleMatches off = match_rules(d, g, signals, cells);
    CHECK(off.net_rule[clk] == RuleMatches::kNone);
}

TEST_CASE("rules: attributes and cell pins") {
    Design d;
    ModuleId id = *d.add_module(d.names().intern("top"));
    Module& m = d.module(id);
    uint32_t first = m.add_signals(3).index;
    SigBit a = SigBit::signal(SignalBitId(first));
    SigBit y = SigBit::signal(SignalBitId(first + 1));
    SigBit q = SigBit::signal(SignalBitId(first + 2));
    WireId wa = *m.add_wire(d.names().intern("n1"), std::span(&a, 1));
    m.wire_attributes(wa).push_back(
        {d.names().intern("tool_clock"), Value{Value::Kind::Bits, "00000001"}});
    m.add_wire(d.names().intern("n2"), std::span(&y, 1));
    m.make_port(*m.add_wire(d.names().intern("n3"), std::span(&q, 1)), Direction::Output);
    NewPin buf[] = {{d.names().intern("A"), Direction::Input, std::span(&a, 1)},
                    {d.names().intern("Y"), Direction::Output, std::span(&y, 1)}};
    CellId b = *m.add_cell(d.names().intern("u_buf"), d.names().intern("BUF"), buf);
    m.cell_attributes(b).push_back(
        {d.names().intern("dont_touch"), Value{Value::Kind::String, "true"}});
    NewPin ff[] = {{d.names().intern("D"), Direction::Input, std::span(&y, 1)},
                   {d.names().intern("GCK"), Direction::Input, std::span(&a, 1)},
                   {d.names().intern("Q"), Direction::Output, std::span(&q, 1)}};
    m.add_cell(d.names().intern("u_reg"), d.names().intern("MYREG"), ff);
    DisplayGraph g = DisplayGraph::build(d, id);

    auto net_of_bit = [&](SigBit bit) {
        for (uint32_t n = 0; n < g.nets().size(); n++)
            for (SignalBitId s : g.bits(g.nets()[n]))
                if (s == bit.signal())
                    return n;
        return UINT32_MAX;
    };

    SignalRule by_attr;
    by_attr.name = "attr";
    by_attr.attributes = {{"tool_clock", "1"}}; // matches the bits value as a number
    SignalRule by_pin;
    by_pin.name = "pin";
    by_pin.cell_pins = {{"myreg", "D"}}; // case-insensitive
    CellRule by_cell_attr;
    by_cell_attr.attributes = {{"dont_*", "*"}};
    CellRule by_name;
    by_name.names = {"u_reg"};

    RuleMatches r = match_rules(d, g, {by_attr, by_pin}, {by_cell_attr, by_name});
    CHECK(r.net_rule[net_of_bit(a)] == 0);
    CHECK(r.net_rule[net_of_bit(y)] == 1);
    CHECK(r.net_rule[net_of_bit(q)] == RuleMatches::kNone);
    CHECK(r.node_rule[b.index] == 0);
    CHECK(r.node_rule[1] == 1); // u_reg, by instance name
}

TEST_CASE("rules: rules survive a settings file round trip") {
    Settings s;
    s.signal_rules[0].nets.push_back("weird \"quoted\" \\\\name");
    s.signal_rules[0].attributes.push_back({"clock_*", "*"});
    s.signal_rules[1].placement = NetPlacement::Label;
    s.cell_rules[0].border = BorderStyle::Dotted;
    s.cell_rules[0].names = {"u_*"};
    s.cell_rules.pop_back();
    Settings read;
    CHECK_FALSE(parse_settings(settings_to_json(s), read));
    CHECK(read.signal_rules == s.signal_rules);
    CHECK(read.cell_rules == s.cell_rules);

    // An empty list means "no rules", not "defaults".
    Settings none;
    none.signal_rules.clear();
    Settings read_none;
    CHECK_FALSE(parse_settings(settings_to_json(none), read_none));
    CHECK(read_none.signal_rules.empty());
    CHECK(read_none.cell_rules == default_cell_rules());
}
