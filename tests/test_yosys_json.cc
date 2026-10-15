#include "netviz/io/yosys_json.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <sstream>

using namespace netviz;

namespace {

// Small helpers to look things up by name in an imported design.
struct Lookup {
    const Design& d;

    NameId n(const char* text) const { return *d.names().find(text); }
    const Module& module(const char* name) const { return d.module(*d.find_module(n(name))); }
    const Wire& wire(const Module& m, const char* name) const {
        return m.wire(*m.find_wire(n(name)));
    }
    const Cell& cell(const Module& m, const char* name) const {
        return m.cell(*m.find_cell(n(name)));
    }
    const Value* attr(const NamedObject& obj, const char* name) const {
        return obj.attribute(n(name));
    }
};

bool has_issue(const ImportResult& r, const std::string& text) {
    return std::any_of(r.issues.begin(), r.issues.end(),
                       [&](const Issue& i) { return i.message.find(text) != std::string::npos; });
}

// A trimmed-down version of what Yosys writes for .bak/ex/fulladder after
// mapping: half_adder is instantiated twice; NOR/NOT have no port_directions.
const char* const kFullAdder = R"({
  "creator": "Yosys",
  "modules": {
    "full_adder": {
      "attributes": { "top": "00000000000000000000000000000001", "src": "fa.v:11.1-22.10" },
      "ports": {
        "a":   { "direction": "input",  "bits": [ 2 ] },
        "b":   { "direction": "input",  "bits": [ 3 ] },
        "cin": { "direction": "input",  "bits": [ 4 ] },
        "sum": { "direction": "output", "bits": [ 5 ] },
        "cout":{ "direction": "output", "bits": [ 6 ] }
      },
      "cells": {
        "ha1": { "hide_name": 0, "type": "half_adder", "parameters": {}, "attributes": {},
                 "port_directions": { "a": "input", "b": "input", "sum": "output", "carry": "output" },
                 "connections": { "a": [ 2 ], "b": [ 3 ], "sum": [ 7 ], "carry": [ 8 ] } },
        "ha2": { "hide_name": 0, "type": "half_adder",
                 "connections": { "a": [ 7 ], "b": [ 4 ], "sum": [ 5 ], "carry": [ 9 ] } },
        "$abc$1$or": { "hide_name": 1, "type": "NOR",
                 "connections": { "A": [ 8 ], "B": [ 9 ], "Y": [ 10 ] } },
        "$abc$1$inv": { "hide_name": 1, "type": "NOT",
                 "connections": { "A": [ 10 ], "Y": [ 6 ] } }
      },
      "netnames": {
        "a":   { "hide_name": 0, "bits": [ 2 ], "attributes": { "src": "fa.v:12" } },
        "c1":  { "hide_name": 0, "bits": [ 8 ], "attributes": {} },
        "$abc$1$c1": { "hide_name": 1, "bits": [ 8 ], "attributes": {} },
        "s1":  { "hide_name": 0, "bits": [ 7 ], "attributes": {} }
      }
    },
    "half_adder": {
      "attributes": { "src": "fa.v:1.1-9.10" },
      "ports": {
        "a":     { "direction": "input",  "bits": [ 2 ] },
        "b":     { "direction": "input",  "bits": [ 3 ] },
        "sum":   { "direction": "output", "bits": [ 4 ] },
        "carry": { "direction": "output", "bits": [ 5 ] }
      },
      "cells": {
        "x": { "hide_name": 1, "type": "XOR",
               "port_directions": { "A": "input", "B": "input", "Y": "output" },
               "connections": { "A": [ 2 ], "B": [ 3 ], "Y": [ 4 ] } },
        "n": { "hide_name": 1, "type": "AND",
               "port_directions": { "A": "input", "B": "input", "Y": "output" },
               "connections": { "A": [ 2 ], "B": [ 3 ], "Y": [ 5 ] } }
      },
      "netnames": {}
    }
  }
})";

} // namespace

TEST_CASE("yosys json: modules, ports and wires") {
    ImportResult r = import_yosys_json_text(kFullAdder);
    REQUIRE_MESSAGE(r.design, r.error);
    Lookup l{*r.design};

    REQUIRE(r.design->modules().size() == 2);
    REQUIRE(r.design->top().has_value());
    const Module& fa = l.module("full_adder");
    CHECK(r.design->module(*r.design->top()).name() == fa.name());

    // Port order follows the file.
    REQUIRE(fa.ports().size() == 5);
    CHECK(r.design->names().str(fa.wire(fa.ports()[2]).name) == "cin");
    CHECK(fa.wire(fa.ports()[4]).direction == Direction::Output);

    // The netname "a" is the same wire as port "a"; its attributes are merged.
    const Wire& a = l.wire(fa, "a");
    CHECK(a.port_index == 0);
    REQUIRE(l.attr(a, "src"));
    CHECK(l.attr(a, "src")->text == "fa.v:12");

    // Aliases share a signal bit; hide_name maps to hidden.
    const Wire& c1 = l.wire(fa, "c1");
    const Wire& alias = l.wire(fa, "$abc$1$c1");
    CHECK(fa.bits(c1)[0] == fa.bits(alias)[0]);
    CHECK(alias.hidden);
    CHECK_FALSE(c1.hidden);

    // Bit numbers 2..10 become 9 signal bits.
    CHECK(fa.signal_count() == 9);
}

TEST_CASE("yosys json: cells, pin directions and resolution") {
    ImportResult r = import_yosys_json_text(kFullAdder);
    REQUIRE(r.design);
    Lookup l{*r.design};
    const Module& fa = l.module("full_adder");

    // From port_directions.
    CHECK(fa.pins(l.cell(fa, "ha1"))[3].direction == Direction::Output);
    // ha2 has no port_directions: resolved from the half_adder module.
    CHECK(fa.pins(l.cell(fa, "ha2"))[0].direction == Direction::Input);
    CHECK(fa.pins(l.cell(fa, "ha2"))[2].direction == Direction::Output);
    // NOR/NOT are library cells without directions: stay Unknown and are reported.
    const Cell& nor = l.cell(fa, "$abc$1$or");
    CHECK(nor.hidden);
    CHECK(fa.pins(nor)[2].direction == Direction::Unknown);
    CHECK(has_issue(r, "pin(s) of cell type NOR have unknown direction"));
    CHECK(has_issue(r, "pin(s) of cell type NOT have unknown direction"));
}

TEST_CASE("yosys json: bus numbering and constants") {
    ImportResult r = import_yosys_json_text(R"({"modules": {"m": {
        "ports": {
          "d": { "direction": "input", "bits": [ 2, 3, 4, 5 ], "offset": 4 },
          "u": { "direction": "output", "bits": [ 6, 7 ], "upto": 1, "signed": 1 }
        },
        "cells": {
          "c": { "type": "BUF", "port_directions": { "A": "input", "Y": "output" },
                 "connections": { "A": [ "0", "1", "x", "z", 2 ], "Y": [ 6 ] } }
        },
        "netnames": { "d_hi": { "hide_name": 0, "bits": [ 5 ], "offset": 7 } }
    }}})");
    REQUIRE_MESSAGE(r.design, r.error);
    Lookup l{*r.design};
    const Module& m = l.module("m");

    const Wire& d = l.wire(m, "d");
    CHECK(d.start_offset == 4);
    CHECK(bit_index(d, 3) == 7);
    const Wire& u = l.wire(m, "u");
    CHECK(u.upto);
    CHECK(u.is_signed);
    CHECK(bit_index(u, 0) == 1);
    CHECK(bit_index(l.wire(m, "d_hi"), 0) == 7);

    std::span<const SigBit> a = m.bits(m.pins(l.cell(m, "c"))[0]);
    REQUIRE(a.size() == 5);
    CHECK(a[0] == SigBit::constant(BitState::Zero));
    CHECK(a[1] == SigBit::constant(BitState::One));
    CHECK(a[2] == SigBit::constant(BitState::X));
    CHECK(a[3] == SigBit::constant(BitState::Z));
    CHECK(a[4] == m.bits(d)[0]);
}

TEST_CASE("yosys json: parameter and attribute values") {
    ImportResult r = import_yosys_json_text(R"({"modules": {"m": {
        "attributes": { "bits": "01x", "text": "hello", "bitlike_string": "0101 ", "num": 5,
                        "neg": -1 },
        "ports": {}, "cells": {}, "netnames": {}
    }}})");
    REQUIRE_MESSAGE(r.design, r.error);
    Lookup l{*r.design};
    const Module& m = l.module("m");
    auto attr = [&](const char* name) -> const Value& {
        for (const NamedValue& nv : m.attributes())
            if (nv.name == l.n(name))
                return nv.value;
        FAIL("missing attribute");
        static Value none;
        return none;
    };
    CHECK(attr("bits").kind == Value::Kind::Bits);
    CHECK(attr("bits").text == "01x");
    CHECK(attr("text").kind == Value::Kind::String);
    // A string that looks like bits is marked by Yosys with a trailing space.
    CHECK(attr("bitlike_string").kind == Value::Kind::String);
    CHECK(attr("bitlike_string").text == "0101");
    CHECK(as_int(attr("num")) == 5u);
    CHECK(attr("num").text.size() == 32);
    CHECK(attr("neg").text == std::string(32, '1'));
}

TEST_CASE("yosys json: top module detection") {
    SUBCASE("inferred: the only module nobody instantiates") {
        ImportResult r = import_yosys_json_text(R"({"modules": {
            "leaf": { "ports": {}, "cells": {}, "netnames": {} },
            "top":  { "ports": {}, "netnames": {},
                      "cells": { "u": { "type": "leaf", "connections": {} } } }
        }})");
        REQUIRE(r.design);
        REQUIRE(r.design->top());
        CHECK(r.design->names().str(r.design->module(*r.design->top()).name()) == "top");
    }
    SUBCASE("blackboxes are never top") {
        ImportResult r = import_yosys_json_text(R"({"modules": {
            "lib": { "attributes": { "blackbox": "00000000000000000000000000000001" },
                     "ports": {}, "cells": {}, "netnames": {} },
            "top": { "ports": {}, "cells": {}, "netnames": {} }
        }})");
        REQUIRE(r.design);
        REQUIRE(r.design->top());
        CHECK(r.design->names().str(r.design->module(*r.design->top()).name()) == "top");
        CHECK(
            r.design->module(*r.design->find_module(*r.design->names().find("lib"))).is_blackbox());
        CHECK_FALSE(r.design->module(*r.design->top()).is_blackbox());
    }
    SUBCASE("ambiguous: no top, with a warning") {
        ImportResult r = import_yosys_json_text(R"({"modules": {
            "a": { "ports": {}, "cells": {}, "netnames": {} },
            "b": { "ports": {}, "cells": {}, "netnames": {} }
        }})");
        REQUIRE(r.design);
        CHECK_FALSE(r.design->top().has_value());
        CHECK(has_issue(r, "2 candidate top modules (a, b)"));
    }
}

TEST_CASE("yosys json: memories are reported, not imported") {
    ImportResult r = import_yosys_json_text(R"({"modules": {"m": {
        "ports": {}, "cells": {}, "netnames": {},
        "memories": { "mem": { "width": 8, "size": 16 } }
    }}})");
    REQUIRE(r.design);
    CHECK(has_issue(r, "module m: 1 memories are not imported"));
}

TEST_CASE("yosys json: malformed input fails with context") {
    CHECK(import_yosys_json_text("{ not json").error.starts_with("invalid JSON"));
    CHECK(import_yosys_json_text(R"({"creator": "x"})").error == "missing 'modules'");

    ImportResult bad_bit = import_yosys_json_text(R"({"modules": {"m": {
        "ports": { "p": { "direction": "input", "bits": [ "q" ] } } }}})");
    CHECK_FALSE(bad_bit.design);
    CHECK(bad_bit.error == "module m: port p: 'bits' has an invalid bit \"q\"");

    ImportResult mismatch = import_yosys_json_text(R"({"modules": {"m": {
        "ports": { "p": { "direction": "input", "bits": [ 2 ] } },
        "netnames": { "p": { "bits": [ 3 ] } } }}})");
    CHECK(mismatch.error == "module m: netname p: bits differ from the port with the same name");

    ImportResult bad_dir = import_yosys_json_text(R"({"modules": {"m": {
        "ports": { "p": { "direction": "sideways", "bits": [ 2 ] } } }}})");
    CHECK(bad_dir.error == "module m: port p: 'direction' has an invalid direction \"sideways\"");

    CHECK(import_yosys_json("/nonexistent/file.json").error.starts_with("invalid JSON"));
}

TEST_CASE("every example in ex/ imports and validates without errors") {
    int count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(NETVIZ_EXAMPLES_DIR)) {
        if (entry.path().extension() != ".json" || entry.path().parent_path().filename() == "scale")
            continue;
        count++;
        CAPTURE(entry.path().string());
        ImportResult r = import_yosys_json(entry.path());
        REQUIRE_MESSAGE(r.design, r.error);
        CHECK(r.design->top().has_value());
        for (const Issue& issue : r.issues) {
            CAPTURE(issue.message);
            CHECK(issue.severity != Issue::Severity::Error);
            MESSAGE(entry.path().filename().string() << ": " << issue.message);
        }
    }
    CHECK(count == 10); // 5 examples x (gate level + word level)
}

TEST_CASE("yosys json: progress reaches the end") {
    ImportProgress progress;
    ImportResult r = import_yosys_json_text(kFullAdder, &progress);
    REQUIRE(r.design);
    CHECK(progress.stage.load() == ImportStage::Checking);
    // 5 + 4 ports, 4 + 0 netnames, 4 + 2 cells
    CHECK(progress.total.load() == 19);
    CHECK(progress.done.load() == 19);
}

TEST_CASE("yosys json: a cancelled import returns no design") {
    ImportProgress progress;
    progress.cancel = true;
    ImportResult r = import_yosys_json_text(kFullAdder, &progress);
    CHECK_FALSE(r.design);
    CHECK(r.cancelled);
}
