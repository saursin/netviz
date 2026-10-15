#include "netviz/view/display_graph.h"

#include "netviz/core/connectivity.h"

#include <algorithm>
#include <unordered_map>

namespace netviz {

namespace {

// "4'b01x0" (MSB first) when every bit is a constant; empty otherwise.
std::string constant_text(std::span<const SigBit> bits) {
    static const char kChars[] = {'0', '1', 'x', 'z'};
    std::string text = std::to_string(bits.size()) + "'b";
    for (size_t i = bits.size(); i-- > 0;) {
        if (!bits[i].is_constant())
            return "";
        text += kChars[static_cast<int>(bits[i].constant())];
    }
    return bits.empty() ? "" : text;
}

// Hash of a list of pin ids (the endpoint set of a bit).
struct KeyHash {
    size_t operator()(const std::vector<uint32_t>& key) const {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t v : key)
            h = (h ^ v) * 1099511628211ull;
        return static_cast<size_t>(h);
    }
};

} // namespace

DisplayGraph DisplayGraph::build(const Design& design, ModuleId id) {
    const Module& module = design.module(id);
    DisplayGraph g;
    g.module_ = id;
    g.cell_count_ = static_cast<uint32_t>(module.cells().size());
    g.nodes_.reserve(module.cells().size() + module.ports().size());

    auto add_pin = [&](DisplayNode& node, DisplayPin pin) {
        uint32_t& count = pin.side == PinSide::Left ? node.left_pins : node.right_pins;
        pin.order = count++;
        g.pins_.push_back(std::move(pin));
        node.pin_count++;
    };

    // Cells, in IR order (so a cell's node id equals its CellId index).
    for (uint32_t c = 0; c < module.cells().size(); c++) {
        const Cell& cell = module.cells()[c];
        DisplayNode node;
        std::optional<ModuleId> type = design.find_module(cell.type);
        node.kind =
            (type && !design.module(*type).is_blackbox()) ? NodeKind::Instance : NodeKind::Cell;
        node.cell = CellId(c);
        node.name = cell.name;
        node.type = cell.type;
        node.hidden_name = cell.hidden;
        node.pins_begin = static_cast<uint32_t>(g.pins_.size());
        DisplayNodeId node_id(static_cast<uint32_t>(g.nodes_.size()));
        for (const Pin& ir_pin : module.pins(cell)) {
            DisplayPin pin;
            pin.node = node_id;
            pin.name = ir_pin.port;
            pin.direction = ir_pin.direction;
            pin.side = ir_pin.direction == Direction::Output ? PinSide::Right : PinSide::Left;
            pin.width = ir_pin.width;
            pin.constant = constant_text(module.bits(ir_pin));
            add_pin(node, std::move(pin));
        }
        g.nodes_.push_back(node);
    }

    // Module ports: inputs (and inout/unknown) drive into the module from the
    // left edge, so their pin is on their right; outputs are on the right edge.
    for (WireId port : module.ports()) {
        const Wire& wire = module.wire(port);
        DisplayNode node;
        node.kind = wire.direction == Direction::Input    ? NodeKind::InputPort
                    : wire.direction == Direction::Output ? NodeKind::OutputPort
                                                          : NodeKind::InOutPort;
        node.port = port;
        node.name = wire.name;
        node.pins_begin = static_cast<uint32_t>(g.pins_.size());
        DisplayPin pin;
        pin.node = DisplayNodeId(static_cast<uint32_t>(g.nodes_.size()));
        pin.direction = wire.direction;
        pin.side = node.kind == NodeKind::OutputPort ? PinSide::Left : PinSide::Right;
        pin.width = wire.width;
        add_pin(node, std::move(pin));
        g.nodes_.push_back(node);
    }

    // Nets: bundle bits whose endpoint pins are exactly the same set.
    Connectivity conn(module);
    auto pin_of = [&](const Endpoint& ep) {
        if (ep.kind() == Endpoint::Kind::CellPin)
            return g.nodes_[ep.cell().index].pins_begin + ep.pin();
        uint32_t port_node = g.cell_count_ + module.wire(ep.port()).port_index;
        return g.nodes_[port_node].pins_begin;
    };
    std::unordered_map<std::vector<uint32_t>, uint32_t, KeyHash> net_of_key;
    std::vector<std::vector<uint32_t>> keys;       // endpoint pins of each net
    std::vector<std::vector<SignalBitId>> bits_of; // bits of each net
    std::vector<uint32_t> key;
    for (uint32_t b = 0; b < module.signal_count(); b++) {
        SignalBitId bit(b);
        key.clear();
        for (auto list : {conn.drivers(bit), conn.loads(bit), conn.unknown(bit)})
            for (const Endpoint& ep : list)
                key.push_back(pin_of(ep));
        std::sort(key.begin(), key.end());
        key.erase(std::unique(key.begin(), key.end()), key.end());
        if (key.size() < 2)
            continue; // nothing to connect
        auto [it, inserted] = net_of_key.try_emplace(key, static_cast<uint32_t>(keys.size()));
        if (inserted) {
            keys.push_back(key);
            bits_of.emplace_back();
        }
        bits_of[it->second].push_back(bit);
    }

    g.nets_.reserve(keys.size());
    for (size_t n = 0; n < keys.size(); n++) {
        DisplayNet net;
        net.bits_begin = static_cast<uint32_t>(g.net_bits_.size());
        net.bit_count = static_cast<uint32_t>(bits_of[n].size());
        g.net_bits_.insert(g.net_bits_.end(), bits_of[n].begin(), bits_of[n].end());

        net.drivers_begin = static_cast<uint32_t>(g.net_pins_.size());
        for (uint32_t p : keys[n])
            if (g.pins_[p].side == PinSide::Right)
                g.net_pins_.push_back(DisplayPinId(p));
        net.driver_count = static_cast<uint32_t>(g.net_pins_.size()) - net.drivers_begin;

        net.loads_begin = static_cast<uint32_t>(g.net_pins_.size());
        for (uint32_t p : keys[n])
            if (g.pins_[p].side == PinSide::Left)
                g.net_pins_.push_back(DisplayPinId(p));
        net.load_count = static_cast<uint32_t>(g.net_pins_.size()) - net.loads_begin;
        g.nets_.push_back(net);
    }
    return g;
}

std::span<const DisplayPin> DisplayGraph::pins_of(const DisplayNode& node) const {
    return std::span<const DisplayPin>(pins_).subspan(node.pins_begin, node.pin_count);
}

std::span<const SignalBitId> DisplayGraph::bits(const DisplayNet& net) const {
    return std::span<const SignalBitId>(net_bits_).subspan(net.bits_begin, net.bit_count);
}

std::span<const DisplayPinId> DisplayGraph::drivers(const DisplayNet& net) const {
    return std::span<const DisplayPinId>(net_pins_).subspan(net.drivers_begin, net.driver_count);
}

std::span<const DisplayPinId> DisplayGraph::loads(const DisplayNet& net) const {
    return std::span<const DisplayPinId>(net_pins_).subspan(net.loads_begin, net.load_count);
}

DisplayNodeId DisplayGraph::node_of_port(const Module& module, WireId port) const {
    return DisplayNodeId(cell_count_ + static_cast<uint32_t>(module.wire(port).port_index));
}

} // namespace netviz
