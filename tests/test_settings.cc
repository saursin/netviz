#include "netviz/gui/settings_file.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using namespace netviz;

namespace {

Settings changed() {
    Settings s;
    s.theme = Theme::Light;
    s.ui_scale = 1.25f;
    s.min_zoom = 0.05f;
    s.max_zoom = 64.0f;
    s.show_grid = false;
    s.max_schematic_nodes = 123456;
    s.show_perf_stats = true;
    return s;
}

std::filesystem::path temp_dir() {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "netviz_test_settings";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("settings: a written file reads back the same") {
    Settings original = changed();
    Settings read;
    CHECK_FALSE(parse_settings(settings_to_json(original), read));
    CHECK(same_file_settings(read, original));

    std::filesystem::path path = temp_dir() / "sub" / "settings.json";
    CHECK_FALSE(write_settings_file(path, original)); // creates the directory
    Settings from_file;
    CHECK_FALSE(read_settings_file(path, from_file));
    CHECK(same_file_settings(from_file, original));
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}

TEST_CASE("settings: missing keys keep defaults, unknown keys are ignored, values are clamped") {
    Settings s;
    CHECK_FALSE(parse_settings(R"({
        "appearance": { "theme": "light", "future_option": [1, 2] },
        "canvas": { "max_zoom": 1e9, "max_schematic_nodes": 5, "show_grid": "yes" },
        "something_new": {}
    })",
                               s));
    CHECK(s.theme == Theme::Light);
    CHECK(s.ui_scale == Settings{}.ui_scale);   // missing: unchanged
    CHECK(s.max_zoom == 1000.0f);               // clamped
    CHECK(s.max_schematic_nodes == 1000u);      // clamped
    CHECK(s.show_grid == Settings{}.show_grid); // wrong type: unchanged
}

TEST_CASE("settings: a broken file is an error and changes nothing") {
    Settings s = changed();
    CHECK(parse_settings("{ \"canvas\": { \"max_zoom\": 2 ", s)); // truncated
    CHECK(same_file_settings(s, changed()));
    CHECK(parse_settings("[1, 2, 3]", s));
    CHECK(read_settings_file(temp_dir() / "does_not_exist.json", s));
}

TEST_CASE("settings: preferences are migrated from an old imgui.ini") {
    std::filesystem::path path = temp_dir() / "imgui.ini";
    std::ofstream(path) << "[Window][Canvas]\nPos=0,0\n\n"
                           "[Netviz][Settings]\nTheme=Light\nUiScale=1.5\nMaxZoom=64\n"
                           "ShowGrid=0\nShowConsole=0\nMaxSchematicNodes=200000\n"
                           "Recent=/a/b.json\nRecent=/c.json\n\n"
                           "[Docking][Data]\nShowGrid=1\n";
    Settings s;
    std::vector<std::string> recent;
    CHECK(read_legacy_ini(path, s, recent));
    CHECK(s.theme == Theme::Light);
    CHECK(s.ui_scale == 1.5f);
    CHECK(s.max_zoom == 64.0f);
    CHECK(s.show_grid == false); // the later line is in another section
    CHECK(s.show_console == false);
    CHECK(s.max_schematic_nodes == 200000u);
    CHECK(recent == std::vector<std::string>{"/a/b.json", "/c.json"});

    std::ofstream(path) << "[Window][Canvas]\nPos=0,0\n";
    CHECK_FALSE(read_legacy_ini(path, s, recent));
}
