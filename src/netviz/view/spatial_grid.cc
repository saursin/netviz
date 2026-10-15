#include "netviz/view/spatial_grid.h"

#include <algorithm>
#include <cmath>

namespace netviz {

namespace {

bool intersects(const Rect& a, const Rect& b) {
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y;
}

bool contains(const Rect& r, Vec2 p) {
    return p.x >= r.min.x && p.x <= r.max.x && p.y >= r.min.y && p.y <= r.max.y;
}

} // namespace

SpatialGrid::SpatialGrid(std::span<const Rect> rects, float cell_size)
    : rects_(rects.begin(), rects.end()), cell_(std::max(cell_size, 1.0f)) {
    if (rects_.empty())
        return;
    Rect bounds = rects_[0];
    for (const Rect& r : rects_) {
        bounds.min.x = std::min(bounds.min.x, r.min.x);
        bounds.min.y = std::min(bounds.min.y, r.min.y);
        bounds.max.x = std::max(bounds.max.x, r.max.x);
        bounds.max.y = std::max(bounds.max.y, r.max.y);
    }
    origin_ = bounds.min;
    columns_ = static_cast<int>(std::floor(bounds.width() / cell_)) + 1;
    rows_ = static_cast<int>(std::floor(bounds.height() / cell_)) + 1;

    // Two passes (count, then fill) into compressed rows.
    size_t cells = static_cast<size_t>(columns_) * static_cast<size_t>(rows_);
    cell_begin_.assign(cells + 1, 0);
    for (int pass = 0; pass < 2; pass++) {
        std::vector<uint32_t> next;
        if (pass == 1) {
            for (size_t c = 0; c < cells; c++)
                cell_begin_[c + 1] += cell_begin_[c];
            items_.resize(cell_begin_[cells]);
            next.assign(cell_begin_.begin(), cell_begin_.end() - 1);
        }
        for (uint32_t i = 0; i < rects_.size(); i++) {
            int x0, y0, x1, y1;
            cell_range(rects_[i], x0, y0, x1, y1);
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    size_t c = static_cast<size_t>(y) * columns_ + x;
                    if (pass == 0)
                        cell_begin_[c + 1]++;
                    else
                        items_[next[c]++] = i;
                }
        }
    }
}

void SpatialGrid::cell_range(const Rect& r, int& x0, int& y0, int& x1, int& y1) const {
    auto clamp_x = [&](float v) {
        return std::clamp(static_cast<int>(std::floor((v - origin_.x) / cell_)), 0, columns_ - 1);
    };
    auto clamp_y = [&](float v) {
        return std::clamp(static_cast<int>(std::floor((v - origin_.y) / cell_)), 0, rows_ - 1);
    };
    x0 = clamp_x(r.min.x);
    x1 = clamp_x(r.max.x);
    y0 = clamp_y(r.min.y);
    y1 = clamp_y(r.max.y);
}

std::vector<uint32_t> SpatialGrid::query(const Rect& area) const {
    std::vector<uint32_t> out;
    if (rects_.empty())
        return out;
    int x0, y0, x1, y1;
    cell_range(area, x0, y0, x1, y1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            size_t c = static_cast<size_t>(y) * columns_ + x;
            for (uint32_t k = cell_begin_[c]; k < cell_begin_[c + 1]; k++)
                if (intersects(rects_[items_[k]], area))
                    out.push_back(items_[k]);
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int64_t SpatialGrid::pick(Vec2 point) const {
    int64_t found = -1;
    for (uint32_t i : query(Rect{point, point}))
        if (contains(rects_[i], point))
            found = i;
    return found;
}

} // namespace netviz
