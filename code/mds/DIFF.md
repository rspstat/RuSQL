# RuSQL vs MySQL vs PostgreSQL vs Oracle — 기능 비교

> 기준: RuSQL v2.2.0 / MySQL 8.0 / PostgreSQL 16 / Oracle 21c  
> ✓ 지원 · △ 부분 지원 · ✗ 미지원

---

## 1. 저장 엔진 / 스토리지

| 항목 | MySQL (InnoDB) | PostgreSQL | Oracle | RuSQL |
|------|---------------|------------|--------|--------|
| 파일 포맷 | B+Tree 클러스터드 인덱스 (.ibd) | 힙 파일 (base/) | 데이터파일 (.dbf), 블록(기본 8KB) | JSON 행 + LZ4 압축 (.rdb) |
| 페이지 크기 | 16 KB | 8 KB | 8 KB (설정 가능) | 16 KB |
| 버퍼 풀 / 캐시 | InnoDB 버퍼 풀 | shared_buffers | Buffer Cache (SGA) | LRU N 페이지 (--buffer-pool-size, 기본 64) |
| 클러스터드 인덱스 | ✓ (PK 기준 물리 정렬) | ✗ (CLUSTER 명령으로 수동) | ✓ (IOT: Index-Organized Table) | ✓ (INSERT 후 PK 기준 물리 정렬 유지) |
| 압축 | Transparent Page Compression | TOAST (가변 길이 컬럼) | Advanced Compression (유료) | LZ4 전체 테이블 |
| WAL | InnoDB Redo Log | WAL (pg_wal/) | Redo Log + Archive Log | ✓ (바이너리, 자동 체크포인트 512 KB) |
| 크래시 복구 | ✓ Redo/Undo | ✓ Redo + MVCC 정리 | ✓ Redo + Undo tablespace | ✓ WAL Replay + Undo log |

---

## 2. 데이터 타입

| 타입 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| 정수 | TINYINT·SMALLINT·INT·BIGINT·UNSIGNED | SMALLINT·INT·BIGINT | NUMBER(p) / INTEGER | INT·BIGINT·SMALLINT·TINYINT |
| 실수 | FLOAT·DOUBLE·DECIMAL(p,s) | REAL·DOUBLE·NUMERIC(p,s) | NUMBER(p,s) / BINARY_FLOAT / BINARY_DOUBLE | FLOAT·DOUBLE·DECIMAL(p,s) |
| 문자열 | CHAR·VARCHAR(n)·TEXT·LONGTEXT | CHAR·VARCHAR(n)·TEXT | CHAR·VARCHAR2(n)·NVARCHAR2·CLOB | VARCHAR(n)·TEXT |
| 이진 | BINARY·VARBINARY·BLOB·LONGBLOB | BYTEA | BLOB·RAW | BLOB (hex 문자열 저장, `0x...` 리터럴 지원) |
| 논리 | TINYINT(1) / BOOL | BOOLEAN | ✗ (NUMBER(1) 관례) | BOOLEAN |
| 날짜/시간 | DATE·TIME·DATETIME·TIMESTAMP·YEAR | DATE·TIME·TIMESTAMP·TIMESTAMPTZ·INTERVAL | DATE·TIMESTAMP·INTERVAL·TIMESTAMP WITH TIME ZONE | DATE·TIME·DATETIME·TIMESTAMP·YEAR |
| 시간대 포함 | ✗ | TIMESTAMPTZ | TIMESTAMP WITH TIME ZONE | ✗ |
| JSON | JSON (텍스트) | JSON·JSONB (바이너리 인덱스 가능) | JSON (21c+, 네이티브 바이너리 저장) | ✓ (`->` / `->>` 연산자, JSON_EXTRACT / JSON_UNQUOTE / JSON_VALUE) |
| 배열 | ✗ | 모든 타입 배열 가능 | NESTED TABLE / VARRAY | ✗ |
| UUID | ✗ (VARCHAR로 저장) | UUID | ✗ (VARCHAR로 저장) | ✗ |
| 범위 타입 | ✗ | int4range·tsrange 등 | ✗ | ✗ |
| 네트워크 주소 | ✗ | INET·CIDR·MACADDR | ✗ | ✗ |
| ENUM | ✓ (컬럼 단위 정의) | ✓ (타입으로 별도 생성) | ✗ (CHECK 제약으로 에뮬레이션) | ✓ |
| SET | ✓ | ✗ | ✗ | ✓ |
| 지리 공간 | GEOMETRY·POINT·POLYGON 등 | PostGIS 확장 | Oracle Spatial (SDO_GEOMETRY) | ✗ |

---

## 3. 트랜잭션 / ACID

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| 원자성 (A) | ✓ | ✓ | ✓ | ✓ (Undo log + WAL) |
| 일관성 (C) | ✓ | ✓ | ✓ | ✓ (PK/FK/UNIQUE/CHECK 검증) |
| 격리성 (I) | ✓ | ✓ | ✓ | ✓ (Deferred Write: DML → session_tables 버퍼, COMMIT 시 반영) |
| 지속성 (D) | ✓ | ✓ | ✓ | ✓ (WAL fsync + 그룹 커밋) |
| MVCC | ✓ (InnoDB Undo segment) | ✓ (튜플 버전 힙 내 저장) | ✓ (Undo tablespace 기반 Consistent Read) | △ (스냅샷 기반, 완전 다중버전 아님) |
| 격리 수준 | 4가지 (기본: REPEATABLE READ) | 3가지 유효 (기본: READ COMMITTED) | 2가지 유효 — READ COMMITTED / SERIALIZABLE (기본: READ COMMITTED) | 4가지 (기본: READ COMMITTED) |
| Serializable 구현 | 잠금 기반 + GAP Lock (팬텀 방지) | SSI (Serializable Snapshot Isolation) | 스냅샷 기반 (ORA-08177 직렬화 오류 반환) | 잠금 기반 (완전 SSI 아님) |
| SAVEPOINT | ✓ | ✓ | ✓ | ✓ (session_tables 기반, ROLLBACK TO 정상 동작) |
| XA (분산 트랜잭션) | ✓ | ✓ | ✓ | ✗ |
| 그룹 커밋 | ✓ (binlog 그룹 커밋) | ✓ (WAL writer 통합) | ✓ (LGWR 배치 플러시) | ✓ (GroupCommitCoordinator) |
| 데드락 감지 | ✓ (wait-for 그래프) | ✓ (wait-for 그래프) | ✓ (wait-for 그래프) | ✓ (DFS 사이클 탐지) |
| VACUUM / 공간 회수 | 자동 Purge (InnoDB) | AUTOVACUUM | 자동 Undo 관리 (AUM) | ✓ (수동 VACUUM + DML 200회마다 AUTO VACUUM) |

---

## 4. 잠금

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| 행 레벨 잠금 | ✓ | ✓ | ✓ | ✓ |
| 테이블 잠금 | ✓ (LOCK TABLES) | ✓ (LOCK TABLE) | ✓ (LOCK TABLE) | ✗ |
| 갭 잠금 (Gap Lock) | ✓ (REPEATABLE READ 이상) | ✗ (SSI로 처리) | ✗ (MVCC로 처리) | ✗ |
| SELECT FOR UPDATE | ✓ | ✓ | ✓ | ✓ |
| SELECT FOR SHARE | ✓ | ✓ | △ (FOR UPDATE SKIP LOCKED로 유사 처리) | ✓ (공유 잠금, 다중 독자 허용) |
| Advisory Lock | ✗ | ✓ (pg_advisory_lock) | ✗ | ✗ |

---

## 5. 인덱스

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| B+Tree | ✓ | ✓ | ✓ | ✓ |
| 비트맵 인덱스 | ✗ | ✗ | ✓ (저카디널리티 컬럼에 효율적) | ✗ |
| 해시 인덱스 | ✓ (Memory 엔진) | ✓ | ✗ | ✓ (`USING HASH`, 등호 O(1)) |
| 전문 검색 (Fulltext) | ✓ | ✓ (GIN) | ✓ (Oracle Text) | ✗ |
| 공간 인덱스 | ✓ (R-Tree) | ✓ (GiST) | ✓ (Spatial Index) | ✗ |
| 부분 인덱스 | ✗ | ✓ (WHERE 조건 인덱스) | ✓ (함수 기반 인덱스로 에뮬레이션) | ✗ |
| 표현식 인덱스 | ✓ (함수 기반) | ✓ | ✓ (Function-Based Index) | ✗ |
| 커버링 인덱스 | ✓ | ✓ | ✓ | ✓ (EXPLAIN에서 Covering 표시) |
| 복합 인덱스 | ✓ | ✓ | ✓ | ✓ |
| 보조 인덱스 증분 갱신 | ✓ (InnoDB 자동) | ✓ | ✓ | ✓ (INSERT/UPDATE/DELETE 시 `index_insert_row` / `index_remove_row`로 O(1) 갱신 — 전체 재빌드 없음) |
| 내림차순 인덱스 | ✓ (8.0+) | ✓ | ✓ | ✗ |
| BRIN (블록 범위 인덱스) | ✗ | ✓ | ✗ | ✗ |
| GIN / GiST | ✗ | ✓ | ✗ | ✗ |

---

## 6. 쿼리 기능

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| SELECT / WHERE / ORDER BY | ✓ | ✓ | ✓ | ✓ |
| GROUP BY / HAVING | ✓ | ✓ | ✓ | ✓ |
| LIMIT / OFFSET | ✓ | ✓ | △ (FETCH FIRST n ROWS ONLY) | ✓ |
| FETCH FIRST n ROWS ONLY | ✗ | ✓ | ✓ (SQL:2003 표준) | ✓ (LIMIT 동의어) |
| DISTINCT | ✓ | ✓ | ✓ | ✓ |
| INNER / LEFT / RIGHT JOIN | ✓ | ✓ | ✓ | ✓ |
| FULL OUTER JOIN | ✗ (UNION으로 에뮬레이션) | ✓ | ✓ | ✓ |
| CROSS JOIN | ✓ | ✓ | ✓ | ✓ |
| NATURAL JOIN | ✓ | ✓ | ✓ | ✓ |
| JOIN ... USING | ✓ | ✓ | ✓ | ✓ |
| SELF JOIN | ✓ | ✓ | ✓ | ✓ |
| LATERAL JOIN | ✓ (8.0.14+) | ✓ | ✓ (CROSS APPLY / OUTER APPLY) | ✗ |
| UNION / UNION ALL | ✓ | ✓ | ✓ | ✓ |
| INTERSECT / EXCEPT | ✓ (8.0+) | ✓ | ✓ (MINUS) | ✓ |
| 서브쿼리 (FROM / WHERE / SELECT) | ✓ | ✓ | ✓ | ✓ |
| 상관 서브쿼리 | ✓ | ✓ | ✓ | ✓ |
| EXISTS / NOT EXISTS | ✓ | ✓ | ✓ | ✓ |
| IN (서브쿼리) | ✓ | ✓ | ✓ | ✓ |
| CTE (WITH) | ✓ | ✓ | ✓ | ✓ |
| 재귀 CTE (WITH RECURSIVE) | ✓ | ✓ | ✓ (CONNECT BY로도 가능) | ✓ |
| CASE WHEN | ✓ | ✓ | ✓ | ✓ |
| EXPLAIN / EXPLAIN ANALYZE | ✓ | ✓ | ✓ (EXPLAIN PLAN FOR + DBMS_XPLAN) | ✓ |

---

## 7. 집계 함수

| 함수 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| COUNT / SUM / AVG / MIN / MAX | ✓ | ✓ | ✓ | ✓ |
| GROUP_CONCAT / STRING_AGG / LISTAGG | GROUP_CONCAT | STRING_AGG | LISTAGG | GROUP_CONCAT |
| STDDEV / VARIANCE | ✓ | ✓ | ✓ | ✓ (STDDEV_POP / VAR_POP 별칭 포함) |
| BIT_AND / BIT_OR | ✓ | ✓ | ✗ | ✗ |
| ARRAY_AGG / COLLECT | ✗ | ARRAY_AGG | COLLECT | ✗ |
| JSON_AGG | ✓ | ✓ | ✓ (JSON_ARRAYAGG) | ✗ |
| PERCENTILE_CONT / DISC | ✗ | ✓ | ✓ | ✗ |
| FILTER (WHERE ...) 절 | ✗ | ✓ | ✗ | ✗ |
| DISTINCT 집계 | ✓ | ✓ | ✓ | ✓ (COUNT / SUM / AVG DISTINCT 모두 지원) |

---

## 8. 윈도우 함수

| 함수 | MySQL 8.0+ | PostgreSQL | Oracle | RuSQL |
|------|------------|------------|--------|--------|
| ROW_NUMBER / RANK / DENSE_RANK | ✓ | ✓ | ✓ | ✓ |
| LAG / LEAD | ✓ | ✓ | ✓ | ✓ |
| FIRST_VALUE / LAST_VALUE | ✓ | ✓ | ✓ | ✓ |
| NTH_VALUE | ✓ | ✓ | ✓ | ✓ |
| NTILE | ✓ | ✓ | ✓ | ✓ |
| CUME_DIST / PERCENT_RANK | ✓ | ✓ | ✓ | ✓ |
| ROWS / RANGE 프레임 절 | ✓ | ✓ | ✓ | ✓ (`ROWS/RANGE BETWEEN ... AND ...`) |
| GROUPS 프레임 | ✗ | ✓ | ✗ | ✗ |
| 집계 함수를 윈도우로 사용 | ✓ | ✓ | ✓ | ✓ (SUM/AVG/COUNT/MIN/MAX OVER) |
| PARTITION BY / ORDER BY | ✓ | ✓ | ✓ | ✓ |

---

## 9. 스칼라 함수

| 범주 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| 문자열 | 50+ (CONCAT, SUBSTR, TRIM, REPLACE, LPAD, RPAD, REGEXP...) | 50+ | 50+ (SUBSTR, INSTR, LPAD, RPAD, REPLACE, INITCAP...) | UPPER, LOWER, LENGTH, TRIM, CONCAT, SUBSTR, REPLACE, LPAD, RPAD, CHAR_LENGTH, LEFT, RIGHT, REVERSE, REPEAT, INSTR, LOCATE, LTRIM, RTRIM, SPACE, ASCII, HEX, FORMAT |
| 날짜/시간 | 40+ | 40+ | 40+ (ADD_MONTHS, TRUNC, MONTHS_BETWEEN, NEXT_DAY...) | NOW, CURDATE, DATE_FORMAT, DATE_ADD, DATE_SUB, DATEDIFF, YEAR, MONTH, DAY, DAYOFWEEK, DAYOFYEAR, WEEKDAY, LAST_DAY, TIMESTAMPDIFF, UNIX_TIMESTAMP, FROM_UNIXTIME |
| 수학 | 20+ | 20+ | 20+ | ROUND, ABS, CEIL, FLOOR, MOD, SQRT, POW, LOG, LOG2, LOG10, EXP, SIN, COS, TAN, PI, SIGN, TRUNCATE, RAND |
| 조건부 | COALESCE, IFNULL, NULLIF, IF, CASE | COALESCE, NULLIF, CASE, GREATEST, LEAST | COALESCE, NVL, NVL2, DECODE, NULLIF, CASE | COALESCE, IFNULL, NULLIF, IF, CAST, GREATEST, LEAST |
| 정규식 | REGEXP, REGEXP_REPLACE (8.0+) | REGEXP_MATCH, REGEXP_REPLACE | REGEXP_LIKE, REGEXP_REPLACE, REGEXP_SUBSTR | ✓ (REGEXP/RLIKE 연산자, REGEXP_LIKE/REGEXP_REPLACE/REGEXP_MATCH) |
| 타입 변환 | CAST, CONVERT | CAST, :: 연산자 | CAST, TO_CHAR, TO_NUMBER, TO_DATE | CAST, CONVERT |
| 기타 | UUID, MD5 | UUID, MD5 | SYS_GUID (UUID 유사), DBMS_CRYPTO (해시) | UUID, MD5 |
| 사용자 정의 함수 | ✓ (CREATE FUNCTION) | ✓ (PL/pgSQL, PL/Python 등) | ✓ (PL/SQL FUNCTION) | ✓ (CREATE FUNCTION name(params) RETURNS type RETURN expr) |

---

## 10. DDL

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| CREATE / DROP TABLE | ✓ | ✓ | ✓ | ✓ |
| ALTER TABLE (ADD/DROP/MODIFY/RENAME COLUMN) | ✓ | ✓ | ✓ | ✓ |
| TRUNCATE | ✓ | ✓ | ✓ | ✓ |
| CREATE / DROP INDEX | ✓ | ✓ | ✓ | ✓ |
| CREATE / DROP VIEW | ✓ | ✓ | ✓ | ✓ |
| CREATE SYNONYM | ✗ | ✗ | ✓ | ✓ (CREATE [OR REPLACE] SYNONYM / DROP SYNONYM / SHOW SYNONYMS) |
| CREATE / DROP DATABASE | ✓ | ✓ | △ (스키마/유저 단위 관리) | ✓ |
| CREATE SEQUENCE | ✗ | ✓ | ✓ | ✗ (AUTO_INCREMENT만) |
| CREATE TYPE | ✗ | ✓ (도메인/열거형/복합) | ✓ (객체 타입, NESTED TABLE) | ✗ |
| ALTER TABLE ADD CONSTRAINT | ✓ | ✓ | ✓ | ✓ (FK/UNIQUE/CHECK) |
| 온라인 DDL | ✓ (대부분 non-blocking) | ✓ (일부 non-blocking) | ✓ (Online Redefinition) | ✗ |
| 파티셔닝 | ✓ (RANGE/LIST/HASH/KEY) | ✓ (선언적 파티셔닝) | ✓ (RANGE/LIST/HASH/COMPOSITE) | ✗ |

---

## 11. 제약조건

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| PRIMARY KEY | ✓ (단일/복합) | ✓ | ✓ | ✓ (단일/복합) |
| FOREIGN KEY + ON DELETE/UPDATE | ✓ | ✓ | ✓ (ON DELETE만 지원) | ✓ |
| UNIQUE | ✓ | ✓ | ✓ | ✓ |
| NOT NULL | ✓ | ✓ | ✓ | ✓ |
| CHECK | ✓ (8.0.16+) | ✓ | ✓ | ✓ |
| DEFAULT | ✓ | ✓ | ✓ | ✓ |
| DEFERRABLE 제약 | ✗ | ✓ | ✓ | ✗ |
| EXCLUDE 제약 | ✗ | ✓ (GiST 인덱스 기반) | ✗ | ✗ |

---

## 12. DML

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| INSERT VALUES | ✓ | ✓ | ✓ | ✓ |
| INSERT SELECT | ✓ | ✓ | ✓ | ✓ |
| INSERT ... ON DUPLICATE KEY UPDATE | ✓ | ✗ | ✗ | ✓ |
| INSERT ... ON CONFLICT (UPSERT) | ✗ | ✓ | ✗ | ✓ (ABORT/IGNORE/UPDATE) |
| REPLACE INTO | ✓ | ✗ | ✗ | ✗ |
| UPDATE (단일 테이블) | ✓ | ✓ | ✓ | ✓ |
| UPDATE (다중 테이블 JOIN) | ✓ | ✗ (FROM 절로 에뮬레이션) | ✗ (서브쿼리로 에뮬레이션) | ✓ (MULTI UPDATE) |
| DELETE (단일 테이블) | ✓ | ✓ | ✓ | ✓ |
| DELETE (다중 테이블 JOIN) | ✓ | ✗ (USING으로 에뮬레이션) | ✗ | ✓ (MULTI DELETE) |
| RETURNING 절 | ✗ | ✓ | ✓ (RETURNING INTO 변수) | ✓ (INSERT/UPDATE/DELETE) |
| MERGE / UPSERT 표준 | ✓ (8.0.31+) | ✓ (15+) | ✓ (SQL:2003 MERGE 표준) | ✓ (MERGE INTO ... USING ... ON ...) |

---

## 13. 쿼리 최적화 / 실행 계획

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| 비용 기반 최적화 (CBO) | ✓ | ✓ | ✓ | ✓ |
| 통계 기반 선택도 | ✓ (histogram) | ✓ (pg_statistic) | ✓ (DBMS_STATS) | ✓ (ANALYZE TABLE — equi-depth 히스토그램 10-bucket, PkRange·PkBetween·SecondaryRange·SecondaryBetween selectivity 추정) |
| 인덱스 자동 선택 | ✓ | ✓ | ✓ | ✓ |
| JOIN 알고리즘 | NestedLoop, Hash, BNL | NestedLoop, Hash, MergeJoin | NestedLoop, Hash, SortMerge | NestedLoop, Hash, SortMerge |
| JOIN 순서 최적화 | ✓ (동적 프로그래밍) | ✓ (Geqo + 동적 프로그래밍) | ✓ (동적 프로그래밍) | ✓ (System-R bitmask DP, INNER 한정 · 그리디 폴백) |
| 병렬 쿼리 | △ (일부) | ✓ | ✓ (Parallel Query) | △ (SeqScan WHERE 필터 + GROUP BY 집계 + Hash Join probe — rayon, `RUSTDB_PARALLEL` 토글) |
| JIT 컴파일 | ✗ | ✓ (LLVM) | ✗ (Native Compilation은 별도 옵션) | ✗ |
| EXPLAIN 출력 | ✓ | ✓ (VERBOSE, BUFFERS, FORMAT JSON) | ✓ (EXPLAIN PLAN + DBMS_XPLAN.DISPLAY) | ✓ (접근 경로·비용·실제 행 수, 74자 포맷) |
| 쿼리 힌트 | ✓ (USE INDEX, STRAIGHT_JOIN) | ✓ (pg_hint_plan 확장) | ✓ (/*+ INDEX(t idx) */ 등 풍부한 힌트) | ✗ |

---

## 14. 보안 / 사용자 관리

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| 사용자 모델 | user@host | 역할 (Role) | 유저 = 스키마 | user@host |
| GRANT / REVOKE | ✓ | ✓ | ✓ | ✓ |
| 역할 (ROLE) | ✓ (8.0+) | ✓ | ✓ | ✓ (CREATE/DROP/GRANT/REVOKE/SHOW ROLES, WITH ADMIN OPTION) |
| 열 레벨 권한 | ✓ | ✓ | ✓ | ✗ |
| 행 레벨 보안 (RLS) | ✗ | ✓ | ✓ (VPD: Virtual Private Database) | ✗ |
| 인증 방식 | Password, PAM, LDAP, Kerberos | Password, scram-sha-256, LDAP, GSSAPI | Password, Kerberos, LDAP, OS 인증 | Native TCP: SHA-256 해시 비교 · MySQL 프로토콜: mysql_native_password (SHA1 챌린지-응답, SHA1(SHA1(pw)) 저장) |
| TLS / SSL | ✓ | ✓ | ✓ | ✗ |
| 감사 로그 | ✓ (엔터프라이즈) | ✓ (pg_audit 확장) | ✓ (Unified Auditing, 기본 내장) | ✗ |

---

## 15. 고가용성 / 분산

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| 복제 | 비동기·반동기·동기 | 스트리밍·논리 복제 | Data Guard (동기/비동기) | ✗ |
| 자동 장애 복구 | InnoDB Cluster / MHA | Patroni / repmgr | Data Guard + Observer | ✗ |
| 샤딩 | NDB Cluster / Vitess | Citus / Crunchy | Oracle Sharding | ✗ |
| 연결 풀링 | ProxySQL | PgBouncer | DRCP (DB Resident Connection Pool) | ✗ |
| 읽기 복제본 | ✓ | ✓ | ✓ (Active Data Guard) | ✗ |
| RAC (다중 인스턴스) | ✗ | ✗ | ✓ (Oracle RAC) | ✗ |

---

## 16. 관리 / 운영

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| SHOW TABLES / DESCRIBE | ✓ | 유사 (\\d, information_schema) | 유사 (ALL_TABLES, DESC) | ✓ |
| INFORMATION_SCHEMA | ✓ (전체) | ✓ (전체) | ✓ (ALL_*/DBA_*/USER_* 딕셔너리 뷰) | ✓ (10개 가상 뷰) |
| SHOW DATABASES | ✓ | ✓ (\\l) | △ (SELECT FROM V$DATABASE) | ✓ |
| SHOW CREATE TABLE | ✓ | ✓ (pg_dump) | ✓ (DBMS_METADATA) | ✓ |
| SHOW PROCESSLIST | ✓ | pg_stat_activity | V$SESSION | ✓ (현재 세션 정보) |
| SHOW LOCKS | ✓ | pg_locks | V$LOCK | ✓ |
| CHECKPOINT | ✓ | ✓ | ✓ (ALTER SYSTEM CHECKPOINT) | ✓ |
| VACUUM | ✓ (자동 Purge) | ✓ (AUTOVACUUM) | ✓ (자동 Undo 관리) | ✓ (수동 + DML 200회 AUTO) |
| 백업 / 덤프 | mysqldump, XtraBackup | pg_dump, pg_basebackup | RMAN (Recovery Manager) | ✓ (BACKUP [DATABASE db] [INTO 'file']) |
| 저장 프로시저 | ✓ | ✓ (PL/pgSQL 등) | ✓ (PL/SQL) | ✓ (CREATE/DROP PROCEDURE, CALL, IF/WHILE/LOOP/REPEAT, UI 직접 실행) |
| 트리거 | ✓ | ✓ | ✓ | ✓ (BEFORE/AFTER INSERT/UPDATE/DELETE) |
| 이벤트 스케줄러 | ✓ | pg_cron 확장 | ✓ (DBMS_SCHEDULER) | ✗ |

---

## 17. 클라이언트 / 네트워크 프로토콜

| 항목 | MySQL | PostgreSQL | Oracle | RuSQL |
|------|-------|------------|--------|--------|
| 공식 프로토콜 | MySQL Protocol (포트 3306) | PostgreSQL wire protocol (포트 5432) | Oracle SQL*Net / TNS (포트 1521) | 텍스트 기반 TCP (포트 7878, ---END--- 구분자) + MySQL wire protocol (포트 3306) |
| MySQL wire protocol 호환 | ✓ | ✗ | ✗ | ✓ (포트 3306, mysql_native_password 인증 구현, mysql CLI / DBeaver / mysql-connector-python 완전 연동 — SHOW VARIABLES/COLLATION/FULL TABLES/FULL COLUMNS 등 자동 쿼리 처리) |
| 드라이버 호환 | JDBC, ODBC, Python, Go, Node.js | JDBC, ODBC, libpq, Python, Go, Node.js | JDBC (ojdbc), ODBC, OCI, Python (cx_Oracle) | rusql-client (전용 CLI, -u/-p/-h/-P 플래그) + MySQL 호환 클라이언트 |
| 커넥션 풀 지원 | ✓ | ✓ | ✓ (DRCP) | ✗ |
| Prepared Statements | ✓ | ✓ | ✓ | ✓ (PREPARE/EXECUTE/DEALLOCATE USING @var) |
| 배치 실행 | ✓ | ✓ | ✓ | ✗ (멀티쿼리 ; 분리) |
| WebSocket / HTTP API | ✗ | ✗ (PostgREST 등) | ✓ (ORDS: Oracle REST Data Services) | ✗ |

---

## 18. UI / 관리 도구

| 항목 | MySQL Workbench | pgAdmin / DBeaver | Oracle SQL Developer | RuSQL UI |
|------|-----------------|-------------------|----------------------|-----------|
| 쿼리 에디터 | ✓ (구문 강조, 자동완성) | ✓ | ✓ | ✓ (Monaco 기반, SQL 구문 강조, BEGIN...END 블록 인식, Ctrl+Enter 실행, Ctrl+Shift+F 포맷, MySQL 스타일 툴바 — SQL 파일 열기/저장/번개 실행, 패널 토글 버튼, 분할 에디터 — 탭 왼쪽 바 이동·복원) |
| ERD 다이어그램 | ✓ | ✓ | ✓ | ✓ (FK 관계선, 팬/줌, 카드 드래그, FK 기반 Auto Layout) |
| 사이드바 컨텍스트 메뉴 | ✓ (DB/테이블/뷰/인덱스 우클릭) | ✓ | ✓ | ✓ (DB/테이블/뷰/인덱스 우클릭, Edit Table 모달, Select/Describe/Drop 등) |
| 결과 테이블 | ✓ (셀 편집 가능) | ✓ | ✓ (셀 편집 가능) | ✓ (Canvas measureText 기반 컬럼 자동 너비, 헤더 정렬 아이콘 포함 너비 계산, 컬럼 너비 조절, 100행 페이징, 헤더 정렬, 행 번호(좌측 정렬 40px), 실시간 검색, 셀 편집, 실행 진행 바) |
| 히스토리 | ✓ | ✓ | ✓ | ✓ (200개 보존, 연결별 분리) |
| 다중 탭 | ✓ | ✓ | ✓ | ✓ (탭별 결과·에디터 상태 유지, 우클릭 컨텍스트 메뉴 — 닫기/분할/고정/이름 변경, 탭 고정 📌, 분할 시 왼쪽 탭바에서 제거·닫으면 복원) |
| 연결 관리 | ✓ | ✓ | ✓ | ✓ (연결별 독립 데이터 디렉토리) |
| 서버 모니터링 | ✓ | ✓ | ✓ (Performance Hub) | ✓ (TCP 서버 on/off·클라이언트 수·로그, 접속 세션 실시간 모니터링 패널 — addr·user·경과 시간·쿼리 건수, 벤치마크 결과 UI 패널) |
| 데이터 임포트/익스포트 | ✓ (CSV, SQL) | ✓ | ✓ (CSV, Excel, XML) | ✓ (CSV 익스포트·임포트) |
| AI 연동 | △ (HeatWave AutoML) | ✗ | △ (Oracle AI) | ✓ (True MCP — mcp_server.py: Claude Desktop 연동, stdio JSON-RPC, 4개 도구 (execute_sql · list_databases · list_tables · get_table_schema), API 키 불필요, UI 자동 연결 버튼) |
