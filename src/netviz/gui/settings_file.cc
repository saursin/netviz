#include "netviz/gui/settings_file.h"

#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

namespace netviz {

namespace {

using simdjson::dom::element;
using simdjson::dom::object;

constexpr int kVersion = 1;

// Limits shared with the Preferences window.
constexpr float kMinUiScale = 0.5f, kMaxUiScale = 3.0f;
constexpr float kMinZoomLow = 1e-4f, kMinZoomHigh = 1.0f; // min_zoom range
constexpr float kMaxZoomLow = 1.0f, kMaxZoomHigh = 1000.0f;
constexpr int64_t kMinNodes = 1000, kMaxNodes = 10000000;

// Optional typed reads: absent or mistyped keys leave the target unchanged.
void read(object o, const char* key, float& out, float lo, float hi) {
    double v;
    if (o[key].get_double().get(v) == simdjson::SUCCESS && std::isfinite(v))
        out = std::clamp(static_cast<float>(v), lo, hi);
}

void read(object o, const char* key, bool& out) {
    bool v;
    if (o[key].get_bool().get(v) == simdjson::SUCCESS)
        out = v;
}

void read(object o, const char* key, uint32_t& out, int64_t lo, int64_t hi) {
    int64_t v;
    if (o[key].get_int64().get(v) == simdjson::SUCCESS)
        out = static_cast<uint32_t>(std::clamp(v, lo, hi));
}

bool section(object o, const char* key, object& out) {
    return o[key].get_object().get(out) == simdjson::SUCCESS;
}

void read(object o, const char* key, std::string& out) {
    std::string_view v;
    if (o[key].get_string().get(v) == simdjson::SUCCESS)
        out = v;
}

void read(object o, const char* key, std::vector<std::string>& out) {
    simdjson::dom::array a;
    if (o[key].get_array().get(a) != simdjson::SUCCESS)
        return;
    out.clear();
    for (element e : a) {
        std::string_view v;
        if (e.get_string().get(v) == simdjson::SUCCESS)
            out.emplace_back(v);
    }
}

std::vector<AttributePattern> read_attributes(object o) {
    std::vector<AttributePattern> out;
    simdjson::dom::array a;
    if (o["attributes"].get_array().get(a) != simdjson::SUCCESS)
        return out;
    for (element e : a) {
        object ao;
        if (e.get_object().get(ao) != simdjson::SUCCESS)
            continue;
        AttributePattern p;
        read(ao, "name", p.name);
        read(ao, "value", p.value);
        out.push_back(p);
    }
    return out;
}

// "#rrggbb"
void read_color(object o, const char* key, RuleColor& out) {
    std::string_view v;
    if (o[key].get_string().get(v) != simdjson::SUCCESS || v.size() != 7 || v[0] != '#')
        return;
    char* end = nullptr;
    std::string hex(v.substr(1));
    unsigned long value = std::strtoul(hex.c_str(), &end, 16);
    if (end == hex.c_str() + hex.size())
        out = static_cast<RuleColor>(value);
}

// An enum stored as one of `names` (index = value).
template <typename E, size_t N>
void read_enum(object o, const char* key, E& out, const char* const (&names)[N]) {
    std::string_view v;
    if (o[key].get_string().get(v) != simdjson::SUCCESS)
        return;
    for (size_t i = 0; i < N; i++)
        if (v == names[i])
            out = static_cast<E>(i);
}

const char* const kLineStyles[] = {"solid", "dashed"};
const char* const kMarkers[] = {"none", "clock"};
const char* const kPlacements[] = {"routed", "label"};
const char* const kBorders[] = {"solid", "dashed", "dotted", "none"};

std::vector<SignalRule> read_signal_rules(simdjson::dom::array a) {
    std::vector<SignalRule> rules;
    for (element e : a) {
        object o;
        if (e.get_object().get(o) != simdjson::SUCCESS)
            continue;
        SignalRule r;
        read(o, "name", r.name);
        read(o, "enabled", r.enabled);
        read(o, "nets", r.nets);
        r.attributes = read_attributes(o);
        simdjson::dom::array pins;
        if (o["cell_pins"].get_array().get(pins) == simdjson::SUCCESS)
            for (element p : pins) {
                object po;
                if (p.get_object().get(po) != simdjson::SUCCESS)
                    continue;
                CellPinPattern cp;
                read(po, "cell", cp.cell);
                read(po, "pin", cp.pin);
                r.cell_pins.push_back(cp);
            }
        read_color(o, "color", r.color);
        read(o, "width", r.width, 0.5f, 8.0f);
        read_enum(o, "line", r.line, kLineStyles);
        read_enum(o, "marker", r.marker, kMarkers);
        read_enum(o, "placement", r.placement, kPlacements);
        rules.push_back(std::move(r));
    }
    return rules;
}

std::vector<CellRule> read_cell_rules(simdjson::dom::array a) {
    std::vector<CellRule> rules;
    for (element e : a) {
        object o;
        if (e.get_object().get(o) != simdjson::SUCCESS)
            continue;
        CellRule r;
        read(o, "name", r.name);
        read(o, "enabled", r.enabled);
        read(o, "types", r.types);
        read(o, "names", r.names);
        r.attributes = read_attributes(o);
        read_color(o, "color", r.color);
        read_enum(o, "border", r.border, kBorders);
        read(o, "border_width", r.border_width, 0.5f, 8.0f);
        read(o, "show_type", r.show_type);
        rules.push_back(std::move(r));
    }
    return rules;
}

// A JSON string literal.
std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
            else
                out += c;
        }
    }
    return out + "\"";
}

std::string quote_list(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); i++)
        out += (i ? ", " : "") + quote(items[i]);
    return out + "]";
}

std::string attributes_text(const std::vector<AttributePattern>& attributes) {
    std::string out = "[";
    for (size_t i = 0; i < attributes.size(); i++)
        out += std::format("{}{{ \"name\": {}, \"value\": {} }}", i ? ", " : "",
                           quote(attributes[i].name), quote(attributes[i].value));
    return out + "]";
}

std::string color_text(RuleColor c) {
    return std::format("\"#{:06x}\"", c & 0xFFFFFF);
}

// Shortest text that reads back as the same float.
std::string number(float v) {
    return std::format("{}", v);
}

} // namespace

std::optional<std::string> parse_settings(std::string_view json, Settings& settings) {
    simdjson::dom::parser parser;
    simdjson::padded_string text(json);
    element doc;
    if (auto error = parser.parse(text).get(doc)) {
        std::string message = simdjson::error_message(error);
        while (!message.empty() && (message.back() == '.' || message.back() == ' '))
            message.pop_back();
        return message;
    }
    object root;
    if (doc.get_object().get(root) != simdjson::SUCCESS)
        return std::string("the top level is not an object");

    Settings s = settings;
    object o;
    if (section(root, "appearance", o)) {
        std::string_view theme;
        if (o["theme"].get_string().get(theme) == simdjson::SUCCESS)
            s.theme = theme == "light" ? Theme::Light : Theme::Dark;
        read(o, "ui_scale", s.ui_scale, kMinUiScale, kMaxUiScale);
    }
    if (section(root, "canvas", o)) {
        read(o, "min_zoom", s.min_zoom, kMinZoomLow, kMinZoomHigh);
        read(o, "max_zoom", s.max_zoom, kMaxZoomLow, kMaxZoomHigh);
        read(o, "show_grid", s.show_grid);
        read(o, "max_schematic_nodes", s.max_schematic_nodes, kMinNodes, kMaxNodes);
    }
    if (section(root, "console", o))
        read(o, "wrap", s.console_wrap);
    // Rule lists replace the defaults when present (an empty list means none).
    if (section(root, "rules", o)) {
        simdjson::dom::array a;
        if (o["signals"].get_array().get(a) == simdjson::SUCCESS)
            s.signal_rules = read_signal_rules(a);
        if (o["cells"].get_array().get(a) == simdjson::SUCCESS)
            s.cell_rules = read_cell_rules(a);
    }
    read(root, "show_perf_stats", s.show_perf_stats);
    settings = s;
    return std::nullopt;
}

std::optional<std::string> read_settings_file(const std::filesystem::path& path,
                                              Settings& settings) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::string("cannot be opened");
    std::ostringstream text;
    text << in.rdbuf();
    return parse_settings(text.str(), settings);
}

std::string settings_to_json(const Settings& s) {
    std::string out;
    out += "{\n";
    out += std::format("  \"version\": {},\n", kVersion);
    out += "  \"appearance\": {\n";
    out += std::format("    \"theme\": \"{}\",\n", s.theme == Theme::Light ? "light" : "dark");
    out += std::format("    \"ui_scale\": {}\n", number(s.ui_scale));
    out += "  },\n";
    out += "  \"canvas\": {\n";
    out += std::format("    \"min_zoom\": {},\n", number(s.min_zoom));
    out += std::format("    \"max_zoom\": {},\n", number(s.max_zoom));
    out += std::format("    \"show_grid\": {},\n", s.show_grid);
    out += std::format("    \"max_schematic_nodes\": {}\n", s.max_schematic_nodes);
    out += "  },\n";
    out += "  \"console\": {\n";
    out += std::format("    \"wrap\": {}\n", s.console_wrap);
    out += "  },\n";
    out += "  \"rules\": {\n";
    out += "    \"signals\": [";
    for (size_t i = 0; i < s.signal_rules.size(); i++) {
        const SignalRule& r = s.signal_rules[i];
        std::string pins = "[";
        for (size_t k = 0; k < r.cell_pins.size(); k++)
            pins += std::format("{}{{ \"cell\": {}, \"pin\": {} }}", k ? ", " : "",
                                quote(r.cell_pins[k].cell), quote(r.cell_pins[k].pin));
        pins += "]";
        out +=
            std::format("{}\n      {{\n"
                        "        \"name\": {},\n"
                        "        \"enabled\": {},\n"
                        "        \"nets\": {},\n"
                        "        \"attributes\": {},\n"
                        "        \"cell_pins\": {},\n"
                        "        \"color\": {},\n"
                        "        \"width\": {},\n"
                        "        \"line\": \"{}\",\n"
                        "        \"marker\": \"{}\",\n"
                        "        \"placement\": \"{}\"\n"
                        "      }}",
                        i ? "," : "", quote(r.name), r.enabled, quote_list(r.nets),
                        attributes_text(r.attributes), pins, color_text(r.color), number(r.width),
                        kLineStyles[static_cast<int>(r.line)], kMarkers[static_cast<int>(r.marker)],
                        kPlacements[static_cast<int>(r.placement)]);
    }
    out += s.signal_rules.empty() ? "],\n" : "\n    ],\n";
    out += "    \"cells\": [";
    for (size_t i = 0; i < s.cell_rules.size(); i++) {
        const CellRule& r = s.cell_rules[i];
        out +=
            std::format("{}\n      {{\n"
                        "        \"name\": {},\n"
                        "        \"enabled\": {},\n"
                        "        \"types\": {},\n"
                        "        \"names\": {},\n"
                        "        \"attributes\": {},\n"
                        "        \"color\": {},\n"
                        "        \"border\": \"{}\",\n"
                        "        \"border_width\": {},\n"
                        "        \"show_type\": {}\n"
                        "      }}",
                        i ? "," : "", quote(r.name), r.enabled, quote_list(r.types),
                        quote_list(r.names), attributes_text(r.attributes), color_text(r.color),
                        kBorders[static_cast<int>(r.border)], number(r.border_width), r.show_type);
    }
    out += s.cell_rules.empty() ? "]\n" : "\n    ]\n";
    out += "  },\n";
    out += std::format("  \"show_perf_stats\": {}\n", s.show_perf_stats);
    out += "}\n";
    return out;
}

std::optional<std::string> write_settings_file(const std::filesystem::path& path,
                                               const Settings& settings) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << settings_to_json(settings);
        if (!out)
            return std::string("cannot be written");
    }
    std::filesystem::rename(temp, path, ec);
    if (ec)
        return ec.message();
    return std::nullopt;
}

bool same_file_settings(const Settings& a, const Settings& b) {
    return a.theme == b.theme && a.ui_scale == b.ui_scale && a.min_zoom == b.min_zoom &&
           a.max_zoom == b.max_zoom && a.max_schematic_nodes == b.max_schematic_nodes &&
           a.show_grid == b.show_grid && a.show_perf_stats == b.show_perf_stats &&
           a.console_wrap == b.console_wrap && a.signal_rules == b.signal_rules &&
           a.cell_rules == b.cell_rules;
}

bool read_legacy_ini(const std::filesystem::path& path, Settings& s,
                     std::vector<std::string>& recent) {
    std::ifstream in(path);
    std::string line;
    bool inside = false, found = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.starts_with('[')) {
            inside = line == "[Netviz][Settings]";
            found |= inside;
            continue;
        }
        size_t eq = line.find('=');
        if (!inside || eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        auto real = [&](float lo, float hi) {
            return std::clamp(std::strtof(value.c_str(), nullptr), lo, hi);
        };
        bool on = value == "1";
        if (key == "Theme")
            s.theme = value == "Light" ? Theme::Light : Theme::Dark;
        else if (key == "UiScale")
            s.ui_scale = real(kMinUiScale, kMaxUiScale);
        else if (key == "MinZoom")
            s.min_zoom = real(kMinZoomLow, kMinZoomHigh);
        else if (key == "MaxZoom")
            s.max_zoom = real(kMaxZoomLow, kMaxZoomHigh);
        else if (key == "MaxSchematicNodes")
            s.max_schematic_nodes = static_cast<uint32_t>(std::clamp<int64_t>(
                std::strtoll(value.c_str(), nullptr, 10), kMinNodes, kMaxNodes));
        else if (key == "ShowGrid")
            s.show_grid = on;
        else if (key == "ShowPerfStats")
            s.show_perf_stats = on;
        else if (key == "ShowHierarchy")
            s.show_hierarchy = on;
        else if (key == "ShowModules")
            s.show_modules = on;
        else if (key == "ShowProperties")
            s.show_properties = on;
        else if (key == "ShowConsole")
            s.show_console = on;
        else if (key == "Recent")
            recent.push_back(value);
    }
    return found;
}

} // namespace netviz
