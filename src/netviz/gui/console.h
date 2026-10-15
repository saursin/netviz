#pragma once

#include "netviz/core/command.h"
#include "netviz/core/log.h"

#include <imgui.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace netviz {

// Bottom panel showing log output, with a command input line.
// Registers itself as a log sink and keeps at most kMaxLines records.
class ConsolePanel : public LogSink {
  public:
    static constexpr const char* kWindowName = "Console";
    static constexpr size_t kMaxLines = 10000;

    ConsolePanel();
    ~ConsolePanel() override;

    ConsolePanel(const ConsolePanel&) = delete;
    ConsolePanel& operator=(const ConsolePanel&) = delete;

    void write(const LogRecord& record) override;

    // `wrap`: wrap long lines to the panel width (a toggle in the panel).
    void draw(bool* open, const CommandRegistry& commands, bool& wrap);
    void clear();

    // Font for log lines and input (e.g. a monospace font); null = default.
    void set_font(ImFont* font) { font_ = font; }

  private:
    void draw_input(const CommandRegistry& commands);
    void draw_line(int index, float height);
    bool is_selected(uint64_t seq) const;
    std::string text_of(const std::vector<int>& visible, bool selected_only) const;
    void draw_lines(const std::vector<int>& visible);
    void draw_wrapped_lines(const std::vector<int>& visible);
    static int input_callback(ImGuiInputTextCallbackData* data);

    std::mutex mutex_;
    std::deque<LogRecord> lines_;
    // Wrapped height of each line (< 0: not measured yet), for wrap_width_.
    std::deque<float> heights_;
    float wrap_width_ = -1.0f;
    float wrap_font_size_ = -1.0f;
    // Line selection, by sequence number (lines_[i] is number first_seq_ + i),
    // so it stays put while old lines are dropped.
    uint64_t first_seq_ = 0;
    uint64_t anchor_ = 0, end_ = 0;
    bool has_selection_ = false;
    bool selecting_ = false; // dragging
    bool lines_hovered_ = false;
    bool clicked_line_ = false; // this frame
    ImGuiTextFilter filter_;
    int min_level_ = static_cast<int>(LogLevel::Debug);
    bool auto_scroll_ = true;
    bool scroll_to_bottom_ = false;
    ImFont* font_ = nullptr;

    char input_[512] = {};
    std::vector<std::string> history_;
    int history_pos_ = -1; // -1: editing a new line
};

} // namespace netviz
