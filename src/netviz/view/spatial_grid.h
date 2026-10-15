#pragma once

#include "netviz/view/camera.h"

#include <cstdint>
#include <span>
#include <vector>

namespace netviz {

// A uniform grid over rectangles (e.g. the node boxes of a layout), for
// finding what is visible and what is under the cursor without scanning
// every rectangle.
class SpatialGrid {
  public:
    SpatialGrid() = default;
    // `cell_size` is in world units; rectangles are indexed by position.
    SpatialGrid(std::span<const Rect> rects, float cell_size);

    // Indices of rectangles intersecting `area`, each once, in index order.
    std::vector<uint32_t> query(const Rect& area) const;

    // The rectangle containing `point` (the highest index if several), or -1.
    int64_t pick(Vec2 point) const;

    const Rect& rect(uint32_t index) const { return rects_[index]; }

  private:
    void cell_range(const Rect& r, int& x0, int& y0, int& x1, int& y1) const;

    std::vector<Rect> rects_;
    float cell_ = 1.0f;
    Vec2 origin_;
    int columns_ = 0;
    int rows_ = 0;
    std::vector<uint32_t>
        cell_begin_; // rectangles of cell c: items_[cell_begin_[c] .. cell_begin_[c+1])
    std::vector<uint32_t> items_;
};

} // namespace netviz
