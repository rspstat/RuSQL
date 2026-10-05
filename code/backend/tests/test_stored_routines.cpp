#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Variables, procedures, functions and triggers in the statements they run: a stored procedure's parameters and variables and a session's @variables
// are values wherever a statement names them (they used to be known only to IF / WHILE / SET: `UPDATE t SET v = 1 WHERE id = p_id` updated nothing),
// a trigger runs once for each row with that row's NEW.x / OLD.x, and a failing statement of its body fails the statement that fired it.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;
const std::string N = "NULL";

void open_db(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
}

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
}

std::string fails(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}

Rows cells(const std::string& text) {
    Rows rows;
    std::istringstream in(text);
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue;
        std::vector<std::string> row;
        std::size_t pos = 1;
        while (pos < line.size()) {
            std::size_t bar = line.find('|', pos);
            if (bar == std::string::npos) break;
            std::string v = line.substr(pos, bar - pos);
            v.erase(0, v.find_first_not_of(' '));
            v.erase(v.find_last_not_of(' ') + 1);
            row.push_back(v);
            pos = bar + 1;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

Rows q(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
    return cells(r.value());
}

void seed(Executor& ex) {
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30)");
    ok(ex, "CREATE TABLE lg (id INT, note VARCHAR(20), v INT)");
}
} // namespace

TEST_CASE("a procedure's parameters and variables are values in its statements", "[routines][procedure]") {
    TempDataDir dir("sr_proc");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    ok(ex, "CREATE PROCEDURE p_upd(IN pid INT, IN pv INT) BEGIN UPDATE t SET v = pv WHERE id = pid; END");
    ok(ex, "CALL p_upd(1, 99)");
    REQUIRE(q(ex, "SELECT id, v FROM t ORDER BY id") == Rows{{"1", "99"}, {"2", "20"}, {"3", "30"}});
    ok(ex, "CREATE PROCEDURE p_ins(IN pid INT, IN pv INT) BEGIN INSERT INTO t VALUES (pid, pv); END");
    ok(ex, "CALL p_ins(7, 70)");
    REQUIRE(q(ex, "SELECT v FROM t WHERE id = 7") == Rows{{"70"}});
    ok(ex, "CREATE PROCEDURE p_del(IN pid INT) BEGIN DELETE FROM t WHERE id = pid; END");
    ok(ex, "CALL p_del(7)");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t") == Rows{{"3"}});
    // the answer of the last SELECT, with a parameter in its WHERE and in its select list
    ok(ex, "CREATE PROCEDURE p_sel(IN lim INT) BEGIN SELECT id, lim, lim AS again FROM t WHERE v > lim ORDER BY id; END");
    REQUIRE(q(ex, "CALL p_sel(25)") == Rows{{"1", "25", "25"}, {"3", "25", "25"}});
    // DECLARE / SET, then a statement that reads the variable
    ok(ex, "CREATE PROCEDURE p_dec(IN pid INT) BEGIN DECLARE n INT; SET n = pid + 1; UPDATE t SET v = n WHERE id = pid; END");
    ok(ex, "CALL p_dec(2)");
    REQUIRE(q(ex, "SELECT v FROM t WHERE id = 2") == Rows{{"3"}});
    // loops and branches around statements
    ok(ex, "CREATE PROCEDURE p_loop(IN n INT) BEGIN DECLARE i INT DEFAULT 1; DECLARE j INT DEFAULT 0; WHILE i <= n DO SET j = i * 10; INSERT INTO lg VALUES (i, 'loop', j); SET i = i + 1; END WHILE; END");
    ok(ex, "CALL p_loop(3)");
    REQUIRE(q(ex, "SELECT id, v FROM lg ORDER BY id") == Rows{{"1", "10"}, {"2", "20"}, {"3", "30"}});
    ok(ex, "CREATE PROCEDURE p_if(IN x INT) BEGIN IF x > 5 THEN INSERT INTO lg VALUES (x, 'big', 0); ELSE INSERT INTO lg VALUES (x, 'small', 0); END IF; END");
    ok(ex, "CALL p_if(9)");
    ok(ex, "CALL p_if(2)");
    REQUIRE(q(ex, "SELECT id, note FROM lg WHERE v = 0 ORDER BY id") == Rows{{"2", "small"}, {"9", "big"}});
    ok(ex, "CREATE PROCEDURE p_up(IN s VARCHAR(20)) BEGIN SELECT UPPER(s) AS u, LENGTH(s) AS n; END");
    REQUIRE(q(ex, "CALL p_up('it''s')") == Rows{{"IT'S", "4"}});
    ok(ex, "CREATE PROCEDURE p_cnt() BEGIN DECLARE n INT; SET n = (SELECT COUNT(*) FROM t WHERE v > 15); INSERT INTO lg VALUES (n, 'count', 77); END");
    ok(ex, "CALL p_cnt()");
    REQUIRE(q(ex, "SELECT id FROM lg WHERE note = 'count'") == Rows{{"2"}});
    // a procedure calling another with its variable and with an expression, and a string argument
    ok(ex, "CREATE PROCEDURE p_nest(IN a INT) BEGIN CALL p_if(a); CALL p_if(a + 10); END");
    ok(ex, "CALL p_nest(1)");
    REQUIRE(q(ex, "SELECT id, note FROM lg WHERE v = 0 ORDER BY id") == Rows{{"1", "small"}, {"2", "small"}, {"9", "big"}, {"11", "big"}});
    ok(ex, "CREATE PROCEDURE p_str(IN s VARCHAR(20)) BEGIN INSERT INTO lg VALUES (100, s, 1); END");
    ok(ex, "CALL p_str('hello world')");
    ok(ex, "CALL p_str('it''s')");
    REQUIRE(q(ex, "SELECT note FROM lg WHERE id = 100 ORDER BY note") == Rows{{"hello world"}, {"it's"}});
    // the table's own column of the same name loses to the variable (MySQL), and a variable that is not used leaves the statement alone
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t WHERE v >= 3") == Rows{{"3"}});
}

TEST_CASE("OUT parameters, SELECT ... INTO and an @variable that was never set", "[routines][out]") {
    TempDataDir dir("sr_out");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    REQUIRE(q(ex, "SELECT @never") == Rows{{N}}); // (a session that has set no variable yet)
    ok(ex, "CREATE PROCEDURE p_out(IN lim INT, OUT c INT) BEGIN SELECT COUNT(*) INTO c FROM t WHERE v > lim; END");
    ok(ex, "CALL p_out(15, @r)");
    REQUIRE(q(ex, "SELECT @r") == Rows{{"2"}});
    ok(ex, "CREATE PROCEDURE p_out2(OUT c INT, INOUT d INT) BEGIN SET c = 42; SET d = d + 1; END");
    ok(ex, "SET @d = 10");
    ok(ex, "CALL p_out2(@c, @d)");
    REQUIRE(q(ex, "SELECT @c, @d") == Rows{{"42", "11"}});
    // SELECT ... INTO outside a procedure: the first row
    ok(ex, "SELECT COUNT(*), MAX(v) INTO @n, @m FROM t");
    REQUIRE(q(ex, "SELECT @n, @m") == Rows{{"3", "30"}});
    ok(ex, "SELECT v INTO @none FROM t WHERE id = 99");
    REQUIRE(q(ex, "SELECT @none") == Rows{{N}});
    REQUIRE(q(ex, "SELECT @never") == Rows{{N}});
    // an OUT parameter that the procedure leaves alone is NULL
    ok(ex, "CREATE PROCEDURE p_o3(OUT c INT) BEGIN SET @touched = 1; END");
    ok(ex, "CALL p_o3(@z)");
    REQUIRE(q(ex, "SELECT @z, @touched") == Rows{{N, "1"}});
}

TEST_CASE("@variables are values in the statements of a session", "[routines][uservars]") {
    TempDataDir dir("sr_uservars");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    ok(ex, "SET @x = 3");
    ok(ex, "SET @y = 4");
    REQUIRE(q(ex, "SELECT id, v FROM t WHERE id = @x") == Rows{{"3", "30"}});
    REQUIRE(q(ex, "SELECT id FROM t WHERE v > @x * 5 ORDER BY id") == Rows{{"2"}, {"3"}});
    ok(ex, "UPDATE t SET v = @x WHERE id = 1");
    ok(ex, "INSERT INTO lg VALUES (@x, 'user var', @y)");
    REQUIRE(q(ex, "SELECT id, v FROM t WHERE id = 1") == Rows{{"1", "3"}});
    REQUIRE(q(ex, "SELECT id, note, v FROM lg") == Rows{{"3", "user var", "4"}});
    REQUIRE(q(ex, "SELECT @x + id FROM t ORDER BY id").size() == 3);
    REQUIRE(q(ex, "SELECT @x + id FROM t ORDER BY id") == Rows{{"4"}, {"5"}, {"6"}});
    // the same text asks for other rows after SET: an answer is not served from the cache
    ok(ex, "SET @x = 2");
    REQUIRE(q(ex, "SELECT id, v FROM t WHERE id = @x") == Rows{{"2", "20"}});
    ok(ex, "SET @x = 1");
    REQUIRE(q(ex, "SELECT id, v FROM t WHERE id = @x") == Rows{{"1", "3"}});
    // NULL is NULL: `v = @n` and `v <> @n` keep no row
    ok(ex, "SET @n = NULL");
    REQUIRE(q(ex, "SELECT id FROM t WHERE v = @n").empty());
    REQUIRE(q(ex, "SELECT id FROM t WHERE v <> @n").empty());
    REQUIRE(q(ex, "SELECT id FROM t WHERE v IN (@x, 20) ORDER BY id") == Rows{{"2"}});
    REQUIRE(q(ex, "SELECT id FROM t WHERE v BETWEEN @x AND 20 ORDER BY id") == Rows{{"1"}, {"2"}});
    // a variable set from another, and from a query
    ok(ex, "SET @a = 5");
    ok(ex, "SET @b = @a + 1");
    REQUIRE(q(ex, "SELECT @b") == Rows{{"6"}});
    ok(ex, "SET @c = (SELECT COUNT(*) FROM t)");
    ok(ex, "SET @m = (SELECT MAX(v) FROM t WHERE id < @c)");
    REQUIRE(q(ex, "SELECT @c, @m") == Rows{{"3", "20"}});
    ok(ex, "SET @s = 'hello'");
    REQUIRE(q(ex, "SELECT CONCAT(@s, ' world')") == Rows{{"hello world"}});
    REQUIRE(q(ex, "SELECT UPPER(@s)") == Rows{{"HELLO"}});
    ok(ex, "SET @n = 7");
    REQUIRE(q(ex, "SELECT ROUND(@n / 2)") == Rows{{"4"}}); // a variable inside the expression that is the argument
    REQUIRE(q(ex, "SELECT id, ROUND(@n / 2) FROM t WHERE id = 1") == Rows{{"1", "4"}}); // (over a table, a row has no variables of its own)
    REQUIRE(q(ex, "SELECT CONCAT(id, @s) FROM t WHERE id = 1") == Rows{{"1hello"}});
}

TEST_CASE("a user-defined function takes typed or untyped parameters", "[routines][function]") {
    TempDataDir dir("sr_function");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    ok(ex, "CREATE FUNCTION dbl(x INT) RETURNS INT RETURN x * 2");
    REQUIRE(q(ex, "SELECT dbl(21)") == Rows{{"42"}});
    REQUIRE(q(ex, "SELECT id, dbl(v) FROM t ORDER BY id") == Rows{{"1", "20"}, {"2", "40"}, {"3", "60"}});
    REQUIRE(q(ex, "SELECT id FROM t WHERE dbl(id) = 4") == Rows{{"2"}});
    ok(ex, "UPDATE t SET v = dbl(v) WHERE id = 1");
    REQUIRE(q(ex, "SELECT v FROM t WHERE id = 1") == Rows{{"20"}});
    ok(ex, "CREATE FUNCTION add_ratio(a DECIMAL(10, 2), b VARCHAR(5)) RETURNS DECIMAL(10, 2) RETURN a + b");
    REQUIRE(q(ex, "SELECT add_ratio(1.5, 2)") == Rows{{"3.5"}});
    ok(ex, "CREATE FUNCTION half(x) RETURNS DECIMAL RETURN x / 2"); // the older spelling without types
    REQUIRE(q(ex, "SELECT half(9)") == Rows{{"4.5"}});
}

TEST_CASE("a trigger runs once for each row, with the row's NEW and OLD", "[routines][trigger]") {
    TempDataDir dir("sr_trigger");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    ok(ex, "CREATE TRIGGER trg_ai AFTER INSERT ON t FOR EACH ROW INSERT INTO lg VALUES (NEW.id, 'ins', NEW.v)");
    ok(ex, "INSERT INTO t VALUES (8, 80), (9, 90)");
    REQUIRE(q(ex, "SELECT id, note, v FROM lg ORDER BY id") == Rows{{"8", "ins", "80"}, {"9", "ins", "90"}});
    ok(ex, "CREATE TRIGGER trg_au AFTER UPDATE ON t FOR EACH ROW INSERT INTO lg VALUES (OLD.id, 'upd', NEW.v)");
    ok(ex, "UPDATE t SET v = v + 1 WHERE id < 3");
    REQUIRE(q(ex, "SELECT id, v FROM lg WHERE note = 'upd' ORDER BY id") == Rows{{"1", "11"}, {"2", "21"}});
    // (the versions an UPDATE left behind are not rows: the next UPDATE of a row runs the trigger once)
    ok(ex, "UPDATE t SET v = v + 1 WHERE id = 1");
    REQUIRE(q(ex, "SELECT id, v FROM lg WHERE note = 'upd' AND id = 1 ORDER BY v") == Rows{{"1", "11"}, {"1", "12"}});
    ok(ex, "CREATE TRIGGER trg_ad AFTER DELETE ON t FOR EACH ROW INSERT INTO lg VALUES (OLD.id, 'del', OLD.v)");
    ok(ex, "DELETE FROM t WHERE id >= 8");
    REQUIRE(q(ex, "SELECT id, v FROM lg WHERE note = 'del' ORDER BY id") == Rows{{"8", "80"}, {"9", "90"}});
    // BEFORE: NEW is what the statement names; a column it leaves out has no value yet
    ok(ex, "CREATE TRIGGER trg_bi BEFORE INSERT ON t FOR EACH ROW INSERT INTO lg VALUES (NEW.id, 'before', NEW.v)");
    ok(ex, "INSERT INTO t VALUES (20, 200)");
    REQUIRE(q(ex, "SELECT id, v FROM lg WHERE note = 'before'") == Rows{{"20", "200"}});
    ok(ex, "CREATE TRIGGER trg_bu BEFORE UPDATE ON t FOR EACH ROW INSERT INTO lg VALUES (OLD.id, 'before-upd', NEW.v)");
    ok(ex, "UPDATE t SET v = v + 5 WHERE id = 3");
    REQUIRE(q(ex, "SELECT id, v FROM lg WHERE note = 'before-upd'") == Rows{{"3", "35"}});
    // lower case, and a trigger of another table or event is not the one that runs
    ok(ex, "CREATE TABLE other (id INT PRIMARY KEY)");
    ok(ex, "CREATE TRIGGER trg_low AFTER INSERT ON other FOR EACH ROW INSERT INTO lg VALUES (new.id, 'low', 0)");
    ok(ex, "INSERT INTO other VALUES (5), (6), (7)");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM lg WHERE note = 'low'") == Rows{{"3"}});
    // a statement that touches no row runs no trigger, and a multi-row one runs it once per row
    const Rows before = q(ex, "SELECT COUNT(*) FROM lg");
    ok(ex, "UPDATE t SET v = 0 WHERE id = 12345");
    ok(ex, "DELETE FROM t WHERE id = 12345");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM lg") == before);
    // two triggers of the same event both run
    ok(ex, "CREATE TRIGGER trg_ai2 AFTER INSERT ON other FOR EACH ROW INSERT INTO lg VALUES (NEW.id, 'second', 0)");
    ok(ex, "INSERT INTO other VALUES (8)");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM lg WHERE id = 8 AND note IN ('low', 'second')") == Rows{{"2"}});
}

TEST_CASE("a trigger that fails fails the statement that fired it", "[routines][trigger][failure]") {
    TempDataDir dir("sr_trigger_fail");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    // BEFORE: nothing was written
    ok(ex, "CREATE TRIGGER trg_bad BEFORE INSERT ON t FOR EACH ROW INSERT INTO no_such_table VALUES (1)");
    const std::string message = fails(ex, "INSERT INTO t VALUES (4, 40)");
    REQUIRE(message.find("Trigger 'trg_bad' failed") != std::string::npos);
    REQUIRE(message.find("no_such_table") != std::string::npos);
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t") == Rows{{"3"}});
    ok(ex, "DROP TRIGGER trg_bad");
    // AFTER: the error says the change was made (a transaction can roll it back)
    ok(ex, "CREATE TRIGGER trg_bad2 AFTER INSERT ON t FOR EACH ROW INSERT INTO no_such_table VALUES (1)");
    REQUIRE(fails(ex, "INSERT INTO t VALUES (5, 50)").find("after the change was made") != std::string::npos);
    ok(ex, "DROP TRIGGER trg_bad2");
    ok(ex, "BEGIN");
    ok(ex, "CREATE TRIGGER trg_bad3 AFTER INSERT ON t FOR EACH ROW INSERT INTO no_such_table VALUES (1)");
    fails(ex, "INSERT INTO t VALUES (6, 60)");
    ok(ex, "ROLLBACK");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t WHERE id = 6") == Rows{{"0"}});
    ok(ex, "DROP TRIGGER trg_bad3");
    // a trigger that fires itself runs into the depth cap instead of a stack overflow
    ok(ex, "CREATE TRIGGER trg_loop AFTER INSERT ON lg FOR EACH ROW INSERT INTO lg VALUES (NEW.id, 'loop', 0)");
    REQUIRE(fails(ex, "INSERT INTO lg VALUES (1, 'start', 0)").find("recursion") != std::string::npos);
}

TEST_CASE("a BEFORE INSERT trigger may set NEW.x", "[routines][trigger][set_new]") {
    TempDataDir dir("sr_set_new");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, v INT, w INT, note VARCHAR(10))");
    ok(ex, "CREATE TRIGGER trg_set BEFORE INSERT ON t FOR EACH ROW BEGIN SET NEW.v = NEW.v * 2; SET NEW.w = NEW.id + 100; END");
    ok(ex, "INSERT INTO t (id, v, note) VALUES (1, 5, 'a'), (2, 7, 'b')");
    REQUIRE(q(ex, "SELECT id, v, w, note FROM t ORDER BY id") == Rows{{"1", "10", "101", "a"}, {"2", "14", "102", "b"}});
    ok(ex, "INSERT INTO t VALUES (3, 1, 0, 'c')");
    REQUIRE(q(ex, "SELECT id, v, w FROM t WHERE id = 3") == Rows{{"3", "2", "103"}});
    // in a BEFORE UPDATE / AFTER trigger it is refused, not ignored
    ok(ex, "CREATE TRIGGER trg_bu BEFORE UPDATE ON t FOR EACH ROW SET NEW.v = 0");
    REQUIRE(fails(ex, "UPDATE t SET w = 1 WHERE id = 1").find("BEFORE INSERT triggers only") != std::string::npos);
    REQUIRE(q(ex, "SELECT w FROM t WHERE id = 1") == Rows{{"101"}});
}

TEST_CASE("procedures and triggers survive a restart", "[routines][persistence]") {
    TempDataDir dir("sr_persist");
    {
        Executor ex(dir.path);
        open_db(ex);
        seed(ex);
        ok(ex, "CREATE PROCEDURE p_nest(IN a INT) BEGIN CALL p_ins(a); CALL p_ins(a + 10); SELECT COUNT(*) INTO @n FROM t; END");
        ok(ex, "CREATE PROCEDURE p_ins(IN x INT) BEGIN INSERT INTO lg VALUES (x, 'p', 0); END");
        ok(ex, "CREATE TRIGGER trg_ai AFTER INSERT ON t FOR EACH ROW INSERT INTO lg VALUES (NEW.id, 'ins', NEW.v)");
        ok(ex, "CREATE TRIGGER trg_set BEFORE INSERT ON t FOR EACH ROW SET NEW.v = NEW.v + 1");
    }
    Executor ex(dir.path);
    ok(ex, "USE d");
    ok(ex, "CALL p_nest(1)");
    REQUIRE(q(ex, "SELECT id FROM lg WHERE note = 'p' ORDER BY id") == Rows{{"1"}, {"11"}});
    REQUIRE(q(ex, "SELECT @n") == Rows{{"3"}});
    ok(ex, "INSERT INTO t VALUES (9, 90)");
    REQUIRE(q(ex, "SELECT id, v FROM lg WHERE note = 'ins'") == Rows{{"9", "91"}});
    REQUIRE(q(ex, "SELECT v FROM t WHERE id = 9") == Rows{{"91"}});
}
