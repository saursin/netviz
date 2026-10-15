#pragma once

#include "netviz/core/connectivity.h"
#include "netviz/core/netlist.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace netviz {

// For each module, the cells that instantiate another (non-blackbox) module
// of the design; computed once per loaded design for the hierarchy tree.
using InstanceLists = std::vector<std::vector<CellId>>;
InstanceLists find_instances(const Design& design);

// The loaded design and the panels that browse it: Hierarchy (instance tree),
// Modules (module definitions) and Properties (the selected object).
class DesignPanels {
  public:
    // What the user has selected: a place in the hierarchy (the instance path
    // from the top and the module there; an empty path is a module definition
    // from the Modules list) and optionally one cell or port inside it. The
    // path matters: the same cell in two instances is two selections.
    struct Selection {
        enum class Kind : uint8_t { None, Cell, Port };

        std::vector<CellId> path;
        ModuleId module;
        Kind kind = Kind::None;
        CellId cell;
        WireId port;

        bool operator==(const Selection&) const = default;

        // A place in the hierarchy, with nothing inside it selected.
        static Selection place(std::vector<CellId> path, ModuleId module) {
            Selection s;
            s.path = std::move(path);
            s.module = module;
            return s;
        }
    };

    static constexpr const char* kHierarchyWindow = "Hierarchy";
    static constexpr const char* kModulesWindow = "Modules";
    static constexpr const char* kPropertiesWindow = "Properties";

    void set_design(std::unique_ptr<Design> design, InstanceLists instances, std::string file);
    const Design* design() const { return design_.get(); }
    const std::string& file() const { return file_; }

    const std::optional<Selection>& selection() const { return selection_; }
    void select(Selection selection) { selection_ = std::move(selection); }
    // Module names along a path from the top, e.g. {"cpu", "dp", "alu"}.
    std::vector<std::string> path_labels(const std::vector<CellId>& path) const;
    // The module at the end of a path from the top.
    ModuleId module_at(const std::vector<CellId>& path) const;

    void draw_hierarchy(bool* open);
    void draw_modules(bool* open);
    void draw_properties(bool* open);

  private:
    // `type` (the module name, shown dimmed after the instance name) may be null.
    void draw_instance_tree(ModuleId module, const char* name, const char* type,
                            std::vector<CellId>& path);
    void draw_module_list();
    bool is_selected(ModuleId module, const std::vector<CellId>& path) const;
    std::string path_name(const std::vector<CellId>& path) const;
    void draw_module_properties(const Module& module, const std::vector<CellId>& path);
    void draw_cell_properties(const Module& module, CellId cell);
    void draw_port_properties(const Module& module, WireId port);
    // Wire names for a list of bits, e.g. "sum[3:0]" (connectivity is cached per module).
    std::string describe_bits(const Module& module, ModuleId id, std::span<const SigBit> bits);

    std::unique_ptr<Design> design_;
    InstanceLists instances_;
    std::string file_;
    std::optional<Selection> selection_;
    std::unique_ptr<Connectivity> connectivity_; // of connectivity_module_
    ModuleId connectivity_module_;
};

} // namespace netviz
