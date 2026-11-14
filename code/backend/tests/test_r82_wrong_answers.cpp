// R82: the answers that were wrong without an error (RECOMMAND.md, the "R81" rows): hexadecimal and bit literals kept as the digits they were written with, COMMENT
// accepted and dropped, a LATERAL derived table missing from `*`, SHA2 of a length other than 256 giving NULL, and MEDIUMINT / TINYTEXT / MEDIUMTEXT / LONGTEXT /
// TINYBLOB / MEDIUMBLOB / LONGBLOB / CHAR(n) stored as INT / TEXT / BLOB / VARCHAR(n). The answers were measured on MySQL 8.4;
// code/test/diff/corpus/{tool_gaps,wrong_answers_r82}.sql repeat them against the server.

#include <filesystem>
#include <sstream>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/parser/lexer.hpp"

using namespace engine;
namespace fs = std::filesystem;

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

void ok(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
}

// the first cell of every row of an answer, in order
std::string first_column(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line, out;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue; // (the header)
        std::size_t end = line.find('|', 1);
        std::string cell = line.substr(1, end - 1);
        const std::size_t a = cell.find_first_not_of(' '), b = cell.find_last_not_of(' ');
        cell = a == std::string::npos ? std::string() : cell.substr(a, b - a + 1);
        out += (out.empty() ? "" : ",") + cell;
    }
    return out;
}

// every cell of the first row
std::vector<std::string> first_row(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line;
    int bars = 0;
    std::vector<std::string> cells;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars != 2) continue;
        std::size_t from = 1;
        while (from < line.size()) {
            std::size_t end = line.find('|', from);
            if (end == std::string::npos) break;
            std::string cell = line.substr(from, end - from);
            const std::size_t a = cell.find_first_not_of(' '), b = cell.find_last_not_of(' ');
            cells.push_back(a == std::string::npos ? std::string() : cell.substr(a, b - a + 1));
            from = end + 1;
        }
    }
    return cells;
}

std::string header_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '|') return line;
    }
    return "";
}

// the text of the error of a statement, "" when it goes through
std::string error_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    return r.is_ok() ? std::string() : r.error();
}

bool says(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

std::string text_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    return r.value();
}

struct World {
    TempDataDir dir;
    Executor ex;
    explicit World(const std::string& name) : dir(name), ex(dir.path) {
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
    }
};
} // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// hexadecimal and bit literals
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("[r82] the lexer reads 0x.., X'..', 0b.., b'..' as the bytes they spell, and drops a character set introducer", "[r82][lexer]") {
    auto lex_all = [](const std::string& sql) { return Lexer(sql).tokenize(); };
    auto one = [&](const std::string& sql) {
        auto tokens = lex_all("SELECT " + sql);
        REQUIRE(tokens.size() == 2);
        return tokens[1];
    };
    CHECK(one("0x4142").kind == TokenKind::StringLit);
    CHECK(one("0x4142").text == "AB");
    CHECK(one("0x4142").word == "hex");
    CHECK(one("0X41").text == "A");
    CHECK(one("0x1A").text == std::string("\x1A"));
    CHECK(one("0x123").text == std::string("\x01\x23", 2)); // (an odd number of digits has a 0 in front)
    CHECK(one("X'41'").text == "A");
    CHECK(one("x'4142'").text == "AB");
    CHECK(one("X''").text == "");
    CHECK(one("b'101'").text == std::string("\x05"));
    CHECK(one("B'1000001'").text == "A");
    CHECK(one("0b101").text == std::string("\x05"));
    CHECK(one("0b101").word == "hex");
    CHECK(one("b'100000000'").text == std::string("\x01\x00", 2)); // (nine bits are two bytes)
    CHECK(one("N'abc'").kind == TokenKind::StringLit);
    CHECK(one("N'abc'").text == "abc");
    CHECK(one("N'abc'").word.empty());
    // the introducer says nothing here
    CHECK(one("_utf8mb4'abc'").text == "abc");
    CHECK(one("_binary 'abc'").text == "abc");
    CHECK(one("_latin1 'abc'").text == "abc");
    CHECK(one("_binary 0x4142").text == "AB");
    CHECK(one("_binary X'4142'").text == "AB");
    // things that are not literals stay what they were
    CHECK(lex_all("SELECT 0x").size() == 3);                // (0, then the name x)
    CHECK(lex_all("SELECT 0x")[1].kind == TokenKind::NumberLit);
    CHECK(lex_all("SELECT 0b102")[1].kind == TokenKind::NumberLit); // (0b102 is no bit literal)
    CHECK(lex_all("SELECT 0b102")[2].kind == TokenKind::Ident);
    CHECK(Lexer("SELECT x FROM t").tokenize()[1].kind == TokenKind::Ident);
    CHECK(Lexer("SELECT _utf8mb4 FROM t").tokenize()[1].kind == TokenKind::Ident); // (a name, not before a string)
    CHECK(Lexer("SELECT '0x41'").tokenize()[1].word.empty());
}

TEST_CASE("[r82] a hexadecimal literal is its bytes as a string and its integer where a number is asked", "[r82][hex]") {
    World w("r82_hex");
    Executor& ex = w.ex;
    CHECK(first_column(ex, "SELECT 0x41") == "A");
    CHECK(first_column(ex, "SELECT 0x4142") == "AB");
    CHECK(first_column(ex, "SELECT X'4142'") == "AB");
    CHECK(first_column(ex, "SELECT 0x41 + 0") == "65");
    CHECK(first_column(ex, "SELECT 0x4142 + 0") == "16706");
    CHECK(first_column(ex, "SELECT 0x10 * 2") == "32");
    CHECK(first_column(ex, "SELECT 0x01 + 1") == "2");
    CHECK(first_column(ex, "SELECT b'101' + 0") == "5");
    CHECK(first_column(ex, "SELECT 0b101 + 0") == "5");
    CHECK(first_column(ex, "SELECT HEX(0x4142)") == "4142");
    CHECK(first_column(ex, "SELECT HEX(X'41')") == "41");
    CHECK(first_column(ex, "SELECT LENGTH(0x4142)") == "2");
    CHECK(first_column(ex, "SELECT CONCAT('x', 0x41)") == "xA");
    CHECK(first_column(ex, "SELECT UPPER(_utf8mb4'abc')") == "ABC");
    CHECK(first_column(ex, "SELECT 'a' = _binary'a'") == "1");
    CHECK(first_column(ex, "SELECT '0x41' + 0") == "0"); // (a quoted one is the text it is)

    ok(ex, "CREATE TABLE bl (id INT PRIMARY KEY, b BLOB, s VARCHAR(10))");
    ok(ex, "INSERT INTO bl VALUES (1, 0x4142, 0x41)");
    ok(ex, "INSERT INTO bl VALUES (2, _binary'xy', N'z')");
    ok(ex, "INSERT INTO bl VALUES (3, X'0141', 0x01)"); // (a value that begins with the byte 1 is how an expression is written: this one is not)
    CHECK(first_column(ex, "SELECT HEX(b) FROM bl ORDER BY id") == "4142,7879,0141");
    CHECK(first_column(ex, "SELECT s FROM bl WHERE id < 3 ORDER BY id") == "A,z");
    CHECK(first_column(ex, "SELECT HEX(s) FROM bl WHERE id = 3") == "01");
    CHECK(first_column(ex, "SELECT LENGTH(b) FROM bl ORDER BY id") == "2,2,2");
    CHECK(first_column(ex, "SELECT id FROM bl WHERE b = 0x4142") == "1");
    CHECK(first_column(ex, "SELECT id FROM bl WHERE s = 'A'") == "1"); // (and a string with the same bytes is the same value)
    ok(ex, "UPDATE bl SET s = 0x42 WHERE id = 1");
    CHECK(first_column(ex, "SELECT s FROM bl WHERE id = 1") == "B");
    ok(ex, "INSERT INTO bl (id, s) VALUES (4, 0x4142 + 0)"); // (an expression with a hexadecimal literal in it)
    CHECK(first_column(ex, "SELECT s FROM bl WHERE id = 4") == "16706");

    // a number meets a hexadecimal literal: the integer it spells (a text meets it: the bytes it is)
    ok(ex, "CREATE TABLE hn (id INT PRIMARY KEY, a INT, s VARCHAR(10))");
    ok(ex, "INSERT INTO hn VALUES (1, 1, 'A'), (2, 65, 'B'), (3, 16706, 'AB')");
    CHECK(first_row(ex, "SELECT 0x01 = 1, 0x41 = 65, 0x41 = 'A', 0x41 > 64") == std::vector<std::string>({"1", "1", "1", "1"}));
    CHECK(first_row(ex, "SELECT 65 = 0x41, 'A' = 0x41, 0x4142 = 'AB', 0x41 = 0x41") == std::vector<std::string>({"1", "1", "1", "1"}));
    CHECK(first_column(ex, "SELECT id FROM hn WHERE a = 0x01") == "1");
    CHECK(first_column(ex, "SELECT id FROM hn WHERE a = 0x41") == "2");
    CHECK(first_column(ex, "SELECT id FROM hn WHERE a = 0x4142") == "3");
    CHECK(first_column(ex, "SELECT id FROM hn WHERE 0x41 = a") == "2");
    CHECK(first_column(ex, "SELECT id FROM hn WHERE a > 0x40 ORDER BY id") == "2,3");
    CHECK(first_column(ex, "SELECT id FROM hn WHERE s = 0x41") == "1"); // (a text column: the bytes)
    CHECK(first_column(ex, "SELECT CAST(0x41 AS UNSIGNED)") == "65");
    CHECK(first_column(ex, "SELECT CAST(0x4142 AS SIGNED)") == "16706");
    CHECK(first_column(ex, "SELECT CAST(0x41 AS CHAR)") == "A");
    // the column is named as the literal is written (the bytes need not be text: 0xFF is no UTF-8)
    CHECK(says(header_of(ex, "SELECT 0x4142 + 0"), "0x4142 + 0"));
    CHECK(first_column(ex, "SELECT 0xFF + 0") == "255");
    CHECK(says(header_of(ex, "SELECT 0xFF + 0"), "0xFF + 0"));
    // ... and so is the column of a derived table made of it
    CHECK(first_column(ex, "SELECT * FROM (SELECT 0xFF + 0) d") == "255");
    CHECK(says(header_of(ex, "SELECT * FROM (SELECT 0x4142 + 0) d"), "0x4142"));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// COMMENT
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("[r82] COMMENT of a column and of a table is kept, shown, copied and survives a restart", "[r82][comment]") {
    TempDataDir dir("r82_comment");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
        ok(ex, "CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL COMMENT 'it''s a name') COMMENT='parents'");
        ok(ex, "CREATE TABLE chi (id INT PRIMARY KEY AUTO_INCREMENT, qty INT DEFAULT 1 COMMENT 'amount\\nsecond line \\\\ done', pid INT, FOREIGN KEY (pid) REFERENCES par (id)) COMMENT = 'kids'");
        CHECK(first_column(ex, "SELECT table_comment FROM information_schema.tables WHERE table_schema = 'v' AND table_name = 'chi'") == "kids");
        CHECK(first_column(ex, "SELECT table_comment FROM information_schema.tables WHERE table_schema = 'v' AND table_name = 'par'") == "parents");
        CHECK(first_column(ex, "SELECT column_comment FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'par' AND column_name = 'name'") == "it's a name");
        CHECK(first_column(ex, "SELECT column_comment = 'amount\\nsecond line \\\\ done' FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'chi' AND column_name = 'qty'") == "1");

        // SHOW CREATE TABLE writes it the way MySQL does (the MySQL protocol's form and the native one)
        auto mysql_form = ex.show_create_table_mysql("par");
        REQUIRE(mysql_form.is_ok());
        CHECK(says(mysql_form.value(), "`name` varchar(20) NOT NULL COMMENT 'it''s a name'"));
        CHECK(says(mysql_form.value(), "COLLATE=utf8mb4_0900_ai_ci COMMENT='parents'"));
        auto chi_form = ex.show_create_table_mysql("chi");
        REQUIRE(chi_form.is_ok());
        CHECK(says(chi_form.value(), "DEFAULT '1' COMMENT 'amount\\nsecond line \\\\ done'"));
        CHECK(says(chi_form.value(), "COMMENT='kids'"));
        const std::string native_form = text_of(ex, "SHOW CREATE TABLE par");
        CHECK(says(native_form, "COMMENT 'it''s a name'"));
        CHECK(says(native_form, ") COMMENT='parents'"));

        // the form it writes is read back as it is
        ok(ex, "CREATE TABLE par2 (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL COMMENT 'it''s a name') COMMENT='parents'");
        CHECK(first_column(ex, "SELECT column_comment FROM information_schema.columns WHERE table_name = 'par2' AND column_name = 'name'") == "it's a name");

        // ALTER TABLE: the table's comment, a column's comment (MODIFY without one clears it), ADD COLUMN, CHANGE, RENAME COLUMN
        ok(ex, "ALTER TABLE par COMMENT = 'people'");
        CHECK(first_column(ex, "SELECT table_comment FROM information_schema.tables WHERE table_name = 'par'") == "people");
        ok(ex, "ALTER TABLE par ADD COLUMN age INT COMMENT 'years'");
        CHECK(first_column(ex, "SELECT column_comment FROM information_schema.columns WHERE table_name = 'par' AND column_name = 'age'") == "years");
        ok(ex, "ALTER TABLE par MODIFY name VARCHAR(30) NOT NULL");
        CHECK(first_column(ex, "SELECT column_comment FROM information_schema.columns WHERE table_name = 'par' AND column_name = 'name'").empty());
        ok(ex, "ALTER TABLE par MODIFY age INT COMMENT 'age in years'");
        CHECK(first_column(ex, "SELECT column_comment FROM information_schema.columns WHERE table_name = 'par' AND column_name = 'age'") == "age in years");
        ok(ex, "ALTER TABLE par CHANGE age years INT COMMENT 'new name'");
        CHECK(first_column(ex, "SELECT column_comment FROM information_schema.columns WHERE table_name = 'par' AND column_name = 'years'") == "new name");
        ok(ex, "ALTER TABLE par RENAME COLUMN years TO yrs");
        CHECK(first_column(ex, "SELECT column_comment FROM information_schema.columns WHERE table_name = 'par' AND column_name = 'yrs'") == "new name");
        ok(ex, "ALTER TABLE par AUTO_INCREMENT = 5, COMMENT = 'both'"); // (two options in one clause are two actions)
        CHECK(first_column(ex, "SELECT table_comment FROM information_schema.tables WHERE table_name = 'par'") == "both");
        ok(ex, "ALTER TABLE par ADD COLUMN z INT, COMMENT = 'after a column'");
        CHECK(first_column(ex, "SELECT table_comment FROM information_schema.tables WHERE table_name = 'par'") == "after a column");
        ok(ex, "ALTER TABLE par ADD COLUMN z2 INT, AUTO_INCREMENT = 5, COMMENT = 'two options after a column'"); // (a clause of two options after another action)
        CHECK(first_column(ex, "SELECT table_comment FROM information_schema.tables WHERE table_name = 'par'") == "two options after a column");
        ok(ex, "ALTER TABLE par COMMENT = ''");
        CHECK(first_column(ex, "SELECT table_comment FROM information_schema.tables WHERE table_name = 'par'").empty());

        // CREATE TABLE LIKE copies both; CREATE TABLE IF NOT EXISTS of a table that is there changes nothing; a view's columns have none
        ok(ex, "CREATE TABLE chi2 LIKE chi");
        CHECK(first_column(ex, "SELECT table_comment FROM information_schema.tables WHERE table_name = 'chi2'") == "kids");
        CHECK(first_column(ex, "SELECT column_comment = 'amount\\nsecond line \\\\ done' FROM information_schema.columns WHERE table_name = 'chi2' AND column_name = 'qty'") == "1");
        ok(ex, "CREATE TABLE IF NOT EXISTS chi (id INT PRIMARY KEY) COMMENT='replaced?'");
        CHECK(first_column(ex, "SELECT table_comment FROM information_schema.tables WHERE table_name = 'chi'") == "kids");
        ok(ex, "CREATE VIEW vw AS SELECT id, qty FROM chi");
        CHECK(first_column(ex, "SELECT column_comment FROM information_schema.columns WHERE table_name = 'vw' AND column_name = 'qty'").empty());

        // BACKUP writes them and reading the backup back brings them back
        const std::string backup_file = "r82_comment_backup.sql"; (void)dir;
        ok(ex, "BACKUP DATABASE v INTO '" + backup_file + "'");
        ok(ex, "DROP VIEW vw");
        ok(ex, "DROP TABLE chi2");
        ok(ex, "DROP TABLE chi");
        ok(ex, "DROP TABLE par2");
        ok(ex, "DROP TABLE par");
        ok(ex, "RESTORE DATABASE v FROM '" + backup_file + "'");
        CHECK(first_column(ex, "SELECT table_comment FROM information_schema.tables WHERE table_name = 'chi'") == "kids");
        CHECK(first_column(ex, "SELECT column_comment = 'amount\\nsecond line \\\\ done' FROM information_schema.columns WHERE table_name = 'chi' AND column_name = 'qty'") == "1");
        CHECK(first_column(ex, "SELECT column_comment FROM information_schema.columns WHERE table_name = 'par2' AND column_name = 'name'") == "it's a name");
    }
    {
        Executor reopened(dir.path); // (a restart: the comments come back from the schema files)
        ok(reopened, "USE v");
        CHECK(first_column(reopened, "SELECT table_comment FROM information_schema.tables WHERE table_name = 'chi'") == "kids");
        CHECK(first_column(reopened, "SELECT column_comment = 'amount\\nsecond line \\\\ done' FROM information_schema.columns WHERE table_name = 'chi' AND column_name = 'qty'") == "1");
        CHECK(first_column(reopened, "SELECT column_comment FROM information_schema.columns WHERE table_name = 'par2' AND column_name = 'name'") == "it's a name");
        CHECK(first_column(reopened, "SELECT table_comment FROM information_schema.tables WHERE table_name = 'par2'") == "parents");
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// LATERAL
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("[r82] SELECT * over a LATERAL derived table lists its columns", "[r82][lateral]") {
    World w("r82_lateral");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20))");
    ok(ex, "CREATE TABLE chi (id INT PRIMARY KEY, pid INT)");
    ok(ex, "INSERT INTO par VALUES (1, 'a'), (2, 'b'), (3, 'c')");
    ok(ex, "INSERT INTO chi VALUES (1, 1), (2, 1), (3, 2)");
    const std::string lateral = "(SELECT COUNT(*) AS c FROM chi WHERE chi.pid = par.id) l";
    // the comma form, JOIN ... ON TRUE, CROSS JOIN: `*` is the columns of par, then the column of l
    for (const char* form : {"par, LATERAL ", "par JOIN LATERAL ", "par CROSS JOIN LATERAL "}) {
        const std::string tail = std::string(form).find("JOIN LATERAL") != std::string::npos && std::string(form).find("CROSS") == std::string::npos ? " ON TRUE" : "";
        const std::string sql = "SELECT * FROM " + std::string(form) + lateral + tail + " ORDER BY par.id";
        INFO(sql);
        CHECK(header_of(ex, sql).find("c") != std::string::npos);
        const auto row = first_row(ex, sql);
        REQUIRE(row.size() == 3);
        CHECK(row[0] == "1");
        CHECK(row[1] == "a");
        CHECK(row[2] == "2");
    }
    CHECK(first_column(ex, "SELECT l.c FROM par, LATERAL " + lateral + " ORDER BY par.id") == "2,1,0");
    const auto qualified = first_row(ex, "SELECT l.* FROM par, LATERAL " + lateral + " ORDER BY par.id");
    REQUIRE(qualified.size() == 1);
    CHECK(qualified[0] == "2");
    const auto both = first_row(ex, "SELECT par.*, l.* FROM par, LATERAL " + lateral + " ORDER BY par.id");
    REQUIRE(both.size() == 3);
    CHECK(both[2] == "2");
    // LEFT JOIN LATERAL keeps the rows the subquery finds nothing for, with NULL
    const std::string first_child = "(SELECT id AS cid FROM chi WHERE chi.pid = par.id ORDER BY id LIMIT 1) l";
    CHECK(first_column(ex, "SELECT cid FROM par LEFT JOIN LATERAL " + first_child + " ON TRUE ORDER BY par.id") == "1,3,NULL");
    const auto left_row = first_row(ex, "SELECT * FROM par LEFT JOIN LATERAL " + first_child + " ON TRUE WHERE par.id = 3");
    REQUIRE(left_row.size() == 3);
    CHECK(left_row[0] == "3");
    CHECK(left_row[2] == "NULL");
    // two lateral tables
    const auto two = first_row(ex, "SELECT * FROM par, LATERAL (SELECT 5 AS x) a, LATERAL (SELECT par.id * 10 AS y) b WHERE par.id = 2");
    REQUIRE(two.size() == 4);
    CHECK(two[2] == "5");
    CHECK(two[3] == "20");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// SHA2
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("[r82] SHA2 of 224, 256, 384, 512 bits (and 0) matches the standard digests; another length is NULL", "[r82][sha2]") {
    World w("r82_sha2");
    Executor& ex = w.ex;
    auto sha2 = [&](const std::string& text, const std::string& bits) { return first_column(ex, "SELECT SHA2('" + text + "', " + bits + ")"); };
    CHECK(sha2("abc", "224") == "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7");
    CHECK(sha2("abc", "256") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha2("abc", "0") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha2("abc", "384") == "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    CHECK(sha2("abc", "512") == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK(sha2("", "224") == "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f");
    CHECK(sha2("", "384") == "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b");
    CHECK(sha2("", "512") == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    // MySQL's own answers for 'a'
    CHECK(sha2("a", "224") == "abd37534c7d9a2efb9465de931cd7055ffdb8879563ae98078d6d6d5");
    CHECK(sha2("a", "384") == "54a59b9f22b0b80880d8427e548b7c23abd873486e1f035dce9cd697e85175033caa88e6d57bc35efae0b5afd3145f31");
    CHECK(sha2("a", "512") == "1f40fc92da241694750979ee6cf582f2d5d7d28e18335de05abc54d0560e0f5302860c652bf08d560252aa5e74210546f369fbbbce8c12cfc7957b2652fe9a75");
    // a message of several blocks (NIST: 56 bytes for SHA-224; 112 bytes for SHA-384 / SHA-512, which is two blocks of 128)
    CHECK(sha2("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "224") == "75388b16512776cc5dba5da1fd890150b0c6455cb4f58b1952522525");
    const std::string long_text = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    CHECK(sha2(long_text, "384") == "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039");
    CHECK(sha2(long_text, "512") == "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    // another length, or a NULL, is NULL
    CHECK(sha2("abc", "123") == "NULL");
    CHECK(sha2("abc", "1") == "NULL");
    CHECK(first_column(ex, "SELECT SHA2(NULL, 256)") == "NULL");
    CHECK(first_column(ex, "SELECT SHA2('abc', NULL)") == "NULL");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// MEDIUMINT, TINYTEXT .. LONGTEXT, TINYBLOB .. LONGBLOB, CHAR(n)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("[r82] MEDIUMINT keeps its range", "[r82][types]") {
    World w("r82_mediumint");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE m (id INT PRIMARY KEY AUTO_INCREMENT, a MEDIUMINT, b MEDIUMINT UNSIGNED, c INT3)");
    ok(ex, "INSERT INTO m (a, b, c) VALUES (8388607, 16777215, -8388608)");
    ok(ex, "INSERT INTO m (a, b) VALUES (-8388608, 0)");
    CHECK(says(error_of(ex, "INSERT INTO m (a) VALUES (8388608)"), "Out of range value for column 'a'"));
    CHECK(says(error_of(ex, "INSERT INTO m (a) VALUES (-8388609)"), "Out of range value for column 'a'"));
    CHECK(says(error_of(ex, "INSERT INTO m (b) VALUES (16777216)"), "Out of range value for column 'b'"));
    CHECK(says(error_of(ex, "INSERT INTO m (b) VALUES (-1)"), "Out of range value for column 'b'"));
    CHECK(says(error_of(ex, "INSERT INTO m (c) VALUES (8388608)"), "Out of range value for column 'c'"));
    CHECK(says(error_of(ex, "UPDATE m SET a = 9000000 WHERE id = 1"), "Out of range value for column 'a'"));
    CHECK(first_column(ex, "SELECT a FROM m ORDER BY id") == "8388607,-8388608");
    CHECK(first_column(ex, "SELECT data_type FROM information_schema.columns WHERE table_name = 'm' AND column_name IN ('a', 'b') ORDER BY column_name") == "mediumint,mediumint");
    CHECK(first_column(ex, "SELECT column_type FROM information_schema.columns WHERE table_name = 'm' AND column_name = 'b'") == "mediumint unsigned");
    CHECK(first_column(ex, "SELECT numeric_precision FROM information_schema.columns WHERE table_name = 'm' AND column_name = 'a'") == "7");
    auto shown = ex.show_create_table_mysql("m");
    REQUIRE(shown.is_ok());
    CHECK(says(shown.value(), "`a` mediumint DEFAULT NULL"));
    CHECK(says(shown.value(), "`b` mediumint unsigned DEFAULT NULL"));
    // INT is still INT; a column changed to MEDIUMINT is checked against the new range
    ok(ex, "CREATE TABLE n (id INT PRIMARY KEY, a INT)");
    ok(ex, "INSERT INTO n VALUES (1, 9000000)");
    CHECK(says(error_of(ex, "ALTER TABLE n MODIFY a MEDIUMINT"), "Out of range value for column 'a'"));
    ok(ex, "UPDATE n SET a = 100");
    ok(ex, "ALTER TABLE n MODIFY a MEDIUMINT");
    CHECK(says(error_of(ex, "INSERT INTO n VALUES (2, 9000000)"), "Out of range value for column 'a'"));
}

TEST_CASE("[r82] TEXT and BLOB hold as many bytes as their size allows", "[r82][types]") {
    World w("r82_textsizes");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, tt TINYTEXT, tx TEXT, mt MEDIUMTEXT, lt LONGTEXT, tb TINYBLOB, bb BLOB, mb MEDIUMBLOB, lb LONGBLOB)");
    auto insert_of = [&](const char* column, std::size_t length) { return "INSERT INTO t (" + std::string(column) + ") VALUES (REPEAT('x', " + std::to_string(length) + "))"; };
    ok(ex, insert_of("tt", 255));
    CHECK(says(error_of(ex, insert_of("tt", 256)), "Data too long for column 'tt'"));
    ok(ex, insert_of("tx", 65535));
    CHECK(says(error_of(ex, insert_of("tx", 65536)), "Data too long for column 'tx'"));
    ok(ex, insert_of("mt", 65536)); // (MEDIUMTEXT holds more than TEXT)
    ok(ex, insert_of("lt", 70000));
    ok(ex, insert_of("tb", 255));
    CHECK(says(error_of(ex, insert_of("tb", 256)), "Data too long for column 'tb'"));
    ok(ex, insert_of("bb", 65535));
    CHECK(says(error_of(ex, insert_of("bb", 65536)), "Data too long for column 'bb'"));
    ok(ex, insert_of("mb", 65536));
    ok(ex, insert_of("lb", 70000));
    CHECK(says(error_of(ex, "UPDATE t SET tt = REPEAT('y', 300) WHERE id = 1"), "Data too long for column 'tt'"));
    CHECK(first_column(ex, "SELECT LENGTH(tt) FROM t WHERE id = 1") == "255");
    // how the columns are called
    CHECK(first_column(ex, "SELECT data_type FROM information_schema.columns WHERE table_name = 't' AND ordinal_position > 1 ORDER BY ordinal_position") ==
          "tinytext,text,mediumtext,longtext,tinyblob,blob,mediumblob,longblob");
    CHECK(first_column(ex, "SELECT character_maximum_length FROM information_schema.columns WHERE table_name = 't' AND ordinal_position > 1 ORDER BY ordinal_position") ==
          "255,65535,16777215,4294967295,255,65535,16777215,4294967295");
    auto shown = ex.show_create_table_mysql("t");
    REQUIRE(shown.is_ok());
    for (const char* line : {"`tt` tinytext,", "`tx` text,", "`mt` mediumtext,", "`lt` longtext,", "`tb` tinyblob,", "`bb` blob,", "`mb` mediumblob,", "`lb` longblob"}) CHECK(says(shown.value(), line));
}

TEST_CASE("[r82] CHAR(n) does not keep the spaces at the end of a value", "[r82][types]") {
    TempDataDir dir("r82_char");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
        ok(ex, "CREATE TABLE c (id INT PRIMARY KEY AUTO_INCREMENT, a CHAR(5), b VARCHAR(5), d CHAR, e NCHAR(3), f CHARACTER VARYING(4), g CHARACTER(2), "
               "m MEDIUMINT UNSIGNED, tt TINYTEXT, lt LONGTEXT, mb MEDIUMBLOB)");
        ok(ex, "INSERT INTO c (a, b, d, e, g) VALUES ('ab  ', 'ab  ', 'x', 'xy ', 'q ')");
        CHECK(first_column(ex, "SELECT CHAR_LENGTH(a) FROM c") == "2");
        CHECK(first_column(ex, "SELECT CONCAT(a, '#') FROM c") == "ab#");
        CHECK(first_column(ex, "SELECT CHAR_LENGTH(b) FROM c") == "4"); // (VARCHAR keeps them)
        CHECK(first_column(ex, "SELECT CHAR_LENGTH(e) FROM c") == "2");
        CHECK(first_column(ex, "SELECT CHAR_LENGTH(g) FROM c") == "1");
        CHECK(first_column(ex, "SELECT a FROM c WHERE a = 'ab'") == "ab");
        CHECK(says(error_of(ex, "INSERT INTO c (a) VALUES ('abcdef')"), "Data too long for column 'a'"));
        ok(ex, "INSERT INTO c (a) VALUES ('abcde   ')"); // (spaces past the length are cut, as for VARCHAR)
        CHECK(says(error_of(ex, "INSERT INTO c (d) VALUES ('xy')"), "Data too long for column 'd'")); // (CHAR alone is CHAR(1))
        CHECK(says(error_of(ex, "INSERT INTO c (g) VALUES ('abc')"), "Data too long for column 'g'"));
        ok(ex, "INSERT INTO c (f) VALUES ('wxyz')");
        ok(ex, "UPDATE c SET a = 'zz   ' WHERE id = 1");
        CHECK(first_column(ex, "SELECT CHAR_LENGTH(a) FROM c WHERE id = 1") == "2");
        // how the columns are called
        CHECK(first_column(ex, "SELECT data_type FROM information_schema.columns WHERE table_name = 'c' AND ordinal_position > 1 ORDER BY ordinal_position") ==
              "char,varchar,char,char,varchar,char,mediumint,tinytext,longtext,mediumblob");
        CHECK(first_column(ex, "SELECT column_type FROM information_schema.columns WHERE table_name = 'c' AND ordinal_position > 1 ORDER BY ordinal_position") ==
              "char(5),varchar(5),char(1),char(3),varchar(4),char(2),mediumint unsigned,tinytext,longtext,mediumblob");
        auto shown = ex.show_create_table_mysql("c");
        REQUIRE(shown.is_ok());
        CHECK(says(shown.value(), "`a` char(5) DEFAULT NULL"));
        CHECK(says(shown.value(), "`b` varchar(5) DEFAULT NULL"));
        // a VARCHAR column changed to CHAR(n) loses the spaces its values end with
        ok(ex, "ALTER TABLE c MODIFY b CHAR(5)");
        CHECK(first_column(ex, "SELECT CHAR_LENGTH(b) FROM c WHERE id = 1") == "2");
    }
    {
        Executor reopened(dir.path); // (a restart: the types come back from the schema files)
        ok(reopened, "USE v");
        CHECK(first_column(reopened, "SELECT column_type FROM information_schema.columns WHERE table_name = 'c' AND ordinal_position > 1 ORDER BY ordinal_position") ==
              "char(5),char(5),char(1),char(3),varchar(4),char(2),mediumint unsigned,tinytext,longtext,mediumblob");
        ok(reopened, "INSERT INTO c (a) VALUES ('kk  ')");
        CHECK(first_column(reopened, "SELECT CHAR_LENGTH(a) FROM c ORDER BY id DESC LIMIT 1") == "2");
        // the limits are the ones of the types that were read back
        CHECK(says(error_of(reopened, "INSERT INTO c (m) VALUES (16777216)"), "Out of range value for column 'm'"));
        CHECK(says(error_of(reopened, "INSERT INTO c (tt) VALUES (REPEAT('x', 256))"), "Data too long for column 'tt'"));
        ok(reopened, "INSERT INTO c (lt) VALUES (REPEAT('x', 70000))");
    }
}

TEST_CASE("[r82] the new types are written in a backup and read back", "[r82][types]") {
    World w("r82_types_backup");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE ty (id INT PRIMARY KEY, m MEDIUMINT UNSIGNED, t TINYTEXT, c CHAR(4), mb MEDIUMBLOB)");
    ok(ex, "INSERT INTO ty VALUES (1, 16777215, 'tiny', 'ab  ', 0x4142)");
    const std::string backup_file = "r82_types_backup.sql";
    ok(ex, "BACKUP DATABASE v INTO '" + backup_file + "'");
    ok(ex, "DROP TABLE ty");
    ok(ex, "RESTORE DATABASE v FROM '" + backup_file + "'");
    CHECK(first_column(ex, "SELECT column_type FROM information_schema.columns WHERE table_name = 'ty' AND ordinal_position > 1 ORDER BY ordinal_position") ==
          "mediumint unsigned,tinytext,char(4),mediumblob");
    CHECK(first_column(ex, "SELECT CONCAT(m, ',', t, ',', c, ',', HEX(mb)) FROM ty") == "16777215,tiny,ab,4142");
}
