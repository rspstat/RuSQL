-- SETUP
DROP TABLE IF EXISTS emp;
DROP TABLE IF EXISTS dept;
DROP VIEW  IF EXISTS v_hi;
DROP INDEX IF EXISTS idx_dept;

-- CREATE
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
    score   INT,
    active  INT DEFAULT 1,
    FOREIGN KEY (dept_id) REFERENCES dept(id) ON DELETE SET NULL ON UPDATE CASCADE
);

-- INSERT
INSERT INTO dept (name, budget) VALUES ('Eng', 500), ('Mkt', 200), ('HR', 100);
INSERT INTO emp (name, dept_id, salary, score, active) VALUES
    ('Alice', 1, 9500, 90, 1), ('Bob', 1, 8500, 75, 1),
    ('Carol', 2, 7200, 88, 0), ('Dave', 2, 6800, 60, 1), ('Eve', 3, 6000, 95, 1);

-- SELECT / ORDER BY / LIMIT / OFFSET / DISTINCT
SELECT id, name, salary FROM emp WHERE salary > 7000 ORDER BY salary DESC LIMIT 3;
SELECT id, name FROM emp ORDER BY id LIMIT 2 OFFSET 2;
SELECT DISTINCT dept_id FROM emp ORDER BY dept_id;

-- ARITHMETIC in SELECT / WHERE
SELECT id, name, salary * 2 AS doubled, salary + score AS combined FROM emp ORDER BY id;
SELECT name FROM emp WHERE salary * 2 > 16000;

-- IN / NOT IN / NOT / BETWEEN / LIKE
SELECT name FROM emp WHERE id IN (1, 3, 5);
SELECT name FROM emp WHERE id NOT IN (2, 4);
SELECT name FROM emp WHERE NOT (active = 1);
SELECT name FROM emp WHERE salary BETWEEN 7000 AND 9000;
SELECT name FROM emp WHERE name LIKE 'A%' OR name LIKE 'E%';

-- IS NULL / IS NOT NULL
INSERT INTO emp (name, salary, score) VALUES ('Frank', 5500, 72);
SELECT id, name FROM emp WHERE dept_id IS NULL;
SELECT id, name FROM emp WHERE dept_id IS NOT NULL ORDER BY id;

-- AGGREGATE / GROUP BY / HAVING
SELECT COUNT(*) AS total, AVG(salary) AS avg_sal, MAX(score) AS top, MIN(score) AS bot, SUM(salary) AS payroll FROM emp;
SELECT dept_id, COUNT(*) AS cnt, SUM(salary) AS pay FROM emp WHERE dept_id IS NOT NULL GROUP BY dept_id HAVING cnt > 1;

-- JOIN
SELECT e.name, d.name AS dept, e.salary FROM emp e JOIN dept d ON e.dept_id = d.id;
SELECT e.name, d.name AS dept FROM emp e LEFT JOIN dept d ON e.dept_id = d.id;

-- SUBQUERY
SELECT name FROM emp WHERE dept_id IN (SELECT id FROM dept WHERE budget > 300);
SELECT name FROM emp WHERE salary > (SELECT AVG(salary) FROM emp);
SELECT name FROM dept WHERE EXISTS (SELECT 1 FROM emp WHERE dept_id = dept.id AND salary > 9000);
SELECT dept_id, avg_s FROM (SELECT dept_id, AVG(salary) AS avg_s FROM emp GROUP BY dept_id) AS s WHERE avg_s > 7000;

-- UNION / UNION ALL
SELECT dept_id FROM emp WHERE salary > 7000 AND dept_id IS NOT NULL
UNION
SELECT dept_id FROM emp WHERE score  > 85  AND dept_id IS NOT NULL;

SELECT name, score FROM emp WHERE score >= 88
UNION ALL
SELECT name, score FROM emp WHERE score <  65
ORDER BY score DESC;

-- SCALAR FUNCTIONS
SELECT UPPER(name) AS up, LENGTH(name) AS len, CONCAT(name, '@co') AS email FROM emp LIMIT 3;
SELECT COALESCE(dept_id, 0) AS dept, IFNULL(dept_id, 0) AS dept2 FROM emp WHERE dept_id IS NULL;

-- CASE WHEN
SELECT name, CASE WHEN salary > 8000 THEN 'High' WHEN salary > 6000 THEN 'Mid' ELSE 'Low' END AS band FROM emp ORDER BY id;

-- UPDATE
UPDATE emp SET salary = 10000 WHERE id = 1;
UPDATE emp SET salary = salary * 2, score = score + 5 WHERE id = 2;
SELECT id, name, salary, score FROM emp WHERE id IN (1, 2);

-- DELETE + FK SET NULL
DELETE FROM dept WHERE id = 3;
SELECT id, name, dept_id FROM emp WHERE name = 'Eve';

-- CONSTRAINT ERROR (expected ERROR)
INSERT INTO emp (name, salary) VALUES ('Bad', -1);

-- INSERT ... SELECT
CREATE TABLE archive (id INT PRIMARY KEY, name VARCHAR(30), salary INT);
INSERT INTO archive SELECT id, name, salary FROM emp WHERE salary >= 10000;
SELECT * FROM archive ORDER BY salary DESC;

-- CTE
WITH hi AS (SELECT name, score FROM emp WHERE score >= 88)
SELECT name, score FROM hi ORDER BY score DESC;

WITH hi AS (SELECT name, score FROM emp WHERE score >= 88),
     lo AS (SELECT name, score FROM emp WHERE score <  65)
SELECT name, score FROM hi UNION ALL SELECT name, score FROM lo ORDER BY score DESC;

WITH mid AS (
    SELECT id, name, salary FROM emp WHERE salary BETWEEN 6000 AND 9999
    AND id NOT IN (SELECT id FROM archive)
)
INSERT INTO archive SELECT id, name, salary FROM mid;
SELECT * FROM archive ORDER BY salary DESC;

TRUNCATE TABLE archive;
DROP TABLE archive;

-- VIEW
CREATE VIEW v_hi AS SELECT id, name, salary FROM emp WHERE salary > 8000;
SELECT * FROM v_hi ORDER BY salary DESC;
DROP VIEW IF EXISTS v_hi;

-- INDEX + EXPLAIN
CREATE INDEX idx_dept ON emp (dept_id);
EXPLAIN SELECT * FROM emp WHERE dept_id = 1;
EXPLAIN SELECT * FROM emp WHERE salary > 7000;

-- ALTER TABLE
ALTER TABLE emp ADD COLUMN note TEXT;
ALTER TABLE emp MODIFY COLUMN note VARCHAR(100);
ALTER TABLE emp RENAME COLUMN note TO memo;
ALTER TABLE emp DROP COLUMN memo;
DESCRIBE emp;

-- TRANSACTION (savepoint)
BEGIN;
INSERT INTO dept (name, budget) VALUES ('Temp', 0);
SAVEPOINT sp1;
UPDATE dept SET budget = 999 WHERE name = 'Temp';
ROLLBACK TO SAVEPOINT sp1;
COMMIT;
SELECT name, budget FROM dept WHERE name = 'Temp';

-- TRANSACTION ROLLBACK
BEGIN;
UPDATE emp SET salary = 1 WHERE id = 1;
ROLLBACK;
SELECT id, salary FROM emp WHERE id = 1;

-- ISOLATION LEVEL
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;

-- SHOW / ADMIN
SHOW TABLES;
DESCRIBE emp;
SHOW BUFFER POOL;
SHOW WAL;
SHOW LOCKS;
CHECKPOINT;
VACUUM;

-- CLEANUP
DROP INDEX IF EXISTS idx_dept;
DROP TABLE emp;
DROP TABLE dept;
SHOW TABLES;
