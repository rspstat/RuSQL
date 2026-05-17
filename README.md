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
- [x] ALTER TABLE ADD CONSTRAINT (FOREIGN KEY / UNIQUE / CHECK)
- [x] ALTER TABLE DROP CONSTRAINT / DROP FOREIGN KEY
- [x] CREATE INDEX / DROP INDEX (단일 / 복합)
- [x] CREATE VIEW / DROP VIEW
- [x] Updatable View — 단순 뷰(JOIN/DISTINCT/GROUP BY 없음)에 INSERT/UPDATE/DELETE 지원, 뷰 조건 자동 병합
- [x] DESCRIBE (테이블 스키마 조회)
- [x] SHOW CREATE TABLE (스키마 기반 DDL 역생성)

### DML
- [x] INSERT (전체 컬럼 / 컬럼 지정 / 멀티 row)
- [x] INSERT ... SELECT (SELECT 결과를 다른 테이블에 삽입)
- [x] INSERT IGNORE (UNIQUE 위반 행 조용히 무시)
- [x] INSERT ... ON DUPLICATE KEY UPDATE (중복 키 시 UPDATE로 전환, 다중 컬럼 대입 지원)
- [x] INSERT ... RETURNING col1, col2 — 삽입된 행 즉시 반환
- [x] SELECT
- [x] UPDATE (상수 / 산술 표현식 / 스칼라 함수 / 자기 참조 — `salary = salary * 1.1`, `name = CONCAT(name, '_v2')`)
- [x] UPDATE ... RETURNING — 수정된 행 반환
- [x] UPDATE 다중 테이블 — `UPDATE t1, t2 SET t1.col = ..., t2.col = ... WHERE ...`
- [x] DELETE (MVCC 논리 삭제 / 물리 삭제)
- [x] DELETE ... RETURNING — 삭제된 행 반환
- [x] DELETE 다중 테이블 — `DELETE t1, t2 FROM t1 JOIN t2 ON ... WHERE ...`
- [x] MERGE INTO — `MERGE INTO target USING source ON ... WHEN MATCHED THEN UPDATE/DELETE WHEN NOT MATCHED THEN INSERT`

### 저장 프로시저 / 트리거 / 사용자 정의 함수
- [x] CREATE PROCEDURE / DROP PROCEDURE — BEGIN...END 본문, IN/OUT/INOUT 파라미터
- [x] CALL procedure_name(args) — 저장 프로시저 실행
- [x] 저장 프로시저 제어문 — IF/ELSEIF/ELSE...END IF, WHILE...DO...END WHILE, LOOP/LEAVE...END LOOP, REPEAT...UNTIL...END REPEAT, DECLARE 변수, SET 변수 = 표현식
- [x] CREATE TRIGGER / DROP TRIGGER — BEFORE/AFTER INSERT/UPDATE/DELETE FOR EACH ROW
- [x] CREATE FUNCTION / DROP FUNCTION — `CREATE FUNCTION name(p1, p2) RETURNS type RETURN expr` 형식의 사용자 정의 스칼라 함수, SELECT/UPDATE에서 일반 함수처럼 호출 가능

### DCL
- [x] CREATE USER [IF NOT EXISTS] `'user'@'host'` [IDENTIFIED BY 'password']
- [x] DROP USER [IF EXISTS] `'user'@'host'`
- [x] GRANT privilege [, ...] ON object TO `'user'@'host'` [WITH GRANT OPTION]
- [x] REVOKE privilege [, ...] ON object FROM `'user'@'host'`
- [x] SHOW GRANTS [FOR `'user'@'host'`]
- [x] 사용자·권한 영속화 (`_users.json`, `_grants.json`)
- [x] 비밀번호 SHA-256 해시 저장 (레거시 평문 자동 마이그레이션)

### 쿼리 기능
- [x] WHERE (=, !=, >, <, >=, <=)
- [x] AND / OR / NOT 복합 조건 — `NOT (price > 100 OR active = 0)`
- [x] IN (리터럴 목록) — `WHERE id IN (1, 2, 3)`
- [x] NOT IN (리터럴 목록) — `WHERE id NOT IN (2, 4)`
- [x] IN / NOT IN (서브쿼리) — `WHERE dept_id IN (SELECT id FROM dept)`
- [x] BETWEEN / LIKE (%, _ 와일드카드)
- [x] IS NULL / IS NOT NULL
- [x] INNER JOIN / LEFT JOIN / RIGHT JOIN
- [x] FULL OUTER JOIN (양쪽 NULL 패딩, 매칭 안 된 우측 행 자동 추가)
- [x] CROSS JOIN (카르테시안 곱, ON 절 없음)
- [x] NATURAL JOIN (공통 컬럼명 자동 equi-join, ON 절 없음)
- [x] LEFT OUTER JOIN / RIGHT OUTER JOIN / INNER JOIN 키워드 별칭 지원
- [x] Sort-Merge Join (양쪽 > 4행 Equi-Join, O((N+M)logN) sort + O(N+M) merge, 투 포인터 키 그룹 병합)
- [x] Hash Join (한쪽 > 4행 Equi-Join, O(N+M)) / Nested Loop Join (소규모·비등가) — ON 조건 방향 무관 (left.col = right.col / right.col = left.col 모두 지원)
- [x] 테이블 별칭 (alias) — `FROM emp e JOIN dept d ON e.dept_id = d.id`
- [x] ORDER BY (ASC / DESC, 다중 컬럼)
- [x] LIMIT / OFFSET — `LIMIT 10 OFFSET 20`
- [x] GROUP BY / HAVING
- [x] DISTINCT
- [x] 산술 표현식 — SELECT / WHERE / UPDATE SET에서 `price * qty`, `salary + 100`
- [x] 비교 표현식 — SELECT 컬럼에서 `expr > val AS alias` 형태 지원 (`LENGTH(UUID()) > 0 AS uuid_ok`, `RAND() >= 0` 등)
- [x] 집계 함수 — COUNT / SUM / AVG / MIN / MAX / STDDEV / VARIANCE (모집단 기준)
- [x] DISTINCT 집계 — COUNT(DISTINCT) / SUM(DISTINCT) / AVG(DISTINCT)
- [x] GROUP_CONCAT (SEPARATOR 옵션, GROUP BY 및 비집계 양쪽 지원)
- [x] 윈도우 함수 — ROW_NUMBER / RANK / DENSE_RANK / LAG / LEAD / FIRST_VALUE / LAST_VALUE / NTH_VALUE / NTILE / PERCENT_RANK / CUME_DIST (OVER PARTITION BY + ORDER BY)
- [x] 윈도우 함수 ROWS/RANGE 프레임 — `ROWS/RANGE BETWEEN <bound> AND <bound>` (UNBOUNDED PRECEDING / CURRENT ROW / FOLLOWING 등)
- [x] 집계 윈도우 함수 — SUM / AVG / COUNT / MIN / MAX OVER (PARTITION BY … ORDER BY … ROWS/RANGE …)
- [x] 정규식 — REGEXP / RLIKE 연산자 (WHERE 조건), REGEXP_LIKE / REGEXP_REPLACE / REGEXP_MATCH 스칼라 함수
- [x] CASE WHEN ... THEN ... ELSE ... END
- [x] 스칼라 함수 — UPPER / LOWER / LENGTH / TRIM / CONCAT / SUBSTR / REPLACE / LPAD / RPAD / CHAR_LENGTH / LEFT / RIGHT / REVERSE / REPEAT / INSTR / LOCATE / LTRIM / RTRIM / SPACE / ASCII / CHAR / HEX / UNHEX / FORMAT
- [x] 수학 함수 — ROUND / ABS / CEIL / FLOOR / MOD / SQRT / POW(POWER) / LOG / LOG2 / LOG10 / EXP / SIN / COS / TAN / PI / SIGN / TRUNCATE / RAND
- [x] 날짜 함수 — NOW / CURDATE / DATE_FORMAT / DATEDIFF / DATE_ADD / DATE_SUB / YEAR / MONTH / DAY / HOUR / MINUTE / SECOND / DAYOFWEEK / DAYOFYEAR / WEEKDAY / LAST_DAY / TIMESTAMPDIFF / CURTIME / CURRENT_TIMESTAMP / UNIX_TIMESTAMP / FROM_UNIXTIME
- [x] NULL 처리 함수 — COALESCE / IFNULL / NULLIF / ISNULL
- [x] 타입 변환 — CAST(expr AS INT/FLOAT/TEXT/DATE) / CONVERT / BIT_LENGTH
- [x] 조건 함수 — IF(cond, true_val, false_val) / GREATEST / LEAST
- [x] 기타 함수 — MD5 / UUID
- [x] FROM 없는 스칼라 SELECT — `SELECT 1+1`, `SELECT NOW()` (`_dual_` 가상 테이블 방식)
- [x] 서브쿼리 — WHERE col = / > / < (SELECT ...)
- [x] 상관 서브쿼리 — WHERE EXISTS / IN / NOT IN (SELECT ... WHERE outer.col = inner.col) 완전 지원
- [x] FROM 절 서브쿼리 — FROM (SELECT ...) AS alias
- [x] SELECT 스칼라 서브쿼리 — `SELECT (SELECT MAX(col) FROM t2) AS alias FROM t1` (비상관·상관 모두 지원)
- [x] JOIN 순서 최적화 — INNER JOIN 그리디 재정렬 (작은 테이블 우선, ON 조건 의존성 자동 분석)
- [x] UNION / UNION ALL (ORDER BY / LIMIT / OFFSET 포함)
- [x] INTERSECT / INTERSECT ALL — 교집합
- [x] EXCEPT / EXCEPT ALL — 차집합
- [x] CTE (WITH ... AS) — 단순 / 다중 / INSERT 메인 쿼리 지원
- [x] 재귀 CTE (WITH RECURSIVE) — base case + UNION ALL 반복, positional 컬럼 매핑
- [x] SELECT ... FOR UPDATE (배타 잠금)
- [x] SELECT ... FOR SHARE (공유 잠금 — 다중 독자 허용, 쓰기 잠금과 충돌)
- [x] table.column dot notation (SELECT / JOIN ON / GROUP BY / ORDER BY)
- [x] EXPLAIN (비용 기반 실행 계획 조회)
- [x] EXPLAIN ANALYZE (실제 실행 후 Actual rows / Actual time 출력)
- [x] SHOW TABLES / DESCRIBE
- [x] INFORMATION_SCHEMA 가상 테이블 — `SELECT * FROM information_schema.tables` 등 10개 가상 뷰 (schemata / tables / columns / key_column_usage / table_constraints / statistics / views / character_sets / collations / engines)

### 데이터 타입
- [x] INT
- [x] BIGINT — 64비트 정수 (최대 9223372036854775807)
- [x] SMALLINT — 소형 정수 (최대 32767)
- [x] TINYINT — 초소형 정수 (최대 127)
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
- [x] BLOB — 이진 데이터 타입 (hex 문자열로 저장, `0x...` 리터럴 파싱 지원)
- [x] JSON — JSON 문자열 저장, `->` / `->>` 연산자, JSON_EXTRACT / JSON_UNQUOTE / JSON_VALUE 함수

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
- [x] SAVEPOINT / ROLLBACK TO SAVEPOINT (session_tables 기반 undo 적용 — Deferred Write와 완전 통합)
- [x] Undo Log 기반 롤백 (B+Tree 인덱스 재빌드 포함)
- [x] Undo Log 영속화 (`data/_undo.log`) — 트랜잭션 중 변경마다 Undo Entry를 디스크에 즉시 기록, COMMIT/ROLLBACK 시 삭제, 크래시 후 재시작 시 미완료 트랜잭션 자동 롤백
- [x] WAL 기반 Crash Recovery (재시작 시 자동 복구)
- [x] Checkpoint (WAL 자동 트런케이션, 512KB 임계값, fsync 보장)
- [x] 트랜잭션 격리 수준 4단계
  - READ UNCOMMITTED / READ COMMITTED
  - REPEATABLE READ (BEGIN 시점 스냅샷 고정)
  - SERIALIZABLE (팬텀 읽기 감지 + 자동 롤백)
- [x] Deferred Write (ACID Isolation 완전 충족) — DML은 session_tables(세션 로컬 버퍼)에만 기록, COMMIT 시 s.tables·buffer_pool에 일괄 반영, ROLLBACK 시 버퍼 폐기. 미커밋 데이터가 다른 세션에 노출(Dirty Read)되지 않음

### 인덱스 & 저장
- [x] B+Tree 인덱스 (단일 컬럼, ORDER=16으로 트리 깊이 최소화)
- [x] 복합 인덱스 (다중 컬럼, null-byte 키 결합)
- [x] 클러스터드 인덱스 (PK 기준 물리적 정렬 유지)
- [x] 보조 인덱스 중복 키 지원 (배열 저장, 동일 컬럼 값 다중 행)
- [x] 보조 인덱스 자동 재빌드 (INSERT / UPDATE / 다중 테이블 UPDATE 후 stale 방지)
- [x] 커버링 인덱스 (SELECT 컬럼 ⊆ 인덱스 컬럼 시 Index-only scan 자동 활성화)
- [x] B+Tree 범위 스캔 최적화 (scan_from_node / scan_to_node 가지치기, O(log N + k))
- [x] 수치 인식 키 비교 (`"10" > "9"` 정상 처리)
- [x] 바이너리 디스크 저장 (.rdb 포맷, 16KB 페이지)
- [x] LZ4 데이터 압축 (.rdb 파일 투명 압축/해제, 하위 호환성 유지)
- [x] Buffer Pool (LRU 캐시, `--buffer-pool-size N` 옵션으로 설정 가능, 기본 64페이지)
- [x] 스키마 영속화 (TableSchema JSON, auto_increment 카운터 포함)
- [x] 인덱스 영속화 — 재시작 시 indexes.json으로 자동 재빌드
- [x] 뷰 영속화 — 재시작 시 views.json에서 AST 복원
- [x] TRUNCATE 후 AUTO INCREMENT 리셋

### MVCC
- [x] 행 버전 스탬프 (`_xmin`, `_xmax`)
- [x] DELETE → MVCC 논리 삭제 (트랜잭션 내) / 물리 삭제 (트랜잭션 외)
- [x] SELECT 가시성 필터 (`_xmax == "0"` 인 행만 표시)
- [x] ROLLBACK → `_xmax` 복원 + PK/보조/복합 인덱스 완전 재빌드 (인덱스 stale 버그 수정)
- [x] VACUUM (dead row 물리 제거)
- [x] AUTO VACUUM — DML 200회 누적 시 자동 dead row 정리 (`dml_since_vacuum` 카운터)

### Row-level Locking
- [x] SELECT ... FOR UPDATE (배타 잠금 획득)
- [x] SELECT ... FOR SHARE (공유 잠금 — 다중 독자 허용)
- [x] 공유/배타 잠금 충돌 감지 및 데드락 감지 (wait-for 그래프 DFS)
- [x] UPDATE / DELETE 시 잠금 충돌 감지
- [x] COMMIT / ROLLBACK 시 잠금 자동 해제
- [x] SHOW LOCKS (활성 잠금 목록 조회)

### 모니터링
- [x] SHOW BUFFER POOL (캐시 히트율, 사용량)
- [x] SHOW WAL (로그 레코드, 파일 크기)
- [x] SHOW ISOLATION LEVEL
- [x] SHOW LOCKS
- [x] SHOW DATABASES
- [x] SHOW PROCESSLIST (현재 세션 정보 표시)
- [x] CHECKPOINT (수동 체크포인트)
- [x] VACUUM (dead row 물리 제거)
- [x] ANALYZE TABLE (컬럼별 통계 수집 — distinct count / null count / min / max, 옵티마이저 선택도 추정에 반영)
- [x] BACKUP [DATABASE db] [INTO 'file'] — mysqldump 스타일 SQL 덤프 생성 (DROP TABLE IF EXISTS + CREATE TABLE + INSERT)

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
- [x] 테이블 우클릭 컨텍스트 메뉴 (MySQL 스타일) — Select Rows / Describe Table / Show Create Table / Copy Table Name / Copy as INSERT / Truncate / DROP
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
- [x] CSV 익스포트 (Tauri `export_csv` 커맨드) — SELECT 결과를 CSV 파일로 저장
- [x] CSV 임포트 (Tauri `import_csv` 커맨드) — CSV 파일을 지정 테이블에 INSERT 일괄 실행
- [x] 결과 컬럼 헤더 클릭 정렬 — ▲ ASC / ▼ DESC / ⇅ 기본 토글, 수치·문자열 자동 감지, 정렬 후 페이징 재계산
- [x] 결과 행 번호 — 결과 테이블 첫 열 `#` (1-based, 페이지 오프셋 반영)
- [x] 결과 내 실시간 검색 — 결과 패널 상단 검색 입력, 해당 키워드 포함 행 실시간 필터 (페이징 연동)
- [x] 키보드 단축키 — Ctrl+T (새 탭), Ctrl+W (탭 닫기), Ctrl+Enter (쿼리 실행), Ctrl+Shift+F (SQL 포매터)
- [x] SQL 포매터 — `sql-formatter` 패키지, Ctrl+Shift+F로 에디터 SQL 자동 들여쓰기 (UPPER case 키워드, 2-space indent)
- [x] 쿼리 북마크 — ★ 버튼으로 현재 쿼리 저장, 사이드바 BOOKMARKS 패널, 클릭 시 에디터 로드, × 삭제, localStorage 영구 보존
- [x] 사이드바 테이블 검색 — SCHEMAS 상단 검색 입력으로 테이블 이름 실시간 필터
- [x] EXPLAIN 트리 시각화 — EXPLAIN / EXPLAIN ANALYZE 결과를 구조화된 카드(Access / Table / Condition 항목)로 렌더링

<br/>

## 진행 예정

### 엔진
- [ ] **GAP Lock / Next-key Lock** — SERIALIZABLE 격리 수준에서 팬텀 읽기 방지 완성. 현재는 범위 조건 팬텀을 감지하면 트랜잭션을 롤백하는 방식이나, 실제 GAP Lock으로 교체하면 불필요한 롤백 없이 직렬성 보장 가능
- [ ] **파티셔닝** — RANGE 파티션 (날짜별 분할 등)
- [ ] **복제** — WAL 스트리밍으로 읽기 복제본 지원

### MCP / AI
- [ ] AI API 클라이언트 연결 (Claude API 연동)
- [ ] 자연어 → SQL 변환 (`\ai` 명령어)
- [ ] 변환된 SQL 확인 후 실행

### UI
- [ ] 다크 / 라이트 테마 전환
- [ ] 테이블 시각적 편집기 (ALTER TABLE GUI — 컬럼 추가/삭제/타입 변경)
- [ ] 결과 셀 직접 편집 (클릭 → 인라인 편집 → UPDATE 자동 생성)
- [ ] AI 뷰 자연어 → SQL 변환 (Claude API 연동)

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
├── rustdb-client/   TCP 클라이언트 CLI (-u/-p/-h/-P 옵션)
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

## B+ Tree에 관하여
[B+ Tree 구조 이해](https://chanho0912.tistory.com/109)

[B+ Tree 이해 - velog](https://velog.io/@emplam27/%EC%9E%90%EB%A3%8C%EA%B5%AC%EC%A1%B0-%EA%B7%B8%EB%A6%BC%EC%9C%BC%EB%A1%9C-%EC%95%8C%EC%95%84%EB%B3%B4%EB%8A%94-B-Plus-Tree)
