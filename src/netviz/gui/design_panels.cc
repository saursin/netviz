#include "netviz/gui/design_panels.h"

#include "netviz/gui/fonts.h"
#include "netviz/gui/icons.h"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace netviz {

namespace {

constexpr size_t kMaxChildren = 1000; // instances listed per tree node

// Bits values that are plain numbers are shown in decimal.
std::string format_value(const Value& value) {
    if (value.kind == Value::Kind::String)
        return std::format("\"{}\"", value.text);
    if (std::optional<uint64_t> n = as_int(value))
        return std::to_string(*n);
    return value.text;
}

// "[7:4]" for a 4-bit wire with offset 4; empty for a plain 1-bit wire.
std::string bit_range(const Wire& wire) {
    if (wire.width == 1 && wire.start_offset == 0)
        return "";
    int64_t first = bit_index(wire, 0);
    int64_t last = bit_index(wire, wire.width - 1);
    return std::format("[{}:{}]", last, first);
}

void values_table(const char* id, const StringPool& names, const std::vector<NamedValue>& values) {
    if (values.empty()) {
        ImGui::TextDisabled("none");
        return;
    }
    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const NamedValue& nv : values) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(names.str(nv.name).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_value(nv.value).c_str());
        }
        ImGui::EndTable();
    }
}

// A small expandable list ("Parameters (3)"), without a colored header bar.
void optional_values(const char* label, const char* id, const StringPool& names,
                     const std::vector<NamedValue>& values) {
    std::string text = std::format("{} ({})##{}", label, values.size(), id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (values.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (ImGui::TreeNodeEx(text.c_str(), flags)) {
        if (!values.empty())
            values_table(id, names, values);
        ImGui::TreePop();
    }
}

} // namespace

InstanceLists find_instances(const Design& design) {
    InstanceLists lists(design.modules().size());
    for (uint32_t m = 0; m < design.modules().size(); m++) {
        const Module& module = design.modules()[m];
        for (uint32_t c = 0; c < module.cells().size(); c++) {
            // Blackbox modules are library cells (gates), not part of the hierarchy.
            std::optional<ModuleId> type = design.find_module(module.cells()[c].type);
            if (type && !design.module(*type).is_blackbox())
                lists[m].push_back(CellId(c));
        }
    }
    return lists;
}

void DesignPanels::set_design(std::unique_ptr<Design> design, InstanceLists instances,
                              std::string file) {
    design_ = std::move(design);
    instances_ = std::move(instances);
    file_ = std::move(file);
    selection_.reset();
    connectivity_.reset();
    if (design_ && design_->top())
        selection_ = Selection::place({}, *design_->top());
}

// A hierarchy node is highlighted when it is the selected place, or when the
// selected object is the instance cell that this node stands for.
bool DesignPanels::is_selected(ModuleId module, const std::vector<CellId>& path) const {
    if (!selection_)
        return false;
    if (selection_->kind == Selection::Kind::None)
        return selection_->module == module && selection_->path == path;
    if (selection_->kind == Selection::Kind::Cell && path.size() == selection_->path.size() + 1 &&
        path.back() == selection_->cell)
        return std::equal(selection_->path.begin(), selection_->path.end(), path.begin());
    return false;
}

ModuleId DesignPanels::module_at(const std::vector<CellId>& path) const {
    ModuleId module = *design_->top();
    for (CellId cell : path)
        module = *design_->find_module(design_->module(module).cell(cell).type);
    return module;
}

std::vector<std::string> DesignPanels::path_labels(const std::vector<CellId>& path) const {
    std::vector<std::string> labels;
    ModuleId module = *design_->top();
    labels.push_back(design_->names().str(design_->module(module).name()));
    for (CellId cell_id : path) {
        const Cell& cell = design_->module(module).cell(cell_id);
        labels.push_back(design_->names().str(cell.name));
        module = *design_->find_module(cell.type);
    }
    return labels;
}

// "top.u1.u2" for an instance path.
std::string DesignPanels::path_name(const std::vector<CellId>& path) const {
    const StringPool& names = design_->names();
    ModuleId module = *design_->top();
    std::string text = names.str(design_->module(module).name());
    for (CellId cell_id : path) {
        const Cell& cell = design_->module(module).cell(cell_id);
        text += '.';
        text += names.str(cell.name);
        module = *design_->find_module(cell.type);
    }
    return text;
}

void DesignPanels::item_actions(const Selection& place, bool clicked) {
    bool middle = ImGui::IsItemClicked(ImGuiMouseButton_Middle);
    std::optional<bool> new_canvas;
    if (clicked || middle)
        new_canvas = middle || ImGui::GetIO().KeyCtrl;
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Open"))
            new_canvas = false;
        if (ImGui::MenuItem("Open in New Canvas", "Ctrl+Click"))
            new_canvas = true;
        ImGui::EndPopup();
    }
    if (new_canvas) {
        selection_ = place;
        open_requests_.push_back({place, *new_canvas});
    }
}

void DesignPanels::draw_open_mark(const std::vector<CellId>& path, ModuleId module) {
    const OpenPlace* shown = nullptr;
    int count = 0;
    for (const OpenPlace& p : open_places_)
        if (p.module == module && p.path == path) {
            shown = shown ? shown : &p;
            count++;
        }
    if (!shown)
        return;
    // At the right edge of the row, sized like a text item so it lines up
    // with the label; the icon is centered on what is visibly drawn.
    ImVec2 size(ImGui::CalcTextSize(icons::kShown).x + 4.0f, ImGui::GetTextLineHeight());
    ImGui::SameLine();
    float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - size.x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right));
    if (ImGui::InvisibleButton("shown", size))
        ImGui::SetWindowFocus(shown->window.c_str());
    ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImU32 color =
        ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    draw_icon(ImGui::GetWindowDrawList(), icons::kShown,
              ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), color);
    if (count > 1)
        ImGui::SetItemTooltip("Shown in %d canvases; click to bring one to the front", count);
    else
        ImGui::SetItemTooltip("Shown in a canvas; click to bring it to the front");
}

// -- Design (hierarchy) panel ------------------------------------------------

void DesignPanels::draw_instance_tree(ModuleId module, const char* name, const char* type,
                                      std::vector<CellId>& path) {
    const std::vector<CellId>& children = instances_[module.index];
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (path.empty())
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    if (children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (is_selected(module, path))
        flags |= ImGuiTreeNodeFlags_Selected;

    bool open = ImGui::TreeNodeEx(name, flags);
    item_actions(Selection::place(path, module),
                 ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen());
    if (type) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", type);
    }
    draw_open_mark(path, module);
    if (!open)
        return;

    const Module& m = design_->module(module);
    const StringPool& names = design_->names();
    for (size_t i = 0; i < children.size() && i < kMaxChildren; i++) {
        const Cell& cell = m.cell(children[i]);
        ModuleId child = *design_->find_module(cell.type);
        ImGui::PushID(static_cast<int>(children[i].index));
        path.push_back(children[i]);
        draw_instance_tree(child, names.str(cell.name).c_str(), names.str(cell.type).c_str(), path);
        path.pop_back();
        ImGui::PopID();
    }
    if (children.size() > kMaxChildren)
        ImGui::TextDisabled("... %zu more", children.size() - kMaxChildren);
    ImGui::TreePop();
}

void DesignPanels::draw_module_list() {
    const StringPool& names = design_->names();
    std::span<const Module> modules = design_->modules();
    auto item = [&](uint32_t m) {
        const Module& module = modules[m];
        std::string label = std::format("{}##m{}", names.str(module.name()), m);
        bool clicked = ImGui::Selectable(label.c_str(), is_selected(ModuleId(m), {}));
        item_actions(Selection::place({}, ModuleId(m)), clicked);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu cells", module.cells().size());
        draw_open_mark({}, ModuleId(m));
    };

    size_t blackboxes = 0;
    for (uint32_t m = 0; m < modules.size(); m++) {
        if (modules[m].is_blackbox())
            blackboxes++;
        else
            item(m);
    }
    if (blackboxes > 0 && ImGui::TreeNode("blackboxes", "Blackboxes (%zu)", blackboxes)) {
        for (uint32_t m = 0; m < modules.size(); m++) {
            if (modules[m].is_blackbox())
                item(m);
        }
        ImGui::TreePop();
    }
}

void DesignPanels::draw_hierarchy(bool* open) {
    if (!ImGui::Begin(kHierarchyWindow, open)) {
        ImGui::End();
        return;
    }
    if (!design_) {
        ImGui::TextDisabled("No design loaded");
    } else if (std::optional<ModuleId> top = design_->top()) {
        std::vector<CellId> path;
        const std::string& name = design_->names().str(design_->module(*top).name());
        draw_instance_tree(*top, name.c_str(), nullptr, path);
    } else {
        ImGui::TextDisabled("No top module (see Modules)");
    }
    ImGui::End();
}

void DesignPanels::draw_modules(bool* open) {
    if (!ImGui::Begin(kModulesWindow, open)) {
        ImGui::End();
        return;
    }
    if (!design_)
        ImGui::TextDisabled("No design loaded");
    else
        draw_module_list();
    ImGui::End();
}

// -- Properties panel ---------------------------------------------------------

void DesignPanels::draw_properties(bool* open) {
    if (!ImGui::Begin(kPropertiesWindow, open)) {
        ImGui::End();
        return;
    }
    if (!design_ || !selection_) {
        ImGui::TextDisabled("Nothing selected");
        ImGui::End();
        return;
    }
    const Module& module = design_->module(selection_->module);
    switch (selection_->kind) {
    case Selection::Kind::Cell: draw_cell_properties(module, selection_->cell); break;
    case Selection::Kind::Port: draw_port_properties(module, selection_->port); break;
    case Selection::Kind::None: draw_module_properties(module, selection_->path); break;
    }
    ImGui::End();
}

void DesignPanels::draw_module_properties(const Module& module, const std::vector<CellId>& path) {
    const StringPool& names = design_->names();

    if (!path.empty()) {
        ImGui::TextDisabled("Instance");
        ImGui::TextWrapped("%s", path_name(path).c_str());
    }
    ImGui::TextDisabled(path.empty() ? "Module" : "Module (type)");
    ImGui::TextWrapped("%s%s", names.str(module.name()).c_str(),
                       module.is_blackbox() ? "  (blackbox)" : "");
    ImGui::Separator();

    ImGui::Text("%zu cells, %zu wires, %zu ports, %u signal bits", module.cells().size(),
                module.wires().size(), module.ports().size(), module.signal_count());
    ImGui::Text("%zu submodule instances", instances_[selection_->module.index].size());

    ImGui::SeparatorText("Ports");
    {
        ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("ports", 3, flags)) {
            for (WireId port : module.ports()) {
                const Wire& wire = module.wire(port);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(names.str(wire.name).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(direction_name(wire.direction));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(bit_range(wire).c_str());
            }
            ImGui::EndTable();
        }
    }

    // The instance's own parameters and attributes live on the cell in its parent.
    if (!path.empty()) {
        ModuleId parent = *design_->top();
        for (size_t i = 0; i + 1 < path.size(); i++)
            parent = *design_->find_module(design_->module(parent).cell(path[i]).type);
        const Cell& cell = design_->module(parent).cell(path.back());
        ImGui::SeparatorText("Instance");
        optional_values("Parameters", "inst_params", names, cell.parameters);
        optional_values("Attributes", "inst_attrs", names, cell.attributes);
    }
    ImGui::SeparatorText("Module");
    optional_values("Parameters", "mod_params", names, module.parameters());
    optional_values("Attributes", "mod_attrs", names, module.attributes());
}

std::string DesignPanels::describe_bits(const Module& module, ModuleId id,
                                        std::span<const SigBit> bits) {
    if (bits.empty())
        return "";
    if (!connectivity_ || connectivity_module_ != id) {
        connectivity_ = std::make_unique<Connectivity>(module);
        connectivity_module_ = id;
    }
    const StringPool& names = design_->names();
    // A name for one bit: prefer a wire whose name is not auto-generated.
    auto name_of = [&](SigBit bit, WireBit* out) -> bool {
        if (bit.is_constant())
            return false;
        std::span<const WireBit> refs = connectivity_->wire_bits(bit.signal());
        if (refs.empty())
            return false;
        *out = refs[0];
        for (const WireBit& ref : refs)
            if (!module.wire(ref.wire).hidden) {
                *out = ref;
                break;
            }
        return true;
    };
    // All bits consecutive in one wire: "w[hi:lo]" (or just "w" for the whole wire).
    WireBit first;
    bool whole = name_of(bits[0], &first);
    for (size_t i = 1; whole && i < bits.size(); i++) {
        WireBit ref;
        whole = name_of(bits[i], &ref) && ref.wire == first.wire && ref.offset == first.offset + i;
    }
    if (whole) {
        const Wire& wire = module.wire(first.wire);
        const std::string& name = names.str(wire.name);
        if (bits.size() == wire.width && wire.width > 1)
            return name;
        int64_t lo = bit_index(wire, first.offset);
        int64_t hi = bit_index(wire, first.offset + static_cast<uint32_t>(bits.size()) - 1);
        if (wire.width == 1)
            return name;
        return bits.size() == 1
                   ? std::format("{}[{}]", name, lo)
                   : std::format("{}[{}:{}]", name, std::max(lo, hi), std::min(lo, hi));
    }
    std::string text;
    for (size_t i = 0; i < bits.size() && i < 3; i++) {
        WireBit ref;
        if (i)
            text += ", ";
        if (bits[i].is_constant())
            text += "const";
        else if (name_of(bits[i], &ref))
            text += std::format("{}[{}]", names.str(module.wire(ref.wire).name),
                                bit_index(module.wire(ref.wire), ref.offset));
        else
            text += "?";
    }
    if (bits.size() > 3)
        text += std::format(", ... ({} bits)", bits.size());
    return text;
}

void DesignPanels::draw_cell_properties(const Module& module, CellId id) {
    const StringPool& names = design_->names();
    const Cell& cell = module.cell(id);
    std::optional<ModuleId> type = design_->find_module(cell.type);
    bool instance = type && !design_->module(*type).is_blackbox();
    ImGui::TextDisabled(instance ? "Instance" : "Cell");
    ImGui::TextWrapped("%s", names.str(cell.name).c_str());
    ImGui::TextDisabled("Type");
    ImGui::TextWrapped("%s", names.str(cell.type).c_str());
    ImGui::TextDisabled("In module");
    ImGui::TextWrapped("%s", names.str(module.name()).c_str());

    ImGui::SeparatorText("Pins");
    if (ImGui::BeginTable("pins", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const Pin& pin : module.pins(cell)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(names.str(pin.port).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(direction_name(pin.direction));
            ImGui::TableNextColumn();
            ImGui::Text("%u", pin.width);
            ImGui::TableNextColumn();
            std::string text = describe_bits(module, selection_->module, module.bits(pin));
            ImGui::TextUnformatted(text.c_str());
            ImGui::SetItemTooltip("%s", text.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Cell");
    optional_values("Parameters", "cell_params", names, cell.parameters);
    optional_values("Attributes", "cell_attrs", names, cell.attributes);
}

void DesignPanels::draw_port_properties(const Module& module, WireId id) {
    const StringPool& names = design_->names();
    const Wire& wire = module.wire(id);
    ImGui::TextDisabled("Port");
    ImGui::TextWrapped("%s %s", names.str(wire.name).c_str(), bit_range(wire).c_str());
    ImGui::TextDisabled("Direction");
    ImGui::TextUnformatted(direction_name(wire.direction));
    ImGui::TextDisabled("Width");
    ImGui::Text("%u bit%s%s", wire.width, wire.width == 1 ? "" : "s",
                wire.is_signed ? ", signed" : "");
    ImGui::TextDisabled("In module");
    ImGui::TextWrapped("%s", names.str(module.name()).c_str());
    ImGui::SeparatorText("Port");
    optional_values("Attributes", "port_attrs", names, wire.attributes);
}

} // namespace netviz
