#include "netviz/gui/app_ui.h"

#include "netviz/core/log.h"
#include "netviz/gui/fonts.h"
#include "netviz/gui/window.h"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder, BeginViewportSideBar, settings handlers

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <sstream>
#include <thread>
#include <tuple>

namespace netviz {

namespace {

constexpr float kFontSize = 15.0f; // base UI font size in pixels, before scaling
constexpr float kToolbarZoomStep = 1.25f;
constexpr float kMinUiScale = 0.5f;
constexpr float kMaxUiScale = 3.0f;
constexpr size_t kMaxDumpLines = 5000;  // larger modules are not dumped to the console
constexpr size_t kMaxIssuesLogged = 50; // issues listed in the log after loading

// Visible (inked) bounds of an icon glyph at the current font and size,
// relative to the text position. ImGui's glyph box is not enough: for Lucide
// icons it includes empty rows below the drawing, which makes icons sit high
// when centered on it. The bounds are measured once from the font texture
// and cached.
struct InkBox {
    float x0, y0, x1, y1;
};

InkBox icon_ink(const char* icon) {
    unsigned int codepoint = 0;
    ImTextCharFromUtf8(&codepoint, icon, nullptr);
    float size = ImGui::GetFontSize();
    static std::map<std::tuple<const ImFont*, float, unsigned int>, InkBox> cache;
    auto key = std::make_tuple(ImGui::GetFont(), size, codepoint);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    const ImFontGlyph* g = ImGui::GetFontBaked()->FindGlyph(static_cast<ImWchar>(codepoint));
    InkBox box{g->X0, g->Y0, g->X1, g->Y1};
    ImTextureData* tex = ImGui::GetIO().Fonts->TexData;
    if (g->Visible && tex && tex->Pixels) {
        int u0 = static_cast<int>(g->U0 * tex->Width + 0.5f);
        int v0 = static_cast<int>(g->V0 * tex->Height + 0.5f);
        int u1 = static_cast<int>(g->U1 * tex->Width + 0.5f);
        int v1 = static_cast<int>(g->V1 * tex->Height + 0.5f);
        int left = u1, right = u0 - 1, top = v1, bottom = v0 - 1;
        for (int y = v0; y < v1; y++) {
            for (int x = u0; x < u1; x++) {
                auto* px = static_cast<const unsigned char*>(tex->GetPixelsAt(x, y));
                int alpha = tex->Format == ImTextureFormat_RGBA32 ? px[3] : px[0];
                if (alpha > 32) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
        }
        if (right >= left && u1 > u0 && v1 > v0) {
            float sx = (g->X1 - g->X0) / static_cast<float>(u1 - u0);
            float sy = (g->Y1 - g->Y0) / static_cast<float>(v1 - v0);
            box = {g->X0 + (left - u0) * sx, g->Y0 + (top - v0) * sy, g->X0 + (right + 1 - u0) * sx,
                   g->Y0 + (bottom + 1 - v0) * sy};
        }
    }
    cache[key] = box;
    return box;
}

// Draws an icon with its visible bounds centered on `center`.
void draw_icon(ImDrawList* dl, const char* icon, ImVec2 center, ImU32 color) {
    InkBox ink = icon_ink(icon);
    ImVec2 pos(std::round(center.x - (ink.x0 + ink.x1) * 0.5f),
               std::round(center.y - (ink.y0 + ink.y1) * 0.5f));
    dl->AddText(pos, color, icon);
}

// An icon laid out like a text item (advance width x line height), centered
// vertically on the line rather than sitting on the text baseline.
void inline_icon(const char* icon, bool dim = true) {
    ImVec2 size(ImGui::CalcTextSize(icon).x, ImGui::GetTextLineHeight());
    ImGui::Dummy(size);
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImU32 color = ImGui::GetColorU32(dim ? ImGuiCol_TextDisabled : ImGuiCol_Text);
    draw_icon(ImGui::GetWindowDrawList(), icon,
              ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), color);
}

// Square icon-only button of `size` with a tooltip; the icon is centered on
// its visible bounds.
bool icon_button(const char* icon, const char* tooltip, float size) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(icon, ImVec2(size, size));
    ImGui::SetItemTooltip("%s", tooltip);

    ImGuiCol bg = ImGui::IsItemActive()    ? ImGuiCol_ButtonActive
                  : ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered
                                           : ImGuiCol_Button;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), ImGui::GetColorU32(bg),
                      ImGui::GetStyle().FrameRounding);
    draw_icon(dl, icon, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f),
              ImGui::GetColorU32(ImGuiCol_Text));
    return pressed;
}

bool toolbar_button(const char* icon, const char* tooltip) {
    return icon_button(icon, tooltip, ImGui::GetFrameHeight());
}

// One segment on the right of the status bar: an icon and a value in a
// fixed-width slot (sized for `widest`) so changing values do not shift the
// bar, with a tooltip explaining it.
struct StatusSegment {
    const char* icon;
    std::string value;
    const char* widest;
    std::string tooltip;
};

constexpr float kIconGap = 4.0f; // between a segment's icon and its value

float segment_width(const StatusSegment& seg) {
    return ImGui::CalcTextSize(seg.icon).x + kIconGap + ImGui::CalcTextSize(seg.widest).x;
}

// Draws sections of segments right-aligned to `right` (a cursor x). Sections
// are separated by dividers; segments within a section are just spaced apart.
void draw_status_sections(const std::vector<std::vector<StatusSegment>>& sections, float right) {
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float segment_gap = spacing * 2.0f;
    float divider = 1.0f + 2.0f * spacing;
    float total = 0.0f;
    for (size_t k = 0; k < sections.size(); k++) {
        const std::vector<StatusSegment>& section = sections[k];
        total += k > 0 ? divider : 0.0f;
        for (size_t i = 0; i < section.size(); i++)
            total += segment_width(section[i]) + (i > 0 ? segment_gap : 0.0f);
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - total));

    for (size_t k = 0; k < sections.size(); k++) {
        const std::vector<StatusSegment>& section = sections[k];
        if (k > 0) {
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine(0.0f, spacing);
        }
        for (size_t i = 0; i < section.size(); i++) {
            const StatusSegment& seg = section[i];
            if (i > 0)
                ImGui::SameLine(0.0f, segment_gap);
            ImGui::BeginGroup();
            inline_icon(seg.icon);
            ImGui::SameLine(0.0f, kIconGap);
            float slot_start = ImGui::GetCursorPosX();
            ImGui::TextUnformatted(seg.value.c_str());
            ImGui::SameLine(0.0f, 0.0f);
            float used = ImGui::GetCursorPosX() - slot_start;
            ImGui::Dummy(ImVec2(std::max(0.0f, ImGui::CalcTextSize(seg.widest).x - used),
                                ImGui::GetTextLineHeight()));
            ImGui::EndGroup();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", seg.tooltip.c_str());
        }
        ImGui::SameLine(0.0f, spacing);
    }
}

void toolbar_separator() {
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
}

// "554 MB", "1.94 GB"; "-" when unknown.
std::string format_bytes(uint64_t bytes) {
    if (bytes == 0)
        return "-";
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb < 1024.0)
        return std::format("{:.0f} MB", mb);
    return std::format("{:.2f} GB", mb / 1024.0);
}

// Draws dim text in a slot as wide as `widest`, so whatever comes after it
// stays in place when the text changes.
void fixed_width_text(const std::string& text, const char* widest) {
    float slot = ImGui::CalcTextSize(widest).x;
    float start = ImGui::GetCursorPosX();
    ImGui::TextDisabled("%s", text.c_str());
    ImGui::SameLine();
    ImGui::SetCursorPosX(
        std::max(ImGui::GetCursorPosX(), start + slot + ImGui::GetStyle().ItemSpacing.x));
}

// One decimal for small coordinates, whole numbers from 1000 on.
std::string format_coord(float v) {
    return std::abs(v) < 1000.0f ? std::format("{:.1f}", v) : std::format("{:.0f}", v);
}

// "100%", "12.5%", "0.0123%"
std::string format_zoom(float zoom) {
    return std::format("{:.3g}%", zoom * 100.0f);
}

// "950", "412k", "1.0M"
std::string format_count(uint64_t n) {
    if (n < 1000)
        return std::to_string(n);
    if (n < 1000000)
        return std::format("{}k", n / 1000);
    return std::format("{:.1f}M", static_cast<double>(n) / 1e6);
}

// Expands a leading "~" to the home directory.
std::filesystem::path expand_path(const std::string& arg) {
    if (!arg.empty() && arg[0] == '~') {
        if (const char* home = std::getenv("HOME"))
            return std::filesystem::path(home) /
                   arg.substr(arg.size() > 1 && arg[1] == '/' ? 2 : 1);
    }
    return std::filesystem::path(arg);
}

constexpr size_t kMaxListed = 200;     // entries printed by `ls`
constexpr size_t kMaxRecentFiles = 10; // entries in File > Open Recent

// Parses "on"/"off" style arguments for commands.
bool parse_on_off(const std::string& arg, bool& out) {
    if (arg == "on" || arg == "1" || arg == "true") {
        out = true;
        return true;
    }
    if (arg == "off" || arg == "0" || arg == "false") {
        out = false;
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void AppUi::init(Window& window) {
    window_ = &window;
    display_scale_ = window.display_scale();
    load_fonts();
    register_settings_handler();
    register_commands();
    apply_style();
}

// Inter for UI text with Lucide icons merged in (so icons work in any label),
// and JetBrains Mono for the console.
void AppUi::load_fonts() {
    ImGuiIO& io = ImGui::GetIO();

    ImFontConfig config;
    config.FontDataOwnedByAtlas = false; // embedded data is static

    // Lucide icons live in the Unicode private use area, where Inter also
    // has glyphs; exclude that range from Inter so the icons are not hidden.
    static const ImWchar kPrivateUseArea[] = {0xE000, 0xF8FF, 0};
    ImFontConfig text_config = config;
    text_config.GlyphExcludeRanges = kPrivateUseArea;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(inter_ttf),
                                   static_cast<int>(inter_ttf_size), kFontSize, &text_config);

    ImFontConfig icon_config = config;
    icon_config.MergeMode = true;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(lucide_ttf),
                                   static_cast<int>(lucide_ttf_size), 0.0f, &icon_config);

    ImFont* mono = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(jetbrains_mono_ttf),
                                                  static_cast<int>(jetbrains_mono_ttf_size),
                                                  kFontSize, &config);
    console_.set_font(mono);
}

// Rebuilds the ImGui style from the theme and scale settings.
void AppUi::apply_style() {
    ImGuiStyle style;
    if (settings_.theme == Theme::Light)
        ImGui::StyleColorsLight(&style);
    else
        ImGui::StyleColorsDark(&style);
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScaleAllSizes(display_scale_ * settings_.ui_scale);
    style.FontScaleDpi = display_scale_;
    style.FontScaleMain = settings_.ui_scale;
    ImGui::GetStyle() = style;
    applied_theme_ = settings_.theme;
    applied_ui_scale_ = settings_.ui_scale;
}

// Stores Settings as a "[Netviz][Settings]" section in imgui.ini, next to
// ImGui's own window and dock layout data.
void AppUi::register_settings_handler() {
    ImGuiSettingsHandler handler;
    handler.TypeName = "Netviz";
    handler.TypeHash = ImHashStr("Netviz");
    handler.UserData = this;
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void* {
        return std::string_view(name) == "Settings" ? reinterpret_cast<void*>(1) : nullptr;
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* h, void*, const char* line) {
        std::string_view text(line);
        size_t eq = text.find('=');
        if (eq == std::string_view::npos)
            return;
        static_cast<AppUi*>(h->UserData)
            ->read_setting(std::string(text.substr(0, eq)), std::string(text.substr(eq + 1)));
    };
    handler.ApplyAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h) {
        AppUi* self = static_cast<AppUi*>(h->UserData);
        self->saved_settings_ = self->settings_;
        self->style_dirty_ = true;
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out) {
        out->append(static_cast<AppUi*>(h->UserData)->write_settings().c_str());
    };
    ImGui::AddSettingsHandler(&handler);
}

void AppUi::read_setting(const std::string& key, const std::string& value) {
    bool on = value == "1";
    if (key == "Theme")
        settings_.theme = (value == "Light") ? Theme::Light : Theme::Dark;
    else if (key == "UiScale")
        settings_.ui_scale =
            std::clamp(std::strtof(value.c_str(), nullptr), kMinUiScale, kMaxUiScale);
    else if (key == "ShowGrid")
        settings_.show_grid = on;
    else if (key == "ShowHierarchy")
        settings_.show_hierarchy = on;
    else if (key == "ShowModules")
        settings_.show_modules = on;
    else if (key == "ShowProperties")
        settings_.show_properties = on;
    else if (key == "ShowConsole")
        settings_.show_console = on;
    else if (key == "ShowPerfStats")
        settings_.show_perf_stats = on;
    else if (key == "Recent")
        recent_files_.push_back(value);
    else if (key == "MinZoom")
        settings_.min_zoom = std::clamp(std::strtof(value.c_str(), nullptr), 1e-4f, 1.0f);
    else if (key == "MaxZoom")
        settings_.max_zoom = std::clamp(std::strtof(value.c_str(), nullptr), 1.0f, 1000.0f);
}

std::string AppUi::write_settings() const {
    std::ostringstream out;
    out << "[Netviz][Settings]\n";
    out << "Theme=" << (settings_.theme == Theme::Light ? "Light" : "Dark") << "\n";
    out << "UiScale=" << settings_.ui_scale << "\n";
    out << "ShowGrid=" << settings_.show_grid << "\n";
    out << "ShowHierarchy=" << settings_.show_hierarchy << "\n";
    out << "ShowModules=" << settings_.show_modules << "\n";
    out << "ShowProperties=" << settings_.show_properties << "\n";
    out << "ShowConsole=" << settings_.show_console << "\n";
    out << "ShowPerfStats=" << settings_.show_perf_stats << "\n";
    out << "MinZoom=" << settings_.min_zoom << "\n";
    out << "MaxZoom=" << settings_.max_zoom << "\n";
    for (const std::string& path : recent_files_)
        out << "Recent=" << path << "\n";
    out << "\n";
    return out.str();
}

void AppUi::register_commands() {
    commands_.add("help", "List commands", [this](const std::vector<std::string>&) {
        for (const auto& [name, command] : commands_.commands())
            log_info("  {:<12} {}", name, command.help);
    });
    commands_.add("clear", "Clear the console",
                  [this](const std::vector<std::string>&) { console_.clear(); });
    commands_.add("open", "open <path>: open a netlist file",
                  [this](const std::vector<std::string>& args) {
                      if (args.size() != 1) {
                          log_error("usage: open <path>");
                          return;
                      }
                      open_file(expand_path(args[0]).string());
                  });
    commands_.add("pwd", "Print the current directory", [](const std::vector<std::string>&) {
        log_info("{}", std::filesystem::current_path().string());
    });
    commands_.add("cd", "cd [dir]: change the current directory (default: home)",
                  [](const std::vector<std::string>& args) {
                      std::filesystem::path dir = expand_path(args.empty() ? "~" : args[0]);
                      std::error_code ec;
                      std::filesystem::current_path(dir, ec);
                      if (ec)
                          log_error("cd: {}: {}", dir.string(), ec.message());
                      else
                          log_info("{}", std::filesystem::current_path().string());
                  });
    commands_.add("ls", "ls [dir]: list a directory (default: current)",
                  [](const std::vector<std::string>& args) {
                      std::filesystem::path dir = expand_path(args.empty() ? "." : args[0]);
                      std::error_code ec;
                      std::vector<std::string> dirs, files;
                      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                          std::string name = entry.path().filename().string();
                          if (entry.is_directory(ec))
                              dirs.push_back(name + "/");
                          else
                              files.push_back(name);
                      }
                      if (ec) {
                          log_error("ls: {}: {}", dir.string(), ec.message());
                          return;
                      }
                      std::sort(dirs.begin(), dirs.end());
                      std::sort(files.begin(), files.end());
                      dirs.insert(dirs.end(), files.begin(), files.end());
                      for (size_t i = 0; i < dirs.size() && i < kMaxListed; i++)
                          log_info("  {}", dirs[i]);
                      if (dirs.size() > kMaxListed)
                          log_info("  ... {} more", dirs.size() - kMaxListed);
                      if (dirs.empty())
                          log_info("  (empty)");
                  });
    commands_.add("dump", "dump [module]: print a module of the loaded design (default: top)",
                  [this](const std::vector<std::string>& args) {
                      const Design* design = design_.design();
                      if (!design) {
                          log_error("No design loaded");
                          return;
                      }
                      std::optional<ModuleId> module = design->top();
                      if (!args.empty()) {
                          std::optional<NameId> name = design->names().find(args[0]);
                          module = name ? design->find_module(*name) : std::nullopt;
                      }
                      if (!module) {
                          log_error("No such module (or no top module): {}",
                                    args.empty() ? "" : args[0]);
                          return;
                      }
                      std::ostringstream out;
                      dump(*design, *module, out);
                      std::string text = out.str();
                      size_t lines = std::count(text.begin(), text.end(), '\n');
                      if (lines > kMaxDumpLines) {
                          log_error("Module is too large to dump here ({} lines)", lines);
                          return;
                      }
                      std::istringstream in(text);
                      for (std::string line; std::getline(in, line);)
                          log_info("{}", line);
                  });
    commands_.add("reload", "Load the current file again", [this](const std::vector<std::string>&) {
        if (!can_reload())
            log_error("Nothing to reload");
        else
            reload();
    });
    commands_.add("close", "Close the loaded design",
                  [this](const std::vector<std::string>&) { close_design(); });
    commands_.add("cancel", "Cancel a running load",
                  [this](const std::vector<std::string>&) { cancel_loading(); });
    commands_.add("stats", "Print a summary of the loaded design",
                  [this](const std::vector<std::string>&) {
                      if (!design_.design()) {
                          log_error("No design loaded");
                          return;
                      }
                      log_info("{}", design_.file());
                      log_design_stats(nullptr);
                  });
    commands_.add("check", "Check the loaded design for problems",
                  [this](const std::vector<std::string>&) {
                      if (!design_.design()) {
                          log_error("No design loaded");
                          return;
                      }
                      std::vector<Issue> issues = validate(*design_.design());
                      for (const Issue& issue : issues) {
                          if (issue.severity == Issue::Severity::Error)
                              log_error("{}", issue.message);
                          else
                              log_warn("{}", issue.message);
                      }
                      log_info("{} issue(s)", issues.size());
                  });
    commands_.add("zoom", "zoom <percent>: set the zoom level",
                  [this](const std::vector<std::string>& args) {
                      float percent = args.size() == 1 ? std::strtof(args[0].c_str(), nullptr) : 0;
                      if (percent <= 0.0f) {
                          log_error("usage: zoom <percent>");
                          return;
                      }
                      camera_.set_zoom(percent / 100.0f);
                  });
    commands_.add("reset-view", "Reset pan and zoom",
                  [this](const std::vector<std::string>&) { camera_.reset(); });
    commands_.add("grid", "grid on|off: show or hide the canvas grid",
                  [this](const std::vector<std::string>& args) {
                      if (args.size() != 1 || !parse_on_off(args[0], settings_.show_grid))
                          log_error("usage: grid on|off");
                  });
    commands_.add("theme", "theme dark|light: switch color theme",
                  [this](const std::vector<std::string>& args) {
                      if (args.size() == 1 && args[0] == "dark")
                          settings_.theme = Theme::Dark;
                      else if (args.size() == 1 && args[0] == "light")
                          settings_.theme = Theme::Light;
                      else {
                          log_error("usage: theme dark|light");
                          return;
                      }
                      style_dirty_ = true;
                  });
    commands_.add("scale", "scale <percent>: set the UI scale",
                  [this](const std::vector<std::string>& args) {
                      float percent = args.size() == 1 ? std::strtof(args[0].c_str(), nullptr) : 0;
                      float scale = percent / 100.0f;
                      if (scale < kMinUiScale || scale > kMaxUiScale) {
                          log_error("usage: scale <percent>  ({:.0f}-{:.0f})", kMinUiScale * 100,
                                    kMaxUiScale * 100);
                          return;
                      }
                      settings_.ui_scale = scale;
                      style_dirty_ = true;
                  });
    commands_.add("quit", "Quit netviz",
                  [this](const std::vector<std::string>&) { quit_requested_ = true; });
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void AppUi::open_file(const std::string& path) {
    if (loading_.valid()) {
        log_warn("Still loading {}; cancel it first or wait until it is done", loading_path_);
        return;
    }
    if (!std::filesystem::exists(path)) {
        log_error("File not found: {}", path);
        return;
    }
    if (design_.design()) {
        pending_open_ = path; // draw_open_prompt() asks before closing the current one
        return;
    }
    start_loading(path);
}

void AppUi::close_design() {
    if (!design_.design())
        return;
    log_info("Closed {}", design_.file());
    design_.set_design(nullptr, {}, "");
    window_->set_title("netviz");
}

bool AppUi::can_reload() const {
    return !last_file_.empty() && !loading_.valid();
}

// Frees the current design first, like opening another file, so memory never
// holds two versions; if the file is now broken, it can be fixed and reloaded.
void AppUi::reload() {
    if (!can_reload())
        return;
    std::string path = last_file_;
    close_design();
    start_loading(path);
}

void AppUi::cancel_loading() {
    if (!loading_.valid() || load_progress_->cancel.load())
        return;
    log_info("Cancelling loading of {} ...", loading_path_);
    load_progress_->cancel = true;
}

// Asks whether to close the open design before opening another one. The old
// design is freed before the new one loads, so both are never in memory.
void AppUi::draw_open_prompt() {
    const char* title = "Open another netlist?";
    if (!pending_open_.empty() && !ImGui::IsPopupOpen(title))
        ImGui::OpenPopup(title);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    auto filename = [](const std::string& p) {
        return std::filesystem::path(p).filename().string();
    };
    ImGui::Text("Close %s and open %s?", filename(design_.file()).c_str(),
                filename(pending_open_).c_str());
    ImGui::Spacing();
    if (ImGui::Button("Open") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        std::string path = std::move(pending_open_);
        pending_open_.clear();
        close_design();
        start_loading(path);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        pending_open_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void AppUi::start_loading(const std::string& path) {
    log_info("Loading {} ...", path);
    memory_before_load_ = Window::memory_usage().current;
    last_file_ = path;
    loading_path_ = path;
    load_progress_ = std::make_shared<ImportProgress>();
    loading_ = std::async(std::launch::async, [path, progress = load_progress_] {
        auto start = std::chrono::steady_clock::now();
        LoadedDesign loaded;
        loaded.result = import_yosys_json(path, progress.get());
        if (loaded.result.design)
            loaded.instances = find_instances(*loaded.result.design);
        loaded.seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        Window::post_wake(); // the UI thread picks up the result on its next frame
        return loaded;
    });
}

// Installs a finished background load (UI thread only).
void AppUi::poll_loading() {
    if (!loading_.valid() ||
        loading_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    LoadedDesign loaded = loading_.get();
    std::string path = std::move(loading_path_);
    ImportResult& result = loaded.result;
    if (result.cancelled) {
        log_info("Loading of {} cancelled", path);
        return;
    }
    if (!result.design) {
        log_error("Could not load {}: {}", path, result.error);
        return;
    }

    log_info("Loaded {} in {:.3f} s", path, loaded.seconds);
    for (size_t i = 0; i < result.issues.size() && i < kMaxIssuesLogged; i++) {
        const Issue& issue = result.issues[i];
        if (issue.severity == Issue::Severity::Error)
            log_error("{}", issue.message);
        else
            log_warn("{}", issue.message);
    }
    if (result.issues.size() > kMaxIssuesLogged)
        log_warn("... {} more issues (run 'check' to list them)",
                 result.issues.size() - kMaxIssuesLogged);

    design_.set_design(std::move(result.design), std::move(loaded.instances), path);
    log_design_stats(&result.timings);
    add_recent_file(path);
    window_->set_title(std::format("{} - netviz", std::filesystem::path(path).filename().string()));
}

// Stage, a progress bar with the percentage inside, the item counts and a
// cancel button. The bar is drawn by hand so it can be a little thinner than
// a frame and keep its text centered instead of following the fill.
void AppUi::draw_load_progress() {
    ImportStage stage = load_progress_->stage.load(std::memory_order_relaxed);
    uint64_t done = load_progress_->done.load(std::memory_order_relaxed);
    uint64_t total = load_progress_->total.load(std::memory_order_relaxed);
    bool cancelling = load_progress_->cancel.load(std::memory_order_relaxed);
    const char* label = cancelling                       ? "cancelling"
                        : stage == ImportStage::Reading  ? "reading file"
                        : stage == ImportStage::Building ? "building netlist"
                                                         : "checking";
    ImGui::SameLine();
    fixed_width_text(label, "building netlist");

    // Bar centered on the status bar's middle (where the text is centered),
    // a little thinner than the text.
    float mid_y = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() * 0.5f;
    float line = ImGui::GetTextLineHeight();
    float height = line * 0.8f;
    float width = ImGui::GetFontSize() * 8.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float top = mid_y - height * 0.5f;
    ImVec2 bar_min(pos.x, top);
    ImVec2 bar_max(pos.x + width, top + height);
    ImGui::Dummy(ImVec2(width, line));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rounding = height * 0.5f;
    dl->AddRectFilled(bar_min, bar_max, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    ImU32 fill = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    bool counted = stage == ImportStage::Building && total > 0;
    if (counted) {
        float fraction = std::min(1.0f, static_cast<float>(done) / static_cast<float>(total));
        if (fraction > 0.0f)
            dl->AddRectFilled(bar_min, ImVec2(bar_min.x + width * fraction, bar_max.y), fill,
                              rounding);

        // Percentage centered in the bar, in a smaller font so it fits.
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
        std::string percent = std::format("{:.0f}%", fraction * 100.0f);
        ImVec2 size = ImGui::CalcTextSize(percent.c_str());
        ImVec2 at(std::floor((bar_min.x + bar_max.x - size.x) * 0.5f),
                  std::floor((bar_min.y + bar_max.y - size.y) * 0.5f));
        // Dark over the filled part, normal text color over the rest, so it
        // stays readable wherever the fill edge is.
        float split = bar_min.x + width * fraction;
        dl->PushClipRect(bar_min, ImVec2(split, bar_max.y), true);
        dl->AddText(at, IM_COL32(20, 20, 20, 255), percent.c_str());
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(split, bar_min.y), bar_max, true);
        dl->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), percent.c_str());
        dl->PopClipRect();
        ImGui::PopFont();
    } else {
        // No count for this stage: a short segment sliding across the bar.
        float t = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.7f, 1.3f) - 0.3f;
        float x0 = bar_min.x + width * std::max(t, 0.0f);
        float x1 = bar_min.x + width * std::min(t + 0.3f, 1.0f);
        if (x1 > x0)
            dl->AddRectFilled(ImVec2(x0, bar_min.y), ImVec2(x1, bar_max.y), fill, rounding);
    }

    ImGui::SameLine();
    std::string counts =
        counted ? std::format("{} / {} objects", format_count(done), format_count(total)) : "";
    fixed_width_text(counts, "999.9M / 999.9M objects");
    if (!cancelling) {
        // Square button as tall as the text line, vertically centered on it.
        float size = line + 2.0f;
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, mid_y - size * 0.5f));
        if (icon_button(icons::kCancel, "Cancel loading", size))
            cancel_loading();
    }
}

// Stops a running load (it notices within a few milliseconds, or right after
// the initial file parse) and waits for its thread to finish.
// Prints timings (if given) and a summary of the loaded design.
void AppUi::log_design_stats(const ImportTimings* timings) {
    const Design* design = design_.design();
    if (!design)
        return;
    if (timings)
        log_info("  time: parse {:.3f} s, build {:.3f} s, check {:.3f} s", timings->parse,
                 timings->build, timings->check);

    size_t blackboxes = 0, cells = 0, wires = 0, ports = 0;
    uint64_t signal_bits = 0;
    for (const Module& module : design->modules()) {
        if (module.is_blackbox()) {
            blackboxes++;
            continue;
        }
        cells += module.cells().size();
        wires += module.wires().size();
        ports += module.ports().size();
        signal_bits += module.signal_count();
    }
    std::string top = design->top() ? design->names().str(design->module(*design->top()).name())
                                    : std::string("none");
    log_info("  {} modules ({} blackboxes), top: {}", design->modules().size(), blackboxes, top);
    log_info("  {} cells, {} wires, {} ports, {} signal bits (excluding blackboxes)", cells, wires,
             ports, signal_bits);
    uint64_t now = Window::memory_usage().current;
    if (timings && now >= memory_before_load_)
        log_info("  memory: {} (+{})", format_bytes(now), format_bytes(now - memory_before_load_));
    else
        log_info("  memory: {}", format_bytes(now));
}

// Most recent first, no duplicates, at most kMaxRecentFiles.
void AppUi::add_recent_file(const std::string& path) {
    std::error_code ec;
    std::string absolute = std::filesystem::absolute(path, ec).lexically_normal().string();
    if (ec)
        absolute = path;
    std::erase(recent_files_, absolute);
    recent_files_.insert(recent_files_.begin(), absolute);
    if (recent_files_.size() > kMaxRecentFiles)
        recent_files_.resize(kMaxRecentFiles);
    ImGui::MarkIniSettingsDirty();
}

// File > Open Recent: file name, with its folder shown dimmed alongside.
void AppUi::draw_recent_menu() {
    if (!ImGui::BeginMenu("Open Recent", !recent_files_.empty() && !loading_.valid()))
        return;
    std::string chosen;
    for (const std::string& path : recent_files_) {
        std::filesystem::path p(path);
        std::string label = std::format("{}##{}", p.filename().string(), path);
        bool exists = std::filesystem::exists(p);
        if (ImGui::MenuItem(label.c_str(), p.parent_path().string().c_str(), false, exists))
            chosen = path;
        if (!exists)
            ImGui::SetItemTooltip("File not found");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Clear Recent")) {
        recent_files_.clear();
        ImGui::MarkIniSettingsDirty();
    }
    ImGui::EndMenu();
    if (!chosen.empty())
        open_file(chosen); // after EndMenu: may open the "close current design?" prompt
}

void AppUi::shutdown() {
    if (loading_.valid()) {
        cancel_loading();
        loading_.wait();
    }
}

// ---------------------------------------------------------------------------
// Per-frame drawing
// ---------------------------------------------------------------------------

void AppUi::draw() {
    auto frame_start = std::chrono::steady_clock::now();
    // Style changes from last frame are applied before any window is drawn.
    if (style_dirty_ || settings_.theme != applied_theme_ ||
        settings_.ui_scale != applied_ui_scale_) {
        apply_style();
        style_dirty_ = false;
    }
    camera_.set_zoom_limits(settings_.min_zoom, settings_.max_zoom);
    while (std::optional<std::string> path = window_->take_opened_file())
        open_file(*path);
    poll_loading();
    // Redraw 10 times a second while loading so the progress bar moves.
    window_->set_idle_timeout_ms(loading_.valid() ? 100 : 500);

    handle_shortcuts();
    draw_menu_bar();
    // Bars are drawn before the dock space so the docked panels fit between them.
    draw_toolbar();
    draw_status_bar();

    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    if (reset_layout_ || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        build_default_layout(dockspace_id);
        reset_layout_ = false;
    }
    ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport());

    CanvasOptions canvas_options;
    canvas_options.show_grid = settings_.show_grid;
    canvas_options.light_theme = settings_.theme == Theme::Light;
    canvas_.draw(camera_, canvas_options, window_->take_pinch_zoom());
    if (settings_.show_hierarchy)
        design_.draw_hierarchy(&settings_.show_hierarchy);
    if (settings_.show_modules)
        design_.draw_modules(&settings_.show_modules);
    if (settings_.show_properties)
        design_.draw_properties(&settings_.show_properties);
    if (settings_.show_console)
        console_.draw(&settings_.show_console, commands_);

    draw_about();
    preferences_.draw(settings_);
    draw_open_prompt();
    if (show_imgui_demo_)
        ImGui::ShowDemoWindow(&show_imgui_demo_);
    if (show_imgui_metrics_)
        ImGui::ShowMetricsWindow(&show_imgui_metrics_);

    if (settings_ != saved_settings_) {
        ImGui::MarkIniSettingsDirty();
        saved_settings_ = settings_;
    }
    last_ui_ms_ =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start)
            .count();
}

// Hides both left panes if either is shown, otherwise shows both.
void AppUi::toggle_left_column() {
    bool show = !(settings_.show_hierarchy || settings_.show_modules);
    settings_.show_hierarchy = show;
    settings_.show_modules = show;
}

void AppUi::handle_shortcuts() {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal))
        window_->show_open_file_dialog();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_R, ImGuiInputFlags_RouteGlobal) ||
        ImGui::Shortcut(ImGuiKey_F5, ImGuiInputFlags_RouteGlobal))
        reload();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_W, ImGuiInputFlags_RouteGlobal) &&
        !loading_.valid())
        close_design();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Comma, ImGuiInputFlags_RouteGlobal))
        preferences_.open();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal))
        quit_requested_ = true;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, ImGuiInputFlags_RouteGlobal))
        toggle_left_column();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_GraveAccent, ImGuiInputFlags_RouteGlobal))
        settings_.show_console = !settings_.show_console;
}

void AppUi::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Ctrl+O", false, !loading_.valid()))
            window_->show_open_file_dialog();
        draw_recent_menu();
        if (ImGui::MenuItem("Reload", "Ctrl+R", false, can_reload()))
            reload();
        if (ImGui::MenuItem("Close", "Ctrl+W", false, design_.design() && !loading_.valid()))
            close_design();
        if (loading_.valid() && ImGui::MenuItem("Cancel Loading"))
            cancel_loading();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q"))
            quit_requested_ = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Hierarchy", nullptr, &settings_.show_hierarchy);
        ImGui::MenuItem("Modules", nullptr, &settings_.show_modules);
        if (ImGui::MenuItem("Left Column", "Ctrl+B"))
            toggle_left_column();
        ImGui::Separator();
        ImGui::MenuItem("Properties", nullptr, &settings_.show_properties);
        ImGui::MenuItem("Console", "Ctrl+`", &settings_.show_console);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset View", "F"))
            camera_.reset();
        if (ImGui::MenuItem("Reset Layout")) {
            settings_.show_hierarchy = true;
            settings_.show_modules = true;
            settings_.show_properties = true;
            settings_.show_console = true;
            reset_layout_ = true;
        }
        ImGui::EndMenu();
    }

    draw_settings_menu();

    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("ImGui Demo", nullptr, &show_imgui_demo_);
        ImGui::MenuItem("ImGui Metrics", nullptr, &show_imgui_metrics_);
        ImGui::Separator();
        if (ImGui::MenuItem("About netviz"))
            show_about_ = true;
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

// Quick toggles that change the current view; configuration lives in the
// Preferences window.
void AppUi::draw_settings_menu() {
    if (!ImGui::BeginMenu("Settings"))
        return;
    ImGui::MenuItem("Show Grid", nullptr, &settings_.show_grid);
    ImGui::MenuItem("Show Performance Stats", nullptr, &settings_.show_perf_stats);
    ImGui::Separator();
    if (ImGui::MenuItem("Preferences...", "Ctrl+,"))
        preferences_.open();
    ImGui::EndMenu();
}

// Top bar with file and view actions. Zoom buttons zoom about the canvas center.
// Uses tighter padding and spacing than regular windows to stay compact.
void AppUi::draw_toolbar() {
    const ImGuiStyle& style = ImGui::GetStyle();
    float pad_y = style.FramePadding.y;
    float gap = style.ItemInnerSpacing.x * 0.5f;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    float height = ImGui::GetFrameHeight() + pad_y * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, pad_y));
    bool open = ImGui::BeginViewportSideBar("##Toolbar", ImGui::GetMainViewport(), ImGuiDir_Up,
                                            height, flags);
    ImGui::PopStyleVar();
    if (open) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, style.ItemSpacing.y));
        Vec2 vp = camera_.viewport_size();
        Vec2 center{vp.x * 0.5f, vp.y * 0.5f};

        ImGui::BeginDisabled(loading_.valid());
        if (toolbar_button(icons::kOpen, "Open (Ctrl+O)"))
            window_->show_open_file_dialog();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!can_reload());
        if (toolbar_button(icons::kReload, "Reload (Ctrl+R)"))
            reload();
        ImGui::EndDisabled();
        toolbar_separator();
        ImGui::BeginDisabled(camera_.zoom() >= camera_.max_zoom());
        if (toolbar_button(icons::kZoomIn, "Zoom in"))
            camera_.zoom_at(center, kToolbarZoomStep);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(camera_.zoom() <= camera_.min_zoom());
        if (toolbar_button(icons::kZoomOut, "Zoom out"))
            camera_.zoom_at(center, 1.0f / kToolbarZoomStep);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (toolbar_button(icons::kResetView, "Reset view (F)"))
            camera_.reset();
        ImGui::PopStyleVar();
    }
    ImGui::End();
}

// Bottom bar. Left: what is open or loading. Right: segments for the view
// (cursor, zoom) and, if enabled, performance stats.
void AppUi::draw_status_bar() {
    // A plain strip (not a menu bar) so the padding above and below the
    // content is equal and everything is centered on one line.
    const ImGuiStyle& style = ImGui::GetStyle();
    float pad = style.FramePadding.y + 2.0f;
    float height = ImGui::GetTextLineHeight() + 2.0f * pad;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, pad));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, style.Colors[ImGuiCol_MenuBarBg]);
    bool open = ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down,
                                            height, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (open) {
        float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

        // Left: status.
        if (loading_.valid()) {
            ImGui::Text("Loading %s",
                        std::filesystem::path(loading_path_).filename().string().c_str());
            draw_load_progress();
        } else if (const Design* design = design_.design()) {
            inline_icon(icons::kFile);
            ImGui::SameLine(0.0f, kIconGap);
            ImGui::TextUnformatted(
                std::filesystem::path(design_.file()).filename().string().c_str());
            ImGui::SetItemTooltip("%s", design_.file().c_str());
            ImGui::SameLine();
            ImGui::BeginGroup();
            inline_icon(icons::kModules);
            ImGui::SameLine(0.0f, kIconGap);
            ImGui::TextDisabled("%zu", design->modules().size());
            ImGui::EndGroup();
            ImGui::SetItemTooltip("%zu modules", design->modules().size());
        } else {
            ImGui::TextDisabled("No design loaded");
        }

        // Right: performance stats (if enabled), then canvas info at the far right.
        std::vector<std::vector<StatusSegment>> sections;
        if (settings_.show_perf_stats) {
            // Reading process memory is a system call; once a second is plenty.
            double now = ImGui::GetTime();
            if (memory_refresh_at_ == 0.0 || now >= memory_refresh_at_) {
                memory_ = Window::memory_usage();
                memory_refresh_at_ = now + 1.0;
            }
            sections.push_back({
                {icons::kMemory, format_bytes(memory_.current), "999.99 GB",
                 std::format("Memory used by netviz: {}\nPeak: {}", format_bytes(memory_.current),
                             format_bytes(memory_.peak))},
                {icons::kTimer, std::format("{:.1f} ms", last_ui_ms_), "99.9 ms",
                 "Time to build the last frame's UI"},
            });
        }
        std::string cursor = "-";
        if (canvas_.hovered()) {
            Vec2 m = canvas_.mouse_world();
            cursor = std::format("{}, {}", format_coord(m.x), format_coord(m.y));
        }
        Vec2 c = camera_.center();
        sections.push_back({
            {icons::kPointer, cursor, "-99999, -99999",
             "Cursor position on the canvas (world units)"},
            {icons::kZoom, format_zoom(camera_.zoom()), "0.0123%",
             std::format("Zoom\nView center: {:.1f}, {:.1f}", c.x, c.y)},
        });
        draw_status_sections(sections, right);
    }
    ImGui::End();
}

void AppUi::draw_about() {
    if (show_about_) {
        ImGui::OpenPopup("About netviz");
        show_about_ = false;
    }
    if (ImGui::BeginPopupModal("About netviz", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("netviz %s", NETVIZ_VERSION);
        ImGui::TextDisabled("Interactive netlist visualizer");
        ImGui::Separator();
        ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
        if (ImGui::Button("Close"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Hierarchy above Modules on the left, properties on the right, console at the bottom
// (under the canvas), canvas in the remaining center.
void AppUi::build_default_layout(ImGuiID dockspace_id) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

    ImGuiID center = dockspace_id;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID left_bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.40f, nullptr, &left);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);

    ImGui::DockBuilderDockWindow(DesignPanels::kHierarchyWindow, left);
    ImGui::DockBuilderDockWindow(DesignPanels::kModulesWindow, left_bottom);
    ImGui::DockBuilderDockWindow(DesignPanels::kPropertiesWindow, right);
    ImGui::DockBuilderDockWindow(ConsolePanel::kWindowName, bottom);
    ImGui::DockBuilderDockWindow(CanvasPanel::kWindowName, center);
    ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace netviz
