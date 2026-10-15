#include "netviz/gui/icons.h"

#include <imgui_internal.h> // ImTextCharFromUtf8, font baking

#include <cmath>
#include <map>
#include <tuple>

namespace netviz {

namespace {

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

} // namespace

// Draws an icon with its visible bounds centered on `center`.
void draw_icon(ImDrawList* dl, const char* icon, ImVec2 center, ImU32 color) {
    InkBox ink = icon_ink(icon);
    ImVec2 pos(std::round(center.x - (ink.x0 + ink.x1) * 0.5f),
               std::round(center.y - (ink.y0 + ink.y1) * 0.5f));
    dl->AddText(pos, color, icon);
}

// An icon laid out like a text item (advance width x line height), centered
// vertically on the line rather than sitting on the text baseline.
void inline_icon(const char* icon, bool dim) {
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

} // namespace netviz
