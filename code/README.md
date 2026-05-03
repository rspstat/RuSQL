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

### 다중 데이터베이스
- [x] CREATE DATABASE / CREATE DATABASE IF NOT EXISTS
- [x] DROP DATABASE / DROP DATABASE IF EXISTS
- [x] USE {database} / USE DATABASE {database}
- [x] SHOW TABLES (현재 DB 기준)
- [x] SHOW DATABASES
- [x] 테이블 이름 자동 한정 (`{current_db}.{table}` 내부 처리)
- [x] 데이터베이스 간 완전한 데이터 격리
- [x] Buffer Pool 무효화 (DROP 후 재생성 시 잔존 데이터 없음)
- [x] 디스크 기반 DB 목록 로드 (하드코딩 제거 — 실제 존재하는 DB만 사이드바에 표시)

### DDL
- [x] CREATE TABLE / DROP TABLE / DROP TABLE IF EXISTS
- [x] TRUNCATE TABLE
- [x] ALTER TABLE (ADD / MODIFY / DROP / RENAME COLUMN)
- [x] ALTER TABLE RENAME TO (테이블 이름 변경)
- [x] CREATE INDEX / DROP INDEX (단일 / 복합)
- [x] CREATE VIEW / DROP VIEW
- [x] DESCRIBE (테이블 스키마 조회)

### DML
- [x] INSERT (전체 컬럼 / 컬럼 지정 / 멀티 row)
- [x] INSERT ... SELECT (SELECT 결과를 다른 테이블에 삽입)
- [x] INSERT IGNORE (UNIQUE 위반 행 조용히 무시)
- [x] INSERT ... ON DUPLICATE KEY UPDATE (중복 키 시 UPDATE로 전환, 다중 컬럼 대입 지원)
- [x] SELECT
- [x] UPDATE (상수 / 산술 표현식 / 스칼라 함수 / 자기 참조 — `salary = salary * 1.1`, `name = CONCAT(name, '_v2')`)
- [x] UPDATE 다중 테이블 — `UPDATE t1, t2 SET t1.col = ..., t2.col = ... WHERE ...`
- [x] DELETE (MVCC 논리 삭제 / 물리 삭제)
- [x] DELETE 다중 테이블 — `DELETE t1, t2 FROM t1 JOIN t2 ON ... WHERE ...`

### 쿼리 기능
- [x] WHERE (=, !=, >, <, >=, <=)
- [x] AND / OR / NOT 복합 조건 — `NOT (price > 100 OR active = 0)`
- [x] IN (리터럴 목록) — `WHERE id IN (1, 2, 3)`
- [x] NOT IN (리터럴 목록) — `WHERE id NOT IN (2, 4)`
- [x] IN / NOT IN (서브쿼리) — `WHERE dept_id IN (SELECT id FROM dept)`
- [x] BETWEEN / LIKE (%, _ 와일드카드)
- [x] IS NULL / IS NOT NULL
- [x] INNER JOIN / LEFT JOIN / RIGHT JOIN
- [x] Hash Join (대용량 Equi-Join O(N+M)) / Nested Loop Join (소규모·비등가) — ON 조건 방향 무관 (left.col = right.col / right.col = left.col 모두 지원)
- [x] 테이블 별칭 (alias) — `FROM emp e JOIN dept d ON e.dept_id = d.id`
- [x] ORDER BY (ASC / DESC, 다중 컬럼)
- [x] LIMIT / OFFSET — `LIMIT 10 OFFSET 20`
- [x] GROUP BY / HAVING
- [x] DISTINCT
- [x] 산술 표현식 — SELECT / WHERE / UPDATE SET에서 `price * qty`, `salary + 100`
- [x] 집계 함수 — COUNT / SUM / AVG / MIN / MAX
- [x] GROUP_CONCAT (SEPARATOR 옵션, GROUP BY 및 비집계 양쪽 지원)
- [x] CASE WHEN ... THEN ... ELSE ... END
- [x] 스칼라 함수 — UPPER / LOWER / LENGTH / TRIM / CONCAT / SUBSTR / REPLACE / LPAD / RPAD
- [x] 수학 함수 — ROUND / ABS / CEIL / FLOOR / MOD (함수 인자 내 산술식 지원: `ROUND(salary / 1000000, 2)`)
- [x] 날짜 함수 — NOW / CURDATE / DATE_FORMAT / DATEDIFF / DATE_ADD (DAY/MONTH/YEAR/HOUR/MINUTE/SECOND)
- [x] NULL 처리 함수 — COALESCE / IFNULL / NULLIF
- [x] 타입 변환 — CAST(expr AS INT/FLOAT/TEXT/DATE)
- [x] 조건 함수 — IF(cond, true_val, false_val)
- [x] FROM 없는 스칼라 SELECT — `SELECT 1+1`, `SELECT NOW()` (`_dual_` 가상 테이블 방식)
- [x] 서브쿼리 — WHERE col = / > / < (SELECT ...)
- [x] 상관 서브쿼리 — WHERE EXISTS (SELECT 1 FROM ... WHERE outer.col = inner.col)
- [x] FROM 절 서브쿼리 — FROM (SELECT ...) AS alias
- [x] UNION / UNION ALL (ORDER BY / LIMIT / OFFSET 포함)
- [x] CTE (WITH ... AS) — 단순 / 다중 / INSERT 메인 쿼리 지원
- [x] 재귀 CTE (WITH RECURSIVE) — base case + UNION ALL 반복, positional 컬럼 매핑
- [x] SELECT ... FOR UPDATE (행 잠금)
- [x] table.column dot notation (SELECT / JOIN ON / GROUP BY / ORDER BY)
- [x] EXPLAIN (비용 기반 실행 계획 조회)
- [x] SHOW TABLES / DESCRIBE

### 데이터 타입
- [x] INT
- [x] VARCHAR(n) — 최대 길이 제한
- [x] DOUBLE / FLOAT
- [x] DECIMAL(p, s) — 정밀도 / 소수 자리수
- [x] TEXT
- [x] DATE — 'YYYY-MM-DD' 형식
- [x] DATETIME — 'YYYY-MM-DD HH:MM:SS' 형식
- [x] TIMESTAMP — 'YYYY-MM-DD HH:MM:SS' (값 없으면 현재 시각 자동 삽입)
- [x] TIME — 'HH:MM:SS' 형식
- [x] YEAR — 연도 값 (예: 2024)
- [x] BOOLEAN — true / false
- [x] ENUM('val1','val2',...) — 열거형
- [x] SET('a','b',...) — 집합형 (콤마 구분 복수 값)

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
- [x] FOREIGN KEY SET DEFAULT (DEFAULT 값으로 변경)
- [x] ON UPDATE CASCADE
- [x] ON UPDATE SET NULL / SET DEFAULT
- [x] NO ACTION (RESTRICT 동등)

### 트랜잭션
- [x] WAL (Write-Ahead Logging) — 바이너리 redo log
- [x] WAL fsync per-commit — COMMIT 레코드 기록 시 `sync_all()` 호출 (`innodb_flush_log_at_trx_commit=1` 동등, 전원 장애 시 커밋 유실 방지)
- [x] BEGIN / COMMIT / ROLLBACK
- [x] SAVEPOINT / ROLLBACK TO SAVEPOINT
- [x] Undo Log 기반 롤백 (B+Tree 인덱스 재빌드 포함)
- [x] WAL 기반 Crash Recovery (재시작 시 자동 복구)
- [x] Checkpoint (WAL 자동 트런케이션, 512KB 임계값, fsync 보장)
- [x] 트랜잭션 격리 수준 4단계
  - READ UNCOMMITTED / READ COMMITTED
  - REPEATABLE READ (BEGIN 시점 스냅샷 고정)
  - SERIALIZABLE (팬텀 읽기 감지 + 자동 롤백)

### 인덱스 & 저장
- [x] B+Tree 인덱스 (단일 컬럼)
- [x] 복합 인덱스 (다중 컬럼, null-byte 키 결합)
- [x] 클러스터드 인덱스 (PK 기준 물리적 정렬 유지)
- [x] 보조 인덱스 중복 키 지원 (배열 저장, 동일 컬럼 값 다중 행)
- [x] 보조 인덱스 자동 재빌드 (UPDATE / 다중 테이블 UPDATE 후 stale 방지)
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

### 사용자 관리 / 권한
- [x] CREATE USER [IF NOT EXISTS] `'user'@'host'` [IDENTIFIED BY 'password']
- [x] DROP USER [IF EXISTS] `'user'@'host'`
- [x] GRANT privilege [, ...] ON object TO `'user'@'host'` [WITH GRANT OPTION]
- [x] REVOKE privilege [, ...] ON object FROM `'user'@'host'`
- [x] SHOW GRANTS [FOR `'user'@'host'`]
- [x] 사용자·권한 영속화 (`_users.json`, `_grants.json`)

### 모니터링
- [x] SHOW BUFFER POOL (캐시 히트율, 사용량)
- [x] SHOW WAL (로그 레코드, 파일 크기)
- [x] SHOW ISOLATION LEVEL
- [x] SHOW LOCKS
- [x] SHOW DATABASES
- [x] CHECKPOINT (수동 체크포인트)
- [x] VACUUM (dead row 물리 제거)

### SQL 문법
- [x] 주석 지원 (-- 한 줄 / # MySQL 스타일 / /* */ 블록)
- [x] 주석 내 세미콜론 안전 처리 (쿼리 분리 오작동 없음)
- [x] 세미콜론(;) 구분 멀티 쿼리 입력
- [x] 세미콜론 뒤 인라인 주석 잔류 처리 (`SELECT 1; -- 0` 패턴에서 `-- 0` 잔류가 다음 쿼리를 주석으로 오파싱하던 버그 수정)
- [x] 함수 인자 내 산술식 지원 — `ROUND(salary / 1000000, 2)` 등 ArithExpr::Func AST + parse_func_args 재작성으로 지원
- [x] UPDATE SET 스칼라 함수 — `UPDATE t SET col = CONCAT(name, '-', dept)` eval_arith Func 분기 개선으로 지원

### UI (rustdb-ui)
- [x] Tauri + React 데스크탑 앱
- [x] Monaco Editor (SQL 문법 강조, 주석 회색)
- [x] 다중 쿼리 탭 (탭 추가 / 전환 / 닫기, localStorage 자동 저장)
- [x] 사이드바 — MySQL Workbench 스타일 (Database > Tables / Views / Indexes)
- [x] 사이드바 다중 데이터베이스 독립 펼치기 (여러 DB 동시 확장 가능)
- [x] 사이드바 더블클릭으로 활성 데이터베이스 전환 (`USE dbname`)
- [x] 사이드바 테이블 / 컬럼 목록 (여러 테이블 동시 펼치기)
- [x] 사이드바 컬럼 상세 — 타입 배지, PK🔑 / FK🔗 아이콘, NOT NULL / UNIQUE 뱃지 (`get_columns_detail` Tauri 커맨드)
- [x] 사이드바 VIEW / INDEX 개별 항목 펼치기 (여러 항목 동시 펼치기)
- [x] 사이드바 섹션 접기/펼치기, 개수 뱃지
- [x] 사이드바 너비 조절 (드래그)
- [x] 테이블 우클릭 컨텍스트 메뉴 (MySQL 스타일)
- [x] GUI 테이블 브라우저 뷰 (데이터 그리드 + 필터)
- [x] TCP 서버 관리 뷰 (시작 / 중지 / 로그)
- [x] AI Assistant 뷰 (사이드바 4번째 아이콘, 준비 중)
- [x] 멀티 쿼리 결과 표시
- [x] 결과 페이지네이션 — PAGE_SIZE=100, 초과 시 ‹/› 버튼 + 페이지 표시
- [x] 쿼리 히스토리 — 결과 패널 HISTORY 탭, localStorage 최대 200개, 클릭 시 에디터 불러오기
- [x] 쿼리 자동 저장 (탭별)
- [x] 결과창 크기 조절 (드래그)
- [x] 전체 스크롤바 스타일 통일 (Monaco 에디터 스크롤바 기준)

<br/>

## 진행 예정

### 엔진 개선 (우선순위 순)
- [ ] Undo Log 영속화 — crash 시 미완료 트랜잭션 복구 (현재 인메모리)
- [ ] GAP Lock / Next-key Lock — Serializable 팬텀 방지 정확도 개선
- [ ] MVCC 버전 체인 — `_xmin/_xmax` 컬럼 방식 → 언두 버전 체인
- [ ] 진정한 다중 세션 동시성 — 세션별 독립 Executor + 공유 BufferPool
- [ ] 커버링 인덱스 (Index-only scan)
- [ ] Sort-Merge Join
- [ ] B+Tree 리프 연결 리스트 (범위 스캔 O(k))
- [ ] WAL Group Commit (TPS 향상)

### 네트워크
- [x] TCP 서버 (포트 7878)
- [x] 멀티 클라이언트 동시 접속 (스레드 per 클라이언트)
- [ ] 클라이언트 CLI (`rustdb-client`)

### MCP 연동 (`rustdb-mcp`)
- [x] AI Assistant 뷰 (UI 레이아웃 완성)
- [ ] AI API 클라이언트 연결
- [ ] 자연어 → SQL 변환 (`\ai` 명령어)
- [ ] 변환된 SQL 확인 후 실행

### UI
- [x] 쿼리 히스토리
- [ ] 결과 CSV 내보내기
- [ ] 다크 / 라이트 테마 전환
- [ ] 탭별 결과 보존 (탭 전환 시 결과 패널 유지)

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

`test/test_full.sql` — 4개 데이터베이스, 34개 섹션, **260여 개 쿼리, 의도된 오류 1개** (UNIQUE 위반 검증)로 전체 기능을 검증합니다.

| DB | 테이블 | 주요 검증 항목 |
|----|--------|----------------|
| hrdb | departments · employees · salaries | SELECT / JOIN / 서브쿼리 / CTE / 집계 / CASE WHEN / VIEW / ALTER TABLE / FK CASCADE / 트랜잭션 / EXPLAIN |
| shopdb | categories · products · orders | INSERT IGNORE / ON DUPLICATE KEY UPDATE / FK ON UPDATE CASCADE / 다중 테이블 DELETE |
| logdb | servers · events · metrics | DOUBLE / TIME 타입 / FK CASCADE DELETE / VIEW / GROUP BY |
| testdb | dept · emp · org_tree · staff … | GROUP_CONCAT / FK SET DEFAULT / 재귀 CTE / ROUND(expr/expr, n) / UPDATE SET CONCAT / 사용자 관리 (CREATE USER · GRANT · REVOKE · SHOW GRANTS · DROP USER) |

```bash
# 전체 기능 테스트
cargo run -p rustdb-cli < test/test_full.sql
```

빠른 확인용 예시 (3개 DB):

```sql
-- SETUP
DROP DATABASE IF EXISTS shopdb;
DROP DATABASE IF EXISTS hrdb;
DROP DATABASE IF EXISTS logdb;

-- DATABASE 1: shopdb (전자상거래)
CREATE DATABASE shopdb;
USE shopdb;

-- Tables: 3개
CREATE TABLE categories (
    id INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(50) NOT NULL UNIQUE,
    discount_rate INT DEFAULT 0
);
CREATE TABLE products (
    id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(100) NOT NULL,
    category_id INT, price INT CHECK (price > 0), stock INT DEFAULT 0,
    FOREIGN KEY (category_id) REFERENCES categories(id) ON DELETE SET NULL ON UPDATE CASCADE
);
CREATE TABLE orders (
    id INT PRIMARY KEY AUTO INCREMENT, product_id INT,
    quantity INT CHECK (quantity > 0), total INT,
    status ENUM('pending','shipped','done') DEFAULT 'pending',
    FOREIGN KEY (product_id) REFERENCES products(id) ON DELETE SET NULL
);

INSERT INTO categories (name, discount_rate) VALUES ('Electronics', 10), ('Clothing', 20), ('Food', 5);
INSERT INTO products (name, category_id, price, stock) VALUES
    ('Laptop', 1, 1200000, 15), ('Phone', 1, 800000, 30),
    ('T-Shirt', 2, 25000, 100), ('Jeans', 2, 60000, 50),
    ('Coffee', 3, 15000, 200), ('Bread', 3, 3500, 80);
INSERT INTO orders (product_id, quantity, total, status) VALUES
    (1, 2, 2400000, 'done'), (2, 5, 4000000, 'shipped'),
    (3, 10, 250000, 'done'), (4, 3, 180000, 'pending'),
    (1, 1, 1200000, 'done'), (5, 20, 300000, 'shipped');

-- Indexes: 3개
CREATE INDEX idx_products_category ON products (category_id);
CREATE INDEX idx_orders_product ON orders (product_id);
CREATE INDEX idx_products_price ON products (price);

-- Views: 2개
CREATE VIEW v_top_products AS
    SELECT p.id, p.name, p.price, c.name AS category
    FROM products p JOIN categories c ON p.category_id = c.id WHERE p.price > 50000;
CREATE VIEW v_order_summary AS
    SELECT o.id, p.name AS product, o.quantity, o.total, o.status
    FROM orders o JOIN products p ON o.product_id = p.id;

SHOW TABLES;

-- DATABASE 2: hrdb (인사관리)
CREATE DATABASE hrdb;
USE hrdb;

-- Tables: 2개
CREATE TABLE employees (
    id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(50) NOT NULL,
    dept VARCHAR(30), position VARCHAR(30), hire_year YEAR, active INT DEFAULT 1
);
CREATE TABLE salaries (
    id INT PRIMARY KEY AUTO INCREMENT, employee_id INT,
    amount INT CHECK (amount > 0), grade ENUM('S1','S2','S3','S4','S5'),
    FOREIGN KEY (employee_id) REFERENCES employees(id) ON DELETE CASCADE
);

INSERT INTO employees (name, dept, position, hire_year, active) VALUES
    ('Alice', 'Engineering', 'Lead Engineer', 2019, 1),
    ('Bob', 'Engineering', 'Senior Engineer', 2020, 1),
    ('Carol', 'Marketing', 'Marketing Manager', 2018, 1),
    ('Dave', 'Marketing', 'Marketing Analyst', 2021, 1),
    ('Eve', 'HR', 'HR Manager', 2017, 1),
    ('Frank', 'HR', 'HR Specialist', 2022, 0),
    ('Grace', 'Finance', 'CFO', 2015, 1),
    ('Henry', NULL, 'Consultant', 2023, 1);
INSERT INTO salaries (employee_id, amount, grade) VALUES
    (1, 9500000, 'S4'), (2, 8500000, 'S3'), (3, 7200000, 'S3'), (4, 5800000, 'S2'),
    (5, 6500000, 'S2'), (6, 4500000, 'S1'), (7, 12000000, 'S5'), (8, 5000000, 'S1');

-- Indexes: 3개
CREATE INDEX idx_emp_dept ON employees (dept);
CREATE INDEX idx_emp_active ON employees (active);
CREATE INDEX idx_sal_employee ON salaries (employee_id);

-- Views: 3개
CREATE VIEW v_active_employees AS
    SELECT id, name, dept, position FROM employees WHERE active = 1;
CREATE VIEW v_high_earners AS
    SELECT employee_id, amount, grade FROM salaries WHERE amount > 7000000;
CREATE VIEW v_emp_detail AS
    SELECT e.id, e.name, e.position, e.dept, s.amount, s.grade
    FROM employees e LEFT JOIN salaries s ON e.id = s.employee_id;

SHOW TABLES;

-- DATABASE 3: logdb (시스템 로그)
CREATE DATABASE logdb;
USE logdb;

-- Tables: 3개
CREATE TABLE servers (
    id INT PRIMARY KEY AUTO INCREMENT, hostname VARCHAR(50) NOT NULL UNIQUE,
    region VARCHAR(20), cpu_cores INT DEFAULT 4
);
CREATE TABLE events (
    id INT PRIMARY KEY AUTO INCREMENT, server_id INT,
    severity ENUM('INFO','WARN','ERROR'), message VARCHAR(200), response_ms INT,
    FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE
);
CREATE TABLE metrics (
    id INT PRIMARY KEY AUTO INCREMENT, server_id INT,
    cpu_pct DOUBLE, mem_pct DOUBLE, checkin TIME,
    FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE
);

INSERT INTO servers (hostname, region, cpu_cores) VALUES
    ('web-01', 'ap-seoul', 8), ('web-02', 'ap-seoul', 8),
    ('db-01', 'ap-busan', 16), ('cache-01', 'ap-seoul', 4);
INSERT INTO events (server_id, severity, message, response_ms) VALUES
    (1, 'INFO', 'Request processed', 45), (1, 'WARN', 'Memory usage high', 120),
    (2, 'INFO', 'Request processed', 38), (3, 'ERROR', 'Disk I/O timeout', 5000),
    (3, 'WARN', 'CPU spike detected', 200), (3, 'ERROR', 'Connection pool exhausted', 3000),
    (4, 'INFO', 'Cache hit', 5), (4, 'WARN', 'Cache eviction', 80);
INSERT INTO metrics (server_id, cpu_pct, mem_pct, checkin) VALUES
    (1, 45.5, 62.3, '09:00:00'), (1, 78.2, 71.0, '10:00:00'),
    (2, 32.1, 55.8, '09:00:00'), (3, 95.7, 88.4, '09:00:00'),
    (3, 91.2, 90.1, '10:00:00'), (4, 12.5, 40.0, '09:00:00');

-- Indexes: 3개
CREATE INDEX idx_events_server ON events (server_id);
CREATE INDEX idx_events_severity ON events (severity);
CREATE INDEX idx_metrics_server ON metrics (server_id);

-- Views: 2개
CREATE VIEW v_error_events AS
    SELECT server_id, message, response_ms FROM events WHERE severity = 'ERROR';
CREATE VIEW v_server_load AS
    SELECT server_id, AVG(cpu_pct) AS avg_cpu, MAX(cpu_pct) AS peak_cpu, AVG(mem_pct) AS avg_mem
    FROM metrics GROUP BY server_id;

SHOW TABLES;

-- SELECT / ORDER BY / LIMIT / OFFSET / DISTINCT / ARITHMETIC
USE hrdb;
SELECT id, name, position FROM employees WHERE active = 1 ORDER BY id;
SELECT id, name FROM employees ORDER BY id LIMIT 3 OFFSET 2;
SELECT DISTINCT dept FROM employees ORDER BY dept;
SELECT employee_id, amount, amount * 1.1 AS raise_10pct FROM salaries WHERE amount > 8000000;

-- IN / NOT IN / NOT / BETWEEN / LIKE / IS NULL
SELECT name FROM employees WHERE dept IN ('Engineering', 'Marketing');
SELECT name FROM employees WHERE dept NOT IN ('HR', 'Finance');
SELECT name FROM employees WHERE NOT (active = 1);
SELECT employee_id, amount FROM salaries WHERE amount BETWEEN 6000000 AND 9000000;
SELECT name FROM employees WHERE name LIKE 'A%' OR name LIKE 'G%';
SELECT name FROM employees WHERE dept IS NULL;
SELECT name FROM employees WHERE dept IS NOT NULL AND active = 1 ORDER BY id;

-- AGGREGATE / GROUP BY / HAVING
SELECT COUNT(*) AS total, AVG(amount) AS avg_sal, MAX(amount) AS max_sal, MIN(amount) AS min_sal, SUM(amount) AS payroll FROM salaries;
SELECT grade, COUNT(*) AS cnt, AVG(amount) AS avg_sal FROM salaries GROUP BY grade HAVING cnt >= 2 ORDER BY avg_sal DESC;

-- JOIN (INNER / LEFT)
SELECT e.name, e.dept, s.amount, s.grade
    FROM employees e JOIN salaries s ON e.id = s.employee_id
    ORDER BY s.amount DESC;
SELECT e.name, e.dept FROM employees e LEFT JOIN salaries s ON e.id = s.employee_id ORDER BY e.id;

-- SUBQUERY (IN / scalar / EXISTS / derived)
SELECT name FROM employees WHERE id IN (SELECT employee_id FROM salaries WHERE amount > 8000000);
SELECT employee_id, amount FROM salaries WHERE amount > (SELECT AVG(amount) FROM salaries);
SELECT name FROM employees WHERE EXISTS (SELECT 1 FROM salaries WHERE employee_id = employees.id AND amount > 9000000);
SELECT grade, avg_amt FROM (SELECT grade, AVG(amount) AS avg_amt FROM salaries GROUP BY grade) AS gs WHERE avg_amt > 6000000;

-- UNION / UNION ALL
SELECT name FROM employees WHERE dept = 'Engineering'
UNION SELECT name FROM employees WHERE dept = 'Finance';
SELECT employee_id, amount FROM salaries WHERE grade = 'S5'
UNION ALL SELECT employee_id, amount FROM salaries WHERE grade = 'S1'
ORDER BY amount DESC;

-- SCALAR FUNCTIONS / CASE WHEN
SELECT UPPER(name) AS up, LENGTH(name) AS len, CONCAT(name, '@company.com') AS email FROM employees WHERE active = 1 LIMIT 4;
SELECT COALESCE(dept, 'N/A') AS dept FROM employees WHERE dept IS NULL;
SELECT employee_id, amount,
    CASE WHEN amount >= 10000000 THEN 'Executive'
         WHEN amount >= 7000000 THEN 'Senior'
         WHEN amount >= 5000000 THEN 'Mid'
         ELSE 'Junior' END AS pay_level
    FROM salaries ORDER BY amount DESC;

-- CTE
WITH high_sal AS (SELECT employee_id, amount, grade FROM salaries WHERE amount > 7000000)
SELECT * FROM high_sal ORDER BY amount DESC;

-- VIEW 조회
SELECT * FROM v_active_employees ORDER BY id;
SELECT * FROM v_high_earners ORDER BY amount DESC;
SELECT * FROM v_emp_detail ORDER BY amount DESC;

-- EXPLAIN (인덱스 활용 확인)
EXPLAIN SELECT * FROM employees WHERE dept = 'Engineering';
EXPLAIN SELECT * FROM salaries WHERE employee_id = 1;
EXPLAIN SELECT * FROM employees WHERE active = 1;

-- ALTER TABLE
ALTER TABLE employees ADD COLUMN email VARCHAR(100);
ALTER TABLE employees MODIFY COLUMN email VARCHAR(150);
ALTER TABLE employees RENAME COLUMN email TO contact;
ALTER TABLE employees DROP COLUMN contact;
DESCRIBE employees;

-- UPDATE / DELETE + FK CASCADE
UPDATE employees SET position = 'Principal Engineer' WHERE id = 1;
UPDATE salaries SET amount = amount * 1.05 WHERE grade = 'S3';
DELETE FROM employees WHERE id = 6;
SELECT e.name, s.amount FROM employees e JOIN salaries s ON e.id = s.employee_id ORDER BY e.id;

-- CONSTRAINT ERROR (expected ERROR)
INSERT INTO salaries (employee_id, amount, grade) VALUES (1, -500, 'S1');

-- INSERT ... SELECT / TRUNCATE
CREATE TABLE sal_archive (id INT PRIMARY KEY, employee_id INT, amount INT);
INSERT INTO sal_archive SELECT id, employee_id, amount FROM salaries WHERE amount > 8000000;
SELECT * FROM sal_archive ORDER BY amount DESC;
TRUNCATE TABLE sal_archive;
DROP TABLE sal_archive;

-- TRANSACTION + SAVEPOINT
BEGIN;
INSERT INTO employees (name, dept, position, hire_year) VALUES ('Ivan', 'Research', 'Researcher', 2024);
SAVEPOINT sp1;
UPDATE employees SET position = 'Senior Researcher' WHERE name = 'Ivan';
ROLLBACK TO SAVEPOINT sp1;
COMMIT;
SELECT name, position FROM employees WHERE name = 'Ivan';

BEGIN;
UPDATE salaries SET amount = 1 WHERE id = 1;
ROLLBACK;
SELECT amount FROM salaries WHERE id = 1;

-- ISOLATION LEVEL
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;

-- logdb: 특수 타입 / 뷰 조회
USE logdb;
SELECT s.hostname, e.severity, e.message, e.response_ms
    FROM events e JOIN servers s ON e.server_id = s.id
    WHERE e.severity IN ('ERROR', 'WARN') ORDER BY e.response_ms DESC;
SELECT * FROM metrics WHERE cpu_pct > 50.0 ORDER BY cpu_pct DESC;
SELECT * FROM v_error_events;
SELECT * FROM v_server_load ORDER BY avg_cpu DESC;
DESCRIBE metrics;

-- shopdb: 뷰 / 인덱스 확인
USE shopdb;
SELECT * FROM v_top_products ORDER BY price DESC;
SELECT * FROM v_order_summary WHERE status != 'pending' ORDER BY total DESC;
EXPLAIN SELECT * FROM products WHERE category_id = 1;
EXPLAIN SELECT * FROM orders WHERE product_id = 1;

-- ADMIN
USE hrdb;
SHOW TABLES;
SHOW BUFFER POOL;
SHOW WAL;
SHOW LOCKS;
CHECKPOINT;
VACUUM;

-- CLEANUP
USE shopdb;
DROP INDEX IF EXISTS idx_products_category;
DROP INDEX IF EXISTS idx_orders_product;
DROP INDEX IF EXISTS idx_products_price;
DROP VIEW IF EXISTS v_top_products;
DROP VIEW IF EXISTS v_order_summary;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS products;
DROP TABLE IF EXISTS categories;

USE hrdb;
DROP INDEX IF EXISTS idx_emp_dept;
DROP INDEX IF EXISTS idx_emp_active;
DROP INDEX IF EXISTS idx_sal_employee;
DROP VIEW IF EXISTS v_active_employees;
DROP VIEW IF EXISTS v_high_earners;
DROP VIEW IF EXISTS v_emp_detail;
DROP TABLE IF EXISTS salaries;
DROP TABLE IF EXISTS employees;

USE logdb;
DROP INDEX IF EXISTS idx_events_server;
DROP INDEX IF EXISTS idx_events_severity;
DROP INDEX IF EXISTS idx_metrics_server;
DROP VIEW IF EXISTS v_error_events;
DROP VIEW IF EXISTS v_server_load;
DROP TABLE IF EXISTS metrics;
DROP TABLE IF EXISTS events;
DROP TABLE IF EXISTS servers;

DROP DATABASE shopdb;
DROP DATABASE hrdb;
DROP DATABASE logdb;
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
  rustdb-ui        rustdb-mcp
  (Tauri + React)  (MCP, 개발 예정)
```

<br/>

## B+ Tree에 관하여
[B+ Tree 구조 이해](https://chanho0912.tistory.com/109)

[B+ Tree 이해 - velog](https://velog.io/@emplam27/%EC%9E%90%EB%A3%8C%EA%B5%AC%EC%A1%B0-%EA%B7%B8%EB%A6%BC%EC%9C%BC%EB%A1%9C-%EC%95%8C%EC%95%84%EB%B3%B4%EB%8A%94-B-Plus-Tree)
