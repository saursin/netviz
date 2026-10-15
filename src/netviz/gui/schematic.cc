#include "netviz/gui/schematic.h"

#include "netviz/gui/window.h"

#include <algorithm>
#include <cctype>
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

// What a node is, for its colors. Cells are grouped by their type name.
enum NodeStyle : uint8_t {
    Logic,
    Sequential,
    Mux,
    Arithmetic,
    OtherCell,
    Instance,
    InPort,
    OutPort,
    InOutPort,
    kStyleCount
};

struct Colors {
    ImU32 accent[kStyleCount]; // header band, port fill and border of each kind of node
    ImU32 body, shadow, port_text, text, dim, wire, bus, highlight, selected, hovered;
};

const Colors kDark = {
    {IM_COL32(32, 128, 128, 255), IM_COL32(128, 82, 184, 255), IM_COL32(184, 128, 32, 255),
     IM_COL32(184, 70, 100, 255), IM_COL32(88, 100, 124, 255), IM_COL32(48, 104, 196, 255),
     IM_COL32(40, 150, 90, 255), IM_COL32(204, 112, 40, 255), IM_COL32(150, 100, 190, 255)},
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
const Colors kLight = {
    {IM_COL32(28, 140, 140, 255), IM_COL32(136, 86, 196, 255), IM_COL32(214, 146, 20, 255),
     IM_COL32(206, 72, 106, 255), IM_COL32(104, 116, 140, 255), IM_COL32(52, 112, 210, 255),
     IM_COL32(40, 160, 92, 255), IM_COL32(222, 118, 36, 255), IM_COL32(150, 100, 190, 255)},
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

bool contains_any(std::string_view s, std::initializer_list<std::string_view> parts) {
    for (std::string_view p : parts)
        if (s.find(p) != std::string_view::npos)
            return true;
    return false;
}

// Guesses a cell's kind from its type name, e.g. "$_DFF_P_" or "NAND".
NodeStyle cell_style(std::string_view type) {
    std::string t;
    for (char ch : type)
        t += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string_view name = t;
    name.remove_prefix(std::min(name.find_first_not_of("$_"), name.size()));
    if (contains_any(name, {"dff", "latch", "flop"}))
        return Sequential;
    if (name.starts_with("mux") || name.starts_with("pmux") || name.starts_with("bmux") ||
        name.starts_with("demux"))
        return Mux;
    static const std::initializer_list<std::string_view> arithmetic = {
        "add",  "sub",  "mul",   "div", "mod", "neg", "alu", "lcu", "macc", "pow", "shl", "shr",
        "sshl", "sshr", "shift", "lt",  "le",  "gt",  "ge",  "eq",  "ne",   "eqx", "nex"};
    for (std::string_view op : arithmetic)
        if (name == op ||
            (name.starts_with(op) && name.size() > op.size() && name[op.size()] == '_'))
            return Arithmetic;
    if (contains_any(name,
                     {"and", "or", "xor", "not", "buf", "inv", "aoi", "oai", "reduce", "logic"}))
        return Logic;
    return OtherCell;
}

NodeStyle node_style(const DisplayNode& node, const StringPool& names) {
    switch (node.kind) {
    case NodeKind::Instance: return Instance;
    case NodeKind::InputPort: return InPort;
    case NodeKind::OutputPort: return OutPort;
    case NodeKind::InOutPort: return InOutPort;
    case NodeKind::Cell: break;
    }
    return cell_style(names.str(node.type));
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
                                                   const LayoutOptions& options,
                                                   std::stop_token stop, LayoutProgress* progress) {
    auto built = std::make_unique<Schematic>();
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
    built->style.reserve(g.nodes().size());
    for (const DisplayNode& node : g.nodes())
        built->style.push_back(node_style(node, design.names()));
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
    worker_ = std::jthread([this, design, module, options](std::stop_token stop) {
        std::unique_ptr<Schematic> built = build(*design, module, options, stop, &progress_);
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

void Schematic::draw(ImDrawList* dl, const Camera2D& camera, ImVec2 origin, const StringPool& names,
                     bool light_theme, int64_t selected, int64_t hovered) const {
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

    // Wires (unhighlighted first, so highlighted ones are drawn on top).
    for (int pass = 0; pass < 2; pass++) {
        for (size_t n = 0; n < layout.nets.size(); n++) {
            if (highlight[n] != pass)
                continue;
            const DisplayNet& net = g.nets()[n];
            bool bus = net.bit_count > 1;
            ImU32 color = pass ? c.highlight : bus ? c.bus : c.wire;
            float thickness = (bus ? 2.5f : 1.2f) * (pass ? 1.5f : 1.0f);
            const NetRoute& route = layout.nets[n];
            for (const Segment& s : route.segments) {
                Rect box{Vec2{std::min(s.a.x, s.b.x), std::min(s.a.y, s.b.y)},
                         Vec2{std::max(s.a.x, s.b.x), std::max(s.a.y, s.b.y)}};
                if (intersects(box, visible))
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
    for (uint32_t v : grid.query(visible)) {
        const DisplayNode& node = g.nodes()[v];
        const Rect& box = layout.nodes[v];
        ImVec2 a = screen(box.min);
        ImVec2 b = screen(box.max);
        ImU32 accent = c.accent[style[v]];
        bool cell = node.kind == NodeKind::Cell || node.kind == NodeKind::Instance;
        bool marked = v == selected || v == hovered;
        ImU32 border = v == selected ? c.selected : v == hovered ? c.hovered : accent;
        float border_px = marked ? 2.5f : 1.2f;

        if (cell) {
            // A box lightly tinted with its kind's color, outlined in it.
            dl->AddRectFilled(ImVec2(a.x + shadow, a.y + shadow),
                              ImVec2(b.x + shadow, b.y + shadow), c.shadow, rounding);
            dl->AddRectFilled(a, b, mix(c.body, accent, kTint), rounding);
            dl->AddRect(a, b, border, rounding, border_px);
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

        // Pin markers, colored like the wire they connect to.
        std::span<const DisplayPin> pins = g.pins_of(node);
        float mark = std::clamp(2.0f * zoom, 1.0f, 3.5f);
        if (cell && mark > 1.0f)
            for (size_t i = 0; i < pins.size(); i++) {
                ImVec2 p = screen(layout.pins[node.pins_begin + i]);
                ImU32 color = pins[i].width > 1 ? c.bus : c.wire;
                dl->AddRectFilled(ImVec2(p.x - mark, p.y - mark), ImVec2(p.x + mark, p.y + mark),
                                  color);
            }

        if (!show_names)
            continue;
        ImVec4 clip(a.x, a.y, b.x, b.y);
        auto line_y = [&](float line) { // top of text centered in text line `line`
            return a.y + (line * lh + (lh - kNameSize) * 0.5f) * zoom;
        };
        if (cell) { // name and type centred at the top, between the pin labels
            auto centered = [&](const std::string& text, float line, ImU32 color) {
                float w = font->CalcTextSizeA(name_px, FLT_MAX, 0.0f, text.c_str()).x;
                dl->AddText(font, name_px, ImVec2((a.x + b.x - w) * 0.5f, line_y(line)), color,
                            text.c_str(), nullptr, 0.0f, &clip);
            };
            float line = 0.0f;
            if (!node.hidden_name) {
                centered(names.str(node.name), line, c.text);
                line += 1.0f;
            }
            centered(names.str(node.type), line, node.hidden_name ? c.text : c.dim);
        } else {
            const std::string& name = names.str(node.name);
            float y = (a.y + b.y - name_px) * 0.5f;
            dl->AddText(font, name_px, ImVec2(a.x + pad, y), c.port_text, name.c_str(), nullptr,
                        0.0f, &clip);
        }

        if (!show_pins || !cell)
            continue;
        for (size_t i = 0; i < pins.size(); i++) {
            const DisplayPin& pin = pins[i];
            ImVec2 p = screen(layout.pins[node.pins_begin + i]);
            float y = p.y - pin_px * 0.5f;
            float inset = pad + mark;
            if (pin.name.valid()) {
                const std::string& label = names.str(pin.name);
                float w = font->CalcTextSizeA(pin_px, FLT_MAX, 0.0f, label.c_str()).x;
                float x = pin.side == PinSide::Left ? p.x + inset : p.x - inset - w;
                dl->AddText(font, pin_px, ImVec2(x, y), c.dim, label.c_str(), nullptr, 0.0f, &clip);
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
