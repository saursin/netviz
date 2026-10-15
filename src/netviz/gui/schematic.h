#pragma once

#include "netviz/layout/layered_layout.h"
#include "netviz/view/spatial_grid.h"

#include <imgui.h>

#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace netviz {

// The schematic of one module: builds its display graph and layout on a
// background thread, then draws it on the canvas and answers picking queries.
//
// Finished schematics are cached per module (a layout depends only on the
// module definition, so every instance of a module shares one), within a
// memory budget; the least recently shown are dropped first.
class SchematicView {
  public:
    enum class State : uint8_t { Empty, Building, Ready, TooLarge, Cancelled };

    SchematicView() = default;
    ~SchematicView() { clear(); }
    SchematicView(const SchematicView&) = delete;
    SchematicView& operator=(const SchematicView&) = delete;

    // Shows `module`: at once if its schematic is cached, otherwise starts
    // laying it out in the background. Does nothing if it is already shown,
    // being laid out or was cancelled (reset_current() allows a retry).
    // `design` must outlive the job and the cache; call clear() before
    // destroying or replacing it.
    void show(const Design& design, ModuleId module);
    // Modules with more display nodes are not laid out (state TooLarge).
    // Raising the limit above a refused module's size lays it out again.
    void set_max_nodes(size_t max_nodes);
    // Stops any job and forgets everything, including the cache.
    void clear();
    // Stops any job and forgets what is shown, but keeps the cache.
    void reset_current();
    // Stops a running job; the state becomes Cancelled.
    void cancel();
    // Installs a finished job. Returns true when a schematic was installed
    // since the last call (also from the cache), e.g. to fit the view.
    bool poll();

    State state() const { return state_; }
    bool busy() const { return state_ == State::Building; }
    LayoutStage stage() const { return progress_.stage.load(std::memory_order_relaxed); }
    ModuleId module() const { return module_; }
    size_t node_count() const { return node_count_; }

    const DisplayGraph* graph() const { return built_ ? &built_->graph : nullptr; }
    const LayoutResult* layout() const { return built_ ? &built_->layout : nullptr; }

    // Display node under a world-space point, or -1.
    int64_t pick(Vec2 world) const;

    // Draws the visible part of the schematic. `selected` and `hovered` are
    // display node indices (or -1); their nets are highlighted.
    void draw(ImDrawList* dl, const Camera2D& camera, ImVec2 origin, const StringPool& names,
              bool light_theme, int64_t selected, int64_t hovered) const;

  private:
    struct Built {
        ModuleId module;
        size_t node_count = 0; // cells and ports
        size_t bytes = 0;      // estimated memory use
        DisplayGraph graph;    // (empty when too large)
        LayoutResult layout;
        SpatialGrid grid;
        std::vector<uint32_t>
            net_begin; // nets of node v: node_nets[net_begin[v] .. net_begin[v+1])
        std::vector<uint32_t> node_nets;
        std::vector<uint8_t> style; // per node: its NodeStyle (colors), see schematic.cc
    };
    struct CacheEntry {
        std::unique_ptr<Built> built;
        uint64_t last_used = 0;
    };

    static std::unique_ptr<Built> build(const Design& design, ModuleId module,
                                        const LayoutOptions& options, std::stop_token stop,
                                        LayoutProgress* progress);
    // Stops the job; a result it already finished goes into the cache.
    void stop_job();
    CacheEntry& add_to_cache(std::unique_ptr<Built> built);
    void install(CacheEntry& entry);
    void evict();

    std::jthread worker_;
    std::mutex mutex_;
    std::unique_ptr<Built> finished_; // handed from the worker to the UI thread
    // UI thread only:
    std::map<uint32_t, CacheEntry> cache_; // by module index
    size_t cache_bytes_ = 0;
    uint64_t clock_ = 0;           // for last_used
    const Built* built_ = nullptr; // what is shown (owned by cache_)
    bool installed_ = false;       // reported by the next poll()
    LayoutProgress progress_;
    State state_ = State::Empty;
    ModuleId module_;
    size_t node_count_ = 0;
    size_t max_nodes_ = LayoutOptions{}.max_nodes;
};

} // namespace netviz
