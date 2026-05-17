-- RustDB 통합 테스트 (전 기능, 최소 길이)

-- ── Setup ──────────────────────────────────────────────────────────────────
DROP USER IF EXISTS 'usr'@'%';
DROP DATABASE IF EXISTS db1;
SHOW DATABASES;
CREATE DATABASE db1;
USE db1;

-- ── DDL ─────────────────────────────────────────────────────────────────────
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
    id      INT PRIMARY KEY AUTO INCREMENT,
    val     ENUM('a','b','c'),
    set_col SET('X','Y','Z')
);
CREATE TABLE nums (
    id       INT PRIMARY KEY AUTO INCREMENT,
    big_val  BIGINT,
    small_val SMALLINT,
    tiny_val  TINYINT
);
CREATE TABLE jdata (
    id   INT PRIMARY KEY AUTO INCREMENT,
    info JSON
);
CREATE TABLE audit_log (
    id  INT PRIMARY KEY AUTO INCREMENT,
    msg VARCHAR(100)
);
CREATE INDEX idx_dept ON emp (dept_id);
CREATE INDEX idx_ds   ON emp (dept_id, salary);
CREATE VIEW v_active AS SELECT id, name, dept_id FROM emp WHERE status = 'active';

SHOW TABLES;
DESCRIBE emp;

-- ── DML: insert ─────────────────────────────────────────────────────────────
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
INSERT INTO nums (big_val, small_val, tiny_val) VALUES (9223372036854775807, 32767, 127);
INSERT INTO jdata (info) VALUES ('{"name":"Alice","age":30,"score":95.5}'),('{"name":"Bob","age":25,"score":80.0}');

-- ── SELECT ──────────────────────────────────────────────────────────────────
SELECT id, name, salary FROM emp WHERE salary >= 700 AND status = 'active' ORDER BY salary DESC;
SELECT name FROM emp ORDER BY id LIMIT 3 OFFSET 2;
SELECT DISTINCT status FROM emp ORDER BY status;
SELECT name FROM emp WHERE dept_id IN (1,2) AND salary BETWEEN 600 AND 900;
SELECT name FROM emp WHERE dept_id NOT IN (3) AND salary > 700;
SELECT name FROM emp WHERE name LIKE 'A%' OR name LIKE 'E%';
SELECT name FROM emp WHERE dept_id IS NULL;

-- ── 정규식 ──────────────────────────────────────────────────────────────────
SELECT name FROM emp WHERE name REGEXP '^[AB]';
SELECT name FROM emp WHERE name RLIKE 'e$';
SELECT name, REGEXP_LIKE(name, '^A') AS starts_a FROM emp ORDER BY id LIMIT 3;
SELECT name, REGEXP_REPLACE(name, 'a', '@') AS replaced FROM emp ORDER BY id LIMIT 3;
SELECT REGEXP_MATCH('Alice123', '[0-9]+') AS mat;

-- ── 집계 ────────────────────────────────────────────────────────────────────
SELECT COUNT(*), SUM(amount), AVG(amount), MAX(amount), MIN(amount) FROM sal;
SELECT grade, COUNT(*) AS n, AVG(amount) AS avg_sal FROM sal GROUP BY grade HAVING n >= 1 ORDER BY avg_sal DESC;
SELECT dept_id, GROUP_CONCAT(name SEPARATOR ', ') AS members FROM emp GROUP BY dept_id ORDER BY dept_id;
SELECT COUNT(DISTINCT grade), SUM(DISTINCT amount) FROM sal;
SELECT STDDEV(amount), VARIANCE(amount) FROM sal;

-- ── JOIN ────────────────────────────────────────────────────────────────────
SELECT e.name, d.name AS dept, s.amount FROM emp e JOIN dept d ON e.dept_id = d.id JOIN sal s ON e.id = s.eid ORDER BY s.amount DESC;
SELECT e.name, d.name AS dept FROM emp e LEFT JOIN dept d ON e.dept_id = d.id ORDER BY e.id;
SELECT d.name AS dept_name, e.name AS emp_name FROM dept d CROSS JOIN emp e ORDER BY d.name, e.name LIMIT 9;
SELECT e.name, s.amount FROM emp e NATURAL JOIN sal s ORDER BY e.name LIMIT 4;

-- ── 서브쿼리 ────────────────────────────────────────────────────────────────
SELECT name FROM emp WHERE id IN (SELECT eid FROM sal WHERE amount > 800);
SELECT eid, amount FROM sal WHERE amount > (SELECT AVG(amount) FROM sal);
SELECT name FROM emp WHERE EXISTS (SELECT 1 FROM sal WHERE eid = emp.id AND amount > 1000);
SELECT grade, avg_a FROM (SELECT grade, AVG(amount) AS avg_a FROM sal GROUP BY grade) AS g WHERE avg_a > 700;
SELECT name, salary, (SELECT MAX(salary) FROM emp) AS max_sal FROM emp ORDER BY salary DESC LIMIT 3;

-- ── UNION / INTERSECT / EXCEPT ──────────────────────────────────────────────
SELECT name FROM emp WHERE dept_id = 1 UNION SELECT name FROM emp WHERE dept_id = 3;
SELECT eid FROM sal WHERE grade = 'S5' UNION ALL SELECT eid FROM sal WHERE grade = 'S1' ORDER BY eid;
SELECT eid FROM sal WHERE amount > 700 INTERSECT SELECT eid FROM sal WHERE grade != 'S1';
SELECT eid FROM sal EXCEPT SELECT eid FROM sal WHERE amount < 700;

-- ── CTE / 재귀 CTE ───────────────────────────────────────────────────────────
WITH top AS (SELECT eid, amount FROM sal WHERE amount > 800)
SELECT e.name, t.amount FROM top t JOIN emp e ON e.id = t.eid ORDER BY t.amount DESC;

WITH RECURSIVE h AS (
    SELECT id, name, pid, 0 AS depth FROM org WHERE pid IS NULL
    UNION ALL
    SELECT o.id, o.name, o.pid, h.depth + 1 FROM org o JOIN h ON o.pid = h.id
)
SELECT id, name, depth FROM h ORDER BY depth, id;

-- ── 윈도우 함수 (기본) ──────────────────────────────────────────────────────
SELECT name, salary,
    ROW_NUMBER() OVER (ORDER BY salary DESC) AS rn,
    RANK()       OVER (PARTITION BY dept_id ORDER BY salary DESC) AS rnk,
    DENSE_RANK() OVER (PARTITION BY dept_id ORDER BY salary DESC) AS drnk
FROM emp WHERE dept_id IS NOT NULL ORDER BY dept_id, salary DESC;

SELECT name, dept_id, salary,
    LAG(salary, 1)  OVER (PARTITION BY dept_id ORDER BY salary) AS prev_sal,
    LEAD(salary, 1) OVER (PARTITION BY dept_id ORDER BY salary) AS next_sal,
    FIRST_VALUE(salary) OVER (PARTITION BY dept_id ORDER BY salary DESC) AS top_sal,
    LAST_VALUE(salary)  OVER (PARTITION BY dept_id ORDER BY salary DESC) AS bot_sal
FROM emp WHERE dept_id IS NOT NULL ORDER BY dept_id, salary;

-- ── 윈도우 함수 ROWS/RANGE 프레임 ─────────────────────────────────────────
SELECT eid, amount,
    SUM(amount) OVER (ORDER BY eid ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_sum,
    AVG(amount) OVER (ORDER BY eid ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS moving_avg,
    COUNT(*)    OVER (ORDER BY eid ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS cnt,
    MIN(amount) OVER (ORDER BY eid ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS win_min,
    MAX(amount) OVER (ORDER BY eid ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS win_max
FROM sal ORDER BY eid;

SELECT eid, amount,
    SUM(amount) OVER (PARTITION BY grade ORDER BY eid RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS part_sum
FROM sal ORDER BY grade, eid;

-- ── 스칼라 함수 ─────────────────────────────────────────────────────────────
SELECT UPPER(name), LOWER(name), LENGTH(name), CONCAT(name,'@co'), TRIM('  hi  '),
       SUBSTR(name,1,2), REPLACE(name,'Alice','Alex'), LPAD(id,4,'0'), RPAD(name,10,'.') FROM emp LIMIT 3;
SELECT salary/100 AS base, ROUND(salary/100,1), ABS(-999), CEIL(3.1), FLOOR(3.9), MOD(salary,7) FROM emp LIMIT 3;
SELECT name, hdate, DATEDIFF('2026-05-01', hdate) AS days, DATE_FORMAT(hdate, '%Y-%m') AS ym FROM emp LIMIT 3;
SELECT COALESCE(dept_id,-1) FROM emp WHERE dept_id IS NULL;
SELECT CAST('2026' AS INT), CAST('3.14' AS FLOAT);
SELECT IF(salary > 800, 'High', 'Normal') AS tier, CASE WHEN salary >= 1000 THEN 'Exec' ELSE 'Other' END AS lvl FROM emp LIMIT 4;

-- ── 추가 스칼라 함수 (수학) ───────────────────────────────────────────────────
SELECT SQRT(144), POW(2,10), LOG(1), LOG2(8), LOG10(100), EXP(0);
SELECT SIN(0), COS(0), TAN(0), PI(), SIGN(-5), SIGN(0), SIGN(3);
SELECT TRUNCATE(3.789, 1), TRUNCATE(3.789, 0), RAND() >= 0;

-- ── 추가 스칼라 함수 (문자열) ─────────────────────────────────────────────────
SELECT CHAR_LENGTH('hello'), LEFT('hello',3), RIGHT('hello',3), REVERSE('hello');
SELECT REPEAT('ab',3), INSTR('hello','ll'), LOCATE('ll','hello');
SELECT LTRIM('  hi'), RTRIM('hi  '), SPACE(4), ASCII('A'), HEX(255);
SELECT FORMAT(1234567.891, 2);
SELECT name, CHAR_LENGTH(name) AS clen, LEFT(name,2) AS l2, RIGHT(name,2) AS r2 FROM emp LIMIT 3;

-- ── 추가 스칼라 함수 (날짜) ──────────────────────────────────────────────────
SELECT YEAR(hdate), MONTH(hdate), DAY(hdate), DAYOFWEEK(hdate), DAYOFYEAR(hdate), WEEKDAY(hdate) FROM emp LIMIT 3;
SELECT TIMESTAMPDIFF(YEAR, hdate, '2026-05-01') AS yrs FROM emp LIMIT 3;
SELECT DATE_SUB('2026-05-01', INTERVAL 30 DAY) AS past;
SELECT UNIX_TIMESTAMP('2024-01-01') AS uts;

-- ── 추가 스칼라 함수 (조건 / 기타) ──────────────────────────────────────────
SELECT GREATEST(1,5,3), LEAST(1,5,3);
SELECT ISNULL(dept_id) AS is_null FROM emp WHERE dept_id IS NULL LIMIT 1;
SELECT MD5('hello') AS md5_hello;
SELECT LENGTH(UUID()) > 0 AS uuid_ok;

-- ── DML: UPDATE / DELETE / INSERT variants ────────────────────────────────
INSERT INTO dept (name) VALUES ('Legal');
INSERT IGNORE INTO dept (name) VALUES ('Eng');
INSERT INTO emp (id, name, dept_id, salary) VALUES (1,'Alice',1,9999) ON DUPLICATE KEY UPDATE salary = 9999;
SELECT id, name, salary FROM emp WHERE id = 1;

CREATE TABLE bak (id INT PRIMARY KEY, eid INT, amount INT);
INSERT INTO bak SELECT id, eid, amount FROM sal WHERE amount > 800;
SELECT * FROM bak ORDER BY amount DESC;
TRUNCATE TABLE bak;
DROP TABLE bak;

ALTER TABLE emp ADD COLUMN email VARCHAR(50);
UPDATE emp SET email = CONCAT(name,'@co.com') WHERE status = 'active';
SELECT id, name, email FROM emp LIMIT 3;
ALTER TABLE emp RENAME COLUMN email TO contact;
ALTER TABLE emp DROP COLUMN contact;

UPDATE sal SET amount = amount * 2 WHERE grade = 'S1';
SELECT eid, amount FROM sal WHERE grade = 'S1';
UPDATE sal SET amount = amount / 2 WHERE grade = 'S1';

-- FK CASCADE
DELETE FROM emp WHERE id = 6;
SELECT * FROM sal WHERE eid = 6;

-- multi-table UPDATE / DELETE
UPDATE emp e, dept d SET e.salary = e.salary + 100, d.budget = d.budget + 1000 WHERE e.dept_id = d.id AND d.id = 1;
SELECT id, name, salary FROM emp WHERE dept_id = 1 ORDER BY id;
UPDATE emp SET salary = salary - 100 WHERE dept_id = 1;
DELETE sal, emp FROM sal JOIN emp ON sal.eid = emp.id WHERE emp.status = 'inactive';

-- RETURNING
INSERT INTO dept (name, budget) VALUES ('Ops', 500) RETURNING id, name;
DELETE FROM dept WHERE name = 'Ops' RETURNING id, name;

-- ENUM/SET validation
INSERT INTO tags (val, set_col) VALUES ('a','X');
INSERT INTO tags (val) VALUES ('bad');           -- ERROR
INSERT INTO tags (val, set_col) VALUES ('b','X,Q'); -- ERROR
SELECT * FROM tags ORDER BY id;

-- ── MERGE INTO ───────────────────────────────────────────────────────────────
CREATE TABLE dept_new (
    id     INT PRIMARY KEY AUTO INCREMENT,
    name   VARCHAR(30) NOT NULL UNIQUE,
    budget INT DEFAULT 0
);
INSERT INTO dept_new (name, budget) VALUES ('Eng', 9999), ('HR', 500);
MERGE INTO dept USING dept_new ON dept.name = dept_new.name
    WHEN MATCHED THEN UPDATE SET budget = dept_new.budget
    WHEN NOT MATCHED THEN INSERT (name, budget) VALUES (dept_new.name, dept_new.budget);
SELECT id, name, budget FROM dept ORDER BY id;
DROP TABLE dept_new;

-- ── 저장 프로시저 ────────────────────────────────────────────────────────────
CREATE PROCEDURE reset_budget() UPDATE dept SET budget = 0 WHERE budget > 5000;
CALL reset_budget();
SELECT id, name, budget FROM dept ORDER BY id;
DROP PROCEDURE IF EXISTS reset_budget;

CREATE PROCEDURE insert_dept(IN dname VARCHAR(30), IN dbudget INT) INSERT INTO dept (name, budget) VALUES ('ProcTest', 777);
CALL insert_dept('ignored', 0);
SELECT name, budget FROM dept WHERE name = 'ProcTest';
DELETE FROM dept WHERE name = 'ProcTest';
DROP PROCEDURE insert_dept;

-- ── 저장 프로시저 제어문 ─────────────────────────────────────────────────────

-- DECLARE + SET + IF/ELSEIF/ELSE
CREATE PROCEDURE test_if(IN n INT)
BEGIN
    DECLARE res VARCHAR(20) DEFAULT 'zero';
    IF n > 0 THEN
        SET res = 'positive';
    ELSEIF n < 0 THEN
        SET res = 'negative';
    END IF;
    SELECT res;
END;
CALL test_if(5);
CALL test_if(-3);
CALL test_if(0);
DROP PROCEDURE test_if;

-- WHILE: sum 1..5 = 15
CREATE PROCEDURE test_while()
BEGIN
    DECLARE i INT DEFAULT 1;
    DECLARE s INT DEFAULT 0;
    WHILE i <= 5 DO
        SET s = s + i;
        SET i = i + 1;
    END WHILE;
    SELECT s AS while_sum;
END;
CALL test_while();
DROP PROCEDURE test_while;

-- LOOP / LEAVE
CREATE PROCEDURE test_loop()
BEGIN
    DECLARE i INT DEFAULT 0;
    lp: LOOP
        SET i = i + 1;
        IF i >= 3 THEN
            LEAVE lp;
        END IF;
    END LOOP;
    SELECT i AS loop_result;
END;
CALL test_loop();
DROP PROCEDURE test_loop;

-- REPEAT / UNTIL
CREATE PROCEDURE test_repeat()
BEGIN
    DECLARE n INT DEFAULT 0;
    REPEAT
        SET n = n + 1;
    UNTIL n >= 5 END REPEAT;
    SELECT n AS repeat_result;
END;
CALL test_repeat();
DROP PROCEDURE test_repeat;

-- ── 트리거 ───────────────────────────────────────────────────────────────────
CREATE TRIGGER trg_dept_insert AFTER INSERT ON dept FOR EACH ROW INSERT INTO audit_log (msg) VALUES ('dept_inserted');

INSERT INTO dept (name, budget) VALUES ('Trigger_test', 100);
SELECT msg FROM audit_log ORDER BY id;
DELETE FROM dept WHERE name = 'Trigger_test';
DROP TRIGGER IF EXISTS trg_dept_insert;

-- ── 트랜잭션 / SAVEPOINT ─────────────────────────────────────────────────────
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

-- ── 격리 수준 ────────────────────────────────────────────────────────────────
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;

-- ── SELECT FOR UPDATE / FOR SHARE / SHOW LOCKS ────────────────────────────
BEGIN;
SELECT id, name, salary FROM emp WHERE id = 1 FOR UPDATE;
SHOW LOCKS;
COMMIT;

BEGIN;
SELECT id, name, salary FROM emp WHERE id = 1 FOR SHARE;
SELECT id, name, salary FROM emp WHERE id = 2 FOR SHARE;
SHOW LOCKS;
COMMIT;

-- ── EXPLAIN / ANALYZE TABLE ─────────────────────────────────────────────────
EXPLAIN SELECT dept_id, salary FROM emp WHERE dept_id = 1;
EXPLAIN SELECT * FROM emp WHERE id = 1;
ANALYZE TABLE emp;
EXPLAIN ANALYZE SELECT * FROM emp WHERE dept_id = 1;

-- ── VIEW / Updatable View ────────────────────────────────────────────────────
SELECT * FROM v_active ORDER BY id;
SHOW CREATE TABLE emp;

-- Updatable View: UPDATE/DELETE via simple view (no JOIN/DISTINCT/GROUP BY)
UPDATE v_active SET salary = salary + 1 WHERE name = 'Alice';
SELECT id, name, salary FROM emp WHERE name = 'Alice';
UPDATE v_active SET salary = salary - 1 WHERE name = 'Alice';

-- ── 사용자 정의 함수 (CREATE FUNCTION) ───────────────────────────────────────
CREATE FUNCTION triple(x) RETURNS INT RETURN x * 3;
SELECT name, salary, triple(salary) AS tripled FROM emp LIMIT 3;
DROP FUNCTION triple;

CREATE FUNCTION greet(n) RETURNS VARCHAR(50) RETURN CONCAT('Hello, ', n);
SELECT greet(name) AS greeting FROM emp LIMIT 2;
DROP FUNCTION greet;

-- ── BIGINT / SMALLINT / TINYINT ─────────────────────────────────────────────
SELECT big_val, small_val, tiny_val FROM nums;
DESCRIBE nums;

-- ── JSON 데이터 타입 ──────────────────────────────────────────────────────────
SELECT id, info FROM jdata ORDER BY id;
SELECT id, info->'$.name' AS jname, info->>'$.age' AS age FROM jdata ORDER BY id;
SELECT id, JSON_EXTRACT(info, '$.score') AS score FROM jdata ORDER BY id;
SELECT id, JSON_VALUE(info, '$.name') AS nm FROM jdata ORDER BY id;
DESCRIBE jdata;

-- ── INFORMATION_SCHEMA ──────────────────────────────────────────────────────
SELECT table_name, table_rows FROM information_schema.tables WHERE table_schema = 'db1' ORDER BY table_name LIMIT 5;
SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'emp' ORDER BY ordinal_position LIMIT 5;

-- ── 사용자 관리 ──────────────────────────────────────────────────────────────
CREATE USER 'usr'@'%' IDENTIFIED BY 'pw';
GRANT SELECT, INSERT ON db1.emp TO 'usr'@'%';
SHOW GRANTS FOR 'usr'@'%';
REVOKE INSERT ON db1.emp FROM 'usr'@'%';
DROP USER 'usr'@'%';

-- ── 모니터링 ─────────────────────────────────────────────────────────────────
CHECKPOINT;
VACUUM;
SHOW BUFFER POOL;
SHOW WAL;
SHOW LOCKS;
SHOW PROCESSLIST;
SHOW DATABASES;

-- ── BACKUP ───────────────────────────────────────────────────────────────────
BACKUP DATABASE db1;

-- ── Cleanup ──────────────────────────────────────────────────────────────────
DROP VIEW  IF EXISTS v_active;
DROP INDEX IF EXISTS idx_dept;
DROP INDEX IF EXISTS idx_ds;
DROP TABLE IF EXISTS jdata;
DROP TABLE IF EXISTS nums;
DROP TABLE IF EXISTS audit_log;
DROP TABLE IF EXISTS tags;
DROP TABLE IF EXISTS org;
DROP TABLE IF EXISTS sal;
DROP TABLE IF EXISTS emp;
DROP TABLE IF EXISTS dept;
DROP DATABASE db1;
SHOW DATABASES;
