#include "netviz/view/camera.h"

#include <algorithm>

namespace netviz {

void Camera2D::set_zoom(float z) {
    zoom_ = std::clamp(z, kMinZoom, kMaxZoom);
}

void Camera2D::reset() {
    center_ = Vec2{};
    zoom_ = 1.0f;
}

Vec2 Camera2D::world_to_screen(Vec2 world) const {
    return Vec2{(world.x - center_.x) * zoom_ + viewport_.x * 0.5f,
                (world.y - center_.y) * zoom_ + viewport_.y * 0.5f};
}

Vec2 Camera2D::screen_to_world(Vec2 screen) const {
    return Vec2{(screen.x - viewport_.x * 0.5f) / zoom_ + center_.x,
                (screen.y - viewport_.y * 0.5f) / zoom_ + center_.y};
}

void Camera2D::pan_by_screen_delta(Vec2 delta) {
    center_.x -= delta.x / zoom_;
    center_.y -= delta.y / zoom_;
}

void Camera2D::zoom_at(Vec2 screen_anchor, float factor) {
    Vec2 before = screen_to_world(screen_anchor);
    set_zoom(zoom_ * factor);
    Vec2 after = screen_to_world(screen_anchor);
    // Shift so the anchor maps back to the same world point.
    center_.x += before.x - after.x;
    center_.y += before.y - after.y;
}

Rect Camera2D::visible_world_rect() const {
    return Rect{screen_to_world(Vec2{0.0f, 0.0f}), screen_to_world(viewport_)};
}

} // namespace netviz
