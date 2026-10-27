#include <filesystem>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Executor::result_columns says what type each column of a SELECT's answer has, for the MySQL protocol (a client that is told INT hands its application a
// number). A column of a table has the table's type, COUNT is a BIGINT, SUM / AVG follow the argument the way MySQL's do, an arithmetic result is "by value".

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

std::string describe(const ResultColumnType& t) {
    if (!t.column) return t.by_value ? "value" : "text";
    const ColumnDef& c = *t.column;
    std::string unsigned_mark = c.is_unsigned ? "u" : "";
    return std::visit(
        [&](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, DataType::TinyInt>) return unsigned_mark + "tinyint";
            else if constexpr (std::is_same_v<T, DataType::Boolean>) return "bool";
            else if constexpr (std::is_same_v<T, DataType::SmallInt>) return unsigned_mark + "smallint";
            else if constexpr (std::is_same_v<T, DataType::Int>) return unsigned_mark + "int";
            else if constexpr (std::is_same_v<T, DataType::BigInt>) return unsigned_mark + "bigint";
            else if constexpr (std::is_same_v<T, DataType::Float>) return "float";
            else if constexpr (std::is_same_v<T, DataType::Double>) return "double";
            else if constexpr (std::is_same_v<T, DataType::Decimal>) return "dec(" + std::to_string(alt.precision) + "," + std::to_string(alt.scale) + ")";
            else if constexpr (std::is_same_v<T, DataType::Date>) return "date";
            else if constexpr (std::is_same_v<T, DataType::DateTime>) return "datetime";
            else if constexpr (std::is_same_v<T, DataType::Timestamp>) return "timestamp";
            else if constexpr (std::is_same_v<T, DataType::Time>) return "time";
            else if constexpr (std::is_same_v<T, DataType::Year>) return "year";
            else return "text";
        },
        c.data_type.data);
}

// "int,dec(10,2),text": the types of the columns the SELECT answers with ("" when none can be told)
std::string types_of(Executor& ex, const std::string& sql) {
    std::string out;
    for (const ResultColumnType& t : ex.result_columns(sql)) out += (out.empty() ? "" : ",") + describe(t);
    return out;
}

void ok(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
}
} // namespace

TEST_CASE("result_columns types the columns of a SELECT", "[result_types]") {
    TempDataDir dir("result_types");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE ty (id INT PRIMARY KEY, t TINYINT, b BIGINT, u INT UNSIGNED, f FLOAT, d DOUBLE, dec1 DECIMAL(10,2), vc VARCHAR(20), tx TEXT, "
           "dt DATE, ts DATETIME, tm TIME, flag BOOLEAN)");
    ok(ex, "CREATE TABLE ty2 (id INT PRIMARY KEY, label VARCHAR(10))");
    ok(ex, "CREATE VIEW ty_v AS SELECT id, dt, dec1 AS price FROM ty");

    // columns of a table, a star, a qualified star, an alias, names in any case
    REQUIRE(types_of(ex, "SELECT * FROM ty") == "int,tinyint,bigint,uint,float,double,dec(10,2),text,text,date,datetime,time,bool");
    REQUIRE(types_of(ex, "SELECT id, DEC1, Dt FROM ty") == "int,dec(10,2),date");
    REQUIRE(types_of(ex, "SELECT dec1 AS price, id pk FROM ty") == "dec(10,2),int");
    REQUIRE(types_of(ex, "SELECT ty.id, ty.ts FROM d.ty") == "int,datetime");
    REQUIRE(types_of(ex, "SELECT t.id, t.dec1 FROM ty t") == "int,dec(10,2)");
    // joins: the columns come in the order of the tables; a star over a USING join shows the common column once, in front (R57)
    REQUIRE(types_of(ex, "SELECT x.id, y.label FROM ty x JOIN ty2 y ON x.id = y.id") == "int,text");
    REQUIRE(types_of(ex, "SELECT ty2.* FROM ty JOIN ty2 ON ty.id = ty2.id") == "int,text");
    REQUIRE(types_of(ex, "SELECT ty2.*, ty.dt FROM ty LEFT JOIN ty2 ON ty.id = ty2.id") == "int,text,date");
    REQUIRE(types_of(ex, "SELECT * FROM ty JOIN ty2 USING (id)") == "int,tinyint,bigint,uint,float,double,dec(10,2),text,text,date,datetime,time,bool,text");
    // aggregates: COUNT is BIGINT, SUM / AVG follow MySQL (an integer sums to DECIMAL(32,0), a DECIMAL(10,2) to (32,2) and averages to (14,6)), MIN / MAX keep the type
    REQUIRE(types_of(ex, "SELECT COUNT(*), COUNT(DISTINCT vc), SUM(id), SUM(dec1), AVG(id), AVG(dec1), SUM(d), AVG(f) FROM ty") ==
            "bigint,bigint,dec(32,0),dec(32,2),dec(14,4),dec(14,6),double,double");
    REQUIRE(types_of(ex, "SELECT MIN(id), MAX(dt), MIN(vc), MAX(dec1), STDDEV(d), BIT_AND(id) FROM ty") == "int,date,text,dec(10,2),double,ubigint");
    REQUIRE(types_of(ex, "SELECT t, COUNT(*) c FROM ty GROUP BY t") == "tinyint,bigint");
    // windows
    REQUIRE(types_of(ex, "SELECT ROW_NUMBER() OVER (ORDER BY id), SUM(id) OVER (), LAG(dec1) OVER (ORDER BY id), PERCENT_RANK() OVER (ORDER BY id) FROM ty") ==
            "bigint,dec(32,0),dec(10,2),double");
    // derived tables, views, a set operation (the left side types it), information_schema
    REQUIRE(types_of(ex, "SELECT * FROM (SELECT id, dec1 FROM ty) q") == "int,dec(10,2)");
    REQUIRE(types_of(ex, "SELECT q.price FROM (SELECT dec1 AS price FROM ty) q") == "dec(10,2)");
    REQUIRE(types_of(ex, "SELECT * FROM ty_v") == "int,date,dec(10,2)");
    REQUIRE(types_of(ex, "SELECT price, id FROM ty_v") == "dec(10,2),int");
    REQUIRE(types_of(ex, "SELECT id FROM ty UNION SELECT id FROM ty2") == "int");
    REQUIRE(types_of(ex, "SELECT table_name, table_rows FROM information_schema.tables") == "text,bigint");
    // expressions: numbers by value (a COALESCE / IF / CASE out of numbers too), text (a string function, a CASE out of strings) left as text
    REQUIRE(types_of(ex, "SELECT id * 2, 1, 'a', LENGTH(vc), ROUND(d, 1), LAST_INSERT_ID(), UPPER(vc), CONCAT(vc, 'x') FROM ty") ==
            "value,value,text,value,double,value,text,text");
    REQUIRE(types_of(ex, "SELECT 1 + 1, 2.5") == "value,value");
    REQUIRE(types_of(ex, "SELECT '12', id + t, dt, UPPER(tx), IF(id > 1, 1, 2), COALESCE(t, 0) FROM ty") == "text,value,date,text,value,value"); // (a quoted number is text)
    REQUIRE(types_of(ex, "SELECT IF(id > 1, 'a', 'b'), COALESCE(vc, 'x'), CASE WHEN id > 1 THEN 1 ELSE 0 END, CASE WHEN id > 1 THEN 'a' END, NULLIF(id, 3), COALESCE(NULL, NULL) FROM ty") ==
            "text,text,value,text,value,text");
    // nothing to tell: something that is not a SELECT, a table that is not there
    REQUIRE(types_of(ex, "INSERT INTO ty2 VALUES (1, 'a')") == "");
    REQUIRE(types_of(ex, "SELECT * FROM no_such_table") == "");
    REQUIRE(types_of(ex, "SELECT this is not sql") == "");
    REQUIRE(types_of(ex, "WITH c AS (SELECT id FROM ty) SELECT * FROM c") == "int"); // (a CTE is a table of the columns its query gives)
    REQUIRE(types_of(ex, "SHOW TABLES") == "");
}
