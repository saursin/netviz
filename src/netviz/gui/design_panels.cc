#include "netviz/gui/design_panels.h"

#include <imgui.h>

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
    if (design_ && design_->top())
        selection_ = Selection{*design_->top(), {}};
}

bool DesignPanels::is_selected(ModuleId module, const std::vector<CellId>& path) const {
    return selection_ && selection_->module == module && selection_->path == path;
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
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        selection_ = Selection{module, path};
    if (type) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", type);
    }
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
        if (ImGui::Selectable(label.c_str(), is_selected(ModuleId(m), {})))
            selection_ = Selection{ModuleId(m), {}};
        ImGui::SameLine();
        ImGui::TextDisabled("%zu cells", module.cells().size());
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

    const StringPool& names = design_->names();
    const Module& module = design_->module(selection_->module);
    const std::vector<CellId>& path = selection_->path;

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

    ImGui::End();
}

} // namespace netviz
