#pragma once

// The display graph: what a schematic of one module shows, derived from the
// IR (which it never modifies). Nodes are cells and module ports, pins are
// their connection points, and nets connect pins. See docs/LAYOUT.md.

#include "netviz/core/netlist.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace netviz {

using DisplayNodeId = Id<struct DisplayNodeTag>;
using DisplayPinId = Id<struct DisplayPinTag>;
using DisplayNetId = Id<struct DisplayNetTag>;

enum class NodeKind : uint8_t {
    Cell,      // a gate or library cell
    Instance,  // an instance of another (non-blackbox) module of the design
    InputPort, // module ports, drawn at the edges of the schematic
    OutputPort,
    InOutPort,
};

// Inputs (and Unknown/InOut pins) are on the left of a node, outputs on the
// right; signals flow left to right. A pin on the right drives its net.
enum class PinSide : uint8_t { Left, Right };

struct DisplayNode {
    NodeKind kind = NodeKind::Cell;
    CellId cell;              // for Cell / Instance
    WireId port;              // for ports
    NameId name;              // instance or port name
    NameId type;              // cell type; invalid for ports
    bool hidden_name = false; // auto-generated name: not drawn (shown in tooltips)
    uint32_t pins_begin = 0;
    uint32_t pin_count = 0;
    uint32_t left_pins = 0; // number of pins on each side
    uint32_t right_pins = 0;
};

struct DisplayPin {
    DisplayNodeId node;
    NameId name; // pin name; invalid for port nodes
    Direction direction = Direction::Unknown;
    PinSide side = PinSide::Left;
    uint32_t order = 0;   // position on its side, top to bottom
    uint32_t width = 0;   // bits
    std::string constant; // e.g. "1'b0" when every bit is a constant, else empty
};

// One drawn wire: bits that have exactly the same set of endpoint pins, so
// bundling them never shows a connection that does not exist.
struct DisplayNet {
    uint32_t bits_begin = 0;
    uint32_t bit_count = 0;
    uint32_t drivers_begin = 0; // pins on the right side of their node
    uint32_t driver_count = 0;
    uint32_t loads_begin = 0; // pins on the left side of their node
    uint32_t load_count = 0;
};

class DisplayGraph {
  public:
    // Builds the display graph of `module`.
    static DisplayGraph build(const Design& design, ModuleId module);

    ModuleId module() const { return module_; }

    std::span<const DisplayNode> nodes() const { return nodes_; }
    std::span<const DisplayPin> pins() const { return pins_; }
    std::span<const DisplayNet> nets() const { return nets_; }

    const DisplayNode& node(DisplayNodeId id) const { return nodes_.at(id.index); }
    const DisplayPin& pin(DisplayPinId id) const { return pins_.at(id.index); }
    const DisplayNet& net(DisplayNetId id) const { return nets_.at(id.index); }

    // A node's pins are stored together: ids pins_begin .. pins_begin + pin_count.
    std::span<const DisplayPin> pins_of(const DisplayNode& node) const;
    std::span<const SignalBitId> bits(const DisplayNet& net) const;
    std::span<const DisplayPinId> drivers(const DisplayNet& net) const;
    std::span<const DisplayPinId> loads(const DisplayNet& net) const;

    // The display node of a cell or module port.
    DisplayNodeId node_of_cell(CellId cell) const { return DisplayNodeId(cell.index); }
    DisplayNodeId node_of_port(const Module& module, WireId port) const;

  private:
    ModuleId module_;
    uint32_t cell_count_ = 0;
    std::vector<DisplayNode> nodes_;
    std::vector<DisplayPin> pins_;
    std::vector<DisplayNet> nets_;
    std::vector<SignalBitId> net_bits_;
    std::vector<DisplayPinId> net_pins_; // drivers and loads of each net
};

} // namespace netviz
