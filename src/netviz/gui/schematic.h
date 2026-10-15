#pragma once

#include "netviz/gui/rules.h"
#include "netviz/layout/layered_layout.h"
#include "netviz/view/spatial_grid.h"

#include <imgui.h>

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace netviz {

// The schematic of one module: display graph, layout and what drawing and
// picking need. Built off the UI thread, then read-only.
struct Schematic {
    ModuleId module;
    size_t node_count = 0; // cells and ports
    size_t bytes = 0;      // estimated memory use
    DisplayGraph graph;    // (empty when too large)
    LayoutResult layout;
    SpatialGrid grid;
    std::vector<uint32_t> net_begin; // nets of node v: node_nets[net_begin[v] .. net_begin[v+1])
    std::vector<uint32_t> node_nets;
    std::vector<uint8_t> style; // per node: its NodeStyle (colors), see schematic.cc
    // Style rules as matched when built (copies; see rules.h).
    std::vector<SignalRule> signal_rules;
    std::vector<CellRule> cell_rules;
    std::vector<uint16_t> net_rule;     // per display net: signal rule, or RuleMatches::kNone
    std::vector<uint16_t> node_rule;    // per display node: cell rule, or RuleMatches::kNone
    std::vector<std::string> net_label; // per display net: its name (shown for label nets)
    std::vector<uint8_t> unrouted;      // per display net: drawn as labels, not wires
    std::vector<uint32_t> pin_net;      // per display pin: its display net, or kNoNet
    SpatialGrid wire_grid;              // over the bounding boxes of all wire segments
    std::vector<uint32_t> segment_net;  // per segment in wire_grid: its display net
    std::vector<uint32_t> net_of_bit;   // per signal bit of the module: display net, or kNoNet
    static constexpr uint32_t kNoNet = UINT32_MAX;

    bool too_large() const { return layout.status == LayoutResult::Status::TooLarge; }
    // Display node under a world-space point, or -1.
    int64_t pick(Vec2 world) const { return too_large() ? -1 : grid.pick(world); }
    // Display net with a wire within `tolerance` (world units) of a point, or -1.
    int64_t pick_net(Vec2 world, float tolerance) const;
    // The display net carrying a signal bit, or -1.
    int64_t net_of(SigBit bit) const;

    // What is selected or under the cursor: display node and net indices, -1
    // for none. Those nets, and the nets of those nodes, are highlighted.
    struct Marks {
        int64_t selected_node = -1, hovered_node = -1;
        int64_t selected_net = -1, hovered_net = -1;
    };
    // Draws the visible part.
    void draw(ImDrawList* dl, const Camera2D& camera, ImVec2 origin, const StringPool& names,
              bool light_theme, const Marks& marks) const;
};

// Builds schematics in the background and caches them, for all canvases.
//
// Canvases ask for the module they show every frame they are visible
// (get()). Jobs run one at a time in the order asked; a job or queued module
// nobody has asked for in the last few frames (its canvas moved on or is
// hidden) is dropped. Finished schematics are cached per module (a layout
// depends only on the module definition, so every instance of a module
// shares one) within a memory budget, least recently used dropped first,
// never one shown in the last few frames.
class SchematicService {
  public:
    enum class Status : uint8_t { None, Queued, Building, Ready, TooLarge, Cancelled };

    SchematicService() = default;
    ~SchematicService() { clear(); }
    SchematicService(const SchematicService&) = delete;
    SchematicService& operator=(const SchematicService&) = delete;

    // Call once per frame before the canvases draw: installs finished jobs,
    // drops unwanted ones and starts the next. Returns true if a schematic
    // was installed.
    bool poll();

    // The schematic of `module` if built (also when refused as too large);
    // otherwise queues it, unless cancelled, and returns null. `design` must
    // outlive the service's jobs and cache; call clear() before destroying
    // or replacing it.
    const Schematic* get(const Design& design, ModuleId module);
    Status status(ModuleId module) const;

    // Stops the running job; its module is not laid out again until retry().
    void cancel();
    void retry(ModuleId module);
    // Forgets the module's schematic (and stops its job), so it is laid out
    // again when next asked for.
    void relayout(ModuleId module);
    // Stops all jobs and forgets everything.
    void clear();

    // Modules with more display nodes are not laid out (Status::TooLarge).
    // Changing the limit forgets earlier refusals, so they are checked again.
    void set_max_nodes(size_t max_nodes);
    // Style rules for new schematics; changing them lays every module out
    // again (label placement changes the layout).
    void set_rules(const std::vector<SignalRule>& signals, const std::vector<CellRule>& cells);

    bool busy() const { return running_.has_value(); }
    std::optional<ModuleId> building() const { return running_; }
    LayoutStage stage() const { return progress_.stage.load(std::memory_order_relaxed); }

  private:
    struct CacheEntry {
        std::unique_ptr<Schematic> schematic;
        uint64_t last_used = 0; // frame
    };

    static std::unique_ptr<Schematic> build(const Design& design, ModuleId module,
                                            const LayoutOptions& options,
                                            std::vector<SignalRule> signals,
                                            std::vector<CellRule> cells, std::stop_token stop,
                                            LayoutProgress* progress);
    void start(ModuleId module);
    // Stops the job; a result it already finished goes into the cache.
    void stop_job();
    void collect(std::unique_ptr<Schematic> schematic);
    bool wanted(ModuleId module) const;
    void evict();

    const Design* design_ = nullptr;
    std::jthread worker_;
    std::mutex mutex_;
    std::unique_ptr<Schematic> finished_; // handed from the worker to the UI thread
    LayoutProgress progress_;
    // UI thread only:
    std::optional<ModuleId> running_;
    std::deque<ModuleId> queue_;
    std::map<uint32_t, uint64_t> asked_;   // module index -> last frame it was asked for
    std::map<uint32_t, CacheEntry> cache_; // by module index
    std::set<uint32_t> cancelled_;         // module indices
    size_t cache_bytes_ = 0;
    uint64_t frame_ = 0;
    size_t max_nodes_ = LayoutOptions{}.max_nodes;
    std::vector<SignalRule> signal_rules_;
    std::vector<CellRule> cell_rules_;
};

} // namespace netviz
