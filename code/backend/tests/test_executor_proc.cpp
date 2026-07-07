#include <filesystem>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};
} // namespace

TEST_CASE("CREATE PROCEDURE / CALL with IN param, DECLARE, and IF/ELSEIF", "[executor][proc]") {
    TempDataDir dir("exec_proc_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());

    auto create = ex.execute_sql(
        "CREATE PROCEDURE classify_salary(IN p_salary INT) BEGIN "
        "DECLARE grade VARCHAR(20) DEFAULT 'standard'; "
        "IF p_salary >= 120000 THEN SET grade = 'senior'; "
        "ELSEIF p_salary >= 80000 THEN SET grade = 'mid'; "
        "END IF; "
        "SELECT grade AS salary_grade; END");
    REQUIRE(create.is_ok());
    REQUIRE(create.value() == "Procedure 'classify_salary' created.");

    auto senior = ex.execute_sql("CALL classify_salary(130000)");
    REQUIRE(senior.is_ok());
    REQUIRE(senior.value().find("senior") != std::string::npos);

    auto mid = ex.execute_sql("CALL classify_salary(85000)");
    REQUIRE(mid.is_ok());
    REQUIRE(mid.value().find("mid") != std::string::npos);

    auto standard = ex.execute_sql("CALL classify_salary(50000)");
    REQUIRE(standard.is_ok());
    REQUIRE(standard.value().find("standard") != std::string::npos);
}

TEST_CASE("WHILE loop accumulates via proc_vars", "[executor][proc]") {
    TempDataDir dir("exec_proc_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());

    REQUIRE(ex.execute_sql("CREATE PROCEDURE sum_to_n(IN n INT) BEGIN "
                            "DECLARE i INT DEFAULT 1; DECLARE total INT DEFAULT 0; "
                            "WHILE i <= n DO SET total = total + i; SET i = i + 1; END WHILE; "
                            "SELECT total AS result; END")
                .is_ok());

    auto r = ex.execute_sql("CALL sum_to_n(10)");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("55") != std::string::npos); // 1+2+...+10
}

TEST_CASE("LOOP with labeled LEAVE/ITERATE", "[executor][proc]") {
    TempDataDir dir("exec_proc_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());

    REQUIRE(ex.execute_sql("CREATE PROCEDURE odd_sum(IN n INT) BEGIN "
                            "DECLARE i INT DEFAULT 0; DECLARE total INT DEFAULT 0; "
                            "calc: LOOP SET i = i + 1; IF i > n THEN LEAVE calc; END IF; "
                            "IF MOD(i,2) = 0 THEN ITERATE calc; END IF; "
                            "SET total = total + i; END LOOP; SELECT total AS odd_sum; END")
                .is_ok());

    auto r = ex.execute_sql("CALL odd_sum(10)");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("25") != std::string::npos); // 1+3+5+7+9
}

TEST_CASE("REPEAT/UNTIL counts down", "[executor][proc]") {
    TempDataDir dir("exec_proc_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());

    REQUIRE(ex.execute_sql("CREATE PROCEDURE countdown(IN start_val INT) BEGIN "
                            "DECLARE counter INT; SET counter = start_val; "
                            "REPEAT SET counter = counter - 1; UNTIL counter <= 0 END REPEAT; "
                            "SELECT counter AS final_val; END")
                .is_ok());

    auto r = ex.execute_sql("CALL countdown(5)");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("0") != std::string::npos);
}

TEST_CASE("DROP PROCEDURE removes it; CALL afterward fails", "[executor][proc]") {
    TempDataDir dir("exec_proc_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE PROCEDURE double_val(IN x INT) BEGIN DECLARE result INT; "
                            "SET result = x * 2; SELECT result AS doubled; END")
                .is_ok());

    auto drop = ex.execute_sql("DROP PROCEDURE double_val");
    REQUIRE(drop.is_ok());
    REQUIRE(drop.value() == "Procedure 'double_val' dropped.");

    auto call = ex.execute_sql("CALL double_val(21)");
    REQUIRE(call.is_err());

    auto drop_again = ex.execute_sql("DROP PROCEDURE double_val");
    REQUIRE(drop_again.is_err());
    auto drop_if_exists = ex.execute_sql("DROP PROCEDURE IF EXISTS double_val");
    REQUIRE(drop_if_exists.is_ok());
}

TEST_CASE("PREPARE / EXECUTE USING / DEALLOCATE PREPARE", "[executor][proc]") {
    TempDataDir dir("exec_proc_data_6");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, first_name VARCHAR(50), salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1, 'Alice', 90000)").is_ok());

    auto prep = ex.execute_sql("PREPARE find_emp FROM 'SELECT id, first_name, salary FROM employee WHERE id = ?'");
    REQUIRE(prep.is_ok());
    REQUIRE(prep.value() == "Query OK");

    REQUIRE(ex.execute_sql("SET @eid = 1").is_ok());
    auto exec1 = ex.execute_sql("EXECUTE find_emp USING @eid");
    REQUIRE(exec1.is_ok());
    REQUIRE(exec1.value().find("Alice") != std::string::npos);

    auto dealloc = ex.execute_sql("DEALLOCATE PREPARE find_emp");
    REQUIRE(dealloc.is_ok());
    REQUIRE(dealloc.value() == "Query OK");

    auto exec2 = ex.execute_sql("EXECUTE find_emp USING @eid");
    REQUIRE(exec2.is_err());
}

TEST_CASE("CREATE TRIGGER fires AFTER INSERT and mutates another table", "[executor][proc]") {
    TempDataDir dir("exec_proc_data_7");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE audit_log (msg VARCHAR(100))").is_ok());

    auto create_trg = ex.execute_sql("CREATE TRIGGER trg_after_insert_emp AFTER INSERT ON employee FOR EACH ROW "
                                      "INSERT INTO audit_log VALUES ('employee inserted')");
    REQUIRE(create_trg.is_ok());
    REQUIRE(create_trg.value() == "Trigger 'trg_after_insert_emp' created.");

    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1, 90000)").is_ok());

    auto s = ex.get_shared()->read();
    REQUIRE(s->tables.at("company.audit_log").size() == 1);
}

TEST_CASE("DROP TRIGGER stops it from firing", "[executor][proc]") {
    TempDataDir dir("exec_proc_data_8");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE audit_log (msg VARCHAR(100))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TRIGGER trg_after_insert_emp AFTER INSERT ON employee FOR EACH ROW "
                            "INSERT INTO audit_log VALUES ('x')")
                .is_ok());

    auto drop = ex.execute_sql("DROP TRIGGER trg_after_insert_emp");
    REQUIRE(drop.is_ok());

    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1)").is_ok());
    auto s = ex.get_shared()->read();
    REQUIRE(s->tables.at("company.audit_log").empty());
}

TEST_CASE("CREATE FUNCTION defines a UDF usable from SELECT", "[executor][proc]") {
    TempDataDir dir("exec_proc_data_9");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, annual_salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1, 120000)").is_ok());

    auto create_fn = ex.execute_sql("CREATE FUNCTION monthly_salary(annual_salary) RETURNS DECIMAL RETURN annual_salary / 12");
    REQUIRE(create_fn.is_ok());
    REQUIRE(create_fn.value() == "Function 'monthly_salary' created.");

    auto r = ex.execute_sql("SELECT monthly_salary(annual_salary) AS m FROM employee");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("10000") != std::string::npos); // 120000 / 12

    auto drop_fn = ex.execute_sql("DROP FUNCTION monthly_salary");
    REQUIRE(drop_fn.is_ok());
    REQUIRE(drop_fn.value() == "Function 'monthly_salary' dropped.");
}

TEST_CASE("DATABASE()/SCHEMA() reflect the current session database", "[executor][proc]") {
    TempDataDir dir("exec_proc_data_10");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());

    auto r = ex.execute_sql("SELECT DATABASE() AS db FROM t");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("company") != std::string::npos);
}
