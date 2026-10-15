#include "netviz/core/command.h"

#include "netviz/core/log.h"

namespace netviz {

std::vector<std::string> split_command_line(std::string_view line) {
    std::vector<std::string> words;
    std::string current;
    bool in_word = false;
    bool in_quotes = false;

    for (char c : line) {
        if (c == '"') {
            in_quotes = !in_quotes;
            in_word = true;
        } else if (!in_quotes && (c == ' ' || c == '\t')) {
            if (in_word)
                words.push_back(current);
            current.clear();
            in_word = false;
        } else {
            current += c;
            in_word = true;
        }
    }
    if (in_word)
        words.push_back(current);
    return words;
}

void CommandRegistry::add(const std::string& name, const std::string& help, Handler handler) {
    commands_[name] = Command{help, std::move(handler)};
}

bool CommandRegistry::run(std::string_view line) const {
    std::vector<std::string> words = split_command_line(line);
    if (words.empty())
        return true;

    auto it = commands_.find(words[0]);
    if (it == commands_.end()) {
        log_error("Unknown command '{}'. Type 'help' for a list.", words[0]);
        return false;
    }
    words.erase(words.begin());
    it->second.handler(words);
    return true;
}

} // namespace netviz
