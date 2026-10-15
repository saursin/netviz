#include "netviz/gui/schematic.h"

#include "netviz/gui/window.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace netviz {

namespace {

const LayoutOptions kLayoutOptions;
constexpr size_t kCacheBytes = 256u << 20; // schematic cache budget
constexpr uint64_t kWantedFrames = 3;      // frames a module stays wanted after being asked for
constexpr float kGridCell = 256.0f;        // spatial grid cell size, world units
constexpr float kMinTextPixels = 6.0f;     // smaller text is not drawn
constexpr float kNameSize = 12.0f;         // node text height, world units
constexpr float kPinTextSize = 10.0f;      // pin label height, world units
constexpr float kTint = 0.18f;             // how much of its kind's color a cell box gets

// What a node is, for its colors. Cells take their color from a cell rule
// (rules.h), else the Cell color.
enum NodeStyle : uint8_t { Cell, Instance, InPort, OutPort, InOutPort, kStyleCount };

struct Colors {
    ImU32 accent[kStyleCount]; // port fill and border of each kind of node
    ImU32 body, shadow, port_text, text, dim, wire, bus, highlight, selected, hovered;
};

const Colors kDark = {{IM_COL32(88, 100, 124, 255), IM_COL32(48, 104, 196, 255),
                       IM_COL32(40, 150, 90, 255), IM_COL32(204, 112, 40, 255),
                       IM_COL32(150, 100, 190, 255)},
                      IM_COL32(38, 42, 52, 255),
                      IM_COL32(0, 0, 0, 90),
                      IM_COL32(255, 255, 255, 255),
                      IM_COL32(226, 229, 236, 255),
                      IM_COL32(150, 158, 172, 255),
                      IM_COL32(80, 196, 130, 255),
                      IM_COL32(100, 160, 255, 255),
                      IM_COL32(255, 200, 70, 255),
                      IM_COL32(255, 200, 70, 255),
                      IM_COL32(150, 205, 255, 255)};
const Colors kLight = {{IM_COL32(104, 116, 140, 255), IM_COL32(52, 112, 210, 255),
                        IM_COL32(40, 160, 92, 255), IM_COL32(222, 118, 36, 255),
                        IM_COL32(150, 100, 190, 255)},
                       IM_COL32(255, 255, 255, 255),
                       IM_COL32(0, 0, 0, 40),
                       IM_COL32(255, 255, 255, 255),
                       IM_COL32(30, 32, 40, 255),
                       IM_COL32(100, 106, 120, 255),
                       IM_COL32(30, 150, 90, 255),
                       IM_COL32(40, 100, 220, 255),
                       IM_COL32(230, 140, 0, 255),
                       IM_COL32(230, 140, 0, 255),
                       IM_COL32(40, 110, 220, 255)};

constexpr float kStub = 15.0f; // length of a label net's stub at each pin, world units

NodeStyle node_style(const DisplayNode& node) {
    switch (node.kind) {
    case NodeKind::Instance: return Instance;
    case NodeKind::InputPort: return InPort;
    case NodeKind::OutputPort: return OutPort;
    case NodeKind::InOutPort: return InOutPort;
    case NodeKind::Cell: break;
    }
    return Cell;
}

ImU32 rgb(RuleColor c) {
    return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
}

// A dashed line from a to b in screen space, drawn only inside `clip`. The
// pattern starts at `a`, so it does not crawl when a long line is clipped.
void dashed_line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness, float dash,
                 float gap, const ImVec4& clip) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.0f)
        return;
    float ux = dx / length, uy = dy / length;
    float t0 = 0.0f, t1 = length;
    auto narrow = [&](float p, float d, float lo, float hi) {
        if (d == 0.0f) {
            if (p < lo || p > hi)
                t1 = -1.0f;
            return;
        }
        float s0 = (lo - p) / d, s1 = (hi - p) / d;
        t0 = std::max(t0, std::min(s0, s1));
        t1 = std::min(t1, std::max(s0, s1));
    };
    narrow(a.x, ux, clip.x, clip.z);
    narrow(a.y, uy, clip.y, clip.w);
    float period = dash + gap;
    for (float t = std::floor(t0 / period) * period; t < t1; t += period) {
        float e = std::min(t + dash, length);
        dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t), ImVec2(a.x + ux * e, a.y + uy * e), color,
                    thickness);
    }
}

// `a` blended toward `b` by t (0..1).
ImU32 mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
    ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, 1.0f));
}

bool intersects(const Rect& a, const Rect& b) {
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y;
}

} // namespace

std::unique_ptr<Schematic> SchematicService::build(const Design& design, ModuleId module,
                                                   const LayoutOptions& layout_options,
                                                   std::vector<SignalRule> signals,
                                                   std::vector<CellRule> cells,
                                                   std::stop_token stop, LayoutProgress* progress) {
    auto built = std::make_unique<Schematic>();
    LayoutOptions options = layout_options;
    built->module = module;
    // A display node per cell and port: refuse before building anything.
    const Module& m = design.module(module);
    built->node_count = m.cells().size() + m.ports().size();
    if (built->node_count > options.max_nodes) {
        built->layout.status = LayoutResult::Status::TooLarge;
        built->bytes = sizeof(Schematic);
        return built;
    }

    built->graph = DisplayGraph::build(design, module);
    if (stop.stop_requested())
        return nullptr;
    // Style rules; label nets are left out of the layout.
    RuleMatches matches = match_rules(design, built->graph, signals, cells);
    built->net_rule = std::move(matches.net_rule);
    built->node_rule = std::move(matches.node_rule);
    built->net_label = std::move(matches.net_name);
    built->unrouted.assign(built->net_rule.size(), 0);
    for (size_t n = 0; n < built->net_rule.size(); n++)
        if (built->net_rule[n] != RuleMatches::kNone &&
            signals[built->net_rule[n]].placement == NetPlacement::Label)
            built->unrouted[n] = 1;
    built->signal_rules = std::move(signals);
    built->cell_rules = std::move(cells);
    options.unrouted_nets = built->unrouted;
    built->layout = layered_layout(built->graph, design.names(), options, stop, progress);
    if (built->layout.status == LayoutResult::Status::Cancelled)
        return nullptr;
    if (built->layout.status != LayoutResult::Status::Ok)
        return built;

    built->grid = SpatialGrid(built->layout.nodes, kGridCell);
    // Nets of each node, for highlighting.
    const DisplayGraph& g = built->graph;
    std::vector<std::vector<uint32_t>> per_node(g.nodes().size());
    for (uint32_t n = 0; n < g.nets().size(); n++) {
        const DisplayNet& net = g.nets()[n];
        for (auto list : {g.drivers(net), g.loads(net)})
            for (DisplayPinId p : list) {
                auto& nets = per_node[g.pin(p).node.index];
                if (nets.empty() || nets.back() != n)
                    nets.push_back(n);
            }
    }
    built->pin_net.assign(g.pins().size(), Schematic::kNoNet);
    for (uint32_t n = 0; n < g.nets().size(); n++)
        for (auto list : {g.drivers(g.nets()[n]), g.loads(g.nets()[n])})
            for (DisplayPinId p : list)
                built->pin_net[p.index] = n;

    // Wire picking: a grid over segment boxes (and label stubs), and each
    // bit's display net.
    std::vector<Rect> boxes;
    for (uint32_t n = 0; n < built->layout.nets.size(); n++)
        for (const Segment& seg : built->layout.nets[n].segments) {
            boxes.push_back(Rect{Vec2{std::min(seg.a.x, seg.b.x), std::min(seg.a.y, seg.b.y)},
                                 Vec2{std::max(seg.a.x, seg.b.x), std::max(seg.a.y, seg.b.y)}});
            built->segment_net.push_back(n);
        }
    for (uint32_t p = 0; p < g.pins().size(); p++) {
        uint32_t n = built->pin_net[p];
        if (n == Schematic::kNoNet || !built->unrouted[n])
            continue;
        Vec2 at = built->layout.pins[p];
        float x = g.pins()[p].side == PinSide::Left ? at.x - kStub : at.x + kStub;
        boxes.push_back(Rect{Vec2{std::min(at.x, x), at.y}, Vec2{std::max(at.x, x), at.y}});
        built->segment_net.push_back(n);
    }
    built->wire_grid = SpatialGrid(boxes, kGridCell);
    built->net_of_bit.assign(m.signal_count(), Schematic::kNoNet);
    for (uint32_t n = 0; n < g.nets().size(); n++)
        for (SignalBitId bit : g.bits(g.nets()[n]))
            built->net_of_bit[bit.index] = n;

    built->style.reserve(g.nodes().size());
    for (const DisplayNode& node : g.nodes())
        built->style.push_back(node_style(node));
    built->net_begin.push_back(0);
    for (const auto& nets : per_node) {
        built->node_nets.insert(built->node_nets.end(), nets.begin(), nets.end());
        built->net_begin.push_back(static_cast<uint32_t>(built->node_nets.size()));
    }

    // Rough memory use, for the cache budget.
    size_t bytes =
        sizeof(Schematic) + g.nodes().size_bytes() + g.pins().size_bytes() + g.nets().size_bytes();
    for (const DisplayNet& net : g.nets())
        bytes += (net.bit_count + net.driver_count + net.load_count) * sizeof(uint32_t);
    const LayoutResult& layout = built->layout;
    // Node boxes are stored twice: in the layout and in the spatial grid.
    bytes += layout.nodes.size() * sizeof(Rect) * 2 + layout.pins.size() * sizeof(Vec2);
    for (const NetRoute& route : layout.nets)
        bytes += sizeof(NetRoute) + route.segments.size() * sizeof(Segment) +
                 route.junctions.size() * sizeof(Vec2);
    bytes += (built->node_nets.size() + built->net_begin.size()) * sizeof(uint32_t) * 2 +
             built->style.size();
    bytes += g.nodes().size() * sizeof(uint32_t) * 2; // spatial grid entries (about)
    bytes += boxes.size() * (sizeof(Rect) + 3 * sizeof(uint32_t)) +
             built->net_of_bit.size() * sizeof(uint32_t); // wire picking
    built->bytes = bytes;
    return built;
}

bool SchematicService::wanted(ModuleId module) const {
    auto it = asked_.find(module.index);
    return it != asked_.end() && it->second + kWantedFrames >= frame_;
}

const Schematic* SchematicService::get(const Design& design, ModuleId module) {
    design_ = &design;
    asked_[module.index] = frame_;
    if (auto it = cache_.find(module.index); it != cache_.end()) {
        it->second.last_used = frame_;
        return it->second.schematic.get();
    }
    if (cancelled_.contains(module.index) || running_ == module ||
        std::find(queue_.begin(), queue_.end(), module) != queue_.end())
        return nullptr;
    if (running_)
        queue_.push_back(module);
    else
        start(module);
    return nullptr;
}

SchematicService::Status SchematicService::status(ModuleId module) const {
    if (auto it = cache_.find(module.index); it != cache_.end())
        return it->second.schematic->too_large() ? Status::TooLarge : Status::Ready;
    if (cancelled_.contains(module.index))
        return Status::Cancelled;
    if (running_ == module)
        return Status::Building;
    if (std::find(queue_.begin(), queue_.end(), module) != queue_.end())
        return Status::Queued;
    return Status::None;
}

void SchematicService::start(ModuleId module) {
    running_ = module;
    progress_.stage.store(LayoutStage::Ranking, std::memory_order_relaxed);
    const Design* design = design_;
    LayoutOptions options = kLayoutOptions;
    options.max_nodes = max_nodes_;
    worker_ = std::jthread([this, design, module, options, signals = signal_rules_,
                            cells = cell_rules_](std::stop_token stop) {
        std::unique_ptr<Schematic> built =
            build(*design, module, options, signals, cells, stop, &progress_);
        if (!built)
            return; // stopped
        {
            std::lock_guard lock(mutex_);
            finished_ = std::move(built);
        }
        Window::post_wake(); // the UI thread collects it on its next frame
    });
}

void SchematicService::stop_job() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    running_.reset();
    std::unique_ptr<Schematic> done;
    {
        std::lock_guard lock(mutex_);
        done = std::move(finished_);
    }
    if (done) // finished just before the stop: keep it
        collect(std::move(done));
}

void SchematicService::collect(std::unique_ptr<Schematic> schematic) {
    CacheEntry& entry = cache_[schematic->module.index];
    if (entry.schematic)
        cache_bytes_ -= entry.schematic->bytes;
    cache_bytes_ += schematic->bytes;
    entry.schematic = std::move(schematic);
    entry.last_used = frame_;
}

bool SchematicService::poll() {
    frame_++;
    bool installed = false;
    std::unique_ptr<Schematic> done;
    {
        std::lock_guard lock(mutex_);
        done = std::move(finished_);
    }
    if (done) {
        if (worker_.joinable())
            worker_.join(); // the job has finished; this returns at once
        running_.reset();
        collect(std::move(done));
        installed = true;
    }
    if (running_ && !wanted(*running_))
        stop_job();
    std::erase_if(queue_, [&](ModuleId m) { return !wanted(m); });
    if (!running_ && !queue_.empty()) {
        ModuleId next = queue_.front();
        queue_.pop_front();
        start(next);
    }
    evict();
    return installed;
}

void SchematicService::cancel() {
    if (!running_)
        return;
    cancelled_.insert(running_->index);
    stop_job();
}

void SchematicService::retry(ModuleId module) {
    cancelled_.erase(module.index);
}

void SchematicService::relayout(ModuleId module) {
    cancelled_.erase(module.index);
    if (running_ == module)
        stop_job();
    if (auto it = cache_.find(module.index); it != cache_.end()) {
        cache_bytes_ -= it->second.schematic->bytes;
        cache_.erase(it);
    }
}

void SchematicService::clear() {
    stop_job();
    queue_.clear();
    asked_.clear();
    cache_.clear();
    cancelled_.clear();
    cache_bytes_ = 0;
    design_ = nullptr;
}

void SchematicService::set_max_nodes(size_t max_nodes) {
    if (max_nodes == max_nodes_)
        return;
    max_nodes_ = max_nodes;
    // Refusals were for the old limit: check those modules again when asked.
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.schematic->too_large()) {
            cache_bytes_ -= it->second.schematic->bytes;
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void SchematicService::set_rules(const std::vector<SignalRule>& signals,
                                 const std::vector<CellRule>& cells) {
    if (signals == signal_rules_ && cells == cell_rules_)
        return;
    signal_rules_ = signals;
    cell_rules_ = cells;
    stop_job();
    queue_.clear();
    cache_.clear();
    cache_bytes_ = 0;
}

// Drops the least recently used schematics, except those shown in the last
// few frames, until the cache fits its budget.
void SchematicService::evict() {
    while (cache_bytes_ > kCacheBytes) {
        auto oldest = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it)
            if (it->second.last_used + kWantedFrames < frame_ &&
                (oldest == cache_.end() || it->second.last_used < oldest->second.last_used))
                oldest = it;
        if (oldest == cache_.end())
            return;
        cache_bytes_ -= oldest->second.schematic->bytes;
        cache_.erase(oldest);
    }
}

int64_t Schematic::pick_net(Vec2 p, float tolerance) const {
    if (too_large())
        return -1;
    Rect area{Vec2{p.x - tolerance, p.y - tolerance}, Vec2{p.x + tolerance, p.y + tolerance}};
    int64_t best = -1;
    float best_distance = tolerance;
    for (uint32_t i : wire_grid.query(area)) {
        // Segments are axis-aligned, so this is the distance to the segment.
        const Rect& box = wire_grid.rect(i);
        float dx = std::max({box.min.x - p.x, 0.0f, p.x - box.max.x});
        float dy = std::max({box.min.y - p.y, 0.0f, p.y - box.max.y});
        float distance = std::max(dx, dy);
        if (distance <= best_distance) {
            best_distance = distance;
            best = segment_net[i];
        }
    }
    return best;
}

int64_t Schematic::net_of(SigBit bit) const {
    if (bit.is_constant() || bit.signal().index >= net_of_bit.size())
        return -1;
    uint32_t net = net_of_bit[bit.signal().index];
    return net == kNoNet ? -1 : net;
}

void Schematic::draw(ImDrawList* dl, const Camera2D& camera, ImVec2 origin, const StringPool& names,
                     bool light_theme, const Marks& marks) const {
    int64_t selected = marks.selected_node;
    int64_t hovered = marks.hovered_node;
    const Colors& c = light_theme ? kLight : kDark;
    const DisplayGraph& g = graph;
    float zoom = camera.zoom();
    Rect visible = camera.visible_world_rect();
    auto screen = [&](Vec2 w) {
        Vec2 s = camera.world_to_screen(w);
        return ImVec2(origin.x + s.x, origin.y + s.y);
    };
    ImFont* font = ImGui::GetFont();
    float name_px = kNameSize * zoom;
    float pin_px = kPinTextSize * zoom;
    bool show_names = name_px >= kMinTextPixels;
    bool show_pins = pin_px >= kMinTextPixels;

    // Nets touching the selected or hovered node are highlighted.
    std::vector<char> highlight(g.nets().size(), 0);
    for (int64_t node : {selected, hovered}) {
        if (node < 0)
            continue;
        for (uint32_t k = net_begin[node]; k < net_begin[node + 1]; k++)
            highlight[node_nets[k]] = 1;
    }
    for (int64_t net : {marks.selected_net, marks.hovered_net})
        if (net >= 0)
            highlight[net] = 1;

    ImVec4 clip(dl->GetClipRectMin().x, dl->GetClipRectMin().y, dl->GetClipRectMax().x,
                dl->GetClipRectMax().y);
    auto rule_of_net = [&](uint32_t n) -> const SignalRule* {
        return n < net_rule.size() && net_rule[n] != RuleMatches::kNone ? &signal_rules[net_rule[n]]
                                                                        : nullptr;
    };

    // Wires (unhighlighted first, so highlighted ones are drawn on top).
    for (int pass = 0; pass < 2; pass++) {
        for (size_t n = 0; n < layout.nets.size(); n++) {
            if (highlight[n] != pass || unrouted[n])
                continue;
            const DisplayNet& net = g.nets()[n];
            const SignalRule* rule = rule_of_net(static_cast<uint32_t>(n));
            bool bus = net.bit_count > 1;
            ImU32 color = pass ? c.highlight : rule ? rgb(rule->color) : bus ? c.bus : c.wire;
            float thickness =
                (bus ? 2.5f : 1.2f) * (rule ? rule->width : 1.0f) * (pass ? 1.5f : 1.0f);
            bool dashed = rule && rule->line == LineStyle::Dashed;
            const NetRoute& route = layout.nets[n];
            for (const Segment& s : route.segments) {
                Rect box{Vec2{std::min(s.a.x, s.b.x), std::min(s.a.y, s.b.y)},
                         Vec2{std::max(s.a.x, s.b.x), std::max(s.a.y, s.b.y)}};
                if (!intersects(box, visible))
                    continue;
                if (dashed)
                    dashed_line(dl, screen(s.a), screen(s.b), color, thickness, 6.0f, 4.0f, clip);
                else
                    dl->AddLine(screen(s.a), screen(s.b), color, thickness);
            }
            float dot = std::clamp(3.0f * zoom, 2.0f, 5.0f);
            for (Vec2 j : route.junctions)
                if (intersects(Rect{j, j}, visible))
                    dl->AddCircleFilled(screen(j), dot, color);
            // Bus width next to the driving pin.
            if (bus && show_pins && !g.drivers(net).empty()) {
                Vec2 p = layout.pins[g.drivers(net)[0].index];
                std::string width = std::to_string(net.bit_count);
                ImVec2 at = screen(Vec2{p.x + 3.0f, p.y - kPinTextSize - 2.0f});
                dl->AddText(font, pin_px, at, color, width.c_str());
            }
        }
    }

    // Nodes.
    const float lh = kLayoutOptions.line_height;
    float rounding = std::min(4.0f * zoom, 8.0f);
    float shadow = std::clamp(3.0f * zoom, 1.0f, 6.0f);
    float pad = 4.0f * zoom;
    // Nodes near the view (label stubs and names reach a little outside them).
    Rect around{Vec2{visible.min.x - 100.0f, visible.min.y},
                Vec2{visible.max.x + 100.0f, visible.max.y}};
    for (uint32_t v : grid.query(around)) {
        const DisplayNode& node = g.nodes()[v];
        const Rect& box = layout.nodes[v];
        ImVec2 a = screen(box.min);
        ImVec2 b = screen(box.max);
        const CellRule* crule = node.kind == NodeKind::Cell && node_rule[v] != RuleMatches::kNone
                                    ? &cell_rules[node_rule[v]]
                                    : nullptr;
        ImU32 accent = crule ? rgb(crule->color) : c.accent[style[v]];
        bool cell = node.kind == NodeKind::Cell || node.kind == NodeKind::Instance;
        bool marked = v == selected || v == hovered;
        ImU32 border = v == selected ? c.selected : v == hovered ? c.hovered : accent;
        float border_px = marked ? 2.5f : 1.2f * (crule ? crule->border_width : 1.0f);
        BorderStyle border_style = marked || !crule ? BorderStyle::Solid : crule->border;

        if (cell) {
            // A box lightly tinted with its kind's color, outlined in it.
            dl->AddRectFilled(ImVec2(a.x + shadow, a.y + shadow),
                              ImVec2(b.x + shadow, b.y + shadow), c.shadow, rounding);
            dl->AddRectFilled(a, b, mix(c.body, accent, kTint), rounding);
            if (border_style == BorderStyle::Solid) {
                dl->AddRect(a, b, border, rounding, border_px);
            } else if (border_style != BorderStyle::None) {
                float dash = border_style == BorderStyle::Dashed ? 6.0f : border_px;
                float gap = border_style == BorderStyle::Dashed ? 4.0f : border_px * 2.0f;
                ImVec2 corners[] = {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)};
                for (int k = 0; k < 4; k++)
                    dashed_line(dl, corners[k], corners[(k + 1) % 4], border, border_px, dash, gap,
                                clip);
            }
        } else {
            // Ports: an arrow pointing in the direction of signal flow.
            float tip = std::min((b.y - a.y) * 0.5f, b.x - a.x);
            float mid = (a.y + b.y) * 0.5f;
            ImVec2 pts[6];
            int count = 0;
            if (node.kind == NodeKind::InOutPort)
                pts[count++] = ImVec2(a.x, mid), pts[count++] = ImVec2(a.x + tip, a.y);
            else
                pts[count++] = ImVec2(a.x, a.y);
            pts[count++] = ImVec2(b.x - tip, a.y);
            pts[count++] = ImVec2(b.x, mid);
            pts[count++] = ImVec2(b.x - tip, b.y);
            pts[count++] =
                node.kind == NodeKind::InOutPort ? ImVec2(a.x + tip, b.y) : ImVec2(a.x, b.y);
            dl->AddConvexPolyFilled(pts, count, accent);
            if (marked)
                dl->AddPolyline(pts, count, border, border_px, ImDrawFlags_Closed);
        }

        // Pin markers, colored like the wire they connect to; the clock ">"
        // inside the box; and for label nets a stub with the net's name.
        std::span<const DisplayPin> pins = g.pins_of(node);
        float mark = std::clamp(2.0f * zoom, 1.0f, 3.5f);
        float clock = 5.0f * zoom; // size of the clock marker
        for (size_t i = 0; i < pins.size(); i++) {
            uint32_t pin = node.pins_begin + static_cast<uint32_t>(i);
            uint32_t net = pin_net[pin];
            const SignalRule* rule = net != kNoNet ? rule_of_net(net) : nullptr;
            ImVec2 p = screen(layout.pins[pin]);
            if (cell && mark > 1.0f) {
                ImU32 color = rule ? rgb(rule->color) : pins[i].width > 1 ? c.bus : c.wire;
                dl->AddRectFilled(ImVec2(p.x - mark, p.y - mark), ImVec2(p.x + mark, p.y + mark),
                                  color);
            }
            if (cell && rule && rule->marker == PinMarker::Clock && pins[i].side == PinSide::Left &&
                clock >= 2.0f)
                dl->AddTriangle(ImVec2(p.x, p.y - clock), ImVec2(p.x + clock, p.y),
                                ImVec2(p.x, p.y + clock), rgb(rule->color), 1.5f);
            if (net != kNoNet && unrouted[net]) {
                float dir = pins[i].side == PinSide::Left ? -1.0f : 1.0f;
                ImU32 color = highlight[net] ? c.highlight : rule ? rgb(rule->color) : c.wire;
                ImVec2 end(p.x + dir * kStub * zoom, p.y);
                dl->AddLine(p, end, color, (rule ? rule->width : 1.0f) * 1.2f);
                if (show_pins && !net_label[net].empty()) {
                    const std::string& text = net_label[net];
                    float w = font->CalcTextSizeA(pin_px, FLT_MAX, 0.0f, text.c_str()).x;
                    float x = dir < 0.0f ? end.x - 3.0f - w : end.x + 3.0f;
                    dl->AddText(font, pin_px, ImVec2(x, p.y - pin_px * 0.5f), color, text.c_str());
                }
            }
        }

        if (!show_names)
            continue;
        ImVec4 box_clip(a.x, a.y, b.x, b.y);
        auto line_y = [&](float line) { // top of text centered in text line `line`
            return a.y + (line * lh + (lh - kNameSize) * 0.5f) * zoom;
        };
        if (cell) { // name and type centred at the top, between the pin labels
            auto centered = [&](const std::string& text, float line, ImU32 color) {
                float w = font->CalcTextSizeA(name_px, FLT_MAX, 0.0f, text.c_str()).x;
                dl->AddText(font, name_px, ImVec2((a.x + b.x - w) * 0.5f, line_y(line)), color,
                            text.c_str(), nullptr, 0.0f, &box_clip);
            };
            float line = 0.0f;
            if (!node.hidden_name) {
                centered(names.str(node.name), line, c.text);
                line += 1.0f;
            }
            if (!crule || crule->show_type || node.hidden_name)
                centered(names.str(node.type), line, node.hidden_name ? c.text : c.dim);
        } else {
            const std::string& name = names.str(node.name);
            float y = (a.y + b.y - name_px) * 0.5f;
            dl->AddText(font, name_px, ImVec2(a.x + pad, y), c.port_text, name.c_str(), nullptr,
                        0.0f, &box_clip);
        }

        if (!show_pins || !cell)
            continue;
        for (size_t i = 0; i < pins.size(); i++) {
            const DisplayPin& pin = pins[i];
            ImVec2 p = screen(layout.pins[node.pins_begin + i]);
            float y = p.y - pin_px * 0.5f;
            float inset = pad + mark;
            uint32_t net = pin_net[node.pins_begin + i];
            const SignalRule* rule = net != kNoNet ? rule_of_net(net) : nullptr;
            if (rule && rule->marker == PinMarker::Clock && pin.side == PinSide::Left)
                inset += clock; // after the clock marker
            if (pin.name.valid()) {
                const std::string& label = names.str(pin.name);
                float w = font->CalcTextSizeA(pin_px, FLT_MAX, 0.0f, label.c_str()).x;
                float x = pin.side == PinSide::Left ? p.x + inset : p.x - inset - w;
                dl->AddText(font, pin_px, ImVec2(x, y), c.dim, label.c_str(), nullptr, 0.0f, &box_clip);
            }
            if (!pin.constant.empty()) { // constant inputs: value outside the box
                float w = font->CalcTextSizeA(pin_px, FLT_MAX, 0.0f, pin.constant.c_str()).x;
                float x = pin.side == PinSide::Left ? p.x - inset - w : p.x + inset;
                dl->AddText(font, pin_px, ImVec2(x, y), c.dim, pin.constant.c_str());
            }
        }
    }
}

} // namespace netviz
