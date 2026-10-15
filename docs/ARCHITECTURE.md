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
            |  layout                       |   layout contract, layered
            |                               |   layout             (no UI deps)
            +---------------+---------------+
                            |
            +---------------v---------------+
            |  view                         |   camera, display graph,
            |                               |   spatial index      (no UI deps)
            +---------------+---------------+
                            |
            +---------------v---------------+       +--------------------------+
            |  core                         | <---- |  io   (simdjson)         |
            |                               |       |  file importers:         |
            |  logging, commands, netlist   |       |  Yosys JSON              |
            |  IR, connectivity (std only)  |       +--------------------------+
            +-------------------------------+
```

Each layer is a CMake library and may only depend on the layers below it. This
is enforced by target links: `core`, `view` and `layout` do not link
ImGui/SDL/GL, so an accidental include fails to build.

File importers (Yosys JSON today, later Liberty/RTLIL) translate formats into the
core IR. They live in `netviz_io` on top of `core`, so the core never depends on a
parser or file format; `gui` uses `io` to load files.

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

## View and layout

Described in [LAYOUT.md](LAYOUT.md).

- `Camera2D` maps world ↔ screen. It is the only state that changes on
  pan/zoom; `fit()` frames a rectangle.
- `DisplayGraph` (`view`) is a read-only drawing view of one module: nodes
  (cells, instances, ports), pins on fixed sides and nets bundled only where
  that cannot show a false connection. Ids map back to the IR.
- `layered_layout()` (`layout`) turns a `DisplayGraph` into a `LayoutResult`:
  node boxes, pin positions and orthogonal wire segments, all on a grid. The
  contract in `layout.h` is algorithm independent so other engines can plug in.
- `SpatialGrid` (`view`) is a uniform grid over node boxes for viewport
  culling and picking.
- *(planned, milestone 2b)* Level of detail by zoom and batched GPU drawing for
  large modules.

## GUI

- `Window` owns SDL3, the OpenGL 3.3 context and the ImGui backends. The loop is
  event driven: after a few frames without input it blocks on the next event,
  so an idle window uses almost no CPU. It also hosts platform services ImGui
  does not provide: the native file dialog and touchpad pinch gestures.
- `AppUi` owns the panels, the toolbar and status bar, the dock layout (design
  sidebar left, properties right, console under the canvas), fonts, theme and
  the user's `Settings`. Preferences are stored in `settings.json` (in
  `$XDG_CONFIG_HOME/netviz` on Linux; `settings_file.h`, tested without the
  GUI); window and dock layout, panel visibility and recent files in
  `layout.ini` (in `$XDG_STATE_HOME/netviz`), written by ImGui with a small
  netviz section. Other platforms use SDL's per-user folder for both. On
  first start, preferences from an older version's `imgui.ini` are moved
  over; an unreadable `settings.json` is reported and not overwritten. Panels
  are plain classes or methods with a `draw()`.
- **Loading** runs in the background: `AppUi::open_file` imports the file with
  `std::async`, the worker wakes the event loop (`Window::post_wake`) when done,
  and the UI thread installs the design on its next frame. The UI never sees a
  half-built design, and the window keeps responding while large files load.
  The importer publishes its stage and fraction in an `ImportProgress` (two
  atomics, updated every 1024 items, no measurable cost); the status bar shows
  them, redrawing 10 times a second while a load runs. Setting
  `ImportProgress::cancel` stops the import at its next progress check
  (milliseconds; the initial file parse itself cannot be interrupted). The
  status bar's cancel button and closing the window both use it, so quitting
  never waits for a large load to finish.
- Opening a file while a design is open asks first, then frees the current
  design *before* loading the new one, so two large designs are never in memory
  at once.
- `DesignPanels` owns the loaded design and the selection, and draws three
  panes: Hierarchy (instance tree from the top module), Modules (all module
  definitions, with blackboxes such as library gates listed separately) and
  Properties (path, module, ports with bit ranges, counts, parameters and
  attributes).
- `ConsolePanel` is a log sink with a bounded buffer, drawn with
  `ImGuiListClipper` so only visible lines are submitted, plus a command input
  line that runs `CommandRegistry` commands.
- `CanvasPanel` handles pan/zoom input (scroll, drag, Ctrl+scroll, pinch),
  draws a background grid generated only for the visible area, calls back to
  draw the scene and overlay widgets, and reports clicks and keys
  (`CanvasEvents`) to its owner.
- **Schematics**: `SchematicService` builds the display graph and layout of
  a module on a `std::jthread`, one job at a time in the order canvases ask,
  and caches the results per module (every instance of a module shares one)
  within a 256 MB budget, least recently used dropped first. A job nobody has
  asked for in the last few frames (its canvas moved on or is hidden behind
  another tab) is stopped, so hidden canvases cost nothing. The cache is
  emptied when the design closes or reloads; the status bar shows the running
  job with a cancel button.
- **Canvases**: each `SchematicCanvas` is a dockable window showing one place
  (a module reached through an instance path, or a module definition) with
  its own camera and remembered pan/zoom per module. The last focused one is
  active: the toolbar, status bar and Hierarchy/Modules clicks act on it.
  Ctrl+click or middle-click (Hierarchy, Modules, an instance on a canvas),
  context menus or Ctrl+T open another canvas, docked as a tab next to the
  active one. The selection is global (Properties shows it); clicking a cell
  or port selects it, double-clicking an instance opens its module,
  breadcrumbs or Backspace go back up. The Hierarchy and Modules panes mark
  what is shown in a canvas with an eye icon that brings it to the front.
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
