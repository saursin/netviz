#pragma once

namespace netviz {

enum class Theme { Dark, Light };

// User preferences and panel visibility. Saved to imgui.ini, restored on start.
struct Settings {
    // Configuration (Preferences window)
    Theme theme = Theme::Dark;
    float ui_scale = 1.0f;  // on top of the display's own scale
    float min_zoom = 0.01f; // canvas zoom limits (1.0 = 100%)
    float max_zoom = 32.0f;

    // Quick view toggles (menus)
    bool show_grid = true;
    bool show_perf_stats = false; // memory and UI time in the status bar

    // Panel visibility
    bool show_hierarchy = true;
    bool show_modules = true;
    bool show_properties = true;
    bool show_console = true;

    bool operator==(const Settings&) const = default;
};

} // namespace netviz
