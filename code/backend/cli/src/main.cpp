// Faithful port of rusql-cli/src/main.rs — an interactive SQL REPL over Executor.
// crossterm's cross-platform color API has no C++ equivalent readily available, so
// (per the migration plan's dependency mapping) this uses raw ANSI escape codes
// directly, matching the established pattern in cpp/client/src/main.cpp.

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "engine/executor/executor.hpp"

namespace {

namespace fs = std::filesystem;

constexpr const char* RESET = "\x1b[0m";
constexpr const char* COLOR_RED = "\x1b[31m";
constexpr const char* COLOR_GREEN = "\x1b[32m";
constexpr const char* COLOR_CYAN = "\x1b[36m";
constexpr const char* COLOR_DARK_CYAN = "\x1b[36m";
constexpr const char* COLOR_DARK_GREY = "\x1b[90m";
constexpr const char* COLOR_WHITE = "\x1b[37m";

void print_color(const std::string& text, const char* color) {
    std::cout << color << text << RESET;
    std::cout.flush();
}

void print_help() {
    print_color("+-------------------------------------------------+\n", COLOR_DARK_CYAN);
    print_color("|", COLOR_DARK_CYAN);
    print_color("                    Commands                     ", COLOR_CYAN);
    print_color("|\n", COLOR_DARK_CYAN);
    print_color("+-------------------------------------------------+\n", COLOR_DARK_CYAN);
    print_color("| exit / quit  -- exit the CLI                     |\n", COLOR_DARK_GREY);
    print_color("| help         -- show this guide                  |\n", COLOR_DARK_GREY);
    print_color("+-------------------------------------------------+\n", COLOR_DARK_CYAN);
    print_color("|", COLOR_DARK_CYAN);
    print_color("              Remote Access Guide                ", COLOR_CYAN);
    print_color("|\n", COLOR_DARK_CYAN);
    print_color("+-------------------------------------------------+\n", COLOR_DARK_CYAN);
    print_color("| 1. Run the server on the host machine:          |\n", COLOR_DARK_GREY);
    print_color("|    engine_server.exe                            |\n", COLOR_WHITE);
    print_color("|                                                 |\n", COLOR_DARK_GREY);
    print_color("| 2. Connect from another computer:               |\n", COLOR_DARK_GREY);
    print_color("|    engine_client.exe                            |\n", COLOR_WHITE);
    print_color("|      -u root -p <password>                      |\n", COLOR_WHITE);
    print_color("|      -h <server-ip> -P 7878                     |\n", COLOR_WHITE);
    print_color("+-------------------------------------------------+\n", COLOR_DARK_CYAN);
    std::cout << "\n";
}

void print_banner() {
    print_color("+-------------------------------------------------+\n", COLOR_DARK_CYAN);
    print_color("|", COLOR_DARK_CYAN);
    print_color("              RuSQL -- Custom RDBMS              ", COLOR_CYAN);
    print_color("|\n", COLOR_DARK_CYAN);
    print_color("+-------------------------------------------------+\n", COLOR_DARK_CYAN);
    print_color("| Welcome to RuSQL \xe2\x80\x94 a lightweight custom RDBMS.  |\n", COLOR_DARK_GREY);
    print_color("| Built from scratch, designed for simplicity.    |\n", COLOR_DARK_GREY);
    print_color("| Type SQL to begin.  Use 'help' for commands.    |\n", COLOR_DARK_GREY);
    print_color("+-------------------------------------------------+\n", COLOR_DARK_CYAN);
    std::cout << "\n";
}

// Matches Rust's str::lines() exactly: an empty string yields zero lines, and a
// trailing '\n' ends the last line rather than introducing a new empty one. The
// previous `while (start <= s.size())` version got both of these wrong (returned
// [""] for an empty string, and an extra trailing "" for a string ending in '\n'),
// which mattered here because colorize_table("") on an empty query result would
// then print a spurious blank line before the "(N.NNN sec)" timing line, breaking
// them onto separate lines instead of the single "rusql> (N.NNN sec)" line Rust
// prints when the result is empty.
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    std::size_t start = 0;
    while (start < s.size()) {
        auto nl = s.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

void colorize_table(const std::string& output) {
    for (auto& line : split_lines(output)) {
        if (!line.empty() && line.front() == '+') {
            std::cout << COLOR_DARK_CYAN << line << RESET << "\n";
        } else if (!line.empty() && line.front() == '|') {
            std::vector<std::string> parts;
            std::size_t start = 0;
            for (;;) {
                auto pipe = line.find('|', start);
                if (pipe == std::string::npos) {
                    parts.push_back(line.substr(start));
                    break;
                }
                parts.push_back(line.substr(start, pipe - start));
                start = pipe + 1;
            }
            std::cout << COLOR_DARK_CYAN << "|" << COLOR_CYAN;
            for (std::size_t i = 0; i < parts.size(); i++) {
                if (i == 0 || i == parts.size() - 1) continue;
                std::cout << parts[i] << COLOR_DARK_CYAN;
                if (i < parts.size() - 2) std::cout << "|";
            }
            std::cout << COLOR_DARK_CYAN << "|" << RESET << "\n";
        } else if (line.find("row(s) returned") != std::string::npos) {
            std::cout << COLOR_GREEN << line << RESET << "\n";
        } else {
            std::cout << RESET << line << "\n";
        }
    }
    std::cout << RESET;
}

void run_query(engine::Executor& executor, const std::string& query) {
    auto start = std::chrono::steady_clock::now();
    auto result = executor.execute_sql(query);
    if (result.is_ok()) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        colorize_table(result.value());
        double secs = std::chrono::duration<double>(elapsed).count();
        std::ostringstream oss;
        oss.precision(3);
        oss << std::fixed << "(" << secs << " sec)\n";
        print_color(oss.str(), COLOR_DARK_GREY);
    } else {
        // The Rust original silently swallows "Unknown statement: None" (its parser's
        // Debug-formatted message for an empty token stream); this port's parser
        // reports the same empty-input case as "Unknown statement: end of input"
        // (see parser_core.cpp's parse_stmt) rather than replicating Rust's Option
        // Debug-format text verbatim, so the suppression check matches that instead.
        if (result.error().find("Unknown statement: end of input") == std::string::npos) {
            print_color("ERROR: " + result.error() + "\n", COLOR_RED);
        }
    }
}

// BEGIN...END depth=0 에서의 첫 번째 ';' 바이트 오프셋을 반환
std::optional<std::size_t> find_stmt_end(const std::string& s) {
    std::size_t len = s.size();
    int begin_depth = 0;
    std::size_t i = 0;
    while (i < len) {
        if (s[i] == '\'') {
            i++;
            while (i < len) {
                char c = s[i];
                i++;
                if (c == '\'') break;
            }
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_') {
            std::size_t start = i;
            while (i < len && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) i++;
            std::string word = s.substr(start, i - start);
            std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return std::toupper(c); });
            if (word == "BEGIN") {
                std::size_t j = i;
                while (j < len && std::isspace(static_cast<unsigned char>(s[j]))) j++;
                bool is_transaction;
                if (j >= len || s[j] == ';') {
                    is_transaction = true;
                } else if (std::isalpha(static_cast<unsigned char>(s[j]))) {
                    std::size_t s2 = j, k = j;
                    while (k < len && (std::isalnum(static_cast<unsigned char>(s[k])) || s[k] == '_')) k++;
                    std::string nw = s.substr(s2, k - s2);
                    std::transform(nw.begin(), nw.end(), nw.begin(), [](unsigned char c) { return std::toupper(c); });
                    is_transaction = (nw == "WORK");
                } else {
                    is_transaction = false;
                }
                if (!is_transaction) begin_depth++;
            } else if (word == "END") {
                std::size_t j = i;
                while (j < len && std::isspace(static_cast<unsigned char>(s[j]))) j++;
                bool next_is_sub = false;
                if (j < len && (std::isalpha(static_cast<unsigned char>(s[j])) || s[j] == '_')) {
                    std::size_t s2 = j, k = j;
                    while (k < len && (std::isalnum(static_cast<unsigned char>(s[k])) || s[k] == '_')) k++;
                    std::string nw = s.substr(s2, k - s2);
                    std::transform(nw.begin(), nw.end(), nw.begin(), [](unsigned char c) { return std::toupper(c); });
                    next_is_sub = (nw == "IF" || nw == "WHILE" || nw == "LOOP" || nw == "REPEAT" || nw == "CASE");
                }
                if (!next_is_sub && begin_depth > 0) begin_depth--;
            }
            continue;
        }
        if (s[i] == ';' && begin_depth == 0) return i;
        i++;
    }
    return std::nullopt;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::optional<std::string> find_arg(const std::vector<std::string>& args, const std::string& flag) {
    for (std::size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == flag) return args[i + 1];
    }
    return std::nullopt;
}

std::string default_data_dir() {
    char path_buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, path_buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return "data";
    fs::path exe_path(std::string(path_buf, len));
    fs::path dir = exe_path.parent_path();
    if (dir.has_parent_path()) dir = dir.parent_path();
    if (dir.has_parent_path()) dir = dir.parent_path();
    if (dir.has_parent_path()) dir = dir.parent_path();
    if (dir.has_parent_path()) dir = dir.parent_path();
    return (dir / "data").string();
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);

    std::size_t buffer_pool_size = 64;
    if (auto v = find_arg(args, "--buffer-pool-size")) {
        try {
            buffer_pool_size = std::stoull(*v);
        } catch (...) {
        }
    }

    std::string data_dir;
    if (auto v = find_arg(args, "--data-dir")) data_dir = *v;
    else data_dir = default_data_dir();

    engine::Executor executor(data_dir, buffer_pool_size);

    print_banner();

    std::string buf;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::string trimmed = trim(line);

        if (trimmed.empty() || (trimmed.size() >= 2 && trimmed[0] == '-' && trimmed[1] == '-')) continue;
        if (trimmed == "help") {
            print_help();
            continue;
        }
        if (trimmed == "exit" || trimmed == "quit") {
            print_color("\nBye!\n", COLOR_CYAN);
            break;
        }

        buf += " ";
        buf += trimmed;

        for (;;) {
            auto pos = find_stmt_end(buf);
            if (!pos) break;
            std::string stmt_str = trim(buf.substr(0, *pos));
            buf = buf.substr(*pos + 1);
            std::string buf_trimmed_start = buf;
            auto ws_end = buf_trimmed_start.find_first_not_of(" \t\r\n");
            if (ws_end != std::string::npos && buf_trimmed_start.compare(ws_end, 2, "--") == 0) buf.clear();
            if (stmt_str.empty()) continue;

            print_color("rusql", COLOR_CYAN);
            print_color("> ", COLOR_WHITE);
            std::cout.flush();

            run_query(executor, stmt_str);
        }
    }

    return 0;
}
