#pragma once

#include "netviz/core/command.h"
#include "netviz/gui/console.h"
#include "netviz/gui/design_panels.h"
#include "netviz/gui/preferences.h"
#include "netviz/gui/schematic.h"
#include "netviz/gui/schematic_canvas.h"
#include "netviz/gui/settings.h"
#include "netviz/gui/window.h"
#include "netviz/io/yosys_json.h"

#include <future>
#include <memory>
#include <string>
#include <vector>

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
    // Preferences live in settings.json (settings_file.h); session state
    // (panel visibility, recent files) in a "[Netviz][State]" section of
    // ImGui's layout.ini, next to the window layout.
    void load_settings();
    void save_settings();
    void register_state_handler();
    void read_state(const std::string& key, const std::string& value);
    std::string write_state() const;
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

    // Canvases (see schematic_canvas.h). The active one is the last focused;
    // the toolbar, status bar and Hierarchy clicks act on it.
    void draw_canvases(unsigned int central_dock_id);
    SchematicCanvas& active_canvas();
    SchematicCanvas::Context canvas_context(float pinch_zoom = 1.0f);
    void open_canvas(const SchematicCanvas::Place& place);
    // Closes all canvases but one and shows `place` (or nothing) in it.
    void reset_canvases(const SchematicCanvas::Place& place);

    Window* window_ = nullptr;
    float display_scale_ = 1.0f;

    ConsolePanel console_;
    CommandRegistry commands_;
    DesignPanels design_;
    SchematicService schematics_; // after design_: destroyed (and its job stopped) first
    std::vector<std::unique_ptr<SchematicCanvas>> canvases_; // never empty
    int active_canvas_ = 0;                                  // id
    int next_canvas_id_ = 1;
    unsigned int central_dock_id_ = 0; // where canvases go by default
    bool docked_first_canvas_ = false;

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
    std::vector<std::string> recent_files_; // newest first, absolute paths; saved in layout.ini
    uint64_t memory_before_load_ = 0; // process memory when the last load started // file waiting
                                      // for "close the current design?"

    PreferencesWindow preferences_;
    Settings settings_;
    Settings saved_settings_;      // last state handed to ImGui's layout.ini
    Settings file_settings_;       // last preferences written to settings.json
    bool settings_file_ok_ = true; // false: unreadable, so it is not overwritten
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
