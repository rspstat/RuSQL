## MCP 기반 커스텀 RDBMS

- Rust로 구현한 데이터베이스 엔진 + RDBMS + AI MCP 

<br/>

## 핵심 기능

| 분류 | 내용 |
|------|------|
| DB 엔진 | B+Tree, WAL, Buffer Pool, MVCC, 트랜잭션 |
| SQL 지원 | DDL / DML / JOIN / 서브쿼리 / 제약조건 / 트랜잭션 |
| MCP | 자연어 입력 → SQL 자동 생성 → 실행 |
| DBMS | TCP 서버, 다중 클라이언트 동시 접속 |
| 언어 | Rust |

<br/>

## 완료된 기능

### 엔진 코어
- [x] Lexer / Tokenizer
- [x] SQL Parser (AST 기반, 재귀 하강)
- [x] Executor (쿼리 실행 엔진)

### DDL
- [x] CREATE TABLE / DROP TABLE / DROP TABLE IF EXISTS
- [x] TRUNCATE TABLE
- [x] ALTER TABLE (ADD / MODIFY / DROP / RENAME COLUMN)
- [x] CREATE INDEX / DROP INDEX (단일 / 복합)
- [x] CREATE VIEW / DROP VIEW
- [x] DESCRIBE (테이블 스키마 조회)

### DML
- [x] INSERT (전체 컬럼 / 컬럼 지정 / 멀티 row)
- [x] SELECT
- [x] UPDATE
- [x] DELETE (MVCC 논리 삭제 / 물리 삭제)

### 쿼리 기능
- [x] WHERE (=, !=, >, <, >=, <=)
- [x] AND / OR 복합 조건
- [x] BETWEEN / LIKE (%, _ 와일드카드)
- [x] IS NULL / IS NOT NULL
- [x] INNER JOIN / LEFT JOIN / RIGHT JOIN
- [x] 테이블 별칭 (alias) — `FROM employees e JOIN departments d ON e.dept_id = d.id`
- [x] ORDER BY (ASC / DESC, 다중 컬럼)
- [x] GROUP BY / HAVING
- [x] LIMIT
- [x] DISTINCT
- [x] 집계 함수 (COUNT, SUM, AVG, MIN, MAX)
- [x] 스칼라 함수 — UPPER / LOWER / LENGTH / TRIM / CONCAT / SUBSTR / REPLACE
- [x] 수학 함수 — ROUND / ABS / CEIL / FLOOR / MOD
- [x] 날짜 함수 — NOW / CURDATE / DATE_FORMAT
- [x] NULL 처리 함수 — COALESCE / IFNULL
- [x] 서브쿼리 — WHERE col IN / NOT IN (SELECT ...)
- [x] 서브쿼리 — WHERE col = / > / < (SELECT ...)
- [x] 상관 서브쿼리 — WHERE EXISTS (SELECT 1 FROM ... WHERE outer.col = inner.col)
- [x] FROM 절 서브쿼리 — FROM (SELECT ...) AS alias
- [x] SHOW TABLES / DESCRIBE
- [x] SELECT ... FOR UPDATE (행 잠금)
- [x] table.column dot notation (SELECT / JOIN ON / GROUP BY / ORDER BY)
- [x] EXPLAIN (실행 계획 조회)

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
- [x] 수치 인식 키 비교 (`"10" > "9"` 정상 처리)
- [x] 바이너리 디스크 저장 (.rdb 포맷, 16KB 페이지)
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

### 저장소
- [ ] 데이터 압축 (.rdb 파일)

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
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS items;
DROP TABLE IF EXISTS emp;
DROP TABLE IF EXISTS dept;
DROP VIEW IF EXISTS v_active;
DROP INDEX IF EXISTS idx_dept;

-- DDL
CREATE TABLE dept (
    id     INT          PRIMARY KEY AUTO INCREMENT,
    name   VARCHAR(50)  NOT NULL UNIQUE,
    budget DECIMAL(10,2) DEFAULT 0.00
);

CREATE TABLE emp (
    id         INT          PRIMARY KEY AUTO INCREMENT,
    name       VARCHAR(50)  NOT NULL,
    dept_id    INT,
    salary     DECIMAL(10,2) CHECK (salary > 0),
    hire_date  DATE,
    active     BOOLEAN      DEFAULT true,
    updated_at TIMESTAMP,
    FOREIGN KEY (dept_id) REFERENCES dept(id) ON DELETE SET NULL ON UPDATE CASCADE
);

CREATE TABLE orders (
    id     INT          PRIMARY KEY AUTO INCREMENT,
    emp_id INT,
    total  DECIMAL(10,2),
    FOREIGN KEY (emp_id) REFERENCES emp(id) ON DELETE RESTRICT
);

CREATE TABLE items (
    order_id INT,
    product  VARCHAR(50),
    qty      INT,
    PRIMARY KEY (order_id, product)
);

-- INSERT
INSERT INTO dept (name, budget) VALUES ('Engineering', 500000.00), ('HR', 100000.00);
INSERT INTO emp (name, dept_id, salary, hire_date, active) VALUES ('Alice', 1, 95000.00, '2020-03-15', true), ('Bob', 1, 85000.00, '2021-06-01', true), ('Carol', 2, 68000.00, '2022-01-10', false);
INSERT INTO orders (emp_id, total) VALUES (1, 1200.00), (2, 350.00);
INSERT INTO items VALUES (1, 'Laptop', 1), (1, 'Mouse', 2), (2, 'Keyboard', 1);

-- SELECT / WHERE / ORDER BY / LIMIT / DISTINCT
SELECT * FROM dept;
SELECT id, name, salary FROM emp WHERE salary > 70000 ORDER BY salary DESC LIMIT 2;
SELECT DISTINCT dept_id FROM emp ORDER BY dept_id;
SELECT id, name FROM emp WHERE salary BETWEEN 60000 AND 90000;
SELECT id, name FROM emp WHERE name LIKE 'A%';
SELECT id, name FROM emp WHERE hire_date IS NOT NULL;
SELECT id, name FROM emp WHERE dept_id IS NULL;

-- AGGREGATE + GROUP BY + HAVING
SELECT COUNT(*) AS total, AVG(salary) AS avg_sal, MAX(salary) AS max_sal FROM emp;
SELECT dept_id, COUNT(*) AS cnt FROM emp GROUP BY dept_id HAVING cnt > 1;

-- JOIN (table alias)
SELECT e.id, e.name, d.name AS dept FROM emp e JOIN dept d ON e.dept_id = d.id;
SELECT e.id, e.name, d.name AS dept FROM emp e LEFT JOIN dept d ON e.dept_id = d.id;

-- SUBQUERY
SELECT id, name FROM emp WHERE dept_id IN (SELECT id FROM dept WHERE budget > 300000);
SELECT id, name FROM emp WHERE salary > (SELECT AVG(salary) FROM emp);
SELECT id, name FROM dept WHERE EXISTS (SELECT 1 FROM emp WHERE dept_id = dept.id AND salary > 90000);
SELECT dept_id, avg_sal FROM (SELECT dept_id, AVG(salary) AS avg_sal FROM emp GROUP BY dept_id) AS s WHERE avg_sal > 80000;

-- SCALAR FUNCTIONS
SELECT UPPER(name) AS up, LOWER(name) AS lo, LENGTH(name) AS len, CONCAT(name, '@corp') AS email FROM emp LIMIT 1;
SELECT ROUND(salary, -3) AS rounded, ABS(salary) AS abs_sal, FLOOR(salary) AS fl, CEIL(salary) AS cl FROM emp LIMIT 1;
SELECT SUBSTR(name, 1, 3) AS nick, REPLACE(name, 'Alice', 'Alicia') AS renamed, TRIM(name) AS trimmed FROM emp LIMIT 1;
SELECT COALESCE(dept_id, 0) AS dept, IFNULL(dept_id, 0) AS dept2 FROM emp LIMIT 1;
SELECT DATE_FORMAT(hire_date, '%Y/%m/%d') AS fmt, CURDATE() AS today, NOW() AS ts FROM emp LIMIT 1;

-- COMPOSITE PK
SELECT * FROM items;
SELECT order_id, SUM(qty) AS total_qty FROM items GROUP BY order_id;

-- CONSTRAINT VIOLATION (expected ERROR)
INSERT INTO emp (name, dept_id, salary) VALUES ('Bad', 1, -500.00);
INSERT INTO orders (emp_id, total) VALUES (999, 100.00);

-- MULTI-COLUMN UPDATE + FK ON UPDATE CASCADE
UPDATE dept SET name = 'Engineering Team', budget = 600000.00 WHERE id = 1;
SELECT id, name, dept_id FROM emp WHERE dept_id = 1;

-- DELETE + FK RESTRICT
DELETE FROM orders WHERE id = 1;
SELECT * FROM orders;

-- INDEX + EXPLAIN
CREATE INDEX idx_dept ON emp (dept_id);
EXPLAIN SELECT * FROM emp WHERE dept_id = 1;
EXPLAIN SELECT * FROM emp WHERE id BETWEEN 1 AND 3;
EXPLAIN SELECT * FROM emp WHERE salary > 70000;

-- VIEW (영속화: 재시작 후에도 유지됨)
CREATE VIEW v_active AS SELECT id, name, salary FROM emp WHERE active = true;
SELECT * FROM v_active;

-- ALTER TABLE
ALTER TABLE emp ADD COLUMN notes TEXT;
ALTER TABLE emp MODIFY COLUMN notes TEXT NOT NULL;
ALTER TABLE emp RENAME COLUMN notes TO memo;
DESCRIBE emp;
ALTER TABLE emp DROP COLUMN memo;

-- TRANSACTION + SAVEPOINT
BEGIN;
INSERT INTO dept (name, budget) VALUES ('Temp', 0.00);
SAVEPOINT sp1;
INSERT INTO dept (name, budget) VALUES ('Ghost', 0.00);
ROLLBACK TO SAVEPOINT sp1;
COMMIT;
SELECT id, name FROM dept;

-- TRANSACTION ROLLBACK
BEGIN;
UPDATE emp SET salary = 999999.00 WHERE id = 2;
SELECT id, name, salary FROM emp WHERE id = 2;
ROLLBACK;
SELECT id, name, salary FROM emp WHERE id = 2;

-- ISOLATION LEVEL
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;

-- SHOW / MONITOR
SHOW TABLES;
DESCRIBE emp;
SHOW BUFFER POOL;
SHOW WAL;
SHOW LOCKS;
CHECKPOINT;
VACUUM;

-- TRUNCATE + DROP
TRUNCATE TABLE items;
DROP TABLE items;
DROP TABLE orders;
DROP TABLE emp;
DROP TABLE dept;
DROP VIEW IF EXISTS v_active;
DROP INDEX IF EXISTS idx_dept;
SHOW TABLES;
```

<br/>

## 기술 스택

| 항목 | 내용 |
|------|------|
| 언어 | Rust |
| 버전 | v2.1.3 |
| 인덱스 | B+Tree (단일 / 복합 / 클러스터드) |
| 트랜잭션 | WAL (바이너리 redo log) + Undo Log + MVCC |
| 격리 수준 | READ UNCOMMITTED ~ SERIALIZABLE (4단계) |
| 동시성 | Row-level Locking (SELECT FOR UPDATE) |
| 캐시 | Buffer Pool (LRU, 64페이지, 16KB) |
| 저장 | 바이너리 .rdb + indexes.json + views.json |
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
│          Executor                        │
│  ┌───────────────────────────────┐       │
│  │ DDL: CREATE/DROP/ALTER/TRUNC  │       │
│  │ DML: INSERT/SELECT/UPDATE/DEL │       │
│  │ JOIN (INNER/LEFT/RIGHT)       │       │
│  │ 테이블 별칭 (alias)           │       │
│  │ WHERE / SUBQUERY / EXISTS     │       │
│  │ FROM 서브쿼리                 │       │
│  │ ORDER BY / GROUP BY / HAVING  │       │
│  │ 스칼라 / 날짜 / NULL 함수     │       │
│  │ 집계함수 / DISTINCT / LIMIT   │       │
│  │ INDEX (단일/복합/클러스터드)  │       │
│  │ EXPLAIN (실행 계획)           │       │
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
│  바이너리 .rdb 저장                      │
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
