# RustDB v2.2.0 — 완료된 기능

### 엔진 코어
- [x] Lexer / Tokenizer
- [x] SQL Parser (AST 기반, 재귀 하강)
- [x] Executor (쿼리 실행 엔진)
- [x] 비용 기반 쿼리 옵티마이저 (Cost-Based Query Planner)
  - AccessPath 선택 (SeqScan / PkPoint / PkBetween / PkRange / SecondaryPoint / SecondaryRange / CompositeIndex)
  - 행 수 / 비용 추정 (log₂N 기반)
  - Join 알고리즘 자동 선택 (Sort-Merge Join / Hash Join / Nested Loop)
  - Join 순서 최적화 (System-R 스타일 비용 기반 동적계획법, 그리디 폴백)
  - EXPLAIN 실행 계획 출력 (비용 · 접근 경로 · Join 알고리즘)
- [x] 병렬 쿼리 실행 (rayon) — SeqScan WHERE 필터 멀티스레드 처리, 청크 단위 워커 thread_local 전파로 사용자 정의 함수/DATABASE() 정확성 유지, 10k행 이상 자동 적용 (`RUSTDB_PARALLEL` 토글), 서브쿼리 포함 시 순차 폴백

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
- [x] CREATE SYNONYM name FOR target — 테이블 별명 생성 (SELECT/INSERT/UPDATE/DELETE에서 실제 테이블처럼 사용)
- [x] CREATE OR REPLACE SYNONYM name FOR target — 기존 별명 덮어쓰기
- [x] DROP SYNONYM [IF EXISTS] name — 별명 삭제
- [x] SHOW SYNONYMS — 정의된 별명 목록 조회
- [x] 별명 영속화 (`_synonyms.json`)
- [x] Updatable View — 단순 뷰(JOIN/DISTINCT/GROUP BY 없음)에 INSERT/UPDATE/DELETE 지원, 뷰 조건 자동 병합
- [x] DESCRIBE (테이블 스키마 조회)
- [x] SHOW CREATE TABLE (스키마 기반 DDL 역생성)
- [x] BACKUP [DATABASE db] [INTO 'file'] — FK ON DELETE / ON UPDATE 액션 포함 완전한 DDL 덤프

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
- [x] MERGE 조건부 DELETE — `WHEN MATCHED AND condition THEN DELETE` (DELETE/UPDATE 분기 처리)

### 저장 프로시저 / 트리거 / 사용자 정의 함수
- [x] CREATE PROCEDURE / DROP PROCEDURE — BEGIN...END 본문, IN/OUT/INOUT 파라미터
- [x] CALL procedure_name(args) — 저장 프로시저 실행
- [x] 저장 프로시저 제어문 — IF/ELSEIF/ELSE...END IF, WHILE...DO...END WHILE, LOOP/LEAVE...END LOOP, REPEAT...UNTIL...END REPEAT, DECLARE 변수, SET 변수 = 표현식
- [x] 저장 프로시저 영속화 (`data/_procedures.json`) — 재시작 후에도 유지
- [x] CREATE TRIGGER / DROP TRIGGER — BEFORE/AFTER INSERT/UPDATE/DELETE FOR EACH ROW
- [x] 트리거 영속화 (`data/_triggers.json`) — 재시작 후에도 유지
- [x] CREATE FUNCTION / DROP FUNCTION — `CREATE FUNCTION name(p1, p2) RETURNS type RETURN expr` 형식의 사용자 정의 스칼라 함수, SELECT/UPDATE에서 일반 함수처럼 호출 가능
- [x] 사용자 정의 함수 영속화 (`data/_functions.json`) — 재시작 후에도 유지

### DCL
- [x] CREATE USER [IF NOT EXISTS] `'user'@'host'` [IDENTIFIED BY 'password']
- [x] DROP USER [IF EXISTS] `'user'@'host'`
- [x] GRANT privilege [, ...] ON object TO `'user'@'host'` [WITH GRANT OPTION]
- [x] REVOKE privilege [, ...] ON object FROM `'user'@'host'`
- [x] SHOW GRANTS [FOR `'user'@'host'`]
- [x] 사용자·권한 영속화 (`_users.json`, `_grants.json`)
- [x] 비밀번호 SHA-256 해시 저장 (레거시 평문 자동 마이그레이션)
- [x] CREATE ROLE name — 역할 생성
- [x] DROP ROLE [IF EXISTS] name — 역할 삭제 (연결된 부여 기록 자동 삭제)
- [x] GRANT ROLE roleName TO `'user'@'host'` [WITH ADMIN OPTION] — 역할 부여
- [x] REVOKE ROLE roleName FROM `'user'@'host'` — 역할 회수
- [x] SHOW ROLES — 정의된 역할 목록 조회
- [x] 역할 영속화 (`_roles.json`, `_role_grants.json`)

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
- [x] JOIN ... USING (col, ...) — 지정 컬럼 기준 equi-join (`ON t1.col = t2.col` 단축 문법)
- [x] LEFT OUTER JOIN / RIGHT OUTER JOIN / INNER JOIN 키워드 별칭 지원
- [x] Sort-Merge Join (양쪽 > 4행 Equi-Join, O((N+M)logN) sort + O(N+M) merge, 투 포인터 키 그룹 병합)
- [x] Hash Join (한쪽 > 4행 Equi-Join, O(N+M)) / Nested Loop Join (소규모·비등가) — ON 조건 방향 무관 (left.col = right.col / right.col = left.col 모두 지원)
- [x] 테이블 별칭 (alias) — `FROM emp e JOIN dept d ON e.dept_id = d.id`
- [x] ORDER BY (ASC / DESC, 다중 컬럼)
- [x] LIMIT / OFFSET — `LIMIT 10 OFFSET 20`
- [x] FETCH FIRST n ROWS ONLY / FETCH NEXT n ROWS ONLY — SQL 표준 페이징 문법, LIMIT의 동의어
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
- [x] 기타 함수 — MD5 / UUID / DATABASE / VERSION / CURRENT_USER (SCHEMA / USER / SESSION_USER / SYSTEM_USER 별칭 포함)
- [x] FROM 없는 스칼라 SELECT — `SELECT 1+1`, `SELECT NOW()` (`_dual_` 가상 테이블 방식)
- [x] 서브쿼리 — WHERE col = / > / < (SELECT ...)
- [x] 상관 서브쿼리 — WHERE EXISTS / IN / NOT IN (SELECT ... WHERE outer.col = inner.col) 완전 지원
- [x] FROM 절 서브쿼리 — FROM (SELECT ...) AS alias
- [x] SELECT 스칼라 서브쿼리 — `SELECT (SELECT MAX(col) FROM t2) AS alias FROM t1` (비상관·상관 모두 지원)
- [x] JOIN 순서 최적화 — 비용 기반 동적계획법 (System-R bitmask DP, 누적 카디널리티 반영; INNER/NATURAL 연속 구간 대상, 테이블 N>8 또는 OUTER 포함 시 그리디 재정렬로 폴백)
- [x] UNION / UNION ALL (ORDER BY / LIMIT / OFFSET 포함)
- [x] INTERSECT / INTERSECT ALL — 교집합
- [x] EXCEPT / EXCEPT ALL — 차집합
- [x] CTE (WITH ... AS) — 단순 / 다중 / INSERT 메인 쿼리 지원
- [x] 재귀 CTE (WITH RECURSIVE) — base case + UNION ALL 반복, positional 컬럼 매핑
- [x] SELECT ... FOR UPDATE (배타 잠금)
- [x] SELECT ... FOR SHARE (공유 잠금 — 다중 독자 허용, 쓰기 잠금과 충돌)
- [x] table.column dot notation (SELECT / JOIN ON / GROUP BY / ORDER BY)
- [x] EXPLAIN (비용 기반 실행 계획 조회 — 너비 74자, 단어 경계 줄바꿈 포맷)
- [x] EXPLAIN ANALYZE (실제 실행 후 Actual rows / Actual time 출력)
- [x] PREPARE / EXECUTE / DEALLOCATE — `PREPARE name FROM 'sql'`, `SET @v = expr`, `EXECUTE name USING @v`, `DEALLOCATE PREPARE name`
- [x] 사용자 변수 (@var) — `SET @v = expr`, `SELECT @v`, EXECUTE USING 바인딩
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
- [x] RELEASE SAVEPOINT
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
- [x] 홈(연결) 화면 — 3티어 실린더 아이콘 헤더, 퀵 액션 버튼 3종 (새 연결·터미널 열기·GitHub 방문), RDBMS 영문 소개 텍스트 (4줄), 저장된 연결 카드 그리드, 하단 상태 표시줄 (브랜치·버전·기술 스택), 좌측 액티비티 바 (유저 아이콘·설정 버튼), Tauri `open_terminal` (기본 경로: dbe/code) · `open_url` 커맨드
- [x] Monaco Editor (SQL 문법 강조, 주석 회색)
- [x] 다중 쿼리 탭 (탭 추가 / 전환 / 닫기, localStorage 자동 저장)
- [x] 탭 이름 변경 — 탭 더블클릭 → 인라인 편집 → Enter/blur 커밋
- [x] 탭 우클릭 컨텍스트 메뉴 (VSCode 스타일) — 닫기 / 다른 탭 닫기 / 오른쪽 탭 닫기 / 모두 닫기 / 이름 변경 / 고정·고정 해제 / 오른쪽으로 분할 / 왼쪽으로 분할 / 분할 및 이동; `source: "main" | "split"` 구분으로 분할 탭바에서도 동일 메뉴 제공
- [x] 탭 고정 — `pinnedTabs: Set<string>`, 📌 아이콘 표시, 고정된 탭은 × 닫기 버튼 비활성화
- [x] 분할 에디터 — 오른쪽으로 분할 / 왼쪽으로 분할 / 분할 및 이동 3종 동작, 드래그 가능한 구분선 (`splitLeftPct`), 독립 Monaco 인스턴스; 분할 시 탭이 왼쪽 탭바에서 사라지고 닫을 때 원래 위치에 복원 (`splitTabStash`)
- [x] 에디터 툴바 (MySQL 스타일) — breadcrumb 아래 고정 행: SQL 파일 열기(폴더 아이콘) / SQL 파일 저장(플로피 아이콘, DOM append 방식으로 WebView2 다운로드 보장) / 번개 실행 버튼 (Ctrl+Enter 연동, `runQueryRef`로 stale closure 방지 — 탭 전환 후에도 항상 현재 활성 탭 기준 실행)
- [x] 패널 토글 버튼 — 탭바 우측: 사이드바 토글 / 결과창 토글 (이전 높이 기억 후 복원) / 우측 패널(표시 전용); 활성 패널은 teal, 비활성은 회색
- [x] 사이드바 DB 아이콘 — 원통형 SVG (`currentColor` 스트로크, 활성 DB 시 teal / 비활성 시 회색)
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
- [x] TCP 서버 관리 뷰 — DBeaver 스타일 연결 구성 폼 (호스트·포트 ±·사용자·비밀번호 토글), 메인/CLI 가이드 탭, 서버 랙 SVG 아이콘, 활동 로그, 연결 사이드바 너비 조절 (드래그, 140~400px)
- [x] AI Agent 채팅 패널 — 사이드바 4번째 아이콘으로 토글, 에디터 오른쪽 사이드 (드래그 너비 조절 240~640px), 채팅 버블 UI (마크다운 렌더링 marked+DOMPurify, 타이핑 인디케이터), 채팅 세션 기록 (시계 버튼 → 세션 목록 패널, 새 채팅 생성, 세션 이름 변경·삭제, localStorage 영구 보존), Enter 전송/Ctrl+Enter 줄바꿈, 자연어 입력 → AI assistant (MCP 서버) → SQL 제안 → 에디터 삽입, 에디터 열린 파일 자동 컨텍스트 주입, @파일명 멘션(자동완성 드롭다운·칩 시각화), AI 파일 편집 블록(파일 수정·삽입·삭제, "파일에 적용" 버튼, Monaco executeEdits Undo 지원)
- [x] 멀티 쿼리 결과 표시
- [x] 결과 페이지네이션 — PAGE_SIZE=100, 초과 시 ‹/› 버튼 + 페이지 표시
- [x] 쿼리 히스토리 — 결과 패널 HISTORY 탭, localStorage 최대 200개, 클릭 시 에디터 불러오기
- [x] 쿼리 자동 저장 (탭별)
- [x] 결과창 크기 조절 (드래그)
- [x] 전체 스크롤바 스타일 통일 (Monaco 에디터 스크롤바 기준)
- [x] 탭별 결과 보존 (탭 전환 시 결과 패널 유지, tabResults Record로 탭별 독립 관리)
- [x] 결과 테이블 컬럼 너비 조절 — th 드래그 resize handle, table-layout: fixed 항상 적용
- [x] 결과 테이블 컬럼 자동 너비 — Canvas `measureText`로 헤더·데이터 실제 픽셀 너비 측정 (한글/CJK 포함), 최대 200행 샘플링, 최소 60px / 최대 500px; 헤더 측정 시 정렬 아이콘(` ⇅`) 포함, `white-space: nowrap` + `vertical-align: middle`로 줄바꿈·수직 밀림 방지
- [x] CSV 익스포트 (Tauri `export_csv` 커맨드) — SELECT 결과를 CSV 파일로 저장
- [x] CSV 임포트 (Tauri `import_csv` 커맨드) — CSV 파일을 지정 테이블에 INSERT 일괄 실행
- [x] 결과 컬럼 헤더 클릭 정렬 — ▲ ASC / ▼ DESC / ⇅ 기본 토글, 수치·문자열 자동 감지, 정렬 후 페이징 재계산
- [x] 결과 행 번호 — 결과 테이블 첫 열 `#` (1-based, 페이지 오프셋 반영, 고정 40px 너비, 왼쪽 정렬)
- [x] 쿼리 실행 진행 바 — `isRunning` 시 탭바 하단에 teal 슬라이딩 바 표시, 최소 400ms 보장으로 빠른 쿼리도 시각적 피드백 제공
- [x] 결과 내 실시간 검색 — 결과 패널 상단 검색 입력, 해당 키워드 포함 행 실시간 필터 (페이징 연동)
- [x] 키보드 단축키 — Ctrl+T (새 탭), Ctrl+W (탭 닫기), Ctrl+Enter (쿼리 실행), Ctrl+Shift+F (SQL 포매터)
- [x] SQL 포매터 — `sql-formatter` 패키지, Ctrl+Shift+F로 에디터 SQL 자동 들여쓰기 (UPPER case 키워드, 2-space indent)
- [x] 쿼리 북마크 — ★ 버튼으로 현재 쿼리 저장, 사이드바 BOOKMARKS 패널, 클릭 시 에디터 로드, × 삭제, localStorage 영구 보존
- [x] 사이드바 테이블 검색 — SCHEMAS 상단 검색 입력으로 테이블 이름 실시간 필터
- [x] EXPLAIN 트리 시각화 — EXPLAIN / EXPLAIN ANALYZE 결과를 구조화된 카드(Access / Table / Condition 항목)로 렌더링
- [x] BEGIN...END 블록 인식 쿼리 분리 — SQL 에디터에서 CREATE PROCEDURE / CREATE TRIGGER 등 BEGIN...END 포함 멀티 쿼리를 올바르게 분리·실행 (IF/ELSEIF/WHILE/LOOP/REPEAT/ITERATE 제어문 UI 검증 완료)
- [x] 결과 셀 직접 편집 — 단일 테이블 SELECT 결과에서 셀 더블클릭 → 인풋 전환, Enter/blur 커밋 시 `UPDATE tableName SET col = val WHERE pk = pkVal` 자동 생성·실행 후 SELECT 재실행 (Escape = 취소, JOIN/PK 없는 경우 자동 비활성)

### TCP 서버 / MySQL 프로토콜 (rustdb-server)
- [x] 커스텀 텍스트 프로토콜 TCP 서버 — 포트 7878, `---END---` 구분자, 멀티 클라이언트 동시 접속, 세션별 독립 Executor
- [x] AUTH 핸드셰이크 — `AUTH user password` 줄 기반 인증, SHA-256 해시 비교, 레거시 평문 자동 마이그레이션
- [x] 기본 사용자 자동 생성 — users가 비어있으면 root/root 자동 생성 (`ensure_default_user`)
- [x] SHOW PROCESSLIST — `ProcessInfo` 구조체 + `process_list: Arc<Mutex<...>>` 실제 활성 세션 추적 (세션 등록/갱신/해제)
- [x] MySQL wire protocol (포트 3306) — COM_QUERY / COM_PING / COM_INIT_DB / COM_STMT_PREPARE / COM_STMT_EXECUTE / COM_STMT_CLOSE / COM_STMT_RESET
- [x] **mysql_native_password 인증 구현** — 연결별 20바이트 nonce 생성, SHA1(password) XOR SHA1(nonce||SHA1(SHA1(pw))) 챌린지-응답 검증, UserRecord에 `mysql_native_hash`(SHA1(SHA1(pw))) 저장, 레거시 사용자(hash 없음) 자동 거부
- [x] **레거시 사용자 자동 마이그레이션** — `migrate_mysql_hash()`: native/Tauri 로그인 성공 시 `mysql_native_hash` 없는 계정에 자동 채움 (평문 비밀번호 보유 시점 활용)
- [x] MySQL 세션 process_list 등록 — MySQL 프로토콜 연결도 SHOW PROCESSLIST에 표시
- [x] **parse_table 탭 구분 형식 지원** — SHOW DATABASES/TABLES, SELECT, DESCRIBE 등 탭 구분 출력을 MySQL result set으로 정상 변환 (기존 박스 형식도 유지)
- [x] Prepared Statement — `?` 플레이스홀더 바인딩, 타입별 파라미터 디코딩 (TINY/SHORT/LONG/LONGLONG/FLOAT/DOUBLE/DATE/DATETIME/VAR_STRING)
- [x] `SHOW FULL TABLES FROM db` — 엔진의 `SHOW TABLES` 실행 후 `Table_type` 컬럼(BASE TABLE/VIEW) 추가 → DBeaver 테이블 트리 정상 표시
- [x] `SHOW FULL COLUMNS FROM table FROM db` — `DESCRIBE table` 실행 후 MySQL 전체 컬럼 구조(Collation/Privileges/Comment 포함)로 확장 → DBeaver 컬럼 패널 정상 표시
- [x] `SHOW INDEX FROM table` / `SHOW INDEXES FROM` / `SHOW KEYS FROM` — 올바른 컬럼 구조(13개 컬럼)의 빈 결과 반환
- [x] `SHOW TABLE STATUS FROM db` — MySQL TABLE STATUS 컬럼 구조(18개)의 빈 결과 반환
- [x] `SHOW TRIGGERS FROM db` — 올바른 컬럼 구조의 빈 결과 반환
- [x] `SHOW FUNCTION STATUS` / `SHOW PROCEDURE STATUS` — 올바른 컬럼 구조의 빈 결과 반환
- [x] `SHOW EVENTS` — 올바른 컬럼 구조의 빈 결과 반환
- [x] `SHOW COLLATION` — utf8 / utf8mb4 / latin1 실제 콜레이션 데이터 반환 (DBeaver 문자셋 초기화 정상 처리)
- [x] `SHOW CHARACTER SET` / `SHOW CHARSET` — utf8 / utf8mb4 / latin1 실제 데이터 반환
- [x] `SHOW ENGINES` — RustDB 엔진 정보 반환 (DBeaver 엔진 목록 초기화 정상 처리)
- [x] `SHOW VARIABLES [LIKE 'pattern']` — 21개 주요 MySQL 시스템 변수 의미 있는 값 반환 (autocommit / character_set / collation / max_allowed_packet / tx_isolation 등), LIKE 패턴 필터링 지원
- [x] `SELECT @@var1 AS a, @@var2 AS b, ...` — 다중 시스템 변수 SELECT를 컬럼별로 올바르게 파싱·반환 (DBeaver 접속 초기화 쿼리 정상 처리)
- [x] `SELECT DATABASE()` / `SELECT SCHEMA()` / `SELECT USER()` — 각각 현재 DB / 현재 DB / 'root@localhost' 반환
- [x] `SET ...` (charset / autocommit / session 변수 등) — 무조건 OK 반환

### 전용 클라이언트 (rustdb-client)
- [x] rustdb-server native 프로토콜 전용 TCP 클라이언트
- [x] CLI 옵션: `-u user` / `-p password` / `-h host` / `-P port` (기본: root/root@127.0.0.1:7878)
- [x] 멀티라인 SQL 입력 — 세미콜론(`;`)으로 실행 트리거, 주석/문자열 내 `;` 제외 계산
- [x] ANSI 컬러 출력 (빨강: 에러, 초록: 성공, 청록: 프롬프트)
- [x] `\status` — 서버 uptime · 연결 수 조회
- [x] `\help` — 사용 가능한 명령 안내
- [x] `exit` / `quit` — 서버에 정상 종료 알림 후 연결 해제

### 스토리지 구조 (data/)
- [x] **`_system/` 서브폴더** — 전역 파일(`_users.json`, `_grants.json`, `_roles.json`, `_role_grants.json`, `_synonyms.json`, `_procedures.json`, `_triggers.json`, `_functions.json`)을 루트 대신 `data/_system/`에 저장
- [x] **레거시 자동 마이그레이션** — `load_sys_json()`: 구 루트 경로 파일이 있으면 `_system/`으로 자동 이동 후 삭제
- [x] **SHOW DATABASES에서 `_system` 제외** — `list_databases()`에서 필터링
- [x] **연결별 독립 데이터 디렉터리** — `code/data/local/` (기본 연결), `code/data/data_숫자/` (추가 연결) — UI·CLI·서버가 `code/data/`를 공유
- [x] **Tauri `get_app_data_dir` 커맨드** — `CARGO_MANIFEST_DIR` 기반 `code/data/` 절대경로 반환
- [x] **상대경로 자동 절대경로 변환** — 앱 시작 시 localStorage의 상대경로 연결을 절대경로로 마이그레이션 후 재저장

### UI 추가 기능 (rustdb-ui)
- [x] **Server Manager MySQL 포트 필드** — `+/-` 버튼 포함, 0 입력 시 MySQL 프로토콜 비활성, Tauri `start_server`에 `mysql_port` 파라미터 추가
- [x] **Tauri UI MySQL 리스너** — UI에서 서버 Start 시 `mysql::start_mysql_listener(mysql_port, shared_db)` 호출로 MySQL 프로토콜 동시 기동
- [x] **서버 연결 아이콘 교체** — Server Manager 헤더의 서버 랙 아이콘 → 주황색 원통형 DB 아이콘
