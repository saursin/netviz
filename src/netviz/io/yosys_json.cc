#include "netviz/io/yosys_json.h"

#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <format>

namespace netviz {

namespace {

using simdjson::dom::array;
using simdjson::dom::element;
using simdjson::dom::object;

// Thrown inside the importer for any problem that makes the input unusable;
// caught at the top and turned into ImportResult::error.
struct ImportError {
    std::string message;
};

// Thrown when ImportProgress::cancel is set.
struct ImportCancelled {};

constexpr int64_t kMaxBitNumber = 1 << 30; // sanity limit for Yosys bit numbers

// Progress is published every this many items; cheap enough not to matter.
constexpr uint64_t kProgressStep = 1024;

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

class Importer {
  public:
    explicit Importer(ImportProgress* progress) : progress_(progress) {}
    // `parse_seconds` is how long the caller spent parsing the file.
    ImportResult run(element root, double parse_seconds);

  private:
    [[noreturn]] void fail(std::string_view what) const;
    void warn(std::string message);
    void count_items(object modules);
    void advance(); // one port, netname or cell done

    // JSON helpers; `what` names the field in error messages.
    object as_object(element e, std::string_view what) const;
    array as_array(element e, std::string_view what) const;
    std::string_view as_string(element e, std::string_view what) const;
    int64_t as_int(element e, std::string_view what) const;
    static std::optional<element> field(object o, std::string_view key);
    int64_t int_field(object o, std::string_view key, int64_t fallback) const;

    Value decode_value(element e) const;
    void read_values(object o, std::string_view key, std::vector<NamedValue>& out);
    void read_bits(element e, std::string_view what, std::vector<SigBit>& out);
    Direction read_direction(element e, std::string_view what) const;
    WireOptions read_wire_options(object o) const;

    void import_module(std::string_view name, object json);
    void import_ports(object ports);
    void import_netnames(object netnames);
    void import_cells(object cells);
    void find_top();

    ImportProgress* progress_;
    uint64_t total_items_ = 0;
    uint64_t done_items_ = 0;

    std::unique_ptr<Design> design_;
    std::vector<Issue> issues_;
    std::string context_; // "module m: cell c" prefix for messages

    // Per module: Yosys bit number -> our signal bit (kInvalid = not seen yet).
    Module* module_ = nullptr;
    std::vector<uint32_t> bit_map_;
};

[[noreturn]] void Importer::fail(std::string_view what) const {
    if (context_.empty())
        throw ImportError{std::string(what)};
    throw ImportError{std::format("{}: {}", context_, what)};
}

void Importer::warn(std::string message) {
    issues_.push_back({Issue::Severity::Warning, std::move(message)});
}

// Total number of ports, netnames and cells, for progress. Object sizes are
// stored in the parsed document, so this does not walk the objects. (simdjson
// caps a stored size at 16.7M; such huge objects just make `total` an estimate.)
void Importer::count_items(object modules) {
    for (auto [name, value] : modules) {
        object module;
        if (value.get_object().get(module) != simdjson::SUCCESS)
            continue;
        for (const char* key : {"ports", "netnames", "cells"}) {
            object o;
            if (std::optional<element> e = field(module, key);
                e && e->get_object().get(o) == simdjson::SUCCESS)
                total_items_ += o.size();
        }
    }
}

void Importer::advance() {
    done_items_++;
    if (progress_ && done_items_ % kProgressStep == 0) {
        progress_->done.store(done_items_, std::memory_order_relaxed);
        if (progress_->cancel.load(std::memory_order_relaxed))
            throw ImportCancelled{};
    }
}

object Importer::as_object(element e, std::string_view what) const {
    object o;
    if (e.get_object().get(o) != simdjson::SUCCESS)
        fail(std::format("'{}' must be an object", what));
    return o;
}

array Importer::as_array(element e, std::string_view what) const {
    array a;
    if (e.get_array().get(a) != simdjson::SUCCESS)
        fail(std::format("'{}' must be an array", what));
    return a;
}

std::string_view Importer::as_string(element e, std::string_view what) const {
    std::string_view s;
    if (e.get_string().get(s) != simdjson::SUCCESS)
        fail(std::format("'{}' must be a string", what));
    return s;
}

int64_t Importer::as_int(element e, std::string_view what) const {
    bool b = false;
    if (e.get_bool().get(b) == simdjson::SUCCESS)
        return b ? 1 : 0;
    int64_t i = 0;
    if (e.get_int64().get(i) != simdjson::SUCCESS)
        fail(std::format("'{}' must be an integer", what));
    return i;
}

std::optional<element> Importer::field(object o, std::string_view key) {
    element e;
    if (o.at_key(key).get(e) != simdjson::SUCCESS)
        return std::nullopt;
    return e;
}

int64_t Importer::int_field(object o, std::string_view key, int64_t fallback) const {
    std::optional<element> e = field(o, key);
    return e ? as_int(*e, key) : fallback;
}

// Same rules as Yosys's own JSON frontend: a string of only 0/1/x/z is a bit
// vector; such a string followed by spaces is a string with one trailing space
// removed (Yosys adds it to mark strings that look like bits); numbers are
// 32-bit constants (64-bit if they do not fit).
Value Importer::decode_value(element e) const {
    std::string_view s;
    if (e.get_string().get(s) == simdjson::SUCCESS) {
        size_t cursor = s.find_first_not_of("01xz");
        if (cursor == std::string_view::npos)
            return {Value::Kind::Bits, std::string(s)};
        if (s.find_first_not_of(' ', cursor) == std::string_view::npos)
            return {Value::Kind::String, std::string(s.substr(0, s.size() - 1))};
        return {Value::Kind::String, std::string(s)};
    }

    int64_t i = 0;
    uint64_t u = 0;
    if (e.get_int64().get(i) == simdjson::SUCCESS) {
        int width = (i >= INT32_MIN && i <= INT32_MAX) ? 32 : 64;
        std::string bits(width, '0');
        uint64_t v = static_cast<uint64_t>(i);
        for (int b = 0; b < width; b++)
            bits[width - 1 - b] = ((v >> b) & 1) ? '1' : '0';
        return {Value::Kind::Bits, bits};
    }
    if (e.get_uint64().get(u) == simdjson::SUCCESS) {
        std::string bits(64, '0');
        for (int b = 0; b < 64; b++)
            bits[63 - b] = ((u >> b) & 1) ? '1' : '0';
        return {Value::Kind::Bits, bits};
    }
    fail("attribute or parameter values must be strings or integers");
}

void Importer::read_values(object o, std::string_view key, std::vector<NamedValue>& out) {
    std::optional<element> e = field(o, key);
    if (!e)
        return;
    for (auto [name, value] : as_object(*e, key))
        out.push_back({design_->names().intern(name), decode_value(value)});
}

void Importer::read_bits(element e, std::string_view what, std::vector<SigBit>& out) {
    for (element bit : as_array(e, what)) {
        std::string_view s;
        if (bit.get_string().get(s) == simdjson::SUCCESS) {
            if (s == "0")
                out.push_back(SigBit::constant(BitState::Zero));
            else if (s == "1")
                out.push_back(SigBit::constant(BitState::One));
            else if (s == "x")
                out.push_back(SigBit::constant(BitState::X));
            else if (s == "z")
                out.push_back(SigBit::constant(BitState::Z));
            else
                fail(std::format("'{}' has an invalid bit \"{}\"", what, s));
            continue;
        }
        int64_t number = as_int(bit, what);
        if (number < 0 || number > kMaxBitNumber)
            fail(std::format("'{}' has an out-of-range bit number {}", what, number));
        size_t index = static_cast<size_t>(number);
        if (index >= bit_map_.size())
            bit_map_.resize(index + 1, SignalBitId::kInvalid);
        if (bit_map_[index] == SignalBitId::kInvalid)
            bit_map_[index] = module_->add_signals(1).index;
        out.push_back(SigBit::signal(SignalBitId(bit_map_[index])));
    }
}

Direction Importer::read_direction(element e, std::string_view what) const {
    std::string_view s = as_string(e, what);
    if (s == "input")
        return Direction::Input;
    if (s == "output")
        return Direction::Output;
    if (s == "inout")
        return Direction::InOut;
    fail(std::format("'{}' has an invalid direction \"{}\"", what, s));
}

WireOptions Importer::read_wire_options(object o) const {
    WireOptions options;
    options.start_offset = static_cast<int32_t>(int_field(o, "offset", 0));
    options.upto = int_field(o, "upto", 0) != 0;
    options.is_signed = int_field(o, "signed", 0) != 0;
    return options;
}

bool attribute_set(const Design& design, const Module& module, const char* attr) {
    for (const NamedValue& nv : module.attributes()) {
        if (design.names().str(nv.name) == attr) {
            std::optional<uint64_t> v = netviz::as_int(nv.value);
            return v && *v != 0;
        }
    }
    return false;
}

bool is_box(const Design& design, const Module& module) {
    return module.is_blackbox() || attribute_set(design, module, "whitebox");
}

// -- Modules ----------------------------------------------------------------

void Importer::import_module(std::string_view name, object json) {
    context_ = std::format("module {}", name);
    std::optional<ModuleId> id = design_->add_module(design_->names().intern(name));
    if (!id)
        fail("duplicate module name");
    module_ = &design_->module(*id);
    bit_map_.clear();

    read_values(json, "attributes", module_->attributes());
    module_->set_blackbox(attribute_set(*design_, *module_, "blackbox"));
    read_values(json, "parameter_default_values", module_->parameters());

    // Ports first: their order in the file is the port order.
    if (std::optional<element> ports = field(json, "ports"))
        import_ports(as_object(*ports, "ports"));
    if (std::optional<element> netnames = field(json, "netnames"))
        import_netnames(as_object(*netnames, "netnames"));
    if (std::optional<element> cells = field(json, "cells"))
        import_cells(as_object(*cells, "cells"));

    if (std::optional<element> memories = field(json, "memories")) {
        size_t count = as_object(*memories, "memories").size();
        if (count > 0)
            warn(std::format("module {}: {} memories are not imported (not supported yet)", name,
                             count));
    }
    context_.clear();
}

void Importer::import_ports(object ports) {
    std::vector<SigBit> bits;
    std::string base = context_;
    for (auto [name, value] : ports) {
        context_ = std::format("{}: port {}", base, name);
        object port = as_object(value, "port");
        std::optional<element> bits_json = field(port, "bits");
        std::optional<element> direction = field(port, "direction");
        if (!bits_json || !direction)
            fail("missing 'bits' or 'direction'");

        bits.clear();
        read_bits(*bits_json, "bits", bits);
        std::optional<WireId> wire =
            module_->add_wire(design_->names().intern(name), bits, read_wire_options(port));
        if (!wire)
            fail("duplicate port name");
        module_->make_port(*wire, read_direction(*direction, "direction"));
        advance();
    }
    context_ = base;
}

void Importer::import_netnames(object netnames) {
    std::vector<SigBit> bits;
    std::string base = context_;
    for (auto [name, value] : netnames) {
        context_ = std::format("{}: netname {}", base, name);
        object net = as_object(value, "netname");
        std::optional<element> bits_json = field(net, "bits");
        if (!bits_json)
            fail("missing 'bits'");

        bits.clear();
        read_bits(*bits_json, "bits", bits);
        NameId name_id = design_->names().intern(name);

        // A port also appears as a netname with the same name: same wire.
        if (std::optional<WireId> existing = module_->find_wire(name_id)) {
            std::span<const SigBit> port_bits = module_->bits(module_->wire(*existing));
            if (!std::equal(port_bits.begin(), port_bits.end(), bits.begin(), bits.end()))
                fail("bits differ from the port with the same name");
            read_values(net, "attributes", module_->wire_attributes(*existing));
            advance();
            continue;
        }

        WireOptions options = read_wire_options(net);
        options.hidden = int_field(net, "hide_name", 0) != 0;
        WireId wire = *module_->add_wire(name_id, bits, options);
        read_values(net, "attributes", module_->wire_attributes(wire));
        advance();
    }
    context_ = base;
}

void Importer::import_cells(object cells) {
    std::vector<SigBit> bits;         // bits of all pins of the current cell
    std::vector<uint32_t> pin_starts; // where each pin's bits start in `bits`
    std::vector<NewPin> pins;
    std::string base = context_;

    for (auto [name, value] : cells) {
        context_ = std::format("{}: cell {}", base, name);
        object cell = as_object(value, "cell");
        std::optional<element> type = field(cell, "type");
        if (!type)
            fail("missing 'type'");

        std::optional<object> directions;
        if (std::optional<element> d = field(cell, "port_directions"))
            directions = as_object(*d, "port_directions");

        bits.clear();
        pin_starts.clear();
        pins.clear();
        if (std::optional<element> connections = field(cell, "connections")) {
            for (auto [port, port_bits] : as_object(*connections, "connections")) {
                NewPin pin;
                pin.port = design_->names().intern(port);
                if (directions) {
                    if (std::optional<element> d = field(*directions, port))
                        pin.direction = read_direction(*d, "port_directions");
                }
                pin_starts.push_back(static_cast<uint32_t>(bits.size()));
                read_bits(port_bits, "connections", bits);
                pins.push_back(pin);
            }
        }
        // `bits` is complete now, so the spans stay valid.
        for (size_t i = 0; i < pins.size(); i++) {
            uint32_t end =
                (i + 1 < pins.size()) ? pin_starts[i + 1] : static_cast<uint32_t>(bits.size());
            pins[i].bits =
                std::span<const SigBit>(bits).subspan(pin_starts[i], end - pin_starts[i]);
        }

        bool hidden = int_field(cell, "hide_name", 0) != 0;
        std::optional<CellId> id =
            module_->add_cell(design_->names().intern(name),
                              design_->names().intern(as_string(*type, "type")), pins, hidden);
        if (!id)
            fail("duplicate cell name");
        read_values(cell, "parameters", module_->cell_parameters(*id));
        read_values(cell, "attributes", module_->cell_attributes(*id));
        advance();
    }
    context_ = base;
}

// -- Top module -------------------------------------------------------------

// Same rules as nextpnr: a non-box module with the `top` attribute; otherwise
// the only non-box module that no other module instantiates.
void Importer::find_top() {
    std::span<const Module> modules = design_->modules();
    std::vector<uint32_t> by_attribute;
    for (uint32_t m = 0; m < modules.size(); m++) {
        if (!is_box(*design_, modules[m]) && attribute_set(*design_, modules[m], "top"))
            by_attribute.push_back(m);
    }
    if (by_attribute.size() == 1) {
        design_->set_top(ModuleId(by_attribute[0]));
        return;
    }
    if (by_attribute.size() > 1) {
        warn("several modules have the 'top' attribute; no top module selected");
        return;
    }

    std::vector<bool> instantiated(modules.size(), false);
    for (const Module& module : modules) {
        for (const Cell& cell : module.cells()) {
            if (std::optional<ModuleId> type = design_->find_module(cell.type))
                instantiated[type->index] = true;
        }
    }
    std::vector<uint32_t> candidates;
    for (uint32_t m = 0; m < modules.size(); m++) {
        if (!instantiated[m] && !is_box(*design_, modules[m]))
            candidates.push_back(m);
    }
    if (candidates.size() == 1) {
        design_->set_top(ModuleId(candidates[0]));
    } else if (candidates.size() > 1) {
        std::string names;
        for (size_t i = 0; i < candidates.size() && i < 5; i++)
            names += (i ? ", " : "") + design_->names().str(modules[candidates[i]].name());
        warn(std::format("{} candidate top modules ({}{}); no top module selected",
                         candidates.size(), names, candidates.size() > 5 ? ", ..." : ""));
    }
}

ImportResult Importer::run(element root, double parse_seconds) {
    ImportTimings timings;
    timings.parse = parse_seconds;
    Clock::time_point build_start = Clock::now();
    design_ = std::make_unique<Design>();
    object top = as_object(root, "top level");
    std::optional<element> modules = field(top, "modules");
    if (!modules)
        fail("missing 'modules'");
    object module_list = as_object(*modules, "modules");
    if (progress_ && progress_->cancel.load(std::memory_order_relaxed))
        throw ImportCancelled{};
    if (progress_) {
        count_items(module_list);
        progress_->total.store(total_items_, std::memory_order_relaxed);
        progress_->stage.store(ImportStage::Building, std::memory_order_relaxed);
    }
    for (auto [name, module] : module_list)
        import_module(name, as_object(module, "module"));

    timings.build = seconds_since(build_start);
    Clock::time_point check_start = Clock::now();
    if (progress_) {
        if (progress_->cancel.load(std::memory_order_relaxed))
            throw ImportCancelled{};
        progress_->done.store(done_items_, std::memory_order_relaxed);
        progress_->stage.store(ImportStage::Checking, std::memory_order_relaxed);
    }
    find_top();
    design_->resolve_pin_directions(); // pins still unknown are reported by validate()
    for (Issue& issue : validate(*design_))
        issues_.push_back(std::move(issue));

    timings.check = seconds_since(check_start);

    ImportResult result;
    result.design = std::move(design_);
    result.issues = std::move(issues_);
    result.timings = timings;
    return result;
}

ImportResult failed(std::string error) {
    ImportResult result;
    result.error = std::move(error);
    return result;
}

ImportResult import_from(simdjson::simdjson_result<element> parsed, ImportProgress* progress,
                         double parse_seconds) {
    element root;
    if (simdjson::error_code err = parsed.get(root); err != simdjson::SUCCESS)
        return failed(std::format("invalid JSON: {}", simdjson::error_message(err)));
    try {
        return Importer(progress).run(root, parse_seconds);
    } catch (const ImportCancelled&) {
        ImportResult result;
        result.error = "cancelled";
        result.cancelled = true;
        return result;
    } catch (const ImportError& e) {
        return failed(e.message);
    } catch (const std::exception& e) {
        return failed(std::format("import failed: {}", e.what()));
    }
}

} // namespace

ImportResult import_yosys_json(const std::filesystem::path& path, ImportProgress* progress) {
    simdjson::dom::parser parser;
    Clock::time_point start = Clock::now();
    simdjson::simdjson_result<element> parsed = parser.load(path.string());
    return import_from(parsed, progress, seconds_since(start));
}

ImportResult import_yosys_json_text(std::string_view json, ImportProgress* progress) {
    simdjson::dom::parser parser;
    Clock::time_point start = Clock::now();
    simdjson::padded_string padded(json);
    simdjson::simdjson_result<element> parsed = parser.parse(padded);
    return import_from(parsed, progress, seconds_since(start));
}

} // namespace netviz
