#pragma once

namespace netviz {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rect {
    Vec2 min;
    Vec2 max;

    float width() const { return max.x - min.x; }
    float height() const { return max.y - min.y; }
};

// 2D camera mapping world coordinates to screen pixels.
//
// The camera is the only thing that changes while panning or zooming.
// Renderers apply it as a single transform, so graph data, layouts and
// GPU buffers never need to be rebuilt for a view change.
//
// Screen coordinates are relative to the viewport's top-left corner,
// y pointing down. World coordinates use the same orientation.
class Camera2D {
  public:
    // Default zoom range: 1% to 3200%.
    static constexpr float kDefaultMinZoom = 0.01f;
    static constexpr float kDefaultMaxZoom = 32.0f;

    // World point shown at the center of the viewport.
    Vec2 center() const { return center_; }
    // Screen pixels per world unit.
    float zoom() const { return zoom_; }

    void set_center(Vec2 c) { center_ = c; }
    void set_zoom(float z); // clamped to the zoom limits
    void reset();

    // Zoom range; the current zoom is clamped into it.
    void set_zoom_limits(float min_zoom, float max_zoom);
    float min_zoom() const { return min_zoom_; }
    float max_zoom() const { return max_zoom_; }

    void set_viewport_size(Vec2 size) { viewport_ = size; }
    Vec2 viewport_size() const { return viewport_; }

    Vec2 world_to_screen(Vec2 world) const;
    Vec2 screen_to_world(Vec2 screen) const;

    // Move the view by a mouse drag of `delta` screen pixels.
    void pan_by_screen_delta(Vec2 delta);

    // Multiply zoom by `factor`, keeping the world point under `screen_anchor` fixed.
    void zoom_at(Vec2 screen_anchor, float factor);

    // Centres `area` and zooms so it fills the viewport, leaving `margin`
    // (a fraction, e.g. 0.05 = 5%) around it. The zoom stays within limits.
    void fit(const Rect& area, float margin = 0.05f);

    // World-space rectangle currently visible; used for culling.
    Rect visible_world_rect() const;

  private:
    Vec2 center_;
    float zoom_ = 1.0f;
    float min_zoom_ = kDefaultMinZoom;
    float max_zoom_ = kDefaultMaxZoom;
    Vec2 viewport_;
};

} // namespace netviz
