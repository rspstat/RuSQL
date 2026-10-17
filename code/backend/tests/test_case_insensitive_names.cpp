#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Names do not depend on case: a database, a table, a column, an alias, an index, a view, a procedure, a trigger, a function and a user variable are
// found by any spelling (MySQL on Windows: lower_case_table_names = 1 for the tables and databases, always for the rest), and what a statement prints
// keeps the spelling it was written with (the header of `SELECT NAME FROM emp` is NAME; `SELECT *` prints the names the table was created with).

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

// the table a statement printed as "header1,header2|cell,cell;cell,cell" (no padding, no borders)
std::string answer(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line, header, body;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        std::string row;
        std::size_t from = 1;
        while (from < line.size()) {
            std::size_t bar = line.find('|', from);
            if (bar == std::string::npos) break;
            std::string cell = line.substr(from, bar - from);
            cell.erase(0, cell.find_first_not_of(" \t\r"));
            const std::size_t last = cell.find_last_not_of(" \t\r");
            cell.erase(last == std::string::npos ? 0 : last + 1);
            row += (row.empty() ? "" : ",") + cell;
            from = bar + 1;
        }
        if (++bars == 1) header = row;
        else body += (body.empty() ? "" : ";") + row;
    }
    return header + "|" + body;
}

std::string error_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_err());
    return r.error();
}

void ok(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
}

struct Case {
    std::string sql;
    std::string expected;
};

void check(Executor& ex, const std::vector<Case>& cases) {
    for (const Case& c : cases) {
        INFO(c.sql);
        CHECK(answer(ex, c.sql) == c.expected);
    }
}

void make_staff(Executor& ex) {
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE Emp (Id INT PRIMARY KEY, Name VARCHAR(20), Dept VARCHAR(10), Pay INT, Boss INT)");
    ok(ex, "CREATE TABLE Depts (Code VARCHAR(10) PRIMARY KEY, Title VARCHAR(20))");
    ok(ex, "INSERT INTO Emp VALUES (1,'ann','a',100,NULL),(2,'bob','a',200,1),(3,'cy','b',300,1),(4,'di','b',150,3)");
    ok(ex, "INSERT INTO Depts VALUES ('a','Alpha'),('b','Beta')");
}
} // namespace

TEST_CASE("a query finds tables, columns and aliases by any spelling and prints what it was given", "[names]") {
    TempDataDir dir("names_select");
    Executor ex(dir.path);
    make_staff(ex);

    check(ex, {
        {"SELECT * FROM EMP WHERE ID = 1", "Id,Name,Dept,Pay,Boss|1,ann,a,100,NULL"},
        {"SELECT NAME, pay FROM emp WHERE iD = 2", "NAME,pay|bob,200"},
        {"SELECT E.NAME FROM EMP e WHERE e.ID = 3", "NAME|cy"},
        {"SELECT e.* FROM emp E WHERE E.id = 1", "Id,Name,Dept,Pay,Boss|1,ann,a,100,NULL"},
        {"SELECT EMP.* FROM emp WHERE id = 1", "Id,Name,Dept,Pay,Boss|1,ann,a,100,NULL"},
        {"SELECT E.NAME, D.TITLE FROM emp e JOIN depts D ON E.DEPT = d.code WHERE e.ID = 3", "NAME,TITLE|cy,Beta"},
        {"SELECT D.* FROM emp e JOIN depts d ON e.dept = D.CODE WHERE E.ID = 4", "Code,Title|b,Beta"},
        {"SELECT name FROM emp WHERE pay = (SELECT MAX(PAY) FROM EMP)", "name|cy"},
        {"SELECT NAME FROM emp e WHERE PAY > (SELECT AVG(pay) FROM EMP x WHERE X.DEPT = E.DEPT) ORDER BY NAME", "NAME|bob;cy"},
        {"SELECT NAME FROM emp WHERE EXISTS (SELECT 1 FROM EMP b WHERE B.ID = EMP.BOSS) ORDER BY NAME", "NAME|bob;cy;di"},
        {"SELECT DEPT, COUNT(*) AS N FROM emp GROUP BY dept HAVING n > 1 ORDER BY DEPT", "DEPT,N|a,2;b,2"},
        {"SELECT PAY + 1 AS P1 FROM emp ORDER BY p1 DESC LIMIT 2", "P1|301;201"},
        {"SELECT SUM(PAY), MAX(pay) FROM EMP", "SUM(PAY),MAX(pay)|750,300"},
        {"SELECT DEPT, MAX(PAY) - MIN(PAY) FROM emp GROUP BY DEPT ORDER BY DEPT", "DEPT,MAX(PAY) - MIN(PAY)|a,100;b,150"},
        {"SELECT ID FROM emp ORDER BY PAY * 2 DESC, ID LIMIT 2", "ID|3;2"},
        {"SELECT DISTINCT DEPT FROM emp ORDER BY dept", "DEPT|a;b"},
        {"SELECT NAME, RANK() OVER (PARTITION BY DEPT ORDER BY PAY DESC) AS rk FROM emp ORDER BY NAME LIMIT 2", "NAME,rk|ann,2;bob,1"},
        {"SELECT NAME FROM emp UNION SELECT TITLE FROM DEPTS ORDER BY 1", "NAME|Alpha;ann;Beta;bob;cy;di"},
        {"SELECT NAME FROM emp UNION SELECT TITLE FROM DEPTS ORDER BY name", "NAME|Alpha;ann;Beta;bob;cy;di"},
        {"SELECT COUNT(*) FROM Emp s1 JOIN emp S2 ON S1.BOSS = S2.ID", "COUNT(*)|3"},
        {"SELECT GROUP_CONCAT(NAME ORDER BY NAME SEPARATOR ',') AS g FROM emp GROUP BY DEPT ORDER BY DEPT", "g|ann,bob;cy,di"},
    });

    // a name the select list gives twice (apart from case) is ambiguous in ORDER BY / GROUP BY unless both stand for the same thing
    CHECK(answer(ex, "SELECT PAY AS X, PAY AS x FROM emp WHERE id = 1 ORDER BY x") == "X,x|100,100");
    CHECK(error_of(ex, "SELECT PAY AS X, BOSS AS x FROM emp ORDER BY X").find("ambiguous") != std::string::npos);
    CHECK(error_of(ex, "SELECT PAY AS x, BOSS AS x FROM emp ORDER BY x").find("ambiguous") != std::string::npos);
    CHECK(answer(ex, "SELECT PAY AS X, BOSS AS x FROM emp WHERE id = 2 ORDER BY 1") == "X,x|200,1");

    // an unknown name is still an error, whatever case it is written in
    CHECK(error_of(ex, "SELECT NOPE FROM emp").find("Unknown column") != std::string::npos);
    CHECK(error_of(ex, "SELECT x.ID FROM emp").find("Unknown column") != std::string::npos);
    CHECK(error_of(ex, "SELECT * FROM EMPS").find("not found") != std::string::npos);
    CHECK(error_of(ex, "SELECT ID FROM emp e JOIN depts d ON e.dept = d.code WHERE CODE = 'a' AND TITLE = 'Alpha' AND ID = ID AND NAME = 'zzz' AND Nope = 1")
              .find("Unknown column") != std::string::npos);
}

TEST_CASE("statements that write find tables, columns and keys by any spelling", "[names]") {
    TempDataDir dir("names_write");
    Executor ex(dir.path);
    make_staff(ex);

    ok(ex, "INSERT INTO EMP (ID, name, PAY) VALUES (5, 'ed', 50)");
    check(ex, {{"SELECT pay FROM emp WHERE id = 5", "pay|50"}});
    ok(ex, "UPDATE EMP E SET E.PAY = E.PAY + 5 WHERE E.ID = 5");
    check(ex, {{"SELECT PAY FROM emp WHERE id = 5", "PAY|55"}});
    ok(ex, "UPDATE emp SET PAY = PAY * 2 WHERE DEPT = 'b' ORDER BY ID LIMIT 1");
    check(ex, {{"SELECT pay FROM emp WHERE id = 3", "pay|600"}});
    ok(ex, "INSERT INTO emp (id, NAME) VALUES (5, 'x') ON DUPLICATE KEY UPDATE NAME = CONCAT(VALUES(NAME), '!')");
    check(ex, {{"SELECT name FROM emp WHERE id = 5", "name|x!"}});
    ok(ex, "INSERT INTO EMP (id, name, dept, pay) SELECT ID + 10, NAME, DEPT, PAY FROM emp WHERE ID = 1");
    check(ex, {{"SELECT name, DEPT, pay FROM emp WHERE id = 11", "name,DEPT,pay|ann,a,100"}});
    ok(ex, "REPLACE INTO EMP (ID, NAME) VALUES (4, 'dee')");
    check(ex, {{"SELECT name, pay FROM emp WHERE id = 4", "name,pay|dee,NULL"}});
    ok(ex, "DELETE FROM EMP WHERE ID > 10 ORDER BY ID");
    check(ex, {{"SELECT COUNT(*) FROM emp", "COUNT(*)|5"}});
}

TEST_CASE("DDL finds tables, columns and indexes by any spelling and refuses a second name that differs in case only", "[names]") {
    TempDataDir dir("names_ddl");
    Executor ex(dir.path);
    make_staff(ex);

    ok(ex, "CREATE INDEX IDX_Pay ON EMP (PAY, NAME)");
    CHECK(error_of(ex, "CREATE INDEX idx_pay ON emp (BOSS)").find("Duplicate key name") != std::string::npos);
    check(ex, {{"SELECT ID FROM emp WHERE PAY = 200", "ID|2"}});
    ok(ex, "CREATE INDEX idx_Pay ON DEPTS (TITLE)"); // the same index name on another table
    ok(ex, "DROP INDEX idx_PAY ON Emp");
    CHECK(error_of(ex, "CREATE INDEX IDX_PAY ON depts (CODE)").find("Duplicate key name") != std::string::npos); // (DROP INDEX ... ON left the other table's)
    ok(ex, "CREATE INDEX idx_pay ON emp (BOSS)"); // the name is free again

    ok(ex, "ALTER TABLE EMP ADD COLUMN Note VARCHAR(5)");
    CHECK(error_of(ex, "ALTER TABLE emp ADD COLUMN NOTE INT").find("Duplicate column name") != std::string::npos);
    ok(ex, "ALTER TABLE emp MODIFY COLUMN NOTE VARCHAR(10)");
    ok(ex, "ALTER TABLE emp RENAME COLUMN NOTE TO Memo");
    CHECK(error_of(ex, "ALTER TABLE emp RENAME COLUMN MEMO TO pay").find("Duplicate column name") != std::string::npos);
    check(ex, {{"SELECT memo FROM EMP WHERE id = 1", "memo|NULL"}});
    ok(ex, "ALTER TABLE emp RENAME COLUMN MEMO TO memo2");
    ok(ex, "ALTER TABLE emp RENAME COLUMN memo2 TO Memo2"); // (a column may be renamed to its own name in another case)
    ok(ex, "ALTER TABLE emp RENAME COLUMN MEMO2 TO Memo");
    ok(ex, "ALTER TABLE emp DROP COLUMN MEMO");
    CHECK(error_of(ex, "SELECT memo FROM emp").find("Unknown column") != std::string::npos);

    CHECK(error_of(ex, "CREATE TABLE T2 (a INT, A INT)").find("Duplicate column name") != std::string::npos);
    CHECK(error_of(ex, "CREATE TABLE EMP (x INT)").find("already exists") != std::string::npos);
    ok(ex, "CREATE TABLE IF NOT EXISTS eMp (x INT)");

    ok(ex, "CREATE VIEW V1 AS SELECT NAME, PAY FROM EMP WHERE PAY > 120");
    check(ex, {
        {"SELECT name FROM v1 ORDER BY NAME", "name|bob;cy;di"},
        {"SELECT NAME, pay FROM V1 WHERE PAY > 160 ORDER BY name", "NAME,pay|bob,200;cy,300"},
    });
    ok(ex, "DROP VIEW v1");

    // a foreign key written in other case works: the cascade finds the child rows
    ok(ex, "CREATE TABLE Kid (Kid_Id INT PRIMARY KEY, Parent INT, FOREIGN KEY (PARENT) REFERENCES EMP (ID) ON DELETE CASCADE)");
    ok(ex, "INSERT INTO KID VALUES (1, 1), (2, 2)");
    error_of(ex, "INSERT INTO kid VALUES (3, 999)"); // (no parent 999)
    ok(ex, "DELETE FROM EMP WHERE ID = 1");
    check(ex, {{"SELECT COUNT(*) FROM KID", "COUNT(*)|1"}});
    ok(ex, "TRUNCATE TABLE kid");
    ok(ex, "DROP TABLE KID");

    // the database name too
    ok(ex, "CREATE DATABASE Dbx");
    CHECK(error_of(ex, "CREATE DATABASE DBX").find("already exists") != std::string::npos);
    ok(ex, "CREATE DATABASE IF NOT EXISTS dbX");
    ok(ex, "CREATE TABLE dbx.Z1 (k INT)");
    ok(ex, "INSERT INTO DBX.z1 VALUES (1)");
    check(ex, {{"SELECT K FROM dBx.Z1", "K|1"}});
    ok(ex, "DROP TABLE DBX.Z1");
    ok(ex, "USE DBX");
    ok(ex, "DROP DATABASE dbx");
}

TEST_CASE("names are found by any spelling after a restart too", "[names]") {
    TempDataDir dir("names_restart");
    {
        Executor ex(dir.path);
        make_staff(ex);
        ok(ex, "CREATE INDEX IDX_Pay ON EMP (PAY)");
        ok(ex, "CREATE VIEW V1 AS SELECT NAME FROM EMP WHERE PAY > 120");
    }
    Executor ex(dir.path);
    ok(ex, "USE D");
    check(ex, {
        {"SELECT * FROM EMP WHERE ID = 2", "Id,Name,Dept,Pay,Boss|2,bob,a,200,1"},
        {"SELECT name FROM V1 ORDER BY NAME", "name|bob;cy;di"},
        {"SELECT ID FROM emp WHERE PAY = 150", "ID|4"},
    });
    ok(ex, "DROP INDEX idx_pay ON EMP");
}

TEST_CASE("user variables, procedures, triggers and functions are found by any spelling", "[names]") {
    TempDataDir dir("names_routines");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE Emp (Id INT PRIMARY KEY, Sal INT, Tag VARCHAR(10))");
    ok(ex, "CREATE TABLE Logt (Id INT, Note VARCHAR(20))");
    ok(ex, "INSERT INTO Emp VALUES (1, 100, 'a'), (2, 200, 'b')");

    ok(ex, "SET @V = 3");
    check(ex, {{"SELECT @v, @V", "@v,@V|3,3"}});
    ok(ex, "SET @Total := 5");
    check(ex, {{"SELECT @total + @TOTAL AS s", "s|10"}});

    ok(ex, "CREATE PROCEDURE Raise(IN pct INT) BEGIN UPDATE Emp SET Sal = Sal + PCT; END");
    ok(ex, "CALL raise(5)");
    ok(ex, "CALL RAISE(1)");
    check(ex, {{"SELECT Id, Sal FROM emp ORDER BY ID", "Id,Sal|1,106;2,206"}});
    ok(ex, "CREATE PROCEDURE Count_Emp(OUT n INT) BEGIN SELECT COUNT(*) INTO N FROM EMP; END");
    ok(ex, "CALL count_emp(@Cnt)");
    check(ex, {{"SELECT @cnt", "@cnt|2"}});
    ok(ex, "DROP PROCEDURE RAISE");
    CHECK(error_of(ex, "CALL raise(1)").find("not found") != std::string::npos);

    ok(ex, "CREATE TRIGGER Trg_Emp BEFORE INSERT ON Emp FOR EACH ROW SET NEW.SAL = NEW.sal * 2");
    ok(ex, "INSERT INTO EMP (ID, SAL) VALUES (3, 10)");
    check(ex, {{"SELECT Sal FROM emp WHERE id = 3", "Sal|20"}});
    ok(ex, "CREATE TRIGGER Trg_Log AFTER INSERT ON Emp FOR EACH ROW INSERT INTO LOGT VALUES (NEW.ID, NEW.TAG)");
    ok(ex, "INSERT INTO emp (id, sal, tag) VALUES (4, 1, 'zz')");
    check(ex, {{"SELECT * FROM logt ORDER BY ID", "Id,Note|4,zz"}});
    ok(ex, "CREATE TRIGGER Trg_Del AFTER DELETE ON Emp FOR EACH ROW INSERT INTO Logt VALUES (OLD.ID, OLD.Tag)");
    ok(ex, "DELETE FROM EMP WHERE ID = 4");
    check(ex, {{"SELECT * FROM logt ORDER BY ID", "Id,Note|4,zz;4,zz"}});
    ok(ex, "DROP TRIGGER trg_log");
    ok(ex, "INSERT INTO emp (id, sal, tag) VALUES (5, 1, 'qq')");
    check(ex, {{"SELECT COUNT(*) FROM logt", "COUNT(*)|2"}});

    ok(ex, "CREATE FUNCTION Dbl(x INT) RETURNS INT DETERMINISTIC RETURN X * 2");
    check(ex, {
        {"SELECT DBL(Sal) FROM emp WHERE id = 1", "DBL(Sal)|212"},
        {"SELECT dbl(3), Dbl(4)", "dbl(3),Dbl(4)|6,8"},
    });
    ok(ex, "DROP FUNCTION DBL");
}

TEST_CASE("|| is the logical OR unless sql_mode has PIPES_AS_CONCAT", "[names]") {
    TempDataDir dir("names_pipes");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, a INT, b INT, s VARCHAR(5))");
    ok(ex, "INSERT INTO t VALUES (1, 1, 0, 'x'), (2, 0, 0, 'y'), (3, 0, 5, 'z'), (4, NULL, 0, 'w')");

    check(ex, {
        {"SELECT 1 || 0 AS a, 0 || 0 AS b, 'a' || 'b' AS c, NULL || 1 AS d, NULL || 0 AS e", "a,b,c,d,e|1,0,0,1,NULL"},
        {"SELECT id FROM t WHERE a = 1 || b = 5 ORDER BY id", "id|1;3"},
        {"SELECT id FROM t WHERE a || b ORDER BY id", "id|1;3"},
        {"SELECT s || '!' AS j FROM t WHERE id = 1", "j|0"}, // (answered, and cached, before the mode changes)
    });

    ok(ex, "SET sql_mode = 'PIPES_AS_CONCAT'");
    check(ex, {
        {"SELECT 'a' || 'b' AS j", "j|ab"},
        {"SELECT s || '!' AS j FROM t WHERE id = 1", "j|x!"},
    });
    ok(ex, "SET sql_mode = 'ANSI'");
    check(ex, {{"SELECT 'a' || 'b' AS j", "j|ab"}});
    ok(ex, "SET sql_mode = 'STRICT_TRANS_TABLES'");
    check(ex, {{"SELECT 'a' || 'b' AS j", "j|0"}, {"SELECT s || '!' AS j FROM t WHERE id = 1", "j|0"}}); // (not the answer the other mode cached)
}

TEST_CASE("a row comparison is a value, and a scan comes in primary key order", "[names]") {
    TempDataDir dir("names_order");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");

    check(ex, {
        {"SELECT (1, 2) = (1, 2) AS a, (1, 2) = (1, 3) AS b, (1, 2) < (1, 3) AS c, (1, 2) <> (1, 2) AS d", "a,b,c,d|1,0,1,0"},
        {"SELECT (1, NULL) = (1, 2) AS a, (2, NULL) = (1, 2) AS b, (1, 2) IN ((0, 0), (1, 2)) AS c, (1, 2) NOT IN ((0, 0), (1, 3)) AS d", "a,b,c,d|NULL,0,1,1"},
    });

    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO t VALUES (3, 30), (1, 10), (2, 20)");
    check(ex, {{"SELECT id FROM t", "id|1;2;3"}});
    ok(ex, "UPDATE t SET v = v + 1 WHERE id = 1");
    check(ex, {
        {"SELECT id, v FROM t", "id,v|1,11;2,20;3,30"},
        {"SELECT id FROM t LIMIT 2", "id|1;2"},
        {"SELECT id FROM t WHERE v > 5", "id|1;2;3"},
    });
    // the order is kept per table and made again after every change of it
    ok(ex, "INSERT INTO t VALUES (0, 0)");
    check(ex, {{"SELECT id FROM t", "id|0;1;2;3"}});
    ok(ex, "BEGIN");
    ok(ex, "UPDATE t SET v = 99 WHERE id = 0");
    ok(ex, "INSERT INTO t VALUES (-1, 5)");
    check(ex, {{"SELECT id FROM t", "id|-1;0;1;2;3"}});
    ok(ex, "ROLLBACK");
    check(ex, {{"SELECT id FROM t", "id|0;1;2;3"}});
    ok(ex, "DELETE FROM t WHERE id = 2");
    check(ex, {{"SELECT id FROM t", "id|0;1;3"}, {"SELECT id FROM t LIMIT 2", "id|0;1"}});
    // a change that leaves the number of row versions as it was (a row deleted and vacuumed away, another one inserted out of order) makes it again too
    ok(ex, "CREATE TABLE z (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO z VALUES (1, 1), (2, 2), (3, 3)");
    check(ex, {{"SELECT id FROM z", "id|1;2;3"}});
    ok(ex, "DELETE FROM z WHERE id = 3");
    ok(ex, "VACUUM");
    ok(ex, "INSERT INTO z VALUES (0, 0)");
    check(ex, {{"SELECT id FROM z", "id|0;1;2"}});
    // a composite key whose first column is in order and the second is not
    ok(ex, "CREATE TABLE c2 (a INT, b INT, PRIMARY KEY (a, b))");
    ok(ex, "INSERT INTO c2 VALUES (1, 2), (1, 1), (2, 0)");
    check(ex, {{"SELECT a, b FROM c2", "a,b|1,1;1,2;2,0"}});
    ok(ex, "CREATE TABLE n (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO n VALUES (100, 1), (9, 2), (10, 3), (2, 4)");
    ok(ex, "UPDATE n SET v = v + 1 WHERE id = 9");
    check(ex, {{"SELECT id FROM n", "id|2;9;10;100"}}); // (numbers by value: 9 before 10)
    ok(ex, "CREATE TABLE c (a VARCHAR(5), b INT, v INT, PRIMARY KEY (a, b))");
    ok(ex, "INSERT INTO c VALUES ('b', 1, 1), ('a', 2, 2), ('a', 1, 3)");
    ok(ex, "UPDATE c SET v = v * 10 WHERE a = 'a' AND b = 1");
    check(ex, {{"SELECT a, b, v FROM c", "a,b,v|a,1,30;a,2,2;b,1,1"}});
}
