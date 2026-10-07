#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/statement_split.hpp"

// The server and the CLI cut their input into statements with engine::find_statement_end (engine/statement_split.hpp), the client counts the answers
// it waits for with engine::count_statement_ends: this is the extraction loop the server builds on it, fed one socket line at a time.
//
// Bug it pins: the connection loop used to accumulate socket lines into a buffer and, the
// instant the buffer contained ANY ';' (via a naive buf.find(';') check), call a
// whole-buffer splitter and unconditionally clear the buffer -- even if that ';' was
// inside a still-open, multi-line BEGIN...END body. Clearing lost the "still inside
// BEGIN" context, so a trigger/procedure body's own inner statements (and a bare
// "END") leaked out as separate top-level queries on later lines. The fix replaces
// that with an incremental, depth-aware extraction loop that never emits (or drops)
// a statement until find_statement_end finds a real depth-0 terminator.
namespace {

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
            auto pos = engine::find_statement_end(buf_);
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

// ---- what a statement is, as the one splitter reads it --------------------------------------------------------------------------------------

namespace {
// the statement is one: its only terminator is the last character
void one_statement(const std::string& sql) {
    INFO(sql);
    auto pos = engine::find_statement_end(sql);
    REQUIRE(pos.has_value());
    REQUIRE(*pos == sql.size() - 1);
    REQUIRE(engine::count_statement_ends(sql) == 1);
}
} // namespace

TEST_CASE("BEGIN and END are names of columns when they are used as names", "[server][statement_split]") {
    // (a column called begin used to open a block that never closed: the server waited for more input for ever)
    one_statement("CREATE TABLE t (id INT PRIMARY KEY, begin INT, end INT);");
    one_statement("CREATE TABLE t (begin INT, end INT);");
    one_statement("INSERT INTO t (id, begin, end) VALUES (1, 2, 3);");
    one_statement("SELECT begin, end FROM t;");
    one_statement("SELECT begin FROM t WHERE end > 5 ORDER BY begin;");
    one_statement("SELECT t.begin, t.end FROM t;");
    one_statement("UPDATE t SET begin = begin + 1, end = 2 WHERE end = 3;");
    one_statement("SELECT id AS begin, id AS end FROM t;");
    one_statement("SELECT COALESCE(begin, 0), MAX(end) FROM t GROUP BY begin HAVING end > 1;");
    one_statement("SELECT CASE WHEN begin > 1 THEN end ELSE 0 END FROM t;");
    one_statement("SELECT CASE WHEN a THEN 1 END, begin FROM t;");
    one_statement("CREATE INDEX i ON t (begin, end);");
    one_statement("ALTER TABLE t ADD COLUMN begin INT;");
    one_statement("SELECT * FROM t WHERE begin IS NULL OR end BETWEEN 1 AND 2;");
    one_statement("SELECT * FROM t WHERE begin NOT IN (1, 2) AND end LIKE 'a%';");
    // (only what comes before the word tells it is a name here, or only what comes after)
    one_statement("SELECT 1 FROM t WHERE a = begin ORDER BY a;");
    one_statement("SELECT 1 FROM t WHERE a = end ORDER BY a;");
    one_statement("SELECT 1 FROM t WHERE a >= t.end GROUP BY a;");
    one_statement("SELECT id begin FROM t;");
    one_statement("SELECT id end FROM t;");
    one_statement("SELECT id begin, id FROM t;");
    one_statement("SELECT COUNT(id begin) FROM t;");
    one_statement("SELECT id end, id FROM t;");
    // a column called end as the result of a CASE branch does not close the CASE
    one_statement("CREATE PROCEDURE p() BEGIN SET @x = CASE WHEN a THEN end ELSE 0 END; END;");
    one_statement("CREATE PROCEDURE p() BEGIN SET @x = CASE WHEN a THEN 1 ELSE end END; SELECT 2; END;");
    // the words are still the keywords of a block
    REQUIRE(!engine::find_statement_end("CREATE PROCEDURE p() BEGIN SELECT 1;").has_value());
    REQUIRE(!engine::find_statement_end("CREATE TRIGGER g AFTER INSERT ON t FOR EACH ROW BEGIN UPDATE t SET a = 1;").has_value());
    one_statement("CREATE PROCEDURE p() BEGIN SELECT 1; END;");
    one_statement("CREATE PROCEDURE p() BEGIN SELECT begin, end FROM t; UPDATE t SET end = 1 WHERE begin = 2; END;");
    one_statement("CREATE TRIGGER g AFTER INSERT ON t FOR EACH ROW BEGIN UPDATE t SET a = 1; UPDATE t SET b = 2; END;");
}

TEST_CASE("a CASE expression's END does not end a block", "[server][statement_split]") {
    // (the END of CASE ... END used to close the BEGIN of the body, so the statement was cut at the ';' after it)
    one_statement("CREATE PROCEDURE p(IN v INT) BEGIN DECLARE s VARCHAR(10); SET s = CASE WHEN v > 5 THEN 'big' ELSE 'small' END; INSERT INTO t VALUES (v, s); END;");
    one_statement("CREATE PROCEDURE p() BEGIN SELECT CASE WHEN a THEN 1 END, CASE WHEN b THEN 2 ELSE 3 END FROM t; END;");
    one_statement("CREATE PROCEDURE p() BEGIN SELECT (CASE x WHEN 1 THEN 'a' END) AS y FROM t; END;");
    one_statement("CREATE PROCEDURE p() BEGIN SELECT CASE WHEN a THEN CASE WHEN b THEN 1 ELSE 2 END ELSE 3 END FROM t; END;");
    one_statement("CREATE TRIGGER g BEFORE INSERT ON t FOR EACH ROW BEGIN SET NEW.s = CASE WHEN NEW.v > 5 THEN 'big' ELSE 'small' END; END;");
    // a CASE statement closes with END CASE; the other kinds of statement end with END IF / END WHILE / END LOOP / END REPEAT
    one_statement("CREATE PROCEDURE p(IN v INT) BEGIN CASE v WHEN 1 THEN SELECT 1; WHEN 2 THEN SELECT 2; END CASE; SELECT 3; END;");
    one_statement("CREATE PROCEDURE p(IN v INT) BEGIN IF v > 1 THEN SELECT 1; ELSE SELECT 2; END IF; SELECT 3; END;");
    one_statement("CREATE PROCEDURE p() BEGIN DECLARE i INT DEFAULT 0; WHILE i < 3 DO SET i = i + 1; END WHILE; REPEAT SET i = i - 1; UNTIL i = 0 END REPEAT; END;");
    one_statement("CREATE PROCEDURE p() BEGIN lbl: LOOP SET @x = 1; LEAVE lbl; END LOOP; END;");
    // a block inside a block
    one_statement("CREATE PROCEDURE p() BEGIN BEGIN SELECT 1; END; SELECT 2; END;");
    // and a plain SELECT with a CASE is still cut at its ';'
    REQUIRE(engine::count_statement_ends("SELECT CASE WHEN a THEN 1 END FROM t; SELECT CASE WHEN a THEN 1 ELSE 2 END FROM t;") == 2);
}

TEST_CASE("comments, strings and quoted names hide their semicolons", "[server][statement_split]") {
    one_statement("SELECT 'a;b' FROM t;");
    one_statement("SELECT 'it\\'s; here' FROM t;");
    one_statement("SELECT 'it''s; here' FROM t;");
    one_statement("SELECT `a;b` FROM t;");
    one_statement("SELECT \"a;b\" FROM t;");
    one_statement("SELECT 1 -- a; comment\n FROM t;");
    one_statement("SELECT 1 # a; comment\n FROM t;");
    one_statement("SELECT 1 /* a; comment */ FROM t;");
    // words inside strings and comments are not words
    one_statement("SELECT 'BEGIN' FROM t;");
    one_statement("SELECT 1 /* BEGIN */ FROM t;");
    one_statement("SELECT 1 -- BEGIN\n FROM t;");
    REQUIRE(engine::count_statement_ends("SELECT 1; SELECT 'x;y'; SELECT 3;") == 3);
    REQUIRE(engine::count_statement_ends("BEGIN; INSERT INTO t VALUES (1); COMMIT;") == 3);
    REQUIRE(engine::count_statement_ends("BEGIN WORK; SELECT 1; COMMIT;") == 3);
    REQUIRE(!engine::find_statement_end("SELECT 1").has_value());
    REQUIRE(!engine::find_statement_end("").has_value());
}
