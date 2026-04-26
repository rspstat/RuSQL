## MCP 기반 커스텀 RDBMS

- Rust로 구현한 데이터베이스 엔진 + RDBMS + AI MCP 

<br/>

## 핵심 기능

| 분류 | 내용 |
|------|------|
| DB 엔진 | B+Tree, WAL, Buffer Pool, MVCC, 트랜잭션, 비용 기반 옵티마이저 |
| SQL 지원 | DDL / DML / JOIN / 서브쿼리 / CTE / UNION / 제약조건 / 트랜잭션 |
| MCP | 자연어 입력 → SQL 자동 생성 → 실행 |
| DBMS | TCP 서버, 다중 클라이언트 동시 접속 |
| 언어 | Rust |

<br/>

## 완료된 기능

### 엔진 코어
- [x] Lexer / Tokenizer
- [x] SQL Parser (AST 기반, 재귀 하강)
- [x] Executor (쿼리 실행 엔진)
- [x] 비용 기반 쿼리 옵티마이저 (Cost-Based Query Planner)
  - AccessPath 선택 (SeqScan / PkPoint / PkRange / SecondaryIndex / CompositeIndex)
  - 행 수 / 비용 추정 (log₂N 기반)
  - Join 알고리즘 자동 선택 (Hash Join vs Nested Loop)
  - EXPLAIN 실행 계획 출력 (비용 · 접근 경로 · Join 알고리즘)

### DDL
- [x] CREATE TABLE / DROP TABLE / DROP TABLE IF EXISTS
- [x] TRUNCATE TABLE
- [x] ALTER TABLE (ADD / MODIFY / DROP / RENAME COLUMN)
- [x] CREATE INDEX / DROP INDEX (단일 / 복합)
- [x] CREATE VIEW / DROP VIEW
- [x] DESCRIBE (테이블 스키마 조회)

### DML
- [x] INSERT (전체 컬럼 / 컬럼 지정 / 멀티 row)
- [x] INSERT ... SELECT (SELECT 결과를 다른 테이블에 삽입)
- [x] SELECT
- [x] UPDATE (상수 / 산술 표현식 / 자기 참조 — `salary = salary * 1.1`)
- [x] DELETE (MVCC 논리 삭제 / 물리 삭제)

### 쿼리 기능
- [x] WHERE (=, !=, >, <, >=, <=)
- [x] AND / OR / NOT 복합 조건 — `NOT (price > 100 OR active = 0)`
- [x] IN (리터럴 목록) — `WHERE id IN (1, 2, 3)`
- [x] NOT IN (리터럴 목록) — `WHERE id NOT IN (2, 4)`
- [x] IN / NOT IN (서브쿼리) — `WHERE dept_id IN (SELECT id FROM dept)`
- [x] BETWEEN / LIKE (%, _ 와일드카드)
- [x] IS NULL / IS NOT NULL
- [x] INNER JOIN / LEFT JOIN / RIGHT JOIN
- [x] Hash Join (대용량 Equi-Join O(N+M)) / Nested Loop Join (소규모·비등가)
- [x] 테이블 별칭 (alias) — `FROM emp e JOIN dept d ON e.dept_id = d.id`
- [x] ORDER BY (ASC / DESC, 다중 컬럼)
- [x] LIMIT / OFFSET — `LIMIT 10 OFFSET 20`
- [x] GROUP BY / HAVING
- [x] DISTINCT
- [x] 산술 표현식 — SELECT / WHERE / UPDATE SET에서 `price * qty`, `salary + 100`
- [x] 집계 함수 (COUNT, SUM, AVG, MIN, MAX)
- [x] CASE WHEN ... THEN ... ELSE ... END
- [x] 스칼라 함수 — UPPER / LOWER / LENGTH / TRIM / CONCAT / SUBSTR / REPLACE
- [x] 수학 함수 — ROUND / ABS / CEIL / FLOOR / MOD
- [x] 날짜 함수 — NOW / CURDATE / DATE_FORMAT
- [x] NULL 처리 함수 — COALESCE / IFNULL
- [x] 서브쿼리 — WHERE col = / > / < (SELECT ...)
- [x] 상관 서브쿼리 — WHERE EXISTS (SELECT 1 FROM ... WHERE outer.col = inner.col)
- [x] FROM 절 서브쿼리 — FROM (SELECT ...) AS alias
- [x] UNION / UNION ALL (ORDER BY / LIMIT / OFFSET 포함)
- [x] CTE (WITH ... AS) — 단순 / 다중 / INSERT 메인 쿼리 지원
- [x] SELECT ... FOR UPDATE (행 잠금)
- [x] table.column dot notation (SELECT / JOIN ON / GROUP BY / ORDER BY)
- [x] EXPLAIN (비용 기반 실행 계획 조회)
- [x] SHOW TABLES / DESCRIBE

### 데이터 타입
- [x] INT
- [x] VARCHAR(n) — 최대 길이 제한
- [x] DECIMAL(p, s) — 정밀도 / 소수 자리수
- [x] DATE — 'YYYY-MM-DD' 형식
- [x] DATETIME — 'YYYY-MM-DD HH:MM:SS' 형식
- [x] TIMESTAMP — 'YYYY-MM-DD HH:MM:SS' (값 없으면 현재 시각 자동 삽입)
- [x] BOOLEAN — true / false
- [x] TEXT / FLOAT

### 제약 조건
- [x] PRIMARY KEY (단일 / 복합)
- [x] NOT NULL
- [x] UNIQUE
- [x] AUTO INCREMENT
- [x] DEFAULT
- [x] CHECK (컬럼/테이블 레벨 표현식)
- [x] FOREIGN KEY RESTRICT (삭제 거부)
- [x] FOREIGN KEY CASCADE (연쇄 삭제)
- [x] FOREIGN KEY SET NULL (NULL 변경)
- [x] ON UPDATE CASCADE

### 트랜잭션
- [x] WAL (Write-Ahead Logging) — 바이너리 redo log
- [x] BEGIN / COMMIT / ROLLBACK
- [x] SAVEPOINT / ROLLBACK TO SAVEPOINT
- [x] Undo Log 기반 롤백 (B+Tree 인덱스 재빌드 포함)
- [x] WAL 기반 Crash Recovery (재시작 시 자동 복구)
- [x] Checkpoint (WAL 자동 트런케이션, 512KB 임계값)
- [x] 트랜잭션 격리 수준 4단계
  - READ UNCOMMITTED / READ COMMITTED
  - REPEATABLE READ (BEGIN 시점 스냅샷 고정)
  - SERIALIZABLE (팬텀 읽기 감지 + 자동 롤백)

### 인덱스 & 저장
- [x] B+Tree 인덱스 (단일 컬럼)
- [x] 복합 인덱스 (다중 컬럼, null-byte 키 결합)
- [x] 클러스터드 인덱스 (PK 기준 물리적 정렬 유지)
- [x] 보조 인덱스 중복 키 지원 (배열 저장, 동일 컬럼 값 다중 행)
- [x] 수치 인식 키 비교 (`"10" > "9"` 정상 처리)
- [x] 바이너리 디스크 저장 (.rdb 포맷, 16KB 페이지)
- [x] LZ4 데이터 압축 (.rdb 파일 투명 압축/해제, 하위 호환성 유지)
- [x] Buffer Pool (LRU 캐시, 64페이지)
- [x] 스키마 영속화 (TableSchema JSON, auto_increment 카운터 포함)
- [x] 인덱스 영속화 — 재시작 시 indexes.json으로 자동 재빌드
- [x] 뷰 영속화 — 재시작 시 views.json에서 AST 복원
- [x] TRUNCATE 후 AUTO INCREMENT 리셋

### MVCC
- [x] 행 버전 스탬프 (`_xmin`, `_xmax`)
- [x] DELETE → MVCC 논리 삭제 (트랜잭션 내) / 물리 삭제 (트랜잭션 외)
- [x] SELECT 가시성 필터 (`_xmax == "0"` 인 행만 표시)
- [x] ROLLBACK → `_xmax` 복원 + B+Tree 인덱스 재빌드
- [x] VACUUM (dead row 물리 제거)

### Row-level Locking
- [x] SELECT ... FOR UPDATE (쓰기 잠금 획득)
- [x] UPDATE / DELETE 시 잠금 충돌 감지
- [x] COMMIT / ROLLBACK 시 잠금 자동 해제
- [x] SHOW LOCKS (활성 잠금 목록 조회)

### 모니터링
- [x] SHOW BUFFER POOL (캐시 히트율, 사용량)
- [x] SHOW WAL (로그 레코드, 파일 크기)
- [x] SHOW ISOLATION LEVEL
- [x] SHOW LOCKS
- [x] CHECKPOINT (수동 체크포인트)
- [x] VACUUM (dead row 물리 제거)

### SQL 문법
- [x] 주석 지원 (-- 한 줄 / # MySQL 스타일 / /* */ 블록)
- [x] 주석 내 세미콜론 안전 처리 (쿼리 분리 오작동 없음)
- [x] 세미콜론(;) 구분 멀티 쿼리 입력

### UI (rustdb-ui)
- [x] Tauri + React 데스크탑 앱
- [x] Monaco Editor (SQL 문법 강조, 주석 회색)
- [x] 사이드바 테이블 / 컬럼 목록 (여러 테이블 동시 펼치기)
- [x] 사이드바 VIEW / INDEX 개별 항목 펼치기 (여러 항목 동시 펼치기)
- [x] 사이드바 섹션 접기/펼치기, 개수 뱃지
- [x] 사이드바 너비 조절 (드래그)
- [x] 테이블 우클릭 컨텍스트 메뉴 (MySQL 스타일)
- [x] GUI 테이블 브라우저 뷰 (데이터 그리드 + 필터)
- [x] TCP 서버 관리 뷰 (시작 / 중지 / 로그)
- [x] 멀티 쿼리 결과 표시
- [x] 쿼리 자동 저장
- [x] 결과창 크기 조절 (드래그)

<br/>

## 진행 예정

### 네트워크
- [x] TCP 서버 (포트 7878)
- [x] 멀티 클라이언트 동시 접속 (스레드 per 클라이언트)
- [ ] 클라이언트 CLI (`rustdb-client`)

### MCP 연동 (`rustdb-mcp`)
- [ ] AI API 클라이언트
- [ ] 자연어 → SQL 변환 (`\ai` 명령어)
- [ ] 변환된 SQL 확인 후 실행

### UI
- [ ] 쿼리 히스토리
- [ ] 결과 CSV 내보내기
- [ ] 다크 / 라이트 테마 전환

<br/>

## 실행 방법
```bash
# REPL 모드
cargo run -p rustdb-cli

# 서버 모드
cargo run -p rustdb-server

# UI 모드
cd rustdb-ui && npm run tauri dev
```

<br/>

## 테스트 쿼리
```sql
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
```

<br/>

## 기술 스택

| 항목 | 내용 |
|------|------|
| 언어 | Rust |
| 버전 | v2.2.0 |
| 인덱스 | B+Tree (단일 / 복합 / 클러스터드) |
| 옵티마이저 | 비용 기반 플래너 (AccessPath · Join 알고리즘 자동 선택) |
| Join | Hash Join (O(N+M)) / Nested Loop Join |
| 트랜잭션 | WAL (바이너리 redo log) + Undo Log + MVCC |
| 격리 수준 | READ UNCOMMITTED ~ SERIALIZABLE (4단계) |
| 동시성 | Row-level Locking (SELECT FOR UPDATE) |
| 캐시 | Buffer Pool (LRU, 64페이지, 16KB) |
| 저장 | 바이너리 .rdb + LZ4 압축 + indexes.json + views.json |
| UI | Tauri + React + Monaco Editor |
| TCP 서버 | 멀티 클라이언트, 포트 7878, 라인 프로토콜 |
| AI 연동 | MCP AI API (예정) |

<br/>

## 프로젝트 구조
```
code/
├── rustdb-core/     DB 엔진 라이브러리
├── rustdb-server/   TCP 서버
├── rustdb-cli/      터미널 REPL
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
│  Buffer Pool (LRU 64p 16KB)              │
│  MVCC (_xmin / _xmax 버전 스탬프)        │
│  바이너리 .rdb + LZ4 압축 저장           │
│  인덱스/뷰 영속화 (indexes/views.json)   │
│                                          │
└──────────────────────────────────────────┘
        ↓              ↓
  rustdb-cli      rustdb-server
  (터미널 REPL)   (TCP 서버)
        ↓
  rustdb-ui        rustdb-mcp
  (Tauri + React)  (MCP, 개발 예정)
```

<br/>

## B+ Tree에 관하여
[B+ Tree 구조 이해](https://chanho0912.tistory.com/109)

[B+ Tree 이해 - velog](https://velog.io/@emplam27/%EC%9E%90%EB%A3%8C%EA%B5%AC%EC%A1%B0-%EA%B7%B8%EB%A6%BC%EC%9C%BC%EB%A1%9C-%EC%95%8C%EC%95%84%EB%B3%B4%EB%8A%94-B-Plus-Tree)
