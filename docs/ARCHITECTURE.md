# Architecture

This document describes how netviz is put together and where it is heading.
Sections marked *(planned)* describe design intent, not existing code.

## Goals

- Stay interactive on large netlists (10⁵–10⁶ cells): panning and zooming must
  never touch the netlist or rebuild geometry.
- Keep the netlist model independent of any UI so it can be reused by CLI tools,
  scripts, tests and other front ends.
- Simple, readable C++ with a small set of well-defined extension points.

## Layers

```
            +-------------------------------+
            |  gui   (SDL3, OpenGL, ImGui)  |   window, panels, input, renderer
            +---------------+---------------+
                            |
            +---------------v---------------+
            |  view                         |   camera, visual graph, layout,
            |                               |   spatial index, LOD   (no UI deps)
            +---------------+---------------+
                            |
            +---------------v---------------+
            |  core                         |   logging, commands, netlist IR,
            |                               |   connectivity  (std only)
            +-------------------------------+
```

Each layer is a CMake library and may only depend on the layers below it. This
is enforced by target links: `core` and `view` do not link ImGui/SDL/GL, so an
accidental include fails to build.

File importers (Yosys JSON, later Liberty/RTLIL) translate formats into the core
IR and will live in their own library on top of `core`, so the core never depends
on a parser or file format.

## Core

Implemented today: the netlist IR (below), logging with pluggable sinks
(`log.h`), and a `CommandRegistry` (`command.h`) that maps command names to
handlers. Commands are the scripting/extension hook: the console runs them
today, and other front ends (CLI, scripts) can reuse them.

### Netlist IR

Specified in [IR.md](IR.md) (`netlist.h`, `connectivity.h`). In short:

- `Design` → `Module`s (hierarchical; a cell whose type is a module is an
  instance). Names are interned in a `StringPool`.
- Each module owns a space of signal bits; wires, ports and cell pins are named
  views onto them (`SigBit` = signal bit or constant). Aliases are wires that
  share bits.
- Built once by an importer, then read: append-only, typed index ids, flat
  arrays.
- `Connectivity` is a derived per-bit index of drivers, loads and names, and
  `validate()` reports design problems.
- Importers (Yosys JSON next, later Liberty/RTLIL) live outside the core and
  translate their formats into this IR.

## View *(partly implemented)*

- `Camera2D` (implemented) maps world ↔ screen. It is the only state that
  changes on pan/zoom.
- *(planned)* A visual graph derived from one module: nodes (cells, ports),
  pins and edges (nets, possibly split per bit/bus), each with a stable id that
  maps back to the IR, plus positions and routed edge geometry.
- *(planned)* Layout engines behind a small interface (`run(graph) → positions`),
  run on a worker thread; the UI shows the previous layout until done.
- *(planned)* A uniform-grid spatial index over node bounds and edge segments
  for viewport culling and O(1)-ish mouse picking.
- *(planned)* Level of detail by zoom: at low zoom draw only cell boxes (or
  cluster summaries), add pins, labels and individual wires as zoom increases.

## GUI

- `Window` owns SDL3, the OpenGL 3.3 context and the ImGui backends. The loop is
  event driven: after a few frames without input it blocks on the next event,
  so an idle window uses almost no CPU. It also hosts platform services ImGui
  does not provide: the native file dialog and touchpad pinch gestures.
- `AppUi` owns the panels, the toolbar and status bar, the dock layout (design
  sidebar left, properties right, console under the canvas), fonts, theme and
  the user's `Settings`. Settings are stored in `imgui.ini` through an ImGui
  settings handler, next to the window layout. Panels are plain classes or
  methods with a `draw()`.
- `ConsolePanel` is a log sink with a bounded buffer, drawn with
  `ImGuiListClipper` so only visible lines are submitted, plus a command input
  line that runs `CommandRegistry` commands.
- `CanvasPanel` handles pan/zoom input (scroll, drag, Ctrl+scroll, pinch) and
  today draws a background grid generated only for the visible area, plus a
  placeholder rectangle.
- *(planned)* Graph rendering: geometry is uploaded once into a few large GPU
  buffers (instanced quads for cells/pins, line segments for wires) whenever the
  graph or layout changes. Each frame draws them into an offscreen framebuffer
  with the camera as a shader uniform, and the canvas shows that texture via
  `ImGui::Image`. Text uses a glyph atlas and is drawn only above a zoom threshold.
  Selection/highlight is a per-instance flag buffer, updated in place.

## Dependencies

Code dependencies are pinned to exact commits in `cmake/Dependencies.cmake`
and fetched with CMake `FetchContent` (SDL3 prefers the system package); no
third-party code is vendored. `FETCHCONTENT_SOURCE_DIR_<NAME>` allows offline
builds and local patches.

Fonts (Inter, JetBrains Mono, Lucide icons) are small binary assets vendored in
`third_party/` with their licenses and provenance, and compiled into the
executable by `cmake/EmbedFile.cmake`, so the program needs no data files at
runtime.
