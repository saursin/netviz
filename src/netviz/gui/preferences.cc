#include "netviz/gui/preferences.h"

#include <imgui.h>
#include <imgui_internal.h> // GetActiveID
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
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

// Lists in text form: comma-separated, spaces around items ignored.
std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> items;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos)
            end = text.size();
        std::string item = text.substr(start, end - start);
        size_t a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
        if (a != std::string::npos)
            items.push_back(item.substr(a, b - a + 1));
        start = end + 1;
    }
    return items;
}

std::string join_list(const std::vector<std::string>& items) {
    std::string text;
    for (size_t i = 0; i < items.size(); i++)
        text += (i ? ", " : "") + items[i];
    return text;
}

// "name=value" ("name" alone: any value).
std::string attributes_text(const std::vector<AttributePattern>& attributes) {
    std::vector<std::string> items;
    for (const AttributePattern& a : attributes)
        items.push_back(a.value == "*" ? a.name : a.name + "=" + a.value);
    return join_list(items);
}

std::vector<AttributePattern> parse_attributes(const std::string& text) {
    std::vector<AttributePattern> out;
    for (const std::string& item : split_list(text)) {
        size_t eq = item.find('=');
        if (eq == std::string::npos)
            out.push_back({item, "*"});
        else
            out.push_back({item.substr(0, eq), item.substr(eq + 1)});
    }
    return out;
}

// "cell:pin"
std::string cell_pins_text(const std::vector<CellPinPattern>& pins) {
    std::vector<std::string> items;
    for (const CellPinPattern& p : pins)
        items.push_back(p.cell + ":" + p.pin);
    return join_list(items);
}

std::vector<CellPinPattern> parse_cell_pins(const std::string& text) {
    std::vector<CellPinPattern> out;
    for (const std::string& item : split_list(text)) {
        size_t colon = item.rfind(':');
        if (colon == std::string::npos)
            out.push_back({"*", item});
        else
            out.push_back({item.substr(0, colon), item.substr(colon + 1)});
    }
    return out;
}

bool color_edit(const char* label, RuleColor& color) {
    float rgb[3] = {((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f,
                    (color & 0xFF) / 255.0f};
    if (!ImGui::ColorEdit3(label, rgb))
        return false;
    auto byte = [](float v) {
        return static_cast<RuleColor>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    color = (byte(rgb[0]) << 16) | (byte(rgb[1]) << 8) | byte(rgb[2]);
    return true;
}

ImVec4 color_vec(RuleColor c) {
    return ImVec4(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f,
                  1.0f);
}

// An ordered list of rules: enabled checkbox, color swatch and name per row,
// and buttons to add, remove and reorder. `selected` is clamped to the list.
template <typename Rule> void rule_list(const char* id, std::vector<Rule>& rules, int& selected) {
    ImGui::PushID(id);
    float rows = static_cast<float>(std::clamp<size_t>(rules.size(), 3, 6));
    ImGui::BeginChild("list", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * rows + 4.0f),
                      ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(rules.size()); i++) {
        Rule& rule = rules[i];
        ImGui::PushID(i);
        ImGui::Checkbox("##on", &rule.enabled);
        ImGui::SetItemTooltip("Enabled");
        ImGui::SameLine();
        ImGui::ColorButton("##color", color_vec(rule.color), ImGuiColorEditFlags_NoTooltip,
                           ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
        ImGui::SameLine();
        std::string label = rule.name.empty() ? "(unnamed)" : rule.name;
        ImGui::AlignTextToFramePadding();
        if (ImGui::Selectable(label.c_str(), selected == i))
            selected = i;
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (ImGui::Button("Add")) {
        Rule rule;
        rule.name = "New rule";
        rules.push_back(rule);
        selected = static_cast<int>(rules.size()) - 1;
    }
    ImGui::SameLine();
    bool valid = selected >= 0 && selected < static_cast<int>(rules.size());
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Remove") && valid)
        rules.erase(rules.begin() + selected);
    ImGui::SameLine();
    if (ImGui::Button("Up") && valid && selected > 0) {
        std::swap(rules[selected], rules[selected - 1]);
        selected--;
    }
    ImGui::SetItemTooltip("Earlier rules win");
    ImGui::SameLine();
    if (ImGui::Button("Down") && valid && selected + 1 < static_cast<int>(rules.size())) {
        std::swap(rules[selected], rules[selected + 1]);
        selected++;
    }
    ImGui::EndDisabled();
    selected = std::clamp(selected, 0, std::max(0, static_cast<int>(rules.size()) - 1));
    ImGui::PopID();
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
    to.round_wires = from.round_wires;
    to.wire_arrows = from.wire_arrows;
    to.animate_selection = from.animate_selection;
    to.signal_rules = from.signal_rules;
    to.cell_rules = from.cell_rules;
}

bool PreferencesWindow::same_config(const Settings& a, const Settings& b) {
    return a.theme == b.theme && a.ui_scale == b.ui_scale && a.min_zoom == b.min_zoom &&
           a.max_zoom == b.max_zoom && a.max_schematic_nodes == b.max_schematic_nodes &&
           a.signal_rules == b.signal_rules && a.cell_rules == b.cell_rules &&
           a.round_wires == b.round_wires && a.wire_arrows == b.wire_arrows &&
           a.animate_selection == b.animate_selection;
}

void PreferencesWindow::draw(Settings& settings) {
    if (!open_)
        return;
    if (just_opened_) {
        copy_config(settings, draft_);
        just_opened_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, ImGui::GetFontSize() * 34),
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
    } sections[] = {{"Appearance", Section::Appearance},
                    {"Canvas", Section::Canvas},
                    {"Rules", Section::Rules}};
    for (const auto& s : sections) {
        if (ImGui::Selectable(s.name, section_ == s.section))
            section_ = s.section;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: the selected section's settings (edits the draft).
    ImGui::BeginChild("settings", ImVec2(0, -footer));
    switch (section_) {
    case Section::Appearance: draw_appearance(draft_); break;
    case Section::Canvas: draw_canvas(draft_); break;
    case Section::Rules: draw_rules(draft_); break;
    }
    ImGui::EndChild();

    // Footer: Reset on the left; OK, Cancel, Apply on the right.
    if (ImGui::Button("Reset section to defaults")) {
        Settings defaults;
        if (section_ == Section::Appearance) {
            draft_.theme = defaults.theme;
            draft_.ui_scale = defaults.ui_scale;
        } else if (section_ == Section::Canvas) {
            draft_.min_zoom = defaults.min_zoom;
            draft_.max_zoom = defaults.max_zoom;
            draft_.max_schematic_nodes = defaults.max_schematic_nodes;
            draft_.round_wires = defaults.round_wires;
            draft_.wire_arrows = defaults.wire_arrows;
            draft_.animate_selection = defaults.animate_selection;
        } else {
            draft_.signal_rules = defaults.signal_rules;
            draft_.cell_rules = defaults.cell_rules;
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

    ImGui::SeparatorText("Wires");
    ImGui::Checkbox("Rounded corners", &settings.round_wires);
    ImGui::SetItemTooltip("Draw bends in wires as small arcs");
    ImGui::Checkbox("Direction arrows", &settings.wire_arrows);
    ImGui::SetItemTooltip("Arrowheads where wires leave their drivers and enter their loads");
    ImGui::Checkbox("Animate the selected net", &settings.animate_selection);
    ImGui::SetItemTooltip("Dashes moving along the selected net, from its driver to its loads.\n"
                          "The window redraws continuously while a net is selected.");
}

bool PreferencesWindow::list_field(const char* label, const std::string& formatted,
                                   std::string& out) {
    ImGuiID id = ImGui::GetID(label);
    std::string& text = texts_[id];
    if (ImGui::GetActiveID() != id)
        text = formatted;
    if (!ImGui::InputText(label, &text))
        return false;
    out = text;
    return true;
}

void PreferencesWindow::draw_rules(Settings& settings) {
    ImGui::TextWrapped("Rules style the schematic. The first enabled rule that matches wins. "
                       "Patterns are case-insensitive, with * and ?; separate several with "
                       "commas. Attributes are name=value (or just name for any value).");

    ImGui::SeparatorText("Signals");
    rule_list("signals", settings.signal_rules, signal_selected_);
    if (!settings.signal_rules.empty())
        draw_signal_rule(settings.signal_rules[signal_selected_]);

    ImGui::SeparatorText("Cells");
    rule_list("cells", settings.cell_rules, cell_selected_);
    if (!settings.cell_rules.empty())
        draw_cell_rule(settings.cell_rules[cell_selected_]);
}

void PreferencesWindow::draw_signal_rule(SignalRule& rule) {
    ImGui::PushID("signal_rule");
    if (begin_rows("signal_rule")) {
        std::string text;
        row_label("Name");
        ImGui::InputText("##name", &rule.name);
        row_label("Net names");
        if (list_field("##nets", join_list(rule.nets), text))
            rule.nets = split_list(text);
        ImGui::SetItemTooltip("Names of wires on the net, e.g. *clk*, *clock*");
        row_label("Attributes");
        if (list_field("##attrs", attributes_text(rule.attributes), text))
            rule.attributes = parse_attributes(text);
        ImGui::SetItemTooltip("Attributes of wires on the net, e.g. keep=1, clock_*");
        row_label("Cell pins");
        if (list_field("##pins", cell_pins_text(rule.cell_pins), text))
            rule.cell_pins = parse_cell_pins(text);
        ImGui::SetItemTooltip("Nets connected to these pins, as cell type:pin name,\n"
                              "e.g. *dff*:CLK, $_DFF_*:C");
        row_label("Color");
        color_edit("##color", rule.color);
        row_label("Width");
        ImGui::SliderFloat("##width", &rule.width, 0.5f, 4.0f, "%.1fx");
        row_label("Line");
        int line = static_cast<int>(rule.line);
        if (ImGui::Combo("##line", &line, "Solid\0Dashed\0"))
            rule.line = static_cast<LineStyle>(line);
        row_label("Pin marker");
        int marker = static_cast<int>(rule.marker);
        if (ImGui::Combo("##marker", &marker, "None\0Clock (>)\0"))
            rule.marker = static_cast<PinMarker>(marker);
        row_label("Placement");
        int placement = static_cast<int>(rule.placement);
        if (ImGui::Combo("##placement", &placement, "Routed\0Label\0"))
            rule.placement = static_cast<NetPlacement>(placement);
        ImGui::SetItemTooltip("Label: no wire, the net's name at each pin instead\n"
                              "(for nets that go everywhere, like clocks)");
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void PreferencesWindow::draw_cell_rule(CellRule& rule) {
    ImGui::PushID("cell_rule");
    if (begin_rows("cell_rule")) {
        std::string text;
        row_label("Name");
        ImGui::InputText("##name", &rule.name);
        row_label("Cell types");
        if (list_field("##types", join_list(rule.types), text))
            rule.types = split_list(text);
        ImGui::SetItemTooltip("e.g. *dff*, $_DFF_*, sky130_fd_sc_hd__*");
        row_label("Instance names");
        if (list_field("##names", join_list(rule.names), text))
            rule.names = split_list(text);
        row_label("Attributes");
        if (list_field("##attrs", attributes_text(rule.attributes), text))
            rule.attributes = parse_attributes(text);
        row_label("Color");
        color_edit("##color", rule.color);
        row_label("Border");
        int border = static_cast<int>(rule.border);
        if (ImGui::Combo("##border", &border, "Solid\0Dashed\0Dotted\0None\0"))
            rule.border = static_cast<BorderStyle>(border);
        row_label("Border width");
        ImGui::SliderFloat("##border_width", &rule.border_width, 0.5f, 4.0f, "%.1fx");
        row_label("Show type");
        ImGui::Checkbox("##show_type", &rule.show_type);
        ImGui::EndTable();
    }
    ImGui::PopID();
}

} // namespace netviz
