#include "netviz/gui/preferences.h"

#include <imgui.h>

#include <algorithm>
#include <format>
#include <string>

namespace netviz {

namespace {

const float kUiScales[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f};

// A labelled row: label in the first column, widget in the second.
void row_label(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool begin_rows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp))
        return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.4f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    return true;
}

} // namespace

void PreferencesWindow::copy_config(const Settings& from, Settings& to) {
    to.theme = from.theme;
    to.ui_scale = from.ui_scale;
    to.min_zoom = from.min_zoom;
    to.max_zoom = from.max_zoom;
    to.max_schematic_nodes = from.max_schematic_nodes;
}

bool PreferencesWindow::same_config(const Settings& a, const Settings& b) {
    return a.theme == b.theme && a.ui_scale == b.ui_scale && a.min_zoom == b.min_zoom &&
           a.max_zoom == b.max_zoom && a.max_schematic_nodes == b.max_schematic_nodes;
}

void PreferencesWindow::draw(Settings& settings) {
    if (!open_)
        return;
    if (just_opened_) {
        copy_config(settings, draft_);
        just_opened_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36, ImGui::GetFontSize() * 20),
                             ImGuiCond_FirstUseEver);
    bool visible = ImGui::Begin(kWindowName, &open_, ImGuiWindowFlags_NoDocking);
    if (!open_ || !visible) { // closed with the window's x: same as Cancel
        ImGui::End();
        return;
    }

    // Left: sections.
    float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("sections", ImVec2(ImGui::GetFontSize() * 9, -footer),
                      ImGuiChildFlags_Borders);
    struct {
        const char* name;
        Section section;
    } sections[] = {{"Appearance", Section::Appearance}, {"Canvas", Section::Canvas}};
    for (const auto& s : sections) {
        if (ImGui::Selectable(s.name, section_ == s.section))
            section_ = s.section;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: the selected section's settings (edits the draft).
    ImGui::BeginChild("settings", ImVec2(0, -footer));
    if (section_ == Section::Appearance)
        draw_appearance(draft_);
    else
        draw_canvas(draft_);
    ImGui::EndChild();

    // Footer: Reset on the left; OK, Cancel, Apply on the right.
    if (ImGui::Button("Reset section to defaults")) {
        Settings defaults;
        if (section_ == Section::Appearance) {
            draft_.theme = defaults.theme;
            draft_.ui_scale = defaults.ui_scale;
        } else {
            draft_.min_zoom = defaults.min_zoom;
            draft_.max_zoom = defaults.max_zoom;
            draft_.max_schematic_nodes = defaults.max_schematic_nodes;
        }
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    auto button_width = [&](const char* label) {
        return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
    };
    float buttons = button_width("OK") + button_width("Cancel") + button_width("Apply") +
                    style.ItemSpacing.x * 2.0f;
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons);
    bool changed = !same_config(draft_, settings);
    if (ImGui::Button("OK")) {
        copy_config(draft_, settings);
        open_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        open_ = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button("Apply"))
        copy_config(draft_, settings);
    ImGui::EndDisabled();
    ImGui::End();
}

void PreferencesWindow::draw_appearance(Settings& settings) {
    ImGui::SeparatorText("Appearance");
    if (!begin_rows("appearance"))
        return;

    row_label("Theme");
    int theme = settings.theme == Theme::Light ? 1 : 0;
    if (ImGui::Combo("##theme", &theme, "Dark\0Light\0"))
        settings.theme = theme == 1 ? Theme::Light : Theme::Dark;

    row_label("UI scale");
    std::string current = std::format("{:.0f}%", settings.ui_scale * 100.0f);
    if (ImGui::BeginCombo("##scale", current.c_str())) {
        for (float scale : kUiScales) {
            std::string label = std::format("{:.0f}%", scale * 100.0f);
            if (ImGui::Selectable(label.c_str(), settings.ui_scale == scale))
                settings.ui_scale = scale;
        }
        ImGui::EndCombo();
    }
    ImGui::EndTable();
}

void PreferencesWindow::draw_canvas(Settings& settings) {
    ImGui::SeparatorText("Zoom");
    if (!begin_rows("canvas"))
        return;

    // Edited in percent; stored as factors (1.0 = 100%).
    row_label("Minimum zoom");
    float min_percent = settings.min_zoom * 100.0f;
    if (ImGui::InputFloat("##min_zoom", &min_percent, 0.0f, 0.0f, "%.2f %%"))
        settings.min_zoom = std::clamp(min_percent, 0.01f, 100.0f) / 100.0f;
    ImGui::SetItemTooltip("Smallest zoom level, in percent (0.01 - 100)");

    row_label("Maximum zoom");
    float max_percent = settings.max_zoom * 100.0f;
    if (ImGui::InputFloat("##max_zoom", &max_percent, 0.0f, 0.0f, "%.0f %%"))
        settings.max_zoom = std::clamp(max_percent, 100.0f, 100000.0f) / 100.0f;
    ImGui::SetItemTooltip("Largest zoom level, in percent (100 - 100000)");
    ImGui::EndTable();

    ImGui::SeparatorText("Schematic");
    if (!begin_rows("schematic"))
        return;
    row_label("Largest schematic");
    int nodes = static_cast<int>(settings.max_schematic_nodes);
    if (ImGui::InputInt("##max_nodes", &nodes, 10000, 100000))
        settings.max_schematic_nodes = static_cast<uint32_t>(std::clamp(nodes, 1000, 10000000));
    ImGui::SetItemTooltip("Modules with more cells and ports than this are not drawn.\n"
                          "Large schematics take a while to lay out and are slow to pan\n"
                          "and zoom when zoomed out (1,000 - 10,000,000)");
    ImGui::EndTable();
}

} // namespace netviz
