#pragma once

// Shapes derived from a routed net, for drawing: rounded corners, and the
// direction signals flow along each piece of wire (for arrows and animation).

#include "netviz/layout/layout.h"

#include <array>
#include <span>
#include <vector>

namespace netviz {

// A bend where exactly two segments of a net meet at a right angle (and no
// junction): drawn as a quarter circle between `from` and `to`.
struct Corner {
    Vec2 center;
    float radius = 0.0f;
    Vec2 from, to; // where the arc meets the two shortened segments
};

struct RoundedRoute {
    std::vector<Corner> corners;
    // Per segment of the route: how much to shorten it at `a` and at `b`.
    std::vector<std::array<float, 2>> trim;
};

// The corners of a route rounded with `radius` (less at short segments).
RoundedRoute round_corners(const NetRoute& route, float radius);

// A straight piece of wire, oriented the way the signal flows.
struct FlowEdge {
    Vec2 from, to;
    float distance = 0.0f; // wire length from the nearest source to `from`
};

// Splits the route's segments where other segments touch them, and orients
// each piece away from the nearest of `sources` (the net's driver pins).
// Pieces no source reaches keep their stored direction, at distance 0.
std::vector<FlowEdge> flow_edges(const NetRoute& route, std::span<const Vec2> sources);

} // namespace netviz
