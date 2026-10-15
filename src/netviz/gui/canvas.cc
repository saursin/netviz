#include "netviz/gui/canvas.h"

#include <imgui.h>

#include <cmath>

namespace netviz {

namespace {

constexpr float kZoomStep = 1.15f;      // zoom factor per mouse-wheel notch
constexpr float kWheelPanLines = 3.0f;  // pan distance per wheel notch, in text lines
constexpr float kMinGridPixels = 12.0f; // smallest on-screen grid spacing
constexpr int kMajorEvery = 10;         // every Nth grid line is a major line

struct CanvasColors {
    ImU32 background;
    ImU32 minor_line;
    ImU32 major_line;
    ImU32 axis_line;
    ImU32 shape_fill;
    ImU32 shape_outline;
};

const CanvasColors kDarkColors = {IM_COL32(24, 26, 30, 255),   IM_COL32(255, 255, 255, 14),
                                  IM_COL32(255, 255, 255, 34), IM_COL32(90, 140, 220, 110),
                                  IM_COL32(60, 90, 140, 160),  IM_COL32(120, 170, 240, 255)};
const CanvasColors kLightColors = {IM_COL32(246, 247, 249, 255), IM_COL32(0, 0, 0, 16),
                                   IM_COL32(0, 0, 0, 40),        IM_COL32(40, 100, 200, 120),
                                   IM_COL32(170, 200, 240, 200), IM_COL32(40, 100, 200, 255)};

// Placeholder scene object, in world units, until netlists are rendered.
const Rect kPlaceholderRect = {Vec2{-80.0f, -50.0f}, Vec2{80.0f, 50.0f}};

// Draws grid lines only inside the visible world rectangle, with a spacing
// picked from the zoom level. Cost depends on the window size, not on how
// far the user has panned or zoomed.
void draw_grid(ImDrawList* dl, const Camera2D& camera, const CanvasColors& colors, ImVec2 origin,
               ImVec2 size) {
    float spacing = std::pow(10.0f, std::ceil(std::log10(kMinGridPixels / camera.zoom())));
    Rect view = camera.visible_world_rect();

    long long first_x = static_cast<long long>(std::floor(view.min.x / spacing));
    long long last_x = static_cast<long long>(std::ceil(view.max.x / spacing));
    for (long long i = first_x; i <= last_x; i++) {
        float sx = origin.x + camera.world_to_screen(Vec2{i * spacing, 0.0f}).x;
        ImU32 color = (i == 0)                 ? colors.axis_line
                      : (i % kMajorEvery == 0) ? colors.major_line
                                               : colors.minor_line;
        dl->AddLine(ImVec2(sx, origin.y), ImVec2(sx, origin.y + size.y), color);
    }

    long long first_y = static_cast<long long>(std::floor(view.min.y / spacing));
    long long last_y = static_cast<long long>(std::ceil(view.max.y / spacing));
    for (long long i = first_y; i <= last_y; i++) {
        float sy = origin.y + camera.world_to_screen(Vec2{0.0f, i * spacing}).y;
        ImU32 color = (i == 0)                 ? colors.axis_line
                      : (i % kMajorEvery == 0) ? colors.major_line
                                               : colors.minor_line;
        dl->AddLine(ImVec2(origin.x, sy), ImVec2(origin.x + size.x, sy), color);
    }
}

} // namespace

void CanvasPanel::draw(Camera2D& camera, const CanvasOptions& options, float pinch_zoom) {
    hovered_ = false;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool visible = ImGui::Begin(kWindowName, nullptr,
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        return;
    }

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1.0f || size.y < 1.0f) {
        ImGui::End();
        return;
    }
    camera.set_viewport_size(Vec2{size.x, size.y});

    // One invisible item covering the canvas captures mouse input.
    ImGui::InvisibleButton("canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    hovered_ = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();

    ImGuiIO& io = ImGui::GetIO();
    Vec2 mouse{io.MousePos.x - origin.x, io.MousePos.y - origin.y};

    // Pan: drag with middle or right mouse button, or scroll (two-finger
    // swipe on a touchpad; Shift+wheel scrolls horizontally on a mouse).
    bool panning =
        ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (active && panning)
        camera.pan_by_screen_delta(Vec2{io.MouseDelta.x, io.MouseDelta.y});

    // Zoom: touchpad pinch or Ctrl + scroll, anchored at the cursor.
    if (hovered_ && pinch_zoom != 1.0f)
        camera.zoom_at(mouse, pinch_zoom);
    if (hovered_ && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
        if (io.KeyCtrl) {
            camera.zoom_at(mouse, std::pow(kZoomStep, io.MouseWheel));
        } else {
            float wheel_x = io.MouseWheelH;
            float wheel_y = io.MouseWheel;
            if (io.KeyShift && wheel_x == 0.0f) {
                wheel_x = wheel_y;
                wheel_y = 0.0f;
            }
            // Positive wheel values mean scrolling up/left, which reveals
            // content above/left, i.e. the scene moves down/right.
            float step = ImGui::GetFontSize() * kWheelPanLines;
            camera.pan_by_screen_delta(Vec2{wheel_x * step, wheel_y * step});
        }
    }

    // Reset view.
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_F, false))
        camera.reset();

    mouse_world_ = camera.screen_to_world(mouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 end(origin.x + size.x, origin.y + size.y);
    dl->PushClipRect(origin, end, true);
    const CanvasColors& colors = options.light_theme ? kLightColors : kDarkColors;
    dl->AddRectFilled(origin, end, colors.background);
    if (options.show_grid)
        draw_grid(dl, camera, colors, origin, size);

    Vec2 a = camera.world_to_screen(kPlaceholderRect.min);
    Vec2 b = camera.world_to_screen(kPlaceholderRect.max);
    ImVec2 p0(origin.x + a.x, origin.y + a.y);
    ImVec2 p1(origin.x + b.x, origin.y + b.y);
    dl->AddRectFilled(p0, p1, colors.shape_fill);
    dl->AddRect(p0, p1, colors.shape_outline, 0.0f, 1.5f);
    dl->PopClipRect();

    ImGui::End();
}

} // namespace netviz
