#pragma once

// Netlist IR: the in-memory representation of a design.
//
// A module owns a space of signal bits. Wires, module ports and cell pins are
// named, ordered views onto those bits; two wires that share bits are aliases.
// The IR is built once (by an importer) and then only read. See docs/IR.md for
// the full specification and the reasoning behind it.

#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace netviz {

// ---------------------------------------------------------------------------
// Ids
// ---------------------------------------------------------------------------

// A typed index into one of the IR's arrays. The Tag only exists so that ids
// of different kinds cannot be mixed up.
template <typename Tag> struct Id {
    static constexpr uint32_t kInvalid = UINT32_MAX;

    uint32_t index = kInvalid;

    constexpr Id() = default;
    constexpr explicit Id(uint32_t i) : index(i) {}

    constexpr bool valid() const { return index != kInvalid; }

    friend constexpr bool operator==(Id a, Id b) { return a.index == b.index; }
    friend constexpr bool operator!=(Id a, Id b) { return a.index != b.index; }
    friend constexpr bool operator<(Id a, Id b) { return a.index < b.index; }
};

using ModuleId = Id<struct ModuleTag>;
using WireId = Id<struct WireTag>;
using CellId = Id<struct CellTag>;
using SignalBitId = Id<struct SignalBitTag>;
using NameId = Id<struct NameTag>;

} // namespace netviz

template <typename Tag> struct std::hash<netviz::Id<Tag>> {
    size_t operator()(netviz::Id<Tag> id) const { return std::hash<uint32_t>{}(id.index); }
};

namespace netviz {

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// Interns strings so names can be stored and compared as small integers.
class StringPool {
  public:
    NameId intern(std::string_view text);
    const std::string& str(NameId id) const;
    size_t size() const { return strings_.size(); }

  private:
    std::deque<std::string> strings_; // deque: references stay valid as it grows
    std::unordered_map<std::string_view, NameId> index_;
};

// ---------------------------------------------------------------------------
// Signal bits
// ---------------------------------------------------------------------------

enum class BitState : uint8_t { Zero, One, X, Z };

// Signal bit ids must be below this; the values above it encode constants.
constexpr uint32_t kMaxSignals = 0xFFFFFFFCu;

// One bit of a connection: either a signal bit of the module or a constant.
class SigBit {
  public:
    static SigBit signal(SignalBitId id) {
        assert(id.index < kMaxSignals);
        return SigBit(id.index);
    }
    static SigBit constant(BitState state) {
        return SigBit(kMaxSignals + static_cast<uint32_t>(state));
    }

    bool is_constant() const { return value_ >= kMaxSignals; }
    SignalBitId signal() const {
        assert(!is_constant());
        return SignalBitId(value_);
    }
    BitState constant() const {
        assert(is_constant());
        return static_cast<BitState>(value_ - kMaxSignals);
    }

    friend bool operator==(SigBit a, SigBit b) { return a.value_ == b.value_; }
    friend bool operator!=(SigBit a, SigBit b) { return a.value_ != b.value_; }
    size_t hash() const { return std::hash<uint32_t>{}(value_); }

  private:
    explicit SigBit(uint32_t value) : value_(value) {}

    uint32_t value_;
};

static_assert(kMaxSignals + static_cast<uint32_t>(BitState::Z) == UINT32_MAX,
              "the four constant states must fit above kMaxSignals");

// ---------------------------------------------------------------------------
// Directions and values
// ---------------------------------------------------------------------------

enum class Direction : uint8_t { Unknown, Input, Output, InOut };

const char* direction_name(Direction direction);

// A parameter or attribute value, kept as given by the importer.
struct Value {
    enum class Kind : uint8_t { Bits, String };

    Kind kind = Kind::String;
    std::string text; // Bits: MSB-first characters 0/1/x/z. String: any text.
};

// Bits values of at most 64 characters containing only 0/1, read as an
// unsigned MSB-first number. Anything else (x/z, wider text, strings) gives
// nullopt.
std::optional<uint64_t> as_int(const Value& value);

struct NamedValue {
    NameId name;
    Value value;
};

// ---------------------------------------------------------------------------
// Wires, pins, cells
// ---------------------------------------------------------------------------

// Fields shared by wires and cells. A plain (non-virtual) base: objects stay in
// flat arrays with no per-object vtable; see docs/IR.md.
struct NamedObject {
    NameId name;
    bool hidden = false; // auto-generated name (e.g. Yosys "$abc$..."), hide by default
    std::vector<NamedValue> attributes;

    // The value of the attribute with this name, or null.
    const Value* attribute(NameId attr) const;
};

struct Wire : NamedObject {
    int32_t port_index = -1;                  // position in Module::ports(), or -1 if not a port
    Direction direction = Direction::Unknown; // meaningful only when port_index >= 0
    uint32_t bits_begin = 0;
    uint32_t width = 0;
};

struct Pin {
    NameId port;
    Direction direction = Direction::Unknown;
    uint32_t bits_begin = 0;
    uint32_t width = 0;
};

struct Cell : NamedObject {
    NameId type; // a module of the design, or a library/primitive cell type
    uint32_t pins_begin = 0;
    uint32_t pin_count = 0;
    std::vector<NamedValue> parameters;
};

// A pin passed to Module::add_cell.
struct NewPin {
    NameId port;
    Direction direction = Direction::Unknown;
    std::span<const SigBit> bits;
};

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

class Module {
  public:
    explicit Module(NameId name) : name_(name) {}

    NameId name() const { return name_; }

    // -- Building (structure is append-only) --------------------------------

    // Adds `count` new signal bits and returns the first one's id.
    SignalBitId add_signals(uint32_t count);

    // Returns nullopt if a wire with this name already exists.
    std::optional<WireId> add_wire(NameId name, std::span<const SigBit> bits, bool hidden = false);

    // Makes a wire a module port, appended to the port order.
    void make_port(WireId wire, Direction direction);

    // Returns nullopt if a cell with this name already exists.
    std::optional<CellId> add_cell(NameId name, NameId type, std::span<const NewPin> pins,
                                   bool hidden = false);

    void set_pin_direction(CellId cell, uint32_t pin, Direction direction);

    // -- Metadata (may change any time; never affects structure) ------------

    std::vector<NamedValue>& attributes() { return attributes_; }
    std::vector<NamedValue>& parameters() { return parameters_; } // default values
    std::vector<NamedValue>& wire_attributes(WireId wire);
    std::vector<NamedValue>& cell_attributes(CellId cell);
    std::vector<NamedValue>& cell_parameters(CellId cell);

    // -- Reading --------------------------------------------------------------

    uint32_t signal_count() const { return signal_count_; }
    const std::vector<NamedValue>& attributes() const { return attributes_; }
    const std::vector<NamedValue>& parameters() const { return parameters_; }

    std::span<const Wire> wires() const { return wires_; }
    std::span<const Cell> cells() const { return cells_; }
    std::span<const WireId> ports() const { return ports_; }

    const Wire& wire(WireId id) const { return wires_.at(id.index); }
    const Cell& cell(CellId id) const { return cells_.at(id.index); }

    std::optional<WireId> find_wire(NameId name) const;
    std::optional<CellId> find_cell(NameId name) const;

    std::span<const Pin> pins(const Cell& cell) const;
    std::span<const SigBit> bits(const Wire& wire) const;
    std::span<const SigBit> bits(const Pin& pin) const;

  private:
    uint32_t append_bits(std::span<const SigBit> bits);

    NameId name_;
    uint32_t signal_count_ = 0;
    std::vector<NamedValue> attributes_;
    std::vector<NamedValue> parameters_;

    std::vector<Wire> wires_;
    std::vector<Cell> cells_;
    std::vector<Pin> pins_;    // all cells' pins; each cell owns a contiguous range
    std::vector<SigBit> bits_; // all wires' and pins' bits, as contiguous ranges
    std::vector<WireId> ports_;

    std::unordered_map<NameId, WireId> wire_index_;
    std::unordered_map<NameId, CellId> cell_index_;
};

// ---------------------------------------------------------------------------
// Design
// ---------------------------------------------------------------------------

class Design {
  public:
    StringPool& names() { return names_; }
    const StringPool& names() const { return names_; }

    // Returns nullopt if a module with this name already exists.
    // Adding a module invalidates Module references (not ModuleIds).
    std::optional<ModuleId> add_module(NameId name);

    Module& module(ModuleId id) { return modules_.at(id.index); }
    const Module& module(ModuleId id) const { return modules_.at(id.index); }
    std::span<const Module> modules() const { return modules_; }
    std::optional<ModuleId> find_module(NameId name) const;

    void set_top(ModuleId id);
    std::optional<ModuleId> top() const;

    // For cells whose type is a module of this design, fills in Unknown pin
    // directions from that module's ports. Returns how many cell pins in the
    // design still have an Unknown direction afterwards.
    int resolve_pin_directions();

  private:
    StringPool names_;
    std::vector<Module> modules_;
    std::unordered_map<NameId, ModuleId> module_index_;
    ModuleId top_;
};

// Writes a human-readable dump of the design. This is a debugging aid with a
// deterministic layout, not a stable file format.
void dump(const Design& design, std::ostream& out);

} // namespace netviz
