#pragma once

#include "netviz/core/command.h"
#include "netviz/gui/canvas.h"
#include "netviz/gui/console.h"
#include "netviz/gui/design_panels.h"
#include "netviz/gui/preferences.h"
#include "netviz/gui/schematic.h"
#include "netviz/gui/settings.h"
#include "netviz/gui/window.h"
#include "netviz/io/yosys_json.h"
#include "netviz/view/camera.h"

#include <future>
#include <map>
#include <memory>
#include <string>

namespace netviz {

// Top-level UI: menu bar, toolbar, status bar, dock space and all panels.
//
// Call init() once after the window is created, then draw() every frame
// between Window::begin_frame() and end_frame().
class AppUi {
  public:
    void init(Window& window);
    void draw();
    // Waits for a file that is still loading. Call before the window closes.
    void shutdown();

    // Opens a Yosys JSON netlist. If a design is already open, asks first
    // whether to close it.
    void open_file(const std::string& path);
    void close_design();
    // Loads the last opened file again (e.g. after re-running Yosys).
    void reload();
    bool can_reload() const;
    void cancel_loading();

    bool quit_requested() const { return quit_requested_; }

  private:
    void load_fonts();
    void apply_style();
    void register_settings_handler();
    void read_setting(const std::string& key, const std::string& value);
    std::string write_settings() const;
    void register_commands();

    void start_loading(const std::string& path);
    void log_design_stats(const ImportTimings* timings);
    void add_recent_file(const std::string& path);
    void draw_recent_menu();
    void poll_loading();
    void draw_open_prompt();
    void draw_load_progress();
    void handle_shortcuts();
    void toggle_left_column();
    void draw_menu_bar();
    void draw_settings_menu();
    void draw_toolbar();
    void draw_status_bar();
    void draw_about();
    void build_default_layout(unsigned int dockspace_id);

    // Schematic of the selected module (see schematic.h).
    void update_schematic();
    void draw_canvas();
    void draw_schematic_scene(ImDrawList* dl, ImVec2 origin);
    void draw_canvas_overlay(ImVec2 origin);
    void draw_node_tooltip(int64_t node);
    void handle_canvas_events(const CanvasEvents& events);
    // Display node of the selected cell or port, if it is in the shown schematic.
    int64_t selected_node() const;
    // Fits the view to the schematic, or resets it when nothing is drawn.
    void fit_view();
    // The view last used for the shown module, else a fit.
    void restore_or_fit_view();

    Window* window_ = nullptr;
    float display_scale_ = 1.0f;

    Camera2D camera_;
    CanvasPanel canvas_;
    ConsolePanel console_;
    CommandRegistry commands_;
    DesignPanels design_;
    SchematicView schematic_; // after design_: destroyed (and its job stopped) first
    int64_t hovered_node_ = -1;
    bool fit_pending_ = false; // fit once the canvas knows its size this frame
    // Pan and zoom per module, restored when going back to it.
    struct SavedView {
        Vec2 center;
        float zoom = 1.0f;
    };
    std::map<uint32_t, SavedView> module_views_; // by module index

    // Result of a background load, handed to the UI thread when ready.
    struct LoadedDesign {
        ImportResult result;
        InstanceLists instances;
        double seconds = 0.0;
    };
    std::future<LoadedDesign> loading_;
    std::string loading_path_;
    std::shared_ptr<ImportProgress> load_progress_; // shared with the loading thread
    std::string last_file_; // last file loaded or attempted; what reload() reads
    std::string pending_open_;
    std::vector<std::string> recent_files_; // newest first, absolute paths; saved in imgui.ini
    uint64_t memory_before_load_ = 0; // process memory when the last load started // file waiting
                                      // for "close the current design?"

    PreferencesWindow preferences_;
    Settings settings_;
    Settings saved_settings_; // last state written to imgui.ini
    bool style_dirty_ = false;
    Theme applied_theme_ = Theme::Dark; // style currently applied, see apply_style()
    float applied_ui_scale_ = 1.0f;

    bool show_about_ = false;
    bool show_imgui_demo_ = false;
    bool show_imgui_metrics_ = false;

    // Performance stats (Settings > Show Performance Stats).
    double last_ui_ms_ = 0.0;    // time to build the previous frame's UI
    Window::MemoryUsage memory_; // refreshed about once a second
    double memory_refresh_at_ = 0.0;
    bool reset_layout_ = false;
    bool quit_requested_ = false;
};

} // namespace netviz
