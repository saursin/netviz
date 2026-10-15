#pragma once

// Derived connectivity of a module: for every signal bit, what drives it,
// what it drives, and which wire bits (names) refer to it. Built once from a
// finished module; see docs/IR.md.

#include "netviz/core/netlist.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace netviz {

// One end of a connection on a signal bit: a bit of a cell pin, or a bit of a
// module port. Built only through the two factory functions.
class Endpoint {
  public:
    enum class Kind : uint8_t { CellPin, ModulePort };

    static Endpoint cell_pin(CellId cell, uint32_t pin, uint32_t offset) {
        return Endpoint(Kind::CellPin, cell, WireId(), pin, offset);
    }
    static Endpoint module_port(WireId wire, uint32_t offset) {
        return Endpoint(Kind::ModulePort, CellId(), wire, 0, offset);
    }

    Kind kind() const { return kind_; }
    CellId cell() const {
        assert(kind_ == Kind::CellPin);
        return cell_;
    }
    uint32_t pin() const { // index into Module::pins(cell)
        assert(kind_ == Kind::CellPin);
        return pin_;
    }
    WireId port() const {
        assert(kind_ == Kind::ModulePort);
        return port_;
    }
    uint32_t offset() const { return offset_; } // bit position within the pin/port

    friend bool operator==(const Endpoint& a, const Endpoint& b) {
        return a.kind_ == b.kind_ && a.cell_ == b.cell_ && a.port_ == b.port_ && a.pin_ == b.pin_ &&
               a.offset_ == b.offset_;
    }

  private:
    Endpoint(Kind kind, CellId cell, WireId port, uint32_t pin, uint32_t offset)
        : kind_(kind), cell_(cell), port_(port), pin_(pin), offset_(offset) {}

    Kind kind_;
    CellId cell_;
    WireId port_;
    uint32_t pin_;
    uint32_t offset_;
};

// A bit of a named wire.
struct WireBit {
    WireId wire;
    uint32_t offset;

    friend bool operator==(const WireBit& a, const WireBit& b) {
        return a.wire == b.wire && a.offset == b.offset;
    }
};

// Connectivity index of one module.
//
// Drivers: cell output pins and module input ports. Loads: cell input pins
// and module output ports. InOut appears in both; Unknown directions go to
// unknown(). Constant bits are not indexed.
//
// Lifetime: build it only after the module is complete and
// Design::resolve_pin_directions() has run. Any structural change to the
// module afterwards invalidates it. It does not keep a reference to the module.
class Connectivity {
  public:
    explicit Connectivity(const Module& module);

    std::span<const Endpoint> drivers(SignalBitId bit) const { return drivers_.get(bit); }
    std::span<const Endpoint> loads(SignalBitId bit) const { return loads_.get(bit); }
    std::span<const Endpoint> unknown(SignalBitId bit) const { return unknown_.get(bit); }
    // Every wire bit (i.e. every name) that refers to this signal bit.
    std::span<const WireBit> wire_bits(SignalBitId bit) const { return wire_bits_.get(bit); }

  private:
    // Compressed rows: the items of signal bit i are items[offsets[i] .. offsets[i+1]).
    template <typename T> struct Table {
        std::vector<uint32_t> offsets;
        std::vector<T> items;

        std::span<const T> get(SignalBitId bit) const {
            uint32_t begin = offsets.at(bit.index);
            uint32_t end = offsets.at(bit.index + 1);
            return std::span<const T>(items).subspan(begin, end - begin);
        }
    };

    Table<Endpoint> drivers_;
    Table<Endpoint> loads_;
    Table<Endpoint> unknown_;
    Table<WireBit> wire_bits_;
};

// A problem found by validate().
struct Issue {
    enum class Severity : uint8_t { Error, Warning };

    Severity severity;
    std::string message;
};

// Checks a design for problems an importer cannot rule out. This is a
// diagnostic: real designs may legitimately produce warnings.
//
// Errors: a pin on a module-typed cell that the module does not have as a
// port, or whose width differs from that port.
// Warnings (summarized, not one per bit): signal bits with several drivers,
// loaded bits with no driver, and pins/ports with Unknown direction.
std::vector<Issue> validate(const Design& design);

} // namespace netviz
