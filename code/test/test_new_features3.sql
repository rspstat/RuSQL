-- SETUP (멱등 실행: 이전 실행 잔류 데이터 정리)
DROP USER IF EXISTS 'alice'@'localhost';
DROP USER IF EXISTS 'bob'@'%';
DROP DATABASE IF EXISTS testdb;
CREATE DATABASE testdb;
USE testdb;

-- ─────────────────────────────────────────────────────────────────────────────
-- 1. FOREIGN KEY ON DELETE SET DEFAULT
-- ─────────────────────────────────────────────────────────────────────────────
CREATE TABLE departments (
    id INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(50) NOT NULL
);
CREATE TABLE employees (
    id INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(50) NOT NULL,
    dept_id INT DEFAULT 0,
    FOREIGN KEY (dept_id) REFERENCES departments(id) ON DELETE SET DEFAULT
);

INSERT INTO departments (name) VALUES ('Engineering'), ('Marketing');
INSERT INTO employees (name, dept_id) VALUES
    ('Alice', 1), ('Bob', 1), ('Carol', 2);

-- Before delete: all employees with dept_id set
SELECT id, name, dept_id FROM employees ORDER BY id;

-- Delete Engineering → dept_id for Alice & Bob should become 0 (DEFAULT)
DELETE FROM departments WHERE id = 1;
SELECT id, name, dept_id FROM employees ORDER BY id;

-- ─────────────────────────────────────────────────────────────────────────────
-- 2. CREATE USER
-- ─────────────────────────────────────────────────────────────────────────────
CREATE USER 'alice'@'localhost' IDENTIFIED BY 'secret123';
CREATE USER 'bob'@'%';
-- IF NOT EXISTS: should not error
CREATE USER IF NOT EXISTS 'alice'@'localhost' IDENTIFIED BY 'other';

-- ─────────────────────────────────────────────────────────────────────────────
-- 3. GRANT
-- ─────────────────────────────────────────────────────────────────────────────
GRANT SELECT, INSERT ON testdb.employees TO 'alice'@'localhost';
GRANT ALL PRIVILEGES ON *.* TO 'bob'@'%' WITH GRANT OPTION;

-- ─────────────────────────────────────────────────────────────────────────────
-- 4. SHOW GRANTS
-- ─────────────────────────────────────────────────────────────────────────────
SHOW GRANTS FOR 'alice'@'localhost';
SHOW GRANTS;

-- ─────────────────────────────────────────────────────────────────────────────
-- 5. REVOKE
-- ─────────────────────────────────────────────────────────────────────────────
REVOKE INSERT ON testdb.employees FROM 'alice'@'localhost';
SHOW GRANTS FOR 'alice'@'localhost';

-- ─────────────────────────────────────────────────────────────────────────────
-- 6. SHOW DATABASES
-- ─────────────────────────────────────────────────────────────────────────────
SHOW DATABASES;

-- ─────────────────────────────────────────────────────────────────────────────
-- 7. DROP USER
-- ─────────────────────────────────────────────────────────────────────────────
DROP USER 'bob'@'%';
DROP USER IF EXISTS 'ghost'@'localhost';
SHOW GRANTS;

-- CLEANUP
DROP TABLE IF EXISTS employees;
DROP TABLE IF EXISTS departments;
DROP DATABASE testdb;
