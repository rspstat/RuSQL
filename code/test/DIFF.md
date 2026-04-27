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
| 다중 데이터베이스 | ✅ (DB 단위 격리, USE 전환) | ✅ (schema 단위 분리) |

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
| TRUNCATE TABLE | ✅ | ✅ |
| ALTER TABLE ADD COLUMN | ✅ | ✅ |
| ALTER TABLE DROP COLUMN | ✅ | ✅ |
| ALTER TABLE RENAME COLUMN | ✅ | ✅ (8.0+) |
| ALTER TABLE MODIFY COLUMN | ✅ | ✅ |
| ALTER TABLE RENAME TABLE | ✅ (`RENAME TO`) | ✅ (`RENAME TABLE`) |
| CREATE INDEX | ✅ (단일/복합) | ✅ |
| DROP INDEX | ✅ | ✅ |
| CREATE VIEW | ✅ | ✅ |
| DROP VIEW | ✅ | ✅ |
| DESCRIBE | ✅ | ✅ |
| CREATE DATABASE | ✅ (IF NOT EXISTS 포함) | ✅ |
| DROP DATABASE | ✅ (IF EXISTS 포함) | ✅ |
| USE [DATABASE] | ✅ | ✅ |
| CREATE USER / GRANT / REVOKE | ❌ | ✅ |
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
| INSERT IGNORE | ❌ | ✅ |
| INSERT ... ON DUPLICATE KEY UPDATE | ❌ | ✅ |
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
| LIMIT / OFFSET | ✅ | ✅ |
| GROUP BY / HAVING | ✅ | ✅ |
| DISTINCT | ✅ | ✅ |
| 산술 표현식 (SELECT/WHERE/UPDATE) | ✅ | ✅ |
| 집계 함수 (COUNT/SUM/AVG/MIN/MAX) | ✅ | ✅ |
| GROUP_CONCAT | ❌ | ✅ |
| CASE WHEN | ✅ | ✅ |
| 윈도우 함수 (ROW_NUMBER, RANK 등) | ❌ | ✅ (8.0+) |
| 스칼라 서브쿼리 | ✅ | ✅ |
| IN 서브쿼리 | ✅ | ✅ |
| EXISTS 서브쿼리 (상관 포함) | ✅ | ✅ |
| FROM 절 서브쿼리 | ✅ | ✅ |
| UNION / UNION ALL | ✅ | ✅ |
| CTE (WITH ... AS) | ✅ (단순/다중/INSERT) | ✅ (8.0+) |
| 재귀 CTE | ❌ | ✅ (8.0+) |
| SELECT ... FOR UPDATE | ✅ | ✅ |
| EXPLAIN | ✅ (비용 기반 실행 계획) | ✅ (EXPLAIN/EXPLAIN ANALYZE) |
| table.column dot notation | ✅ | ✅ |
| 테이블 별칭 (alias) | ✅ | ✅ |
| 준비된 문 (Prepared Statements) | ❌ | ✅ |

---

## 6. 스칼라 / 내장 함수

| 함수 범주 | RustDB | MySQL |
|-----------|--------|-------|
| 문자열 | UPPER, LOWER, LENGTH, TRIM, CONCAT, SUBSTR, REPLACE | 위 포함 + LPAD, RPAD, INSTR, LOCATE, LEFT, RIGHT, REPEAT, REVERSE, FORMAT 등 |
| 수학 | ROUND, ABS, CEIL, FLOOR, MOD | 위 포함 + POWER, SQRT, LOG, EXP, RAND 등 |
| 날짜 | NOW, CURDATE, DATE_FORMAT | 위 포함 + DATEDIFF, DATE_ADD, DATE_SUB, YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, TIMESTAMPDIFF 등 |
| NULL 처리 | COALESCE, IFNULL | 위 포함 + NULLIF, IF, ISNULL |
| 기타 | - | CAST, CONVERT, UUID, MD5, SHA1/SHA2, COMPRESS 등 |

---

## 7. 제약 조건

| 제약 | RustDB | MySQL |
|------|--------|-------|
| PRIMARY KEY (단일) | ✅ | ✅ |
| PRIMARY KEY (복합) | ✅ | ✅ |
| NOT NULL | ✅ | ✅ |
| UNIQUE | ✅ | ✅ |
| AUTO INCREMENT | ✅ | ✅ |
| DEFAULT | ✅ | ✅ |
| CHECK | ✅ (컬럼/테이블 레벨) | ✅ (8.0.16+) |
| FOREIGN KEY RESTRICT | ✅ | ✅ |
| FOREIGN KEY CASCADE | ✅ | ✅ |
| FOREIGN KEY SET NULL | ✅ | ✅ |
| FOREIGN KEY SET DEFAULT | ❌ | ✅ |
| ON UPDATE CASCADE | ✅ | ✅ |

---

## 8. 트랜잭션 / ACID

| 항목 | RustDB | MySQL (InnoDB) |
|------|--------|----------------|
| BEGIN / COMMIT / ROLLBACK | ✅ | ✅ |
| SAVEPOINT | ✅ | ✅ |
| ROLLBACK TO SAVEPOINT | ✅ | ✅ |
| RELEASE SAVEPOINT | ✅ | ✅ |
| WAL (Redo Log) | ✅ 바이너리, 단일 파일, 512KB 자동 Checkpoint | ✅ 바이너리 redo log (ib_logfile), 복수 파일 |
| Undo Log | ✅ 인메모리 Undo Log | ✅ 디스크 기반 Undo Tablespace |
| Crash Recovery | ✅ (재시작 시 WAL replay) | ✅ (자동 복구) |
| Checkpoint | ✅ 수동(`CHECKPOINT`) + 자동(512KB) | ✅ 자동 (fuzzy checkpoint) |
| MVCC | ✅ (`_xmin`/`_xmax` 컬럼 방식) | ✅ (언두 버전 체인 방식) |
| VACUUM | ✅ 수동 (`VACUUM`) | ❌ (purge thread 자동 처리) |
| Durability 보장 단위 | 세션 단위 WAL | 트랜잭션 단위 fsync |
| 바이너리 로그 (Binlog) | ❌ | ✅ (복제/PITR용) |
| PITR (Point-in-Time Recovery) | ❌ | ✅ |

---

## 9. 격리 수준

| 격리 수준 | RustDB | MySQL (InnoDB) |
|-----------|--------|----------------|
| READ UNCOMMITTED | ✅ | ✅ |
| READ COMMITTED | ✅ | ✅ |
| REPEATABLE READ | ✅ (BEGIN 시점 전체 스냅샷) | ✅ (MVCC 버전 체인 스냅샷) |
| SERIALIZABLE | ✅ (행 수 변화로 팬텀 감지, 자동 롤백) | ✅ (모든 SELECT에 Shared Lock) |
| 갭 락 (Gap Lock) | ❌ | ✅ |
| 넥스트 키 락 (Next-key Lock) | ❌ | ✅ |
| 팬텀 읽기 방지 방식 | 커밋 전 행 수 비교 (단순) | Next-key Lock / MVCC (정확) |

---

## 10. 동시성 제어 / 잠금

| 항목 | RustDB | MySQL (InnoDB) |
|------|--------|----------------|
| Row-level Lock | ✅ (`SELECT FOR UPDATE`, UPDATE/DELETE 충돌 감지) | ✅ |
| Table-level Lock | ❌ | ✅ (`LOCK TABLES`) |
| 데드락 감지 | ✅ (LockManager, wait-for graph) | ✅ (자동 감지 후 victim rollback) |
| SHOW LOCKS | ✅ | ✅ (`SHOW ENGINE INNODB STATUS`) |
| 다중 세션 동시 트랜잭션 | ⚠️ TCP 서버는 멀티스레드이나 Executor가 단일 인스턴스 (Arc<Mutex>) | ✅ 완전한 동시성 |

---

## 11. 인덱스 / 스토리지

| 항목 | RustDB | MySQL (InnoDB) |
|------|--------|----------------|
| B+Tree 인덱스 | ✅ ORDER=4 (소형, 인메모리) | ✅ 16KB 페이지 기반 (디스크) |
| 클러스터드 인덱스 | ✅ (PK 기준 정렬) | ✅ (InnoDB 기본) |
| 보조 인덱스 | ✅ 중복 키 배열 저장 | ✅ |
| 복합 인덱스 | ✅ (null-byte 키 결합) | ✅ |
| 해시 인덱스 | ❌ | ✅ (Memory 엔진) |
| 어댑티브 해시 인덱스 | ❌ | ✅ (InnoDB 자동) |
| FULLTEXT 인덱스 | ❌ | ✅ |
| SPATIAL 인덱스 | ❌ | ✅ |
| 페이지 크기 | 16KB (`PAGE_SIZE=16384`) | 16KB (기본, 4/8/32/64KB 설정 가능) |
| 데이터 압축 | ✅ LZ4 (.rdb 투명 압축) | ✅ zlib/zstd (테이블 단위 설정) |
| Buffer Pool | ✅ LRU 64페이지 (인메모리) | ✅ innodb_buffer_pool_size 설정 (디스크 기반) |
| 저장 포맷 | 바이너리 `.rdb` + JSON 스키마 | InnoDB `.ibd` 파일 (자체 바이너리 포맷) |

---

## 12. 옵티마이저

| 항목 | RustDB | MySQL |
|------|--------|-------|
| 방식 | 비용 기반 (Cost-Based) | 비용 기반 (Cost-Based) |
| 접근 경로 선택 | ✅ SeqScan, PkPoint, PkRange, SecondaryIndex, CompositeIndex | ✅ + 인덱스 머지, 루스 인덱스 스캔 등 |
| Join 알고리즘 선택 | ✅ Hash Join vs Nested Loop (행 수 > 4 기준) | ✅ Hash Join (8.0+), BNL, BKA, NLJ |
| 통계 정보 | 인메모리 행 수 기반 (log₂N 추정) | 히스토그램, 인덱스 통계, ANALYZE TABLE |
| EXPLAIN 출력 | ✅ 텍스트 박스 (비용·접근경로·Join 알고리즘) | ✅ 트리형/JSON/ANALYZE 포맷 |
| 쿼리 힌트 | ❌ | ✅ (`USE INDEX`, `STRAIGHT_JOIN` 등) |
| 쿼리 캐시 | ❌ | ❌ (8.0에서 제거됨) |

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
| 사용자 인증 | ❌ | ✅ (플러그인 인증, caching_sha2_password 등) |
| SSL/TLS | ❌ | ✅ |
| Unix Socket | ❌ | ✅ |
| 권한 관리 (GRANT/REVOKE) | ❌ | ✅ |
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
| `USE DATABASE name` | ✅ (DATABASE 키워드 선택적) | ✅ (`USE name` 형식) |

---

## 16. 미구현 / 개발 예정

| 항목 | 상태 |
|------|------|
| 재귀 CTE | 미구현 |
| 윈도우 함수 (ROW_NUMBER, RANK 등) | 미구현 |
| INSERT IGNORE / ON DUPLICATE KEY | 미구현 |
| 저장 프로시저 / 트리거 | 미구현 |
| 사용자 인증 / 권한 관리 | 미구현 |
| 복제 / 클러스터링 | 미구현 |
| `rustdb-mcp` (자연어 → SQL) | 폴더만 생성, 미개발 |
| 쿼리 히스토리 (UI) | 예정 |
| CSV 내보내기 (UI) | 예정 |

---

> **요약**: RustDB는 핵심 RDBMS 기능(B+Tree, WAL, MVCC, 4단계 격리, 비용 기반 옵티마이저, CTE, UNION, 서브쿼리, JOIN, 다중 DB 격리)을 학습 목적으로 직접 구현한 프로젝트로, 단일 사용자 환경의 SQL 처리는 대부분 지원합니다. MySQL 대비 **사용자 인증/권한, 저장 프로시저/트리거, 윈도우 함수, 완전한 다중 세션 동시성**이 미구현 상태입니다.
