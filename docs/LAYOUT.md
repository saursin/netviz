# Schematic layout

How netviz turns one module into a drawing. The code is in
`src/netviz/view/display_graph.*` (what is drawn) and
`src/netviz/layout/` (where it goes).

## Pipeline

```
Module (IR) --> DisplayGraph --> layered_layout() --> LayoutResult --> SchematicView (draw, pick)
                 view lib          layout lib          layout.h          gui lib
```

`SchematicView` runs the first three steps on a `std::jthread` and hands the
result to the UI thread under a mutex. Selecting another module, closing the
design or pressing ✕ in the status bar requests stop through the
`std::stop_token`; the layout checks it between stages and inside long loops.

## Display graph

A read-only view of one module; the IR is never modified.

- **Nodes**: one per cell (`Cell`, or `Instance` when its type is a
  non-blackbox module of the design), then one per module port. A cell's node id
  equals its `CellId`.
- **Pins**: each node's pins are stored together. Outputs sit on the right;
  inputs, `InOut` and `Unknown` pins on the left. Input ports have their pin on
  the right (they drive), output ports on the left.
- **Nets**: bits are bundled into one net only when they have exactly the same
  set of endpoint pins. Pins tied only to constants get a label (`1'b0`, …)
  instead of a net.

> Why bundle by identical endpoints? The drawing must never show a connection
> that does not exist. Merging two buses that only partly overlap would draw
> wires between pins that are not connected. The cost is that a bus whose bits
> fan out to different cells is drawn as several wires; a later pass can
> regroup those with explicit bus rippers.

## Layered layout

A small Sugiyama-style subset, deterministic and replaceable. Each stage is its
own function, so e.g. the router can be swapped alone.

Everything is on a grid (`LayoutOptions::grid`, 10 units, the canvas's minor
grid): node corners, pins, wire tracks, bends and junctions. Node sizes are
rounded up to the grid, the pin pitch is two grid cells, and vertical placement
snaps to it.

> Why a grid? Schematics read better when parallel wires are evenly spaced and
> pins line up across cells, as on paper or in a schematic editor. It costs
> little: every spacing is already a constant, so only node sizes and the
> vertical placement need rounding.

1. **Size** nodes from label lengths (`char_width`, no font needed) and pin
   counts. Pins start at the top of a cell; its name and type are centred at
   the top between the two pin-label columns (auto-generated names are not
   drawn). A cell is as tall as its most pins on one side, or its label lines.
2. **Edges** from each net's drivers to its loads.
3. **Cycles**: Tarjan's strongly connected components; inside each component a
   DFS marks feedback edges, which are ignored for ranking.
4. **Ranks**: longest path. Input ports are pinned to rank 0, output ports to
   the last rank.
5. **Dummy nodes** along edges that span several ranks, one chain per net and
   driver, so ordering and routing see long wires.
6. **Order** within ranks: DFS order, then 4 rounds of barycenter sweeps. Ranks
   holding only ports keep port declaration order.
7. **Vertical placement**: 4 rounds of median alignment to neighbours, each
   followed by a pool-adjacent-violators pass that removes overlaps with the
   least total movement.
8. **Tracks**: in each channel between ranks, each net's vertical trunk gets a
   track, with the constrained left-edge algorithm of channel routing. Trunks
   share a track when they are a track pitch apart. Where net A comes from the
   left at height y and net B leaves to the right at the same y, A's track must
   be left of B's, or their horizontal wires would overlap. Nets whose
   constraints form a cycle (they cross) get doglegs: the trunk is split at a
   free grid height into pieces on different tracks, joined by a short jog.
   The channel is as wide as its tracks.
9. **Horizontal placement**: one column per rank; input ports right-aligned,
   output ports left-aligned.
10. **Routing**: orthogonal. Driver pin → its track → along the dummy chain →
    load pin. Feedback edges run in lanes below the drawing. Junction dots mark
    points where three or more segments of a net meet.

> Why must wires never overlap? An overlap or a wire ending on another net's
> wire reads as a connection. Like bundling, routing must never show one that
> does not exist. The tests check this on every example module: wires of two
> nets only touch where they cross at right angles, or when the nets share a
> pin (bits of one bus pin going to different places).

> Why our own layout instead of Graphviz or ELK? Schematics need things a
> general graph layout does not give: pins on fixed sides, ports on the edges,
> orthogonal wires and buses. Owning a small, readable implementation keeps
> the dependency list short and lets us tune it for netlists. The
> `DisplayGraph → LayoutResult` contract (`layout.h`) lets other engines plug in
> later.

> Why invariant tests instead of golden coordinates? Tests check what every
> layout must satisfy (no overlapping boxes, pins on their node's edge, ports at
> the edges, every load reached by its net, determinism), so the algorithm can
> improve without rewriting tests.

## Drawing

`SchematicView::draw` uses ImGui's draw list and only submits what is visible:
nodes via the `SpatialGrid` (also used for picking), wire segments by their
bounding box. Text is drawn only when it would be at least 6 px tall.

- Cells have a header band colored by kind, guessed from the type name: logic
  gates, flip-flops/latches, muxes, arithmetic/compare, others; submodule
  instances are blue. Ports are arrows colored by direction.
- Single-bit wires and buses have different colors; buses are thicker and
  labeled with their width at the driver. Pins are marked in the color of
  their wire.
- Nets of the hovered and selected node are highlighted.

## Performance

Release build, synthetic modules from `ex/scale/gen_netlist.py`:

| Module | Display graph | Layout | Segments |
|---|---|---|---|
| 10k cells | 3 ms | 80 ms | 66k |
| 49k cells | 15 ms | 430 ms | 333k |

## Known limits

- **Size limit**: modules with more display nodes (cells and ports) than
  `LayoutOptions::max_nodes` are not laid out; the canvas says so. The GUI sets
  it from Settings > Preferences > Canvas > Largest schematic (default 50,000,
  saved in `imgui.ini`); raising it lays out a refused module again. It exists
  because drawing is not yet level-of-detail aware, so big schematics are slow
  to pan and zoom when zoomed out. Big designs are meant to be explored via the
  hierarchy now, and via cones and filtering later.
- **Wide channels**: each net gets its own track in each channel, so high-fanout
  layouts get wide.
- **Overlap fallback**: a dogleg needs a free grid height between two wire
  heights; if there is none, the constraint is dropped and those two wires may
  overlap (not seen in the examples).
- **Shared bus pins**: when the bits of one bus pin go to different places,
  their wires leave the pin together and split, which looks like a bus ripper
  without the bit labels.
- **Bit fanout**: buses whose bits go to different cells are drawn as one wire
  per group of bits (see "Why bundle by identical endpoints?").
- **Driverless nets** are not routed.
- **Drawing cost**: every visible segment is submitted each frame; zoomed out
  over a large module this is slow. Level of detail and GPU batching are
  milestone 2b.
