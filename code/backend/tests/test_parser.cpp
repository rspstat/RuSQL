#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

#include "catch.hpp"
#include "engine/parser/parser.hpp"

using namespace engine;

namespace {

bool is_word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

std::string upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// Test-only statement splitter: NOT a port of any Rust file (statement splitting for
// multi-query input lives in the CLI/server binaries, ported in Phase 9). This exists
// solely to feed individual statements from test/test_full.sql to the Phase 4 parser,
// tracking BEGIN/END nesting so procedure/trigger bodies aren't split on their
// internal semicolons.
std::vector<std::string> split_statements(const std::string& sql) {
    std::vector<std::string> out;
    std::string current;
    std::size_t i = 0;
    const std::size_t n = sql.size();
    int depth = 0;

    auto flush = [&]() {
        std::size_t a = current.find_first_not_of(" \t\r\n");
        if (a != std::string::npos) {
            std::size_t b = current.find_last_not_of(" \t\r\n");
            out.push_back(current.substr(a, b - a + 1));
        }
        current.clear();
    };

    while (i < n) {
        char c = sql[i];
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
            while (i < n && sql[i] != '\n') i++;
            continue;
        }
        if (c == '#') {
            while (i < n && sql[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        if (c == '\'') {
            current.push_back(c);
            i++;
            while (i < n) {
                if (sql[i] == '\\' && i + 1 < n) {
                    current.push_back(sql[i]);
                    current.push_back(sql[i + 1]);
                    i += 2;
                    continue;
                }
                if (sql[i] == '\'') {
                    current.push_back('\'');
                    i++;
                    if (i < n && sql[i] == '\'') { current.push_back('\''); i++; continue; }
                    break;
                }
                current.push_back(sql[i]);
                i++;
            }
            continue;
        }
        bool at_word_start = is_word_char(c) && (i == 0 || !is_word_char(sql[i - 1]));
        if (at_word_start) {
            std::size_t j = i;
            while (j < n && is_word_char(sql[j])) j++;
            std::string word = upper(sql.substr(i, j - i));

            auto peek_word_after = [&](std::size_t from) -> std::string {
                std::size_t k = from;
                while (k < n && !is_word_char(sql[k]) && sql[k] != '(' && sql[k] != ';') k++;
                std::size_t m = k;
                while (m < n && is_word_char(sql[m])) m++;
                return upper(sql.substr(k, m - k));
            };
            auto next_nonspace_is_lparen = [&](std::size_t from) -> bool {
                std::size_t k = from;
                while (k < n && std::isspace(static_cast<unsigned char>(sql[k]))) k++;
                return k < n && sql[k] == '(';
            };

            // Every one of these opens a block terminated by a bare "END" token
            // (END, END IF, END LOOP, END WHILE, END REPEAT all just decrement once).
            // IF/LOOP/WHILE/REPEAT are also used as function calls (IF(...), REPEAT('-',3))
            // or DDL clauses (IF [NOT] EXISTS) elsewhere in the grammar — only treat them
            // as block openers when neither of those applies.
            bool is_block_opener = false;
            if (word == "BEGIN" || word == "CASE") {
                is_block_opener = true;
            } else if (word == "IF") {
                std::string nxt = peek_word_after(j);
                is_block_opener = !next_nonspace_is_lparen(j) && nxt != "NOT" && nxt != "EXISTS";
            } else if (word == "LOOP" || word == "WHILE" || word == "REPEAT") {
                is_block_opener = !next_nonspace_is_lparen(j);
            }

            if (is_block_opener) {
                depth++;
            } else if (word == "END") {
                depth = std::max(0, depth - 1);
                // Consume a trailing decoration word (END IF / END LOOP / END WHILE /
                // END REPEAT) so it isn't mistaken for a fresh block opener.
                std::size_t k = j;
                while (k < n && !is_word_char(sql[k]) && sql[k] != ';') k++;
                std::size_t m = k;
                while (m < n && is_word_char(sql[m])) m++;
                std::string next_word = upper(sql.substr(k, m - k));
                if (next_word == "IF" || next_word == "LOOP" || next_word == "WHILE" || next_word == "REPEAT") {
                    current += sql.substr(i, j - i);
                    current += sql.substr(j, k - j);
                    current += sql.substr(k, m - k);
                    i = m;
                    continue;
                }
            }
            current += sql.substr(i, j - i);
            i = j;
            continue;
        }
        if (c == ';' && depth == 0) {
            flush();
            i++;
            continue;
        }
        current.push_back(c);
        i++;
    }
    flush();
    return out;
}

} // namespace

TEST_CASE("CREATE TABLE with constraints parses correctly", "[parser]") {
    Parser p("CREATE TABLE employee ("
             "id INT PRIMARY KEY AUTO_INCREMENT, "
             "name VARCHAR(50) NOT NULL, "
             "salary DECIMAL(12,2) CHECK (salary > 0), "
             "department_id INT, "
             "FOREIGN KEY (department_id) REFERENCES department(id) ON DELETE SET NULL"
             ")");
    auto res = p.parse();
    REQUIRE(res.is_ok());
    REQUIRE(std::holds_alternative<Statement::CreateTable>(res.value().data));
    auto& ct = std::get<Statement::CreateTable>(res.value().data);
    REQUIRE(ct.name == "employee");
    REQUIRE(ct.columns.size() == 4);
    REQUIRE(ct.columns[0].primary_key);
    REQUIRE(ct.columns[0].auto_increment);
    REQUIRE(ct.columns[2].check_expr.has_value());
    REQUIRE(ct.columns[3].foreign_key.has_value());
    REQUIRE(ct.columns[3].foreign_key->on_delete == FkAction::SetNull);
}

TEST_CASE("SELECT with JOIN, WHERE, GROUP BY, ORDER BY, LIMIT parses", "[parser]") {
    Parser p("SELECT e.name, d.name AS dept FROM employee e "
             "JOIN department d ON e.department_id = d.id "
             "WHERE e.salary > 50000 "
             "GROUP BY d.name HAVING COUNT(*) > 1 "
             "ORDER BY e.name DESC LIMIT 10 OFFSET 5");
    auto res = p.parse();
    REQUIRE(res.is_ok());
    auto& sel = std::get<Statement::Select>(res.value().data);
    REQUIRE(sel.table == "employee");
    REQUIRE(sel.joins.size() == 1);
    REQUIRE(sel.condition.has_value());
    REQUIRE(sel.group_by.has_value());
    REQUIRE(sel.having.has_value());
    REQUIRE(sel.order_by.size() == 1);
    REQUIRE(sel.limit == 10u);
    REQUIRE(sel.offset == 5u);
}

TEST_CASE("subquery, EXISTS, and IN (subquery) parse", "[parser]") {
    Parser p1("SELECT name FROM employee WHERE salary > (SELECT AVG(salary) FROM employee)");
    REQUIRE(p1.parse().is_ok());

    Parser p2("SELECT name FROM employee e WHERE EXISTS (SELECT 1 FROM project p WHERE p.lead_id = e.id)");
    REQUIRE(p2.parse().is_ok());

    Parser p3("SELECT name FROM employee WHERE department_id IN (SELECT id FROM department WHERE is_active = true)");
    REQUIRE(p3.parse().is_ok());
}

TEST_CASE("CTE and recursive CTE parse", "[parser]") {
    Parser p("WITH dept_stats AS (SELECT department_id, COUNT(*) AS n FROM employee GROUP BY department_id) "
             "SELECT * FROM dept_stats");
    REQUIRE(p.parse().is_ok());

    Parser p2("WITH RECURSIVE mgmt_tree AS ("
              "SELECT id, manager_id, 0 AS depth FROM employee WHERE manager_id IS NULL "
              "UNION ALL "
              "SELECT e.id, e.manager_id, t.depth + 1 FROM employee e JOIN mgmt_tree t ON e.manager_id = t.id"
              ") SELECT * FROM mgmt_tree");
    auto res2 = p2.parse();
    REQUIRE(res2.is_ok());
    REQUIRE(std::holds_alternative<Statement::With>(res2.value().data));
}

TEST_CASE("UNION / INTERSECT / EXCEPT parse", "[parser]") {
    Parser p1("SELECT id FROM employee UNION SELECT id FROM project");
    REQUIRE(p1.parse().is_ok());
    Parser p2("SELECT id FROM employee INTERSECT SELECT lead_id FROM project");
    REQUIRE(p2.parse().is_ok());
    Parser p3("SELECT id FROM employee EXCEPT SELECT lead_id FROM project");
    REQUIRE(p3.parse().is_ok());
}

TEST_CASE("window functions parse", "[parser]") {
    Parser p("SELECT name, salary, "
             "ROW_NUMBER() OVER (ORDER BY salary DESC) AS rn, "
             "RANK() OVER (PARTITION BY department_id ORDER BY salary DESC) AS rk, "
             "SUM(salary) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running "
             "FROM employee");
    auto res = p.parse();
    REQUIRE(res.is_ok());
}

TEST_CASE("INSERT / UPDATE / DELETE / MERGE parse", "[parser]") {
    Parser p1("INSERT INTO employee (name, salary) VALUES ('Alice', 100000) ON DUPLICATE KEY UPDATE salary=100000");
    REQUIRE(p1.parse().is_ok());

    Parser p2("UPDATE employee SET salary = salary * 1.1 WHERE department_id = 1 RETURNING id, salary");
    REQUIRE(p2.parse().is_ok());

    Parser p3("DELETE FROM employee WHERE id = 1 RETURNING id");
    REQUIRE(p3.parse().is_ok());

    Parser p4("MERGE INTO department USING dept_upd ON department.code = dept_upd.code "
              "WHEN MATCHED THEN UPDATE SET budget = dept_upd.budget "
              "WHEN NOT MATCHED THEN INSERT (code, name) VALUES (dept_upd.code, dept_upd.name)");
    REQUIRE(p4.parse().is_ok());
}

TEST_CASE("stored procedure with control flow parses", "[parser]") {
    Parser p("CREATE PROCEDURE classify_salary(IN p_salary INT) BEGIN "
             "DECLARE grade VARCHAR(20) DEFAULT 'standard'; "
             "IF p_salary >= 120000 THEN SET grade = 'senior'; "
             "ELSEIF p_salary >= 80000 THEN SET grade = 'mid'; "
             "END IF; "
             "SELECT grade AS salary_grade; "
             "END");
    auto res = p.parse();
    REQUIRE(res.is_ok());
    REQUIRE(std::holds_alternative<Statement::CreateProcedure>(res.value().data));
    auto& proc = std::get<Statement::CreateProcedure>(res.value().data);
    REQUIRE(proc.body.size() == 3);
}

TEST_CASE("trigger, transactions, and DCL parse", "[parser]") {
    Parser p1("CREATE TRIGGER trg_after_insert_emp AFTER INSERT ON employee FOR EACH ROW "
              "UPDATE department SET headcount = headcount + 1 WHERE id = 1");
    REQUIRE(p1.parse().is_ok());

    Parser p2("BEGIN");
    REQUIRE(p2.parse().is_ok());
    Parser p3("COMMIT");
    REQUIRE(p3.parse().is_ok());
    Parser p4("ROLLBACK TO SAVEPOINT sp1");
    REQUIRE(p4.parse().is_ok());

    Parser p5("CREATE USER 'testuser'@'%' IDENTIFIED BY 'secure_pass_123'");
    REQUIRE(p5.parse().is_ok());
    Parser p6("GRANT SELECT, INSERT, UPDATE ON company.employee TO 'testuser'@'%' WITH GRANT OPTION");
    REQUIRE(p6.parse().is_ok());
}

TEST_CASE("errors are reported, not silently accepted", "[parser]") {
    Parser p("SELEC bogus syntax");
    auto res = p.parse();
    REQUIRE(res.is_err());
}

TEST_CASE("every statement in test/test_full.sql parses without error", "[parser][integration]") {
    std::ifstream file(TEST_FULL_SQL_PATH);
    REQUIRE(file.is_open());
    std::stringstream buf;
    buf << file.rdbuf();
    std::string content = buf.str();
    REQUIRE(!content.empty());

    auto statements = split_statements(content);
    INFO("Total statements split from test_full.sql: " << statements.size());
    REQUIRE(statements.size() > 100);

    std::size_t failures = 0;
    std::string all_failures;

    for (auto& stmt : statements) {
        Parser p(stmt);
        auto res = p.parse();
        if (res.is_err()) {
            all_failures += "\n---\nSQL: " + stmt + "\nERROR: " + res.error() + "\n";
            failures++;
        }
    }

    INFO(all_failures);
    REQUIRE(failures == 0);
}
