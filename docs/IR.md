# Netlist IR

This is the specification of netviz's in-memory netlist representation
(`src/netviz/core/netlist.h`, `src/netviz/core/connectivity.h`). Quoted blocks
explain why a decision was made.

## Overview

A **design** holds **modules**. Each module owns a space of **signal bits**.
**Wires** (including module **ports**) and the **pins** of **cells** are named,
ordered views onto those bits. Connectivity is a derived index, built after a
module is complete.

```
Design
├── StringPool                 interned names
└── modules (vector<Module>)
     ├── signal bits           s0 .. sN-1 (implicit, just a count)
     ├── wires[]               name + slice of bits[]
     ├── cells[]               name, type + slice of pins[]
     │    └── pins[]           port name, direction + slice of bits[]
     ├── bits[]                one flat SigBit array shared by wires and pins
     └── ports[]               wires in port order

Connectivity(Module)           derived, read-only
├── signal bit → drivers
├── signal bit → loads
├── signal bit → unknown-direction endpoints
└── signal bit → wire bits (names)
```

The IR lives in `netviz_core` and depends only on the standard library.

> **Why:** the same design is used by the GUI, tests and (later) command-line
> tools. Keeping it free of UI types means it can be tested and reused on its own.

## Object model: plain structs, no virtual hierarchy

IR objects are plain structs stored by value in arrays and referred to by typed
ids (`ModuleId`, `WireId`, `CellId`, …). There is no polymorphic `NetObject`
base class with virtual functions and no per-object heap allocation.

What wires and cells have in common lives in a small **non-virtual** base:

```cpp
struct NamedObject {
    NameId name;
    bool hidden;
    std::vector<NamedValue> attributes;
    const Value* attribute(NameId attr) const;
};
struct Wire : NamedObject { ... };
struct Cell : NamedObject { ... };
```

Code that only needs a name or attributes takes a `const NamedObject&` and works
for both. The base adds no size: `Wire` (48 bytes at the time) and `Cell` (72
bytes) were the same size with or without it.

> **Why not a virtual hierarchy:** a benchmark of the same 1M-cell netlist
> (3M pins, Release build) compared this IR with a classic `NetObject` design
> (virtual base, one heap object per cell/pin/net, pointer links):
>
> | | flat IR | `NetObject` hierarchy |
> |---|---|---|
> | build | 1.1 s | 1.6–1.8 s |
> | memory | 532 MB | 800–870 MB |
> | scan all pins | 6 ms | 38–50 ms |
> | graph walk (fan-out levels) | 11 ms | 60–80 ms |
>
> Traversal is 6–8× faster and memory about 1.6× lower. Allocating the
> objects from an arena barely helps the hierarchy (38 ms scan), so the cost is
> the object layout itself (vtable and parent pointers, pin objects, pointer
> chasing), not just allocation. A viewer traverses constantly (highlighting
> fan-in/fan-out, search, layout, visibility), so this matters at the
> million-cell sizes netviz targets.

> **Why this matches existing tools:** Yosys's `Wire`/`Cell` derive from the
> non-virtual `AttrObject`; nextpnr uses plain `CellInfo`/`NetInfo` structs;
> OpenROAD's `dbInst`/`dbNet` are non-virtual `dbObject` handles (type tag + id)
> over paged tables. None of them makes netlist objects polymorphic. OpenSTA does
> use virtual functions, but on its `Network` interface (to sit on different
> databases), not on each object.

> **Why the old versions did not need it:** in earlier netviz prototypes, the
> shared part of `NetObject` was data (name, parent, attributes, a type tag); the
> only real virtual functions were `draw()` and hit-testing, which belong in the
> view/UI layers, not in the netlist.

`Module` does not derive from `NamedObject`; it keeps its own `name()` and
`attributes()`.

> **Why:** a module is handed out as a mutable `Module&` while building, and its
> name is the key of the design's module index. A public, writable `name` field
> would let callers break `find_module`.

When the UI needs to refer to "any object" (selection, properties panel), it will
use a small value handle (kind + module + index) rather than a base-class pointer.

## Signal bits and `SigBit`

A module's signal bits are identified by `SignalBitId` (0 .. `signal_count()-1`),
created with `Module::add_signals(n)`. A connection bit is a `SigBit`: either a
signal bit or a constant `0`, `1`, `x` or `z` (`BitState`).

A signal bit *is* the connectivity identity: everything that refers to the same
`SignalBitId` is connected.

> **Why bit-normalized:** netviz's first input is Yosys JSON, which already
> identifies every connected bit by an integer. Storing RTLIL-style wire chunks and
> `connect` statements would mean inventing structure the file does not contain
> (which of two aliased names "owns" a bit is arbitrary). Bits also make every
> connectivity query an array lookup.

> **Why no separate `NetId` (yet):** for these designs a bit id already is the
> connected class, so a net id would be a one-to-one wrapper with no benefit.
> If grouping bits into higher-level nets becomes useful, it can be added as a
> derived layer.

`SigBit` is a 32-bit value. It is used only through `SigBit::signal(id)`,
`SigBit::constant(state)`, `is_constant()`, `signal()` and `constant()`. Signal
ids must be below `kMaxSignals` (`0xFFFFFFFC`); the four values above it encode
the constants. `add_signals` asserts this limit.

> **Why a tagged integer:** bits are the most numerous objects in the IR (millions
> in large designs). Four bytes per bit, trivially compared and hashed, keeps the
> flat bit array compact. The encoding stays private so it can change.

## Wires, ports and aliases

A `Wire` has a name, a width and an ordered list of `SigBit`s (bit 0 first). Its
bits may include constants. Wires are added with
`Module::add_wire(name, bits, WireOptions{hidden, start_offset, upto, is_signed})`.

A wire also records how its bits are numbered in the source, as in RTLIL:

- `start_offset`: the lowest declared index, e.g. `4` for `a[7:4]`;
- `upto`: declared `[lo:hi]` instead of `[hi:lo]`, so `bits()[0]` is the highest
  index;
- `is_signed`: declared `signed`.

`bit_index(wire, offset)` gives the declared index of the bit at position
`offset`: `start_offset + offset`, or `start_offset + width - 1 - offset` when
`upto`.

> **Why:** without these, bit names would be wrong for any bus that does not start
> at 0 or counts upwards. Yosys writes them on most real netlists (e.g. after
> splitting a bus, wire `a[1]` has `offset: 1`). They cost 8 bytes per wire
> (`Wire` is 56 bytes).

Two wires whose bits overlap are **aliases** of each other (e.g. Yosys's `c1` and
`$abc$84$c1` both naming bit `s6`). No alias object exists; aliasing follows from
sharing bits.

> **Why:** a wire is a *name for bits*, not their owner. That matches how
> netlists are written, and answers "what is this bit called?" by listing every
> wire bit that refers to it.

A wire becomes a module **port** with `Module::make_port(wire, direction)`, which
appends it to `ports()` and sets `Wire::port_index`. `port_index` is `-1` for
non-port wires; `Wire::direction` is meaningful only when `port_index >= 0`, and
may be `Unknown`.

> **Why ports are wires:** as in RTLIL, a port is a wire with interface metadata.
> The port order is kept separately so the builder, not callers, controls
> `port_index`.

`name`, `hidden` and `attributes` come from `NamedObject`. `hidden` marks
auto-generated names (Yosys `hide_name`, `$`-prefixed names) that the UI should
not show by default.

## Cells and pins

A `Cell` has a name, a **type** and pins. The type is a `NameId`: either the name
of a module in the design (an **instance**) or a library/primitive cell type
(`NAND`, `$add`, …). Each `Pin` has a port name, a `Direction` and an ordered list
of `SigBit`s.

Cells are added in one call with all their pins:
`Module::add_cell(name, type, std::span<const NewPin>, hidden)`.

> **Why all pins at once:** every cell's pins are stored as one contiguous range
> of the module's `pins[]` array, and every pin's bits as one contiguous range of
> `bits[]`. Adding a cell in one call keeps those ranges contiguous without
> per-cell vectors (millions of small heap allocations in large designs).

A cell's `parameters` are a `NamedValue` list; its name, `hidden` flag and
`attributes` come from `NamedObject`.

## Directions

`Direction` is `Unknown`, `Input`, `Output` or `InOut`.

An importer sets a pin's direction when the input says it (Yosys writes
`port_directions` for cell types it knows). Otherwise the pin starts as `Unknown`.
`Design::resolve_pin_directions()` then fills `Unknown` pins of module instances
from that module's port directions (matched by port name) and returns how many
pins in the design are still `Unknown`.

> **Why `Unknown` is allowed:** gate-level Yosys output often has no directions for
> library cells (`NOR`, `DFF`, …). Guessing would silently produce wrong drivers.
> Keeping `Unknown` makes the gap visible; Liberty files or a Yosys cell library
> can fill it later.

> **Why the core has no built-in cell table:** knowledge such as "`$dff.Q` is an
> output" is specific to Yosys. It belongs in a Yosys-specific layer, not in the
> generic IR.

## Values (parameters and attributes)

```cpp
struct Value { enum class Kind { Bits, String }; Kind kind; std::string text; };
```

`Bits` values are MSB-first strings of `0`, `1`, `x`, `z`, exactly as Yosys writes
them. `String` values are arbitrary text.

`as_int(value)` returns the value as an unsigned number only when it is `Bits`, at
most 64 characters long (by text width, regardless of leading zeros) and contains
only `0`/`1`. Everything else, including empty text, returns `nullopt`.

> **Why text + kind:** most parameter and attribute values are only displayed.
> Keeping the original text is lossless and simple; numeric conversion is an
> explicit, narrowly defined operation rather than guesswork inside `Value`.

## Modules and hierarchy

`Design::add_module(name)` adds a module and returns its `ModuleId`, or `nullopt`
if a module with that name exists. `find_module(name)` looks modules up;
`set_top`/`top` record the top module.

Hierarchy is kept, not flattened: an instance is a cell whose type names a module.

A module can be a **blackbox** (`is_blackbox()`, set with `set_blackbox()`): it
declares only ports and has no contents, typically a library cell such as a
`NAND` gate. Instances of blackboxes are leaf cells, not hierarchy; the UI lists
them separately and they are never the top module.

> **Why a first-class flag:** Yosys includes library cells as blackbox modules
> (marked with a `blackbox` attribute). Checking an empty module for drivers
> produces false warnings, and the hierarchy should not show every gate as a
> submodule. A flag in the IR keeps the core independent of Yosys's attribute
> names; the importer translates the attribute.

> **Why hierarchical:** flattening duplicates every repeated submodule (bad for
> large designs) and loses the module boundaries users navigate by. Flattening
> can be added later as an operation that produces a new module.

Modules are stored in a `std::vector`. Adding a module invalidates `Module&`
references, but not `ModuleId`s.

> **Why a vector:** the design is built first and read afterwards, and code holds
> ids rather than long-lived references, so stable addresses are not needed.
> A vector also lets `modules()` return a span.

## Names

All names are interned in the design's `StringPool` and stored as `NameId`.

Names are unique per kind: module names in a design, wire names in a module, and
cell names in a module (wires and cells are separate name spaces, as in RTLIL).
`add_module`, `add_wire` and `add_cell` return `nullopt` for a duplicate, and
`find_module`/`find_wire`/`find_cell` look names up.

> **Why the builder rejects duplicates:** lookups by name assume uniqueness, and
> valid Yosys output never has duplicates. Refusing them at construction is
> simpler than allowing an ambiguous state and reporting it later.

## Lifecycle: build, then read

The IR is **append-only**: wires, cells, pins and signal bits are never removed,
so ids are plain array indices.

1. An importer builds modules (`add_signals`, `add_wire`, `make_port`,
   `add_cell`).
2. `Design::resolve_pin_directions()` runs.
3. `Connectivity` indexes are built and the design is read.

After construction, only metadata may change: `attributes()`, `parameters()`,
`wire_attributes()`, `cell_attributes()`, `cell_parameters()`. Structural fields
(bit ranges, pin ranges, names, port indices) are not writable through the API.

> **Why:** a viewer loads a design and then reads it. Without deletion there are
> no stale ids, no free lists and no incremental index updates, which keeps the
> code small and the storage flat. Editing can be added later with
> generation-checked ids if it is ever needed.

Programmer errors are caught early: bits outside the module's signal range,
making a wire a port twice, an invalid top module or pin index are checked with
`assert`; looking up an object by an out-of-range id (`wire(id)`, `cell(id)`,
`module(id)`, …) throws `std::out_of_range`.

## Connectivity

`Connectivity(module)` indexes one finished module:

| Query | Contents |
|---|---|
| `drivers(bit)` | cell `Output` pins and module `Input` ports |
| `loads(bit)` | cell `Input` pins and module `Output` ports |
| `unknown(bit)` | pins and ports with `Unknown` direction |
| `wire_bits(bit)` | every `(wire, offset)` that refers to the bit, i.e. its names |

`InOut` pins and ports appear in both `drivers` and `loads`. Constant bits are not
indexed.

> **Why module ports flip:** inside a module, a module *input* brings a signal in,
> so it drives; a module *output* consumes it, so it is a load. Cell pins are the
> other way round.

Each entry is an `Endpoint`: either `Endpoint::cell_pin(cell, pin, offset)` or
`Endpoint::module_port(wire, offset)`, with `kind()` telling which. Endpoints can
only be created through these two functions, and `cell()`/`pin()`/`port()` assert
the matching kind.

> **Why typed endpoints:** a plain integer meaning "cell or wire, depending" would
> throw away the type safety of `CellId`/`WireId`.

The index is stored as compressed rows (one offsets array and one items array per
table), built in two passes: count per bit, prefix sum, fill.

> **Why compressed rows:** one allocation per table instead of a vector per bit;
> compact and fast for millions of bits.

**Lifetime rule:** build `Connectivity` only after the module is complete and
`resolve_pin_directions()` has run. Any later structural change to the module
invalidates it. The index does not keep a reference to the module.

> **Why no freeze mechanism:** with an append-only builder, a documented rule is
> enough for now.

## Validation

`validate(design)` returns a list of `Issue`s (`Error` or `Warning`). It is a
diagnostic, not a gate: real imported designs may legitimately produce warnings.

Errors:
- a pin on a module instance that the module does not have as a port;
- a pin on a module instance whose width differs from that port.

Warnings:
- signal bits with more than one driver, and loaded bits with no driver — one
  summary per module, naming up to 5 example bits (a bit that has an
  `Unknown`-direction endpoint is not reported as undriven, since that endpoint
  may drive it). Blackbox modules are skipped: nothing inside them drives their
  outputs;
- module ports with `Unknown` direction (one per port);
- cell pins with `Unknown` direction — one summary per cell type for the whole
  design.

States the builder already prevents (duplicate names, out-of-range bits, an
invalid top module) are not re-checked.

> **Why summaries:** a design with a black-box cell type or a common floating
> signal can have hundreds of thousands of affected pins; one line per pin would
> bury everything else.

## Debug dump

`dump(design, out)` writes a deterministic, human-readable description:

```
module inv (top)
  signals 2
  attribute src = "inv.v:1"
  wire a [1] port0 input: s0
  wire y [1] port1 output: s1
  wire $tmp [2] hidden: s1 0
  cell u1 NOT
    parameter DRIVE = 10
    A input: s0
    Y output: s1
```

Signal bits print as `s<N>`, constants as `0 1 x z`; `String` values are quoted,
`Bits` values are not.

> **Why it is not a file format:** it exists for debugging and golden tests
> (importer → IR → dump → compare). Keeping it informal means it can change
> freely; netviz has no serialization format of its own.

## Importers

Importers translate an input format into this IR. They live in the separate
`netviz_io` library, so the core never depends on a parser.

### Yosys JSON (`netviz/io/yosys_json.h`)

`import_yosys_json(path)` reads `write_json` output with simdjson and returns an
`ImportResult`: the design (or an error with context such as
`module m: cell c: ...`) and a list of `Issue`s. Malformed structure fails the
whole import; unknown keys are ignored.

Per module, in file order:

| Yosys JSON | IR |
|---|---|
| module `attributes`, `parameter_default_values` | module attributes and parameters |
| `blackbox` attribute | `Module::set_blackbox(true)` |
| integer bit numbers | new `SignalBitId`s, numbered in order of first appearance |
| `"0"`, `"1"`, `"x"`, `"z"` bits | constant `SigBit`s |
| `ports` (key order = port order) | wires made ports, with `offset`/`upto`/`signed` |
| `netnames` | wires; a netname with a port's name is that port's wire (bits must match) |
| `hide_name` | `hidden` |
| `cells`: `type`, `parameters`, `attributes` | cells |
| `connections` (in order) | pins |
| `port_directions` | pin directions; pins without one stay `Unknown` |
| non-empty `memories` | not imported yet; reported as a warning |

Values follow Yosys's own JSON frontend: a string of only `0`/`1`/`x`/`z` is a
bit vector; such a string followed by spaces is a string with one trailing space
removed (Yosys adds it to mark strings that look like bits); numbers become
32-bit (or 64-bit) bit vectors.

Afterwards the top module is chosen as nextpnr does (a non-box module with the
`top` attribute, otherwise the only non-box module no other module
instantiates; otherwise none, with a warning), `resolve_pin_directions()` runs,
and `validate()` issues are added to the result.

> **Why our own importer:** Yosys's JSON frontend builds RTLIL with a
> character-by-character parser (slow and memory-hungry on large files), and
> nextpnr's flattens the hierarchy and uses a JSON library that loses key order
> (which is the port order). Their rules are reused; their code is not.

> **Why simdjson:** it parses hundreds of megabytes per second and keeps key
> order. A 1M-cell netlist (333 MB) imports in about 2.4 s (Release); the parsed
> document briefly needs about 4x the file size while the IR is built.

### Others (possible later)

- **RTLIL**: resolve `connect` statements and wire chunks into shared signal bits
  at import time, producing the same IR.

> **Why raw input ids never reach the core:** different importers can then feed
> exactly the same representation, and the rest of netviz never depends on a file
> format's numbering.

## Not yet

- Editing (removing or rewiring objects) and generation-checked ids.
- A `NetId` layer grouping bits into named nets/buses.
- An "any object" handle (kind + module + index) for selection and the
  properties panel.
- Instance paths (occurrences): two instances of one module share a single
  `Module` definition, so per-occurrence data (waveform values, timing paths,
  selection) needs a path of instance cells from the top. This is a derived
  concept; the stored IR does not change. See [ROADMAP.md](ROADMAP.md).
- Structured locations in `Issue` (module and objects/bits), for clickable checks.
- A Yosys cell library (pin directions and semantics of `$and`, `$dff`, `$mux`, …).
- Liberty cell definitions.
- Fan-in/fan-out traversal beyond the per-bit queries above.
