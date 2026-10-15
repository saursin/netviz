#include "netviz/core/connectivity.h"
#include "netviz/core/netlist.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <sstream>
#include <vector>

using namespace netviz;

namespace {

SigBit sig(uint32_t index) {
    return SigBit::signal(SignalBitId(index));
}

// Builds the full adder from .bak/ex/fulladder: two half_adder instances and an
// OR gate, plus a hidden alias wire. Library gates carry pin directions (as when
// Yosys writes port_directions); half_adder instances do not, so they need
// Design::resolve_pin_directions().
struct FullAdder {
    Design design;
    ModuleId half_adder;
    ModuleId full_adder;

    NameId n(const char* text) { return design.names().intern(text); }

    FullAdder() {
        // module half_adder(input a, b, output sum, carry)
        half_adder = *design.add_module(n("half_adder"));
        {
            Module& m = design.module(half_adder);
            m.add_signals(4); // a=s0 b=s1 sum=s2 carry=s3
            const char* ports[] = {"a", "b", "sum", "carry"};
            Direction dirs[] = {Direction::Input, Direction::Input, Direction::Output,
                                Direction::Output};
            for (uint32_t i = 0; i < 4; i++) {
                SigBit bit = sig(i);
                m.make_port(*m.add_wire(n(ports[i]), std::span(&bit, 1)), dirs[i]);
            }
            SigBit a = sig(0), b = sig(1), sum = sig(2), carry = sig(3);
            NewPin xor_pins[] = {{n("A"), Direction::Input, std::span(&a, 1)},
                                 {n("B"), Direction::Input, std::span(&b, 1)},
                                 {n("Y"), Direction::Output, std::span(&sum, 1)}};
            NewPin and_pins[] = {{n("A"), Direction::Input, std::span(&a, 1)},
                                 {n("B"), Direction::Input, std::span(&b, 1)},
                                 {n("Y"), Direction::Output, std::span(&carry, 1)}};
            m.add_cell(n("x1"), n("XOR"), xor_pins);
            m.add_cell(n("a1"), n("AND"), and_pins);
        }

        // module full_adder(input a, b, cin, output sum, cout)
        full_adder = *design.add_module(n("full_adder"));
        {
            Module& m = design.module(full_adder);
            m.add_signals(8); // a b cin sum cout s1 c1 c2 = s0..s7
            const char* ports[] = {"a", "b", "cin", "sum", "cout"};
            Direction dirs[] = {Direction::Input, Direction::Input, Direction::Input,
                                Direction::Output, Direction::Output};
            for (uint32_t i = 0; i < 5; i++) {
                SigBit bit = sig(i);
                m.make_port(*m.add_wire(n(ports[i]), std::span(&bit, 1)), dirs[i]);
            }
            SigBit s1 = sig(5), c1 = sig(6), c2 = sig(7);
            m.add_wire(n("s1"), std::span(&s1, 1));
            m.add_wire(n("c1"), std::span(&c1, 1));
            m.add_wire(n("c2"), std::span(&c2, 1));
            m.add_wire(n("$abc$84$c1"), std::span(&c1, 1), /*hidden=*/true);

            SigBit a = sig(0), b = sig(1), cin = sig(2), sum = sig(3), cout = sig(4);
            NewPin ha1[] = {{n("a"), Direction::Unknown, std::span(&a, 1)},
                            {n("b"), Direction::Unknown, std::span(&b, 1)},
                            {n("sum"), Direction::Unknown, std::span(&s1, 1)},
                            {n("carry"), Direction::Unknown, std::span(&c1, 1)}};
            NewPin ha2[] = {{n("a"), Direction::Unknown, std::span(&s1, 1)},
                            {n("b"), Direction::Unknown, std::span(&cin, 1)},
                            {n("sum"), Direction::Unknown, std::span(&sum, 1)},
                            {n("carry"), Direction::Unknown, std::span(&c2, 1)}};
            NewPin or1[] = {{n("A"), Direction::Input, std::span(&c1, 1)},
                            {n("B"), Direction::Input, std::span(&c2, 1)},
                            {n("Y"), Direction::Output, std::span(&cout, 1)}};
            m.add_cell(n("ha1"), n("half_adder"), ha1);
            m.add_cell(n("ha2"), n("half_adder"), ha2);
            m.add_cell(n("or1"), n("OR"), or1);
        }
        design.set_top(full_adder);
    }
};

bool has_issue(const std::vector<Issue>& issues, Issue::Severity severity,
               const std::string& text) {
    return std::any_of(issues.begin(), issues.end(), [&](const Issue& issue) {
        return issue.severity == severity && issue.message.find(text) != std::string::npos;
    });
}

} // namespace

// ---------------------------------------------------------------------------
// Basic types
// ---------------------------------------------------------------------------

TEST_CASE("SigBit stores signal bits and constants") {
    SigBit s = sig(5);
    CHECK_FALSE(s.is_constant());
    CHECK(s.signal() == SignalBitId(5));

    SigBit x = SigBit::constant(BitState::X);
    CHECK(x.is_constant());
    CHECK(x.constant() == BitState::X);

    CHECK(s != x);
    CHECK(SigBit::constant(BitState::Zero) != SigBit::constant(BitState::One));
    CHECK(sig(kMaxSignals - 1).signal() == SignalBitId(kMaxSignals - 1));
}

TEST_CASE("StringPool interns each string once") {
    StringPool pool;
    NameId a = pool.intern("a");
    CHECK(pool.intern("a") == a);
    CHECK(pool.intern("b") != a);
    CHECK(pool.str(a) == "a");
    CHECK(pool.size() == 2);
}

TEST_CASE("as_int reads MSB-first 0/1 bit strings only") {
    auto bits = [](std::string text) { return Value{Value::Kind::Bits, std::move(text)}; };
    CHECK(as_int(bits("001101")) == 13u);
    CHECK(as_int(bits(std::string(64, '1'))) == UINT64_MAX);
    CHECK_FALSE(as_int(bits("1x0")).has_value());
    CHECK_FALSE(as_int(bits("z")).has_value());
    CHECK_FALSE(as_int(bits(std::string(64, '0') + "1")).has_value()); // 65 chars
    CHECK_FALSE(as_int(bits("")).has_value());
    CHECK_FALSE(as_int(Value{Value::Kind::String, "12"}).has_value());
}

// ---------------------------------------------------------------------------
// Building and reading
// ---------------------------------------------------------------------------

TEST_CASE("modules expose wires, ports, cells and their bits") {
    FullAdder fa;
    const Module& m = fa.design.module(fa.full_adder);

    CHECK(m.signal_count() == 8);
    CHECK(m.wires().size() == 9);
    REQUIRE(m.ports().size() == 5);

    const Wire& cout = m.wire(m.ports()[4]);
    CHECK(fa.design.names().str(cout.name) == "cout");
    CHECK(cout.port_index == 4);
    CHECK(cout.direction == Direction::Output);
    CHECK(m.bits(cout)[0] == sig(4));

    const Wire& c1 = m.wire(*m.find_wire(fa.n("c1")));
    CHECK(c1.port_index == -1);
    CHECK(m.wire(*m.find_wire(fa.n("$abc$84$c1"))).hidden);

    const Cell& ha1 = m.cell(*m.find_cell(fa.n("ha1")));
    CHECK(ha1.type == fa.n("half_adder"));
    REQUIRE(m.pins(ha1).size() == 4);
    CHECK(m.pins(ha1)[3].port == fa.n("carry"));
    CHECK(m.bits(m.pins(ha1)[3])[0] == sig(6));

    CHECK_FALSE(m.find_wire(fa.n("nope")).has_value());
    CHECK(fa.design.find_module(fa.n("half_adder")) == fa.half_adder);
    CHECK(fa.design.top() == fa.full_adder);
}

TEST_CASE("duplicate names are rejected by the builder") {
    FullAdder fa;
    CHECK_FALSE(fa.design.add_module(fa.n("half_adder")).has_value());

    Module& m = fa.design.module(fa.full_adder);
    SigBit bit = sig(0);
    CHECK_FALSE(m.add_wire(fa.n("c1"), std::span(&bit, 1)).has_value());
    CHECK_FALSE(m.add_cell(fa.n("or1"), fa.n("OR"), {}).has_value());
    // Wires and cells are separate name spaces.
    CHECK(m.add_cell(fa.n("c1"), fa.n("BUF"), {}).has_value());
}

TEST_CASE("metadata can be added after construction") {
    FullAdder fa;
    Module& m = fa.design.module(fa.full_adder);
    CellId or1 = *m.find_cell(fa.n("or1"));
    m.cell_attributes(or1).push_back({fa.n("src"), {Value::Kind::String, "fa.v:12"}});
    m.cell_parameters(or1).push_back({fa.n("WIDTH"), {Value::Kind::Bits, "01"}});
    CHECK(m.cell(or1).attributes.size() == 1);
    CHECK(as_int(m.cell(or1).parameters[0].value) == 1u);
}

TEST_CASE("wires and cells share NamedObject fields and helpers") {
    FullAdder fa;
    Module& m = fa.design.module(fa.full_adder);
    WireId c1 = *m.find_wire(fa.n("c1"));
    CellId or1 = *m.find_cell(fa.n("or1"));
    m.wire_attributes(c1).push_back({fa.n("src"), {Value::Kind::String, "fa.v:7"}});
    m.cell_attributes(or1).push_back({fa.n("src"), {Value::Kind::String, "fa.v:12"}});

    // One function works on any named object through the common base.
    auto src_of = [&](const NamedObject& obj) {
        const Value* v = obj.attribute(fa.n("src"));
        return v ? v->text : std::string("-");
    };
    CHECK(src_of(m.wire(c1)) == "fa.v:7");
    CHECK(src_of(m.cell(or1)) == "fa.v:12");
    CHECK(src_of(m.wire(*m.find_wire(fa.n("a")))) == "-");
    CHECK(m.cell(or1).attribute(fa.n("nope")) == nullptr);
}

TEST_CASE("pin directions are resolved from module definitions") {
    FullAdder fa;
    const Module& m = fa.design.module(fa.full_adder);
    const Cell& ha1 = m.cell(*m.find_cell(fa.n("ha1")));
    CHECK(m.pins(ha1)[0].direction == Direction::Unknown);

    CHECK(fa.design.resolve_pin_directions() == 0);
    CHECK(m.pins(ha1)[0].direction == Direction::Input);  // a
    CHECK(m.pins(ha1)[2].direction == Direction::Output); // sum
}

TEST_CASE("unresolvable pin directions are counted") {
    FullAdder fa;
    Module& m = fa.design.module(fa.full_adder);
    SigBit bit = sig(0);
    NewPin pin{fa.n("A"), Direction::Unknown, std::span(&bit, 1)};
    m.add_cell(fa.n("blackbox1"), fa.n("MYSTERY"), std::span(&pin, 1));
    CHECK(fa.design.resolve_pin_directions() == 1);
}

// ---------------------------------------------------------------------------
// Connectivity
// ---------------------------------------------------------------------------

TEST_CASE("connectivity finds drivers, loads and names per bit") {
    FullAdder fa;
    fa.design.resolve_pin_directions();
    const Module& m = fa.design.module(fa.full_adder);
    Connectivity conn(m);

    CellId ha1 = *m.find_cell(fa.n("ha1"));
    CellId or1 = *m.find_cell(fa.n("or1"));
    WireId a = *m.find_wire(fa.n("a"));
    WireId cout = *m.find_wire(fa.n("cout"));

    // c1 (s6): driven by ha1.carry, loaded by or1.A, named c1 and $abc$84$c1.
    SignalBitId c1(6);
    REQUIRE(conn.drivers(c1).size() == 1);
    CHECK(conn.drivers(c1)[0] == Endpoint::cell_pin(ha1, 3, 0));
    REQUIRE(conn.loads(c1).size() == 1);
    CHECK(conn.loads(c1)[0] == Endpoint::cell_pin(or1, 0, 0));
    CHECK(conn.unknown(c1).empty());
    REQUIRE(conn.wire_bits(c1).size() == 2);
    CHECK(conn.wire_bits(c1)[0] == WireBit{*m.find_wire(fa.n("c1")), 0});
    CHECK(conn.wire_bits(c1)[1] == WireBit{*m.find_wire(fa.n("$abc$84$c1")), 0});

    // Module input a (s0) drives; module output cout (s4) loads.
    REQUIRE(conn.drivers(SignalBitId(0)).size() == 1);
    CHECK(conn.drivers(SignalBitId(0))[0].kind() == Endpoint::Kind::ModulePort);
    CHECK(conn.drivers(SignalBitId(0))[0].port() == a);
    CHECK(conn.loads(SignalBitId(4))[0] == Endpoint::module_port(cout, 0));
    CHECK(conn.drivers(SignalBitId(4))[0] == Endpoint::cell_pin(or1, 2, 0));
}

TEST_CASE("connectivity handles inout, unknown directions and constants") {
    Design design;
    auto n = [&](const char* text) { return design.names().intern(text); };
    Module& m = design.module(*design.add_module(n("pads")));
    m.add_signals(2);

    SigBit pad = sig(0), other = sig(1), one = SigBit::constant(BitState::One);
    m.make_port(*m.add_wire(n("pad"), std::span(&pad, 1)), Direction::InOut);
    NewPin io[] = {{n("IO"), Direction::InOut, std::span(&pad, 1)},
                   {n("EN"), Direction::Input, std::span(&one, 1)},
                   {n("X"), Direction::Unknown, std::span(&other, 1)}};
    CellId cell = *m.add_cell(n("iobuf"), n("IOBUF"), io);

    Connectivity conn(m);
    // The inout port and inout pin are both drivers and loads.
    CHECK(conn.drivers(SignalBitId(0)).size() == 2);
    CHECK(conn.loads(SignalBitId(0)).size() == 2);
    // Unknown direction goes only to unknown().
    REQUIRE(conn.unknown(SignalBitId(1)).size() == 1);
    CHECK(conn.unknown(SignalBitId(1))[0] == Endpoint::cell_pin(cell, 2, 0));
    CHECK(conn.drivers(SignalBitId(1)).empty());
    CHECK(conn.wire_bits(SignalBitId(1)).empty()); // unnamed bit
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

TEST_CASE("a well-formed design has no issues") {
    FullAdder fa;
    fa.design.resolve_pin_directions();
    CHECK(validate(fa.design).empty());
}

TEST_CASE("validate reports instance pins that do not match the module") {
    FullAdder fa;
    Module& m = fa.design.module(fa.full_adder);
    SigBit bits[] = {sig(0), sig(1)};
    NewPin bad[] = {{fa.n("bogus"), Direction::Input, std::span(bits, 1)},
                    {fa.n("a"), Direction::Input, std::span(bits, 2)}};
    m.add_cell(fa.n("ha3"), fa.n("half_adder"), bad);
    fa.design.resolve_pin_directions();

    std::vector<Issue> issues = validate(fa.design);
    CHECK(has_issue(issues, Issue::Severity::Error, "has pin bogus"));
    CHECK(has_issue(issues, Issue::Severity::Error, "pin a is 2 bit(s) wide"));
}

TEST_CASE("validate summarizes driver problems and unknown directions") {
    FullAdder fa;
    Module& m = fa.design.module(fa.full_adder);
    SigBit c1 = sig(6), spare = sig(7);
    // A second driver on c1, and two cells of an unknown type.
    NewPin drv{fa.n("Y"), Direction::Output, std::span(&c1, 1)};
    m.add_cell(fa.n("extra"), fa.n("BUF"), std::span(&drv, 1));
    NewPin u{fa.n("A"), Direction::Unknown, std::span(&spare, 1)};
    m.add_cell(fa.n("bb1"), fa.n("MYSTERY"), std::span(&u, 1));
    m.add_cell(fa.n("bb2"), fa.n("MYSTERY"), std::span(&u, 1));
    fa.design.resolve_pin_directions();

    std::vector<Issue> issues = validate(fa.design);
    CHECK(has_issue(issues, Issue::Severity::Warning,
                    "1 signal bit(s) have multiple drivers: s6 (c1)"));
    // One summary line for the cell type, not one per pin.
    CHECK(has_issue(issues, Issue::Severity::Warning,
                    "2 pin(s) of cell type MYSTERY have unknown direction"));
    CHECK(std::count_if(issues.begin(), issues.end(), [](const Issue& i) {
              return i.message.find("MYSTERY") != std::string::npos;
          }) == 1);
}

TEST_CASE("validate reports loaded bits without a driver") {
    Design design;
    auto n = [&](const char* text) { return design.names().intern(text); };
    Module& m = design.module(*design.add_module(n("top")));
    m.add_signals(1);
    SigBit floating = sig(0);
    m.add_wire(n("floating"), std::span(&floating, 1));
    NewPin in{n("A"), Direction::Input, std::span(&floating, 1)};
    m.add_cell(n("u1"), n("BUF"), std::span(&in, 1));

    CHECK(has_issue(validate(design), Issue::Severity::Warning,
                    "1 signal bit(s) are loaded but not driven: s0 (floating)"));
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------

TEST_CASE("dump writes a deterministic description") {
    Design design;
    auto n = [&](const char* text) { return design.names().intern(text); };
    ModuleId id = *design.add_module(n("inv"));
    Module& m = design.module(id);
    m.add_signals(2);
    m.attributes().push_back({n("src"), {Value::Kind::String, "inv.v:1"}});

    SigBit in = sig(0), out = sig(1), zero = SigBit::constant(BitState::Zero);
    WireId a = *m.add_wire(n("a"), std::span(&in, 1));
    m.make_port(a, Direction::Input);
    m.make_port(*m.add_wire(n("y"), std::span(&out, 1)), Direction::Output);
    SigBit bus[] = {out, zero};
    m.add_wire(n("$tmp"), bus, /*hidden=*/true);

    NewPin pins[] = {{n("A"), Direction::Input, std::span(&in, 1)},
                     {n("Y"), Direction::Output, std::span(&out, 1)}};
    CellId u1 = *m.add_cell(n("u1"), n("NOT"), pins);
    m.cell_parameters(u1).push_back({n("DRIVE"), {Value::Kind::Bits, "10"}});
    design.set_top(id);

    std::ostringstream out_text;
    dump(design, out_text);
    CHECK(out_text.str() == "module inv (top)\n"
                            "  signals 2\n"
                            "  attribute src = \"inv.v:1\"\n"
                            "  wire a [1] port0 input: s0\n"
                            "  wire y [1] port1 output: s1\n"
                            "  wire $tmp [2] hidden: s1 0\n"
                            "  cell u1 NOT\n"
                            "    parameter DRIVE = 10\n"
                            "    A input: s0\n"
                            "    Y output: s1\n");
}
