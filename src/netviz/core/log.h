#pragma once

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace netviz {

enum class LogLevel { Debug, Info, Warn, Error };

const char* log_level_name(LogLevel level);
// Parses "debug", "info", "warn" or "error".
std::optional<LogLevel> parse_log_level(std::string_view name);

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::chrono::system_clock::time_point time;
    std::string text;
};

// Receives every log record at or above the global level.
// Sinks are called with the logger lock held, so write() must be quick
// and must not log itself.
class LogSink {
  public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
};

// Sinks are not owned by the logger; remove a sink before destroying it.
void add_log_sink(LogSink* sink);
void remove_log_sink(LogSink* sink);

// Enable/disable the built-in stderr sink (enabled by default).
void set_log_to_stderr(bool enabled);

void set_log_level(LogLevel level);
LogLevel log_level();

void log_message(LogLevel level, std::string_view text);

template <typename... Args> void log_debug(std::format_string<Args...> fmt, Args&&... args) {
    if (log_level() <= LogLevel::Debug)
        log_message(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void log_info(std::format_string<Args...> fmt, Args&&... args) {
    if (log_level() <= LogLevel::Info)
        log_message(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void log_warn(std::format_string<Args...> fmt, Args&&... args) {
    if (log_level() <= LogLevel::Warn)
        log_message(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void log_error(std::format_string<Args...> fmt, Args&&... args) {
    log_message(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace netviz
