-- SETUP (멱등 실행: 이전 실행 잔류 데이터 정리)
DROP DATABASE IF EXISTS testdb;
CREATE DATABASE testdb;
USE testdb;

CREATE TABLE departments (
    id INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(50) NOT NULL UNIQUE
);
CREATE TABLE employees (
    id INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(50) NOT NULL,
    dept_id INT,
    salary INT,
    hire_date DATE,
    FOREIGN KEY (dept_id) REFERENCES departments(id) ON DELETE SET NULL
);

INSERT INTO departments (name) VALUES ('Engineering'), ('Marketing'), ('Finance');
INSERT INTO employees (name, dept_id, salary, hire_date) VALUES
    ('Alice',  1, 9000000, '2019-03-15'),
    ('Bob',    1, 8500000, '2020-07-01'),
    ('Carol',  2, 7200000, '2018-11-20'),
    ('Dave',   2, 5800000, '2021-04-10'),
    ('Eve',    3, 12000000, '2015-01-05'),
    ('Frank',  3, 6500000, '2022-08-30'),
    ('Grace',  1, 9500000, '2017-06-12');

-- GROUP_CONCAT
SELECT d.name AS dept, GROUP_CONCAT(e.name SEPARATOR ', ') AS members
    FROM employees e JOIN departments d ON e.dept_id = d.id
    GROUP BY d.name ORDER BY d.name;

SELECT d.name AS dept, GROUP_CONCAT(e.name) AS members
    FROM employees e JOIN departments d ON e.dept_id = d.id
    GROUP BY d.name ORDER BY d.name;

-- GROUP_CONCAT with HAVING
SELECT dept_id, GROUP_CONCAT(name SEPARATOR ' / ') AS team
    FROM employees GROUP BY dept_id HAVING COUNT(*) >= 3;

-- INSERT IGNORE
INSERT INTO departments (name) VALUES ('Engineering');
INSERT IGNORE INTO departments (name) VALUES ('Engineering');
SELECT * FROM departments ORDER BY id;

-- ON DUPLICATE KEY UPDATE
INSERT INTO employees (id, name, dept_id, salary, hire_date)
    VALUES (1, 'Alice', 1, 10000000, '2019-03-15')
    ON DUPLICATE KEY UPDATE salary = 10000000;
SELECT id, name, salary FROM employees WHERE id = 1;

-- INSERT IGNORE multi-row (일부만 중복)
INSERT IGNORE INTO departments (name) VALUES ('Legal'), ('Marketing'), ('HR');
SELECT * FROM departments ORDER BY id;

-- NULLIF
SELECT name, NULLIF(dept_id, 3) AS non_finance_dept FROM employees ORDER BY id;

-- LPAD / RPAD
SELECT LPAD(id, 5, '0') AS padded_id, name FROM employees ORDER BY id LIMIT 4;
SELECT name, RPAD(name, 10, '.') AS padded_name FROM employees ORDER BY id LIMIT 4;

-- CAST
SELECT name, CAST(salary AS FLOAT) AS salary_float FROM employees ORDER BY salary DESC LIMIT 3;
SELECT CAST('2026' AS INT) AS year_int, CAST('3.14' AS FLOAT) AS pi;

-- DATEDIFF
SELECT name, hire_date, DATEDIFF('2026-05-02', hire_date) AS days_employed
    FROM employees ORDER BY days_employed DESC;

-- DATE_ADD
SELECT name, hire_date,
    DATE_ADD(hire_date, INTERVAL 365 DAY) AS one_year_after
    FROM employees ORDER BY id LIMIT 4;

SELECT name, hire_date,
    DATE_ADD(hire_date, INTERVAL 6 MONTH) AS six_months_after
    FROM employees ORDER BY id LIMIT 3;

-- WITH RECURSIVE (조직 계층 트리)
CREATE TABLE org_tree (
    id INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(50),
    parent_id INT
);
INSERT INTO org_tree (name, parent_id) VALUES
    ('CEO', NULL),
    ('CTO', 1), ('CFO', 1),
    ('Backend Lead', 2), ('Frontend Lead', 2),
    ('Alice', 4), ('Bob', 4), ('Carol', 5);

WITH RECURSIVE hierarchy AS (
    SELECT id, name, parent_id, 0 AS depth
        FROM org_tree WHERE parent_id IS NULL
    UNION ALL
    SELECT o.id, o.name, o.parent_id, h.depth + 1
        FROM org_tree o JOIN hierarchy h ON o.parent_id = h.id
)
SELECT id, name, depth FROM hierarchy ORDER BY depth, id;

-- WITH RECURSIVE (1~10 숫자 시퀀스)
CREATE TABLE nums_seed (n INT PRIMARY KEY);
INSERT INTO nums_seed (n) VALUES (1);

WITH RECURSIVE seq AS (
    SELECT n FROM nums_seed
    UNION ALL
    SELECT n + 1 FROM seq WHERE n < 10
)
SELECT n FROM seq ORDER BY n;

-- CLEANUP
DROP TABLE IF EXISTS org_tree;
DROP TABLE IF EXISTS nums_seed;
DROP TABLE IF EXISTS employees;
DROP TABLE IF EXISTS departments;
DROP DATABASE testdb;
