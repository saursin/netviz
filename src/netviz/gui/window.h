#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_GLContextState;

namespace netviz {

struct WindowConfig {
    std::string title = "netviz";
    int width = 1280;
    int height = 800;
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

  private:
    void handle_event(const void* sdl_event);
    static void on_file_dialog_result(void* userdata, const char* const* files, int filter);

    SDL_Window* window_ = nullptr;
    SDL_GLContextState* gl_context_ = nullptr;
    bool imgui_ready_ = false;
    bool quit_ = false;
    int idle_frames_ = 0;
    float display_scale_ = 1.0f;
    float pinch_zoom_ = 1.0f;      // pinch factor accumulated since last take
    float pinch_last_ = 1.0f;      // last cumulative scale of the current gesture
    bool pinch_cumulative_ = true; // see handle_event()
    std::string ini_path_;

    // Filled by the file dialog callback, which may run on another thread.
    std::mutex opened_mutex_;
    std::vector<std::string> opened_files_;
};

} // namespace netviz
