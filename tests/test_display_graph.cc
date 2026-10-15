#include "netviz/io/yosys_json.h"
#include "netviz/view/display_graph.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <set>

using namespace netviz;

namespace {

SigBit sig(uint32_t i) {
    return SigBit::signal(SignalBitId(i));
}

// The pins of a net, as (node name, pin name) strings, e.g. "Q.A".
std::set<std::string> net_pins(const Design& d, const DisplayGraph& g, const DisplayNet& net) {
    std::set<std::string> out;
    auto add = [&](DisplayPinId id) {
        const DisplayPin& pin = g.pin(id);
        std::string name = d.names().str(g.node(pin.node).name);
        if (pin.name.valid())
            name += "." + d.names().str(pin.name);
        out.insert(name);
    };
    for (DisplayPinId p : g.drivers(net))
        add(p);
    for (DisplayPinId p : g.loads(net))
        add(p);
    return out;
}

// One driver P.Y = [s0, s1, s2]; loads Q.A = [s0, s1] and R.A = [s1, s2].
// Per bit: s0 -> {P, Q}, s1 -> {P, Q, R}, s2 -> {P, R}.
Design overlap_design() {
    Design d;
    auto n = [&](const char* t) { return d.names().intern(t); };
    Module& m = d.module(*d.add_module(n("top")));
    m.add_signals(3);
    SigBit p_bits[] = {sig(0), sig(1), sig(2)};
    SigBit q_bits[] = {sig(0), sig(1)};
    SigBit r_bits[] = {sig(1), sig(2)};
    NewPin p{n("Y"), Direction::Output, p_bits};
    NewPin q{n("A"), Direction::Input, q_bits};
    NewPin r{n("A"), Direction::Input, r_bits};
    m.add_cell(n("P"), n("SRC"), std::span(&p, 1));
    m.add_cell(n("Q"), n("SNK"), std::span(&q, 1));
    m.add_cell(n("R"), n("SNK"), std::span(&r, 1));
    return d;
}

} // namespace

TEST_CASE("display graph: nodes, pins and sides for the full adder") {
    ImportResult r = import_yosys_json(std::filesystem::path(NETVIZ_EXAMPLES_DIR) /
                                       "01_fulladder/full_adder.json");
    REQUIRE(r.design);
    const Design& d = *r.design;
    ModuleId top = *d.top();
    const Module& m = d.module(top);
    DisplayGraph g = DisplayGraph::build(d, top);

    CHECK(g.nodes().size() == m.cells().size() + m.ports().size());
    size_t instances = std::count_if(g.nodes().begin(), g.nodes().end(), [](const DisplayNode& n) {
        return n.kind == NodeKind::Instance;
    });
    CHECK(instances == 2); // ha1, ha2

    for (const DisplayNode& node : g.nodes()) {
        for (const DisplayPin& pin : g.pins_of(node)) {
            if (node.kind == NodeKind::InputPort)
                CHECK(pin.side == PinSide::Right);
            if (node.kind == NodeKind::OutputPort)
                CHECK(pin.side == PinSide::Left);
            // Cell pins: outputs on the right. (A port's pin direction is the
            // port's; an output port's pin is on its left, as it is a load.)
            bool cell = node.kind == NodeKind::Cell || node.kind == NodeKind::Instance;
            if (cell && pin.direction == Direction::Output)
                CHECK(pin.side == PinSide::Right);
        }
        CHECK(node.left_pins + node.right_pins == node.pin_count);
    }

    // Every net connects at least two pins; drivers are right-side pins.
    for (const DisplayNet& net : g.nets()) {
        CHECK(net.driver_count + net.load_count >= 2);
        CHECK(net.bit_count >= 1);
        for (DisplayPinId p : g.drivers(net))
            CHECK(g.pin(p).side == PinSide::Right);
        for (DisplayPinId p : g.loads(net))
            CHECK(g.pin(p).side == PinSide::Left);
    }

    // Input port "a" drives ha1's pin a.
    bool found = false;
    for (const DisplayNet& net : g.nets())
        found |= net_pins(d, g, net) == std::set<std::string>{"a", "ha1.a"};
    CHECK(found);
}

TEST_CASE("display graph: bits with the same endpoints bundle into one net") {
    Design d;
    auto n = [&](const char* t) { return d.names().intern(t); };
    ModuleId id = *d.add_module(n("top"));
    Module& m = d.module(id);
    m.add_signals(8);
    std::vector<SigBit> bus;
    for (uint32_t i = 0; i < 8; i++)
        bus.push_back(sig(i));
    NewPin out{n("Y"), Direction::Output, bus};
    NewPin in{n("A"), Direction::Input, bus};
    m.add_cell(n("X"), n("SRC"), std::span(&out, 1));
    m.add_cell(n("Z"), n("SNK"), std::span(&in, 1));

    DisplayGraph g = DisplayGraph::build(d, id);
    REQUIRE(g.nets().size() == 1);
    CHECK(g.nets()[0].bit_count == 8);
    CHECK(net_pins(d, g, g.nets()[0]) == std::set<std::string>{"X.Y", "Z.A"});
}

TEST_CASE("display graph: partially overlapping buses never merge") {
    Design d = overlap_design();
    DisplayGraph g = DisplayGraph::build(d, ModuleId(0));
    REQUIRE(g.nets().size() == 3);
    std::set<std::set<std::string>> nets;
    for (const DisplayNet& net : g.nets()) {
        CHECK(net.bit_count == 1);
        nets.insert(net_pins(d, g, net));
    }
    // No net may claim that Q and R are connected through s0 or s2.
    CHECK(nets ==
          std::set<std::set<std::string>>{{"P.Y", "Q.A"}, {"P.Y", "Q.A", "R.A"}, {"P.Y", "R.A"}});
}

TEST_CASE("display graph: constant-tied pins get a label, not a net") {
    Design d;
    auto n = [&](const char* t) { return d.names().intern(t); };
    ModuleId id = *d.add_module(n("top"));
    Module& m = d.module(id);
    SigBit consts[] = {SigBit::constant(BitState::One), SigBit::constant(BitState::Zero)};
    NewPin pin{n("A"), Direction::Input, consts};
    m.add_cell(n("u"), n("SNK"), std::span(&pin, 1));

    DisplayGraph g = DisplayGraph::build(d, id);
    CHECK(g.nets().empty());
    CHECK(g.pins()[0].constant == "2'b01"); // MSB first: bit 1 = 0, bit 0 = 1
}
