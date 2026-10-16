#include "netviz/layout/route_shapes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <utility>

namespace netviz {

namespace {

using Key = std::pair<float, float>; // a point (coordinates are on the layout grid)

Key key(Vec2 p) {
    return {p.x, p.y};
}

float length(const Segment& s) {
    return std::abs(s.b.x - s.a.x) + std::abs(s.b.y - s.a.y); // axis-aligned
}

Vec2 toward(Vec2 from, Vec2 to) { // unit vector (axis-aligned)
    float dx = to.x - from.x, dy = to.y - from.y;
    float len = std::abs(dx) + std::abs(dy);
    return len > 0.0f ? Vec2{dx / len, dy / len} : Vec2{};
}

} // namespace

RoundedRoute round_corners(const NetRoute& route, float radius) {
    RoundedRoute out;
    out.trim.assign(route.segments.size(), {0.0f, 0.0f});
    std::map<Key, std::vector<std::pair<uint32_t, int>>> ends; // point -> (segment, end)
    for (uint32_t i = 0; i < route.segments.size(); i++) {
        ends[key(route.segments[i].a)].push_back({i, 0});
        ends[key(route.segments[i].b)].push_back({i, 1});
    }
    std::set<Key> junctions;
    for (Vec2 j : route.junctions)
        junctions.insert(key(j));
    // A point inside another segment of the net is a T, not a corner.
    // Segments indexed by their row (horizontal) or column (vertical).
    std::map<float, std::vector<std::pair<float, float>>> rows, columns;
    for (const Segment& seg : route.segments) {
        if (seg.a.y == seg.b.y)
            rows[seg.a.y].push_back({std::min(seg.a.x, seg.b.x), std::max(seg.a.x, seg.b.x)});
        else
            columns[seg.a.x].push_back({std::min(seg.a.y, seg.b.y), std::max(seg.a.y, seg.b.y)});
    }
    auto inside_any = [&](Vec2 p) {
        auto within = [](const auto& index, float line, float v) {
            auto it = index.find(line);
            if (it == index.end())
                return false;
            for (auto [lo, hi] : it->second)
                if (v > lo && v < hi)
                    return true;
            return false;
        };
        return within(rows, p.y, p.x) || within(columns, p.x, p.y);
    };

    for (const auto& [point, list] : ends) {
        if (list.size() != 2 || junctions.contains(point))
            continue;
        const Segment& s1 = route.segments[list[0].first];
        const Segment& s2 = route.segments[list[1].first];
        Vec2 p{point.first, point.second};
        Vec2 o1 = list[0].second == 0 ? s1.b : s1.a; // far ends
        Vec2 o2 = list[1].second == 0 ? s2.b : s2.a;
        Vec2 d1 = toward(p, o1), d2 = toward(p, o2);
        if (d1.x * d2.x + d1.y * d2.y != 0.0f || inside_any(p))
            continue; // straight through, or a T
        float r = std::min({radius, length(s1) * 0.5f, length(s2) * 0.5f});
        if (r <= 0.0f)
            continue;
        out.trim[list[0].first][list[0].second] = r;
        out.trim[list[1].first][list[1].second] = r;
        Corner c;
        c.radius = r;
        c.from = Vec2{p.x + d1.x * r, p.y + d1.y * r};
        c.to = Vec2{p.x + d2.x * r, p.y + d2.y * r};
        c.center = Vec2{p.x + (d1.x + d2.x) * r, p.y + (d1.y + d2.y) * r};
        out.corners.push_back(c);
    }
    return out;
}

std::vector<FlowEdge> flow_edges(const NetRoute& route, std::span<const Vec2> sources) {
    // Points where pieces meet: segment ends and sources.
    std::map<float, std::vector<float>> on_row, on_column; // y -> xs, x -> ys
    auto add_point = [&](Vec2 p) {
        on_row[p.y].push_back(p.x);
        on_column[p.x].push_back(p.y);
    };
    for (const Segment& s : route.segments) {
        add_point(s.a);
        add_point(s.b);
    }
    for (Vec2 p : sources)
        add_point(p);

    // Split each segment at the points lying on it.
    std::vector<std::pair<Vec2, Vec2>> pieces;
    for (const Segment& s : route.segments) {
        bool horizontal = s.a.y == s.b.y;
        float lo = horizontal ? std::min(s.a.x, s.b.x) : std::min(s.a.y, s.b.y);
        float hi = horizontal ? std::max(s.a.x, s.b.x) : std::max(s.a.y, s.b.y);
        const std::vector<float>& along = horizontal ? on_row[s.a.y] : on_column[s.a.x];
        std::vector<float> cuts{lo, hi};
        for (float v : along)
            if (v > lo && v < hi)
                cuts.push_back(v);
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        // Keep the stored direction (a -> b) for pieces nothing reaches.
        bool forward = horizontal ? s.a.x <= s.b.x : s.a.y <= s.b.y;
        for (size_t k = 0; k + 1 < cuts.size(); k++) {
            Vec2 p = horizontal ? Vec2{cuts[k], s.a.y} : Vec2{s.a.x, cuts[k]};
            Vec2 q = horizontal ? Vec2{cuts[k + 1], s.a.y} : Vec2{s.a.x, cuts[k + 1]};
            pieces.push_back(forward ? std::pair{p, q} : std::pair{q, p});
        }
    }

    // Shortest wire distance from the sources (Dijkstra over the pieces).
    std::map<Key, uint32_t> id;
    auto node = [&](Vec2 p) {
        auto [it, added] = id.try_emplace(key(p), static_cast<uint32_t>(id.size()));
        return it->second;
    };
    std::vector<std::vector<std::pair<uint32_t, float>>> next;
    std::vector<std::pair<uint32_t, uint32_t>> ends;
    for (const auto& [p, q] : pieces) {
        uint32_t u = node(p), v = node(q);
        next.resize(id.size());
        float len = std::abs(q.x - p.x) + std::abs(q.y - p.y);
        next[u].push_back({v, len});
        next[v].push_back({u, len});
        ends.push_back({u, v});
    }
    const float kFar = std::numeric_limits<float>::infinity();
    std::vector<float> dist(id.size(), kFar);
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    for (Vec2 s : sources)
        if (auto it = id.find(key(s)); it != id.end() && dist[it->second] != 0.0f) {
            dist[it->second] = 0.0f;
            queue.push({0.0f, it->second});
        }
    while (!queue.empty()) {
        auto [d, u] = queue.top();
        queue.pop();
        if (d > dist[u])
            continue;
        for (auto [v, len] : next[u])
            if (d + len < dist[v]) {
                dist[v] = d + len;
                queue.push({dist[v], v});
            }
    }

    std::vector<FlowEdge> out;
    out.reserve(pieces.size());
    for (size_t i = 0; i < pieces.size(); i++) {
        auto [u, v] = ends[i];
        auto [p, q] = pieces[i];
        if (dist[u] == kFar && dist[v] == kFar)
            out.push_back({p, q, 0.0f});
        else if (dist[u] <= dist[v])
            out.push_back({p, q, dist[u]});
        else
            out.push_back({q, p, dist[v]});
    }
    return out;
}

} // namespace netviz
