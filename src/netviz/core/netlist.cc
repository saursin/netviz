#include "netviz/core/netlist.h"

#include <ostream>

namespace netviz {

// ---------------------------------------------------------------------------
// StringPool
// ---------------------------------------------------------------------------

NameId StringPool::intern(std::string_view text) {
    auto it = index_.find(text);
    if (it != index_.end())
        return it->second;

    NameId id(static_cast<uint32_t>(strings_.size()));
    const std::string& stored = strings_.emplace_back(text);
    index_.emplace(stored, id);
    return id;
}

std::optional<NameId> StringPool::find(std::string_view text) const {
    auto it = index_.find(text);
    if (it == index_.end())
        return std::nullopt;
    return it->second;
}

const std::string& StringPool::str(NameId id) const {
    return strings_.at(id.index);
}

// ---------------------------------------------------------------------------
// Directions and values
// ---------------------------------------------------------------------------

const char* direction_name(Direction direction) {
    switch (direction) {
    case Direction::Unknown: return "unknown";
    case Direction::Input: return "input";
    case Direction::Output: return "output";
    case Direction::InOut: return "inout";
    }
    return "?";
}

std::optional<uint64_t> as_int(const Value& value) {
    if (value.kind != Value::Kind::Bits || value.text.empty() || value.text.size() > 64)
        return std::nullopt;

    uint64_t result = 0;
    for (char c : value.text) {
        if (c != '0' && c != '1')
            return std::nullopt;
        result = (result << 1) | static_cast<uint64_t>(c == '1');
    }
    return result;
}

const Value* NamedObject::attribute(NameId attr) const {
    for (const NamedValue& nv : attributes) {
        if (nv.name == attr)
            return &nv.value;
    }
    return nullptr;
}

int64_t bit_index(const Wire& wire, uint32_t offset) {
    assert(offset < wire.width);
    uint32_t position = wire.upto ? wire.width - 1 - offset : offset;
    return static_cast<int64_t>(wire.start_offset) + position;
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

SignalBitId Module::add_signals(uint32_t count) {
    assert(count <= kMaxSignals - signal_count_);
    SignalBitId first(signal_count_);
    signal_count_ += count;
    return first;
}

uint32_t Module::append_bits(std::span<const SigBit> bits) {
    uint32_t begin = static_cast<uint32_t>(bits_.size());
    for (SigBit bit : bits) {
        assert(bit.is_constant() || bit.signal().index < signal_count_);
        bits_.push_back(bit);
    }
    return begin;
}

std::optional<WireId> Module::add_wire(NameId name, std::span<const SigBit> bits,
                                       const WireOptions& options) {
    if (wire_index_.contains(name))
        return std::nullopt;

    WireId id(static_cast<uint32_t>(wires_.size()));
    Wire wire;
    wire.name = name;
    wire.hidden = options.hidden;
    wire.start_offset = options.start_offset;
    wire.upto = options.upto;
    wire.is_signed = options.is_signed;
    wire.bits_begin = append_bits(bits);
    wire.width = static_cast<uint32_t>(bits.size());
    wires_.push_back(std::move(wire));
    wire_index_.emplace(name, id);
    return id;
}

void Module::make_port(WireId id, Direction direction) {
    Wire& wire = wires_.at(id.index);
    assert(wire.port_index < 0 && "wire is already a port");
    wire.port_index = static_cast<int32_t>(ports_.size());
    wire.direction = direction;
    ports_.push_back(id);
}

std::optional<CellId> Module::add_cell(NameId name, NameId type, std::span<const NewPin> pins,
                                       bool hidden) {
    if (cell_index_.contains(name))
        return std::nullopt;

    CellId id(static_cast<uint32_t>(cells_.size()));
    Cell cell;
    cell.name = name;
    cell.type = type;
    cell.hidden = hidden;
    cell.pins_begin = static_cast<uint32_t>(pins_.size());
    cell.pin_count = static_cast<uint32_t>(pins.size());
    for (const NewPin& new_pin : pins) {
        Pin pin;
        pin.port = new_pin.port;
        pin.direction = new_pin.direction;
        pin.bits_begin = append_bits(new_pin.bits);
        pin.width = static_cast<uint32_t>(new_pin.bits.size());
        pins_.push_back(pin);
    }
    cells_.push_back(std::move(cell));
    cell_index_.emplace(name, id);
    return id;
}

void Module::set_pin_direction(CellId id, uint32_t pin, Direction direction) {
    const Cell& cell = cells_.at(id.index);
    assert(pin < cell.pin_count);
    pins_[cell.pins_begin + pin].direction = direction;
}

std::vector<NamedValue>& Module::wire_attributes(WireId id) {
    return wires_.at(id.index).attributes;
}

std::vector<NamedValue>& Module::cell_attributes(CellId id) {
    return cells_.at(id.index).attributes;
}

std::vector<NamedValue>& Module::cell_parameters(CellId id) {
    return cells_.at(id.index).parameters;
}

std::optional<WireId> Module::find_wire(NameId name) const {
    auto it = wire_index_.find(name);
    if (it == wire_index_.end())
        return std::nullopt;
    return it->second;
}

std::optional<CellId> Module::find_cell(NameId name) const {
    auto it = cell_index_.find(name);
    if (it == cell_index_.end())
        return std::nullopt;
    return it->second;
}

std::span<const Pin> Module::pins(const Cell& cell) const {
    return std::span<const Pin>(pins_).subspan(cell.pins_begin, cell.pin_count);
}

std::span<const SigBit> Module::bits(const Wire& wire) const {
    return std::span<const SigBit>(bits_).subspan(wire.bits_begin, wire.width);
}

std::span<const SigBit> Module::bits(const Pin& pin) const {
    return std::span<const SigBit>(bits_).subspan(pin.bits_begin, pin.width);
}

// ---------------------------------------------------------------------------
// Design
// ---------------------------------------------------------------------------

std::optional<ModuleId> Design::add_module(NameId name) {
    if (module_index_.contains(name))
        return std::nullopt;

    ModuleId id(static_cast<uint32_t>(modules_.size()));
    modules_.emplace_back(name);
    module_index_.emplace(name, id);
    return id;
}

std::optional<ModuleId> Design::find_module(NameId name) const {
    auto it = module_index_.find(name);
    if (it == module_index_.end())
        return std::nullopt;
    return it->second;
}

void Design::set_top(ModuleId id) {
    assert(id.index < modules_.size());
    top_ = id;
}

std::optional<ModuleId> Design::top() const {
    if (!top_.valid())
        return std::nullopt;
    return top_;
}

int Design::resolve_pin_directions() {
    int unknown = 0;
    for (Module& module : modules_) {
        for (uint32_t c = 0; c < module.cells().size(); c++) {
            CellId cell_id(c);
            const Cell& cell = module.cell(cell_id);
            std::optional<ModuleId> type = find_module(cell.type);
            std::span<const Pin> pins = module.pins(cell);

            for (uint32_t p = 0; p < pins.size(); p++) {
                if (pins[p].direction != Direction::Unknown)
                    continue;
                if (type) {
                    const Module& definition = modules_[type->index];
                    std::optional<WireId> port = definition.find_wire(pins[p].port);
                    if (port && definition.wire(*port).port_index >= 0)
                        module.set_pin_direction(cell_id, p, definition.wire(*port).direction);
                }
                if (pins[p].direction == Direction::Unknown)
                    unknown++;
            }
        }
    }
    return unknown;
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------

namespace {

void dump_bits(std::ostream& out, std::span<const SigBit> bits) {
    static const char* const kConstants[] = {"0", "1", "x", "z"};
    for (SigBit bit : bits) {
        out << ' ';
        if (bit.is_constant())
            out << kConstants[static_cast<int>(bit.constant())];
        else
            out << 's' << bit.signal().index;
    }
}

void dump_values(std::ostream& out, const StringPool& names, const char* label,
                 const std::vector<NamedValue>& values, const char* indent) {
    for (const NamedValue& nv : values) {
        out << indent << label << ' ' << names.str(nv.name) << " = ";
        if (nv.value.kind == Value::Kind::String)
            out << '"' << nv.value.text << '"';
        else
            out << nv.value.text;
        out << '\n';
    }
}

} // namespace

void dump(const Design& design, std::ostream& out) {
    for (uint32_t m = 0; m < design.modules().size(); m++)
        dump(design, ModuleId(m), out);
}

void dump(const Design& design, ModuleId id, std::ostream& out) {
    const StringPool& names = design.names();
    std::optional<ModuleId> top = design.top();
    const Module& module = design.module(id);
    out << "module " << names.str(module.name());
    if (top && *top == id)
        out << " (top)";
    if (module.is_blackbox())
        out << " (blackbox)";
    out << "\n  signals " << module.signal_count() << '\n';
    dump_values(out, names, "parameter", module.parameters(), "  ");
    dump_values(out, names, "attribute", module.attributes(), "  ");

    for (const Wire& wire : module.wires()) {
        out << "  wire " << names.str(wire.name) << " [" << wire.width << ']';
        if (wire.start_offset != 0)
            out << " offset " << wire.start_offset;
        if (wire.upto)
            out << " upto";
        if (wire.is_signed)
            out << " signed";
        if (wire.port_index >= 0)
            out << " port" << wire.port_index << ' ' << direction_name(wire.direction);
        if (wire.hidden)
            out << " hidden";
        out << ':';
        dump_bits(out, module.bits(wire));
        out << '\n';
        dump_values(out, names, "attribute", wire.attributes, "    ");
    }

    for (const Cell& cell : module.cells()) {
        out << "  cell " << names.str(cell.name) << ' ' << names.str(cell.type);
        if (cell.hidden)
            out << " hidden";
        out << '\n';
        dump_values(out, names, "parameter", cell.parameters, "    ");
        dump_values(out, names, "attribute", cell.attributes, "    ");
        for (const Pin& pin : module.pins(cell)) {
            out << "    " << names.str(pin.port) << ' ' << direction_name(pin.direction) << ':';
            dump_bits(out, module.bits(pin));
            out << '\n';
        }
    }
}

} // namespace netviz
