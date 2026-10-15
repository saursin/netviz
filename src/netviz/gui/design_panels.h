#pragma once

#include "netviz/core/netlist.h"

#include <memory>
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
    static constexpr const char* kHierarchyWindow = "Hierarchy";
    static constexpr const char* kModulesWindow = "Modules";
    static constexpr const char* kPropertiesWindow = "Properties";

    void set_design(std::unique_ptr<Design> design, InstanceLists instances, std::string file);
    const Design* design() const { return design_.get(); }
    const std::string& file() const { return file_; }

    void draw_hierarchy(bool* open);
    void draw_modules(bool* open);
    void draw_properties(bool* open);

  private:
    // An instance in the hierarchy (path of instance cells from the top), or
    // a module definition when `path` is empty.
    struct Selection {
        ModuleId module;
        std::vector<CellId> path;
    };

    // `type` (the module name, shown dimmed after the instance name) may be null.
    void draw_instance_tree(ModuleId module, const char* name, const char* type,
                            std::vector<CellId>& path);
    void draw_module_list();
    bool is_selected(ModuleId module, const std::vector<CellId>& path) const;
    std::string path_name(const std::vector<CellId>& path) const;

    std::unique_ptr<Design> design_;
    InstanceLists instances_;
    std::string file_;
    std::optional<Selection> selection_;
};

} // namespace netviz
