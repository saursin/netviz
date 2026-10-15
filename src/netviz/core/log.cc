#include "netviz/core/log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace netviz {

namespace {

struct Logger {
    std::mutex mutex;
    std::vector<LogSink*> sinks;
    bool to_stderr = true;
    std::atomic<LogLevel> level = LogLevel::Info;
};

Logger& logger() {
    static Logger instance;
    return instance;
}

} // namespace

const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    }
    return "?";
}

std::optional<LogLevel> parse_log_level(std::string_view name) {
    for (LogLevel level : {LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error}) {
        if (name == log_level_name(level))
            return level;
    }
    return std::nullopt;
}

void add_log_sink(LogSink* sink) {
    Logger& lg = logger();
    std::lock_guard lock(lg.mutex);
    if (std::find(lg.sinks.begin(), lg.sinks.end(), sink) == lg.sinks.end())
        lg.sinks.push_back(sink);
}

void remove_log_sink(LogSink* sink) {
    Logger& lg = logger();
    std::lock_guard lock(lg.mutex);
    std::erase(lg.sinks, sink);
}

void set_log_to_stderr(bool enabled) {
    Logger& lg = logger();
    std::lock_guard lock(lg.mutex);
    lg.to_stderr = enabled;
}

void set_log_level(LogLevel level) {
    logger().level = level;
}

LogLevel log_level() {
    return logger().level;
}

void log_message(LogLevel level, std::string_view text) {
    Logger& lg = logger();
    if (level < lg.level)
        return;

    LogRecord record;
    record.level = level;
    record.time = std::chrono::system_clock::now();
    record.text = std::string(text);

    std::lock_guard lock(lg.mutex);
    if (lg.to_stderr)
        std::fprintf(stderr, "[%s] %s\n", log_level_name(level), record.text.c_str());
    for (LogSink* sink : lg.sinks)
        sink->write(record);
}

} // namespace netviz
