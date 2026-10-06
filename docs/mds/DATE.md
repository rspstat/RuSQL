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

> **번복 (2026-10-01)**: 이 결정(NL→SQL 파인튜닝을 캡스톤 AI 센터피스로 확정)은 번복되어 사설 모델 기능은 제품 범위에서 제외됐다 — 아래 10월 1일 "AI 방향 최종 정리" 항목 참고.

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

### 9월 16일~18일 — Colab Pro에서 첫 실제 LoRA/QLoRA 파인튜닝 실행 + 실패 케이스 분석으로 데이터 생성 버그 3건 발견·수정

9월 12일~13일에 작성해둔 `rusql_nl2sql_finetune.ipynb`를 사용자가 실제로 Colab Pro에서 실행하며 겪은 문제들을 세션 내내 그때그때 진단·수정: (1) pip 설치 경고를 에러로 오인한 것으로 판명(실제 문제 아님), (2) `bitsandbytes==0.44.1`이 `transformers==4.46.3`과 버전 불일치로 4bit 양자화 `ImportError` → `0.46.1`로 상향, (3) Google Drive 연동 경로를 `내 드라이브/projects/RuSQL`로 재구성(데이터셋·출력 디렉터리를 Colab 휘발성 `/content/`가 아니라 `MyDrive/projects/RuSQL/dataset`·`.../rusql-nl2sql-lora`로), (4) 그 과정에서 반복 재현된 `FileNotFoundError: /content/train.jsonl`은 실제로는 사용자가 이전 커밋 이전 상태의 Colab 탭을 계속 재사용한 클라이언트 측 문제였음(GitHub main 기준 새 탭으로 열도록 안내해 해결), (5) T4(16GB)에서 배치 크기 8·max_seq_len 1024로 학습 시 CUDA OOM(Qwen2.5의 큰 vocab 때문에 loss 계산의 logits 텐서가 큼) → `BATCH_SIZE` 8→2 + `GRAD_ACCUM` 2→8(실효 배치 동일) + `MAX_SEQ_LEN` 1024→512 + `gradient_checkpointing=True`+`model.enable_input_require_grads()`로 해결.

첫 실제 학습 결과: held-out 3개 스키마(`real_estate`/`airline`/`insurance`) 572문항 기준 **정확 일치 87.9%**. 평가 셀에 도메인별/언어별 실패율 breakdown을 추가해 재분석한 결과(영어 실패율 2.8% vs 한국어 21.7%로 큰 격차) 실패 상당수가 모델이 아니라 **데이터 생성기 자체의 모호성 버그**로 드러남 — 자세한 내용은 `AI.md` 3번 항목 참고. 3가지 다 고치고(`term_dict.py`의 `property`/`listing` 라벨 충돌, `flight`의 FK 2개(`origin_id`/`destination_id`)가 같은 참조 테이블을 가리킬 때의 문구 모호성, `gen_order_no_limit`의 정렬 방향 누락) 데이터셋을 재생성(train 2993/val 157/test **568**, 총 3718행 — 최초 3722행에서 소폭 변동).

재생성된 데이터셋으로 재학습한 결과(같은 날 세션 내): held-out 568문항 기준 **정확/정규화 일치 93.7% (532/568)**로 대폭 개선 — `airline` 실패율 7.0%→1.9%, `real_estate` ~14.5~15.1%→1.1%로 거의 해소. `insurance`만 13.6%로 거의 그대로인데, 실패 대부분이 `policy`↔`claim` 테이블 혼동(두 한글 라벨이 명확히 다름에도 발생)이라 데이터 버그가 아니라 모델의 진짜 일반화 한계로 결론. 상세 수치·실패 예시는 `AI.md`가 canonical.

### 9월 19일 — MCP에 Connections 관리 도구 3개 추가 (list/add/delete_connection)

사용자가 "Claude가 모든 버튼을 다 누를 수 있게" 만들 수 있는지 질문 — 대부분의 버튼은 결국 SQL문 하나라 이미 `execute_sql`로 가능하고, ERD 줌/탭 전환 같은 순수 UI 동작은 자연어로 조작할 실익이 없으며, 무엇보다 예전에 정확히 이런 시도(UI 제어용 도구 9개, Phase 17에서 전부 가짜였음이 밝혀져 제거)가 있었던 전례를 근거로 반대 — 대신 실제로 가치 있고 지금 비어있는 한 가지, **홈 화면의 저장된 Connections 목록 관리**로 스코프를 좁히자고 역제안, 사용자 승인.

기술적 난제: Connections 목록은 웹뷰 `localStorage`에 있고 MCP 서버(별도 Python 프로세스)는 그 프로세스에 접근할 방법이 전혀 없음 — 둘 사이에 다리가 아예 없었음. `code/data/connections.json` 파일을 새 공유 저장소로 도입해 해결:

- **`main.rs`**: `get_connections`/`save_connections` Tauri 커맨드 신규 추가(파일 없으면 빈 배열, 저장은 임시 파일+원자적 rename으로 절반만 쓰인 JSON 방지 — WAL 원자적 재작성과 동일한 이유). 기존 localStorage 기반 `loadConnections`/`saveConnections`를 파일 기반으로 교체하면서, 파일이 비어있으면(예전 설치) 기존 localStorage 내용을 1회 이관하는 마이그레이션도 같은 자리에 통합(기존 dataDir 마이그레이션·고아 디렉토리 정리 로직과 순서가 꼬이지 않도록 한 `useEffect`로 합침). 로그인 전(홈 화면) 상태에서는 3초 간격으로 파일을 다시 읽어, 앱을 재시작하지 않아도 MCP가 방금 추가/삭제한 연결이 화면에 반영되게 함.
- **`mcp_server.py`**: `list_connections`(비밀번호는 항상 제외하고 반환) · `add_connection`(name/host/port/user/password 전부 필수 파라미터로 선언 — Claude가 대화 중에 먼저 물어보게 강제) · `delete_connection`(id 우선 매칭, 이름은 유일할 때만 삭제하고 겹치면 후보 id 목록을 반환해 안전하게 거부) 3개 신규. 같은 파일을 직접 읽고 쓰며, 쓰기는 Rust 쪽과 동일하게 임시 파일+원자적 교체.
- **검증**: `cargo build --release`/`cargo test --release`(3/3)/`tsc --noEmit`/`vite build` 전부 클린. `mcp` 패키지가 로컬에 없어 `FastMCP`를 스텁으로 대체해 `mcp_server.py`를 직접 import하는 방식으로(Phase 48과 동일 기법) 7가지 시나리오(빈 목록, 추가, `list_connections`의 비밀번호 제외 확인, 이름으로 삭제, id로 삭제, 이름 중복 시 안전 거부, 존재하지 않는 id/이름 처리) 전부 실제 로직으로 통과 확인. 실제 `code/data/connections.json` 경로에 대해서도 추가→삭제 왕복 확인(테스트 잔여물 없이 정리). 개발 모드로 띄워둔 실제 앱이 Rust 변경을 자동 재빌드하는 것도 확인.

### 9월 19일 — MCP 쿼리 에디터 UI 제어 부활 (write_to_editor / 탭 조작 / execute_in_editor)

Connections 관리 기능을 마친 뒤, 사용자가 이어서 요청: (1) VS Code처럼 탭을 드래그해서 순서를 바꿀 수 있게, (2) Claude가 쿼리 입력·탭 조작·쿼리 실행도 UI 조작으로 할 수 있게, (3) "예전에 있었던 것 같은데 삭제된 UI 조작 기능"을 다시 살려달라 — 이 세 번째 요청이 정확히 Phase 17에서 제거됐던 그 기능을 가리킴.

먼저 새 폴링 방식(`invoke("get_pending_ui_commands")`를 1초마다 호출)으로 구현을 시작했다가, `App.tsx`에 이미 존재하던 죽은 코드를 발견: `uiCmdHandlerRef`(액션 디스패처를 담은 ref, `runQueryRef`와 동일한 stale-closure 방지 패턴)와 `listen("ui-command", ...)` 이벤트 리스너가 `write_to_editor`/`new_tab`/`execute_in_editor`/`close_tab`/`switch_to_tab`/`refresh_sidebar`를 이미 전부 처리할 수 있게 구현돼 있었음 — 다만 이 `"ui-command"` 이벤트를 emit하는 쪽이 어디에도 없어서 죽어있었을 뿐(Phase 17에서 제거된 `_run_ui`가 SQL TCP 포트로 문자열을 보내는 방식이었던 흔적으로 추정, 그 서버 쪽만 제거되고 프런트 쪽은 안 치워졌던 것). 새로 짠 폴링 코드를 지우고, 이 기존 디스패처를 실제로 살리는 방향으로 설계를 바꿈:

- **`main.rs`**: `UiCommand{id, action, params, status, result}` 구조체 + `code/data/ui_commands.json` 큐(읽기/쓰기 헬퍼는 Connections와 동일한 임시 파일+원자적 rename 패턴). 앱 시작 시(`setup`) 배경 스레드를 하나 띄워 400ms 간격으로 이 큐를 폴링 — `pending` 항목을 찾으면 `in_progress`로 바꾸고 `app_handle.emit("ui-command", {id, action, data})`로 프런트에 전달(`data`는 문자열이면 그대로, 객체면 JSON 텍스트로 직렬화해 프런트가 필요시 `JSON.parse`). `complete_ui_command` Tauri 커맨드는 프런트가 처리를 끝낸 뒤 같은 항목에 결과를 채워 `done`으로 바꿈. 처음 만들었던 `get_pending_ui_commands`(프런트 폴링용)는 이 이벤트 기반 설계로 대체되며 안 쓰게 돼 삭제.
- **`App.tsx`**: 기존 `uiCmdHandlerRef`를 확장 — `id`를 받아 처리 후 `invoke("complete_ui_command", {id, result})`로 결과를 돌려주도록 모든 액션 분기에 `finish(...)` 호출 추가. `execute_in_editor`는 기존의 `setTimeout(..., 200)` 실행 후 결과를 안 돌려주던 방식에서, 실행 후 실제 `tabResults`를 읽어 JSON으로 반환하도록 개선. `write_to_editor`는 특정 탭을 지정할 수 있도록(`{tab, content}`) 확장. `list_tabs`/`get_tab_content` 두 조회용 액션도 같은 디스패처에 추가.
- **동시성 버그 발견 및 수정** (모두 실제 테스트로 재현 후 수정, 가정으로 넘어가지 않음):
  1. 이 큐 파일은 Rust 배경 스레드(400ms)와 별도 Python 프로세스(200ms) 양쪽이 자주 쓰기 때문에, 고정된 이름의 임시 파일(`ui_commands.json.tmp`)을 공유하면 두 쓰기가 겹칠 때 한쪽의 rename이 상대가 이미 지운 파일을 찾다 `FileNotFoundError`/`PermissionError`가 나는 경합을 실제로 재현 — 양쪽 다 호출마다 고유한 임시 파일명(pid+스레드id+타임스탬프)을 쓰도록 수정.
  2. 그걸 고친 뒤에도, 두 MCP 도구 호출이 거의 동시에 큐에 항목을 추가하면 "읽고-고치고-통째로 다시 쓰기"가 원자적이지 않아 나중에 쓰는 쪽이 먼저 쓴 쪽의 항목을 통째로 덮어써 사라지는 경합을 멀티스레드 테스트로 재현(둘 중 하나가 응답을 영영 못 받고 자기 타임아웃까지 멈춰있었음) — `code/data/ui_commands.json.lock`(`O_CREAT|O_EXCL`로 만드는 락 파일 자체를 뮤텍스로 사용)로 Rust·Python 양쪽의 read-modify-write 구간을 직렬화해 해결. 두 프로세스가 동일한 락 파일명 규칙을 공유하므로 언어와 무관하게 서로 잠금.
- **`mcp_server.py`**: `write_to_editor`/`new_editor_tab`/`close_editor_tab`/`switch_editor_tab`/`list_editor_tabs`/`get_editor_tab_content`/`execute_in_editor` 7개 신규 도구 — 전부 위 큐에 pending 항목을 넣고 최대 20초 폴링해 `done` 결과를 기다리는 공통 헬퍼(`_send_ui_command`) 사용, 타임아웃 시 큐에서 자동 제거(응답 포기한 호출이 나중에 앱이 켜졌을 때 뒤늦게 처리되는 것 방지).
- **검증**: `cargo build/test --release`, `tsc --noEmit`, `vite build` 전부 클린. `mcp_server.py`는 Phase 48/9월 19일 Connections 작업과 동일한 스텁 기법으로 9가지 시나리오(타임아웃+큐 정리, 개별 액션 6종의 라운드트립과 파라미터 모양, 오래된 완료 항목 정리, 동시 호출 2개 격리) 3회 연속 반복 통과 확인. 여기서 그치지 않고 **실제로 켜져 있던 개발 모드 앱**(사용자가 직접 로그인)을 대상으로 `code/data/ui_commands.json`에 직접 명령을 써 넣는 방식으로 `list_tabs`·`execute_in_editor`(실제 쿼리 실행 결과 반환 확인)·`new_tab`·`write_to_editor`·`get_tab_content`·`switch_to_tab`·`close_tab` 7개 액션을 실제 화면에 반영되는 것까지 라이브로 확인 — Phase 17이 "그럴듯해 보이지만 실제로는 응답하는 쪽이 없었던" 실패였던 것과 달리, 이번엔 파일을 통해 실제 앱 창의 탭이 생기고 사라지고 쿼리가 실행되는 것을 직접 확인.

VS Code 스타일 탭 드래그 순서 변경(요청의 1번 항목)은 같은 세션에서 별도로 완료 — 메인 탭바 안에서의 순서 변경만 지원(스플릿 뷰는 탭 1개만 지원하는 기존 구조라 드래그로 스플릿에 넣는 것은 범위 밖으로 명시적으로 제외).

### 9월 19일 — 탭 드래그가 실제로는 동작 안 함 발견 + 네이티브 HTML5 DnD → 마우스 이벤트 기반으로 전면 교체, Connections 드래그 재정렬 신규 추가

사용자가 방금 완료한 탭 드래그 순서 변경이 실제로는 동작하지 않는다고 리포트. 코드만 읽어서는 로직 자체(`draggable`/`onDragStart`/`onDragOver`/`onDrop`)에 문제가 없어 보였는데, 이게 바로 함정 — Phase 53과 같은 교훈으로, tsc 통과와 "로직이 맞아 보임"은 실제 동작을 보장하지 않는다는 걸 다시 확인.

원인을 코드 검토만으로 확신할 수 없어, 사용자 동의를 받고 떠있던 개발 모드 앱을 `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9223`로 재시작 후 Chrome DevTools Protocol에 raw WebSocket으로 직접 붙어(`websocket-client`/`websockets` 둘 다 로컬에 없어 소켓+HTTP 업그레이드 핸드셰이크부터 직접 구현한 최소 CDP 클라이언트) `Input.dispatchMouseEvent`로 실제 마우스 press→move(여러 스텝)→release 시퀀스를 재현해 디버깅. 놀랍게도 이 합성 마우스 이벤트로는 네이티브 HTML5 드래그가 실제로 동작했음 — 즉 로직 자체는 맞았지만, Chromium이 CDP 합성 입력을 처리하는 경로와 WebView2에서 실제 물리 마우스가 트리거하는 네이티브 OS 드래그 세션 경로가 다를 수 있어(자동화 테스트에서 종종 보고되는 WebView2/Electron류의 알려진 간극), 진짜 사용자 마우스로는 안 될 가능성이 남음. 근본 원인을 100% 특정하기보다, 브라우저/webview에 무관하게 항상 동작이 보장되는 방식으로 아예 교체하는 쪽을 택함.

**네이티브 HTML5 DnD(`draggable`+`onDragStart`/`onDragOver`/`onDrop`/`onDragEnd`) 완전 제거, 순수 `mousedown`/`mousemove`/`mouseup` 기반 커스텀 드래그로 교체** — 공용 헬퍼 `startReorderDrag(e, id, containerSelector, onDragState, onReorder, justDraggedRef)`를 새로 작성해 탭 재정렬과 (아래) Connections 재정렬 둘 다 하나의 구현을 공유:
- `mousedown` 시점엔 아직 드래그로 취급하지 않고, 4px 이상 움직여야(threshold) 비로소 "드래그 중"으로 전환 — 그 전까지는 그냥 클릭으로 남아 기존 `onClick`(탭 전환/연결 열기)이 그대로 동작.
- 드래그 중엔 `document.elementFromPoint(x, y)`로 커서 아래 어느 항목인지 찾아 `dragOverId` 갱신(각 항목에 `data-drag-id` 속성 부여).
- `mouseup` 시 실제로 드래그가 있었으면 재정렬 콜백 호출 + `justDraggedRef`를 한 틱만 세워 뒤이어 발생하는 `click` 이벤트가 `switchTab`/연결 열기를 잘못 트리거하지 않게 막음(마우스다운·업 타깃이 같으면 이동 거리와 무관하게 `click`이 항상 발생하는 브라우저 특성 때문에 필요).

**Connections 드래그 재정렬 신규 추가** (요청 2번 항목) — 홈 화면의 사이드바 목록(`.home-sidebar-item`)과 카드 그리드(`.home-conn-card`) 둘 다, 같은 `connections` 배열을 그리므로 `reorderConnections(fromId, toId)` 하나만 공유해서 적용. 어느 쪽에서 순서를 바꾸든 `saveConnections`를 통해 `code/data/connections.json`에 저장되므로 두 뷰가 항상 같은 순서를 보여줌. 그리드 카드는 우상단 삭제(✕) 버튼 위에서 시작한 마우스다운은 드래그로 취급하지 않도록 제외.

**라이브 검증** (전부 CDP로 실제 합성 마우스 press/move/release 시퀀스 재현, 코드만 보고 넘어가지 않음):
- 탭: query1.sql → query.sql 위치로 드래그해 실제 DOM 순서가 바뀌는 것 확인, 이어서 순수 클릭(이동 없음)으로 다른 탭 전환도 정상 동작 확인.
- Connections 사이드바: 두 항목 드래그로 순서 변경 → `code/data/connections.json` 파일에 실제로 반영됨을 파일 읽기로 직접 확인.
- Connections 그리드: 사이드바와 같은 배열을 공유하므로 순서가 자동으로 동기화되는 것 확인, 반대 방향 드래그로 원래 순서 복원, 이후 순수 클릭이 연결 다이얼로그를 정상적으로 여는 것까지 확인(드래그 판정이 클릭을 막지 않음을 검증).

`tsc --noEmit`/`vite build` 클린. Rust 쪽 변경 없음(순수 프런트엔드 수정).

### 9월 21일 — AI 로컬 추론 배포 계획 철회, AI 트랙 최종 스코프를 파인튜닝/평가까지로 확정

로컬 추론 통합(로드맵 4번 항목)을 진행하려다, 사용자가 먼저 근본 질문을 던짐: "이거 굳이 추가해야 되나? MCP 기능으로 다 되잖아." 이어서 "심사자(교수님)는 AI 모델보다 이 RDBMS 자체의 성능·기능 지원 범위에 더 관심 가질 것 같다"는 판단을 제시.

논의 결과 로컬 추론 배포를 철회하기로 결정. 근거:
- RuSQL은 이미 `DIFF.md` 기준 MySQL/PostgreSQL/Oracle 대비 18개 카테고리·약 230줄 비교표와 실측 벤치마크(인덱스 조회 45.8배 등)를 갖춘 성숙한 RDBMS라, 심사가 RDBMS 자체를 더 본다면 남은 기간을 그쪽에 쓰는 기대값이 더 높음.
- MCP 서버가 이미 자연어 조작 + UI 제어(2026-09-19 완료)까지 지원해서, 로컬 추론을 앱에 추가로 배포해도 실사용 가치가 크지 않음.
- held-out 93.7%는 최상 조건 수치이고 한국어 실패율(12.0%)이 영어(0.7%)보다 훨씬 높아, 발표 현장 즉석 질문에 소형 양자화 모델이 틀린 SQL을 낼 라이브 데모 리스크가 있음 — RDBMS 데모보다 오히려 역효과일 수 있음.

**AI 트랙은 "합성 데이터 생성 → LoRA 파인튜닝 → held-out 평가(93.7%)"까지로 최종 확정**하고, 로컬 추론 통합·평가/스코프 방어(로드맵 4·5번) 관련 계획은 전부 폐기. `docs/mds/AI.md`에 철회 사유와 함께 전체 수정 반영(canonical). 남은 개발 기간은 RDBMS 견고화·벤치마크·데모 준비로 재배분하기로 함 — 단, 데모 준비는 아직 이르다고 판단해 보류, 우선 RDBMS 쪽 항목부터 순서대로 진행 예정.

### 9월 21일 — MCP 기능 현황 점검 후 6개 항목 추가 (Connection 수정, 위험 SQL 안전장치, 긴 쿼리 타임아웃, 다중 앱 인스턴스, 앱 실행/로그인, 서버 시작/중지) — 17개 → 25개 도구

사용자가 "지금 MCP 기능에서 되는 것, 안되는 것 뭐가 있음"이라고 질문 — 현재 17개 도구를 되는 것/안되는 것으로 정리해 보고한 뒤, 안되는 것 중 6개(서버 시작/중지, 앱 실행/로그인, Connection 수정, 위험한 SQL 안전장치, 20초 타임아웃보다 오래 걸리는 쿼리, 다중 앱 인스턴스)를 전부 추가해달라는 요청을 받음.

시작 전 설계 확인 두 가지: (1) 위험한 SQL 처리 방식 — "거부 + 별도 confirm 도구" 선택(영향받는 행 수 미리보기 방식 대신); (2) 다중 앱 인스턴스의 실제 필요 상황 — "여러 DB에 동시 접속해서 쓰는 것" 확인. 서버 시작/중지는 묻지 않고 바로 설계 방향을 정함: 코드에 이미 "같은 data_dir을 두 프로세스가 동시에 열 수 없다"는 제약이 있어(Phase 52의 LOCK TABLES 경험과 같은 종류의 교훈), MCP가 `engine_server.exe`를 독자적으로 띄우면 안 되고 반드시 이미 떠있는 앱의 기존 Start/Stop 로직을 그대로 호출해야 한다고 판단.

구현한 것:
- **Connection 수정** (`update_connection`) — id 우선, 없으면 유일한 이름으로 매칭(기존 `delete_connection`과 동일 규칙); 준 필드만 갱신.
- **위험한 SQL 안전장치** (`confirm_dangerous_sql`) — 정규식 휴리스틱으로 `DROP`/`TRUNCATE`, `WHERE` 없는 `UPDATE`/`DELETE`를 감지해 `execute_sql`/`execute_in_editor`가 실행 전에 거부; 사용자 확인 후 `confirm_dangerous_sql`을 명시적으로 다시 호출해야만 실행됨(Claude가 판단만으로 우회 불가). 실제 엔진에 `CREATE`→`INSERT`→`DELETE ... WHERE`(정상 실행)→`DROP TABLE`(거부 확인)→`confirm_dangerous_sql`로 실제 DROP까지 라이브로 검증.
- **긴 쿼리 타임아웃** — `execute_in_editor`에 `timeout_seconds`(최대 600초) 파라미터 추가, 기본값도 20초→30초로 소폭 상향.
- **다중 앱 인스턴스** (`list_app_instances`) — 각 인스턴스가 `code/data/app_instances.json`에 자기 `id`/`pid`/로그인 여부/현재 DB를 배경 스레드(main.rs, ~2초 간격)로 하트비트, 15초 이상 끊기면 다음 기록 때 정리. 모든 UI 제어 도구 + login/서버 도구에 `instance` 파라미터 추가(`UiCommand`에 `target_instance` 필드, 비어있으면 기존처럼 아무 인스턴스나 처리 — 하위 호환).
- **앱 실행/로그인** (`launch_app`, `login`) — `launch_app`은 실행 중인 인스턴스가 없을 때만 `RUSQL_APP_PATH`(setup_mcp_config가 `std::env::current_exe()`로 자동 기록)로 새 프로세스 스폰. `login`은 저장된 Connection으로 로그인 전 홈 화면의 창을 로그인시킴 — 여기서 진짜 설계 문제를 하나 발견: 기존 `"ui-command"` 이벤트 리스너가 `[loggedIn]`에 의존해 로그인 후에만 등록되고 있어서, 로그인 전 명령(`login` 자신)을 받을 방법이 원천적으로 없었음. 리스너를 앱 시작 시부터 항상 등록하도록 바꾸고, 로그인 게이트를 리스너 자체가 아니라 디스패처 안의 액션별 분기(`login`만 예외)로 옮겨서 해결.
- **서버 시작/중지** (`start_server`/`stop_server`/`get_server_status`) — Server Manager 탭의 공개 리스너(네이티브 + 선택적 MySQL)를 `ui_commands.json` 큐를 통해 이미 로그인된 세션의 `connId`로 그대로 제어. 별도 프로세스를 새로 띄우는 게 아니라 앱이 이미 갖고 있는 로직을 그대로 재사용하므로 위에서 우려한 data_dir 충돌 리스크 자체가 없음.
- `setup_mcp_config`의 `alwaysAllow` 목록이 최초 7개 도구 기준 그대로 방치돼 있던(Connections/UI 제어 도구 10개가 전부 매번 권한 팝업이 뜨고 있었던) 걸 이번에 같이 발견해 수정 — 파괴적이거나 재확인이 필요한 도구(`delete_connection`/`update_connection`/`confirm_dangerous_sql`/`stop_server`)는 의도적으로 빼서 Claude Desktop 자체 팝업이 마지막 방어선으로 남게 함.

**검증**: `cargo build/test --release`(3/3), `tsc --noEmit`, `vite build` 전부 클린. 기존 9-시나리오 큐 회귀 테스트 재실행해 하위 호환 확인. 그 외 전부 실제로 켜져 있던 앱을 대상으로 라이브 검증(정규식 유닛 테스트가 아니라) — 홈 화면에서 `login` 액션으로 실제 로그인 성공(`app_instances.json`의 `loggedIn`이 `false`→`true`로 실제로 바뀌는 것까지 확인), `start_server`→`get_server_status`(실행 중 확인)→`stop_server` 전부 실제 포트에서 성공, `launch_app`은 "이미 떠있으면 무시" 분기와 "떠있는 인스턴스가 없으면 실제로 새 프로세스 스폰" 분기 둘 다 확인(후자는 실제 앱 대신 `notepad.exe`로 대체 검증 — 앱 자체가 아니라 spawn 로직만 검증하면 되므로), `update_connection`/`list_app_instances`도 실제 파일 대상으로 확인. 테스트에 쓴 임시 Connection·데이터 디렉터리·큐 항목은 전부 정리, 테스트 도중 강제 종료로 생긴 고아 `engine_server.exe` 프로세스(정상 로그아웃이 아니라 강제 종료라 Drop이 안 돌아서 발생)도 발견해 정리.

---

> **후속 (2026-10-01)**: 로컬 추론 *배포*만 철회했던 이 결정을 파인튜닝 결과물 자체로 확대 — 사설 모델은 제품 범위에서 제외, 아래 10월 1일 "AI 방향 최종 정리" 항목 참고.

### 9월 28일 — SSI 재검토: 그래프 없이도 이미 정확함을 실측 검증 + predicate lock V1 갭 3개 마무리

"큰 작업 하나 해서 뭘 하나 이루고 싶다"는 요청으로 SSI(Serializable Snapshot Isolation)를 다시 살펴봄. XA/페이지 단위 버퍼 풀/WAL 복제는 이미 범위 밖으로 결정된 항목이라 후보에서 제외하고, SSI만 유일하게 기존 결정과 안 부딪히는 데다 이미 절반은 있어(read-set 자체검증 + predicate lock) "완성"에 가까운 작업으로 판단.

코드를 다시 훑다가, 처음 세웠던 "conflict-graph 기반 pivot(위험 구조) 탐지를 새로 구현해야 한다"는 계획이 잘못된 전제였음을 발견: 트랜잭션 T가 커밋 시점에 "자기가 읽은 행이 이미 커밋된 다른 트랜잭션에 의해 바뀌었는가"만 재검증하는 현재 방식(`validate_serializable`)이, 별도 그래프 구조 없이도 이미 이론적으로 충분함을 직접 증명(사이클 T1→T2→T3→T1의 세 rw-엣지가 전부 "쓰는 쪽이 읽는 쪽보다 먼저 커밋해야 캐치됨" 조건이라, 셋 다 동시에 못 캐치되려면 커밋 순서에 모순(T1<T2<T3<T1)이 생겨 불가능 — 즉 사이클 안에서 반드시 누군가 마지막에 커밋하고, 그 자체검증이 항상 잡아냄). 말로만 끝내지 않고 실제로 3-트랜잭션 rw-반의존 순환 테스트(`test_concurrency.cpp`)를 새로 작성해 실행 — 증명대로 정확히 마지막 커밋 트랜잭션이 "Serialization failure"로 실패함을 확인. **코드 변경 없이 검증만으로 끝.**

그래서 실제 작업 범위는 predicate lock(phantom 탐지)에 이미 주석으로 명시돼 있던 "V1 갭" 3개로 좁혀짐:
- **집계 쿼리** — `SELECT COUNT(*) FROM t WHERE ...` 같은 집계 쿼리는 `has_agg` 블록이 predicate-lock 등록 코드에 도달하기 전에 먼저 리턴해버려서, SERIALIZABLE인데도 팬텀 탐지가 완전히 무방비였음. 집계 처리 직전에 동일한 등록 로직 추가.
- **복합/무-PK 테이블** — 범위를 `[lo, hi]`로 좁히려면 단일 컬럼 PK가 필요한데, 없으면 predicate 등록 자체를 통째로 건너뛰고 있었음. 좁힐 수 없을 땐 "테이블 전체"를 predicate로 등록하는 폴백으로 변경 — 그런데 이것만으론 부족했던 게, INSERT 쪽 팬텀 체크 자체가 `gap_pk_col_count == 1` 조건 안에 갇혀 있어서 복합 PK 테이블은 등록을 해도 검사를 안 하고 있었던 걸 추가로 발견(`executor_dml.cpp`). 그 체크를 조건 밖으로 꺼내 PK 값 없이도(등록된 predicate가 무제한 범위면 항상 매치) 동작하도록 재구성.
- **JOIN** — 조인된(구동 테이블이 아닌) 테이블에는 predicate가 아예 등록되지 않아 그쪽 팬텀은 완전히 안 잡혔음. 구동 테이블 + 조인된 모든 테이블에 predicate를 등록하도록 확장(각 테이블 전체를 보수적으로 보호 — 정밀한 병합 컬럼 추적 대신 안전한 쪽 선택).

**하나는 만들다가 되돌림**: 비-PK 컬럼 조건(`WHERE val = 5`)으로 스캔한 행이 UPDATE로 나중에 그 조건에 새로 걸리는 경우(INSERT가 아니라 기존 행의 값 변경)도 같은 방식으로 잡아보려 했는데, 붙이자마자 방금 만든 3-트랜잭션 pivot 테스트가 깨짐. 원인: 이 체크가 "이미 존재하는 행에 대한 어떤 쓰기든" 무조건 즉시 플래그하는 방식이라, `validate_serializable`이 커밋 순서를 존중해 정교하게 처리하던 케이스(T1이 읽은 행을 T2가 나중에 쓰지만 T2가 아직 커밋 전인 상황)까지 성급하게 실패시켜버림 — 고치려던 것보다 부작용이 더 커서 되돌리고, 이 케이스는 정직하게 "아직 안 잡힘"으로 문서에 남김(`FUNCTIONS.md` Predicate Lock 항목 참고).

신규 회귀 테스트 4건(3-트랜잭션 pivot 검증 1 + predicate lock V1 갭 3개) 추가.

**결과**: 397 케이스 / **22,703 assertions** 전부 통과(392/22,635에서 +5케이스/+68assertion). git: `d41fe03`.

### 9월 29일 — 명시적 트랜잭션 "성능 이상" 조사 → 이상이 아니라 비대칭 비교였음 + 트랜잭션 로깅 2.8배 개선, MEDIAN·DESC 인덱스 문법 추가

이전에 벤치마크로 발견했던 "BEGIN/COMMIT이 autocommit보다 ~78배 느림"을 조사. 결과부터: **이상 현상이 아니라 비교 자체가 비대칭**이었음. 실측(`engine_server`+bench 클라이언트, 1,000건): autocommit 0.16초 / 건당 BEGIN·INSERT·COMMIT 12초 / 한 트랜잭션에 INSERT 1,000건 0.56초. 그리고 서버를 강제 종료하는 실험으로 더 중요한 사실을 확인 — **autocommit INSERT는 디스크에 쓰지 않는다**(`txn.log_insert`가 트랜잭션 밖에선 no-op이고 buffer pool에 dirty 표시조차 안 함 → 응답은 OK인데 크래시하면 5행이 0행으로 사라짐). 즉 autocommit이 빠른 건 내구성이 없어서이고, 명시적 COMMIT의 ~12ms는 실제로 내구성 있는 커밋의 비용(테이블 통째 원자적 재작성 tmp+fsync+rename ≈3ms, WAL COMMIT fsync ≈2~7ms, WAL/undo 파일 정리). 사용자 결정으로 autocommit 내구성 변경(WAL 기록+그룹 커밋)은 범위 밖으로 두고 COMMIT 경로만 최적화하기로 함 — 다만 autocommit 미내구성은 **알려진 한계**로 문서화.

계측(임시 코드로 COMMIT 단계별 시간 측정 후 전부 제거)으로 찾은 실제 병목은 커밋이 아니라 **트랜잭션 안의 문장당 로깅**: WAL·Undo 레코드마다 파일을 열고(`fopen "ab"`)·쓰고·닫아서 레코드당 0.184ms(파일시스템 메타데이터+Defender 실시간 검사), 핸들을 유지하면 0.004ms(46배). 문장 하나가 두 파일에 각각 쓰므로 트랜잭션 내 INSERT가 autocommit의 3.5배였음. 수정: `TxnIoShared`가 WAL/Undo 추가 전용 핸들을 세션 공유로 유지(기존 락으로 보호, 레코드마다 `fflush`·COMMIT은 여전히 fsync라 내구성 의미 동일). Windows는 열린 파일을 삭제·덮어쓰기(rename) 못 하므로 삭제/truncate/원자적 교체 경로 전부(clear, remove_txn, truncate_to_last_checkpoint, Undo의 clear/remove_txn/rewrite_txn)에서 먼저 핸들을 닫도록 함.

**결과**: 한 트랜잭션에 INSERT 1,000건 0.56초 → **0.20초(2.8배)**, 건당 BEGIN/COMMIT은 fsync 지배라 그대로(~12~14ms, 이 PC는 Defender 실시간 검사·백그라운드 게임 프로세스 때문에 회차별 6~21ms로 변동이 커 A/B 반복 측정 필요했음). `bench.py`에 "1개 트랜잭션 N건 INSERT" 지표(`txn_batch_s`)를 추가 — 기존 "건당 BEGIN/COMMIT"만으론 오해를 부르는 수치였음.

**SQL 기능 갭 저비용 항목**: `MEDIAN(col)`(PERCENTILE_CONT(0.5), 기존 집계 패턴 재사용 — ARRAY_AGG와 같은 지점들 수정)과 `CREATE INDEX ... (col DESC)`(컬럼별 ASC/DESC 문법 수용, 이전엔 "Expected ')'" 파싱 에러). 원래 후보였던 PERCENTILE_CONT/DISC 전체는 AggFunc에 매개변수(분위수)와 `WITHIN GROUP` 문법이 필요해 범위가 커서 MEDIAN만. DESC 인덱스도 실제 내림차순 저장이 아니라 문법 수용(B+Tree는 항상 오름차순)이라는 점을 코드 주석·문서에 명시. UUID는 이미 `UUID()` 함수가 있어(타입만 없음) 제외. `DIFF.md`에서 이미 구현돼 있는데 ✗로 남아 있던 3행(ARRAY_AGG, LOCK TABLES, REPLACE INTO)도 바로잡음(전날 문서 최신화에서 놓쳤던 것).

**테스트/검증**: 신규 4케이스(MEDIAN 홀수·짝수·NULL·빈 집합·GROUP BY, DESC 인덱스 결과 동일성·문법 오류 거부, 25회 커밋/롤백 반복과 핸들 재사용, 다른 세션 레코드가 남은 채 재작성되는 경로). Debug+Release **401 케이스/22,896 assertions** 통과, `test_full.sql`/`test_full-ver2.sql` Debug CLI 실행 결과가 변경 전 CLI와 에러 수 동일(3/2, 차이는 시각·소요시간뿐), 실서버 크래시 복구 라이브 검증(커밋된 2행 유지·미커밋 2행 롤백, 커밋 후 강제 종료 시 3행 유지).

### 10월 1일 — Redo 로그 기반 내구성: autocommit 내구성 확보 + 커밋당 테이블 전체 재작성 제거 (+ 크래시 퍼저가 잡은 버그 3건)

9/29에 "알려진 한계"로 남겨둔 두 문제(autocommit이 "OK" 후 크래시하면 유실 / COMMIT마다 테이블 전체를 JSON 직렬화+원자적 재작성+fsync)를 하나의 작업으로 묶어 해결.

**설계**: COMMIT과 autocommit 문장이 "테이블 파일 재작성" 대신 **커밋된 변경만 기술한 작은 redo 배치를 `rusql.redo`에 추가 + 그룹 커밋 fsync 1번**으로 내구성을 확보하고, 테이블 `.rdb`는 체크포인트(로그 4MB 초과/DDL 직전/부팅 복구 직후)에서만 다시 씀. 핵심 난점은 행이 MVCC 버전 체인이라 "SQL을 다시 실행"하는 방식이 불가능하다는 것 — 그래서 연산을 둘로 한정: `InsertVersion`(이미 같은 이미지가 있으면 건너뜀)과 `SetXmax`(그 버전이 살아 있을 때만 `_xmax`를 찍음, 없으면 무시). 버전은 만들어진 뒤 `_xmax` 한 번 외엔 불변이고 그 한 번을 찍는 트랜잭션도 하나뿐이라, 재생이 **멱등이면서 배치 순서와 무관**(INSERT 전부 → SetXmax 전부 순)하게 만들 수 있음 — 행 클레임이 배치 기록보다 먼저 풀려서 파일 순서가 논리적 커밋 순서와 달라질 수 있는 문제를 순서 불변성으로 회피.

기존 코드가 `log_insert/update/delete`로 이미 모든 변경 경로의 이미지를 한 곳에 모으고 있어서 `TransactionManager`가 이를 항상(트랜잭션 밖에서도) redo 연산으로 기록하도록 하고(UPDATE는 undo 항목 1개당 연산 2개라 `redo_marks_`로 `ROLLBACK TO SAVEPOINT`와 길이를 맞춤), autocommit DELETE의 3개 분기에 빠져 있던 `log_delete`를 추가. 감사 결과 단일 테이블 INSERT/UPDATE/DELETE(락 집합이 자기 테이블 하나 = FK 이웃 없음)만 로그 연산으로 완전히 설명되므로 이것만 redo 방식("covered")으로, 그 외(캐스케이드/MERGE/다중 테이블/ON DUPLICATE KEY/REPLACE/INSERT..SELECT, 트리거)는 기존처럼 테이블을 flush(이전엔 autocommit INSERT·MERGE 등은 flush조차 안 해서 유실 — 이제는 flush함).

**크래시 퍼저(`code/test/crash/`)가 잡은 실제 버그 3건** — 이 작업의 핵심 교훈은 "단위 테스트만으로는 못 잡았다":
1. **되살아난 삭제 행**: covered 문장이 남긴 로그와 이후의 레거시 문장(REPLACE의 물리 삭제, ON DUPLICATE KEY의 in-place 수정 — 둘 다 로그에 안 남음)이 flush한 테이블 파일이 서로 모순 → 재생이 삭제된 행을 되살림(`INSERT; REPLACE`만으로 재현). 레거시 flush 직후 `TableFlushed` 마커를 로그에 남기고(테이블 데이터 락 배타 유지 중이라 마커와 스냅샷 사이에 로그된 변경이 끼어들 수 없음, 마커는 fsync) 재생이 그 테이블의 마커 이전 연산을 건너뛰도록 해결.
2. **`SELECT ... FOR UPDATE` 자기교착**: "순수 읽기 전용이 아님"이라 쓰기 문장으로 분류돼 에필로그가 테이블 데이터 락을 배타로 잡으려 했는데, SELECT 계열은 디스패처가 같은 락을 이미 SHARED로 쥐고 있음 → 같은 스레드 교착(전체 스위트가 "SELECT FOR UPDATE requires..." 테스트에서 멈춰서 발견). SELECT 계열은 영속화 대상에서 제외.
3. **미완료 트랜잭션 undo 복구가 존재하지 않던 행을 만들어냄**: 기존 undo 복구는 MVCC 이전 코드라 "옛 이미지 재삽입/첫 행 덮어쓰기"를 맹목적으로 수행 — 변경이 디스크에 이미 반영됐다고 가정. COMMIT이 flush를 안 하게 되자 열린 트랜잭션의 변경은 보통 디스크에 없어서, `BEGIN; UPDATE 518→118; DELETE; (크래시)`가 복구 후 값 118(디스크에 없던 중간 버전)을 만들어냄. 런타임 `apply_rollback`과 같은 MVCC 규칙(자기 txn_id로 만든 버전 제거, 자기가 죽인 버전 되살림)으로 교체 — 변경이 디스크에 있든 없든 멱등.

**성능(이 PC, Defender 실시간 검사로 회차별 변동이 커서 조용할 때 반복 측정)**: autocommit INSERT 0.13ms(내구성 없음) → **0.67ms(내구성 있음, fsync 바닥 0.53ms에 거의 근접)**. BEGIN/INSERT/COMMIT 건당 약 12ms → **약 4ms**. 1,000~20,000행 테이블에서 INSERT·커밋 지연이 평탄(0.66~0.69ms / ~8.6ms 일정)해 커밋 비용의 테이블 크기 의존이 사라짐을 확인. 단건 INSERT/DELETE 10,000건 벤치는 2.8초(내구성 없음)→약 7초(내구성 있음)로 오히려 느려졌고 이는 의도된 비용. 부수 개선: WAL/Undo 파일을 매 트랜잭션 삭제·재생성하던 것을 핸들 유지한 채 제자리 truncate로 변경(파일 생성 메타데이터 비용 제거), 로그 fsync를 매번 파일을 다시 열지 않고 열려 있는 핸들에 직접 수행.

**검증**: 신규 Catch2 12케이스(`test_redo.cpp`: autocommit 3종·명시적/미커밋 트랜잭션·SAVEPOINT 롤백·레거시 문장군·DDL 후 재생 비부활·멱등/찢어진 꼬리·체크포인트 임계 통과·복구 후 인덱스/2차 크래시·모든 문장군 byte 동일 복구·COMMIT이 테이블 파일을 안 건드림·버그 3번 회귀), Debug+Release **412 케이스/23,842 assertions** 통과, `test_full.sql`/`test_full-ver2.sql` 에러 수 변경 전과 동일(3/2). 무작위 `kill -9` 크래시 퍼저(실서버, 확인 응답된 커밋만 남아야 하는 오라클 대조 — covered/레거시/TRUNCATE/ROLLBACK 혼합, 체크포인트 임계를 2.5KB로 낮춰 킬 사이에 체크포인트가 끼게도 함) 순차 총 200+라운드와 4클라이언트 동시 퍼저 총 35라운드(진행 중이던 문장은 반영/미반영 모두 허용, 명시적 트랜잭션은 전부-또는-전무) **불일치 0건**(버그 1~3 수정 전엔 각각 실제로 불일치가 재현됐음).

**정직한 한계** (UPDATE 관련은 바로 아래 항목에서 해소): UPDATE 문은 대상 행을 찾으려고 테이블을 복제·스캔해서 여전히 테이블 크기에 비례(내구성과 무관한 기존 설계 — 20,000행에서 autocommit UPDATE 47ms). 로그 연산으로 완전히 설명되지 않는 문장군은 여전히 문장마다 테이블 flush(O(테이블)). 커밋 약 4ms 중 fsync는 0.5ms 정도이고 나머지는 WAL/Undo 파일 정리(읽기+truncate) 비용이라 추가 개선 여지가 남아 있음. 트랜잭션 id가 autocommit 배치마다 하나씩 소모돼 이전보다 빨리 증가(무해).

### 10월 1일 — UPDATE 스캔 비용 개선: 테이블 복제 제거 + PK 위치 캐시 (+ 테스트 중 발견한 기존 버그 3건)

바로 위 redo 로그 작업에서 "정직한 한계"로 남긴 항목. UPDATE는 대상 행을 찾으려고 테이블 전체(MVCC 버전 체인 포함)를 행마다 `std::map` 복제로 떠낸 뒤 스캔하고 복합 키 문자열을 다시 두 번 계산해서, `WHERE id = 5` 같은 PK 단건 UPDATE도 20,000행에서 약 55ms(행당 약 3µs, 행 수에 선형)였음.

**변경**:
1. **복제 없는 후보 탐색** — 복제가 필요했던 유일한 이유는 서브쿼리 조건이 `exec_select`를 호출하면서 `s.tables`를 흔들 수 있어서였음. 서브쿼리가 없는 조건은 제자리(in-place)에서 매칭하고 **행 위치와 복합 키만** 수집(서브쿼리 조건은 기존 복제 경로 유지). 배타 구간에서 각 위치를 재검증(범위 안 · 여전히 가시 · 같은 복합 키)하고, 하나라도 어긋나면(공유 스캔과 배타 구간 사이에 vacuum/롤백이 행을 밀어낸 경우) 처음부터 재스캔.
2. **`pk = 리터럴` O(1)** — 단일 컬럼 PK의 등호 조건은 `row_pk_pos` 위치 캐시로 후보를 바로 찾음. 캐시는 매번 재검증(위치 범위·가시성·PK 일치)하고 믿을 수 없으면 우회해 전체 스캔, UPDATE가 새 버전을 추가하면 캐시를 갱신(키가 바뀌었거나 우회했던 경우엔 재구축).
3. **자동 VACUUM 임계값 스케일** — `max(고정 임계값, 테이블 행 수/10)`. 큰 테이블에서 수백 건 UPDATE마다 O(테이블) VACUUM이 끼어들어 생기던 지연 스파이크를 분산.

**테스트 중 발견한 기존 버그 3건** (3건 모두 변경 전 바이너리에서 똑같이 재현됨을 확인 — 이번 변경이 만든 게 아님):
1. **다른 세션의 미커밋 UPDATE가 있으면 `SELECT ... WHERE pk = N`이 "0 rows"** — PK 인덱스는 그 미커밋 새 버전을 가리키는데 이 읽기에는 보이지 않으니, "못 찾음 = 없음"으로 단정해 가시한 옛 버전을 놓침(`WHERE v >= 0` 스캔은 정상 — 세션 A `BEGIN; UPDATE t SET v=11 WHERE id=1`, 세션 B `SELECT v FROM t WHERE id=1` → 0행). 미커밋 버전은 명시적 트랜잭션에서만 생기므로 "다른 트랜잭션이 열려 있는가"(`active_txn_ids`)가 정확한 조건 — 열려 있으면 PK 점 조회가 못 찾을 때 일반 스캔으로 폴백하고, 범위/보조/해시/복합 인덱스 경로는 건너뜀.
2. **복합 PK 테이블에서 `WHERE a = 2`(`PRIMARY KEY (a, b)`)가 PK 점 조회로 오인되어 1행만 반환** — 플래너의 `pk_col`이 복합 PK의 첫 컬럼을 단일 PK처럼 돌려줬음. 복합 PK면 `nullopt`.
3. **복합 PK 테이블의 보조 인덱스(B+Tree·해시 모두)에서 한 행을 UPDATE/DELETE하면 PK 첫 컬럼을 공유하는 모든 형제 행이 인덱스 버킷에서 사라짐** — `index_remove_row`가 호출자가 넘긴 첫 PK 컬럼 하나만으로 버킷 항목을 식별. 5×5 복합 PK 표에서 `UPDATE ... WHERE a=2 AND b=4` 한 건 뒤 `WHERE v = 6`이 4행 대신 0행, `DELETE` 한 건 뒤 `WHERE v = 2`가 2행 대신 0행(변경 전 바이너리로 재현). 전체 PK 컬럼으로 식별하도록 수정(`HashIndex`에 전체 키 오버로드 추가, 기존 시그니처는 유지).

**성능** (이 PC, 20,000행 테이블, 문장당 ms, 같은 서버 바이너리 전/후 반복 측정):

| 조건 | 변경 전 | 변경 후 |
|---|---|---|
| `pk = 리터럴` (autocommit) | 55.4 | **0.67** (redo fsync 바닥에 근접) |
| `pk = 리터럴` (명시적 트랜잭션 안) | 50.7 | **0.19** |
| PK 범위(10건) | 53.5 | 7.1 |
| 비 PK 조건 | 51.4 | 4.7 |
| 매치 없음 | 49.3 | 4.4 |

1,000행에서는 `pk = 리터럴` 2.06 → 0.72(트랜잭션 내 1.52 → 0.20) — 변경 후엔 1,000행과 20,000행이 같은 값(테이블 크기 무관).

**검증**: 신규 Catch2 9케이스(`test_update_fast.cpp`, 3,830 assertions): PK 반복/키 변경/무매치/추가 조건, RETURNING·범위·전체 테이블 형태, DELETE·ROLLBACK·자동 VACUUM으로 위치가 밀린 뒤의 UPDATE, **모델 대조 무작위 테스트**(autocommit+트랜잭션 혼합), 복합 PK·보조/해시 인덱스(버그 3번 회귀), 뷰·파티션·트리거 경유, 세션 간 가시성/쓰기 충돌(버그 1번 회귀), 같은 행 동시 증가 유실 없음. Debug+Release **421 케이스/27,672 assertions** 통과, `test_full.sql`/`test_full-ver2.sql` 에러 수 변경 전과 동일(3/2, Debug·Release 모두), 크래시 퍼저(실서버 `kill -9` + 오라클 대조) 순차 50라운드 + 4클라이언트 동시 40라운드(체크포인트 임계 3KB 설정 포함) **불일치 0건**.

**정직한 한계**: `pk = 리터럴`이 아닌 조건(PK 범위·비 PK·복합 PK)은 여전히 테이블 크기에 비례하는 선형 스캔 — 복제가 사라져 약 8~11배 저렴해졌을 뿐(20,000행에서 4.4~7.1ms). 복합 PK 테이블은 위치 캐시를 쓰지 않음. 서브쿼리가 든 조건은 기존 복제 경로 그대로. 그리고 autocommit UPDATE의 0.67ms는 이제 대부분 redo 로그 fsync 비용(= 내구성의 바닥).

### 10월 1일 — AI 방향 최종 정리: 사설 AI(NL→SQL 파인튜닝) 기능 제외, AI는 Claude + MCP로 일원화

사용자가 "자체 파인튜닝한 AI로 자연어→SQL을 하는 기능은 하지 않기로 했다 — 그 기능은 전부 MCP로 해결되고 사설 모델보다 Claude가 더 잘할 것이라 상충한다"고 정리해, 이를 문서와 앱의 Diagram 페이지에 반영.

기록상 이전 상태는 "파인튜닝 자체는 유지(9/7 확정), 결과물의 앱 배포만 철회(9/21)"였다. 이번 정리로 **결과물 자체도 제품에 넣지 않는 것**으로 확정 — 자연어→SQL 경로는 Claude Desktop + MCP 서버(25개 도구) 하나뿐. 근거는 (1) MCP가 `execute_sql`·스키마 조회·에디터 제어까지 같은 일을 이미 전부 하고, (2) 1.5B급 LoRA 모델은 한국어·복잡한 쿼리·처음 보는 스키마에서 Claude를 못 따라가며(자체 평가도 held-out 93.7%·한국어 실패율 12.0%·`policy`↔`claim` 일반화 실패), (3) 같은 기능이 두 경로로 존재하면 상충하고 틀린 SQL을 내는 쪽이 앱 신뢰도를 깎는다는 것.

**반영한 것**: `AI.md`(맨 위 요약 + 맨 아래 "최종 결정" 절 신설, 9/7 판단 번복 표시, "현재 상태" 절을 대체됨으로 표시), 이 항목과 9/7·9/21 항목의 번복/후속 주석, `README.md` AI Integration 행, `FUNCTIONS.md` MCP 항목, `DIFF.md` AI 연동 행, 앱의 Diagram 페이지(오프라인 "AI track" 패널 → "AI integration: Claude via MCP" 패널). **기록은 지우지 않음** — 9/7~9/21의 파인튜닝 작업 기록(데이터 생성, Colab 학습, 87.9%→93.7%, 데이터 버그 3건)은 실제로 수행한 일이라 그대로 남기고, `code/AI/`도 탐색 실험의 기록으로만 보존(제품 어디에서도 참조하지 않음). 캡스톤 산출물 D01~D08은 이미 AI 연동을 MCP 기반으로만 기술해 변경 불필요.

**열린 항목**: ~~사이드바의 빈 "AI" 탭(`AiView.tsx`)~~ → 같은 날 제거함(아래 항목). `code/AI/` 삭제 여부는 사용자가 나중에 정하기로 해서 미정.

### 10월 1일 — 프런트: Diagram 페이지 신설 (시스템 전체 흐름 그림 + EN/한국어 토글)

액티비티 바 맨 아래(AI 아이콘 아래)에 다이어그램 아이콘을 추가하고, 처음엔 빈 페이지로 두었다가 이어서 시스템 전체 흐름을 그리는 페이지로 채움. README.md·`docs/mds`(FUNCTIONS.md, architecture-diagram.md)를 근거로, 클라이언트 5종 → `engine_server` → `engine_core`(파서·플래너·실행기) → 트랜잭션·동시성 → 스토리지 → 내구성·복구를 번호 1~8의 계층과 화살표로 배치. 연결 경로는 코드로 확인(Tauri Rust 백엔드가 `engine_server`를 띄우고 TCP 클라이언트로 접속, MCP 서버는 :7878로 접속하며 `ui_commands.json` 큐로 UI를 조작). 새 라이브러리 없이 HTML/CSS 그리드로 구현하고 스타일은 App.css 대신 별도 `DiagramView.css`로 분리.

두 가지 후속 반영: (1) 맨 아래 패널을 "오프라인 AI track"에서 "AI 연동: Claude via MCP, 사설 모델 없음"으로 교체(위 "AI 방향 최종 정리" 결정), (2) 맨 위에 **EN / 한국어 토글** 추가 — 모든 문구를 영어·한국어 쌍으로 두고 즉시 전환, 선택은 `rusql_diagram_lang` 키로 localStorage에 저장(다른 탭에 갔다 와도 유지). 한국어는 단어 중간에서 줄바꿈되지 않도록 `word-break: keep-all`.

이어서 **인터랙션/애니메이션** 추가: (1) 박스(카드·칩·파이프라인 단계)에 마우스를 올리면 테두리가 해당 레이어 색으로 바뀌고 글로우와 함께 1.03배로 커짐(큰 패널은 1.008배 — 더 키우면 행 전체가 튐), (2) 화살표 선 위로 방향대로 흐르는 펄스(점선 우회 경로는 행진하는 점선)와 화살촉 상하/좌우 움직임, (3) 페이지 로드 시 위에서 아래로 행 단위로 차례차례 나타남, (4) 같은 번호의 배지(상단 리본·패널 헤더·파이프라인 단계)가 1→8 순서로 함께 깜빡여 번호 경로를 눈으로 따라갈 수 있게 함. 검증은 Edge를 DevTools 프로토콜로 직접 조작해 실제 마우스 호버 상태의 computed style(확대 행렬·테두리 색, 마우스를 떼면 원복)과 애니메이션 실행 여부(배경 위치가 시간에 따라 변함)를 측정. **발견 사항**: 이 PC의 Windows "애니메이션 효과"가 꺼져 있어(설정 > 접근성 > 시각 효과) WebView2가 `prefers-reduced-motion: reduce`를 보고함 — 처음에 넣었던 reduced-motion 대응 규칙이 이 PC에서는 모든 애니메이션을 가려 버려 제거(앱의 다른 곳도 이 설정을 따르지 않음). 접근성 설정을 존중하는 쪽으로 되돌릴 수는 있으나 그러면 이 PC에서는 애니메이션이 보이지 않음.

**보기 전환 토글 + 원통형 그림** 추가: 맨 위 오른쪽에서 EN/한국어 토글을 조금 왼쪽으로 옮기고 그 오른쪽에 아이콘 2개짜리 보기 토글을 둠 — 왼쪽은 기존 계층형 그림, 오른쪽은 **원통 모양과 화살표로 정리한 그림**(선택은 `rusql_diagram_view`로 localStorage에 기억). 원통형 그림은 SVG로 직접 배치한 13개 원통(클라이언트 5 → engine_server → 파서·플래너·실행기 → 트랜잭션 → 스토리지 → 내구성 → 디스크 파일)이며, 원통에는 이름만 적고 **설명은 마우스를 올릴 때만 툴팁으로** 표시(키보드 포커스도 동일, 아래쪽 행은 툴팁이 위로 열림). 애니메이션: 원통이 순서대로 팝인, 화살표 위로 패킷 점(SVG `animateMotion`)과 미끄러지는 점선, 흐름 순서대로 빛나는 원통 뚜껑, 호버 시 확대+글로우. 공용 부분(EN/KO 문구 타입·색)은 `diagramShared.ts`로 분리하고 기존 화면은 `LayersView`로 분리. 검증: Edge를 DevTools 프로토콜로 조작해 토글 클릭, 원통 13개 렌더, 원통 위에 설명 문구가 없음, 호버 시 툴팁 표시와 방향(위/아래), 마우스를 떼면 사라짐, 패킷 시계와 점선 오프셋이 시간에 따라 변함, 한국어 전환, 새로고침 후 보기·언어 유지, 왼쪽 버튼으로 복귀를 확인하고 스크린샷으로 라벨 겹침을 잡아 수정.

**후속 — 두 토글 모두 왼쪽 버튼이 기본**: 언어 토글은 `한국어 | EN`(한국어가 기본), 보기 토글은 `원통형 | 계층형`(원통형이 기본)으로 순서와 기본값을 바꿈. 저장된 선택이 없을 때만 기본값이 적용되고, 한 번이라도 눌러 저장된(`rusql_diagram_lang`/`rusql_diagram_view`) 선택이 있으면 그것이 우선. 새 프로필에서 기본 상태 → 오른쪽 버튼 클릭 → 새로고침 후 유지 → 왼쪽 버튼으로 복귀까지 Edge 조작으로 확인.

**후속 — 화살표 스타일 통일**: 원통형 그림의 화살표(옅은 base 선 + 흐르는 점선 + 빛나는 패킷 점 + 색깔 화살촉)가 더 깔끔하다는 판단으로 계층형 그림의 화살표도 같은 스타일로 교체. 계층형의 화살표는 칸마다 길이가 달라서(72px~449px) 퍼센트 좌표 SVG(`FlowArrow`, 칸 전체에 늘어남)로 만들고, 패킷은 SMIL `animate`로 `cy`/`cx`를 `0%→100%`로 움직여 어떤 길이에서도 끝까지 달림. 화살촉 정의(`ArrowDefs`)와 선/점선/패킷 스타일은 두 그림이 한 벌을 공유하도록 `diagramShared.tsx`와 `DiagramView.css`로 모으고, 예전의 CSS로 그린 선·삼각형·키프레임(`dg-flow-*`, `dg-march`, `dg-bob-*`)은 제거. 검증: 세로 10개·가로 2개 화살표 모두 선/점선/패킷/화살촉이 렌더되고 화살촉 참조가 해석됨, 241px 높이의 칸에서 패킷이 칸 끝까지 이동, 점선 오프셋이 시간에 따라 변함, 원통형 그림(원통 13개·화살표 12개)은 그대로 정상.

**검증**: `tsc --noEmit` 클린. 실제 앱 창을 띄워 눈으로 확인하지는 못했고, 같은 컴포넌트를 헤드리스 Edge로 렌더링해 확인 — 영어/한국어 두 화면, 한국어 버튼을 실제로 클릭해 전환되는지, 같은 프로필로 다시 열었을 때 한국어가 유지되는지, 1400px·1000px 폭에서 레이아웃이 깨지지 않는지(최소 폭 940px, 그 이하는 가로 스크롤).

### 10월 1일 — 빈 "AI" 탭 제거

사설 AI 모델을 제품에서 뺀다는 결정(위 "AI 방향 최종 정리")으로 용도가 없어진 사이드바의 빈 "AI" 탭을 삭제: 활동 바 아이콘, `ActiveView`의 `"ai"`, 렌더 분기, `AiView.tsx`, App.css의 `.ai-view` 선택자(공유 규칙은 `.diagram-view`만 남김). 활동 바는 SQL 에디터·ERD·서버 매니저·Diagram 4개가 됨. 검증: `tsc --noEmit` 클린, `vite build` 성공(임시 폴더로 출력해 `dist`는 건드리지 않음), `AiView`/`ai-view`/`"ai"` 잔여 참조 0건. 서버 매니저의 "AI MCP" 패널과 MCP 서버는 별개라 그대로. **발견만 하고 고치지 않은 것**: 홈 화면 소개 문구(`App.tsx`)에 "내장 AI 어시스턴트가 항상 도와준다"와 "Rust로 처음부터 만든"이라는 이제 사실이 아닌 문장이 남아 있음 — 요청 범위 밖이라 두고 보고만 함.

### 10월 1일 — 인덱스를 쓰는 UPDATE/DELETE (+ 테스트가 잡은 기존 버그 6건, 인덱스 일괄 갱신, 죽은 코드 정리)

바로 위 UPDATE 항목의 "정직한 한계"(PK 등호가 아닌 조건은 여전히 테이블 크기에 비례하는 스캔)를 해소. UPDATE/DELETE가 대상 행을 찾을 때 테이블의 인덱스를 쓰도록 함.

**설계** (`executor_dml_index.cpp`): ① SELECT와 같은 단일 판단 지점인 `Planner::choose_access`에 물어 접근 경로를 얻고, AND 조건이면 인덱스를 쓸 수 있는 리프 하나를 고름(결과는 그 리프의 부분집합이고 전체 조건은 다시 검사). ② 인덱스는 **후보 공급자일 뿐** — 후보마다 `row_pk_pos`로 실제 행을 찾아 (같은 버전인지: pk와 `_xmin` 일치, 문장에 보이는지, 전체 조건 만족하는지) 확인. ③ 이상한 점이 하나라도 있으면(캐시 미스, 위치 어긋남, 인덱스와 행 저장소 불일치) 기존 스캔으로 되돌아감 — 틀린 인덱스는 속도를 잃게 할 뿐 정확성은 잃게 하지 않음(인덱스가 *놓치는* 행만은 검증할 수 없으므로 아래 숫자 동치 처리를 함). 지원 경로: PK 점/범위/BETWEEN, 보조 B+Tree 점/범위/BETWEEN/LIKE 접두사, 해시(이 시점엔 비숫자 리터럴만 — 아래 항목에서 해시 인덱스가 숫자 값 기준 버킷이 되어 제한 해제), IndexIntersection. 쓰지 않는 경우: 복합 인덱스, 복합 PK·PK 없는 테이블(위치 캐시가 단일 PK 전용), 서브쿼리 조건, **다른 트랜잭션이 열려 있을 때**(SELECT와 같은 규칙 — 인덱스에는 최신 버전만 있으므로), 결과가 테이블의 1/8(최소 128행)을 넘을 때(JSON 파싱이 스캔보다 비쌈), 64행 미만 테이블(`RUSQL_DML_INDEX_MIN_ROWS`로 조정, 0=항상 — 테스트와 퍼저가 사용).

**숫자 동치 문제** (실측으로 발견): B+Tree는 숫자로 같은 키(`"7"`, `"07"`, `"7.00"`)를 글자로 구분해 서로 다른 키로 저장하지만 WHERE는 숫자로 같다고 봄 → 기존 SELECT의 인덱스 경로가 `price = 7`에서 `7.00`을 놓치고(스캔은 둘 다 찾음) `code = 7`도 1행 vs 3행. UPDATE/DELETE가 이를 물려받으면 행을 조용히 빠뜨리므로, 숫자 경계는 한 ulp 넓혀 범위로 훑고 실제 행에 조건을 다시 적용. 해시 인덱스는 숫자 리터럴이면 쓰지 않음. (SELECT 쪽은 이 항목에서 고치지 않았고, 바로 아래 항목에서 고침)

**인덱스 일괄 갱신** (`index_replace_rows`): 후보 탐색을 O(1)로 만들고 재어 보니 20,000행에서도 4ms가 남았음 — 보조 인덱스 버킷이 "행 전체 복사본의 JSON 배열"이라 행 하나를 바꿀 때마다 그 버킷(`grp`가 50종이면 400행)을 통째로 파싱·재직렬화했고, 다중 행 UPDATE는 행마다 반복해 제곱으로 증가(400행 UPDATE에 1.2초). 문장이 건드린 행들을 버킷별로 한 번만 파싱하고 한 번만 쓰도록 변경(UPDATE, DELETE의 물리/소프트 삭제 모두). 옛 행과 새 행을 한 번에 처리하므로 `SET id = id + 1` 같은 연쇄에서 새 항목을 옛 항목으로 오인해 지우는 일도 구조적으로 사라짐.

**테스트가 잡은 기존 버그 6건** — 1~5는 같은 무작위 문장열을 "인덱스 경로 강제"와 "스캔 강제" 엔진에 돌려 결과와 테이블 상태를 매 단계 대조하자(시드 0~60 중) 하나씩 나왔고, 6은 동시성 테스트를 CPU 부하 아래서 반복하자 나옴. 모두 이번 변경이 만든 것이 아니라 변경 전부터 있던 것(1~5는 변경 전 바이너리로 확인):
1. **인덱스 버킷에 같은 pk의 죽은 버전과 살아 있는 버전이 함께 있음** — ROLLBACK/재구성(`rebuild_secondary_indexes`)이 죽은 버전까지 모든 행으로 인덱스를 다시 만들기 때문. 내 첫 구현이 pk로 중복 제거하며 먼저 나온 죽은 사본을 채택해 살아 있는 행을 놓침 → 죽은 사본은 건너뛰고, PK B+Tree(pk당 항목 1개라 죽은 사본이 살아 있는 버전을 가릴 수 있음)의 죽은 사본만 실제 행으로 확인(확인 불가면 스캔).
2. **UPDATE가 PRIMARY KEY/UNIQUE 중복을 검사하지 않음** — `UPDATE u SET id = 2 WHERE id = 1`이 id 2인 행을 둘로 만들고, UNIQUE 컬럼을 남의 값으로 바꿔도, 여러 행을 한 키로 바꿔도 성공. "pk당 행 하나" 가정이 전부 깨짐 → 새 행들끼리의 중복과 이 문장이 바꾸지 않는 살아 있는 행과의 중복을 거부(INSERT와 같은 `Duplicate value '…' for column '…'`). 같은 문장이 비워 주는 자리로 옮기는 것(`SET id = id + 1`)과 UNIQUE의 NULL 여러 개는 허용.
3. **여러 행 UPDATE가 중간 행에서 CHECK/ENUM 위반으로 실패하면 앞서 처리한 행이 사라짐(데이터 유실)** — 각 행의 옛 버전을 루프 안에서 먼저 "죽음"으로 표시하고 나중에 새 버전을 넣어서, `UPDATE t SET v = v + 8`이 2번째 행에서 CHECK 위반으로 실패하면 1번째 행이 없어짐(실행 후 행 3개 중 2개만 남음). 모든 새 버전을 만들고 검증한 뒤에야 옛 버전을 표시하도록 변경.
4. **PK를 연쇄로 바꾸면 PK B+Tree·보조 인덱스 항목이 사라짐** — `SET id = id + 1`(id 2, 3)에서 행 2→3의 새 항목을 넣은 뒤 행 3→4가 "옛 키 3"을 지우며 방금 넣은 항목까지 삭제. 모든 옛 항목을 먼저 지우고 새 항목을 넣는 2단계로 변경(PK 위치 캐시도 같은 방식).
5. **`DELETE ... WHERE pk BETWEEN`이 존재하지 않는 행을 지웠다고 보고** — PK 인덱스에 소프트 삭제된 행의 키가 남아 있고 위치 캐시가 그 죽은 행을 가리키면, 검증 없이 swap-remove해 삭제 건수에 셈(`0 row(s)`여야 할 것이 `1 row(s) deleted`, 캐시가 따뜻한지 차가운지에 따라 달라짐). 위치마다 pk 일치와 가시성을 검증하도록 수정.

6. **같은 행을 동시에 UPDATE하면 행이 두 개로 갈라짐(버전 분기)** — 기존 `UPDATE: concurrent increments of one row` 테스트가 CPU 부하 아래서 `(id=1 v=179) (id=1 v=479) (id=2 v=120)`처럼 같은 id의 살아 있는 행 2개를 남김(부하 없이는 거의 안 보여 한 번의 간헐 실패로만 나타났고, 부하용 프로세스 3~4개를 띄우자 첫 반복에서 재현). 원인: 후보를 찾는 시점(시도 시작)에 만든 스냅샷을 배타 락을 얻은 뒤의 검증에도 그대로 씀 — 락을 기다리는 사이 다른 문장이 같은 행을 갱신하고 끝나면, 그 문장의 id가 옛 스냅샷의 cutoff보다 커서 "아직 커밋 전"으로 보여 이미 죽은 버전이 살아 있는 것으로 검증을 통과하고 한 번 더 갱신됨. 배타 락을 잡은 직후 스냅샷을 새로 만들도록 수정 — 수정 후 같은 부하에서 120회 반복·동시성 태그 전체 10회 반복 모두 실패 0건(수정 전엔 첫 반복에서 실패).

**성능** (이 PC, 20,000행, 문장당 ms, 직전 커밋 바이너리 → 이번):

| 문장 | 인덱스 1개 테이블 | `grp`(50종) 인덱스가 하나 더 있는 테이블 |
|---|---|---|
| UPDATE ... WHERE 유일키 = x (1행) | 17.6 → **1.0** | 11.9 → 2.3 |
| UPDATE ... WHERE 유일키 BETWEEN (10행) | 11.6 → **1.0** | 43.4 → 14.1 |
| UPDATE ... WHERE 유일키 = x AND grp >= 0 | 10.3 → **0.7** | 14.2 → 2.0 |
| UPDATE ... WHERE id > N (PK 범위, 8행) | 11.1 → **0.9** | 32.8 → 9.9 |
| UPDATE ... WHERE grp = k (약 400행) | (인덱스 없음, 스캔) | 1,237 → **15.7** |
| DELETE ... WHERE 유일키 = x | 8.6 → **0.7** | 9.7 → 2.0 |
| DELETE ... WHERE grp = k (약 400행) | (인덱스 없음, 스캔) | 353 → **7.7** |
| 인덱스가 없는 조건(스캔) | 6.6 → 6.7 (그대로) | 6.8 → 7.2 (그대로) |

인덱스 1개 테이블의 단일/소수 행 문장은 이제 크기와 무관(1,000행과 20,000행이 같은 값, fsync 바닥 근처). 오른쪽 열의 단일 행 값(2.3ms)이 1.0이 아닌 이유는 다음 한계 참고. 직전 라운드의 `pk = 리터럴` 수치(0.7ms/트랜잭션 내 0.2ms)는 그대로이고, 같은 프로브의 PK 범위·AND 조건 UPDATE는 7.1 → 1.0ms, 4.7 → 0.26ms.

**검증**: 신규 Catch2 8케이스(`test_dml_index.cpp`) — 무작위 비교 테스트(인덱스 강제 vs 스캔 강제, 2개 테이블(B+Tree/해시)에 UPDATE·DELETE·INSERT·BEGIN/COMMIT/ROLLBACK·VACUUM·RETURNING·SELECT를 섞어 1,200문장, 같은 문장열로 결과와 테이블 상태를 대조, 추가로 **한 엔진 안에서 "인덱스로 찾은 결과 = 스캔으로 찾은 결과"**를 확인하는 일관성 검사 — 두 엔진이 같은 인덱스 갱신 코드를 쓰므로 비교만으로는 못 보는 맹점), 숫자 동치, 인덱스를 쓰는/안 쓰는 경우 구분(카운터로 증명), 다른 트랜잭션이 열려 있을 때, 콜드 캐시(재시작·스캔 삭제·VACUUM 뒤 자가 복구), DELETE 변형(PK 범위, RETURNING 순서, 트랜잭션+ROLLBACK, FK 부모 RESTRICT/CASCADE), 동시 실행, 무결성(위 버그 2·3). Release **429 케이스/95,126 assertions**를 기본 설정과 **모든 DML을 인덱스 경로로 강제한 설정**(`RUSQL_DML_INDEX_MIN_ROWS=0`) 양쪽에서 통과(기존 400여 케이스 전체가 인덱스 경로의 회귀 검증이 됨), Debug도 양쪽 통과. 간헐 실패를 추적하다 얻은 교훈: 병렬로 돌리는 테스트 프로세스끼리는 작업 디렉터리를 분리해야 임시 폴더가 충돌하지 않음(처음 한 번의 실패 중 하나가 이것이었음). 무작위 비교는 **시드 210개**(0~209, 문장 1,200개 × 테이블 2개씩; `RUSQL_FUZZ_SEEDS`·`RUSQL_FUZZ_START`·`RUSQL_FUZZ_TRACE`·`RUSQL_FUZZ_DUMP_STEP`으로 길게/재현/추적/직전 상태 덤프)를 최종 코드로 통과(불일치 0건, 시드 60개가 일관성 검사 포함 약 147만 assertions). 버그 1~5는 이 캠페인 도중 시드 11/8415/8797 등에서 하나씩 나왔고 수정 후 처음부터 다시 돌려 확인. 크래시 퍼저에 `g` 컬럼 보조 인덱스를 통한 그룹 UPDATE/DELETE를 추가하고 서버를 인덱스 강제로 띄워 순차 100라운드(체크포인트 3KB 포함, 스캔 대조군 20라운드) + 동시 40라운드 불일치 0건. `test_full.sql`/`test_full-ver2.sql` 에러 수 변경 전과 동일(3/2), 기본/강제 출력이 시각·소요시간 외 동일.

**죽은 코드 정리**: 호출처가 없던 함수 4개(`TransactionManager::has_redo_ops`·`has_undo_log_file`·이것만 쓰던 `UndoLogFile::exists`, `LockManager::insert_lock`, `DiskManager::save_schema_columns`)와 `App.css`의 쓰이지 않던 서버 매니저 탭바·CLI 가이드 규칙 67줄 삭제. `code/AI/`는 사용자가 정하기로 해서 손대지 않음.

**Diagram 페이지 갱신**(영어/한국어, 계층형·원통형 두 그림 모두): 쿼리 플래너 설명에 "접근 경로가 UPDATE·DELETE의 대상 행 탐색에도 쓰임", 실행기 설명에 "인덱스 결과는 후보로만 쓰고 실제 행으로 검증, 의심스러우면 스캔 / UPDATE는 PRIMARY KEY·UNIQUE를 검사하고 전부 적용되거나 전혀 적용되지 않음", 스토리지의 B+Tree 인덱스·테이블 데이터 설명에 "문장당 버킷 1회 기록", "인덱스 결과를 실제 행으로 연결하는 PK→위치 캐시"를 추가. 길어진 실행기 툴팁이 아래 행 원통을 가려, 파이프라인 줄(x ≥ 500)의 원통은 툴팁을 위쪽 빈 공간으로 열도록 규칙을 바꿈. 새 구성요소가 생긴 것은 아니라 그림의 구조(원통·화살표)는 그대로.

**정직한 한계**: ① 보조 인덱스 버킷이 행 전체 복사본의 JSON 배열이라, 행 하나를 바꿔도 그 행이 속한 버킷 크기에 비례한 비용이 남음(20,000행·`grp` 50종에서 단일 행 UPDATE 2.3ms vs 인덱스 없을 때 1.0ms) — 근본적으로는 버킷을 pk 목록으로 바꾸는 저장 구조 변경이 필요. ② 위의 **숫자 동치 문제는 SELECT의 인덱스 경로에 아직 남아 있었음**(`WHERE price = 7`이 `7.00`을 놓침 — 이 항목은 "비정규 숫자 문자열이 있는 DECIMAL/VARCHAR 한정, INT는 해당 없음"이라 적었으나 **틀린 서술**: INT 컬럼도 `007`/`7.0`을 입력한 글자 그대로 저장하므로 INT도 해당) → 바로 아래 항목에서 해결. ③ 복합 PK/복합 UNIQUE는 UPDATE에서 여전히 검사하지 않고, 복합 인덱스·복합 PK 테이블은 이 경로를 쓰지 않음. ④ FK로 참조되는 테이블의 물리 삭제는 16행 이하일 때만 위치로 지우고 그 이상은 한 번의 `remove_if` 패스. ⑤ 다른 트랜잭션이 하나라도 열려 있으면 UPDATE/DELETE는 스캔(SELECT와 같은 규칙).

### 10월 1일 — SELECT 인덱스 경로의 숫자 동치 + 같은 조사에서 나온 기존 버그 8건 (복합 인덱스 행 손실, 재시작 뒤 인덱스, PK 위치, NULL 조인 외)

바로 위 항목의 "정직한 한계 ②"(SELECT의 인덱스 경로가 숫자 동치를 모름)를 고치려 시작했는데, 모든 경로를 같은 질의의 스캔 결과와 대조하는 테스트를 만들자 같은 부류("인덱스로 찾은 결과 ≠ 스캔으로 찾은 결과")의 기존 버그가 연달아 나왔음. 전부 변경 전 코드(커밋 `074a17e`)에서 재현되고, 새 테스트 파일을 변경 전 소스에 얹어 빌드하면 7케이스가 모두 실패.

**정정**: 바로 위 항목은 이 문제가 "비정규 숫자 문자열이 있는 DECIMAL/VARCHAR 한정, INT는 해당 없음"이라 적었으나 틀렸음 — INT 컬럼도 입력한 글자 그대로 저장(`007`, `7.0`이 그대로 남음)하므로 INT도 해당.

**숫자 동치 수정** (공통 헬퍼 `numeric_key.hpp`, DML과 공용):
- `widen_numeric_bound`: B+Tree 범위 경계를 한 ulp 넓혀 훑고, 찾은 행에 전체 조건을 다시 적용(인덱스는 후보 공급자일 뿐). 적용 범위: PK 점/범위/BETWEEN(여기는 재검사 자체가 없었음), 보조 점/범위/BETWEEN, 커버링, Top-K(`ORDER BY col LIMIT n`), IndexIntersection. 넓힐 수 없는 경계(무한대)나 한쪽 끝만 숫자인 BETWEEN은 스캔으로. LIKE 접두사는 숫자처럼 읽히는 접두사(`'12%'`)일 때 쓰지 않음(수치 순서에서는 13이 120보다 앞이라 접두사 구간이 이어지지 않음).
- `normalize_numeric_key`: 해시 인덱스 버킷을 숫자 값으로 키잉(`"7"`, `"7.0"`, `"07"`이 한 버킷 — 근사가 아니라 정확). 그래서 DML 쪽의 "숫자 리터럴이면 해시 미사용" 규칙은 삭제.
- 커버링 경로(`SELECT price ... WHERE price = 7`): 조회 키가 아니라 **저장된 값**을 돌려줌(전: `7`과 `7.00`이 있어도 `7` 한 행만, `price > 7`이 경계값 `7.00`까지 반환). 또 선택 컬럼이 그 인덱스의 컬럼 하나뿐일 때만 사용 — 전에는 선택 컬럼이 어떤 복합 인덱스의 부분집합이기만 해도 "커버링"으로 보고 나머지 컬럼을 빈 칸으로 돌려줬음(`SELECT a, b FROM t WHERE a = 1`이 `b`가 빈 행들을 반환, 변경 전 서버로 재현).
- 조인: 해시 조인은 키를 글자 그대로 해시해 `7`과 `7.00`이 만나지 못했고(→ 정규화 키), 정렬 병합 조인은 정렬은 수치로 하면서 같은 키 묶음은 글자 일치로 판단(→ 정렬과 같은 수치 비교), IndexNL/ReverseIndexNL은 `search(키)` 정확 일치(→ 넓힌 범위 + 키마다 `=` 검증). 해시/정렬 병합/IndexNL/ReverseIndexNL(B+Tree·해시) 5가지 모두 중첩 루프와 같은 행 쌍을 만듦.

**같은 조사에서 발견·수정한 기존 버그 8건** (1은 SELECT 차분 테스트, 2·8은 조인 테스트, 4~6은 재시작 테스트와 차분 퍼저가 잡음):
1. **비유일 복합 인덱스가 같은 컬럼 값을 가진 행 중 마지막 하나만 기억** — 키가 컬럼 값만이라 같은 값의 행끼리 항목 하나를 덮어씀. `CREATE INDEX kab ON k (a, b)` 뒤 (1, 7)을 가진 행이 3개면 `WHERE a = 1 AND b = 7`이 1행(접두사 `a = 1`은 3행 중 2행)만 반환 — 인덱스가 있고 없고에 따라 답이 달라짐. 복합 인덱스를 재설계: **항목 키 = 정규화한 컬럼 값들 + 행의 PK**(`CompositeIndex::pk_columns`, 생성 시 카탈로그에서 채움). 같은 값의 행마다 항목이 하나씩이고 조회는 "선두 컬럼들이 같은 항목 구간"의 범위 스캔(`range_search("v1\0…\0", "v1\0…\0\xFF")`). 삽입/삭제/증분 갱신 호출처(약 15곳)는 "키 연산"이라는 성질이 그대로여서 수정하지 않음. 옛 `prefix_scan`은 접두사 이후 트리 끝까지 훑었음(O(꼬리)) → 이제 해당 구간만(O(log N + k)).
2. **해시 조인·정렬 병합 조인이 NULL을 NULL과 매칭** — 키가 글자 `"NULL"`로 같다고 판단. NULL이 섞인 두 40행 테이블의 `a JOIN b ON a.v = b.v`가 218행(중첩 루프는 138행 — 정답). 두 알고리즘 모두 NULL 키를 건너뛰도록 수정(IndexNL은 이미 건너뜀).
3. **`a = b`(컬럼 비교)가 복합 인덱스의 조회 키가 됨** — 플래너가 `collect_eq_map`의 값 `"b"`를 상수로 취급해 `WHERE a = a2 AND b = 7`이 `a`가 글자 `"a2"`인 행을 찾음. `Planner::constant_eq_map`(컬럼 참조 제외)로 교체.
4. **재시작 뒤 보조 인덱스가 비어 있음** — 보조 B+Tree는 `.idx` 파일에 `CREATE INDEX` 시점의 상태로만 저장되고 이후 INSERT/UPDATE/DELETE로는 갱신되지 않는데, 시작할 때 그 파일을 그대로 로드. 데이터를 넣기 **전에** 만든 인덱스는 체크포인트(`CHECKPOINT`·DDL·redo 4MB) 뒤 재시작하면 빈 채로 올라와 `WHERE price = 8`이 0행(스캔은 찾음)이었음. redo 재생이 있었던 재시작은 우연히 재구성해 가려졌고, 크래시 퍼저는 테이블을 스캔으로만 비교해 못 봤음. **인덱스는 파생 데이터이므로 시작 때 항상 행에서 재구성**하도록 변경하고 `.idx` 쓰기/읽기와 B+Tree JSON 직렬화(`btree_json.*`, `DiskManager::save/load_btree_index`, 관련 테스트 3개)를 삭제(구버전이 남긴 `.idx`는 읽지 않고 DROP INDEX 때만 지움). README/아키텍처 문서의 "`.idx` 자동 저장"은 실제와 달랐음(CREATE INDEX 때만 저장) — 함께 정정.
5. **PK가 첫 컬럼이 아닌 테이블에서 `WHERE id = N`이 항상 0행** — INSERT가 PK B+Tree·undo 로그·upsert 조회를 PK 컬럼이 아니라 테이블의 *첫 컬럼*으로 키잉(`col_names[0]`), 시작 때의 PK 인덱스 재구성도 첫 컬럼 기준. `CREATE TABLE t (name VARCHAR(10), id INT PRIMARY KEY)` 같은 테이블에서 재현. PK 컬럼(없으면 첫 컬럼)으로 통일.
6. **PK B+Tree 재구성에서 죽은 버전이 산 버전을 가림** — ROLLBACK/VACUUM/redo 복구/시작 때 모든 물리 행을 순서대로 넣으며 "마지막 행이 이김"인데, VACUUM이 행 위치를 옮기고 나면 죽은 버전이 산 버전 뒤에 올 수 있어 PK 항목이 죽은 사본이 되고 그 행의 PK 점/범위 조회가 "없음"을 반환(차분 퍼저 시드 4956, 같은 pk의 산 행과 죽은 행이 공존). 죽은 버전을 먼저 넣고 산 버전이 덮어쓰도록 `build_pk_tree`로 8곳을 통일.
7. **Top-K 인덱스 경로가 다른 트랜잭션이 열려 있어도 쓰임** — 일반 SELECT 인덱스 경로에 있던 "다른 트랜잭션이 열려 있으면 스캔" 가드가 Top-K에는 없었음. 가드를 맞춤(재현 테스트는 만들지 못한 방어적 수정).
8. **트랜잭션 안에서 IndexNL/ReverseIndexNL 조인이 결과를 못 냄** — 트랜잭션이 열려 있으면 인덱스 조인은 해시 조인으로 되돌아가는데, 그 폴백이 양쪽 모두 *왼쪽* 컬럼 이름으로 해시해 두 컬럼 이름이 다른 조인(`l.k = r.rid`)은 아무 행도 못 맞췄음. 오른쪽 PK 컬럼 이름(IndexNL)과 왼쪽 조인 컬럼 이름(`ReverseIndexNL::left_col`, 신설)을 쓰도록 수정.

**검증**: 신규 Catch2 7케이스(`test_select_index.cpp`) — 모든 접근 경로의 숫자 동치(점/범위/BETWEEN/커버링/Top-K/LIKE/교집합, 기대 결과를 직접 적은 것과 "인덱스 vs `WHERE (조건) OR id < 0`로 강제한 스캔" 대조 둘 다), 해시, 복합 인덱스 중복 행(DELETE/UPDATE/PK 변경/ROLLBACK 후), 조인 5알고리즘(EXPLAIN으로 실제 알고리즘 확인, NULL 키 포함, 트랜잭션 안의 폴백 포함, 중첩 루프를 강제한 질의와 대조), PK가 첫 컬럼이 아닌 테이블, 체크포인트+재시작 뒤 인덱스, 그리고 무작위 차분 퍼저(B+Tree·복합·해시 인덱스 테이블 2개에 INSERT/UPDATE/DELETE/BEGIN/COMMIT/ROLLBACK/VACUUM을 섞어 700문장, 문장마다 "PK 전체 범위 = 스캔" 불변식 검사, 질의마다 `id`·`*`·커버링·Top-K 대조). **변경 전 소스에 같은 파일을 얹으면 7케이스 전부 실패.** Release **434 케이스/125,695 assertions**를 기본 설정과 **모든 DML을 인덱스 경로로 강제한 설정**(`RUSQL_DML_INDEX_MIN_ROWS=0`) 양쪽에서 통과, Debug도 양쪽 통과. 무작위 SELECT 퍼저는 **시드 270개**(0~269; 60개분 572,843 + 210개분 2,004,612 assertions)를 최종 코드로 통과(불일치 0), DML 차분 퍼저 60시드 1,474,822 assertions 통과. 크래시 퍼저에 "복구 뒤 인덱스 질의"(보조 인덱스 `g`·PK 점 조회가 oracle과 같아야 함) 검사를 추가했고, 이 검사는 변경 전 서버로는 12라운드 중 2라운드에서 불일치를 냄(위 4번). 최종 서버로 순차 180라운드(체크포인트 3KB 100 + 기본 60 + 스캔 대조군 20) + 동시 40라운드 불일치 0, 마지막 수정(트랜잭션 안 조인 폴백) 뒤 최종 코드로 다시 순차 100 + 동시 20라운드, SELECT 퍼저 270시드, DML 퍼저 30시드 불일치 0. `test_full.sql`/`test_full-ver2.sql` 에러 수 변경 전과 동일(3/2).

**Diagram 페이지 갱신**(영어/한국어, 계층형·원통형 두 그림 모두): 스토리지의 B+Tree/해시 설명에 "키를 숫자로 비교(7 = 7.00)"·"숫자 값 기준 버킷", 크래시 복구·내구성 설명에 "모든 인덱스를 행에서 재구성"을 추가하고, 디스크 파일 목록에서 `*.idx`를 제거. 새 구성요소가 생긴 것은 아니라 그림의 구조(원통·화살표)는 그대로.

**죽은 코드 정리**: 인덱스 영속화 제거로 쓸모가 없어진 `btree_json.cpp/.hpp`, `DiskManager::save_btree_index`/`load_btree_index`, `BPlusTree::set_root`와 그 테스트 3개, 복합 인덱스 재설계로 엔진에서 더는 안 쓰는 `CompositeIndex::search_from_eq_map`/`prefix_scan`(테스트는 `lookup`으로 교체), DML 쪽 로컬 `widen`/`parse_f64`(공용 헬퍼로 이동) 삭제. `code/AI/`는 그대로 둠.

**정직한 한계**: ① B+Tree 키 순서는 "둘 다 숫자면 수치, 아니면 사전순"이라, 한 컬럼에 숫자처럼 보이는 값과 아닌 값이 섞이면 전순서가 아님(`9 < 10 < '1a' < 9`처럼 순환 가능). 비숫자 경계 리터럴로 범위 질의를 하면 이론상 행을 놓칠 수 있음 — 차분 퍼저(숫자/비숫자 혼합 컬럼 포함)에서는 나오지 않았으나 구조적 한계이고, 없애려면 인덱스 정렬을 (타입, 값)으로 바꿔야 함. ② 숫자 동치는 WHERE와 같은 double 비교라 2^53을 넘는 정수는 서로 다른 값도 같다고 봄. ③ `JOIN ... USING`/`NATURAL JOIN`은 여전히 글자 비교(중첩 루프의 기존 동작, 이번 범위 밖). ④ 시작 때 모든 인덱스를 행에서 재구성하므로 부팅 시간이 행 수·인덱스 수에 비례(실측: 100,000행에 보조 인덱스 2개인 테이블에서 서버 시작 후 첫 질의 응답까지 약 217ms로, 인덱스를 파일에서 읽던 이전 빌드의 약 200ms와 17ms 차이 — 이 규모에서는 무시할 만함). ⑤ 복합 인덱스는 UPDATE/DELETE의 후보 탐색에는 아직 쓰지 않음.

### 10월 2일 — 명시적 트랜잭션 커밋 비용 8배 절감 (+ 벤치마크 재측정)

**왜 이 항목인가**: 벤치마크에서 건당 `BEGIN·INSERT·COMMIT`(1,000건)이 7.5초인데 같은 내구성의 autocommit은 0.67초 — RuSQL **자기 자신 안의 약 11배 불균형**이었음. 사용자 방침("MySQL 급으로 끌어올릴 필요 없고 우리만의 최적화를 한다")에 따라 MySQL과의 격차가 아니라 이 내부 불균형을 대상으로 골랐고, MySQL 수치는 맥락용으로만 둠.

**측정으로 원인 찾기** (임시 계측을 넣어 쟀고 끝난 뒤 전부 제거): 한 번의 `BEGIN`/`INSERT`/`COMMIT` 중 `COMMIT`이 거의 전부(0.06 / 0.18 / 약 7ms). COMMIT을 단계별로 나누자 redo 로그 fsync는 0.55ms로 autocommit과 같았고, 나머지가 `commit_finalize` — 트랜잭션의 WAL·undo 레코드를 파일에서 지우는 일 — 이 3.3ms였음. 그 안을 다시 나누니 **파일을 열어 전체를 읽고 파싱하는 데 파일당 약 1.5ms(×2), 실제 비우기(truncate)는 0.07ms**. 즉 "이 파일에 다른 트랜잭션의 레코드가 있는지"를 알아보려고 매번 파일을 읽는 것이 병목(Windows에서 파일 열기마다 실시간 검사가 붙음). 처음 세운 가설("여러 파일을 따로 fsync")은 틀렸음.

**수정** (`TxnIoShared::LogContents`): WAL과 undo 파일에 어떤 트랜잭션의 레코드가 들어 있는지를 메모리에서 추적(파일 경로별). 레코드를 쓸 때 추가하고, 지울 때 — 그 트랜잭션이 파일에 없으면 아무것도 하지 않고(읽기 전용 트랜잭션), 혼자 있으면 **읽지 않고 제자리에서 비우고**, 다른 트랜잭션도 있으면 기존의 읽기+재작성 경로를 타며 그 뒤 추적을 갱신. 이전 실행이 남긴 파일이나 쓰기 실패 뒤처럼 내용을 모를 때는 `known=false`로 두고 첫 제거 때 한 번 읽어 확립. **내구성·복구 프로토콜은 그대로**(redo fsync 뒤 레코드 제거, 파일이 진실이고 추적은 그것을 읽지 않게 해 주는 캐시일 뿐).

**전/후** (같은 PC·같은 상태에서 이전 빌드와 번갈아 잰 A/B, 중앙값):

| 문장 묶음 | 이전 | 이후 |
|---|---|---|
| `BEGIN; INSERT; COMMIT` ×1,000 (bench) | 7.47초 | **0.92초** (8.1배, autocommit 0.67초의 1.4배까지) |
| `BEGIN; INSERT; ROLLBACK` | 7.14ms | 0.56ms |
| `BEGIN; UPDATE; COMMIT` | 7.45ms | 0.86ms |
| `BEGIN; INSERT 5개; COMMIT` | 8.43ms | 1.54ms |
| `BEGIN; SELECT; COMMIT` (읽기 전용) | 0.26ms | 0.17ms |
| 단건 INSERT/DELETE, Bulk, 포인트 조회, autocommit, 한 트랜잭션에 1,000건 | — | 변화 없음(이 경로를 안 탐) |

**측정 방법에 대한 주의**: 같은 날 앞서 잰 건당 트랜잭션 10.87초는 PC가 더 느린 상태(백그라운드 작업·실시간 검사)였고 같은 빌드도 7~11초로 흔들렸음 — 그래서 이 항목의 비교는 전부 같은 상태에서 이전 빌드와 번갈아 쟀고, 앞선 표는 이 표로 대체.

**벤치마크 (이 PC, 같은 상태, 중앙값)** — `bench.py` 5회, 참고용 MySQL 8.4.9(`bench_mysql.py`, 내구성 맞춤: `innodb_flush_log_at_trx_commit=1`·`--skip-log-bin`, 개인 데이터 디렉터리의 임시 mysqld) 3회:

| 항목 | RuSQL | MySQL 8.4 |
|---|---|---|
| 단건 INSERT 10,000건 | 6.68초 | 6.61초 |
| 단건 DELETE 10,000건 | 6.79초 | 7.06초 |
| Bulk INSERT 100,000건(500행 묶음) | 1.52초 | 0.44초 |
| Bulk DELETE 100,000건(PK 범위) | 0.80초 | 0.37초 |
| 포인트 조회, 인덱스 없음(5,000행) | 2.85ms | 0.95ms |
| 포인트 조회, 보조 인덱스 | **0.078ms** | 0.167ms |
| autocommit INSERT 1,000건 | 0.67초 | 0.73초 |
| 건당 BEGIN·INSERT·COMMIT 1,000건 | **0.92초** | 1.24초 |
| 한 트랜잭션에 INSERT 1,000건 | 0.16초 | 0.11초 |

7월 대비 단건 INSERT가 느려진 것(2.83초 → 6.7초)은 퇴보가 아니라 **내구성 확보의 대가**(응답 전에 fsync). 이 PC에서 fsync 한 번이 약 0.55ms라 단건 쓰기는 사실상 디스크가 정하는 바닥이고, 같은 내구성의 MySQL과 같은 값. 남은 RuSQL 고유의 개선 후보는 Bulk 쓰기(10만 건 1.5초 — 문장당 JSON 직렬화·인덱스 갱신·redo 기록 중 어디가 큰지 아직 모름)와 인덱스 없는 스캔(행이 `unordered_map`이라 행마다 복사·해시 비용이 드는 것이 유력한 원인이나 프로파일링은 안 함) → 둘 다 바로 아래 항목에서 프로파일링하고 개선.

**검증**: 신규 Catch2 3케이스(`test_txn.cpp`) — 혼자 있는 트랜잭션/파일에 없는 트랜잭션/다른 트랜잭션의 레코드 보존, 이전 실행이 남긴 파일을 한 번 읽고 이후 추적, 40시드 무작위 연산(append·remove_txn·rewrite_txn·checkpoint·truncate·clear·"재시작")을 메모리 모델과 매 단계 대조(113,440 assertions). 추적의 "레코드 추가"를 일부러 끊은 변형으로 3케이스 중 2개가 실패함을 확인. Release **437 케이스/239,135 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과, Debug도 양쪽 통과. 크래시 퍼저(복구 뒤 인덱스 질의 포함) 순차 160라운드(체크포인트 3KB 100 + 기본 60) + 동시 4클라이언트 70라운드(체크포인트 3KB 40 + 기본 30) 불일치 0 — 트랜잭션 중 `kill -9`, 다른 세션의 열린 트랜잭션 레코드가 있는 상태에서의 커밋이 모두 포함됨.

**Diagram 페이지 갱신**(영어/한국어, 계층형·원통형 두 그림 모두): 내구성의 WAL·Undo 로그 설명에 "어떤 트랜잭션이 파일에 있는지 메모리에서 추적해 COMMIT이 파일을 다시 읽지 않음"을 추가. 새 구성요소가 생긴 것은 아니라 그림의 구조는 그대로.

**차트 스크립트 버그 수정**(`graph.py`): 그래프의 "47×" 같은 배수 라벨을 축 좌표계에 데이터 좌표(수천)로 놓아 그림 바깥 멀리 밀려나, tight bounding box가 거대해져 PNG가 17.7MB였고 수치가 커지자 `MemoryError`로 죽었음 → 축 좌표(0.55)로 수정해 PNG가 130KB로. `result.json`(UI 패널용)·`benchmark_result.png`를 이 표의 값으로 갱신하고 `bench_mysql.py`/`result_mysql.json`을 추가.

**정직한 한계**: ① 추적은 프로세스 안의 메모리 상태 — 엔진은 `SharedDatabase`당 `TxnIoShared`를 하나만 쓰므로 일관되지만, 다른 프로세스가 같은 파일을 고치면 알 수 없음(원래도 지원하지 않는 사용). ② redo로 기술되지 않는 문장(캐스케이드·MERGE 등)을 포함한 트랜잭션의 COMMIT은 아직 매번 WAL 파일을 열어 fsync하는 옛 경로를 탐. ③ COMMIT의 하한은 redo fsync 한 번(약 0.55ms) — 이보다 낮추려면 여러 세션의 커밋이 한 번의 fsync를 나눠 쓰는 그룹 커밋에 기대야 함.

### 10월 2일 — Bulk 쓰기·스캔 비용 절감 (프로파일링 → 직렬화·중복 검사·행 복사 제거) + 복구 때 같은 행이 사라지던 데이터 유실 버그

**왜 이 항목인가**: 바로 위 항목이 남긴 RuSQL 고유의 개선 후보 둘 — Bulk 쓰기(10만 건 1.5초)와 인덱스 없는 스캔(5,000행 포인트 조회 2.9ms) — 은 "어디서 시간을 쓰는지"를 몰라서 후보로만 남아 있었음. MySQL과의 격차가 아니라 자기 측정으로 고른다는 방침(MySQL 급으로 끌어올릴 필요 없음)에 따라 먼저 측정함.

**측정으로 원인 찾기** (임시 계측을 넣어 쟀고 끝난 뒤 전부 제거, 소스에 계측 코드 잔재 없음을 `grep`으로 확인): 500행 INSERT 문장 하나가 계측 빌드에서 약 5.6ms — 행별 검증 루프 2.7ms(기존 행과의 중복 검사 0.87, **같은 문장 안의 중복 검사 0.64**, 갭 락 0.27 …)와 쓰기 구간 2.8ms(행 벡터 push 1.1, PK 트리 삽입 1.0, 행 JSON 직렬화 0.37)로 둘이 비슷하게 나뉨. 스캔 쪽은 `Row`가 `unordered_map<string,string>`이라 **행을 복사하거나 옮기는 것 자체**가 비용의 대부분(MSVC에서는 맵 이동도 노드를 할당) — 조건에 맞는 행을 결과로 복사, 정렬하는 동안 행 이동, 체크포인트 때 테이블 통째 복사.

**수정** (측정으로 가장 큰 것부터; 단계마다 같은 질의의 결과가 이전 빌드와 같은지 확인):
1. **행 직렬화 직접 생성**(`row_json.hpp/.cpp`, 신규): `nlohmann::json j = row; j.dump()`(맵 → JSON 객체 → 문자열)를 키 정렬 순서로 문자열을 바로 만드는 `row_to_json`/`rows_to_json`으로 교체 — **nlohmann과 바이트 단위로 같은 출력**(이스케이프 `\" \\ \b \f \n \r \t`, 그 밖의 0x20 미만은 소문자 `\u00xx`, 유효한 UTF-8은 그대로, 유효하지 않은 UTF-8은 nlohmann에 위임해 예외까지 같게). INSERT/UPDATE/DELETE/redo/트랜잭션/체크포인트 `save_table`/복합 인덱스의 약 26곳.
2. **같은 문장 안 중복 검사 O(n²) → O(n)**: 다중 행 INSERT가 각 행을 앞선 모든 행과 쌍으로 비교하던 것(500행 = 약 12만 번)을 해시 맵(컬럼별 값 → 처음 나온 행)으로. 오류 메시지는 쌍별 비교와 같은 규칙(가장 앞선 행, 가장 낮은 컬럼)으로 같은 문구. 복합 PK도 같은 방식.
3. **보조 인덱스를 문장당 한 번 갱신**(`index_replace_rows`, UPDATE/DELETE에 이미 있던 것을 INSERT에도), `row_pk_pos` 참조 hoist, 벡터 `reserve`.
4. **B+Tree 키 비교(`cmp_keys`) 빠른 경로**: 숫자 파싱을 `string_view`로(문자열 복사 없음), 널 바이트가 없는 평범한 키는 복합 키 분할 루프를 건너뜀, 맨 숫자(앞자리 0 없는 15자리 이하 정수)끼리는 길이 → 글자 비교. 전순서 규칙은 그대로.
5. **`BufferPool::write_through`**: 테이블 파일 기록이 `write_page`(테이블 전체를 풀에 복사) + `flush_page`(그 사본을 저장)였던 것을 "바로 저장하고 캐시 항목 폐기"로 — 10만 행 테이블을 쓰려고 해시 맵 10만 개를 복사하고 풀에 사본을 하나 더 남기던 것이 없어짐. 17곳 교체, 호출처가 없어진 `flush_page`는 삭제.
6. **체크포인트 한도가 테이블 크기를 따름**: 한도 = max(4MB, 행당 128B × 마지막 체크포인트 이후 바뀐 테이블들의 행 수) (`RUSQL_REDO_CHECKPOINT_ROW_BYTES`, 0이면 예전의 고정 4MB). 전에는 큰 테이블에 4MB마다 테이블 전체를 다시 썼음(20만 행을 적재하는 동안 여러 번).
7. **SELECT 스캔이 행을 복사하지 않음**: 단일 테이블 스캔은 `const Row*`로 제자리에서 조건을 평가하고 일치한 행만 복사. 집계·윈도우·GROUP BY·HAVING·DISTINCT·조인이 없는 `ORDER BY`(+LIMIT/OFFSET)는 정렬 키를 행마다 한 번만 읽고 파싱해 **인덱스만 정렬**한 뒤 반환할 행만 복사(`order_rows`, 비교 규칙은 `cmp_key`와 같음).

**전/후** (같은 PC·같은 상태에서 이전 빌드(커밋 `fe96c3d`)와 번갈아 잰 A/B, `bench.py` 5회 중앙값):

| 항목 | 이전 | 이후 |
|---|---|---|
| Bulk INSERT 100,000건(500행 묶음) | 2.36초 | **1.04초** (2.3배) |
| Bulk DELETE 100,000건(PK 범위) | 1.21초 | **0.72초** (1.7배) |
| 포인트 조회, 인덱스 없음(5,000행) | 6.45ms | **1.27ms** (5.1배) |
| 포인트 조회, 보조 인덱스 | 0.131ms | 0.134ms (변화 없음) |
| 단건 INSERT / DELETE 10,000건 | 8.81 / 8.43초 | 8.16 / 8.29초 (fsync가 바닥, 오차 범위) |
| autocommit / 건당 BEGIN·COMMIT / 한 트랜잭션 1,000건 | 0.80 / 1.18 / 0.24초 | 0.79 / 1.16 / 0.24초 (변화 없음) |

스캔 계열(50,000행 테이블, 질의마다 결과 캐시를 무효화하고 14회 중앙값, 두 번 반복해 같은 경향; 결과 지문은 모든 질의에서 이전 빌드와 동일):

| 질의 | 이전 | 이후 |
|---|---|---|
| `COUNT(*)` | 261ms | 79ms (3.3배) |
| `SUM/AVG` | 262ms | 86ms (3.0배) |
| `WHERE val = 4242` / `val < 300` / `code LIKE 'CODE4999%'` (인덱스 없음) | 91 / 100 / 99ms | 23 / 27 / 25ms (약 4배) |
| `ORDER BY val DESC LIMIT 10` | 446ms | 40ms (11배) |
| `GROUP BY` / `DISTINCT` | 343 / 251ms | 183 / 96ms (1.9 / 2.6배) |
| 조인(필터 후 2,000행과 조인) | 280ms | 257ms (거의 그대로) |

서버 재시작(`kill -9` 뒤 복구, 20만 행): 적재 4.9초 → 1.9초, 질의 가능해질 때까지 4.0 → 2.6초, 첫 `COUNT(*)` 0.94 → 0.32초. 복구 시 redo 로그는 2MB → 19MB로 커졌는데도(체크포인트가 덜 자주 일어나므로) 더 빨리 뜸 — 이전에는 적재 중 테이블 전체 재기록이 반복되었고 시작 후 질의도 느렸기 때문.

**측정 방법에 대한 주의**: 이 PC의 fsync 상태가 앞선 항목 때보다 느려(단건 INSERT 10,000건이 같은 빌드로 6.7초 → 8.2~8.8초) 이전 항목의 표와 숫자를 직접 비교하면 안 됨 — 이 항목의 전/후는 전부 같은 상태에서 번갈아 쟀음. `result.json`/`benchmark_result.png`는 이 5회 중앙값으로 갱신했고, MySQL 참고값(`result_mysql.json`)은 앞선 상태에서 잰 값 그대로(이번에 다시 재지 않음 — MySQL 비교는 맥락용).

**같은 조사에서 발견·수정한 기존 데이터 유실 버그 1건 — 복구 때 "같은 행"이 하나로 합쳐짐**: 기본 키가 없는 테이블에서 한 명시적 트랜잭션이 완전히 같은 행(값·`_xmin`이 모두 같음)을 여러 개 INSERT하고 COMMIT한 뒤 `kill -9`로 서버를 죽이고 다시 띄우면 그 행들이 하나만 남았음(`BEGIN; INSERT (2,'y'),(2,'y'); COMMIT` → 복구 뒤 1행). redo 재생이 "같은 행 이미지가 이미 있으면 건너뜀"으로 멱등성을 확보했는데, 이미지가 같다고 같은 버전은 아니었음. autocommit 문장은 행마다 `_xmin`을 따로 받아 우연히 피했음(그래서 크래시 퍼저가 못 잡았고, 문장 단위 `_xmin`으로 바꾸려다 이 문제를 발견). 재생을 **개수 기반**으로 변경: 기록된 같은 이미지의 k번째 사본은 테이블에 사본이 k개 미만일 때만 추가하고, 같은 트랜잭션이 죽인 같은 이미지의 k번째 `SetXmax`는 이미 그 트랜잭션이 죽인 사본이 k개 미만일 때만 적용(재생 위치는 이미지별 위치 목록). 변경 전 서버(`fe96c3d`)로 재현: 5행이어야 하는데 복구 뒤 4행, 수정 뒤 5행. 이미 테이블 파일에 반영된 redo를 다시 재생해도 아무것도 바뀌지 않는 것(멱등성)도 별도 테스트로 확인.

**검증**:
- 신규/확장 Catch2 12케이스: `test_row_json.cpp`(무작위 문자열·제어 문자·따옴표·역슬래시·다국어 UTF-8·잘못된 UTF-8을 nlohmann의 `dump()`와 바이트 단위로 대조, 빈 행·키 정렬), `test_btree.cpp`(`cmp_keys`를 기준 구현과 무작위 40만 쌍으로 대조 — 숫자처럼 보이는 문자열·앞자리 0·부호·소수·지수·복합 키 포함), `test_bulk_write.cpp`(문장 안 중복 검사를 쌍별 기준 구현과 무작위 대조, 복합 PK 중복, 인덱스가 있는 Bulk INSERT 결과를 스캔과 대조, 재시작, `write_through`, 체크포인트 한도, `ORDER BY`/`LIMIT`/`OFFSET` 무작위 대조), `test_redo.cpp`(같은 행 보존, 이미 반영된 redo 재생이 무변화).
- Release **449 케이스/1,133,945 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0`(모든 DML을 인덱스 경로로 강제) 양쪽에서 통과. Debug도 양쪽 통과(449 케이스/1,133,945 assertions).
- 크래시 퍼저(복구 뒤 인덱스 질의 포함): 순차 60라운드(고정 체크포인트) + 60라운드(체크포인트 3KB) + 40라운드(운영 기본 한도 = 테이블 크기 비례, 긴 redo 로그에서 복구), 동시 4클라이언트 40 + 30 + 20라운드에서 불일치 0(`fuzz_crash.py`/`fuzz_concurrent.py`는 기본으로 `RUSQL_REDO_CHECKPOINT_ROW_BYTES=0`을 줘 작은 한도로 자주 체크포인트하고, 운영 한도는 환경 변수로 재정의해 따로 돌림). 무작위 SELECT 차분 퍼저 150시드(300~449, 1,434,694 assertions), DML 차분 퍼저 60시드(300~359, 1,426,759 assertions) 불일치 0.
- 변경 후 `test_full.sql`/`test_full-ver2.sql`의 에러 수는 변경 전과 동일(3/2).

**Diagram 페이지 갱신**(영어/한국어, 계층형·원통형 두 그림 모두): 실행기에 "단일 테이블 스캔은 행을 복사하지 않고 제자리에서 읽음, ORDER BY…LIMIT은 포인터 정렬, 여러 행 INSERT는 중복 검사·인덱스 갱신을 문장당 한 번", 체크포인트에 "테이블 크기에 비례하는 한도(최소 4MB)", 내구성에 "redo 재생 때 한 테이블의 같은 행도 모두 보존"을 추가. 새 구성요소가 생긴 것은 아니라 그림의 구조는 그대로.

**죽은 코드 정리**: 호출처가 없어진 `BufferPool::flush_page`와 그 선언 삭제, 이번 변경이 남긴 계측 코드 없음 확인. `code/AI/`는 그대로 둠.

**정직한 한계**: ① 집계·GROUP BY·DISTINCT·조인 경로는 아직 행을 복사해서 집계는 2~3배, GROUP BY/DISTINCT는 2배 안팎, 조인은 거의 개선이 없음(`compute_aggregates`가 `vector<Row>`를 받는 구조) → 바로 아래 항목에서 해결. ② 서버 시작 때 테이블 파일 읽기(20만 행 약 2.6초 중 대부분)는 그대로 — 행마다 `unordered_map`을 만드는 비용. ③ redo로 기술되지 않는 문장을 포함한 트랜잭션의 COMMIT(캐스케이드·MERGE 등)은 아직 매번 WAL 파일을 열어 fsync하는 옛 경로. ④ 체크포인트 한도가 커져 크래시 직후 복구가 재생할 redo가 길어질 수 있음(20만 행 테이블에서 19MB를 재생해도 부팅이 더 빨랐지만, 한도는 `RUSQL_REDO_CHECKPOINT_ROW_BYTES`로 조절). ⑤ 복합 인덱스는 UPDATE/DELETE 후보 탐색에 아직 쓰지 않음. ⑥ `code/test/perf/chart.py`는 예전 결과 형식용이라 현재 `result.json`과 맞지 않음(그대로 둠).

### 10월 3일 — 집계·GROUP BY·DISTINCT·조인의 행 복사 제거, LEFT JOIN·다중 조건 ON의 해시 조인, 연쇄 조인이 틀린 답을 내던 기존 버그

**왜 이 항목인가**: 바로 위 항목의 한계 ① — 집계·GROUP BY·DISTINCT·조인은 아직 행마다 `unordered_map`을 복사해 2~3배밖에 못 줄였고 조인은 거의 그대로였음. 조인은 어디서 시간을 쓰는지 몰라서 먼저 쟀음.

**측정으로 원인 찾기** (임시 계측, 끝난 뒤 전부 제거 — `grep`으로 잔재 없음 확인): 5만 행 테이블과 2,000행 테이블의 선택적 조인(`WHERE t.val < 50`, 250행 반환)이 약 270ms인데 — 왼쪽 테이블 복사 65ms, 조인 알고리즘 165ms(IndexNL이 왼쪽 행마다 B+Tree를 찾고 오른쪽 행의 JSON을 파싱), 조인 뒤 WHERE 50ms. 즉 **병합 행 5만 개를 다 만들어 놓고 99.5%를 버렸음**. 필터 없는 5만 행 조인(690ms)은 조인 알고리즘 약 130ms 외에 결과 벡터에 `push_back`하며 행을 다시 옮기는 데 270ms, 서식 140ms. 그리고 `LEFT JOIN`과 `ON a = b AND ...`처럼 등호가 여럿인 ON은 **중첩 루프**여서 쌍마다 병합 행을 만듦(5,000 × 2,000행의 LEFT JOIN이 14초, 5만 행은 분 단위).

**수정**:
1. **집계·GROUP BY·HAVING·OFFSET/LIMIT·DISTINCT·ORDER BY가 행 포인터 위에서 동작**: `compute_aggregates`/`compute_agg_from_key`가 `const Row*` 목록을 받고, 단일 테이블 스캔은 테이블 행을 가리키는 포인터(`rows_p`)로 끝까지 가며, 조인·윈도우 함수가 만든 새 행은 `result`에 두고 같은 방식으로 가리킴. 마지막에 반환할 행만 복사(`result`가 만든 행이면 이동). GROUP BY는 키 값을 길이 접두사로 인코딩한 해시 테이블로 그룹을 찾고(첫 등장 순서 유지, 그룹은 행 복사본 대신 포인터를 가짐), DISTINCT도 해시(전엔 지금까지 본 키 목록을 선형 검색해 O(n·서로 다른 값 수)). 정렬은 항상 안정 정렬 하나(전엔 큰 결과에서 병렬 비안정 정렬이라 동순위 행의 순서가 실행마다 달랐고 `GROUP_CONCAT`·부동소수점 `SUM`의 결과도 따라 달라짐 — 이전 빌드는 같은 질의를 순차/병렬로 두 번 돌리면 800개 중 47개가 달랐고 새 빌드는 0개).
2. **`format_rows`**: 서식 단계(`format_result`)가 `vector<Row>`를 값으로 받아 복사하던 것을, SELECT 목록 서브쿼리와 FOR UPDATE/SHARE가 없는 문장은 행을 복사하지 않고 포인터로 읽도록 분리(`format_result`는 서브쿼리 값을 주입하는 데 필요한 경우만).
3. **조인 결과가 `WHERE` 없이 그대로면 벡터를 통째로 이동**, 정렬·LIMIT가 없으면 행을 다시 옮기지 않음.
4. **WHERE 선적용(pushdown)**: 최상위 AND로 묶인 항 가운데 **한 테이블의 열만 읽는 것**을 조인 전에 그 테이블의 행에 적용(왼쪽 = FROM 테이블은 INNER/LEFT 모두, 오른쪽은 INNER 조인만) — 조인이 병합 행을 훨씬 적게 만듦. 안전장치: 전체 WHERE는 조인 뒤에 그대로 다시 평가하므로 선적용은 "WHERE가 어차피 버릴 행을 일찍 버리는" 지름길일 뿐. 읽는 테이블이 확실한 경우만 선적용 — `<table>.<열>`(DB 접두가 붙은 내부 이름 `d.t`와 질의에 쓴 `t` 둘 다 인정), 이름만 쓴 열은 그 열을 가진 **첫 번째** 테이블(`merge_right`가 기존 키를 덮어쓰지 않으므로), 우변이 식·서브쿼리·함수이거나 **다른 테이블의 열 이름처럼 보이는 식별자 리터럴**(평가가 식별자 모양의 리터럴을 먼저 열로 찾음: `t.grp = 'qty'`)이면 선적용하지 않음. USING/NATURAL/LATERAL/RIGHT/FULL/CROSS 조인이 있는 문장, 테이블 이름이 겹치는 문장도 제외.
5. **ON 등호로 해시한 뒤 ON 전체를 다시 검사하는 조인**(`hashed_join_verified`): LEFT JOIN, 계획기가 알고리즘을 정하지 못한(NestedLoop) INNER JOIN, 첫 번째가 아닌 INNER JOIN에서 ON의 최상위 AND 항 중에 `<왼쪽 열> = <오른쪽 테이블>.<열>`이 있으면, 오른쪽 행을 그 열로 해시(숫자는 숫자값으로 정규화, NULL은 어디에도 매칭되지 않음)하고 왼쪽 행마다 같은 키의 오른쪽 행만 후보로 삼아 **ON 조건 전체**를 병합 행에 다시 평가. 후보를 빼기만 하고 더하지는 않으므로 결과는 중첩 루프와 같은 행, 같은 순서(왼쪽 순서, 오른쪽 순서)이고 LEFT는 매칭 안 된 왼쪽 행을 NULL로 채움. 키를 읽을 수 없는 행이 하나라도 있으면 중첩 루프로 되돌아감.
6. **IndexNL이 같은 키의 오른쪽 행을 한 번만 찾고 파싱**: 같은 키를 가진 왼쪽 행이 많으면(5만 개의 왼쪽 행이 2,000개의 키를 나눠 가질 때) 오른쪽 행의 JSON 파싱이 5만 번 → 2,000번.

**같은 조사에서 발견·수정한 기존 버그 1건 — 세 번째 이후 조인이 틀린 답**: `t JOIN u ON u.id = t.grp JOIN w ON w.k = u.id`처럼 두 번째 이후 조인의 ON이 앞서 조인한 테이블의 열을 읽으면, 계획기가 고른 해시/정렬 병합/IndexNL이 왼쪽 열을 **이름만**(`id`)으로 읽어 이미 합쳐진 행에서는 FROM 테이블의 `id`를 가져왔음. 90/12/18행 테이블에서 208행이어야 하는 결과가 16행(중첩 루프로 쓴 같은 질의는 208행; 계획기가 둘 다 `Hash Join`을 골랐음). 이전 빌드(`b3ca102`)에서 재현. 첫 번째가 아닌 INNER 조인은 위 5번의 검증 해시 조인(ON에 쓴 열 이름을 `get_col`로 그대로 읽음)으로 처리해서 해결. 부수 효과로 첫 번째가 아닌 조인에서 계획기가 SortMerge를 골랐을 때 키 순서로 나오던 행이 이제 왼쪽 순서로 나옴(순서는 원래 보장하지 않음).

**전/후** (같은 PC·같은 상태에서 이전 빌드(`b3ca102`)와 번갈아 잰 3회 중앙값, 5만 행 테이블 + 2,000행 테이블, 신규 `code/test/perf/bench_query.py`; 질의마다 결과 캐시 무효화):

| 질의 | 이전 | 이후 |
|---|---|---|
| `COUNT(*)` | 87.7ms | **18.2ms** (4.8배) |
| `SUM`/`AVG` | 87.6ms | 23.7ms (3.7배) |
| `GROUP BY` (50그룹) | 189ms | **34.3ms** (5.5배) |
| `DISTINCT grp` | 104ms | 31.5ms (3.3배) |
| `SELECT id, val` (5만 행 반환) | 219ms | 80ms (2.7배) |
| 조인 + `WHERE t.val < 50` (250행) | 310ms | **38.5ms** (8.0배) |
| 조인, WHERE 없음 (5만 행 반환) | 824ms | 363ms (2.3배) |
| 조인 + `GROUP BY` | 736ms | 301ms (2.4배) |
| `LEFT JOIN` + `WHERE t.val < 50` | **40초 초과(타임아웃)** | **39.5ms** |
| `JOIN ... ON a = b AND t.val > 5000` | 40초 초과 | 496ms |
| `LEFT JOIN ... ON a = b AND t.val > 5000` | 40초 초과 | 439ms |
| 인덱스 없는 필터 스캔, `ORDER BY ... LIMIT 10` | 21~40ms | 같음(이미 위 항목에서 줄임) |

5,000행 테이블에서 이전 빌드의 LEFT JOIN은 14.2초 → 6.5ms, 두 조건 ON의 INNER JOIN은 13.5초 → 55ms. 5,000행 선택적 조인(IndexNL)은 30 → 7.8ms.

**검증**:
- 신규 Catch2 6케이스(`test_query_paths.cpp`): 복합 키 인코딩(`("ab","c")`와 `("a","bc")`는 다른 그룹), GROUP BY(COUNT/SUM/MIN/MAX/COUNT DISTINCT/`GROUP_CONCAT`)·WHERE·HAVING·ORDER BY·LIMIT·OFFSET을 **C++로 다시 구현한 기준**과 250회, DISTINCT + ORDER BY/LIMIT/OFFSET을 기준과 150회, **조인 400회를 "같은 질의의 ON과 WHERE를 `(...) OR 1 = 0`으로 감싼 쌍둥이"(최상위가 OR라 해시도 선적용도 못 하는 중첩 루프 경로)와 대조**(INNER/LEFT, 최대 3개 테이블, ON 20종, WHERE는 한 테이블·여러 테이블·모호한 이름·열 이름 같은 리터럴·NULL/빈 값/숫자 모양 키), NULL·숫자 모양 키의 LEFT JOIN 기대값을 손으로 적은 케이스, IndexNL 캐시. 쌍둥이 테스트는 `RUSQL_FUZZ_SEEDS`/`RUSQL_FUZZ_START`로 늘릴 수 있어 4,000개씩 3번(12,000개)도 통과.
- **심은 버그(mutation)로 테스트가 버그를 잡는지 확인**: 13종 가운데 11종(이름만 쓴 열의 소유 테이블 규칙, 열 이름처럼 보이는 리터럴 선적용, LEFT 조인 오른쪽 선적용, 매칭 안 된 LEFT 행 누락, INNER 해시 조인의 NULL 채움, 계획기 알고리즘을 해시로 덮어쓰기, 숫자 키를 글자로 해시, IndexNL 캐시 키, GROUP BY 키의 길이 접두사, 그룹 안 행 순서, NULL 키 매칭+ON 재검사 제거)을 차분 도구 또는 Catch2 테스트가 잡고(Catch2만으로 8종), 나머지 2종(ON 재검사만 제거, NULL 키만 해시)은 ON 재검사·NULL 제외가 서로를 받쳐 주는 안전망이라 결과를 바꾸지 않는 동치 변형. 처음에는 5종이 살아남았고, 계측으로 새 경로가 실제 실행되는지 세어 보다가 **선적용이 DB 접두가 붙은 내부 이름(`d.t`) 때문에 거의 안 걸리고 LEFT 해시 조인이 한 번도 실행되지 않던 구현 결함**을 발견해 고침 — 0 차이만 보고 안심하면 안 된다는 사례.
- **빌드 간 차분 도구 신설**(`code/test/diff/diff_builds.py`, 사용법은 같은 폴더 README.txt): 두 `engine_server` 빌드에 같은 스키마·데이터·무작위 질의(집계 전 종류·FILTER, GROUP BY/HAVING, DISTINCT, ORDER BY/LIMIT/OFFSET, 윈도우, 서브쿼리, FOR UPDATE, 격리 수준별 트랜잭션, 조인 등)를 보내 출력 텍스트를 비교. 이전 빌드 대비 최종 빌드로 무작위 질의 16,500개(시드 51~63·81~86, 연쇄 조인 참조는 `--skip-chain-refs`로 제외한 것 포함)에서 차이는 위 연쇄 조인 버그를 건드리는 5건뿐(쌍둥이 질의로 새 쪽이 맞음을 확인)이고 나머지 0; 새 빌드끼리 순차/병렬 모드를 **텍스트 그대로** 비교하면 12,000행에서 800개 질의 모두 같음(이전 빌드는 47개가 다름).
- Release/Debug **455 케이스/1,135,762 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. 무작위 SELECT 차분 퍼저 150시드(500~649, 1,431,038 assertions), DML 퍼저 60시드(500~559, 1,367,757 assertions), 크래시 퍼저 30라운드(체크포인트 3KB) + 동시 15라운드 불일치 0.

**Diagram 페이지 갱신**(영어/한국어, 계층형·원통형 두 그림 모두): 실행기 설명에 "스캔·집계·GROUP BY·DISTINCT·ORDER BY는 테이블 행을 복사하지 않고 포인터로 처리, 조인은 한 테이블만 읽는 WHERE 조건을 조인 전에 적용하고 LEFT JOIN과 여러 조건 ON을 등호로 해시 처리"를 추가. 새 구성요소가 생긴 것은 아니라 그림의 구조는 그대로.

**죽은 코드 정리**: 이번 변경으로 쓸모가 없어진 `presort` 특수 경로(일반 포인터 경로로 흡수), 조인 쪽 `right_rows_raw` 이중 복사, 집계의 행 복사본 인자는 모두 대체됐고 남은 호출처 없는 함수 없음을 확인. `code/AI/`는 그대로 둠.

**정직한 한계**: ① 조인이 만드는 병합 행(왼쪽 행 복사 + 오른쪽 열을 두 이름으로 삽입)은 여전히 행마다 해시 맵 하나라 필터 없는 5만 행 조인이 360ms. ② 선적용은 한 테이블의 열만 읽는 최상위 AND 항에 한정 — OR로 여러 테이블이 섞인 조건, 함수·산술·서브쿼리가 들어간 조건은 조인 뒤에 걸러짐. 오른쪽 선적용은 INNER 조인만. ③ RIGHT/FULL OUTER/CROSS/NATURAL/USING 조인은 아직 중첩 루프(RIGHT/FULL OUTER는 아래 두 번째 뒤 항목에서 해결). ④ 계획기의 EXPLAIN은 이번에도 LEFT JOIN을 "Index NL Join"으로 표시하지만 실제로는 해시로 실행됨(EXPLAIN과 실행 경로가 어긋남) → 바로 아래 항목에서 해결. ⑤ **WHERE 안의 스칼라 서브쿼리(`WHERE val > (SELECT AVG(val) FROM t)`)는 행마다 다시 실행**되어 12,000행에서 2분을 넘김 → 바로 아래 항목에서 해결. ⑥ 첫 번째 INNER 조인에서 계획기가 SortMerge를 고르면 여전히 키 순서로 출력(원래 동작).

### 10월 3일 — WHERE 안 서브쿼리를 문장당 한 번만 실행, `인덱스 등호 AND …`의 인덱스 시작, 행 JSON 직접 파싱(서버 시작 23% 단축), LEFT JOIN EXPLAIN 정정

**왜 이 항목인가**: 바로 위 항목의 한계 ⑤(WHERE 안 스칼라 서브쿼리가 행마다 재실행되어 12,000행에서 2분 초과)가 이번 라운드 가장 큰 성능 절벽이었음. 같은 방식(질의 형태별 시간을 먼저 재고, 이전 빌드와 결과를 비교)으로 서브쿼리 전반을 훑었음.

**측정으로 원인 찾기** (5,000행 `t` + 2,000행 `u`, 임시 계측은 끝난 뒤 제거):
- `IN (SELECT …)`(비상관)만 캐시가 있었고(문장당 1회), **스칼라 비교(`val > (SELECT AVG(val) FROM t)`)와 `EXISTS`는 비상관이어도 외부 행마다 서브쿼리 전체를 다시 실행**: 3.7초, 2.1초. WHERE에 스칼라 서브쿼리가 든 `UPDATE`는 7.4~7.8초.
- 상관 서브쿼리(`EXISTS (SELECT 1 FROM u WHERE u.id = t.grp AND u.flag = 1)`)는 2.5초 — 외부 행당 0.5ms. 문장 복사·치환(28ms/2만 회)이나 계획 수립(26ms)이 아니라 **서브쿼리 안의 스캔**이었음: 치환 뒤 조건이 `u.id = 5 AND u.flag = 1`인데 **SELECT 계획기는 AND 조건에서 인덱스를 전혀 고르지 않았음**(UPDATE/DELETE에는 "AND의 인덱스 가능한 항 하나로 시작해 전체 조건으로 재검사"가 이미 있었음). `WHERE id = 5 AND val > 3`이 PK가 있어도 풀 스캔.
- 서버 시작은 행마다 `nlohmann::json::parse(...).get<Row>()`(json 트리를 만든 뒤 `Row`로 복사, 4열 행 약 10us) — 직전 항목에서 쓰기 쪽(`row_to_json`)만 직접 생성으로 바꿨고 읽기 쪽은 그대로였음.

**수정**:
1. **비상관 서브쿼리 캐시 확대**(`subquery_scalar_cache_`, `subquery_exists_cache_`): 서브쿼리 AST의 주소로 키를 잡아 문장마다 비움(기존 `IN` 캐시와 같은 방식). **캐시해도 되는 조건**: `substitute_correlated_condexpr`가 서브쿼리의 WHERE를 바꿀 수 없을 때 — 점이 든 비숫자 리터럴(`t.grp`)이나 점이 든 열 참조(산술식 안 포함)가 없을 때(`cond_may_be_substituted`). 그렇지 않으면 예전처럼 외부 행마다 치환해 실행. 서브쿼리 오류도 같은 방식으로 한 번만 평가하고 이후 행은 `false`. (`RAND()`가 든 비상관 서브쿼리는 이제 문장당 한 번 평가 — `IN`이 원래 그랬고 SQL 표준 의미와도 같음.)
2. **SELECT 계획기가 `AND` 안의 점 인덱스를 시작점으로 사용**(`Planner::point_leaf_of_and`): 단일 테이블 SELECT가 풀 스캔으로 정해졌고 조건이 AND이면, 그 가운데 PK 등호·해시 인덱스 등호·B+Tree 인덱스 등호 하나로 시작(후보 행마다 전체 조건 재검사 — 인덱스는 후보 공급자일 뿐). 범위 조건은 일부러 제외(큰 범위를 인덱스 JSON으로 읽는 것은 스캔보다 비쌈). 조인이 있으면 적용하지 않음(그쪽 기본 접근은 조인 알고리즘 비용 추정에만 쓰임). **커버링은 조건이 단일 조건일 때만**: 인덱스 항목에는 그 열만 있어서 AND의 나머지 조건을 못 평가함(처음부터 막지 않았다면 `SELECT grp FROM g WHERE grp = 3 AND val > 5`가 틀린 답을 냈을 것 — 테스트로 확인).
3. **행 JSON 직접 파싱**(`row_from_json`/`rows_from_json`, `row_json.cpp`): `nlohmann::json::parse(text).get<Row>()`와 같은 결과를 내되, 엔진이 쓰는 형태(문자열 값만 있는 평평한 객체, 키가 엄격히 증가)는 직접 읽고 그 밖의 모든 것(다른 값 형식, 정렬 안 된 키·중복 키, nlohmann이 거부할 이스케이프·UTF-8·짝 없는 서로게이트, 문법 오류)은 nlohmann에 넘겨 같은 결과·같은 예외. 키 순서까지 같아야 `Row`의 순회 순서가 이전과 같음(`DISTINCT *`의 키가 순회 순서에 의존). 테이블 파일 로딩, 인덱스 버킷, PK 점 조회, redo 재생, undo, 트랜잭션 롤백 등 문자열→`Row` 27곳에 적용.
4. **LEFT JOIN의 EXPLAIN**: 계획기가 LEFT JOIN에도 "Index NL Join"을 표시했지만 실제로는 해시로 실행됨(직전 항목) — 이제 `Hash Join`으로 표시.

**전/후** (같은 PC·같은 상태에서 직전 빌드(`d86fd9d`)와 번갈아 잰 결과):

| 항목 | 이전 | 이후 |
|---|---|---|
| 비상관 스칼라 서브쿼리 `WHERE val > (SELECT AVG(val) FROM t)` (5,000행) | 3,693ms | **2.9ms** |
| 비상관 `EXISTS` | 2,130ms | **1.5ms** |
| 상관 `EXISTS (… u.id = t.grp AND u.flag = 1)` | 2,535ms | **55ms** (46배) |
| 상관 `IN (… WHERE u.id = t.grp AND …)` | 2,533ms | **60ms** (42배) |
| `UPDATE … WHERE val > (SELECT AVG(val) FROM t)` | 7,846ms | **62ms** (126배) |
| 서버 시작, 20만 행, 체크포인트 직후 | 1.33초 | **1.03초** (23% 단축) |
| 서버 시작, 20만 행, `kill -9` 뒤 redo 재생 | 2.2초 | 1.8초 |
| `bench.py` 단건 쓰기·Bulk·트랜잭션·인덱스 조회 | — | 변화 없음(3회 번갈아, 비율 0.97~1.02) |

**검증**:
- 신규/확장 Catch2: `test_query_paths.cpp`에 "인덱스 등호 AND …" 1케이스(PK·해시·B+Tree 인덱스, `3`/`3.0`/`03` 같은 숫자 모양 키, NULL, 다른 조건과 `OR`/`NOT`, 선택 열이 인덱스 열 하나뿐인 경우, DML 뒤까지 — 300개 조건을 같은 조건에 `OR id < 0`을 붙여 스캔으로 강제한 쌍둥이와 대조, 그리고 EXPLAIN이 인덱스를 쓰는지/안 쓰는지 확인), `test_row_json.cpp`에 2케이스(`row_to_json`이 쓴 4만 행 왕복을 키 순서까지 nlohmann과 대조, 손으로 쓴 이상한 텍스트 40여 개와 **무작위로 한 바이트 바꾸거나 지우거나 끼우거나 자른 텍스트 6만 개**를 nlohmann과 대조 — 예외가 나는 경우와 값까지 같아야 함).
- **심은 버그로 확인**: 상관 서브쿼리를 비상관처럼 캐시하면 차분 도구가 질의 1,500개 중 515개에서 차이를 잡고, 커버링 제한을 풀면 AND 테스트가 실패하고, 파서가 정렬 안 된 키를 직접 받아들이면 `row_json` 테스트가 실패.
- 빌드 간 차분 도구(`diff_builds.py`)에 서브쿼리 생성기(스칼라 비교 5연산자, 한 행·여러 행·없음·NULL·오류를 내는 서브쿼리, `EXISTS`/`NOT EXISTS`/`IN`/`NOT IN`, 상관·비상관, `UPDATE`/`DELETE` 안의 서브쿼리)와 `인덱스 등호 AND …` 생성기를 추가해 직전 빌드 대비 6,000개 질의에서 차이 0.
- Release/Debug **458 케이스/1,356,824 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. 무작위 SELECT 차분 퍼저 150시드(900~1049, 1,432,317 assertions), DML 퍼저 60시드(1,456,272 assertions), 크래시 퍼저 60라운드(체크포인트 3KB) + 30라운드(운영 한도) + 동시 30라운드 불일치 0 — 파서가 redo 재생·undo·롤백 경로에도 쓰이므로 포함.

**Diagram 페이지 갱신**(영어/한국어, 계층형·원통형 두 그림 모두): 플래너에 "인덱스 등호가 든 AND는 그 인덱스로 시작", 실행기에 "비상관 서브쿼리(스칼라·EXISTS·IN)는 문장당 한 번 실행"을 추가. 구성요소가 새로 생긴 것은 아니라 그림의 구조는 그대로.

**정직한 한계**: ① 상관 서브쿼리는 여전히 외부 행마다 한 번씩 실행(55ms/5,000행 = 행당 약 10us까지는 줄었지만 안쪽 조건에 쓸 인덱스가 없으면 행당 안쪽 테이블 스캔 — 5,000 × 2,000행이면 수 초). 서브쿼리를 해시 세미조인으로 풀어 쓰는 것(decorrelation)이 다음 후보 → 바로 아래 항목에서 해결(문장 범위의 임시 해시 인덱스). ② `AND`는 점 조건(PK/해시/B+Tree 등호)만 인덱스로 시작 — 범위 조건과 `OR`는 스캔. ③ 서버 시작은 20만 행에 약 1초(테이블 파일 읽기와 PK 트리 재구성이 아직 순차). ④ `bench_query.py`의 질의 경로(필터 스캔·정렬 등)는 이번 변경이 건드리지 않아 그대로.

### 10월 3일 — 상관 서브쿼리를 위한 문장 범위 임시 해시 인덱스, RIGHT/FULL OUTER JOIN 해시 조인, 전체 행 UPDATE 1.8배

**왜 이 항목인가**: 바로 위 항목이 남긴 다음 후보 — 안쪽 열에 인덱스가 없는 상관 서브쿼리(`orders.customer_id`처럼 FK 열에 인덱스를 안 만든 흔한 경우)가 외부 행마다 안쪽 테이블을 전부 스캔. 그 김에 같은 방식으로 100,000행에서 maintenance·벌크·분석 문장을 한 번씩 재서 이상치를 찾았음.

**측정으로 찾은 것** (임시 계측, 끝난 뒤 제거 — `grep`으로 잔재 없음 확인):
- 상관 `EXISTS`/`NOT EXISTS`/`IN`/스칼라가 `cust`(3,000행) × `ord`(3,000행, `cust_id`에 인덱스 없음)에서 각 약 3.5초 — 외부 행마다 0.9~1.2ms의 안쪽 스캔.
- `RIGHT JOIN`/`FULL OUTER JOIN`은 2,000 × 2,000행에 각 약 6.5초(쌍마다 병합 행을 만드는 중첩 루프 — LEFT/INNER만 직전 항목에서 고쳤음).
- 100,000행 전체 `UPDATE`가 약 4초(행당 40us). 단계별로 재니 `txn.log_update` 252ms 중 대부분이 행마다 `nlohmann::json::parse`로 `_xmin`을 읽는 것(`xmin_of_image`), 그리고 **문장 끝에서 모든 행의 옛/새 이미지 JSON을 다시 파싱하는 데 947ms**(전체의 약 4분의 1) — 인덱스 유지에 쓰려고 만든 이미지였음.
- 그 밖에 느린 것으로 보인 `ANALYZE`(1.5초), `INSERT … SELECT`(1.7초), `UNION ALL`(1.6초), `NATURAL JOIN`(2,000행 3.3초)은 이번에 건드리지 않았음(아래 한계).

**수정**:
1. **문장 범위 임시 해시 인덱스**(`point_index_cache_`): 순수 읽기 문장(`is_pure_read_only`)이 한 테이블에서 `<인덱스 없는 열> = <상수>`를 **세 번째** 찾는 순간 그 테이블의 행을 그 열의 값(숫자는 숫자 키)으로 한 번 묶어 두고, 이후 조회는 묶음만 읽음. 후보는 전체 WHERE와 가시성을 그대로 통과해야 하므로 묶음은 훑을 행을 줄일 뿐 결과를 바꾸지 않음(순서는 테이블 순서). 조건: 512행 이상, 최상위 AND 항의 `열 = 상수`(상수가 그 테이블의 열 이름처럼 보이면 제외), 조인 없음. **수명은 문장 하나**: `execute()`가 문장 시작과 끝에 비우고(저장 프로시저 안에서 호출된 문장이 바깥 문장의 것을 물려받거나 남기지 않도록 이전 값을 복원), 쓰는 문장(INSERT/UPDATE/DELETE/DDL)과 FOR UPDATE는 사용하지 않아서 읽는 도중 테이블이 바뀔 수 없음 — 묶음이 행 포인터를 들고 있으므로 핵심 불변식.
2. **RIGHT / FULL OUTER JOIN 해시 조인**: `hashed_join_verified`에 RIGHT·FULL OUTER 추가 — ON 등호의 한쪽을 해시, 반대쪽 행마다 후보를 찾고 ON 전체를 재검사. 행 순서와 NULL 채우기는 중첩 루프와 같음(RIGHT: 오른쪽 순서대로 매칭 안 된 오른쪽 행은 왼쪽 첫 행의 일반 열을 NULL로 채움; FULL: 왼쪽 순서로 처리한 뒤 매칭 안 된 오른쪽 행을 뒤에 붙임).
3. **UPDATE의 행 이미지 복사 제거**: 보조/해시/복합 인덱스가 없는 테이블은 옛/새 행 이미지 자체가 필요 없음(PK B+Tree는 새 버전이 이미 가진 JSON 텍스트를 그대로 받음) → 만들지 않음. 인덱스가 있는 테이블은 JSON을 다시 파싱하는 대신 버전을 만들 때 복사해 둠(복사와 파싱의 비용이 비슷해 이쪽은 이득이 거의 없음). 트랜잭션 로그의 `_xmin` 읽기는 `nlohmann` 전체 파싱 대신 `row_from_json`.

**전/후** (같은 PC·같은 상태에서 직전 빌드(`16e7704`)와 번갈아 잰 결과):

| 항목 | 이전 | 이후 |
|---|---|---|
| 상관 `EXISTS`, 인덱스 없는 FK (3,000 × 3,000행) | 3,563ms | **46ms** (78배) |
| 상관 `NOT EXISTS` / `IN` / 스칼라 비교 | 3,516 / 3,568 / 3,564ms | 42 / 43 / 45ms |
| 상관 `EXISTS` + 나머지 조건 | 3,538ms | 50ms |
| SELECT 목록의 상관 스칼라 서브쿼리 (1,000행) | 1,204ms | 28ms |
| `RIGHT JOIN` / `FULL OUTER JOIN` (2,000 × 2,000행) | 6,505 / 6,511ms | **42 / 37ms** (155 / 176배) |
| 100,000행 `UPDATE t SET note = note` (PK만 있는 테이블) | 4.0초 | **2.2초** (1.8배) |
| 100,000행 `UPDATE t SET val = val + 1` (PK만) | 4.1초 | 2.1~2.6초 |
| 같은 UPDATE, 보조 인덱스가 있는 테이블 | 4.4초 | 4.1초 |
| 100,000행 `DELETE`, `bench.py`의 단건 쓰기·Bulk·트랜잭션·인덱스 조회 | — | 변화 없음(3회 번갈아, 비율 0.94~1.02) |

**검증**:
- 신규 Catch2 3케이스 + 확장: `test_query_paths.cpp`에 ① "인덱스 없는 열에 대한 상관 서브쿼리"(`EXISTS`/`NOT EXISTS`/`IN`/스칼라 비교/SELECT 목록, 열 값은 `7`/`007`/`7.0`/`07`/`1e1`/NULL 같은 숫자 모양 포함, 120개를 같은 질의의 서브쿼리 WHERE를 `(…) OR 1 = 0`으로 감싸 인덱스를 못 쓰게 한 쌍둥이와 대조), ② "문장 범위 임시 인덱스는 문장을 넘지 않는다"(DELETE/INSERT 뒤의 다음 문장이 새 행을 보는지, 서브쿼리가 든 UPDATE, 열린 트랜잭션 안의 쓰기와 ROLLBACK 뒤), ③ "UPDATE가 인덱스를 테이블과 같게 유지"(PK만/B+Tree/해시/복합 인덱스 테이블에서 일반 변경·인덱스 열 변경·**PK 자체의 변경과 연쇄 변경**을 섞은 240문장, 문장마다 인덱스 경로와 `OR id < 0` 스캔 쌍둥이 대조, 재시작 뒤에도 대조), 조인 쌍둥이 테스트에 RIGHT/FULL OUTER 추가(+ 외부 조인만 있는 질의는 **행 순서까지** 같아야 함, 8,000개 더 확인).
- **심은 버그로 확인**: 임시 인덱스가 값을 숫자 키가 아닌 글자로 묶거나, 문장 시작·끝에 비우지 않거나(→ 다음 문장이 오래된 포인터를 읽음), FULL OUTER가 매칭 안 된 오른쪽 행을 빠뜨리거나, RIGHT가 채우기를 안 하거나, UPDATE가 필요한 이미지를 만들지 않거나, PK B+Tree에 옛 버전 텍스트를 넣거나 — 모두 테스트가 실패.
- 빌드 간 차분 도구에 RIGHT/FULL OUTER 조인 생성기 추가, 직전 빌드 대비 1,200행 테이블에서 3,600개 질의 차이 0(서브쿼리가 임시 인덱스를 쓰려면 안쪽 테이블이 512행 이상이어야 해서 `--rows 1200`).
- Release/Debug **461 케이스/1,358,781 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. 무작위 SELECT 차분 퍼저 150시드(1100~1249, 1,432,752 assertions), DML 퍼저 80시드(인덱스 경로 강제, 1,915,112 assertions), 크래시 퍼저 60라운드(체크포인트 3KB) + 30라운드(운영 한도) + 동시 30라운드 불일치 0.

**Diagram 페이지 갱신**(영어/한국어, 계층형·원통형 두 그림 모두): 실행기에 "LEFT·RIGHT·FULL JOIN … 해시 처리"와 "읽기 문장이 인덱스 없는 열을 반복 조회하면 그 문장 동안만 임시 해시 인덱스를 만듦"을 추가. 구성요소가 새로 생긴 것은 아니라 그림의 구조는 그대로.

**정직한 한계**: ① 임시 해시 인덱스는 순수 읽기 문장의 조회에만 — `UPDATE … WHERE EXISTS (상관 서브쿼리)`처럼 쓰는 문장 안의 서브쿼리는 안쪽 열에 인덱스가 없으면 여전히 행마다 스캔(쓰기 중에는 테이블이 바뀔 수 있어 안전한 수명을 보장할 수 없음). ② 임시 인덱스는 `열 = 상수`만 — 범위·LIKE·`열 IN (…)`은 스캔. ③ `NATURAL JOIN`(2,000행 3.3초)과 `USING`(2,000행 0.15~0.25초, 비교만 하는 이차 루프)은 아직 중첩 루프. ④ 인덱스가 있는 테이블의 전체 행 UPDATE(100,000행 4초)는 버킷 재기록이 지배해서 거의 그대로. ⑤ `ANALYZE`(100,000행 1.5초)·`INSERT … SELECT`(1.7초)·`UNION ALL`(1.6초)은 측정만 하고 손대지 않았음.

### 10월 3일 — 성능 작업 동결, 마지막 점검에서 찾은 임시 인덱스 버그, 벤치마크 갱신, 발표용 요약

**왜 이 항목인가**: 이번 날 네 번째 성능 커밋까지 엔진 약 1,400줄·테스트 약 1,350줄이 `executor_select.cpp`·`join.cpp`·`row_json.cpp`에 들어갔고, 사용자가 "어디까지 개선해야 하느냐, 아예 갈아엎는 것 아니냐"고 물었다. 중단 기준을 먼저 정했다: **흔한 SQL 모양에 이차 시간 절벽이 없고, 10만 행 이하 데모에서 1초를 넘는 문장이 없을 것**. 이 기준은 직전 커밋(145b5d8)에서 이미 충족돼 있어(남은 것은 `ANALYZE`·`INSERT … SELECT`·`UNION ALL`의 1.5~1.7초 같은 상수 배수뿐) 성능 작업을 동결하고, 사용자가 고른 대로 ① 최종 회귀 ② 벤치마크 갱신 ③ `exec_select` 큰 변경 재검토 ④ 발표용 한 장 요약만 했다.

**재검토에서 찾은 버그(직전 커밋 145b5d8에서 들어온 것)**: 문장 범위 임시 해시 인덱스(`point_index_cache_`)는 실제 테이블 행의 **포인터**를 묶어 둔다. 그런데 뷰나 FROM 서브쿼리는 결과를 임시 테이블(`s.tables[alias]`)에 넣었다가 문장이 끝나면 지우고, 다음 문장이 다른 행으로 다시 만든다. 순수 읽기 문장이 이 임시 테이블(512행 이상)에서 `열 = 상수`를 세 번 찾으면 인덱스가 곧 지워질 임시 테이블의 행을 가리키게 되어, 이후 조회가 이미 해제된 메모리를 읽었다(뷰를 둔 테스트에서 SIGSEGV로 재현). 기존 테스트는 실제 테이블에서만 임시 인덱스를 검사해 놓치고 있었다. 수정: `temporary_tables_`(지금 `s.tables`에 결과가 들어 있는 뷰·FROM 서브쿼리 이름)를 두고 `exec_select_with_subquery`가 넣고 빼며, 임시 인덱스는 이 집합에 있는 테이블에는 만들지 않음(결과는 그대로 스캔). 같은 자리에서 지역 변수 `columns`가 가리던 바깥 이름을 `table_columns`로 정리.

**검증**:
- 신규 Catch2 1케이스(뷰 위의 반복 점 조회가 스캔과 같은 답, `Shape{before,cond,after}`로 뷰 앞뒤 형태를 바꿔 가며): 수정 전 빌드에서 크래시 재현 → 수정 후 통과. 심은 버그(`pi_view_guard`: 보호 조건 제거)가 이 테스트에 잡힘.
- 최종 회귀 Release/Debug **462 케이스/1,358,797 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. 무작위 SELECT 차분 퍼저 150시드, DML 퍼저 80시드, 조인 쌍둥이 4,000건, 크래시 퍼저 90라운드, 동시 퍼저 30라운드 불일치 0.
- 이번 날 쓴 모든 임시 계측 잔재 없음 확인(`grep UPROF|SQPROF|JPROF|UPlap`).

**벤치마크 갱신**(`bench.py` 최종 빌드 5회, 항목별 중앙값 → `result.json`, `benchmark_result.png`): 단건 INSERT/DELETE 10,000건 7.40/7.59초, Bulk INSERT/DELETE 100,000건 **0.76/0.54초**(10월 2일 작업 전 2.36/1.21초 — 날짜·PC 상태가 달라 절댓값끼리의 비교는 참고용이고, 같은 상태에서 번갈아 잰 쌍은 위 항목의 표), 인덱스 포인트 조회 SeqScan 0.962ms vs B+Tree 0.109ms(8.8배), 트랜잭션 1,000건 AutoCommit 0.78초 / 건당 BEGIN·COMMIT 1.16초 / 한 트랜잭션 0.20초. 5회 가운데 단건 INSERT 22.8초, AutoCommit 4.6초 같은 값이 하나씩 튀었고(백신·디스크 fsync 지연) 중앙값이 이를 흡수. 단건 쓰기는 fsync가 바닥이라 이번 작업으로 달라지지 않음.

**발표용 자료**: `docs/mds/SUMMARY.md`(한 장 요약: 규모·엔진 구성·성능·검증 방법·찾아 고친 버그·한계) 신설, `code/test/perf/speedup_chart.py` → `speedup_chart.png`(같은 상태에서 이전 빌드와 번갈아 잰 쌍의 개선 배율만 로그 눈금으로, 서로 다른 날의 절댓값은 섞지 않음).

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없고(버그 수정과 문서뿐) 이전 항목에서 갱신한 설명 그대로 유효해 변경 없음.

**정직한 한계**: ① 점검은 `exec_select` diff를 사람이 다시 읽고 컴파일러 경고·심은 버그·퍼저로 확인한 것이라 "버그가 더는 없다"는 증명이 아님 — 이번에 찾은 것도 테스트가 아니라 포인터 수명을 따져 읽다가 발견. ② 동결은 이번 학기 방침이며, 남은 후보는 `PLAN.md`에 기록.

### 10월 4일~5일 — 교수님께 보여 드릴 시연 자료, 쓰지 않는 자료를 `archive/`로 정리

**시연 자료**(`code/test/demo/`): 사석에서 3분쯤 보여 드릴 용도라 길게 만들지 않았다 — `seed.py`(`demo` 데이터베이스에 고객 5,000행 + 주문 100,000행, 시드 고정, 약 1.3초)와 한 장짜리 `DEMO.md`(① 10만 행 집계·조인 ② 인덱스를 만들면 EXPLAIN의 계획이 Seq Scan → Index Scan으로 바뀜 ③ 서버를 강제 종료한 뒤 재시작해도 커밋한 데이터는 남고 커밋하지 않은 것은 사라짐 ④ 선택: Claude MCP 자연어 질의). `test_full.sql`은 데이터가 몇 행뿐이고 `DROP DATABASE`로 시작하므로 시연에는 쓰지 않고 기능 폭의 회귀 확인용으로만 둠. 시연 순서를 실제 서버에 그대로 돌려 `DEMO.md`의 모든 시간·출력은 측정값(10만 행 `GROUP BY` 0.07초, 조인+집계 0.76초, 인덱스 전 35ms → 후 1ms 미만, 크래시 후 재시작 약 3초, 커밋한 행만 복구).

**리허설에서 찾은 엔진 문제 2건**(다음 항목에서 수정): ① `orders.customer_id`에 인덱스를 만들면 `orders JOIN customers`가 0.7초 → 약 2초로 느려짐(플래너가 인덱스 조인을 고르는데 비용식이 프로브마다 일치하는 행 수를 빼먹음). ② `SELECT COUNT(*) … WHERE 인덱스 열 = 값`이 EXPLAIN에는 Index Scan인데 실제로는 테이블 전체를 읽음(집계 질의가 인덱스 경로 조건 `!has_agg`에서 제외돼 있던 기존 동작). 리허설 중에 "결과 캐시는 테이블 단위라 다른 테이블을 UPDATE해도 비워지지 않는다"는 것도 확인(처음엔 이를 모르고 0.000초를 인덱스 효과로 잘못 읽을 뻔함).

**`archive/` 정리**: 사용자가 만든 폴더로 `code/AI/`(사설 NL→SQL 파인튜닝 실험), `docs/legacy/`(1학기 docx·옛 다이어그램), `code/test/perf/chart.py`(현재 `result.json`과 형식이 맞지 않던 7월 차트 스크립트)를 옮김. 먼저 옛 경로를 가리키는 곳이 없음을 확인(CMake·프론트·MCP·테스트·산출물 D01~D08 모두 없음, 이동 전후 파일 수 일치: AI 9개, docs 21개). 문서의 경로(`AI.md`, `FUNCTIONS.md`), README 트리, perf `README.txt`를 고치고 `archive/README.md`를 신설(폴더별 내용과 보관 이유, 1학기 Rust 원본은 `legacy` 브랜치에 있음). 이 항목 이전의 기록(예: 위의 9월 `code/AI/` 항목들)은 당시 경로 그대로 둠. `AI.md`의 "열린 항목"이던 `code/AI/` 보존/삭제는 "삭제하지 않고 `archive/AI/`로 보존"으로 결정됨.

### 10월 5일 — 시연 연습에서 찾은 엔진 문제 2건 수정(FK 인덱스 조인, 집계의 인덱스 미사용)과 그 과정에서 찾은 기존 버그 2건(미수정)

**왜 이 항목인가**: 위 항목의 시연 연습에서 찾은 두 문제를 사용자가 수정하기로 했다. 동결 때의 판단("10만 행 이하 데모에서 1초를 넘는 문장이 없다")이 틀렸던 부분이다.

**수정 ① FK 열에 인덱스를 만들면 조인이 느려짐**(`orders JOIN customers`가 해시 조인 0.8초 → Reverse Index NL 2.0초): 플래너 비용식 `right_size × log2(left_size)`이 비유일 인덱스를 한 번 찾을 때 그 키의 **모든** 행이 나오고(실행기가 각 행을 JSON에서 파싱·복사) 그 비용을 한 행으로 셌다. 첫 행을 뺀 나머지 행마다 해시 조인 행 5개분(실측: 10만 행에서 2.5배)을 부과 — 한 번 찾을 때 나오는 행 수는 왼쪽 열의 고유값 수(ANALYZE)로, **통계가 없으면 인덱스를 16번 찔러 본 평균 버킷 크기**로 구함(`Planner::sampled_rows_per_key`). 후자가 필요했던 이유: 자동 ANALYZE는 문장 수를 세서 500행씩 200문장으로 적재한 테이블은 영영 통계가 없음(처음엔 통계 없으면 한 행으로 가정했다가 시연 데이터에서 안 고쳐진 걸 다시 재서 발견). 유일 키(PK)는 그대로.

**수정 ② 집계·ORDER BY·LIMIT·DISTINCT·윈도우 쿼리가 인덱스를 안 씀**(`SELECT COUNT(*) … WHERE 인덱스 열 = 값`이 EXPLAIN은 Index Scan인데 10만 행을 전부 읽음): 인덱스 지름길이 `!has_agg` 등으로 "집계·정렬·LIMIT 없는 SELECT"에만 걸려 있었다(원본 포팅 때부터). UPDATE/DELETE가 이미 검증을 거친 후보 탐색(`dml_index_positions`)을 SELECT가 재사용: 단일 테이블·서브쿼리 없는 WHERE·FOR UPDATE/SHARE 아님·고정 스냅샷 아님이면 일치하는 행의 위치를 구해(각 후보를 실제 행·전체 WHERE로 재검사, 의심스러우면 "사용 불가" → 스캔) 그 행 포인터에서 나머지 파이프라인을 이어감. 위치는 테이블 순서라 출력 순서도 같음. **후보 상한을 SELECT는 테이블의 1/32로**(UPDATE/DELETE는 1/8 그대로): 처음엔 1/8을 그대로 썼다가 선택도 스윕을 재니 넓은 범위(12~50%)에서 스캔보다 최대 2배 느려졌다 — 인덱스 후보 하나는 스캔 행의 약 10배 비용(손익분기 약 6%). 그래서 상한을 줄이고, 버킷을 파싱하기 전에 `"_xmin"` 개수로 먼저 세어(범위는 통째로) 상한을 넘으면 파싱 없이 포기.

**전/후** (같은 PC, 이전 빌드와 번갈아, 중앙값 5회):

| 항목 | 이전 | 이후 |
|---|---|---|
| `COUNT(*)`/`SUM`·`AVG`/`GROUP BY`/`ORDER BY … LIMIT`/`DISTINCT`/`AND` 조합, 인덱스 열 `= 값` (10만 행) | 41~43ms | **0.3~0.4ms** |
| 같은 모양, PK `= 값` | 41ms | 0.3ms |
| 같은 모양, `BETWEEN` 1,100행(1.1%) | 45ms | 1.8ms |
| FK 인덱스 있는 `orders JOIN customers` 개수 | 1,981ms | **727ms** |
| 같은 조인 + `WHERE` + `GROUP BY` | 2,568ms | **1,064ms** |
| FK 인덱스 없는 같은 조인(변화 없어야 함) | 792 / 980ms | 846 / 1,120ms |
| 범위 선택도 스윕 `customer_id < X`: 0.2% / 1% / 2% | 46 / 46 / 47ms | **1.1 / 7 / 12.7ms** |
| 같은 스윕 5% 이상(스캔으로 돌아감) | 45~47ms | 47~53ms(`COUNT`), 최대 약 +16% |

**검증**:
- 신규 Catch2 7케이스: 플래너 2(행 수 비용, 통계 없이 표본 추출), `test_dml_index` 4(집계·GROUP BY·DISTINCT·LIMIT·윈도우 등 20가지 쿼리가 인덱스를 쓰는지/안 쓰는지와 결과가 스캔과 같은지, 1/32 상한, 다른 세션의 열린 트랜잭션·고정 스냅샷·자기 트랜잭션·ROLLBACK, 재시작 직후 위치 캐시가 빈 상태), `test_executor_select` 1(FK 인덱스 조인이 해시로 바뀌고 인덱스 유무와 무관하게 같은 답). 기존 `test_dml_index`의 무작위 차분 퍼저에 SELECT 모양 10종을, `test_select_index` 퍼저에 집계·GROUP BY·DISTINCT·LIMIT·윈도우를 추가. 기존 테스트 1건(`size-asymmetric INNER JOIN chooses ReverseIndexNL`)은 데이터를 바꿈 — 고객 2명에 주문 50개를 25개씩 나누던 데이터는 새 비용식에서 해시 조인이 더 싸서(결과는 같음) 계획이 Hash로 바뀜. 같은 테스트의 의도(Reverse Index NL 실행 경로가 맞는 행을 줌)를 유지하려고 매칭 안 되는 주문을 섞어 프로브당 행 수가 적게 만듦.
- Release/Debug **469 케이스/1,371,000 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. SELECT 차분 퍼저 150시드(1100~1249, 1,819,528 assertions), DML 퍼저 80시드(2000~2079, 1,946,487 assertions), 크래시 퍼저 90라운드, 동시 퍼저 30라운드 불일치 0.
- 빌드 간 차분(`diff_builds.py`, 이전 빌드 대비): **플래너 표본 추출만 끈 빌드는 4시드(행 1,200/6,000) × 1,500질의 = 6,000질의 차이 0**(집계 인덱스 시작은 결과를 바꾸지 않음). 최종 빌드는 계획이 바뀐 조인에서 같은 행 집합이 **다른 순서로** 나오고 `LIMIT`과 겹치면 다른 행이 잘림 — 아래 기존 버그 때문에 `ORDER BY`가 정렬을 안 하는 쿼리의 순서는 원래 조인 알고리즘에 달려 있어서 계획이 바뀌면 달라지는 것. 차이가 난 쿼리를 모두 분류: 순서만 다르거나, `LIMIT`이 있거나, `SELECT DISTINCT u.name`(아래 두 번째 버그).
- 심은 버그 9종 중 8종을 새 테스트가 잡음(집계 경로에서 서브쿼리 조건 보호, 잠금 읽기 제외, 위치 어긋남, 집계 제외, 행 수 비용 제거, 유일 키 구분 제거, 표본 추출 제거, 상한 1/8). 하나(후보 탐색 결과를 버리고 스캔)는 결과가 같아서 결과만 보는 테스트로는 구분 불가(동등 변이).

**리허설에서 찾은 기존 버그 2건(원본 포팅 때부터, 수정 안 함 — 사용자 결정 사항)**:
1. **`ORDER BY 테이블.열`(한정 이름)이 정렬을 안 함**: `SELECT id FROM a ORDER BY a.id DESC`가 1,2,3,4,5로 나옴(`ORDER BY id`는 정상). 비교기가 행 키를 한정 이름 그대로 `Row::find`로 찾는데 행의 키는 열 이름뿐이라 모든 값이 같다고 보고 정렬이 일어나지 않음(`row_order_less`, 이번 변경 전 커밋 `fe96c3d`에도 똑같이 있음). 조인에서는 결과가 조인 알고리즘의 출력 순서가 됨.
2. **`SELECT DISTINCT 테이블.열`이 한 행만 돌려줌**: `SELECT DISTINCT a.g FROM a`가 {2,1,3} 대신 2 하나. DISTINCT 키 계산이 같은 방식으로 `row.find(한정 이름)`을 해 모든 행의 키가 빈 문자열이 됨. 열 이름만 쓰면 정상, `GROUP BY 테이블.열`·`WHERE`·SELECT 목록의 한정 이름은 정상.

**Diagram 페이지**(영어/한국어, 계층형·원통형): 플래너에 "집계·GROUP BY·DISTINCT·ORDER BY … LIMIT도 인덱스로 시작", "비유일 인덱스 조인은 한 번 찾을 때 나오는 행 수까지 비용에 반영"을 추가. `tsc --noEmit` 통과.

**정직한 한계**: ① 서버를 재시작한 직후에는 행 위치 캐시가 비어 있어(읽기만으로는 못 채움) 인덱스를 쓰는 `UPDATE`/`DELETE`가 한 번 지나가야 집계의 인덱스 시작이 시작됨(시연 `DEMO.md`에 적음). ② 왼쪽이 10만 행 테이블인 조인은 선택적이어도 0.9~1.0초(조인 전에 왼쪽 행을 복사하는 비용이 지배하는 것으로 보임, 프로파일링은 안 함). ③ 통계가 없을 때의 행 수는 16행 표본의 평균 버킷 크기라 분포가 심하게 치우친 열에서는 어긋날 수 있음. ④ 후보 상한 1/32와 "행 5개분"은 이 PC에서 잰 상수 하나씩.

### 10월 5일 (두 번째) — `ORDER BY 테이블.열`이 정렬 안 하던 것, `SELECT DISTINCT 테이블.열`이 한 행만 돌려주던 것 수정

**왜 이 항목인가**: 바로 위 항목에서 찾은 기존 버그 2건을 사용자가 수정하기로 했다. 원본 Rust 포팅 때부터 있던 것(`fe96c3d`의 같은 코드에도 있음)이라 이번 날의 작업 때문에 생긴 회귀는 아니다.

**원인**: 행은 열을 맨 이름(`id`)으로 보관하고 오른쪽 조인 테이블의 열만 `d.u.id` 같은 한정 키를 하나 더 가진다. 그런데 `ORDER BY`(`row_order_less`/`order_rows`)와 `DISTINCT`의 키 계산은 열 이름을 질의에 쓴 그대로 `Row::find`로 찾았다. `a.id`는 키가 아니라서 값이 비어 있었고, 정렬은 모든 행이 같다고 보고 안정 정렬이 아무것도 안 했고(조인에서는 조인 알고리즘이 내는 순서가 그대로 나옴), DISTINCT는 모든 행의 키가 빈 문자열이라 한 행만 남겼다. 파서는 별칭을 테이블 이름으로 풀어 주므로(`x.id` → `테이블.id`) 별칭도 같은 문제였다. WHERE·GROUP BY·SELECT 목록은 이미 `get_col`(정확한 키 → `테이블.열` 접미 일치 → 열 이름)을 쓰고 있었다.

**수정**: `order_rows`·그룹 행 재정렬(`row_order_less`)·DISTINCT의 `Column`/`ColumnAlias`가 `get_col`로 읽는다(`get_col`이 private라 정렬 함수에는 함수 포인터 `RowLookup`으로 넘김). 열 이름만 쓴 질의는 정확한 키를 먼저 보므로 같은 경로.

**검증**:
- 신규 Catch2 2케이스(`[qualified]`): ① **두 테이블이 같은 열 이름(`id`, `grp`)을 가진 상태에서** 무작위 질의 720개 — 별칭 유무, INNER/LEFT JOIN, 1~4개 열 선택, 정렬 키 0~2개 + 유일한 마지막 키(알고리즘과 무관한 답), `LIMIT`/`OFFSET`, 모든 선택 열로 정렬한 DISTINCT, 정렬 없는 DISTINCT(LEFT JOIN의 NULL 포함), 한정 이름으로 `GROUP BY`한 뒤 한정/맨 이름으로 `ORDER BY` — 를 **테스트 안에서 데이터로 계산한 기준값**과 비교(엔진의 다른 철자와 비교하지 않음). 여섯 가지 크기·인덱스 구성으로 Nested Loop·Hash·Index NL·Reverse Index NL을 모두 거치게 하고 `EXPLAIN`으로 확인. ② 열 이름이 겹치지 않는 테이블에서 한정 ↔ 맨 이름 쌍둥이 21쌍(단일 테이블·별칭·조인·LEFT JOIN·그룹·LIMIT·인덱스 top-K 경로·DISTINCT·윈도우·CTE·뷰·파생 테이블·서브쿼리). 긴 캠페인(30시드, 10,800질의)도 통과.
- 심은 버그 5종 중 4종을 새 테스트가 잡음(정렬 한 곳, DISTINCT 두 곳, 한정자를 떼고 읽기). 하나(그룹 행 재정렬이 한정 이름을 못 읽게)는 같은 키로 행을 먼저 정렬하기 때문에 결과가 같은 동등 변이.
- 새 도구 `code/test/diff/verify_orderby_distinct.py`: 빌드 하나를 `diff_builds.py`와 같은 무작위 질의(이름이 겹치는 4개 테이블, 열을 `t.id`로 씀)로 검사 — DISTINCT는 DISTINCT 없는 같은 문장의 서로 다른 행 집합과 같고 중복이 없는지, ORDER BY는 ORDER BY 없는 같은 문장과 같은 행을 내는지와 첫 정렬 키가 선택 열이면 그 열이 순서대로인지. **이전 빌드는 위반을 찾아내고**(`t.id ASC`인데 399 다음 13), 새 빌드는 5시드 × 3,000질의(DISTINCT 1,305건, ORDER BY 596건, 순서까지 확인한 276건)에서 위반 0.
- 빌드 간 차분(`diff_builds.py`, 이전 빌드 대비 4시드 × 1,500 = 6,000질의): 차이 216건이 **전부** `ORDER BY`에 한정 이름이 있거나 `DISTINCT` 한정 열이 있는 질의(분류 스크립트로 확인)이고 나머지 5,784개는 글자까지 같음 — 고친 것 말고는 아무 결과도 바꾸지 않음.
- 성능 변화 없음(50,000행, 이전 → 이후): `ORDER BY val DESC LIMIT 10` 37.8 → 34.9ms, `DISTINCT grp` 30.7 → 30.3ms, `GROUP BY` 33.9 → 30.3ms.
- Release/Debug **471 케이스/1,373,288 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. SELECT 차분 퍼저 150시드, DML 퍼저 80시드, 크래시 퍼저 90라운드, 동시 퍼저 30라운드 불일치 0.

**같은 점검에서 찾았지만 고치지 않은 것(사용자 결정 대기)**: ① **집계 인자의 한정자가 파서에서 버려짐** — 열 이름이 겹치면 `SUM(b.id)`·`COUNT(b.id)`·`MAX(b.id)`가 왼쪽(FROM) 테이블의 열을 읽는다(재현: `a(id, g)`, `b(id, nm)`에서 `a LEFT JOIN b`의 `COUNT(b.id)`가 짝이 없는 행도 세고 `SUM(b.id)`가 `SUM(a.id)`와 같은 6, 헤더도 둘 다 `SUM(id)`). `parser_select.cpp`가 `.`을 만나면 한정자를 버리고 열 이름만 남김. 고치려면 한정 이름을 보존하고 헤더 규칙을 정해야 함. ② `HAVING SUM(a.v) > 30`은 `Expected ')' after aggregate` 파싱 오류(조용한 오답은 아님).

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음. **정직한 한계**: 정렬에서 NULL이 어디에 놓이는지(`NULL`을 글자로 비교해 숫자보다 큼: 오름차순 맨 뒤, 내림차순 맨 앞)는 이번에 건드리지 않았고, 테스트도 NULL이 정렬 키가 되는 경우를 일부러 빼 두었음. 기존 동작 그대로이며 MySQL(NULL이 가장 작음)과 다름.

### 10월 5일 (세 번째) — 집계 인자의 `테이블.열`(한정자 보존)과 HAVING 안의 한정 이름·`COUNT(열)` 수정

**왜 이 항목인가**: 바로 위 항목에서 찾았지만 고치지 않은 두 가지(집계 인자의 한정자가 파서에서 버려지는 것, `HAVING SUM(a.v)`가 파싱 오류인 것)를 사용자가 수정하기로 했다. 원본 Rust 포팅 때부터 있던 문제다.

**원인과 영향**: 파서가 집계 인자의 `.`을 만나면 한정자를 버렸다(`SUM(b.id)` → `SUM(id)`). 두 테이블에 같은 열 이름(`id`)이 있으면 실행기는 누구의 열인지 알 수 없어 항상 왼쪽(FROM) 테이블의 열을 읽었다: `LEFT JOIN`에서 `COUNT(b.id)`가 짝이 없는 행도 세고(주문 없는 고객이 1), `SUM(a.id)`와 `SUM(b.id)`가 같은 수·같은 이름(`SUM(id)`)으로 나옴. 결과 열 이름이 같으면 집계 행의 같은 키를 덮어쓰는 문제도 있었다. HAVING의 집계는 `SUM(a.v)`처럼 점이 있으면 `Expected ')' after aggregate` 파싱 오류.

**수정**:
- **AST**(`SelectColumn::Agg`/`AggAlias`): `col`은 인자를 쓴 그대로(`b.id`, `o.amount`)이고 결과 열 이름이 된다(`SUM(o.amount)`, MySQL과 같음). 별칭은 파서가 알고 있으므로 `source`에 테이블 이름으로 풀어 둔다(`orders.amount`; 별칭이 없으면 비어 있음). 뷰·프로시저가 저장되는 JSON 형태에도 `source`를 넣고(비어 있으면 안 씀), 예전에 저장된 뷰는 `source` 없이 그대로 읽힌다.
- **파서**: 집계·`GROUP_CONCAT`의 인자와 `SUM(col > x)` 꼴의 조건이 한정자를 보존. 별칭 풀기를 집계의 `FILTER` 조건과 윈도우 함수의 열·`PARTITION BY`·`ORDER BY`까지 넓힘. HAVING 안의 집계 이름(`SUM(o.amount)`)도 안의 인자를 풀고 `table.column`을 받음.
- **실행기**: `resolve_arg_key`가 인자를 행이 실제로 가진 키(맨 이름 또는 `<db>.<table>.<column>`)로 **집계·그룹당 한 번** 해석하고 그 키로 읽는다. 윈도우 함수는 이미 같은 이름 해석(`get_col`)을 쓰고 있어 파서만 고쳐 해결.
- **같은 함수에서 함께 고친 것**: `compute_agg_from_key`(HAVING의 집계)가 `COUNT(열)`을 NULL을 세지 않고 **그룹의 행 수**로 돌려주고 있었다. select 목록에 같은 집계가 있으면 그 값을 재사용해서 가려졌지만, 없으면(`SELECT c.name … HAVING COUNT(o.id) = 0`) 주문 없는 고객을 못 찾았다. 이제 NULL이 아닌 값만 센다(`COUNT(*)`는 그대로).

**검증**:
- 신규 Catch2 7케이스: 파서 구조(집계 인자·별칭·FILTER·윈도우·HAVING), JSON 왕복(예전 형태 포함), ① **같은 열 이름의 두 테이블에서 무작위 집계 질의 640개**(스칼라/그룹, 별칭 유무, INNER/LEFT JOIN, `COUNT`·`COUNT(DISTINCT)`·`SUM`·`AVG`·`MIN`·`MAX`, 텍스트 열의 `MIN`/`MAX`, 선택 안 된 집계의 HAVING)를 테스트 안에서 데이터로 계산한 기준값과 비교(결과 열 이름도 확인), ② 결과 열 이름(타이핑한 그대로), ③ 주문 없는 고객 `LEFT JOIN`(select 목록과 HAVING의 `COUNT`, 별칭, 윈도우), ④ 한정 ↔ 맨 이름 쌍둥이 15쌍(스캔·조인·LEFT JOIN·그룹·뷰·CTE·파생 테이블·FILTER·윈도우·HAVING), ⑤ 한정자와 별칭이 든 집계의 뷰를 저장 후 서버 재시작해 다시 읽기. 긴 캠페인(30시드, 9,600질의)도 통과.
- 심은 버그 10종을 **모두** 새 테스트가 잡음(한정자 버리기, 별칭 미해석 두 곳, 키 미해석, HAVING COUNT 행 수, 결과 열 이름을 해석된 쪽으로, HAVING·윈도우·FILTER의 별칭 풀기, JSON의 `source`). (심은 버그를 되돌리는 내 도구가 교체 텍스트가 빈 문자열인 두 건을 되돌리지 못해 소스에 두 줄이 빠진 채로 전체 회귀를 돌렸고, 그 회귀가 바로 3건을 잡아서 알아챘다 — 복구하고 도구를 고친 뒤 전체 검증을 처음부터 다시 돌렸다.)
- 새 도구 `code/test/diff/verify_aggregates.py`: 이름이 겹치는 4개 테이블의 INNER/LEFT/RIGHT/FULL OUTER 조인(별칭·WHERE 포함)에서 `GROUP BY`한 집계의 모든 그룹을 **같은 FROM을 집계 없이 읽은 행에서 다시 계산**(NULL 건너뜀, 숫자 열의 `SUM`/`AVG`/`MIN`/`MAX`, 모든 값이 숫자일 때만 숫자 비교하는 엔진의 `MIN`/`MAX` 규칙)하고 결과 열 이름까지 확인. **이전 빌드는 위반**(`AVG(u.id)`가 4.53이어야 하는데 214.3 = `t.id`의 평균), 새 빌드는 5시드 × 1,500질의(약 7,300문장, 58,000그룹) 위반 0.
- 빌드 간 차분(`diff_builds.py`에 `--ignore-header` 추가: 행만 비교): 이전 빌드 대비 6개 시드, 약 9,000질의에서 **차이 0**(말뭉치의 집계는 기본 테이블 열이라 값이 안 바뀌어야 함). 같은 비교에 `ag_noresolve` 변이 빌드를 넣으면 41건을 찾아내 도구가 눈이 있음을 확인. 헤더까지 비교하면 4시드 240건이 달라지고 **전부** 한정된 집계 인자(`SUM(t.val)`)가 든 문장(결과 열 이름이 `SUM(val)` → `SUM(t.val)`).
- Release/Debug **478 케이스/1,386,692 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. SELECT 차분 퍼저 150시드, DML 퍼저 80시드, 크래시 퍼저 90라운드, 동시 퍼저 30라운드 불일치 0.
- 성능(50,000행, 번갈아 두 번씩): `COUNT(*)` 13.0 → 13.7ms, `SUM`/`AVG` 11.4 → 12.0ms, `GROUP BY` 16.9 → 16.3ms — 오차 범위(인자 해석은 집계·그룹당 해시 조회 한 번).

**눈에 띄는 변화(의도한 것)**: 한정자가 든 집계의 **결과 열 이름이 쓴 그대로** 나온다(`SUM(o.amount)`, 전에는 `SUM(amount)`). 한정하지 않은 집계(`SUM(amount)`)와 `AS` 별칭은 그대로. 기존 테스트 중 이 이름에 기대는 것은 없었다.

**이 점검에서 새로 찾았지만 고치지 않은 것(→ 사용자 승인으로 같은 날 네 번째 항목에서 수정)**: **select 목록의 집계 간 산술**(`MAX(v) - MIN(v)`, `SUM(v) / COUNT(*)`)은 피연산자 집계가 select 목록에 따로 들어 있을 때만 맞는다. 아니면 `GROUP BY`가 있어도 0이고, `GROUP BY`가 없으면 집계로 취급되지 않아 **입력 행마다 0 한 행**이 나온다(`SELECT MAX(v) - MIN(v) FROM a` → 5행). HAVING의 같은 식은 맞다(`extract_agg_refs_from_cond`가 미리 계산).

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음. **정직한 한계**: FILTER 안에 `SUM(o.amount > 5)` 꼴로 쓰는 조건부 집계(`CASE` 변환)의 안쪽 열은 별칭이 풀리지 않는다(별칭을 쓰지 않거나 기본 테이블 열이면 정상, 오른쪽 조인 테이블 열을 별칭으로 쓰는 드문 경우만 영향).

### 10월 5일 (네 번째) — select 목록의 식·함수·CASE 안에 든 집계(`MAX(v) - MIN(v)`, `ROUND(AVG(v), 2)`, `COALESCE(SUM(v), 0)`) 수정

**왜 이 항목인가**: 바로 위 항목에서 찾았지만 고치지 않은 "select 목록의 집계 간 산술"을 사용자가 수정하기로 했다. 조사해 보니 `MAX(v) - MIN(v)`만의 문제가 아니라 **식·함수·CASE 안에 든 집계 전부**가 같은 원인이었다.

**원인과 영향**: 실행기는 select 목록의 `Agg` 열(`SUM(v)`처럼 집계가 열 자체인 것)만 집계로 인식했다. 식(`MAX(v) - MIN(v)`, `SUM(v) * 2`)·함수(`ROUND(AVG(v), 2)`, `COALESCE(SUM(v), 0)`, `UPPER(MAX(g))`)·`CASE WHEN COUNT(*) > 2 …` 안의 집계는 ① 같은 집계가 select 목록에 **따로** 있어야만 값이 있었고(없으면 0 또는 NULL을 이어 붙인 글자, 예: `NULLNULL`), ② 그런 열만 있는 질의는 **집계 질의로 취급되지도 않아** `GROUP BY` 없이 `SELECT MAX(v) - MIN(v) FROM a`가 테이블의 **행마다 0 한 행**을 돌려줬다(`SELECT SUM(v), SUM(v) + 1`은 두 번째 열이 사라짐). HAVING의 같은 식은 `extract_agg_refs_from_cond`가 미리 계산해서 맞았다. 같은 함수(`compute_agg_from_key`, HAVING의 집계 계산기)에 따로 있던 결함도 이번에 같이 드러났다: 함수로 감싼 집계(`HAVING ROUND(AVG(v), 0) >= 30`)·비교 오른쪽의 집계(`HAVING SUM(v) > MAX(v)`)를 계산하지 않았고, `MIN`/`MAX`가 숫자가 아닌 값을 건너뛰어(전부 텍스트인 열이면 ±무한대) select 목록의 같은 집계와 값이 달랐다.

**수정**:
- `Executor::column_agg_refs`/`select_agg_refs`/`column_has_aggregate`/`columns_have_aggregate`(신규): select 열 하나(식·CASE 조건·함수 인자 텍스트)가 품은 집계 호출을 모은다. 함수의 인자는 파서가 **텍스트**로 들고 있어서 텍스트 스캐너(`collect_agg_refs_text`: `COUNT/SUM/AVG/MIN/MAX` + `(`…`)`, 단어 경계, **작은따옴표 안의 글자는 제외** — `UPPER('max(v)')`는 집계가 아님)를 따로 둠. CASE 조건은 비교의 오른쪽 값(리터럴·BETWEEN·IN 목록·산술)까지 훑는다.
- **실행기**: 집계 질의 판정(`has_agg`)이 `columns_have_aggregate`를 쓴다. 그룹 행(`make_group_row`)과 `GROUP BY` 없는 집계 한 행에 select 열이 품은 집계를 `compute_agg_from_key`로 계산해 넣은 뒤 식·함수·CASE를 평가한다. `GROUP BY` 없는 질의는 **정확히 한 행**(출력은 일반 집계와 같은 형식, "N row(s) returned." 줄 없음). 식에 들어가는 값은 반올림하지 않은 정확한 값(`format_exact`: 정수는 정수, 아니면 왕복 가능한 최단 표기).
- `compute_agg_from_key`를 다시 씀: `DISTINCT` 접두, `COUNT(*)`, `resolve_arg_key`, NULL 제외, `SUM`/`AVG` 정확값, `MIN`/`MAX`는 값이 **전부 숫자면 숫자, 아니면 텍스트**로 비교(select 목록과 같은 규칙), 빈 그룹의 `MIN`/`MAX`는 NULL.
- **파서**: `expand_alias_str`이 문자열 전체에서 집계 호출을 찾아 각 호출의 인자 별칭을 테이블 이름으로 풀어 준다(`ROUND(SUM(o.v) / COUNT(*), 1)`; 따옴표 안은 건드리지 않음).
- **파티션 테이블**은 식 속 집계도 일반 집계처럼 오류로 거절(전에는 자식 테이블별로 조용히 잘못 합쳐질 수 있었음). 재귀 CTE의 반-순진(semi-naive) 판정도 같은 `column_has_aggregate`를 씀.

**검증**:
- 신규 Catch2 3케이스(478 → 481): ① 결정적 케이스 — 스칼라·그룹·조인·LEFT JOIN·별칭·`HAVING SUM(v) > MAX(v)`·함수·CASE·따옴표 속 텍스트·반올림 안 된 값·파티션 거절, ② **같은 열 이름의 두 테이블에서 무작위 식 질의**를 테스트 안에서 데이터로 계산한 기준값과 비교, ③ 파서 — 식·함수 인자·CASE 속 별칭 풀기.
- 심은 버그 14종을 **모두** 새 테스트가 잡음(집계 질의 판정, 그룹 행에 집계 넣기 누락, 스칼라 경로, 스칼라 결과의 꼬리 줄, 함수 인자 텍스트 수집, 산술 속 함수 수집, 비교 오른쪽 값 수집, 정확값 대신 반올림값, NULL 건너뛰기, `COUNT(DISTINCT)`, 텍스트 `MIN`/`MAX`, 파티션 거절, 파서의 별칭 풀기, 따옴표 처리). 도구가 되돌린 소스는 md5로 다시 확인했다.
- 새 도구 `code/test/diff/verify_agg_expressions.py`: 이름이 겹치는 4개 테이블의 INNER/LEFT/RIGHT/FULL OUTER 조인(별칭·WHERE 포함)에서 식·함수·CASE 속 집계의 모든 그룹을 **같은 FROM을 집계 없이 읽은 행에서 다시 계산**하고 스칼라 질의는 정확히 한 행인지 확인. **이전 빌드는 위반**(`SELECT x.grp, AVG(x.price) + MAX(x.price) … GROUP BY x.grp` → `NULLNULL`, 정답 142.83), 새 빌드는 5시드 × 1,500질의(7,279문장, 53,528그룹) 위반 0. `verify_aggregates.py`에는 HAVING 검증(select 목록에 없는 집계의 `HAVING <집계> > k`)을 더해 5시드 6,814문장·50,768그룹(HAVING 1,500)에서 위반 0. `verify_orderby_distinct.py` 3시드 × 3,000질의도 위반 0.
- 빌드 간 차분(이전 빌드 대비, `--ignore-header`가 아닌 전체 비교): 6시드 × 1,500질의(9,000질의)에서 차이 32건이고 **전부** `HAVING`에 `MIN`/`MAX`가 든 문장(말뭉치에는 UPDATE로 생긴 텍스트 값 `NULL1` 등이 있어 예전에는 건너뛰던 값이 이제 텍스트 비교에 들어감, 예: `HAVING MAX(val) > 10`이 `NULL1`이 든 그룹을 이제 포함 — `SELECT MAX(val)`이 같은 그룹에서 보여주는 값과 일치). 말뭉치에는 select 목록 안의 집계 식이 없어 다른 차이는 0.
- Release/Debug **481 케이스/1,395,341 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,946,487 assertions), `[aggregate]` 긴 캠페인 30시드, 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 15,075) 불일치 0.
- 성능(50,000행, 이전/새 빌드를 번갈아 두 번씩): `COUNT(*)` 12.2·10.6 → 10.5·12.5ms, `SUM`/`AVG` 11.5·11.7 → 11.2·11.2, `GROUP BY` 15.8·15.7 → 16.6·15.8, 조인+`GROUP BY` 206·207 → 204·205ms — 오차 범위(집계 식이 없는 질의는 경로가 같다). 집계 식 질의 자체(100,000행 시연 데이터의 스칼라 집계 식)는 약 76ms로 단순 집계(약 35ms)의 두 배 남짓 — HAVING과 같은 계산 경로(`compute_agg_from_key`, 값 문자열을 모아서 계산)를 쓰기 때문(성능 작업은 동결 상태라 최적화하지 않음).

**눈에 띄는 변화(의도한 것)**: 식·함수·CASE 속 집계가 값을 갖고, `GROUP BY` 없는 집계 식은 한 행만 돌려준다. `HAVING`의 `MIN`/`MAX`가 텍스트 값이 섞인 열에서 텍스트로 비교한다(select 목록과 같은 규칙).

**이 점검에서 새로 찾았지만 고치지 않은 것(→ 사용자 승인으로 같은 날 다섯 번째 항목에서 ① 수정)**: ① **산술에서 NULL이 NULL로 계산되지 않음** — `SELECT v + 1`이 NULL인 행에서 `NULL1`(글자 이어 붙이기), `v * 2`가 0(NULL이어야 함)이고, `MIN`/`MAX`가 NULL인 식(`SUM(v) + MAX(v)`)도 같은 영향을 받는다. 집계 식을 고치다가 나온 것이지만 집계와 무관한 `SELECT v + 1`에서도 그대로 나타난다(산술 평가에 NULL 규칙이 없음). ② 함수 열의 결과 열 이름이 인자를 빼고 `ROUND()`/`COALESCE()`로 나온다(MySQL은 `ROUND(AVG(v), 2)`).

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음. **정직한 한계**: select 목록에 같은 `AVG(x)`가 따로 있고 식에도 `AVG(x)`가 쓰이면 식은 select 목록의 4자리 반올림 값을 읽는다(키가 같아서; 다른 집계는 해당 없음). `SUM(o.amount > 5)` 꼴(`CASE` 변환)의 안쪽 열은 오른쪽 조인 테이블 열을 별칭으로 쓰면 별칭이 풀리지 않는다(이전 항목과 같음).

### 10월 5일 (다섯 번째) — 식 속 NULL(`v + 1`, `v * 2`, `x / 0`, `v > 5`, 스칼라 함수 인자)과 함수 인자 속 한정된 열·별칭 수정

**왜 이 항목인가**: 바로 위 항목에서 찾았지만 고치지 않은 "산술에서 NULL이 NULL로 계산되지 않음"을 사용자가 수정하기로 했다. 실제로 조사해 보니 `+`만의 문제가 아니라 **식을 평가하는 곳 전체**가 NULL을 모르고 있었고, 새 검증 도구가 같은 점검에서 함수 인자의 별개 버그 2건을 더 찾았다.

**원인과 영향** (모두 원본 Rust 포팅 때부터):
- **산술**(`eval_arith`): NULL은 엔진 안에서 글자 `"NULL"`이라 `v + 1`은 두 글자를 이어 붙여 `NULL1`, `v - 1`·`v * 2`·`v / 2`는 숫자가 아니라서 0이 됐다. `x / 0`도 0. select 목록의 비교(`v > 5`, `v = NULL`)는 글자 비교로 NULL 행에서 1이 나왔다. 그리고 **`UPDATE a SET v = v + 1`이 그 값을 테이블에 저장**했다(NULL 행이 `NULL1` 글자가 되고, `SET w = w * 2`는 NULL을 0으로 바꿈).
- **스칼라 함수**(`apply_scalar_func`): 인자가 NULL이면 `ROUND(NULL)` 0, `ABS`/`CEIL`/`FLOOR`/`SQRT`/`SIGN` 0, `POWER(NULL, 2)` 0, `LENGTH(NULL)` **4**(글자 "NULL"의 길이), `LOWER(NULL)` `null`, `REVERSE(NULL)` `LLUN`, `LEFT(NULL, 3)` `NUL`, `SUBSTR(NULL, 2, 3)` `ULL`, `LPAD('5', 3, NULL)` `NU5`, `LEAST(3, 7, NULL)` 3. `%`는 `MOD`라서 **`WHERE v % 5 = 0`이 NULL 행을 골랐다**: RIGHT/FULL 조인이 짝 없는 쪽을 NULL로 채운 행이 `WHERE t.id % 5 = 0`을 통과했다(조인 질의의 답에 맞지 않는 행이 섞임).
- **함수 인자가 한정된 열의 식일 때**(검증 도구가 찾음): `ABS(a.x * a.y)`처럼 select 목록의 함수 인자가 `테이블.열`이 든 식이면 `resolve`가 `get_col`을 먼저 불러, 마지막 점 뒤(`y`)를 열 이름으로 읽고 **그 열의 값**을 돌려줬다(곱셈이 사라짐: `ROUND(p.v / p.w, 1)`이 `w`). 별칭 풀기(`expand_alias_str`)는 인자 텍스트에서 **첫 번째** 별칭만 풀어, 같은 이름의 열이 두 테이블에 있으면 `ROUND(y.g * y.g, 2)`가 `u.g * y.g`로 읽혔다.

**수정**:
- `eval_arith`: `+ - * /`는 한쪽이 NULL이면 NULL, 0으로 나누면 NULL(숫자가 아닌 글자의 `- * /`가 0인 것과 `+`가 이어 붙이는 것은 그대로), 비교(`Cmp`)도 NULL 피연산자면 NULL.
- `apply_scalar_func`: `null_propagating_functions()` 표(함수 이름 → 앞의 몇 개 인자가 "값"인지; `ROUND(v, d)`는 2, `DATE_ADD`는 날짜와 양만, `GREATEST`/`LEAST`는 전부) 약 70개 함수가 값 인자 중 하나가 NULL이면 NULL. NULL을 스스로 다루는 `COALESCE`·`IFNULL`·`NULLIF`·`ISNULL`·`IF`·`CONCAT`·`CONCAT_WS`·`CHAR`는 표에 없음. 검사한 인자는 `checked`에 담아 다시 풀지 않는다(인자가 식이면 풀 때마다 파싱).
- `resolve`: 연산자·괄호·공백이 든 인자는 열 이름으로 보지 않고 식으로 평가. `expand_alias_str`: 텍스트 안의 모든 `별칭.열`을 풀고(따옴표 안은 제외) 집계 호출 안의 인자 처리는 그대로.

**검증**:
- 신규 Catch2 5케이스(481 → 486): ① 결정적 케이스(`v + 1`·`v - w`·`3 * d`·`9 / d`·NULL 리터럴·`x / 0`, 비교, WHERE·CASE, 집계 식, `%`와 RIGHT JOIN으로 채운 행, UPDATE 세 가지, 문자 값 산술이 그대로임), ② **표 기반 함수 테스트** — 표의 모든 함수를 유효한 인자로 부르면 전과 같은 값이고(값은 이전 빌드의 출력으로 확인) 값 인자 자리마다 NULL을 넣으면 NULL, 열로도(`LENGTH(s)`, `DATE_ADD(dt, INTERVAL 5 DAY)`, `CAST(n AS INT)`), 식 인자(`ROUND(n / 3, 2)`, `GREATEST(n + 1, n + 1)`)와 NULL을 스스로 다루는 함수는 그대로, ③ 한정된 열의 식 인자(단일 테이블·조인·별칭·같은 이름의 열이 두 테이블에 있는 경우), ④ **무작위 식 테스트**: NULL이 든 `a`·`b`·`c`와 0 포함 리터럴로 `x op y`, 우선순위 `x op y op z`, 괄호 두 모양, `ABS(...)`, `ROUND(..., 2)`, 비교를 만들어 SELECT(절반은 `n.a`처럼 한정)·WHERE·UPDATE(VARCHAR 열에 저장 후 읽기)를 테스트 안에서 계산한 기준값과 비교(연산마다 엔진이 쓰는 6자리 반올림을 같이 적용). 파서 테스트 1개(인자 속 모든 별칭).
- 심은 버그 16종을 **모두** 새 테스트가 잡음(`+`·`-`·`*`·`/`의 NULL 검사 각각, 0으로 나누기, 비교, 함수 표 검사 끄기·첫 인자만 보기·`ROUND`/`CAST`/`GREATEST`·`LEAST`/`LOCATE`/`MOD` 항목 빼기, 검사한 인자의 캐시가 다른 인자 값을 돌려줌, 식 인자를 열로 읽음, 첫 번째 별칭만 풀기). 심은 버그를 되돌린 소스는 md5로 확인했다.
- 새 도구 `code/test/diff/verify_null_expressions.py`: 단일 테이블과 INNER/LEFT/RIGHT/FULL OUTER 조인(별칭 유무, 짝 없는 쪽은 NULL)에서 `val + 1`·`val * price`·`grp / val`·`(x - y) * z`·`ABS(x - y)`·`ROUND(x / y, 2)`·`x > y`를 만들고, **같은 FROM/WHERE의 평범한 열을 읽어** 식을 다시 계산해 SELECT 답의 모든 행, WHERE 필터, UPDATE로 저장한 값이 같은지 확인. **이전 빌드는 위반**(`ABS(x.id * x.price)`가 109.84가 아니라 54.92), 도구가 찾은 별칭 버그(`ROUND(y.grp * y.grp, 2)` 18, 정답 36)를 고친 뒤 새 빌드는 5시드 × 1,500질의(7,500문장, NULL 답 324,469행, WHERE 3,260, UPDATE 421) 위반 0. `verify_agg_expressions.py` 3시드(4,366문장·31,519그룹)·`verify_aggregates.py` 3시드(4,090문장·30,230그룹, HAVING 876)·`verify_orderby_distinct.py` 2시드 위반 0.
- 빌드 간 차분(`diff_builds.py`에 `--no-null-arithmetic` 추가: 말뭉치의 `SET val = val + 1`이 NULL 행을 건너뛰어 이전 빌드가 `NULL1`을 저장해 두 표가 달라지는 것을 막음): 이전 빌드 대비 6시드 × 1,500질의(9,000질의)에서 차이 153건이고 **전부** 두 모양: ① 선택 목록의 `val * 2` over NULL 행(0 → NULL) 141건, ② RIGHT/FULL OUTER 조인의 `WHERE t.id % k = r`에서 짝 없이 채워진 행이 더 이상 `= 0`을 통과하지 않음 12건(예: `SELECT t.id, t.val, v.qty FROM t RIGHT JOIN v ON … WHERE t.id % 5 = 0 OR …`의 이전 답에 `NULL | NULL | 3` 행이 섞여 있었음). 설명되지 않는 차이 0.
- Release/Debug **486 케이스/1,413,676 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,946,487), `[aggregate]` 30시드·`[null_expr]` 40시드(233,817 assertions) 긴 캠페인, 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 16,732) 불일치 0.
- 성능(10만 행, 이전/새 빌드를 번갈아 두 번씩): `v * 2 + w` 107·104 → 105·102ms, `v / w` 159·158 → 162·152, `ROUND(v / 3, 2)` 290·304 → 284·278(검사한 인자를 다시 풀지 않아 오히려 약간 빠름), `ABS(v - w)` 194·196 → 189·176, `UPPER(s)` 99·109 → 105·100, `SUBSTR(s, 2, 3)` 138·144 → 150·144, `WHERE v * 2 > 100` 78·73 → 75·72 — 오차 범위. `bench_query.py 50000`(번갈아 두 번): `COUNT(*)` 18.8·21.6 → 17.1·18.9ms, `SUM`/`AVG` 27.0·24.3 → 24.3·24.9, `GROUP BY` 35.0·35.5 → 34.4·35.4, 조인+`GROUP BY` 297·292 → 296·283ms — 오차 범위(이 측정은 앞 항목보다 전체적으로 느린 상태의 PC에서 이전·새 빌드 모두 같게 나왔다).

**눈에 띄는 변화(의도한 것)**: NULL이 든 식은 NULL로 나온다(`v + 1`, `v * 2`, `ROUND(v)`, `LENGTH(s)`, `v > 5`); `x / 0`은 NULL; `WHERE v % 5 = 0`이 NULL 행을 고르지 않는다; 한정된 열의 식을 함수 인자로 쓸 수 있다. 문자 값 산술(`'x' + 1` → `x1`)은 바뀌지 않았다.

**이 점검에서 새로 찾았지만 고치지 않은 것(→ 같은 날 여섯 번째 항목에서 ③ 수정, 나머지는 이어서 수정 예정)**: 데모·실사용에 걸릴 가능성이 큰 순서로 ① **집계 인자에 식을 쓸 수 없음**(`SUM(price * qty)`, `SUM(COALESCE(x, 0))`: 파싱 오류), ② **함수 결과를 산술의 왼쪽에 쓸 수 없음**(`ROUND(x, 1) * 100`, `COALESCE(a, 0) + COALESCE(b, 0)`: 파싱 오류; `1 + ROUND(x)`은 됨), ③ **`UPDATE`가 NOT NULL을 검사하지 않음**(`UPDATE t SET not_null_col = NULL` 통과, INSERT는 막음; NULL 산술이 고쳐져 `SET x = x / y`도 NULL을 넣을 수 있게 됨), ④ `IF(c, 'n', …)`·`CASE … THEN 'n'`의 문자열이 열 이름과 같으면 그 열의 값이 나옴, ⑤ `SUM`/`AVG`가 빈 입력(또는 전부 NULL)에서 NULL이 아니라 0/0.0000, ⑥ `INSERT … VALUES (1 + 2)` 불가, ⑦ 문자 값 산술(`'12abc' + 1`), ⑧ 함수 열 이름 `ROUND()`, ⑨ 같은 `AVG(x)`를 select하고 식에도 쓰면 식은 반올림 값을 읽음.

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

### 10월 5일 (여섯 번째) — 쓰기 경로의 정합성: 빈 문자열이 NULL로 저장되던 것, 타입 검사 부재, UPDATE의 제약 미검사, 실패한 REPLACE가 행을 지우던 것, ON DUPLICATE KEY UPDATE, 3값 논리, NULL 정렬 위치, AUTO_INCREMENT

**왜 이 항목인가**: 사용자가 "정확해야 하는 소프트웨어에서 버그는 치명적"이라며 열려 있던 문제를 **전부** 고치라고 했다. 데이터를 잃거나 잘못 저장하는 쓰기 경로를 가장 먼저 잡았다(순서: 쓰기 정합성 → 집계·문자 산술 의미 → 집계 인자의 식 → 식 문법). 이전 항목들의 점검에서 나온 "고치지 않은 것" 목록(UPDATE의 NOT NULL 미검사 외)에 더해, 같은 경로를 `probe_many.py`로 더 파헤쳐 심각한 것들을 찾았다. 모두 원본 Rust 포팅 때부터 있던 문제다.

**원인과 영향**:
- **빈 문자열이 NULL로 저장됨**: `INSERT … VALUES (1, '')`가 `final_values[i].empty() ? NULL`로 NULL이 됐다(열을 생략한 경우와 명시한 `''`를 구별하지 못함). `IS NULL`·`COALESCE`·`IFNULL`·`ISNULL`·UNIQUE 검사가 `''`를 NULL로 취급했고 `WHERE s = ''`는 아무 행도 못 찾았다.
- **타입 검사가 전혀 없음**: `INT`에 `'abc'`, `VARCHAR(3)`에 `'toolong'`, `DATE`에 `'not a date'`·`'2024-02-30'`, `DECIMAL(5,2)`에 `123456.789`, `BOOLEAN`에 `'maybe'`가 그대로 저장됐다(MySQL 엄격 모드는 오류 1366/1406/1292/1264).
- **UPDATE가 제약을 검사하지 않음**: NOT NULL(PK·AUTO_INCREMENT 포함)과 자식 쪽 외래 키(`UPDATE ch SET p_id = 99`가 통과)와 타입을 검사하지 않았다. 부모 키를 바꾸는 UPDATE의 ON UPDATE RESTRICT는 **행을 이미 바꾼 뒤** 검사해 오류를 내도 행이 바뀌어 있었다.
- **`REPLACE INTO`가 실패해도 옛 행을 이미 지움**: 충돌 행 삭제가 새 행의 검증보다 먼저라, NOT NULL 위반 같은 오류로 REPLACE가 실패하면 **옛 행만 사라졌다**(데이터 유실). 같은 REPLACE 안의 중복 행도 처리가 맞지 않았다.
- **`INSERT … ON DUPLICATE KEY UPDATE`가 행을 제자리에서 바꿈**: MVCC 버전도 되돌리기(undo) 기록도 남기지 않아 트랜잭션 안에서 ROLLBACK해도 되돌아오지 않았고, NOT NULL·UNIQUE·CHECK·ENUM·FK 검사가 전혀 없었으며, 메시지는 갱신된 행이 있어도 "0 row(s) inserted"였다.
- **MERGE와 다중 테이블 UPDATE/DELETE**: 위와 같은 검증(NOT NULL·UNIQUE·FK·RESTRICT)이 문장 단위로 되지 않았고, 다중 테이블 UPDATE는 행을 제자리에서 바꿔 ROLLBACK이 되돌리지 못했다.
- **3값 논리가 없음**: NULL과의 비교·`NOT`·`IN`·`NOT IN`·`NOT LIKE`가 UNKNOWN이 아니라 TRUE/FALSE로 나왔다(`x NOT IN (1, NULL)`이 TRUE, 행이 없거나 NULL인 스칼라 서브쿼리는 글자 "NULL"을 0으로 비교). CHECK는 UNKNOWN을 거부했다(MySQL은 통과).
- **NULL이 정렬에서 글자 "NULL"로 비교됨**: `ORDER BY`·윈도우·집합 연산에서 NULL의 위치가 값의 모양에 달려 있었다(MySQL: ASC에서 맨 앞, DESC에서 맨 뒤).
- **AUTO_INCREMENT 카운터가 직접 넣은 번호를 모름**: 명시한 id 뒤에 자동 번호가 겹쳐 충돌했고(MERGE 점검 중 발견), `0`/NULL로 번호를 생성하지 않았다. UNIQUE 열은 한 문장 안에서든 기존 행과든 NULL끼리 중복으로 취급됐다(MySQL은 NULL 여럿 허용). 외래 키 검사는 삭제된 부모 행도 부모로 인정했고, 자기 참조 다중 행 INSERT는 같은 문장의 앞 행을 부모로 인정하지 않았다.

**수정**:
- 새 sentinel `INSERT_DEFAULT`(`"__INSERT_DEFAULT__"`, `parser.hpp`): 생략한 값(`(1, , 3)`, `DEFAULT`, 열 목록에 없는 열)만 기본값을 받고 명시한 `''`는 진짜 값. `IS NULL`·`COALESCE`·`IFNULL`·`ISNULL`·UNIQUE 건너뛰기는 글자 `NULL`만 NULL로 본다(`executor_update_unique.cpp`·`executor_ddl.cpp`·`executor_maint.cpp`·윈도우 `MIN/MAX/COUNT`도 `''`를 건너뛰지 않음).
- 새 `executor_types.cpp`의 `coerce_column_value`: INT 계열(범위, 반올림은 0에서 먼 쪽, `true`/`false`), FLOAT/DOUBLE(왕복 가능한 가장 짧은 표기), DECIMAL(p,s)(자릿수 문자열로 정확히 반올림하고 소수부를 채움), VARCHAR(n)(글자 수, 뒤쪽 공백은 잘림), DATE/DATETIME/TIMESTAMP/TIME/YEAR(윤년 포함 검증 후 표준 표기), BOOLEAN(TINYINT), JSON(`nlohmann::json::accept`). 메시지는 MySQL 엄격 모드 형식(`at row N`).
- 새 `executor_row_check.cpp`: UPDATE·ODKU·MERGE·다중 테이블 UPDATE가 같이 쓰는 `rewritten_row_violation`(NOT NULL, 자식 쪽 FK — 바뀌지 않은 FK 값은 건너뜀), `update_restrict_violation`(**바꾸기 전에** ON UPDATE RESTRICT 검사), `delete_restrict_violation`, 외래 키 이웃 테이블까지 정렬 순서로 잡는 `acquire_table_data_locks_mixed`. 삭제된 부모 행은 부모가 아니다.
- `REPLACE`: 전체 행 목록을 `exec_insert_inner(…, validate_only=true)`로 **먼저 검증**(기존 행과의 충돌과 문장 안 중복은 허용)하고, 피해 행들의 ON DELETE RESTRICT와 BEFORE INSERT 트리거를 확인한 뒤에야 삭제하고 넣는다(`replace_supersede`: 뒤 행이 대체하는 앞 행은 건너뛰되 그 앞 행의 충돌 행은 여전히 지움). 실패하면 아무것도 바뀌지 않는다.
- `ON DUPLICATE KEY UPDATE`: 충돌 행을 PK(복합 PK 포함)로 찾아 **UPDATE 문장 큐**로 실행(`VALUES(col)`은 넣으려던 값으로 묶음; 파서가 `VALUES(col)` 지원). MVCC·undo·모든 제약 검사를 UPDATE와 공유하고 메시지는 "N row(s) inserted, M row(s) updated."
- MERGE·다중 테이블 UPDATE/DELETE: 갱신은 `exec_update(PerRowValues)`(행 키 → 열 → 값; 한 번의 원자적 단일 테이블 UPDATE 문장), 삭제는 `exec_delete`, 삽입은 `exec_insert`로 재작성. MERGE의 잠금 집합은 외래 키 이웃을 포함하고 트리거가 있으면 전역 잠금으로 물러선다.
- 3값 논리: `enum class Tri {False, True, Unknown}`, `eval_cond3`/`eval_single3`와 서브쿼리 버전이 NOT/AND/OR/IN/NOT IN/BETWEEN/LIKE를 3값으로 평가하고 `eval_condexpr`/`eval_single`은 `== True` 래퍼. CHECK는 False만 거부. 스칼라 서브쿼리(행 없음/NULL)와의 비교와 IN 목록·서브쿼리 안의 NULL은 UNKNOWN.
- NULL 정렬: `cmp_key`(SELECT·집합 연산·윈도우)와 `order_rows` 모두 NULL이 ASC에서 맨 앞, DESC에서 맨 뒤.
- AUTO_INCREMENT: `counter_of`가 보이는 최댓값에서 지연 초기화하고 직접 넣은 정수가 카운터를 올림(스키마도 저장), NULL·0·생략이 다음 번호를 생성. 검증 전용 실행은 음수 자리표시자 `-(행번호+1)`을 쓴다. UNIQUE의 NULL 건너뛰기(기존 행·문장 안 모두), 자기 참조 FK는 같은 문장의 앞 행을 부모로 허용.

**검증**:
- 신규 Catch2 11케이스(486 → 497, `test_write_integrity.cpp`): 빈 문자열, 타입 표(모든 타입의 허용·거부·표준 표기), UPDATE 제약(NOT NULL·양쪽 FK·RESTRICT, 실패한 UPDATE는 아무것도 바꾸지 않음), REPLACE(실패하면 변화 없음·충돌 행 전부 삭제·ROLLBACK), ODKU(VALUES(col)·복합 PK·ROLLBACK·제약), 3값 논리 두 케이스(비교·NOT·IN·BETWEEN·LIKE·CHECK), NULL 정렬, 다중 테이블 UPDATE/DELETE·MERGE(ROLLBACK 포함), AUTO_INCREMENT(재시작 포함), 그리고 **무작위 모델 테스트** — 테스트 안의 모델이 제약·원자적 문장·REPLACE·ODKU를 계산해 엔진과 행 단위로 비교(40시드 43,182 assertions). 기존 테스트 중 빈 문자열 기대값·NULL 정렬 기준 비교기(`ref_cmp`)·DECIMAL 표기에 의존하던 부분(`test_select_index.cpp`는 표기 변형을 VARCHAR 열로)을 새 의미에 맞게 고쳤다.
- 심은 버그 34종(`mutate.py`의 `wi_*`: 빈 문자열을 NULL로 보기, 생략한 값, UNIQUE의 NULL(기존 행·문장 안), INT 반올림, VARCHAR 길이, DECIMAL 채우기, 윤년, UPDATE의 NOT NULL·FK·RESTRICT 순서, 삭제된 부모, REPLACE의 검증·RESTRICT·대체·삭제 대상, ODKU의 `VALUES(col)`·복합 PK, 3값 논리(NOT·AND·OR를 SELECT용·UPDATE/ON용 두 평가기에 각각, IN, CHECK), NULL 정렬, AUTO_INCREMENT 카운터·0, 자기 참조 FK, MERGE의 검증·RESTRICT, 다중 테이블 UPDATE의 선택)을 **모두** 새 테스트가 잡음. 첫 실행에서 살아남은 5건이 테스트의 구멍을 알려 줘서 보강했다: ① 삭제된 부모 행(UPDATE로 키가 바뀌어 남은 옛 버전과 아직 커밋 안 된 DELETE)을 부모로 인정하는 것, ② SELECT의 WHERE는 서브쿼리를 아는 별도 평가기(`eval_cond3_with_subquery`)를 쓰므로 UPDATE/DELETE/조인 ON이 쓰는 `eval_cond3`의 NOT·AND·OR는 따로 검증해야 했다(두 평가기 모두 UPDATE·DELETE·ON 케이스로 확인), ③ 문장 안 UNIQUE NULL 검사의 조회 쪽 건너뛰기는 기록 쪽 건너뛰기와 겹쳐 있어 심은 버그가 실제로는 동작이 같았다(겹치는 줄을 지움), ④ MERGE의 삭제 분기가 외래 키로 참조되는 행을 지울 때 같은 MERGE의 다른 행 갱신까지 되돌려지는지 확인하는 케이스가 없었다, ⑤ 한 심은 버그의 되돌리기 텍스트가 소스에서 유일하지 않아 소스가 심어진 채로 남았다(그 뒤 심은 버그 5건을 다시 돌렸고, 소스 체크섬이 되돌려졌음을 확인).
- 새 도구 `code/test/diff/verify_writes.py`: 열 `t`(자식, FK·NOT NULL·UNIQUE·CHECK·타입)와 `c`의 쓰기(INSERT·REPLACE·ODKU·UPDATE·DELETE, `UPDATE … JOIN`, `DELETE … JOIN`, MERGE)를 모델로 따라 계산해 **매 문장의 성공/거부와 그 뒤 표 내용 전체**가 엔진과 같은지 확인. 5시드 × 1,500문장(7,500문장: 성공 2,674·거부 4,638) 위반 0. 이전 빌드는 위반(실패한 REPLACE가 행을 지움 등).
- 같은 변경이 닿는 기존 도구: `verify_null_expressions.py` 3시드(NULL 답 198,441행, WHERE 1,993, UPDATE 248)·`verify_agg_expressions.py` 2시드(2,920질의, 21,438그룹)·`verify_aggregates.py` 2시드(2,735질의, 20,308그룹, HAVING 572)·`verify_orderby_distinct.py` 2시드(DISTINCT 549·ORDER 241·정렬 키 103; NULL이 앞이라는 새 규칙으로 비교기를 고침) 위반 0.
- 빌드 간 차분(`diff_builds.py`에 `--no-empty-strings`(말뭉치가 `''` 대신 NULL을 씀: 이전 빌드는 `''`를 NULL로 저장했다)와 `--order-as-sets`(ORDER BY는 집합으로, ORDER BY … LIMIT은 비교 안 함: NULL 위치가 바뀌어 LIMIT 경계가 움직인다) 추가): 이전 빌드 대비 30시드 × 99질의(2,970질의; ORDER BY … LIMIT 561개는 비교 제외)에서 차이 113건이고 **전부 설명됨**: ① 윈도우 `OVER (… ORDER BY)` 36건(NULL이 먼저 정렬돼 순위·행 번호·LAG가 옮겨짐), ② 스칼라 서브쿼리 비교 6건(행 없음/NULL → UNKNOWN), ③ NOT 70건(`NOT IN`·`NOT LIKE`·`NOT (…)` over NULL은 UNKNOWN), ④ 1건: `ARRAY_AGG(val) … ORDER BY val DESC`의 NULL 원소 위치. 설명되지 않는 차이 0.
- Release/Debug **497 케이스/1,417,892 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,946,487), `[aggregate]` 긴 캠페인(11케이스 317,618 assertions), 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 16,744) 불일치 0.
- 성능(이전/새 빌드를 번갈아 두 번씩, 10만 행을 1,000행씩 INSERT): INT·INT 163~177k → 163~170k rows/s, INT·VARCHAR(20) 178k → 170~173k, INT·DECIMAL·DATE 154k → 143~146k, INT·DOUBLE·DATETIME 145~147k → 125~130k(타입 검사·표준 표기 변환 비용, 약 5~15% 느림; DOUBLE·DATETIME이 가장 큼). 5만 행 UPDATE는 0.46~0.60초로 같음. `bench_query.py 50000`(번갈아 두 번): `COUNT(*)` 15.2·14.6 → 14.6·15.1ms, `GROUP BY` 25.3·22.4 → 26.4·28.2, 조인+`GROUP BY` 229·221 → 221·224, 인덱스 없는 `WHERE` 3종 17~22 → 14~16ms(오차 범위 또는 약간 빠름).

**눈에 띄는 변화(의도한 것)**: `''`는 NULL이 아니라 값이다(`WHERE s = ''`가 찾고 `IS NULL`은 아님); 타입에 맞지 않는 값은 오류(`INSERT INTO t(n) VALUES ('abc')`, 범위를 벗어난 INT, 없는 날짜); DECIMAL은 열의 소수 자릿수로 반올림·채움(`1.5` → `1.50`), DATE 등은 표준 표기, VARCHAR(n)을 넘으면 오류; `UPDATE`가 NOT NULL·외래 키를 지킨다; 실패한 REPLACE/MERGE/다중 테이블 UPDATE는 아무것도 바꾸지 않는다; `NULL`과의 비교는 UNKNOWN이라 `NOT IN (…, NULL)`은 행을 고르지 않고 CHECK는 UNKNOWN을 통과; NULL은 `ORDER BY ASC`에서 맨 앞.

**정직한 한계(MySQL과 다르게 남긴 것)**: ① **정렬 규칙(collation)이 바이트 단위·대소문자 구분**이다(`'a' < 'B'`가 거짓). ② **비교와 정렬은 열 타입이 아니라 값의 모양**(숫자로 읽히면 숫자)으로 정한다: 문자열 열의 `'10'`과 `'9'`가 숫자로 비교된다. ③ 산술은 6자리 반올림이다(앞 항목과 같음). ④ MERGE는 단계별로 잠금 시간 초과가 나면 단계 사이가 완전히 원자적이지 않을 수 있다(각 단계는 원자적). ⑤ 여러 테이블을 지우는 대규모 DELETE는 IN 목록 크기에 비례(O(N·M)). ⑥ `COUNT(조건)`은 참인 행만 센다. ⑦ 파서가 만든 AST JSON에서 생략한 INSERT 값이 `__INSERT_DEFAULT__` 글자로 보인다.

**이 점검에서 새로 찾았지만 고치지 않고 다음 항목으로 넘긴 것(조인 이름)**: 3값 논리 테스트를 조인 ON으로 확인하다가 **같은 테이블을 두 번 쓰는 조인이 틀린 답을 낸다**는 것을 찾았다(이전 빌드에도 있음, 이번 변경과 무관). 파서가 별칭을 실제 테이블 이름으로 바꾸기 때문에 `FROM emp e JOIN emp m ON e.mgr = m.id`의 `e.x`와 `m.x`가 둘 다 `emp.x`가 되고, 조인이 `emp.x` 키를 오른쪽 행으로 덮어쓴다: `SELECT e.name, m.name`이 `ann | ann …`, `ON a.id < b.id`가 0행, `LEFT JOIN`이 전부 NULL. 또 `SELECT * FROM a JOIN b ON …`에서 오른쪽 테이블의 같은 이름 열(`id`)이 왼쪽 값으로 나오고, `SELECT a.*`와 쉼표 조인(`FROM a, b WHERE …`)은 파싱 오류, `NATURAL`/`USING`의 `*`가 공통 열을 두 번 보여 준다. 이어지는 항목에서 조인 이름 체계를 고친다. 그 뒤로 남은 묶음: "집계·문자 산술(SUM/AVG 빈 입력, 문자 값 산술, AVG 반올림)", "집계 인자의 식", "식 문법(함수로 시작하는 식, CASE의 문자열, ORDER BY/GROUP BY 식, INSERT VALUES의 식, 열 이름 `ROUND()`, `1e3` 리터럴)".

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어(실행기 안의 새 파일 2개는 같은 "실행기" 상자 안) 변경 없음.

### 10월 6일 — 조인의 이름: 테이블 별칭의 `AS`, 같은 테이블을 두 번 쓰는 조인, `*`·`t.*`, USING/NATURAL, 쉼표 조인, `JOIN (SELECT …)`, 바깥 조인의 NULL 채우기, 한 테이블 UPDATE의 `SET t.열`

**왜 이 항목인가**: 앞 항목에서 3값 논리를 조인의 `ON`으로 확인하다가 **같은 테이블을 두 번 쓰는 조인이 틀린 행을 낸다**는 것을 찾았다(`FROM emp e JOIN emp m ON e.mgr = m.id`가 `ann | ann …`). 이전 빌드에도 있던 문제이고, 조사해 보니 한 군데가 아니라 **조인이 테이블과 열의 이름을 다루는 방식 전체**가 문제였다. 사용자 결정("전부 고쳐")에 따라 한 번에 고쳤다. 특히 `AS`로 별칭을 쓰는 SQL(`FROM orders AS o`)이 파싱 오류였다는 점은 Claude(MCP)가 만드는 SQL이 거의 항상 그렇게 쓰기 때문에 가장 먼저 걸리는 문제였다.

**원인과 영향** (모두 원본 Rust 포팅 때부터):
- **`AS`가 없으면 별칭을 못 씀**: `FROM a AS x`, `JOIN b AS y`, `UPDATE t AS x`, `DELETE FROM t AS x`가 모두 파싱 오류(`parse_select`가 테이블 뒤의 `Ident`만 별칭으로 읽음).
- **같은 테이블을 두 번 쓰면 둘이 한 테이블이 됨**: 파서가 별칭을 **실제 테이블 이름으로 치환**해서 `e.x`와 `m.x`가 둘 다 `emp.x`가 되고, 조인이 그 키를 오른쪽 행으로 덮어쓴다. `SELECT e.name, m.name`이 `ann | ann`, `ON a.id < b.id`가 0행, `LEFT JOIN`이 전부 NULL, `a.x = b.x`가 중복 행. 직원–상사 조회 같은 흔한 질의가 전부 오답.
- **`SELECT *`가 조인에서 같은 이름의 열을 왼쪽 값으로 채움**: 별 확장이 모든 테이블의 열을 이름만으로 읽어서, `a(id, …)`와 `b(id, …)`의 `*`에서 b의 `id` 열에 a의 값이 나왔다(b.id가 2인데 1). `SELECT a.*`는 파싱 오류, `SELECT *, x + 1`은 `x + 1`을 **조용히 버림**, `SELECT DISTINCT *`는 행에 숨어 있는 `_xmin`까지 비교해서(INSERT가 다르면 값이 같아도 다른 행) 중복을 제거하지 못했다.
- **쉼표 조인(`FROM a, b WHERE …`)**과 **조인 안의 파생 테이블(`JOIN (SELECT …) AS d`)**은 파싱 오류(LATERAL만 됐음).
- **`USING`/`NATURAL`**: `USING`이 조인 종류를 무시하고 항상 INNER로 동작(`LEFT JOIN … USING`이 짝 없는 행을 버림), `NATURAL`은 NULL끼리도 같다고 보고, 둘 다 `*`에서 공통 열을 두 번 보여 줌.
- **RIGHT / FULL OUTER JOIN의 NULL 채우기**: 짝 없는 오른쪽 행의 왼쪽 열을 **첫 번째 왼쪽 행의 일반 키**만 보고 채워서, 앞서 조인된 테이블의 `테이블.열`과 왼쪽에 행이 하나도 없는 경우(빈 파생 테이블 등)는 채워지지 않고 오른쪽 값이 새어 들어갔다.
- **행이 없는 FROM 파생 테이블**은 열 이름을 몰라 곧바로 "0 rows returned."를 돌려줘서 `SELECT COUNT(*) FROM (SELECT … WHERE false) d`가 0 행이 아니라 빈 답이고, 바깥 조인의 짝 없는 쪽이 비었다.
- **한 테이블 UPDATE의 `SET 별칭.열 = …` / `SET 테이블.열 = …`**: "1 row(s) updated"를 돌려주는데 값이 바뀌지 않고, 행에 `t.v` 같은 **보이지 않는 키**가 생겼다(이후 `x.v + 1`이 그 키를 읽어 100이 나옴). 테이블에 없는 열(`SET zz = 5`)도 같은 방식으로 모든 행에 보이지 않는 열을 만들었다.

**수정**:
- 파서: `[AS] 별칭`(`parse_table_alias`)을 FROM·JOIN·UPDATE·DELETE에서 읽는다. FROM 목록의 이름(별칭 또는 테이블 이름)이 겹치면 `Not unique table/alias: 'x'`. **같은 테이블의 두 번째 사용에는 별칭이 필수**이고 그 별칭은 테이블 이름으로 치환하지 않고 유지하며 `Join.alias`에 기억한다(첫 번째 사용은 전처럼 테이블 이름으로 풀림). `FROM a, b`는 CROSS JOIN, 선택 목록의 `별칭.*`는 `SelectColumn::All{table}`, `JOIN (SELECT …) [AS] 별칭`은 LATERAL과 같은 `subquery` 칸(`lateral=false`)에 담는다. AST JSON은 `alias`가 없으면 빈 문자열(예전에 저장된 뷰·프로시저 그대로 읽힘), `*`는 그대로 `"All"`, `t.*`는 `{"AllOf": …}`.
- 실행기: 병합 행의 열 접두어는 `join_qualifier(j)`(별칭이 있으면 별칭, 없으면 테이블 이름). 별칭이 있는 조인은 계획기의 알고리즘을 쓰지 않고(계획기는 테이블 이름으로 추론해 두 사용을 한 테이블로 봄) ON 조건 — 해시 조인 포함 — 으로 조인한다. 새 `resolve_join_columns`가 `exec_select` 첫머리에서: ① NATURAL/USING을 평범한 ON(`공통열 = 오른쪽.공통열`)으로 바꾸고(조인 종류·해시 조인·NULL 규칙이 ON의 것이 됨, USING 열은 `*`에 한 번 앞쪽에), ② WHERE의 `a.k = b.k`로 짝지어지는 CROSS 조인(쉼표 조인)을 INNER 조인으로 바꿔 해시할 수 있게 하고, ③ `*`/`t.*`(조인에서, 다른 열과 함께, DISTINCT 아래)를 그것이 뜻하는 열로 펼친다(첫 테이블은 열 이름, 나머지는 `이름.열`; 알 수 없는 테이블은 `Unknown table 'z'`).
- 파생 테이블 조인은 FROM의 파생 테이블처럼 한 번 평가해 별칭 이름의 임시 테이블로 읽는다. **행이 없어도** 열 이름은 select 목록에서 얻는다(`derived_column_names`; FROM 파생 테이블과 뷰도 같이 고침).
- RIGHT / FULL OUTER JOIN은 짝 없는 오른쪽 행의 왼쪽을 **스키마에서 얻은 모든 키**(FROM 테이블의 열, 앞서 조인된 테이블의 `이름.열`)로 NULL 채운다(`left_pad`; 4개 조인 함수와 8곳의 채우기). `RIGHT/FULL … USING (c)`의 병합 열(`c`·`*`)은 짝 없는 오른쪽 행에서 오른쪽 값이다.
- 한 테이블 UPDATE: SET 대상의 한정자를 제거하고(`SET t.v = 1` → `v`), 테이블에 없는 열은 `Unknown column 'zz' in 'field list'` 오류.

**검증**:
- 신규 Catch2 9케이스(497 → 506, `test_join_names.cpp`): ① 별칭(AS 유무, SELECT·UPDATE·DELETE·UPDATE…JOIN, `Not unique table/alias`), ② 자기 조인(직원–상사: INNER/LEFT/RIGHT, 3단계, 비등호 ON, `COUNT`, `GROUP BY`, 첫 사용에 별칭 없음, 뷰, 사용 3번), ③ 별(같은 이름의 열 값, 열 이름 머리글, `a.*`/`b.*`/`b.k, a.*`, 별칭, 다른 열과 함께, `DISTINCT *`, 알 수 없는 테이블), ④ USING/NATURAL(INNER/LEFT/RIGHT/FULL, 병합 열 한 번·앞쪽, NULL 키는 매칭 안 함, 공통 열 없음은 곱, 알 수 없는 열), ⑤ 쉼표 조인(2·3개 테이블, 추가 조건, OR이 든 WHERE), ⑥ 파생 테이블(집계·`*`·둘·빈 결과·`COUNT`·별칭 충돌), ⑦ 바깥 조인 채우기(빈 왼쪽, 3단계), ⑧ JSON 왕복과 옛 JSON, ⑨ **무작위 모델 테스트** — 인스턴스 2~4개(같은 테이블 반복, 별칭 유무, `AS` 유무)·INNER/LEFT/RIGHT/FULL·여러 부분의 ON(등호·비등호·상수)·WHERE·`*`/`t.*`/열을 테스트 안의 중첩 루프와 3값 논리로 계산한 답과 행 집합·열 이름까지 비교.
- 새 도구 `code/test/diff/verify_joins.py`: 같은 이름의 열을 가진 3개 테이블과 위 모양의 질의(+ NATURAL/USING, 쉼표 조인, `(SELECT * FROM t WHERE …) AS d`가 FROM과 JOIN에, `COUNT(*)`)를 만들어 모델과 행 집합·머리글을 비교. 이전 빌드는 곧바로 위반(예: 별칭 자기 조인), 새 빌드는 **8시드 × 600문장(4,800문장; 답 85,693행, 자기 조인 2,906·별 1,536·파생 테이블 1,525·쉼표 조인 330·USING/NATURAL 501·바깥 조인 2,594·빈 답 1,262)과 30행 테이블 3시드 × 250문장(750문장, 답 918,306행)에서 위반 0**.
- 심은 버그 19종(`jn_*`: 별칭을 테이블 이름으로 치환, 두 번째 사용에 별칭 불필요, 별칭 무시, 별이 모든 열을 이름으로 읽음, USING의 첫 열만 비교, NATURAL의 공통 열 하나만, 쉼표 조인의 짝짓기 조건 완화, 채우기 키를 첫 행에서, 병합 열의 NULL 유지, FROM 테이블의 한정 키, 파생 테이블 열 이름, 계획기 알고리즘, DISTINCT/다른 열과의 `*` 전개 끄기, 병합 열 규칙, 병합 열 앞쪽 표시, 쉼표 조인을 INNER로) 중 15종을 새 테스트가 잡았다. 나머지 4종은 결과로는 구별되지 않는 것이다: 별칭 조인의 JOIN 순서 최적화 금지(두 방식의 답이 같음을 검증 도구 8시드로 확인하고 **금지를 없앴다**), FROM 테이블의 한정 키 복사(채움 키가 이미 같은 일을 해서 **복사를 없앴다**), 범위 이름 목록에 별칭을 쓰는 것(해시·선적용의 성능만 달라짐), 파생 테이블 별칭이 기존 테이블과 겹치는 검사(다른 세션이 같은 별칭의 임시 테이블을 쓰는 동안에만 일어남). 소스는 md5로 되돌려졌는지 확인했다.
- 빌드 간 차분(`diff_builds.py`, 이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의)에서 차이 44건이고 **전부** 조인의 `SELECT *`(오른쪽 테이블의 같은 이름 열이 이전엔 왼쪽 값)였다. 설명되지 않는 차이 0.
- Release/Debug **506 케이스/1,418,625 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,946,487), 쓰기 퍼저 40시드(43,182), 새 조인 퍼저 60시드(7,178), `[aggregate]` 긴 캠페인(11케이스 317,618), 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 11,468) 불일치 0. 앞 항목들의 검증 도구(`verify_writes` 3시드, `verify_null_expressions`·`verify_agg_expressions`·`verify_aggregates`·`verify_orderby_distinct` 각 2시드) 위반 0.
- 성능: 5만 행 `bench_query.py`와 별 조인을 이전/새 빌드로 번갈아 두 번씩: `t JOIN u`의 `*` 237·236 → 240·234ms, `t.*, u.name` 228·230ms(이전엔 파싱 오류), 열을 직접 쓴 조인 256·259 → 255·254, 조인 `COUNT(*)` 163·160 → 161·162, `LEFT JOIN … *` 38·37 → 39·40; 일반 질의도 같다(`COUNT(*)` 13.8·13.8 → 14.3·12.8, `GROUP BY` 21.5·20.9 → 20.7·17.7, 조인+`GROUP BY` 212·218 → 216·218, 인덱스 없는 `WHERE` 11~13ms) — 오차 범위.

**눈에 띄는 변화(의도한 것)**: `FROM a AS x`, `JOIN b AS y`, `FROM a, b WHERE …`, `JOIN (SELECT …) AS d`, `a.*`가 된다; 같은 테이블을 두 번 쓰는 조인(별칭 필수)이 맞는 답을 낸다; 조인의 `*`는 각 테이블의 자기 열을 보여 주고 `*, 열`은 둘 다, `SELECT DISTINCT *`는 값이 같은 행을 합친다; `LEFT/RIGHT/FULL JOIN … USING`이 바깥 조인이고 NATURAL은 NULL을 매칭하지 않는다; `UPDATE t SET t.v = 1`이 값을 바꾸고 없는 열은 오류; 행이 없는 파생 테이블도 열이 있어 `COUNT(*)`는 0.

**정직한 한계**: ① **모호한 열 이름**(`SELECT id FROM a JOIN b …`)은 오류(MySQL 1052) 없이 첫 번째 테이블의 값을 읽는다. ② **존재하지 않는 열은 SELECT·WHERE·ORDER BY에서 여전히 오류 없이 빈 값/0행**이다(`SELECT nosuch FROM t`, `WHERE nosuch > 1`, `ORDER BY nosuch`; 이번에는 UPDATE의 SET만 고침) — 오타가 조용히 빈 답이 되는 문제라 다음 항목으로 한다. ③ 두 번째 이후의 조인에 쓴 `USING`은 앞 테이블 중 그 열을 가진 첫 테이블의 열과 비교한다(MySQL은 병합된 열). ④ 같은 별칭의 파생 테이블을 동시에 쓰는 두 세션은 임시 테이블 이름이 겹친다(FROM의 파생 테이블과 같은 구조적 한계; 이름이 겹치면 오류). ⑤ 같은 테이블의 두 번째 사용에 별칭이 없는 `FROM emp e JOIN emp`는 MySQL과 달리 오류. ⑥ 자기 조인의 두 사용을 한 번에 다루는 계획기 알고리즘(IndexNL 등)은 쓰지 않는다(해시 조인은 쓴다). ⑦ 다중 테이블 `UPDATE`/`DELETE`의 자기 조인은 되지 않는다. ⑧ 별칭 없는 식 열의 머리글은 별칭이 테이블 이름으로 치환된 `u.v+1`로 나온다(식을 쓴 그대로 보이게 하는 것은 식 문법 항목에서).

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어(실행기 안의 단계 하나가 늘었을 뿐) 변경 없음.

### 10월 6일 (두 번째) — 존재하지 않는 열 이름이 오류가 아니라 빈 값이던 것(`SELECT nosuch`, `WHERE nosuch > 1`, `ORDER BY nosuch` …), 그리고 이 점검에서 찾은 앞 항목의 회귀(`WHERE flag = TRUE`)

**왜 이 항목인가**: 앞 항목(조인 이름)의 `UPDATE … SET zz = 5`가 "1 row(s) updated"인데 행에 보이지 않는 열을 만들던 것을 고치다가, **어느 문장에서든 없는 열 이름이 조용히 받아들여진다**는 것을 확인했다. 오타가 "데이터 없음"으로 보이는 문제라 정확성이 중요한 이 프로젝트에서 사용자가 고치기로 한 것에 포함되고, Claude(MCP)가 만든 SQL의 철자 오류를 알려 주는 유일한 신호이기도 하다.

**원인과 영향** (원본 Rust 포팅 때부터): 열을 읽는 곳은 값이 없으면 빈 값을 돌려주고 조건은 거짓으로 평가한다. `SELECT nosuch FROM t`는 **열 하나가 비어서** 나오고(NULL도 아님), `WHERE nosuch > 1`은 0행, `UPDATE/DELETE … WHERE nosuch = 1`은 "0 row(s)", `ORDER BY nosuch`는 순서가 아무렇게나 바뀌고, `GROUP BY nosuch`는 머리글만 나오고, `COUNT(nosuch)`는 0. MySQL은 모두 오류 1054 `Unknown column 'nosuch' in 'where clause'`.

**수정**: 새 `executor_bind.cpp`(`Executor::check_columns`)가 **최상위 문장마다 실행 전에 한 번** 열 이름을 정적으로 확인한다(`execute_with_s`의 첫 호출에서만; 파생 테이블·서브쿼리·뷰·트리거·프로시저 본문이 도는 중첩 실행은 시작한 문장의 일부로 이미 확인됐거나 바깥 행의 값이 들어가 있어서 확인하지 않고, 확인 비용은 문장당 마이크로초 단위):
- 대상: SELECT의 select 목록(함수 호출 인자는 제외)·ON·WHERE·GROUP BY·HAVING·ORDER BY, UPDATE/DELETE의 WHERE와 SET 식(다중 테이블 UPDATE의 SET 대상도), 그 안의 서브쿼리와 UNION/CTE/`INSERT … SELECT`/`CREATE VIEW`의 SELECT. 보는 이름은 `이름`과 `테이블.이름`뿐이다(함수 호출·JSON 경로·`@변수`·`*`·숫자는 열이 아님).
- 범위: FROM의 테이블들(카탈로그 스키마; 파생 테이블은 select 목록의 이름; 뷰·CTE·information_schema·알 수 없는 테이블은 아무 이름이나 받음). 서브쿼리는 바깥 쿼리의 테이블도 본다. `GROUP BY`/`HAVING`/`ORDER BY`(와 WHERE)는 select 목록이 붙인 이름(별칭)도 받는다. 어느 테이블도 아닌 한정자(`z.id`)는 오류지만 서브쿼리 안(바깥 쿼리의 별칭일 수 있음, 파서가 서브쿼리 안의 바깥 별칭은 풀지 않음)과 UPDATE/DELETE(자기 별칭을 SET 식에 둠)에서는 받는다. 비교의 오른쪽은 파서가 따옴표 없이 보관해서 글자와 구별되지 않으므로 **`테이블.열`이고 그 테이블이 이 쿼리에 있을 때만** 열로 본다(`ON a.x = b.nosuch`는 오류, `name = 'e.g'`는 글자).
- 메시지는 MySQL과 같다: `Unknown column 'x' in 'field list'` / `'where clause'` / `'on clause'` / `'group statement'` / `'having clause'` / `'order clause'`.
- 이 점검에서 **앞 항목(여섯 번째)이 만든 회귀**를 찾아 같이 고쳤다: BOOLEAN이 1/0으로 저장되게 되면서(타입 검증) `WHERE flag = TRUE`가 아무것도 찾지 못했다(`TRUE`가 글자 "true"로 비교됨; 이전 빌드는 "true"/"false"를 저장해서 우연히 맞았다). 파서가 `TRUE`/`FALSE`를 숫자 1/0으로 만든다(`flag = TRUE`, `SET flag = TRUE`, `SELECT TRUE` → 1, MySQL과 같음). 이 회귀는 커밋 b9c9653과 ffcf3c7에 있었다.

**검증**:
- 신규 Catch2 6케이스(506 → 512, `test_unknown_columns.cpp`): select 목록·절별·UPDATE/DELETE·서브쿼리(바깥 열 읽기: 맨이름·한정·바깥 별칭·식 안, `CREATE VIEW`가 만들 때 거절)·받아 주는 것들(따옴표 글자, 뷰·CTE, 파생 테이블의 열, 별칭을 쓴 ORDER BY/HAVING, 함수 인자)·트리거 — 메시지까지 정확히 비교하고, 실패한 문장이 아무것도 바꾸지 않았는지(UPDATE) 확인. BOOLEAN 비교는 `test_write_integrity.cpp`의 타입 케이스에 추가(`= TRUE`, `= FALSE`, `<> TRUE`, 갱신 뒤 `COUNT`, `SELECT TRUE, FALSE`).
- 심은 버그 19종(`uc_*`: 절마다 확인 끄기, 한정자 허용, 별칭 모름, 파생 테이블이 아무 열이나 받음, 서브쿼리가 바깥을 못 봄, 오른쪽 `테이블.열` 안 봄, 서브쿼리·UNION·`INSERT … SELECT`·`CREATE VIEW` 안 봄, 다중 테이블 UPDATE의 SET 대상, 키워드 목록, 중첩 문장도 확인) 중 17종을 새 테스트가 잡았다. 나머지 둘: 키워드 목록(파서가 TRUE를 숫자로 만들면서 도달할 수 없게 돼 **코드를 지움**), 최상위에서만 확인하는 가드(결과가 아니라 서브쿼리 반복 실행의 비용만 달라짐). 소스는 md5로 되돌려졌는지 확인했다.
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의)에서 차이 14건이고 **전부** 말뭉치가 오류 사례로 일부러 넣은 `(SELECT nosuchcol FROM t)` 스칼라 서브쿼리(이전 빌드는 빈 답이나 0행, 새 빌드는 `Unknown column 'nosuchcol' in 'field list'`). 설명되지 않는 차이 0.
- Release/Debug **512 케이스/1,418,851 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,946,487), 쓰기 퍼저 40시드(43,182), 조인 퍼저 60시드(7,178), `[aggregate]` 긴 캠페인(11케이스 317,618), 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 16,773) 불일치 0. 검증 도구(`verify_joins` 8시드 × 600문장 + 30행 3시드, `verify_writes` 3시드, `verify_null_expressions`·`verify_agg_expressions`·`verify_aggregates`·`verify_orderby_distinct` 각 2시드) 위반 0.
- 성능(이전/새 빌드를 번갈아 두 번씩, 짧은 문장 5,000개): 키 `SELECT` 130·122 → 131·106µs, `GROUP BY … HAVING` 92·82 → 82·80, `UPDATE … WHERE id = N` 919·859 → 957·862, `INSERT` 804·738 → 766·764 — 오차 범위(확인이 문장당 마이크로초 단위).

**눈에 띄는 변화(의도한 것)**: 없는 열 이름은 오류(`SELECT nosuch`, `WHERE t.nosuch = 1`, `ORDER BY nosuch`, `JOIN … ON a.x = b.nosuch`, `UPDATE … SET v = nosuch`, `CREATE VIEW` 안의 SELECT); `flag = TRUE`가 다시 맞고 `TRUE`는 1.

**정직한 한계**: ① 함수의 인자(`UPPER(nosuch)`, `WHERE LENGTH(nosuch) > 3`)는 확인하지 않는다(단위·타입 이름과 열이 구별되지 않음; 인자를 식으로 파싱하는 식 문법 항목에서). ② 서브쿼리 안의 모르는 한정자는 바깥 별칭일 수 있어 받아 준다(`WHERE id IN (SELECT zz.id FROM u)`는 오류가 아님). ③ 프로시저·트리거 본문의 문장은 실행할 때 확인하지 않는다. ④ 비교의 오른쪽이 따옴표 없는 맨이름(`WHERE a = nosuch`)이면 열인지 글자인지 구별할 수 없어 확인하지 않는다. ⑤ 모호한 열 이름(`SELECT id FROM a JOIN b …`, MySQL 1052)은 아직 오류가 아니다.

**이 점검에서 새로 찾았지만 고치지 않은 것**: ① **변수가 문장 안에서 쓰이지 않음** — 저장 프로시저의 매개변수·`DECLARE` 변수는 `IF`/`WHILE` 조건과 FROM 없는 `SELECT`에서만 쓰이고, `UPDATE t SET … WHERE id = p_id`는 "0 row(s) updated"로 아무것도 안 하며(`CALL`이 오류 없이 끝남), 사용자 변수도 `SET @x = 15; SELECT … WHERE v > @x`가 0행(MySQL은 행을 돌려줌). ② 문법 틈: `CURRENT_DATE`/`CURRENT_TIMESTAMP`를 괄호 없이, `WHERE TRUE`·`WHERE flag`·`x IS TRUE`, `db.테이블.열` 세 부분 이름은 파싱 오류. 다음 항목들에서 고친다.

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어(실행기 안의 단계 하나가 늘었을 뿐) 변경 없음.

### 10월 6일 (세 번째) — 저장 프로시저·변수·트리거·함수가 문장 안에서 쓰이지 않던 것(`UPDATE … WHERE id = p_id`가 "0 row(s)", `WHERE v > @x`가 0행, 트리거의 `NEW.id` 없음) 수정

**왜 이 항목인가**: 앞 항목에서 프로시저에 `UPDATE … WHERE id = p_id`를 넣어 `CALL`했더니 **오류 없이 "0 row(s) updated"**였다. 조사해 보니 저장 프로시저·트리거·사용자 변수는 **제어문과 FROM 없는 `SELECT`에서만** 동작하고 일반 문장 안에서는 변수 이름이 열 이름으로 취급돼 있었다(원본 Rust 포팅 때부터). 사용자 결정("전부 고쳐")에 따라 이 기능들이 실제로 쓸 수 있게 만들었다.

**원인과 영향**:
- **변수가 문장 안에서 값이 아님**: 프로시저의 매개변수·`DECLARE` 변수(`proc_vars`)와 `@변수`(`user_vars`)는 `IF`/`WHILE`/`SET`의 조건 평가와 FROM 없는 SELECT의 한 행에서만 읽혔다. `UPDATE t SET v = pv WHERE id = pid`는 `pid`라는 열을 찾다가 못 찾아 0행, `INSERT … VALUES (pid, pv)`는 `Incorrect integer value: 'pid'`, `SELECT … WHERE v > lim`은 0행, `WHERE v > @x`는 0행(MySQL은 행을 돌려줌), `UPDATE t SET v = @x`는 "1 row(s) updated"인데 글자 `@x`가 값이 됐고, `INSERT … VALUES (@x, 1)`은 파싱 오류, `SELECT @x + id`는 NULL.
- **설정하지 않은 `@변수`**를 읽으면 NULL이 아니라 **변수 이름 글자**(`@r`)가 나왔다.
- **트리거는 문장당 한 번만, 행 값 없이** 돌았다: `FOR EACH ROW`인데 3행 INSERT도 한 번, 0행을 바꾼 UPDATE/DELETE도 한 번 돌았고, `NEW.id`/`OLD.id`는 토큰만 있고 파서가 쓰지 않아 쓸 수 없었다. 본체의 문장이 실패해도 **무시**됐다.
- **사용자 정의 함수** `CREATE FUNCTION f(x INT) RETURNS INT RETURN …`가 파싱 오류(매개변수에 타입을 못 씀; 타입 없는 옛 문법만 됨).
- **`CALL`**: 인자는 값(문자열은 따옴표까지)과 변수 이름뿐이고 식(`a + 10`)은 파싱 오류, `OUT`/`INOUT` 매개변수가 호출한 쪽의 `@변수`로 돌아오지 않았고, `SELECT … INTO 변수`와 `SET @x = (SELECT …)`가 없었다.

**수정**:
- 새 `executor_vars.cpp`(`substitute_variables`): 문장을 실행하기 전에 변수를 부르는 모든 곳을 그 값으로 바꾼다 — 조건의 양쪽(`WHERE id = p_id`, `IN (@x, 20)`, `BETWEEN`)·식(`SET v = pv + 1`)·`INSERT` 값·select 목록(`SELECT id, lim`)·함수 인자(`ROUND(@n / 2)`, 문자열은 따옴표로 써서)·`CALL` 인자·서브쿼리·UNION/CTE. 열과 이름이 같으면 변수가 이긴다(MySQL과 같음). 설정하지 않은 `@변수`는 NULL(`SELECT @never`도). 이름이 따옴표 없이 보관되는 곳(비교의 오른쪽, `INSERT` 값, 함수 인자)에서는 변수 이름과 같은 글자가 변수로 읽힌다. `execute_with_s`의 맨 앞에서 한 번(변수가 없으면 아무 일도 안 함). `@변수`를 읽는 SELECT는 글자가 같아도 답이 달라지므로 **결과 캐시를 쓰지 않는다**.
- 파서: `INSERT … VALUES (@x, NEW.id)`·`IN (@x, …)`·`BETWEEN @a AND @b`, 트리거의 `NEW.x`/`OLD.x`(소문자도), `CALL p(a + 10, @r, 'it''s')`(식 인자는 JSON으로 두었다가 호출할 때 계산), 타입 있는 함수 매개변수(`x INT`, `d DECIMAL(10, 2)`), `SELECT … INTO 변수[, 변수]`(새 문장 `SelectInto`; AST JSON 호환), `SET @x = (SELECT …)`, 프로시저의 `SET v = (SELECT …)`.
- `CALL`: 문자열 인자는 따옴표를 벗기고, `@변수` 인자는 값으로, 식은 계산해서 넘기며 `OUT` 매개변수는 NULL로 시작해서 끝나면 `@변수`에 값을 돌려준다(`INOUT`도). `SELECT … INTO`는 첫 행을 변수에 넣는다(행이 없으면 NULL).
- 새 `executor_trigger.cpp`(`fire_triggers`, `trigger_rows_for`): 트리거 본체를 **행마다** 한 번씩 돌린다 — INSERT는 넣은 행(`NEW`), UPDATE는 바뀔 행(`OLD`)과 바뀐 뒤의 행(`NEW`; 갱신 식을 행에 적용해 계산), DELETE는 지울 행(`OLD`). 행이 없으면 돌지 않는다. BEFORE는 쓰기 전에, AFTER는 쓴 뒤에 돈다. 본체의 문장이 실패하면 그 문장이 실패한다(`Trigger 'x' failed: …`; AFTER는 이미 쓴 뒤라 `… failed after the change was made: …`). **BEFORE INSERT 트리거는 `SET NEW.v = NEW.v * 2`로 넣는 값을 바꾸거나 INSERT가 안 쓴 열의 값을 정할 수 있다**(다른 트리거에서는 오류로 거절).

- **트리거 재귀 깊이 상한 32 → 16단**: 최종 회귀의 Debug 실행이 자기 참조 트리거 테스트에서 **스택 오버플로로 중단**됐다(그 뒤 200여 케이스는 실행조차 안 됨). 한 단계의 스택 사용량을 직접 재 보니 Release는 4.7KB(32단 = 150KB)지만 Debug는 32KB라 1MB 스택에 32단이 들어가지 않았다(원래도 한계 직전이었고 이번 변경으로 한 단계가 조금 커져 넘었다). Release 제품에는 문제가 없었으나 가장 불리한 구성에서도 안전하도록 16단으로 낮췄다(합법적인 트리거 체인은 2~3단). 이 상한 오류는 이제 최상위 문장의 오류로 돌아오므로 기존 테스트의 낡은 주석("트리거문의 실패를 무시")을 고치고 그 오류를 검증하게 했다.

**검증**:
- 신규 Catch2 8케이스(512 → 520, `test_stored_routines.cpp`): ① 프로시저 매개변수·변수가 UPDATE/INSERT/DELETE/SELECT에서 값(마지막 SELECT의 답에 매개변수가 들어감, DECLARE/SET, WHILE과 IF 안의 문장, 프로시저가 프로시저를 변수·식 인자로 호출, 문자열 인자와 `''`), ② OUT/INOUT·SELECT INTO·설정 안 한 변수, ③ `@변수`(WHERE·SET·VALUES·식·IN·BETWEEN·NULL, SET 뒤 같은 글자의 SELECT가 새 답, 서브쿼리로 설정), ④ 함수(타입 있는/없는 매개변수, WHERE·UPDATE·select 목록), ⑤ 트리거(AFTER/BEFORE × INSERT/UPDATE/DELETE, 여러 행, 소문자, 0행, 같은 이벤트의 두 트리거, 지난 UPDATE가 남긴 옛 버전은 행이 아님), ⑥ 트리거 실패(BEFORE는 아무것도 안 씀, AFTER 메시지, 트랜잭션 안에서 ROLLBACK, 자기 자신을 부르는 트리거는 깊이 제한), ⑦ `SET NEW.x`(바꾼 값·안 쓴 열·다른 트리거는 거절), ⑧ 재시작 뒤에도 프로시저·트리거(JSON 저장)가 그대로 동작.
- 심은 버그 28종(`rv_*`/`tr_*`: 조건·식·VALUES·select 목록·SET의 치환 끄기, 캐시, OUT 값 반환·NULL 시작, 식 인자·따옴표 벗기기, 설정 안 한 변수, INTO(사용자·프로시저 변수), 트리거의 행마다·오류 무시·AFTER 메시지·UPDATE의 NEW·SET NEW·안 쓴 열, 지난 버전, 함수 매개변수 타입, IN 목록·CALL의 `@`, 함수 인자 속 변수·따옴표, `@`로 시작하는 식 인자, SET의 서브쿼리) 중 27종을 새 테스트가 잡았고, 처음에 살아남은 3종이 테스트의 구멍을 알려 줘 보강했다(FROM 있는 select 목록의 변수 이름, 변수가 하나도 없는 세션의 `SELECT @never`, 함수 인자 속 변수가 테이블 위에서도 치환되는지). 나머지 1종(트리거의 `new.x` 대문자 변환)은 파서가 이미 대문자로 써서 도달할 수 없어 **코드를 지웠다**.
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) 차이 0. 검증 도구·퍼저는 아래.
- 성능: 앞 항목 빌드와 번갈아 3라운드(문장 8,000개 × 3회 중 최솟값) — 키 SELECT 124→127µs(+2%), HAVING 집계 94→95µs(+1%), 키 UPDATE 950→947µs, INSERT 816→819µs로 측정 잡음 안(문장마다 변수·트리거 확인이 추가되지만 둘 다 없으면 곧바로 반환). 50,000행 질의 벤치(`bench_query.py`)도 잡음 안.
- Release/Debug **520 케이스/1,419,112 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,946,487), 쓰기 퍼저 40시드(43,182), 조인 퍼저 60시드(7,178), `[aggregate]` 긴 캠페인(11케이스 317,618), 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 15,930) 불일치 0. 검증 도구(`verify_joins` 8시드 × 600문장 + 30행 3시드, `verify_writes` 3시드, `verify_null_expressions`·`verify_agg_expressions`·`verify_aggregates`·`verify_orderby_distinct` 각 2시드) 위반 0.

**눈에 띄는 변화(의도한 것)**: 프로시저·`@변수`가 문장 안에서 값이다(`CALL p(1)` 안의 `UPDATE … WHERE id = pid`가 갱신, `WHERE v > @x`가 행을 찾음); `CALL p(@r)`의 OUT 값이 `@r`로 돌아오고 `SELECT … INTO`가 된다; 함수 매개변수에 타입을 쓸 수 있다; **트리거가 행마다 돌고 `NEW.x`/`OLD.x`를 읽으며 본체의 오류가 문장의 오류**다(전에는 문장당 한 번, 오류 무시).

**정직한 한계**: ① **AFTER 트리거가 실패하면** 오류를 돌려주지만 그 문장이 쓴 행(과 앞선 트리거의 부작용)은 남는다(메시지에 그렇게 적음; 명시적 트랜잭션 안에서는 ROLLBACK으로 되돌림). BEFORE 트리거의 실패는 아무것도 쓰기 전이다. ② `SET NEW.x = …`는 BEFORE **INSERT** 트리거에서만(BEFORE UPDATE는 오류로 거절). ③ 이름이 따옴표 없이 보관되는 곳에서는 변수 이름과 같은 문자열이 변수로 읽힌다(`WHERE name = 'n'`인데 변수 `n`이 있으면 변수 값). ④ `LIMIT @n`, `@x := 식`, `@@시스템변수`는 안 됨. ⑤ `INSERT … VALUES (1 + 2)`처럼 VALUES의 식(`VALUES (@x + 1)`)은 아직(식 문법 항목). ⑥ 트리거의 행은 쓰기 전에 계산한 값이다(UPDATE의 NEW는 갱신 식을 적용한 값이고 타입 변환 전). ⑦ 커서·`DECLARE … HANDLER`는 없음.

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

### 10월 6일 (네 번째) — 값이 없는 집계가 NULL이 아니라 0이던 것, 텍스트 산술(`'x' + 1` = `x1`), 소수·큰 정수 합계의 오차, AVG의 두 얼굴, FROM 없는 SELECT가 빈 값, 그리고 이 점검에서 찾은 R6 회귀(`SUM(CASE …)`가 오류)

**왜 이 항목인가**: 사용자 결정("전부 고쳐")의 집계·산술 의미 항목. 프로브를 돌리다 **앞 항목(R6, 이미 푸시된 `f813213`)이 `SUM(CASE WHEN …)`·`COUNT(CASE WHEN …)`·`SUM(v > 1)` — 조건부 집계 전부를 `Unknown column '__case__'`로 깨뜨렸다**는 것도 찾았다(테스트에 `SUM(CASE`가 한 건도 없어 못 잡았다). 가장 먼저 고쳤다.

**원인과 영향**:
- **값이 없는 집계**: 빈 입력(행이 없음, 전부 NULL)에서 `SUM`·`AVG`·`STDDEV`·`VARIANCE`·`MEDIAN`·윈도 `SUM`이 `0`/`0.0000`, `GROUP_CONCAT`이 `''`, `JSON_AGG`가 `[]`였다(MySQL은 모두 NULL, `COUNT`만 0). 그래서 `v IN (SELECT SUM(v) … 빈 집합)`이 `v = 0`인 행을 맞혔고(NULL이어야 해서 아무 행도 안 맞아야 함), `SUM(v) + 1`이 `1`, `HAVING AVG(v) < 60`이 값 없는 그룹을 통과시켰다.
- **텍스트 산술**: `+`가 글자를 이어 붙였고(`'x' + 1` → `x1`, `s + v` → `x10`), `- * /`는 숫자가 아닌 글자를 통째로 0으로 읽었다(`'12abc' * 2` = 0, `s - 1` = 0 — MySQL은 앞의 숫자만 읽어 24, -1). `' 5' + 1`은 `51`. `MOD(7, 'x')`는 0(MySQL은 NULL), `7 % '4q'`는 0(3). `ABS`·`ROUND`·`CAST` 등 숫자 인자도 같은 규칙이 필요했다.
- **정확도**: 합계를 double로 더해 `SUM(0.10 × 10)`이 `0.9999999999999999`라 `HAVING SUM(x) = 1`이 거짓이었고, 2^53을 넘는 정수는 `n + 1`·`SUM(n)`·`MIN/MAX`가 조용히 틀렸다(`9007199254740993` → `…992`).
- **AVG의 두 얼굴**: select 목록은 4자리로 반올림해 보이고 `HAVING`/식은 전체 정밀도로 계산해, **같은 `HAVING AVG(v) = 1.6667`이 select 목록에 `AVG(v)`가 있느냐에 따라 결과가 달랐다**. MySQL은 반올림된 값(`AVG * 3` = 5.0001)으로 계속 계산한다. 4자리 반올림도 이진 반올림이라 정확한 중간값(43.15625)이 43.1562로 내려갔다(MySQL 43.1563).
- `MIN`/`MAX`가 숫자를 다시 찍어(`1.98` → `1.9800`, 큰 정수 부정확), `BIT_AND`의 빈 집합이 `-1`(부호 없는 64비트 18446744073709551615), `LOG(b, x)`의 인자 순서가 거꾸로(`LOG(2, 8)` = 0.333).
- **FROM 없는 SELECT가 빈 값**: `SELECT CASE WHEN 1 = 1 THEN 'a' END`, `SELECT IF(…)`, `SELECT (SELECT COUNT(*) FROM t)`, `SELECT COUNT(*)`가 전부 `''`(조용한 오답). 서브쿼리의 NULL과 오류도 빈 칸이 됐다.

**수정**:
- 새 헤더 `numeric_text.hpp`: MySQL식 텍스트→숫자(`text_to_number`: 앞 공백 건너뜀, 부호·소수·지수, 뒤는 무시, 숫자가 없으면 0), 정확한 int64 산술(오버플로는 double로), 정확한 10진 합계(`DecimalSum`: 소수 자리가 다른 값도 10^-자리 단위의 정수로 더함, 합이 int64를 넘거나 지수 표기면 double로), 정확한 몫을 4자리로 반올림하는 평균(0에서 먼 쪽으로), 큰 정수를 정확히 비교하는 `compare_numbers`.
- 집계(`executor_select.cpp`, `executor_window.cpp`): 값이 없으면 NULL(`SUM`·`AVG`·`STDDEV`·`VARIANCE`·`MEDIAN`·`GROUP_CONCAT`·`JSON_AGG`·`ARRAY_AGG`·`SUM(CASE…)`·윈도 `SUM`/`AVG`), 텍스트 값은 앞의 숫자로 합산, `SUM`은 정수·소수를 정확히 더하고(결과의 끝자리 0은 정리), **`AVG`는 정확한 몫을 4자리로 반올림한 값 하나**를 select 목록·`HAVING`·식이 같이 쓴다(윈도 `AVG`도 4자리), `MIN`/`MAX`는 저장된 값 그대로(큰 정수는 정확히 비교), `BIT_AND`/`BIT_OR`는 부호 없는 64비트. 숫자를 모으는 일은 문자열 복사 대신 포인터로 해서 **집계가 8~19% 빨라졌다**.
- `eval_arith`: `+`는 더하기만(정수는 정확히, 아니면 앞의 숫자로), `- * /`도 같은 규칙, 0으로 나누기는 NULL. 숫자 인자를 받는 스칼라 함수(`ROUND`·`ABS`·`CEIL`·`FLOOR`·`MOD`·`SQRT`·`POWER`·`LOG*`·`EXP`·`SIN`·`COS`·`TAN`·`SIGN`·`TRUNCATE`·`FORMAT`)와 `CAST(… AS INT/DECIMAL …)`가 같은 규칙으로 읽는다. `LOG(b, x)`의 순서를 바로잡았다.
- FROM 없는 SELECT(`_dual_`): `CASE`/`IF`, 스칼라 서브쿼리(없는 행·NULL은 NULL, 오류는 문장의 오류), 집계(`COUNT(*)` = 1)를 계산하고, 표가 필요한 열(`*`, 윈도 함수)은 빈 값 대신 오류.
- 바인더(R6 회귀): `SUM`/`COUNT` of CASE의 자리표시자 `__case__`를 열 이름으로 검사하지 않고 CASE의 조건들을 검사한다(`SUM(CASE WHEN nosuch > 1 …)`는 여전히 `Unknown column 'nosuch'`).

**검증**:
- 신규 Catch2 7케이스(520 → 527, `test_aggregate_semantics.cpp`): ① 값 없는 집계(14개 집계 × 행 없음/전부 NULL, 그룹별, 윈도 프레임, 식·`COALESCE`·`HAVING`·서브쿼리·`IN`, `BIT_AND`/`BIT_OR`, `JSON_AGG`), ② 텍스트 산술(26개 문자열 × 연산자 × 리터럴/열/두 텍스트를 **정규식으로 쓴 독립 참조**와 비교, `SUM`/`AVG` of 텍스트 열, 숫자 함수, `CAST`, `LOG`), ③ 정확한 정수·소수(`SUM(0.10 × 10)` = 1, `HAVING SUM(x) = 0.3`, 2^53 넘는 `SUM`/`+`/`-`/`*`/`MIN`/`MAX`, int64 넘는 것은 double, 소수 5자리 `SUM`의 `HAVING`), ④ AVG(양수·음수·같은 집계가 select 목록에 있을 때와 없을 때의 `HAVING`·32행 평균의 정확한 중간값 0.0313·윈도 `AVG`), ⑤ 무작위 모델(NULL·소수 3자리·텍스트 섞인 표의 `SUM`/`AVG`/`MIN`/`MAX`/`COUNT`를 정수 천분 단위 참조로 계산해 정확히 비교, 그룹별), ⑥ FROM 없는 SELECT, ⑦ `SUM(CASE …)`·`COUNT(CASE …)`·`SUM(v > 1)`(R6 회귀)와 존재하지 않는 열 오류. 기존 테스트 7곳의 기대값이 옛 의미(빈 SUM = 0, `'x' + 1` = `x1`, `AVG * 3` = 5, `LOG(8, 2)` = 3)를 박아 둔 것이어서 MySQL의 값으로 고쳤다.
- 심은 버그 46종(숫자 변환의 공백·지수·부호, 정수 `+ - *`의 오버플로와 정확 산술, 10진 합계·끝자리 정리, AVG의 반올림·동점·음수, 큰 정수 비교, 빈 입력의 `SUM`/`AVG`/`STDDEV`/`GROUP_CONCAT`/`JSON_AGG`/윈도, 텍스트 `+ - *`·0 나누기, `MOD`·`LOG`·`CAST`·`ABS`, `MIN`/`MAX`의 형식·비교, `SUM(DISTINCT)`/`AVG(DISTINCT)`, `BIT_AND`, HAVING이 읽는 SUM, FROM 없는 SELECT의 CASE·집계·서브쿼리·오류, 바인더) 중 45종을 새 테스트가 잡았고, 살아남은 1종(뺄셈 오버플로 검사 제거)의 구멍을 메워 잡게 했다.
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) 중 **187개가 달랐고 전부 의도한 변화**로 분류했다(값 없는 집계의 0 → NULL 101 · 숫자 표기가 저장된 값 그대로로 58 · NULL AVG의 `HAVING`이 그룹을 거름 17 · AVG 동점 반올림 5 · 정렬/LIMIT 경계가 NULL 때문에 바뀜·`ARRAY_AGG`의 빈 집합 6). 모든 차이 질의가 바뀐 집계(SUM/AVG/MIN/MAX/STDDEV/VARIANCE/GROUP_CONCAT/JSON_AGG/ARRAY_AGG/BIT_*)를 쓰는 것이었고 그 밖의 질의는 차이가 없었다. 분류를 위해 `diff_builds.py`에 `--diff-lines`(차이 줄을 3줄이 아니라 전부 나열) 옵션을 추가했다.
- Release/Debug **527 케이스/1,421,575 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과. SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,946,487), 쓰기 퍼저 40시드(43,182), 조인 퍼저 60시드(7,178), `[aggregate]` 긴 캠페인(11케이스 317,626), 새 무작위 모델 60시드(23,151), 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 16,080) 불일치 0. 검증 도구는 참조 모델을 MySQL 의미로 고쳤다(`verify_aggregates`·`verify_agg_expressions`: 값 없는 집계는 NULL, 식 속 NULL 전파 — 건너뛰는 그룹이 288 → 0이 되어 더 많이 검증한다) — 두 도구와 `verify_joins` 8시드 × 600문장 + 30행 3시드, `verify_writes` 3시드, `verify_null_expressions`·`verify_orderby_distinct` 각 2시드 위반 0.
- 성능(앞 항목 빌드와 번갈아 3라운드, 캐시를 피하려고 매번 다른 문장): 50,000행에서 `SUM`/`AVG` 71.8 → 63.9ms, 소수 열 67.3 → 54.6, `MIN`/`MAX` 70.8 → 64.2, `STDDEV` 64.4 → 55.6, `GROUP BY` + 4개 집계 91.4 → 79.1, `HAVING AVG` 72.3 → 66.1, `COUNT(*)` 44.9 → 45.1(최솟값 기준, 모두 같거나 빨라짐).

**눈에 띄는 변화(의도한 것)**: 값이 없는 `SUM`/`AVG`/`STDDEV`/`GROUP_CONCAT`/`JSON_AGG`가 NULL이고 그것으로 한 계산도 NULL(`COALESCE(SUM(v), 0)`이 0을 되돌려 줌); `'x' + 1` = 1, `'12abc' * 2` = 24; `AVG(v) * 3`이 5.0001(1, 2, 2); `MIN`/`MAX`가 저장된 값 그대로(`1.98`); `BIT_AND`의 빈 집합이 18446744073709551615; `LOG(2, 8)` = 3; `SELECT CASE …`/`SELECT (SELECT …)`가 값을 낸다; `SUM(CASE …)`가 다시 된다.

**정직한 한계**: ① `/`의 몫은 최대 6자리(MySQL은 피연산자 소수 자리 + 4; `1 / 3`이 0.333333 대 0.3333); ② `AVG`는 DOUBLE/FLOAT 열에서도 4자리로 반올림한 값으로 식을 계산한다(MySQL은 double 전체 정밀도); ③ select 목록에 같은 `SUM(x)`가 있고 소수 5자리 이상이면 `HAVING SUM(x) = …`는 표시된 4자리 값과 비교한다; ④ int64를 넘는 합은 double이며 1e15 이상의 정수 합 표기가 지수일 수 있다; ⑤ `GROUP_CONCAT`의 순서·`DISTINCT`·`SEPARATOR`, `STDDEV_SAMP`/`VAR_SAMP`, 집계 인자의 식(`SUM(a * b)`)은 아직(R3). **이 점검에서 새로 찾아 따로 고칠 것**: **VARCHAR 열의 비교·정렬·인덱스가 열 타입이 아니라 값의 모양으로 정해진다**(숫자처럼 보이는 문자열과 영숫자 키가 섞인 VARCHAR 기본키에서 `WHERE code = '32'`가 존재하는 행을 못 찾음 — 344개 중 58개 조회 실패; `ORDER BY code`가 사전식이 아님; `code = '7'`이 `'007'`·`'7.0'`도 맞힘; 우변의 따옴표를 잃어 `WHERE name = 'city'`가 `city` 열과 비교됨; 2^53 넘는 정수 비교) → 타입 인식 비교 항목, **스칼라 서브쿼리가 여러 행·여러 열을 돌려줘도 오류 없이 첫 값**(MySQL 1242/1241) → 서브쿼리 항목, 대소문자 구분(MySQL 기본은 구분 안 함)은 바이트 비교로 두고 문서에만 적는다.

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

### 10월 6일 (다섯 번째) — 비교가 값의 모양이 아니라 열의 타입을 따르도록: VARCHAR 키 조회 누락, 정렬·MIN/MAX·조인, 따옴표를 잃은 문자열, 2^53 넘는 정수 — 그리고 이 점검에서 찾은 기본키 단축 경로의 엉뚱한 행 삭제

**왜 이 항목인가**: 사용자 결정("전부 고쳐")의 타입 인식 비교 항목(앞 항목 점검에서 찾은 ⑮). 새 기능이 아니라 **기존 비교·정렬·인덱스가 MySQL처럼 동작하도록 고친 것**이다: MySQL은 비교를 값의 모양이 아니라 **피연산자의 타입**으로 정한다 — 문자열끼리는 글자 비교(`'10' < '9'`, `'007' ≠ '7'`), 숫자와 다른 것은 숫자 비교(문자열은 앞의 숫자로 읽음, `'abc'` = 0).

**원인과 영향**:
- **VARCHAR 키가 인덱스에서 사라짐**: B+Tree 비교기가 "둘 다 숫자처럼 보이면 숫자, 아니면 글자"였는데 이것은 이행적이지 않다(9 < 10, 10 < `1a`, `1a` < 9). 숫자처럼 보이는 문자열과 영숫자 키가 섞인 VARCHAR 기본키·보조 인덱스에서 트리 탐색이 길을 잃어 **존재하는 키를 못 찾았다(344개 키 중 58개 조회 실패)**.
- 문자열 비교가 값의 모양을 따랐다: `code = '7'`이 `'007'`·`'7.0'`도 맞히고, `ORDER BY code`가 사전식이 아니며(`'9'` 앞에 `'10'`이 아니라 숫자 순), `MIN`/`MAX`·`DISTINCT`·`GROUP BY`·윈도 정렬·조인(`t.code = w.code`)도 같았다.
- **우변의 따옴표가 파서에서 사라졌다**: `WHERE name = 'city'`가 `city` **열**과 비교되고(`IN ('city')`·`BETWEEN`·변수 값도), 엔진이 REPLACE·ON DUPLICATE KEY UPDATE·MERGE·다중 테이블 DELETE를 위해 직접 만드는 조건도 키 값이 열 이름과 같으면 열 비교가 됐다.
- **2^53을 넘는 정수**가 비교·정렬·`MIN`/`MAX`·조인 해시 키에서 서로 같은 수로 취급됐다(double로 읽음).
- **(점검 중 새로 찾음) UPDATE·DELETE의 기본키 단축 경로가 조건을 다시 보지 않고 키 글자만 믿었다**: `DELETE FROM t WHERE a = b`(열끼리 비교)가 **`a = 'b'`인 행을 지웠고**(맞는 행은 `a`와 `b`가 같은 행), 텍스트 키에 숫자를 쓴 `WHERE code = 7`이 `'7'` 한 행만 처리하고(`'007'`·`'7.0'`도 7이다), 정수 키의 `BETWEEN 5.0 AND 9`가 5번 행을 빠뜨렸다(트리에서 `"5"`가 `"5.0"` 앞). 이 세 가지는 심은 버그가 살아남은 것을 따라가다 발견했다.

**수정**:
- 새 헤더 `value_class.hpp`: 값의 종류(`ValueClass`: 숫자/글자/모름)와 `compare_classed`(둘 다 글자면 바이트 비교, 한쪽이라도 숫자면 숫자 비교(정수는 정확히), 종류를 모르는 피연산자끼리만 예전 규칙 "둘 다 숫자면 숫자, 아니면 글자"), 스칼라 함수의 결과 종류(`UPPER`·`CONCAT`… 글자, `LENGTH`·`ROUND`·`ABS`… 숫자).
- **파서가 따옴표를 기록**한다(`Literal.quoted`, `Between.lo_quoted/hi_quoted`, IN 목록의 `quoted`; AST JSON에 저장돼 뷰·프로시저·트리거가 재시작 뒤에도 문자열을 기억). 변수 값은 숫자가 아니면 문자열, 엔진이 만드는 키 조건은 문자열.
- **바인더**(앞 항목들의 `bind_statement`)가 열·식·리터럴의 종류를 주석으로 단다(`Col.cls`, `Condition.left_class/right_class`, `OrderBy.cls`, `Agg.arg_class`, `WinFunc.col_class`); 뷰·CTE·파생 테이블은 안쪽 질의의 열 종류를 물려받는다. 평가기(`WHERE`·`HAVING`·`ON`·`CASE`·select 목록의 비교, `IN`·`BETWEEN`·`NOT BETWEEN`·서브쿼리와의 비교)가 `compare_classed`를 쓴다.
- **B+Tree `KeyKind`**: 키의 열마다 `Text`(바이트 순서), `Number`(값 순서, 정수는 정확히, 같은 값의 다른 철자는 글자로 구별), `Mixed`(종류를 모를 때: 빈 키 < 숫자 < 글자의 **이행적인 전순서**); 복합 키는 구간마다 자기 종류. 인덱스는 만들 때·재시작 때 열 타입에서 종류를 정한다.
- **플래너**: 글자 열의 인덱스는 따옴표 문자열(과 `LIKE` 접두사)만, 숫자 열의 인덱스는 숫자로 읽히는 값만, 복합 인덱스는 숫자 열만 답한다 — 아니면 스캔. 글자↔숫자 조인은 중첩 루프, 글자↔글자 조인은 글자 그대로의 해시(정수 해시 키는 double이 아니라 정확한 정수).
- **정렬·집계**: `ORDER BY`(select·윈도)·`MIN`/`MAX`가 열의 종류로 비교한다(글자 열은 바이트, 숫자 열은 값). `x BETWEEN lo AND hi`는 경계가 NULL(변수)일 때 3값 논리(`x >= NULL AND x <= hi`는 `x > hi`이면 FALSE, 아니면 UNKNOWN — 그래서 `NOT BETWEEN`은 다른 경계가 배제하는 행을 고른다).
- **UPDATE·DELETE의 기본키 단축**(`extract_pk_eq_value`·`extract_pk_between_value`가 이제 스키마를 받는다): 키로 읽는 것은 정확히 한 행을 뜻할 때뿐 — 따옴표 문자열, 또는 숫자 열에 쓴 숫자; 글자 열에 쓴 숫자와 맨 단어(열 이름)는 스캔으로 넘기고, BETWEEN은 글자 키에는 따옴표 경계, INT 키에는 정수 경계일 때만 단축한다.

**검증**:
- 신규·보강 Catch2 21케이스(527 → 548): `test_typed_comparison.cpp` 11케이스(① VARCHAR 키 450개가 기본키·보조·해시·인덱스 없음 어디서나 조회되고 정렬·한쪽 범위·UPDATE/DELETE 범위가 바이트 순서이며 재시작 뒤에도 그대로, ② 비교의 종류 규칙(문자열/숫자 × 열/리터럴 × `=`·`<`·`BETWEEN`·`IN`, 인덱스 있을 때와 없을 때), ③ **무작위 표를 독립 참조(정규식으로 MySQL의 수 읽기를 구현)와 비교**, ④ 따옴표가 열 이름이 아님(REPLACE·ON DUPLICATE·MERGE·다중 DELETE 포함), ⑤ 정렬·MIN/MAX·윈도·HAVING, ⑥ 조인(2^53 넘는 정수, 글자 기본키), ⑦ 뷰·파생 테이블·CTE, ⑧ 함수·식, ⑨ 뷰의 따옴표가 재시작 뒤에도, ⑩ 숫자 열 타입 9종과 글자 열 타입 3종, ⑪ UPDATE/DELETE의 기본키 단축 경로), `test_btree`(같은 값의 다른 철자, 복합 키의 구간별 종류), `test_planner`(종류를 모르는 열), `test_select_index`(숫자 열의 `LIKE`, 복합 인덱스), `test_join`(2^53 넘는 정수, 해시 키). 기존 테스트 여러 곳이 옛 의미(`code = '07.0'`이 `'7'`도 맞힘, `MAX('7','10') = '10'`)를 박아 둔 것이어서 MySQL의 값으로 고쳤다.
- **새 검증 도구 `verify_compare.py`**(한 빌드를 MySQL 규칙으로 직접 검사): VARCHAR·INT·DECIMAL·BIGINT(2^53 근처) 열 × 인덱스 없음/모든 열 인덱스/해시/VARCHAR 기본키 표 4개에 같은 행을 넣고, 무작위 `WHERE`(= <> < <= > >=, BETWEEN, IN, NOT IN, AND/OR; 리터럴은 문자열 또는 숫자)·`ORDER BY`·`MIN`/`MAX`·조인(글자=글자, 숫자=숫자, 글자=숫자)을 **정확한 분수 산술로 계산한 참조**와 비교. 24시드 × 1,500문장 위반 0. `verify_orderby_distinct.py`는 글자 열 목록을 고쳤다.
- **심은 버그 97종**(따옴표 파싱, 종류 판정, `compare_classed`의 각 분기, BETWEEN·IN, 정렬 비교, MIN/MAX, B+Tree 종류별 비교기·빈 키·복합 키, 플래너 인덱스 허용 규칙, 조인 해시 키, 정수 정확성, 변수·내부 조건의 따옴표, 바인더, 열 타입 분류, AST JSON, 기본키 단축 규칙 …) 가운데 **처음에는 27종이 살아남았고**(타입이 다른 키 조건, 복합 인덱스, JSON 왕복, 정수 해시, 조건을 안 보는 단축 경로 …) 각각 테스트를 보강해 잡게 했다. 최종 88종을 테스트가 잡고, 4종은 **결과가 같고 속도만 다른 변이**(정확 키 해시 옵션, 단축 경로를 아예 끄기 등)라 근거를 남기고 제외, 5종은 고치면서 해당 코드가 사라져 폐기.
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) 중 **319개가 달랐고 전부 의도한 변화**로 분류했다(VARCHAR 열을 문자열과 비교 226 · `DISTINCT`/`GROUP BY`/`MIN`/`MAX` 107 · 숫자 열과 따옴표 낱말 비교 95(`t.grp = 'qty'`가 예전에는 `qty` 열이었다) · 글자 열 조인 91 · 글자 열 `ORDER BY` 73 · 글자 집계 `ORDER BY` 10; 한 질의가 여러 범주에 속함). 모든 차이 질의가 글자 열·따옴표 낱말·글자 집계를 쓰는 것이었고 그 밖의 질의는 차이가 없었다.
- Release/Debug **548 케이스/1,329,342 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과(Debug도 548케이스: 스택 오버플로로 케이스가 조용히 빠지지 않았음). assertion 수가 앞 항목(1,421,575)보다 줄어든 것은 **옛 비교기와 같은 순서인지 80만 번 확인하던 테스트 한 개**(그 순서가 이행적이지 않은 것이 버그의 원인이라 더는 기준이 될 수 없음)를 종류별 새 테스트로 바꿨기 때문이다 — 케이스별로 앞 빌드와 대조해 다른 테스트는 하나도 줄지 않았고 늘었음을 확인했다. SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,950,808), 쓰기 퍼저 40시드(43,182), 조인 퍼저 60시드(7,178), `[aggregate]` 긴 캠페인(11케이스 317,626), `[aggregate_semantics][random]` 60시드(23,151), 새 `[typed_comparison][random]` 200시드(227,000; 기본과 DML 인덱스 강제 양쪽), 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 17,113) 불일치 0. `verify_compare.py` 34시드 × 1,500문장, `verify_joins` 8시드 × 600문장 + 30행 3시드, `verify_writes` 3시드, `verify_null_expressions`·`verify_agg_expressions`·`verify_aggregates`·`verify_orderby_distinct` 각 2시드 위반 0.
- 성능(앞 항목 빌드와 번갈아, 캐시를 피하려고 매번 다른 문장): 50,000행, 최솟값 기준, 3라운드 중 가장 빠른 값(old → new). 같거나 빨라진 것: 글자 열 스캔 `WHERE code = 'CODEn'` 23.9 → 15.6ms, 글자 열 `ORDER BY code LIMIT 10` 64.8 → 59.7, VARCHAR 기본키 한쪽 범위 17.6 → 14.4, 숫자 열 스캔 19.4 → 17.5(잡음 범위), 소수 열 스캔·글자 범위 스캔·`MIN`/`MAX`·`GROUP BY`·`DISTINCT`·정수 조인·VARCHAR 기본키/보조 인덱스 점 조회(0.08ms)는 같음. **느려진 것 둘**: 글자 열 5만 행 전체 `ORDER BY s` 79.4 → 85.1ms(+7%), **글자 열 조인**(2,000 × 50,000행) 76.8 → 88.7ms(+15%) — 글자 키는 숫자로 정규화한 해시를 쓸 수 없어(`'007'` ≠ `'7'`) 플래너의 해시 조인 대신 쌍마다 ON 조건을 확인하는 해시 조인을 쓰기 때문이며, 정확함의 대가로 받아들였다.

**눈에 띄는 변화(의도한 것)**: `WHERE code = '7'`이 `'007'`을 맞히지 않는다(`code = 7`은 맞힌다); 글자 열 `ORDER BY`·`MIN`/`MAX`가 바이트 순서(`'10' < '9'`); `WHERE name = 'city'`가 문자열 비교; VARCHAR 기본키·보조 인덱스가 모든 키를 찾는다; 2^53 넘는 정수가 구별된다; `DELETE … WHERE a = b`가 `a`와 `b`가 같은 행을 지운다.

**정직한 한계**: ① `IN (서브쿼리)`의 소속 판정은 아직 정확한 글자 비교(숫자/글자를 구별하지 않음); ② `UNION`/`INTERSECT`/`EXCEPT`의 `ORDER BY` 비교기·`CHECK` 제약(실행 때 파싱)·갭 락과 파티션 경계 비교는 옛 규칙; ③ 프로시저의 `IF`/`WHILE` 조건은 바인더를 거치지 않아 옛 규칙; ④ 종류를 모르는 식(`COALESCE`·`IF`·`GREATEST`의 결과)은 옛 규칙("둘 다 숫자면 숫자")이라 문자열 열을 담은 `IF(c, s, 'x') = '7'`이 `'007'`에도 참; ⑤ 날짜 리터럴의 여러 철자는 글자로 비교; ⑥ **대소문자 구분 비교**(MySQL 기본 콜레이션은 구분 안 함)는 구현하지 않음; ⑦ 글자 열의 복합 인덱스는 쓰이지 않음(속도만). **이 점검에서 새로 찾아 따로 고칠 것**: 스칼라 서브쿼리가 여러 행·여러 열을 돌려줘도 오류 없이 첫 값(MySQL 1242/1241, 이미 ⑯), 서브쿼리 안 오류가 조용히 FALSE.

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어(실행기에 단계 하나, B+Tree에 키 종류가 늘었을 뿐) 변경 없음.

### 10월 6일 (여섯 번째) — 집계 함수의 인자에 식 쓰기(`SUM(price * qty)`·`AVG(a + b)`·`COUNT(1)`), 한 select의 조건부 집계가 서로의 값을 덮어쓰던 것, 식의 괄호가 사라져 함수 인자가 틀리게 계산되던 것

**왜 이 항목인가**: 사용자 결정("전부 고쳐")의 집계 인자 항목(R3). 매출 합계 같은 흔한 질의 `SUM(price * qty)`가 파싱 오류(`Expected comparison operator`)였다 — 집계의 인자는 열 이름 하나뿐이었다. 구현하며 같은 곳의 **조용한 오답** 둘을 찾아 함께 고쳤다. 새 SQL 문법은 이 한 가지(집계 인자)뿐이고 나머지는 버그 수정이다.

**원인과 영향**:
- **집계 인자가 열 이름뿐**: `SUM(price * qty)`, `AVG(a + b)`, `SUM(COALESCE(x, 0))`, `COUNT(1)`, `SUM(2)`, `COUNT(DISTINCT a + b)`, `GROUP_CONCAT(CONCAT(a, b))`, `HAVING SUM(price * qty) > 100`, `SUM(a * b) OVER (…)`, `SUM(a * b > 5)`가 모두 파싱 오류였다. HAVING·식 안의 집계는 `SUM(x)`·`COUNT(*)` 같은 단순한 모양만 읽었다.
- **(찾음) 한 select 목록의 조건부 집계 둘이 같은 결과 열 `SUM(CASE)`**: `SELECT SUM(CASE WHEN a > 1 THEN 1 ELSE 0 END), SUM(CASE WHEN a > 4 THEN 1 ELSE 0 END) FROM o`가 `3`과 `1`이 아니라 **둘 다 마지막 값 `1`**을 보여 줬다(별칭이 없을 때; `SUM(a > 1), SUM(a > 4)`·`COUNT(CASE …)`도). 같은 이름의 열이 서로를 덮어쓴 것 — 조건부 집계로 만드는 집계표에서 흔한 모양이라 조용한 오답이었다.
- **(찾음) 식의 텍스트에서 괄호가 사라짐**: 함수 인자는 식을 글자로 적어 두었다가 다시 읽는데 `Parser::arith_to_string`이 우선순위를 몰라 `(a + b) * 2`를 `a + b * 2`로 적었다 — **`ROUND((a + b) * 2, 1)`이 `a + (b * 2)`로 계산됐다**(a=1, b=2에서 6이 아니라 5; `ABS((a - b) * 2)`, `COALESCE(NULL, (a + b) * 2)`도). 문자열 속 작은따옴표도 두 배로 적지 않았다.

**수정**:
- 파서: 집계의 인자(select 목록의 집계, `GROUP_CONCAT`, 윈도 집계, HAVING·식 안의 `SUM(…)`/`COUNT(DISTINCT …)`)를 식으로 읽고, 열이면 열 이름, 아니면 **식의 텍스트**(`price * qty`, `COALESCE(x, 0)`, `1`)를 `col`에 둔다. 결과 열 이름은 쓴 모양 그대로(`SUM(price * qty)`, 괄호 포함). `arith_to_string`은 우선순위에 맞게 괄호를 적고 문자열의 따옴표를 두 배로 적는다.
- 실행기: 인자가 식인 집계가 있으면 행마다 그 식의 값을 계산해 식의 텍스트를 키로 행에 넣고, 집계·윈도 함수가 열처럼 읽는다(`SUM`/`AVG`/`MIN`/`MAX`/`COUNT`/`COUNT DISTINCT`/`GROUP_CONCAT`/`STDDEV`/`BIT_*`/`JSON_AGG`, `FILTER`, GROUP BY, HAVING, 식 안의 집계, 윈도, FROM 없는 SELECT, 서브쿼리 모두 같은 경로). 테이블 별칭(`SUM(x.price * c.rate)`)은 앞 항목들의 별칭 풀이를 그대로 쓰고, 변수(`SUM(price * @rate)`)는 값으로 바뀐다(결과 열 이름은 쓴 그대로). 바인더는 식 안의 열이 있는지 확인하고(`Unknown column 'nosuch' in 'field list'`) 식의 종류(숫자/글자)를 `MIN`/`MAX`의 비교에 쓴다. 식이 아닌 집계는 행을 복사하지 않고 전과 같이 포인터로 읽는다.
- 한 select 목록의 조건부 집계는 번호를 매긴 열 이름을 받는다(`SUM(CASE)`, `SUM(CASE)2`, `COUNT(CASE)`, `COUNT(CASE)2`).

**검증**:
- 신규 Catch2 7케이스(548 → 555, `test_aggregate_arguments.cpp`): ① 식 인자(곱·합·`COALESCE`·괄호의 묶음·상수·`NULL`·`DISTINCT`), ② HAVING·식 안의 집계·윈도(분할/누적)·서브쿼리, ③ `NULL`·행 없음·`GROUP_CONCAT`·글자 식의 `MIN`/`MAX`·FROM 없는 SELECT·없는 열의 오류, ④ 테이블 별칭·변수·뷰(재시작 뒤에도), ⑤ 식의 텍스트가 괄호를 지킴(함수 인자 포함), ⑥ 조건부 집계의 열이 서로 다름, ⑦ **무작위 표와 무작위 식(+ − *, 괄호, `COALESCE`, `NULL`)을 독립 참조로 계산해 전체·그룹별·HAVING·`COUNT(DISTINCT)`·`AVG`(정확한 몫을 4자리로)를 비교**.
- **새 검증 도구 `verify_agg_arguments.py`**(한 빌드를 정확한 분수 산술로 계산한 참조와 비교): 정수·소수 2자리·글자 열이 섞인 표에서 무작위 식의 `SUM`/`COUNT`/`MIN`/`MAX`/`AVG`/`COUNT(DISTINCT)`, GROUP BY, HAVING, 윈도, 조인의 한정된 열. 이전 빌드는 첫 질의에서 위반(파싱 오류)이라 도구가 버그를 본다는 것도 확인했다.
- 심은 버그 44종(괄호 규칙 각각, 따옴표 두 배, HAVING의 DISTINCT, 집계 인자를 요청하는 여섯 곳, 행 복사·행 포인터, 식 계산, 읽지 못한 인자의 오류, FROM 없는 SELECT, 열 이름의 번호, 바인더의 열 확인·종류, 변수, 인자 텍스트 판별) 가운데 **처음에는 10종이 살아남았고**(`a / (b * c)`의 괄호, 문자열 따옴표, HAVING의 `DISTINCT`, 식의 종류, 결과 열 이름의 괄호 …) 테스트를 보강해 잡게 했다. 최종 40종을 테스트가 잡고, 3종은 중복이던 코드(조건부 집계의 `__case__` 걸러내기가 두 군데, DISTINCT의 숫자 열 검사)라 코드를 줄여 폐기, 1종은 도달할 수 없는 방어 코드(읽을 수 없는 인자는 오류로 알림)로 남김.
- Release/Debug **556 케이스/1,330,024 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과(Debug도 556케이스). SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,950,808), 쓰기 퍼저 40시드(43,182), 조인 퍼저 60시드(7,178), `[aggregate]` 긴 캠페인(11케이스 317,626), `[aggregate_semantics][random]` 60시드(23,151), 새 `[aggregate_arguments][random]` 60시드(4,560), `[typed_comparison][random]`(68,100 / DML 인덱스 강제 227,000), 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 16,503) 불일치 0. 새 `verify_agg_arguments.py` 8시드 × 300질의(시드마다 1,100번 검사), `verify_compare` 3시드, `verify_joins` 8시드 × 600문장 + 30행 3시드, `verify_writes` 3시드, `verify_null_expressions`·`verify_agg_expressions`·`verify_aggregates`·`verify_orderby_distinct` 각 2시드 위반 0. (같은 폴더에서 회귀 두 개를 동시에 돌리면 임시 폴더를 서로 지워 한 케이스가 실패한 적이 있어, 하나씩 따로 다시 돌려 통과를 확인했다.) 검증 도구가 찾은 것: `COUNT(DISTINCT COALESCE(w, 7))`이 `7.00`과 `7`을 다른 값으로 센 것 — DISTINCT를 숫자는 값으로 가르게 고쳤다(테스트·심은 버그 9종 추가).
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) 중 **0개가 달랐다** — 이 말뭉치에는 식 인자도 조건부 집계 둘도 없어 달라질 질의가 없다는 뜻이고, 새 동작은 `verify_agg_arguments.py`가 직접 검증한다.
- 성능(앞 항목 빌드와 번갈아, 캐시를 피하려고 매번 다른 문장): 50,000행, 3라운드 중 가장 빠른 값(이전 → 이번): `COUNT(*)` 43.4 → 42.9ms, `SUM`/`AVG` 55.8 → 55.6, 소수 49.9 → 49.2, `MIN`/`MAX` 55.9 → 55.7, `GROUP BY` + 4집계 69.0 → 68.8, `HAVING AVG` 58.4 → 57.8, `COUNT(DISTINCT val)` 55.2 → 54.7, 글자 열 `COUNT(DISTINCT code)` 53.6 → 51.4, `SUM(DISTINCT val)` 53.7 → 54.6 — 식이 아닌 집계는 같다. 새로 되는 것: `SUM(price * val)` 153ms(행을 복사해 식을 계산하는 값이라 같은 5만 행 `SUM(val)`의 약 2.8배), `GROUP BY grp, SUM(price * val)` 163ms, `HAVING COUNT(DISTINCT val)` 187ms(전에는 파싱 오류).

**눈에 띄는 변화(의도한 것)**: `SUM(price * qty)` 같은 식 인자가 된다; 한 select의 조건부 집계가 서로 다른 값을 보인다(열 이름은 `SUM(CASE)`, `SUM(CASE)2`); `ROUND((a + b) * 2, 1)` 같은 함수 인자의 괄호가 지켜진다; 식 안 집계 `SUM(a * b) / COUNT(*)`.

**정직한 한계**: ① `SUM(CASE WHEN … END) * 100.0 / COUNT(*)`처럼 **CASE 집계를 식 안에 쓰면 NULL**(앞부터 그랬다)이고 `HAVING SUM(CASE …) >= 1`은 파싱 오류, 조건부 집계의 열 이름은 MySQL처럼 식 전체가 아니라 `SUM(CASE)` — CASE를 식으로 다루는 다음 항목(R4)에서 고친다; ② `ROUND(SUM(a * b), 2)`처럼 **함수 안의 집계**는 아직 파싱 오류(R4); ③ `COUNT(DISTINCT a, b)`(여러 열)는 아직; ④ `HAVING`·식 안의 집계에 쓴 변수(`HAVING SUM(price * @rate) > 1`)는 값으로 바뀌지 않음; ⑤ 산술의 결과는 끝자리 0 없이 출력(`10`, MySQL은 `10.00`).

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

---

### 10월 6일 (일곱 번째) — 서브쿼리: 여러 행·여러 열을 돌려줘도 오류가 없던 것(MySQL 1242/1241), 서브쿼리 안의 오류가 "행 없음"이 되던 것, HAVING의 서브쿼리 비교가 늘 거짓이던 것, GROUP BY 없는 집계의 HAVING

**왜 이 항목인가**: 사용자 결정("전부 고쳐")의 서브쿼리 항목(R9; 앞 항목들의 점검에서 찾은 ⑯). 새 문법이 아니라 **틀린 답을 오류로** 바꾸고, 서브쿼리가 든 조건이 조용히 잘못 답하던 곳 둘을 고친 것이다.

**원인과 영향**:
- **여러 행을 돌려주는 스칼라 서브쿼리**: `WHERE v = (SELECT a FROM u)`가 `u`에 행이 셋이어도 첫 행의 값으로 비교하고 `1 row(s) returned`(MySQL: 오류 1242 `Subquery returns more than 1 row`). select 목록의 `(SELECT a FROM u)`, FROM 없는 SELECT, `UPDATE`/`DELETE`의 WHERE도 같았다 — **`UPDATE t SET v = 0 WHERE v = (SELECT a FROM u)`가 첫 행의 값이 가리키는 행을 조용히 바꿨다**.
- **여러 열을 돌려주는 서브쿼리**: `v = (SELECT a, b FROM u)`, `v IN (SELECT a, b FROM u)`, select 목록의 `(SELECT a, b …)`가 첫 열만 읽었다(MySQL: 1241 `Operand should contain 1 column(s)`, 행이 없어도 실행 전에 오류).
- **서브쿼리 안의 오류가 "행 없음"**: 없는 테이블(`v IN (SELECT a FROM nosuch)`)이나 안쪽의 서브쿼리가 두 행인 경우에 `IN`/`EXISTS`는 FALSE, 스칼라 비교는 UNKNOWN, select 목록은 NULL, `UPDATE`/`DELETE`는 "0 row(s)" — 오류가 어디에도 보이지 않았다.
- **HAVING의 서브쿼리 비교가 늘 FALSE**: `HAVING SUM(v) > (SELECT AVG(a) FROM u)`가 어떤 값이든 그룹을 하나도 남기지 않았다(HAVING 평가기는 서브쿼리를 몰랐다).
- **(찾음) GROUP BY 없는 집계의 HAVING**: `SELECT SUM(v) FROM t HAVING SUM(v) > 5`가 `NULL`, `SELECT COUNT(*) FROM t HAVING COUNT(*) = 3`이 `0`이었다 — HAVING을 집계하기 **전의 행마다** 평가해서 `SUM(v)`가 없는 값이라 모든 행이 떨어지고 집계는 행 없는 입력 위에서 계산됐다.

**수정**:
- 새 `StatementError`(`statement_error.hpp`): 비교 안에서 평가되는 식은 오류를 위로 돌려줄 길이 없어서 던지고, `execute_with_s`(와 `execute_sql`)가 그 문장의 오류로 바꾼다. 던지는 곳은 행을 고르는 단계라 문장은 아무것도 바꾸지 않은 채 실패한다(트랜잭션 안에서도 앞의 문장들은 그대로, 문장만 실패; 저장 프로시저의 CALL은 오류를 돌려주고 다음 호출은 정상).
- 스칼라 비교(`= <> < <= > >=`)에서 서브쿼리가 두 행 이상이면 `Subquery returns more than 1 row`(행 없음·NULL은 UNKNOWN 그대로), select 목록과 FROM 없는 SELECT도 같다. 서브쿼리가 오류를 내면(`IN`·`NOT IN`·`EXISTS`·스칼라·select 목록) 그 오류가 문장의 오류.
- 바인더: 비교·`IN`·`NOT IN`·select 목록의 서브쿼리가 두 열 이상이면 `Operand should contain 1 column(s)`(`EXISTS`는 열 수를 가리지 않음).
- HAVING은 서브쿼리를 아는 평가기를 쓴다. GROUP BY 없는 집계의 HAVING은 한 줄의 집계 결과에 적용하고 거짓이면 빈 결과(`0 rows returned.`).

**검증**:
- 신규 Catch2 8케이스(556 → 564, `test_subquery_cardinality.cpp`): ① 두 행 서브쿼리가 연산자 여섯 개 × WHERE·HAVING·select 목록·FROM 없는 SELECT·UPDATE·DELETE에서 오류이고 UPDATE·DELETE는 아무것도 바꾸지 않음, 한 행·행 없음·NULL·`LIMIT 1`은 정상, 아무 행도 요구하지 않으면 실행되지 않음; ② 두 열(비교·`IN`·`NOT IN`·`*`·select 목록·중첩·UPDATE/DELETE), `EXISTS`는 정상; ③ 상관 서브쿼리는 **보는 행에** 두 건이 있을 때만 실패; ④ 안쪽 서브쿼리의 오류(`IN`·`NOT IN`·`EXISTS`·`NOT EXISTS`·스칼라); ⑤ HAVING의 서브쿼리 비교와 GROUP BY 없는 HAVING; ⑥ 트랜잭션·저장 프로시저에서 실패한 문장 뒤; ⑦ 없는 테이블; ⑧ **무작위 표의 서브쿼리를 독립 참조(행 수 규칙)와 비교**(비상관·상관·select 목록).
- **새 검증 도구 `verify_subqueries.py`**(한 빌드를 모델과 비교): 스칼라·상관·select 목록·HAVING(GROUP BY 있음/없음)·`IN`/`NOT IN`(3값 논리)·두 열·UPDATE/DELETE(오류면 바뀐 것이 없음)를 무작위 표에서. 이전 빌드는 첫 시드에서 위반(HAVING)이라 도구가 버그를 본다는 것도 확인했다.
- 심은 버그 19종(행 수 규칙·한계, 안쪽 오류를 숨기는 곳 넷, 캐시, select 목록의 행 수·오류·상관, FROM 없는 SELECT, HAVING의 평가기·GROUP BY 없는 적용·집계 계산, 바인더의 열 수 규칙 셋, 오류 메시지) 가운데 **처음에는 6종이 살아남았고**(안쪽 서브쿼리의 오류가 `Err`로 돌아오는 경로 — 없는 테이블 — 가 테스트에 없었다) 테스트를 보강해 잡게 했다. 최종 17종을 잡고, 1종은 속도만 다른 변이(답의 캐시), 1종은 중복이던 코드(서브쿼리를 아무 행도 요구하지 않을 때 건너뛰는 검사 — 그 경로에 닿지 않음)라 코드를 줄여 폐기.
- Release/Debug **564 케이스/1,330,774 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과(Debug도 564케이스). SELECT 차분 퍼저 150시드, DML 퍼저 80시드, 쓰기 퍼저 40시드, 조인 퍼저 60시드, `[aggregate]` 긴 캠페인 30시드, `[aggregate_semantics][random]` 60시드, 새 `[subquery_cardinality][random]` 80시드(5,269 assertions), 크래시 퍼저 90라운드·동시 퍼저 30라운드 불일치 0. 새 `verify_subqueries.py` 12시드 × 400문장 위반 없음, 앞 항목들의 검증 도구(`verify_agg_arguments`·`verify_compare`·`verify_joins`·`verify_writes`·`verify_null_expressions`·`verify_agg_expressions`·`verify_aggregates` 등)도 이전과 같이 위반 없음.
- 빌드 간 차분(이전 빌드 = 앞 항목): 2,970질의 중 **28개가 달랐고, 28개 모두 두 행 이상을 돌려주는 스칼라 서브쿼리가 오류로 바뀐 것**(의도한 변화; 전에는 첫 행으로 답함). 그 밖의 달라진 질의는 없다.
- 성능: 앞 항목 빌드와 번갈아 3라운드, 가장 빠른 값(이전 → 이번): 서브쿼리 5종(2만 행) `WHERE val > (SELECT AVG…)` 7.46 → 7.71ms, `WHERE val IN (SELECT …)` 3.09 → 3.31, 상관 `EXISTS`(50그룹) 272.3 → 273.3, `HAVING SUM(val) > (SELECT …)` 3.25 → 3.34, select 목록의 상관 스칼라 10.21 → 10.52; 집계 10종(5만 행) `COUNT(*)` 43.12 → 42.94, `GROUP BY` + 4집계 68.46 → 67.56, `HAVING AVG` 57.6 → 57.83, `SUM`/`AVG`(정수) 56.27 → 55.37 — 모두 측정 오차 안(서브쿼리 쪽이 최대 +0.3ms인 것은 행 수 검사가 서브쿼리 결과 한 번을 더 보는 값).

**눈에 띄는 변화(의도한 것)**: 두 행을 돌려주는 `WHERE v = (SELECT …)`가 오류(전에는 첫 행); `IN (SELECT a, b …)`가 오류; 없는 테이블이 든 서브쿼리가 오류(전에는 빈 결과); `HAVING SUM(v) > (SELECT …)`가 값에 따라 그룹을 남긴다(전에는 늘 없음); `SELECT SUM(v) FROM t HAVING SUM(v) > 5`가 합계(전에는 NULL).

**정직한 한계**: ① 서브쿼리의 행 수는 그 값을 쓰는 행이 있을 때 검사한다(바깥 결과가 비면 서브쿼리를 실행하지 않아 오류가 없다; MySQL은 비상관 서브쿼리를 미리 평가할 수 있어 이 경우 오류를 낼 수도 있다); ② `= ANY (…)`/`ALL`/`SOME`과 행 생성자 `(a, b) = (SELECT …)`는 아직 파싱 오류; ③ `(SELECT …) + 1`, `UPDATE … SET v = (SELECT …)`, `INSERT … VALUES (…, (SELECT …))`는 식 문법의 빈틈이라 파싱 오류(R4); ④ 없는 테이블의 오류 문구는 엔진의 것(`Table 'd.nosuch' not found`)이고 MySQL 1146의 문구가 아님.

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

### 10월 6일 (여덟 번째) — 식: 함수·집계·CASE·IF·CAST가 식의 처음에 올 수 없던 것(`ROUND(x) * 100`), CASE의 결과가 식이 아니던 것·따옴표 문자열이 열 이름으로 읽히던 것, 조건이 값이 아니던 것(`v BETWEEN 5 AND 20`·`a AND b`), `SUM(CASE …) * 100.0 / COUNT(*)`가 NULL이던 것, `COUNT(CASE … ELSE 0 END)`가 조건이 참인 행만 세던 것, INSERT VALUES·UPDATE SET의 식

**왜 이 항목인가**: 사용자 결정("전부 고쳐")의 식 문법 항목(R4) 가운데 값을 만드는 식(select 목록·WHERE·HAVING·VALUES·SET). 정렬·GROUP BY의 식과 서브쿼리가 든 식은 다음 항목으로 남겼다. 새 기능이 아니라 **파싱 오류이거나 조용히 틀리던 것**을 고친 것이다.

**원인과 영향** (프로브 81문장 가운데 52개가 파싱 오류였고, 조용히 틀린 것도 있었다):
- **함수·CAST·CASE·IF·집계 뒤에 연산자가 오면 파싱 오류**: `ROUND(d) * 100`, `COALESCE(a, 0) + COALESCE(b, 0)`, `LENGTH(s) + 1`, `UPPER(s) || '!'`, `CAST(v AS SIGNED) + 1`, `ABS(a - b) * 2`, `IF(c, a, b) * 3`, `IFNULL(MAX(v), 0) + 1`(오른쪽 `1 + ROUND(x)`는 됨). select 목록은 첫 토큰이 함수면 함수만 읽고 끝냈다.
- **`CASE`·`IF`의 결과가 식이 아니었다**: `CASE WHEN v > 5 THEN v * 2 ELSE w + 1 END`, `CASE x WHEN 1 THEN … END`는 파싱 오류, **`THEN 'n'`은 열 n이 있으면 그 열의 값**(따옴표를 잃은 문자열을 열 이름으로 읽음).
- **조건이 값이 아니고, 값이 조건이 아니었다**: `SELECT v BETWEEN 5 AND 20`·`v IN (7, 10)`·`v IS NULL`·`a AND b`는 파싱 오류, `WHERE flag`·`WHERE TRUE`·`WHERE v IS TRUE`·`WHERE NOT v`도 파싱 오류. 괄호로 시작하는 값(`WHERE (v + w) * 2 > 20`)과 `WHERE CAST(v AS SIGNED) > 5`, `x < NULL + 3`도.
- **BETWEEN의 경계가 토큰 하나**: `v BETWEEN w AND w + 20`은 파싱 오류, **`v BETWEEN w AND 20`은 열 w를 문자열 "w"로 비교**해 조용히 틀린 답.
- **집계와 CASE**: `SUM(CASE WHEN … END) * 100.0 / COUNT(*)`가 조용히 `NULL`(내부 이름 `AGG(__case__)`), `HAVING SUM(CASE …) > 0`·`SUM(CASE WHEN c THEN a * b END)`·`AVG`/`MIN`/`MAX(CASE …)`가 파싱 오류 또는 `Unknown column '__case__'`, **`COUNT(CASE WHEN v > 5 THEN 1 ELSE 0 END)`가 5가 아니라 조건이 참인 행의 수 3**(MySQL은 NULL이 아닌 값 = 모든 행), `COUNT(age >= 30)`도 같다.
- **INSERT VALUES·UPDATE SET의 식**: `INSERT … VALUES (1 + 2, UPPER('x'), NOW())`는 파싱 오류, `UPDATE … SET v = CASE …`·`ON DUPLICATE KEY UPDATE v = CASE …`도.
- **괄호 없는 `CURRENT_DATE`/`CURRENT_TIMESTAMP`**, 식 안의 `DATE_ADD(d, INTERVAL n DAY)`·`DATE_FORMAT`·`DATABASE()`·`USER()`가 파싱 오류(`WHERE d > DATE_SUB(CURDATE(), INTERVAL 30 DAY)` 포함).
- (무작위 검증이 찾은 것) **`NULLIF(5, 5.00)`이 NULL이 아님**(문자열로 비교), **5자리 이상 소수의 합이 4자리로 반올림**(`SUM(price * rate)` = 14.451375가 14.4514), **리터럴 `-0`이 `-0`으로 출력**.

**수정**:
- 새 식 노드 `ArithExpr::Pred`(조건을 값으로: 참 1·거짓 0·모름 NULL)와 `CASE`(함수 `CASE(when1, then1, …, [else])`, 조건은 Pred). 조건의 비교·3값 논리·타입 규칙은 WHERE의 것을 그대로 쓴다. 식 파서 `parse_value_expr`(OR > AND > NOT > 술어 > 산술)가 select 목록·함수 인자·괄호·집계 인자·SET·VALUES·ON DUPLICATE의 식을 읽고, 술어가 없으면 값 그대로(조건으로 쓰이면 "NULL도 0도 아니면 참", 텍스트는 앞의 숫자로).
- select 목록: 함수 호출·`CASE`·`IF`·`CAST`는 모두 식으로 읽고(결과 열 `SelectColumn::Expr`), 집계는 뒤에 연산자가 이어질 때 식으로 읽는다(`select_item_continues`: 괄호·CASE 밖의 연산자를 앞서 살핌). `SUM`/`COUNT`/`AVG`/`MIN`/`MAX(CASE …)`는 앞의 항목의 "인자는 식의 텍스트" 경로로 한 곳에서 처리(옛 `SumCase`/`CountCase` 경로와 `__case__` 번호는 읽기 전용으로만 남음).
- 조건 `(a + b) * 2 > 10`처럼 괄호로 시작하는 값: 조건의 묶음으로 먼저 읽어 보고, 뒤에 식이 이어지면 술어 하나로 다시 읽는다. `BETWEEN`의 경계가 숫자·문자열·`@변수`가 아니면 두 비교(`>= lo AND <= hi`)로 푼다.
- INSERT VALUES: 값이 글자·숫자·NULL·DEFAULT·`@변수`·`NEW.x`이면 전과 같이 텍스트, 아니면 `"\x01" + 식의 JSON`으로 두고 실행기가 변수를 넣은 뒤(트리거의 NEW/OLD 포함) 계산해 값으로 만든다(파티션 라우팅 전에도).
- `Parser::arith_to_string`/`cond_to_string`이 CASE·조건·CAST·DATE_ADD를 다시 읽으면 같은 식이 되게 쓴다(집계 인자의 텍스트가 되기 때문); 서브쿼리가 든 조건은 쓸 수 없어 오류(`A subquery inside this expression is not supported`).
- 그 밖: `NULLIF`가 숫자는 값으로 비교, 합의 표시는 정수가 아니면 4자리 이상(5자리 이상이면 모든 자리), `-0`은 `0`, 비교의 오른쪽 `NULL + 3`.

**검증**:
- 신규 Catch2 10케이스(564 → 574, `test_value_expressions.cpp`): ① 함수·집계·CAST가 식의 처음에, ② CASE·IF(결과가 식, 단순 CASE, NULL 조건, 중첩, 열 이름과 같은 문자열, 따옴표, 타입, 형식 오류, 없는 열), ③ 조건이 값/값이 조건(3값 논리, IS TRUE/FALSE/UNKNOWN, 텍스트, BETWEEN 식 경계), ④ 집계 안의 식(비율·HAVING·COUNT(CASE …)·옛 형태와 같은 답), ⑤ INSERT/REPLACE/UPDATE/ON DUPLICATE의 식(변수·날짜 함수), ⑥ 트리거·프로시저·뷰·재시작 뒤, ⑦ 쓴 식을 다시 읽으면 같은 식, ⑧ 날짜 함수·`DATABASE()`, ⑨ 작은 것들(NULLIF·`-0`·`NULL + 3`·합 표시·파티션 키의 식·프로시저/변수의 식), ⑩ **무작위 식(정수 산술·ABS·COALESCE·IFNULL·NULLIF·CASE 둘·IF·CAST·조건을 숫자로·비교·IS …·BETWEEN·IN(NULL 포함)·AND/OR/NOT·숫자를 조건으로)을 독립 참조(3값 논리)와 비교**: select 목록·WHERE·NOT WHERE·집계·`SUM(CASE WHEN … END)`·GROUP BY/HAVING·UPDATE·상수의 INSERT.
- **새 검증 도구 `verify_value_expressions.py`**(한 빌드를 정확한 분수 산술·3값 논리로 계산한 참조와 비교; 정수·소수 2자리·글자 열, LIKE, 문자열 CASE/IF/CONCAT/UPPER). 첫 실행에서 `NULLIF`와 합의 반올림 두 건을 찾았다.
- 심은 버그 48종(식의 파서 — 술어 연산자·`IS TRUE`·BETWEEN 경계·OR/AND/NOT·CASE의 ELSE·단순 CASE·CAST 형식·NULL 오른쪽 식·`-0`·함수 뒤 연산자 판단·집계 인자·INSERT 값; 식을 다시 쓰는 곳 여섯; 실행기 — CASE·Pred 평가·타입·바인더·변수·집계 찾기·INSERT 식 계산 두 곳·합 표시·NULLIF·쿼리 캐시 목록; SET·RETURN의 식 셋) 가운데 **처음에는 6종이 살아남았고**(`CAST(x AS SIGNED INT)`, 다시 읽기에서 남은 글자, `- 0`, 함수·집계 뒤의 `<` `>=` `<=`, `CASE x WHEN NULL`, INSERT 값 `a.b`) 테스트를 보강해 4종을 잡게 했다. 최종 45종을 테스트가 잡고, 2종은 결과가 같은 변이(`CASE x WHEN NULL`의 NULL을 따옴표 없는 `NULL`로 쓰기 — 어느 쪽이든 아무 값과도 같지 않음; INSERT 값이 `a.b` 모양일 때의 옛 입력 처리 — 뜻 있는 입력이 없음)이고 1종은 죽은 코드(`SIGNED INTEGER`의 `INTEGER` 건너뛰기: 이미 키워드 토큰이라 닿지 않아 코드를 줄임)다.
- Release/Debug **574 케이스/1,333,363 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과(Debug도 574케이스). SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,950,808), 쓰기 퍼저 40시드(43,182), 조인 퍼저 60시드(7,178), `[aggregate]` 긴 캠페인(11케이스 317,627), `[aggregate_semantics][random]` 60시드(23,151), `[aggregate_arguments][random]` 60시드(4,560), `[subquery_cardinality][random]` 80시드(5,269), `[typed_comparison][random]`(68,100 / DML 인덱스 강제 227,000), 새 `[value_expressions][random]` 100시드(34,700 / DML 인덱스 강제 60시드 20,820), 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 16,356) 불일치 0. 새 `verify_value_expressions.py` 12시드 × 250라운드 위반 없음(그 앞의 시드 31개에서 `NULLIF`와 합의 반올림 두 건을 찾아 고침), `verify_subqueries`·`verify_agg_arguments`·`verify_compare`·`verify_joins`·`verify_writes`·`verify_null_expressions`·`verify_agg_expressions`·`verify_aggregates`·`verify_orderby_distinct`도 위반 없음.
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) 중 **40개가 달랐고, 40개 모두 `SELECT DISTINCT UPPER(tag) …`의 결과 열 이름이 `UPPER()`에서 `UPPER(tag)`로 바뀐 것**(의도한 변화)이며 값이 달라진 질의는 없다 — 이 말뭉치에는 함수가 식의 처음에 오거나 CASE가 든 질의가 없어서 새로 되는 것은 `verify_value_expressions.py`와 새 테스트가 직접 검증한다.
- 성능: 앞 항목 빌드와 번갈아 3라운드, 5만 행, 가장 빠른 값(이전 → 이번): 집계 `COUNT(*)` 47.5 → 45.8ms, `GROUP BY` + 4집계 79.3 → 78.2, `HAVING AVG` 69.3 → 68.2, `SUM`/`AVG` 64.9 → 63.3, `SUM(price * val)` 143.3 → 138.8; select 목록의 함수 `UPPER(s), LENGTH(s)` 239 → 250ms, `ROUND(price, 1), ABS(val)` 283 → 302(+7%: 함수 인자를 식으로 계산해 넘김), `CASE WHEN … THEN 'a' ELSE 'b'` 244 → 233, `val * 2 + 1` 239 → 228; WHERE `val BETWEEN a AND b` 32.4 → 32.1. **`SUM(CASE WHEN … END)` + GROUP BY는 62 → 119ms로 느려졌다**(조건부 집계가 앞 항목의 "식을 행마다 계산해 숨은 열로 두는" 경로를 쓰게 되어 행을 복사하기 때문; 전에는 틀린 답을 더 빨리 냈다). 새로 되는 것: `WHERE (val + id) * 2 > n` 35ms, `WHERE CASE … END = 1` 29ms, `COALESCE(val, 0) + 1` 225ms.

**눈에 띄는 변화(의도한 것)**: `COUNT(CASE WHEN … THEN 1 ELSE 0 END)`·`COUNT(조건)`이 NULL이 아닌 값을 센다(전에는 조건이 참인 행만); `SUM(조건)`이 모든 값이 NULL이면 NULL; 결과 열 이름: 함수·CASE 열은 `UPPER(s)`/`CASE`처럼 식 모양(전에는 `UPPER()`), 조건부 집계는 `SUM(CASE WHEN … END)`(전에는 `SUM(CASE)`, `SUM(CASE)2`); `-0`이 `0`; 5자리 이상 소수의 합이 반올림 없이.

**정직한 한계**: ① 결과 열 이름은 아직 MySQL처럼 타이핑한 글자 그대로가 아니다(공백을 뺀 `ROUND(d)*100`, `CASE`); ② 함수 안의 서브쿼리·`(SELECT …) + 1`·`WHERE x = (SELECT …) * 1.1`·`IN (식)`·`LIKE 식`은 파싱 오류(다음 항목); ③ `ORDER BY`·`GROUP BY`의 식·번호·별칭이 아직 안 됨(별칭으로 정렬하면 조용히 정렬 안 됨은 다음 항목에서); ④ 서브쿼리의 바깥 참조가 비교의 **왼쪽**에 있으면(`WHERE a.id = b.a_id`) 조용히 틀림(별도 항목); ⑤ 집계 안 CASE에 서브쿼리가 있으면 오류; ⑥ `STDDEV(x) / AVG(x)`처럼 `COUNT/SUM/AVG/MIN/MAX` 밖의 집계 뒤에 연산자를 쓰면 파싱 오류.

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

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
