#include "netviz/gui/schematic_canvas.h"

#include <imgui.h>

#include <cmath>
#include <format>

namespace netviz {

namespace {

constexpr float kNetPickPixels = 5.0f; // how close (on screen) a click must be to a wire

std::vector<SigBit> net_bits(const DisplayGraph& graph, const DisplayNet& net) {
    std::vector<SigBit> bits;
    for (SignalBitId bit : graph.bits(net))
        bits.push_back(SigBit::signal(bit));
    return bits;
}

} // namespace

std::string SchematicCanvas::window_id(int id) {
    return std::format("###canvas{}", id);
}

// "cpu", "dp (datapath)" or "alu8 (definition)", then the fixed ID part.
std::string SchematicCanvas::window_name(const Design* design) const {
    std::string title = "Canvas";
    if (design && place_.module.valid()) {
        const StringPool& names = design->names();
        const std::string& module = names.str(design->module(place_.module).name());
        if (is_definition(*design)) {
            title = module + " (definition)";
        } else if (place_.path.empty()) {
            title = module;
        } else {
            // The instance's name in its parent module.
            ModuleId parent = *design->top();
            for (size_t i = 0; i + 1 < place_.path.size(); i++)
                parent = *design->find_module(design->module(parent).cell(place_.path[i]).type);
            const Cell& cell = design->module(parent).cell(place_.path.back());
            title = std::format("{} ({})", names.str(cell.name), module);
        }
    }
    return title + window_id(id_);
}

bool SchematicCanvas::is_definition(const Design& design) const {
    return place_.path.empty() && place_.module != design.top();
}

void SchematicCanvas::navigate(Place place) {
    place.kind = Place::Kind::None;
    if (place.path == place_.path && place.module == place_.module)
        return;
    if (shown_)
        views_[shown_module_.index] = {camera_.center(), camera_.zoom()};
    place_ = std::move(place);
    shown_ = nullptr;
    hovered_node_ = -1;
}

void SchematicCanvas::fit_view(const Context& ctx) {
    const Design* design = ctx.panels.design();
    const Schematic* s =
        design && place_.module.valid() ? ctx.schematics.get(*design, place_.module) : nullptr;
    if (s && !s->too_large())
        camera_.fit(s->layout.bounds);
    else
        camera_.reset();
}

// The schematic to draw this frame; restores the module's last view (or
// fits) when it is new on this canvas.
const Schematic* SchematicCanvas::schematic(Context& ctx) {
    const Design* design = ctx.panels.design();
    if (!design || !place_.module.valid()) {
        shown_ = nullptr;
        return nullptr;
    }
    const Schematic* s = ctx.schematics.get(*design, place_.module);
    if (s && s->too_large())
        s = nullptr;
    if (s && (s != shown_ || shown_module_ != place_.module)) {
        auto it = views_.find(place_.module.index);
        if (it != views_.end()) {
            camera_.set_center(it->second.center);
            camera_.set_zoom(it->second.zoom);
        } else {
            camera_.fit(s->layout.bounds);
        }
    }
    shown_ = s;
    shown_module_ = place_.module;
    return s;
}

SchematicCanvas::Requests SchematicCanvas::draw(Context& ctx, bool* open) {
    Requests requests;
    if (dock_at_) {
        ImGui::SetNextWindowDockID(*dock_at_, ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        dock_at_.reset();
    }
    camera_.set_zoom_limits(ctx.settings.min_zoom, ctx.settings.max_zoom);
    CanvasOptions options;
    options.show_grid = ctx.settings.show_grid;
    options.light_theme = ctx.settings.theme == Theme::Light;
    std::string name = window_name(ctx.panels.design());
    CanvasEvents events = panel_.draw(
        name.c_str(), open, camera_, options, ctx.pinch_zoom,
        [&](ImDrawList* dl, ImVec2 origin) { draw_scene(ctx, dl, origin); },
        [&](ImVec2 origin) { draw_overlay(ctx, origin); });
    handle_events(ctx, events, requests);
    draw_tooltip(ctx);
    return requests;
}

Schematic::Marks SchematicCanvas::marks(const Context& ctx, const Schematic& s) const {
    Schematic::Marks m;
    m.hovered_node = hovered_node_;
    m.hovered_net = hovered_net_;
    const Design* design = ctx.panels.design();
    const auto& selection = ctx.panels.selection();
    if (!selection || selection->module != place_.module || selection->path != place_.path)
        return m;
    switch (selection->kind) {
    case Place::Kind::Cell: m.selected_node = s.graph.node_of_cell(selection->cell).index; break;
    case Place::Kind::Port:
        m.selected_node =
            s.graph.node_of_port(design->module(selection->module), selection->port).index;
        break;
    case Place::Kind::Net:
        if (!selection->bits.empty())
            m.selected_net = s.net_of(selection->bits[0]);
        break;
    case Place::Kind::None: break;
    }
    return m;
}

// The schematic, or a message saying why there is none.
void SchematicCanvas::draw_scene(Context& ctx, ImDrawList* dl, ImVec2 origin) {
    const Schematic* s = schematic(ctx);
    hovered_node_ = s && panel_.hovered() ? s->pick(panel_.mouse_world()) : -1;
    // Wires a few pixels wide are easy to hit at any zoom.
    hovered_net_ = s && panel_.hovered() && hovered_node_ < 0
                       ? s->pick_net(panel_.mouse_world(), kNetPickPixels / camera_.zoom())
                       : -1;
    const Design* design = ctx.panels.design();
    if (!design || !place_.module.valid())
        return;
    const StringPool& names = design->names();
    if (s) {
        s->draw(dl, camera_, origin, names, ctx.settings.theme == Theme::Light, marks(ctx, *s));
        return;
    }

    std::string message;
    const std::string& module = names.str(design->module(place_.module).name());
    switch (ctx.schematics.status(place_.module)) {
    case SchematicService::Status::Queued:
    case SchematicService::Status::Building:
        message = std::format("Laying out {} ...", module);
        break;
    case SchematicService::Status::TooLarge:
        message =
            std::format("{} has {} cells and ports, more than the schematic limit of {}.\n"
                        "Open a submodule from the Hierarchy, or raise the limit in\n"
                        "Settings > Preferences > Canvas (large schematics are slow to draw).",
                        module,
                        design->module(place_.module).cells().size() +
                            design->module(place_.module).ports().size(),
                        ctx.settings.max_schematic_nodes);
        break;
    case SchematicService::Status::Cancelled:
        message = std::format("Layout of {} stopped.", module);
        break;
    default: return;
    }
    Vec2 size = camera_.viewport_size();
    ImVec2 text = ImGui::CalcTextSize(message.c_str());
    ImVec2 at(std::floor(origin.x + (size.x - text.x) * 0.5f),
              std::floor(origin.y + (size.y - text.y) * 0.5f));
    dl->AddText(at, ImGui::GetColorU32(ImGuiCol_TextDisabled), message.c_str());
}

// Breadcrumbs of the shown place (click one to go up) and, after a cancelled
// layout, a button to run it again.
void SchematicCanvas::draw_overlay(Context& ctx, ImVec2 origin) {
    const Design* design = ctx.panels.design();
    if (!design || !place_.module.valid())
        return;
    const StringPool& names = design->names();
    const ImGuiStyle& style = ImGui::GetStyle();

    if (ctx.schematics.status(place_.module) == SchematicService::Status::Cancelled) {
        Vec2 size = camera_.viewport_size();
        const char* label = "Lay Out Again";
        float width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
        ImGui::SetCursorScreenPos(
            ImVec2(std::floor(origin.x + (size.x - width) * 0.5f),
                   std::floor(origin.y + size.y * 0.5f + ImGui::GetTextLineHeightWithSpacing())));
        if (ImGui::Button(label))
            ctx.schematics.retry(place_.module);
    }

    ImGui::SetCursorScreenPos(
        ImVec2(origin.x + style.WindowPadding.x, origin.y + style.WindowPadding.y));
    auto module_name = [&](ModuleId m) { return names.str(design->module(m).name()); };
    std::vector<std::string> labels;
    if (is_definition(*design))
        labels.push_back(module_name(place_.module));
    else
        labels = ctx.panels.path_labels(place_.path);
    std::optional<Place> go_to;
    ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_FrameBg]);
    for (size_t i = 0; i < labels.size(); i++) {
        std::vector<CellId> prefix(place_.path.begin(), place_.path.begin() + i);
        std::string label = labels[i];
        if (i > 0) {
            label += std::format(" ({})", module_name(ctx.panels.module_at(prefix)));
            ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
            ImGui::TextDisabled("›");
            ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        }
        ImGui::PushID(static_cast<int>(i));
        bool current = i + 1 == labels.size();
        ImGui::BeginDisabled(current);
        if (ImGui::SmallButton(label.c_str()) && !current)
            go_to = Place::place(prefix, ctx.panels.module_at(prefix));
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::PopStyleColor();
    if (go_to) {
        navigate(*go_to);
        ctx.panels.select(*go_to);
    }
}

void SchematicCanvas::draw_tooltip(Context& ctx) const {
    if (hovered_net_ >= 0 && shown_ && ImGui::BeginTooltip()) {
        const DisplayGraph& graph = shown_->graph;
        const DisplayNet& net = graph.nets()[hovered_net_];
        std::vector<SigBit> bits = net_bits(graph, net);
        ImGui::TextUnformatted(ctx.panels.describe_bits(place_.module, bits).c_str());
        ImGui::TextDisabled("%u bit%s, %u driver%s, %u load%s", net.bit_count,
                            net.bit_count == 1 ? "" : "s", net.driver_count,
                            net.driver_count == 1 ? "" : "s", net.load_count,
                            net.load_count == 1 ? "" : "s");
        ImGui::EndTooltip();
        return;
    }
    if (hovered_node_ < 0 || !shown_ || !ImGui::BeginTooltip())
        return;
    const StringPool& names = ctx.panels.design()->names();
    const DisplayGraph& graph = shown_->graph;
    const DisplayNode& n = graph.nodes()[hovered_node_];
    ImGui::TextUnformatted(names.str(n.name).c_str());
    switch (n.kind) {
    case NodeKind::Cell:
        ImGui::TextDisabled("%s, %u pins", names.str(n.type).c_str(), n.pin_count);
        break;
    case NodeKind::Instance:
        ImGui::TextDisabled("instance of %s, %u pins", names.str(n.type).c_str(), n.pin_count);
        ImGui::TextDisabled("Double-click to open, middle-click to open in a new canvas");
        break;
    default: {
        const char* direction = n.kind == NodeKind::InputPort    ? "input"
                                : n.kind == NodeKind::OutputPort ? "output"
                                                                 : "inout";
        ImGui::TextDisabled("%s port, %u bits", direction, graph.pins_of(n)[0].width);
        break;
    }
    }
    ImGui::EndTooltip();
}

// Click selects the cell or port under the cursor (or the shown module),
// double-clicking an instance opens its module here, middle-clicking it (or
// its context menu) in a new canvas; Backspace goes up, F fits.
void SchematicCanvas::handle_events(Context& ctx, const CanvasEvents& events, Requests& requests) {
    if (events.fit_requested)
        fit_view(ctx);
    const Design* design = ctx.panels.design();
    if (!design || !place_.module.valid())
        return;

    if (events.up_requested && !place_.path.empty()) {
        std::vector<CellId> path = place_.path;
        CellId instance = path.back();
        path.pop_back();
        Place up = Place::place(path, ctx.panels.module_at(path));
        navigate(up);
        up.kind = Place::Kind::Cell; // select where we came from
        up.cell = instance;
        ctx.panels.select(up);
        return;
    }

    // The place inside an instance node, if one is under the cursor.
    std::optional<Place> inside;
    const DisplayNode* node =
        hovered_node_ >= 0 && shown_ ? &shown_->graph.nodes()[hovered_node_] : nullptr;
    if (node && node->kind == NodeKind::Instance) {
        const Module& module = design->module(place_.module);
        ModuleId child = *design->find_module(module.cell(node->cell).type);
        std::vector<CellId> path = place_.path;
        if (!is_definition(*design)) // inside a module definition there is no instance path
            path.push_back(node->cell);
        inside = Place::place(std::move(path), child);
    }

    if (events.middle_clicked && inside)
        requests.open_in_new_canvas = inside;
    std::string menu = std::format("instance_menu{}", id_);
    if (events.right_clicked && inside) {
        menu_place_ = *inside; // the cursor may leave the instance while the menu is open
        ImGui::OpenPopup(menu.c_str());
    }
    if (ImGui::BeginPopup(menu.c_str())) {
        if (ImGui::MenuItem("Open")) {
            navigate(menu_place_);
            ctx.panels.select(menu_place_);
        }
        if (ImGui::MenuItem("Open in New Canvas", "Middle-click"))
            requests.open_in_new_canvas = menu_place_;
        ImGui::EndPopup();
    }

    if (!events.clicked)
        return;
    if (events.double_clicked && inside) {
        navigate(*inside);
        ctx.panels.select(*inside);
        return;
    }
    Place picked = Place::place(place_.path, place_.module);
    if (!node && hovered_net_ >= 0 && shown_) {
        picked.kind = Place::Kind::Net;
        picked.bits = net_bits(shown_->graph, shown_->graph.nets()[hovered_net_]);
    } else if (node && (node->kind == NodeKind::Cell || node->kind == NodeKind::Instance)) {
        picked.kind = Place::Kind::Cell;
        picked.cell = node->cell;
    } else if (node) {
        picked.kind = Place::Kind::Port;
        picked.port = node->port;
    }
    ctx.panels.select(picked);
}

} // namespace netviz
