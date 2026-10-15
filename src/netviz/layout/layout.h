#pragma once

// The layout contract: what any layout engine produces for a DisplayGraph,
// independent of the algorithm. Drawing code depends only on this.

#include "netviz/view/camera.h" // Vec2, Rect
#include "netviz/view/display_graph.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace netviz {

struct Segment {
    Vec2 a;
    Vec2 b;
};

// Sizes in world units (1 unit = 1 pixel at 100% zoom).
//
// Everything is placed on a grid: node corners, pins, wire tracks and bends
// all land on multiples of `grid`. For that, line_height, node_gap and
// track_pitch must be multiples of the grid and channel_gap a multiple of
// twice the grid (tracks are centred in their channel).
struct LayoutOptions {
    float grid = 10.0f;
    float char_width = 7.0f;   // estimated width of one character of label text
    float line_height = 20.0f; // text line height; also the pin spacing
    float node_padding = 6.0f; // inside node boxes (sizes are then rounded up to the grid)
    float node_gap = 20.0f;    // vertical space between nodes in a layer
    float channel_gap = 40.0f; // minimum horizontal space between layers
    float track_pitch = 10.0f; // spacing of parallel wire tracks
    // Graphs with more display nodes are not laid out (status TooLarge).
    // A temporary guard until large-module work (see docs/LAYOUT.md).
    size_t max_nodes = 50000;
};

enum class LayoutStage : uint8_t { Ranking, Ordering, Placing, Routing };

// Written by the layout thread, readable from any thread.
struct LayoutProgress {
    std::atomic<LayoutStage> stage{LayoutStage::Ranking};
};

struct NetRoute {
    std::vector<Segment> segments; // axis-aligned
    std::vector<Vec2> junctions;   // where the net branches (drawn as dots)
};

struct LayoutResult {
    enum class Status : uint8_t { Ok, TooLarge, Cancelled };

    Status status = Status::Ok;
    std::vector<Rect> nodes;    // per DisplayNodeId
    std::vector<Vec2> pins;     // per DisplayPinId: where wires attach
    std::vector<NetRoute> nets; // per DisplayNetId
    Rect bounds;                // everything drawn
};

} // namespace netviz
