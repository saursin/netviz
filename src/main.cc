#include "netviz/core/log.h"
#include "netviz/gui/app_ui.h"
#include "netviz/gui/window.h"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace {

const char* const kUsage = R"(usage: netviz [options] [file]

Interactive netlist visualizer.

options:
  -h, --help               show this help and exit
  -V, --version            show the version and exit
  --log-level <level>      debug, info, warn or error (default: info)
)";

void log_banner() {
    netviz::log_info("--------------------------------------------------");
    netviz::log_info("                     netviz");
    netviz::log_info("                     v{}", NETVIZ_VERSION);
    netviz::log_info("--------------------------------------------------");
}

struct Options {
    std::string file;
    netviz::LogLevel log_level = netviz::LogLevel::Info;
};

// Returns the parsed options, or an exit code if the program should stop
// (after --help/--version, or on a bad argument).
std::optional<Options> parse_args(int argc, char** argv, int& exit_code) {
    Options options;
    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::fputs(kUsage, stdout);
            exit_code = 0;
            return std::nullopt;
        }
        if (arg == "-V" || arg == "--version") {
            std::printf("netviz %s\n", NETVIZ_VERSION);
            exit_code = 0;
            return std::nullopt;
        }
        if (arg == "--log-level") {
            std::optional<netviz::LogLevel> level;
            if (i + 1 < argc)
                level = netviz::parse_log_level(argv[++i]);
            if (!level) {
                std::fputs("netviz: --log-level needs one of: debug, info, warn, error\n", stderr);
                exit_code = 2;
                return std::nullopt;
            }
            options.log_level = *level;
            continue;
        }
        if (arg.starts_with("-") || !options.file.empty()) {
            std::fprintf(stderr, "netviz: unexpected argument '%s'\n\n%s", argv[i], kUsage);
            exit_code = 2;
            return std::nullopt;
        }
        options.file = arg;
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    int exit_code = 0;
    std::optional<Options> options = parse_args(argc, argv, exit_code);
    if (!options)
        return exit_code;
    netviz::set_log_level(options->log_level);

    // Created before the window so the console captures startup messages.
    // AppUi only touches ImGui in init() and draw().
    netviz::AppUi ui;
    log_banner();

    netviz::Window window;
    if (!window.init(netviz::WindowConfig{}))
        return 1;
    ui.init(window);

    if (!options->file.empty())
        ui.open_file(options->file);

    while (!ui.quit_requested() && window.process_events()) {
        window.begin_frame();
        ui.draw();
        window.end_frame();
    }
    return 0;
}
