#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace netviz {

// Splits a command line into words. Whitespace separates words; double
// quotes group words containing spaces ("my file.json").
std::vector<std::string> split_command_line(std::string_view line);

// Named commands that can be run from text, e.g. the console input.
//
// Commands are the extension point for anything scriptable: register a
// name, a one-line help text and a handler that receives the arguments.
class CommandRegistry {
  public:
    using Handler = std::function<void(const std::vector<std::string>& args)>;

    struct Command {
        std::string help;
        Handler handler;
    };

    void add(const std::string& name, const std::string& help, Handler handler);

    // Runs a command line. Returns false (and logs an error) if the line
    // names an unknown command. Empty lines are ignored.
    bool run(std::string_view line) const;

    // Commands sorted by name.
    const std::map<std::string, Command>& commands() const { return commands_; }

  private:
    std::map<std::string, Command> commands_;
};

} // namespace netviz
