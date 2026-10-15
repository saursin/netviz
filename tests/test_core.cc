#include "netviz/core/command.h"
#include "netviz/core/log.h"

#include <doctest/doctest.h>

#include <vector>

using namespace netviz;

namespace {

struct CaptureSink : LogSink {
    std::vector<LogRecord> records;
    void write(const LogRecord& record) override { records.push_back(record); }
};

} // namespace

TEST_CASE("sinks receive formatted records") {
    set_log_to_stderr(false);
    CaptureSink a, b;
    add_log_sink(&a);
    add_log_sink(&b);

    log_info("hello {} {}", "world", 42);
    log_error("bad");

    REQUIRE(a.records.size() == 2);
    CHECK(a.records[0].text == "hello world 42");
    CHECK(a.records[0].level == LogLevel::Info);
    CHECK(a.records[1].level == LogLevel::Error);
    CHECK(b.records.size() == 2);

    remove_log_sink(&a);
    remove_log_sink(&b);
    log_info("not captured");
    CHECK(a.records.size() == 2);
    set_log_to_stderr(true);
}

TEST_CASE("records below the level are dropped") {
    set_log_to_stderr(false);
    CaptureSink s;
    add_log_sink(&s);

    set_log_level(LogLevel::Warn);
    log_debug("d");
    log_info("i");
    log_warn("w");
    set_log_level(LogLevel::Info);

    REQUIRE(s.records.size() == 1);
    CHECK(s.records[0].text == "w");

    remove_log_sink(&s);
    set_log_to_stderr(true);
}

TEST_CASE("log levels parse by name") {
    CHECK(parse_log_level("debug") == LogLevel::Debug);
    CHECK(parse_log_level("error") == LogLevel::Error);
    CHECK_FALSE(parse_log_level("loud").has_value());
}

TEST_CASE("command lines split on whitespace and respect quotes") {
    using V = std::vector<std::string>;
    CHECK(split_command_line("") == V{});
    CHECK(split_command_line("  zoom   2 ") == V{"zoom", "2"});
    CHECK(split_command_line("open \"my file.json\" x") == V{"open", "my file.json", "x"});
    CHECK(split_command_line("echo \"\"") == V{"echo", ""});
}

TEST_CASE("registry dispatches commands with arguments") {
    set_log_to_stderr(false);
    CommandRegistry registry;
    std::vector<std::string> got;
    registry.add("zoom", "Set zoom", [&](const std::vector<std::string>& args) { got = args; });

    CHECK(registry.run("zoom 150"));
    CHECK(got == std::vector<std::string>{"150"});
    CHECK(registry.run("   "));
    CHECK_FALSE(registry.run("nope"));
    set_log_to_stderr(true);
}
