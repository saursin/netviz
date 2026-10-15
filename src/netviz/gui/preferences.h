#pragma once

#include "netviz/gui/settings.h"

namespace netviz {

// The Preferences window: sections on the left, the selected section's
// settings on the right. It edits a copy of the settings; Apply or OK copies
// the configuration into `settings`, Cancel (or closing) discards it.
class PreferencesWindow {
  public:
    static constexpr const char* kWindowName = "Preferences";

    void open() {
        just_opened_ = !open_;
        open_ = true;
    }
    void draw(Settings& settings);

  private:
    enum class Section { Appearance, Canvas };

    void draw_appearance(Settings& settings);
    void draw_canvas(Settings& settings);

    // Copies the configuration part of the settings (not the view toggles,
    // which the menus may change while the window is open).
    static void copy_config(const Settings& from, Settings& to);
    static bool same_config(const Settings& a, const Settings& b);

    bool open_ = false;
    bool just_opened_ = false;
    Settings draft_; // edited copy
    Section section_ = Section::Appearance;
};

} // namespace netviz
