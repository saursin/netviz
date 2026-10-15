#pragma once

#include "netviz/view/camera.h"

namespace netviz {

struct CanvasOptions {
    bool show_grid = true;
    bool light_theme = false;
};

// The central drawing area. Handles pan/zoom input and draws the scene.
//
// Today it only draws a background grid with ImDrawList. Graph geometry
// will be drawn by a dedicated GPU renderer (batched buffers + camera
// uniform) into a texture shown here; see docs/ARCHITECTURE.md.
class CanvasPanel {
  public:
    static constexpr const char* kWindowName = "Canvas";

    // `pinch_zoom` is the touchpad pinch factor for this frame (1.0 = none).
    void draw(Camera2D& camera, const CanvasOptions& options, float pinch_zoom);

    bool hovered() const { return hovered_; }
    // Mouse position in world coordinates (valid while hovered).
    Vec2 mouse_world() const { return mouse_world_; }

  private:
    bool hovered_ = false;
    Vec2 mouse_world_;
};

} // namespace netviz
