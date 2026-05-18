## MCP 기반 커스텀 RDBMS (v2.2.0)

- Rust로 구현한 데이터베이스 엔진 + RDBMS + AI MCP 

<br/>

## 핵심 기능

| 분류 | 내용 |
|------|------|
| DB 엔진 | B+Tree, WAL, Buffer Pool, MVCC, 트랜잭션, 비용 기반 옵티마이저 |
| SQL 지원 | DDL / DML / JOIN / 서브쿼리 / CTE / UNION / 제약조건 / 트랜잭션 |
| MCP | 자연어 입력 → SQL 자동 생성 → 실행 |
| DBMS | TCP 서버, 다중 클라이언트 동시 접속, 세션별 독립 Executor + `Arc<RwLock<SharedDatabase>>` 공유 |
| 언어 | Rust |

<br/>

## 실행 방법
```bash
# REPL 모드
cargo run -p rustdb-cli

# 서버 모드 (커스텀 프로토콜 7878 + MySQL 프로토콜 3306 동시 기동)
cargo run -p rustdb-server

# MySQL wire protocol만 포트 변경 또는 비활성화
cargo run -p rustdb-server -- --mysql-port 13306
cargo run -p rustdb-server -- --no-mysql

# 버퍼 풀 크기 지정 (기본: 64 페이지)
cargo run -p rustdb-server -- --buffer-pool-size 256
cargo run -p rustdb-cli -- --buffer-pool-size 128

# 커스텀 클라이언트 CLI (서버 실행 후)
cargo run -p rustdb-client -- -u root -p root -h 127.0.0.1 -P 7878

# MySQL 클라이언트로 직접 접속 (mysql CLI, DBeaver, JDBC 등)
mysql -h 127.0.0.1 -P 3306 -u root --skip-auto-rehash

# UI 모드
cd rustdb-ui && npm run tauri dev
```

<br/>

## 테스트 쿼리

`test/test_full.sql` — **단일 DB(`db1`), 전 기능 커버, 의도된 오류 2개** (ENUM/SET 유효성 위반)

```bash
# code/ 디렉터리에서 실행
cargo run -p rustdb-cli < test/test_full.sql
```

```sql
-- RustDB 통합 테스트 v2.2.0

-- Setup
DROP USER IF EXISTS 'usr'@'%';
DROP DATABASE IF EXISTS db1;
SHOW DATABASES;
CREATE DATABASE db1;
USE db1;

-- DDL
CREATE TABLE dept (
    id     INT AUTO INCREMENT,
    name   VARCHAR(30) NOT NULL,
    budget INT DEFAULT 0,
    CONSTRAINT pk_dept PRIMARY KEY (id),
    UNIQUE KEY uk_dept_name (name)
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
SHOW TABLES;
DESCRIBE emp;

-- IF NOT EXISTS
CREATE DATABASE IF NOT EXISTS db1;
CREATE TABLE IF NOT EXISTS dept (dummy INT);

-- DML insert
INSERT INTO dept (name, budget) VALUES ('Eng',1000),('Mkt',800),('Fin',1200);
INSERT INTO emp (name, dept_id, salary, hdate, status) VALUES
    ('Alice',1,900,'2020-01-15','active'),('Bob',1,800,'2021-06-01','active'),
    ('Carol',2,700,'2019-11-20','inactive'),('Dave',2,600,'2022-03-10','active'),
    ('Eve',3,1200,'2015-05-01','active'),('Frank',NULL,500,'2023-07-01','active');
INSERT INTO sal (eid, amount, grade) VALUES (1,900,'S4'),(2,800,'S3'),(3,700,'S3'),(4,600,'S2'),(5,1200,'S5'),(6,500,'S1');
INSERT INTO org (name, pid) VALUES ('CEO',NULL),('CTO',1),('CFO',1),('Lead',2),('Alice',4),('Bob',4);
INSERT INTO tags (val, set_col) VALUES ('a','X,Y'),('b','Z');
INSERT INTO nums (big_val, small_val, tiny_val) VALUES (9223372036854775807,32767,127);
INSERT INTO jdata (info) VALUES ('{"name":"Alice","age":30,"score":95.5}'),('{"name":"Bob","age":25,"score":80.0}');

-- SELECT
SELECT id, name, salary FROM emp WHERE salary >= 700 AND status = 'active' ORDER BY salary DESC;
SELECT name FROM emp ORDER BY id LIMIT 3 OFFSET 2;
SELECT DISTINCT status FROM emp ORDER BY status;
SELECT name FROM emp WHERE dept_id IN (1,2) AND salary BETWEEN 600 AND 900;
SELECT name FROM emp WHERE dept_id NOT IN (3) AND salary > 700;
SELECT name FROM emp WHERE name LIKE 'A%' OR name LIKE 'E%';
SELECT name FROM emp WHERE dept_id IS NULL;

-- REGEXP
SELECT name FROM emp WHERE name REGEXP '^[AB]';
SELECT name FROM emp WHERE name RLIKE 'e$';
SELECT name, REGEXP_LIKE(name,'^A') AS sa, REGEXP_REPLACE(name,'a','@') AS rr FROM emp LIMIT 3;
SELECT REGEXP_MATCH('Alice123','[0-9]+') AS mat;

-- 집계
SELECT COUNT(*), SUM(amount), AVG(amount), MAX(amount), MIN(amount) FROM sal;
SELECT grade, COUNT(*) AS n, AVG(amount) AS avg_sal FROM sal GROUP BY grade HAVING n >= 1 ORDER BY avg_sal DESC;
SELECT dept_id, GROUP_CONCAT(name SEPARATOR ', ') AS members FROM emp GROUP BY dept_id ORDER BY dept_id;
SELECT COUNT(DISTINCT grade), SUM(DISTINCT amount) FROM sal;
SELECT STDDEV(amount), VARIANCE(amount) FROM sal;

-- JOIN
SELECT e.name, d.name AS dept, s.amount FROM emp e JOIN dept d ON e.dept_id=d.id JOIN sal s ON e.id=s.eid ORDER BY s.amount DESC;
SELECT e.name, d.name FROM emp e LEFT JOIN dept d ON e.dept_id=d.id ORDER BY e.id;
SELECT d.name, e.name FROM dept d CROSS JOIN emp e ORDER BY d.name, e.name LIMIT 9;
SELECT e.name, s.amount FROM emp e NATURAL JOIN sal s ORDER BY e.name LIMIT 4;
SELECT d.name, e.name FROM dept d RIGHT JOIN emp e ON d.id=e.dept_id ORDER BY e.id LIMIT 3;
SELECT d.name, e.name FROM dept d FULL OUTER JOIN emp e ON d.id=e.dept_id ORDER BY e.id LIMIT 5;

-- 서브쿼리
SELECT name FROM emp WHERE id IN (SELECT eid FROM sal WHERE amount > 800);
SELECT eid, amount FROM sal WHERE amount > (SELECT AVG(amount) FROM sal);
SELECT name FROM emp WHERE EXISTS (SELECT 1 FROM sal WHERE eid=emp.id AND amount > 1000);
SELECT grade, avg_a FROM (SELECT grade, AVG(amount) AS avg_a FROM sal GROUP BY grade) AS g WHERE avg_a > 700;
SELECT name, salary, (SELECT MAX(salary) FROM emp) AS max_sal FROM emp ORDER BY salary DESC LIMIT 3;

-- UNION/INTERSECT/EXCEPT
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
    LAG(salary,1) OVER (PARTITION BY dept_id ORDER BY salary) AS prev_sal,
    LEAD(salary,1) OVER (PARTITION BY dept_id ORDER BY salary) AS next_sal,
    FIRST_VALUE(salary) OVER (PARTITION BY dept_id ORDER BY salary DESC) AS top_sal,
    LAST_VALUE(salary) OVER (PARTITION BY dept_id ORDER BY salary DESC) AS bot_sal
FROM emp WHERE dept_id IS NOT NULL ORDER BY dept_id, salary DESC;

-- 윈도우 프레임 ROWS/RANGE
SELECT eid, amount,
    SUM(amount) OVER (ORDER BY eid ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running,
    AVG(amount) OVER (ORDER BY eid ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS moving,
    SUM(amount) OVER (PARTITION BY grade ORDER BY eid RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS part_sum
FROM sal ORDER BY eid;

-- NTH_VALUE/NTILE/PERCENT_RANK/CUME_DIST
SELECT eid, amount,
    NTH_VALUE(amount,2) OVER (ORDER BY eid ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS nth2,
    NTILE(3) OVER (ORDER BY amount) AS bucket,
    PERCENT_RANK() OVER (ORDER BY amount) AS pct_rank,
    CUME_DIST() OVER (ORDER BY amount) AS cume_d
FROM sal ORDER BY eid;

-- 스칼라 함수 (문자열)
SELECT UPPER(name), LOWER(name), LENGTH(name), CONCAT(name,'@co'), TRIM('  hi  '),
    SUBSTR(name,1,2), REPLACE(name,'Alice','Alex'), LPAD(id,4,'0'), RPAD(name,10,'.'),
    CHAR_LENGTH(name), LEFT(name,2), RIGHT(name,2), REVERSE(name) FROM emp LIMIT 2;
SELECT REPEAT('ab',3), INSTR('hello','ll'), LOCATE('ll','hello'),
    LTRIM('  hi'), RTRIM('hi  '), SPACE(4), ASCII('A'), HEX(255), FORMAT(1234567.89,2);

-- 스칼라 함수 (수학)
SELECT salary/100, ROUND(salary/100,1), ABS(-999), CEIL(3.1), FLOOR(3.9), MOD(salary,7) FROM emp LIMIT 2;
SELECT SQRT(144), POW(2,10), LOG(1), LOG2(8), LOG10(100), EXP(0),
    SIN(0), COS(0), TAN(0), PI(), SIGN(-5), TRUNCATE(3.789,1), RAND() >= 0;

-- 스칼라 함수 (날짜)
SELECT YEAR(hdate), MONTH(hdate), DAY(hdate), DAYOFWEEK(hdate), DAYOFYEAR(hdate), WEEKDAY(hdate),
    DATEDIFF('2026-05-01', hdate) AS days, DATE_FORMAT(hdate,'%Y-%m') AS ym,
    TIMESTAMPDIFF(YEAR, hdate, '2026-05-01') AS yrs FROM emp LIMIT 2;
SELECT DATE_ADD('2026-01-01', INTERVAL 30 DAY) AS added,
    DATE_SUB('2026-05-01', INTERVAL 30 DAY) AS past,
    UNIX_TIMESTAMP('2024-01-01') AS uts;

-- 스칼라 함수 (조건/기타)
SELECT COALESCE(dept_id,-1) AS c, IFNULL(dept_id,-1) AS i, ISNULL(dept_id) AS n FROM emp WHERE dept_id IS NULL LIMIT 1;
SELECT NULLIF(salary,900) AS maybe_null FROM emp ORDER BY id LIMIT 1;
SELECT SUBSTRING(name,2,3) AS sub FROM emp LIMIT 2;
SELECT GREATEST(1,5,3), LEAST(1,5,3), MD5('hello') AS md5, LENGTH(UUID()) > 0 AS uuid_ok;
SELECT CAST('2026' AS INT), CAST('3.14' AS FLOAT);
SELECT IF(salary > 800,'High','Normal') AS tier, CASE WHEN salary >= 1000 THEN 'Exec' ELSE 'Other' END AS lvl FROM emp LIMIT 4;

-- DML UPDATE/DELETE/INSERT 변형
INSERT INTO dept (name) VALUES ('Legal');
INSERT IGNORE INTO dept (name) VALUES ('Eng');
INSERT INTO emp (id, name, dept_id, salary) VALUES (1,'Alice',1,9999) ON DUPLICATE KEY UPDATE salary = 9999;
SELECT id, name, salary FROM emp WHERE id = 1;
CREATE TABLE bak (id INT PRIMARY KEY, eid INT, amount INT);
INSERT INTO bak SELECT id, eid, amount FROM sal WHERE amount > 800;
SELECT * FROM bak ORDER BY amount DESC;
TRUNCATE TABLE bak;
DROP TABLE bak;

-- ALTER TABLE
ALTER TABLE emp ADD COLUMN email VARCHAR(50);
UPDATE emp SET email = CONCAT(name,'@co.com') WHERE status = 'active';
SELECT id, name, email FROM emp LIMIT 3;
ALTER TABLE emp RENAME COLUMN email TO contact;
ALTER TABLE emp DROP COLUMN contact;
ALTER TABLE nums RENAME TO nums_bak;
ALTER TABLE nums_bak RENAME TO nums;
ALTER TABLE dept MODIFY COLUMN budget INT DEFAULT 0;

-- ALTER TABLE CONSTRAINT
ALTER TABLE audit_log ADD CONSTRAINT fk_al FOREIGN KEY (id) REFERENCES dept(id);
ALTER TABLE audit_log ADD CONSTRAINT uq_al UNIQUE (msg);
ALTER TABLE audit_log ADD CONSTRAINT ck_al CHECK (id > 0);
ALTER TABLE audit_log DROP CONSTRAINT ck_al;
ALTER TABLE audit_log DROP FOREIGN KEY id;
ALTER TABLE audit_log DROP CONSTRAINT uq_al;

-- UPDATE/DELETE multi/join/returning
UPDATE sal SET amount = amount * 2 WHERE grade = 'S1';
SELECT eid, amount FROM sal WHERE grade = 'S1';
UPDATE sal SET amount = amount / 2 WHERE grade = 'S1';
DELETE FROM emp WHERE id = 6;
SELECT * FROM sal WHERE eid = 6;
UPDATE emp e, dept d SET e.salary=e.salary+100, d.budget=d.budget+1000 WHERE e.dept_id=d.id AND d.id=1;
SELECT id, name, salary FROM emp WHERE dept_id=1 ORDER BY id;
UPDATE emp SET salary=salary-100 WHERE dept_id=1;
DELETE sal, emp FROM sal JOIN emp ON sal.eid=emp.id WHERE emp.status='inactive';
UPDATE emp e JOIN dept d ON e.dept_id=d.id SET e.salary=e.salary WHERE d.name='Eng';
UPDATE emp SET salary=salary+1 WHERE id=1 RETURNING id, name, salary;
UPDATE emp SET salary=salary-1 WHERE id=1;
INSERT INTO dept (name, budget) VALUES ('Ops',500) RETURNING id, name;
DELETE FROM dept WHERE name='Ops' RETURNING id, name;

-- ENUM/SET 검증 오류
INSERT INTO tags (val, set_col) VALUES ('a','X');
INSERT INTO tags (val) VALUES ('bad');              -- ERROR
INSERT INTO tags (val, set_col) VALUES ('b','X,Q'); -- ERROR
SELECT * FROM tags ORDER BY id;

-- MERGE INTO
CREATE TABLE dept_new (id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(30) NOT NULL UNIQUE, budget INT DEFAULT 0);
INSERT INTO dept_new (name, budget) VALUES ('Eng',9999),('HR',500);
MERGE INTO dept USING dept_new ON dept.name = dept_new.name
    WHEN MATCHED THEN UPDATE SET budget = dept_new.budget
    WHEN NOT MATCHED THEN INSERT (name, budget) VALUES (dept_new.name, dept_new.budget);
SELECT id, name, budget FROM dept ORDER BY id;
DROP TABLE dept_new;

-- MERGE WHEN MATCHED THEN DELETE
INSERT INTO dept (name, budget) VALUES ('MrgDelTest',0);
CREATE TABLE dept_del_src (id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(30) NOT NULL);
INSERT INTO dept_del_src (name) VALUES ('MrgDelTest');
MERGE INTO dept USING dept_del_src ON dept.name = dept_del_src.name WHEN MATCHED THEN DELETE;
SELECT name FROM dept ORDER BY id;
DROP TABLE dept_del_src;

-- 저장 프로시저
CREATE PROCEDURE reset_budget() UPDATE dept SET budget=0 WHERE budget > 5000;
CALL reset_budget();
DROP PROCEDURE IF EXISTS reset_budget;

CREATE PROCEDURE insert_dept(IN dname VARCHAR(30), IN dbudget INT) INSERT INTO dept (name, budget) VALUES ('ProcTest',777);
CALL insert_dept('ignored',0);
SELECT name, budget FROM dept WHERE name = 'ProcTest';
DELETE FROM dept WHERE name = 'ProcTest';
DROP PROCEDURE insert_dept;

-- IF/ELSEIF
CREATE PROCEDURE test_if(IN n INT)
BEGIN
    DECLARE res VARCHAR(20) DEFAULT 'zero';
    IF n > 0 THEN SET res = 'positive';
    ELSEIF n < 0 THEN SET res = 'negative';
    END IF;
    SELECT res;
END;
CALL test_if(5);
CALL test_if(-3);
CALL test_if(0);
DROP PROCEDURE test_if;

-- WHILE
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

-- LOOP/LEAVE
CREATE PROCEDURE test_loop()
BEGIN
    DECLARE i INT DEFAULT 0;
    lp: LOOP
        SET i = i + 1;
        IF i >= 3 THEN LEAVE lp; END IF;
    END LOOP;
    SELECT i AS loop_result;
END;
CALL test_loop();
DROP PROCEDURE test_loop;

-- ITERATE
CREATE PROCEDURE test_iterate()
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
CALL test_iterate();
DROP PROCEDURE test_iterate;

-- REPEAT/UNTIL
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

-- PREPARE/EXECUTE/DEALLOCATE
PREPARE sel_emp FROM 'SELECT id, name, salary FROM emp WHERE id = ?';
SET @eid = 1;
EXECUTE sel_emp USING @eid;
SET @eid = 2;
EXECUTE sel_emp USING @eid;
DEALLOCATE PREPARE sel_emp;
PREPARE ins_emp FROM 'INSERT INTO emp (name, dept_id, salary) VALUES (?, ?, ?)';
SET @n = 'PrepTest';
SET @d = 1;
SET @s = 9999;
EXECUTE ins_emp USING @n, @d, @s;
SELECT id, name, salary FROM emp WHERE name = 'PrepTest';
DEALLOCATE PREPARE ins_emp;
SET @x = 42;
SELECT @x;
SET @msg = 'hello';
SELECT @msg;
DELETE FROM emp WHERE name = 'PrepTest';

-- 트리거
CREATE TRIGGER trg_dept_insert AFTER INSERT ON dept FOR EACH ROW INSERT INTO audit_log (msg) VALUES ('dept_inserted');
INSERT INTO dept (name, budget) VALUES ('TrgTest',100);
SELECT msg FROM audit_log ORDER BY id;
DELETE FROM dept WHERE name = 'TrgTest';
DROP TRIGGER IF EXISTS trg_dept_insert;

CREATE TRIGGER trg_dept_upd BEFORE UPDATE ON dept FOR EACH ROW INSERT INTO audit_log (msg) VALUES ('dept_updating');
UPDATE dept SET budget=budget WHERE name='Eng';
SELECT msg FROM audit_log ORDER BY id DESC LIMIT 1;
DROP TRIGGER IF EXISTS trg_dept_upd;

CREATE TRIGGER trg_dept_del AFTER DELETE ON dept FOR EACH ROW INSERT INTO audit_log (msg) VALUES ('dept_deleted');
INSERT INTO dept (name, budget) VALUES ('TrgDelTest',0);
DELETE FROM dept WHERE name='TrgDelTest';
SELECT msg FROM audit_log ORDER BY id DESC LIMIT 1;
DROP TRIGGER IF EXISTS trg_dept_del;

-- 트랜잭션/SAVEPOINT
BEGIN;
INSERT INTO emp (name, dept_id, salary) VALUES ('Tmp',1,300);
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
SAVEPOINT sp_rel;
RELEASE SAVEPOINT sp_rel;
COMMIT;

-- 격리 수준
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL REPEATABLE READ;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ UNCOMMITTED;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;

-- SELECT FOR UPDATE/SHARE
BEGIN;
SELECT id, name FROM emp WHERE id=1 FOR UPDATE;
SHOW LOCKS;
COMMIT;
BEGIN;
SELECT id FROM emp WHERE id=1 FOR SHARE;
SELECT id FROM emp WHERE id=2 FOR SHARE;
SHOW LOCKS;
COMMIT;

-- EXPLAIN/ANALYZE
EXPLAIN SELECT dept_id, salary FROM emp WHERE dept_id=1;
EXPLAIN SELECT * FROM emp WHERE id=1;
ANALYZE TABLE emp;
EXPLAIN ANALYZE SELECT * FROM emp WHERE dept_id=1;

-- VIEW
SELECT * FROM v_active ORDER BY id;
SHOW CREATE TABLE emp;
UPDATE v_active SET salary=salary+1 WHERE name='Alice';
SELECT id, name, salary FROM emp WHERE name='Alice';
UPDATE v_active SET salary=salary-1 WHERE name='Alice';

-- CREATE FUNCTION
CREATE FUNCTION triple(x) RETURNS INT RETURN x * 3;
SELECT name, salary, triple(salary) AS tripled FROM emp LIMIT 3;
DROP FUNCTION triple;
CREATE FUNCTION greet(n) RETURNS VARCHAR(50) RETURN CONCAT('Hello, ', n);
SELECT greet(name) AS greeting FROM emp LIMIT 2;
DROP FUNCTION greet;

-- BIGINT/SMALLINT/TINYINT
SELECT big_val, small_val, tiny_val FROM nums;
DESCRIBE nums;

-- JSON
SELECT id, info FROM jdata ORDER BY id;
SELECT id, info->'$.name' AS jname, info->>'$.age' AS age FROM jdata ORDER BY id;
SELECT id, JSON_EXTRACT(info,'$.score') AS score FROM jdata ORDER BY id;
SELECT id, JSON_VALUE(info,'$.name') AS nm FROM jdata ORDER BY id;
DESCRIBE jdata;

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
BACKUP DATABASE db1;
BACKUP DATABASE db1 INTO 'db1_backup.json';

-- Cleanup
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
```

<br/>

## 기술 스택

| 항목 | 내용 |
|------|------|
| 언어 | Rust |
| 버전 | v2.2.0 |
| 인덱스 | B+Tree (단일 / 복합 / 클러스터드) |
| 옵티마이저 | 비용 기반 플래너 (AccessPath · Join 알고리즘 자동 선택) |
| Join | Sort-Merge Join (O((N+M)logN)) / Hash Join (O(N+M)) / Nested Loop Join |
| 트랜잭션 | WAL (바이너리 redo log) + Undo Log (인메모리 + 디스크 영속화) + MVCC |
| 격리 수준 | READ UNCOMMITTED ~ SERIALIZABLE (4단계) |
| 동시성 | Row-level Locking (SELECT FOR UPDATE / FOR SHARE, 공유·배타 잠금, 데드락 감지) |
| 캐시 | Buffer Pool (LRU, 64페이지, 16KB) |
| 저장 | 바이너리 .rdb + LZ4 압축 + indexes.json + views.json + _users.json + _grants.json |
| 다중 DB | CREATE / DROP / USE / SHOW DATABASES, 테이블 자동 한정, 격리 |
| 사용자 관리 | CREATE/DROP USER, GRANT/REVOKE, SHOW GRANTS, 영속화 |
| UI | Tauri + React + Monaco Editor (멀티 탭) |
| TCP 서버 | 멀티 클라이언트, 포트 7878, 라인 프로토콜 |
| AI 연동 | MCP AI API (예정) |

<br/>

## 프로젝트 구조
```
code/
├── rustdb-core/     DB 엔진 라이브러리
├── rustdb-server/   TCP 서버
├── rustdb-cli/      터미널 REPL (stdin 직접 실행)
├── rustdb-client/   TCP 클라이언트 CLI (-u/-p/-H/-P 옵션)
├── rustdb-ui/       Tauri + React UI
└── rustdb-mcp/      MCP 자연어 쿼리 (개발 예정)
```

<br/>

## 아키텍처
```
┌──────────────────────────────────────────┐
│               rustdb-core                │
│                                          │
│  Lexer → Parser → AST                    │
│              ↓                           │
│        Query Planner (비용 기반)         │
│   AccessPath / JoinAlgo / Cost Est.      │
│              ↓                           │
│          Executor                        │
│  ┌───────────────────────────────┐       │
│  │ DDL: CREATE/DROP/ALTER/TRUNC  │       │
│  │ DML: INSERT/SELECT/UPDATE/DEL │       │
│  │ INSERT ... SELECT             │       │
│  │ Hash Join / Nested Loop Join  │       │
│  │ 테이블 별칭 (alias)           │       │
│  │ WHERE / SUBQUERY / EXISTS     │       │
│  │ IN (리터럴/서브쿼리) / NOT IN │       │
│  │ NOT 조건 / 산술 표현식        │       │
│  │ FROM 서브쿼리                 │       │
│  │ UNION / UNION ALL             │       │
│  │ CTE (WITH ... AS)             │       │
│  │ ORDER BY / GROUP BY / HAVING  │       │
│  │ LIMIT / OFFSET                │       │
│  │ CASE WHEN / DISTINCT          │       │
│  │ 스칼라 / 날짜 / NULL 함수     │       │
│  │ 집계함수 (COUNT/SUM/AVG/...)  │       │
│  │ INDEX (단일/복합/클러스터드)  │       │
│  │ EXPLAIN (비용 기반 실행 계획) │       │
│  │ VIEW / 제약조건 (PK/FK/CHECK) │       │
│  │ FK SET DEFAULT / NO ACTION    │       │
│  │ CREATE/DROP USER              │       │
│  │ GRANT / REVOKE / SHOW GRANTS  │       │
│  │ BEGIN / COMMIT / ROLLBACK     │       │
│  │ SAVEPOINT / ROLLBACK TO sp    │       │
│  │ 격리 수준 4단계               │       │
│  │ MVCC (논리삭제 / VACUUM)      │       │
│  │ Row-level Locking (FOR UPDATE)│       │
│  │ Checkpoint / WAL Recovery     │       │
│  └───────────────────────────────┘       │
│          ↓                               │
│  B+Tree 인덱스 (단일/복합/클러스터드)    │
│  WAL 바이너리 redo log + Checkpoint      │
│  Undo Log 디스크 영속화 (_undo.log)      │
│  Buffer Pool (LRU 64p 16KB)              │
│  MVCC (_xmin / _xmax 버전 스탬프)        │
│  바이너리 .rdb + LZ4 압축 저장           │
│  인덱스/뷰 영속화 (indexes/views.json)   │
│  사용자/권한 영속화 (_users/_grants.json)│
│                                          │
└──────────────────────────────────────────┘
        ↓              ↓
  rustdb-cli      rustdb-server
  (터미널 REPL)   (TCP 서버)
                      ↓
               rustdb-client
               (TCP 클라이언트 CLI)
        ↓
  rustdb-ui        rustdb-mcp
  (Tauri + React)  (MCP, 개발 예정)
```

<br/>

## B+ Tree
[B+ Tree 구조](https://chanho0912.tistory.com/109)

[B+ Tree 이해](https://velog.io/@emplam27/%EC%9E%90%EB%A3%8C%EA%B5%AC%EC%A1%B0-%EA%B7%B8%EB%A6%BC%EC%9C%BC%EB%A1%9C-%EC%95%8C%EC%95%84%EB%B3%B4%EB%8A%94-B-Plus-Tree)
