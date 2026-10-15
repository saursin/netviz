# Roadmap

Where netviz is heading and why. Order within a tier is a rough priority, not a
commitment.

## Positioning

Commercial netlist debuggers (Synopsys Verdi nSchema/nTrace, Cadence SimVision,
Siemens Visualizer) are powerful but expensive, licensed per seat, closed, and
tied to their own flows. Open-source options (`yosys show` via Graphviz,
netlistsvg, DigitalJS) produce static pictures and break down beyond a few
thousand cells.

netviz aims to be the **interactive netlist explorer for open-source flows**:
fast at millions of cells, explored incrementally, connected to source code,
waveforms and timing, and scriptable.

## Done

- Application shell: docked UI, pannable/zoomable canvas, console with commands,
  persistent settings.
- Netlist IR with connectivity index and validation ([IR.md](IR.md)).

## Foundation milestones

1. **Yosys JSON import**: parse `write_json` output into the IR; File → Open,
   `open` command and the command line; Hierarchy panel shows the real module tree,
   Properties shows the selected object.
2. **Visual graph and schematic rendering**: derive nodes/pins/edges from a module
   (or from a cone), a first left-to-right layout, culling and level of detail,
   then batched GPU drawing once large designs need it.

## Tier 1: core differentiators

| Feature | Why | Builds on |
|---|---|---|
| **Incremental schematic exploration**: start from a signal or instance and expand on demand instead of drawing a whole module | Real designs are too big to view at once; this is how people debug | Visual graph, `Connectivity` |
| **Trace and cones**: driver/load tracing, fan-in/fan-out cones with a depth limit, path between two signals, across hierarchy | The most-used netlist debug operation | `Connectivity` (per-bit walks are ~11 ms per 1M cells) |
| **Source cross-probing**: click a cell to open its RTL line; select a line to highlight its cells | Connects the netlist back to the code people wrote | Yosys `src` attributes (stored today) |
| **Clickable design checks**: `validate()` issues in a panel that jumps to the problem | Turns log messages into navigation | `validate()` |

## Tier 2: things commercial tools do poorly or not at all

| Feature | Why |
|---|---|
| **Yosys pass explorer**: capture the design after each pass (`proc`, `opt`, `techmap`, `abc`, …), step through them and see what each pass changed | Unique to open-source flows: no commercial tool shows the inside of its own synthesis. Useful for learning, debugging results and Yosys development |
| **Visual netlist diff**: compare two netlists (before/after a pass, two tool versions, two commits) and highlight added, removed and changed logic | Equivalence checkers say *whether* designs match, not *what changed* |
| **Waveform overlay**: load VCD/FST, show values on wires, scrub time, trace an X back to its source | A flagship commercial feature; absent in open source |
| **Timing path overlay**: load an OpenSTA/OpenROAD timing report and highlight the critical path on the schematic | OpenROAD shows physical layout, not the logical path |
| **Area and structure views**: per-module area from Liberty, cell-type histograms, a hierarchy treemap | Answers "where is my area going?" without reading text reports |

## Tier 3: things only an open tool can do well

| Feature | Why |
|---|---|
| **Smart abstraction**: bundle bits back into buses, collapse instances, recognize register banks, mux trees and adder chains and draw them as blocks | Makes gate-level netlists readable |
| **Headless/CI rendering**: e.g. `netviz --render cone:top.u_alu.y -o alu.svg` | Schematics in docs and pull-request reviews; licensed GUIs cannot run in CI |
| **Browser build (WebAssembly)**: share a link, view a design in the browser | Teaching, papers, bug reports; impossible with license-locked tools |
| **Scripting and sessions**: grow the console commands into a query language (in the spirit of Yosys `select`), saved views, Python bindings | Commercial tools are scriptable only through their own Tcl dialects |
| **Lightweight "what-if" simulation**: toggle inputs and watch values propagate | Interactive exploration of small and gate-level designs; real simulation stays with Verilator/Icarus via waveform overlay |

## What the IR needs for these

The stored IR ([IR.md](IR.md)) supports all of the above as it is. New
capabilities come as derived layers next to it, the same way `Connectivity` does:

| Addition | Needed by | Kind of change |
|---|---|---|
| **Instance paths** (occurrences): a chain of instance cells from the top, identifying e.g. `top.ha1.sum` vs `top.ha2.sum` when both instances share one `half_adder` module | Waveform overlay, timing overlay, cross-hierarchy trace, per-instance selection | New derived concept; stored IR unchanged |
| **Cell library**: per cell type pin directions, area, logic function, sequential behavior (from Liberty and a Yosys built-in table) | Area views, filling unknown pin directions, simulation | New structure next to `Design` |
| **Structured issue locations**: `Issue` gains the module and the objects/bits it refers to | Clickable design checks | Small additive change to `validate()` |
| **Source index**: parsed `src` attributes and a reverse file:line → objects map | Source cross-probing | Derived index |
| **Cross-design matching**: pairing objects between two designs by name and structure | Netlist diff, pass explorer | Algorithm over two `Design`s (each already has its own name pool) |

Constraints worth keeping in mind:

- A browser build is 32-bit WebAssembly with about 4 GB of memory, roughly
  5M cells at today's ~0.5 KB per cell (names are the largest share).
- Matching auto-generated names (`$abc$…`) between runs is unreliable; diffing
  needs structural matching, not just names.
