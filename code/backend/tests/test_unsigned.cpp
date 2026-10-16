#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// TINYINT / SMALLINT / INT / BIGINT UNSIGNED keep the unsigned range: 0 up to 255 / 65535 / 4294967295 / 18446744073709551615, and refuse a negative value
// (MySQL: error 1264 "Out of range value"). The signed types keep theirs, including the smallest BIGINT.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

bool ok(Executor& ex, const std::string& sql) { return ex.execute_sql(sql).is_ok(); }

// the cells of the first data row of a SELECT, joined with ","
std::string first_row(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars < 2) continue; // the header
        std::string out, cell;
        std::istringstream cs(line);
        while (std::getline(cs, cell, '|')) {
            cell.erase(0, cell.find_first_not_of(" \t\r"));
            cell.erase(cell.find_last_not_of(" \t\r") + 1);
            if (cell.empty()) continue;
            out += (out.empty() ? "" : ",") + cell;
        }
        return out;
    }
    return "";
}
} // namespace

TEST_CASE("UNSIGNED integer columns keep their range", "[unsigned]") {
    TempDataDir dir("unsigned_ranges");
    {
        Executor ex(dir.path);
        REQUIRE(ok(ex, "CREATE DATABASE d"));
        REQUIRE(ok(ex, "USE d"));
        REQUIRE(ok(ex, "CREATE TABLE u (id INT PRIMARY KEY, t TINYINT UNSIGNED, s SMALLINT UNSIGNED, i INT UNSIGNED, b BIGINT UNSIGNED, n INT, nb BIGINT)"));

        // the edges that are allowed
        REQUIRE(ok(ex, "INSERT INTO u VALUES (1, 255, 65535, 4294967295, 18446744073709551615, -2147483648, -9223372036854775808)"));
        REQUIRE(ok(ex, "INSERT INTO u VALUES (2, 0, 0, 0, 0, 2147483647, 9223372036854775807)"));
        REQUIRE(ok(ex, "INSERT INTO u (id, b) VALUES (3, 9223372036854775808)")); // above the largest signed BIGINT
        REQUIRE(first_row(ex, "SELECT t, s, i, b FROM u WHERE id = 1") == "255,65535,4294967295,18446744073709551615");
        REQUIRE(first_row(ex, "SELECT n, nb FROM u WHERE id = 1") == "-2147483648,-9223372036854775808");
        REQUIRE(first_row(ex, "SELECT b FROM u WHERE id = 3") == "9223372036854775808");

        // one past an edge, a negative value for an unsigned column, a word
        const std::vector<std::string> refused = {
            "INSERT INTO u (id, t) VALUES (10, 256)", "INSERT INTO u (id, t) VALUES (10, -1)", "INSERT INTO u (id, s) VALUES (10, 65536)",
            "INSERT INTO u (id, s) VALUES (10, -1)", "INSERT INTO u (id, i) VALUES (10, 4294967296)", "INSERT INTO u (id, i) VALUES (10, -1)",
            "INSERT INTO u (id, b) VALUES (10, 18446744073709551616)", "INSERT INTO u (id, b) VALUES (10, -1)",
            "INSERT INTO u (id, n) VALUES (10, 2147483648)", "INSERT INTO u (id, n) VALUES (10, -2147483649)",
            "INSERT INTO u (id, nb) VALUES (10, 9223372036854775808)", "INSERT INTO u (id, nb) VALUES (10, -9223372036854775809)",
            "INSERT INTO u (id, i) VALUES (10, 1 - 2)", // an expression that comes out negative
            "INSERT INTO u (id, i) VALUES (10, 4294967295 + 1)",
            "INSERT INTO u (id, i) VALUES (10, -0.4)", // (it would round to 0, but MySQL refuses it for an unsigned column)
        };
        for (auto& sql : refused) {
            INFO(sql);
            auto r = ex.execute_sql(sql);
            REQUIRE(r.is_err());
            REQUIRE(r.error().find("Out of range") != std::string::npos);
        }
        REQUIRE(ex.execute_sql("INSERT INTO u (id, i) VALUES (10, 'abc')").is_err()); // (not a number: another message)
        REQUIRE(first_row(ex, "SELECT COUNT(*) FROM u") == "3");

        // values above the largest signed BIGINT compare exactly
        REQUIRE(first_row(ex, "SELECT COUNT(*) FROM u WHERE b > 9223372036854775807") == "2");
        REQUIRE(first_row(ex, "SELECT 9223372036854775808 > 9223372036854775807, 18446744073709551615 > 18446744073709551614") == "1,1");
        REQUIRE(first_row(ex, "SELECT MAX(b) FROM u") == "18446744073709551615");

        // an UPDATE that would leave the range changes nothing
        REQUIRE(ex.execute_sql("UPDATE u SET i = i - 5 WHERE id = 2").is_err());
        REQUIRE(ex.execute_sql("UPDATE u SET i = i + 1 WHERE id = 1").is_err());
        REQUIRE(ex.execute_sql("UPDATE u SET t = t * 2 WHERE id = 1").is_err());
        REQUIRE(first_row(ex, "SELECT i FROM u WHERE id = 2") == "0");
        REQUIRE(first_row(ex, "SELECT i FROM u WHERE id = 1") == "4294967295");
        REQUIRE(ok(ex, "UPDATE u SET i = i - 5 WHERE id = 1"));
        REQUIRE(first_row(ex, "SELECT i FROM u WHERE id = 1") == "4294967290");

        // how the type is shown
        auto create = ex.execute_sql("SHOW CREATE TABLE u");
        REQUIRE(create.is_ok());
        REQUIRE(create.value().find("TINYINT UNSIGNED") != std::string::npos);
        REQUIRE(create.value().find("BIGINT UNSIGNED") != std::string::npos);
        REQUIRE(create.value().find("`n` INT,") != std::string::npos); // a signed column has no UNSIGNED after it
        auto desc = ex.execute_sql("DESCRIBE u");
        REQUIRE(desc.is_ok());
        REQUIRE(desc.value().find("UNSIGNED") != std::string::npos);
        REQUIRE(first_row(ex, "SELECT column_type FROM information_schema.columns WHERE table_schema = 'd' AND table_name = 'u' AND column_name = 'i'") == "int unsigned");
        REQUIRE(first_row(ex, "SELECT column_type FROM information_schema.columns WHERE table_schema = 'd' AND table_name = 'u' AND column_name = 'n'") == "int");

        // ALTER TABLE: ADD COLUMN takes UNSIGNED; MODIFY COLUMN to UNSIGNED is refused while a value is negative
        REQUIRE(ok(ex, "ALTER TABLE u ADD COLUMN extra SMALLINT UNSIGNED"));
        REQUIRE(ex.execute_sql("INSERT INTO u (id, extra) VALUES (11, -1)").is_err());
        REQUIRE(ok(ex, "INSERT INTO u (id, extra) VALUES (11, 65535)"));
        REQUIRE(ex.execute_sql("ALTER TABLE u MODIFY COLUMN n INT UNSIGNED").is_err()); // n holds -2147483648
        REQUIRE(ok(ex, "UPDATE u SET n = 7 WHERE id = 1"));
        REQUIRE(ok(ex, "ALTER TABLE u MODIFY COLUMN n INT UNSIGNED"));
        REQUIRE(ex.execute_sql("INSERT INTO u (id, n) VALUES (12, -1)").is_err());
        REQUIRE(ok(ex, "INSERT INTO u (id, n) VALUES (12, 4000000000)"));

        // a BACKUP keeps UNSIGNED
        auto backup = ex.execute_sql("BACKUP DATABASE d INTO 'unsigned_backup.sql'");
        REQUIRE(backup.is_ok());
        std::ifstream in(dir.path + "/_backups/unsigned_backup.sql");
        std::stringstream text;
        text << in.rdbuf();
        REQUIRE(text.str().find("INT UNSIGNED") != std::string::npos);
    }

    // after a restart the columns are still unsigned
    Executor ex(dir.path);
    REQUIRE(ok(ex, "USE d"));
    REQUIRE(ex.execute_sql("INSERT INTO u (id, t) VALUES (20, -1)").is_err());
    REQUIRE(ex.execute_sql("INSERT INTO u (id, i) VALUES (20, 4294967296)").is_err());
    REQUIRE(ok(ex, "INSERT INTO u (id, i) VALUES (20, 4294967295)"));
    REQUIRE(first_row(ex, "SELECT column_type FROM information_schema.columns WHERE table_schema = 'd' AND table_name = 'u' AND column_name = 'b'") == "bigint unsigned");
}
