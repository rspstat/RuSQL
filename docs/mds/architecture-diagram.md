┌────────────────────────────────────────────────────────────────────┐
│                      RuSQL 데이터베이스 엔진                        │
│                                                                    │
│   Lexer  ──►  Parser  ──►  AST                                     │
│                               │                                    │
│                               ▼                                    │
│               Query Planner (비용 기반)                            │
│          AccessPath / JoinAlgo(비용 기반) / Cost Est. / Top-K      │
│          Join Order DP / 히스토그램 selectivity / IndexIntersection │
│                               │                                    │
└───────────────────────────────┼────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                           Executor                                               │
│                                                                                                  │
│  DDL                    DML                    DCL                    TCL                        │
│  ──────────────────     ────────────────────   ────────────────────   ──────────────────────     │
│  CREATE / DROP          INSERT                 CREATE / DROP USER     BEGIN / COMMIT             │
│  ALTER / TRUNCATE         VALUES / SELECT      GRANT / REVOKE         ROLLBACK                  │
│  USE                      IGNORE               SHOW GRANTS            SAVEPOINT                  │
│                           ON DUPLICATE         CREATE / DROP ROLE     ROLLBACK TO               │
│  대상                       KEY UPDATE         GRANT / REVOKE ROLE    RELEASE SAVEPOINT         │
│  TABLE / DATABASE         ON CONFLICT          SHOW ROLES             SET ISOLATION LEVEL       │
│  INDEX (B+Tree / HASH)      (ABORT/IGNORE/     CREATE / DROP          SHOW ISOLATION LEVEL      │
│  VIEW / SYNONYM             UPDATE)            SYNONYM                                           │
│  PARTITION (RANGE/LIST/                                                                          │
│    HASH), ADD/DROP PARTITION                                                                     │
│  PROCEDURE / TRIGGER      RETURNING            SHOW SYNONYMS                                     │
│  FUNCTION               UPDATE                                                                   │
│                           단일 / 다중          Join 연산              모니터링                   │
│  ALTER 세부               RETURNING            ────────────────────   ──────────────────────     │
│  ADD / DROP / MODIFY    DELETE                 INNER / LEFT /         SHOW TABLES / DESCRIBE    │
│  RENAME COLUMN / TABLE    단일 / 다중          RIGHT / FULL OUTER     SHOW CREATE TABLE/VIEW    │
│  ADD / DROP CONSTRAINT    RETURNING            CROSS / NATURAL        SHOW INDEX                │
│  (FK / UNIQUE / CHECK)  MERGE INTO             SELF JOIN              SHOW DATABASES            │
│                           MATCHED UPDATE       JOIN ... USING         SHOW PROCESSLIST          │
│                           MATCHED DELETE       Hash Join              SHOW BUFFER POOL / WAL    │
│                           NOT MATCHED INSERT   Sort-Merge Join        SHOW LOCKS / GRANTS       │
│                                                Nested Loop Join       SHOW ROLES / SYNONYMS     │
│                                                ReverseIndexNL Join                                │
│                                                LATERAL JOIN (INNER/                              │
│                                                  LEFT/CROSS, per-row 상관 서브쿼리)              │
│                                                                       CHECKPOINT / VACUUM        │
│                                                                       ANALYZE TABLE              │
│                                                                       BACKUP DATABASE            │
│                                                                                                  │
│  집계 함수              윈도우 함수              저장 프로시저 제어문                             │
│  ──────────────────     ─────────────────────   ──────────────────────────────────              │
│  COUNT / COUNT(DISTINCT)  ROW_NUMBER             DECLARE / SET                                  │
│  SUM / AVG / MIN / MAX    RANK / DENSE_RANK      IF / ELSEIF / ELSE                             │
│  SUM(DISTINCT)            LAG / LEAD             WHILE / LOOP / REPEAT                          │
│  AVG(DISTINCT)            FIRST / LAST VALUE     LEAVE / ITERATE                                │
│  STDDEV / VARIANCE        NTH_VALUE / NTILE                                                     │
│  GROUP_CONCAT(SEPARATOR)  PERCENT_RANK / CUME_DIST                                              │
│  BIT_AND / BIT_OR         OVER (PARTITION BY / ORDER BY)                                        │
│  JSON_AGG                 ROWS / RANGE BETWEEN / 집계 윈도우 함수                               │
│  FILTER (WHERE ...)                                                                              │
│                                                                                                  │
│  쿼리 기능              고급 쿼리                                                               │
│  ──────────────────     ──────────────────────────────────────────────────────────              │
│  SELECT / DISTINCT      서브쿼리 (FROM절 / WHERE절 / SELECT 스칼라 / 상관)                      │
│  WHERE                  CTE (WITH ... AS) / 재귀 CTE (WITH RECURSIVE,                          │
│                            semi-naive 평가 — 안전 조건 충족 시 직전 반복분만 재조인)             │
│    =/!=/<>/>/</>=/<= )  UNION / UNION ALL / INTERSECT / EXCEPT                                  │
│    AND / OR / NOT       Updatable VIEW                                                          │
│    IN / NOT IN          INFORMATION_SCHEMA (가상 뷰 10개)                                       │
│    BETWEEN / NOT BETWEEN  PREPARE / EXECUTE / DEALLOCATE                                        │
│    LIKE / NOT LIKE      SET @var / 사용자 변수                                                  │
│    IS NULL / IS NOT NULL  병렬 쿼리 (rayon, 10k행+ 자동 적용)                                   │
│    EXISTS / NOT EXISTS  쿼리 결과 캐시 (LRU-512, DML 자동 무효화)                               │
│    REGEXP / NOT REGEXP  MVCC 가시성 필터 / SELECT FOR UPDATE / FOR SHARE                        │
│    TRUE / FALSE 리터럴  FK ON DELETE / ON UPDATE 액션 처리                                      │
│  ORDER BY (ASC / DESC)  PK / FK / UNIQUE / CHECK 제약 검증                                      │
│  GROUP BY / HAVING      산술 표현식 / `%` 모듈로 / `\|\|` 연결                                  │
│  LIMIT n / LIMIT m,n    스칼라 함수 (문자열/날짜/수학/NULL)                                     │
│  FETCH FIRST n ROWS ONLY  EXPLAIN / EXPLAIN ANALYZE                                             │
│  CASE WHEN              BACKUP DATABASE / RESTORE FROM                                           │
│                                                                                                  │
└──────────────────────────────────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────────────────────────────────┐
│                              엔진 고도화 & 최적화                                                │
│                                                                                                  │
│  인덱스                                        트랜잭션 & 동시성                                 │
│  ────────────────────────────────────────      ──────────────────────────────────────────────   │
│  B+Tree (단일 / 복합 / 클러스터드, ORDER=16)    WAL 바이너리 redo log                            │
│  Hash Index (USING HASH, 등호 O(1))             WAL fsync per-commit                            │
│  커버링 인덱스 (Index-only scan)                WAL Group Commit (leader/follower, 단일 fsync)   │
│  보조 인덱스 중복 키 배열 저장                   Undo Log 디스크 영속화 (_undo.log)               │
│  (테이블/DB 간 인덱스 이름 재사용 안전)           WAL/Undo 레코드 체크섬 (FNV-1a, 손상 감지)      │
│  증분 보조 인덱스 갱신 O(1) per row              Crash Recovery (WAL Replay)                    │
│    (전체 재빌드 없음, DML마다 즉시 반영)          Checkpoint (자동 512KB / 수동)                 │
│  B+Tree 범위 스캔 가지치기                       진짜 행 단위 MVCC (_xmin/_xmax 버전 체인)        │
│    scan_from / scan_to O(log N + k)              UPDATE도 신규 버전 append(제자리 수정 아님)     │
│                                                   GC 호라이즌 기반 VACUUM(열린 스냅샷 보호)       │
│  range_keys BETWEEN 삭제 최적화                  AUTO VACUUM (DML 200회 누적 임계값)             │
│    (역순 swap_remove, 인덱스 깨짐 없음)           행 단위 동시 쓰기(같은 테이블 다른 행도 병렬)  │
│  row_pk_pos 위치 인덱스                          Row-level Locking + Gap Lock (팬텀 방지)        │
│    DELETE WHERE PK = ? → O(1) swap_remove        SSI predicate lock (non-blocking, SIREAD 스타일 │
│    FK 피참조 테이블은 safe-path 폴백                — 잠금 없는 SELECT도 SERIALIZABLE 팬텀 감지) │
│                                                   SELECT FOR UPDATE / FOR SHARE                   │
│                                                   진짜 블로킹 대기 + 데드락 감지                 │
│                                                     (@lock_wait_timeout, wait-for 그래프)         │
│                                                   4단계 격리 수준 (RC ~ SERIALIZABLE)             │
│  Top-K 인덱스 조기 종료                          MVCC 가시성 (SnapshotCtx)                      │
│    ORDER BY + LIMIT 시 조건 충족 즉시 중단         (DML → s.tables 직접 기록,                   │
│    SecondaryRange / Between / LikePrefix /         격리수준별 가시성 필터 적용)                │
│    CompositeIndexPrefix 경로 지원                SAVEPOINT undo (Undo Log 기반)                  │
│  수치 인식 키 비교 ("10" > "9" 정상 처리)         세션별 독립 Executor                           │
│                                            (shared_ptr<RwLock<SharedDatabase>> 공유)            │
│  쿼리 실행 최적화                                                                               │
│  ────────────────────────────────────────      영속화                                           │
│  비용 기반 AccessPath 자동 선택                  ──────────────────────────────────────────────  │
│    Hash Index 등호 조건 우선 선택                바이너리 .rdb + LZ4 압축                        │
│    Index Intersection (AND 다중 인덱스            B+Tree .idx 자동 저장                           │
│      PK HashSet 교집합, ∩ EXPLAIN 표시)          인덱스 메타 (indexes.json)                      │
│  Join 순서 최적화                                스키마 (auto_increment 포함)                    │
│    System-R bitmask DP (N≤8)                    뷰 (views.json)                                │
│    그리디 폴백 (N>8 / OUTER JOIN 포함)           저장 프로시저 (_procedures.json)               │
│  Join 알고리즘 비용 기반 선택                    트리거 (_triggers.json)                        │
│    NL vs Hash vs ReverseIndexNL(작은 쪽         UDF (_functions.json)                          │
│    기준 반대 방향 인덱스 프로브) 비용 비교                                                       │
│  비상관 서브쿼리 HashSet 머티리얼라이제이션       사용자 / 권한 (_users / _grants.json)           │
│    (최초 1회 실행 후 O(1) 조회)                  역할 (_roles / _role_grants.json)              │
│  히스토그램 + MCV(최빈값) selectivity 추정        동의어 (_synonyms.json)                        │
│    ANALYZE TABLE → equi-depth 10-bucket          전역 파일 → data/_system/ 분리                 │
│    + 최빈값 10개 정확 카운트(쏠린 컬럼 보정)                                                      │
│    Auto-ANALYZE: 크기 단계별 임계값              구버전 루트 경로 → _system/ 자동 마이그레이션  │
│  병렬 쿼리 실행 (rayon)                                                                          │
│    SeqScan WHERE 필터 par_chunks                 인증 & 보안                                    │
│    GROUP BY 부분 집계 par_chunks → 병합           ──────────────────────────────────────────────  │
│    ORDER BY par_sort_unstable_by                 Native TCP + MySQL 프로토콜 둘 다 동일한        │
│    Hash Join probe par_iter                      mysql_native_password 방식 챌린지-응답          │
│    적응형 임계값 (10k/thread_count, 최소 1k)       (SHA1(SHA1(pw)), 비밀번호 평문 전송 없음)      │
│    SET @rusql_parallel / RUSTDB_PARALLEL 제어    COM_CHANGE_USER / COM_RESET_CONNECTION          │
│  쿼리 결과 캐시 (LRU-512, O(k) 무효화)             (커넥션 풀 드라이버 지원, 예: HikariCP)        │
│    트랜잭션 외부 SELECT 전용                      root/root 기본 계정 자동 생성                  │
│    DML 시 참조 테이블 항목 즉시 무효화                                                           │
│    COMMIT 시 변경 테이블 자동 무효화                                                             │
│    서브쿼리 포함 SELECT 캐싱 스킵                                                                │
│  Buffer Pool (LRU, O(1) 히트)                                                                   │
│    기본 64p × 16KB / --buffer-pool-size 조정                                                    │
│    dirty 페이지 flush / 캐시 히트율 추적                                                        │
│    SELECT는 s.tables 직접 읽기로 우회                                                           │
│  Lock 타임아웃 (SET @lock_wait_timeout, ms)                                                     │
│  TRUNCATE 후 AUTO INCREMENT 리셋                                                                 │
│                                                                                                  │
└──────────────────────────────────────────────────────────────────────────────────────────────────┘

> WAL/Undo Log(`rusql.wal`, `_undo.log`)는 같은 data_dir을 쓰는 모든 세션이 공유하는 파일이지만,
> 모든 레코드가 세션 간 공유 전역 카운터(`TxnIoShared`)로 발급되는 유일한 `txn_id`로 태깅되어
> COMMIT/ROLLBACK/ABORT가 자기 트랜잭션 레코드만 제거한다 (다른 세션의 진행 중인 트랜잭션을
> 파괴하지 않음). 체크포인트는 다른 세션에 활성 트랜잭션이 있으면 연기되고, 크래시 복구는
> txn_id 그룹 단위로 커밋된 트랜잭션만 redo하고 미완료 트랜잭션은 undo한다. 자세한 내용은
> FUNCTIONS.md의 "트랜잭션" 절 참고.

                                │
                                ▼
                              실행