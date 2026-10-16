#include "netviz/layout/route_shapes.h"

#include <doctest/doctest.h>

using namespace netviz;

namespace {

NetRoute route(std::initializer_list<Segment> segments,
               std::initializer_list<Vec2> junctions = {}) {
    NetRoute r;
    r.segments = segments;
    r.junctions = junctions;
    return r;
}

bool same(Vec2 a, Vec2 b) {
    return a.x == b.x && a.y == b.y;
}

} // namespace

TEST_CASE("route shapes: an L bend gets one rounded corner") {
    // (0,0) -> (20,0) -> (20,30)
    NetRoute r = route({{{0, 0}, {20, 0}}, {{20, 0}, {20, 30}}});
    RoundedRoute rounded = round_corners(r, 4.0f);
    REQUIRE(rounded.corners.size() == 1);
    const Corner& c = rounded.corners[0];
    CHECK(c.radius == 4.0f);
    CHECK(same(c.center, Vec2{16, 4}));
    CHECK(((same(c.from, Vec2{16, 0}) && same(c.to, Vec2{20, 4})) ||
           (same(c.from, Vec2{20, 4}) && same(c.to, Vec2{16, 0}))));
    CHECK(rounded.trim[0][1] == 4.0f); // end b of the horizontal segment
    CHECK(rounded.trim[1][0] == 4.0f); // end a of the vertical segment
    CHECK(rounded.trim[0][0] == 0.0f);
}

TEST_CASE("route shapes: short segments limit the radius; straight lines and Ts are not rounded") {
    NetRoute short_leg = route({{{0, 0}, {4, 0}}, {{4, 0}, {4, 30}}});
    REQUIRE(round_corners(short_leg, 4.0f).corners.size() == 1);
    CHECK(round_corners(short_leg, 4.0f).corners[0].radius == 2.0f);

    NetRoute straight = route({{{0, 0}, {10, 0}}, {{10, 0}, {30, 0}}});
    CHECK(round_corners(straight, 4.0f).corners.empty());

    // A trunk (10,0)-(10,40) with a branch ending on its middle, and one
    // ending at its top: the top is a corner only if no junction is there.
    NetRoute tee = route({{{0, 20}, {10, 20}}, {{10, 0}, {10, 40}}, {{10, 40}, {30, 40}}});
    RoundedRoute rounded = round_corners(tee, 4.0f);
    REQUIRE(rounded.corners.size() == 1); // only at (10,40); (10,20) is a T
    CHECK(same(rounded.corners[0].center, Vec2{14, 36}));

    NetRoute dotted = route({{{0, 0}, {20, 0}}, {{20, 0}, {20, 30}}}, {{20, 0}});
    CHECK(round_corners(dotted, 4.0f).corners.empty());
}

TEST_CASE("route shapes: flow runs from the source through a T to every load") {
    // Source at (0,20); trunk at x=10 from y=0 to y=40; loads at (30,0), (30,40).
    NetRoute tee =
        route({{{0, 20}, {10, 20}}, {{10, 0}, {10, 40}}, {{10, 0}, {30, 0}}, {{10, 40}, {30, 40}}});
    Vec2 source{0, 20};
    std::vector<FlowEdge> edges = flow_edges(tee, std::span(&source, 1));
    REQUIRE(edges.size() == 5); // the trunk splits at y=20
    for (const FlowEdge& e : edges) {
        CAPTURE(e.from.x);
        CAPTURE(e.from.y);
        if (same(e.to, Vec2{30, 0}) || same(e.from, Vec2{30, 0}))
            CHECK(same(e.to, Vec2{30, 0})); // into the load
        if (same(e.to, Vec2{30, 40}) || same(e.from, Vec2{30, 40}))
            CHECK(same(e.to, Vec2{30, 40}));
        if (same(e.from, Vec2{10, 20}) || same(e.to, Vec2{10, 20}))
            CHECK(((same(e.from, Vec2{10, 20}) && e.distance == 10.0f) || same(e.from, source)));
    }
    // Distances grow along the flow.
    for (const FlowEdge& e : edges)
        if (same(e.to, Vec2{30, 40}))
            CHECK(e.distance == 30.0f); // 10 to the trunk + 20 down the trunk
}
