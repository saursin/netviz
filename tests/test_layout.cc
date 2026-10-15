#include "netviz/io/yosys_json.h"
#include "netviz/layout/layered_layout.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace netviz;

namespace {

constexpr float kEps = 1e-3f;

SigBit sig(uint32_t i) {
    return SigBit::signal(SignalBitId(i));
}

bool finite(Vec2 p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}

bool inside(const Rect& outer, Vec2 p) {
    return p.x >= outer.min.x - kEps && p.x <= outer.max.x + kEps && p.y >= outer.min.y - kEps &&
           p.y <= outer.max.y + kEps;
}

bool on_grid(Vec2 p) {
    float grid = LayoutOptions{}.grid;
    auto on = [&](float v) { return std::abs(v / grid - std::round(v / grid)) < kEps; };
    return on(p.x) && on(p.y);
}

bool is_cell(const DisplayNode& n) {
    return n.kind == NodeKind::Cell || n.kind == NodeKind::Instance;
}

// True if two wires of different nets would look connected: collinear and
// overlapping, or one's end lying on the other. (Perpendicular crossings are
// fine; they are drawn without a dot.)
bool look_connected(const Segment& a, const Segment& b) {
    auto box = [](const Segment& s) {
        return Rect{Vec2{std::min(s.a.x, s.b.x), std::min(s.a.y, s.b.y)},
                    Vec2{std::max(s.a.x, s.b.x), std::max(s.a.y, s.b.y)}};
    };
    Rect p = box(a), q = box(b);
    auto on = [](const Rect& r, Vec2 v) {
        return v.x >= r.min.x - kEps && v.x <= r.max.x + kEps && v.y >= r.min.y - kEps &&
               v.y <= r.max.y + kEps;
    };
    bool ph = p.min.y == p.max.y, qh = q.min.y == q.max.y;
    if (ph && qh && p.min.y == q.min.y)
        return p.min.x <= q.max.x && q.min.x <= p.max.x;
    if (!ph && !qh && p.min.x == q.min.x)
        return p.min.y <= q.max.y && q.min.y <= p.max.y;
    return on(p, b.a) || on(p, b.b) || on(q, a.a) || on(q, a.b);
}

// Properties every layout must have (no exact coordinates, so the algorithm
// can improve without rewriting tests).
void check_invariants(const DisplayGraph& g, const LayoutResult& r,
                      std::span<const uint8_t> r_unrouted = {}) {
    REQUIRE(r.status == LayoutResult::Status::Ok);
    REQUIRE(r.nodes.size() == g.nodes().size());
    REQUIRE(r.pins.size() == g.pins().size());
    REQUIRE(r.nets.size() == g.nets().size());

    // Finite, inside bounds, non-empty.
    for (const Rect& box : r.nodes) {
        CHECK(finite(box.min));
        CHECK(finite(box.max));
        CHECK(box.max.x > box.min.x);
        CHECK(box.max.y > box.min.y);
        CHECK(inside(r.bounds, box.min));
        CHECK(inside(r.bounds, box.max));
    }

    // Node corners, pins, wire ends and junctions are all on the grid.
    for (const Rect& box : r.nodes) {
        CHECK(on_grid(box.min));
        CHECK(on_grid(box.max));
    }
    for (Vec2 p : r.pins)
        CHECK(on_grid(p));
    for (const NetRoute& route : r.nets) {
        for (const Segment& s : route.segments) {
            CHECK(on_grid(s.a));
            CHECK(on_grid(s.b));
        }
        for (Vec2 j : route.junctions)
            CHECK(on_grid(j));
    }

    // Wires of different nets never look connected, unless the nets share a
    // pin (bits of one bus pin that go to different places).
    std::vector<std::vector<uint32_t>> net_pins(g.nets().size());
    for (size_t n = 0; n < g.nets().size(); n++) {
        for (DisplayPinId p : g.drivers(g.nets()[n]))
            net_pins[n].push_back(p.index);
        for (DisplayPinId p : g.loads(g.nets()[n]))
            net_pins[n].push_back(p.index);
        std::sort(net_pins[n].begin(), net_pins[n].end());
    }
    auto share_pin = [&](size_t a, size_t b) {
        for (uint32_t p : net_pins[a])
            if (std::binary_search(net_pins[b].begin(), net_pins[b].end(), p))
                return true;
        return false;
    };
    for (size_t a = 0; a < r.nets.size(); a++)
        for (size_t b = a + 1; b < r.nets.size(); b++) {
            if (r.nets[a].segments.empty() || r.nets[b].segments.empty() || share_pin(a, b))
                continue;
            for (const Segment& s : r.nets[a].segments)
                for (const Segment& t : r.nets[b].segments)
                    if (look_connected(s, t)) {
                        CAPTURE(a);
                        CAPTURE(b);
                        FAIL_CHECK("wires of unrelated nets touch");
                    }
        }

    // No two node boxes overlap.
    for (size_t i = 0; i < r.nodes.size(); i++)
        for (size_t j = i + 1; j < r.nodes.size(); j++) {
            const Rect& a = r.nodes[i];
            const Rect& b = r.nodes[j];
            bool overlap = a.min.x < b.max.x - kEps && b.min.x < a.max.x - kEps &&
                           a.min.y < b.max.y - kEps && b.min.y < a.max.y - kEps;
            CHECK_FALSE(overlap);
        }

    // Pins sit on the matching edge of their node.
    for (const DisplayPin& pin : g.pins()) {
        const Rect& box = r.nodes[pin.node.index];
        Vec2 p = r.pins[&pin - g.pins().data()];
        float edge = pin.side == PinSide::Left ? box.min.x : box.max.x;
        CHECK(std::abs(p.x - edge) < kEps);
        CHECK(p.y >= box.min.y - kEps);
        CHECK(p.y <= box.max.y + kEps);
    }

    // Input ports are left of every cell, output ports right of every cell.
    float cells_min = 1e30f, cells_max = -1e30f;
    for (size_t i = 0; i < g.nodes().size(); i++)
        if (is_cell(g.nodes()[i])) {
            cells_min = std::min(cells_min, r.nodes[i].min.x);
            cells_max = std::max(cells_max, r.nodes[i].max.x);
        }
    for (size_t i = 0; i < g.nodes().size(); i++) {
        if (g.nodes()[i].kind == NodeKind::InputPort)
            CHECK(r.nodes[i].max.x <= cells_min + kEps);
        if (g.nodes()[i].kind == NodeKind::OutputPort)
            CHECK(r.nodes[i].min.x >= cells_max - kEps);
    }

    // Every load of a driven net is the end of one of its segments; all
    // segments are axis-aligned and finite.
    for (size_t n = 0; n < g.nets().size(); n++) {
        const DisplayNet& net = g.nets()[n];
        const NetRoute& route = r.nets[n];
        for (const Segment& s : route.segments) {
            CHECK(finite(s.a));
            CHECK(finite(s.b));
            CHECK((std::abs(s.a.x - s.b.x) < kEps || std::abs(s.a.y - s.b.y) < kEps));
        }
        if (net.driver_count == 0 || (n < r_unrouted.size() && r_unrouted[n]))
            continue;
        for (DisplayPinId load : g.loads(net)) {
            if (g.pin(load).node == g.pin(g.drivers(net)[0]).node)
                continue; // a pin looping back into its own node
            Vec2 q = r.pins[load.index];
            bool reached = false;
            for (const Segment& s : route.segments)
                reached |= (std::abs(s.a.x - q.x) < kEps && std::abs(s.a.y - q.y) < kEps) ||
                           (std::abs(s.b.x - q.x) < kEps && std::abs(s.b.y - q.y) < kEps);
            CHECK(reached);
        }
    }
}

bool same_layout(const LayoutResult& a, const LayoutResult& b) {
    if (a.nodes.size() != b.nodes.size() || a.nets.size() != b.nets.size())
        return false;
    for (size_t i = 0; i < a.nodes.size(); i++)
        if (a.nodes[i].min.x != b.nodes[i].min.x || a.nodes[i].min.y != b.nodes[i].min.y)
            return false;
    for (size_t i = 0; i < a.nets.size(); i++)
        if (a.nets[i].segments.size() != b.nets[i].segments.size())
            return false;
    return true;
}

// A ring of `count` cells (Y of each feeds A of the next), with no ports.
void add_ring(Design& d, Module& m, const char* prefix, int count) {
    uint32_t first = m.add_signals(static_cast<uint32_t>(count)).index;
    for (int i = 0; i < count; i++) {
        SigBit in = sig(first + static_cast<uint32_t>(i));
        SigBit out = sig(first + static_cast<uint32_t>((i + 1) % count));
        NewPin pins[] = {{d.names().intern("A"), Direction::Input, std::span(&in, 1)},
                         {d.names().intern("Y"), Direction::Output, std::span(&out, 1)}};
        std::string name = std::string(prefix) + std::to_string(i);
        m.add_cell(d.names().intern(name), d.names().intern("BUF"), pins);
    }
}

LayoutResult layout_of(const Design& d, const DisplayGraph& g) {
    return layered_layout(g, d.names());
}

} // namespace

TEST_CASE("layout: every module of every example satisfies the invariants") {
    int modules = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(NETVIZ_EXAMPLES_DIR)) {
        if (entry.path().extension() != ".json" || entry.path().parent_path().filename() == "scale")
            continue;
        ImportResult r = import_yosys_json(entry.path());
        REQUIRE(r.design);
        for (uint32_t m = 0; m < r.design->modules().size(); m++) {
            ModuleId id(m);
            CAPTURE(entry.path().filename().string());
            CAPTURE(r.design->names().str(r.design->module(id).name()));
            DisplayGraph g = DisplayGraph::build(*r.design, id);
            LayoutResult first = layout_of(*r.design, g);
            check_invariants(g, first);
            CHECK(same_layout(first, layout_of(*r.design, g))); // deterministic
            modules++;
        }
    }
    CHECK(modules > 40);
}

TEST_CASE("layout: a ring A -> B -> C -> A terminates and is valid") {
    Design d;
    ModuleId id = *d.add_module(d.names().intern("ring"));
    add_ring(d, d.module(id), "c", 3);
    DisplayGraph g = DisplayGraph::build(d, id);
    check_invariants(g, layout_of(d, g));
}

TEST_CASE("layout: a cycle not reachable from any input is handled") {
    Design d;
    ModuleId id = *d.add_module(d.names().intern("top"));
    Module& m = d.module(id);
    // input in -> X
    SigBit in = sig(m.add_signals(1).index);
    m.make_port(*m.add_wire(d.names().intern("in"), std::span(&in, 1)), Direction::Input);
    NewPin x{d.names().intern("A"), Direction::Input, std::span(&in, 1)};
    m.add_cell(d.names().intern("X"), d.names().intern("SNK"), std::span(&x, 1));
    // separate A -> B -> A
    add_ring(d, m, "r", 2);
    DisplayGraph g = DisplayGraph::build(d, id);
    check_invariants(g, layout_of(d, g));
}

TEST_CASE("layout: too-large graphs and cancellation") {
    Design d;
    ModuleId id = *d.add_module(d.names().intern("ring"));
    add_ring(d, d.module(id), "c", 5);
    DisplayGraph g = DisplayGraph::build(d, id);

    LayoutOptions small;
    small.max_nodes = 3;
    CHECK(layered_layout(g, d.names(), small).status == LayoutResult::Status::TooLarge);

    std::stop_source stop;
    stop.request_stop();
    CHECK(layered_layout(g, d.names(), {}, stop.get_token()).status ==
          LayoutResult::Status::Cancelled);
}

TEST_CASE("layout: unrouted nets get no wires and do not break the layout") {
    ImportResult imported = import_yosys_json(std::filesystem::path(NETVIZ_EXAMPLES_DIR) /
                                              "03_regfile" / "regfile.json");
    REQUIRE(imported.design);
    const Design& d = *imported.design;
    ModuleId id = *d.top();
    DisplayGraph g = DisplayGraph::build(d, id);
    // Leave every other net unrouted.
    std::vector<uint8_t> unrouted(g.nets().size(), 0);
    for (size_t n = 0; n < unrouted.size(); n += 2)
        unrouted[n] = 1;
    LayoutOptions options;
    options.unrouted_nets = unrouted;
    LayoutResult r = layered_layout(g, d.names(), options);
    check_invariants(g, r, unrouted);
    for (size_t n = 0; n < unrouted.size(); n++)
        if (unrouted[n]) {
            CHECK(r.nets[n].segments.empty());
            CHECK(r.nets[n].junctions.empty());
        }
}
