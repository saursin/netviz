#pragma once

#include <cstdint>

namespace netviz {

enum class Theme { Dark, Light };

// User preferences (saved in settings.json, see settings_file.h) and panel
// visibility (saved with the window layout in layout.ini).
struct Settings {
    // Configuration (Preferences window)
    Theme theme = Theme::Dark;
    float ui_scale = 1.0f;  // on top of the display's own scale
    float min_zoom = 0.01f; // canvas zoom limits (1.0 = 100%)
    float max_zoom = 32.0f;
    // Modules with more display nodes (cells and ports) are not laid out or
    // drawn: large ones are slow to draw until level of detail exists.
    uint32_t max_schematic_nodes = 50000;

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
