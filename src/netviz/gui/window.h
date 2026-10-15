#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_GLContextState;

namespace netviz {

// Where netviz keeps its per-user files. Both in the folder given by
// --config-dir or $NETVIZ_CONFIG_DIR if set; otherwise on Linux and BSD
// settings in $XDG_CONFIG_HOME/netviz (~/.config/netviz) and state in
// $XDG_STATE_HOME/netviz (~/.local/state/netviz), elsewhere both in SDL's
// per-user folder.
struct AppPaths {
    std::filesystem::path settings;   // settings.json: preferences
    std::filesystem::path layout;     // layout.ini: window/dock layout, panels, recent files
    std::filesystem::path legacy_ini; // imgui.ini of older versions (read once to migrate;
                                      // empty with a chosen folder)
};

struct WindowConfig {
    std::string title = "netviz";
    int width = 1280;
    int height = 800;
    std::filesystem::path config_dir; // see AppPaths; empty: $NETVIZ_CONFIG_DIR or the default
};

// Owns the OS window, the OpenGL context and the ImGui context/backends.
//
// Typical loop:
//     while (window.process_events()) {
//         window.begin_frame();
//         ... ImGui calls ...
//         window.end_frame();
//     }
class Window {
  public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Returns false (and logs the reason) if the window could not be created.
    bool init(const WindowConfig& config);
    void shutdown();

    // Handles pending OS events. When nothing has happened for a few frames it
    // blocks until the next event, so an idle app uses almost no CPU.
    // Returns false once the user asked to close the window.
    bool process_events();

    void begin_frame();
    void end_frame();

    const AppPaths& paths() const { return paths_; }

    // Display content scale (e.g. 2.0 on a 200% HiDPI monitor).
    float display_scale() const { return display_scale_; }

    void set_title(const std::string& title);

    // Shows the native "open file" dialog without blocking. The chosen
    // path is returned by take_opened_file() on a later frame.
    void show_open_file_dialog();
    std::optional<std::string> take_opened_file();

    // Combined zoom factor of touchpad pinch gestures since the last call
    // (1.0 if none). ImGui does not handle pinch, so it is collected here.
    float take_pinch_zoom();

    // Wakes the event loop from any thread (e.g. when background work finishes).
    static void post_wake();

    // While idle, redraw at least this often (ms). Default 500; lower it while
    // something changes without input, e.g. a progress bar.
    void set_idle_timeout_ms(int ms) { idle_timeout_ms_ = ms; }

    // Memory used by this process, in bytes (0 if the platform does not say).
    struct MemoryUsage {
        uint64_t current = 0; // resident set size
        uint64_t peak = 0;    // highest resident set size so far
    };
    static MemoryUsage memory_usage();

  private:
    void handle_event(const void* sdl_event);
    static void on_file_dialog_result(void* userdata, const char* const* files, int filter);

    SDL_Window* window_ = nullptr;
    SDL_GLContextState* gl_context_ = nullptr;
    bool imgui_ready_ = false;
    bool quit_ = false;
    int idle_frames_ = 0;
    float display_scale_ = 1.0f;
    int idle_timeout_ms_ = 500;
    float pinch_zoom_ = 1.0f;      // pinch factor accumulated since last take
    float pinch_last_ = 1.0f;      // last cumulative scale of the current gesture
    bool pinch_cumulative_ = true; // see handle_event()
    AppPaths paths_;
    std::string ini_path_; // paths_.layout, kept alive for ImGui

    // Filled by the file dialog callback, which may run on another thread.
    std::mutex opened_mutex_;
    std::vector<std::string> opened_files_;
};

} // namespace netviz
