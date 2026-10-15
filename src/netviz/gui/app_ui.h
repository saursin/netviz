#pragma once

#include "netviz/core/command.h"
#include "netviz/gui/canvas.h"
#include "netviz/gui/console.h"
#include "netviz/view/camera.h"

#include <string>

namespace netviz {

class Window;

enum class Theme { Dark, Light };

// User preferences and panel visibility. Saved to imgui.ini, restored on start.
struct Settings {
    Theme theme = Theme::Dark;
    float ui_scale = 1.0f; // on top of the display's own scale
    bool show_grid = true;
    bool show_sidebar = true;
    bool show_properties = true;
    bool show_console = true;

    bool operator==(const Settings&) const = default;
};

// Top-level UI: menu bar, toolbar, status bar, dock space and all panels.
//
// Call init() once after the window is created, then draw() every frame
// between Window::begin_frame() and end_frame().
class AppUi {
  public:
    void init(Window& window);
    void draw();

    // Opens a netlist file. (Loading is not implemented yet; this records
    // the file and reports it.)
    void open_file(const std::string& path);

    bool quit_requested() const { return quit_requested_; }

  private:
    void load_fonts();
    void apply_style();
    void register_settings_handler();
    void read_setting(const std::string& key, const std::string& value);
    std::string write_settings() const;
    void register_commands();

    void handle_shortcuts();
    void draw_menu_bar();
    void draw_settings_menu();
    void draw_toolbar();
    void draw_status_bar();
    void draw_sidebar();
    void draw_properties();
    void draw_about();
    void build_default_layout(unsigned int dockspace_id);

    Window* window_ = nullptr;
    float display_scale_ = 1.0f;

    Camera2D camera_;
    CanvasPanel canvas_;
    ConsolePanel console_;
    CommandRegistry commands_;

    Settings settings_;
    Settings saved_settings_; // last state written to imgui.ini
    bool style_dirty_ = false;

    std::string current_file_;
    bool show_about_ = false;
    bool show_imgui_demo_ = false;
    bool reset_layout_ = false;
    bool quit_requested_ = false;
};

} // namespace netviz
