#pragma once

#include "netviz/view/camera.h"

#include <imgui.h>

#include <functional>

namespace netviz {

struct CanvasOptions {
    bool show_grid = true;
    bool light_theme = false;
};

// What the user did on the canvas this frame; the owner acts on it.
struct CanvasEvents {
    bool clicked = false;        // left button pressed on the canvas
    bool double_clicked = false; // (clicked is also set)
    bool fit_requested = false;  // F
    bool up_requested = false;   // Backspace: leave the shown module
};

// The central drawing area. Handles pan/zoom input, draws the background
// grid and calls back to draw the scene and any overlay widgets.
//
// The scene is drawn with ImDrawList for now. Large designs will move to a
// dedicated GPU renderer (batched buffers + camera uniform); see
// docs/ARCHITECTURE.md.
class CanvasPanel {
  public:
    static constexpr const char* kWindowName = "Canvas";

    // Draws into the canvas's draw list; `origin` is the canvas's top-left
    // corner in screen coordinates (camera screen coordinates are relative to it).
    using SceneFn = std::function<void(ImDrawList* dl, ImVec2 origin)>;
    // Submits ImGui widgets drawn over the canvas (they take mouse input
    // before the canvas does).
    using OverlayFn = std::function<void(ImVec2 origin)>;

    // `pinch_zoom` is the touchpad pinch factor for this frame (1.0 = none).
    CanvasEvents draw(Camera2D& camera, const CanvasOptions& options, float pinch_zoom,
                      const SceneFn& scene, const OverlayFn& overlay);

    bool hovered() const { return hovered_; }
    // Mouse position in world coordinates (valid while hovered).
    Vec2 mouse_world() const { return mouse_world_; }

  private:
    bool hovered_ = false;
    Vec2 mouse_world_;
};

} // namespace netviz
