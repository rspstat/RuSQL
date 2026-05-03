# RustDB vs MySQL 비교표

## 1. 기본 정보

| 항목 | RustDB | MySQL |
|------|--------|-------|
| 구현 언어 | Rust | C++ |
| 버전 | v2.2.0 | 8.x |
| 라이선스 | 비공개(개인 프로젝트) | GPL v2 / 상용 |
| 스토리지 엔진 구조 | 단일 엔진 (내장) | 플러그인 방식 (InnoDB, MyISAM, Memory 등) |
| 기본 포트 | 7878 | 3306 |
| 네트워크 프로토콜 | 자체 TCP 라인 프로토콜 | MySQL Wire Protocol |
| 클라이언트 도구 | rustdb-cli REPL, Tauri GUI, TCP | mysql CLI, MySQL Workbench, JDBC/ODBC |
| 다중 데이터베이스 | ✅ (DB 단위 격리, USE 전환, 디스크 기반 로드) | ✅ (schema 단위 분리) |

---

## 2. 데이터 타입

| 타입 | RustDB | MySQL |
|------|--------|-------|
| 정수 | `INT` | `TINYINT`, `SMALLINT`, `MEDIUMINT`, `INT`, `BIGINT`, `UNSIGNED` 변형 |
| 문자열 | `VARCHAR(n)`, `TEXT` | `CHAR(n)`, `VARCHAR(n)`, `TINYTEXT`, `TEXT`, `MEDIUMTEXT`, `LONGTEXT` |
| 실수 | `FLOAT`, `DOUBLE`, `DECIMAL(p,s)` | `FLOAT`, `DOUBLE`, `DECIMAL(p,s)` |
| 날짜/시간 | `DATE`, `DATETIME`, `TIMESTAMP`, `TIME`, `YEAR` | `DATE`, `DATETIME`, `TIMESTAMP`, `TIME`, `YEAR` |
| 논리값 | `BOOLEAN` | `TINYINT(1)` (별칭으로 BOOL/BOOLEAN) |
| 이진 | 미지원 | `BINARY`, `VARBINARY`, `BLOB` 계열 |
| 열거형 | `ENUM`, `SET` | `ENUM`, `SET` |
| JSON | 미지원 | `JSON` (네이티브 타입, 함수 풍부) |
| 공간 | 미지원 | `GEOMETRY`, `POINT`, `POLYGON` 등 |

---

## 3. DDL (데이터 정의어)

| 기능 | RustDB | MySQL |
|------|--------|-------|
| CREATE TABLE | ✅ (IF NOT EXISTS 포함) | ✅ |
| DROP TABLE | ✅ (IF EXISTS 포함) | ✅ |
| TRUNCATE TABLE | ✅ (AUTO INCREMENT 리셋 포함) | ✅ |
| ALTER TABLE ADD COLUMN | ✅ | ✅ |
| ALTER TABLE DROP COLUMN | ✅ | ✅ |
| ALTER TABLE RENAME COLUMN | ✅ | ✅ (8.0+) |
| ALTER TABLE MODIFY COLUMN | ✅ | ✅ |
| ALTER TABLE RENAME TABLE | ✅ (`RENAME TO`) | ✅ (`RENAME TABLE`) |
| CREATE INDEX | ✅ (단일/복합) | ✅ |
| DROP INDEX | ✅ (IF EXISTS 포함) | ✅ |
| CREATE VIEW | ✅ (AST 직렬화 영속) | ✅ |
| DROP VIEW | ✅ (IF EXISTS 포함) | ✅ |
| DESCRIBE | ✅ | ✅ |
| CREATE DATABASE | ✅ (IF NOT EXISTS 포함) | ✅ |
| DROP DATABASE | ✅ (IF EXISTS 포함) | ✅ |
| USE [DATABASE] | ✅ | ✅ |
| CREATE USER / DROP USER (IF NOT EXISTS / IF EXISTS) | ✅ | ✅ |
| GRANT / REVOKE (ALL PRIVILEGES, WITH GRANT OPTION) | ✅ | ✅ |
| SHOW GRANTS [FOR user] / SHOW DATABASES | ✅ | ✅ |
| 저장 프로시저 / 함수 | ❌ | ✅ |
| 트리거 (TRIGGER) | ❌ | ✅ |
| 이벤트 스케줄러 | ❌ | ✅ |
| 파티셔닝 | ❌ | ✅ |
| FULLTEXT INDEX | ❌ | ✅ |
| SPATIAL INDEX | ❌ | ✅ |
| 인덱스 힌트 (USE/FORCE/IGNORE INDEX) | ❌ | ✅ |

---

## 4. DML (데이터 조작어)

| 기능 | RustDB | MySQL |
|------|--------|-------|
| INSERT (단일/멀티 행) | ✅ | ✅ |
| INSERT (컬럼 지정) | ✅ | ✅ |
| INSERT ... SELECT | ✅ | ✅ |
| INSERT IGNORE | ✅ (중복 행 조용히 무시) | ✅ |
| INSERT ... ON DUPLICATE KEY UPDATE | ✅ (다중 컬럼 대입 지원) | ✅ |
| REPLACE INTO | ❌ | ✅ |
| SELECT | ✅ | ✅ |
| UPDATE | ✅ (산술식, 자기 참조) | ✅ |
| 다중 테이블 UPDATE | ✅ (`UPDATE t1, t2 SET ...`) | ✅ |
| DELETE | ✅ | ✅ |
| 다중 테이블 DELETE | ✅ (`DELETE t1, t2 FROM t1 JOIN t2 ...`) | ✅ |
| LOAD DATA INFILE | ❌ | ✅ |

---

## 5. 쿼리 기능

| 기능 | RustDB | MySQL |
|------|--------|-------|
| WHERE (=, !=, >, <, >=, <=) | ✅ | ✅ |
| AND / OR / NOT | ✅ | ✅ |
| IN / NOT IN (리터럴) | ✅ | ✅ |
| IN / NOT IN (서브쿼리) | ✅ | ✅ |
| BETWEEN | ✅ | ✅ |
| LIKE (%, _) | ✅ | ✅ |
| REGEXP | ❌ | ✅ |
| IS NULL / IS NOT NULL | ✅ | ✅ |
| INNER JOIN | ✅ | ✅ |
| LEFT JOIN | ✅ | ✅ |
| RIGHT JOIN | ✅ | ✅ |
| FULL OUTER JOIN | ❌ | ❌ (UNION으로 우회) |
| CROSS JOIN | ❌ | ✅ |
| NATURAL JOIN | ❌ | ✅ |
| ORDER BY (다중 컬럼, ASC/DESC) | ✅ | ✅ |
| LIMIT / OFFSET | ✅ (인덱스 경로 포함 정확히 적용) | ✅ |
| GROUP BY / HAVING | ✅ | ✅ |
| DISTINCT | ✅ | ✅ |
| 산술 표현식 (SELECT/WHERE/UPDATE) | ✅ | ✅ |
| 집계 함수 (COUNT/SUM/AVG/MIN/MAX) | ✅ | ✅ |
| GROUP_CONCAT | ✅ (SEPARATOR 옵션 포함) | ✅ |
| CASE WHEN | ✅ | ✅ |
| 윈도우 함수 (ROW_NUMBER, RANK 등) | ❌ | ✅ (8.0+) |
| 스칼라 서브쿼리 | ✅ | ✅ |
| IN 서브쿼리 | ✅ | ✅ |
| EXISTS 서브쿼리 (상관 포함) | ✅ | ✅ |
| FROM 절 서브쿼리 | ✅ | ✅ |
| UNION / UNION ALL | ✅ | ✅ |
| CTE (WITH ... AS) | ✅ (단순/다중/INSERT) | ✅ (8.0+) |
| 재귀 CTE | ✅ (WITH RECURSIVE, base+반복, 최대 1000회) | ✅ (8.0+) |
| FROM 없는 스칼라 SELECT | ✅ (`SELECT 1+1`, `SELECT CAST(...)` 등) | ✅ |
| SELECT ... FOR UPDATE | ✅ | ✅ |
| EXPLAIN | ✅ (비용 기반 실행 계획, Join 알고리즘 포함) | ✅ (EXPLAIN/EXPLAIN ANALYZE) |
| table.column dot notation | ✅ | ✅ |
| 테이블 별칭 (alias) | ✅ (파서에서 실테이블명으로 자동 확장) | ✅ |
| 준비된 문 (Prepared Statements) | ❌ | ✅ |

---

## 6. 스칼라 / 내장 함수

| 함수 범주 | RustDB | MySQL |
|-----------|--------|-------|
| 문자열 | UPPER, LOWER, LENGTH, TRIM, CONCAT, SUBSTR, REPLACE, **LPAD, RPAD** | 위 포함 + INSTR, LOCATE, LEFT, RIGHT, REPEAT, REVERSE, FORMAT 등 |
| 수학 | ROUND, ABS, CEIL, FLOOR, MOD | 위 포함 + POWER, SQRT, LOG, EXP, RAND 등 |
| 날짜 | NOW, CURDATE, DATE_FORMAT, **DATEDIFF, DATE_ADD** (DAY/MONTH/YEAR/HOUR/MINUTE/SECOND) | 위 포함 + DATE_SUB, YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, TIMESTAMPDIFF 등 |
| NULL 처리 | COALESCE, IFNULL, **NULLIF** | 위 포함 + IF, ISNULL |
| 타입 변환 | **CAST** (INT, FLOAT, TEXT, DATE) | CAST, CONVERT |
| 기타 | IF, CASE WHEN | UUID, MD5, SHA1/SHA2, COMPRESS 등 |

---

## 7. 제약 조건

| 제약 | RustDB | MySQL |
|------|--------|-------|
| PRIMARY KEY (단일) | ✅ | ✅ |
| PRIMARY KEY (복합) | ✅ | ✅ |
| NOT NULL | ✅ | ✅ |
| UNIQUE | ✅ | ✅ |
| AUTO INCREMENT | ✅ (TRUNCATE 시 리셋, JSON 영속) | ✅ |
| DEFAULT | ✅ | ✅ |
| CHECK | ✅ (컬럼/테이블 레벨) | ✅ (8.0.16+) |
| FOREIGN KEY RESTRICT | ✅ | ✅ |
| FOREIGN KEY CASCADE | ✅ | ✅ |
| FOREIGN KEY SET NULL | ✅ | ✅ |
| FOREIGN KEY SET DEFAULT | ✅ | ✅ |
| ON UPDATE CASCADE | ✅ | ✅ |
| ON UPDATE SET NULL / SET DEFAULT | ✅ | ✅ |
| NO ACTION (RESTRICT 동등) | ✅ | ✅ |

---

## 8. 트랜잭션 / ACID

| 항목 | RustDB | MySQL (InnoDB) |
|------|--------|----------------|
| BEGIN / COMMIT / ROLLBACK | ✅ | ✅ |
| SAVEPOINT | ✅ | ✅ |
| ROLLBACK TO SAVEPOINT | ✅ | ✅ |
| RELEASE SAVEPOINT | ✅ | ✅ |
| WAL (Redo Log) | ✅ 바이너리, 단일 파일 `rustdb.wal`, 512KB 자동 Checkpoint | ✅ 바이너리 redo log (ib_logfile), 복수 파일 |
| WAL group commit | ❌ (레코드별 단건 기록) | ✅ (fsync 배치 처리) |
| Undo Log | ✅ 인메모리 Undo Log (미완료 트랜잭션 롤백) | ✅ 디스크 기반 Undo Tablespace |
| Undo Log 영속화 | ❌ (crash 시 미완료 트랜잭션 복구 불가) | ✅ |
| Crash Recovery | ✅ (재시작 시 WAL redo replay) | ✅ (자동 복구) |
| Checkpoint | ✅ 수동(`CHECKPOINT`) + 자동(512KB) | ✅ 자동 (fuzzy checkpoint) |
| MVCC | ✅ (`_xmin`/`_xmax` 컬럼 방식) | ✅ (언두 버전 체인 방식) |
| VACUUM | ✅ 수동 (`VACUUM`) | ❌ (purge thread 자동 처리) |
| Durability 보장 단위 | 트랜잭션 단위 fsync (`log_commit` + `sync_all`) | 트랜잭션 단위 fsync (`innodb_flush_log_at_trx_commit`) |
| 바이너리 로그 (Binlog) | ❌ | ✅ (복제/PITR용) |
| PITR (Point-in-Time Recovery) | ❌ | ✅ |

---

## 9. 격리 수준

| 격리 수준 | RustDB | MySQL (InnoDB) |
|-----------|--------|----------------|
| READ UNCOMMITTED | ✅ | ✅ |
| READ COMMITTED | ✅ | ✅ |
| REPEATABLE READ | ✅ (BEGIN 시점 전체 테이블 스냅샷) | ✅ (MVCC 버전 체인 스냅샷) |
| SERIALIZABLE | ✅ (커밋 전후 행 수 비교로 팬텀 감지 → 자동 롤백) | ✅ (모든 SELECT에 Shared Lock) |
| 갭 락 (Gap Lock) | ❌ | ✅ |
| 넥스트 키 락 (Next-key Lock) | ❌ | ✅ |
| 팬텀 읽기 방지 방식 | 커밋 전 행 수 비교 (단순, 근사적) | Next-key Lock + MVCC (정확) |

---

## 10. 동시성 제어 / 잠금

| 항목 | RustDB | MySQL (InnoDB) |
|------|--------|----------------|
| Row-level Lock | ✅ (`SELECT FOR UPDATE`, UPDATE/DELETE 충돌 감지) | ✅ |
| Table-level Lock | ❌ | ✅ (`LOCK TABLES`) |
| 데드락 감지 | ✅ (LockManager, wait-for graph DFS) | ✅ (자동 감지 후 victim rollback) |
| SHOW LOCKS | ✅ | ✅ (`SHOW ENGINE INNODB STATUS`) |
| 다중 세션 동시 트랜잭션 | ⚠️ TCP 서버는 멀티스레드이나 Executor가 단일 인스턴스 (`Arc<Mutex>`) | ✅ 완전한 동시성 |

---

## 11. 인덱스 / 스토리지

| 항목 | RustDB | MySQL (InnoDB) |
|------|--------|----------------|
| B+Tree 인덱스 | ✅ ORDER=4 (인메모리 노드, 소형) | ✅ 16KB 페이지 기반 (디스크, ORDER≈수백) |
| B+Tree 리프 연결 리스트 | ❌ (범위 스캔은 트리 순회 `collect_all_kv`) | ✅ (리프 간 이중 연결 리스트) |
| 클러스터드 인덱스 | ✅ (PK 기준 정렬) | ✅ (InnoDB 기본) |
| 보조 인덱스 | ✅ 중복 키 배열 저장, 자동 재빌드 | ✅ |
| 복합 인덱스 | ✅ (null-byte 키 결합) | ✅ |
| 커버링 인덱스 (Index-only scan) | ❌ | ✅ |
| 해시 인덱스 | ❌ | ✅ (Memory 엔진) |
| 어댑티브 해시 인덱스 | ❌ | ✅ (InnoDB 자동) |
| FULLTEXT 인덱스 | ❌ | ✅ |
| SPATIAL 인덱스 | ❌ | ✅ |
| 페이지 크기 | 16KB (`PAGE_SIZE=16384`) | 16KB (기본, 4/8/32/64KB 설정 가능) |
| 데이터 압축 | ✅ LZ4 (.rdb 투명 압축) | ✅ zlib/zstd (테이블 단위 설정) |
| Buffer Pool | ✅ LRU 64페이지 (인메모리) | ✅ innodb_buffer_pool_size 설정 (디스크 기반) |
| Dirty page write-back | ✅ (CHECKPOINT 시 flush) | ✅ (자동 background flushing) |
| 저장 포맷 | 바이너리 `.rdb` (16KB 페이지) + `indexes.json` + `views.json` + `schema.json` | InnoDB `.ibd` 파일 (자체 바이너리 포맷) |

---

## 12. 옵티마이저

| 항목 | RustDB | MySQL |
|------|--------|-------|
| 방식 | 비용 기반 (Cost-Based) | 비용 기반 (Cost-Based) |
| 접근 경로 수 | 5가지 (SeqScan / PkPoint / PkRange / SecondaryIndex / CompositeIndex) | 다수 (+ 인덱스 머지, 루스 인덱스 스캔 등) |
| Join 알고리즘 선택 | ✅ Hash Join (행 수 > 4) vs Nested Loop / ON 조건 방향 무관 | ✅ Hash Join (8.0+), BNL, BKA, NLJ |
| Sort-Merge Join | ❌ | ✅ (정렬된 데이터셋 활용) |
| 통계 정보 | 인메모리 행 수 기반 (log₂N 추정, 단순) | 히스토그램, 인덱스 통계, ANALYZE TABLE |
| EXPLAIN 출력 | ✅ 텍스트 박스 (비용·접근경로·Join 알고리즘) | ✅ 트리형/JSON/ANALYZE 포맷 |
| 쿼리 힌트 | ❌ | ✅ (`USE INDEX`, `STRAIGHT_JOIN` 등) |
| 쿼리 캐시 | ❌ | ❌ (8.0에서 제거됨) |
| 병렬 쿼리 | ❌ | ✅ (8.0+ 제한적 지원) |

---

## 13. 모니터링 / 관리

| 명령 | RustDB | MySQL 동등 명령 |
|------|--------|----------------|
| `SHOW TABLES` | ✅ (현재 DB 기준) | `SHOW TABLES` |
| `DESCRIBE table` | ✅ | `DESCRIBE table` |
| `SHOW BUFFER POOL` | ✅ (히트율, 사용량) | `SHOW ENGINE INNODB STATUS` |
| `SHOW WAL` | ✅ (레코드, 파일 크기) | `SHOW BINARY LOGS` (binlog) |
| `SHOW LOCKS` | ✅ | `SELECT * FROM performance_schema.data_locks` |
| `SHOW ISOLATION LEVEL` | ✅ | `SELECT @@transaction_isolation` |
| `CHECKPOINT` | ✅ 수동 | 자동 (명시적 없음) |
| `VACUUM` | ✅ 수동 dead row 제거 | 자동 purge thread (OPTIMIZE TABLE으로 유사) |
| `SHOW DATABASES` | ✅ | ✅ |
| `SHOW VARIABLES` | ❌ | ✅ |
| `SHOW STATUS` | ❌ | ✅ |
| `SHOW PROCESSLIST` | ❌ | ✅ |
| INFORMATION_SCHEMA | ❌ | ✅ |
| Performance Schema | ❌ | ✅ |
| 슬로우 쿼리 로그 | ❌ | ✅ |

---

## 14. 네트워크 / 보안

| 항목 | RustDB | MySQL |
|------|--------|-------|
| TCP 서버 | ✅ 포트 7878, 멀티스레드 | ✅ 포트 3306 |
| 프로토콜 | 자체 라인 텍스트 프로토콜 | MySQL Wire Protocol (바이너리) |
| 사용자 계정 관리 (CREATE/DROP USER) | ✅ (JSON 영속화) | ✅ (플러그인 인증, caching_sha2_password 등) |
| 권한 관리 (GRANT/REVOKE/SHOW GRANTS) | ✅ (JSON 영속화) | ✅ |
| SSL/TLS | ❌ | ✅ |
| Unix Socket | ❌ | ✅ |
| 복제 (Replication) | ❌ | ✅ (비동기/반동기/그룹 복제) |
| 클러스터링 | ❌ | ✅ (InnoDB Cluster, NDB Cluster) |
| 연결 풀링 | ❌ | ✅ (서버사이드 connection pool) |

---

## 15. SQL 문법 특이사항

| 항목 | RustDB | MySQL |
|------|--------|-------|
| 주석 | `--`, `#`, `/* */` | `--`, `#`, `/* */` |
| `AUTO INCREMENT` | 두 단어 (공백) | `AUTO_INCREMENT` (언더스코어) |
| 세미콜론 멀티 쿼리 | ✅ | ✅ |
| 주석 내 세미콜론 안전 처리 | ✅ | ✅ |
| 세미콜론 뒤 인라인 주석 잔류 처리 | ✅ (`SELECT 1; -- 0` 패턴에서 `-- 0` 잔류가 다음 문장을 오파싱하던 CLI 버그 수정) | ✅ (미해당 — 서버 프로세스가 처리) |
| `USE DATABASE name` | ✅ (DATABASE 키워드 선택적) | ✅ (`USE name` 형식) |
| `DROP INDEX IF EXISTS` | ✅ | ✅ |
| `DROP VIEW IF EXISTS` | ✅ | ✅ |

---

## 16. 미구현 항목 현황

| 항목 | 상태 | 비고 |
|------|------|------|
| 재귀 CTE (`WITH RECURSIVE`) | ✅ 구현 완료 | base case + 반복 실행, positional 컬럼 매핑 |
| INSERT IGNORE / ON DUPLICATE KEY UPDATE | ✅ 구현 완료 | 인덱스 동기화 포함 |
| GROUP_CONCAT (SEPARATOR 포함) | ✅ 구현 완료 | GROUP BY·비집계 양쪽 지원 |
| NULLIF, LPAD, RPAD, CAST, DATEDIFF, DATE_ADD | ✅ 구현 완료 | 스칼라 함수 추가 |
| FROM 없는 스칼라 SELECT | ✅ 구현 완료 | `_dual_` 가상 테이블 방식 |
| FOREIGN KEY SET DEFAULT (ON DELETE / ON UPDATE) | ✅ 구현 완료 | FK 컬럼을 DEFAULT 값으로 자동 변경, NO ACTION 별칭 포함 |
| CREATE USER / DROP USER | ✅ 구현 완료 | IF NOT EXISTS / IF EXISTS, IDENTIFIED BY, JSON 영속화 |
| GRANT / REVOKE / SHOW GRANTS | ✅ 구현 완료 | ALL PRIVILEGES, WITH GRANT OPTION, 객체별 누적·제거, JSON 영속화 |
| SHOW DATABASES | ✅ 구현 완료 | 디스크 기반 DB 목록 출력 |
| 전체 통합 테스트 (`test_full.sql`) | ✅ 완료 | 4 DB · 34 섹션 · **260여 쿼리 · 의도된 오류 1개** (UNIQUE 위반 검증). ROUND(expr/expr, n) · UPDATE SET CONCAT 직접 검증 포함 |
| CLI 인라인 주석 잔류 버그 (`; -- 0` 오염) | ✅ 수정 | `SELECT 1; -- 0` 이후 `buf`에 `-- 0` 잔류 → 다음 문장이 주석으로 오파싱·묵시 스킵. `rustdb-cli/main.rs` 1줄 수정으로 해결 |
| HAVING 절 미참조 집계 함수 누락 | ✅ 수정 | `HAVING COUNT(*) >= n`에서 COUNT(*)가 SELECT에 없으면 그룹 행에 해당 키 없음 → 0건 반환. executor.rs에 HAVING CondExpr 스캔 후 누락 집계 보완 로직 추가 |
| `ROUND(expr / expr, n)` — 함수 인자 내 산술식 | ✅ 수정 | ArithExpr::Func AST 노드 + parse_func_args ArithExpr 기반 재작성으로 `ROUND(salary / 1000000, 2)` 등 정상 지원 |
| `UPDATE t SET col = CONCAT(...)` — UPDATE SET ScalarFunc | ✅ 수정 | eval_arith Func 분기 개선 (Col → 이름 전달, Str → 따옴표, 복합식 → 선평가 후 따옴표). `CONCAT(name, '-', dept)` 정상 반환 |
| 윈도우 함수 (ROW_NUMBER, RANK, LAG 등) | ❌ 미구현 | 실행기 대규모 확장 필요 |
| 저장 프로시저 / 트리거 | ❌ 미구현 | — |
| 복제 / 클러스터링 | ❌ 미구현 | — |
| Sort-Merge Join | ❌ 미구현 | planner.rs 확장 필요 |
| 커버링 인덱스 (Index-only scan) | ❌ 미구현 | — |
| GAP Lock / Next-key Lock | ❌ 미구현 | Serializable 정확도 개선 필요 |
| Undo Log 영속화 | ❌ 미구현 | crash 시 미완료 트랜잭션 잔존 가능 |
| WAL fsync per-commit | ✅ 구현 완료 | COMMIT · CHECKPOINT 레코드에 `sync_all()` 추가 |
| WAL group commit | ❌ 미구현 | 고성능 환경에서 TPS 향상 필요 시 |
| B+Tree 리프 연결 리스트 | ❌ 미구현 | 범위 스캔 최적화 |
| 히스토그램 통계 (ANALYZE TABLE) | ❌ 미구현 | 옵티마이저 정확도 개선 |
| Prepared Statements | ❌ 미구현 | — |
| `rustdb-mcp` (자연어 → SQL) | 🔧 폴더만 생성 | AI MCP 연동 미개발 |
| 쿼리 히스토리 (UI) | ✅ 구현 완료 | 결과 패널 HISTORY 탭. localStorage 최대 200개, 클릭 시 에디터 불러오기, 전체 삭제 버튼 |
| CSV 내보내기 (UI) | 🔧 예정 | — |

---

## 17. DB 엔진 개발 로드맵

| 우선순위 | 항목 | 분류 | 설명 | 관련 파일 | 난이도 |
|----------|------|------|------|-----------|--------|
| ✅ 완료 | WAL fsync per-commit | 내구성 | COMMIT / CHECKPOINT 레코드 기록 시 `sync_all()` 호출 추가. 전원 장애 시 커밋된 트랜잭션 유실 방지. `innodb_flush_log_at_trx_commit=1` 동등. 데이터 변경 레코드(Insert/Update/Delete)는 fsync 생략(커밋 시 보장되므로) | `wal.rs` | ★★ |
| 🔴 높음 | Undo Log 영속화 | 내구성 | 현재 인메모리 undo log는 crash 시 소실됨. 디스크 기반으로 영속화하면 재시작 후 미완료 트랜잭션 롤백 가능 | `txn_manager.rs`, `disk.rs` | ★★★ |
| 🔴 높음 | GAP Lock / Next-key Lock | 동시성 | Serializable 격리에서 팬텀을 행 수 비교로 근사 감지 중. 범위 기반 갭 잠금으로 정확한 팬텀 방지 | `lock_manager.rs` | ★★★★ |
| 🔴 높음 | MVCC 버전 체인 | 동시성 | `_xmin`/`_xmax` 컬럼 방식은 단일 세션 중심. 언두 버전 체인으로 개선하면 다중 세션 읽기 일관성 향상 | `executor.rs`, `txn_manager.rs` | ★★★★ |
| 🔴 높음 | 진정한 다중 세션 동시성 | 동시성 | Executor가 `Arc<Mutex<Executor>>` 단일 인스턴스. 세션별 독립 Executor + 공유 BufferPool 구조로 분리 필요 | `executor.rs`, `buffer_pool.rs` | ★★★★★ |
| 🟡 중간 | B+Tree 리프 연결 리스트 | 스토리지 | 범위 스캔이 `collect_all_kv()`로 전체 트리 순회. 리프 노드 간 `next` 포인터 추가 시 범위 스캔 O(k)로 개선 | `btree.rs` | ★★★ |
| 🟡 중간 | B+Tree ORDER 증가 | 스토리지 | 현재 `ORDER=4` (학습용 최솟값). 16KB 페이지 기준 `ORDER≈100`으로 늘리면 트리 깊이 감소, 검색 성능 향상 | `btree.rs` | ★★★ |
| 🟡 중간 | WAL Group Commit | 성능 | 트랜잭션마다 개별 기록 중. 여러 commit을 하나의 `fsync`로 묶으면 TPS 향상 | `wal.rs` | ★★★ |
| 🟡 중간 | 히스토그램 통계 (ANALYZE TABLE) | 옵티마이저 | 현재 log₂N 행 수 추정만 사용. 컬럼별 값 분포를 수집하면 선택도(selectivity) 추정 정확도 향상 | `planner.rs`, `catalog/` | ★★★ |
| 🟡 중간 | 커버링 인덱스 (Index-only scan) | 옵티마이저 | SELECT 컬럼이 인덱스에 포함된 경우 테이블 로드 없이 인덱스만으로 결과 반환. Buffer Pool 부하 감소 | `planner.rs`, `executor.rs` | ★★★ |
| 🟡 중간 | Sort-Merge Join | 옵티마이저 | JOIN 키 기준 정렬된 두 테이블을 O(N+M)으로 병합. 현재 Hash Join / Nested Loop만 지원 | `planner.rs`, `executor.rs` | ★★★ |
| 🟡 중간 | 증분 VACUUM | 유지보수 | 현재 전체 테이블 스캔 방식. dead row 비율 기준 증분 제거로 온라인 부하 감소 | `executor.rs` | ★★ |
| ✅ 완료 | 재귀 CTE (`WITH RECURSIVE`) | SQL 기능 | base case + UNION ALL 반복 실행. positional 컬럼 매핑으로 depth 등 계산 컬럼 정상 전파 | `ast.rs`, `parser.rs`, `executor.rs` | ★★★★ |
| ✅ 완료 | INSERT IGNORE / ON DUPLICATE KEY UPDATE | SQL 기능 | UNIQUE 위반 시 무시 또는 UPDATE로 전환. 인덱스(B+Tree) 동기화까지 완전 구현 | `executor.rs` | ★★ |
| ✅ 완료 | GROUP_CONCAT / NULLIF / LPAD / RPAD / CAST / DATEDIFF / DATE_ADD | SQL 기능 | 스칼라·집계 함수 확장. FROM 없는 스칼라 SELECT(_dual_) 포함 | `executor.rs`, `parser.rs`, `lexer.rs` | ★★ |
| ✅ 완료 | FOREIGN KEY SET DEFAULT / NO ACTION | 제약 조건 | ON DELETE / ON UPDATE SET DEFAULT. DEFAULT 컬럼값 조회 후 적용. NO ACTION = RESTRICT 별칭 | `ast.rs`, `schema.rs`, `parser.rs`, `executor.rs` | ★★ |
| ✅ 완료 | CREATE USER / DROP USER / GRANT / REVOKE / SHOW GRANTS / SHOW DATABASES | 사용자 관리 | IF NOT EXISTS / IF EXISTS / WITH GRANT OPTION / 객체별 권한 누적·제거. `_users.json`, `_grants.json` 영속화 | `lexer.rs`, `ast.rs`, `parser.rs`, `executor.rs`, `disk.rs` | ★★ |
| ✅ 완료 | 전체 통합 테스트 (`test_full.sql`) | 테스트 | 4 DB · 34 섹션 · **260여 쿼리 · 의도된 오류 1개** (UNIQUE 위반 검증). ROUND(expr/expr, n) · UPDATE SET CONCAT 직접 검증 (우회 없음) | `test/test_full.sql` | ★ |
| ✅ 완료 | CLI 인라인 주석 잔류 버그 수정 | CLI | `SELECT 1; -- 0` 패턴에서 `;` 이후 `-- 0`이 `buf`에 잔류해 다음 문장을 주석으로 오파싱·묵시 스킵. 12개 문장 영향(SET ISOLATION LEVEL, CREATE DATABASE testdb 등). `main.rs` 1줄 추가 수정 | `rustdb-cli/src/main.rs` | ★ |
| ✅ 완료 | HAVING 절 미참조 집계 함수 누락 수정 | 실행기 | `HAVING COUNT(*) >= n`에서 COUNT(*)가 SELECT 목록에 없으면 그룹 행에 해당 키 없음 → 조건 항상 false → 0건 반환. HAVING CondExpr를 스캔해 누락 집계를 보완하는 helper 4개 추가 | `executor.rs` | ★★ |
| ✅ 완료 | 함수 인자 내 ArithExpr 확장 | 파서 | `ROUND(salary / 1000000, 2)` — parse_func_args를 ArithExpr 기반으로 재작성. ArithExpr::Func AST 노드 추가 + expand_arith/eval_arith Func 분기 구현 | `ast.rs`, `parser.rs`, `executor.rs` | ★★ |
| ✅ 완료 | UPDATE SET에서 ScalarFunc 허용 | 파서 | `UPDATE t SET col = CONCAT(name, '-', dept)` — ArithExpr::Func를 UPDATE 우변에서도 평가. eval_arith Func 분기에서 Col은 이름 그대로 전달(resolve가 조회), Str은 따옴표, Num은 그대로, 복합식은 선평가 후 따옴표 처리 | `executor.rs` | ★★ |
| ✅ 완료 | 쿼리 히스토리 (UI) | UI | 결과 패널 HISTORY 탭. localStorage 최대 200개. ✓/✗ 아이콘·시각·소요시간 표시. 클릭 → 에디터 불러오기. 전체 삭제 버튼 | `App.tsx`, `App.css` | ★ |
| ✅ 완료 | 사이드바 컬럼 상세 (UI) | UI | 타입 배지, PK🔑/FK🔗 아이콘, NN/UQ 뱃지. `get_columns_detail` Tauri 커맨드로 ColumnDef 전체 정보 반환 | `main.rs`, `App.tsx` | ★★ |
| ✅ 완료 | 결과 페이지네이션 (UI) | UI | PAGE_SIZE=100. 100행 초과 시 ‹/› 버튼 + 페이지 표시. 쿼리 실행마다 페이지 리셋 | `App.tsx`, `App.css` | ★ |
| 🟡 중간 | CSV 내보내기 (UI) | UI | 결과 테이블을 CSV 파일로 저장. Tauri `save_dialog` + `write_file` 연동 필요 | `App.tsx`, `main.rs` | ★★ |
| 🟡 중간 | 탭 분리 에디터 상태 유지 (UI) | UI | 탭 전환 시 결과·커서 위치 보존. 현재는 탭 전환 시 결과 패널 초기화됨 | `App.tsx` | ★★ |
| 🟢 낮음 | 다크/라이트 테마 토글 (UI) | UI | CSS 변수 기반 테마 전환. 현재 하드코딩된 dark 전용 | `App.css` | ★ |
| 🟢 낮음 | 윈도우 함수 | SQL 기능 | `ROW_NUMBER()`, `RANK()`, `LAG()`, `SUM() OVER (PARTITION BY ...)` 등. SelectColumn AST + 파티션/프레임 실행 엔진 구현 | `ast.rs`, `parser.rs`, `executor.rs` | ★★★★★ |
| 🟢 낮음 | Prepared Statements | SQL 기능 | `PREPARE / EXECUTE / USING` 형식. 반복 실행 쿼리의 파싱 오버헤드 제거 | `parser.rs`, `executor.rs` | ★★★ |
| 🟢 낮음 | INFORMATION_SCHEMA | SQL 기능 | `information_schema.tables / columns / indexes` 시스템 뷰. 클라이언트 툴 연동에 필요 | `executor.rs`, `catalog/` | ★★★ |
| 🟢 낮음 | 병렬 쿼리 실행 | 성능 | Rayon으로 SeqScan을 멀티스레드 분할 처리. 대규모 집계 속도 향상 | `executor.rs` | ★★★★ |
| 🟢 낮음 | 연결 풀링 (TCP 서버) | 네트워크 | 현재 클라이언트별 스레드 생성. 연결 풀로 스레드 오버헤드 감소 | `rustdb-server/` | ★★ |
| 🟢 낮음 | 슬로우 쿼리 로그 | 모니터링 | 임계값(예: 100ms) 초과 쿼리를 파일에 기록. 성능 진단에 활용 | `executor.rs` | ★★ |

---

## 18. 엔진 내부 구조

| 모듈 | 파일 | 역할 |
|------|------|------|
| 파서 | `lexer.rs` | Tokenizer — 키워드, 리터럴, 연산자 분리 |
| 파서 | `parser.rs` | 재귀 하강 파서, 테이블 별칭 → 실테이블명 자동 확장 |
| 파서 | `ast.rs` | AST 노드 정의 (Statement, CondExpr, SelectColumn, DataType 등) |
| 엔진 | `executor.rs` | 쿼리 실행 엔진 — DDL / DML / 트랜잭션 / 뷰 / JOIN 전 처리 |
| 엔진 | `planner.rs` | 비용 기반 옵티마이저 — AccessPath 선택, Join 알고리즘 결정, EXPLAIN 출력 |
| 엔진 | `lock_manager.rs` | Row-level 잠금 + wait-for 그래프 기반 데드락 감지 |
| 스토리지 | `btree.rs` | B+Tree 인덱스 (ORDER=4, 인메모리, 수치 키 비교 지원) |
| 스토리지 | `buffer_pool.rs` | LRU Buffer Pool (64페이지, 16KB, dirty page 추적) |
| 스토리지 | `disk.rs` | 디스크 I/O — `.rdb` 읽기/쓰기, DB 디렉토리 관리, 스키마 영속화 |
| 스토리지 | `page.rs` | 페이지 헤더 구조 + LZ4 투명 압축/해제 |
| 스토리지 | `composite_index.rs` | 복합 인덱스 — null-byte 키 결합, 등치 조건 매칭 |
| 트랜잭션 | `txn_manager.rs` | 트랜잭션 상태, 인메모리 Undo Log, SAVEPOINT, 격리 수준, 스냅샷 |
| 트랜잭션 | `wal.rs` | WAL 바이너리 로그 (op코드: Insert/Update/Delete/Commit/Rollback/Checkpoint) |
| 카탈로그 | `schema.rs` | 테이블 스키마, 컬럼 정의, FK, CHECK 제약, auto_increment 카운터 |
