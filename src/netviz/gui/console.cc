#include "netviz/gui/console.h"

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
    if (lines_.size() > kMaxLines)
        lines_.pop_front();
}

void ConsolePanel::clear() {
    std::lock_guard lock(mutex_);
    lines_.clear();
}

void ConsolePanel::draw(bool* open, const CommandRegistry& commands) {
    // Only the log area scrolls; the panel itself never does.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin(kWindowName, open, flags)) {
        ImGui::End();
        return;
    }

    {
        std::lock_guard lock(mutex_);

        // Toolbar
        if (ImGui::Button("Clear"))
            lines_.clear();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::Combo("##level", &min_level_, "debug\0info\0warn\0error\0");
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &auto_scroll_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        filter_.Draw("Filter");
        ImGui::Separator();

        // Log lines, leaving room for the input line below. Only visible rows
        // are submitted (ImGuiListClipper), so cost does not grow with the log.
        // Room for the separator and the input line, measured in the input's font.
        ImGui::PushFont(font_, 0.0f);
        float footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        ImGui::PopFont();
        if (ImGui::BeginChild("lines", ImVec2(0, -footer), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
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

            ImGui::PushFont(font_, 0.0f);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                    const LogRecord& rec = lines_[visible[row]];
                    ImGui::PushStyleColor(ImGuiCol_Text, level_color(rec.level));
                    ImGui::TextUnformatted(rec.text.c_str());
                    ImGui::PopStyleColor();
                }
            }
            ImGui::PopFont();

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
