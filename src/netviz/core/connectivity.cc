#include "netviz/core/connectivity.h"

#include <format>
#include <map>

namespace netviz {

namespace {

// Calls fn(bit, endpoint, direction) for every non-constant bit of every
// module port and cell pin.
template <typename Fn> void for_each_endpoint(const Module& module, Fn fn) {
    for (WireId port : module.ports()) {
        const Wire& wire = module.wire(port);
        std::span<const SigBit> bits = module.bits(wire);
        for (uint32_t i = 0; i < bits.size(); i++) {
            if (!bits[i].is_constant())
                fn(bits[i].signal(), Endpoint::module_port(port, i), wire.direction, true);
        }
    }
    for (uint32_t c = 0; c < module.cells().size(); c++) {
        CellId cell(c);
        std::span<const Pin> pins = module.pins(module.cell(cell));
        for (uint32_t p = 0; p < pins.size(); p++) {
            std::span<const SigBit> bits = module.bits(pins[p]);
            for (uint32_t i = 0; i < bits.size(); i++) {
                if (!bits[i].is_constant())
                    fn(bits[i].signal(), Endpoint::cell_pin(cell, p, i), pins[p].direction, false);
            }
        }
    }
}

// Inside a module, a module input drives signals and a module output loads
// them; for cell pins it is the other way round.
bool is_driver(Direction direction, bool is_port) {
    if (direction == Direction::InOut)
        return true;
    return direction == (is_port ? Direction::Input : Direction::Output);
}

bool is_load(Direction direction, bool is_port) {
    if (direction == Direction::InOut)
        return true;
    return direction == (is_port ? Direction::Output : Direction::Input);
}

} // namespace

Connectivity::Connectivity(const Module& module) {
    const uint32_t n = module.signal_count();

    // Pass 1: count entries per signal bit (stored at offsets[bit + 1]).
    drivers_.offsets.assign(n + 1, 0);
    loads_.offsets.assign(n + 1, 0);
    unknown_.offsets.assign(n + 1, 0);
    wire_bits_.offsets.assign(n + 1, 0);

    for_each_endpoint(module, [&](SignalBitId bit, const Endpoint&, Direction dir, bool port) {
        if (dir == Direction::Unknown)
            unknown_.offsets[bit.index + 1]++;
        if (is_driver(dir, port))
            drivers_.offsets[bit.index + 1]++;
        if (is_load(dir, port))
            loads_.offsets[bit.index + 1]++;
    });
    for (const Wire& wire : module.wires()) {
        for (SigBit bit : module.bits(wire)) {
            if (!bit.is_constant())
                wire_bits_.offsets[bit.signal().index + 1]++;
        }
    }

    // Prefix sums, then pass 2: fill.
    const Endpoint placeholder = Endpoint::module_port(WireId(), 0);
    std::vector<uint32_t> d_next, l_next, u_next, w_next;
    for (auto* table : {&drivers_, &loads_, &unknown_}) {
        for (size_t i = 1; i < table->offsets.size(); i++)
            table->offsets[i] += table->offsets[i - 1];
        table->items.assign(table->offsets.back(), placeholder);
    }
    for (size_t i = 1; i < wire_bits_.offsets.size(); i++)
        wire_bits_.offsets[i] += wire_bits_.offsets[i - 1];
    wire_bits_.items.assign(wire_bits_.offsets.back(), WireBit{WireId(), 0});

    d_next.assign(drivers_.offsets.begin(), drivers_.offsets.end() - 1);
    l_next.assign(loads_.offsets.begin(), loads_.offsets.end() - 1);
    u_next.assign(unknown_.offsets.begin(), unknown_.offsets.end() - 1);
    w_next.assign(wire_bits_.offsets.begin(), wire_bits_.offsets.end() - 1);

    for_each_endpoint(module, [&](SignalBitId bit, const Endpoint& ep, Direction dir, bool port) {
        if (dir == Direction::Unknown)
            unknown_.items[u_next[bit.index]++] = ep;
        if (is_driver(dir, port))
            drivers_.items[d_next[bit.index]++] = ep;
        if (is_load(dir, port))
            loads_.items[l_next[bit.index]++] = ep;
    });
    for (uint32_t w = 0; w < module.wires().size(); w++) {
        std::span<const SigBit> bits = module.bits(module.wire(WireId(w)));
        for (uint32_t i = 0; i < bits.size(); i++) {
            if (!bits[i].is_constant())
                wire_bits_.items[w_next[bits[i].signal().index]++] = WireBit{WireId(w), i};
        }
    }
}

// ---------------------------------------------------------------------------
// validate
// ---------------------------------------------------------------------------

namespace {

constexpr size_t kMaxExamples = 5; // example bits listed in a summarized warning

// "s4 (q[0])": a signal bit with its first name, if it has one.
std::string describe_bit(const Design& design, const Module& module, const Connectivity& conn,
                         SignalBitId bit) {
    std::span<const WireBit> names = conn.wire_bits(bit);
    if (names.empty())
        return std::format("s{}", bit.index);
    const Wire& wire = module.wire(names[0].wire);
    const std::string& name = design.names().str(wire.name);
    if (wire.width == 1)
        return std::format("s{} ({})", bit.index, name);
    return std::format("s{} ({}[{}])", bit.index, name, names[0].offset);
}

// Adds one warning for a list of problem bits, naming up to kMaxExamples.
void summarize(std::vector<Issue>& issues, const Design& design, const Module& module,
               const Connectivity& conn, const std::vector<SignalBitId>& bits, const char* what) {
    if (bits.empty())
        return;
    std::string examples;
    for (size_t i = 0; i < bits.size() && i < kMaxExamples; i++) {
        if (i > 0)
            examples += ", ";
        examples += describe_bit(design, module, conn, bits[i]);
    }
    if (bits.size() > kMaxExamples)
        examples += ", ...";
    issues.push_back({Issue::Severity::Warning,
                      std::format("module {}: {} signal bit(s) {}: {}",
                                  design.names().str(module.name()), bits.size(), what, examples)});
}

} // namespace

std::vector<Issue> validate(const Design& design) {
    std::vector<Issue> issues;
    const StringPool& names = design.names();
    std::map<uint32_t, uint32_t> unknown_pins_by_type; // cell type NameId -> count

    for (const Module& module : design.modules()) {
        const std::string& module_name = names.str(module.name());

        // Instances of modules must match the module's ports.
        for (const Cell& cell : module.cells()) {
            std::optional<ModuleId> type = design.find_module(cell.type);
            for (const Pin& pin : module.pins(cell)) {
                if (pin.direction == Direction::Unknown)
                    unknown_pins_by_type[cell.type.index]++;
                if (!type)
                    continue;
                const Module& definition = design.module(*type);
                std::optional<WireId> port = definition.find_wire(pin.port);
                if (!port || definition.wire(*port).port_index < 0) {
                    issues.push_back({Issue::Severity::Error,
                                      std::format("module {}: cell {} has pin {}, but module {} "
                                                  "has no such port",
                                                  module_name, names.str(cell.name),
                                                  names.str(pin.port), names.str(cell.type))});
                } else if (definition.wire(*port).width != pin.width) {
                    issues.push_back(
                        {Issue::Severity::Error,
                         std::format("module {}: cell {} pin {} is {} bit(s) wide, "
                                     "but port {}.{} is {}",
                                     module_name, names.str(cell.name), names.str(pin.port),
                                     pin.width, names.str(cell.type), names.str(pin.port),
                                     definition.wire(*port).width)});
                }
            }
        }

        for (WireId port : module.ports()) {
            const Wire& wire = module.wire(port);
            if (wire.direction == Direction::Unknown) {
                issues.push_back({Issue::Severity::Warning,
                                  std::format("module {}: port {} has unknown direction",
                                              module_name, names.str(wire.name))});
            }
        }

        // Driver checks. A bit with an Unknown-direction endpoint might be
        // driven by it, so it is not reported as undriven.
        Connectivity conn(module);
        std::vector<SignalBitId> multi_driven;
        std::vector<SignalBitId> undriven;
        for (uint32_t b = 0; b < module.signal_count(); b++) {
            SignalBitId bit(b);
            size_t drivers = conn.drivers(bit).size();
            if (drivers > 1)
                multi_driven.push_back(bit);
            else if (drivers == 0 && !conn.loads(bit).empty() && conn.unknown(bit).empty())
                undriven.push_back(bit);
        }
        summarize(issues, design, module, conn, multi_driven, "have multiple drivers");
        summarize(issues, design, module, conn, undriven, "are loaded but not driven");
    }

    for (const auto& [type, count] : unknown_pins_by_type) {
        issues.push_back({Issue::Severity::Warning,
                          std::format("{} pin(s) of cell type {} have unknown direction", count,
                                      names.str(NameId(type)))});
    }
    return issues;
}

} // namespace netviz
