#include "netviz/view/camera.h"

#include <doctest/doctest.h>

using namespace netviz;

namespace {

Camera2D make_camera() {
    Camera2D cam;
    cam.set_viewport_size(Vec2{800.0f, 600.0f});
    cam.set_center(Vec2{10.0f, -20.0f});
    cam.set_zoom(2.5f);
    return cam;
}

} // namespace

TEST_CASE("viewport center maps to camera center") {
    Camera2D cam = make_camera();
    Vec2 w = cam.screen_to_world(Vec2{400.0f, 300.0f});
    CHECK(w.x == doctest::Approx(10.0f));
    CHECK(w.y == doctest::Approx(-20.0f));
}

TEST_CASE("world/screen round trip") {
    Camera2D cam = make_camera();
    Vec2 s{123.0f, 456.0f};
    Vec2 back = cam.world_to_screen(cam.screen_to_world(s));
    CHECK(back.x == doctest::Approx(s.x));
    CHECK(back.y == doctest::Approx(s.y));
}

TEST_CASE("zoom_at keeps the anchor point fixed") {
    Camera2D cam = make_camera();
    Vec2 anchor{100.0f, 50.0f};
    Vec2 before = cam.screen_to_world(anchor);
    cam.zoom_at(anchor, 3.0f);
    Vec2 after = cam.screen_to_world(anchor);
    CHECK(cam.zoom() == doctest::Approx(7.5f));
    CHECK(after.x == doctest::Approx(before.x));
    CHECK(after.y == doctest::Approx(before.y));
}

TEST_CASE("zoom is clamped") {
    Camera2D cam;
    cam.set_zoom(1e9f);
    CHECK(cam.zoom() == Camera2D::kDefaultMaxZoom);
    cam.set_zoom(0.0f);
    CHECK(cam.zoom() == Camera2D::kDefaultMinZoom);

    // Narrower limits clamp the current zoom too.
    cam.set_zoom(4.0f);
    cam.set_zoom_limits(0.5f, 2.0f);
    CHECK(cam.zoom() == 2.0f);
    cam.zoom_at(Vec2{0.0f, 0.0f}, 100.0f);
    CHECK(cam.zoom() == 2.0f);
}

TEST_CASE("panning moves the world opposite to the drag") {
    Camera2D cam = make_camera();
    Vec2 p = cam.world_to_screen(Vec2{0.0f, 0.0f});
    cam.pan_by_screen_delta(Vec2{30.0f, -10.0f});
    Vec2 q = cam.world_to_screen(Vec2{0.0f, 0.0f});
    CHECK(q.x == doctest::Approx(p.x + 30.0f));
    CHECK(q.y == doctest::Approx(p.y - 10.0f));
}

TEST_CASE("visible rect covers the viewport") {
    Camera2D cam = make_camera();
    Rect r = cam.visible_world_rect();
    CHECK(r.width() == doctest::Approx(800.0f / 2.5f));
    CHECK(r.height() == doctest::Approx(600.0f / 2.5f));
    CHECK((r.min.x + r.max.x) * 0.5f == doctest::Approx(10.0f));
}
