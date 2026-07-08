#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "catch.hpp"

// Test-only copies of code/backend/server/src/main.cpp's find_stmt_end() and the
// per-connection incremental extraction loop it feeds. server/main.cpp is a standalone
// Winsock executable (not linked into engine_core), so it can't be unit-tested directly;
// mirroring test_parser.cpp's split_statements() precedent, this reproduces the exact
// logic here so the regression below is pinned at the unit level.
//
// Bug: the connection loop used to accumulate socket lines into a buffer and, the
// instant the buffer contained ANY ';' (via a naive buf.find(';') check), call a
// whole-buffer splitter and unconditionally clear the buffer -- even if that ';' was
// inside a still-open, multi-line BEGIN...END body. Clearing lost the "still inside
// BEGIN" context, so a trigger/procedure body's own inner statements (and a bare
// "END") leaked out as separate top-level queries on later lines. The fix replaces
// that with an incremental, depth-aware extraction loop that never emits (or drops)
// a statement until find_stmt_end finds a real depth-0 terminator.
namespace {

std::optional<std::size_t> find_stmt_end(const std::string& input) {
    int begin_depth = 0;
    std::size_t i = 0;
    std::size_t len = input.size();

    auto to_upper = [](const std::string& s) {
        std::string out = s;
        for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    };

    while (i < len) {
        if (input[i] == '-' && i + 1 < len && input[i + 1] == '-') {
            while (i < len && input[i] != '\n') i++;
            continue;
        }
        if (input[i] == '#') {
            while (i < len && input[i] != '\n') i++;
            continue;
        }
        if (input[i] == '/' && i + 1 < len && input[i + 1] == '*') {
            i += 2;
            while (i + 1 < len) {
                if (input[i] == '*' && input[i + 1] == '/') { i += 2; break; }
                i++;
            }
            continue;
        }
        if (input[i] == '\'') {
            i++;
            while (i < len) {
                char c = input[i];
                i++;
                if (c == '\'') break;
            }
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(input[i])) || input[i] == '_') {
            std::size_t start = i;
            while (i < len && (std::isalnum(static_cast<unsigned char>(input[i])) || input[i] == '_')) i++;
            std::string upper = to_upper(input.substr(start, i - start));
            if (upper == "BEGIN") {
                std::size_t j = i;
                while (j < len && std::isspace(static_cast<unsigned char>(input[j]))) j++;
                bool is_transaction;
                if (j >= len || input[j] == ';') {
                    is_transaction = true;
                } else if (std::isalpha(static_cast<unsigned char>(input[j]))) {
                    std::size_t s2 = j, k = j;
                    while (k < len && (std::isalnum(static_cast<unsigned char>(input[k])) || input[k] == '_')) k++;
                    is_transaction = to_upper(input.substr(s2, k - s2)) == "WORK";
                } else {
                    is_transaction = false;
                }
                if (!is_transaction) begin_depth++;
            } else if (upper == "END") {
                std::size_t j = i;
                while (j < len && std::isspace(static_cast<unsigned char>(input[j]))) j++;
                bool next_is_sub = false;
                if (j < len && (std::isalpha(static_cast<unsigned char>(input[j])) || input[j] == '_')) {
                    std::size_t s2 = j, k = j;
                    while (k < len && (std::isalnum(static_cast<unsigned char>(input[k])) || input[k] == '_')) k++;
                    std::string nw = to_upper(input.substr(s2, k - s2));
                    next_is_sub = (nw == "IF" || nw == "WHILE" || nw == "LOOP" || nw == "REPEAT" || nw == "CASE");
                }
                if (!next_is_sub && begin_depth > 0) begin_depth--;
            }
            continue;
        }
        if (input[i] == ';' && begin_depth == 0) return i;
        i++;
    }
    return std::nullopt;
}

std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Mirrors the server connection loop: feed one socket line at a time, and collect
// every complete statement that becomes available. A still-open BEGIN block must
// leave the buffer untouched (nothing returned, nothing lost) until its END arrives.
class IncrementalSplitter {
public:
    std::vector<std::string> feed_line(const std::string& line) {
        buf_ += line;
        buf_ += "\n";
        std::vector<std::string> out;
        for (;;) {
            auto pos = find_stmt_end(buf_);
            if (!pos) break;
            std::string q = trim(buf_.substr(0, *pos));
            buf_ = buf_.substr(*pos + 1);
            if (!q.empty()) out.push_back(q);
        }
        return out;
    }

    const std::string& pending() const { return buf_; }

private:
    std::string buf_;
};

} // namespace

TEST_CASE("Multi-line CREATE TRIGGER BEGIN...END body is not split across socket lines", "[server][regression]") {
    // Reproduces test_full-ver2.sql section 25: a CREATE TRIGGER whose body spans
    // several lines, sent to the server one line at a time (as read_line() delivers
    // it). Before the fix, the ';' after the trigger's first inner UPDATE closed the
    // buffer prematurely, so the second UPDATE and the bare "END" leaked out as their
    // own top-level statements on later feed_line() calls.
    IncrementalSplitter sp;

    REQUIRE(sp.feed_line("CREATE TRIGGER trg_after_insert_order AFTER INSERT ON order_header FOR EACH ROW").empty());
    REQUIRE(sp.feed_line("BEGIN").empty());
    // The first inner ';' must NOT close the statement -- begin_depth is still 1 here.
    REQUIRE(sp.feed_line("    UPDATE customer SET loyalty_points = loyalty_points + 10 WHERE id = 1;").empty());
    REQUIRE(sp.feed_line("    UPDATE warehouse SET is_operational = true WHERE id = 1;").empty());

    auto out = sp.feed_line("END;");
    REQUIRE(out.size() == 1);
    CHECK(out[0].find("CREATE TRIGGER trg_after_insert_order") != std::string::npos);
    CHECK(out[0].find("UPDATE warehouse") != std::string::npos);
    CHECK(out[0].find("END") != std::string::npos);

    // The next statement must come through whole and alone -- nothing from the
    // trigger body should have leaked into (or been lost from) the buffer.
    auto next = sp.feed_line("INSERT INTO order_header (customer_id, order_status, placed_at, total) VALUES (2,'pending','2026-06-05 00:00:00',0);");
    REQUIRE(next.size() == 1);
    CHECK(next[0] == "INSERT INTO order_header (customer_id, order_status, placed_at, total) VALUES (2,'pending','2026-06-05 00:00:00',0)");
}

TEST_CASE("A single line containing multiple semicolon-separated statements still splits eagerly", "[server][regression]") {
    IncrementalSplitter sp;
    auto out = sp.feed_line("SELECT 1; SELECT 2;");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "SELECT 1");
    CHECK(out[1] == "SELECT 2");
}

TEST_CASE("A bare transaction BEGIN; does not open a block", "[server][regression]") {
    // Companion to the multi-line trigger case above: a transaction BEGIN has no
    // matching END, so it must not make begin_depth stick above 0 for the rest of
    // the connection.
    IncrementalSplitter sp;
    auto out = sp.feed_line("BEGIN;");
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "BEGIN");

    auto next = sp.feed_line("SELECT 1;");
    REQUIRE(next.size() == 1);
    CHECK(next[0] == "SELECT 1");
}
