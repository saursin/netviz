#pragma once

// A small layered ("left to right") layout for schematics: signal-flow
// ranks, cycle handling via strongly connected components, barycenter
// ordering, simple coordinates and a simple orthogonal router. It is a
// deliberately limited subset of Sugiyama-style layout; see docs/LAYOUT.md.

#include "netviz/layout/layout.h"

#include <stop_token>

namespace netviz {

// `names` is used to estimate label sizes. Deterministic: the same input gives
// the same result. Checks `stop` between stages and periodically within them.
LayoutResult layered_layout(const DisplayGraph& graph, const StringPool& names,
                            const LayoutOptions& options = {}, std::stop_token stop = {},
                            LayoutProgress* progress = nullptr);

} // namespace netviz
