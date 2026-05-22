-- RustDB 통합 테스트 v2.2.0

-- 초기화
DROP USER IF EXISTS 'usr'@'%';
DROP DATABASE IF EXISTS db1;
CREATE DATABASE db1;
USE db1;

-- DDL
CREATE TABLE dept (
    id     INT AUTO INCREMENT,
    name   VARCHAR(30) NOT NULL,
    budget INT DEFAULT 0,
    CONSTRAINT pk_dept PRIMARY KEY (id),
    UNIQUE KEY uk_name (name)
);
CREATE TABLE emp (
    id      INT AUTO INCREMENT,
    name    VARCHAR(30) NOT NULL,
    dept_id INT,
    salary  INT CHECK (salary > 0),
    hdate   DATE,
    status  ENUM('active','inactive') DEFAULT 'active',
    CONSTRAINT pk_emp PRIMARY KEY (id),
    CONSTRAINT fk_emp FOREIGN KEY (dept_id) REFERENCES dept(id) ON DELETE SET NULL
);
CREATE TABLE sal (
    id     INT AUTO INCREMENT,
    eid    INT,
    amount INT CHECK (amount > 0),
    grade  ENUM('S1','S2','S3','S4','S5'),
    CONSTRAINT pk_sal PRIMARY KEY (id),
    CONSTRAINT fk_sal FOREIGN KEY (eid) REFERENCES emp(id) ON DELETE CASCADE,
    INDEX idx_sal_grade (grade)
);
CREATE TABLE org (id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(30), pid INT);
CREATE TABLE tags (id INT PRIMARY KEY AUTO INCREMENT, val ENUM('a','b','c'), set_col SET('X','Y','Z'));
CREATE TABLE nums (id INT PRIMARY KEY AUTO INCREMENT, big_val BIGINT, small_val SMALLINT, tiny_val TINYINT);
CREATE TABLE jdata (id INT PRIMARY KEY AUTO INCREMENT, info JSON);
CREATE TABLE audit_log (id INT PRIMARY KEY AUTO INCREMENT, msg VARCHAR(100));
CREATE INDEX idx_dept ON emp (dept_id);
CREATE INDEX idx_ds ON emp (dept_id, salary);
CREATE VIEW v_active AS SELECT id, name, dept_id FROM emp WHERE status = 'active';

-- DDL 확인
SHOW TABLES;
DESCRIBE emp;
SHOW INDEX FROM emp;
SHOW CREATE TABLE emp;
SHOW CREATE VIEW v_active;
CREATE DATABASE IF NOT EXISTS db1;
CREATE TABLE IF NOT EXISTS dept (dummy INT);

-- INSERT
INSERT INTO dept (name, budget) VALUES ('Eng',1000),('Mkt',800),('Fin',1200);
INSERT INTO emp (name, dept_id, salary, hdate, status) VALUES
    ('Alice',1,900,'2020-01-15','active'),('Bob',1,800,'2021-06-01','active'),
    ('Carol',2,700,'2019-11-20','inactive'),('Dave',2,600,'2022-03-10','active'),
    ('Eve',3,1200,'2015-05-01','active'),('Frank',NULL,500,'2023-07-01','active');
INSERT INTO sal (eid, amount, grade) VALUES (1,900,'S4'),(2,800,'S3'),(3,700,'S3'),(4,600,'S2'),(5,1200,'S5'),(6,500,'S1');
INSERT INTO org (name, pid) VALUES ('CEO',NULL),('CTO',1),('CFO',1),('Lead',2),('Alice',4),('Bob',4);
INSERT INTO tags (val, set_col) VALUES ('a','X,Y'),('b','Z');
INSERT INTO nums (big_val, small_val, tiny_val) VALUES (9223372036854775807,32767,127);
INSERT INTO jdata (info) VALUES ('{"name":"Alice","age":30,"score":95.5}'),('{"name":"Bob","age":25}');

-- SELECT
SELECT id, name, salary FROM emp WHERE salary >= 700 AND status = 'active' ORDER BY salary DESC;
SELECT name FROM emp ORDER BY id LIMIT 3 OFFSET 1;
SELECT DISTINCT status FROM emp ORDER BY status;
SELECT name FROM emp WHERE dept_id IN (1,2) AND salary BETWEEN 600 AND 900;
SELECT name FROM emp WHERE name LIKE 'A%' OR dept_id IS NULL;
SELECT name FROM emp WHERE name REGEXP '^[AB]';
SELECT name, REGEXP_LIKE(name,'^A') AS sa, REGEXP_REPLACE(name,'a','@') AS rr FROM emp LIMIT 2;

-- 집계
SELECT COUNT(*), SUM(amount), AVG(amount), MAX(amount), MIN(amount) FROM sal;
SELECT grade, COUNT(*) AS n, AVG(amount) AS avg_sal FROM sal GROUP BY grade HAVING n >= 1 ORDER BY avg_sal DESC;
SELECT dept_id, GROUP_CONCAT(name SEPARATOR ', ') AS members FROM emp GROUP BY dept_id ORDER BY dept_id;
SELECT COUNT(DISTINCT grade), SUM(DISTINCT amount), STDDEV(amount), VARIANCE(amount) FROM sal;

-- JOIN
SELECT e.name, d.name AS dept, s.amount FROM emp e JOIN dept d ON e.dept_id=d.id JOIN sal s ON e.id=s.eid ORDER BY s.amount DESC;
SELECT e.name, d.name FROM emp e LEFT JOIN dept d ON e.dept_id=d.id ORDER BY e.id;
SELECT d.name, e.name FROM dept d RIGHT JOIN emp e ON d.id=e.dept_id ORDER BY e.id LIMIT 3;
SELECT d.name, e.name FROM dept d FULL OUTER JOIN emp e ON d.id=e.dept_id ORDER BY e.id LIMIT 5;
SELECT d.name, e.name FROM dept d CROSS JOIN emp e ORDER BY d.name LIMIT 6;
SELECT e.name, s.amount FROM emp e NATURAL JOIN sal s ORDER BY e.name LIMIT 3;

-- 서브쿼리
SELECT name FROM emp WHERE id IN (SELECT eid FROM sal WHERE amount > 800);
SELECT eid, amount FROM sal WHERE amount > (SELECT AVG(amount) FROM sal);
SELECT name FROM emp WHERE EXISTS (SELECT 1 FROM sal WHERE eid=emp.id AND amount > 1000);
SELECT grade, avg_a FROM (SELECT grade, AVG(amount) AS avg_a FROM sal GROUP BY grade) AS g WHERE avg_a > 700;
SELECT name, (SELECT MAX(salary) FROM emp) AS max_sal FROM emp ORDER BY salary DESC LIMIT 2;

-- UNION / INTERSECT / EXCEPT
SELECT name FROM emp WHERE dept_id=1 UNION SELECT name FROM emp WHERE dept_id=3;
SELECT eid FROM sal WHERE grade='S5' UNION ALL SELECT eid FROM sal WHERE grade='S1' ORDER BY eid;
SELECT eid FROM sal WHERE amount > 700 INTERSECT SELECT eid FROM sal WHERE grade != 'S1';
SELECT eid FROM sal EXCEPT SELECT eid FROM sal WHERE amount < 700;

-- CTE
WITH top AS (SELECT eid, amount FROM sal WHERE amount > 800)
SELECT e.name, t.amount FROM top t JOIN emp e ON e.id=t.eid ORDER BY t.amount DESC;

-- 재귀 CTE
WITH RECURSIVE h AS (
    SELECT id, name, pid, 0 AS depth FROM org WHERE pid IS NULL
    UNION ALL
    SELECT o.id, o.name, o.pid, h.depth+1 FROM org o JOIN h ON o.pid=h.id
)
SELECT id, name, depth FROM h ORDER BY depth, id;

-- 윈도우 함수
SELECT name, salary,
    ROW_NUMBER() OVER (ORDER BY salary DESC) AS rn,
    RANK() OVER (PARTITION BY dept_id ORDER BY salary DESC) AS rnk,
    DENSE_RANK() OVER (PARTITION BY dept_id ORDER BY salary DESC) AS drnk,
    LAG(salary,1) OVER (PARTITION BY dept_id ORDER BY salary) AS prev,
    LEAD(salary,1) OVER (PARTITION BY dept_id ORDER BY salary) AS nxt,
    FIRST_VALUE(salary) OVER (PARTITION BY dept_id ORDER BY salary DESC) AS top_sal
FROM emp WHERE dept_id IS NOT NULL ORDER BY dept_id, salary DESC;
SELECT eid, amount,
    SUM(amount) OVER (ORDER BY eid ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running,
    AVG(amount) OVER (ORDER BY eid ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS moving,
    NTH_VALUE(amount,2) OVER (ORDER BY eid ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS nth2,
    NTILE(3) OVER (ORDER BY amount) AS bucket,
    PERCENT_RANK() OVER (ORDER BY amount) AS pct_rank,
    CUME_DIST() OVER (ORDER BY amount) AS cume_d
FROM sal ORDER BY eid;

-- 스칼라 함수 (문자열)
SELECT UPPER(name), LOWER(name), LENGTH(name), CONCAT(name,'@co'), SUBSTR(name,1,2),
    REPLACE(name,'Alice','Alex'), LPAD(id,4,'0'), CHAR_LENGTH(name), LEFT(name,2),
    REVERSE(name), REPEAT('ab',2), INSTR(name,'l'), ASCII('A'), HEX(255), FORMAT(1234.5,1) FROM emp LIMIT 2;

-- 스칼라 함수 (수학)
SELECT ROUND(3.567,1), ABS(-9), CEIL(3.1), FLOOR(3.9), MOD(7,3), SQRT(16), POW(2,8),
    LOG2(8), LOG10(100), PI(), SIGN(-1), TRUNCATE(3.789,1), RAND() >= 0 AS rand_ok;

-- 스칼라 함수 (날짜)
SELECT YEAR(hdate), MONTH(hdate), DAY(hdate), DAYOFWEEK(hdate),
    DATEDIFF('2026-05-01',hdate) AS days, DATE_FORMAT(hdate,'%Y-%m') AS ym,
    DATE_ADD(hdate, INTERVAL 30 DAY), TIMESTAMPDIFF(YEAR,hdate,'2026-05-01') AS yrs FROM emp LIMIT 2;

-- 스칼라 함수 (조건/기타)
SELECT COALESCE(dept_id,-1), IFNULL(dept_id,-1), NULLIF(salary,900),
    GREATEST(1,5,3), LEAST(1,5,3), CAST('42' AS INT),
    IF(salary>800,'High','Normal'), CASE WHEN salary>=1000 THEN 'Exec' ELSE 'Other' END,
    MD5('hello'), LENGTH(UUID()) > 0 AS uuid_ok FROM emp LIMIT 2;

-- INSERT 변형
INSERT IGNORE INTO dept (name) VALUES ('Eng');
INSERT INTO emp (id,name,dept_id,salary) VALUES (1,'Alice',1,9999) ON DUPLICATE KEY UPDATE salary=9999;
SELECT id, name, salary FROM emp WHERE id=1;
UPDATE emp SET salary=salary-99 WHERE id=1;
CREATE TABLE bak (id INT PRIMARY KEY, eid INT, amount INT);
INSERT INTO bak SELECT id, eid, amount FROM sal WHERE amount > 800;
SELECT * FROM bak ORDER BY amount DESC;
TRUNCATE TABLE bak;
DROP TABLE bak;

-- RETURNING
INSERT INTO dept (name,budget) VALUES ('Tmp',1) RETURNING id, name;
DELETE FROM dept WHERE name='Tmp' RETURNING id, name;
UPDATE emp SET salary=salary+1 WHERE id=1 RETURNING id, salary;
UPDATE emp SET salary=salary-1 WHERE id=1;

-- UPDATE/DELETE 다중 테이블
UPDATE emp e, dept d SET e.salary=e.salary+100, d.budget=d.budget+1000 WHERE e.dept_id=d.id AND d.id=1;
UPDATE emp SET salary=salary-100 WHERE dept_id=1;
DELETE sal, emp FROM sal JOIN emp ON sal.eid=emp.id WHERE emp.status='inactive';

-- ALTER TABLE
ALTER TABLE emp ADD COLUMN email VARCHAR(50);
UPDATE emp SET email=CONCAT(name,'@co.com') WHERE status='active';
ALTER TABLE emp RENAME COLUMN email TO contact;
ALTER TABLE emp DROP COLUMN contact;
ALTER TABLE emp MODIFY COLUMN salary INT DEFAULT 0;
ALTER TABLE audit_log ADD CONSTRAINT fk_al FOREIGN KEY (id) REFERENCES dept(id);
ALTER TABLE audit_log ADD CONSTRAINT uq_al UNIQUE (msg);
ALTER TABLE audit_log DROP CONSTRAINT uq_al;
ALTER TABLE audit_log DROP FOREIGN KEY id;

-- ENUM/SET 제약 검증
INSERT INTO tags (val, set_col) VALUES ('a','X');
INSERT INTO tags (val) VALUES ('bad');              -- ERROR
INSERT INTO tags (val, set_col) VALUES ('b','X,Q'); -- ERROR
SELECT * FROM tags ORDER BY id;

-- MERGE
CREATE TABLE dept_mrg (id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(30) NOT NULL UNIQUE, budget INT DEFAULT 0);
INSERT INTO dept_mrg (name,budget) VALUES ('Eng',9999),('HR',500),('Mkt',0);
MERGE INTO dept USING dept_mrg ON dept.name=dept_mrg.name
    WHEN MATCHED AND dept_mrg.name='Mkt' THEN DELETE
    WHEN MATCHED THEN UPDATE SET budget=dept_mrg.budget
    WHEN NOT MATCHED THEN INSERT (name,budget) VALUES (dept_mrg.name, dept_mrg.budget);
SELECT name, budget FROM dept ORDER BY id;
DROP TABLE dept_mrg;

-- 저장 프로시저 (IF/ELSEIF)
CREATE PROCEDURE p_if(IN n INT)
BEGIN
    DECLARE res VARCHAR(20) DEFAULT 'zero';
    IF n > 0 THEN SET res = 'positive';
    ELSEIF n < 0 THEN SET res = 'negative';
    END IF;
    SELECT res;
END;
CALL p_if(5);
CALL p_if(-3);
CALL p_if(0);
DROP PROCEDURE p_if;

-- 저장 프로시저 (WHILE)
CREATE PROCEDURE p_while()
BEGIN
    DECLARE i INT DEFAULT 1;
    DECLARE s INT DEFAULT 0;
    WHILE i <= 5 DO
        SET s = s + i;
        SET i = i + 1;
    END WHILE;
    SELECT s AS while_sum;
END;
CALL p_while();
DROP PROCEDURE p_while;

-- 저장 프로시저 (LOOP/LEAVE/ITERATE)
CREATE PROCEDURE p_loop()
BEGIN
    DECLARE i INT DEFAULT 0;
    DECLARE s INT DEFAULT 0;
    lp: LOOP
        SET i = i + 1;
        IF i > 5 THEN LEAVE lp; END IF;
        IF MOD(i,2) = 0 THEN ITERATE lp; END IF;
        SET s = s + i;
    END LOOP;
    SELECT s AS odd_sum;
END;
CALL p_loop();
DROP PROCEDURE p_loop;

-- 저장 프로시저 (REPEAT/UNTIL)
CREATE PROCEDURE p_repeat()
BEGIN
    DECLARE n INT DEFAULT 0;
    REPEAT
        SET n = n + 1;
    UNTIL n >= 5 END REPEAT;
    SELECT n AS repeat_result;
END;
CALL p_repeat();
DROP PROCEDURE p_repeat;

-- PREPARE / EXECUTE
PREPARE sel FROM 'SELECT id, name, salary FROM emp WHERE id = ?';
SET @id = 1;
EXECUTE sel USING @id;
SET @id = 2;
EXECUTE sel USING @id;
DEALLOCATE PREPARE sel;
SET @x = 42;
SELECT @x;

-- 트리거 (INSERT / UPDATE / DELETE)
CREATE TRIGGER trg_ins AFTER INSERT ON dept FOR EACH ROW INSERT INTO audit_log (msg) VALUES ('dept_inserted');
INSERT INTO dept (name,budget) VALUES ('TrgTest',0);
SELECT msg FROM audit_log ORDER BY id;
DELETE FROM dept WHERE name='TrgTest';
DROP TRIGGER IF EXISTS trg_ins;

CREATE TRIGGER trg_upd BEFORE UPDATE ON dept FOR EACH ROW INSERT INTO audit_log (msg) VALUES ('dept_updating');
UPDATE dept SET budget=budget WHERE name='Eng';
SELECT msg FROM audit_log ORDER BY id DESC LIMIT 1;
DROP TRIGGER IF EXISTS trg_upd;

CREATE TRIGGER trg_del AFTER DELETE ON dept FOR EACH ROW INSERT INTO audit_log (msg) VALUES ('dept_deleted');
INSERT INTO dept (name) VALUES ('TrgDel');
DELETE FROM dept WHERE name='TrgDel';
SELECT msg FROM audit_log ORDER BY id DESC LIMIT 1;
DROP TRIGGER IF EXISTS trg_del;

-- 트랜잭션 / SAVEPOINT
BEGIN;
INSERT INTO emp (name,dept_id,salary) VALUES ('Tmp',1,300);
SAVEPOINT sp1;
UPDATE emp SET salary=999 WHERE name='Tmp';
ROLLBACK TO SAVEPOINT sp1;
COMMIT;
SELECT name, salary FROM emp WHERE name='Tmp';
BEGIN;
UPDATE sal SET amount=1 WHERE id=1;
ROLLBACK;
SELECT amount FROM sal WHERE id=1;
BEGIN;
SAVEPOINT sp2;
RELEASE SAVEPOINT sp2;
COMMIT;

-- 격리 수준
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL REPEATABLE READ;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;

-- SELECT FOR UPDATE / FOR SHARE
BEGIN;
SELECT id, name FROM emp WHERE id=1 FOR UPDATE;
SHOW LOCKS;
COMMIT;
BEGIN;
SELECT id FROM emp WHERE id=1 FOR SHARE;
COMMIT;

-- EXPLAIN / ANALYZE
EXPLAIN SELECT * FROM emp WHERE dept_id=1;
EXPLAIN SELECT * FROM emp WHERE id=1;
ANALYZE TABLE emp;
EXPLAIN ANALYZE SELECT * FROM emp WHERE dept_id=1;

-- VIEW
SELECT * FROM v_active ORDER BY id;

-- CREATE FUNCTION
CREATE FUNCTION triple(x) RETURNS INT RETURN x * 3;
SELECT name, salary, triple(salary) AS tripled FROM emp LIMIT 3;
DROP FUNCTION triple;
CREATE FUNCTION greet(n) RETURNS VARCHAR(50) RETURN CONCAT('Hello, ', n);
SELECT greet(name) AS greeting FROM emp LIMIT 2;
DROP FUNCTION greet;

-- BIGINT / SMALLINT / TINYINT
SELECT big_val, small_val, tiny_val FROM nums;
DESCRIBE nums;

-- JSON
SELECT id, info->>'$.name' AS jname, info->>'$.age' AS age FROM jdata ORDER BY id;
SELECT id, JSON_EXTRACT(info,'$.score') AS score, JSON_VALUE(info,'$.name') AS nm FROM jdata ORDER BY id;

-- INFORMATION_SCHEMA
SELECT table_name, table_rows FROM information_schema.tables WHERE table_schema='db1' ORDER BY table_name LIMIT 5;
SELECT column_name, data_type FROM information_schema.columns WHERE table_name='emp' ORDER BY ordinal_position LIMIT 5;

-- 사용자 관리
CREATE USER 'usr'@'%' IDENTIFIED BY 'pw';
GRANT SELECT, INSERT ON db1.emp TO 'usr'@'%';
SHOW GRANTS FOR 'usr'@'%';
REVOKE INSERT ON db1.emp FROM 'usr'@'%';
DROP USER 'usr'@'%';

-- 모니터링
CHECKPOINT;
VACUUM;
VACUUM emp;
SHOW BUFFER POOL;
SHOW WAL;
SHOW LOCKS;
SHOW PROCESSLIST;
SHOW DATABASES;

-- BACKUP
BACKUP DATABASE db1 INTO 'db1_backup.json';

-- 정리
DROP VIEW IF EXISTS v_active;
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
