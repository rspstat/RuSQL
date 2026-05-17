# RustDB vs MySQL vs PostgreSQL — 기능 비교 및 로드맵

> 기준: RustDB v2.2.0 / MySQL 8.0 / PostgreSQL 16  
> ✓ 지원 · △ 부분 지원 · ✗ 미지원

---

## 1. 저장 엔진 / 스토리지

| 항목 | MySQL (InnoDB) | PostgreSQL | RustDB |
|------|---------------|------------|--------|
| 파일 포맷 | B+Tree 클러스터드 인덱스 (.ibd) | 힙 파일 (base/) | JSON 행 + LZ4 압축 (.rdb) |
| 페이지 크기 | 16 KB | 8 KB | 16 KB |
| 버퍼 풀 / 캐시 | InnoDB 버퍼 풀 (수 GB 단위) | shared_buffers | LRU N 페이지 (--buffer-pool-size 옵션, 기본 64) |
| 클러스터드 인덱스 | ✓ (PK 기준 물리 정렬) | ✗ (CLUSTER 명령으로 수동) | ✓ (INSERT 후 PK 기준 물리 정렬 유지) |
| 압축 | Transparent Page Compression | TOAST (가변 길이 컬럼) | LZ4 전체 테이블 |
| WAL | InnoDB Redo Log | WAL (pg_wal/) | ✓ (바이너리, 자동 체크포인트 512 KB) |
| 크래시 복구 | ✓ Redo/Undo | ✓ Redo + MVCC 정리 | ✓ WAL Replay + Undo log |

---

## 2. 데이터 타입

| 타입 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| 정수 | TINYINT·SMALLINT·INT·BIGINT·UNSIGNED | SMALLINT·INT·BIGINT | INT·BIGINT·SMALLINT·TINYINT |
| 실수 | FLOAT·DOUBLE·DECIMAL(p,s) | REAL·DOUBLE·NUMERIC(p,s) | FLOAT·DOUBLE·DECIMAL(p,s) |
| 문자열 | CHAR·VARCHAR(n)·TEXT·MEDIUMTEXT·LONGTEXT | CHAR·VARCHAR(n)·TEXT | VARCHAR(n)·TEXT |
| 이진 | BINARY·VARBINARY·BLOB·LONGBLOB | BYTEA | BLOB (hex 문자열 저장, `0x...` 리터럴 지원) |
| 논리 | TINYINT(1) / BOOL | BOOLEAN | BOOLEAN |
| 날짜/시간 | DATE·TIME·DATETIME·TIMESTAMP·YEAR | DATE·TIME·TIMESTAMP·TIMESTAMPTZ·INTERVAL | DATE·TIME·DATETIME·TIMESTAMP·YEAR |
| 시간대 포함 | ✗ (TIMESTAMP는 UTC 내부) | TIMESTAMPTZ | ✗ |
| JSON | JSON (텍스트) | JSON·JSONB (바이너리 인덱스 가능) | ✓ (JSON 타입 저장, `->` / `->>` 연산자, JSON_EXTRACT / JSON_UNQUOTE / JSON_VALUE 함수) |
| 배열 | ✗ | 모든 타입 배열 가능 | ✗ |
| UUID | ✗ (VARCHAR로 저장) | UUID | ✗ |
| 범위 타입 | ✗ | int4range·tsrange 등 | ✗ |
| 네트워크 주소 | ✗ | INET·CIDR·MACADDR | ✗ |
| ENUM | ✓ (컬럼 단위 정의) | ✓ (타입으로 별도 생성) | ✓ |
| SET | ✓ | ✗ | ✓ |
| 지리 공간 | GEOMETRY·POINT·POLYGON 등 | PostGIS 확장 (geometry, geography) | ✗ |

---

## 3. 트랜잭션 / ACID

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| 원자성 (A) | ✓ | ✓ | ✓ (Undo log + WAL) |
| 일관성 (C) | ✓ | ✓ | ✓ (PK/FK/UNIQUE/CHECK 검증) |
| 격리성 (I) | ✓ | ✓ | ✓ (Deferred Write: DML → session_tables 버퍼, COMMIT 시 반영) |
| 지속성 (D) | ✓ | ✓ | ✓ (WAL fsync + 그룹 커밋) |
| MVCC | ✓ (InnoDB Undo segment) | ✓ (튜플 버전 힙 내 저장) | △ (스냅샷 기반, 완전 다중버전 아님) |
| 격리 수준 | 4가지 (기본: REPEATABLE READ) | 3가지 유효 (기본: READ COMMITTED) | 4가지 (기본: READ COMMITTED) |
| Serializable 구현 | 잠금 기반 (팬텀 방지 gap lock) | SSI (Serializable Snapshot Isolation) | 잠금 기반 (완전 SSI 아님) |
| SAVEPOINT | ✓ | ✓ | ✓ (session_tables 기반 undo 적용, ROLLBACK TO 정상 동작) |
| XA (분산 트랜잭션) | ✓ | ✓ | ✗ |
| 그룹 커밋 | ✓ (binlog 그룹 커밋) | ✓ (WAL writer 통합) | ✓ (GroupCommitCoordinator) |
| 데드락 감지 | ✓ (wait-for 그래프) | ✓ (wait-for 그래프) | ✓ (DFS 사이클 탐지) |
| VACUUM / 공간 회수 | 자동 Purge (InnoDB) | AUTOVACUUM | ✓ (수동 VACUUM 명령 + DML 200회마다 AUTO VACUUM) |

---

## 4. 잠금

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| 행 레벨 잠금 | ✓ | ✓ | ✓ |
| 테이블 잠금 | ✓ (LOCK TABLES) | ✓ (LOCK TABLE) | ✗ |
| 갭 잠금 (Gap Lock) | ✓ (REPEATABLE READ 이상) | ✗ (SSI로 처리) | ✗ |
| SELECT FOR UPDATE | ✓ | ✓ | ✓ |
| SELECT FOR SHARE | ✓ | ✓ | ✓ (공유 잠금, 다중 독자 허용) |
| Advisory Lock | ✗ | ✓ (pg_advisory_lock) | ✗ |

---

## 5. 인덱스

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| B+Tree | ✓ | ✓ | ✓ |
| 해시 인덱스 | ✓ (Memory 엔진) | ✓ | ✗ |
| 전문 검색 (Fulltext) | ✓ | ✓ (GIN) | ✗ |
| 공간 인덱스 (R-Tree) | ✓ | ✓ (GiST) | ✗ |
| 부분 인덱스 | ✗ | ✓ (WHERE 조건 인덱스) | ✗ |
| 표현식 인덱스 | ✓ (함수 기반) | ✓ | ✗ |
| 커버링 인덱스 (Index Only Scan) | ✓ | ✓ | ✓ (EXPLAIN에서 Covering 표시) |
| 복합 인덱스 | ✓ | ✓ | ✓ |
| 내림차순 인덱스 | ✓ (8.0+) | ✓ | ✗ |
| BRIN (블록 범위 인덱스) | ✗ | ✓ | ✗ |
| GIN / GiST | ✗ | ✓ | ✗ |

---

## 6. 쿼리 기능

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| SELECT / WHERE / ORDER BY | ✓ | ✓ | ✓ |
| GROUP BY / HAVING | ✓ | ✓ | ✓ |
| LIMIT / OFFSET | ✓ | ✓ | ✓ |
| DISTINCT | ✓ | ✓ | ✓ |
| INNER / LEFT / RIGHT JOIN | ✓ | ✓ | ✓ |
| FULL OUTER JOIN | ✗ (UNION으로 에뮬레이션) | ✓ | ✓ |
| CROSS JOIN | ✓ | ✓ | ✓ |
| NATURAL JOIN | ✓ | ✓ | ✓ |
| SELF JOIN | ✓ | ✓ | ✓ |
| LATERAL JOIN | ✓ (8.0.14+) | ✓ | ✗ |
| UNION / UNION ALL | ✓ | ✓ | ✓ |
| INTERSECT / EXCEPT | ✓ (8.0+) | ✓ | ✓ (INTERSECT / EXCEPT [ALL]) |
| 서브쿼리 (FROM / WHERE / SELECT) | ✓ | ✓ | ✓ |
| 상관 서브쿼리 | ✓ | ✓ | ✓ |
| EXISTS / NOT EXISTS | ✓ | ✓ | ✓ |
| IN (서브쿼리) | ✓ | ✓ | ✓ |
| CTE (WITH) | ✓ | ✓ | ✓ |
| 재귀 CTE (WITH RECURSIVE) | ✓ | ✓ | ✓ |
| CASE WHEN | ✓ | ✓ | ✓ |
| EXPLAIN / EXPLAIN ANALYZE | ✓ | ✓ | ✓ |

---

## 7. 집계 함수

| 함수 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| COUNT / SUM / AVG / MIN / MAX | ✓ | ✓ | ✓ |
| GROUP_CONCAT / STRING_AGG | GROUP_CONCAT | STRING_AGG | GROUP_CONCAT |
| STDDEV / VARIANCE | ✓ | ✓ | ✓ (모집단 기준, STDDEV_POP / VAR_POP 별칭 포함) |
| BIT_AND / BIT_OR | ✓ | ✓ | ✗ |
| ARRAY_AGG | ✗ | ✓ | ✗ |
| JSON_AGG | ✓ | ✓ | ✗ |
| PERCENTILE_CONT / DISC | ✗ | ✓ | ✗ |
| FILTER (WHERE ...) 절 | ✗ | ✓ | ✗ |
| DISTINCT 집계 (COUNT DISTINCT) | ✓ | ✓ | ✓ (COUNT / SUM / AVG DISTINCT 모두 지원) |

---

## 8. 윈도우 함수

| 함수 | MySQL 8.0+ | PostgreSQL | RustDB |
|------|------------|------------|--------|
| ROW_NUMBER / RANK / DENSE_RANK | ✓ | ✓ | ✓ |
| LAG / LEAD | ✓ | ✓ | ✓ |
| FIRST_VALUE / LAST_VALUE | ✓ | ✓ | ✓ |
| NTH_VALUE | ✓ | ✓ | ✓ |
| NTILE | ✓ | ✓ | ✓ |
| CUME_DIST / PERCENT_RANK | ✓ | ✓ | ✓ |
| ROWS / RANGE 프레임 절 | ✓ | ✓ | ✓ (`ROWS/RANGE BETWEEN ... AND ...`) |
| GROUPS 프레임 | ✗ | ✓ | ✗ |
| 집계 함수를 윈도우로 사용 | ✓ | ✓ | ✓ (SUM/AVG/COUNT/MIN/MAX OVER) |
| PARTITION BY / ORDER BY | ✓ | ✓ | ✓ |

---

## 9. 스칼라 함수

| 범주 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| 문자열 | 50+ (CONCAT, SUBSTR, TRIM, REPLACE, LPAD, RPAD, REGEXP...) | 50+ (+ 정규식, FORMAT) | UPPER, LOWER, LENGTH, TRIM, CONCAT, SUBSTR, REPLACE, LPAD, RPAD, CHAR_LENGTH, LEFT, RIGHT, REVERSE, REPEAT, INSTR, LOCATE, LTRIM, RTRIM, SPACE, ASCII, CHAR, HEX, UNHEX, FORMAT |
| 날짜/시간 | 40+ (DATE_FORMAT, DATEDIFF, TIMESTAMPDIFF, PERIOD_ADD...) | 40+ (EXTRACT, AT TIME ZONE, AGE...) | NOW, CURDATE, DATE_FORMAT, DATE_ADD, DATE_SUB, DATEDIFF, YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, DAYOFWEEK, DAYOFYEAR, WEEKDAY, LAST_DAY, TIMESTAMPDIFF, CURTIME, CURRENT_TIMESTAMP, UNIX_TIMESTAMP, FROM_UNIXTIME |
| 수학 | 20+ (ROUND, ABS, CEIL, FLOOR, MOD, SQRT, POWER, LOG, SIN...) | 20+ | ROUND, ABS, CEIL, FLOOR, MOD, SQRT, POW/POWER, LOG, LOG2, LOG10, EXP, SIN, COS, TAN, PI, SIGN, TRUNCATE, RAND |
| 조건부 | COALESCE, IFNULL, NULLIF, IF, CASE | COALESCE, NULLIF, CASE, GREATEST, LEAST | COALESCE, IFNULL, NULLIF, IF, CAST, GREATEST, LEAST |
| 정규식 | REGEXP, REGEXP_REPLACE (8.0+) | REGEXP_MATCH, REGEXP_REPLACE | ✓ (REGEXP/RLIKE 연산자, REGEXP_LIKE/REGEXP_REPLACE/REGEXP_MATCH 함수) |
| 타입 변환 | CAST, CONVERT | CAST, :: 연산자 | CAST, CONVERT, ISNULL, BIT_LENGTH |
| 기타 | UUID, MD5 | UUID, MD5 | UUID, MD5 |
| 사용자 정의 함수 | ✓ (CREATE FUNCTION) | ✓ (PL/pgSQL, PL/Python 등) | ✓ (CREATE FUNCTION name(params) RETURNS type RETURN expr; DROP FUNCTION) |

---

## 10. DDL

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| CREATE / DROP TABLE | ✓ | ✓ | ✓ |
| ALTER TABLE (ADD/DROP/MODIFY/RENAME COLUMN) | ✓ | ✓ | ✓ |
| TRUNCATE | ✓ | ✓ | ✓ |
| CREATE / DROP INDEX | ✓ | ✓ | ✓ |
| CREATE / DROP VIEW | ✓ | ✓ | ✓ |
| CREATE / DROP DATABASE | ✓ | ✓ | ✓ |
| CREATE SCHEMA | ✓ | ✓ (DB·스키마 분리) | ✓ (CREATE DATABASE 별칭, MySQL 방식) |
| CREATE TYPE (도메인/열거형/복합) | ✗ | ✓ | ✗ |
| CREATE SEQUENCE | ✗ | ✓ | ✗ (AUTO_INCREMENT만) |
| ALTER TABLE ADD CONSTRAINT | ✓ | ✓ | ✓ (FK/UNIQUE/CHECK) |
| 온라인 DDL | ✓ (대부분 non-blocking) | ✓ (일부 non-blocking) | ✗ |
| 파티셔닝 | ✓ (RANGE/LIST/HASH/KEY) | ✓ (선언적 파티셔닝) | ✗ |

---

## 11. 제약조건

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| PRIMARY KEY | ✓ (단일/복합) | ✓ | ✓ (단일/복합) |
| FOREIGN KEY + ON DELETE/UPDATE | ✓ (RESTRICT/CASCADE/SET NULL/SET DEFAULT) | ✓ | ✓ |
| UNIQUE | ✓ | ✓ | ✓ |
| NOT NULL | ✓ | ✓ | ✓ |
| CHECK | ✓ (8.0.16+, 실제 강제) | ✓ | ✓ |
| DEFAULT | ✓ | ✓ | ✓ |
| DEFERRABLE 제약 | ✗ | ✓ | ✗ |
| EXCLUDE 제약 | ✗ | ✓ (GiST 인덱스 기반) | ✗ |

---

## 12. DML

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| INSERT VALUES | ✓ | ✓ | ✓ |
| INSERT SELECT | ✓ | ✓ | ✓ |
| INSERT ... ON DUPLICATE KEY UPDATE | ✓ | ✗ | ✓ (ON DUPLICATE KEY UPDATE / ON CONFLICT UPDATE) |
| INSERT ... ON CONFLICT (UPSERT) | ✗ | ✓ | ✓ (ABORT/IGNORE/UPDATE) |
| REPLACE INTO | ✓ | ✗ | ✗ |
| UPDATE (단일 테이블) | ✓ | ✓ | ✓ |
| UPDATE (다중 테이블 JOIN) | ✓ | ✗ (FROM 절로 에뮬레이션) | ✓ (MULTI UPDATE) |
| DELETE (단일 테이블) | ✓ | ✓ | ✓ |
| DELETE (다중 테이블 JOIN) | ✓ | ✗ (USING으로 에뮬레이션) | ✓ (MULTI DELETE) |
| RETURNING 절 | ✗ | ✓ | ✓ (INSERT/UPDATE/DELETE) |
| MERGE / UPSERT 표준 | ✓ (8.0.31+) | ✓ (15+) | ✓ (MERGE INTO ... USING ... ON ... WHEN MATCHED/NOT MATCHED) |

---

## 13. 쿼리 최적화 / 실행 계획

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| 비용 기반 최적화 (CBO) | ✓ | ✓ | ✓ |
| 통계 기반 선택도 | ✓ (histogram) | ✓ (pg_statistic) | ✓ (ANALYZE TABLE) |
| 인덱스 자동 선택 | ✓ | ✓ | ✓ |
| JOIN 알고리즘 | NestedLoop, Hash, BNL | NestedLoop, Hash, MergeJoin | NestedLoop, Hash, SortMerge |
| JOIN 순서 최적화 | ✓ (동적 프로그래밍) | ✓ (Geqo + 동적 프로그래밍) | △ (그리디 INNER JOIN만) |
| 병렬 쿼리 | △ (일부 지원) | ✓ | ✗ |
| JIT 컴파일 | ✗ | ✓ (LLVM) | ✗ |
| 적응형 해시 인덱스 | ✓ (AHI, InnoDB 내부) | ✗ | ✗ |
| EXPLAIN 출력 | ✓ | ✓ (VERBOSE, BUFFERS, FORMAT JSON...) | ✓ (접근 경로·비용·실제 행 수) |
| 쿼리 힌트 | ✓ (USE INDEX, STRAIGHT_JOIN...) | ✓ (pg_hint_plan 확장) | ✗ |

---

## 14. 보안 / 사용자 관리

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| 사용자 모델 | user@host | 역할 (Role) | user@host |
| GRANT / REVOKE | ✓ | ✓ | ✓ (기본) |
| 역할 (ROLE) | ✓ (8.0+) | ✓ | ✗ |
| 열 레벨 권한 | ✓ | ✓ | ✗ |
| 행 레벨 보안 (RLS) | ✗ | ✓ | ✗ |
| 인증 방식 | Password, PAM, LDAP, Kerberos, ED25519... | Password, md5, scram-sha-256, LDAP, GSSAPI, PAM... | Password (SHA-256 해시) |
| 비밀번호 해싱 | SHA-2, caching_sha2_password | scram-sha-256 | ✓ (SHA-256, 레거시 평문 자동 마이그레이션) |
| TLS / SSL | ✓ | ✓ | ✗ |
| 감사 로그 | 엔터프라이즈 | pg_audit 확장 | ✗ |

---

## 15. 고가용성 / 분산

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| 복제 | 비동기·반동기·동기 | 스트리밍·논리 복제 | ✗ |
| 자동 장애 복구 | InnoDB Cluster / MHA | Patroni / repmgr | ✗ |
| 샤딩 | NDB Cluster / Vitess | Citus / Crunchy | ✗ |
| 연결 풀링 | ProxySQL | PgBouncer | ✗ |
| 읽기 복제본 | ✓ | ✓ | ✗ |

---

## 16. 관리 / 운영

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| SHOW TABLES / DESCRIBE | ✓ | 유사 (\\d, information_schema) | ✓ |
| INFORMATION_SCHEMA | ✓ (전체) | ✓ (전체) | ✓ (schemata, tables, columns, key_column_usage, table_constraints, statistics, views, character_sets, collations, engines) |
| SHOW DATABASES | ✓ | ✓ (\\l) | ✓ |
| SHOW CREATE TABLE | ✓ | ✓ (pg_dump) | ✓ |
| SHOW PROCESSLIST | ✓ | pg_stat_activity | ✓ (현재 세션 정보 표시) |
| SHOW LOCKS | ✓ | pg_locks | ✓ |
| SHOW WAL | ✗ (별도 바이너리 로그) | pg_walinspect | ✓ |
| CHECKPOINT | ✓ | ✓ | ✓ |
| VACUUM | ✓ (자동 Purge) | ✓ (AUTOVACUUM) | ✓ (수동) |
| 백업 / 덤프 | mysqldump, XtraBackup | pg_dump, pg_basebackup | ✓ (BACKUP [DATABASE db] [INTO 'file'] — mysqldump 스타일 SQL 생성) |
| 저장 프로시저 | ✓ | ✓ (PL/pgSQL 등) | ✓ (CREATE/DROP PROCEDURE, CALL) |
| 트리거 | ✓ | ✓ | ✓ (BEFORE/AFTER INSERT/UPDATE/DELETE) |
| 이벤트 스케줄러 | ✓ | pg_cron 확장 | ✗ |

---

## 17. 클라이언트 / 네트워크 프로토콜

| 항목 | MySQL | PostgreSQL | RustDB |
|------|-------|------------|--------|
| 공식 프로토콜 | MySQL Protocol (포트 3306) | PostgreSQL wire protocol (포트 5432) | 텍스트 기반 TCP (---END--- 구분자) |
| MySQL wire protocol | ✓ | ✗ | ✓ (v2.2.0, 포트 3306, mysql CLI / DBeaver 호환) |
| 드라이버 호환 | JDBC, ODBC, Python, Go, Node.js... | JDBC, ODBC, libpq, Python, Go, Node.js... | rustdb-client CLI + MySQL 호환 클라이언트 |
| 커넥션 풀 지원 | ✓ | ✓ | ✗ |
| Prepared Statements | ✓ | ✓ | ✓ (MySQL 바이너리 프로토콜 COM_STMT_PREPARE/EXECUTE) |
| 배치 실행 | ✓ | ✓ | ✗ (멀티쿼리 ; 분리) |
| WebSocket / HTTP API | ✗ (별도 미들웨어) | ✗ (PostgREST 등) | ✗ |

---

## 18. UI / 관리 도구

| 항목 | MySQL Workbench | pgAdmin / DBeaver | RustDB UI |
|------|-----------------|-------------------|-----------|
| 쿼리 에디터 | ✓ (구문 강조, 자동완성) | ✓ | ✓ (Monaco, SQL 자동완성) |
| ERD 다이어그램 | ✓ | ✓ | ✓ (FK 관계선, 팬/줌, 카드 드래그) |
| 결과 테이블 | ✓ (셀 편집 가능) | ✓ | ✓ (컬럼 너비 조절, 100행 페이징, 헤더 클릭 정렬, 행 번호, 실시간 검색) |
| 히스토리 | ✓ | ✓ | ✓ (200개 보존, 연결별 분리) |
| 다중 탭 | ✓ | ✓ | ✓ (탭별 결과·에디터 상태 유지) |
| 연결 관리 | ✓ | ✓ | ✓ (연결별 독립 데이터 디렉토리) |
| 서버 모니터링 | ✓ (Process·메모리·쿼리 통계) | ✓ | △ (TCP 서버 on/off·클라이언트 수·로그) |
| 데이터 임포트/익스포트 | ✓ (CSV, SQL) | ✓ | ✓ (CSV 익스포트: export_csv 명령, CSV 임포트: import_csv 명령) |
| 인덱스/테이블 시각적 편집 | ✓ | ✓ | ✗ |

---

## 앞으로 하면 좋은 것 (우선순위 순)

### 엔진

#### 1순위 — 안정성과 정확성 ✓ 전부 구현 완료 (v2.2.0)

| 항목 | 설명 | 상태 |
|------|------|------|
| 비밀번호 해싱 | SHA-256 해시 저장, 레거시 평문 자동 마이그레이션 | ✓ 완료 |
| ALTER TABLE ADD/DROP CONSTRAINT | FK / UNIQUE / CHECK 제약 추가·제거 지원 | ✓ 완료 |
| COUNT(DISTINCT col) | GROUP BY 및 전체 집계 모두 지원 | ✓ 완료 |
| FULL OUTER JOIN | Nested Loop 기반, NULL 패딩 정확 구현 | ✓ 완료 |
| 상관 서브쿼리 완전 지원 | IN / NOT IN 경로에도 외부 행 값 치환 | ✓ 완료 |
| RETURNING 절 | INSERT / UPDATE / DELETE 후 영향받은 행 반환 | ✓ 완료 |

#### 1.5순위 — 추가 데이터 타입 / 잠금 (v2.2.0 추가)

| 항목 | 설명 | 상태 |
|------|------|------|
| BIGINT / SMALLINT / TINYINT | 정수 타입 추가, UNSIGNED 키워드 무시 처리 | ✓ 완료 |
| JSON 데이터 타입 | JSON 저장, `->` / `->>` 연산자, JSON_EXTRACT / JSON_UNQUOTE / JSON_VALUE | ✓ 완료 |
| SELECT FOR SHARE | 공유 잠금 (FOR SHARE), 공유/배타 잠금 충돌·업그레이드·해제 완전 구현 | ✓ 완료 |
| SHOW CREATE TABLE UI | 테이블 우클릭 컨텍스트 메뉴에 "Show Create Table" 항목 추가 | ✓ 완료 |

#### 2순위 — 기능 완성도 ✓ 전부 구현 완료 (v2.2.0 이전)

| 항목 | 설명 | 상태 |
|------|------|------|
| 윈도우 함수 ROWS/RANGE 프레임 | `ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW` | ✓ 완료 (v2.2.0) |
| INTERSECT / EXCEPT | 집합 연산 완성 | ✓ 완료 |
| NTILE / CUME_DIST / PERCENT_RANK | 윈도우 함수 나머지 | ✓ 완료 |
| 집계 함수에 DISTINCT | `COUNT(DISTINCT col)`, `SUM(DISTINCT col)`, `AVG(DISTINCT col)` | ✓ 완료 |
| STDDEV / VARIANCE | 통계 집계 함수 (모집단 기준, STD/STDDEV_POP/VAR_POP 별칭 포함) | ✓ 완료 |
| SHOW CREATE TABLE | 현재 스키마에서 DDL 역생성 | ✓ 완료 |
| MERGE / UPSERT 표준 문법 | SQL:2003 MERGE INTO | ✓ 완료 (v2.2.0) |
| 정규식 함수 | REGEXP/RLIKE 연산자, REGEXP_LIKE/REGEXP_REPLACE/REGEXP_MATCH | ✓ 완료 (v2.2.0) |

#### 3순위 — 성능

| 항목 | 설명 | 난이도 |
|------|------|--------|
| 완전한 MVCC | 튜플 레벨 버전 관리 → VACUUM 효율 개선 | 대 |
| 행 기반 저장 → 페이지별 저장 | 현재 테이블 전체를 한 파일에 저장 → 페이지 단위 분할로 대용량 대응 | 대 |
| 인덱스 전용 스캔 개선 | 현재 인덱스 후 행 재조회 → 커버링 시 행 재조회 생략 | 중 |
| JOIN 최적화 고도화 | INNER 외 JOIN도 순서 최적화 적용 | 중 |
| 동적 프로그래밍 JOIN 플래너 | 그리디 → DP 기반 전체 탐색 (테이블 수 적을 때) | 대 |
| 버퍼 풀 크기 설정 | ✓ 완료 (v2.2.0) — `--buffer-pool-size N` CLI 옵션 추가 | ✓ 완료 |
| AUTO VACUUM | ✓ 완료 (v2.2.0) — DML 200회 누적 시 자동 dead row 정리 | ✓ 완료 |

#### 4순위 — 생태계

| 항목 | 설명 | 난이도 |
|------|------|--------|
| MySQL wire protocol 호환 | 기존 MySQL 드라이버(JDBC, Python mysql-connector 등)로 직접 접속 | ✓ 완료 (v2.2.0) |
| INFORMATION_SCHEMA 가상 테이블 | DBeaver/Workbench 테이블·컬럼 자동 목록 (10개 가상 뷰) | ✓ 완료 (v2.2.0) |
| 트리거 | BEFORE/AFTER INSERT/UPDATE/DELETE | ✓ 완료 (v2.2.0) |
| 저장 프로시저 제어문 | IF/ELSEIF/ELSE, WHILE, LOOP/LEAVE, REPEAT/UNTIL, DECLARE, SET 변수 — BEGIN...END 블록 내 완전한 제어 흐름 지원 | ✓ 완료 (v2.2.0) |
| 사용자 정의 스칼라 함수 | CREATE FUNCTION name(params) RETURNS type RETURN expr | ✓ 완료 (v2.2.0) |
| 뷰 업데이트 (Updatable View) | 단순 뷰에 INSERT/UPDATE/DELETE — JOIN/DISTINCT/GROUP BY 없는 뷰 지원 | ✓ 완료 (v2.2.0) |
| BACKUP 명령 | mysqldump 스타일 SQL 덤프 생성, BACKUP [DATABASE db] [INTO 'file'] | ✓ 완료 (v2.2.0) |
| SHOW PROCESSLIST | 현재 세션 정보 표시 | ✓ 완료 (v2.2.0) |
| 추가 스칼라 함수 | SQRT/POW/LOG/SIN/COS 등 수학 함수, CHAR_LENGTH/LEFT/RIGHT/REVERSE 등 문자열, TIMESTAMPDIFF/LAST_DAY/FROM_UNIXTIME 등 날짜, UUID/MD5 등 | ✓ 완료 (v2.2.0) |
| ROLLBACK 인덱스 복원 버그 수정 | 트랜잭션 ROLLBACK 후 PK/보조/복합 인덱스 정확히 복원 | ✓ 완료 (v2.2.0) |
| 파티셔닝 | RANGE 파티션 (예: 날짜별 분할) | 대 |
| 복제 (단방향) | WAL 스트리밍으로 읽기 복제본 지원 | 대 |

---

### UI

#### 완료된 항목 (v2.2.0)

| 항목 | 설명 | 상태 |
|------|------|------|
| CSV 익스포트 | export_csv 명령 — SELECT 결과를 CSV 파일로 저장 | ✓ 완료 |
| CSV 임포트 | import_csv 명령 — CSV 파일을 테이블에 INSERT 일괄 실행 | ✓ 완료 |
| SHOW CREATE TABLE UI | 테이블 우클릭 → DDL 스크립트 보기 | ✓ 완료 |
| 결과 컬럼 헤더 클릭 정렬 | ▲/▼/⇅ 토글 — 클라이언트 사이드, 수치/문자열 자동 감지 | ✓ 완료 |
| 결과 행 번호 | 결과 테이블 첫 열에 #1, #2... 자동 표시 | ✓ 완료 |
| 결과 내 실시간 검색 | 결과 패널 상단 검색 입력 → 해당 키워드를 포함한 행만 표시 | ✓ 완료 |
| 키보드 단축키 | Ctrl+T (새 탭), Ctrl+W (탭 닫기), Ctrl+Enter (쿼리 실행), Ctrl+Shift+F (포매터) | ✓ 완료 |
| SQL 포매터 | sql-formatter 패키지 — Ctrl+Shift+F 로 에디터 SQL 자동 정렬 | ✓ 완료 |
| 쿼리 북마크 | ★ 버튼으로 북마크 저장, 사이드바 BOOKMARKS 목록 표시, 클릭 시 에디터 로드, × 삭제 | ✓ 완료 |
| 사이드바 테이블 검색 | SCHEMAS 패널 상단 검색 입력 → 테이블 이름 실시간 필터 | ✓ 완료 |
| EXPLAIN 트리 시각화 | EXPLAIN/EXPLAIN ANALYZE 결과를 테이블 대신 구조화된 카드 형태로 렌더링 | ✓ 완료 |

#### 남은 항목

| 항목 | 설명 | 난이도 |
|------|------|--------|
| 결과 셀 직접 편집 | 그리드 셀 클릭 → 인라인 편집 → UPDATE 자동 생성 | 중 |
| 다크/라이트 테마 전환 | 현재 다크 고정 | 소 |
| ERD 자동 레이아웃 | FK 관계 기반 자동 배치 개선 (Dagre, ELK 알고리즘) | 중 |
| AI 뷰 | 자연어 → SQL 변환 (Claude API 연동) | 중 |
| 테이블 시각적 편집기 | ALTER TABLE을 GUI로 (컬럼 추가/삭제/타입 변경) | 중 |
