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

    void draw(bool* open, const CommandRegistry& commands);
    void clear();

    // Font for log lines and input (e.g. a monospace font); null = default.
    void set_font(ImFont* font) { font_ = font; }

  private:
    void draw_input(const CommandRegistry& commands);
    static int input_callback(ImGuiInputTextCallbackData* data);

    std::mutex mutex_;
    std::deque<LogRecord> lines_;
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
