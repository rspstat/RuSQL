## MCP 기반 커스텀 RDBMS

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

## 완료된 기능

### 엔진 코어
- [x] Lexer / Tokenizer
- [x] SQL Parser (AST 기반, 재귀 하강)
- [x] Executor (쿼리 실행 엔진)
- [x] 비용 기반 쿼리 옵티마이저 (Cost-Based Query Planner)
  - AccessPath 선택 (SeqScan / PkPoint / PkBetween / PkRange / SecondaryPoint / SecondaryRange / CompositeIndex)
  - 행 수 / 비용 추정 (log₂N 기반)
  - Join 알고리즘 자동 선택 (Sort-Merge Join / Hash Join / Nested Loop)
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

### DCL
- [x] CREATE USER [IF NOT EXISTS] `'user'@'host'` [IDENTIFIED BY 'password']
- [x] DROP USER [IF EXISTS] `'user'@'host'`
- [x] GRANT privilege [, ...] ON object TO `'user'@'host'` [WITH GRANT OPTION]
- [x] REVOKE privilege [, ...] ON object FROM `'user'@'host'`
- [x] SHOW GRANTS [FOR `'user'@'host'`]
- [x] 사용자·권한 영속화 (`_users.json`, `_grants.json`)

### 쿼리 기능
- [x] WHERE (=, !=, >, <, >=, <=)
- [x] AND / OR / NOT 복합 조건 — `NOT (price > 100 OR active = 0)`
- [x] IN (리터럴 목록) — `WHERE id IN (1, 2, 3)`
- [x] NOT IN (리터럴 목록) — `WHERE id NOT IN (2, 4)`
- [x] IN / NOT IN (서브쿼리) — `WHERE dept_id IN (SELECT id FROM dept)`
- [x] BETWEEN / LIKE (%, _ 와일드카드)
- [x] IS NULL / IS NOT NULL
- [x] INNER JOIN / LEFT JOIN / RIGHT JOIN
- [x] Sort-Merge Join (양쪽 > 4행 Equi-Join, O((N+M)logN) sort + O(N+M) merge, 투 포인터 키 그룹 병합)
- [x] Hash Join (한쪽 > 4행 Equi-Join, O(N+M)) / Nested Loop Join (소규모·비등가) — ON 조건 방향 무관 (left.col = right.col / right.col = left.col 모두 지원)
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
- [x] ENUM('val1','val2',...) — 열거형 (INSERT/UPDATE 시 허용값 검사, 허용 목록 외 값 오류 반환)
- [x] SET('a','b',...) — 집합형 (콤마 구분 복수 값, 각 요소 유효성 검사)

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
- [x] WAL Group Commit — 여러 세션의 COMMIT을 단일 fsync로 묶어 TPS 향상, SharedDatabase 락 해제 후 fsync
- [x] WAL fsync per-commit — COMMIT 레코드 기록 시 `sync_all()` 호출 (`innodb_flush_log_at_trx_commit=1` 동등, 전원 장애 시 커밋 유실 방지)
- [x] BEGIN / COMMIT / ROLLBACK
- [x] SAVEPOINT / ROLLBACK TO SAVEPOINT
- [x] Undo Log 기반 롤백 (B+Tree 인덱스 재빌드 포함)
- [x] Undo Log 영속화 (`data/_undo.log`) — 트랜잭션 중 변경마다 Undo Entry를 디스크에 즉시 기록, COMMIT/ROLLBACK 시 삭제, 크래시 후 재시작 시 미완료 트랜잭션 자동 롤백
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
- [x] 보조 인덱스 자동 재빌드 (INSERT / UPDATE / 다중 테이블 UPDATE 후 stale 방지)
- [x] 커버링 인덱스 (SELECT 컬럼 ⊆ 인덱스 컬럼 시 Index-only scan 자동 활성화)
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
- [x] SQL 키워드를 컬럼/별칭 이름으로 사용 — `ORDER BY avg DESC`, `GROUP BY count` 등 집계 함수명을 컬럼 참조 위치에서 식별자로 수락 (`expect_any_ident`)

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
- [x] ERD Editor 뷰 — 테이블 카드 + FK 관계선(직각 꺾임, 라운드 코너), 카드 드래그 / 캔버스 팬 / 휠 줌, 카드 클릭 시 하단 데이터 패널 (데이터 그리드 + 필터)
- [x] TCP 서버 관리 뷰 — DBeaver 스타일 연결 구성 폼 (호스트·포트 ±·사용자·비밀번호 토글), 메인/CLI 가이드 탭, 서버 랙 SVG 아이콘, 활동 로그
- [x] AI Assistant 뷰 (사이드바 4번째 아이콘, 준비 중)
- [x] 멀티 쿼리 결과 표시
- [x] 결과 페이지네이션 — PAGE_SIZE=100, 초과 시 ‹/› 버튼 + 페이지 표시
- [x] 쿼리 히스토리 — 결과 패널 HISTORY 탭, localStorage 최대 200개, 클릭 시 에디터 불러오기
- [x] 쿼리 자동 저장 (탭별)
- [x] 결과창 크기 조절 (드래그)
- [x] 전체 스크롤바 스타일 통일 (Monaco 에디터 스크롤바 기준)
- [x] 탭별 결과 보존 (탭 전환 시 결과 패널 유지, tabResults Record로 탭별 독립 관리)
- [x] Monaco SQL 자동완성 — 테이블명·컬럼명·SQL 키워드 약 60개, schemaRef로 실시간 스키마 반영
- [x] 결과 테이블 컬럼 너비 조절 — th 드래그 resize handle, 첫 드래그 시 DOM 실측값 기반 초기화, table-layout: fixed 전환

<br/>

## 진행 예정

### 엔진 개선 (우선순위 순)
- [x] Undo Log 영속화 — 크래시 후 재시작 시 미완료 트랜잭션 자동 롤백 (`data/_undo.log` 바이너리 영속화)
- [ ] GAP Lock / Next-key Lock — Serializable 팬텀 방지 정확도 개선
- [ ] MVCC 버전 체인 — `_xmin/_xmax` 컬럼 방식 → 언두 버전 체인
- [x] 진정한 다중 세션 동시성 — 세션별 독립 Executor + 공유 SharedDatabase (Arc<RwLock<SharedDatabase>> 분리)
- [x] 커버링 인덱스 (Index-only scan) — SELECT 컬럼 ⊆ 인덱스 컬럼 시 JSON 역직렬화 생략, EXPLAIN "(Covering)" 표시
- [x] B+Tree ORDER 증가 (4 → 16) — 트리 깊이 감소, 노드 분할 빈도 절감
- [x] Sort-Merge Join
- [x] B+Tree 범위 스캔 최적화 (scan_from_node / scan_to_node 가지치기, O(log N + k))
- [x] WAL Group Commit (TPS 향상) — 여러 세션의 COMMIT을 단일 fsync로 묶음, SharedDatabase 락 해제 후 fsync 수행

### 네트워크
- [x] TCP 서버 (포트 7878)
- [x] 멀티 클라이언트 동시 접속 (스레드 per 클라이언트)
- [x] 진정한 다중 세션 동시성 — `SharedDatabase`를 `Arc<RwLock<SharedDatabase>>`로 추출, 각 TCP 클라이언트는 `Executor::new_session(shared)`으로 독립 Executor(트랜잭션·`current_db`) 보유, 전역 `Mutex` 없이 병렬 쿼리 실행
- [x] TCP AUTH 인증 — 연결 시 `AUTH user pass` 핸드셰이크, users 없으면 `root`/`root` 자동 생성 (`rustdb-server` + Tauri 내장 서버 동일 프로토콜)
- [x] 클라이언트 CLI (`rustdb-client`) — AUTH 핸드셰이크, 멀티라인 SQL, ANSI 색상 출력

### MCP 연동 (`rustdb-mcp`)
- [x] AI Assistant 뷰 (UI 레이아웃 완성)
- [ ] AI API 클라이언트 연결
- [ ] 자연어 → SQL 변환 (`\ai` 명령어)
- [ ] 변환된 SQL 확인 후 실행

### UI
- [x] 쿼리 히스토리
- [ ] 결과 CSV 내보내기
- [ ] 다크 / 라이트 테마 전환
- [x] 탭별 결과 보존 (탭 전환 시 결과 패널 유지)

<br/>

## 실행 방법
```bash
# REPL 모드
cargo run -p rustdb-cli

# 서버 모드 (TCP, 포트 7878)
cargo run -p rustdb-server

# 클라이언트 CLI (서버 실행 후)
cargo run -p rustdb-client -- -u root -p root -h 127.0.0.1 -P 7878

# UI 모드
cd rustdb-ui && npm run tauri dev
```

<br/>

## 테스트 쿼리

`test/test_full.sql` — **단일 DB(`db1`), 전 기능 커버, 의도된 오류 3개** (ENUM/SET 유효성 위반)

```bash
# code/ 디렉터리에서 실행
cargo run -p rustdb-cli < test/test_full.sql
```

```sql
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
    id      INT PRIMARY KEY AUTO INCREMENT,
    val     ENUM('a','b','c'),
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

-- scalar functions: date
SELECT name, hdate, DATEDIFF('2026-05-01', hdate) AS days,
       DATE_ADD(hdate, INTERVAL 1 YEAR) AS nxt,
       DATE_FORMAT(hdate, '%Y-%m') AS ym
FROM emp WHERE status = 'active' ORDER BY hdate LIMIT 3;

-- null / cast / scalars
SELECT COALESCE(dept_id,-1) FROM emp WHERE dept_id IS NULL;
SELECT IFNULL(dept_id,0), NULLIF(dept_id,3) FROM emp ORDER BY id LIMIT 4;
SELECT CAST('2026' AS INT), CAST('3.14' AS FLOAT);
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

-- UPDATE arithmetic
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
UPDATE emp SET salary = salary - 100 WHERE dept_id = 1;
UPDATE dept SET budget = budget - 1000 WHERE id = 1;

-- ENUM / SET validation
INSERT INTO tags (val, set_col) VALUES ('a','X');     -- ok
INSERT INTO tags (val) VALUES ('bad');                -- ERROR: invalid ENUM
INSERT INTO tags (val, set_col) VALUES ('b','X,Q');  -- ERROR: invalid SET
UPDATE tags SET val = 'b' WHERE id = 1;              -- ok
UPDATE tags SET val = 'zzz' WHERE id = 1;            -- ERROR: invalid ENUM
SELECT * FROM tags ORDER BY id;

-- SELECT FOR UPDATE / SHOW LOCKS
SHOW LOCKS;
BEGIN;
SELECT id, name, salary FROM emp WHERE id = 1 FOR UPDATE;
SHOW LOCKS;
UPDATE emp SET salary = salary + 1 WHERE id = 1;
COMMIT;
SHOW LOCKS;

-- EXPLAIN (covering index / PkPoint)
EXPLAIN SELECT dept_id, salary FROM emp WHERE dept_id = 1;
EXPLAIN SELECT * FROM emp WHERE dept_id = 1;
EXPLAIN SELECT * FROM emp WHERE id = 1;

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
  rustdb-ui        rustdb-mcp
  (Tauri + React)  (MCP, 개발 예정)
```

<br/>

## B+ Tree에 관하여
[B+ Tree 구조 이해](https://chanho0912.tistory.com/109)

[B+ Tree 이해 - velog](https://velog.io/@emplam27/%EC%9E%90%EB%A3%8C%EA%B5%AC%EC%A1%B0-%EA%B7%B8%EB%A6%BC%EC%9C%BC%EB%A1%9C-%EC%95%8C%EC%95%84%EB%B3%B4%EB%8A%94-B-Plus-Tree)
