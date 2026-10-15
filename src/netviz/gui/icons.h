#pragma once

// Drawing Lucide icons (see fonts.h) centered on what is visibly drawn, not
// on the glyph box, which for these icons includes empty rows below.

#include <imgui.h>

namespace netviz {

// Draws an icon with its visible bounds centered on `center`.
void draw_icon(ImDrawList* dl, const char* icon, ImVec2 center, ImU32 color);

// An icon laid out like a text item (advance width x line height), centered
// vertically on the line rather than sitting on the text baseline.
void inline_icon(const char* icon, bool dim = true);

// Square icon-only button of `size` with a tooltip; the icon is centered on
// its visible bounds.
bool icon_button(const char* icon, const char* tooltip, float size);

} // namespace netviz
