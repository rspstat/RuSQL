# RuSQL 개발 타임라인 (2026년 6월 ~ 8월)

> **용도**: 2학기 진행 상황이 1학기 대비 무엇이 달라졌는지 정리하기 위한 자료.
> **날짜 기준**: 실제 개발(로컬 세션)이 이뤄진 날짜를 기준으로 정리했고, 괄호 안에 해당 작업이 실제로 GitHub에 커밋된 커밋 해시/날짜를 함께 표기했습니다. 이 프로젝트는 세션당 바로 커밋하지 않고 여러 세션 분량을 모아서 한 번에 커밋하는 경우가 많아, "개발한 날짜"와 "커밋 날짜"가 최대 12일까지 차이 나는 구간이 있습니다(예: 7/22에 만든 MCP 수정이 8/2에 커밋됨). 두 날짜가 다른 곳은 명시했습니다.
> **작성 시점**: 2026년 8월 27일. 이 문서는 그 시점까지의 실제 작업만 담고 있으며(9월 항목은 아직 발생하지 않아 없음), 이후 진행되는 대로 이어서 채우면 됩니다.
> **범위**: 리포지토리 전체 커밋 기준으로는 2026년 3월부터 시작(Rust 버전), 요청 범위인 6~9월에 맞춰 6월 상황을 "1학기 말 스냅샷"으로 간단히 요약하고, 실질적인 2학기 개발 내용(7~8월, C++ 전면 재작성 이후)을 상세히 다룹니다.

---

## 1학기 말 스냅샷 (2026년 6월 기준, Rust 버전)

2학기 시작 시점(C++ 전면 재작성 직전)의 프로젝트 상태입니다. `README.md`의 2026-06-17 갱신본 기준:

- **언어**: Rust (+ Python, MCP 서버)
- **엔진**: B+Tree, WAL, Buffer Pool, MVCC(초기 버전), 비용 기반 쿼리 옵티마이저, 히스토그램 통계, DML 시 자동 통계 수집, B+Tree 인덱스 디스크 영속화, 증분 보조 인덱스 갱신(O(1)), DELETE PK fast-path, 병렬 쿼리 실행(SeqScan/GROUP BY/ORDER BY), 쿼리 결과 캐시(LRU 512), 저장 프로시저·트리거·UDF 영속화
- **SQL 지원**: DDL/DML/JOIN/서브쿼리/CTE/UNION/제약조건/트랜잭션/저장 프로시저/트리거/UDF
- **AI MCP 연동**: Claude Desktop 대상 stdio JSON-RPC, 도구 16개(DB 7개 + UI 9개) — 이 중 UI 제어용 9개는 **실제로는 한 번도 작동한 적 없었음**이 2학기 초반(Phase 17)에 밝혀짐
- **DBMS**: TCP 서버(자체 프로토콜 + MySQL 와이어 프로토콜 동시 지원), 다중 클라이언트, 세션 모니터링
- **UI**: Tauri + React 데스크톱 앱

즉 1학기 말 시점에도 이미 상당히 성숙한 RDBMS였습니다 — 이번 2학기의 핵심은 **기능 추가보다는 "언어 전면 교체 + 정합성/동시성/성능을 실제로 파고드는 심화"** 였습니다.

---

## 2026년 7월: C++ 전면 재작성 (2학기 개발의 출발점)

### 7월 4일 — 개발 문서 체계 도입
`docs/mds/PLAN.md`(개선 과제 추적) / `FUNCTIONS.md`(구현된 기능 목록) / `DIFF.md`(MySQL·PostgreSQL·Oracle 대비 기능 비교표) 최초 작성. 이후 모든 개발 단계가 이 세 문서를 기준으로 추적됨.

### 7월 7일~8일 — C++ 마이그레이션 마라톤

**핵심 결정**: "충실한 1:1 포팅" — 동작(버그까지)을 그대로 재현하고, 재설계하지 않는다는 원칙으로 시작. `rusql-core`/`rusql-cli`/`rusql-client`/`rusql-server` 전체를 C++20으로 재작성.

- **Phase 1~7**: 빌드 골격, 클라이언트, AST/스키마, 렉서/파서, 스토리지 엔진, 트랜잭션 레이어, 플래너/조인/락매니저/쿼리캐시.
- **Phase 8** (executor.rs 9,454줄 전체 포팅): DDL·DML 코어(INSERT/UPDATE/DELETE, FK CASCADE/RESTRICT/SET NULL/SET DEFAULT)·SELECT(WHERE/JOIN/GROUP BY/HAVING/ORDER BY/서브쿼리/CTE/UNION/윈도우함수 전체 스위트)·트랜잭션/MVCC/락(BEGIN/COMMIT/ROLLBACK/SAVEPOINT, 그룹 커밋, WAL 복구)·저장 프로시저/트리거/UDF·DCL·INFORMATION_SCHEMA까지 전부.
- **Phase 9**: `rusql-cli`(REPL), `rusql-server`(자체 TCP 프로토콜 + MySQL 와이어 프로토콜 전체 — 텍스트/바이너리 prepared statement 포함) 포팅. 실제 MySQL 8.4 클라이언트로 검증.
- **Phase 10** ("마이그레이션 완료" 선언 시점): `test_full.sql`(603줄)을 Rust판과 C++판 양쪽에서 실행해 출력 diff — **실제 버그 9개 발견/수정**(CLI 배너, 빈 줄 처리 차이, 함수 헤더 포맷, 부동소수점 정밀도, SELECT-list 스칼라 서브쿼리 미구현, 뷰/서브쿼리 재파싱 시 빈 셀 누락으로 인한 데이터 밀림, SHOW TABLES 크래시, EXPLAIN의 UTF-8 폭 계산 오류, 비용 반올림 오류).
- **Phase 11**(+후속 2건): "완료"로 끝난 게 아니라 사용자가 "진짜 bug-for-bug 동일한가?"를 재차 요구 — 조사해보니 플래너가 실제 실행 경로에 전혀 연결 안 돼 있었음(EXPLAIN 표시용으로만 존재). 인덱스 기반 접근경로 9종·Top-K fast-path·병렬 실행(스레드풀 도입, GROUP BY/ORDER BY/집계)을 실제 실행에 배선. 이어서 벤치마크를 실제로 돌려보고서야 "Debug 빌드로 측정하면 병렬이 오히려 느려 보인다"는 함정 발견 → Release 빌드로 재측정해 진짜 이득 확인.
- **Phase 12**: `PLAN.md` Section A의 P0(데이터 무결성) 버그 8건 전부 수정 — VACUUM이 PK 인덱스를 손상시키는 버그, WHERE 우변에 산술식 미지원, 파서가 뒤에 남은 토큰을 검증 안 함, B+Tree가 `"007"`과 `"07"`을 같은 값으로 오인, 복합 인덱스가 숫자 컬럼을 문자열로 정렬, 쿼리 캐시가 NOW()/RAND() 같은 비결정 함수도 캐싱, 이중 음수(`- -5`)가 문자열 `"--5"`로 잘못 평가, RENAME COLUMN이 인덱스를 전혀 안 갱신.
- **Phase 13**: 서버의 세미콜론 분리 로직 버그 2건(트랜잭션 BEGIN을 프로시저 블록으로 오인, 여러 줄짜리 트리거 본문이 별도 statement로 새어나감) — 실제 UI로 `test_full.sql`/`test_full-ver2.sql`을 돌려보다 발견.

**결과**: 테스트 218 → **3,192 assertions**. (git: `a351bfa`, `cac984c` 등, 2026-07-08 커밋)

### 7월 11일~12일 — Phase 14~17

- **Phase 14**: 복합 PK 테이블에서 UPDATE/MERGE가 첫 번째 PK 컬럼만 보고 행을 특정하던 버그(3개 파일) 수정 + UI의 PK 값 이스케이프/복합 PK 셀 편집 버그.
- **Phase 15**: 내구성/보안 P0 5건 — 파일 저장 시 fsync 없음(원자적 쓰기로 교체), 크래시 복구가 보조/해시/복합 인덱스를 재구축 안 함, 사용자 테이블이 비면 인증이 fail-open, BACKUP/RESTORE 경로 미검증(임의 파일 읽기/쓰기·SQL로 실행까지 가능했던 취약점), **WAL 커밋 fsync가 사실상 완전 무동작**(C++ 포팅 과정에서 새로 생긴 진짜 회귀, Rust엔 없던 버그). 감사 중 2건 추가 발견(fsync 실패를 아무도 체크 안 함, MySQL 바이너리 프로토콜이 DOUBLE 파라미터를 6자리로 잘라버림).
- **Phase 16**: 실제 UI로 벤치마크를 돌리다 **진짜 서버 크래시** 발견(연결 스레드에 예외 안전성 전무) → try/catch로 프로세스 전체가 죽지 않도록 수정. 그 외 UI/벤치마크 도구 자잘한 버그 다수.
- **Phase 17**: `legacy/` Rust 원본 폴더 완전 삭제(별도 브랜치에 보존). 실제 `mysql` CLI와 실제 MCP 클라이언트로 검증하다 3건 발견: `SUM(비교식)` 파싱 불가, `SHOW INDEX`가 사실상 항상 빈 결과, **MCP 도구 16개 중 9개(UI 제어용)가 애초부터 한 번도 작동한 적 없었음**(가짜 프로토콜에 아무도 응답 안 함, 심지어 클라이언트가 무한 대기하는 경우까지) — 제거.

**결과**: 244 test cases / **3,292 assertions**. (git: `d2999cc`/`839e17f`/`938efd1`, 2026-07-11~12)

### 7월 14일 — Phase 18: 동시성 개선 착수
`SharedDatabase`가 이미 진짜 `shared_mutex` 기반이라는 걸 활용해, 순수 읽기 전용 SELECT는 공유 락으로 동시 실행 허용(기존엔 SELECT 포함 모든 문장이 전체 배타 락). 실측: 대용량 SELECT 진행 중에도 다른 세션의 `SELECT 1`이 1.06초가 아니라 0.00024초 만에 응답. 프로시저 루프/트리거 재귀에 상한(10만회/32단) 추가. (git: `fcdaee7`)

### 7월 17일 — Phase 19: 보안 강화 + B+Tree 구조 개선
자체 TCP 프로토콜의 평문 비밀번호 전송을 challenge-response 방식으로 교체, MySQL 리스너 기본 바인드를 `0.0.0.0`→`127.0.0.1`로 변경(기본 계정 root/root가 LAN에 노출되던 문제), B+Tree 삭제 시 언더플로우 리밸런싱 구현(대량 삭제 시 트리가 무한정 성겨지던 문제). (git: `54c351a`)

### 7월 19일 — Phase 20
재귀 CTE가 1000회 상한에 걸리면 조용히 불완전한 결과를 반환하던 것 → 명확한 에러로 변경. `SELECT USER()`가 항상 `root@localhost`로 고정돼 있던 것 → 실제 세션 사용자 반영. UI에 DROP/TRUNCATE 등 파괴적 작업 확인 다이얼로그 추가. (git: `04d9a44`)

### 7월 22일 — Phase 20 후속 + 21 + 22
- Phase 20에서 UI 다이얼로그를 코드로만 검증했던 걸, WebView2의 Chrome DevTools Protocol을 이용한 자체 브라우저 자동화 스크립트로 실제 클릭까지 검증.
- Phase 21: "진짜 필요한" P1 5건 — WAL/Undo 로그 재작성이 원자적이지 않음, 체크포인트-그룹커밋 TOCTOU, Tauri가 개발 머신 절대경로를 하드코딩, MCP 설정 파일이 파싱 실패 시 조용히 덮어써짐, MySQL `SET` 문이 실제로는 아무것도 안 하면서 항상 OK 반환.
- Phase 22: main/old 브랜치 분리 감사(레거시 Rust 코드가 실행 경로에 전혀 안 남아있는지 확인) + **MCP 서버가 인증 방식 변경 이후 완전히 고장나 있던 것**을 실제 클라이언트로 접속해보고 발견/수정, JSON 파싱 버그도 함께 수정.

(git: `68a5f22`, 2026-07-22 커밋. 단, Phase 22의 MCP 수정 자체는 **8월 2일에 별도 커밋**됨 — 아래 참고.)

---

## 2026년 8월: MVCC 전면 재설계 + 엔진 고도화 (2학기 개발의 핵심)

### 8월 2일 — (커밋 `a0d42df`) MCP 서버 수정 반영
7/22에 만들어둔 MCP 인증/JSON 파싱 수정이 이 날 커밋됨.

### 7월 22일 ~ 8월 3일 — Phase 23~24: 진짜 행 단위 MVCC

사용자가 "MVCC + XA 분산 트랜잭션"을 어떻게 계획할지 물어봐서, MVCC를 먼저 하는 걸 권장 — 기존 `_xmin`/`_xmax` 의사 컬럼 + 전역 트랜잭션 ID를 재활용하는 단계적 설계로 승인받음.

- **Stage 1/2**: `SnapshotCtx` 도입, 격리 수준(RU/RC/RR/Serializable)이 실제로 다르게 동작하도록 재작성. UPDATE가 더 이상 제자리 수정이 아니라 실제 버전 체인을 생성(구버전엔 `_xmax` 스탬프, 신버전을 append). `session_tables`(세션별 버퍼 방식) 완전 삭제 — 모든 DML이 `s.tables`에 직접, 실제 트랜잭션 ID로 기록. VACUUM도 "GC 호라이즌" 기반으로 교체.
- **Stage 3**: SERIALIZABLE의 충돌 탐지를 "행 개수 비교"(같은 개수라도 내용이 다르면 못 잡음)에서 진짜 read-set 기반 검증으로 교체.
- **Stage 4**: 전역 단일 배타 락 → 구조 락(DDL 등)+테이블별 락 2계층으로, 서로 다른 테이블에 대한 쓰기가 실제로 동시 실행되도록.

이 과정에서 실제 멀티스레드 스트레스 테스트로 레이스 컨디션 다수 발견/수정(맵 lazy-init 레이스로 인한 세그폴트 등).

(git: `d1ac37a`, 2026-08-04 새벽 커밋)

### 8월 4일~5일 — Phase 25: Gap Lock (팬텀 읽기 방지)
InnoDB 스타일로, REPEATABLE READ/SERIALIZABLE에서 범위를 잠근 뒤 다른 트랜잭션이 그 범위에 새로 INSERT하는 걸 거부. 검증 중 실제 버그 발견(inline PK 테이블에서 항상 스킵되던 버그) — 통합 테스트가 즉시 잡아냄. (git: `c8ac583`, 2026-08-05)

### 8월 5일 — Phase 26: 증분 인덱스 유지보수
UPDATE/DELETE가 몇 행이 바뀌든 항상 테이블 전체를 복제해서 PK/복합 인덱스를 통째로 재구축하던 것 → 바뀐 행만 증분 갱신하도록 재작성. 검증 중 `CompositeIndexPath`가 MVCC 가시성 검사를 아예 안 하던 버그도 발견/수정. (git: `a5477ee`, 2026-08-05)

### 8월 5일 ~ 12일 — Phase 27~30 (한 커밋에 몰림, 이번 학기 최대 규모)

- **Phase 27 — 행 단위 완전 동시 쓰기**: 같은 테이블의 서로 다른 행에 대한 INSERT/UPDATE/DELETE가 실제로 동시 실행되도록. 실제 스트레스 테스트로 **8개의 진짜 버그**를 발견/수정했는데, 그중 가장 심각했던 건 커밋 처리 중 짧은 시간 창에서 발생하는 MVCC 가시성 레이스 — 두 세션이 같은 행을 반복 갱신하면 저장된 행 개수가 **233→377→610→987→1597처럼 피보나치 수열 모양으로 발산**하고 커밋 지연이 수십 초까지 늘어나던 버그(실 서버 기준 60초 이상 응답 없음). fsync부터 트랜잭션 정리까지를 하나의 연속된 배타 구간으로 병합해 해결 — 수정 후 동일 스트레스 테스트가 4초 안에 안정적으로 끝남.
- **Phase 28 — 진짜 블로킹 대기 락 + 데드락 감지**: 원래부터 최우선 순위였던 항목. 충돌 시 즉시 실패 대신 실제로 대기(`condition_variable`)하다가 타임아웃되거나 락이 풀리면 깨어남. 실제 라이브 행(hang)으로 두 번째의 더 심각한 교차 데드락(테이블 락 ↔ 락매니저)을 발견해 함께 수정.
- **Phase 29**: Phase 28에서 범위 밖으로 남겨뒀던 3건 — FK 캐스케이드가 인덱스를 전혀 안 갱신하던 버그, SELECT FOR UPDATE/FOR SHARE 블로킹, INSERT-vs-Gap-Lock 블로킹 — 모두 마무리.
- **Phase 30 — 테이블 파티셔닝**: `PARTITION BY RANGE/LIST/HASH` V1. 파티션 테이블 = 항상 비어있는 논리적 phantom + N개의 실제 자식 테이블, statement를 자식별로 재작성해 기존 락/MVCC/인덱스 로직을 그대로 재사용하는 방식으로 설계해 리스크를 낮춤. `ALTER TABLE ADD/DROP PARTITION`까지 포함.

(git: `187be58`, 2026-08-12, **4,647줄 추가 — 이번 학기 최대 규모 단일 커밋**)

### 8월 12일 — Phase 31: 쿼리 기능 확장
`BIT_AND`/`BIT_OR`, `FILTER (WHERE ...)`, `JSON_AGG`, 그리고 아키텍처적으로 가장 규모가 큰 **LATERAL JOIN**(바깥쪽 행마다 서브쿼리를 다시 평가) 추가. (git: `d0cc0f9`, 2026-08-12)

> **(12일간 공백 — 8/12~8/24 사이 개발 없음, 캡스톤 관련 다른 논의/일정으로 추정)**

### 8월 24일 — Phase 32~33 + AI 탭 UI placeholder
졸업작품 일정(당시 기준 약 3개월 남음)을 고려해 PLAN.md의 남은 항목을 재감사 — 이미 고쳐졌는데 문서만 안 갱신된 stale 항목 2건 발견.
- **Phase 32**: 실사용 클라이언트(`rusql-client`)의 세미콜론 카운팅이 BEGIN/END 블록을 인식 못 해 멀티 statement 트리거/프로시저 입력 시 행(hang)되던 버그, 플래너가 3개 이상 테이블 조인 시 두 번째 이후 조인의 알고리즘 선택을 첫 테이블의 원본 크기로만 계산하던 버그(누적 카디널리티 미반영).
- **Phase 33**: MySQL 커넥션 풀 명령(`COM_CHANGE_USER`/`COM_RESET_CONNECTION`) 추가(HikariCP 같은 풀링 드라이버 지원), WAL/Undo 레코드에 체크섬 추가(손상 감지), **BACKUP/RESTORE가 자기 참조 FK가 있는 테이블에서 데이터를 유실하던 버그** 수정(`test_full-ver2.sql` 기준 RESTORE 에러 24건→0건).
- 캡스톤 AI 방향 논의 시작과 함께 사이드바에 AI 탭 placeholder 추가.

(git: `e42c057` + `d92dd0d`, 2026-08-24)

### 8월 26일 — Phase 34~37: 엔진 성능/정합성 심화

사용자가 "SSI 같은 엔진 측면에서 강화할 수 있는 것"을 요청, 이후 스코프를 계속 넓혀가며 진행:

- **Phase 34**: MCV(최빈값) 기반 카디널리티 추정 추가(쏠린 컬럼에서 계획 품질 향상). SERIALIZABLE의 마지막 남은 갭이던 "잠금 없는 일반 SELECT의 팬텀"을 PostgreSQL SIREAD 스타일의 비차단 predicate lock으로 해결(단, 진짜 Cahill 알고리즘의 dangerous-structure 탐지는 아니고 보수적 근사임을 명시).
- **Phase 35**: FK/UNIQUE 제약 검증이 참조 테이블 전체를 선형 스캔하던 것 → 기존 인덱스(PK/보조/해시) 활용하도록 수정, 실측 **최대 약 33배 속도 개선**(벤치마크로 검증하다가 첫 구현 자체의 속도 버그까지 발견해 재수정). 재귀 CTE의 중복 제거를 O(n²) → O(1) 평균으로 개선.
- **Phase 36**: 재귀 CTE가 매 반복마다 JOIN을 누적 테이블 전체에 다시 실행하던 것 → semi-naive 평가(직전 반복분만 조인)로 개선. 체인 깊이 900 기준 최종 **원본 대비 약 3.8배** 개선.
- **Phase 37**: 조인 알고리즘이 항상 FROM절의 첫 번째 테이블을 기준으로 삼던 것 → 크기가 작은 쪽을 기준으로 큰 쪽의 인덱스를 프로브하는 `ReverseIndexNL` 신규 추가. 검증 중 LEFT/RIGHT JOIN이 인덱스 프로브 실패 시 NULL 채움 없이 조용히 행을 버리던 실제 정확성 버그도 함께 발견/수정.

### 8월 27일 — Phase 38~39: 버그 재진단 및 수정

- **Phase 38**: Phase 37 검증 중 "보조 인덱스 컬럼에 NULL이 하나라도 있으면 그 인덱스 전체가 제외된다"고 오진했던 걸, 정밀 재현 실험으로 반증하고 **진짜 원인**을 찾아냄 — 인덱스 이름이 테이블/데이터베이스 간에 구분 없이 등록되고 있어서, 같은 이름을 재사용하면 나중 인덱스가 조용히 등록 누락되던 버그. 인메모리 키를 `테이블명_인덱스명`으로 통일해 수정, 그 과정에서 `SHOW INDEX` 표시·`ALTER TABLE RENAME` 후 인덱스 유실 등 연쇄 버그도 함께 발견/수정.
- **Phase 39**: `INSERT ... ON DUPLICATE KEY UPDATE`/다중 테이블 `DELETE`/`MERGE`가 일부(또는 전부) 인덱스 종류를 전혀 갱신하지 않던 버그(Phase 26에서 발견했지만 스코프 밖으로 미뤄뒀던 항목) 수정 — 특히 `MERGE`는 PK 인덱스조차 전혀 안 건드리고 있었음.

(git: `7985acf`, 2026-08-27 — Phase 34~38 포함. Phase 39는 별도 커밋으로 뒤이어 반영.)

### 8월 27일 — Phase 40: 프런트엔드 리팩터링 (AI 탭 분리)

App.tsx(4090줄 단일 컴포넌트)를 전면 리팩터링할지, AI 탭 부분만 분리할지 논의 — 아직 AI 방향이 안 정해진 상태(`AI.md` 참고)라 지금 당장 필요한 범위(AI 탭이 나중에 실제 기능을 담을 자리를 미리 분리해두는 것)만 진행하기로 결정. 지금까지 빈 `<div className="ai-view" />`로만 존재하던 AI 탭을 `code/frontend/src/components/AiView.tsx`(이 프런트엔드 최초의 `components/` 폴더)로 추출 — App.tsx는 import 한 줄 + 렌더 한 줄만 변경, 동작 변화 없음. WebView2 CDP 드라이버 스크립트로 앱을 직접 빌드·실행해 로그인부터 AI 탭 클릭까지 라이브로 확인(리팩터링 전과 화면이 동일함을 스크린샷으로 검증). 이 과정에서 `App.css`에 아무 데서도 참조 안 하는 정교한 AI 채팅 UI용 CSS(`.ai-chat-*`, 툴콜 표시, 파일 편집 미리보기, 히스토리 사이드바 등 250여 줄)가 이미 존재하는 걸 발견 — AI 탭이 예전에 훨씬 구체적으로 구상됐던 흔적으로 보여 기록만 해둠(손 안 댐).

### 8월 27일 — Phase 41: 오래 방치됐던 진짜 버그 2건 수정

문서화만 해두고 "충실히 보존"하기로 했던 버그들을 재검토해 실제로 고칠 수 있는지 판단:

- **`DELETE ... WHERE <서브쿼리>`가 항상 0행 삭제**: Phase 8c(2026-07-07)에서 처음 발견해 원본 Rust의 버그로 그동안 보존해온 것. 실제 삭제를 수행하는 3개 지점 모두, 후보 행은 서브쿼리를 이해하는 매처로 정확히 찾아놓고 정작 지울 땐 서브쿼리를 모르는 매처로 다시 확인하고 있어서 매번 0행이 삭제되던 것 — 세 곳 다 조건부 분기로 수정. 예전에 "0행 삭제가 정상"이라고 잘못 문서화해뒀던 테스트 자체를 올바른 기대값으로 교체.
- **`DROP DATABASE`가 해시 인덱스를 전혀 정리 안 함**: 일반/복합 인덱스는 정리하면서 해시 인덱스만 정리 루프에서 빠져 있던 메모리 누수 — 같은 패턴으로 추가.

Debug+Release **377 케이스/22,506 assertions** 전부 통과, `test_full.sql`/`test_full-ver2.sql` 재검증 완료.

### 8월 29일 — Phase 42: 전체 문서 최신화 + 커밋/푸시

사용자가 "최종 테스트를 진행한 후, 모든 문서 파일을 전부 최신화해달라"고 명시적으로 요청, 완료되면 커밋&푸시까지 승인. 리포지토리 전체 `.md` 파일을 훑어 stale한 부분을 찾아 갱신:

- **`README.md`**(루트): 가장 크게 갱신됐음 — 이전에는 "읽기 전용 동시성만 지원", "논리 삭제 → VACUUM"만 언급하는 등 1학기 수준 설명이 그대로 남아있었음. Core Features/Technology Stack 표와 아키텍처 다이어그램을 실제 행 단위 MVCC, 블로킹 대기 락 + 데드락 감지, Gap Lock, SSI predicate lock, 테이블 파티셔닝, LATERAL JOIN, ReverseIndexNL, MCV, WAL/Undo 체크섬, 커넥션 풀 명령까지 반영하도록 전면 수정. 깨진 링크(`docs/instructions/` → `docs/mds/`) 2곳도 수정. 내장된 `test_full.sql` 사본이 실제 파일과 바이트 단위로 동일한지 diff로 확인(변경 없음, 그대로 둠).
- **`docs/mds/architecture-diagram.md`**: README와 동일한 최신화 — PARTITION BY DDL, LATERAL JOIN, ReverseIndexNL, BIT_AND/BIT_OR/JSON_AGG/FILTER, semi-naive 재귀 CTE, 실제 블로킹 락+데드락 감지, SSI predicate lock, MCV, WAL/Undo 체크섬, 커넥션 풀 명령 추가.
- **`docs/mds/DIFF.md`**: stale한 행 2건 수정 — "커넥션 풀 지원"(✗ → △, 내장 풀러는 없지만 클라이언트 드라이버 풀링 지원 명시), "데이터 임포트/익스포트"(✓ → △, 당시 CSV 임포트에 UI가 없었음).

(git: `b890def`, 2026-08-29, origin/main에 푸시 완료)

### 8월 31일 — Phase 43~45: 프런트엔드 정리 + CSV 임포트 UI + 우클릭 메뉴 검증에서 발견한 캐시 버그

- **Phase 43**: AI 방향이 아직 정해지지 않아 대기하는 동안, 죽은 코드 정리 — 프런트엔드 어디서도 호출하지 않는 Tauri 커맨드 `get_views`/`get_indexes`(각각 `_for_db` 버전이 실제로 쓰이고 있어 이쪽만 완전히 죽어있었음)와 빈 스크래치 파일 `srv_cut.txt` 삭제.
- **Phase 44**: 마지막까지 UI가 없던 CSV 임포트 기능 구현. 기존 SQL 파일 임포트 버튼과 동일한 방식(`<input type="file">` + `FileReader`, 별도 Tauri 다이얼로그 플러그인 없이)으로 테이블 우클릭 메뉴에 "Import CSV..." 항목 추가. `import_csv` Tauri 커맨드 시그니처를 파일 경로 대신 파일 내용을 직접 받도록 변경. WebView2 CDP 드라이버로 실제 앱을 띄워 라이브 검증(네이티브 OS 파일 선택 창은 자동화가 불가능해 `HTMLInputElement.prototype.click`을 가로채 가짜 `File` 객체를 주입하는 방식 사용) — CSV 3행 임포트 후 실제로 테이블에 반영된 것까지 확인.
- **Phase 45**: 사용자가 "우클릭 메뉴들 전부 작동하는지" 확인을 요청, 10개 항목을 실제 앱으로 하나씩 라이브 검증. 9개는 정상이었고, 10번째(DROP Table)는 SQL 실행 자체는 정상이었지만 검증 과정에서 **별개의 진짜 버그**를 발견 — 쿼리 결과 캐시가 `SELECT ... FROM information_schema.tables`류 쿼리를 캐싱할 때 "information_schema.tables"라는 가상 테이블명을 캐시 의존성으로 등록하는데, DDL(DROP TABLE 등)의 캐시 무효화는 항상 실제로 건드린 진짜 테이블만 대상으로 해서, 한 번 캐싱된 information_schema 조회 결과는 이후 어떤 스키마 변경에도 절대 무효화되지 않음 — DROP TABLE 후에도 사이드바가 드롭된 테이블을 영구히 계속 보여주는 버그였음. 기존 `has_subquery`/`has_nondeterministic` 캐시 제외 패턴과 동일하게, information_schema를 참조하는 쿼리는 아예 캐싱하지 않도록 수정(`references_infoschema` 단어경계 인식 헬퍼 신규 추가). 정확히 이 시나리오(캐시 데우기 → DROP TABLE → 동일 SQL 재실행 → 드롭된 테이블이 결과에서 사라졌는지)를 검증하는 회귀 테스트 추가.

Debug+Release **378 케이스/22,514 assertions** 전부 통과, `test_full.sql`/`test_full-ver2.sql`을 `engine_cli.exe`로 양쪽 설정 모두 재검증 완료.

### 9월 6일 — Phase 46: App.tsx UI 섹션 분리 리팩터링

AI 방향을 제외하고 남은 엔진/프런트/서버/성능 개선 항목을 순서대로 진행하기로 함(사용자 승인) — 그 1번인 App.tsx(4231줄 단일 컴포넌트) 리팩터링. 실제 코드를 열어 상태 결합도를 재확인한 뒤 리팩터링 범위를 사전 확정: 전체 재작성이나 Redux/Zustand 같은 상태관리 라이브러리 도입은 이번 스코프에서 배제하고, "UI 섹션만 컴포넌트로 분리, 상태는 App.tsx에 그대로 유지"로 결정. 그중에서도 사이드바/다이얼로그/컨텍스트메뉴/ERD 뷰(거의 순수 프레젠테이션)만 1차로 분리하고, 에디터·탭바·결과패널(Monaco 에디터 ref·쿼리 실행·split-view 드래그 상태와 가장 깊게 얽혀있어 정확성 리스크가 큼)은 이번 라운드에서 의도적으로 제외.

신규 파일: `src/types.ts`(공유 인터페이스), `src/lib/erd.ts`(ERD 순수 레이아웃 계산 헬퍼), `src/lib/measureText.ts`(캔버스 텍스트 측정, 결과 표/ERD 데이터 패널 공유), `src/components/Sidebar.tsx`·`ErdView.tsx`·`ConfirmDialog.tsx`·`EditTableModal.tsx`·`ContextMenus.tsx`(DB/테이블/뷰/인덱스 4종 우클릭 메뉴를 한 파일에 묶음). 전부 동작 변경 없는 순수 이동(로직 그대로, JSX와 지역 클로저만 이전) — App.tsx가 4231줄 → 3332줄로 약 21% 감소.

**검증**: `tsc --noEmit`(strict + noUnusedLocals 전부 켜진 채로 클린) → `vite build` 클린 → 실제 앱을 재빌드·재실행해 WebView2 CDP 드라이버로 사이드바 펼치기, DB/테이블 우클릭 메뉴, Edit Table 모달, DROP Table→ConfirmDialog→실제 삭제 후 사이드바 즉시 갱신, ERD 뷰(FK 있는 2테이블 카드 렌더링)까지 전부 라이브 재현·확인. 이 라이브 검증 과정에서 Phase 45 정보-스키마 캐시 수정이 실제로 살아있는 서버에서도 동작함을 재확인(사이드바가 DROP 직후 즉시 갱신됨), 그리고 별개의 새 버그도 발견 — `get_columns_detail` Tauri 커맨드가 FK 컬럼의 `fk_ref`를 인라인/테이블 레벨 선언 방식과 무관하게 항상 null로 반환해(엔진 자체는 `SHOW CREATE TABLE`로 확인한 대로 FK를 정확히 저장 중 — 조회 커맨드만의 누락) 사이드바 Foreign Keys 섹션과 ERD 관계선이 항상 비어 보이는 증상으로 이어짐 — 이번 스코프 밖이라 원인 위치만 특정해 기록, 미수정.

(같은 세션, 아직 커밋 전)

### 9월 6일 — Phase 47: 내부 쿼리 합성 ASCII 재파싱 방식의 값-손상 버그 수정

두 번째로 진행한 항목. UNION/CTE/서브쿼리/LATERAL/파티션 자식 라우팅 등이 실행 결과를 사람이 보는 ASCII 표 문자열로 만든 뒤 다시 `\|`/개행으로 split해서 `Row`로 복원하는 구조(`parse_table_output`, 13개 호출부가 공유)라, 값 안에 실제 `\|`나 개행이 있으면 셀 경계를 잘못 잡아 값 뒷부분이 조용히 잘리거나 줄 경계 자체가 깨지는 문제. 조사 결과 표를 만드는 코드 자체가 `executor_select.cpp` 3곳(스칼라 `_dual_` SELECT/집계 SELECT/일반 SELECT)·`executor_setops.cpp`·`executor_infoschema.cpp`에 흩어져 있어, "구조화된 내부 API로 완전 교체"는 예상보다 훨씬 큰 작업임을 재확인 — 대신 두 지점(표 생성 + 재파싱)에 이스케이프를 추가하는 낮은 리스크 수정으로 진행. 신규 `Executor::escape_cell`을 5개 표-생성 지점 전부에 적용, `parse_table_output`은 이스케이프 인식 스캐너 + unescape 쌍 추가. UNION(`\|` 포함 값)·CTE(개행 포함 값) 회귀 테스트 2건 신규 추가, 나머지 3개 지점은 `engine_cli.exe`로 라이브 확인(`SELECT 'a|b'`, `GROUP_CONCAT`, `information_schema.tables` 전부 정확히 이스케이프되어 표시·보존됨).

Debug+Release **380 케이스/22,530 assertions** 전부 통과, `test_full.sql`/`test_full-ver2.sql` 양쪽 설정 재검증 완료.

### 9월 6일 — Phase 48: mcp_server.py 접속정보 하드코딩 + 재시도 부재 수정

세 번째로 진행한 항목. `mcp_server.py`의 접속정보(host/port/계정)가 모듈 상수로 고정돼 있고, 이를 Claude Desktop에 등록하는 `main.rs`의 `setup_mcp_config`도 실제 접속 중인 서버 정보를 전혀 전달하지 않아 기본값(127.0.0.1:7878, root/root)과 다른 서버에서는 MCP 도구가 항상 실패하던 문제. `mcp_server.py`는 환경변수(`RUSQL_HOST`/`PORT`/`USER`/`PASS`, 없으면 기존 기본값 폴백)를 읽도록, `setup_mcp_config`는 매개변수로 받은 실제 접속정보를 Claude Desktop 설정의 `"env"` 필드에 실어 보내도록 수정 — 프런트의 "Auto-connect Claude Desktop" 버튼이 같은 Server Manager 탭의 기존 포트/계정 입력 필드를 그대로 전달. 재시도 부재는 `_Conn.__init__`에 0.5초 간격 최대 3회 재시도 추가(서버가 막 재시작된 순간의 일시적 연결 실패 흡수).

`cargo build --release`/`cargo test`(기존 write_mcp_into 테스트 2건 포함)/`tsc --noEmit` 전부 클린. 실제 Python으로 env var 오버라이드·재시도 타임아웃·실제 `engine_server.exe` 상대 연결/인증/쿼리 실행까지 라이브 검증 완료(성공 경로는 0.022초, 불필요한 재시도 없음). 실제 앱을 띄워 "Auto-connect Claude Desktop" 버튼도 라이브 검증 — 기본값(root/root)으로 클릭 시 `claude_desktop_config.json`에 `"env"` 필드가 정확히 기록되는지, 폼의 사용자명을 "bob"으로 바꾼 뒤 재클릭하면 `RUSQL_USER`가 실제로 "bob"으로 갱신되는지(하드코딩이 아니라 진짜 동적으로 반영됨) 둘 다 확인.

### 9월 6일 — Phase 49: Mutex unwrap→패닉 전파 수정 (main.rs)

네 번째로 진행한 항목. `state.db`/`state.servers`/`state.ui.*` Mutex에 대한 `.lock().unwrap()` 24곳(실제로 세어보니 PLAN.md의 "52곳"은 부정확했음, 전부 `.write()`/`.read()`가 아닌 `.lock()`) — 어느 한 Tauri 커맨드가 이 Mutex를 잡은 채 패닉하면 poison되고, 이후 같은 Mutex를 쓰는 모든 커맨드가 전부 패닉해 사실상 앱 전체가 재시작 전까지 먹통이 됨. 24곳 전부 `.lock().unwrap_or_else(\|e\| e.into_inner())`로 일괄 교체(poison 여부와 무관하게 guard 복구 — 실제 데이터는 별도 `engine_server` 프로세스에 있어 "가용성"이 맞는 트레이드오프). `AppState` 옆에 이유를 설명하는 주석 추가, poison-recovery 자체를 증명하는 유닛테스트 신규 추가(평범한 Mutex를 일부러 패닉으로 poison시킨 뒤 실제로 복구되는지 확인).

`cargo build --release`/`cargo test`(기존 2건 + 신규 1건, 총 3건)/`tsc --noEmit` 전부 클린.

### 9월 6일 — Phase 50: 병렬 임계값 환경변수 오타 수정

다섯 번째로 진행한 항목(가장 낮은 우선순위, 기본 동작 자체엔 영향 없음). `parallel_min_rows()`가 확인하던 환경변수 이름이 `RUSTDB_parallel_min_rows()`처럼 괄호가 포함돼 있어 애초에 어떤 플랫폼에서도 설정 불가능했음 — `RUSTDB_PARALLEL_MIN_ROWS`로 수정(기존 `parallel_enabled()`의 `RUSTDB_PARALLEL` 네이밍과 통일). 실제로 설정 시 값이 반영되는지 확인하는 유닛 테스트 신규 추가.

Debug+Release **381 케이스/22,531 assertions** 전부 통과.

### 9월 6일 — Phase 51: 6번 항목(신규 SQL 기능) 중 가치 높은 2개 진행 — ARRAY_AGG, REPLACE INTO

6번 항목(LOCK TABLES/REPLACE INTO/표현식·부분·내림차순 인덱스/ARRAY_AGG·PERCENTILE_CONT/CREATE SEQUENCE/열 레벨 권한·이벤트 스케줄러, 6개 묶음)을 전부 진행하는 대신 가치 높은 것만 선택 진행하기로 사용자와 합의 — ARRAY_AGG, REPLACE INTO, LOCK TABLES 3개를 우선순위로 선정.

- **ARRAY_AGG**: 이미 구현된 JSON_AGG와 완전히 동일한 로직(이 엔진엔 배열 타입이 없어 JSON 배열 텍스트로 직렬화)을 공유하는 새 `AggFunc::ArrayAgg` variant 추가 — 렉서/파서/실행기/AST 직렬화 전부 JsonAgg와 대칭으로 확장. 회귀 테스트(숫자/문자열/NULL/GROUP BY) 추가.
- **REPLACE INTO**: PK/UNIQUE 충돌 시 기존 행을 삭제 후 새 행을 통째로 삽입하는 MySQL 관용구. 기존 `InsertConflict`에 `Replace` variant만 추가해 `Statement::Insert`/`InsertSelect`를 그대로 재사용(새 Statement 타입 불필요) — 가장 delicate한 `exec_insert_inner` 내부는 전혀 안 건드리고, 그 앞단에서 충돌 컬럼마다 실제 `DELETE` 문을 `execute_with_s`로 실행(락·인덱스·트리거 정합성 전부 재사용)한 뒤 원래 INSERT 경로로 넘기는 방식으로 낮은 리스크로 구현. **라이브 테스트 중 실제 버그 발견+수정**: 테이블 레벨 복합 PK `(a,b)`에서 각 컬럼의 `primary_key` 플래그가 개별적으로도 true라는 걸 몰라 컬럼별 순회가 `a=1`만으로 삭제해 다른 행까지 잘못 지워짐 — 복합 PK일 땐 컬럼별 개별 처리를 건너뛰고 AND로 묶은 전용 분기만 타도록 수정. 회귀 테스트 5건(무충돌/단일PK/UNIQUE/복합PK/REPLACE INTO...SELECT) 신규 추가.

Debug+Release **387 케이스/22,591 assertions** 전부 통과(항목 5까지의 381→387, +6건: ARRAY_AGG 1건, REPLACE INTO 5건), `test_full.sql`/`test_full-ver2.sql` 양쪽 설정 재검증 완료. `engine_cli.exe`로 두 기능 모두 실제 시나리오 라이브 확인.

### 9월 6일 — Phase 52: LOCK TABLES / UNLOCK TABLES (V1) — 라이브 테스트로 설계를 한 번 뒤집은 사례

3번째로 선정한 항목. 여러 문장에 걸쳐 잠금을 들고 있어야 하는 LOCK TABLES의 특성상, 기존 `table_locks`/`table_data_locks`/`LockManager`(전부 "한 문장 실행 동안만" 전제로 여러 단계에 걸쳐 하드닝됨)와는 완전히 분리된 전용 레지스트리로 구현하기로 사용자와 사전 합의 — V1 스코프: 세션간 LOCK TABLES끼리만 상호 조율, LOCK TABLES를 안 쓰는 세션의 평범한 DML엔 영향 없음.

처음엔 "진짜 블로킹 대기"(폴링+sleep)로 구현하고 멀티스레드 Catch2 테스트까지 작성했는데, 실행해보니 테스트가 수십 초씩 걸리며 실패 — 원인 추적 결과, `execute()`의 최상위 디스패처가 LOCK TABLES 문장 실행 내내 데이터베이스 전체의 구조적 배타 락(`shared->write()`)을 잡고 있어서, 그 안에서 폴링하며 sleep하는 동안 **다른 모든 세션의 모든 문장이 멈춰버리는** 심각한 문제였음 — 충돌 중이던 세션 자신의 UNLOCK TABLES조차 대기 세션의 폴링이 끝날 때까지 실행이 안 되는 것까지 직접 확인. 이걸 제대로 고치려면 `execute()`의 락 획득 전략 자체를 바꿔야 하는데, 그건 이번 V1의 스코프를 좁힌 이유(기존 하드닝된 락 코드 비침습)와 정면으로 충돌하는 선택 — 그래서 **NOWAIT 방식으로 재설계**: 충돌 시 즉시 명확한 에러로 실패, 재시도는 애플리케이션 레벨에 맡김. 대기 로직 자체가 없어져 문제가 원천 차단됨.

AST에 `Statement::LockTables`/`UnlockTables` 신규 추가, `LOCK`/`UNLOCK`/`READ`/`WRITE`는 `BACKUP`/`RESTORE`와 동일하게 전용 예약어 없이 문맥상 식별자 텍스트로만 인식(실제 컬럼/테이블명과의 충돌 방지). 연결 종료 시 자동 해제도 `deregister_process()`와 동일한 명시적 호출 컨벤션으로 서버의 5개 연결종료 지점에 추가. 실제 멀티스레드(`Executor::new_session`) 테스트 5건 신규.

Debug+Release **392 케이스/22,635 assertions** 전부 통과(+5). `engine_cli.exe`로 기본 문법 라이브 확인.

Phase 46~52(App.tsx 리팩터링부터 LOCK TABLES까지 6개 항목 전부)를 한 번에 커밋 `4626f84`로 커밋+푸시(2026-09-07, 사용자의 "넵 해주세요" 승인).

### 9월 7일 — AI 방향 확정: NL→SQL 파인튜닝

캡스톤 AI 센터피스 방향을 최종 확정. 남은 기간(~11주) 재점검 결과 학습형 쿼리 옵티마이저는 버퍼가 부족해 리스크가 컸고, NL→SQL 파인튜닝(4~6주 추정)이 더 안전한 선택으로 재평가됨. "이미 MCP로 자연어 조작이 되는데 굳이 모델을 또 만들 필요가 있나"는 사용자의 재확인 질문에는, MCP 연동은 "기존 모델을 API로 소비"하는 것이라 캡스톤이 애초에 배제한 범주와 같고, 직접 파인튜닝이 캡스톤이 요구하는 "AI/ML을 직접 다뤄본 경험"에 부합한다고 정리해 재확인(MCP 연동은 그대로 유지). 배포 아키텍처도 함께 확정: Colab Pro에서 학습, 추론은 로컬(양자화 GGUF + CPU/GPU 겸용 런타임, 릴리즈에 모델 가중치 번들)로 완전 오프라인 동작. 상세 결정 기록은 `AI.md`가 canonical.

### 9월 12일~13일 — NL→SQL 데이터 생성 파이프라인 + Colab 파인튜닝 노트북

`code/AI/` 폴더(사용자 지시로 MCP 제외 AI 관련 파일 전용 위치) 신설. 베이스 모델 Qwen2.5-Coder-1.5B-Instruct 확정(Apache 2.0, 코드/SQL 특화 사전학습, CPU 추론 적합 크기).

`code/AI/data_generation/`에 합성 {스키마, 질문, SQL} 데이터 생성 파이프라인 구현 — 스키마 20개(그중 `real_estate`/`airline`/`insurance` 3개는 학습에서 완전히 제외한 held-out 일반화 테스트용), 쿼리 의도(intent) 16종, 한/영 이중언어, 한국어 조사(이/가·은/는·을/를·과/와·으로/로) 배치침 기반 자동 선택. 1차 생성(1916행) 후 사용자가 목표치(3000~6000행) 미달을 이유로 확장을 요청 — 의도 6종·스키마 3개 추가, 최종 3722행(train 2993/val 157/test 572)으로 확장.

이 확장 과정에서 실제 생성 파일을 `Read` 도구로 직접 읽어 검증하며(터미널 출력이 아니라) 진짜 품질 버그 4건 발견·수정: (1) 테이블/컬럼 식별자 68개가 한/영 용어 사전에 없어 한글 문장에 영어 원문이 그대로 섞여 나옴, (2) 영어 문장이 단수 라벨을 복수 문맥에 그대로 써서 문법 오류, (3) 한국어 조사가 받침 여부와 무관하게 여러 곳에서 하드코딩됨, (4) DISTINCT 질문에서 컬럼 라벨과 문구 단어가 중복 출력. 참고로 터미널에 한글이 깨져 보인 최초 현상은 Windows 콘솔 인코딩 문제였을 뿐 실제 파일은 처음부터 정상이었음 — `Read` 도구로 직접 읽어야만 진짜 문제와 터미널 표시 문제를 구분할 수 있었음.

`code/AI/finetuning/rusql_nl2sql_finetune.ipynb`: Colab Pro에 그대로 올려 실행하는 단일 노트북(사용자가 "ipynb 파일로, 하나의 파일로" 요청해 기존 4개 분리 스크립트를 통합) — 의존성 설치(버전 고정) → Drive 마운트 → 설정 → 공유 프롬프트 포맷 → Qwen2.5-Coder-1.5B-Instruct 로드+LoRA 부착(기본 4bit QLoRA) → 데이터셋 로드 → trl `SFTTrainer` 학습 → held-out 스키마로 문자열 일치 평가, 순서로 구성.

이 로컬(CPU-only) 환경에서는 실제 GPU 학습을 돌려볼 수 없어 노트북은 JSON 구조 유효성만 확인, 실제 Colab GPU 실행은 아직 안 됨 — 사용자가 Colab에서 직접 실행 후 결과 공유 예정.

이 작업들이 기존 엔진/프런트/서버에 영향이 없는지 사용자 요청으로 전체 재검증: C++ 엔진 Debug+Release Catch2 스위트(392 케이스/22,635 assertions) 양쪽 다 통과, `engine_cli.exe`로 `test_full.sql`/`test_full-ver2.sql` 재실행 클린, 프런트 `tsc --noEmit`/`vite build` 클린, Tauri(`src-tauri`) `cargo build --release`/`cargo test --release`(3/3) 클린 — AI 작업이 새 폴더(`code/AI/`)에만 있었다는 것과 일치하게 전부 이전과 동일하게 정상.

### 9월 14일 — `code/mcp/server.py` 죽은 코드 삭제

사용자가 `code/mcp/`를 검토하다 `server.py`(FastAPI + Google Gemini API 기반 REST 서버 — `/api/nl-to-sql`·`/api/chat`·`/api/explain` 등, 사용자가 넘긴 `api_key`로 Gemini를 직접 호출)가 실제 쓰이는 `mcp_server.py`(FastMCP, 로컬 엔진에 직접 연동, 외부 LLM API 호출 없음)와 별개로 남아있다는 걸 지적 — 캡스톤이 애초에 배제한 "기존 모델 API 소비" 방향의 잔재로 보인다는 지적이었음. `git log --follow`로 확인한 결과 마지막 수정이 2026-07-08(C++ 마이그레이션 마라톤 시작 직후)이고 이후 두 달 넘게 미수정, `main.rs`/프런트(TS·TSX)/`docs/` 어디에도 `server.py`나 그 포트(8765)·엔드포인트를 참조하는 코드가 전혀 없음을 확인 — 실제로 완전한 죽은 코드였음. 사용자 승인 후 삭제, `requirements.txt`도 `server.py` 전용 의존성(`google-genai`/`fastapi`/`uvicorn`/`pydantic`) 제거하고 `mcp[cli]`만 남김.

### 9월 14일 — 프로젝트 전역 죽은 코드 스윕

`server.py` 건을 계기로 사용자가 "이런 것처럼 죽은 코드가 또 있는지" 전체 검사를 요청 — C++ 백엔드/Rust(Tauri)/TS·React 프런트엔드 3개 영역을 병렬 Explore 에이전트로 조사(빌드 설정 대조 + 실제 참조 여부 grep, 추측 배제).

- **C++ 백엔드**: `core/include/engine/version.hpp` + `core/src/version.cpp` 발견 — `engine::version()`을 CLI/서버/클라이언트 어디서도 호출 안 함(Phase 1 시절 스캐폴딩이 그대로 방치됨). 삭제 + `CMakeLists.txt`에서 참조 제거, `cmake --build`로 `engine_core` 재빌드 클린 확인.
- **Rust(Tauri) 백엔드**: `export_csv` 커맨드(`main.rs`) — 이미 `PLAN.md`에 "의도적으로 남겨둔 잔여 항목"으로 기록돼 있던 건이 재발견됨(실제 CSV 내보내기는 프런트 Blob 방식으로만 동작, 이 커맨드는 등록만 되고 호출처가 전혀 없었음). `lib.rs`(모바일 지원용으로 예약된 빈 파일, 정상적인 Tauri 2.0 스캐폴딩)는 죽은 코드 아님으로 확인, 손 안 댐. 사용자 승인 후 `export_csv` + 전용 헬퍼 `csv_escape` 삭제, `generate_handler!` 등록도 제거, `cargo build --release` 클린 확인.
- **프런트엔드 CSS**: `App.css`에서 이전 감사(Phase 40) 때 기록됐던 "~250줄" `.ai-chat-*` 블록이 실제로는 **541줄/59개 클래스**로 더 컸다는 게 확인됐고, 그 감사에서 놓쳤던 **완전히 별개의, 더 오래된 죽은 AI 설정 UI 블록(307줄/31개 클래스 — API 키 입력·모드 탭·쿼리 텍스트에어리아·결과/가이드 패널을 갖춘 완결된 미사용 UI)**도 새로 발견. 그 외 사이드바 인덱스 아이콘/split-pane-close/home-topbar-ver/dlg-readonly(4개, ~38줄)와 `.ai-view` 중복 정의(6줄, 최근 AI 탭 placeholder 추가 커밋에서 실수로 중복 생성된 것)도 함께 발견. 실제 사용 중인 `.ai-view` 규칙 1개만 남기고 총 **~890줄(App.css의 약 23%)** 삭제 — 삭제 후 `tsc --noEmit`/`vite build` 클린 확인, 빌드된 CSS 번들이 58.21kB → 43.84kB로 실제 감소한 것으로 교차 검증.
- `.ts`/`.tsx` 파일 단위 죽은 파일, 죽은 컴포넌트/함수 export, 프런트→백엔드 커맨드 이름 불일치는 전부 없음(0건) — 이 세 영역은 이번 스윕에서 새로 발견되지 않음.

정리 후 전체 재검증: C++ Debug+Release Catch2(392/22,635, 둘 다 통과) + `engine_cli.exe` 스모크 테스트, 프런트 `tsc`/`vite build`, `cargo build --release`/`cargo test --release`(3/3) 전부 클린.

### 9월 14일 — Phase 46에서 발견만 하고 미뤄뒀던 `get_columns_detail`의 `fk_ref` null 버그 진짜 원인 특정 + 수정

사용자가 "null 버그 같은 게 있는 것 같다"고 막연히 언급 — Phase 46에서 발견했지만 원인 위치까지만 특정하고 수정은 미뤄뒀던 그 버그(사이드바 FK 섹션·ERD 관계선이 항상 비어 보임)로 추정하고 재조사.

`engine_cli.exe`로 직접 재현하며 진짜 원인을 찾음: 엔진은 **데이터베이스 이름은 소문자로 정규화해 저장하지만 테이블 이름은 입력한 대소문자를 그대로 보존**한다(`executor_infoschema.cpp`의 `columns`/`key_column_usage` 정보 스키마 뷰 둘 다 확인). 그런데 `main.rs`의 `get_columns_detail`이 UNIQUE/FK 메타데이터를 보강 조회하는 `uniq_sql`/`fk_sql` 두 곳 모두 테이블명까지 `.to_lowercase()`로 강제 변환한 뒤 `WHERE table_name='...'`로 비교하고 있어서, 테이블명에 대문자가 하나라도 있으면(`Child`, `Employee` 등 흔한 파스칼/카멜케이스 관례) 절대 매칭이 안 되어 조용히 0행이 반환되고 `fk_ref`/`is_unique`가 항상 비어버림 — Phase 46 당시 테스트에 쓴 테이블명이 소문자였다면 그때는 다른 이유로 실패했을 가능성이 있으나(당시 코드가 지금과 달랐을 수도 있음), 현재 코드 기준으로는 이 대소문자 불일치가 유일하고 확실한 원인.

라이브로 재현·검증: `CREATE TABLE Child (... FOREIGN KEY (parent_id) REFERENCES Parent(id))` 후 `WHERE table_name='child'`(강제 소문자)는 0행, `table_name='Child'`(원본 대소문자)는 정상 반환됨을 `engine_cli.exe`로 직접 확인. 두 쿼리 모두 테이블명 쪽 `.to_lowercase()`만 제거(DB명 쪽은 실제로 소문자 저장이므로 그대로 유지) — `bare`(테이블명 파라미터) 자체는 프런트의 `SHOW TABLES` 결과에서 이미 올바른 원본 대소문자로 넘어오는 것도 확인해 안전한 수정임을 확인. `cargo build --release`/`cargo test --release`(3/3)/`tsc --noEmit` 클린. 수정된 정확한 쿼리 문자열을 `engine_cli.exe`로 재실행해 대문자 포함 테이블(`Employee`/`Department`)에서 FK·UNIQUE 정보가 정상 반환되는 것까지 라이브로 재확인(단, 실행 중인 Tauri 앱을 직접 띄워 사이드바/ERD 화면으로 재확인하는 것까지는 이번 라운드에서 하지 않음 — 아래 참고).

### 9월 19일 — MCP에 Connections 관리 도구 3개 추가 (list/add/delete_connection)

사용자가 "Claude가 모든 버튼을 다 누를 수 있게" 만들 수 있는지 질문 — 대부분의 버튼은 결국 SQL문 하나라 이미 `execute_sql`로 가능하고, ERD 줌/탭 전환 같은 순수 UI 동작은 자연어로 조작할 실익이 없으며, 무엇보다 예전에 정확히 이런 시도(UI 제어용 도구 9개, Phase 17에서 전부 가짜였음이 밝혀져 제거)가 있었던 전례를 근거로 반대 — 대신 실제로 가치 있고 지금 비어있는 한 가지, **홈 화면의 저장된 Connections 목록 관리**로 스코프를 좁히자고 역제안, 사용자 승인.

기술적 난제: Connections 목록은 웹뷰 `localStorage`에 있고 MCP 서버(별도 Python 프로세스)는 그 프로세스에 접근할 방법이 전혀 없음 — 둘 사이에 다리가 아예 없었음. `code/data/connections.json` 파일을 새 공유 저장소로 도입해 해결:

- **`main.rs`**: `get_connections`/`save_connections` Tauri 커맨드 신규 추가(파일 없으면 빈 배열, 저장은 임시 파일+원자적 rename으로 절반만 쓰인 JSON 방지 — WAL 원자적 재작성과 동일한 이유). 기존 localStorage 기반 `loadConnections`/`saveConnections`를 파일 기반으로 교체하면서, 파일이 비어있으면(예전 설치) 기존 localStorage 내용을 1회 이관하는 마이그레이션도 같은 자리에 통합(기존 dataDir 마이그레이션·고아 디렉토리 정리 로직과 순서가 꼬이지 않도록 한 `useEffect`로 합침). 로그인 전(홈 화면) 상태에서는 3초 간격으로 파일을 다시 읽어, 앱을 재시작하지 않아도 MCP가 방금 추가/삭제한 연결이 화면에 반영되게 함.
- **`mcp_server.py`**: `list_connections`(비밀번호는 항상 제외하고 반환) · `add_connection`(name/host/port/user/password 전부 필수 파라미터로 선언 — Claude가 대화 중에 먼저 물어보게 강제) · `delete_connection`(id 우선 매칭, 이름은 유일할 때만 삭제하고 겹치면 후보 id 목록을 반환해 안전하게 거부) 3개 신규. 같은 파일을 직접 읽고 쓰며, 쓰기는 Rust 쪽과 동일하게 임시 파일+원자적 교체.
- **검증**: `cargo build --release`/`cargo test --release`(3/3)/`tsc --noEmit`/`vite build` 전부 클린. `mcp` 패키지가 로컬에 없어 `FastMCP`를 스텁으로 대체해 `mcp_server.py`를 직접 import하는 방식으로(Phase 48과 동일 기법) 7가지 시나리오(빈 목록, 추가, `list_connections`의 비밀번호 제외 확인, 이름으로 삭제, id로 삭제, 이름 중복 시 안전 거부, 존재하지 않는 id/이름 처리) 전부 실제 로직으로 통과 확인. 실제 `code/data/connections.json` 경로에 대해서도 추가→삭제 왕복 확인(테스트 잔여물 없이 정리). 개발 모드로 띄워둔 실제 앱이 Rust 변경을 자동 재빌드하는 것도 확인.

---

## 요약: 1학기 대비 2학기에 달라진 것

| 항목 | 1학기 (~2026년 6월) | 2학기 (2026년 7~8월) |
|---|---|---|
| 구현 언어 | Rust | **C++20 (전면 재작성)** |
| 개발의 중심축 | 기능 추가(SQL 문법·MCP 연동 확장) | **정합성·동시성·성능을 실제로 파고드는 심화** |
| 동시성 | 전역 단일 락(사실상 모든 문장 완전 직렬화) | 읽기 동시 실행 → **진짜 MVCC(버전 체인)** → 테이블 단위 동시 쓰기 → **행 단위 완전 동시 쓰기** → **블로킹 대기 락 + 데드락 감지** |
| 트랜잭션 격리 | 이름만 있고 실질적으로 동일 동작 | RU/RC/RR/Serializable이 **실제로 다르게 동작**, Gap Lock, SSI predicate lock까지 |
| 인덱스 유지보수 | 일부 경로에서 전체 재구축 | 대부분 증분 갱신으로 전환, 인덱스 이름 충돌·미유지보수 버그 다수 발견/수정 |
| 신규 SQL 기능 | JOIN/서브쿼리/CTE/윈도우함수 | **테이블 파티셔닝**, LATERAL JOIN, FILTER/JSON_AGG/BIT_AND·OR |
| 쿼리 플래너 | 존재하나 실제 실행에 미연결 | 실제 실행에 배선 + MCV 통계 + ReverseIndexNL + 누적 카디널리티 반영 |
| 검증 방식 | — | **매 변경마다 Debug+Release 전체 회귀 테스트 + 실 서버/실 클라이언트(mysql CLI, pymysql, MCP SDK 등)로 라이브 검증**하는 방법론이 이번 학기에 정착 (테스트 218 → 378 케이스, 3,080 → 22,514 assertions) |

이 기간 동안 발견되어 수정된 실제 버그는 60건 이상이며, 그중 다수(피보나치형 행 증가 MVCC 레이스, 크로스 프리미티브 데드락, MCP 서버 완전 고장, 원본 Rust부터 이어져 온 DELETE-서브쿼리 버그, information_schema 쿼리 캐시 영구 stale 버그 등)는 실제 라이브 테스트 없이는 발견 불가능했던 것으로, 이 프로젝트의 테스트 방법론 자체가 "Catch2 단위 테스트만으로는 부족하다"는 교훈을 반복적으로 확인하며 발전해 온 과정이기도 합니다.
