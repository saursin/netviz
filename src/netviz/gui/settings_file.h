#pragma once

// The preferences file (settings.json): what the user configures, kept apart
// from window layout and session state (layout.ini, written by ImGui).
// No ImGui or SDL dependency, so it can be tested on its own.

#include "netviz/gui/settings.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace netviz {

// Reads preferences from `path` into `settings`. Missing keys keep their
// current values, unknown keys are ignored and out-of-range values are
// clamped. Returns an error message (and leaves `settings` unchanged) if the
// file cannot be read or parsed.
std::optional<std::string> read_settings_file(const std::filesystem::path& path,
                                              Settings& settings);
// Same, from JSON text.
std::optional<std::string> parse_settings(std::string_view json, Settings& settings);

std::string settings_to_json(const Settings& settings);
// Writes atomically (a temporary file, then a rename). Returns an error
// message on failure.
std::optional<std::string> write_settings_file(const std::filesystem::path& path,
                                               const Settings& settings);

// Whether two Settings differ in what the preferences file stores.
bool same_file_settings(const Settings& a, const Settings& b);

// Older versions kept preferences as a "[Netviz][Settings]" section of
// Key=Value lines in imgui.ini. Applies those to `settings`, and appends the
// recent files listed there to `recent`. Returns false if the file has no
// such section.
bool read_legacy_ini(const std::filesystem::path& path, Settings& settings,
                     std::vector<std::string>& recent);

} // namespace netviz
