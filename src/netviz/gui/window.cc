#include "netviz/gui/window.h"

#include "netviz/core/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <string_view>

namespace netviz {

namespace {

// Frames to keep rendering after the last event, so ImGui can settle
// hover states and layout changes before the loop goes idle.
constexpr int kActiveFrames = 3;

// While idle, still wake up this often (ms) to pick up e.g. new log lines.
constexpr int kIdleTimeoutMs = 500;

} // namespace

Window::~Window() {
    shutdown();
}

bool Window::init(const WindowConfig& config) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log_error("SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.0f)
        scale = 1.0f;

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
                            SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(config.title.c_str(), static_cast<int>(config.width * scale),
                               static_cast<int>(config.height * scale), flags);
    if (!window_) {
        log_error("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }

    gl_context_ = SDL_GL_CreateContext(window_);
    if (!gl_context_) {
        log_error("SDL_GL_CreateContext failed: {}", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, gl_context_);
    SDL_GL_SetSwapInterval(1);
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Keep imgui.ini in the per-user config dir instead of the working directory.
    if (char* pref = SDL_GetPrefPath("netviz", "netviz")) {
        ini_path_ = std::string(pref) + "imgui.ini";
        SDL_free(pref);
        io.IniFilename = ini_path_.c_str();
    }

    display_scale_ = scale;
    const char* driver = SDL_GetCurrentVideoDriver();
    pinch_cumulative_ = !(driver && std::string_view(driver) == "cocoa");

    ImGui_ImplSDL3_InitForOpenGL(window_, gl_context_);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    imgui_ready_ = true;

    log_info("OpenGL {} on {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
             reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    log_debug("ImGui settings: {}", ini_path_);
    return true;
}

void Window::shutdown() {
    if (imgui_ready_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    if (gl_context_) {
        SDL_GL_DestroyContext(gl_context_);
        gl_context_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
    }
}

void Window::handle_event(const void* sdl_event) {
    const SDL_Event* event = static_cast<const SDL_Event*>(sdl_event);
    ImGui_ImplSDL3_ProcessEvent(event);
    if (event->type == SDL_EVENT_QUIT)
        quit_ = true;

    // SDL 3.4 reports pinch scale differently per platform: Wayland and X11
    // pass the scale relative to the start of the gesture, macOS the change
    // since the last update. Convert to per-update steps.
    if (event->type == SDL_EVENT_PINCH_BEGIN)
        pinch_last_ = 1.0f;
    if (event->type == SDL_EVENT_PINCH_UPDATE && event->pinch.scale > 0.0f) {
        float scale = event->pinch.scale;
        if (pinch_cumulative_) {
            pinch_zoom_ *= scale / pinch_last_;
            pinch_last_ = scale;
        } else {
            pinch_zoom_ *= scale;
        }
    }
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
        event->window.windowID == SDL_GetWindowID(window_))
        quit_ = true;
}

bool Window::process_events() {
    SDL_Event event;
    bool got_event = false;

    bool idle = idle_frames_ >= kActiveFrames;
    bool minimized = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
    if (idle || minimized) {
        if (SDL_WaitEventTimeout(&event, kIdleTimeoutMs)) {
            handle_event(&event);
            got_event = true;
        }
    }
    while (SDL_PollEvent(&event)) {
        handle_event(&event);
        got_event = true;
    }

    if (got_event)
        idle_frames_ = 0;
    else
        idle_frames_++;
    return !quit_;
}

void Window::set_title(const std::string& title) {
    SDL_SetWindowTitle(window_, title.c_str());
}

void Window::show_open_file_dialog() {
    // Must stay valid until the dialog closes, hence static.
    static const SDL_DialogFileFilter filters[] = {
        {"Netlists (*.json, *.v, *.sv)", "json;v;sv"},
        {"Liberty (*.lib)", "lib"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(on_file_dialog_result, this, window_, filters, SDL_arraysize(filters),
                           nullptr, false);
}

void Window::on_file_dialog_result(void* userdata, const char* const* files, int /*filter*/) {
    if (!files) {
        log_error("File dialog failed: {}", SDL_GetError());
        return;
    }
    if (!files[0])
        return; // cancelled

    Window* self = static_cast<Window*>(userdata);
    {
        std::lock_guard lock(self->opened_mutex_);
        self->opened_files_.push_back(files[0]);
    }
    // Wake the main loop in case it is waiting for events.
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);
}

std::optional<std::string> Window::take_opened_file() {
    std::lock_guard lock(opened_mutex_);
    if (opened_files_.empty())
        return std::nullopt;
    std::string path = opened_files_.front();
    opened_files_.erase(opened_files_.begin());
    return path;
}

float Window::take_pinch_zoom() {
    float zoom = pinch_zoom_;
    pinch_zoom_ = 1.0f;
    return zoom;
}

void Window::begin_frame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void Window::end_frame() {
    ImGui::Render();

    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window_);
}

} // namespace netviz
