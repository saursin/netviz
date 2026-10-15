#pragma once

// Importer for netlists written by Yosys `write_json`. See docs/IR.md
// ("Importers") for how the format maps onto the IR.

#include "netviz/core/connectivity.h"
#include "netviz/core/netlist.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace netviz {

// Seconds spent in each stage of an import.
struct ImportTimings {
    double parse = 0.0; // reading and parsing the file
    double build = 0.0; // building the netlist
    double check = 0.0; // top module, pin directions, validation
};

struct ImportResult {
    std::unique_ptr<Design> design; // null if the import failed
    std::string error;              // why it failed, with context ("module m: cell c: ...")
    std::vector<Issue> issues;      // warnings from the importer and from validate()
    bool cancelled = false;         // stopped because ImportProgress::cancel was set
    ImportTimings timings;
};

// Progress of an import, written by the importing thread and safe to read from
// any other thread while it runs.
enum class ImportStage : uint8_t {
    Reading,  // reading and parsing the file (no counts yet)
    Building, // building the netlist; `done` goes from 0 to `total`
    Checking, // choosing the top module and validating
};
struct ImportProgress {
    std::atomic<ImportStage> stage{ImportStage::Reading};
    std::atomic<uint64_t> done{0};  // ports, netnames and cells imported so far
    std::atomic<uint64_t> total{0}; // ports, netnames and cells in the file
    // Set from any thread to stop the import early. It is checked while the
    // netlist is built, not during the initial parse of the file.
    std::atomic<bool> cancel{false};
};

// Reads a Yosys JSON netlist. Never throws; problems are reported in the result.
// If `progress` is given, it is updated as the import runs.
ImportResult import_yosys_json(const std::filesystem::path& path,
                               ImportProgress* progress = nullptr);

// Same, from JSON text in memory.
ImportResult import_yosys_json_text(std::string_view json, ImportProgress* progress = nullptr);

} // namespace netviz
