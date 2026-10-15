#include "netviz/gui/app_ui.h"

#include "netviz/core/log.h"
#include "netviz/gui/fonts.h"
#include "netviz/gui/window.h"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder, BeginViewportSideBar, settings handlers

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <sstream>

namespace netviz {

namespace {

const char* const kSidebarWindow = "Design";
const char* const kPropertiesWindow = "Properties";

constexpr float kFontSize = 15.0f; // base UI font size in pixels, before scaling
constexpr float kToolbarZoomStep = 1.25f;
constexpr float kMinUiScale = 0.5f;
constexpr float kMaxUiScale = 3.0f;

const float kUiScales[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f};

// Square icon-only button with a tooltip.
//
// The icon is centered on its visible bounds rather than its advance box,
// since icon glyphs rarely fill their box evenly and would look off-center.
bool toolbar_button(const char* icon, const char* tooltip) {
    float size = ImGui::GetFrameHeight();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(icon, ImVec2(size, size));
    ImGui::SetItemTooltip("%s", tooltip);

    ImGuiCol bg = ImGui::IsItemActive()    ? ImGuiCol_ButtonActive
                  : ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered
                                           : ImGuiCol_Button;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), ImGui::GetColorU32(bg),
                      ImGui::GetStyle().FrameRounding);

    unsigned int codepoint = 0;
    ImTextCharFromUtf8(&codepoint, icon, nullptr);
    const ImFontGlyph* glyph = ImGui::GetFontBaked()->FindGlyph(static_cast<ImWchar>(codepoint));
    float x = pos.x + (size - (glyph->X1 - glyph->X0)) * 0.5f - glyph->X0;
    float y = pos.y + (size - (glyph->Y1 - glyph->Y0)) * 0.5f - glyph->Y0;
    dl->AddText(ImVec2(std::floor(x), std::floor(y)), ImGui::GetColorU32(ImGuiCol_Text), icon);
    return pressed;
}

void toolbar_separator() {
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
}

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
    else if (key == "ShowSidebar")
        settings_.show_sidebar = on;
    else if (key == "ShowProperties")
        settings_.show_properties = on;
    else if (key == "ShowConsole")
        settings_.show_console = on;
}

std::string AppUi::write_settings() const {
    std::ostringstream out;
    out << "[Netviz][Settings]\n";
    out << "Theme=" << (settings_.theme == Theme::Light ? "Light" : "Dark") << "\n";
    out << "UiScale=" << settings_.ui_scale << "\n";
    out << "ShowGrid=" << settings_.show_grid << "\n";
    out << "ShowSidebar=" << settings_.show_sidebar << "\n";
    out << "ShowProperties=" << settings_.show_properties << "\n";
    out << "ShowConsole=" << settings_.show_console << "\n\n";
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
                      open_file(args[0]);
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
    if (!std::filesystem::exists(path)) {
        log_error("File not found: {}", path);
        return;
    }
    current_file_ = path;
    std::string name = std::filesystem::path(path).filename().string();
    window_->set_title(std::format("{} - netviz", name));
    log_info("Opened {}", path);
    log_warn("Netlist loading is not implemented yet");
}

// ---------------------------------------------------------------------------
// Per-frame drawing
// ---------------------------------------------------------------------------

void AppUi::draw() {
    // Style changes from last frame are applied before any window is drawn.
    if (style_dirty_) {
        apply_style();
        style_dirty_ = false;
    }
    while (std::optional<std::string> path = window_->take_opened_file())
        open_file(*path);

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
    if (settings_.show_sidebar)
        draw_sidebar();
    if (settings_.show_properties)
        draw_properties();
    if (settings_.show_console)
        console_.draw(&settings_.show_console, commands_);

    draw_about();
    if (show_imgui_demo_)
        ImGui::ShowDemoWindow(&show_imgui_demo_);

    if (settings_ != saved_settings_) {
        ImGui::MarkIniSettingsDirty();
        saved_settings_ = settings_;
    }
}

void AppUi::handle_shortcuts() {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal))
        window_->show_open_file_dialog();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal))
        quit_requested_ = true;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, ImGuiInputFlags_RouteGlobal))
        settings_.show_sidebar = !settings_.show_sidebar;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_GraveAccent, ImGuiInputFlags_RouteGlobal))
        settings_.show_console = !settings_.show_console;
}

void AppUi::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Ctrl+O"))
            window_->show_open_file_dialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q"))
            quit_requested_ = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Sidebar", "Ctrl+B", &settings_.show_sidebar);
        ImGui::MenuItem("Properties", nullptr, &settings_.show_properties);
        ImGui::MenuItem("Console", "Ctrl+`", &settings_.show_console);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset View", "F"))
            camera_.reset();
        if (ImGui::MenuItem("Reset Layout")) {
            settings_.show_sidebar = true;
            settings_.show_properties = true;
            settings_.show_console = true;
            reset_layout_ = true;
        }
        ImGui::EndMenu();
    }

    draw_settings_menu();

    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("ImGui Demo", nullptr, &show_imgui_demo_);
        ImGui::Separator();
        if (ImGui::MenuItem("About netviz"))
            show_about_ = true;
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void AppUi::draw_settings_menu() {
    if (!ImGui::BeginMenu("Settings"))
        return;

    if (ImGui::BeginMenu("Theme")) {
        if (ImGui::MenuItem("Dark", nullptr, settings_.theme == Theme::Dark)) {
            settings_.theme = Theme::Dark;
            style_dirty_ = true;
        }
        if (ImGui::MenuItem("Light", nullptr, settings_.theme == Theme::Light)) {
            settings_.theme = Theme::Light;
            style_dirty_ = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("UI Scale")) {
        for (float scale : kUiScales) {
            std::string label = std::format("{:.0f}%", scale * 100.0f);
            if (ImGui::MenuItem(label.c_str(), nullptr, settings_.ui_scale == scale)) {
                settings_.ui_scale = scale;
                style_dirty_ = true;
            }
        }
        ImGui::EndMenu();
    }

    ImGui::MenuItem("Show Grid", nullptr, &settings_.show_grid);

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

        if (toolbar_button(icons::kOpen, "Open (Ctrl+O)"))
            window_->show_open_file_dialog();
        toolbar_separator();
        if (toolbar_button(icons::kZoomIn, "Zoom in"))
            camera_.zoom_at(center, kToolbarZoomStep);
        ImGui::SameLine();
        if (toolbar_button(icons::kZoomOut, "Zoom out"))
            camera_.zoom_at(center, 1.0f / kToolbarZoomStep);
        ImGui::SameLine();
        if (toolbar_button(icons::kResetView, "Reset view (F)"))
            camera_.reset();
        ImGui::PopStyleVar();
    }
    ImGui::End();
}

// Bottom bar showing the current view: zoom, center and cursor position.
void AppUi::draw_status_bar() {
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    float height = ImGui::GetFrameHeight();
    if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, height,
                                    flags)) {
        if (ImGui::BeginMenuBar()) {
            Vec2 c = camera_.center();
            std::string cursor = "-";
            if (canvas_.hovered()) {
                Vec2 m = canvas_.mouse_world();
                cursor = std::format("{:.1f}, {:.1f}", m.x, m.y);
            }
            std::string text =
                std::format("Cursor {}   |   Center {:.1f}, {:.1f}   |   Zoom {:.1f}%", cursor, c.x,
                            c.y, camera_.zoom() * 100.0f);

            // Right-align within the bar.
            float width = ImGui::CalcTextSize(text.c_str()).x;
            float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(right - width);
            ImGui::TextUnformatted(text.c_str());
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

// Left panel: design hierarchy (placeholder until netlists can be loaded).
void AppUi::draw_sidebar() {
    if (!ImGui::Begin(kSidebarWindow, &settings_.show_sidebar)) {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Hierarchy", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (current_file_.empty()) {
            ImGui::TextDisabled("No design loaded");
        } else {
            ImGui::TextUnformatted(
                std::filesystem::path(current_file_).filename().string().c_str());
            ImGui::TextDisabled("(not parsed yet)");
        }
    }

    ImGui::End();
}

// Details of the selected object (placeholder until selection exists).
void AppUi::draw_properties() {
    if (!ImGui::Begin(kPropertiesWindow, &settings_.show_properties)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Nothing selected");
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

// Sidebar on the left, properties on the right, console at the bottom
// (under the canvas), canvas in the remaining center.
void AppUi::build_default_layout(ImGuiID dockspace_id) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

    ImGuiID center = dockspace_id;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);

    ImGui::DockBuilderDockWindow(kSidebarWindow, left);
    ImGui::DockBuilderDockWindow(kPropertiesWindow, right);
    ImGui::DockBuilderDockWindow(ConsolePanel::kWindowName, bottom);
    ImGui::DockBuilderDockWindow(CanvasPanel::kWindowName, center);
    ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace netviz
