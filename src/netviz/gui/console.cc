#include "netviz/gui/console.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace netviz {

namespace {

// Debug/info follow the current theme's text colors; warn/error use
// mid-tone colors that read on both dark and light backgrounds.
ImVec4 level_color(LogLevel level) {
    const ImGuiStyle& style = ImGui::GetStyle();
    switch (level) {
    case LogLevel::Debug: return style.Colors[ImGuiCol_TextDisabled];
    case LogLevel::Info: return style.Colors[ImGuiCol_Text];
    case LogLevel::Warn: return ImVec4(0.85f, 0.55f, 0.0f, 1.0f);
    case LogLevel::Error: return ImVec4(0.9f, 0.25f, 0.25f, 1.0f);
    }
    return style.Colors[ImGuiCol_Text];
}

// The tick at the start of each line tells its level at a glance: debug
// dim, info the theme's accent, warn and error as their text.
ImVec4 tick_color(LogLevel level) {
    if (level == LogLevel::Info)
        return ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
    ImVec4 color = level_color(level);
    if (level == LogLevel::Debug)
        color.w *= 0.6f;
    return color;
}

// Width of the gutter left of the log lines, where a tick marks where each
// line starts (wrapped continuation rows have none).
float gutter_width() {
    return std::round(ImGui::GetFontSize() * 0.6f);
}

// Draws the tick for a line starting at the cursor and moves the cursor past
// the gutter. Ticks fill the whole row, so consecutive lines form one bar
// that breaks only at wrapped continuation rows.
void line_start_tick(LogLevel level) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float line = ImGui::GetTextLineHeight();
    float x = pos.x + std::round(gutter_width() * 0.3f);
    ImVec4 color = tick_color(level);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, pos.y), ImVec2(x + 3.0f, pos.y + line),
                                              ImGui::GetColorU32(color));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gutter_width());
}

} // namespace

ConsolePanel::ConsolePanel() {
    add_log_sink(this);
}

ConsolePanel::~ConsolePanel() {
    remove_log_sink(this);
}

void ConsolePanel::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);
    lines_.push_back(record);
    heights_.push_back(-1.0f);
    if (lines_.size() > kMaxLines) {
        lines_.pop_front();
        heights_.pop_front();
        first_seq_++;
    }
}

void ConsolePanel::clear() {
    std::lock_guard lock(mutex_);
    first_seq_ += lines_.size();
    lines_.clear();
    heights_.clear();
    has_selection_ = false;
}

bool ConsolePanel::is_selected(uint64_t seq) const {
    if (!has_selection_)
        return false;
    return seq >= std::min(anchor_, end_) && seq <= std::max(anchor_, end_);
}

// One log line of `height` at the cursor: selection (click, Shift+click or
// drag across lines), highlight, tick and text.
void ConsolePanel::draw_line(int i, float height) {
    const LogRecord& rec = lines_[i];
    uint64_t seq = first_seq_ + static_cast<uint64_t>(i);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 min(ImGui::GetWindowPos().x, pos.y);
    ImVec2 max(min.x + ImGui::GetWindowWidth(), pos.y + height);
    if (lines_hovered_ && ImGui::IsMouseHoveringRect(min, max, false)) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (!(ImGui::GetIO().KeyShift && has_selection_))
                anchor_ = seq;
            end_ = seq;
            has_selection_ = true;
            selecting_ = true;
            clicked_line_ = true;
        } else if (selecting_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            end_ = seq;
        }
    }
    if (is_selected(seq))
        ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_Header));
    line_start_tick(rec.level);
    ImGui::PushStyleColor(ImGuiCol_Text, level_color(rec.level));
    ImGui::TextUnformatted(rec.text.c_str());
    ImGui::PopStyleColor();
}

// Selected lines (or all shown) as text, one per line.
std::string ConsolePanel::text_of(const std::vector<int>& visible, bool selected_only) const {
    std::string text;
    for (int i : visible) {
        if (selected_only && !is_selected(first_seq_ + static_cast<uint64_t>(i)))
            continue;
        text += lines_[i].text;
        text += '\n';
    }
    return text;
}

// One row per line: ImGuiListClipper submits only the visible ones.
void ConsolePanel::draw_lines(const std::vector<int>& visible) {
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
            draw_line(visible[row], ImGui::GetTextLineHeight());
}

// Lines wrapped to the panel width have different heights, which the
// clipper cannot handle. Each line's height is measured once per width and
// font size; lines outside the view are skipped by position.
void ConsolePanel::draw_wrapped_lines(const std::vector<int>& visible) {
    float width = ImGui::GetContentRegionAvail().x - gutter_width();
    if (width != wrap_width_ || ImGui::GetFontSize() != wrap_font_size_) {
        std::fill(heights_.begin(), heights_.end(), -1.0f);
        wrap_width_ = width;
        wrap_font_size_ = ImGui::GetFontSize();
    }
    float top = ImGui::GetScrollY();
    float bottom = top + ImGui::GetWindowHeight();
    float y = ImGui::GetCursorPosY();
    ImGui::PushTextWrapPos(0.0f);
    for (int i : visible) {
        const LogRecord& rec = lines_[i];
        float& height = heights_[i];
        if (height < 0.0f)
            height = ImGui::CalcTextSize(rec.text.c_str(), nullptr, false, width).y;
        if (y + height >= top && y <= bottom) {
            ImGui::SetCursorPosY(y);
            draw_line(i, height);
        }
        y += height;
    }
    ImGui::PopTextWrapPos();
    // Extend the scrollable area to the end of the last line.
    ImGui::SetCursorPosY(y);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void ConsolePanel::draw(bool* open, const CommandRegistry& commands, bool& wrap) {
    // Only the log area scrolls; the panel itself never does.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin(kWindowName, open, flags)) {
        ImGui::End();
        return;
    }

    {
        std::lock_guard lock(mutex_);

        // Toolbar
        if (ImGui::Button("Clear")) {
            first_seq_ += lines_.size();
            lines_.clear();
            heights_.clear();
            has_selection_ = false;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::Combo("##level", &min_level_, "debug\0info\0warn\0error\0");
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &auto_scroll_);
        ImGui::SameLine();
        ImGui::Checkbox("Wrap", &wrap);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        filter_.Draw("Filter");
        ImGui::Separator();

        // Log lines, leaving room for the input line below (the separator and
        // the input line, measured in the input's font). Only visible rows are
        // submitted, so cost does not grow with the log.
        ImGui::PushFont(font_, 0.0f);
        float footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        ImGui::PopFont();
        if (ImGui::BeginChild("lines", ImVec2(0, -footer), ImGuiChildFlags_None,
                              wrap ? 0 : ImGuiWindowFlags_HorizontalScrollbar)) {
            std::vector<int> visible;
            visible.reserve(lines_.size());
            for (int i = 0; i < static_cast<int>(lines_.size()); i++) {
                const LogRecord& rec = lines_[i];
                if (static_cast<int>(rec.level) < min_level_)
                    continue;
                if (!filter_.PassFilter(rec.text.c_str()))
                    continue;
                visible.push_back(i);
            }

            // Lines touch (no item spacing), so selections are one block.
            lines_hovered_ = ImGui::IsWindowHovered();
            clicked_line_ = false;
            ImGui::PushFont(font_, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
            if (wrap)
                draw_wrapped_lines(visible);
            else
                draw_lines(visible);
            ImGui::PopStyleVar();
            ImGui::PopFont();
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                selecting_ = false;
            if (lines_hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !clicked_line_)
                has_selection_ = false; // clicked below the last line

            if (has_selection_ && ImGui::IsWindowFocused() &&
                ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
                ImGui::SetClipboardText(text_of(visible, true).c_str());
            if (ImGui::BeginPopupContextWindow("lines_menu")) {
                if (ImGui::MenuItem("Copy", "Ctrl+C", false, has_selection_))
                    ImGui::SetClipboardText(text_of(visible, true).c_str());
                if (ImGui::MenuItem("Copy All"))
                    ImGui::SetClipboardText(text_of(visible, false).c_str());
                ImGui::EndPopup();
            }

            if (scroll_to_bottom_ ||
                (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
                ImGui::SetScrollHereY(1.0f);
            scroll_to_bottom_ = false;
        }
        ImGui::EndChild();
    }

    // Outside the lock: commands log, and logging writes back into this panel.
    ImGui::Separator();
    draw_input(commands);
    ImGui::End();
}

void ConsolePanel::draw_input(const CommandRegistry& commands) {
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                ImGuiInputTextFlags_EscapeClearsAll |
                                ImGuiInputTextFlags_CallbackHistory;
    ImGui::PushFont(font_, 0.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool entered = ImGui::InputTextWithHint("##command", "Type a command ('help' for a list)",
                                            input_, sizeof(input_), flags, input_callback, this);
    ImGui::PopFont();
    if (!entered)
        return;

    std::string line = input_;
    input_[0] = '\0';
    history_pos_ = -1;
    ImGui::SetKeyboardFocusHere(-1); // keep focus for the next command

    if (line.find_first_not_of(" \t") == std::string::npos)
        return;
    if (history_.empty() || history_.back() != line)
        history_.push_back(line);
    scroll_to_bottom_ = true;
    log_info("> {}", line);
    commands.run(line);
}

// Up/down arrows walk through previously entered commands.
int ConsolePanel::input_callback(ImGuiInputTextCallbackData* data) {
    ConsolePanel* self = static_cast<ConsolePanel*>(data->UserData);
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || self->history_.empty())
        return 0;

    int count = static_cast<int>(self->history_.size());
    int pos = self->history_pos_;
    if (data->EventKey == ImGuiKey_UpArrow)
        pos = (pos == -1) ? count - 1 : (pos > 0 ? pos - 1 : 0);
    else if (data->EventKey == ImGuiKey_DownArrow)
        pos = (pos == -1 || pos + 1 >= count) ? -1 : pos + 1;

    if (pos != self->history_pos_) {
        self->history_pos_ = pos;
        data->DeleteChars(0, data->BufTextLen);
        if (pos != -1)
            data->InsertChars(0, self->history_[pos].c_str());
    }
    return 0;
}

} // namespace netviz
