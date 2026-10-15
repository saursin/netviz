#pragma once

#include "netviz/gui/canvas.h"
#include "netviz/gui/design_panels.h"
#include "netviz/gui/schematic.h"
#include "netviz/gui/settings.h"

#include <map>
#include <optional>
#include <string>

namespace netviz {

// One canvas window: shows a place in the design (a module, reached through
// an instance path or as a definition) as a schematic, with its own camera.
// Several can be open; the schematics themselves come from a shared
// SchematicService.
class SchematicCanvas {
  public:
    using Place = DesignPanels::Selection; // kind None

    struct Context {
        DesignPanels& panels;
        SchematicService& schematics;
        const Settings& settings;
        float pinch_zoom = 1.0f;
    };
    // What the canvas asks its owner to do.
    struct Requests {
        std::optional<Place> open_in_new_canvas;
    };

    explicit SchematicCanvas(int id) : id_(id) {}

    int id() const { return id_; }
    // The window's ImGui name; its ID part ("###canvasN") never changes.
    std::string window_name(const Design* design) const;
    static std::string window_id(int id);

    const Place& place() const { return place_; }
    // Shows another place; the current view is remembered for its module.
    void navigate(Place place);

    Camera2D& camera() { return camera_; }
    const CanvasPanel& panel() const { return panel_; }
    // Fits the view to the schematic, or resets it when nothing is drawn.
    void fit_view(const Context& ctx);

    // Docks the window into `dock_id` on its next frame and focuses it.
    void dock_into(unsigned int dock_id) { dock_at_ = dock_id; }

    // Draws the window; `open` (null: cannot be closed) is cleared when the
    // user closes it.
    Requests draw(Context& ctx, bool* open);

  private:
    struct SavedView {
        Vec2 center;
        float zoom = 1.0f;
    };

    const Schematic* schematic(Context& ctx);
    // What to highlight: the selected cell, port or net if it is in this
    // canvas's place, and what is under the cursor.
    Schematic::Marks marks(const Context& ctx, const Schematic& s) const;
    void draw_scene(Context& ctx, ImDrawList* dl, ImVec2 origin);
    void draw_overlay(Context& ctx, ImVec2 origin);
    void draw_tooltip(Context& ctx) const;
    void handle_events(Context& ctx, const CanvasEvents& events, Requests& requests);
    bool is_definition(const Design& design) const;

    int id_;
    Place place_;
    Camera2D camera_;
    CanvasPanel panel_;
    std::map<uint32_t, SavedView> views_; // pan and zoom per module, by module index
    const Schematic* shown_ = nullptr;    // drawn last frame (to notice new ones)
    ModuleId shown_module_;
    int64_t hovered_node_ = -1;
    int64_t hovered_net_ = -1; // when no node is under the cursor
    std::optional<unsigned int> dock_at_;
    Place menu_place_; // the instance whose context menu is open
};

} // namespace netviz
