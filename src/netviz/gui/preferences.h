#pragma once

#include "netviz/gui/settings.h"

#include <map>
#include <string>

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
    enum class Section { Appearance, Canvas, Rules };

    void draw_appearance(Settings& settings);
    void draw_canvas(Settings& settings);
    void draw_rules(Settings& settings);
    void draw_signal_rule(SignalRule& rule);
    void draw_cell_rule(CellRule& rule);
    // A text field editing a list in text form (`formatted`); returns true
    // with the typed text in `out` when edited. While the user types, their
    // text is kept as is; otherwise it follows the value.
    bool list_field(const char* label, const std::string& formatted, std::string& out);

    // Copies the configuration part of the settings (not the view toggles,
    // which the menus may change while the window is open).
    static void copy_config(const Settings& from, Settings& to);
    static bool same_config(const Settings& a, const Settings& b);

    bool open_ = false;
    bool just_opened_ = false;
    Settings draft_; // edited copy
    Section section_ = Section::Appearance;
    int signal_selected_ = 0; // in the Rules section
    int cell_selected_ = 0;
    std::map<unsigned int, std::string> texts_; // list_field text by widget ID
};

} // namespace netviz
