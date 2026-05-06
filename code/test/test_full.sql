-- RustDB 통합 테스트 (전 기능)

-- setup
DROP USER IF EXISTS 'usr'@'%';
DROP DATABASE IF EXISTS db1;
SHOW DATABASES;

CREATE DATABASE db1;
USE db1;

-- DDL: tables
CREATE TABLE dept (
    id     INT PRIMARY KEY AUTO INCREMENT,
    name   VARCHAR(30) NOT NULL UNIQUE,
    budget INT DEFAULT 0
);
CREATE TABLE emp (
    id      INT PRIMARY KEY AUTO INCREMENT,
    name    VARCHAR(30) NOT NULL,
    dept_id INT,
    salary  INT CHECK (salary > 0),
    hdate   DATE,
    status  ENUM('active','inactive') DEFAULT 'active',
    FOREIGN KEY (dept_id) REFERENCES dept(id) ON DELETE SET NULL
);
CREATE TABLE sal (
    id     INT PRIMARY KEY AUTO INCREMENT,
    eid    INT,
    amount INT CHECK (amount > 0),
    grade  ENUM('S1','S2','S3','S4','S5'),
    FOREIGN KEY (eid) REFERENCES emp(id) ON DELETE CASCADE
);
CREATE TABLE org (
    id   INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(30),
    pid  INT
);
CREATE TABLE tags (
    id  INT PRIMARY KEY AUTO INCREMENT,
    val ENUM('a','b','c'),
    set_col SET('X','Y','Z')
);

-- DDL: index + view
CREATE INDEX idx_dept ON emp (dept_id);
CREATE INDEX idx_ds   ON emp (dept_id, salary);
CREATE VIEW v_active AS SELECT id, name, dept_id FROM emp WHERE status = 'active';

SHOW TABLES;
DESCRIBE emp;

-- DML: insert
INSERT INTO dept (name, budget) VALUES ('Eng',1000),('Mkt',800),('Fin',1200);
INSERT INTO emp (name, dept_id, salary, hdate, status) VALUES
    ('Alice', 1, 900, '2020-01-15', 'active'),
    ('Bob',   1, 800, '2021-06-01', 'active'),
    ('Carol', 2, 700, '2019-11-20', 'inactive'),
    ('Dave',  2, 600, '2022-03-10', 'active'),
    ('Eve',   3, 1200,'2015-05-01', 'active'),
    ('Frank', NULL, 500,'2023-07-01','active');
INSERT INTO sal (eid, amount, grade) VALUES
    (1,900,'S4'),(2,800,'S3'),(3,700,'S3'),(4,600,'S2'),(5,1200,'S5'),(6,500,'S1');
INSERT INTO org (name, pid) VALUES
    ('CEO',NULL),('CTO',1),('CFO',1),('Lead',2),('Alice',4),('Bob',4);
INSERT INTO tags (val, set_col) VALUES ('a','X,Y'),('b','Z');

-- select: WHERE / ORDER BY / LIMIT / OFFSET / DISTINCT
SELECT id, name, salary FROM emp WHERE salary >= 700 AND status = 'active' ORDER BY salary DESC;
SELECT name FROM emp ORDER BY id LIMIT 3 OFFSET 2;
SELECT DISTINCT status FROM emp ORDER BY status;
SELECT name FROM emp WHERE dept_id IN (1,2) AND salary BETWEEN 600 AND 900;
SELECT name FROM emp WHERE dept_id NOT IN (3) AND salary > 700;
SELECT name FROM emp WHERE name LIKE 'A%' OR name LIKE 'E%';
SELECT name FROM emp WHERE dept_id IS NULL;
SELECT name FROM emp WHERE dept_id IS NOT NULL ORDER BY id;

-- aggregates
SELECT COUNT(*), SUM(amount), AVG(amount), MAX(amount), MIN(amount) FROM sal;
SELECT grade, COUNT(*) AS n, AVG(amount) AS avg_sal FROM sal GROUP BY grade HAVING n >= 1 ORDER BY avg_sal DESC;
SELECT dept_id, GROUP_CONCAT(name SEPARATOR ', ') AS members FROM emp GROUP BY dept_id ORDER BY dept_id;

-- joins
SELECT e.name, d.name AS dept, s.amount, s.grade
    FROM emp e JOIN dept d ON e.dept_id = d.id JOIN sal s ON e.id = s.eid ORDER BY s.amount DESC;
SELECT e.name, d.name AS dept FROM emp e LEFT JOIN dept d ON e.dept_id = d.id ORDER BY e.id;

-- subqueries
SELECT name FROM emp WHERE id IN (SELECT eid FROM sal WHERE amount > 800);
SELECT eid, amount FROM sal WHERE amount > (SELECT AVG(amount) FROM sal);
SELECT name FROM emp WHERE EXISTS (SELECT 1 FROM sal WHERE eid = emp.id AND amount > 1000);
SELECT grade, avg_a FROM (SELECT grade, AVG(amount) AS avg_a FROM sal GROUP BY grade) AS g WHERE avg_a > 700;

-- union
SELECT name FROM emp WHERE dept_id = 1
UNION
SELECT name FROM emp WHERE dept_id = 3;
SELECT eid, amount FROM sal WHERE grade = 'S5'
UNION ALL
SELECT eid, amount FROM sal WHERE grade = 'S1' ORDER BY amount DESC;

-- scalar functions: string
SELECT UPPER(name), LOWER(name), LENGTH(name), CONCAT(name,'@co'), TRIM('  hi  '),
       SUBSTR(name,1,2), REPLACE(name,'Alice','Alex'), LPAD(id,4,'0'), RPAD(name,10,'.')
FROM emp LIMIT 3;

-- scalar functions: math
SELECT salary/100 AS base, ROUND(salary/100,1), ABS(-999), CEIL(3.1), FLOOR(3.9), MOD(salary,7)
FROM emp WHERE id <= 3;
SELECT ROUND(9500000/1000000, 1), ABS(-3.7), CEIL(2.1), FLOOR(2.9);

-- scalar functions: date
SELECT name, hdate, DATEDIFF('2026-05-01', hdate) AS days,
       DATE_ADD(hdate, INTERVAL 1 YEAR) AS nxt,
       DATE_ADD(hdate, INTERVAL 6 MONTH) AS six_mo,
       DATE_FORMAT(hdate, '%Y-%m') AS ym
FROM emp WHERE status = 'active' ORDER BY hdate LIMIT 3;

-- null / cast / scalars without FROM
SELECT COALESCE(dept_id,-1) FROM emp WHERE dept_id IS NULL;
SELECT IFNULL(dept_id,0), NULLIF(dept_id,3) FROM emp ORDER BY id LIMIT 4;
SELECT CAST('2026' AS INT), CAST('3.14' AS FLOAT), CAST(hdate AS TEXT) FROM emp LIMIT 2;
SELECT 1+1, 10*3;

-- CASE / IF
SELECT eid, amount,
    CASE WHEN amount >= 1000 THEN 'Exec' WHEN amount >= 800 THEN 'Senior' ELSE 'Junior' END AS lvl
FROM sal ORDER BY amount DESC;
SELECT name, salary, IF(salary > 800, 'High', 'Normal') AS tier FROM emp ORDER BY salary DESC;

-- CTE
WITH top AS (SELECT eid, amount FROM sal WHERE amount > 800)
SELECT e.name, t.amount FROM top t JOIN emp e ON e.id = t.eid ORDER BY t.amount DESC;

-- WITH RECURSIVE
WITH RECURSIVE h AS (
    SELECT id, name, pid, 0 AS depth FROM org WHERE pid IS NULL
    UNION ALL
    SELECT o.id, o.name, o.pid, h.depth + 1 FROM org o JOIN h ON o.pid = h.id
)
SELECT id, name, depth FROM h ORDER BY depth, id;

-- INSERT .. SELECT / TRUNCATE
CREATE TABLE bak (id INT PRIMARY KEY, eid INT, amount INT);
INSERT INTO bak SELECT id, eid, amount FROM sal WHERE amount > 800;
SELECT * FROM bak ORDER BY amount DESC;
TRUNCATE TABLE bak;
DROP TABLE bak;

-- ALTER TABLE
ALTER TABLE emp ADD COLUMN email VARCHAR(50);
ALTER TABLE emp MODIFY COLUMN email VARCHAR(100);
UPDATE emp SET email = CONCAT(name, '@co.com') WHERE status = 'active';
SELECT id, name, email FROM emp WHERE status = 'active' LIMIT 3;
ALTER TABLE emp RENAME COLUMN email TO contact;
ALTER TABLE emp DROP COLUMN contact;

-- UPDATE arithmetic / scalar fn
UPDATE sal SET amount = amount * 2 WHERE grade = 'S1';
SELECT eid, amount FROM sal WHERE grade = 'S1';
UPDATE sal SET amount = amount / 2 WHERE grade = 'S1';

-- FK CASCADE DELETE
DELETE FROM emp WHERE id = 6;
SELECT * FROM sal WHERE eid = 6; -- 0 rows (CASCADE)

-- INSERT IGNORE / ON DUPLICATE KEY UPDATE
INSERT IGNORE INTO dept (name) VALUES ('Eng');
INSERT IGNORE INTO dept (name) VALUES ('Legal'),('Eng');
SELECT * FROM dept ORDER BY id;
INSERT INTO emp (id, name, dept_id, salary) VALUES (1,'Alice',1,9999)
    ON DUPLICATE KEY UPDATE salary = 9999;
SELECT id, name, salary FROM emp WHERE id = 1;

-- multi-table DELETE
DELETE sal, emp FROM sal JOIN emp ON sal.eid = emp.id WHERE emp.status = 'inactive';
SELECT * FROM emp ORDER BY id;
SELECT * FROM sal ORDER BY id;

-- multi-table UPDATE
UPDATE emp e, dept d SET e.salary = e.salary + 100, d.budget = d.budget + 1000
    WHERE e.dept_id = d.id AND d.id = 1;
SELECT id, name, salary FROM emp WHERE dept_id = 1 ORDER BY id;
SELECT id, name, budget FROM dept WHERE id = 1;
UPDATE emp SET salary = salary - 100 WHERE dept_id = 1;
UPDATE dept SET budget = budget - 1000 WHERE id = 1;

-- ENUM / SET validation
INSERT INTO tags (val, set_col) VALUES ('a','X');       -- ok
INSERT INTO tags (val) VALUES ('bad');                   -- ERROR: invalid ENUM
INSERT INTO tags (val, set_col) VALUES ('b','X,Q');     -- ERROR: invalid SET
UPDATE tags SET val = 'b' WHERE id = 1;                 -- ok
UPDATE tags SET val = 'zzz' WHERE id = 1;               -- ERROR: invalid ENUM
SELECT * FROM tags ORDER BY id;

-- SELECT FOR UPDATE / SHOW LOCKS
SHOW LOCKS;
BEGIN;
SELECT id, name, salary FROM emp WHERE id = 1 FOR UPDATE;
SHOW LOCKS;
UPDATE emp SET salary = salary + 1 WHERE id = 1;
COMMIT;
SHOW LOCKS;
SELECT id, name, salary FROM emp WHERE id = 1;

-- EXPLAIN (covering index / PkPoint)
EXPLAIN SELECT dept_id, salary FROM emp WHERE dept_id = 1;  -- Covering
EXPLAIN SELECT * FROM emp WHERE dept_id = 1;                -- non-Covering
EXPLAIN SELECT * FROM emp WHERE id = 1;                     -- PkPoint

-- VIEW
SELECT * FROM v_active ORDER BY id;

-- TRANSACTION + SAVEPOINT
BEGIN;
INSERT INTO emp (name, dept_id, salary) VALUES ('Tmp', 1, 300);
SAVEPOINT sp1;
UPDATE emp SET salary = 999 WHERE name = 'Tmp';
ROLLBACK TO SAVEPOINT sp1;
COMMIT;
SELECT name, salary FROM emp WHERE name = 'Tmp';
BEGIN;
UPDATE sal SET amount = 1 WHERE id = 1;
ROLLBACK;
SELECT amount FROM sal WHERE id = 1;

-- ISOLATION LEVEL
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;
SHOW ISOLATION LEVEL;

-- CHECKPOINT / VACUUM / SHOW
CHECKPOINT;
VACUUM;
SHOW BUFFER POOL;
SHOW WAL;
SHOW LOCKS;
SHOW DATABASES;

-- user management
CREATE USER 'usr'@'%' IDENTIFIED BY 'pw';
CREATE USER IF NOT EXISTS 'usr'@'%';
GRANT SELECT, INSERT ON db1.emp TO 'usr'@'%';
SHOW GRANTS FOR 'usr'@'%';
REVOKE INSERT ON db1.emp FROM 'usr'@'%';
SHOW GRANTS FOR 'usr'@'%';
DROP USER 'usr'@'%';
DROP USER IF EXISTS 'nobody'@'%';

-- cleanup
DROP VIEW  IF EXISTS v_active;
DROP INDEX IF EXISTS idx_dept;
DROP INDEX IF EXISTS idx_ds;
DROP TABLE IF EXISTS tags;
DROP TABLE IF EXISTS org;
DROP TABLE IF EXISTS sal;
DROP TABLE IF EXISTS emp;
DROP TABLE IF EXISTS dept;
DROP DATABASE db1;
SHOW DATABASES;
