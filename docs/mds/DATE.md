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

### 10월 7일 — 정렬·그룹: `ORDER BY`·`GROUP BY`가 select 목록의 이름·번호·식을 받음(별칭으로 정렬하면 조용히 정렬이 안 되던 것), UNION의 `ORDER BY`, 집계 안 식의 열 타입(앞 항목의 회귀), 숫자 그룹 키·`DISTINCT`, 함수 인자 안의 열 타입

**왜 이 항목인가**: 사용자 결정("전부 고쳐")의 식 문법 항목(R4) 가운데 정렬·그룹 쪽. 앞 항목의 점검에서 **`ORDER BY 별칭`이 정렬을 하지 않는다**는 조용히 틀리는 버그를 찾았고(`SELECT v AS val … ORDER BY val DESC`가 테이블 순서 그대로; 집계의 별칭만 정렬됨), 같은 뿌리의 파싱 오류(번호·식)와 함께 고쳤다.

**원인과 영향**:
- **select 목록의 이름으로 정렬하면 정렬이 안 됨**: 정렬은 `ORDER BY` 이름을 *원본 행*에서 찾는데 별칭은 행에 없는 키라 모든 행이 같은 값으로 비교되어 순서가 그대로였다 — `v AS val`·`id AS i`·`v + w AS k`·`UPPER(s) AS up`·`CASE … AS size`·윈도 함수 별칭(집계의 별칭만 됨). 오류 없이 틀린 순서.
- **번호·식은 파싱 오류**: `ORDER BY 2`·`GROUP BY 1`, `ORDER BY a + b`·`ABS(x)`·`CASE …`·`x IS NULL`·`COUNT(*) DESC`·`SUM(v)`, `GROUP BY id % 2`·`GROUP BY 별칭`.
- **UNION/INTERSECT/EXCEPT의 `ORDER BY`**: `ORDER BY 1`은 정렬 안 함(열 이름만 찾음), 없는 이름도 오류 없이 정렬 안 함.
- (이 점검에서 찾은 앞 항목의 **회귀**) 집계 안의 `CASE`·식은 텍스트로 다시 읽혀 열 타입을 잃었다: `SUM(CASE WHEN code = '007' THEN 1 ELSE 0 END)`가 `'7'`·`'7.0'`까지 센다(VARCHAR는 글자 비교인데 숫자 비교; 옛 `SumCase` 경로는 맞았음).
- **숫자 그룹 키·DISTINCT**: `GROUP BY COALESCE(w, 0)`(w DECIMAL)가 `0.00`과 NULL의 `0`을 서로 다른 그룹으로, `SELECT DISTINCT COALESCE(w, 0)`도 같음(그룹·중복 키가 글자).
- **함수 인자 안의 열은 타입을 모름**: `COALESCE(code, 'x') = '007'`이 숫자 비교라 `'7'`도 맞음(함수 인자는 바인더가 보지 않았음).

**수정**:
- 바인더: `ORDER BY`·`GROUP BY`의 항목이 번호이면 select 목록의 그 열(별 `*`는 열 이름들)로, select 목록이 주는 이름이면 그 이름이 뜻하는 것(열의 이름·식의 텍스트·집계 라벨)으로 바꾼다(이름은 같은 이름의 열보다 우선, MySQL처럼; 상수의 이름은 괄호로 감싸 번호와 구별). 한 `SELECT`에 한 번만(`Select::sort_resolved`; 다시 묶여도 — 파생 테이블·뷰 — 그대로) 하고, 범위 밖 번호는 `Unknown column 'N' in 'order clause'`.
- 파서: `ORDER BY`·`GROUP BY` 항목은 열(`date`처럼 키워드인 열 이름도)이면 이름, 정수면 번호, 아니면 **식의 텍스트**(`parse_value_expr`).
- 실행기: 식인 항목은 그 텍스트를 이름으로 한 숨은 값을 만들어 정렬·그룹이 열처럼 읽는다 — 행 단위(`ORDER BY` 식, `GROUP BY` 식)는 집계 인자와 같은 "행마다 계산" 단계에서, 그룹된 질의의 `ORDER BY` 식은 그룹마다(집계 호출 `SUM(v)`, 그룹 키, 그룹된 식을 읽음; 그룹된 식이 든 select 식·ORDER BY 식은 그 값을 읽도록 바꿈: `SELECT id % 2 … GROUP BY id % 2 ORDER BY id % 2`). 식은 질의의 테이블에 묶어(`parse_bound_expression`) 다시 읽으므로 열과 비교의 타입이 남는다(이 점검의 회귀도 이 경로로 고침; 바인더가 함수 인자 안의 열에도 타입을 줌, `COALESCE`·`IFNULL`·`GREATEST`·`LEAST`·`NULLIF`의 타입은 인자들의 공통 타입).
- 숫자 그룹 키·`DISTINCT` 키는 값으로(`0`과 `0.00`은 한 그룹), 글자는 글자로.
- UNION/INTERSECT/EXCEPT: `ORDER BY`는 답의 열 이름 또는 번호, 없는 이름·범위 밖 번호는 오류.

**검증**:
- 신규 Catch2 6케이스(574 → 580, `test_sort_group.cpp`): ① select 목록의 이름(열·식·함수·CASE·윈도·집계·집계의 식, LIMIT·OFFSET·DISTINCT·WHERE, 같은 이름의 열보다 우선, 상수의 이름), ② 번호(`*` 포함, 범위 밖 오류), ③ 식(산술·ABS·CASE·`IS NULL`·`LENGTH`·`COUNT(*)`·`SUM`·집계의 식·그룹된 식, 번호·별칭으로 `GROUP BY`), ④ 조인·뷰·파생 테이블·CTE·UNION/INTERSECT/EXCEPT, ⑤ 타입(VARCHAR의 `'007'`, 함수 인자, 숫자 그룹 키·DISTINCT), ⑥ **무작위 정렬·그룹을 독립 참조와 비교**(열·`t.열`·별칭·번호·식 키, 오름·내림, 그룹 식 × 정렬 항목).
- **새 검증 도구 `verify_sort_group.py`**(정렬 결과의 순서를 참조와 비교: 열·별칭·번호·식(숫자·글자) 키, 그룹 + 정렬, 조인, UNION). 앞 빌드는 첫 질의에서 위반이라 도구가 버그를 본다는 것을 확인했고, 이 점검에서 숫자 그룹 키와 함수 인자의 타입 두 건을 찾아 고쳤다.
- 심은 버그 25종(파서의 항목 판정·번호·GROUP BY, 바인더의 번호 풀이·이름 풀이·집계 라벨·식 텍스트·상수·다시 묶기·함수 인자 타입·식 묶기, 그룹 키 타입·`COALESCE`/`NULLIF` 타입, 숨은 값 넷, 그룹된 식 바꾸기 둘, DISTINCT 키, 집합 연산 둘) 가운데 **처음에는 2종이 살아남았고**(`NULLIF`의 타입, 그룹된 식이 든 ORDER BY 식) 테스트를 보강해 잡게 했다. 최종 25종을 모두 테스트가 잡는다. 이 점검 중 테스트가 찾은 구현 버그 하나: 풀이했다는 표시가 이름 한정 단계에서 지워져 파생 테이블 안의 정렬이 다시 풀이됨(`SELECT id AS v, v AS w … ORDER BY w`) — 고쳐서 테스트로 남김.
- Release/Debug **580 케이스/1,333,872 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과(네 가지 모두). SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,950,808), 쓰기 퍼저 40시드(43,182), 조인 퍼저 60시드(7,178), `[aggregate]` 긴 캠페인(11케이스 317,627), `[aggregate_semantics][random]`(23,151), `[aggregate_arguments][random]`(4,560), `[subquery_cardinality][random]`(5,269), `[typed_comparison][random]`(68,100 / DML 인덱스 강제 227,000), `[value_expressions][random]`(34,700 / 강제 20,820), 새 `[sort_group][random]`(7,800 / 강제 4,160), 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 15,398) 불일치 0. 새 `verify_sort_group.py` 12시드 × 200라운드 위반 없음(앞 빌드는 첫 질의에서 위반), `verify_value_expressions`·`verify_subqueries`·`verify_agg_arguments`·`verify_compare`·`verify_joins`(큰 표 포함)·`verify_writes`·`verify_null_expressions`·`verify_agg_expressions`·`verify_aggregates`·`verify_orderby_distinct`도 위반 없음.
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) **차이 0건** — 이 말뭉치의 질의는 별칭·번호·식으로 정렬하지 않아 의도한 변화가 나타나지 않는다(그 경우는 `verify_sort_group.py`와 새 테스트가 직접 검증한다). 곧 평범한 질의의 답과 열 이름은 이 항목 전후로 같다.
- 성능: 앞 항목 빌드와 번갈아 측정(5만 행): 집계(`COUNT(*)` 약 50ms, `SUM`/`AVG` 약 70, `GROUP BY` + 4집계 약 88, `HAVING AVG` 약 77), 식(`UPPER(s), LENGTH(s)` 약 300ms, `val * 2 + 1` 약 280, `SUM(CASE …)` + GROUP BY 약 140, `SUM(price * val)` 약 170), WHERE 식(35~40ms) 모두 **두 빌드가 ±3% 안(측정 잡음)에서 같다** — 숫자 그룹 키를 값으로 바꾼 것(`GROUP BY grp` 88.7 vs 90.5ms)도 차이가 보이지 않는다. 정렬·그룹이 식이 아닌 평범한 질의는 새 경로를 지나지 않는다.

**눈에 띄는 변화(의도한 것)**: 별칭으로 정렬한 질의가 이제 정렬됨(전에는 테이블 순서); `ORDER BY 1`이 UNION에서 정렬함, 없는 이름은 오류; 결과 열 이름은 그룹된 식이 select에 있으면 그 식의 텍스트(`MOD(id, 2)`, `g + 1`).

**정직한 한계**: ① 결과 열 이름은 아직 MySQL처럼 타이핑한 글자 그대로가 아니다(공백이 빠진 `ROUND(d)*100`·`CASE`); ② 윈도의 `OVER (ORDER BY 식)`·`PARTITION BY 식`은 열만; ③ 괄호로 감싼 UNION 피연산자 `(SELECT …) UNION (SELECT …)`는 파싱 오류; ④ 그룹에 없는 열을 select에 쓰면(`SELECT id … GROUP BY g`) MySQL의 ONLY_FULL_GROUP_BY 오류 대신 빈 값; ⑤ `ORDER BY (3)`처럼 숫자 하나는 괄호가 있어도 번호로 읽음; ⑥ `date`·`year`·`time`·`count`·`text`·`level`·`user` 같은 이름의 열은 `CREATE TABLE`이 거절함(다음 항목); ⑦ 서브쿼리의 바깥 참조가 왼쪽에 있으면 틀림(별도 항목).

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

### 10월 7일 (두 번째) — 서브쿼리의 바깥 참조: 비교의 왼쪽에 있으면 모든 행에 참이던 것(`DELETE … WHERE EXISTS (… a.id = b.a_id)`가 전부 지움), 함수·select 목록·HAVING·ON·바깥 별칭·테이블 없이 쓴 열·서브쿼리 안의 서브쿼리 — 그리고 이 점검에서 찾은 뷰 결과 캐시와 뷰의 뷰

**왜 이 항목인가**: 사용자 결정("전부 고쳐")의 R10. 앞 항목들의 점검에서 **상관 서브쿼리의 바깥 열이 비교의 왼쪽에 있으면 조용히 틀린다**는 것을 찾았고(`WHERE EXISTS (SELECT 1 FROM b WHERE a.id = b.a_id)`가 모든 행에 참), 실제로 쓰는 문장에서 `DELETE FROM a WHERE EXISTS (SELECT 1 FROM b WHERE a.id = b.a_id)`가 **5행을 전부 지웠다**(데이터 손실). 같은 뿌리의 틀림을 모두 한 번에 고쳤다.

**원인과 영향**:
- **바깥 열을 글자 모양으로 짐작**: 서브쿼리가 바깥 행을 읽는다고 보는 것은 "비교의 오른쪽에 점이 든 이름이 있을 때"뿐이었다(`has_outer_ref`/`substitute_correlated_condexpr`/`cond_may_be_substituted`). 그래서 **왼쪽**(`a.id = b.a_id`), **함수 안**(`ABS(a.id) = b.a_id`, `COALESCE(a.w, 1)`), **식**(`a.id + 0 = b.a_id`), **서브쿼리의 select 목록**(`SELECT a.v + b.k`가 NULL, `SELECT a.v`가 빈 값), **HAVING**(`HAVING COUNT(*) >= a.w`가 모든 행에 참), **조인의 ON**, **바깥 테이블의 별칭**(`FROM a x … WHERE x.id = b.a_id`), **테이블 없이 쓴 열**(`WHERE b.k = w`), **서브쿼리 안의 서브쿼리**는 바깥 값이 들어가지 않았다. 오류 없이 틀린 답(스칼라 서브쿼리는 모든 행에 같은 값: `SELECT id, (SELECT COUNT(*) … WHERE a.id = b.a_id) FROM a`가 전부 2).
- **안팎의 같은 열 이름 오인**: 값을 넣을 때 바깥 행에서 "점이 든 이름"을 이름 모양으로 찾았으므로(`get_col`의 맨 열 이름 대체 검색) 안쪽 테이블의 열도 바깥 행의 같은 이름 열로 읽힐 수 있었다.
- **UNION이 든 서브쿼리**(`x IN (SELECT … UNION SELECT …)`, `EXISTS (… UNION …)`)는 늘 거짓, select 목록의 UNION 서브쿼리는 NULL(Select가 아니면 실행하지 않았음).
- (이 점검에서 찾은 별개의 기존 버그) **뷰를 조회한 결과가 바탕 테이블이 바뀌어도 그대로**: SELECT 결과 캐시가 뷰의 이름만 의존했으므로 `INSERT INTO a …` 뒤에도 `SELECT * FROM 뷰`가 이전 답(실증: 단순 뷰·조인 뷰·서브쿼리 뷰 모두, `DROP VIEW`/다시 `CREATE VIEW` 뒤에도). 그리고 **뷰를 읽는 뷰를 만들 수 없음**(`CREATE VIEW v2 AS SELECT … FROM v1` → "Table 'v1' not found").

**수정**:
- **바인더가 이름을 정한다**(`executor_bind.cpp`): 스코프 체인에서 안쪽 테이블이 먼저(MySQL과 같음), 그다음 바깥. 바깥 열은 **몇 단계 밖인지**(`ArithExpr::Col::outer`, `ConditionValue::Literal::outer`, `SelectColumn::Column/ColumnAlias::outer`)를 적고 이름을 "테이블.열"로 바꾼다(테이블 없이 쓴 열도). 식의 열·비교의 오른쪽 이름·select 항목·함수 인자 안(날짜 함수의 단위 `INTERVAL 1 MONTH`는 제외)·HAVING·ON·LATERAL 서브쿼리를 본다. 바깥 값을 담을 수 없는 자리(집계 인자 식·`ORDER BY`·`GROUP BY`·윈도의 열)에 바깥 열을 쓰면 틀린 답 대신 오류(`Outer reference 'a.w' is not supported in 'field list'`).
- **바깥 테이블의 별칭**: 파서는 별칭을 자기 질의 안에서만 풀었으므로 서브쿼리 안의 `x.id`를 풀 길이 없었다 → `Select::table_aliases`(별칭→테이블)를 AST에 남기고(JSON 저장) 바인더가 읽는다. 안쪽이 바깥과 같은 테이블을 별칭으로 쓰면(`FROM e WHERE sal > (SELECT AVG(sal) FROM e e2 WHERE e2.dept = e.dept)`) 파서가 `e2`를 `e`로 바꿔 바깥 `e`와 구별이 안 되므로, **바깥 질의의 FROM에 있는 테이블을 별칭으로 쓰는 서브쿼리는 별칭을 유지**한다(`Select::table_alias`, 조인은 기존 `Join::alias`). select 목록의 서브쿼리는 바깥 FROM보다 먼저 파싱되므로 FROM 목록을 미리 읽는다(`scan_from_names`).
- **실행기가 표시된 참조만 바꾼다**(`executor_outer.cpp`, 새 파일): `substitute_outer`가 서브쿼리 문장의 표시된 참조를 바깥 행의 값으로 바꾼다(문장 안의 깊이를 세어 2·3단 중첩도: 한 단계 안쪽은 자기 행으로, 더 바깥은 이미 들어간 값으로). `refers_outside`가 서브쿼리가 바깥을 읽는지 알려 준다(상관이 아니면 문장당 한 번만 실행하고 캐시). 바꾼 값이 왼쪽이 되면 `열 op 값`으로 뒤집어(`a.id = b.a_id` → `b.a_id = <값>`) 인덱스·해시 경로가 그대로 쓰인다. 바깥 값이 문자열이면 따옴표 표시(열 이름과 같은 글자가 열로 읽히지 않음). 서브쿼리가 UNION/INTERSECT/EXCEPT/WITH여도 실행(`run_subquery`), select 목록·LATERAL도 같은 길.
- **서브쿼리 복사본의 캐시 범위**(`CopyScope`): 바깥 행마다 서브쿼리를 복사해 실행하는데 캐시 키가 주소라서, 복사본 안의 서브쿼리 결과가 다음 복사본(같은 주소에 다시 할당될 수 있음)에 잘못 쓰일 수 있었다 → 복사본이 도는 동안은 캐시를 새로 쓰고 끝나면 문장의 캐시를 되돌린다.
- **옛 짐작 코드 삭제**: `has_outer_ref`, `substitute_correlated_condexpr`, `substitute_arith_outer_refs`, `cond_may_be_substituted`와 그 도우미(약 160줄).
- **뷰**: 결과 캐시가 읽은 테이블 목록에 뷰 본문의 테이블(뷰의 뷰·본문 안의 서브쿼리 포함)과 뷰 자신의 이름을 넣고, `CREATE VIEW`/`DROP VIEW`가 그 이름의 캐시를 지운다. `CREATE VIEW`가 뷰를 읽는 SELECT를 받아들인다.

**검증**:
- 신규 Catch2 11케이스(580 → 591, `test_outer_references.cpp`): ① 비교의 양쪽, ② 함수·식·CASE/IF·select 목록·HAVING·ON, ③ 이름(같은 열 이름, 테이블 없이 쓴 열, 바깥 별칭, 같은 테이블을 안팎에서 쓰는 모든 별칭 조합, 안쪽 이름이 가림), ④ select 목록의 서브쿼리와 날짜 단위가 바깥 열 이름과 같을 때, ⑤ 서브쿼리 안의 서브쿼리(2·3단, 바깥에만 상관인 것, 형제 서브쿼리), ⑥ UPDATE·DELETE(데이터 손실), ⑦ 다른 타입(`'007'` vs `'7'`, 열 이름 같은 글자의 값), ⑧ 값을 담을 수 없는 자리의 오류, ⑨ 뷰·LATERAL·UNION 서브쿼리·다시 시작 뒤, ⑩ 뷰 결과가 바탕 테이블을 따라감(뷰의 뷰·DROP/CREATE), ⑪ **무작위 상관 서브쿼리를 독립 참조(3값 논리)와 비교**(EXISTS/NOT EXISTS·COUNT·SUM·IN·안의 서브쿼리 × 바깥/안쪽 별칭 × 같은 테이블 × 양쪽 위치 × 함수/식).
- **새 검증 도구 `verify_correlated.py`**(소수·문자열·NULL 열, 비교 1~3개와 OR, 별칭·같은 테이블, 2단 중첩, DELETE/UPDATE의 복사 테이블). 앞 빌드는 첫 시드에서 위반이라 도구가 버그를 본다는 것을 확인했다.
- 심은 버그 44종(바인더 — 바깥 열 판정·별칭·스코프 순서·표시 위치 넷·날짜 단위·거절·LATERAL; 값 넣기 — 깊이·탈출 판정·뒤집기·따옴표·select 항목·조건식·UNION·HAVING·ON·왼쪽·오른쪽 식; 실행 — 캐시 둘(저장과 읽기를 같이 바꿔야 오답)·복사본 캐시 범위·값 넣기 끔; 뷰 — 본문 테이블·뷰 이름·만들기/지우기 무효화·뷰의 뷰; 파서 — 별칭 유지 둘·FROM 미리 읽기) 가운데 **처음에는 5종이 살아남았고**(날짜 단위 판정 — **진짜 버그**: 바깥 테이블에 `MONTH` 열이 있으면 `INTERVAL 1 MONTH`의 단위를 열로 읽음, CASE 조건 안의 바깥 열, 복사본 캐시 범위, 뷰 만들기·지우기 무효화 각각) 테스트를 보강하고 버그를 고쳐 모두 잡게 했다. 최종 39종을 테스트가 잡고, 5종은 결과가 같은 변이(바깥 값을 넣은 뒤 비교를 뒤집지 않기 — 인덱스·해시 경로의 성능만 다름; 캐시 저장만/읽기만 바꾸기 — 둘을 같이 바꿔야 달라져 합친 변이가 잡힘; 집합 연산의 다른 쪽을 바깥 FROM으로 보기 — 별칭만 더 남을 뿐 같은 답)이다.
- Release/Debug **591 케이스/1,334,724 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과(네 가지 모두). SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,950,808), 쓰기 퍼저 40시드(43,182), 조인 퍼저 60시드(7,178), `[aggregate]` 긴 캠페인(11케이스 317,627), `[aggregate_semantics][random]`(23,151), `[aggregate_arguments][random]`(4,560), `[subquery_cardinality][random]`(5,269), `[typed_comparison][random]`(68,100 / DML 인덱스 강제 227,000), `[value_expressions][random]`(34,700 / 강제 20,820), `[sort_group][random]`(7,800 / 강제 4,160), 새 `[outer_references][random]` 150시드(12,900 / 강제 80시드 6,880), 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 16,788) 불일치 0. 새 `verify_correlated.py` 12시드 × 150문장 위반 없음(앞 빌드는 첫 시드에서 위반), `verify_sort_group`·`verify_value_expressions`·`verify_subqueries`·`verify_agg_arguments`·`verify_compare`·`verify_joins`(큰 표 포함)·`verify_writes`·`verify_null_expressions`·`verify_agg_expressions`·`verify_aggregates`·`verify_orderby_distinct`도 위반 없음.
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) **차이 0건** — 이 말뭉치의 상관 서브쿼리는 바깥 열이 비교의 오른쪽에만 있어 앞 빌드도 맞았던 모양이라 의도한 변화는 나타나지 않는다(바깥 열이 왼쪽·함수·select 목록·별칭 등에 있는 경우는 `verify_correlated.py`와 새 테스트가 직접 검증한다).
- 성능: 앞 항목 빌드와 번갈아 2라운드, 3,000행: 바깥 열이 비교의 **오른쪽**인 상관 서브쿼리는 거의 같다 — `EXISTS`(인덱스) 33.1 → 36.8 / 33.4 → 34.9ms(바깥 행마다 문장을 복사하고 값을 넣는 일이 조금 늘어 +4~11%), 스칼라 `AVG` 비교 22.3 → 22.7, select 목록의 `COUNT(*)` 20.0 → 21.1 / 19.5 → 21.0, `IN` 125.6 → 118.8, 인덱스 없는 열의 `EXISTS` 108.6 → 107.0, 비상관 스칼라 1.37 → 1.30ms. **바깥 열이 왼쪽이던 모양은 전에는 틀린 답을 2.4초(인덱스 있는 열)·1.96초(없는 열)에 냈고 이제 36.6ms·107.9ms에 맞는 답을 낸다**(값이 왼쪽이 되면 `열 op 값`으로 뒤집어 인덱스·해시 경로가 그대로 쓰임).

**눈에 띄는 변화(의도한 것)**: 바깥 열이 어디에 있든 서브쿼리가 바깥 행의 값으로 답한다(전에는 비교의 오른쪽만); 바깥 열이 값을 담을 수 없는 자리(집계 인자 식 등)에 있으면 오류; 뷰 조회가 바탕 테이블의 변경을 바로 반영; 뷰를 읽는 뷰를 만들 수 있음; `x IN (SELECT … UNION SELECT …)`가 동작.

**정직한 한계**: ① 집계 인자의 식·`ORDER BY`·`GROUP BY`·윈도의 열 안의 바깥 참조는 지원하지 않고 오류로 거절한다; ② 서브쿼리 안에서 어느 테이블의 것도 아닌 `x.열`은 여전히 오류 없이 받아들인다(다른 문장의 별칭일 수 있어서); ③ `MERGE`는 바인딩하지 않아 그 조건 안의 서브쿼리는 바깥 참조를 표시하지 못하고, (이 점검에서 찾은 기존 버그) `WHEN MATCHED AND 조건 THEN UPDATE`가 조건을 무시한다(조건은 DELETE에만 적용됨) — 둘 다 다음 항목에서 고침; ④ 함수·식 안의 서브쿼리(`(SELECT …) + 1`, `COALESCE((SELECT …), 0)`)·`IN (식 목록)`(`w IN (v, 3)`의 `v`는 글자로 읽힘)·`LIKE 식`은 파싱 오류거나 글자로 읽힘(R4c); ⑤ `CREATE OR REPLACE VIEW`는 파싱 오류(기존); ⑥ 열 이름은 대소문자를 구분한다(`SELECT MONTH FROM t`가 `month` 열을 못 찾음; MySQL은 구분 안 함, 기존); ⑦ `NOT IN (서브쿼리)`가 비어 있고 왼쪽이 NULL이면 MySQL은 참인데 이 엔진은 UNKNOWN(기존).

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

### 10월 7일 (세 번째) — MERGE의 조건: `WHEN MATCHED AND 조건 THEN UPDATE`가 조건을 무시하던 것, MERGE 조건 안의 서브쿼리가 늘 거짓이던 것

**왜 이 항목인가**: 앞 항목(서브쿼리의 바깥 참조)의 점검에서 MERGE의 조건 안에 상관 서브쿼리를 써 보다가 찾았다. `MERGE INTO tgt t USING src s ON t.id = s.id WHEN MATCHED AND t.id = 1 THEN UPDATE SET n = s.n`이 **모든 매칭 행을 갱신**했다(조건 무시, 오류 없음).

**원인과 영향**:
- **UPDATE 절의 조건을 버림**: 파서가 `WHEN MATCHED AND <조건>`의 조건을 읽고도 DELETE 절에만 넣었다(`when_matched_delete_cond`). UPDATE 절에는 조건을 둘 자리가 없어 `AND`가 있어도 모든 매칭 행에 UPDATE가 적용됐다.
- **절의 순서를 보지 않음**: 같은 행이 두 절의 조건을 다 만족하면 먼저 쓴 절이 쓰여야 하는데 DELETE가 항상 먼저였다(`WHEN MATCHED AND … THEN UPDATE … WHEN MATCHED THEN DELETE`에서 DELETE가 UPDATE를 가림).
- **조건 안의 서브쿼리는 거짓**: 조건을 서브쿼리를 모르는 평가 함수로 계산해 `EXISTS (SELECT …)`·`x > (SELECT …)`가 늘 거짓(ON도 같음).
- **바인더가 MERGE를 보지 않음**: 조건의 열 타입(숫자/문자 비교)·없는 열 오류·서브쿼리의 바깥 참조 표시가 없었고, 조건 안 서브쿼리의 테이블 이름이 한정되지 않아 `Table 'b' not found`.

**수정**:
- AST: `Merge::when_matched_update_cond`, `when_matched_update_first`(JSON에 저장, 옛 JSON은 그대로 읽힘). 파서는 UPDATE 절의 조건을 저장하고 어느 절이 먼저인지 적는다.
- 실행기: 매칭 행마다 먼저 쓴 절부터 조건이 참인 첫 절을 쓴다(조건이 거짓이거나 NULL이면 그 절은 건너뜀); ON·조건 모두 서브쿼리를 아는 평가(`eval_condexpr_with_subquery`/`matches_condition_with_subquery`)로 계산.
- 바인더: `merge()` — 대상과 원본을 한 범위로(별칭, 없으면 자기 이름 — 합친 행이 열을 그렇게 부른다), ON·두 조건·UPDATE의 값 식을 묶는다(없는 열은 오류, 타입, 서브쿼리의 바깥 참조). 한정(qualify)이 조건 안 서브쿼리의 테이블도 한정한다. 테이블 잠금 목록에 UPDATE 조건의 테이블 포함.

**검증**:
- 신규 Catch2 7케이스(591 → 598, `test_merge_conditions.cpp`): ① UPDATE 절의 조건(대상·원본·둘 다·NULL은 거짓·아무도 만족 못 함·별칭 없이·NOT MATCHED는 그대로), ② 절 순서(둘 다 만족하는 행, 순서를 뒤집음, 조건이 행을 나눔), ③ 조건·ON 안의 서브쿼리(바깥 열이 양쪽·별칭·테이블 이름, 비상관), ④ 원본의 열을 별칭 없이 서브쿼리가 부름, ⑤ 저장 프로시저 안의 MERGE와 다시 시작 뒤, ⑥ 없는 열은 오류(아무것도 바뀌지 않음), ⑦ **무작위 MERGE(절 1~2개·순서·조건 0~2개와 AND/OR·별칭 유무·NOT MATCHED 삽입)를 독립 참조(3값 논리)와 비교**.
- 심은 버그 13종(파서 둘·집행 다섯·바인더 둘·한정 둘·JSON 둘) 가운데 **처음에는 1종이 살아남았고**(JSON의 절 순서 — 프로시저 시험에 절이 하나뿐이었음) 시험을 보강해 잡았다. 최종 13종 모두 잡는다.
- Release/Debug **598 케이스/1,336,066 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과(네 가지 모두). SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,950,808), 쓰기 퍼저 40시드(43,182), 조인 퍼저 60시드(7,178), 집계·타입·식·정렬·서브쿼리 무작위 시험 긴 캠페인, 새 `[merge_conditions][random]` 150시드(22,800 / 강제 80시드 12,160), `[outer_references][random]`, 크래시 퍼저 90라운드·동시 퍼저 30라운드 불일치 0, 검증기(`verify_correlated`·`verify_sort_group`·`verify_value_expressions`·`verify_subqueries`·`verify_agg_arguments`·`verify_compare`·`verify_joins`·`verify_writes` 등) 위반 없음.
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) **차이 0건** — 이 말뭉치에는 MERGE가 없어 의도한 변화는 나타나지 않는다(MERGE는 새 테스트가 직접 검증한다).
- 성능: 앞 항목 빌드와 번갈아 2라운드, 원본 600행 × 대상 600행: UPDATE + INSERT 524 → 508ms, 조건이 있는 UPDATE 607 → 583ms로 같다(MERGE는 원본 행 × 대상 행의 중첩 루프라 행이 늘면 제곱으로 느려지는 것은 그대로이며 성능 동결 항목으로 남김).

**눈에 띄는 변화(의도한 것)**: `WHEN MATCHED AND 조건 THEN UPDATE`가 조건이 참인 행만 갱신; 두 절이 다 맞으면 먼저 쓴 절; MERGE 조건의 서브쿼리가 평가됨; MERGE 조건에 없는 열을 쓰면 오류.

**정직한 한계**: ① `WHEN NOT MATCHED AND 조건`은 파싱하지 않는다(기존); ② 원본 행 하나는 ON을 만족하는 첫 대상 행에만 적용된다(기존); ③ `UPDATE SET 열 = (SELECT …)`처럼 값에 서브쿼리를 쓰는 것은 식 안의 서브쿼리(R4c)라 안 됨; ④ 별도의 출력 검증 도구는 만들지 않았고 독립 참조는 무작위 시험이 맡는다.

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

### 10월 7일 (네 번째) — 예약어가 아닌 키워드를 이름으로 쓰기(`date`·`year`·`count`·`level`·`user`·`text`…), `begin`/`end` 열이 서버를 멈추던 것과 프로시저 본문의 `CASE … END`가 문장을 자르던 것(문장 분리기를 하나로), 집계 질의의 select 항목(ONLY_FULL_GROUP_BY), `TIMESTAMPDIFF`의 단위

**왜 이 항목인가**: 사용자 결정("전부 고쳐")의 R11. `CREATE TABLE t (id INT, date INT)`가 "Expected identifier"로 거절되었다 — MySQL에서 `date`·`time`·`year`·`count`·`text`·`level`·`user`·`timestamp`·`password`·`role`·`min`/`max`/`sum`/`avg` 같은 단어는 예약어가 아니라서 따옴표 없이 열 이름으로 쓰고, 실제 스키마(`created_date`뿐 아니라 `date`, `level`, `count`, `year`)에서 흔하다. 확인하는 과정에서 같은 뿌리의 문제 셋을 더 찾아 같이 고쳤다.

**원인과 영향**:
- **이름 자리에 키워드 토큰이 오면 거절**: 렉서는 `DATE`·`COUNT`·`LEVEL`… 을 키워드 토큰으로 만들고, 이름이 필요한 자리(`expect_ident`)는 식별자 토큰만 받았다. 식 안에서도 `COUNT`·`LENGTH`·`ABS` 같은 단어는 괄호가 없어도 함수로 읽으려다 `Expected '(' after aggregate`로 실패했고(`SELECT count FROM t`), 맨 `YEAR`는 열이 아니라 문자열 `'YEAR'`로 읽혔다(`SELECT year FROM t`가 모든 행에 `YEAR`). 이름을 받는 도우미 셋(`expect_any_name`/`expect_any_ident`/`expect_alias_ident`)은 서로 다른 단어 목록을 따로 갖고 있었다.
- **`begin`/`end`라는 열이 서버를 멈춤**: 서버·CLI·클라이언트의 문장 분리기가 `BEGIN` 단어를 만나면 프로시저 본문의 시작으로 보고 `END`를 기다렸다 — `CREATE TABLE t (id INT, begin INT)`를 보내면 서버가 입력을 더 기다리며 **응답을 주지 않았다**(연결이 멈춘 것처럼 보임).
- **본문 안의 `CASE … END`가 블록을 닫음**: `CREATE PROCEDURE … BEGIN … SET s = CASE WHEN … END; … END`에서 CASE의 `END`가 본문의 `BEGIN`을 닫은 것으로 세어져 첫 `;`에서 문장이 잘렸고, 이후 본문과 응답이 한 칸씩 어긋났다(분리기가 서버·CLI·클라이언트·테스트에 네 벌로 복사되어 있었다).
- **집계 질의의 select 항목**: `SELECT COUNT(*), 1 FROM t`가 **한 열만** 돌려주고(상수·상수의 함수가 답에서 사라짐), `SELECT COUNT(*), v FROM t`도 `v`가 사라졌다. `GROUP BY`가 있는 질의에서 그룹에 없는 열(`SELECT g, v FROM t GROUP BY g`)은 **빈 값**이었다. MySQL(ONLY_FULL_GROUP_BY가 기본)은 오류 1140/1055이고, 기본키로 묶으면(`GROUP BY id`) 나머지 열을 쓸 수 있다 — 엔진은 그 열도 빈 값으로 냈다.
- **`TIMESTAMPDIFF(MONTH, a, b)`가 NULL**: 단위가 열 이름으로 읽혀 NULL이 나왔다(우연히 `YEAR`만 됐다).

**수정**:
- **렉서가 키워드가 쓰인 단어를 남긴다**(`Token::word`, 소문자; 토큰의 동일성 비교에는 쓰지 않음). `Parser::is_plain_word`가 MySQL이 예약하지 않는 키워드 종류(타입 이름·집계 함수·스칼라 함수·`LEVEL`·`USER`·`END`·`OFFSET` …)를 정하고, 이름이 필요한 모든 자리(`expect_ident`·`expect_any_name`·`expect_any_ident`·`expect_alias_ident`)와 식의 마지막 대안이 그 단어를 이름으로 받는다. 함수 이름인 단어는 `(`가 따라올 때만 호출이고(`NOW`·`CURDATE`라고 쓴 것은 열; `CURRENT_TIMESTAMP`·`CURRENT_DATE`는 괄호 없이 값), 맨 `YEAR`는 열이다. 예약어(`SELECT`·`ORDER`·`NULL` …)는 그대로 거절(따옴표나 백틱으로 쓰면 이름). `TIMESTAMPDIFF`/`TIMESTAMPADD`의 첫 인자는 단위 단어.
- **문장 분리기를 하나로**(`engine/statement_split.hpp`, 헤더만): 서버·CLI·클라이언트(응답 개수를 세는 쪽)가 같은 함수를 쓴다. `BEGIN`/`END`가 **이름으로 쓰였는지**(앞이 `,` `(` `.` 연산자 또는 `SELECT`/`WHERE`/`BY`/`AS` …, 뒤가 `,` `)` `.` 연산자 또는 `FROM`/`AS`/타입 이름 …)를 보고, `CASE`를 세어 CASE 식의 `END`는 블록을 닫지 않으며(`END CASE`는 문장 CASE를 닫음, `THEN`/`ELSE` 바로 뒤의 `END`는 이름), 문자열의 `\\'`, 큰따옴표·백틱 이름, `--`/`#`/`/* */` 주석 안의 `;`을 건너뛴다.
- **집계 질의의 규칙**(바인더 `only_full_group_by`): 집계하거나 묶는 질의에서 집계 아닌 select 항목은 묶은 열, 묶은 식, 묶은 기본키에 종속된 열, 또는 열이 없는 식만 쓸 수 있고 아니면 MySQL과 같은 오류(`In aggregated query without GROUP BY, expression #2 of SELECT list contains nonaggregated column 'v'; this is incompatible with sql_mode=only_full_group_by` / `Expression #2 of SELECT list is not in GROUP BY clause and contains nonaggregated column …`). 저장된 뷰·프로시저 본문은 검사하지 않는다(만든 때 검사). 실행기는 `GROUP BY 기본키`의 종속 열을 그룹의 한 행에서 읽고(`SELECT * FROM a GROUP BY id`), 열이 없는 항목(`SELECT COUNT(*), 1`, `UPPER('a')`, `CASE WHEN 1 = 1 …`)을 집계 한 행의 답에 포함한다.

**검증**:
- 신규 Catch2 11케이스(598 → 609): `test_keyword_names.cpp` 3개 — ① **차분 시험**: 키워드 단어 102개(타입·집계·함수·`level`/`user`/`end`/`begin`/`month`/`name` …) × 문장 61가지(CREATE/INSERT/SELECT/별칭/WHERE/IN/BETWEEN/ORDER BY/GROUP BY/집계/식/함수 인자/CASE/IF/윈도/조인/서브쿼리/상관 서브쿼리/UNION/뷰/UPDATE/인덱스/ON DUPLICATE/DELETE/DESCRIBE/SHOW CREATE) — 같은 문장을 그 단어를 열 이름으로 쓴 표와 평범한 이름 `zz9`를 쓴 표에서 실행해 답이 같아야 한다(이름만 다름), ② 테이블·별칭·점 이름·`DATE_ADD` 단위·`TIMESTAMPDIFF`·`NOW`와 `CURRENT_TIMESTAMP`·백틱 이름, ③ 예약어는 이름이 될 수 없음; `test_server_stmt_split.cpp` 3개 — 이름으로 쓴 `begin`/`end`(이름 자리 20여 가지)·블록의 키워드·CASE 식의 `END`(프로시저·트리거 본문, `END CASE`/`END IF`/`END WHILE`/`END LOOP`/`END REPEAT`, 중첩 블록)·주석·문자열·백틱·트랜잭션 `BEGIN`; `test_group_by_rules.cpp` 5개 — 열 없는 항목, 오류들(번호·메시지), 허용되는 것들(묶은 열·식·위치·별칭·서브쿼리·윈도), 기본키 종속 열의 값, 저장된 뷰.
- 심은 버그 44종(렉서·이름을 받는 자리·단어 목록·함수 이름의 괄호·식·select 목록·문장 분리기·집계 규칙) 가운데 **처음에는 4종이 살아남았고**(`DATE_ADD`/`DATE_SUB` 이름의 괄호 판정, 분리기가 앞 문맥만으로/뒤 문맥만으로 이름을 알아보는 두 경우, `THEN end`) 시험을 보강해 잡았다. 최종 41종을 테스트가 잡고, 3종은 결과가 같은 변이(select 목록에서 함수 이름 단어를 `Column`이 아니라 `Expr{Col}`로 읽는 경로 — 같은 답; 바깥 질의의 열 이름은 안쪽 범위에서 풀리지 않아 규칙이 어차피 건너뜀; 검사하는 문장에서는 닿지 않는 상수 항목의 조건 판정)이다.
- Release/Debug **609 케이스/1,342,651 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과(네 가지 모두). SELECT 차분 퍼저 150시드(1,819,528 assertions), DML 퍼저 80시드(1,950,808), 쓰기 퍼저 40시드(43,182), 조인 퍼저 60시드(7,178), 집계·타입·식·정렬·서브쿼리·MERGE·상관 서브쿼리 무작위 시험 긴 캠페인, 크래시 퍼저 90라운드·동시 퍼저 30라운드(확인한 확정 행 16,696) 불일치 0, 검증기(`verify_correlated`·`verify_sort_group`·`verify_value_expressions`·`verify_subqueries`·`verify_agg_arguments`·`verify_compare`·`verify_joins`·`verify_writes`·`verify_aggregates`·`verify_agg_expressions`·`verify_orderby_distinct` 등) 위반 없음. 기존 `test_full.sql`(319문장)·`test_full-ver2.sql`(360응답)를 서버에 스트림으로 보내 앞 빌드와 응답을 비교: 차이 없음(새 분리기와 집계 규칙이 이 스크립트들에는 영향 없음).
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) **차이 0건** — 이 말뭉치의 집계 질의는 모두 묶은 열만 고르므로 의도한 변화(그룹에 없는 열 오류 등)는 나타나지 않는다(새 테스트가 직접 검증한다).
- 성능: 앞 항목 빌드와 번갈아 2라운드, 5만 행: `COUNT(*)` 43.4 → 42.9 / 43.4 → 45.0ms, `SUM`/`AVG` 56.9 → 56.1 / 56.1 → 60.9, `GROUP BY` + 4집계 69.7 → 70.0 / 70.7 → 73.3, `HAVING AVG` 58.0 → 59.7 / 65.0 → 62.6, `SUM(v) + 1` 53.0 → 53.0 / 56.3 → 55.9 — 두 빌드가 같다(±5% 안의 측정 잡음). 그룹마다 한 행을 복사하는 일은 select에 그룹에 없는 열(기본키로 묶은 질의의 종속 열)이 있을 때만 하고, 평범한 `GROUP BY`는 새 경로를 지나지 않는다.

**눈에 띄는 변화(의도한 것)**: `date`·`year`·`count`·`level`·`text` … 를 따옴표 없이 열·테이블·별칭 이름으로 쓸 수 있다; `begin`/`end` 열이 서버를 멈추지 않는다; 프로시저·트리거 본문의 CASE 식이 본문을 자르지 않는다; `SELECT COUNT(*), 1` 같은 질의에 상수 항목이 나온다; **그룹에 없는 열을 select에 쓰면 오류**(전에는 빈 값/누락 — MySQL 8 기본과 같음; 기본키로 묶으면 나머지 열을 쓸 수 있음); `TIMESTAMPDIFF`가 모든 단위(`MONTH`/`DAY`/…)를 지원.

**정직한 한계**: ① `FROM t date`처럼 `AS` 없는 별칭이 키워드인 것은 받지 않는다(`AS date`는 됨); ② MySQL 8이 예약하는 단어(`rank`·`row`·`rows`·`key`·`index`·`check`·`default`…)는 `CREATE TABLE`에서 열 이름으로 받지 않는다(MySQL과 같음; 식 안의 `rank`/`row`는 예전처럼 열로 읽힘); ③ 분리기는 SQL을 이해하는 것이 아니라 단어와 문맥으로 `BEGIN`/`END`가 이름인지 추정한다 — `THEN`/`ELSE` 바로 뒤의 `BEGIN`은 블록으로 읽고, 앞뒤에 단서가 전혀 없는 이름은 트랜잭션 `BEGIN;`과 구별할 수 없다; ④ 함수적 종속은 기본키 하나로만 판정한다(고유 키·`WHERE x = 상수`로 인한 종속은 오류로 본다); ⑤ ONLY_FULL_GROUP_BY는 select 항목만 본다(`ORDER BY`·`HAVING`의 묶이지 않은 열은 검사하지 않음); ⑥ 열 이름은 대소문자를 구분한다(기존).

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

### 10월 7일 (다섯 번째) — 식 안의 서브쿼리(`(SELECT …) + 1`, `COALESCE((SELECT …), 0)`, `UPDATE … SET v = (SELECT …)`, `INSERT … VALUES (…, (SELECT …))`), `IN (식 목록)`, `LIKE 식`, 값 자리의 조건이 서브쿼리를 답하지 못하던 것(`CASE WHEN EXISTS (…)`, `JOIN … ON x IN (SELECT …)`), 프로시저 반복문에서 서브쿼리 답이 묵은 채 남던 것

**왜 이 항목인가**: 사용자 결정("전부 고쳐")의 R4c. `SELECT (SELECT MAX(k) FROM b) + 1`, `WHERE x > (SELECT AVG(v) FROM t) * 1.1`, `WHERE a IN (b, c + 1)`, `WHERE s LIKE CONCAT(prefix, '%')`, `UPDATE t SET v = (SELECT …)`이 파싱 오류였다. 확인하는 과정에서 **오류 없이 틀린 답**을 내는 같은 뿌리의 문제를 더 찾아 같이 고쳤다. (사용자가 크레딧 사정으로 "소규모 작업만 완료하고 문서·커밋"을 요청해, 아래 검증 목록은 이 항목의 것만 적었다 — 실행하지 않은 것은 맨 아래에 적는다.)

**원인과 영향**:
- **서브쿼리는 식의 항이 아니었다**: `(SELECT …)`는 select 항목의 전부이거나 비교의 오른쪽 전부일 때만 파서가 받았다. 산술의 피연산자·함수 인자·CASE 결과·BETWEEN 한계·`SET`/`VALUES`의 값에 쓰면 파싱 오류. `IN (a, b)`는 숫자·문자열·NULL·변수만 받았고, `LIKE`의 패턴은 문자열 하나만 받았다.
- **값 자리의 조건이 서브쿼리를 답하지 못함 (오류 없이 틀림)**: WHERE·HAVING의 서브쿼리 조건은 서브쿼리용 경로로 평가되지만, 조건이 값으로만 쓰이는 자리 — `CASE WHEN EXISTS (…) THEN …`, `IF(x IN (SELECT …), 1, 0)`, select 항목 `x IN (SELECT …)`, **JOIN의 ON**(`ON b.a_id = a.id AND b.k > (SELECT AVG(k) FROM b)`), 다중 테이블 UPDATE/DELETE의 ON — 의 평가기는 서브쿼리 조건을 **그냥 거짓**으로 돌려주었다(`CASE WHEN EXISTS`는 늘 ELSE, 조인은 0행).
- **프로시저의 `WHILE (SELECT …)`·`IF EXISTS (…)`·`SET v = (SELECT …) + 1`**: 같은 이유로 조건이 거짓/식이 오류.
- **묵은 서브쿼리 답 (오류 없이 틀림)**: 서브쿼리 답의 캐시는 서브쿼리 노드의 **주소**를 열쇠로 하고 최상위 문장이 시작할 때만 지웠다. 프로시저 본문은 반복마다 문장을 복사해 같은 주소에 다시 만들 수 있어서, 반복 안의 `SELECT COUNT(*) INTO c FROM a WHERE id IN (SELECT a_id FROM b WHERE k >= i * 4)`가 `i`가 바뀌어도(또는 본문의 `INSERT INTO b`가 b를 바꿔도) **첫 반복의 답**을 읽었다(시험 프로시저: 기대 211/334가 222/333).
- 부수로 찾은 것: 왼쪽이 서브쿼리이고 오른쪽이 `IN (SELECT …)`인 조건에서 왼쪽 서브쿼리의 테이블에 데이터베이스 접두어가 붙지 않아 `Table 'b' not found`; `INSERT … VALUES`의 서브쿼리는 열 개수/존재하지 않는 열을 검사하지 않음(`(SELECT k, id …)`가 첫 열로 들어가고 `nosuch`는 "Incorrect integer value"); 함수 본문(`CREATE FUNCTION f(x) … RETURN x + (SELECT …)`)은 오류를 삼키고 엉뚱한 값.

**수정**:
- **식의 항 `ArithExpr::Subquery`**: 파서가 `(SELECT …)`를 식의 항으로 받는다(뒤에 연산자가 이어지면 되감아 식으로 다시 읽음 — select 항목·비교의 오른쪽·`SET @x =`·프로시저의 `SET v =`). 바인더가 안쪽 질의를 중첩 질의로 묶고(바깥 참조 표시, 열 둘 이상 = 1241, 없는 열/테이블 오류) 그 한 열의 종류를 값의 종류로 삼는다. 값은 행이 없으면 NULL, 둘 이상이면 1242. 바깥 열을 쓰지 않는 서브쿼리는 **문장마다 한 번**(그리고 그 문장 사이에 쓰기 문장이 끝나면 다시) 답하고 노드에 보관, 바깥 열을 쓰면 행마다. 정적 평가기가 서브쿼리를 실행할 수 있도록 실행 중인 문장을 스레드-로컬 `StatementContext`로 알린다(`StatementScope`: 문장 번호·쓰기 번호).
- **`IN (식 목록)`**: 목록에 숫자·문자열·NULL·변수 말고 다른 것이 있으면 `x = a OR x = b …`(`NOT IN`은 `x <> a AND x <> b …`)로 푼다 — NULL의 3값 논리가 같다. 값만 있는 목록은 전과 같다. `LIKE`·`NOT LIKE`·`REGEXP`의 패턴도 식(함수·열·서브쿼리)을 받는다(따옴표 문자열은 열 이름이 아니라 문자열로 유지).
- **값 자리의 서브쿼리 조건**: 평가기가 서브쿼리 조건을 만나면 실행 중인 문장의 서브쿼리용 경로로 답한다(WHERE의 것과 같은 코드·캐시) — `CASE WHEN`·`IF()`·select 항목·JOIN의 ON(해시 조인의 검증 포함; 같은 평가기를 쓰는 다중 테이블 UPDATE/DELETE의 ON도 같은 경로이지만 따로 시험하지는 않았다). 컨텍스트가 없는 곳에서 만나면 거짓이 아니라 오류. 읽는 테이블을 잠금 집합·읽기 전용 판정·병렬 스캔 제외(`cond_has_any_subquery`)에 포함.
- **독립적으로 평가되는 식**(프로시저의 `SET`/`IF`/`WHILE`/`UNTIL`, `SET @x = …`, 트리거의 `SET NEW.x = …`, `CALL p((SELECT …))`, `INSERT`의 값, 함수 본문): 데이터베이스 접두어를 붙이고 `SELECT <식> FROM _dual_`의 항목으로 묶어 같은 바인더를 거친다(`bind_expression`/`bind_condition`). 함수 본문에서 난 `StatementError`는 삼키지 않는다.
- **묵은 답 막기**: 서브쿼리 답 캐시(IN 집합·스칼라·EXISTS·상관 여부)는 **그 답을 만든 문장 번호와 쓰기 번호**가 같을 때만 쓴다(`sync_subquery_caches`; 복사본 실행 `CopyScope`는 자기 번호를 보존). 문장이 바뀌거나 쓰기 문장이 끝나면 버린다.
- **고치지 않고 오류로 거절하는 것**: 텍스트로 보관되는 자리 — `ORDER BY`·`GROUP BY` 식, 집계 인자 안(`SUM(v + (SELECT …))`) — 의 서브쿼리는 `A subquery inside this expression is not supported`(틀린 답 대신 오류).

**검증**:
- 신규 Catch2 10케이스(609 → 619, `test_expression_subqueries.cpp`, 1,481 assertions): 식 안 서브쿼리(산술·함수·비교 양쪽·BETWEEN·중첩·상관·집계/HAVING), 값 자리의 조건(`CASE WHEN EXISTS`/`IN`/비교, `IF`, select 항목, JOIN ON 3종과 LEFT JOIN), `IN (식 목록)`(NULL 3값 논리 포함), LIKE/REGEXP 식 패턴(따옴표 문자열은 문자열), INSERT/UPDATE/DELETE/트리거/`SET @x`, 오류들(1241·1242·없는 열·없는 테이블·원자성), 저장 루틴·뷰·재시작(루프의 묵은 답 3종: 쓰기가 있는 루프 334, 변수가 바뀌는 루프 411, `WHILE EXISTS`/`WHILE i < (SELECT COUNT(*) …)`), 12,000행 스캔(병렬 경로 제외), **무작위 시험**(독립 기준 모델: 8행 a, 0~5행 b, 깊이 2의 식 — `+ - *`·`COALESCE`·열·상수·서브쿼리 6종(상관 3종 포함); 값, 비교, `IN`/`NOT IN` 세 식, `UPDATE SET`을 기준과 비교; 기본 6시드, 80시드 캠페인 16,317 assertions 통과).
- Release **619 케이스/1,344,132 assertions**(기본 설정 1,344,132, `RUSQL_DML_INDEX_MIN_ROWS=0` 1,344,132)를 통과 — 기존 609케이스도 그대로 통과(기존 시험은 한 줄도 바꾸지 않음). (Debug 회귀·심은 버그·차분·성능 비교는 이번 항목에서 하지 않았다.)

**눈에 띄는 변화(의도한 것)**: 식 안의 서브쿼리, `IN (식)`, `LIKE 식`이 동작; `CASE WHEN EXISTS (…)`·`IF(x IN (SELECT …), …)`·`JOIN … ON x > (SELECT …)`가 맞는 답(전에는 늘 거짓/0행); 프로시저 반복문의 서브쿼리가 매 반복 현재 값으로 답; 프로시저의 `WHILE (SELECT …)`·`IF EXISTS`·`SET v = (SELECT …) + 1`; `INSERT … VALUES`의 서브쿼리가 열 개수·없는 열을 검사; 결과 열 이름은 `(subquery)+1`처럼 서브쿼리를 `(subquery)`로 표시(MySQL은 입력한 글자 그대로 — 기존의 "열 이름이 입력한 글자가 아님" 한계).

**정직한 한계**: ① `ORDER BY`·`GROUP BY`·집계 인자 안의 서브쿼리는 거절(위). ② `SET @x = (SELECT …)`·프로시저의 `SET v = (SELECT …)`(뒤에 연산자가 없는 것)는 예전의 `SELECT … INTO` 경로라 행이 여럿이어도 오류 없이 첫 행(MySQL은 1242) — 남김. ③ 서브쿼리가 있는 `UPDATE`는 같은 테이블을 읽어도 바뀌기 전의 행을 읽는다(MySQL은 1093 오류). ④ 문장 하나 안에서 트리거가 쓰기를 하면 바깥 문장의 바깥 열을 쓰지 않는 서브쿼리는 그 뒤 행부터 새 데이터로 다시 답한다(MySQL은 한 번 답해 보관할 수 있어 다를 수 있음 — 정의되지 않은 경우). ⑤ `SELECT 1 UNION SELECT 2`(FROM 없는 SELECT에 UNION)는 파싱 오류(기존; 이번 점검에서 발견, 고치지 않음). ⑥ 이번 항목은 아래 "실행하지 않은 것"을 건너뛰었다.

**실행하지 않은 것 (크레딧 사정)**: Debug 빌드 회귀, 심은 버그(mutant) 시험, 빌드 간 차분(`diff_builds.py`), 성능 비교, 크래시·동시 퍼저와 기존 검증기 재실행. 위 시험이 모두 통과해도 이 항목은 이전 항목들보다 **적게 검증**되었다.

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

### 10월 11일 — 모호한 열 이름(1052), 하위 질의 안의 없는 한정자, `x IN (빈 서브쿼리)`, 집계 질의의 `HAVING`·`ORDER BY`(ONLY_FULL_GROUP_BY), 별칭을 쓴 UPDATE/DELETE의 상관 서브쿼리, `SET @x = (SELECT …)`의 행·열 수, 프로시저 `IF`/`WHILE` 조건 안의 변수, `CREATE OR REPLACE VIEW`·FROM 없는 SELECT의 UNION·`DIV`·지수 표기

**왜 이 항목인가**: 사용자 결정("계속 이어서 고치고 같은 진행방식으로")에 따라 앞 항목들에서 남긴 목록 가운데 **오류 없이 틀린 답을 내는 것**을 먼저, 그 다음 파싱 오류로 막히는 것을 골랐다. 앞 항목의 점검에서 이미 찾아 둔 것들이다: `SELECT id FROM a JOIN b …`가 첫 테이블의 값을 조용히 돌려주고, 서브쿼리의 `zz.id`가 어느 테이블의 것도 아니어도 받아들여지고(`EXISTS (… zz.id = 1)`이 모든 행에 참), `v NOT IN (빈 서브쿼리)`가 NULL 행을 빼고, `HAVING v > 3`(v는 묶이지 않음)이 0행, `SET @x = (SELECT k FROM b)`가 여러 행이어도 첫 행.

**원인과 영향**:
- **모호한 열 이름**: 이름 하나를 둘 이상의 테이블이 가져도 엔진은 오류 없이 첫 테이블의 값을 읽었다(`SELECT id FROM a JOIN b …`, `WHERE id = 1`, `ORDER BY id`, `GROUP BY id`, `COUNT(id)`, ON의 `a_id = id`). MySQL은 1052 "Column 'id' in field list is ambiguous".
- **하위 질의 안의 없는 한정자**: 하위 질의에서 어느 테이블·별칭도 아닌 `zz.id`는 "바깥 질의의 별칭일 수 있다"는 이유로 늘 받아들여졌다 → `EXISTS (SELECT 1 FROM b WHERE zz.id = 1)`이 모든 행에서 참.
- **빈 서브쿼리의 IN**: `x IN (SELECT …)`·`NOT IN`은 왼쪽이 NULL이면 서브쿼리를 보기도 전에 UNKNOWN을 돌려줬다. SQL에서 `NULL IN (빈 집합)`은 거짓, `NULL NOT IN (빈 집합)`은 참(`DELETE … WHERE v NOT IN (빈 서브쿼리)`가 NULL 행을 남김).
- **집계 질의의 `HAVING`·`ORDER BY`**: 앞 항목의 ONLY_FULL_GROUP_BY는 select 항목만 봤다. `GROUP BY g HAVING v > 3`은 0행, `GROUP BY g ORDER BY v`는 정렬 없이 답했다(MySQL 1463 / 1055).
- **별칭을 쓴 UPDATE·DELETE의 상관 서브쿼리 (앞 항목 이전부터)**: UPDATE·DELETE는 테이블 별칭을 WHERE에서만 풀고 서브쿼리 안의 `x.id`는 남겨서, `UPDATE a x SET w = 99 WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = x.id)`가 **모든 행**을 바꾸고 `UPDATE a x SET w = (SELECT COUNT(*) … x.id)`는 모든 행에 같은 값, `DELETE FROM a x WHERE EXISTS (… x.id AND b.k > 5)`는 0행을 지웠다. `DELETE x FROM a x JOIN …`은 별칭 `x`를 테이블로 찾아 `Table 'd.x' not found`.
- **`SET @x = (SELECT …)` / 프로시저의 `SET v = (SELECT …)`**: 예전 `SELECT … INTO` 경로라 여러 행이면 오류 없이 첫 행, 여러 열이어도 첫 열(MySQL 1242 / 1241); 서브쿼리 안의 `@c` 변수는 풀리지 않아 NULL(`SET @m = (SELECT MAX(v) FROM t WHERE id < @c)`).
- **프로시저 `IF`/`WHILE`/`UNTIL` 조건 안의 서브쿼리에서 쓴 변수**: 조건은 반복마다 다시 평가되는데 서브쿼리 안의 변수가 풀리지 않아 `WHILE (SELECT COUNT(*) FROM b WHERE k > i) > 0`이 바로 끝났다.
- **파싱 오류로 막히던 것**: `CREATE OR REPLACE VIEW`, `SELECT 1 UNION SELECT 2`(FROM 없는 SELECT에 집합 연산), `a DIV b`, `1e3`.

**수정**:
- **모호한 이름 검사**(바인더 `ambiguous`): 한정자 없는 열을 이름을 가진 테이블이 둘 이상인 가장 안쪽 질의에서 찾으면 오류 `Column 'id' in <절> is ambiguous`(field list / where clause / on clause / order clause / group statement / having clause; 비교 오른쪽의 맨 단어도). `USING`·`NATURAL JOIN`이 하나로 합친 열은 모호하지 않고, `ORDER BY`·`HAVING`은 select 목록이 부르는 이름(`SELECT a.id … ORDER BY id`, `AS id`)을 먼저 읽는다. 안쪽 질의의 자기 테이블이 바깥보다 먼저.
- **없는 한정자**: 한정자가 어느 질의의 테이블·별칭도 아니면 서브쿼리 안에서도 `Unknown column`. 바깥 질의의 별칭은 그 질의의 `table_aliases`로 확인하고 그 별칭이 가리키는 테이블에 열이 있는지도 본다.
- **빈 서브쿼리의 IN**: 서브쿼리를 먼저 보고, 행이 없으면 왼쪽이 NULL이어도 `IN`은 거짓·`NOT IN`은 참, 행이 있으면 NULL 왼쪽은 UNKNOWN.
- **`HAVING`·`ORDER BY`의 묶기 규칙**: `Non-grouping field 'v' is used in HAVING clause`, `Expression #N of ORDER BY clause is not in GROUP BY clause and contains nonaggregated column …`(집계만 있고 `GROUP BY`가 없으면 `In aggregated query without GROUP BY, expression #N of ORDER BY contains …`). select 목록의 별칭, 묶은 열, 묶은 기본키에 딸린 열은 허용.
- **별칭 보존**: `Statement::Update`/`Delete`/`MultiUpdate`/`MultiDelete`가 `table_aliases`(별칭 → 테이블)를 저장하고(JSON에도; 옛 저장본은 비어 있음) 바인더의 쓰기 범위가 읽어 서브쿼리의 `x.id`를 바깥 열로 표시한다 — `SELECT`에서 하던 방식 그대로. `DELETE x FROM a x JOIN …`의 대상 목록도 별칭을 테이블로 푼다.
- **`SET @x = (SELECT …)`**: 파서가 `SelectInto` 특례를 없애고 일반 식으로 읽어 스칼라 서브쿼리(행 없음 NULL, 여럿 1242, 열 여럿 1241)가 된다. 변수 치환이 `SetUserVar`·`ProcSet`의 식 안 서브쿼리에도 들어가고, `IF`/`WHILE`/`UNTIL` 조건은 서브쿼리가 있으면 평가할 때마다 현재 변수 값으로 치환한 복사본을 쓴다(`proc_condition`).
- **구문**: `CREATE OR REPLACE VIEW`(같은 이름의 뷰를 대체 — 평범한 `CREATE VIEW`도 전부터 그랬다); FROM 없는 SELECT 뒤의 `UNION`/`INTERSECT`/`EXCEPT`(집합 연산 처리를 람다로 빼서 두 경우가 같이 씀); `a DIV b` = `TRUNCATE(a / b, 0)`(0으로 나누면 NULL, 다음 토큰이 값을 시작할 때만 연산자라 `div`라는 열·별칭은 그대로); `1e3`·`2.5E-1`은 쓴 수 그대로 펼쳐 `1000`·`0.25`.

**검증**:
- 신규 Catch2 7케이스(`test_name_resolution.cpp`; 이전 항목의 `test_expression_subqueries.cpp`에 함수 본문의 실패·`CALL echo((SELECT …) + 1)`을 더함): 모호한 이름(모든 절, 비교 오른쪽, 자기 조인, USING/NATURAL, 안쪽 질의 우선, 안 모호한 것들), 하위 질의의 없는 한정자·바깥 별칭, **별칭을 쓴 UPDATE/DELETE**(WHERE·SET·다중 테이블·`DELETE x FROM a x JOIN`·안쪽 별칭 가림·프로시저 안), 빈 서브쿼리의 IN·NOT IN(캐시를 타는 경로와 안 타는 경로, 상관, DELETE), HAVING/ORDER BY 묶기 규칙, `SET @x`/프로시저 `SET`/`IF`/`WHILE`의 서브쿼리와 변수, 새 구문.
- 기존 시험 두 곳이 새 규칙에 걸렸다: `test_query_paths.cpp`의 무작위 조인 질의가 `u`와 `t`가 둘 다 가진 `id`·`grp`를 한정자 없이 썼다(→ `t.id`·`t.grp`; 그 질의는 MySQL이면 오류); `test_stored_routines.cpp`의 `SET @m = (SELECT … WHERE id < @c)`는 서브쿼리 안 변수 치환이 빠져 있었다(→ 치환 추가). `verify_subqueries.py`의 기준 모델이 `NULL NOT IN (빈 집합)`을 UNKNOWN으로 두고 있었다(→ 참으로 고침).
- 심은 버그 49종(모호한 이름 검사의 각 분기·USING/NATURAL·select 목록 이름·빈 서브쿼리 IN의 분기들·캐시 무효화·값 자리 조건의 평가·변수 치환·바인딩 호출들·별칭 보존·구문 파서) 가운데 **처음에는 4종이 살아남았고**(`amb_open`: LATERAL 같은 열을 알 수 없는 테이블을 모호함 판정에 세는 경우, `amb_aliases`: HAVING이 select 목록의 별칭을 먼저 읽는 것, `like_quoted`: LIKE의 따옴표 패턴이 열 이름으로 읽히지 않는 것, `sync_statement_only`) 앞의 셋은 시험을 보강해 잡았다. 최종 48종을 테스트가 잡고, 1종(`sync_statement_only`: 서브쿼리 답 캐시를 문장 번호로 버리는 검사를 빼도 같은 결과 — 프로시저의 `SET`·`SELECT … INTO`가 쓰기 문장으로 세어져 쓰기 번호가 먼저 캐시를 비우므로 지금의 시험 범위에서는 구별할 수 없다; 문장 번호 검사는 주소가 재사용되는 읽기 전용 반복에 대비해 남김)은 결과가 같은 변이다. (앞 항목에서 시험하지 못한 식 안의 서브쿼리 코드의 변이 중 이 캠페인이 닿는 것 — `IN (식 목록)`의 `=`/`<>`·AND/OR 선택, LIKE 따옴표, 값 자리 조건의 평가, 컨텍스트 바인딩, 변수 치환, 비교 오른쪽의 되감기 — 도 모두 잡혔다.)
- Release/Debug **626 케이스/1,344,384 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과(네 가지 모두). 시간에 민감한 갭 락 시험 한 건(`elapsed_ms >= 100`)이 Debug 빌드와 동시에 돌릴 때 한 번 96ms로 실패했고, 기계가 조용할 때 5회 연속 통과 + 강제 설정 전체 재실행 통과. 무작위 시험 긴 캠페인(`[random]` 40시드 기본·강제 각 100,022 assertions, `[fuzz]` 30시드 1,422,936 assertions) 통과, 크래시 퍼저 60라운드·동시 퍼저 20라운드(확인한 확정 행 11,151) 불일치 0, 검증기 12종(`verify_agg_arguments`·`verify_agg_expressions`·`verify_aggregates`·`verify_compare`·`verify_correlated`·`verify_joins`·`verify_null_expressions`·`verify_orderby_distinct`·`verify_sort_group`·`verify_subqueries`·`verify_value_expressions`·`verify_writes`) 위반 없음.
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) 차이 63건 **전부 의도한 새 오류**다 — 모호한 이름 39건(옛 빌드는 첫 테이블의 값으로 답함), 집계 질의의 `ORDER BY` 묶이지 않은 열 24건; 그 밖의 차이 0건. 저장소의 `test_full.sql`(319문장)·`test_full-ver2.sql`(359문장)을 두 빌드에 스트림으로 보내 비교: 시각·연결 번호 말고 차이 없음(새 규칙에 걸린 문장 없음).
- 성능: 앞 항목 빌드와 번갈아 2라운드, 5만 행(결과 캐시를 피하려고 질의마다 다른 상수): 기본키 점 조회 0.10 → 0.10ms, 조인 33.8 → 33.3 / 33.6 → 33.6, `GROUP BY` + 3집계 41.7 → 35.6 / 40.8 → 48.0, `HAVING` + `ORDER BY` 40.3 → 38.3 / 46.2 → 40.6, 스캔 24.7 → 25.0 / 26.2 → 24.1, `IN (서브쿼리)` 19.3 → 24.3 / 18.3 → 18.3, 스칼라 서브쿼리 94.7 → 88.5 / 87.1 → 89.8, 별칭 UPDATE 46.8 → 46.7 / 46.6 → 47.2 — 두 빌드가 같다(±15% 안의 측정 잡음).

**눈에 띄는 변화(의도한 것)**: 두 테이블이 같은 이름의 열을 가지면 한정자 없이 쓸 수 없다(MySQL과 같음 — 스크립트가 `SELECT id FROM a JOIN b …`를 쓰고 있었다면 오류가 난다); 서브쿼리에서 없는 한정자가 오류; `NOT IN (빈 서브쿼리)`가 NULL 행도 고름; 집계 질의의 `HAVING`/`ORDER BY`에 묶이지 않은 열이 오류; 별칭을 쓴 UPDATE/DELETE의 `EXISTS (… x.id)`가 맞는 행만 바꿈; `SET @x = (SELECT …)`가 여러 행/열이면 오류; `CREATE OR REPLACE VIEW`, `SELECT 1 UNION SELECT 2`, `7 DIV 2`, `1e3`이 동작.

**정직한 한계**: ① ON의 모호한 이름 검사는 그 JOIN에 이어 나중에 붙는 테이블까지 한 범위로 본다(앞선 JOIN의 ON이 뒤의 테이블에만 있는 열을 쓰면 MySQL은 오류가 아닌데 여기서는 모호하다고 할 수 있음 — 드문 모양); ② 함수 인자 안의 열(`COUNT`·`SUM` 밖의 일반 함수)은 이전처럼 존재·모호성을 확인하지 않는다; ③ HAVING 메시지는 MySQL 8의 1463, 다른 메시지도 글자까지 같지는 않을 수 있다; ④ `CREATE VIEW`가 같은 이름이 있어도 오류 없이 대체한다(MySQL은 `CREATE VIEW`에서 1050) — `OR REPLACE`와 같은 동작으로 둠; ⑤ 괄호로 감싼 UNION 피연산자 `(SELECT …) UNION (SELECT …)`, `COUNT(DISTINCT a, b)`, `FROM t date`(AS 없는 키워드 별칭)는 여전히 파싱 오류; ⑥ 앞 항목의 한계(`ORDER BY`·`GROUP BY` 식·집계 인자 안의 서브쿼리 거절)는 그대로.

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

### 10월 12일 — 실제 MySQL 8.4와 같은 문장을 돌려 비교: 모르는 함수가 글자를 돌려주던 것, 함수 약 140종, DECIMAL/DOUBLE 산술, 복합 UNIQUE 키(첫 열만 보던 것·재시작하면 사라지던 것), 파티션 테이블이 재시작 뒤 0행이던 것

**왜 이 항목인가**: 사용자 결정("일단 그럼 계속 수정하는 게 낫죠. 버그는 치명적인 거 아닌가요?")에 따라, 이번에는 지금까지의 "코드에서 의심되는 곳을 찾아 고치는" 방식 대신 **실제 MySQL 8.4.9에 같은 문장을 보내 답을 비교**했다(`vs_mysql.py`: 같은 스키마·데이터·문장을 RuSQL과 MySQL에 넣고 행 집합이 다른 문장을 목록으로 뽑음; 문장 약 350개). 코드를 읽어서는 보이지 않던 것이 한꺼번에 나왔다 — 아래가 그 가운데 "오류 없이 틀린 답"이거나 "MySQL이 받는데 여기서는 막히는" 것이다.

**원인과 영향**:
- **모르는 함수가 오류 없이 글자를 돌려줌**: `SELECT NOSUCH(1)`이 `NOSUCH(1)`이라는 글자를, `SELECT BIT_COUNT(7)`처럼 아직 없는 함수도 같은 식으로 "답"했다. MySQL은 1305 `FUNCTION db.nosuch does not exist`.
- **빠진 함수·틀린 함수**: 약 140개를 MySQL 규칙대로 다시 만들거나 새로 만들었다(`builtin_functions.cpp`의 함수 표) — 문자열(`SUBSTRING`의 음수 위치, `LEFT`/`RIGHT`/`INSERT`/`FIELD`/`ELT`/`FIND_IN_SET`/`SUBSTRING_INDEX`/`CONV`/`TO_BASE64`…), 수학(`ROUND`/`TRUNCATE`/`MOD`/`POW`/`LOG(b, x)`/`ATAN2`/`COT`…, 정수·문자열 인자), 날짜(`DATE_ADD`가 월·년·시간 단위를 무시하거나 월말을 넘기던 것 — `2024-01-31 + 1 MONTH`는 `2024-02-29`; `HOUR`/`MINUTE`/`SECOND`; `TIMESTAMPDIFF(MONTH, …)`; `DATE_FORMAT`; `LAST_DAY`/`MAKEDATE`/`TO_DAYS`/`WEEK`/`EXTRACT`/`STR_TO_DATE`…), `CAST`(`CAST(3.9 AS SIGNED)`는 4, 문자열은 앞쪽 숫자까지), JSON(`JSON_OBJECT`/`ARRAY`/`SET`/`INSERT`/`REPLACE`/`REMOVE`/`CONTAINS`/`KEYS`/`LENGTH`/`TYPE`/`VALID`), 해시(`SHA1`/`SHA2`/`CRC32`/`SOUNDEX`), 연산자(`<=>`, `XOR`, `DIV`, `<<`, `>>`, `&`, `|`, `^`, `~`).
- **산술이 double이던 것**: `10 / 3 * 3`이 `9.999999`, 큰 정수의 합·곱이 조용히 double로 넘어가 끝자리가 틀림, 문자열 `'12abc' + 1`의 규칙, BIGINT 넘침이 오류가 아님. MySQL은 정확한 DECIMAL 산술(몫은 피제수의 소수 자리 + 4자리, 계산 중에는 9자리까지 들고 가다 식 맨 위에서 한 번 반올림)이고 BIGINT 넘침은 오류(1690).
- **DOUBLE/FLOAT 열은 부동소수점**: 정확한 십진수로 계산하면 `d / 3 * 3`이 `3.0001`, `ROUND(2.5)`가 3(MySQL은 DOUBLE이면 짝수쪽 반올림 2, DECIMAL이면 3), `STDDEV`·`VARIANCE`가 4자리로 잘림(MySQL은 DOUBLE 전체 자리).
- **몫의 자리수**: `ROUND(8 / 59.27, 2)`가 0.14(몫이 보여 주는 `0.1350`을 다시 반올림). MySQL은 0.13 — 몫은 9자리(`0.134975535`)를 들고 있고 4자리는 보여 줄 때만. `FLOOR(10 / 3 * 3)`은 9, `8 / 59.27 >= 0.135`는 거짓, DECIMAL(10,2) 열에 넣으면 0.13, VARCHAR 열에는 `0.134975535`가 저장된다.
- **복합 UNIQUE 키가 첫 열만 남음**: `UNIQUE (a, b)`·`CREATE UNIQUE INDEX … (a, b)`·`ALTER TABLE … ADD UNIQUE (a, b)`가 `a`만 유일하게 만들거나 아예 무시했다 → `(1, 1)`과 `(1, 2)`가 충돌하거나, 진짜 중복 `(1, 1)`이 들어갔다. UPDATE·REPLACE·ON DUPLICATE KEY도 복합 키는 보지 않았다.
- **복합 UNIQUE 키·파티션 정보가 재시작하면 사라짐 (전부터)**: 카탈로그를 디스크에서 읽을 때 열·기본키·CHECK만 복원하고 나머지 스키마는 버렸다. UNIQUE 키는 재시작 뒤 아무도 지키지 않았고, **파티션 테이블은 재시작 뒤 `SELECT`가 0행**(행은 파티션 테이블에 있는데 그리로 가는 길이 없음), 새 행은 부모 테이블에 들어갔다.
- **`ALTER TABLE … ADD UNIQUE (c)`가 중복이 있어도 성공 (한 열도)**, `ALTER TABLE … DROP INDEX name`이 파싱 오류.
- **DROP TABLE이 외래키로 참조되는 테이블도 지움**(MySQL 3730), **UPDATE의 SET이 앞 배정을 보지 못함**(`SET a = a + 1, b = a`는 MySQL에서 새 a), 실패한 INSERT가 AUTO_INCREMENT를 쓰고 가기도 하고 안 하기도 함(→ 일관되게 돌려줌), `INSERT INTO t SELECT … FROM t`가 서버를 멈춤(읽기 락을 쥔 채 쓰기 락 승격), `INSERT … SELECT`의 열 매핑이 이름 기준(MySQL은 위치), `START TRANSACTION`·`UPDATE/DELETE … ORDER BY … LIMIT`·`SET col = DEFAULT`·`1e-7`·`-2.5e-7`·`3 * -2e2`가 파싱 오류.
- **`VALUES(col)`이 CASE/IF 조건 안에서 치환되지 않음**(옛 시험이 우연히 통과하고 있었음).

**수정**:
- **함수 표**(`builtin_functions.hpp/.cpp`, 약 2,000줄): 이름 → 함수 포인터 표. 인자는 값과 "종류"(`S` 따옴표 문자열, `N` 쓴 숫자, `C` 열, `D` DOUBLE로 읽는 열·문자열 열·부동소수점 식, `E` 그 밖의 식)로 받는다 — 종류가 없으면 인자 글자의 따옴표로 짐작하던 옛 방식은 중첩 식(`GREATEST(x + y, y * y)`)을 문자열로 오해했다. 표에 없고 옛 디스패처에도 없는 이름은 오류 1305. 같은 이름의 저장 함수가 있어도 내장이 먼저(MySQL과 같음). `sql_value_util.hpp`: UTF-8·대소문자·`Decimal`·`Num` 산술·달력·`DateTime`·`add_interval`·MySQL이 DOUBLE을 쓰는 방식(`format_double`: 999999999999999까지·1e-15까지는 풀어서, 그 밖은 `1.5e16`/`1e-16`).
- **산술**(`executor_eval.cpp`): `+ - * /` 식 전체를 정확한 십진수로 계산(`number_tree`/`combine`): 정수는 정수로(BIGINT 넘침 오류), 곱은 자리수 합, 몫은 `((피제수 자리 + 4 + 8) / 9) * 9`자리까지 **잘라서**(MySQL처럼 반올림 아님) 들고 가다 식 맨 위에서 한 번 반올림. DOUBLE/FLOAT 열·지수 표기 수·숫자가 아닌 글자가 한 번이라도 읽히면 그 위쪽 연산은 부동소수점(바인더가 `ArithExpr::Col::approx`를 달고, DECIMAL 아래쪽 식은 9자리 십진수를 double로 바꿔 넘김). `eval_arith_exact`: 식의 모든 자릿수를 그대로 내는 판 — 수학 함수(`ROUND`·`FLOOR`·`SQRT`·`MOD`…·`FORMAT`)의 인자, 비교의 양쪽, 열에 저장되는 값(UPDATE의 SET·INSERT의 VALUES)이 읽는다.
- **`ROUND`/`FORMAT`**: DOUBLE로 읽는 인자(DOUBLE/FLOAT 열, 문자열, 부동소수점 식)는 짝수쪽 반올림(`rint`), DECIMAL·정수는 0에서 먼 쪽.
- **`STDDEV`/`VARIANCE`**: MySQL과 같은 순환식(`m += (x - m) / n; s += (x - m_old) * (x - m)`)으로 DOUBLE 전체 자리(40시드 × 12 집계 변형을 실제 MySQL과 비교해 자리까지 일치).
- **복합 UNIQUE 키**: 스키마(`unique_keys`)·파서(`UNIQUE KEY name (a, b)`, `CREATE UNIQUE INDEX`, `ALTER … ADD [CONSTRAINT n] UNIQUE (a, b)`, `DROP CONSTRAINT`·`DROP INDEX`·`DROP KEY`)·INSERT(`CompositeUnique`: 기존 행 + 같은 문장 안, NULL이 든 조합은 충돌 아님)·UPDATE(`update_unique_violation`)·REPLACE(대상 행 삭제 + 문장 안 뒤의 행이 앞의 행을 대체)·ON DUPLICATE KEY·`SHOW CREATE TABLE`·JSON 저장·**카탈로그 복원**. `ADD UNIQUE`는 이미 든 행이 같은 조합을 갖는지 한 열짜리도 검사.
- **카탈로그 복원**: 디스크에서 읽을 때 `unique_keys`·`partition_info`도 되살린다.
- 그 밖에: `DROP TABLE`의 외래키 참조 검사, UPDATE 배정을 쓴 순서대로(앞 배정 결과를 보게), UPDATE/DELETE의 `ORDER BY … LIMIT`(정렬 키로 행을 뽑아 대상 지정), `SET col = DEFAULT`, `START TRANSACTION [WORK]`, `INSERT … SELECT` 자기 테이블(읽기 락 해제 후 승격)·열 위치 매핑, 실패한 INSERT의 AUTO_INCREMENT 되돌림(`CounterGuard`), `VALUES(col)`의 CASE/IF 안 치환(`bind_insert_values_cond`), 음수 지수 표기 리터럴(`read_exponent`).

**검증**:
- **실제 MySQL과의 대조가 시험의 중심**: 신규 `test_mysql_conformance.cpp`는 MySQL 8.4.9가 돌려준 답이 든 문장 332개(함수·연산·형 변환 61, 표와 질의 143, 제약·쓰기·트랜잭션 128; `vs_mysql.py --emit`이 만들고 RuSQL이 같은 답을 내는 문장만 "답 비교", 알려진 차이(정렬 규칙·DOUBLE 집계·`||` …)는 "실행만"으로 표시해 표의 상태만 맞춘다. 숫자는 값으로, 행은 `ORDER BY`가 없으면 집합으로 비교). 신규 `test_mysql_semantics.cpp`(6케이스: DOUBLE 열 산술·몫의 9자리·DOUBLE 글자 표기·`ROUND`/`FORMAT`의 짝수쪽 반올림·정수 산술의 자리와 한계·윤년과 내장 함수 우선 — 값은 전부 MySQL에서 받아 옴), `test_composite_unique.cpp`(복합 키 시나리오 — 같은 문장 안 중복, NULL 조합, REPLACE와 외래키, ALTER/CREATE UNIQUE INDEX, 재시작 — 과 **무작위 모델**: UNIQUE (a, b) · UNIQUE (c) · 기본키 표에 220문장 × 시드마다 INSERT / INSERT IGNORE / REPLACE / UPDATE / DELETE를 섞어 모델과 **문장마다** 표를 비교하고 중간에 서버를 다시 열어 비교), `test_partitioning.cpp`에 재시작 후 파티션 1케이스. 기존 시험 몇 곳은 MySQL 값으로 고쳤다(`DATE_ADD('2024-01-31', INTERVAL 1 MONTH)` = 2024-02-29, `CAST(3.99 AS INT)` = 4, `LOG`·`SQRT`의 자리, `SET v = …, d = (v > 5)`는 새 v를 봄, 몫의 자리, DOUBLE 열의 `ROUND`, `STDDEV`의 자리).
- 처음에 시험이 놓친 것들(MySQL과 비교하다 시험 자체가 찾아냄): 무작위 모델이 **REPLACE가 복합 키 충돌 행을 지우지 않고 오류를 내던 것**, **재시작하면 복합 UNIQUE 키·파티션 정보가 사라지던 것**, `ROUND(8/59.27, 2)`의 이중 반올림, `-2.5e-7`·`3 * -2e2` 파싱 오류, `ALTER TABLE … DROP INDEX` 파싱 오류를 찾았다.
- 심은 버그 83종(복합 키의 INSERT·UPDATE·REPLACE·ALTER·로드 각 분기, 산술의 자리·몫 자름·DOUBLE 판정·정수 빠른 길, STDDEV 순환식, `ROUND`/`FORMAT` 분기, 함수 표, 날짜·윤년·DOUBLE 표기 경계, 렉서·파서) 가운데 **처음에는 22종이 살아남았고** 시험을 더해 18종을 잡았다. 남은 4종은 결과가 같은 것으로 판단: `cu_delete_null`(REPLACE의 삭제 조건에서 `'NULL'` 비교는 3값 논리로 UNKNOWN이라 어차피 지우지 않음 — 중복 방어), `sd_sample_null`(표본 분산이 값 하나면 0/0 = NaN이 "NULL"로 찍힘 — 중복 방어), `ex_cmp_node`(`ArithExpr::Cmp`는 파서가 더 이상 만들지 않고 옛 저장 JSON에서만 옴), `fp_div_branch`(정수 몫의 빠른 길을 끄면 느린 길이 같은 값을 냄 — 성능만).
- Release/Debug **639 케이스/1,358,145 assertions**를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과(네 가지 모두). 무작위 시험 긴 캠페인(`[random]` 40시드 기본·강제 각 198,184 assertions, `[fuzz]` 30시드 1,531,973 assertions)도 통과.
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) 차이 160건 **전부 `STDDEV`·`VARIANCE`의 자리**(옛 빌드는 4자리로 잘랐고 이제 DOUBLE 전체 자리 — 값은 1e-3 안에서 같음); 그 밖의 차이 0건. 출력 검증 도구 12종(`verify_*.py`) 모두 위반 0건(4시드씩, `verify_writes` 3시드 × 1,500문장, `verify_null_expressions` 12시드 × 400질의). `verify_null_expressions.py`의 기준 모델은 매 연산을 6자리로 반올림하던 옛 규칙이라 MySQL의 십진수 규칙(몫의 9자리, 맨 위에서 한 번 반올림, DOUBLE 열은 부동소수점)과 어긋났다 — 값을 1e-4 안에서 비교하고(`WHERE`는 한계에서 1e-3 안의 행을 건너뜀, 몫 안의 몫은 값 비교를 건너뜀, `ROUND`는 십진수로) 정확한 글자는 MySQL 대조 시험이 맡도록 고침.
- 저장소의 `test_full.sql`(319문장)·`test_full-ver2.sql`(359문장)을 이전 빌드와 새 빌드에 스트림으로 보내 비교: 차이는 모두 의도한 것이다 — `STDDEV`·`VARIANCE`의 자리, 수학 함수의 표기(`SQRT(256)` 16.000000 → 16, `LOG(8, 2)` → `0.33333333333333337` 등 MySQL과 글자까지 같음), `MONTH`·`DAY`가 `03` 대신 `3`, `CAST(4.7 AS DECIMAL)` → 5(MySQL의 `DECIMAL(10,0)`), 실패한 INSERT가 AUTO_INCREMENT 번호를 쓰지 않아 감사 로그 id가 한 칸씩 당겨짐, `SHOW CREATE TABLE`·`information_schema`에 UNIQUE 키 이름이 나옴, 그리고 `test_full-ver2.sql`의 `ALTER TABLE supplier ADD CONSTRAINT uq_supplier_name UNIQUE (name)`이 이미 같은 이름('Nordic Components')이 두 번 들어 있어 이제 오류(MySQL도 같음). **이 비교가 회귀 하나를 잡았다**: 새로 넣은 "외래키가 참조하는 테이블은 `DROP TABLE` 거부"가 `RESTORE`가 같은 데이터베이스 위에 덤프를 되돌릴 때(덤프는 `DROP TABLE IF EXISTS`를 표 하나씩 재생) 부모 표에서 막혀 `31 statement(s) OK, 0 error(s)`가 `9 OK, 22 error(s)`로 바뀌었다 → RESTORE는 이미 있던 `skip_fk_checks`(mysqldump 파일이 `FOREIGN_KEY_CHECKS = 0`을 쓰는 것과 같음)로 이 검사를 건너뛰게 하고, 부모·자식 표를 백업한 뒤 그 위에 복원하는 시험(`test_executor_misc.cpp`)을 더했다(검사를 지운 심은 버그 1종 잡힘).
- 크래시 퍼저 60라운드 + 동시성 퍼저 20라운드(확인한 행 10,194): 불일치 0건.
- 성능(이전 빌드와 번갈아 2라운드, 5만 행, 결과 캐시를 피하려고 질의마다 다른 상수): 점 조회 0.13 → 0.14 / 0.13 → 0.13 ms, 조인 43.4 → 46.1 / 43.9 → 41.7, `GROUP BY` 65.2 → 65.7 / 64.2 → 65.4, `HAVING` + `ORDER BY` 68.0 → 70.4 / 69.3 → 66.3, 스캔 36.2 → 37.2 / 37.7 → 36.4, 벌크 적재 521 → 484 / 503 → 507, **산술 `WHERE` 167.6 → 103.2 / 166.0 → 97.2**, 식의 `SUM(ROUND(…))` 244 → 199 / 239 → 201, `UPDATE … SET v = v * 3 / 3` 78.4 → 78.1 / 74.2 → 82.8, 함수 호출이 든 `WHERE` 84.3 → 92.2 / 85.5 → 90.7(함수 표 + 인자 종류, +8%). 정확한 십진수를 문자열로 계산한 첫 구현은 산술 `WHERE`가 +47%(169 → 246 ms)였는데, **정수 값은 십진수를 만들지 않고 64비트로** 계산하도록 고쳐(정수 읽기·정수끼리의 연산·정수 몫의 9자리) 이전 빌드보다 빨라졌다.
- **죽은 코드 정리**: 새 함수 표가 먼저 불리기 때문에 옛 `apply_scalar_func`의 이름 분기 56개(389줄)가 도달할 수 없게 되어 지웠고(남은 것은 `NOW`·`COALESCE`·`IFNULL`·`IF`·`NULLIF`·`REGEXP*`·`RAND`·`MD5`·`UUID`·`JSON_EXTRACT`·`JSON_UNQUOTE`·`JSON_VALUE`·`DATABASE`·`USER` …), 그 분기들만 쓰던 보조 함수 17개(`fmt_double`·`add_months`·`trim_*` …)와 `is_number_literal`, 안 쓰는 `#include` 3개도 지웠다. 지운 뒤 Release 전체를 기본·강제 양쪽으로 다시 돌렸다.


**눈에 띄는 변화(의도한 것)**: 모르는 함수가 오류; `10 / 3 * 3`이 `10`(9.9999가 아님); `ROUND(x / y, 2)`가 몫의 4자리가 아니라 전체 자리를 반올림; DOUBLE 열의 `ROUND(2.5)`가 2; `d * 1.1`이 `3.3000000000000003`(DOUBLE이면 MySQL도 그렇다); `STDDEV`가 `31.483874840940402`; 복합 UNIQUE가 실제로 복합 키; 재시작 뒤에도 UNIQUE 키·파티션이 남음; 같은 이름의 저장 함수보다 내장 함수가 먼저; `DROP TABLE`이 참조되는 테이블에서 오류; `UPDATE t SET a = a + 1, b = a`의 b가 새 a. `ALTER TABLE … ADD UNIQUE`는 이미 같은 값이 들어 있으면 오류(MySQL과 같음 — 전에는 받아들여 제약이 깨진 채로 있었다); `SHOW CREATE TABLE`이 열 옆의 `UNIQUE` 대신 `UNIQUE KEY `이름` (`열`)` 줄을 보여 주고 `information_schema`에도 제약 이름이 나옴; 오류로 실패한 INSERT가 AUTO_INCREMENT 번호를 쓰지 않음.

**정직한 한계 (MySQL과 아직 다른 것)**:
- ① **기본 정렬 규칙**: MySQL 기본(`utf8mb4_0900_ai_ci`)은 대소문자·악센트를 구분하지 않는다(`'a' = 'A'`, `LIKE 'A%'`, `GROUP BY`·`DISTINCT`·`UNIQUE`·`ORDER BY`·`MIN`/`MAX`도). 여기는 바이트 비교다. 비교·해시 키·인덱스·LIKE 전부에 걸친 큰 변경이라 이번에 하지 않았다 — **결정이 필요한 항목**(R15 후보).
- ② `SUM`/`AVG`/`MIN`/`MAX` of DOUBLE 열은 아직 정확한 십진수로 계산한다(MySQL은 double: `AVG(d)` = `13.11111111111111`, 여기는 `13.1111`; 합은 부동소수점 잡음 `0.30000000000000004`까지 같음).
- ③ 지수 표기 수는 `0.5e0`처럼 정수·소수로 펼쳐지면 정확한 십진수로 읽힌다(MySQL은 항상 DOUBLE); `DATE('2024-03-05') + 0`은 20240305(여기는 2024); `||`는 MySQL에서 OR(여기는 문자열 이음 — 일부러 둠); `WITH ROLLUP` 없음; `PI()` 표시는 6자리.
- ④ AUTO_INCREMENT: MySQL(InnoDB)은 실패한 INSERT도 번호를 쓰고 가서 틈이 생긴다(`INSERT … VALUES (a), (b)`는 두 개를 미리 잡음). 여기는 오류로 실패하면 돌려준다(`INSERT IGNORE`가 건너뛴 행은 MySQL처럼 번호를 씀) — 틈 없는 쪽을 일부러 택함.
- ⑤ `UPDATE u SET k = k + 1`(유일 키 이동)은 MySQL이 행마다 검사해 오류, 여기는 문장 끝에서 검사해 허용; `UPDATE t SET … WHERE id IN (SELECT … FROM t)`는 MySQL이 1093 오류, 여기는 실행.
- ⑥ 함수 인자가 `SQRT(2)` 같은 함수 결과일 때 `ROUND(SQRT(2), 3)`은 DECIMAL처럼 반올림(MySQL은 DOUBLE이라 짝수쪽) — 부동소수점 식과 열만 DOUBLE로 취급.
- ⑦ 알려진 함수 이름 검사는 평가할 때 한다(빈 테이블에 `SELECT nosuch(x)`는 MySQL과 달리 오류가 나지 않음).

**Diagram 페이지**: 구성요소·흐름이 바뀐 것이 없어 변경 없음.

### 10월 12일 (두 번째) — 기본 정렬 규칙 `utf8mb4_0900_ai_ci`: 글자가 MySQL처럼 대소문자·악센트를 구분하지 않고 비교·정렬·묶임 (R15)

**왜 이 항목인가**: 앞 항목(R14, 실제 MySQL과의 대조)이 남긴 가장 큰 차이였고, 사용자 지시("R15 진행")로 했다. MySQL 8.4의 기본 정렬 규칙은 대소문자·악센트를 보지 않는다 — `'a' = 'A'`, `'e' = 'é'`, `'ß' = 'ss'`, `ORDER BY`가 `a, A, b, B` 순서, `GROUP BY`·`DISTINCT`·`UNIQUE`·`PRIMARY KEY`·`IN`·조인 키·`LIKE`·`MIN`/`MAX`가 모두 그렇다. 여기는 지금까지 **모든 글자 비교가 바이트 비교**였다. 오류가 나지 않고 답이 틀리는 종류라서(`WHERE name = 'alice'`가 `'Alice'`를 놓침, `GROUP BY`가 대소문자별로 갈라짐, `UNIQUE`가 `'Alice'`와 `'alice'`를 둘 다 받음, `ORDER BY`가 대문자를 모두 소문자 앞에 둠) 정확해야 하는 DB로는 치명적인 차이다.

**범위 결정**: 기본 정렬 규칙 하나만 구현했다. 열 단위 `COLLATE`·`BINARY`·`CHARACTER SET`과 표 옵션(`ENGINE=… DEFAULT CHARSET=… COLLATE=…`)은 이번에도 파싱 오류로 거절된다(조용히 무시하면 `utf8mb4_bin` 열이 대소문자를 구분하지 않아 오히려 위험하므로 오류가 맞다) — 아래 한계 ①.

**수정**:
- **새 모듈 `collation`**(`include/engine/collation.hpp`, `src/collation.cpp`, 생성된 `src/collation_data.inc`): MySQL이 `WEIGHT_STRING`으로 돌려주는 1차 가중치 표(7,787개 구간으로 압축; 유니코드 전 영역 1,112,065개 코드 포인트가 MySQL과 한 개도 다르지 않음). 규칙: 대소문자·악센트는 같은 가중치, 공백과 구두점은 구분(NO PAD: 끝의 공백도 셈), 제어 문자는 무시, 한글 음절은 자모 가중치로 풀어서, 한자·미할당 코드 포인트는 코드 포인트에서 나오는 implicit 가중치, UTF-8이 아닌 바이트는 한 바이트당 U+FFFD. 함수: `compare`·`equal`·`key`(같은 글자는 같은 키 — 해시·정렬 묶음용)·`sort_key`(순서를 보존하는 바이트열 — `ORDER BY` 캐시용)·`like`(코드 포인트 하나가 가중치 하나와 맞는 `%`·`_`).
- **통합**(전부 한 모듈을 부름): 값 비교 `compare_classed`(= `<>` `<` `>` `BETWEEN` `IN` `CASE x WHEN`…), `ORDER BY`(`sort_key`를 행마다 한 번), `GROUP BY`·`DISTINCT`·`COUNT(DISTINCT)`·`GROUP_CONCAT(DISTINCT)`·`UNION`·`INTERSECT`·`EXCEPT`·재귀 CTE·윈도 `PARTITION BY`의 키, `MIN`/`MAX`, `IN`/`NOT IN`(서브쿼리 포함), 해시·정렬 병합·중첩 루프 조인, B+Tree(글자 열은 정렬 규칙 순서, 같게 비교되는 키는 한 항목이고 처음 쓴 철자를 남김), 해시·복합 인덱스 키(`normalize_numeric_key`), 보조 인덱스 버킷(`IndexBuckets`), `UNIQUE`·`PRIMARY KEY`·복합 키·`FOREIGN KEY`(INSERT·UPDATE·REPLACE·`ON DUPLICATE KEY`·`ALTER … ADD UNIQUE`·한 문장 안의 반복), `LIKE`(인덱스 접두 스캔 포함)·`REGEXP`(대소문자), `STRCMP`·`FIELD`·`FIND_IN_SET`·`GREATEST`/`LEAST`·`NULLIF`·`INSTR`/`LOCATE`(MySQL처럼 대소문자는 무시·악센트는 구분)·`REPLACE`/`SUBSTRING_INDEX`/`TRIM`(MySQL처럼 대소문자 구분 그대로).
- **시험이 찾아낸, 정렬 규칙을 따라가지 않던 곳**(전부 고침): ① 인덱스를 거친 `WHERE s LIKE 'ab%' ORDER BY id`가 키의 바이트 접두만 보고 **빈 결과**(`'ABC'`로 저장된 키를 못 찾음); ② 외래키 `ON DELETE`/`ON UPDATE` **CASCADE·SET NULL·SET DEFAULT가 자식 행을 바이트로 찾음** — 부모 `'abc'`를 지워도 자식 `'ABC'`가 남아 고아 행이 생김(INSERT의 외래키 검사는 이미 정렬 규칙을 따르므로 `'ABC'`가 `'abc'`를 가리키는 것은 허용되어 있었다); ③ **행 락이 철자 그대로** — `'Bob'`과 `'bob'`을 동시에 넣는 두 세션이 서로 다른 락을 잡아 둘 다 들어갈 수 있었음(이제 락은 정렬 키로 잡고 `SHOW LOCKS`에는 처음 쓴 철자가 보임); ④ **갭 락의 범위 판정이 바이트**라 `['a', 'c']` 범위의 락이 `'Bar'`의 INSERT를 막지 못함; ⑤ **파티션**: `RANGE` 파티션의 글자 열에서 `'Zed'`는 `'m'` 앞, `'zed'`는 뒤로 갈라져 `WHERE name = 'zed'`가 두 행 중 하나만 돌려줌(조용한 오답), `LIST` 파티션은 `'ny'`가 `'NY'` 파티션을 못 찾아 INSERT 오류; ⑥ 가중치 표가 U+1FFFF까지만 있어 평면 14의 태그 문자(U+E0001 등 — MySQL은 무시)와 평면 2의 호환 한자가 다르게 정렬됨(MySQL에서 다시 뽑아 U+10FFFF까지 채움).
- 도구: `code/test/collation/`의 `dump_weights.py`(MySQL에서 가중치를 뽑음)·`gen_collation_data.py`(표를 만듦)·`gen_collation_cases.py`(MySQL의 `STRCMP` 답이 든 시험 쌍을 만듦)·`gen_ascii_model.py`(검증 도구가 쓰는 ASCII 모델 `code/test/diff/collation_model.py`를 만듦).

**검증**:
- **MySQL 8.4.9와의 대조**: `STRCMP` 쌍 10,503개(여러 문자 체계·대소문자·악센트·공백·길이 차이, 표 구간의 가장자리, 한글 음절과 그 자모, 한자 구간 가장자리) 전부 일치; 환경 변수 `RUSQL_COLLATION_WEIGHTS=<dump>`로 켜는 시험은 코드 포인트 1,112,065개의 가중치를 MySQL의 `WEIGHT_STRING`과 한 개씩 비교해 전부 일치; SQL 수준 시험(`mysql_conformance_collation.inc`)은 MySQL이 돌려준 답이 든 문장을 같은 답으로 돌려주는지 본다(비교·범위·`IN`·`LIKE`·`ORDER BY`·`GROUP BY`·`DISTINCT`·집계·조인·`UNION`·윈도·유일 키·외래키·`REPLACE`·재귀 CTE). 180문장 중 5개가 다른데 전부 아래 한계 ②의 것.
- **새 시험**: `test_collation.cpp`(쌍 비교·키·정렬 키·`LIKE` 20개·잘못된 UTF-8·모든 코드 포인트의 키 일치), `test_collation_sql.cpp`(무작위: 인덱스 없음·B+Tree·해시·글자 기본키 네 가지 표에 같은 행을 넣고 `= <> < >= BETWEEN IN NOT IN LIKE`·`ORDER BY`·`GROUP BY`·`DISTINCT`·`MIN`/`MAX`·조인을 정렬 규칙 기준 모델과 비교, 재시작 포함; 유일 키·외래키 동작·`REPLACE`·`LIKE` 접두 스캔·동시 INSERT·갭 락·여러 세션의 경쟁), `test_lock_manager.cpp`·`test_join.cpp`에 한 케이스씩. 옛 시험 가운데 바이트 순서를 전제한 6곳(B+Tree·타입 비교·벌크 쓰기·쓰기 무결성)을 고쳤다.
- **심은 버그** 96종(가중치 표·UTF-8 디코더·키·정렬 키·`LIKE`의 각 분기, B+Tree 비교, 조인·`ORDER BY`·`GROUP BY`·`DISTINCT`·`MIN`/`MAX`·집합 연산·CTE·윈도·`IN` 서브쿼리, 함수, `UNIQUE`/`PRIMARY KEY`/외래키의 INSERT·UPDATE·REPLACE 각 분기, 외래키 동작 8곳, 행 락·갭 락, 파티션 3곳, 헤더 3곳): 첫 78종 가운데 **34종이 살아남았고** 시험을 더해 잡았다(그 시험이 외래키·락·갭 락·파티션·인덱스 재구축의 구멍을 더 찾아, 거기에 18종을 더 심었다). **88종을 잡았다.** 잡지 못한 8종: 7종은 결과가 같은 것으로 판단 — `cl_implicit_base1`/`cl_implicit_base2`(한자 블록의 기본 가중치 상수를 1 올려도 그 사이에 낀 가중치가 없어 모든 비교가 같은 방향), `cl_implicit_low`(두 번째 가중치의 맨 위 비트는 같은 블록끼리만 비교되어 순서가 변하지 않음), `cl_hangul_two`(받침 없는 음절에 더하는 세 번째 자모 U+11A7은 표에 가중치가 없어 더해도 달라지지 않음), `se_equal_texts`(숫자 모양 키의 범위를 훑다가 글자끼리 비교하는 줄 — 끝에 무시되는 제어 문자만 다른 값에서만 차이), `uu_changed`·`uu_same`(바뀐 행의 옛 값이 "지금 들고 있는 값" 집합에 이미 들어 있어 앞 분기가 걸러 내도 결과가 같음). 1종 `dm_fk_parent`는 락을 기다린 뒤 중복을 다시 확인하는 줄이라 결정적으로 시험할 수 없다(한 세션의 미확정 행은 다른 세션의 중복 검사에 이미 보여서 기다리기 전에 오류가 난다).
- Release/Debug **661 케이스/약 1,435,330 assertions**(639 → 661)를 기본 설정과 `RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과(네 가지 모두). 긴 캠페인도 통과: `[random]` 40시드 기본 249,156·강제 249,154 assertions, `[fuzz]` 30시드 1,555,183 assertions, 정렬 규칙 무작위 시험(`[collation][random]`) 400시드 309,492 assertions.
- 빌드 간 차분(이전 빌드 = 앞 항목): 30시드 × 99질의(2,970질의) 차이 0건 — 이 말뭉치의 글자 값은 소문자 단어와 숫자 모양 문자열뿐이라 정렬 규칙이 달라지는 곳이 없다는 확인이다(차이가 없다는 것은 규칙 밖의 것이 바뀌지 않았다는 뜻; 규칙 안의 변화는 위 MySQL 대조와 아래 검증 도구가 본다). 출력 검증 도구 12종 4시드씩(+ 마지막 빌드에서 5종 2시드 더) 위반 0건, 단 `verify_null_expressions` 시드 4는 식 안의 몫을 6자리로 들고 가는 옛 기준 모델 때문에 `id * (price / id)`에서 허용 오차를 넘는다(R14 빌드에서도 같은 위반; 엔진의 값은 MySQL과 같은 `0.110000`): `verify_compare`·`verify_sort_group`·`verify_value_expressions`의 기준 모델은 바이트 순서였으므로 MySQL의 ASCII 가중치 모델(`collation_model.py`)로 고쳤다(`verify_compare`는 이제 영문 대소문자 쌍 `abc`/`ABC`와 글자 기본키의 중복도 본다).
- 저장소의 `test_full.sql`·`test_full-ver2.sql`을 이전 빌드와 새 빌드에 스트림으로 보내 비교: 시각·접속 번호·실행 시간 말고 의미 있는 차이는 `name LIKE '%Book%'`가 `'Ultrabook Air 13'`도 고르는 것 하나(MySQL의 `LIKE`는 대소문자를 보지 않으므로 의도한 변화). 크래시 퍼저 90라운드 + 동시성 퍼저 30라운드(확인한 행 16,736): 불일치 0건.
- 성능(이전 빌드와 번갈아 2라운드, 5만 행; 글자 열 14가지 모양 포함): 점 조회 0.10 → 0.10, 글자 기본키 점 조회 0.10 → 0.11, 인덱스 글자 `=` 0.11 → 0.11, 글자 `ORDER BY` 31.1 → 30.0 / 30.1 → 30.9 ms, `GROUP BY` 글자 28.7 → 29.2 / 25.2 → 29.2, `DISTINCT` 24.3 → 22.6 / 25.0 → 23.5, 글자 조인 26.9 → 26.9 / 28.9 → 25.7, 일괄 적재 717~737 → 738~740(+3%, 글자 인덱스를 만드는 비용) — 대부분 ±10% 잡음 안. **`LIKE '접두%'` 훑기만 2배(18.5 → 38 ms)로 느려졌다가** 행마다 코드 포인트 목록을 만들던 것을 ASCII 전용 빠른 길(할당 없음)로 바꿔 18.7~21.1 → 19.6~22.5로 돌려놓았다(빠른 길은 정의대로 짠 느린 기준 구현과 무작위 20,000쌍을 비교하는 시험이 지킨다). `집계 GROUP BY + ORDER BY` 모양은 5라운드 평균 39.1 → 41.4(+6%)로 잡음이 큼(글자와 무관한 질의).
- **죽은 코드 정리**: 이번 변경이 안 쓰게 만든 함수·include는 없었다(옛 `like_match` 재귀 구현만 `collation::like` 호출로 바뀜). 저장소에 없던 비교 도구(`vs_mysql.py`·`mysql_probe.py`)와 그 말뭉치 4개를 `code/test/diff/`로 옮겼다(앞 라운드의 생성된 `.inc` 머리말이 이미 그 경로를 가리키고 있었다).

**눈에 띄는 변화(의도한 것)**: `'a' = 'A'`; `WHERE name = 'alice'`가 `'Alice'`도 찾음; `ORDER BY name`이 `alice, Alice, ALICE, Bob`…(대문자가 앞서지 않음); `GROUP BY`·`DISTINCT`가 철자가 달라도 하나(처음 나온 철자를 보여 줌); `UNIQUE`·`PRIMARY KEY`·`FOREIGN KEY`가 `'Alice'`와 `'alice'`를 같은 값으로; `LIKE 'a%'`가 `'Apple'`도; 끝의 공백은 값의 일부(`'bob '` ≠ `'bob'`, MySQL NO PAD).

**정직한 한계 (MySQL과 아직 다른 것)**:
- ① **열 단위 `COLLATE`·`BINARY`·`CHARACTER SET`, 표 옵션(`ENGINE=InnoDB DEFAULT CHARSET=… COLLATE=…`), `COLLATION()`, `SHOW COLLATION`이 없음** — 모두 파싱 오류(조용히 틀리지는 않음). 대소문자를 구분하는 열(`utf8mb4_bin`)을 만들 수 없고, `ENGINE=` 같은 옵션이 붙은 mysqldump 형태의 `CREATE TABLE`도 받지 못한다.
- ② **결과 표를 글자로 다시 읽는 경로가 값의 앞뒤 공백을 잃음** — `INSERT … SELECT`, `UNION`/`INTERSECT`/`EXCEPT`, CTE, 파생 테이블, `IN (서브쿼리)`가 한 번 표(글자)로 만들어진 결과를 `parse_table_output`으로 다시 읽는데 그때 셀의 앞뒤 공백을 지운다. 정렬 규칙이 NO PAD이므로 `'bob '`와 `'bob'`은 다른 값인데 이 경로에서는 같아진다(전부터 있던 한계가 이제 비교 결과에 보임). 해결은 결과를 글자 표가 아니라 구조로 넘기는 일이라 큰 변경 — 다음 후보.
- ③ `INSTR`/`LOCATE`에서 `'ß'`·`'æ'` 같은 확장 문자를 가중치 열 단위가 아니라 문자 단위로 찾음(MySQL과 다른 곳이 있음), 통계(ANALYZE)의 히스토그램은 글자를 아직 바이트 순서로 정렬(추정에만 영향).
- ④ 같은 키를 동시에 넣는 경쟁에서 락을 기다린 뒤 중복을 다시 확인하는 줄은 결정적으로 시험할 수 없다(경쟁 창이 극히 좁음 — 6개 스레드로 같은 글자를 여러 철자로 넣는 스트레스 시험이 있지만 그 줄을 반드시 잡지는 못함).
- ⑤ 이 점검에서 따로 찾은, 정렬 규칙과 무관한 파싱 한계: CTE의 열 목록 `WITH r(n) AS (…)`(파싱 오류), 윈도 별칭을 `AS` 없이 쓰기, 파생 테이블 안이나 함수로 감싼 `GROUP_CONCAT(DISTINCT …)`, `COUNT(DISTINCT a, b)`.

**Diagram 페이지**: B+Tree·해시 인덱스·Cylinders 설명에 "글자는 MySQL 기본 정렬 규칙으로 비교(A = a = Á)" 문구를 추가(영어·한국어 두 보기).

### 10월 12일 (세 번째) — UNIQUE 값의 동시 INSERT/UPDATE: 두 세션이 같은 값을 동시에 쓰면 둘 다 들어가던 것 (R16), 그리고 R15가 만든 `NULL`과 문자열 `'null'`의 혼동

**왜 이 항목인가**: R15 보고에서 따로 적어 둔 "정렬 규칙과 무관한 전부터의 무결성 구멍"(사용자 지시: "R16부터 시작해서 계속 진행"). 정확해야 하는 DB에서 `UNIQUE`가 동시에 깨지는 것은 치명적이다. 고치는 중에 시험이 R15의 회귀 하나(`NULL` ↔ `'null'`)도 찾아냈다.

**원인과 영향**:
- **같은 `UNIQUE` 값을 동시에 INSERT하면 둘 다 들어간다.** INSERT의 중복 검사는 표를 읽는데, 다른 세션이 막 쓰려는 행은 아직 표에 없다 → 둘 다 검사를 지나 둘 다 쓴다. 기본키 열에는 행 락 "클레임"(락 매니저에 값을 잡아 두는 것)이 있어 두 번째가 첫 번째를 기다렸다가 다시 검사하지만, **기본키가 아닌 `UNIQUE` 열, `UNIQUE (a, b)` 키, 복합 `PRIMARY KEY`, 기본키 없는 표에는 클레임이 없었다.** 8세션이 같은 새 값을 동시에 쓰는 시험(`test_unique_claims.cpp`)에서 R15 빌드는 **6번 가운데 6번 모두** 중복을 남겼다(`UNIQUE` 열·`UNIQUE (a, b)`·복합 기본키·기본키 없는 표).
- **기본키 클레임이 있는 표에서도 구멍**: 한 행이 무엇이든 기다리느라 데이터 락을 놓았다 다시 잡으면(갭 락 대기, 다른 클레임 대기) **그 전에 한 중복 검사는 낡은 것**인데 다른 키를 다시 보지 않았다. `AUTO_INCREMENT` 표에서는 실패한 INSERT가 번호를 돌려주기 때문에 같은 번호를 두 세션이 잡는 일이 잦아(시험에서 218~423번) 이 구멍이 거의 매번 열렸다.
- **UPDATE도 같다**: INSERT가 중복 검사를 마치고 행을 쓰기 전에 UPDATE가 같은 값을 다른 행에 쓸 수 있다.
- **R15가 만든 회귀**: 엔진의 NULL은 글자 `NULL`이라서 대소문자를 보지 않는 정렬 규칙에서 **문자열 `'null'`·`'Null'`이 NULL과 같은 값**이 되었다 — `GROUP BY`에서 NULL 그룹에 섞이고, `UNIQUE` 열에 NULL 행이 있으면 `'null'`을 넣을 수 없었다(MySQL에서 NULL은 값이 아니다).

**수정**:
- **모든 키에 클레임**(`Executor::unique_key_claims`, `executor_update_unique.cpp`): `UNIQUE` 열(NULL이 아닌 값), `UNIQUE (a, b)` 키(NULL이 든 조합은 제외), 복합 기본키. 락 매니저의 이름은 `d.t#uk:열` / `d.t#ck:pk` / `d.t#ck:u0`, 값은 열의 글자(락 매니저가 정렬 규칙으로 같은 값을 알아봄) 또는 정렬 키로 인코딩한 튜플.
- **INSERT**(`executor_dml.cpp`): 기본키 클레임 바로 뒤에서 행의 키마다 클레임을 잡는다. 이미 누가 잡았으면 데이터 락을 놓고 기다린 뒤 **표를 다시 훑어 값이 있는지 본다** — 있으면 문장의 충돌 절이 말하는 대로(오류 / `IGNORE`는 건너뜀 / `ON DUPLICATE KEY UPDATE`는 갱신). 기본키와 앞서 나온 모든 클레임에 대해 "검사는 클레임보다 먼저이므로, 이 행이 무엇이든 기다린 적이 있으면 클레임을 받은 뒤 다시 본다"(`dropped_shared`): 갭 락 대기 뒤의 기본키, `AUTO_INCREMENT` 번호 충돌 대기 뒤의 `UNIQUE` 값.
- **UPDATE**(`executor_update.cpp`): 새 버전이 새 값을 갖는 `UNIQUE` 열·키(그리고 바뀌는 기본키)를 같은 클레임으로 잡는다. 이미 잡혀 있으면 아직 아무것도 도장 찍지 않았으므로 시도를 버리고 기다린 뒤 처음부터 다시(기존 행 락 충돌과 같은 길) — 다시 돌면 상대의 행이 보여 중복 오류.
- **락 매니저**: 복합 키 클레임(`#ck:`)은 이미 정렬 키를 거친 튜플이라 다시 정규화하지 않는다.
- **NULL**(`collation.cpp`): 글자 `NULL`은 엔진의 NULL — 자기 자신하고만 같고, 모든 글자보다 앞서고, 키가 다르며, `LIKE`에 걸리지 않는다. `'null'`·`'Null'`은 보통 글자.

**검증**:
- 신규 `test_unique_claims.cpp`(8케이스): 8세션이 장벽으로 줄을 서서 **같은 새 값을 동시에** 쓰고(INSERT 4가지 모양 + UPDATE 2가지) 한 세션이 모든 표에서 두 번 든 값을 찾는다 — R15 빌드 6/6 실패, 수정 뒤 연속 50번 이상 통과; 결정적 시험: 손으로 잡은 클레임(= 중간에 있는 다른 세션) 위에서 INSERT·UPDATE가 **기다리는지(≥150 ms)**, 풀리면 **다시 보고** 충돌 절대로 행동하는지, 기다리는 도중 상대가 값을 쓰면 중복 오류인지, NULL·바뀌지 않은 키·서로 다른 한글 튜플은 기다리지 않는지, `@lock_wait_timeout`이 지켜지는지, 갭 락을 기다린 뒤의 기본키·UNIQUE 중복.
- MySQL 대조 말뭉치에 `NULL`과 `'null'`/`'Null'` 17문장(GROUP BY·DISTINCT·IN·조인·UNIQUE·LIKE·MIN/MAX) — 전부 MySQL과 같은 답.
- **심은 버그 32종**(클레임을 만드는 모든 분기 — NULL·바뀌지 않은 키·열/복합 기본키/UNIQUE 키·락 키 인코딩·메시지, INSERT의 클레임·기다림·다시 보기·충돌 절 3가지·가시성·갭 락 뒤의 다시 보기, UPDATE의 클레임·기본키 클레임·충돌 처리·기다릴 락, 락 매니저의 복합 키, 글자 `NULL`의 4곳): 처음 28종 가운데 4종이 살아남았고 — `ins_visible_not_checked`(지워진 행도 중복으로 셈)·`upd_wait_on_table`(기다릴 락을 잘못 잡아 **기다리지 않고 계속 돌며 timeout도 무시** — 시험이 끝나지 않고 멈춘 것으로 드러남) 두 종은 시험을 더해 잡았고, 남은 2종은 결과가 같은 것으로 판단: `ins_always_look_again`(검사를 건너뛰어도 되는 경우에 항상 다시 봄 — 결과는 같고 느려지기만 함), `uc_holds_null`(NULL 행이 클레임의 임자로 보이는 것은 글자 `NULL`을 값으로 따로 다루게 된 뒤로 불가능해짐). 글자 `NULL` 4종은 모두 잡았다. **30종 잡음.**
- Release **670 케이스/약 1,435,660 assertions**(661 → 670)를 기본·`RUSQL_DML_INDEX_MIN_ROWS=0` 양쪽에서 통과, Debug도 같은 670 케이스/1,435,661 assertions(기본·강제 양쪽). 긴 캠페인: `[random]` 40시드 249,486 assertions(기본·강제 같음), `[fuzz]` 30시드 1,555,183, 동시 쓰기 시험 60시드(8세션 × 100단계) 491 assertions — 모두 통과.
- 빌드 간 차분(이전 빌드 = R15): 30시드 × 99질의 차이 0건. 출력 검증 도구 12종 4시드씩 위반 0건(`verify_null_expressions` 시드 4만 R14부터 있던 기준 모델 문제 그대로: `id * (price / id)`의 몫을 모델이 6자리로 들고 가서 0.11016, 엔진·MySQL은 0.110000). `test_full.sql`·`test_full-ver2.sql` 스트림: 시각·접속 번호·실행 시간 말고 차이 없음. 크래시 퍼저 60라운드 + 동시성 퍼저 20라운드(확인한 행 11,659): 불일치 0건.
- 성능(이전 빌드와 번갈아 3라운드, 2만 행): 일반 표 일괄 적재 130 → 130 ms, 단일 INSERT 500개 322 → 323 ms; UNIQUE 열이 든 표의 단일 INSERT 500개 815 → 871 / 787 → 818 / 826 → 827 ms(+0~7%); 한 문장 `UPDATE` 5,000행의 UNIQUE 열 2,632 → 3,384 / 2,585 → 2,765 / 2,615 → 2,785(+6% 안팎, 첫 값은 잡음); 일괄 적재와 복합 키는 ±3% 안. **클레임의 비용은 잡음 수준이다.** 같은 측정이 드러낸 전부터의 문제: **UNIQUE 열(기본키 아님)이나 복합 기본키가 든 표의 일괄 적재가 이차식** — 2만 행에 UNIQUE 열이 있으면 8.5초, 복합 기본키면 21.8초(일반 표는 0.13초): 행마다 표 전체를 훑어 중복을 검사한다(R17 후보).

**한계**: 아래 R17 후보. `UNIQUE` 값 클레임은 기본키처럼 명령문(트랜잭션이면 COMMIT)이 끝날 때까지 잡힌다 — 열린 트랜잭션이 어떤 값을 지웠다가 다시 쓰려는 사이에 다른 세션이 같은 값을 넣으면 그 트랜잭션이 끝날 때까지 기다린다(InnoDB와 비슷한 동작).

### 10월 12일 (네 번째) — 외래키의 동시성: 자식 INSERT/UPDATE와 부모 DELETE/UPDATE가 동시에 일어나면 고아 행이 남던 것, UPDATE의 데이터 락 순서 때문에 서로를 기다리며 멈추던 것 (R17)

**왜 이 항목인가**: R16 보고에서 따로 적어 둔 구멍(사용자 지시: "계속 진행"). 외래키는 "부모가 있다"는 약속인데 동시에 일어나면 지켜지지 않았다.

**원인과 영향**:
- **고아 행**: 자식 INSERT는 부모가 있는지 읽고(데이터 락 SHARED) 락을 놓은 뒤 행을 쓰고, 부모 DELETE는 자식이 있는지 읽고 지운다. 둘이 겹치면 — 자식이 부모를 확인한 뒤 부모가 지워지고 그 뒤에 자식이 쓰이면 — 둘 다 성공해 **부모 없는 자식**이 남는다. 3세션이 같은 부모의 자식을 넣고 1세션이 그 부모를 지우는 시험(150개 부모)에서 R16 빌드는 6라운드 모두, 한 라운드에 **200개 안팎의 고아 행**을 남겼다. 부모 키를 바꾸는 UPDATE(`ON UPDATE RESTRICT`)와 자식 UPDATE(다른 부모로 옮김)도 같은 구멍이다.
- **서로를 기다리며 멈춤(데드락) — 전부터 있던 것**: 위 시험을 5세션(부모 DELETE, 자식 INSERT 둘, 부모 키 UPDATE, 자식 UPDATE)으로 넓히자 R16 빌드가 12라운드 가운데 2번 **멈췄다**. 락 매니저에도 기다리는 세션이 없고 잡힌 클레임도 없어서 데이터 락(표 단위 읽기/쓰기 락) 문제였다: `UPDATE`는 후보를 찾는 읽기 단계에서 데이터 락을 "자기 표 먼저, 이웃(외래키로 이어진) 표는 나중에" 두 번에 나눠 잡았는데, 다른 문장들은 모두 표 이름 순서로 한 번에 잡는다. 쓰기를 기다리는 세션이 있으면 새 읽기를 막는 락이라서 `par → kid` 순서로 잡는 UPDATE, `mov → par` 순서로 잡는 UPDATE와 `par` 쓰기를 기다리는 문장이 서로를 기다렸다.

**수정**:
- **외래키 값의 클레임**(R16의 UNIQUE 클레임과 같은 방식, 이름 `d.par#fk:id` = 부모 표와 참조되는 열): 자식 행은 가리키는 부모 값을 **공유**로, 값을 잃는 부모 행(DELETE, 참조되는 열을 바꾸는 UPDATE)은 **배타**로 잡는다. 같은 값의 자식끼리는 서로 기다리지 않는다(공유), 부모가 먼저면 자식이 기다렸다가 부모가 없음을 알고(`Foreign key violation`), 자식이 먼저면 부모가 기다렸다가 처음부터 다시 돌아 자식을 본다(`RESTRICT`면 오류, `CASCADE`면 같이 지움). 자식이 이미 눈에 보이면 기다리지 않고 바로 오류.
- **INSERT**(`executor_dml.cpp`): 외래키 열마다 부모 값 클레임을 잡고, 기다렸다면(데이터 락을 놓았으므로) 부모를 다시 찾는다. 자기 참조 표의 "앞 행이 부모" 규칙은 하나의 함수(`fk_parent_missing`)로 모았다.
- **UPDATE**(`executor_update.cpp`, 도우미 `fk_claims`): 새 값을 갖는 외래키 열(공유)과 참조되는 열에서 잃는 값(배타)을 UNIQUE 클레임과 같은 자리에서 잡고, 못 잡으면 시도를 버리고 기다렸다 처음부터. 읽기 단계의 데이터 락은 **이름 순서로 한 번에** 잡도록 고쳤다(멈춤의 원인).
- **DELETE**(`executor_delete.cpp`): 지울 행의 참조되는 열 값을 배타로 잡고(자식 검사 뒤), 못 잡으면 연쇄 시도를 기다렸다 다시.
- `Executor::referenced_columns`(다른 표가 가리키는 열), `fk_claim_table`.

**검증**:
- 신규 시험(`test_unique_claims.cpp`, 케이스 4개 추가): 5세션이 장벽에 줄을 서서 **같은 순간에** 부모 DELETE·자식 INSERT·부모 키 UPDATE·자식 UPDATE(+ `ON DELETE CASCADE` 자식)를 하고 한 세션이 고아 행을 센다 — R16 빌드의 같은 부하는 12라운드 중 10라운드 고아·2라운드 멈춤, 수정 뒤 0·0(Catch 시험으로 연속 120회 이상, 감시 스레드가 30초 동안 진전이 없으면 각 세션이 하던 문장을 찍고 중단); 결정적 시험: 손으로 잡은 클레임 위에서 자식 INSERT/UPDATE와 부모 DELETE/UPDATE가 **기다리는지**, 풀리면 상대의 행을 **다시 보는지**(`CASCADE`는 새 자식도 지움), 자식끼리는 기다리지 않는지(기다린 뒤에도), 바꾸지 않은 열·NULL·참조되지 않는 열·참조되는 두 열은 올바르게 잡는지, 이미 보이는 자식이 있는 부모의 DELETE는 기다리지 않고 거절되는지, `@lock_wait_timeout`.
- 심은 버그 22종(클레임 분기 전부, 공유/배타 바꾸기, NULL, 자기 참조, 참조되는 열 목록, UPDATE의 변경 판정·기다림·락 순서): 처음 21종 가운데 8종이 살아남았고 — 7종은 시험을 더해 잡았다(자식끼리의 공유·기다린 뒤의 공유·바꾸지 않은 열·참조되지 않는 열·두 번째 참조 열). 남은 1종 `fk_upd_lock_order_old`(UPDATE의 두 번에 나눠 잡는 옛 순서)는 **확률적으로만** 잡힌다 — 그 데드락은 라운드마다 약 1/45 확률이어서 시험이 멈추지 않고 중단되게 한 감시 스레드와 긴 반복(`RUSQL_FUZZ_SEEDS`)이 지키는 방식이다.
- Release **673 케이스/약 1,435,925 assertions**을 기본·강제 양쪽으로 통과, Debug도 같은 673 케이스/약 1,435,895 assertions(기본·강제 양쪽). 긴 캠페인: `[random]` 40시드 249,486, `[fuzz]` 30시드 1,555,183, 외래키 경쟁 25시드, UNIQUE 경쟁 60시드 — 모두 통과.
- 빌드 간 차분(R16 대비) 30시드 × 99질의 차이 0건; 출력 검증 도구 12종 위반 0건(`verify_null_expressions` 시드 4만 R14부터 있던 기준 모델 문제); `test_full.sql`·`test_full-ver2.sql` 스트림 차이 없음; 크래시 퍼저 60라운드 + 동시성 퍼저 20라운드(확인한 행 11,434) 불일치 0건.
- 성능(R16 대비 번갈아 3라운드, 1만 부모 행): 일반 표 일괄 INSERT 61 → 67 / 67 → 65 ms, **자식 표 일괄 INSERT 1만 행 122 → 143 / 125 → 139 / 123 → 139 ms(+12%, 행마다 공유 클레임 하나)**, 자식 단일 INSERT 500개 6.5 s → 6.7 / 6.5 → 6.8 / 6.5 → 6.5 (잡음), 자식 UPDATE 300개·부모 DELETE·UPDATE 300개·연쇄 DELETE는 ±3%(몇 번의 이상치 제외). **같은 측정이 보여 준 전부터의 문제**: 자식 INSERT 하나가 13 ms(일반 표는 2.5 ms) — 부모 존재 확인이 부모 표를 훑는다(1만 행에 O(n)).

**한계**: 아래 R18 후보. `REPLACE`가 지우는 부모 행·`MERGE`의 DELETE·`TRUNCATE`는 클레임을 잡지 않는다(그 경로는 `delete_restrict_violation`만 본다). 외래키 클레임도 명령문(트랜잭션이면 COMMIT)이 끝날 때까지 잡힌다 — 열린 트랜잭션이 어떤 부모의 자식을 쓰면 그 부모의 DELETE는 그 트랜잭션이 끝날 때까지 기다린다(InnoDB와 비슷하지만, 자식 행이 이미 보이면 기다리지 않고 바로 거절).

### 10월 13일 — 실제 `mysqldump` 파일이 읽힘: 102문장 중 92문장이 거절되고 표가 하나도 안 만들어지던 것 (R18)

**왜 이 항목인가**: 사용자 지시("일단 급한 것만 수정", 문서·커밋·푸시까지). 시연에서 가장 먼저 부딪힐 일이 "MySQL에서 뜬 덤프를 읽히는 것"인데, 실제 MySQL 8.4에서 만든 상점 스키마(표 4개, 뷰·프로시저·트리거·외래키·한글)의 덤프를 R17 빌드에 읽히니 **표가 하나도 만들어지지 않았다**. 그 점검 도구(`code/test/diff/dump_roundtrip.py`)가 이 라운드의 합격 기준이다: 실제 MySQL에서 덤프를 뜨고 → RuSQL에 MySQL 프로토콜로 읽히고 → 모든 표의 행을 MySQL과 비교 → 실제 `mysql` 클라이언트(`--force`)로도 읽혀 오류 수를 센다.

**원인과 수정** (한 가지가 아니었다):
- **실행되는 주석** `/*!40101 SET … */`, `/*!50001 CREATE … */`: 실행하지 않고 빈 문장으로 거절했다 → 렉서가 `/*!NNNNN … */`를 서버 버전(8.4.9)이 같거나 새로우면 안의 글을 SQL로, 아니면 버린다(`expand_executable_comments`; 문자열 안은 건드리지 않음). 주석만 있는 문장은 오류가 아니라 빈 OK.
- **`SET`**: `SET @a = 1, @b = @a + 1`(목록), 서버 변수 `SET sql_mode = …`·`SET @@session.x = …`·`SET GLOBAL x = …`, `@@변수` 읽기, `SET NAMES`·`SET CHARACTER SET`, `SET SESSION TRANSACTION ISOLATION LEVEL`을 받는다. 서버 변수는 세션의 `user_vars`에 `@이름`으로 두고(기본값 표 `default_system_variables`), `SELECT @@x`는 세션 값을 돌려준다. **`SET FOREIGN_KEY_CHECKS = 0`이 실제로 외래키 검사를 끈다**(덤프는 표를 알파벳순으로 만들어 아직 없는 표를 가리킨다). ON/OFF/TRUE/FALSE는 1/0.
- **`CREATE TABLE`**: `DEFAULT CURRENT_TIMESTAMP[(n)]`, `DEFAULT (0)`, 명시적 `NULL`, 열의 `CHARACTER SET`·`COLLATE`·`COMMENT`, **`ON UPDATE CURRENT_TIMESTAMP`(UPDATE가 행을 바꾸면 그 열이 문장 시각이 됨 — 열이 직접 대입되면 그 값, 바뀐 게 없으면 그대로)**, 표 옵션 `ENGINE`·`AUTO_INCREMENT = n`(다음 번호가 n)·`[DEFAULT] CHARSET`·`COLLATE`·`COMMENT`·`ROW_FORMAT` 등, `KEY 이름 (열(길이) DESC) USING BTREE`. 데이터베이스의 `DEFAULT CHARACTER SET …`·`DEFAULT ENCRYPTION = 'N'`도.
- **정렬 규칙 정책**: `_ci`로 끝나는 정렬 규칙과 문자 집합 utf8mb4/utf8/utf8mb3/ascii/latin1은 엔진의 기본 비교(`utf8mb4_0900_ai_ci`)로 받아들이고, **`_bin`·`_cs`·`binary`는 거절**한다(받아들이고 무시하면 대소문자 구분이 조용히 뒤집힌다).
- **`CREATE ALGORITHM=… DEFINER=… SQL SECURITY … VIEW|PROCEDURE|FUNCTION|TRIGGER`**: 속성절을 건너뛴다. 프로시저·함수의 특성(`DETERMINISTIC`, `READS SQL DATA`, `COMMENT`, `SQL SECURITY` …)도, 프로시저 매개변수의 아무 타입(`BIGINT`, `DECIMAL(10,2)` …)도. 함수는 `BEGIN RETURN 식; END` 모양을 받는다(문장이 여럿이면 오류).
- **뷰 본문의 괄호 친 조인** `from ((a x join b y on(…)) join c z on(…))`: 왼쪽으로 깊은 조인 묶음의 괄호를 걷어 낸다. 덤프가 처음 만드는 자리표시 뷰 `SELECT 1 AS a`(FROM 없음)도 만들어진다.
- **`ALTER TABLE … DISABLE|ENABLE KEYS`**(없는 표면 오류), **`ADD COLUMN x INT NOT NULL DEFAULT 0`**(제약절을 받아 적용; 전에는 파싱 오류).
- **MySQL 프로토콜 결과 읽기**: 값에 줄바꿈이 있으면 `\n` 두 글자로, `|`가 있으면 칸이 쪼개져 돌아왔다(JDBC·DBeaver·mysql 클라이언트 모두) → 서버가 `\n`·`\|`·`\\`를 풀고 이스케이프된 `|`에서 칸을 나누지 않는다. 문자열 이스케이프 `\b`·`\Z`도.
- **조용히 틀리던 것 하나**: 복합 `FOREIGN KEY (a, b)`가 **첫 열만** 남기고 받아들여졌다 → 이제 "한 열짜리 외래키만 지원"이라는 오류로 거절(전에는 검사가 표가 말하는 것보다 약했다).

**검증**:
- `dump_roundtrip.py`: 상점 덤프 **102문장 중 0문장 거절**, 5개 표 행 모두 MySQL과 같음, 실제 `mysql` 클라이언트 오류 0. 열 종류 덤프(`dump_schemas/types.sql`: `bigint unsigned`, `tinyint(1)`, `double`, `decimal(12,4)`, `char`, `longtext`, `blob`, `enum`, `date`/`datetime`/`timestamp ON UPDATE`, `json`, 복합 PK·복합 UNIQUE, 자기 참조 외래키 `ON DELETE CASCADE`, 트리거, 함수; 값에 `|`·줄바꿈·`\`·따옴표·2^53 넘는 수) 85문장 0 거절, 4개 표 모두 같음. 비교는 UTC 세션에서(아래 한계).
- 신규 시험 `test_dump_syntax.cpp`(6 케이스, 196 assertions): 실행 주석, SET 목록·서버 변수·검사 스위치, 외래키 검사 끄기, 표/열 옵션·정렬 규칙 정책, 시각 기본값·ON UPDATE, 속성절·괄호 조인·DISABLE KEYS·ADD COLUMN·함수·프로시저.
- Release 전체 **679 케이스 / 1,436,091 assertions**을 기본·강제(`RUSQL_DML_INDEX_MIN_ROWS=0`) 양쪽으로 통과. 빌드 간 차분(R17 대비) 10시드 × 99질의 차이 0건; `test_full.sql`·`test_full-ver2.sql` 스트림은 시각·포트·실행 시간만 다름.
- **뒤이어 돌린 검증(같은 날)**: 출력 검증 도구 12종(시드 3·4, 위반 0), 크래시 퍼저 20라운드 + 동시성 퍼저 10라운드(확인한 행 5,824) 불일치 0, `[random]` 40시드 249,733 · `[fuzz]` 30시드 1,555,183 assertions 통과, **심은 버그 34종**(실행 주석 버전·문자열 안, `@@` 범위·소문자, SET 목록·베어 워드·NAMES·TRANSACTION·오류 중단, ON/OFF 정규화·검사 스위치, 서버 변수 기본값·이름 검사, 빈 문장, `AUTO_INCREMENT = n`, DISABLE KEYS의 표 검사, ON UPDATE 4종, 시각 기본값·괄호 기본값, 정렬 규칙·문자 집합 정책, 복합 외래키, ADD COLUMN 기본값, 괄호 조인, DEFINER·SQL SECURITY·특성절·함수 END): 처음 4종이 살아남았고 — 3종은 시험을 더해 잡았고(`SET TRANSACTION ISOLATION LEVEL`, 목록 중간의 오류, 문장 안의 `@@변수`), 1종(`function_block_end_kept`: 함수 본문의 `END`를 요구하는 줄을 빼도 뒤따르는 토큰 때문에 어차피 오류 — 결과가 같은 변이)은 같은 결과라 둠. **아직 안 돌린 것**: Debug 빌드, 성능 번갈아 측정(파서·렉서 변경이라 실행 경로는 그대로이나 `mysql.cpp`의 결과 읽기는 모든 프로토콜 결과를 지난다).
- **CTE의 열 목록** `WITH r (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 5)`: 질의의 select 목록(UNION이면 첫 질의)에 그 이름을 별칭으로 붙인다; 개수가 다르면 오류 1353, `*`를 고르는 질의는 이름을 지어 달라는 오류.
- **`box_table`**(서버가 직접 만드는 SHOW류 표)도 엔진처럼 `\`·줄바꿈·`|`를 이스케이프해, 읽는 쪽이 풀어도 값이 그대로다.

**한계(남긴 것)**: ① `CREATE TABLE` 안의 `KEY 이름 (열)`은 읽고도 색인을 만들지 않는다(답은 같고 느릴 수 있음; 덤프의 `SHOW INDEX`도 비어 있음). ② `TIMESTAMP`는 시간대 변환 없이 받은 글자 그대로 저장한다(MySQL은 UTC로 저장해 세션 시간대로 보여 줌): UTC 덤프를 읽혀도 값은 같지만 다른 시간대의 세션에서 읽은 MySQL과는 다르게 보인다. ③ 외래키 부모 표가 없어도 `CREATE TABLE`이 성공(MySQL은 검사가 켜져 있으면 오류). ④ `FOREIGN_KEY_CHECKS = 0`은 `CREATE TABLE`·`INSERT`·`DROP TABLE`에만 적용(`DELETE`/`UPDATE`의 RESTRICT·CASCADE는 그대로 검사). ⑤ 끝 공백 있는 글자 값은 MySQL 프로토콜 결과에서 앞뒤 공백을 잃는다(NO PAD 한계와 같은 뿌리). ⑥ 함수는 `RETURN 식` 하나만. ⑦ `LIKE '\%'` 이스케이프, 열 `BINARY`, `FULLTEXT KEY`, 생성 열은 지원하지 않는다.

### 10월 13일 (두 번째) — UNIQUE 열·복합 기본키가 있는 표의 여러 행 INSERT가 행마다 표 전체를 훑던 것, 복합 기본키가 대소문자를 구분하던 것 (R19)

**왜 이 항목인가**: R18의 질문에서 사용자가 성능 동결의 예외로 허락했다("네 진행해도 됩니다"). 덤프를 읽는 일과 바로 이어진다 — 상점 샘플 덤프에도 `UNIQUE KEY`가 있고, 큰 표의 덤프는 `INSERT … VALUES (…), (…), …` 여러 행짜리다.

**원인**: 한 INSERT의 각 행이 "표에 같은 값이 이미 있나"를 표 전체를 훑어 확인했다(기본키 한 열만 색인으로 찾음). UNIQUE 열(색인 없음)과 복합 기본키는 행마다 O(표 크기) → 문장 하나가 O(행 × 표). 2만 행을 2,000행짜리 문장 10개로 넣는 데 **UNIQUE 열 9.0초, 복합 기본키 23초**(일반 표 0.12초).

**수정**(`executor_dml.cpp`): 여러 행 문장은 표의 보이는 행으로 **문장당 한 번** 해시를 만든다 — 복합 기본키는 열 값을 정렬 규칙 키로 길이 접두사를 붙여 이은 튜플로, 색인 없는 UNIQUE 열은 `collation::key(값)`으로(NULL은 제외; 같은 키의 첫 행이 이김, 전에 훑던 것과 같은 행) — 행마다 O(1)로 찾는다. 한 행짜리 문장은 해시 만드는 값이 훑는 값과 같아 훑기를 그대로 둔다. 해시가 만든 행은 `IGNORE`·`ON DUPLICATE KEY UPDATE`·`REPLACE`가 쓰는 행과 같다.

**가는 길에 찾아 고친 것(R15가 놓침)**: 복합 기본키 `(a, b)`의 INSERT가 표에 **이미 있는** 행을 **바이트로** 비교했다 — `('Alice', 1)`을 넣은 뒤 `('alice', 1)`이 그대로 들어갔다(MySQL은 `Duplicate entry`). 문장 안의 두 행은 정렬 규칙으로 비교하고 있었다. 이제 한 행이든 여러 행이든 같은 규칙이다.

**검증**:
- 속도(구 빌드 → 새 빌드, 2번 번갈아): 2만 행 **UNIQUE 열 9,059 → 159 ms / 8,957 → 158 ms(약 57배)**, **복합 기본키 23,204 → 150 / 23,302 → 138 ms(약 165배)**, UNIQUE int(500행/문장) 9,839 → 219 / 10,260 → 238 ms, 일반 표 126 → 122 / 121 → 124 ms(변화 없음), 한 행씩 200번: UNIQUE 표 356 → 354 / 348 → 425(잡음), 복합 기본키 731 → 369 / 732 → 580, 일반 표 135 → 131 / 135 → 137.
- 신규 시험(`test_dump_syntax.cpp`, 케이스 2개): **여러 행 문장 하나와 같은 행을 한 줄씩 넣은 쌍둥이 표가 같은 표를 남긴다**(기본키·대소문자가 다른 UNIQUE 열·NULL·복합 키가 겹치는 모든 경우 × 오류·`IGNORE`·`ON DUPLICATE KEY UPDATE`·`REPLACE`, 150단계 × 시드; 쌍둥이로는 못 잡는 불변식도: 정렬 규칙으로 같은 키가 둘 없음, `REPLACE`가 쓴 행이 남음); 복합 기본키의 대소문자 중복(한 행·여러 행); 복합 기본키 `REPLACE`가 참조된 행을 만나면 문장 전체를 거절하고 아무것도 바꾸지 않음.
- 심은 버그 10종(해시를 바이트로 만들기·바이트로 찾기, 지워진 행 포함 2종, 해시가 절대 안 맞음 2종, 훑기를 바이트로 비교, `REPLACE`/`ON DUPLICATE`의 충돌 행 처리 빼기 2종): 처음 1종이 살아남았고(복합 키 `REPLACE`가 참조된 행을 미리 거절하는 점검) 위의 마지막 시험으로 잡았다.
- Release 전체 **682 케이스 / 1,445,713 assertions**을 기본·강제 양쪽으로 통과, `[bulk_unique]` 60시드(193,469), `[unique_claim]` 25시드, **`[random]` 40시드(378,850)** 통과; 빌드 간 차분(R18 대비) 10시드 × 99질의 차이 0건; `test_full*.sql` 스트림은 시각·포트만 다름; 크래시 퍼저 20라운드 + 동시성 퍼저 10라운드(확인한 행 5,754) 불일치 0건; 덤프 점검(`dump_roundtrip.py`) 두 덤프 모두 거절 0·행 일치·`mysql` 클라이언트 오류 0.
- 이번에 안 돌린 것: Debug 빌드, `[fuzz]` 긴 캠페인, 검증 도구 12종(질의 경로는 안 건드림), 성능 3회 번갈아(2회만).

**한계**: ① **한 행씩 넣는 INSERT는 여전히 표를 훑는다**(UNIQUE 표 2만 행에서 한 문장 약 1.7 ms, 복합 기본키 약 3.5 ms): `--skip-extended-insert`로 뜬 덤프는 행마다 한 문장이라 이 길로 가며, 고치려면 UNIQUE 값의 지속 색인(UPDATE·DELETE·롤백이 모두 유지)이 필요하다. ② UPDATE의 복합 UNIQUE 검사(`update_unique_violation`)와 외래키 부모 확인은 그대로. ③ 자식 표의 한 행짜리 INSERT는 부모·자식 크기와 상관없는 고정 비용이 약 7 ms(일반 표 0.7 ms) — 표 크기에 비례하는 부분은 1만 행당 약 4 ms로 작다(R17 이전부터).

### 10월 13일 (세 번째) — `DROP TABLE`이 표의 색인을 남겨서, 같은 이름으로 다시 만든 표가 옛 표의 행을 돌려주던 것 (R20 시험 중 발견)

**발견 경위**: R20(아래 네 번째 항목)의 임의 시험이 시드마다 `DROP TABLE IF EXISTS`로 표를 지우고 같은 이름으로 다시 만드는데, 두 번째 시드에서 `INSERT INTO uu_b VALUES (1, 'y', 10)`이 "Duplicate value 'y' for column 'u'"로 거절됐다. 표는 비어 있었다.

**증상(실제 서버로 확인)**: 색인(해시·보조 B+Tree·복합)이 있는 표를 `DROP TABLE`하고 같은 이름으로 다시 만들면 새 표가 **옛 표의 색인을 그대로 통해 읽혔다** — `SELECT id FROM t WHERE u = 'y'`가 새 표에 없는 행(옛 표의 1번)을 돌려주고, 색인 종류를 바꿔 가며 되풀이하면 같은 행이 2번·3번 나오고(`WHERE u = 'z'`는 새 표의 행이 두 번), 해시 색인이 있으면 옛 표가 가졌던 UNIQUE 값의 INSERT가 중복으로 거절됐다. 서버를 재시작해도 `indexes.json`에 옛 색인 메타가 남아 있어 새 표에 색인이 되살아났다. `DROP DATABASE`는 색인을 지우고 있었고 `DROP TABLE`만 지우지 않았다.

**수정**(`executor_ddl.cpp`의 `exec_drop`): 표의 보조 B+Tree·해시·복합 색인(메타와 본체)을 함께 지우고 색인 메타 파일을 다시 쓴다.

**검증**: 신규 시험 `DROP TABLE takes the indexes of the table with it`(색인 3종 × 지우고 다시 만들기 × 질의·UNIQUE 값 INSERT·색인 맵·재시작, 다른 표의 색인은 남음). 심은 버그 11종 중 9종이 잡혔고, 2종(옛 `.idx` 파일 삭제, 버퍼풀 무효화)은 색인이 이제 디스크·버퍼풀에 쓰이지 않아 결과가 같은 줄이라 수정에서 뺐다. 전체 시험은 아래 네 번째 항목의 것과 같다(두 항목을 한 번에 돌렸다).

### 10월 13일 (네 번째) — UNIQUE 열 하나를 바꾸는 여러 행 UPDATE가 바뀐 행마다 표 전체를 훑던 것 (R20)

**왜 이 항목인가**: R19 끝에 사용자가 성능 동결의 예외로 허락한 3항목(UPDATE의 UNIQUE 검사, 자식 표의 한 행 INSERT, 한 행씩 넣는 INSERT의 UNIQUE 검사) 중 첫째.

**계획의 사실 정정**: R19의 한계에는 "UPDATE의 복합 UNIQUE 검사(`update_unique_violation`)가 행마다 훑는다"고 적었지만 재 보니 **복합 키(복합 기본키·`UNIQUE (a, b)`)는 이미 문장당 한 번만** 훑고 있었다(2만 행 표에서 2000행을 바꾸는 UPDATE: 복합 UNIQUE 31 ms, 복합 기본키 29 ms, 키 없는 대조군 34 ms). 행마다 훑던 것은 **색인 없는 UNIQUE 열 하나**였다: 새 값마다 `index_or_scan_exists`가 표를 처음부터 훑어(색인이 있어도 보조 B+Tree는 "있다"만 확인하고 "없다"는 못 가림) 2000행 UPDATE가 1,329 ms.

**수정**(`executor_update_unique.cpp`): 찾아볼 새 값이 둘 이상이고 그 열의 "없다"를 색인이 답하지 못하면(단일 기본키의 트리·해시 색인만 답함: `lookup_scans`) 표를 **한 번** 훑으면서 새 값들의 정렬 규칙 키 집합에 드는 값을 모아 비교한다. 훑기와 같은 판정(살아 있는 행, 정렬 규칙 동등)이고, 오류는 전과 같이 문장에서 처음 부딪히는 행의 값을 말한다.

**속도**(구 빌드 → 새 빌드, 3번 번갈아): 2만 행 표에서 UNIQUE 열 2000행 UPDATE **1,329 → 26 ms (51배)**, 복합 UNIQUE 31 → 32 ms, 복합 기본키 29 → 28 ms, 일반 표 UPDATE 2000행 20 → 20 ms · 한 행씩 135 → 133 ms · DELETE 22 → 22 ms / 135 → 136 ms · INSERT 190 → 191 ms / 197 → 199 ms(변화 없음; 스캔·GROUP BY는 ±잡음).

**검증**: 신규 시험 `test_update_unique.cpp` — ① 임의 UPDATE(여러 행이 각자 단어를 받음·NULL·대소문자만 바꿈·단어 둘이 자리를 바꿈·한 행·다른 열) 120단계를 모델과 비교하는데, 같은 문장을 **해시 색인이 있는 표(값마다 색인에 물음)와 없는 표(한 번 훑기)**에 돌려 둘의 오류 메시지까지 같은지 본다(60시드 49,806 assertions); ② 600행에서 줄줄이 옮기는 문장은 되고, 끝이 안 비는 문장은 문장의 첫 충돌 값(`602`, `902`)을 말하며 표를 바꾸지 않는다. 심은 버그 12종 중 9종이 잡혔고, 3종은 결과가 같다(값이 하나일 때 한 번 훑기를 쓰기·색인을 무시하고 한 번 훑기·열이 없는 행 처리 — 앞의 둘은 속도만 다르고 마지막은 모든 행이 모든 열을 가지므로 닿지 않는 방어 줄).
- Release 전체 **685 케이스 / 1,448,284 assertions**을 기본·강제 양쪽으로 통과, `[random]` 40시드(411,950), `[unique_claim]` 25시드 통과; 빌드 간 차분(R19 대비) 10시드 × 99질의 차이 0건; `verify_writes.py` 시드 1~8(문장 3000개씩) 위반 0; 크래시 퍼저 20라운드 + 동시성 퍼저 10라운드(확인한 행 5,388) 불일치 0; Debug로 바뀐 시험(`[update_unique]`·`[ddl]`·`[composite_unique]`·`[bulk_unique]`) 기본·강제 통과.
- **Debug 전체 시험(R19 커밋 기준)**: 682 케이스 중 681 통과, 1건 실패 — 작업 트리에 커밋되지 않은 채 남아 있던 `test_composite_unique.cpp`의 중복 블록(같은 파일 131~137행의 `CREATE TABLE cs …`를 한 번 더 넣어 "Table 'd.cs' already exists")이 원인이고 엔진 코드와 무관하다. 그 변경은 `git stash`에 보관했다(`git stash list`).

**한계**: 키 열을 바꾸는 UPDATE는 문장당 한 번 표를 훑는다(한 행짜리도; 2만 행 표에서 약 5~10 ms). 이것을 없애려면 UPDATE·DELETE도 지속 색인을 유지해야 한다.

### 10월 13일 (다섯 번째) — 외래키가 있는 자식 표의 한 행 INSERT가 문장마다 두 표 파일을 다시 쓰고 fsync하던 것 (R21)

**왜 이 항목인가**: 성능 동결의 예외 3항목 중 둘째. R19의 한계에는 "자식 표의 한 행짜리 INSERT는 표 크기와 상관없는 고정 비용이 약 7 ms(일반 표 0.7 ms)"라고만 적었고 원인은 모른 채였다.

**원인(단계별로 따라가 찾음)**: 같은 열을 가진 외래키 없는 표(0.66 ms)와 외래키 있는 표(5.2 ms)를 나란히 재니 차이가 외래키 자체에서 왔다. 명령문이 끝난 뒤의 내구성 처리(`Executor::execute`의 `covered`)가 "잠금 집합이 정확히 한 표"인 INSERT/UPDATE/DELETE만 redo 로그 한 배치(append + 묶음 fsync)로 처리하고, 나머지는 **옛 방식 — 문장에 걸린 모든 표의 파일을 다시 쓰고 `TableFlushed` 표시를 fsync**한다. 자식 표의 INSERT는 부모의 존재를 읽으려고 부모 표가 잠금 집합에 들어가 두 표가 되어 옛 방식으로 갔다. 그 방식은 자식이 아니라 **부모까지** 매번 다시 쓰므로 부모가 클수록 비싸다(부모 1,000행 5.2 ms, 2만 행 12.6 ms). 구조 문제가 아니라 조건 한 줄이었다: INSERT는 부모를 **읽기만** 하고 자기 표에만 쓰며 그 변경은 redo 연산이 다 설명한다. UPDATE/DELETE는 자식 표들을 잠그는데, 그쪽은 연쇄(CASCADE)가 로그에 안 남으므로 그대로 둔다.

**수정**(`executor_core.cpp`): `covered`의 "한 표" 조건을 "한 표이거나 INSERT"로. 옛 방식이 남는 것: `INSERT … SELECT`·`REPLACE`·`ON DUPLICATE KEY UPDATE`(원래 `is_redo_covered_kind`가 아님), 자식을 건드리는 UPDATE/DELETE. 트랜잭션 안에서도 자식 INSERT가 이제 "redo로 설명 안 되는 문장" 표시(`mark_redo_incomplete`)를 만들지 않아 COMMIT이 표를 다시 쓰지 않고 로그만 붙인다.

**속도**(구 빌드 → 새 빌드, 번갈아 3번): 자식 표 한 행 INSERT 부모 1,000행 **5.2 → 0.67 ms**, 부모 2만 행 **12.6 → 0.69 ms**(일반 표 0.68 ms와 같음); 자식 200건 한 트랜잭션 + COMMIT 49.6 → 51.7 ms(잡음; 각 INSERT 0.2 ms); 일반 표 INSERT 197 → 198 ms, UPDATE 19.0 → 19.2 ms, DELETE 135 → 135 ms, SELECT 스캔 58 → 57 ms(변화 없음).

**검증**:
- 신규 시험 2개(`test_redo.cpp`): ① 자식 INSERT(한 행·두 행·트랜잭션 COMMIT)가 **표 파일(.rdb)을 하나도 만들지 않고** 로그에 남고 크래시 뒤 살아 있으며(ROLLBACK된 행·열린 트랜잭션의 행은 없음), 없는 부모는 여전히 거절되고 복구된 표에서 연쇄가 동작한다; ② `ON UPDATE CASCADE`·`ON DELETE CASCADE`와 자식의 UPDATE가 **로그된 자식 INSERT 바로 뒤에서** 크래시에도 살아남는다(뒤의 문장이 표를 다시 써 주는 일이 없도록 시험 대상 문장으로 끝낸다 — 처음 버전은 뒤의 연쇄가 앞의 것을 가려 심은 버그가 살아남았다).
- 신규 도구 `code/test/crash/fuzz_crash_fk.py`(외래키가 있는 `p`/`c` 표에 부모·자식 INSERT·UPDATE·DELETE, 트랜잭션 COMMIT/ROLLBACK을 섞고 kill -9 뒤 비교; 없는 부모의 자식과 자식이 있는 부모의 삭제가 복구 뒤에도 거절되는지 확인): 구·신 빌드 모두 60라운드 불일치 0.
- 심은 버그 5종: 자식 INSERT를 다시 옛 방식으로, 모든 문장을 로그 방식으로, UPDATE도, DELETE도(전부 잡힘), `INSERT … SELECT`도(`is_redo_covered_kind`가 이미 걸러 결과가 같음).
- Release 전체 **687 케이스 / 1,448,383 assertions**을 기본·강제 양쪽으로 통과(강제는 1,448,382), `[random]` 40시드, `[unique_claim]` 25시드 통과; 빌드 간 차분(R20 대비) 10시드 × 99질의 차이 0건; `verify_writes.py` 시드 1~8 위반 0; 크래시 퍼저 20라운드 + 동시성 퍼저 10라운드(확인한 행 5,499) 불일치 0; Debug로 `[redo]`·`[durability]` 기본·강제 통과.

**한계(남긴 것)**: **자식 표의 UPDATE와 부모 표의 UPDATE/DELETE는 여전히 옛 방식**이다 — 자식의 외래키 열을 바꾸는 UPDATE 약 16 ms, 자식이 있는 부모의 UPDATE 약 40 ms, 부모 DELETE(연쇄) 약 27 ms(2만 행 표, 한 문장). 연쇄가 로그에 안 남기 때문이고, 연쇄를 로그에 남기거나 연쇄할 자식이 없는 문장을 가려내야 고칠 수 있다.

### 10월 13일 (여섯 번째) — 한 행씩 넣는 INSERT가 UNIQUE 열·복합 기본키·`UNIQUE (a, b)`를 찾느라 표를 훑던 것 (R22)

**왜 이 항목인가**: 성능 동결의 예외 3항목 중 셋째. `mysqldump --skip-extended-insert`로 뜬 덤프는 행마다 한 문장이라 R19의 "문장당 한 번 만든 해시"가 소용없었고, 2만 행 표에서 문장 하나가 UNIQUE 열 약 2 ms, `UNIQUE (a, b)` 약 7 ms(이 키는 한 행짜리 문장도 표 전체로 해시를 만들었다), 복합 기본키 약 2 ms였다.

**설계(보고하고 진행)**: 기존 `hash_indexes`/`composite_indexes`를 UNIQUE에 자동으로 두지 않았다 — 이름·`SHOW INDEX`·플래너의 접근 경로 선택·덤프·`DROP INDEX`로 새고, NULL이 한 버킷에 쌓이고, 버킷의 `front()`만 보는 기존 검사 문제를 끌어온다. 대신 **표마다 보이지 않는 파생 캐시** `UniqueProbe`(`engine/unique_probe.hpp`, `executor_unique_probe.cpp`)를 둔다: 키(정렬 규칙 키; 복합은 길이 접두사를 붙여 이음) → 살아 있는(`_xmax = 0`, 스캔과 같은 정의) 행의 수. 대상은 기본키가 아닌 UNIQUE 열, 복합 기본키, `UNIQUE (a, b)`.
- **쓰임**: 한 행 INSERT의 첫 중복 검사에서 캐시가 "그 키를 가진 살아 있는 행이 없다"를 **증명할 때만** 스캔을 건너뛴다. "있을 수 있다"면 전과 같은 스캔이 돈다(`IGNORE`·`ON DUPLICATE KEY UPDATE`·`REPLACE`가 쓰는 충돌 행도 그 스캔이 찾는다). 그래서 거짓 충돌은 있을 수 없고, 놓침은 캐시가 살아 있는 키를 빠뜨릴 때뿐이다. 잠금을 기다린 뒤의 재검사(R16)는 스캔 그대로.
- **낡음 감지는 추측하지 않고 잠금으로**: `FairSharedMutex`가 독점으로 잡힌 횟수(`exclusive_count`)와 객체 id(`instance_id`)를 센다. 캐시는 만들어진 때의 (표 잠금 + 표 데이터 잠금 + 구조 잠금)의 횟수 합을 기억하고, 합이 그대로일 때만 "최신"이다. 행을 바꾸는 모든 문장(UPDATE·DELETE·ROLLBACK·ROLLBACK TO·COMMIT·VACUUM·TRUNCATE·DDL·RESTORE)은 이 셋 중 하나를 독점으로 잡으므로 **그 코드를 건드리지 않아도 캐시는 자동으로 낡은 것이 된다**(롤백·재시작·MVCC·락 순서는 바꾸지 않았다; 재시작 뒤에는 빈 캐시에서 시작).
- **제자리 갱신은 INSERT만**: INSERT가 자기 독점 구간(표 데이터 잠금)에서 합이 정확히 +1일 때(그동안 다른 쓰기가 없었을 때) 넣는 행의 키를 더하고, 아니면 캐시를 버린다.
- **언제 만드나**: 낡은 캐시는 INSERT만 이어질 때(연속 2회, 표의 잠금 횟수가 INSERT가 센 횟수와 같게만 늘어난 경우)에 O(표)로 다시 만든다. UPDATE/DELETE와 번갈아 오는 표는 전과 같은 속도로 스캔한다(캐시를 매번 만들면 오히려 느려진다).
- **`UNIQUE (a, b)`**: 문장 시작에 표 전체로 해시를 만들던 것을 "처음 필요할 때"로 미뤘다(여러 행 문장은 전과 같이 먼저 만든다).
- **점검 장치**: `RUSQL_PROBE_CHECK=1`이면 캐시를 쓸 때마다(그리고 INSERT가 갱신한 뒤) 표의 행을 다시 세어 비교하고 다르면 오류를 낸다; `RUSQL_PROBE_STREAK=n`(만들기까지의 INSERT 연속 횟수), `RUSQL_UNIQUE_PROBE=0`(끔). `DROP TABLE`/`DROP DATABASE`는 그 표의 캐시를 지운다.

**속도**(구 빌드 → 새 빌드, 번갈아 3번, 2만 행 표, 한 행 INSERT 200번): **UNIQUE 열 402 → 142 ms**(문장당 2.0 → 0.71 ms), **`UNIQUE (a, b)` 1,388 → 158 ms**(6.9 → 0.79 ms), **복합 기본키 401 → 145 ms**(2.0 → 0.73 ms; 일반 표는 0.68 ms). 일반 표 INSERT 196 → 196 ms · bulk 193 → 191 ms, UPDATE 19.8 → 20.0 ms · 한 행씩 134 → 133 ms, DELETE 21.6 → 21.4 ms · 135 → 133 ms, SELECT 스캔 59 → 61 ms(잡음 안), UNIQUE 표의 UPDATE 26 → 26 ms / 32 → 32 ms(변화 없음).

**검증**:
- 신규 시험 `test_unique_probe.cpp`(4 케이스): ① 센 횟수로 본 동작 — 연속 INSERT에서 만들어지고 스캔을 건너뛴 횟수, 있는 값은 스캔이 거절, INSERT·DELETE가 번갈아 오면 만들지 않음, 삭제된 값·트랜잭션이 지운 행은 세지 않음, 복합 기본키·`UNIQUE (a, b)`도; ② **낡은 캐시를 믿지 않음** — 키 종류 3가지 × (지운 행을 ROLLBACK이 되살림, 캐시가 만들어진 뒤 ROLLBACK이 행만 되살림, 세이브포인트 ROLLBACK TO, 키를 옮기는 UPDATE, 롤백된 INSERT, TRUNCATE, DROP TABLE 뒤 같은 이름, 재시작); ③ **캐시를 켠 엔진과 끈 엔진에 같은 임의 문장 열**(한 행·여러 행·`IGNORE`·`ON DUPLICATE KEY UPDATE`·`REPLACE`·UPDATE·DELETE·BEGIN/COMMIT/ROLLBACK/SAVEPOINT/ROLLBACK TO·TRUNCATE·재시작, UNIQUE 열 둘과 `UNIQUE (a, b)`와 복합 기본키, NULL·대소문자 섞임)를 넣어 매 문장의 성공/오류 메시지와 표 내용이 같은지 본다(켠 쪽은 매번 행을 다시 세어 비교; 60시드 33,044 assertions); ④ 세션 6개가 같은 표에 INSERT·UPDATE·DELETE·REPLACE·IGNORE·트랜잭션을 섞어 동시에 쓴 뒤 같은 값이 둘 있는지 본다(60라운드).
- **전체 시험을 점검 모드로**: `RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0`(처음부터 캐시를 만들고 매번 비교)으로 기본·강제 양쪽 **690 케이스 통과, 불일치 0**(기존 시험 전부가 캐시 유지 누락을 찾는 역할을 했다); `[random]`·`[unique_claim]`·`verify_writes.py` 8시드도 점검 모드로 통과.
- 심은 버그 27종: 캐시에 넣는 행을 빼기, 합의 조건을 느슨하게/엄격하게, 옛 합이 맞는지 보지 않기, 정렬 규칙 키 대신 바이트, 복합 키 길이 접두사, 죽은 행 세기, "없음"을 뒤집기/항상 "없음", 엉뚱한 키 고르기, 연속 조건 무시, 만든 직후 도장 틀림, INSERT가 갱신 호출을 빼기/빈 행을 넘기기, 키 열 이름 틀림, `UNIQUE (a, b)` 지연 생성 건너뛰기, **`lock()`이 세지 않기, 구조 잠금/표 잠금/데이터 잠금 횟수를 합에서 빼기**: 결과가 달라지는 것은 모두 잡혔다. 5종이 살아남았고 모두 결과가 같은 방어 줄이다 — 객체 id 비교 2종과 id를 상수로 만들기, `DROP TABLE`의 캐시 지우기(DDL은 항상 구조 잠금을 독점으로 잡아 합이 이미 바뀌므로 id와 지우기는 겹치는 안전장치), 복합 키의 길이 접두사(없어도 같은 키는 같은 문자열이라 "없음"을 잘못 말할 수 없고 "있음"이 늘 뿐).
- Release 전체 **691 케이스 / 1,450,764 assertions**을 기본·강제 양쪽으로 통과(강제는 1,450,763), `[random]` 40시드(433,761)·`[unique_claim]` 25시드 통과(점검 모드도), `[composite_unique]` 40시드; 빌드 간 차분(R21 대비) 10시드 × 99질의 차이 0건; `verify_writes.py` 시드 1~8 위반 0(일반·점검 모드); 크래시 퍼저 20라운드 + 동시성 퍼저 10라운드(확인한 행 5,715) + 외래키 퍼저 30라운드 + 점검 모드 10라운드 불일치 0. **Debug 빌드 전체 시험도 691 케이스 통과**(캐시 점검 모드·`STREAK=0`으로 기본 1,450,785, 강제 모드 1,450,784 assertions) — 앞의 A 항목에서 "Debug 전체 681/682"였던 것(미커밋 중복 블록)이 이 트리에서는 깨끗이 통과한다.

**시험이 찾은, 이번 변경과 무관한 기존 문제(고치지 않고 `PLAN.md`에 적음)**: 세션 하나가 트랜잭션 안에서 UNIQUE 값을 가진 행을 DELETE(키를 UPDATE해도 같다)하고 끝내지 않은 사이에 다른 세션이 그 값을 INSERT하면 들어가고(엔진은 지워진 행을 없는 행으로 본다), 앞 세션이 ROLLBACK하면 **같은 UNIQUE 값을 가진 행이 둘**이 된다. R19 빌드로 재현했고 캐시를 끄고도 같다(InnoDB는 두 번째 INSERT가 앞 트랜잭션이 끝나기를 기다린다). 그래서 동시 시험의 트랜잭션 안에는 INSERT만 넣었다.

**한계**: 여러 행 문장(R19의 문장당 해시)과 UPDATE의 키 검사는 캐시를 쓰지 않는다. 캐시는 메모리에 키마다 문자열 하나를 더 쓴다(2만 행 · 키 3개에서 수 MB 이하). INSERT 연속이 끊기면 다시 만들 때까지(최대 INSERT 두 번) 스캔한다. `INSERT IGNORE`·`ON DUPLICATE KEY UPDATE`·`REPLACE`·`INSERT … SELECT`는 문장 뒤 옛 방식 저장이 표 잠금을 독점으로 잡아(문장당 약 14 ms는 그 저장 비용) 캐시가 매번 낡으므로 캐시를 만들지 않고 전처럼 스캔한다.

### 10월 13일 (일곱 번째) — 되돌려야 할 변경이 되돌려지지 않던 것: 연쇄(CASCADE)의 ROLLBACK, 미커밋 DELETE가 푼 UNIQUE 값, 세이브포인트 되돌리기, 그리고 연쇄·IGNORE·REPLACE·UPDATE의 저장 비용 (R23)

**왜 이 항목인가**: 사용자 "고치지 않은 거 고쳐주세요". 앞 항목들에서 "고치지 않음/범위 밖"으로 적은 것은 두 덩어리였다 — ① 정합성: 미커밋 DELETE가 UNIQUE 값을 풀어 다른 세션이 같은 값을 넣고 ROLLBACK 뒤 같은 값의 행이 둘이 되는 것; ② 속도: 자식 UPDATE(약 22 ms)·부모 UPDATE/DELETE(약 40/32 ms)·`REPLACE`/`INSERT IGNORE`/`ON DUPLICATE KEY UPDATE`/`INSERT … SELECT`(5~11 ms)가 문장마다 표 파일을 다시 쓰는 옛 방식을 탐. ①을 시험하다가 더 큰 정합성 문제 셋을 찾아 같이 고쳤다.

**고친 것 (정합성)**
1. **미커밋 DELETE/UPDATE가 푼 UNIQUE 값** — 트랜잭션 안의 `DELETE`와 키를 옮기는 `UPDATE`(그리고 `ON DELETE CASCADE`로 지워지는 자식 행)가 지우거나 비우는 UNIQUE 값(UNIQUE 열·`UNIQUE (a, b)`·복합 기본키)의 클레임을 **트랜잭션이 끝날 때까지 쥔다**(기본키는 원래 그랬다). 같은 값을 INSERT/UPDATE하는 세션은 기다리고, 끝나면 다시 보아 ROLLBACK이면 중복 오류, COMMIT이면 들어간다(InnoDB와 같음). 바꾸지 않은 키는 쥐지 않아 기다리지 않는다. 서로 기다리면 교착으로 알려 준다.
2. **연쇄(CASCADE/SET NULL/SET DEFAULT)가 트랜잭션의 일부가 아니었음** — 자식 행을 제자리에서 고치고 지웠을 뿐 트랜잭션 관리자에 알리지 않아서: `BEGIN; DELETE FROM p …; ROLLBACK`이 부모만 돌려주고 **자식은 사라진 채**, `SET NULL`은 NULL인 채, `ON UPDATE CASCADE`는 **부모에 없는 키를 가리키는 고아 자식**을 남겼다(`ROLLBACK TO SAVEPOINT`도 같음). 이미 시작된 REPEATABLE READ 스냅샷도 자식의 제자리 수정을 그대로 봤고, 크래시 뒤 redo가 연쇄를 다시 만들 수도 없었다. 이제 `Executor::cascade_write`(`executor_cascade.cpp`)가 자식 표의 UPDATE/DELETE처럼 쓴다: 옛 버전은 `_xmax`로 끝내고 새 버전을 붙이고(지우는 행은 끝내기만, 아무도 못 보는 자동 커밋 삭제는 진짜 삭제), `log_update`/`log_delete`로 되돌리기 로그·redo에 남기고, PK 트리·보조·해시·복합 색인과 위치 캐시를 맞춘다.
3. **`ROLLBACK TO SAVEPOINT`가 행을 둘로 만들던 두 경우**(연쇄 시험 중 발견, 외래키와 무관) — 기본키를 바꾼 `UPDATE`를 되돌릴 때 새 버전을 옛 키로 찾아 못 지움(`UndoEntry::new_key` 추가), 같은 트랜잭션에서 `UPDATE` 뒤 `DELETE`한 행의 DELETE를 되돌릴 때 같은 키·같은 `_xmax`의 버전을 전부 되살림(이제 지운 버전만).

**고친 것 (속도)** — `is_redo_covered_kind`/`covered`: 연쇄까지 로그에 남으므로 잠금 집합이 한 표인지는 더 묻지 않는다. `INSERT`(IGNORE·ON DUPLICATE KEY UPDATE·REPLACE 포함)·`INSERT … SELECT`·`UPDATE`·`DELETE`는 전부 redo 한 배치. (MERGE와 다중 표 UPDATE/DELETE는 옛 방식 그대로.) 연쇄가 대는 `write_through`는 로그가 덮지 않는 문장에서만 남음.

**속도**(구 빌드 → 새 빌드, 번갈아 3번; 2만 행 부모, 5,000행 자식, 문장 하나): 자식 `UPDATE` **22.7 → 0.7 ms**, 부모 `UPDATE v`(자식 있음) **38.9 → 0.7**, 부모 `UPDATE id`(연쇄) **45.1 → 0.9**, 부모 `DELETE`(연쇄) **32.0 → 5.1**, `REPLACE` 5.3 → 1.8, `INSERT IGNORE` 4.8 → 0.7, `ON DUPLICATE KEY UPDATE` 10.8 → 1.2, `INSERT … SELECT`(10행) 5.4 → 1.5. 다른 경로는 변화 없음(일반 표 INSERT 197 → 200 ms·UPDATE 20.3 → 20.2·DELETE 136 → 139, UNIQUE 표 INSERT/UPDATE 같음; GROUP BY·스캔은 ±잡음). 연쇄 삭제 뒤 위치 캐시를 지워 버려 이후 자식 DELETE가 0.66 → 1.1 ms로 느려지던 것을 시험 중 찾아, 캐시를 비우기만 하도록 고쳤다(0.66).

**검증**:
- 신규 시험: `test_uncommitted_unique.cpp`(UNIQUE 열·`UNIQUE (a, b)`·복합 기본키 × DELETE/키 옮기는 UPDATE × ROLLBACK/COMMIT × INSERT/INSERT IGNORE, 바꾸지 않은 키는 기다리지 않음, 교착은 알림, 연쇄 삭제의 자식 값), `test_cascade_rollback.cpp`(연쇄 6가지 × ROLLBACK/ROLLBACK TO/COMMIT/트랜잭션 없음, 모든 색인으로 본 자식 행, 이미 시작한 스냅샷은 옛 자식 행을 봄, 부모·자식 임의 문장 220단계를 RESTRICT/CASCADE/SET NULL 9조합의 모델과 비교하는 임의 시험 80시드 243,777 assertions, 세이브포인트 두 경우), `test_redo.cpp`(IGNORE·ODKU·REPLACE·INSERT…SELECT·UPDATE/DELETE·연쇄가 표 파일을 쓰지 않고 마지막 문장이어도 크래시에 살아남음). 옛 시험 하나(`test_unique_claims.cpp`)가 "트랜잭션이 지운 행의 값은 바로 쓸 수 있다"는 바로 이 버그를 기대하고 있어 "지우고 커밋한 행의 값"으로 바꿨다.
- `fuzz_crash_fk.py`를 키운 크래시 퍼저: 외래키 동작 9조합(RESTRICT/CASCADE/SET NULL × 둘)을 시드로 돌리고 자식·부모 INSERT/REPLACE/IGNORE/ODKU/UPDATE/DELETE, 트랜잭션·세이브포인트, kill -9 뒤 모델과 비교 — 새 빌드 90라운드 불일치 0(옛 빌드는 ROLLBACK TO 뒤 RESTRICT가 깨지는 시드가 있음).
- 심은 버그 24종: 21종 잡힘. 살아남은 3종은 결과가 같다 — 삭제 클레임 충돌 무시(PK 클레임이 먼저 막아 닿지 않음), `ROLLBACK TO`의 `break`(같은 내용의 행이 둘일 때뿐), MERGE를 로그 대상으로(MERGE도 로그되는 문장으로만 쓰여 시험이 같은 결과 — 속도만의 문제라 건드리지 않음).
- Release 전체 **700 케이스 / 1,479,621 assertions**을 기본·강제 양쪽으로 통과, `[random]` 40시드(554,672)·`[unique_claim]` 25시드·`[unique_probe]` 60시드 통과, 캐시 점검 모드 전체 통과; 빌드 간 차분(R22 대비) 10시드 × 99질의 차이 0건; `verify_writes.py` 시드 1~8 위반 0; 크래시 퍼저 20라운드 + 동시성 퍼저 10라운드(확인한 행 5,700) 불일치 0; **Debug 빌드 전체 시험도 700 케이스를 기본·강제 양쪽으로 통과**.

**한계**: 연쇄가 자식에게 만드는 새 버전은 `UPDATE`처럼 죽은 옛 버전을 남기고 자동 VACUUM이 치운다(연쇄만 받는 자식 표는 자기 DML이 없으면 VACUUM 횟수가 늘지 않음). 자기 자신을 가리키는 외래키의 연쇄는 전부터 없다. `SET NULL`/`SET DEFAULT`가 비우는 값은 새 클레임을 쥐지 않는다(그 값의 부모가 없어서 어떤 자식도 그 값을 쓸 수 없음).

### 10월 13일 (여덟 번째) — 트랜잭션 도중에 표 파일이 쓰이면 ROLLBACK한 행이 크래시 뒤에 되살아나던 것, 그리고 UNIQUE 캐시가 UPDATE·DELETE·COMMIT·연쇄를 따라가고 여러 행 INSERT·키 UPDATE도 쓰게 한 것 (R24)

**왜 이 항목인가**: 사용자 "고치지 않은 거 고쳐주세요"의 나머지. R22·R23 끝에 "고치지 않음"으로 적은 것은 ① 한 행 INSERT의 UNIQUE 캐시가 UPDATE·DELETE·ROLLBACK·COMMIT마다 낡아져서, 섞어 쓰는 표에서는 INSERT가 다시 표를 훑는 것과 여러 행 INSERT·UPDATE의 키 검사가 캐시를 쓰지 않는 것, ② 열두 검증기 중 `verify_null_expressions.py`가 시드 4에서 실패하던 것(옛 모델)이었다. ①을 시험하려고 외래키 크래시 퍼저(`fuzz_crash_fk.py`)를 시드 6000~6044로 키워 돌리다가 시드 6007에서 불일치가 나왔고, 아래 정합성 문제가 그 원인이었다.

**고친 것 (정합성) — 표 파일에는 커밋된 것만 쓴다**
- **증상**: `BEGIN; INSERT …; SAVEPOINT s; ROLLBACK TO s; ROLLBACK;` 뒤에 서버가 죽으면 INSERT한 행이 되살아났다(UPDATE·DELETE한 행도 바뀐 채로). 다른 세션이 트랜잭션이 열려 있는 동안 체크포인트(redo 로그가 커져서)나 표 파일을 쓰는 문장(`MERGE`, DDL, `USE`처럼 로그가 덮지 않는 문장)을 실행해도 같다. R22·R23 빌드에서도 재현되는 기존 문제이며, 퍼저 시드 6007과, 서버로 재현해 본 다섯 가지(자기 `ROLLBACK TO` 뒤 ROLLBACK, ROLLBACK만, `ROLLBACK TO` 뒤 COMMIT, 다른 세션의 MERGE, 다른 세션의 크기 체크포인트) 중 세 가지가 옛 빌드에서 실패했다.
- **원인**: 표 파일을 쓰는 곳은 열려 있는 트랜잭션이 만든 미커밋 버전(`_xmin`이 그 트랜잭션)과 끝낸 버전(`_xmax`가 그 트랜잭션)까지 그대로 적었다. `ROLLBACK`은 메모리만 되돌리고 그 트랜잭션의 undo 기록도 지우므로, 파일에 남은 미커밋 버전을 다음 시작이 되돌릴 방법이 없었다. 자기 트랜잭션의 `ROLLBACK TO SAVEPOINT`는 문장 앞에서 체크포인트를 하므로 혼자서도 이 경우를 만든다.
- **수정**: `BufferPool::write_through`가 열린 트랜잭션 id(`SharedDatabase::active_txn_ids`)를 알아서, 그 트랜잭션이 만든 버전은 건너뛰고 끝낸 버전은 살아 있는 것(`_xmax` 0)으로 적는다. 커밋하는 트랜잭션 자신의 행은 적는다(`keep_txn`, 레거시 COMMIT 두 곳). 열린 트랜잭션이 없거나 그 표에 닿는 행이 없으면 복사 없이 전과 같이 쓴다. 파일이 커밋된 상태만 담으면 트랜잭션이 나중에 COMMIT할 때 redo 배치가 그 행들을 다시 만들므로 정합성이 그대로 유지된다.
- **한계**: `ALTER TABLE` 같은 DDL이 `DiskManager::save_table`을 직접 부르는 경로는 걸러내지 않는다(열린 트랜잭션 도중의 DDL).

**고친 것 (속도) — 캐시가 쓰기를 따라간다**: UPDATE(키를 옮기는 것 포함)·DELETE(빠른 길과 소프트 삭제)·COMMIT·외래키 연쇄가 자기 독점 구간 안에서 캐시를 제자리에서 고치고 도장(잠금 독점 횟수 합)을 다시 찍는다. 다른 쓰기·ROLLBACK·VACUUM·DDL은 전과 같이 도장이 어긋나서 캐시를 낡게 만든다. 여러 행 INSERT와 UPDATE의 키 검사도 캐시가 "없음"을 증명하면 훑지 않는다(캐시를 새로 만들지는 않음). 캐시는 계속 "없다"만 말할 수 있고, 모르면 옛 스캔이 답한다.

**속도**(R23 빌드 → 새 빌드, 번갈아 3번; 2만 행 UNIQUE 표, 반복 횟수 안의 합계): UNIQUE 열 표에서 INSERT·UPDATE v·DELETE를 섞어 100번 **451 → 207 ms**, `UNIQUE (a, b)` 표 **1,110 → 211**, 한 행 `UPDATE SET u` 100번 **153 → 68**, 한 행 `UPDATE SET b`(복합) 100번 **632 → 71**, 5행 INSERT + COMMIT 뒤 INSERT 40번 **660 → 102**, 5행 INSERT 100번 **709 → 74**. 통제 경로(일반 표 INSERT/UPDATE/DELETE, 색인 없는 UNIQUE UPDATE, 자식 표 INSERT, 외래키 문장)는 5번 번갈아 돌려 대부분 ±3% 안이다. 예외는 `SELECT` 스캔 +7%·GROUP BY +9%(실행마다 62~134 ms로 흩어지고 이 변경이 닿지 않는 경로)와 복합 기본키 한 행 INSERT +8%(새 실행 5번이 158~180 ms로 흩어짐), 반대로 색인 없는 UNIQUE 열 2000행 UPDATE는 -6%.

**도구**: `verify_null_expressions.py`의 모델이 연산마다 소수 6자리로 반올림하던 것을 정확한 Decimal 계산으로 고쳤다(엔진이 틀린 것이 아니라 모델이 틀렸음). 시드 1~8 통과, 열두 검증기 전부 깨끗(표 파일 걸러내기를 넣기 전 빌드로 돌렸고, 이 변경은 읽기 경로를 건드리지 않는다). `test_unique_probe.cpp`의 쌍둥이 시험이 재시작하면 카운터가 0이 되는 것을 몰라 40시드 캠페인의 시드 78에서 실패했다(시험의 결함) — 재시작 전의 사용 횟수를 더하도록 고쳤다.

**검증**:
- 신규 시험: `test_redo.cpp` "표를 트랜잭션 도중에 쓰면 ROLLBACK 뒤에도 그 행이 남지 않는다"(플러시를 일으키는 것 2가지[자기 `ROLLBACK TO SAVEPOINT`, 다른 세션 DDL] × 트랜잭션 끝 3가지[ROLLBACK, COMMIT, 열린 채 크래시] × 작업 2가지[INSERT+UPDATE+DELETE, 커밋된 행의 DELETE만], 문장 사이에 로그가 덮는 다른 세션의 INSERT를 넣어 플러시가 실제로 일어나게 하고 끝 문장 뒤에는 아무것도 하지 않음 — 처음 쓴 시험은 새 세션의 `USE d`가 먼저 체크포인트를 해서(로그가 덮지 않는 문장은 실행 전에 체크포인트를 한다) 트랜잭션 도중의 플러시가 일어나지 않았고, 걸러내기를 꺼도 통과했다 — 심은 버그로 알아내 시험을 고침), `test_unique_probe.cpp`(캐시가 UPDATE·DELETE·COMMIT·연쇄를 따라가는 것, 따라가지 못하는 변경 뒤에는 다시 기다리는 것, 여러 행 INSERT와 키 UPDATE가 캐시를 쓰는 것을 사용 횟수 카운터로 확인).
- 심은 버그: 표 파일 걸러내기 8종 중 7종 잡힘(남은 1종은 `"0"`을 숫자로 읽는 검사를 없앤 것으로 결과가 같다), 캐시 따라가기 21종 중 19종 잡힘(남은 2종 — 연쇄가 지우는 키를 캐시에서 빼지 않은 것, 변화 없는 도장을 받아들이지 않은 것 — 은 캐시가 "있다"고 틀리게 말해 스캔이 한 번 더 도는 속도 차이일 뿐이라 결과가 같다; 캐시는 "없다"고 틀리게 말할 수 없음).
- 크래시 퍼저: 외래키 퍼저 90라운드(시드 6000~6089, 캐시 점검 모드) 불일치 0(옛 빌드는 시드 6007에서 불일치), 일반 퍼저 60라운드·동시성 퍼저 40라운드(확인한 행 21,494) 불일치 0.
- Release 전체 **701 케이스 / 1,479,962 assertions**을 기본으로, 1,479,961을 강제로 통과; `[random]` 40시드·`[unique_claim]`·`[unique_probe]`·`[cascade_rollback]`을 캐시 점검 모드로 34 케이스 556,351 assertions 통과; 빌드 간 차분(R23 빌드 대비) 10시드 × 99질의 차이 0건; `verify_writes.py` 시드 1~8 위반 0; Debug 전체 시험은 표 파일 걸러내기를 넣기 전 빌드(캐시 따라가기까지)로 700 케이스를 기본(캐시 점검 모드)·강제 양쪽으로 통과했고, 그 뒤의 Debug 전체 재실행은 하지 않았다.

**한계**: 위 DDL 경로. 캐시는 메모리에 키마다 문자열 하나씩을 두는 파생 캐시라 큰 표에서는 메모리를 쓴다(R22와 같음). MERGE와 다중 표 UPDATE/DELETE는 여전히 표 파일을 통째로 다시 쓰는 옛 방식이다.

### 10월 16일 — `ALTER TABLE`과 버퍼 풀의 flush·교체도 열린 트랜잭션의 행을 표 파일에 적던 것 (R25)

**왜 이 항목인가**: 사용자 "계속 같은 진행방식으로". R24가 "남은 한계"로 적어 둔 것 — 표 파일을 `BufferPool::write_through`가 아니라 `DiskManager::save_table`로 직접 쓰는 곳 — 을 닫는다. R24는 체크포인트·다른 세션의 flush 문장·자기 `ROLLBACK TO SAVEPOINT` 앞 체크포인트가 쓰는 파일에서 열린 트랜잭션의 미커밋 버전을 걸렀지만, 같은 일이 두 군데에서 남아 있었다.

**고친 것 (정합성)**
- **`ALTER TABLE`** 네 가지(`ADD COLUMN`, `DROP COLUMN`, `RENAME COLUMN`, `RENAME TO`)가 열의 모양이 바뀐 표 전체를 `save_table`로 직접 적었다. 다른 세션의 트랜잭션이 열려 있으면 그 트랜잭션의 미커밋 행이 파일에 들어가고, 그 트랜잭션이 ROLLBACK한 뒤 크래시하면 되살아났다(R24 이전부터 있던 기존 문제, 같은 원인). 네 곳 모두 `write_through`를 거치게 했다.
- **버퍼 풀의 다른 쓰기 두 길** — `flush_all`(`CHECKPOINT` 문장·자동 체크포인트)과 더러운 쪽 교체(`get_page`가 밀어내는 더러운 쪽)도 `save_table`을 직접 불렀다. 이제 세 길(`write_through`, `flush_all`, 교체)이 한 함수 `BufferPool::save_committed`를 거친다.
- 참고로 발견만 한 것(고치지 않음): `BufferPool::write_page`가 새 쪽을 넣으며 더러운 옛 쪽을 밀어낼 때는 디스크를 받지 못해(`insert_page(…, nullptr)`) 그 쪽을 쓰지 않고 버린다. 이 길은 CTE·별칭 같은 가상 표 쪽을 주로 다루고, 실제 표(`RENAME COLUMN`)는 바로 앞에서 파일을 이미 썼으므로 잃는 데이터는 없다.

**검증**:
- 신규 시험: `test_storage.cpp` "BufferPool writes committed rows only while transactions are open"(`write_through`·커밋하는 트랜잭션의 `keep_txn`·`flush_all`·더러운 쪽 교체를, 열린 트랜잭션 5와 7이 만들거나 끝낸 행·둘 다 한 행·커밋된 트랜잭션이 끝낸 행으로), `test_redo.cpp` "an ALTER TABLE in the middle of another session's transaction …"(`ALTER` 네 가지 × 트랜잭션이 ROLLBACK됨/크래시 때 열려 있음, 끝 문장 뒤에는 아무것도 하지 않음).
- 심은 버그 6종(네 `ALTER` 곳, `flush_all`, 교체를 각각 원래대로 되돌림) 6종 모두 잡힘.
- 전체 시험 **703 케이스 / 1,480,092 assertions**을 Release 기본·강제 양쪽과 **Debug 기본(캐시 점검 모드)·강제 양쪽**으로 통과(Debug는 1,480,092 / 1,480,093); `[random]` 40시드·`[unique_claim]`·`[unique_probe]`·`[cascade_rollback]`을 캐시 점검 모드로 34 케이스 556,351 assertions 통과. Release 기본은 처음 한 번 `test_unique_claims.cpp`의 시간 재는 단언 두 개(`t.ms >= 150`이 141, 아무것도 기다리지 않는 INSERT가 150 ms 미만이어야 하는데 315)가 Debug 빌드·퍼저와 함께 돌아 CPU가 바쁠 때 어긋났고, 그 시험만 다시 돌리면 통과, 조용한 기계에서 전체를 다시 돌려 통과(코드 경로와 무관한 부하 탓).
- 크래시 퍼저: 외래키 퍼저 90라운드(시드 6000~6089, 캐시 점검 모드)·일반 퍼저 60라운드·동시성 퍼저 40라운드(확인한 행 14,271) 불일치 0, `verify_writes.py` 시드 1~8 위반 0.
- 하지 않은 것: 빌드 간 차분(비교할 이전 빌드가 임시 폴더 정리로 사라짐; 이 변경은 읽기 경로를 건드리지 않음)·속도 비교(`ALTER`와 flush 경로만 닿는 변경).

**한계**: `CREATE TABLE`·`TRUNCATE`·`DROP`은 빈 표를 쓰거나 파일을 지우므로 해당 없다. 복구 때(`recover_from_wal`)와 부팅 때 쓰는 `save_table`은 열린 트랜잭션이 없는 시점이다. `MERGE`와 다중 표 `UPDATE/DELETE`는 여전히 표 파일을 통째로 다시 쓰는 옛 방식이다.

### 10월 16일 (두 번째) — select 항목의 별칭을 `AS` 없이 쓰면 파싱 오류이던 것 (R26)

**왜 이 항목인가**: 사용자 "계속 고치기". 남은 한계를 옛 메모에서 고르는 대신, 새로 띄운 진짜 MySQL 8.4.9(임시 폴더, 3391 포트)와 DML 의미·범위 검사·산술 한계·다중 표 UPDATE/DELETE·INSERT 형태·부질의 형태 73문장을 비교했다(임시 코퍼스 `probe_dml.sql`). 20곳이 달랐고(둘 다 오류인 13곳은 제외), 원인별로 묶으면 다음과 같다: ① **select 항목의 별칭을 `AS` 없이 씀**(`COUNT(*) c`, `a x`, `ROW_NUMBER() OVER (…) r` — 이번 항목), ② `INSERT … SET col = 값` 없음, ③ **UNSIGNED 정수 범위**를 부호 있는 정수처럼 검사(`INT UNSIGNED`에 `-1`이 들어가고 `4294967295`는 거절되며 `u - 5`가 음수가 됨; MySQL은 1264/1690 오류), ④ `= ANY / ALL / SOME (서브쿼리)` 파싱 오류, ⑤ 행 생성자 `(a, b) IN (서브쿼리)`·`(1, 2) = (1, 2)` 파싱 오류, ⑥ `COUNT(DISTINCT a, b)` 파싱 오류, ⑦ `||`는 MySQL 기본 모드에서 OR인데 RuSQL은 문자열 연결, ⑧ UPDATE가 행을 표 끝으로 옮겨서 `ORDER BY` 없는 SELECT·윈도 함수의 출력 순서가 InnoDB의 기본키 순서와 다름(표준 SQL상 정해진 순서는 아님). ①이 가장 흔한 문체라 먼저 고쳤고 나머지는 `PLAN.md`에 후보로 적었다.

**고친 것**: 파서에 `Parser::parse_column_alias()`를 두어 `[AS] 별칭`을 읽는다 — `AS`가 없으면 키워드가 아닌 일반 식별자만 별칭으로 본다. select 항목을 읽는 모든 길(스칼라 함수·`CASE`·`CAST`·집계·`FILTER`·`GROUP_CONCAT`·윈도 함수·스칼라 서브쿼리·산술식·열)이 같은 함수를 부르도록 일곱 곳을 바꿨다(앞에서는 `AS`만 받았다). 항목을 끝내는 낱말(`FROM`, `INTO`, `WHERE`, `UNION`, `LIMIT`, `FOR` …)은 키워드라 그대로 항목을 끝내고, 낱말이 둘 연달아 오면(`SELECT a b c`) 여전히 오류다.

**검증**:
- 신규 시험 `test_implicit_alias.cpp`: `AS`가 있는 문장과 없는 문장 19쌍(집계·GROUP BY/HAVING의 별칭·`COUNT(DISTINCT …)`·`GROUP_CONCAT`·열·`표.열`·산술식·함수·`CASE`·`CAST`·`ROW_NUMBER()`/`SUM() OVER`·스칼라 서브쿼리·FROM 없는 SELECT·파생 표·`UNION`)의 출력이 글자까지 같다, 열 이름이 실제로 별칭이다, 뷰·`INSERT … SELECT`에서도 되고, `SELECT a b c`와 `COUNT(*) c d`는 오류다.
- 심은 버그 8종(일곱 군데를 `AS`만 받도록 되돌리기, 일반 식별자를 별칭으로 받는 줄 지우기) 8종 모두 잡힘.
- 전체 시험 **704 케이스 / 1,480,170 assertions**을 Release 기본·강제(1,480,169) 양쪽으로 통과; 빌드 간 차분(앞 빌드 대비) 10시드 × 99질의 차이 0건; 기존 MySQL 비교 코퍼스 4개(collation·constraints·functions·tables)는 앞 빌드와 같은 결과(차이 5/7/6/8곳 그대로); 비교 코퍼스 73문장은 달랐던 곳이 21 → 20곳(`HAVING c > 1` 일치).
- 하지 않은 것: Debug 전체 시험·크래시 퍼저·속도 비교(파서만 바뀌었고 실행 경로는 그대로).

**한계**: `SELECT a 'x'`처럼 문자열 낱말을 별칭으로 쓰는 것과, 이름이 키워드인 낱말(`date`, `count` 같은 비예약어)을 `AS` 없이 별칭으로 쓰는 것은 받지 않는다(MySQL은 받음). `RETURNING` 목록과 `UPDATE … SET`은 그대로다.

### 10월 16일 (세 번째) — `INSERT … SET`·`REPLACE … SET`가 없던 것, 같은 열을 두 번 쓴 INSERT가 받아들여지던 것 (R27)

**왜 이 항목인가**: 사용자 "계속 진행해주세요". R26의 진짜 MySQL 비교가 남긴 후보 중 첫째(`PLAN.md`).

**고친 것**
- **`INSERT [IGNORE] INTO t SET a = 1, b = 'x'`, `REPLACE [INTO] t SET …`**: `(a, b) VALUES (1, 'x')`와 같은 한 행이다. `ON DUPLICATE KEY UPDATE`·`IGNORE`·`RETURNING`·`REPLACE`가 VALUES 형식과 같은 길을 탄다(문장만 다르고 실행기는 그대로). 값을 읽는 코드를 `Parser::parse_insert_value(set_form)`로 뽑아 두 형식이 함께 쓴다 — SET 형식에서는 값이 `,`뿐 아니라 `ON`·`RETURNING`·`;`·문장 끝에서도 끝난다. 열 목록과 SET을 같이 쓰면(`INSERT INTO t (a) SET b = 1`) 오류다.
- **같은 열을 두 번 쓴 INSERT/REPLACE**(`SET id = 21, id = 22`, `(id, id) VALUES …`, `INSERT … SELECT`의 열 목록)는 MySQL의 1110 "Column 'id' specified twice" 오류로 거절한다 — 전에는 VALUES 형식도 받아들여 뒤의 값이 이겼다.

**이 비교가 새로 찾은 것 — 크다, 고치지 않음, 사용자 결정 대기**: **열 이름·표 이름이 대소문자를 구분한다.** `INSERT INTO ci2 (ID, NAME) VALUES …`, `SELECT NAME FROM ci2`, `WHERE ID = 1`, `UPDATE … SET NAME = …`, `SELECT Ci2.Id`가 "Column 'ID' not found"·"Unknown column"으로 거절되고 `FROM CI2`는 "Table 'v.CI2' not found"다(비교한 9문장 중 8문장이 다름). MySQL은 열 이름·별칭을 어느 플랫폼에서나 구분하지 않고(표·데이터베이스 이름은 Windows에서 구분하지 않고 Linux에서 구분), 결과 열 이름은 쓴 글자 그대로 돌려준다. `SELECT NAME FROM users`처럼 열을 대문자로 쓰는 흔한 문체가 통하지 않는다는 뜻이다. 이름을 찾는 곳 전부(파서가 만든 이름, 실행기의 열 조회, 인덱스·제약·뷰·트리거·프로시저 본문)에 걸친 변경이라 범위가 크다 — `PLAN.md`에 적고 방향을 묻는다.

**검증**:
- 신규 시험 `test_insert_set.cpp`: SET 형식과 VALUES 형식 11쌍(자동 증가·`DEFAULT`·NULL·산술·함수·사용자 변수·따옴표·`IGNORE`·`ON DUPLICATE KEY UPDATE`·`REPLACE`·`REPLACE` INTO 없이)이 같은 표를 만들고, 거절해야 할 문장 10개(열 목록+SET, 빈 SET, `=` 빠짐, 끝의 쉼표, 같은 열 두 번 네 형식)와 중복 기본키 오류 메시지가 VALUES 형식과 같다.
- MySQL 비교 코퍼스 `corpus/tables.sql`에 21문장을 더하고 `mysql_conformance_tables.inc`를 다시 만들었다(진짜 MySQL 8.4의 답이 기록된다; 기존 case는 변하지 않고 21줄만 늘어남) — 새 문장 모두 MySQL과 같은 답이고 `[mysql_conformance]` 4 케이스 921 assertions 통과.
- 심은 버그 8종 중 5종 잡힘: 남은 3종 중 2종은 결과가 같다(SET 형식의 값 하나를 "일반 값"으로 읽든 식으로 읽든 같은 행이 나옴), 1종은 대소문자를 접는 비교였는데 열 이름이 대소문자를 구분하는 지금은 `ID`와 `id`가 다른 열이라 접을 일이 없어서 그 코드를 지웠다.
- 전체 시험 **705 케이스 / 1,480,266 assertions**을 Release 기본으로, 강제로(1,480,265) 통과; 빌드 간 차분(앞 빌드 대비) 10시드 × 99질의 차이 0건; 기존 MySQL 코퍼스 4개는 앞 빌드와 같은 결과(차이 5/7/6/8곳 그대로); 비교 코퍼스 73문장의 달랐던 곳 20 → 16곳.
- 하지 않은 것: Debug 전체 시험·크래시 퍼저·속도 비교(파서만 바뀜).

**한계**: `INSERT … SET`의 `VALUES(col)`·`DEFAULT(col)` 같은 함수는 VALUES 형식과 같은 규칙을 따른다(새로 만든 것 없음). 대소문자 문제는 위.

### 10월 17일 — `UNSIGNED` 정수 열이 부호 있는 정수처럼 검사되던 것, 2^63 이상의 정수 비교 (R28)

**왜 이 항목인가**: 사용자 "그 다음 후보도 해주세요". R26·R27의 진짜 MySQL 비교가 남긴 후보를 위에서부터 (열 이름 대소문자 문제는 아직 답이 없어 건드리지 않았다).

**증상**: 파서가 `INT UNSIGNED`의 `UNSIGNED`를 읽고 버려서 어느 정수 열이든 부호 있는 범위로 검사했다 — `INT UNSIGNED`에 `-1`이 들어가고 최댓값 `4294967295`는 "Out of range"로 거절됐고, `UPDATE … SET u = u - 5`가 음수를 만들었다. mysqldump가 쓰는 `int unsigned`·`bigint unsigned` 열이 그대로 이런 범위였다. 같은 비교에서 찾은 짝: 가장 작은 `BIGINT`(`-9223372036854775808`)도 거절됐고, `9223372036854775808 > 9223372036854775807`이 0이었다(int64 밖은 double로 비교).

**고친 것**
- **스키마**: `ColumnDef::is_unsigned`(저장 이름 `unsigned`, 없던 옛 스키마는 거짓). 파서가 `TINYINT/SMALLINT/INT/BIGINT … UNSIGNED`를 읽은 것을 `CREATE TABLE`, `ALTER … ADD COLUMN`, `ALTER … MODIFY COLUMN` 세 곳에서 열에 넣는다.
- **범위**(`coerce_column_value`): 부호 없는 `TINYINT` 0..255, `SMALLINT` 0..65535, `INT` 0..4294967295, `BIGINT` 0..18446744073709551615; 음수는 "Out of range value" 오류(0으로 반올림되는 `-0.4`도 MySQL처럼 거절). 검사가 부호와 크기를 따로 들고 가서 부호 있는 `BIGINT`의 최솟값도 이제 들어간다.
- **표시**: `SHOW CREATE TABLE`·`DESCRIBE`에 `INT UNSIGNED`, `information_schema.columns.COLUMN_TYPE`에 `int unsigned`, `BACKUP`의 `CREATE TABLE`에 `UNSIGNED`.
- **`MODIFY COLUMN … UNSIGNED`**는 음수 값이 이미 있으면 거절하고 플래그를 바꾼다.
- **비교**: `compare_numbers`가 int64 밖의 정수도 부호+크기로 정확히 비교한다(2^64 − 1까지; 그 밖은 전처럼 double).

**한계**: `SELECT u - 5`처럼 식 안에서 부호 없는 피연산자의 뺄셈이 음수가 되면 MySQL은 1690 오류를 내지만 RuSQL의 식에는 열 타입이 없어 음수를 돌려준다(`UPDATE … SET u = u - 5`는 대입할 때의 범위 검사로 오류 — 번호만 1264). `FLOAT/DOUBLE/DECIMAL … UNSIGNED`는 읽지 않는다. 2^63 이상의 값은 저장·표시·비교·정렬·`MAX`/`MIN`은 정확하지만 `SUM`/`AVG`는 int64를 넘으면 double로 더한다.

**검증**:
- 신규 시험 `test_unsigned.cpp`: 네 정수형의 경계에 닿는 행 3개(부호 있는 열의 경계 포함)와 거절해야 할 16문장(경계를 하나 넘음·음수·`-0.4`·식의 결과·숫자가 아닌 값), 2^63 이상 값의 비교·`MAX`, 범위를 벗어나는 `UPDATE` 세 가지가 행을 바꾸지 않음, `SHOW CREATE TABLE`/`DESCRIBE`/`information_schema`, `ALTER ADD/MODIFY`, `BACKUP`, 재시작 뒤에도 부호 없음.
- MySQL 비교 코퍼스 `corpus/constraints.sql`에 38줄을 더하고 `mysql_conformance_constraints.inc`를 다시 만들었다(MySQL 8.4의 답 기록; 기존 case는 그대로) — 새 문장 모두 MySQL과 같은 답, `[mysql_conformance]` 통과. UNSIGNED 비교 코퍼스(38문장)의 차이는 8곳 → 1곳(위 식 안의 뺄셈).
- 심은 버그 18종 중 17종 잡힘(남은 1종은 결과가 같다 — 하한이 0이면 음수 검사가 두 군데에서 겹침).
- 전체 시험 **706 케이스 / 1,480,400 assertions**을 Release 기본으로, 강제로(1,480,399) 통과; 빌드 간 차분(앞 빌드 대비) 10시드 × 99질의 차이 0건; 기존 MySQL 코퍼스 4개는 앞 빌드와 같은 결과(차이 5/7/6/8곳 그대로); `dump_roundtrip.py`(진짜 `mysqldump`로 뜬 두 덤프, 그중 하나는 `unsigned` 열 포함) 행이 모두 같고 mysql 클라이언트 오류 0; 크래시 퍼저(외래키 45·일반 30·동시성 20라운드) 불일치 0, `verify_writes.py` 시드 1~4 위반 0.
- 하지 않은 것: Debug 전체 시험·속도 비교(범위 검사 한 줄과 파서만 바뀜).

### 10월 17일 (두 번째) — `x op ANY / SOME / ALL (서브쿼리)`가 파싱 오류이던 것 (R29)

**왜 이 항목인가**: 사용자 "그 다음 후보도 해주세요". R26 비교가 남긴 후보 중 셋째(`PLAN.md`의 (c)).

**고친 것**
- **`x = ANY (SELECT …)`, `x <> ALL (SELECT …)`, `>`·`<`·`>=`·`<=` 와 `SOME`(= `ANY`)**: 파서가 비교 연산자 뒤의 `ANY`/`SOME`/`ALL`을 — 괄호 친 `SELECT`가 뒤따를 때만 — 한정사로 읽어 `Condition::quantifier`(`Any`/`All`)에 담는다(`ANY`라는 이름의 열은 그대로 열). 한정사가 있으면 서브쿼리 뒤의 산술(`… ANY (SELECT …) + 1`)은 식으로 이어가지 않고 문법 오류다.
- **평가**(`eval_single_with_subquery`): 서브쿼리가 내는 값 전체를 보고 ANY는 하나라도 참이면 참, ALL은 하나라도 거짓이면 거짓; NULL(x의 NULL이든 서브쿼리 값의 NULL이든)은 다른 값이 결정하지 못하면 UNKNOWN; **값이 하나도 없으면 ANY는 거짓, ALL은 참이고 x가 NULL이어도 그렇다**(`NOT (x > ANY (빈 것))`도 참). 상관 서브쿼리, 문장당 한 번 캐시, 값 자리(`SELECT a > ALL (…)`)·`HAVING`·`UPDATE`/`DELETE`의 `WHERE`, 텍스트의 기본 정렬 규칙 비교가 기존 서브쿼리 길을 그대로 쓴다.
- **저장**: 뷰는 파싱한 문장의 JSON으로 저장되므로 `quantifier`를 JSON에 넣었다(없는 옛 파일은 한정사 없음). 표 별칭을 펼치는 `expand_leaf`가 조건을 새로 만들면서 한정사를 잃던 것도 고쳤다(시험이 찾음 — 처음엔 `Subquery returns more than 1 row`).

**검증**:
- 신규 시험 `test_quantified.cpp`: 6가지 연산자 × `ANY`/`SOME`/`ALL`에 값이 {2, 3}·{2, NULL}·빈 집합인 서브쿼리와 NULL인 x(30가지), `NOT` 안의 한정사, `IN`/`NOT IN`과 같아지는 경우, 텍스트(`'Alice'` ↔ `'ALICE'`), 상관 서브쿼리, 값 자리·`HAVING`·`UPDATE`·`DELETE`, 거절해야 할 문장 4개(`ANY (1, 2)`, 뒤가 없음, `IS ANY`, 뒤에 산술), 그리고 뷰가 재시작 뒤에도 같은 뜻.
- **진짜 MySQL 8.4.9와 비교**: 같은 39문장(위 + 값 자리·HAVING·UPDATE·DELETE)을 양쪽에 돌려 **차이 0건**; `corpus/tables.sql`에 44줄을 더하고 `mysql_conformance_tables.inc`를 다시 만들었다(MySQL의 답 기록; 기존 case는 그대로) — `[mysql_conformance]` 통과.
- 심은 버그 14종 모두 잡힘(처음엔 12종이었고 남은 둘을 위해 "한정사 뒤 산술은 문법 오류" 시험과 재시작 뒤 뷰 시험을 더했다).
- 전체 시험 **708 케이스 / 1,480,576 assertions**을 Release 기본으로, 강제로(1,480,574) 통과; 빌드 간 차분(앞 빌드 대비) 10시드 × 99질의 차이 0건; 기존 MySQL 코퍼스 4개는 앞 빌드와 같은 결과(차이 5/7/6/8곳 그대로); 비교 코퍼스 73문장의 달랐던 곳 11 → 8곳.
- 하지 않은 것: Debug 전체 시험·크래시 퍼저·속도 비교(파서와 서브쿼리 평가 한 곳만 바뀜).

**한계**: 서브쿼리가 열을 둘 이상 내면(`x = ANY (SELECT a, b …)`) MySQL의 "Operand should contain 1 column(s)" 대신 첫 열만 본다(`IN`과 같은 기존 동작). 행 생성자(`(a, b) = ANY (…)`)는 다음 항목.

### 10월 17일 (세 번째) — 행 생성자 `(a, b) = (1, 2)`·`(a, b) IN (…)`가 파싱 오류이던 것 (R30)

**왜 이 항목인가**: 사용자 "계속". R26 비교가 남긴 후보 중 넷째(`PLAN.md`의 (d)).

**고친 것**
- **행끼리의 비교** `(a, b) = (1, 2)`·`<>`·`<`·`<=`·`>`·`>=`: 파서가 MySQL의 뜻 그대로 풀어 쓴다 — `=`는 `a = 1 AND b = 2`, `<>`는 `a <> 1 OR b <> 2`, `<`·`<=`·`>`·`>=`는 처음 다른 값이 정하는 `a < 1 OR (a = 1 AND b < 2)`(마지막 값만 `<=`·`>=` 그대로). 세 값 이상도 된다. NULL은 앞의 값이 이미 정하지 못했을 때만 답을 열어 둔다(`(NULL, 1) = (1, 2)`는 `1 = 2`가 거짓이라 거짓).
- **`(a, b) [NOT] IN ((1, 2), (3, 4))`**: 행 같음의 OR(`NOT IN`은 그 부정).
- **`(a, b) [NOT] IN (SELECT x, y …)`**: 풀어 쓸 수 없는 경우(`NOT IN`에서 NULL의 세 값 논리가 EXISTS로는 달라짐)라 왼쪽을 `ROW(a, b)`로 둔 하나의 조건으로 남기고 `eval_single_with_subquery`가 읽는다: 서브쿼리가 내는 행마다 값 쌍이 모두 같으면 참, 어느 쌍이 거짓이면 그 행은 거짓, 그 밖에 NULL이 끼면 UNKNOWN; IN은 어느 행이 참이면 참, 아니면 열린 행이 있으면 UNKNOWN, 아니면 거짓; 행이 하나도 없으면 IN 거짓·NOT IN 참(왼쪽이 NULL이어도). 상관 서브쿼리, 문장당 한 번 캐시(`subquery_scalar_cache_`에 칸을 한 줄로 펴서 둠), UPDATE/DELETE의 WHERE, 텍스트의 기본 정렬 규칙 비교, 뷰(JSON에 저장)가 기존 길을 그대로 쓴다.
- **열 개수**: 서브쿼리의 열 수가 왼쪽 행의 열 수와 다르면 MySQL의 1241 "Operand should contain N column(s)" 오류(바인더가 행의 길이를 기대값으로 삼도록 고침 — 전에는 무조건 1열). 괄호 친 값 하나(`(a) = 1`, `(a + b) * 2 > 5`)는 행이 아니고 전처럼 읽는다.

**검증**:
- 신규 시험 `test_row_constructors.cpp`: WHERE 조건 25가지(행 비교 여섯 연산자와 세 값 행, 행 목록 `IN`/`NOT IN`과 NULL이 든 목록, 다른 조건과 `AND`/`OR`/`NOT`, 서브쿼리 `IN`/`NOT IN`·빈 서브쿼리·상관 서브쿼리·텍스트·`OR`와 함께, 괄호 친 값), 거절해야 할 문장 6개(열 수 불일치·행과 서브쿼리 비교·`LIKE`·`UNION`의 열 수 불일치 등), `UPDATE`/`DELETE`, 그리고 두 뷰가 재시작 뒤에도 같은 뜻.
- **진짜 MySQL 8.4.9와 비교**: 같은 30문장에서 **차이 0건**; `corpus/tables.sql`에 35줄을 더하고 `mysql_conformance_tables.inc`를 다시 만들었다(MySQL의 답 기록; 기존 case는 그대로).
- 심은 버그 15종 중 13종 잡힘(처음 실행에서는 12종 — "괄호 친 값 하나는 행이 아님" 시험을 더해 하나가 더 잡힘). 남은 둘은 결과가 같다: `ROW` 서브쿼리 결과의 열 수 검사(바인더가 먼저 같은 오류를 내는데, 바인더 검사를 건너뛰는 경로가 있어 실행기의 검사는 남김)와, 한 칸의 세 값 논리 분기 둘의 순서를 바꾼 것(두 분기가 서로 배타적).
- 전체 시험 **710 케이스 / 1,480,718 assertions**을 Release 기본으로, 강제로(1,480,719) 통과; 빌드 간 차분(앞 빌드 대비) 10시드 × 99질의 차이 0건; 기존 MySQL 코퍼스 4개는 앞 빌드와 같은 결과(차이 5/7/6/8곳 그대로); 비교 코퍼스 73문장의 달랐던 곳 8 → 6곳.
- 하지 않은 것: Debug 전체 시험·크래시 퍼저·속도 비교(파서와 서브쿼리 평가만 바뀜).

**한계**: 행 생성자는 조건(WHERE·HAVING·ON·CASE의 WHEN)에서만 쓸 수 있고 값 자리(`SELECT (a, b) = (1, 2)`)는 아직 아니다. `(a, b) = (SELECT x, y …)`(행 하나를 돌려주는 서브쿼리와 비교)는 "IN (SELECT …)를 쓰라"는 오류로 거절한다. `ROW(a, b)` 글자로 쓴 행은 읽지 않는다.

### 10월 17일 (네 번째) — `COUNT(DISTINCT a, b)`가 파싱 오류이던 것 (R31)

**왜 이 항목인가**: 사용자 "계속". R26 비교가 남긴 후보 중 다섯째(`PLAN.md`의 (e)). 이로써 그 비교가 가리킨 후보는 — 일부러 두기로 한 `||`의 뜻과 행 순서를 빼고 — 모두 처리했다.

**고친 것**: `COUNT(DISTINCT a, b, …)`가 서로 다른 값 묶음의 수를 센다; 묶음 중 하나라도 NULL인 행은 세지 않는다(MySQL). 집계 인자가 글자로 저장되고 실행기가 그 글자를 식으로 되읽어 행마다 계산하는 길을 그대로 쓴다 — 파서가 `COUNT(DISTINCT` 뒤의 쉼표 목록을 `a, b`라는 글자로 잇고(결과 열 이름이 `COUNT(DISTINCT a, b)`로 나옴), `Parser::str_to_arith`가 쉼표 목록을 한 값으로 바꾼다: 각 값의 글자 수와 글자를 이어 붙인 `CONCAT(CHAR_LENGTH(a), ':', a, CHAR_LENGTH(b), ':', b)`. 글자 수를 앞에 붙여서 다시 읽을 때 갈라지므로 구분자가 필요 없고(`('a,b', 'c')`와 `('a', 'b,c')`가 다르다), 하나라도 NULL이면 `CONCAT`이 NULL이라 그 행은 세지 않으며, 같은 글자는 기본 정렬 규칙으로 같다(`'e'`·`'E'`·`'é'`는 한 값). `SUM(DISTINCT a, b)`·`COUNT(a, b)`는 MySQL처럼 오류다. 식 안(`COUNT(DISTINCT a, b) + 1`)·`GROUP BY`·`HAVING`·`ORDER BY`·별칭에서도 된다.

**검증**:
- 신규 시험 `test_count_distinct_columns.cpp`: 두 열·세 열·식인 값, NULL이 든 묶음, 글자 수로만 갈라지는 묶음, 대소문자·악센트만 다른 묶음, 빈 표, 한 열일 때의 옛 동작, 별칭(`AS` 있음·없음), 식 안, `GROUP BY`/`HAVING`/`ORDER BY`, 결과 열 이름, 거절해야 할 문장 4개.
- **진짜 MySQL 8.4.9와 비교**: 같은 17문장에서 **차이 0건**; `corpus/tables.sql`에 27줄을 더하고 `mysql_conformance_tables.inc`를 다시 만들었다(MySQL의 답 기록; 기존 case는 그대로).
- 심은 버그 8종 중 7종 잡힘(글자 수 빼기, 글자 수를 바이트로, NULL을 값으로 세기, 집계 두 자리의 쉼표 목록 켜고 끄기 등; 남은 하나는 "뒤에 남은 글자가 있으면 식이 아니다" 검사로, 이 글자들은 파서가 만든 것이라 거기에 닿지 않는다).
- 전체 시험 **711 케이스 / 1,480,803 assertions**을 Release 기본으로, 강제로(1,480,804) 통과; 빌드 간 차분(앞 빌드 대비) 10시드 × 99질의 차이 0건; 기존 MySQL 코퍼스 4개는 앞 빌드와 같은 결과(차이 5/7/6/8곳 그대로); 처음의 비교 코퍼스 73문장은 차이 6곳 → 5곳이 남고, 남은 것은 모두 일부러 두었거나(`||`는 MySQL에서 OR인데 RuSQL은 연결, 값 자리의 행 비교 `SELECT (1, 2) = (1, 2)`) 표준 SQL이 순서를 정하지 않은 것(UPDATE가 행을 표 끝으로 옮겨 `ORDER BY` 없는 윈도 함수·`GROUP_CONCAT`의 행 순서가 InnoDB의 기본키 순서와 다름)이다.
- 하지 않은 것: 이 항목 자체의 Debug·크래시 퍼저·속도 비교(파서만 바뀜) — R26~R31을 묶은 Debug 전체 시험과 크래시 퍼저는 이 항목 뒤에 따로 돌려 `DATE.md`의 같은 날 마지막 줄에 적는다.

**한계**: `COUNT(DISTINCT …)` 안에 쉼표 목록을 쓰는 `OVER (…)` 창 집계는 읽지 않는다.

**R26~R31 묶음 검증 (10월 17일, 마지막 커밋 `00fdb9f`의 빌드)**: Debug 전체 시험 **711 케이스를 기본(캐시 점검 모드, 1,480,804 assertions)·강제(1,480,803) 양쪽으로 통과**; 크래시 퍼저 — 외래키 90라운드(시드 6000~6089, 캐시 점검 모드)·일반 60라운드·동시성 40라운드(확인한 행 16,017) 불일치 0; `verify_writes.py` 시드 1~8 위반 0; 모델 검증기 열한 가지(`verify_agg_arguments`·`agg_expressions`·`aggregates`·`compare`·`correlated`·`joins`·`null_expressions`·`orderby_distinct`·`sort_group`·`subqueries`·`value_expressions`) 위반 0; `dump_roundtrip.py`(진짜 `mysqldump` 파일 둘) 행이 모두 같고 mysql 클라이언트 오류 0. 이 여섯 항목(R26 AS 없는 별칭 · R27 `INSERT … SET` · R28 `UNSIGNED` · R29 `ANY`/`ALL` · R30 행 생성자 · R31 `COUNT(DISTINCT a, b)`)에서 "Debug·크래시 퍼저는 따로"라고 적은 것을 이것으로 채운다. 속도 비교는 하지 않았다(파서와 서브쿼리 평가, 범위 검사 한 줄만 바뀜).

### 10월 18일 — 열·표·별칭·색인·루틴 이름의 대소문자 구분 없음, `db.표` 이름, 중복 이름 거절, 기본키 순서 스캔, `||`는 OR, 값 자리의 행 비교 (R32~R36)

**왜 이 항목인가**: 사용자 "다 고쳐야죠 계속 진행합니다. 대신 완료되면 문서 갱신 및 커밋&푸시". R27부터 사용자 결정을 기다리던 가장 큰 차이(열·표 이름이 대소문자를 구분함)와, R26 비교가 "일부러 둠"으로 남긴 세 항목(`||`의 뜻, 행 순서, 값 자리의 행 비교)을 모두 고친다. 항목마다 멈추던 규칙은 이번에만 풀고 끝에서 문서·커밋·푸시를 한 번에 한다. 진짜 MySQL 8.4.9와 같은 문장을 돌려 보며 하나씩 닫았다(이름 비교 문장 55 + 51 + 33 + 32개: 처음 차이 38곳 → 일부러 둔 몇 곳만 남음).

**R32 — 이름에 대소문자가 없다 (MySQL: Windows의 `lower_case_table_names = 1`, 열·별칭·색인·루틴은 어디서나)**
- **규칙**: 데이터베이스·표·뷰·열·별칭·색인·프로시저·트리거·함수·사용자 변수가 어느 철자로 쓰여도 같은 것을 가리킨다. 보이는 글자는 만든 대로 둔다 — `SELECT *`는 표를 만들 때의 열 이름, `SELECT NAME`의 머리글은 쓴 글자 그대로(`NAME`). 예전에는 `SELECT NAME FROM Users`가 "Unknown column"이었다.
- **구조 세 겹**: (A) 새 `executor_names.cpp`의 `Executor::resolve_names`가 문장을 파싱한 직후 — 잠금 집합·질의 캐시·권한 검사보다 앞에서 — 표·DB 이름(FROM/JOIN/INSERT/UPDATE/DELETE/DDL/뷰/CTE/`REFERENCES`/트리거 `ON`/`LOCK TABLES`…)과 쓰는 문장의 열 목록(INSERT 열 목록, `UPDATE … SET`, `ON DUPLICATE KEY UPDATE`의 `VALUES(col)`, `CREATE INDEX`, `ALTER`, `PRIMARY KEY`/`UNIQUE`/`KEY`/`FOREIGN KEY`/`PARTITION BY`의 열)을 카탈로그의 철자로 고쳐 쓴다(정확히 같으면 그대로, 아니면 대소문자만 다른 단 하나의 후보; 여럿이거나 없으면 그대로 두어 원래 오류가 난다). 프로시저·트리거 본문의 문장은 실행할 때 같은 함수(잠금을 다시 잡지 않는 오버로드)로 고친다. (B) 바인더(`executor_bind.cpp`)가 식 안의 열 참조·별칭·`ORDER BY`/`GROUP BY` 항목·집계 인자·`T.*`의 한정자·오른쪽 `col = other.col`을 표 철자로 바꾸고(`Binder::canonical`), 결과 열 머리글은 쓴 글자 그대로 둔다(집계는 머리글용 `col`과 행에서 읽는 `source`를 갈랐다). (C) 파서의 표 별칭 맵·자기 조인 판별·`PRIMARY KEY`/`UNIQUE`/`FOREIGN KEY`/`PARTITION BY` 열 비교도 대소문자를 무시한다(`Emp a JOIN emp b`가 자기 조인으로 읽힘).
- **루틴과 변수**: 프로시저·트리거·함수 이름이 `CREATE`/`CALL`/`DROP`에서 대소문자를 무시한다(다른 철자로 다시 만들면 하나로 교체); 프로시저의 매개변수·`DECLARE` 변수(`PCT`/`pct`, `SELECT … INTO N`); 트리거의 `NEW.SAL`/`NEW.sal`과 `SET NEW.SAL = …`; 사용자 변수 `@V` = `@v`(저장은 소문자 키, `SELECT @V`의 머리글은 쓴 글자); 저장 함수 본문의 매개변수 `X`/`x`.
- **새 문법**: `DROP INDEX 이름 ON 표`(MySQL 형식. 같은 색인 이름이 두 표에 있어도 그 표 것만 지움; 없이 쓴 `DROP INDEX 이름`은 옛 동작).
- **함께 고친 것**: `UNION`/`INTERSECT`/`EXCEPT`의 `ORDER BY` 비교가 기본 정렬 규칙을 무시하고 바이트로 비교하던 것(`Alpha;ann;Beta` 순서가 `Alpha;Beta;ann`으로 나옴); **같은 이름이 두 번 나오는 DDL이 조용히 받아들여지던 것** — `CREATE TABLE t (a INT, A INT)`·`ALTER TABLE … ADD COLUMN`으로 이미 있는 열(이름이 정확히 같아도 검사가 없었음)·`RENAME COLUMN` 대상이 다른 열 이름 → 1060 "Duplicate column name"(열을 자기 이름의 다른 대소문자로 바꾸는 것은 허용), 같은 표의 같은 색인 이름 → 1061 "Duplicate key name"(전에는 바이트가 같은 이름은 조용히 무시, 다른 철자는 둘이 생김), INSERT 열 목록의 `aa, AA` → 1110; select 목록이 한 이름을 대소문자만 다르게 두 번, 서로 다른 것에 붙였을 때 그 이름의 `ORDER BY`/`GROUP BY` → 1052 "ambiguous"(같은 것에 붙였으면 허용 — MySQL과 같음).

**R33 — `db.표` 이름이 FROM 말고는 파싱 오류였다**: `CREATE TABLE d.t (…)`는 "Expected '('", `INSERT INTO d.t …`는 "Expected VALUES, SET or SELECT", `DROP TABLE d.t`는 "Unexpected token(s)…". 이름 대소문자 점검(`dbx.Z1` 같은 문장)이 찾았다. `Parser::expect_table_name`(이름 하나 + `.` + 이름)으로 표 자리 전부 — INSERT/REPLACE/UPDATE(조인 포함)/DELETE/CREATE TABLE·VIEW/ALTER/DROP TABLE·VIEW/TRUNCATE/DESCRIBE/SHOW CREATE TABLE·VIEW/SHOW INDEX/CREATE [UNIQUE] INDEX … ON/DROP INDEX … ON/`REFERENCES`/트리거 `ON` — 를 바꿨다.

**R34 — 순서 없는 읽기는 기본키 순서로**: InnoDB는 기본키가 있는 표를 클러스터드 인덱스 순서로 읽는데, 여기는 행이 쓰인 순서로 읽고 `UPDATE`는 새 버전을 표 끝에 붙여 갱신한 행이 마지막에 나왔다(`SELECT * FROM t` · `LIMIT 2` · `GROUP_CONCAT(id)` · `ROW_NUMBER() OVER ()`가 갱신 뒤 MySQL과 달랐고, 기본키를 순서 없이 넣어도 입력 순서로 나왔다). 이제 표 전체를 읽는 스캔(한 표 읽기와 조인의 첫 표)은 기본키 순서로 행을 읽는다. **비용을 어떻게 막았나**: 처음 만든 판은 스캔마다 이웃한 행의 기본키를 비교해 5만 행 스캔이 14 → 30 ms로 두 배가 되었다(`bench_query` COUNT(*)). 그래서 표마다 "행 버전 전부의 기본키 순서 위치 목록"을 `ScanOrders`(`include/engine/scan_order.hpp`)에 두었다: UNIQUE 값 캐시와 같은 방식으로 표의 잠금이 배타로 잡힌 횟수(쓰는 문장은 모두 하나를 배타로 잡는다)를 도장으로 찍어, 도장이 같은 동안은 그 목록(이미 순서이면 null이라 아무 비용 없음)을 그대로 쓰고, 표가 바뀐 뒤 처음 읽을 때만 한 번 만든다(정수 기본키 하나면 행마다 키를 한 번 읽고 정렬, 나머지 키는 `ORDER BY`가 쓰는 정렬로). 색인·버킷으로 일부 행만 읽는 길은 읽은 행만 이웃 비교 후 필요할 때 정렬한다(`in_primary_key_order`). 기본키 없는 표와 조인의 나머지 표는 그대로(InnoDB의 숨은 행 번호/플래너의 선택까지 따라 하지 않음).

**R35 — `||`는 기본으로 논리 OR**: MySQL은 `sql_mode`에 `PIPES_AS_CONCAT`(또는 `ANSI`)가 없으면 `a || b`를 OR로 읽는다(`'a' || 'b'`는 0, `NULL || 1`은 1). `Parser`가 `||`를 `OR` 토큰으로 바꿔 우선순위·조건·값 자리가 `OR`와 같게 되고, 세션의 `sql_mode`가 `PIPES_AS_CONCAT`/`ANSI`를 담으면 옛 연결(`CONCAT`)이다(`Executor::pipes_as_concat`; 질의 캐시 키에도 모드가 들어감). `test_full.sql`·`test_full-ver2.sql`의 `||` 두 줄은 `CONCAT`으로 바꿨다.

**R36 — 값 자리의 행 비교**: `SELECT (1, 2) = (1, 2)`·`SELECT id, (a, b) < (0, 5)`·`(a, b) IN (SELECT …)`를 select 목록·`CASE`·`ORDER BY` 같은 값 자리에서도 쓴다(R30의 조건용 풀어쓰기를 값으로 감쌈; `parse_value_pred`).

**검증**:
- 신규 시험 `test_case_insensitive_names.cpp`(7 케이스): 질의(표·열·별칭·`T.*`·조인·상관 서브쿼리·`GROUP BY`/`HAVING`/`ORDER BY` 별칭·윈도·`UNION`의 `ORDER BY`·머리글이 쓴 글자 그대로인지), 쓰는 문장(INSERT/UPDATE 별칭/ODKU/REPLACE/INSERT…SELECT/DELETE), DDL(색인 이름·ALTER·중복 거절·뷰·외래키·`db.표`·DB 이름), 재시작 뒤, 변수·프로시저·트리거·함수, `||`(모드 셋), 행 비교와 기본키 순서(숫자 `9 < 10`, 복합 기본키, 갱신 뒤).
- **진짜 MySQL 8.4.9와 비교**: 이름 점검 문장 171개 + 별칭 모호성 8 + 조인 순서 11 — 일부러 두거나 이 항목 밖인 차이만 남음(`DESCRIBE`의 출력 모양, 자기 표를 서브쿼리로 읽는 `UPDATE`의 1093, `SHOW CREATE TABLE`, `SELECT @x := 7`, `information_schema`의 `COUNT(*)`, 내부 조인 `jg JOIN jo`에서 MySQL 플래너가 고른 구동 표); `corpus/tables.sql`에 약 180줄을 더하고 `mysql_conformance_tables.inc`를 다시 만들었다(MySQL의 답 기록), `functions.sql`의 `'a' || 'b'`가 이제 MySQL과 같아져 `mysql_conformance_functions.inc`의 그 줄이 "실행만"에서 "답 확인"으로 바뀌었다. 코퍼스 4개의 차이: 정렬 5 · 제약 7 · 함수 6 → 5 · 표 8 (그대로).
- 심은 버그 39종(이름 30종 + 기본키 순서·스캔 순서 캐시 9종): 처음 33종이 잡혔고 남은 여섯 중 다섯은 시험을 더해 잡음 — 자기 이름의 다른 대소문자로 `RENAME COLUMN`, 서로 다른 모드에서 같은 글자의 질의 캐시(모드를 바꾸기 전에 캐시된 답), 행 버전 수가 그대로인 변경(삭제 + `VACUUM` + 순서 없는 INSERT)이 순서 캐시를 낡게 하는 것, 복합 기본키에서 첫 열은 순서이고 둘째 열은 아닌 것, 색인으로 읽은 행의 순서(`RUSQL_DML_INDEX_MIN_ROWS=0`인 강제 모드에서만 이 길을 탐); 마지막 하나(저장 함수 키를 소문자로 모으는 한 줄)는 파서가 이미 소문자로 만든 이름이라 새 데이터에서는 같은 결과(옛 데이터 폴더의 섞인 철자 보호용으로 남김).
- 전체 시험 **718 케이스 / 1,481,474 assertions**을 Release 기본으로, 강제로(1,481,474) 통과; Debug 전체 기본(캐시 점검 모드 1,481,474)·강제(1,481,475) 통과; 빌드 간 차분(앞 빌드 대비) 10시드 × 99질의: 순서 없는 읽기가 기본키 순서가 되어 달라진 곳 말고는 없다 — 모든 차이가 행 순서에 따라 값이 달라지는 질의(`LIMIT`만 있는 것, 동점 `ORDER BY … LIMIT`, `JSON_AGG`/`GROUP_CONCAT`/`ARRAY_AGG`의 이은 순서, `VARIANCE`/`STDDEV`/`SUM(price)`의 부동소수점 마지막 자리)이다.
- 같은 빌드로: 모델 검증기 열한 가지(`verify_agg_arguments`·`agg_expressions`·`aggregates`·`compare`·`correlated`·`joins`·`null_expressions`·`orderby_distinct`·`sort_group`·`subqueries`·`value_expressions`) 위반 0; `verify_writes.py` 시드 1~8 위반 0; `dump_roundtrip.py`(진짜 `mysqldump` 파일 둘) 행이 모두 같고 mysql 클라이언트 오류 0; 크래시 퍼저 — 외래키 90라운드(시드 6000~6089, 캐시 점검 모드)·일반 60라운드·동시성 40라운드(확인한 행 21,533) 불일치 0.
- **속도** (`code/test/perf`, 5만 행·구 빌드와 번갈아): 단건 INSERT/DELETE 1만 건(7.3 s)·bulk 10만 건·점 조회(0.89 ms / 색인 0.11 ms)·트랜잭션 TPS는 변화 없음. 표를 쓰지 않고 반복하는 스캔(`COUNT(*) WHERE`·`SELECT … WHERE val < k`·`GROUP BY`)의 중앙값도 그대로(16~19 / 31 ms). 표를 쓴 직후의 첫 스캔만 순서 목록을 한 번 만드느라 5만 행에서 약 7 ms 더 든다(`bench_query`가 매번 UPDATE 뒤에 재는 방식으로는 `COUNT(*)` 14 → 21 ms, 조인 +9 ms). 처음 만든 판(스캔마다 이웃 비교)은 모든 스캔이 14 → 30 ms로 두 배가 되어 순서 목록 캐시로 바꿨다.

**한계**: ① 기본키 없는 표는 `UPDATE` 뒤에 갱신한 행이 끝에 나온다(InnoDB는 숨은 행 번호로 제자리); 조인의 둘째 표 이하와 플래너가 구동 표를 바꾼 내부 조인의 순서는 MySQL과 다를 수 있다. ② 세 부분 열 이름 `db.t.col`, `SHOW TABLES LIKE`/`FROM db`, `SELECT @x := 식`(select 안의 대입), `information_schema`에 대한 `COUNT(*)`(0행이 나옴), 윈도 함수·`GROUP_CONCAT(… ORDER BY …)`·행 비교·`||` 식의 결과 열 머리글(MySQL은 쓴 글자 그대로, 여기는 풀어쓴 글자 — 값은 같음)은 이 항목 밖이다. ③ 저장 프로시저·트리거 본문 안의 `IF`/`WHILE` 조건에 든 서브쿼리의 표 이름은 실행할 때 고치지 않는다.

### 10월 18일 (두 번째) — 진짜 MySQL 클라이언트가 보내는 문장들: DDL 여러 형태, 여러 동작의 `ALTER`, 세션 함수, `autocommit = 0`, `information_schema`, MySQL 프로토콜 계층 (R37)

**왜 이 항목인가**: 사용자 "계속 버그 수정 진행합니다". R32~R36을 닫은 뒤, DBeaver·`mysql` CLI·ORM·mysqldump가 실제로 보내는 문장 형태를 진짜 MySQL 8.4.9와 나란히 돌려(탐색 질의 `probe_x1`~`probe_x3`, `probe_b1`) 차이를 찾았다. 차이가 나오는 대로 닫고, 같은 문장을 `corpus/tables.sql`에 142줄(setup 10 + 비교 132) 더해 MySQL의 답을 `mysql_conformance_tables.inc`에 기록했다(기존 줄의 답은 한 글자도 바뀌지 않음 — 새 파일이 옛 파일로 시작함을 확인).

**DDL 형태** (전부 전에는 파싱 오류였다)
- `CREATE TABLE t LIKE s`(열·기본값·키를 복사, 행은 없음), `CREATE TABLE [IF NOT EXISTS] t [(열 정의 …)] AS SELECT …`(원본 열이 있으면 그 타입 그대로, 계산 열은 값에서 정수 → `BIGINT`, 소수 → `DECIMAL(30, 자릿수)`, 그 밖 → `TEXT`), `RENAME TABLE a TO b, c TO d`, `DROP TABLE a, b`(하나라도 없으면 아무것도 지우지 않음; `IF EXISTS`면 있는 것만), `TRUNCATE t`(`TABLE` 생략).
- `ALTER TABLE`의 **여러 동작을 쉼표로**: `ADD COLUMN … FIRST | AFTER c`, `MODIFY`/`CHANGE [COLUMN] … FIRST | AFTER c`, `ALTER COLUMN c SET DEFAULT v | DROP DEFAULT`, `ADD INDEX | KEY [이름] (열, …)`, 표 옵션 `ENGINE=…`; `ALTER VIEW v AS SELECT …`.
- `CREATE TABLE` 안의 `KEY | INDEX [이름] (열, …)` 줄이 색인으로 만들어진다(`information_schema.statistics`·`SHOW CREATE TABLE`에 나옴). 괄호 친 집합 연산 `(SELECT …) UNION (SELECT …) ORDER BY …`(`Parser::drop_parentheses_around_operands`), `JOIN db.표 [별칭]`, 세 부분 열 이름 `db.t.col`.
- **여러 동작의 `ALTER`는 전부 되거나 전혀 안 된다**: 처음 만든 판은 동작을 차례로 적용해 `ADD COLUMN a, ADD COLUMN A`가 "Duplicate column name"으로 실패해도 앞의 `a`가 남았다(비교 코퍼스의 `SELECT *`가 열 하나 더 가진 것으로 발견). 이제 `alter_actions_error`(`executor_ddl.cpp`)가 MySQL의 규칙대로 먼저 검사한다: `DROP`/`CHANGE`/`MODIFY`/`RENAME COLUMN`/`ALTER COLUMN`은 **문장 앞의 표**에 있던 열만 가리킬 수 있고(같은 문장에서 방금 더한 열은 오류, 진짜 MySQL이 `ADD COLUMN c INT, ALTER COLUMN c SET DEFAULT 4`를 거절하는 것을 확인), 새 이름은 그때까지의 이름과 겹치면 안 되며, 색인은 방금 더한 열도 가리킬 수 있다.
- **함께 찾아 고친 기존 버그**: ① `ALTER TABLE t DROP COLUMN 없는열`이 오류 없이 "dropped"라고 답함(MySQL 1091 "Can't DROP"), `RENAME COLUMN 없는열 TO x`도 같음; ② **`MODIFY`/`CHANGE COLUMN`이 그 열의 `UNIQUE KEY`를 지웠다**(새 정의에 `UNIQUE`가 없다고 열의 `unique`를 덮어씀 — MySQL에서 UNIQUE KEY는 열 정의와 별개의 색인이라 남는다) → 같은 값이 다시 들어가는 무결성 구멍; ③ `MODIFY … AFTER 없는열`이 타입을 바꾼 **뒤에** 오류를 냄.

**세션 기능**
- `LAST_INSERT_ID()`(문장이 만든 첫 번호; 값을 직접 준 INSERT는 바꾸지 않음), `LAST_INSERT_ID(식)`, `ROW_COUNT()`(마지막 쓰기 문장의 행 수, 읽기 뒤에는 -1), `FOUND_ROWS()`와 `SELECT SQL_CALC_FOUND_ROWS …`(LIMIT 없이 한 번 더 세어 기록). 질의 캐시가 이 함수들의 답을 저장하지 않게 비결정 함수 목록에 더함.
- **`SET autocommit = 0`**: 첫 쓰기 문장(또는 잠그는 읽기)이 트랜잭션을 시작하고 `COMMIT`/`ROLLBACK`/`SET autocommit = 1`이 끝낸다; `CREATE`/`DROP`/`ALTER`/`TRUNCATE`/`RENAME`은 열린 트랜잭션을 먼저 커밋한다(MySQL의 암묵 커밋). 전에는 설정만 기억하고 모든 문장이 즉시 커밋되었다.
- `SELECT … FOR UPDATE | FOR SHARE | LOCK IN SHARE MODE`가 트랜잭션 밖에서는 오류 대신 일반 읽기(autocommit에서는 잠금이 곧 풀리므로 MySQL과 같은 결과), `NOWAIT`·`SKIP LOCKED`는 받아서 무시. 기본 시스템 변수를 더 채움.

**`information_schema`를 표처럼 읽는다**: `TABLES`·`COLUMNS`·`STATISTICS` 등이 MySQL의 열 순서로 임시 표(파생 테이블과 같은 방식)가 되어 `WHERE`·`GROUP BY`·집계·`COUNT(*)`(전에는 0행)·`JOIN`(두 `information_schema` 표끼리 포함)이 그냥 된다. `COLUMN_KEY`는 `PRI`/`UNI`/`MUL`, `STATISTICS`는 기본키·UNIQUE·보조·해시·복합 색인과 카디널리티(살아 있는 서로 다른 값 수, 기본 정렬 규칙으로), `ENGINE`은 InnoDB. `SHOW TABLES [FROM db] [LIKE '패턴']`.

**윈도 함수와 머리글**: `COUNT(*) OVER ()`(옆의 `COUNT(col) OVER ()`와 함께), `LAG`/`LEAD(c, n, 기본값)`, `PERCENT_RANK`/`CUME_DIST`는 DOUBLE의 최단 표기(`0.3333333333333333`, 한 행이면 `0`); 별칭 없는 윈도 함수와 `ORDER BY`/`SEPARATOR`가 있는 `GROUP_CONCAT`의 결과 열 머리글이 쓴 글자 그대로(`Token::begin`/`end`로 원문 조각을 잘라 씀; R36의 한계 ②).

**함수**: `TIME_FORMAT`, `ADDTIME`, `SUBTIME`, `TIMEDIFF`, `PERIOD_ADD`, `PERIOD_DIFF`, `YEARWEEK`, `TO_SECONDS`, `BIT_COUNT`, `QUOTE`, `EXPORT_SET`, `MAKE_SET`, `REGEXP_INSTR`, `UTC_DATE`/`UTC_TIME`/`UTC_TIMESTAMP`, `SLEEP`; `UNIX_TIMESTAMP`/`FROM_UNIXTIME`은 시스템 시간대를 쓴다. 트리거의 `NEW.col`이 열 타입으로 맞춰진 값이 된다(`DECIMAL(10,2)`에서 `100 → 101`이 아니라 `100.00 → 101.00`).

**MySQL 프로토콜 계층(`server/src/mysql.cpp`)**: 클라이언트가 접속하며 보내는 문장을 엔진의 `information_schema` 질의와 새 `show_create_table_mysql`/`show_create_view_mysql` 위에 올렸다 — `SHOW COLUMNS | FIELDS`, `SHOW INDEX | KEYS`, `SHOW TABLE STATUS`, `SHOW CREATE TABLE | VIEW`(MySQL 8.4 형식: 백틱, `CONSTRAINT 표_ibfk_N`, `ENGINE=InnoDB AUTO_INCREMENT=n DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci`), `SHOW VARIABLES`의 값들, OK 패킷의 affected rows와 last insert id, `UNHEX('FF')` 같은 이진 값을 가진 열을 이진 열로 표시(전에는 연결이 어긋남). 비교 도구에 `vs_mysql.py --proto`(pymysql로 RuSQL의 MySQL 포트를 직접 두드림)와 코퍼스 `corpus/protocol.sql`(86문장)을 더했다.

**검증**
- 신규 시험 `test_ddl_session_forms.cpp`(10 케이스 · 214 assertions: DDL 형태, 여러 동작 ALTER의 원자성, `MODIFY`가 UNIQUE를 지키는지, `JOIN db.표`, 세션 함수, autocommit, `information_schema`, 윈도, 함수, 트리거); `test_executor_select.cpp`(트랜잭션 밖의 `FOR UPDATE`가 이제 성공)와 `test_null_expressions.cpp`(`FROM_UNIXTIME` 시험의 시간대를 UTC로 고정) 두 곳을 고침.
- **진짜 MySQL 8.4.9와 비교**: 새 코퍼스 142줄 — 표 코퍼스 전체(696줄)에서 남은 차이 8곳은 모두 이 항목 밖의 옛 것뿐(값 앞뒤 공백을 잃는 상자 출력, DOUBLE 열의 `SUM`/`AVG`가 정확한 십진수라 `118` 대 `118.0`, `WITH ROLLUP`). 기존 코퍼스 4개는 앞 빌드와 같은 결과(표 8 · 제약 7 · 함수 5 · 정렬 5곳); `--proto`로 `protocol.sql` 86문장은 4곳(아래 한계 ⑤).
- 심은 버그 25종(DDL 검사기 8 · autocommit·세션 카운터 5 · 함수 6 · 윈도·트리거·`information_schema`·`FOR UPDATE`·`JOIN db.표` 6): 처음 23종이 잡혔고 나머지 둘은 시험을 더해 잡음 — `YEARWEEK`의 연초·연말 주(`| 2` 플래그: 진짜 MySQL이 `YEARWEEK('2024-01-01')`에 `202353`을 주는 것)와, 트랜잭션 밖의 `FOR UPDATE`가 잠금을 남기는 것(두 세션 시험을 더함: 다른 세션의 `UPDATE`가 기다리다 실패했을 것).
- 전체 시험 **729 케이스 / 1,481,940 assertions**을 Release 기본으로, 강제로(1,481,940) 통과; Debug 전체 기본(캐시 점검 모드 1,481,941)·강제(1,481,941) 통과; 빌드 간 차분(앞 빌드 = R32~R36의 `c9dfed6`) 10시드 × 99질의 차이 0건.
- 같은 빌드로: 모델 검증기 열한 가지(`verify_agg_arguments`·`agg_expressions`·`aggregates`·`compare`·`correlated`·`joins`·`null_expressions`·`orderby_distinct`·`sort_group`·`subqueries`·`value_expressions`) 위반 0; `verify_writes.py` 시드 1~8 위반 0; `dump_roundtrip.py`(진짜 `mysqldump` 파일 둘, 102문장과 85문장) 거절 0·행이 모두 같고 mysql 클라이언트 오류 0; 크래시 퍼저 — 외래키 90라운드(시드 6000~6089, 캐시 점검 모드)·일반 60라운드·동시성 40라운드(확인한 행 21,723) 불일치 0.
- **속도** (`code/test/perf`, 5만 행·앞 빌드와 번갈아 두 번씩): 스캔·집계·`GROUP BY`·`DISTINCT`·`ORDER BY`·조인 15가지의 중앙값이 앞 빌드와 같다(앞 빌드 → 이 빌드, 두 번씩: `COUNT(*)` 23.2 / 23.0 → 23.3 / 23.0 ms, `GROUP BY` 38.1 / 39.8 → 36.3 / 36.9 ms, 조인 5만 행 출력 280 / 251 → 251 / 255 ms — 오차 안); 단건 INSERT/DELETE 1만 건(7.4~7.6 s)·bulk 10만 건(0.82~0.84 s)·점 조회(색인 0.12 ms)·트랜잭션 처리량도 그대로.

**한계**: ① ~~**MySQL 프로토콜이 모든 열을 문자열(VARSTRING)로 보낸다**~~ → 같은 날 세 번째 항목(R38)에서 고침(파이썬 클라이언트가 `1`을 정수로 받음). ② `SUM`/`AVG`의 DECIMAL 자릿수(`SUM(pay)`가 `30.7500`, MySQL은 `30.75`; `pay * 2`가 `20.5`, MySQL은 `20.50`). ③ `SELECT @x := 식`(select 안의 대입), `SUM(SUM(x)) OVER ()`, `INTERVAL()`, `GET_FORMAT`, `ALTER … ADD/DROP PRIMARY KEY`, `WITH ROLLUP`. ④ 상자 표 출력이 값 앞뒤의 공백을 잃는다(`' x '` → `x`; 그 값을 `UPPER`·`SUBSTRING`·`TRIM` 등에 넣은 결과가 달라짐). ⑤ `SHOW TABLE STATUS`의 이름·엔진·행 수 말고 다른 값, `SHOW ENGINES`(엔진 하나), `SHOW COLLATION`의 `Pad_attribute` 열, `@@version`(핸드셰이크와 같게 `5.7.0-rusql`, 일부러), `tinyint(1)` 표시 폭. ⑥ 기본키 없는 표의 `UPDATE` 뒤 행 순서(R34의 한계 ①)는 그대로.

### 10월 18일 (세 번째) — MySQL 프로토콜이 결과 열의 타입을 알려 준다 (R38)

**왜 이 항목인가**: 사용자 "계속". R37이 남긴 첫 후보. `server/src/mysql.cpp`는 결과의 모든 열을 `VARSTRING`(문자열, 타입 253)으로 보냈기 때문에, 진짜 MySQL이라면 정수·`Decimal`·`date`로 돌려줄 값을 파이썬(`pymysql`)이 `'1'`·`'10.50'`·`'2024-01-31'`로 받았고(`SUM(id)`에 1을 더할 수 없음), JDBC·DBeaver 같은 도구도 숫자 열을 문자열로 다뤘다. 비교 도구에 `vs_mysql.py --proto --types`(두 서버의 열 타입 종류 — 정수·소수·실수·날짜·날짜시간·시간·문자열 — 를 대조)와 코퍼스 `corpus/protocol_types.sql`(30문장)을 더해 진짜 MySQL 8.4.9와 맞췄다.

**구조 두 겹**
- **엔진: `Executor::result_columns(sql)`**(새 `executor_result_types.cpp`): 서버가 방금 돌린 SELECT를 다시 읽어(읽기 잠금 아래, 이름은 `resolve_names`로 정리) 결과 열마다 타입을 정한다. 표의 열은 표의 타입(별칭·대소문자·`db.t.col`·`*`·`t.*`·조인; `USING`/`NATURAL` 조인은 공통 열이 한 번만 나오므로 말하지 않음), 파생 표·뷰·집합 연산의 왼쪽·`information_schema`는 안쪽 SELECT를 따라가며 정한다. 집계는 MySQL의 규칙대로: `COUNT` = `BIGINT`, `SUM(정수)` = `DECIMAL(32,0)`, `SUM(DECIMAL(p,s))` = `DECIMAL(p+22,s)`, `AVG(정수)` = `DECIMAL(14,4)`, `AVG(DECIMAL(p,s))` = `DECIMAL(p+4,s+4)`, `FLOAT`/`DOUBLE`의 합·평균과 `STDDEV`/`VARIANCE` = `DOUBLE`, `MIN`/`MAX`는 인자의 타입, `BIT_AND`/`BIT_OR` = 부호 없는 `BIGINT`; 윈도 함수도(`ROW_NUMBER` = `BIGINT`, `PERCENT_RANK` = `DOUBLE`, `SUM(…) OVER` 등은 인자를 따름). 산술식·비교·숫자를 돌려주는 함수(`LENGTH`, `ROUND`, `LAST_INSERT_ID` …)는 "값으로 판단"하고, 문자열 함수·`CASE`·`IF`·`COALESCE`·서브쿼리는 문자열 그대로 둔다(숫자처럼 보이는 문자열이 정수가 되는 일이 없도록 식의 모양으로 먼저 가른다).
- **서버: `WireType`·`wire_type_for`**(`mysql.cpp`): 열 정의 패킷에 MySQL의 타입 코드(`TINY`·`SHORT`·`LONG`·`LONGLONG`·`YEAR`·`FLOAT`·`DOUBLE`·`NEWDECIMAL`·`DATE`·`DATETIME`·`TIMESTAMP`·`TIME`), 표시 길이, 소수 자릿수, `UNSIGNED` 플래그, 숫자·날짜가 쓰는 이진 문자 집합과 `BINARY` 플래그를 싣는다. **타입을 믿기 전에 셀을 검사한다**: 그 열의 셀이 하나라도 타입이 요구하는 모양(정수, 소수, `YYYY-MM-DD`, `YYYY-MM-DD hh:mm:ss[.f]`, `[-]h:mm:ss`)이 아니면 그 열은 예전처럼 문자열로 보낸다(클라이언트가 변환하다 죽는 것을 막음; 예: `SELECT id FROM t UNION SELECT label FROM t`). 찾은 열 수가 응답의 열 수와 다르면 전부 문자열이다. 준비된 문장(`COM_STMT_EXECUTE`)과 `SHOW …`처럼 호환 계층이 만드는 답은 전과 같이 문자열이다(앞엣것은 이진 결과 행 형식이 아직 없음).

**검증**
- 신규 시험 `test_result_types.cpp`(1 케이스 · 32 assertions): 표 열·별칭·`db.t.col`·조인·`USING` 조인(말하지 않음)·집계 규칙·윈도·파생 표·뷰·`UNION`·`information_schema`·식(숫자인 것과 문자열인 것)·말할 수 없는 문장.
- **진짜 MySQL 8.4.9와 열 종류 비교** (`--proto --types`): 30문장 중 4곳만 다름 — `ROUND(double 열, 1)`을 소수로 읽음(MySQL은 실수), `2 * 3.5` = `7.0`이 정수로 읽힘(값 `7`로 판단하므로), `information_schema.columns`의 열 수(28 대 32; 앞선 항목의 한계), `VERSION()`(`2.3.0` 대 `8.4.9`, 일부러). `protocol.sql`(86문장)은 이전 4곳(`SHOW TABLE STATUS`·`SHOW ENGINES`·`SHOW COLLATION`·`@@version`)에 숫자 열이 아직 문자열인 5곳 — `SELECT @@autocommit`, 저장 함수의 결과 `SELECT tax(100)`(MySQL은 `DECIMAL`), `SHOW WARNINGS`, `SHOW CHARACTER SET` — 이 더해져 9곳(`--types`를 켠 때만).
- 심은 버그 27종(엔진 13: 합·평균 정밀도, `MIN`/`MAX`, `COUNT(DISTINCT)`, `USING` 조인, `UNION` 왼쪽, `FROM` 없는 SELECT, 세션 함수, `information_schema` 숫자 열, 뷰, 부호 없는 `BIT_AND`, `DOUBLE` 집계, 별칭 열; 프로토콜 14: 각 셀 검사, 각 타입 코드, 식의 값 판단, 실행한 문장 넘기기, 타입 전체 끄기, 셀 검사 무시): 모두 잡힘. 엔진 쪽에서 둘이 먼저 죽지 않았다 — 열 이름이 아닌 숫자 리터럴을 `Column`으로 읽는 가지는 파서가 리터럴을 식(`Num`/`Str`)으로 만들어 한 번도 타지 않는 죽은 코드라 지웠다.
- 전체 시험 **730 케이스 / 1,481,972 assertions**을 Release 기본으로, 강제로 통과; 빌드 간 차분(앞 빌드 = R37) 5시드 × 99질의 차이 0건; `dump_roundtrip.py` 둘(102문장과 85문장) 거절 0·행이 모두 같고 mysql 클라이언트 오류 0. 이 항목으로는 Debug·모델 검증기·크래시 퍼저를 다시 돌리지 않았다(엔진에는 읽기 전용 멤버 함수 하나만 더해졌고 그 밖은 `mysql.cpp`).
- **속도** (MySQL 프로토콜, `pymysql`, 앞 빌드와 번갈아 두 번씩): 점 조회 600건의 평균 0.35~0.37 ms → 0.36~0.37 ms, 5만 행 조회 2090 / 2093 ms → 2080 / 2163 ms — 오차 안(응답마다 SELECT를 한 번 더 읽는 비용이 보이지 않음).

**한계**: ① `ROUND(실수 열, n)`처럼 식의 타입은 모양과 값으로 판단하므로 MySQL이 `DOUBLE`로 말하는 것을 소수로, `7.0`을 정수로 읽을 수 있다. ② `SHOW …`·`DESCRIBE`·호환 계층의 답, 시스템 변수(`SELECT @@autocommit`)와 저장 함수의 결과(`RETURNS DECIMAL(…)`를 알려 주지 않음), 준비된 문장의 결과는 여전히 문자열. ③ 문자열 계열은 모두 `VARSTRING`(MySQL은 `TEXT`를 `BLOB`, `ENUM`/`SET`/`JSON`을 따로 말함) — 클라이언트에는 같은 문자열. ④ 열의 `NOT NULL`·`PRIMARY KEY`·`AUTO_INCREMENT` 플래그와 표 이름·데이터베이스 이름은 아직 열 정의에 싣지 않는다(JDBC의 `isNullable`/`getTableName`).

### 10월 18일 (네 번째) — DECIMAL 결과를 MySQL이 쓰는 자릿수로 (R39)

**왜 이 항목인가**: 사용자 "계속 진행합니다". R38이 남긴 다음 후보(`SUM(DECIMAL)`이 `30.7500`). 비교 도구가 숫자를 값으로만 견주어(`30.7500` = `30.75`) 이 차이를 지금까지 가렸다. `vs_mysql.py --exact`(글자까지 같아야 함)를 더해 진짜 MySQL 8.4.9와 `--proto`로 견주니 23문장 중 15곳이 달랐다: `10.50 * 2`는 `21`(MySQL `21.00`), `10 / 4`는 `2.5`(`2.5000`), `SUM(price)`는 `35.7500`(`35.75`), `AVG(price)`는 `11.9167`(`11.916667`), `ROUND(price / 3, 2)`는 `3.5`(`3.50`) …

**고친 것**
- **산술 결과의 자릿수**: `+ -`는 두 쪽 중 큰 자릿수, `*`는 두 자릿수의 합, `/`는 나뉘는 쪽 + 4를 그대로 쓴다(`arithmetic_text`가 `NumVal::scale`을 이미 알고 있었는데 끝에서 `compact()`로 뒤의 0을 지웠다 → `.str(scale)`): `10.50 * 2` = `21.00`, `10 / 4` = `2.5000`, `1.5 * 2` = `3.0`, `0 / 5` = `0.0000`, `price * rate` = `13.12500`. 함수에 넘기는 정확한 숫자도 지우지 않는다(`ROUND(price / 3, 2)`의 입력은 `3.500000000`).
- **SUM / AVG의 자릿수**: `SUM`은 값이 쓰인 자릿수 가운데 가장 많은 것(DECIMAL(10,2)의 합은 2자리, 정수의 합은 정수), `AVG`는 그 자릿수 + 4(DECIMAL(10,2) → 6자리, 정수 → 4자리); 윈도 `SUM/AVG … OVER`, `DISTINCT`, `GROUP BY`, `HAVING`, 식 안의 집계도 같다(`DecimalSum::max_places`, `average_text(divisor, places)`). 전에는 4자리로 채웠다(`35.7500`).
- **DOUBLE / FLOAT / 문자열 인자는 DOUBLE로**: MySQL은 이것들을 DOUBLE로 더하고 평균해 가장 짧은 글자로 쓴다(`0.1 + 0.2` = `0.30000000000000004`, `SUM(문자열 열)` = `107.05499999999999`, `AVG(FLOAT 열)` = `1.4166666666666667`): 전에는 정확한 십진수로 더해 `107.055`·`1.4167`이었다. 어느 쪽인지는 바인더가 알려 준다 — `SelectColumn::Agg`/`AggAlias::arg_approx`, 윈도 `WinFunc::col_approx`, `HAVING`·식 안의 집계 참조(`SUM(d)`)의 `Col::approx` — 인자가 FLOAT/DOUBLE 열, 문자열 열, 문자열 리터럴, 부동소수점 함수(`SQRT`, `POW` …)를 쓰는 식이면 참이다.
- **문자열 피연산자는 DOUBLE**: `2 * '2.5'`, `문자열 열 * 2`가 DOUBLE로 계산되어 `5`, `'10.50' * 2` = `21`(전에는 정확한 십진수로 계산해 `5`가 우연히 맞았고, 자릿수를 지키게 하자 `5.0`이 되려던 것을 이 규칙으로 막았다).
- **`ROUND` / `TRUNCATE` / `ABS` / `MOD`**: DECIMAL의 `ROUND`/`TRUNCATE`는 가진 자릿수 안에서 요청한 자릿수까지만 쓴다(`ROUND(10.50, 4)` = `10.50`, `ROUND(12.00, 1)` = `12.0`, `ROUND(3.5000, 2)` = `3.50`, `ROUND(price, 0)` = `11`); `ABS`는 쓰인 자릿수를 지키고(`ABS(-10.50)` = `10.50`); `MOD`는 DECIMAL이면 정확한 나머지를 두 쪽 중 큰 자릿수로 쓴다(`MOD(10.50, 3)` = `1.50`, 부호는 나뉘는 쪽). 전에는 모두 뒤의 0을 지우거나 `MOD`가 부동소수점으로 계산했다.
- **프로토콜 열 타입**(R38 후속): `d * 2`·`ROUND(d, 1)`·`POW(qty, 2)`처럼 DOUBLE 열·문자열·부동소수점 함수가 든 식은 `DOUBLE`, `SUM`/`AVG(문자열 열)`도 `DOUBLE`로 알린다.

**검증**
- 신규 코퍼스 `corpus/decimals.sql`(32문장: 열, 합·평균, 식 안의 집계, 곱·나눗셈, `ROUND`/`TRUNCATE`/`ABS`/`MOD`/`CAST`, 리터럴, 윈도, `GROUP BY`/`HAVING`, `UPDATE`/`INSERT`의 저장값)를 `--exact`로 진짜 MySQL과 견주어 차이 0; `--emit`이 새 종류 **5**(글자까지 같은지)로 `mysql_conformance_decimals.inc`를 만들고 `test_mysql_conformance.cpp`가 글자 그대로 견준다. 처음 한 곳(`0.0001 * 0.001`)은 파이썬 `Decimal('1E-7')`의 표기 때문에 생긴 도구의 차이라 문장을 바꿨다. `DOUBLE` 열은 클라이언트가 실수로 받아 글자 비교가 맞지 않으므로 이 코퍼스에 두지 않았다(`probe_dec.sql`은 `--proto --exact`로: 15곳 → 2곳).
- 기대값을 고친 기존 시험 14곳(`10 / 4`, `SUM(DECIMAL)`, `ROUND(n, 2)` …)은 모두 진짜 MySQL의 출력으로 확인했다. 이 과정에서 **`SUM(문자열 열)`의 글자까지 MySQL과 같아졌고**(`107.05499999999999`), 모델 검증기 둘(`verify_agg_arguments`, `verify_value_expressions`)의 기준이 옛 규칙(AVG 4자리)이어서 "AVG는 4번째 자리의 반 단위 안"으로 고쳤다.
- 심은 버그 23종(자릿수 추적 · 합·평균의 자릿수 · 산술 결과의 글자 · 문자열 피연산자 · `ROUND`/`ABS`/`MOD` · 바인더의 DOUBLE 표시 · select 목록·키 경로·윈도에서 DOUBLE 알림 · 프로토콜 타입): 17종은 바로 잡혔고 여섯(문자열 열의 산술, `AVG(FLOAT)`, 식 안의 `SUM(DOUBLE)`, 윈도 `SUM`/`AVG`의 DOUBLE, `SUM(SQRT(n))`)은 처음에 살아남아 새 시험 `test_decimal_display.cpp`(2 케이스 · 40 assertions, 기대값은 전부 진짜 MySQL 8.4.9의 출력)를 더해 잡았다. 헤더(`numeric_text.hpp`)의 변이는 하나에 19분씩 걸려 같은 곳의 비슷한 셋은 돌리지 않았다.
- 전체 시험 **733 케이스 / 1,482,083 assertions**을 Release 기본으로, 강제로(1,482,081) 통과; Debug 기본(캐시 점검 모드 1,482,082)·강제(1,482,081) 통과; 빌드 간 차분(앞 빌드 = R38) 10시드 × 99질의: 숫자를 값으로 견주는 새 옵션 `--number-form`으로 남는 것은 모두 `AVG(DECIMAL 열)`이 소수 6자리가 된 것뿐(`52.4761` → `52.476111`); 모델 검증기 열한 가지(시드 1, 그중 다섯은 시드 1~5)·`verify_writes.py` 시드 1~8·`mysqldump` 왕복 둘(102문장과 85문장, 거절 0)·크래시 퍼저 셋(외래키 90 · 일반 60 · 동시성 40라운드) 위반·불일치 0.
- **속도** (5만 행, 앞 빌드와 번갈아 두 번씩): 스캔·집계·조인 15가지의 중앙값이 앞 빌드와 같다(`SUM / AVG` 40.2 / 38.7 → 40.4 / 39.4 ms, 조인 5만 행 출력 343 / 326 → 335 / 329 ms; 이 기계가 앞 항목 때보다 약 20% 느려 절대값은 높지만 두 빌드가 같다); 단건 INSERT/DELETE 1만 건(8.5 s)·bulk 10만 건(1.1 s)·점 조회·트랜잭션 처리량도 그대로.

**한계**: ① **몫을 들고 있는 값의 숨은 자릿수**: MySQL은 나눗셈의 몫을 9자리(`1.666666666`, 끝을 잘라서)로 들고 있고 보이는 자릿수로 쓸 때만 반올림한다. 그래서 `AVG(v)`(1, 2, 2의 평균)는 `1.6667`로 보이지만 `AVG(v) * 3` = `5.0000`, `AVG(v) = 1.6667`은 거짓, `AVG(v) / 1` = `1.66666667`이고, `SUM(n / 3)`(n = 2, 3, 5)은 `3.3333`, `AVG(n / 3)`은 `1.11111111`이다(진짜 MySQL 8.4.9에서 확인). 여기는 쓰이는 값(보이는 4자리)으로 이어 계산해 `5.0001`·참·`3.3334`·`1.11113333`이다(`DATE.md` 앞 항목의 "`AVG`는 MySQL과 같다"는 틀린 말이었다). 합이 정확히 나누어떨어지는 값에서는 같다. ② `COALESCE`/`IFNULL`/`IF`/`CASE`가 DECIMAL을 고르면 MySQL은 그 열의 자릿수로 쓰지만(`COALESCE(NULL, 0)`이 DECIMAL(10,2)와 만나면 `0.00`) 여기는 고른 값 그대로(`0`). ③ 18자리를 넘는 DECIMAL의 `SUM`은 DOUBLE 경로로 가서 끝자리가 틀린다(`12345678901234567891` → `…168`). ④ `UNION`이 두 DECIMAL을 합칠 때 자릿수를 맞추지 않는다. ⑤ `DOUBLE` 합의 부동소수점 오차는 이제 MySQL과 같지만 합산 순서가 다르면(행 순서) 마지막 자리가 다를 수 있다.

### 10월 18일 (다섯 번째) — 결과 표를 다시 읽을 때 값의 앞뒤 공백이 사라지던 것, CTE 열의 타입 (R40)

**왜 이 항목인가**: 사용자 "계속 고칩니다". R39가 남긴 다음 후보 "상자 표 출력이 값 앞뒤 공백을 잃음"을 파 보니 보이는 모양의 문제가 아니라 **데이터가 바뀌는 버그**였다. 엔진은 결과를 상자 표 글자로 쓰고, 파생 테이블·CTE·`UNION`·`INSERT … SELECT`·`CREATE TABLE … AS SELECT`·`IN (서브쿼리)`·스칼라 서브쿼리가 그 글자를 다시 읽는다. 셀은 열 너비만큼 공백을 채워 쓰므로 값 양 끝의 공백이 채움과 구별되지 않았고, 읽을 때 지워졌다: `' x '`를 `INSERT INTO t2 SELECT s FROM t`로 옮기면 `'x'`가 되고, `WHERE s IN (SELECT s …)`는 `'bob '`을 `'bob'`과 같다고 보았다(기본 정렬 규칙은 NO PAD라 둘은 다른 값). MySQL 프로토콜 계층·데스크톱 클라이언트(Tauri)·MCP 서버·비교 도구도 같은 글자를 같은 방식으로 읽어 같은 값을 잃었다. 고치는 중 두 번째 버그가 드러났다: CTE의 임시 테이블이 모든 열을 글자 타입으로 만들어 `WITH c AS (SELECT id FROM t) SELECT id FROM c ORDER BY id`가 `1, 10, 2, 9`로 정렬되고 `MAX(id)`가 `9`였다(`id`가 1, 2, 9, 10일 때).

**고친 것**
- **셀을 쓰고 읽는 규칙을 한 곳으로** (`engine/result_text.hpp`: `escape_cell` / `unescape_cell` / `split_row`): 전부터 쓰던 `\\`(역슬래시)·`\n`(줄바꿈)·`\|`(막대)에 더해 **값 양 끝의 공백·탭을 `\s`·`\t`로 쓴다**(`' x '` → `| \sx\s |`; 공백뿐인 값은 전부). 셀 읽기는 모두 `split_row`: 이스케이프된 막대는 경계가 아니고, 첫 막대 앞과 마지막 막대 뒤는 셀이 아니며, 공백뿐인 조각은 빈 셀. 읽는 곳 — 엔진 안의 세 곳(`parse_table_output`, `extract_cells_from_output`, `extract_values_from_output`), `RETURNING` 표를 쓰는 곳(데이터와 머리글), MySQL 프로토콜 계층(`parse_table`·`box_table`), Tauri 백엔드(`main.rs`의 `split_row`), MCP 서버(`_split_row`), 비교 도구(`vs_mysql.py`; 전에는 `|`·역슬래시·가장자리 공백을 "도구의 차이"로 넘기던 곳)가 같은 규칙을 쓴다.
- **CTE 열에 타입** (`Executor::derived_column_defs`, `exec_with`의 두 경로): 열이 보여 주는 표 열이면 그 타입, 산술식이면 값을 보고 BIGINT/DECIMAL/DOUBLE, 그 밖은 글자. 바인더가 CTE를 한 번 더 읽으므로 이 타입이 `ORDER BY`·`MAX`·`SUM`·비교의 방식을 정한다. CTE가 다른 CTE를 읽는 경우와 재귀 CTE(기초 질의로 정함)도 같다.
- **숫자를 내는 식을 더 알아봄** (`Resolver`): `COALESCE`/`IFNULL`/`NULLIF`/`GREATEST`/`LEAST`/`IF`/`CASE`가 숫자에서 숫자를 고르면 숫자(`gives_numbers`; 전에는 "글자로 둔다"였다, R38), `CAST(x AS SIGNED/UNSIGNED/INT/DECIMAL(10,2)/DOUBLE …)`·`CONVERT`(`cast_to_number`), 식 안의 집계(`COALESCE(SUM(x), 0)`; `COUNT`/`SUM`/`AVG`는 숫자, `MIN`/`MAX`는 인자를 따름 — `aggregate_is_number`), select 목록의 스칼라 서브쿼리 `(SELECT MAX(id) FROM t)`는 안쪽 열의 타입. 글자 인자가 하나라도 있으면(`COALESCE(name, 'x')`, `IF(c, 'a', 'b')`, 안 타는 가지여도) 글자 그대로. CTE 열의 정렬·`MAX`가 맞아지고, MySQL 프로토콜은 `COALESCE(id, 0)`·`CASE WHEN … THEN id ELSE 0 END`를 `BIGINT`처럼 값으로 판단해 알린다(MySQL도 `LONGLONG`).

**검증**
- 신규 시험 `test_result_text.cpp`(3 케이스): 무작위 2만 행(알파벳 `{공백, 탭, \, |, 줄바꿈, a, n, s, t, 0}`, 길이 0~8, 열 1~4개, 여분 채움)을 쓰고 읽어 같은지 + SQL 시험(`' x '`·`'a|b'`·`'  '`·`''`·탭·줄바꿈·역슬래시·`'bob'`/`'bob '`이 `SELECT`·파생 테이블·CTE·`UNION`·`CREATE TABLE AS`·`INSERT … SELECT`·`IN (서브쿼리)`·행 비교 `(id, s) IN (…)`·스칼라 서브쿼리·`COUNT(DISTINCT)`·`LENGTH`·`RETURNING`을 지나 그대로이고 보이는 글자가 `| \sx\s |`) + CTE 열 시험(열·산술·`COALESCE`·`IFNULL`·`CASE`·`IF`·CTE의 CTE·재귀 CTE·글자 열은 글자로 정렬). 기존 시험의 셀 읽기 도우미(`table_cells`·`cells()`)도 같은 `split_row`로 바꿨다. `test_result_types.cpp`는 `IF(id > 1, 1, 2)`·`COALESCE(t, 0)`가 이제 `value`이도록 고쳤다.
- 진짜 MySQL 8.4.9와: 표 코퍼스(`corpus/tables.sql`)에 공백·막대·역슬래시·줄바꿈 탐침(`sp`, `sp_copy`)과 CTE·파생 테이블·뷰·`CREATE TABLE AS` 탐침(`nn`, `nn2`, `nvv`)과 CTE 열 타입 탐침(`cq`·`cr`·`ctm`·`ct`: `COALESCE`/`IFNULL`/`NULLIF`/`GREATEST`/`CASE`/`IF`, `CAST`, 집계, 서브쿼리)을 더해 699문장 중 차이 1(`WITH ROLLUP`, 전부터); 전에는 CTE 탐침 중 `COALESCE`/`CASE`/`IF` 열 19문장에서 11곳, `CAST`·집계·서브쿼리 열 17문장에서 7곳이 달랐다. `--emit`으로 `mysql_conformance_tables.inc`를 다시 만들었고 "이 문장은 실행만 함"이던 7줄이 MySQL과 견주는 줄이 되었다. MySQL 프로토콜(`--proto`)로도 같은 699문장이 같다; `protocol.sql`·`protocol_types.sql`은 R39 빌드와 같은 차이(`--types`로 9곳·2곳); `functions`(3곳)·`constraints`(7곳)·`collation`(0곳) 코퍼스도 앞 빌드와 같은 차이뿐이다(R39 빌드로 돌리면 새 도구가 `\s` 없이 쓴 가장자리 공백을 못 맞춰 `functions`에서 2곳이 더 보인다).
- Rust: `cargo test --offline cell_tests` 통과(Tauri의 `split_row`).
- 전체 시험 **736 케이스 / 1,502,403 assertions**을 Release 기본으로 통과; 같은 시험을 캐시 점검 모드(`RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0`)·`RUSQL_DML_INDEX_MIN_ROWS=0`으로 Release와 Debug 모두에서 통과(1,502,402~1,502,403). 마지막으로 `Resolver::add_table`의 CTE 이름 줄을 지운 뒤에는 Release 기본 시험·표 코퍼스(`--proto` 포함)·서버 빌드만 다시 돌렸다(그 줄을 뺀 채로도 이미 모든 시험이 통과함을 심은 버그 실행에서 봄).
- 심은 버그: 엔진·읽는 쪽·CTE 타입 40종 — 처음 한 번에 31종이 잡혔고, 살아남은 것 가운데 다섯은 시험을 더해 잡았다(`COALESCE(COUNT(*), 0)`·`MAX(DISTINCT id)`·`NULLIF(MAX(code), '')`·`NULLIF(code, 'x')`·`COALESCE(code, NULL)`·안 타는 글자 가지가 있는 `IF`/`CASE` → CTE 시험 몇 줄과 코퍼스 열 문장; 모두 진짜 MySQL 8.4.9의 답과 같음을 확인한 문장). 남은 것은 같은 동작인 것뿐 — 두 열 스칼라 서브쿼리는 어차피 오류(`z_subquery_item_width_unchecked`). 살아남은 것이 가리킨 코드는 지웠다: `SelectColumn::Func`/`CaseWhen` 항목에 더했던 가지(파서는 이제 둘을 만들지 않고 `IF(c, a, b)`는 `CASE`로 읽혀서 그 가지도 안 탔다), `gives_numbers`의 안 쓰이는 `IF` 가지, `Resolver::add_table`이 CTE 이름을 따로 찾던 줄(없어도 CTE의 CTE가 같은 답), `derived_column_defs`가 지우던 키·제약 6줄(열 정의에는 이름·타입만 있으면 됨). 열 정의의 자릿수·BIGINT/DECIMAL 구분 같이 겉으로 드러나지 않는 것(여섯)은 심은 버그를 다시 돌리지 않았다. `result_text.hpp`(헤더) 20종: 18종이 잡히고 둘(구분 문자를 공백만으로 자르는 것)은 같은 동작 — 이스케이프를 거친 셀의 끝에 날 탭은 없다. MySQL 프로토콜 계층 5종: `parse_table` 둘은 `tables.sql`(668문장 `--proto`)로, `box_table` 셋 가운데 하나는 `protocol.sql`(`SHOW CREATE TABLE`의 줄바꿈 셀)로 잡히고 둘(폭 계산을 이스케이프 전 글자 수로)은 보이는 모양만 달라 같은 동작.
- 모델 검증기 열한 가지(시드 1)·빌드 간 차분 10시드 × 99질의(앞 빌드 = R39, 숫자 형태 비교 없이 글자 그대로) 차이 0·`mysqldump` 왕복 둘(102문장과 85문장, 거절 0, 행이 모두 같음, mysql 클라이언트 오류 0)·`verify_writes.py` 시드 1~8·크래시 퍼저 셋(외래키 90 · 일반 60 · 동시성 40라운드) 위반·불일치 0(CTE 열 타입을 더하기 전 빌드로 돌렸고, 그 뒤에는 `Resolver`만 바뀌어 모델 검증기·빌드 간 차분·왕복을 마지막 빌드로 다시 돌렸다).
- **속도** (5만 행, 앞 빌드와 번갈아 두 번씩): 스캔·집계·조인 15가지의 평균이 앞 빌드와 같다(가장 느려진 것은 `SELECT id, val` 5만 행 출력 71.4 → 74.2 ms, +4%로 오차 안; `ORDER BY … LIMIT`·`LIKE`는 오히려 빨라 보이나 앞 빌드 첫 실행의 흔들림); 단건 INSERT/DELETE 1만 건(7.2 s)·bulk 10만 건(0.8 s)·점 조회(인덱스 0.13 ms)·트랜잭션 처리량도 그대로. 셀마다 `escape_cell`이 양 끝을 먼저 찾지만 값이 짧아 보이지 않는다.

**한계**: ① 명령줄 상자 표에서는 값 양 끝의 공백·탭이 `\s`·`\t`로 보인다(MySQL 클라이언트에는 프로토콜 계층이 되돌려 준다). ② CTE 열의 타입은 SELECT 목록에서 읽을 수 있는 만큼이다: 식 안의 스칼라 서브쿼리(`COALESCE((SELECT …), 0)`)와 `CASE` 안 서브쿼리 결과는 글자로 둔다(select 목록의 `(SELECT …)` 열 자체는 그 열의 타입). 파생 테이블·뷰는 바인더가 정한 열 종류로, CTE는 임시 표의 타입으로 비교 방식을 정해 두 길이 같은 답을 내지만 한 곳으로 합치지 않았다. ③ MySQL 프로토콜로 CTE를 읽는 SELECT는 결과 열 타입을 글자로 알린다(`Resolver`가 CTE를 모름). ④ `COALESCE`/`CASE` 안에 숫자와 글자가 섞이면 글자(MySQL도 그렇다); 숫자만 든 `COALESCE(NULL, NULL)`처럼 고를 값이 없으면 글자.
### 10월 19일 — 소수초 열(`DATETIME(3)`)과 날짜·시간 값의 여러 꼴, 실제 스키마가 쓰는 열 타입·인덱스 옵션, `ALTER TABLE … AUTO_INCREMENT = n` (R41)

**왜 이 항목인가**: 사용자 "계속 그냥 버그 수정합니다". 진짜 MySQL 8.4.9와 견주는 탐침을 새 영역(윈도, 날짜·시간, DML·DDL, 함수 목록)으로 넓혀 보니 MySQL이 받아들이는데 여기서 거절하거나 다른 답을 내는 곳이 많았다. 그 가운데 **스키마를 통째로 못 읽게 만드는 것**(`DATETIME(3)`, `TIMESTAMP(6)`, `DEC(4,1)`, `DOUBLE PRECISION`, `KEY … USING BTREE COMMENT '…'`, `CHECK … NOT ENFORCED`)과 **날짜·시간 값을 틀리게 저장하는 것**(소수초가 반올림이 아니라 버려짐, `'20240229134510'`·`'24-02-29'`·`'134510'` 거절)을 먼저 고쳤다. 윈도 문법(`OVER w`, 집계 위의 윈도)·`ANY_VALUE`·`JSON_ARRAYAGG`·`WITH ROLLUP`·생성 열·`DEFAULT (식)`은 `PLAN.md`에 남겼다.

**고친 것**
- **소수초 열**: `DATETIME(n)`·`TIMESTAMP(n)`·`TIME(n)`(n ≤ 6, 7 이상은 MySQL처럼 오류)을 타입이 가진다(`DataType`의 `fsp`; 저장은 `{"DateTime": 3}`, 없으면 전처럼 `"DateTime"`). 값은 그 자릿수로 **반올림**하고(`'13:45:10.678'` → `DATETIME`에서 `13:45:11`; 일곱째 자리가 먼저 반올림되어 `.4999999`는 `.5`) 올림이 초·분·시·일·월·년으로 번진다(`2024-12-31 23:59:59.5` → `2025-01-01 00:00:00`, `9999-12-31 23:59:59.5`·`838:59:59.5`는 범위 오류). 항상 정해진 자릿수로 쓴다(`DATETIME(3)` = `.100`). `SHOW CREATE TABLE`·`DESCRIBE`·`information_schema.columns`(`COLUMN_TYPE`·`DATETIME_PRECISION`)·`BACKUP`이 자릿수를 말한다.
- **`NOW(n)`·`CURTIME(n)`·`CURRENT_TIMESTAMP(n)`·`SYSDATE(n)`·`UTC_TIME(n)`·`UTC_TIMESTAMP(n)`**과 `DEFAULT CURRENT_TIMESTAMP(n)`·`ON UPDATE CURRENT_TIMESTAMP(n)`이 그 자릿수만큼의 소수초를 쓴다(버림; `engine/now_text.hpp`). 소수초 열의 기본값은 열의 자릿수를 따른다.
- **날짜·시간 값의 여러 꼴**: 숫자만으로 쓴 `YYYYMMDD`·`YYMMDD`·`YYYYMMDDhhmmss`·`YYMMDDhhmmss`(문자열이든 숫자든), 두 자리 해(`'24-02-29'` = 2024, 70 이상은 19xx), 한 자리 달·일·시(`'2024-2-9 3:4:5'`), `'T'` 구분자; `TIME`은 `'D HH:MM:SS'`·`'D HH'`·`'HHMMSS'`·`'MMSS'`·`'SS'`·숫자(`134510` = `13:45:10`)·`-838:59:59`까지(`'-1 1'`처럼 음수 `D HH`는 MySQL처럼 오류); `YEAR`는 두 자리(`'24'` = 2024, `70` = 1970).
- **열 타입 문법**: `DEC(p,s)`·`FIXED(p,s)`·`DOUBLE PRECISION`·`CHARACTER VARYING(n)`·`INT SIGNED`·`TEXT(n)`·`YEAR(4)`; `DECIMAL`·`DECIMAL(6)`은 `(10,0)`·`(6,0)`(전에는 `DECIMAL`이 `(10,2)`); `UNSIGNED`를 `DECIMAL`·`FLOAT`·`DOUBLE`에도(음수는 범위 오류, `-0.001` → `0.00`은 허용); **`REAL`은 `DOUBLE`**(전에는 단정밀 `FLOAT`로 읽혀 `0.1 * 3`이 `0.30000001…`).
- **인덱스 옵션**: `PRIMARY KEY (…)`·`UNIQUE KEY`·`KEY`·`INDEX`·`CONSTRAINT … UNIQUE`·`CREATE INDEX`·`ALTER TABLE … ADD INDEX|UNIQUE` 앞뒤의 `USING BTREE|HASH`·`COMMENT '…'`·`VISIBLE|INVISIBLE`·`KEY_BLOCK_SIZE [=] n`(`CREATE INDEX … USING HASH`는 해시 인덱스를 만들고, 표 안의 `KEY … USING HASH`는 전처럼 보통 인덱스). **`CHECK (…) [NOT] ENFORCED`**: `NOT ENFORCED`는 정의에만 있고 적용하지 않는다.
- **`ALTER TABLE t AUTO_INCREMENT = n`**: 전에는 문장은 되지만 아무 일도 안 했다. 이제 다음 번호가 n이 되고(표가 가진 가장 큰 번호보다 작으면 그 다음 번호), 저장되어 재시작 뒤에도 같다; `, ADD COLUMN …`처럼 다른 동작과 쉼표로 이을 수 있다(표 옵션 읽기가 다음 동작 앞의 쉼표를 삼키던 것도 고침). `information_schema.tables.AUTO_INCREMENT`가 다음 번호를 말한다(전에는 NULL).
- **`CURRENT_TIME`은 날짜를 돌려주고 있었다**(어휘 분석기가 `CURDATE`로 읽음): 이제 `CURTIME`; `CURRENT_TIME(n)`도.
- **함수**: `DATE_ADD`/`DATE_SUB`/`+ INTERVAL`의 **합성 단위** `YEAR_MONTH`·`DAY_HOUR`·`DAY_MINUTE`·`DAY_SECOND`·`DAY_MICROSECOND`·`HOUR_MINUTE`·`HOUR_SECOND`·`HOUR_MICROSECOND`·`MINUTE_SECOND`·`MINUTE_MICROSECOND`·`SECOND_MICROSECOND`(`INTERVAL '1:30' HOUR_MINUTE`, 모자라는 앞 부분은 생략된 것으로 읽음, 앞의 `-`는 전체를 음수로), `EXTRACT`의 같은 단위(`DAY_HOUR` = 일·시를 두 자리씩 이은 수), **`GET_FORMAT(DATE|TIME|DATETIME, 'USA'|'JIS'|'ISO'|'EUR'|'INTERNAL')`**, **`MAKETIME`**, **`MICROSECOND`**, **`TIMESTAMP(식[, 시간])`**; `CAST(… AS TIME)`·`AS TIME(n)`·`AS DATETIME(n)`·`AS YEAR`(`CAST AS TIME`과 `YEAR`는 아예 안 읽혔다); `SEC_TO_TIME`·`ADDTIME`·`SUBTIME`·`TIMEDIFF`·`TIME()`·`TIMESTAMP()`가 소수초를 잃지 않는다(인자에 쓰인 자릿수만큼).
- **`information_schema.columns`**: 정수 열의 `NUMERIC_PRECISION`(`TINYINT` 3, `SMALLINT` 5, `INT` 10, `BIGINT` 19, 부호 없는 `BIGINT` 20)과 `NUMERIC_SCALE` = 0.

**검증**
- 신규 코퍼스 `corpus/temporal.sql`(72문장: 소수초 열 일곱 INSERT·반올림·올림, 날짜·시간 꼴, 합성 단위, `GET_FORMAT`·`MAKETIME`·`TIMESTAMP`·`CAST`, 기본값의 자릿수, 열 타입·인덱스 옵션·`CHECK … NOT ENFORCED`, `AUTO_INCREMENT = n`)를 진짜 MySQL 8.4.9와 견주어 차이 0; `--emit`이 `mysql_conformance_temporal.inc`를 만들고 `test_mysql_conformance.cpp`가 새 사례 묶음으로 돈다. 비교 도구 `vs_mysql.py`는 소수초를 **열이 가진 자릿수로** 쓰고(전에는 버렸다) 끝의 0을 눈감지 않는다.
- 신규 시험 `test_temporal.cpp`(8 케이스): 재시작 뒤에도 자릿수가 남는 것, 올림이 번지는 것·범위 오류, `AUTO_INCREMENT = n`(재시작 포함, 다른 동작과 쉼표로 이음, 지운 행은 세지 않음), `UNSIGNED`와 `REAL`, 소수초 기본값·`ON UPDATE`, `UTC_TIMESTAMP`(이 기계의 UTC 차이와 견줌)와 `NOW(7)` 오류, 인덱스 옵션과 `USING HASH`.
- 전체 시험 **745 케이스 / 1,502,724 assertions**을 Release 기본으로 통과; 같은 시험이 캐시 점검 모드(1,502,723)·`RUSQL_DML_INDEX_MIN_ROWS=0`(1,502,722)으로도 Release에서, `RUSQL_DML_INDEX_MIN_ROWS=0`(1,502,724)으로 Debug에서 통과했다. Debug 캐시 점검 모드는 `test_unique_claims.cpp`(812)의 시간 검사(`t.ms < 150`, 175 ms) 한 건만 틀렸는데 그 시험만 따로 돌리면 통과한다(앞서도 부하에서 나온 알려진 흔들림).
- **심은 버그 155종**(소수초 읽기·반올림·올림 · 날짜·시간 꼴 · 시계 함수 · 합성 단위와 `EXTRACT` · `GET_FORMAT`·`MAKETIME`·`TIMESTAMP`·`CAST` · 열 타입 문법 · 인덱스 옵션 · `AUTO_INCREMENT = n` · `information_schema`): 처음 113종이 잡혔고, 살아남은 것에서 시험 줄을 더해(코퍼스 31문장, 단위 시험 네 케이스) 35종을 더 잡아 **148종이 잡혔다**. 남은 일곱은 같은 동작 — `NOW(7)`의 6자리 자름(소수 부분이 6자리라 `substr`이 같음), `UTC_TIMESTAMP`를 현지 시각으로(이 기계는 UTC), `GET_FORMAT(DATE, NULL)`의 NULL 검사(찾기 실패가 어차피 NULL), 7자리 넘는 자릿수 상한(`substr`이 같음), `NOW(-1)`(자릿수가 0으로 잘림), 지운 행이 가장 큰 번호에 들어가는지(지운 행은 표에 남지 않음), 합성 단위에서 숫자가 너무 많은 경우의 검사(지우면 정의되지 않은 동작이라 값이 정해지지 않음). 시험하다 찾은 진짜 구멍은 고쳤다: 일곱째 자리 올림의 초 단위 번짐(`TIME_TO_SEC('…10.9999995')`), `INTERVAL`의 숫자 없는 글자(0으로), `EXTRACT`의 음수 시간, `CAST('abc' AS YEAR)`(NULL)과 `CAST('0' AS YEAR)`(2000), `NOW(7)` 오류, `CREATE INDEX 이름 USING HASH ON …`.
- 모델 검증기 열한 가지(시드 1)·빌드 간 차분 10시드 × 99질의(앞 빌드 = R40, 글자 그대로) 차이 0·`mysqldump` 왕복 둘(102문장과 85문장, 거절 0, 행이 모두 같음, mysql 클라이언트 오류 0)·`verify_writes.py` 시드 1~8·크래시 퍼저 셋(외래키 90 · 일반 60 · 동시성 40라운드) 위반·불일치 0 — 이 검증은 마지막 작은 수정들(시험으로 찾은 위 구멍들) 전 빌드로 돌렸고, 그 수정은 모두 날짜·시간 함수 안이라 코퍼스와 전체 시험을 마지막 빌드로 다시 돌렸다.
- **속도** (5만 행, 앞 빌드와 번갈아 두 번씩): 스캔·집계·조인 15가지가 앞 빌드와 같다(가장 느려진 것은 `WHERE val < 300` 26.3 → 28.1 ms로 오차 안; 나머지는 같거나 조금 빠름); 단건 INSERT/DELETE 1만 건(7.6 s)·bulk 10만 건(0.9 s)·점 조회(인덱스 0.13 ms)·트랜잭션 처리량도 그대로.

**한계**: ① **날짜·시간 값을 산술에 쓰면 MySQL은 수로 바꾼다**(`dt + 0` = `20240229134511`, `tt + 0` = `134511`, `NOW() + 0`)는데 여기는 앞의 숫자만 읽는다(열 정의가 산술 노드까지 닿지 않음). `ENUM`·`SET`의 `+ 0`(번호·비트 값)도 같다. ② 글자 인자의 `ADDTIME`·`SUBTIME`·`DATE_ADD`: MySQL은 문자열 결과의 소수초를 6자리로 쓰는데(`01:30:01.250000`) 여기는 `ADDTIME`·`SUBTIME`이 인자의 자릿수대로(`01:30:01.25`; 시간 열에는 이쪽이 맞다), `DATE_ADD`는 6자리. ③ `UPDATE IGNORE`, 생성 열(`GENERATED ALWAYS AS`), `DEFAULT (식)`, `CREATE TEMPORARY TABLE`, `BINARY`/`VARBINARY`/`BIT`/`SERIAL`/`ZEROFILL`, 열 단위 `COLLATE utf8mb4_bin`은 아직 거절. ④ 한 열에 UNIQUE 제약 둘이면 이름이 하나만 남는다. ⑤ `INSERT IGNORE`·`ON DUPLICATE KEY UPDATE`가 실패한 행에 `AUTO_INCREMENT` 번호를 쓰는 InnoDB의 틈과 여기의 틈이 다르다(번호가 몇 칸 어긋남). ⑥ `RENAME COLUMN`이 `CHECK`가 쓰는 열을 바꾸는 것을 MySQL(3959)처럼 막지 않는다.

### 10월 19일 — 틀린 답을 내던 곳: 저장 루틴의 타입, 0으로 나누기가 NULL을 쓰던 것, `ALTER … MODIFY`의 값 변환과 인덱스, 준비된 문장, `!`, DOUBLE/FLOAT 글자 (R42)

**왜 이 항목인가**: 사용자 "문법 추가는 하지 않습니다. 버그만 계속 수정합니다. 완료되면 문서를 전부 갱신하고 커밋&푸시 해주세요". 진짜 MySQL 8.4.9와 견주는 탐침을 저장 루틴·DML·준비된 문장·`ALTER TABLE` 쪽으로 넓혀 **문법이 없어서가 아니라 답이 틀린 곳**만 골랐다(문법이 없는 것은 `PLAN.md`에 남겼다). 가장 나쁜 것은 조용히 틀린 값을 **저장**하는 둘이다: `INSERT … VALUES (1/0)`·`UPDATE … SET a = a / b`가 MySQL에서는 오류 1365인데 여기는 NULL을 썼고, 저장 프로시저 매개변수·변수는 타입이 없어 `DECIMAL(8,2)`에 `1.005`를 주어도, `INT` 변수에 `7 / 2`를 넣어도 글자 그대로(`1.005`, `3.5000`) 남았다.

**고친 것**
- **저장 함수·프로시저의 타입**: 매개변수·`DECLARE` 변수·`RETURNS`의 타입을 쓴 그대로(`DECIMAL(8,2)`, `INT UNSIGNED`, `VARCHAR(10)`) 간직하고(`Parser::read_declared_type` — 읽을 수 없는 타입이면 전처럼 건너뜀) 값을 그 타입에 맞춘다(`Executor::coerce_declared` → `coerce_column_value`): `f_total(2, 1.005)` = `2.02`, `INT` 변수에 `7 / 2` = `4`, `RETURNS DECIMAL(10,3)`의 `1 / 3` = `0.333`, `RETURNS INT`의 `x / 2`; 안 맞으면 `Out of range value for column 'x' at row 1`·`Incorrect integer value: 'abc' for column 'x' at row 1`; `SET v = …`·`SELECT … INTO v`·`DECLARE v T DEFAULT …`도 변수의 타입대로, `OUT` 매개변수가 넘기는 값도. 프로시저를 인자 수가 다르게 부르면 `Incorrect number of arguments for PROCEDURE d.p; expected 3, got 1`(전에는 모자란 인자를 빈 글자로 채움). 함수는 `{routine_expr, param_types, return_type}` 틀로 저장해(타입 없이 만든 옛 함수는 그대로 동작) 재시작 뒤에도 같다. 자릿수 없는 `DECIMAL`은 `DECIMAL(10,0)`으로 저장하고, 옛 카탈로그에 자릿수가 잘려 저장된 `DECIMAL`은 적용하지 않는다(`Parser::declared_column`).
- **오류로 끝난 `CALL`이 호출한 쪽의 변수를 되돌린다**: Rust 원본의 `?`처럼 피호출자의 변수가 남아, 그 뒤 문장의 같은 이름 열을 대신했다(실패한 `CALL p(12.5, 4, @r)` 뒤 `SELECT a FROM t`가 모든 행에서 `12.50`).
- **0으로 나누기**: `/`·`DIV`·`%`·`MOD`의 0 나누기는 `SELECT`에서 NULL이지만, 값을 쓰는 곳에서는 MySQL(엄격 모드)처럼 오류 1365 `Division by 0`이고 문장은 아무것도 쓰지 않는다: `INSERT … VALUES`·`INSERT … SELECT`·`UPDATE … SET`(`REPLACE`·`ON DUPLICATE KEY UPDATE`도 같은 길)·저장 함수의 결과·프로시저 변수 대입(`SET x = …`, `SELECT … INTO x`). `INSERT IGNORE`는 NULL을 쓰고, `@x` 대입은 NULL. 세는 곳은 한 줄(`sqlval::division_by_zero_count`, 스레드마다)이고 문장이 값을 계산하기 전·후의 차이로 판단한다. `COALESCE`·`IFNULL`이 모든 인자를 미리 계산해 안 타는 가지의 `1 / 0`이 오류가 되던 것은 첫 NULL 아닌 값까지만 계산하도록 고쳤다(`CASE`·`IF`는 원래 그랬다).
- **`ALTER TABLE … MODIFY COLUMN`이 값을 바꾼다**: 전에는 열 정의만 바꾸고 값은 글자 그대로 두어 `VARCHAR '007'` → `INT`가 `007`로 남았다. 이제 새 타입으로 변환하고(`'7'` → `7`, `7.9` → `8`), 안 맞으면 `Incorrect integer value: …`처럼 오류에 아무것도 바뀌지 않고, `NOT NULL`로 바꾸는데 NULL이 있으면 `Invalid use of NULL value`, `ENUM`·`SET`에 없는 값이면 오류. **`ADD`·`DROP`·`MODIFY COLUMN` 뒤에는 인덱스(기본키 포함)가 옛 행 사본을 가지고 있어** 인덱스로 찾은 행에 새 열이 없고(비어 보임) 바꾼 값이 안 보였다 → 인덱스를 다시 만든다.
- **준비된 문장**: `PREPARE p FROM '… ? …'; EXECUTE p USING @a, @b`의 `?`가 따옴표 안의 `?`(`'what?'`)까지 바뀌고 값이 따옴표 없이 들어갔다. 이제 따옴표·백틱 밖의 `?`만, 숫자는 그대로·`NULL`·나머지는 `'…'`(안의 `'`·`\`는 두 번)으로 쓰고, 인자 수가 다르면 `Incorrect arguments to EXECUTE`; 준비된 `INSERT` 뒤 `LAST_INSERT_ID()`가 맞다.
- **트리거가 쓴 행이 문장의 `LAST_INSERT_ID()`를 바꿨다**: 트리거가 끝나면 이전 값으로 되돌린다.
- **프로시저 본문의 `INSERT` 뒤 `LAST_INSERT_ID()`**: 가장 바깥 문장이 `CALL`이라 번호를 기록하지 않아, 프로시저 안의 `SELECT LAST_INSERT_ID()`도 `CALL` 뒤의 것도 앞선 `INSERT`의 번호(`1`)를 말했다 → 본문의 `INSERT`도 번호를 기록한다. **`CALL`의 답**: 본문의 마지막 문장이 `SET`·`INSERT`처럼 결과 표가 없으면 앞선 `SELECT`의 표가 사라졌다 → 본문이 만든 마지막 결과 표를 답한다.
- **`BEFORE INSERT` 트리거의 `NEW`는 열이 받아들일 꼴로**: 트리거가 쓴 그대로의 값(`1.005`)을 보아 `SET NEW.total = NEW.qty * NEW.price`가 `3.02`였다(열은 `DECIMAL(8,2)`라 `NEW.price`는 `1.01`이고 MySQL의 답은 `3.03`). 이제 트리거에 주는 `NEW`의 값을 열의 타입으로 맞춘 꼴로 주고(맞지 않는 값은 쓴 그대로 두어 `INSERT`가 거절한다), 트리거가 바꾼 값을 되받는 길은 그대로다.
- **홀로 있는 `!`는 `NOT`**: 어휘 분석기가 `!`를 버려 `WHERE !(a > 1)`이 `WHERE (a > 1)`이 되어 거른 행만 남겼다(`!=`는 그대로).
- **`DOUBLE`·`FLOAT` 열의 글자**: 넣은 글자가 그대로 남던 것(`1e10`이 `1e10`, `FLOAT`에 넣은 `3.14159265358979`가 그대로)을 값으로 바꿔 쓴다. `DOUBLE`은 MySQL의 글자(`1e10` → `10000000000`, `0.0001`, `1e15` 이상은 지수), `FLOAT`는 단정밀 값으로 읽어 그 값을 되읽을 수 있는 가장 짧은 글자(`3.14159265358979` → `3.1415927`, `1234567.891` → `1234567.9`). MySQL은 `FLOAT`를 여섯 자리로 보여 주지만(`3.14159`) 여기는 글자가 곧 값이라 단정밀 값이 그대로 남도록 했다(`f + 0`이 MySQL처럼 `1234567.875`에 가깝다). **64비트 부호 없는 끝값**: `CAST('18446744073709551616' AS UNSIGNED)`·`~0`·`1 << 63`·`18446744073709551615 | 0`이 마지막 자리에서 어긋나던 것.

**검증**
- 신규 코퍼스 `corpus/routines.sql`(193문장: 타입 있는 함수 열 가지·프로시저 일곱 가지, 0으로 나누기와 안 타는 가지, 준비된 문장과 인자 수, 트리거와 `LAST_INSERT_ID`, 프로시저 안 `INSERT`·`SELECT`의 답, `BEFORE INSERT` 트리거의 `NEW`, 범위 오류)과 `corpus/schema_change.sql`(69문장: `MODIFY`의 변환과 오류, `ADD`/`DROP`/`MODIFY` 뒤 조회, `CHANGE`·`RENAME`·`CREATE TABLE … LIKE|AS SELECT`·뷰)를 진짜 MySQL 8.4.9와 견주어 차이 0, `functions.sql`에 `!`·64비트 끝값 네 문장을 더함(그 파일의 옛 차이 셋은 그대로); `--emit`이 `mysql_conformance_routines.inc`·`mysql_conformance_schema_change.inc`를 만들고 `test_mysql_conformance.cpp`가 새 사례 묶음 둘로 돈다.
- 신규 시험 `test_wrong_answers_r42.cpp`(10 케이스: DOUBLE/FLOAT 열 글자 14행, `!`, 재시작 뒤에도 남는 함수 타입과 오류 글, 프로시저 매개변수·변수·인자 수, 0으로 나누기가 쓰는 곳에서는 오류·보이는 곳에서는 NULL·`IGNORE`·안 타는 가지, 타입 읽기와 옛 카탈로그의 `DECIMAL`, `ALTER` 뒤 인덱스로 찾은 행, `CALL`의 답과 프로시저 안 `INSERT` 뒤 `LAST_INSERT_ID()`, `BEFORE INSERT` 트리거의 `NEW`, 트리거가 낸 번호가 바깥 문장의 것을 바꾸지 않는 것)와 `test_lexer.cpp`의 `!` 한 케이스. 옛 시험 셋은 틀린 동작을 기대하고 있어 고쳤다: `UPDATE … SET d = d / w`(0 나누기가 NULL을 쓴다고 보던 것 둘)와 `UPDATE p SET a = a / 0`이 `NOT NULL` 오류라고 보던 것 — 이제 `Division by 0`이고 NULL이 필요한 시험은 `NULLIF`를 쓴다.
- 전체 시험 **758 케이스 / 1,503,331 assertions**을 Release 기본으로 통과; `RUSQL_DML_INDEX_MIN_ROWS=0`으로도 Release에서 통과(1,503,332)했고, 캐시 점검 모드(`RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0`)는 Debug에서 통과(1,503,331; Release에서는 마지막 시험 둘을 더하기 전 빌드로 1,503,320), Debug의 `RUSQL_DML_INDEX_MIN_ROWS=0`도 통과(1,503,331)했다.
- **심은 버그 70종**(어휘 `!` · DOUBLE/FLOAT 글자 · 64비트 끝값 · `ALTER … MODIFY`/`ADD`/`DROP` · 준비된 문장 · 트리거의 번호 · 저장 루틴의 타입·인자 수·복원 · 0으로 나누기의 세는 곳과 검사하는 곳 · `COALESCE` 게으른 계산 · `NEW`의 값 · `CALL`의 답): 처음 61종이 잡혔고, 살아남은 것에 시험을 더해(`ALTER`의 ENUM · `EXECUTE`의 변수 수 · 트리거의 번호 둘 · 프로시저의 `INSERT … SELECT`) 5종을 더 잡아 **66종이 잡혔다**. 남은 넷은 같은 동작이다 — 64비트 끝값 `18446744073709551615`에서 `>`를 `>=`로(`CAST`와 비트 연산 둘; 같은 최댓값을 돌려줌), `DROP COLUMN` 뒤 인덱스를 다시 만들지 않는 것(다음 `ADD COLUMN`이 다시 만들고, 그 전에는 지운 열이 보이지 않음), 준비된 문장의 `''`(따옴표 두 개)를 한 따옴표로 읽는 대신 문자열 둘로 읽는 것(`?`가 따옴표 안에 있는 것은 같음).
- 모델 검증기 열한 가지(시드 1)·빌드 간 차분 10시드 × 99질의(앞 빌드 = R41, 글자 그대로) 차이 0·`mysqldump` 왕복 둘(102문장과 85문장, 거절 0, 행이 모두 같음, mysql 클라이언트 오류 0)·`verify_writes.py` 시드 1~8·크래시 퍼저 셋(외래키 90 · 일반 60 · 동시성 40라운드) 위반·불일치 0.
- **속도** (5만 행, 앞 빌드(R41)와 번갈아 두 번씩): 스캔·집계·조인 15가지가 앞 빌드와 같다(두 번의 평균에서 `GROUP BY`만 +10%로 보이는데 첫 실행의 흔들림이다: 새 빌드 33.1·25.3 ms, 앞 빌드 27.1·25.9 ms); 단건 INSERT/DELETE 1만 건(6.8 s)·bulk 10만 건(0.7 s)·점 조회(인덱스 0.10 ms)·트랜잭션 처리량도 그대로.

**한계**: ① `FLOAT`는 MySQL의 여섯 자리 표시와 다르다(위). ② (쓰는 문장에서 글자를 수로 읽는 산술이 오류 1292여야 하는 것은 같은 날 R43에서 고쳤다.) ③ `SELECT`를 여러 번 하는 프로시저는 MySQL이 결과 집합을 모두 보내는데 여기는 마지막 표 하나다. ④ 이 라운드는 문법을 더하지 않았다(`PLAN.md`의 R42 항목): `SELECT @x := 7`, `INSERT INTO t () VALUES ()`, 산술 안의 `1 + !0`은 구문 오류/읽기 오류로 그대로. `UPDATE`·`DELETE`의 `WHERE` 안 0 나누기는 MySQL이 문장 전체를 오류로 하는데 여기는 그 행을 건너뛴다. 병렬 스캔 스레드의 0 나누기는 세지 못한다. `INSERT IGNORE`로 건너뛴 행은 AUTO_INCREMENT 번호를 쓰지 않는다(MySQL은 쓴다). `UPDATE t SET u = u + 1`(UNIQUE)은 MySQL이 행마다 검사해 중간 겹침을 오류로 하는데 여기는 최종 값을 검사한다.

### 10월 19일 — 쓰는 문장에서 글자를 수로 읽는 오류, 조용히 버려지던 `:=` (R43)

**왜 이 항목인가**: R42를 끝낸 뒤 사용자가 "결국 다 고쳐야하는거야?"라고 물었고, 남은 차이를 오류 없이 틀린 값이 나가는 것과 오류로 바로 드러나는 것·외관 차이로 나누어 보여 준 다음 "고칠만한것만 일단 고쳐봐". 오류도 없이 틀린 값이 나가던 둘만 골랐고 나머지는 `PLAN.md`의 R42 항목에 그대로 남겼다.

**고친 것**
- **글자를 수로 읽는 산술이 쓰는 문장에서도 조용히 통과했다**: `INSERT INTO d SELECT id, s + 1 FROM src`에서 `s`가 `'abc'`면 `1`, `'12abc'`면 `13`이 저장됐다. MySQL은 `SELECT`에서는 앞의 수를 쓰지만(경고만) 값을 쓰는 문장에서는 오류 1292 `Truncated incorrect DOUBLE value: 'abc'`이다: `INSERT … VALUES|SELECT`·`UPDATE … SET`(`REPLACE`·`ON DUPLICATE KEY UPDATE`·`CREATE TABLE … AS SELECT`도 같은 길)·저장 함수의 결과·프로시저 변수 대입(`SET x = …`, `SELECT … INTO x`). `INSERT IGNORE`와 `SELECT … INTO @x`는 값을 받는다. 온전한 수의 기준은 MySQL과 같게 맞췄다(진짜 MySQL로 13가지 글자를 견줌): `''`와 공백뿐인 글자는 0이고 오류가 아니며, 앞뒤 공백·부호·소수(`'.5'`, `'5.'`)·지수(`'1e3'`)는 수, `'abc'`·`'12abc'`·`'0x10'`·`'--5'`는 아니다. 세는 곳은 `number_tree`에서 글자 열(날짜·시간 열은 글자가 날짜·시간이면 제외)과 글자 상수를 수로 읽는 자리 하나, 묻는 곳은 R42의 0으로 나누기와 같은 `sqlval::StrictWrite`(R42에서 여섯 곳에 흩어져 있던 비교를 이 하나로 묶었다). 날짜·시간 열은 세지 않는다(날짜를 수로 읽는 것은 R41 한계 ①). `'2024-01-31' + INTERVAL 1 DAY`는 그대로 된다.
- **어휘 분석기가 `:`를 버려 `SELECT @a := 1`이 `@a = 1` 비교로 읽혔다**: 결과는 조용한 NULL이고, 행 번호를 매기는 관용구 `SELECT @i := @i + 1`도 NULL이었다. 이제 `:=`는 한 토큰(`TokenKind::ColonEq`)이라 대입을 받는 자리 — `SET @a := 5`, 프로시저의 `SET i := i + 1`, `UPDATE … SET a := 1`(여러 표 `UPDATE … JOIN`·`MERGE … UPDATE SET`도), `INSERT … SET`, `ON DUPLICATE KEY UPDATE` — 에서는 `=`처럼 받고(전에는 `:`가 버려져 우연히 되던 것을 그대로 지킨다), 식 안의 `:=`는 구문 오류다. 식 안의 대입(`SELECT @a := 1`)을 지원하는 것은 문법 추가라 하지 않았다. 한 개의 `:`(라벨 `lp: WHILE`)는 전처럼 토큰이 아니다.
- **비교 도구 `verify_null_expressions.py`**: R42부터 0으로 나누는 `UPDATE`가 오류인데 이 도구는 NULL을 쓴다고 보아 "ERROR: UPDATE s SET r = 2 / b"로 멈추고 있었다(R42 검증 기록에는 그 줄이 비어 있었는데 놓쳤다). 이제 0으로 나누는 행을 가진 `UPDATE`는 오류 `Division by 0`이고 아무것도 쓰지 않았는지를 확인한다(시드 1~3에서 `UPDATE` 62~79번, 거절 1~13번, 위반 0).

**검증**
- 코퍼스 `constraints.sql`에 45줄(글자-수 쓰기와 `:=`)과 `routines.sql`에 20줄(함수·프로시저·`INTO @x`·라벨)을 더해 진짜 MySQL 8.4.9와 견주어 새 차이 없음(`constraints.sql`의 옛 차이 일곱 개는 전부터 실행만 하는 문장이다); `--emit`이 두 `.inc`를 다시 만들었다.
- 신규 시험 `test_wrong_answers_r42.cpp`에 두 케이스(`:=`이 대입을 받는 자리와 식 안에서의 오류·라벨, 글자-수를 쓰는 곳에서 거절하는 것과 `SELECT`·`IGNORE`·날짜+`INTERVAL`)와 `test_lexer.cpp`의 `:=` 케이스. 
- 전체 시험 **761 케이스 / 1,503,482 assertions**을 Release 기본으로 통과; 같은 시험이 캐시 점검 모드(`RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0`, 1,503,483)와 `RUSQL_DML_INDEX_MIN_ROWS=0`(1,503,482)으로도 Release에서, 그 두 모드(1,503,483)로 Debug에서도 통과했다. (부하가 있는 첫 Release 실행에서는 `test_unique_claims.cpp:237`의 시간 검사 한 건이 2.9 s로 틀렸는데 그 시험만 따로 돌리면 통과하고 조용한 기계에서의 전체 실행도 통과했다 — 알려진 부하 흔들림.)
- **심은 버그 30종**(`:=` 토큰과 라벨 · 대입을 받는 일곱 자리 · 글자-수를 세는 자리와 기준(빈 글자, 글자 열, 글자 상수, 앞의 수 기억, 날짜와 시간을 빼는 것 셋) · `StrictWrite`의 두 오류 · 쓰는 곳 여섯 군데의 묻는 줄과 `IGNORE`·`@x` 예외): 28종이 잡혔고, 남은 둘은 같은 동작이다 — 프로시저 변수가 아닌 이름에도 오류를 묻는 `SET`/`SELECT … INTO`의 `proc_vars.count` 검사(`SET @x`와 `INTO @x`는 그 길로 오지 않는다). 처음 22종이 잡혔고, 살아남은 것에 시험을 더해(`MERGE … UPDATE SET :=`, `INSERT IGNORE … SELECT`, `SELECT … INTO @x`) 3종을 더 잡았으며, 날짜·시간 글자를 빼는 줄을 고친 뒤 만든 변형 셋도 잡혔다. R42의 0으로 나누기 변형 여덟은 그 줄이 `StrictWrite`로 바뀌어 위 열 가지(쓰는 곳 변형)로 다시 만들었다.
- 빌드 간 차분 10시드 × 99질의(앞 빌드 = R42) 차이 0·모델 검증기 열한 가지(시드 1; `verify_null_expressions.py`는 위 고침으로)·`verify_writes.py` 시드 1~8·`mysqldump` 왕복 둘(102문장과 85문장, 거절 0, 행이 모두 같음, mysql 클라이언트 오류 0)·크래시 퍼저 셋(외래키 90 · 일반 60 · 동시성 40라운드) 위반·불일치 0.
- **속도** (5만 행, 앞 빌드(R42)와 번갈아 두 번씩, 순서를 바꿔 한 번 더): 스캔·집계·조인 13가지가 앞 빌드와 같다(한 쌍에서 `COUNT(*)`가 +13%로 보였으나 순서를 바꾼 쌍에서는 같다: 새 18.2·18.0 대 옛 19.8·17.8 ms); 단건 INSERT/DELETE 1만 건(7.0 s)·bulk 10만 건(0.7 s)·점 조회(인덱스 0.12 ms)·트랜잭션 처리량도 그대로.

**한계**: `UPDATE`·`DELETE`의 `WHERE` 안 글자-수 비교(`UPDATE … WHERE s = 5`는 MySQL이 오류 1292)는 여전하고, 날짜 열을 산술에 쓰면 MySQL은 `20240305`처럼 수로 바꾸는데 여기는 앞의 수를 읽는다(R41 한계 ①). 식 안의 `:=`는 지원하지 않는다. `ENUM`·`SET` 열을 쓰는 문장에서 산술에 쓰면 MySQL은 번호로 읽는데(`e + 0` = 2) 여기는 글자라 이제 오류다(전에는 0으로 조용히 읽었다). 날짜·시간 글자(`2024-03-05`, `12:30:00`)는 세지 않는다 — 열의 타입 대신 글자를 `parse_datetime`·`parse_time_seconds`로 알아본다(열 정의가 산술 노드까지 닿지 않음).

### 10월 21일 — −2^63보다 작은 음수를 부호 없는 수로 바꿀 때 (R44 ①)

**왜 이 항목인가**: R43 뒤 "남은 게 뭐야"에 대한 답으로 "조용히 어긋나지만 영향이 작은 것" 일곱 가지를 꼽았고, 사용자가 "전부 고칩시다. 항목이 완료될 때마다 문서 갱신, 커밋&푸시"라고 해 하나씩 고친다. 첫 항목은 가장 작은 것.

**고친 것**: `CAST('-18446744073709551615' AS UNSIGNED)`·`-9223372036854775809 | 0`처럼 BIGINT의 가장 작은 값보다 작은 음수는 MySQL이 먼저 BIGINT로 바꾸므로 가장 작은 값(−2^63)으로 잘리고, 그 값을 부호 없이 읽어 `9223372036854775808`이다. 그 안쪽의 음수는 2^64에서 빼서 감싼다(`-1` → `18446744073709551615`, `-9223372036854775807` → `9223372036854775809`, `-0.4` → `0`). 전에는 어느 쪽이든 2^64에서 그대로 빼서 `1`·`0` 같은 엉뚱한 값이 나왔다. `CAST … AS UNSIGNED`와 비트 연산(`& | ^ ~ << >>`)이 같은 줄 `negative_as_unsigned`를 쓰고, 20자리를 넘는 글자(`'-999999999999999999999'`)도 같다.

**검증**: `corpus/functions.sql`에 두 문장(CAST 열 가지, 비트 연산 열두 가지)을 더해 진짜 MySQL 8.4.9와 차이 없음. 숫자를 double로 견주는 조건 시험은 `…807`과 `…808`을 구별하지 못해 글자까지 견주는 시험 `[r44]`를 따로 더했다. 심은 버그 8종 모두 잡힘(처음 6종, 나머지 둘은 이 정확한 시험으로). 전체 시험 **762 케이스 / 1,503,493 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과.

### 10월 21일 — AUTO_INCREMENT 번호를 문장이 잡았다가 쓰지 않는 방식, 같은 문장 안의 키 겹침 (R44 ②)

**왜 이 항목인가**: R44 둘째 항목("INSERT IGNORE로 건너뛴 행이 번호를 쓰지 않아 다음 번호가 다르다"). 진짜 MySQL에서 스물 가지가 넘는 경우를 돌려 보니 번호의 규칙이 한 줄이 아니었다.

**고친 것**
- **번호 예약**(InnoDB와 같게): `INSERT … VALUES`가 N행이면 첫 행이 표에 닿을 때 번호 N개를 예약하고 행이 차례로 하나씩 가져간다; `IGNORE`·`ON DUPLICATE KEY UPDATE`로 건너뛴 행은 번호를 가져가지 않고(돌려주고), 예약에서 남은 것은 문장이 끝나면 버려진다. `INSERT … SELECT`는 1개, 2개, 4개, 8개 … 묶음으로 예약한다(행 4개면 7개를 쓴다). 직접 쓴 번호는 예약을 끝낸다. 문장이 실패해도 첫 행이 열 검사를 통과해 표에 닿았다면 예약한 번호는 소진된다(중복 키·부모 없음: 다음 번호가 N 건너뜀); 첫 행이 열 검사(`NOT NULL`·길이·`CHECK`)에서 걸리면 아무것도 쓰지 않는다. 전에는 건너뛴 행이 번호를 먹어 `INSERT IGNORE … VALUES ('x',50),('y',1),('w',52)`에서 `w`가 5번(MySQL 4번)이었고, 실패한 문장은 번호를 모두 돌려놓았다(MySQL은 건너뛴다). `LAST_INSERT_ID()`는 건너뛰지 않은 첫 행의 번호.
- **`CHECK`가 외래키보다 먼저**(MySQL의 순서): 둘 다 어긋나는 행의 오류가 `CHECK`이고, 외래키만 어긋나는 행은 번호를 태운다.
- **같은 문장 안에서 앞 행과 키가 겹치는 행**: 전에는 "Duplicate value" 오류였다. `INSERT IGNORE`는 그 행을 건너뛰고, `ON DUPLICATE KEY UPDATE`는 앞 행(아직 표에 없는 행)에 대입을 적용한다(`VALUES(n)`는 이 행의 값; 열 타입에 맞추어 `DECIMAL(5,2)`의 `1 / 3`은 `0.33`). 복합 기본키·복합 UNIQUE는 `IGNORE`만.

**검증**
- 신규 코퍼스 `corpus/autoinc.sql`(219문장: 서른네 가지 경우 — 단일·다중 행, `IGNORE`, `ON DUPLICATE KEY UPDATE`, `REPLACE`, `NOT NULL`·길이·`CHECK`·외래키 오류, `INSERT … SELECT`의 묶음, 직접 쓴 번호, `ROLLBACK`, 같은 문장 안의 겹침, 뷰를 거친 `INSERT … SELECT`, `LAST_INSERT_ID()`)를 진짜 MySQL 8.4.9와 견주어 차이 0; `--emit`이 `mysql_conformance_autoinc.inc`를 만들고 `test_mysql_conformance.cpp`가 새 사례 묶음으로 돈다.
- 심은 버그 23종 가운데 22종이 잡혔다(처음 19종 + 열 `CHECK` 둘과 `ON DUPLICATE KEY UPDATE`의 열 타입 맞춤을 코퍼스에 더해 3종); 남은 하나는 같은 동작이다(번호를 돌려주기 전에 "마지막으로 가져간 번호"인지 보는 검사: 표에 `AUTO_INCREMENT` 열이 하나뿐이라 늘 참).
- 전체 시험 **763 케이스 / 1,503,970 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과; `verify_writes.py` 시드 1~8 위반 없음, `mysqldump` 왕복 둘(102문장과 85문장, 거절 0, 행이 모두 같음).

**한계**: 다시 시작한 뒤에는 소진된 번호를 저장하지 않는다(MySQL 8은 저장). 복합 키가 같은 문장 안에서 겹치는 `ON DUPLICATE KEY UPDATE`는 아직 오류. `INSERT … SELECT`가 가져오는 행의 순서가 MySQL과 다르면(조인 순서) 어느 행이 번호를 태우는지가 다를 수 있다.

### 10월 21일 — 바뀐 행 수: UPDATE·ON DUPLICATE KEY UPDATE·REPLACE의 `ROW_COUNT()`와 OK 패킷 (R44 ③)

**왜 이 항목인가**: R44 셋째 항목. `UPDATE`가 "찾은 행"을 세어 같은 값으로 다시 쓴 행도 셌고(MySQL은 바뀐 행만), `ON DUPLICATE KEY UPDATE`·`REPLACE`는 삽입한 행 수만 말했다. `ROW_COUNT()`와 MySQL 프로토콜의 OK 패킷(`cursor.rowcount`, JDBC `executeUpdate`)이 그 수를 그대로 쓴다.

**고친 것**
- **`UPDATE`**: 값이 실제로 바뀐 행만 센다(`UPDATE t SET n = 5`를 두 번 하면 3, 0). 클라이언트가 `CLIENT_FOUND_ROWS`(0x2: Connector/J·MySQLdb 기본)를 켜고 접속하면 찾은 행(3, 3)을 센다 — 접속 인사의 능력에 이 비트를 더해 알리고 `Executor::client_found_rows`로 받는다.
- **`ON DUPLICATE KEY UPDATE`**: 삽입한 행 1, 바뀐 행 2, 같은 값으로 둔 행 0(`CLIENT_FOUND_ROWS`면 1); 같은 문장 안에서 앞 행과 겹쳐 그 행을 고친 경우도 같다. **`REPLACE`**: 대체한 행은 지우고 넣으니 2, 새 행은 1.
- **트리거**: 문장이 일으킨 `AFTER` 트리거의 문장(`UPDATE lg SET k = k + 1`)이 바깥 문장의 수를 덮어쓰던 것을 고침 — `DELETE`·`UPDATE`의 `AFTER` 트리거 뒤에 바깥 문장의 수를 되돌려 놓는다(`INSERT`는 이미 그랬다).
- 그 수를 만드는 곳이 하나(`Executor::statement_affected_rows`)라 `ROW_COUNT()`·`SELECT ROW_COUNT()`와 MySQL 프로토콜이 같은 값을 말한다.

**검증**
- `vs_mysql.py --proto`가 OK 패킷의 바뀐 행 수를 견주도록 했다(전에는 오류 여부만); `corpus/protocol.sql`·`corpus/constraints.sql`에 UPDATE·ODKU·REPLACE·IGNORE·트리거 경우를 더해 진짜 MySQL 8.4.9와 바뀐 행 수 차이 0. `CLIENT_FOUND_ROWS` 쪽은 ODKU 여섯 문장을 진짜 MySQL 두 모드(기본·`FOUND_ROWS`)에서 돌려 얻은 값을 `[r44][affected]` 시험에 넣었다.
- 심은 버그 21종 모두 잡힘(바뀐 행 대신 찾은 행 세기, ODKU 1/2/0 값, `REPLACE`의 2, 트리거가 덮어쓰는 수 ...). 시험 몇 곳이 "찾은 행" 수를 기대하고 있어(`UPDATE … SET v = v`가 1) MySQL 규칙으로 고쳤다.
- 전체 시험 **764 케이스 / 1,504,095 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과.

**한계**: 이미 같은 값인 행을 `REPLACE`하면 MySQL은 1(제자리 갱신이 아무것도 바꾸지 않음; 키가 하나뿐인 표만, 외래키가 가리키지 않고 삭제 트리거가 없을 때), 여기는 2(지우고 넣음). 멀티 표 `UPDATE`(`UPDATE a JOIN b …`, `UPDATE a, b SET …`)의 두 모드 수는 진짜 MySQL과 같음을 확인했다.

### 10월 21일 — UNIQUE·PRIMARY KEY 값을 밀어 옮기는 UPDATE는 행마다 검사 (R44 ④)

**왜 이 항목인가**: R44 넷째 항목. 진짜 MySQL(InnoDB)은 `UPDATE`가 행을 하나씩 쓰며 그때그때 키가 겹치는지 보기 때문에 `UPDATE t SET u = u + 1`(u = 1, 2, 3)은 첫 행이 아직 2를 가진 둘째 행과 겹쳐 1062 오류인데, 여기는 문장이 끝난 뒤의 값 전체만 검사해 통과시켰다(더 너그러움). 반대로 `SET u = u - 1`이나 `… ORDER BY u DESC`는 MySQL도 통과한다. 진짜 MySQL에서 마흔 가지 남짓 돌려 규칙을 확인했다.

**고친 것**
- **쓰는 순서**: `ORDER BY`가 준 순서; 없으면 **기본키 순서**(InnoDB가 표를 읽는 순서 — 행을 3, 1, 2로 넣었어도, 앞서 한 `UPDATE`가 그 행을 표의 끝으로 보냈어도 같다). `WHERE`가 **인덱스가 있는 열**을 값과 비교하면(`WHERE u >= 10`) 그 인덱스 순서일 수도 있다. 어느 쪽을 MySQL의 계획이 고르는지는 알 수 없어서, 순서를 모두 따져 **어느 순서에서나 겹칠 때만** 오류로 한다(통과하는 순서가 하나라도 있으면 통과: 잘못 거절하지 않는다).
- 단일 열 `PRIMARY KEY`·`UNIQUE`, 복합 `PRIMARY KEY (a, b)`·`UNIQUE (a, b)`, 기본키 없는 표, 뷰를 거친 `UPDATE`, 멀티 표 `UPDATE a JOIN b …`·`UPDATE a, b SET …`에 같은 규칙. 오류의 값은 처음 겹친 행의 새 값이다(`n = n + 300`에서 `'503'`; 전에는 문장 끝 기준 `'902'`).
- 새 값이 겹치는 행이 없으면(`SET u = <새 값>`) 순서를 따지지 않아 비용이 늘지 않는다 — 어떤 행의 새 값이 다른 대상 행의 옛 값일 때만 순서를 만든다.
- 이 규칙에 기대고 있던 기존 시험 일곱 곳(`UPDATE t SET u = u + 100`이 통과한다고 가정하던 것)을 MySQL 규칙으로 고쳤다.

**검증**
- 신규 코퍼스 `corpus/shift.sql`(106문장: 올림·내림·교환·`ORDER BY id DESC`/`u ASC`/`u DESC`·`LIMIT`·기본키 이동·복합 키·기본키 없는 표·뒤바뀐 삽입 순서·앞선 UPDATE 뒤·인덱스 열 `WHERE`·글자 키와 대소문자·뷰·`JOIN`) 진짜 MySQL 8.4.9와 차이 0(둘 다 실패 28); 이전 빌드는 같은 코퍼스에서 32문장이 달랐다. `--emit`이 `mysql_conformance_shift.inc`를 만들고 `test_mysql_conformance.cpp`가 새 사례 묶음으로 돈다.
- `verify_writes.py`·`test_write_integrity`·`test_update_unique`의 무작위 모형도 "행을 차례로 쓴다"로 바꿔(전에는 끝 값만) 시드 1~4에서 위반 없음.
- 심은 버그 11종(자기 옛 키 비우기 빼기, 순서 무시, 인덱스 순서 빼기, `ORDER BY` 무시, 저장 순서 사용, 복합 키 건너뛰기, …) 모두 잡힘.
- 전체 시험 **765 케이스 / 1,504,450 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과.

**한계**: `WHERE`가 인덱스 열의 범위를 고르고 그 인덱스 순서가 기본키 순서와 반대일 때(값이 키와 거꾸로인 표의 `SET u = u + 10 WHERE u >= 10`), MySQL은 인덱스 순서로 읽어 오류인데 여기는 기본키 순서에서 통과하므로 통과시킨다(너그러운 쪽). 뷰를 거친 `UPDATE … ORDER BY`는 순서와 상관없이 "Table not found" 오류(이전부터).

### 10월 21일 — UPDATE·DELETE의 WHERE 안 0 나누기와 글자-수 비교는 오류 (R44 ⑤)

**왜 이 항목인가**: R44 다섯째 항목. 진짜 MySQL은 `UPDATE`·`DELETE`가 쓰는 값뿐 아니라 `WHERE`도 같은 엄격함으로 읽는다 — 읽은 행에서 `n / b`의 `b`가 0이면 문장이 통째로 오류 1365 `Division by 0`이고, 숫자 열을 글자와(`n = 'abc'`) 또는 글자 열을 숫자와(`s = 5`, 그 열에 `'abc'`가 있으면) 견주면 오류 1292 `Truncated incorrect DOUBLE value: 'abc'`다(`SELECT`는 NULL·앞의 수로 읽고 경고뿐). 여기는 `WHERE`를 `SELECT`처럼 읽어 그런 행을 조용히 건너뛰었다.

**고친 것**
- `UPDATE`와 `DELETE`가 `WHERE`를 읽는 동안(`sqlval::StrictComparison` + `StrictWrite`) 0 나누기(`/`, `DIV`, `MOD`)와 숫자와 견주어 읽은 글자(`=`, `<>`, `<`, `>`, `BETWEEN`, `IN`)에 수가 아닌 글자가 있으면 문장 전체가 실패하고 아무것도 쓰지도 지우지도 않는다. 하위 질의 안(`n > (SELECT 1 / 0)`)과 함수의 글자(`UPPER(s) = 5`)도 같다. 날짜·시간·ENUM·SET 열은 글자가 아니라 수로 견주므로 제외한다(바인더가 열에 `ColKind`를 단다).
- **인덱스가 있는 열을 `WHERE`가 값과 비교하면 이 오류를 내지 않는다.** MySQL은 그때 인덱스로 일부 행만 읽을 수 있고 어느 행을 읽는지는 그쪽 계획이라(`n / (id - 2) > 1 AND id <> 2`는 `id = 2`를 읽지 않아 통과한다), 잘못 거절하느니 놓치는 쪽을 택했다(R44 ④와 같은 원칙). 인덱스 없는 열만 보는 `WHERE`는 모든 행을 읽으니 오류가 확실하다.
- `DELETE`의 빠른 길은 지우면서 조건을 읽었는데, 오류를 내려면 아무것도 지우기 전에 모든 행을 읽어야 해서 읽기와 지우기를 둘로 나눴다(`remove_if`와 같은 순서, 같은 복사 없는 이동; 성능은 같다).
- 거절된 문장은 `ROLLBACK`할 것도 없다 — 트랜잭션 안과 외래키가 가리키는 표(느린 길)에서도 같다.

**검증**
- 신규 코퍼스 `corpus/where_errors.sql`(98문장: 0 나누기 열두 가지·`DELETE` 넷, 글자-수 비교 스물네 가지와 `DELETE` 셋, 인덱스 열 `WHERE` 여덟 가지, 가드 순서) 진짜 MySQL 8.4.9와 차이 0(둘 다 실패 27); 이전 빌드는 같은 코퍼스에서 35문장이 달랐다. `--emit`이 `mysql_conformance_where_errors.inc`를 만들고 `test_mysql_conformance.cpp`가 새 사례 묶음으로 돈다. 트랜잭션·외래키 표·날짜/ENUM/SET은 `[r44][where_errors]` 시험.
- 심은 버그 11종 가운데 10종이 잡혔다(`UPDATE`·`DELETE`의 엄격함 끄기·확인 빼기, 인덱스 예외 빼기, 비교의 수 아님 세기 끄기, 열 종류 예외 빼기 ...); 남은 하나는 오른쪽이 맨 열인 `ConditionValue::Arith` 가지의 열 종류 검사로, 지금 파서는 그 모양을 만들지 않아 같은 동작이다.
- 전체 시험 **767 케이스 / 1,504,789 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과.

**한계**: 인덱스 열을 비교하는 `WHERE`는 MySQL이 오류를 내도(`id = 3 AND s = 7`에서 `'7x'`) 여기는 통과시킨다(너그러운 쪽). `CASE`·`IF` 안의 글자(`CASE WHEN … THEN s ELSE 1 END = 5`)와 `CAST(s AS SIGNED)`의 글자는 아직 세지 않는다. 날짜·ENUM·SET 열을 수와 견주는 것(`e = 2`, `d > 20240101`)은 오류는 없지만 결과 행이 MySQL과 다르다(R44 ⑥).

### 10월 21일 — 날짜·시간·ENUM·SET 열을 수로 읽을 때 (R44 ⑥)

**왜 이 항목인가**: R44 여섯째 항목(R41 한계 ①). 값이 글자로 저장되는 이 엔진에서 날짜·시간·ENUM·SET 열을 수가 필요한 자리에 쓰면 글자의 앞부분을 수로 읽어 `d + 0`이 `2024`, `e + 1`이 `1`이었다. MySQL은 `DATE '2024-01-02'`를 `20240102`, `DATETIME`을 `20240102030405`, `TIME '10:20:30'`을 `102030`, ENUM을 목록에서의 자리(`'green'` → 2), SET을 비트(`'a,c'` → 5)로 읽는다. 진짜 MySQL에서 예순 가지 남짓을 돌려 규칙을 확인했다(ENUM·SET은 DOUBLE, 날짜·시간은 정수, 소수초 열은 그 자릿수의 DECIMAL).

**고친 것**
- 바인더가 열마다 **종류**(`ColKind`: 날짜, 날짜시간, 시간, ENUM, SET)와 ENUM·SET의 값 목록, 소수초 자릿수를 단다. 수로 읽는 자리는 이것으로 열의 "수 보기"(`column_number`)를 쓴다: `DATE` 20240102, `DATETIME(3)` 20240102030405.678, `TIME` 부호와 함께 102030(838:59:59는 8385959), ENUM의 자리(없으면 0), SET의 비트.
- 쓰이는 곳: 산술(`+ - * /`, 단항 `-`, `d + n`, `d + d`), 숫자 함수(`ABS FLOOR CEIL ROUND TRUNCATE SIGN SQRT POW MOD EXP LN LOG*` 삼각함수 `DEGREES RADIANS BIT_COUNT`), 비트 연산자와 `DIV`·`%`, `CAST(… AS SIGNED / UNSIGNED / DECIMAL / DOUBLE / FLOAT)`, 그리고 **수와의 비교**(`=`, `<>`, `<`, `>`, `BETWEEN`, `IN`: `d > 20240101`, `e = 2`, `e IN (1, 3)`, `st & 4`) — 인덱스가 있는 열도 같다. 글자와의 비교는 글자대로(`e = 'green'`, `d = '2024-01-02'`, `st = 'a,c'`), `YEAR(d)`·`DATEDIFF(d, …)`·`CONCAT(d, …)`처럼 날짜로 읽는 함수는 그대로다.
- R44 ⑤의 `UPDATE … WHERE e = 2`·`d > 20240101`은 오류는 없었지만 행이 틀렸는데 이제 같다.

**검증**
- 신규 코퍼스 `corpus/arith_types.sql`(137문장: 산술 서른여 가지, 비교·BETWEEN·IN, 숫자 함수, 비트 연산, CAST 열 가지, `ORDER BY`·`GROUP BY` 식, `UPDATE … SET n = d + 0`, `INSERT … SELECT`, 인덱스 있는 열, JOIN 조건) 진짜 MySQL 8.4.9와 차이 0 — 이전 빌드는 앞 74문장 중 57문장이 달랐다. `--emit`이 `mysql_conformance_arith_types.inc`를 만들고 `test_mysql_conformance.cpp`가 새 사례 묶음으로 돈다; `corpus/where_errors.sql`에 날짜·ENUM·SET 다섯 문장이 돌아왔다.
- 다른 코퍼스(함수, 표, 제약, 정렬 규칙, 날짜·시간, 루틴, 스키마 변경, 자동 번호)는 전과 같은 차이 수.
- 심은 버그 15종 모두 잡힘.
- 전체 시험 **769 케이스 / 1,505,102 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과.

**한계**: 집계 `SUM`·`AVG`는 열 그대로를 받으면 앞부분의 수로 읽는다(`SUM(d)`는 2024, MySQL 20240102; `SUM(d + 0)`은 맞다). `CAST(DATETIME(3) AS SIGNED)`는 소수초를 수로 반올림해(…59.999 → …60) MySQL처럼 달력으로 올리지 않는다. 열과 열을 견줄 때(`WHERE f = e`) 오른쪽이 날짜·ENUM·SET 열이면 아직 글자로 읽는다. `GREATEST(d, 20240101)`처럼 날짜와 수를 섞는 함수는 그대로.

### 10월 21일 — FLOAT 열은 유효숫자 여섯 자리로 보인다 (R44 ⑦)

**왜 이 항목인가**: R44 일곱째이자 마지막 항목. `FLOAT`은 단정밀도(float32)라 MySQL은 값을 유효숫자 여섯 자리로 보여 준다: `1.23456789`를 넣으면 `1.23457`, `123456789`는 `123457000`, `16777217`은 `16777200`. 여기는 단정밀도로 줄인 값을 읽어 돌아가는 가장 짧은 글자(`1.2345679`)를 그대로 보여 일곱 자리 넘는 값이 달랐다(여섯 자리 안쪽의 값 — 가격 `19.99`, `0.1`, `1.5` — 은 원래 같았다).

**고친 것**
- `SELECT`가 `FLOAT` 열을 내놓을 때(`SELECT f`, `SELECT *`, `AS` 이름, 조인, `ORDER BY`·`DISTINCT`·`GROUP BY` 결과) 유효숫자 여섯 자리로 보인다(`sqlval::format_float_display`): 저장된 값은 그대로 두고 보이는 글자만 바꾸므로 비교·정렬·산술은 그대로이다.
- `CAST(x AS FLOAT)`가 단정밀도로 줄인 뒤 같은 여섯 자리로 보인다(`CAST(16777217 AS FLOAT)` = 16777200, `CAST(0.1 AS FLOAT)` = 0.1). 전에는 글자를 그대로 돌려줬다.

**검증**
- 신규 코퍼스 `corpus/float.sql`(40문장: 스물한 가지 값을 `FLOAT`·`DOUBLE`·`REAL` 열에 넣어 `SELECT`·정렬·`DISTINCT`·`GROUP BY`·별칭·`UPDATE … f * 1.1`·`CAST`) 진짜 MySQL 8.4.9와 차이 0 — 이전 빌드는 같은 코퍼스에서 일곱 문장이 달랐다. 글자까지 정확해야 하는 값은 `[r44][float]` 시험에서 MySQL이 낸 글자와 견준다.
- 심은 버그 4종 모두 잡힘.
- 전체 시험 **771 케이스 / 1,505,171 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과.

**한계**(고치지 않음): 산술이 단정밀도 값의 정확한 크기를 읽는 것(`f + 0`은 MySQL에서 `1.2345678806304932`, 여기는 `1.2345679`), `f = 0.1`이 MySQL에서 거짓인 것(단정밀도 0.1은 배정밀도 0.1과 다름 — MySQL의 잘 알려진 함정이라 따라 하지 않음), `CAST(f AS CHAR)`·`CONCAT(f, '')`가 여섯 자리를 쓰지 않는 것, `MAX(f)`·`SUM(f)`의 보이는 모양, `FLOAT(7,3)`의 자릿수 지정(받기만 하고 무시).

### 10월 21일 — R45: ANY_VALUE, 뷰 경유 UPDATE/DELETE의 ORDER BY, EXPLAIN ANALYZE 단계, 실행 상태 표, 앱 기능 셋

**왜 이 항목인가**: R44를 끝내고 낸 추천 목록(`docs/mds/RECOMMAND.md`)에서 사용자가 여섯 개를 골랐다 — `ANY_VALUE`, `UPDATE 뷰 … ORDER BY`, `EXPLAIN ANALYZE` 확장, 스키마를 아는 자동완성, 세션·트랜잭션·락 패널, 앱 안의 MySQL 호환 점수판. 항목마다 시험하고 끝에 문서를 한꺼번에 갱신하는 방식으로 진행했다.

**엔진**
- **`ANY_VALUE(x)`**: MySQL처럼 **함수**(집계가 아님)다. `ONLY_FULL_GROUP_BY` 검사가 그 안쪽 열을 면제하고(`SELECT grp, ANY_VALUE(name) FROM av GROUP BY grp`), 그룹은 첫 행의 값으로 보인다. 산술 안에서도(`ANY_VALUE(val) + 1`), `GROUP BY` 없는 질의에서는 행마다(집계가 아니라 한 행으로 모이지 않는다), 집계만 있는 질의(`SELECT COUNT(*), ANY_VALUE(grp)`)에서는 그 한 그룹의 첫 행으로 나온다. 처음에 집계로 만들었다가 MySQL과 비교해 함수가 맞다는 것을 알고 고쳤다.
- **뷰를 거친 `UPDATE`·`DELETE … ORDER BY`/`LIMIT`**: 전에는 "Table not found" 오류였다(이전부터 있던 버그). 이제 문장 처리기가 뷰를 먼저 풀어 기본 표와 뷰의 조건으로 행을 고른다(R44 ④의 `ORDER BY` 순서 규칙도 그대로 쓴다).
- **`EXPLAIN ANALYZE`의 단계**: 계획과 추정/실제 행 수 아래에 문장이 지난 단계를 보여 준다 — `Step 1 Scan d.o + JOIN d.c : 4 rows, 0.105 ms, 72%`처럼 단계별 남은 행 수, 시간, 전체 대비 비율(스캔·조인·필터, 정렬, 그룹/집계, HAVING, 그룹 정렬, 나머지(LIMIT·DISTINCT·결과 쓰기)). 가장 바깥 `SELECT`만 세고 하위 질의는 그것을 실행한 단계에 포함되며, 인덱스·캐시 지름길과 파생 표는 `Execute` 한 단계다(`engine/explain_trace.hpp`: 실행 중인 문장에만 `thread_local` 기록기를 두므로 `EXPLAIN ANALYZE`가 아닌 문장의 비용은 없다).
- **`information_schema`의 실행 상태 표 다섯 개**: `processlist`(세션과 실행 중인 문장), `innodb_trx`(열린 트랜잭션: 상태 `RUNNING`/`LOCK WAIT`, 잠근 행 수, 기다리는 상대), `innodb_locks`(행 락), `innodb_lock_waits`(누가 누구를 기다리는지), `innodb_deadlocks`(교착으로 되돌려진 요청). 이름과 열은 MySQL 8의 것을 따랐고 데이터는 락 관리자·세션 목록·열린 트랜잭션 집합에서 읽는다.

**앱**
- **스키마를 아는 자동완성**(`lib/sqlCompletion.ts`): `FROM`·`JOIN`·`INTO`·`UPDATE` 뒤에는 현재 데이터베이스의 표, `별칭.`/`표.` 뒤에는 그 표의 열(PK·타입·FK 표시), 그 밖에는 문장이 쓰는 표의 열 + 표 + 함수 + 키워드. 표·열은 엔진에서 읽어 8초 보관하고 문장을 실행할 때마다 비운다. 에디터 두 개에 `quickSuggestions`를 켰다(전에는 일부러 꺼 둠).
- **세션·트랜잭션·락 패널**(`components/ActivityPanel.tsx` + Tauri 명령 `get_activity`): Server Manager의 Sessions 패널 아래에 열린 트랜잭션(잠근 행, `LOCK WAIT`은 빨간 테두리와 "waits for txn N"), 기다리는 쌍, 세션별 실행 중인 문장, 최근 교착을 1.5초마다 새로 고쳐 보여 준다.
- **Compat 페이지**(`components/CompatView.tsx`, 활동 표시줄의 저울 아이콘): 코퍼스마다 RuSQL과 진짜 MySQL에서 돌려 같은 답을 낸 문장 수를 막대로 보여 준다(총 비교 문장, 일치율, 둘 다 같은 오류로 거절한 수, C++ 시험의 MySQL 대조 사례, 시험 규모, 찾아 고친 버그, 알려진 차이). 숫자는 `code/test/diff/make_scoreboard.py`가 코퍼스 17종을 실제로 돌려 `lib/compatSnapshot.json`에 쓴다(EN/KO 토글은 Diagram 페이지와 같은 키).

**검증**
- 신규 코퍼스 `corpus/any_value.sql`(15문장)·`corpus/view_order.sql`(18문장) 진짜 MySQL 8.4.9와 차이 0; `--emit`이 `mysql_conformance_any_value.inc`·`…_view_order.inc`를 만든다. `[r45]` 시험: `EXPLAIN ANALYZE` 단계(조인·그룹·정렬·집계·지름길·하위 질의), `information_schema` 상태 표(한 세션), `ANY_VALUE`와 뷰 `ORDER BY`.
- 두 세션을 실제로 돌려 확인: 한 트랜잭션이 잡은 행을 다른 세션이 기다릴 때 `innodb_trx`가 `LOCK WAIT`/`waiting for 10`, `innodb_lock_waits`가 `11 → 10`을 보였고, 교착을 만들면 `innodb_deadlocks`에 `13 → 12`가 남았다.
- 프론트: `tsc`, `vite build`, `cargo check` 통과. 자동완성의 제안 공급자는 모형 Monaco로 아홉 가지 위치(`FROM`, `JOIN`, `별칭.`, 쉼표 목록, `WHERE`, 문장 둘, `UPDATE`, 파생 표 …)를 돌려 확인했고, Compat 페이지와 활동 패널은 헤드리스 Chrome으로 그려 화면을 확인했다. **실제 Tauri 앱에서 눌러 본 것은 아니다.**
- 심은 버그 17종(`ANY_VALUE`의 면제·값·집계 행, 뷰 `ORDER BY`의 표·조건, `EXPLAIN ANALYZE`의 단계 표시·깊이·범위, 상태 표의 열)을 모두 잡았다.
- 전체 시험 **776 케이스 / 1,505,297 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과.

**한계**: `ANY_VALUE`를 `HAVING`에서 쓰면 MySQL은 "Unknown column" 오류인데 여기는 받는다. `EXPLAIN ANALYZE`는 가장 바깥 `SELECT`의 단계만 나누고 조인 안의 알고리즘별 시간·하위 질의별 시간은 없다. 락 대기·교착 표는 두 세션이 필요해 C++ 시험이 아니라 손으로 확인했다. 자동완성은 파생 표의 별칭·CTE 열은 모른다.

### 10월 21일 — R46: 윈도 단축형·이름 있는 윈도, MAX_EXECUTION_TIME, 외래키 이름과 ERD 관계 편집기, MCP run_script, SQL 한 문장의 여행(TRACE)

**왜 이 항목인가**: R45 뒤 추천 목록(`docs/mds/RECOMMAND.md`)에서 다섯 개를 골랐다 — 윈도 단축형, `MAX_EXECUTION_TIME`, ERD에서 FK 편집 + DDL 미리보기, MCP `run_script`, SQL 한 문장의 여행. 항목마다 시험하고 끝에 문서를 한꺼번에 갱신하는 방식으로 진행했다.

**엔진**
- **윈도 프레임 단축형·이름 있는 윈도**: `ROWS UNBOUNDED PRECEDING`/`ROWS 2 PRECEDING`/`ROWS CURRENT ROW`(RANGE도)는 `BETWEEN … AND CURRENT ROW`와 같다. 뒤로 시작하는 단축형(`ROWS 2 FOLLOWING`)은 MySQL처럼 오류. `WINDOW w AS (…)`, `OVER w`, `OVER (w ORDER BY …)`, `OVER (w ROWS …)`는 **파서가 토큰 단계에서** 풀어 쓴다 — `WINDOW` 절을 토큰에서 잘라 이름별로 보관하고 `OVER` 뒤의 이름을 그 토큰으로 바꾸며, 바꿔 넣은 토큰은 이름이 있던 자리의 위치를 물려받아 이름 없는 결과 열이 쓴 글자(`SUM(v) OVER w`) 그대로 나온다. AST·JSON·실행기는 건드리지 않았다. 이름은 대소문자를 구분하지 않고 없는 이름은 오류.
- **`MAX_EXECUTION_TIME`** (`statement_deadline.hpp`, 새 파일): `SELECT /*+ MAX_EXECUTION_TIME(ms) */ …` 힌트나 `SET [SESSION] max_execution_time = ms`. 넘으면 오류 3024 `Query execution was interrupted, maximum statement execution time exceeded`(SELECT만, 세션은 그대로). 문장을 **밖에서 끊지 않고** 실행하는 스레드의 시계를 루프가 확인한다 — WHERE 평가, 조인 반복, 결과 쓰기에서 1024번에 한 번, `exec_select`의 단계 경계마다. 병렬 스캔의 작업 스레드는 확인하지 않는다(시작한 스레드가 다음 단계에서 멈춘다). `SLEEP`은 기다리지 않아 시험에 못 쓴다.
- **`TRACE <문장>`** (`explain_trace.hpp` 확장, `run_trace`): 문장을 **실제로 실행**하고 지나온 길을 표로 준다 — Parse → Prepare(캐시·락) → Table locks → (SELECT의 스캔·정렬·그룹 단계) → Execute → Commit(redo 로그 fsync) → Return the answer, 단계마다 남은 행·시간·비율과 마지막 `Total` 줄(문장의 답, 또는 오류). MySQL에 없는 RuSQL 문법. 한 문장의 여행 재생기가 이것을 쓴다.
- **외래키 이름과 두 버그** (ERD 편집기를 만들다 찾았다): ① `ALTER TABLE c ADD FOREIGN KEY (pid) REFERENCES p (id)`처럼 부모를 데이터베이스 없이 쓰면 부모를 `p`로만 저장해, **그 뒤 `c`의 모든 INSERT가 "Referenced table 'p' not found"로 실패**했다(CREATE TABLE은 `db.p`로 저장). ② 제약 이름이 저장되지 않아 `ALTER TABLE … DROP FOREIGN KEY <이름>`이 듣지 않았고(열 이름·`fk_<열>`만 먹었다) SHOW CREATE TABLE에도 이름이 없었으며 `information_schema`의 이름이 두 표에서 서로 달랐다. 이제 `ForeignKey.name`을 저장(스키마 JSON `name`, 예전 파일은 이름 없음)하고 `fk_constraint_name()`(`fk_name.hpp`) 한 곳에서 보고 이름을 만든다 — 준 이름, 없으면 MySQL처럼 `<표>_ibfk_<n>`. DROP은 그 이름(또는 옛 철자)으로 지우고, 한 ALTER의 `DROP FOREIGN KEY a, ADD CONSTRAINT b …`는 전부 되거나 전혀 안 되며, 같은 이름을 다시 쓰면 오류. SHOW CREATE TABLE·BACKUP이 이름을 쓰고 `information_schema.referential_constraints`(UPDATE_RULE·DELETE_RULE·REFERENCED_TABLE_NAME …)가 새로 생겼다.

**앱**
- **ERD 관계 편집기**(`components/FkEditorModal.tsx`, `ErdView.tsx`): 관계선을 누르거나 `＋ Relationship`을 누르면 자식·부모 표와 열, 이름, ON DELETE/UPDATE를 고르는 창이 열리고, 아래에 `ALTER TABLE …` DDL이 **그대로 미리 보인다**(고를 때마다 갱신, `Apply`를 누르기 전에는 아무것도 실행하지 않는다). 수정은 DROP+ADD가 한 ALTER, 삭제는 `DROP FOREIGN KEY`. 타입 불일치, 부모가 PK·UNIQUE가 아님, NOT NULL 열의 SET NULL, 이미 FK가 있는 열은 경고. 현재 이름·규칙은 `information_schema.referential_constraints`에서 읽는다.
- **한 문장의 여행**(`components/JourneyModal.tsx`, 에디터 도구 막대의 점선 경로 버튼): 선택한(없으면 커서가 있는) 문장을 `TRACE`로 한 번 돌린 뒤 기록된 단계를 사람이 따라갈 속도로 다시 보여 준다 — 단계 상자가 차례로 켜지고, 아래에 설명 한 줄과 단계별 시간 막대. 재생·일시정지·한 단계씩·0.5~4x. **실시간 점등이 아니라 기록 재생인 이유**: 문장이 밀리초에 끝나서 실시간은 눈에 안 보인다. 막대와 시간은 실제 값. SELECT가 아니면 실제로 실행된다고 먼저 알리고 눌러야 돈다.

**MCP**
- **`run_script(statements, database, commit, confirmed_dangerous)`**: 한 연결·한 트랜잭션에서 문장 여러 개를 실행하고 문장마다 결과(SELECT 행 20개까지, "N row(s) updated", 오류)를 JSON으로 준다. `commit=False`(기본)는 **미리보기**(끝에서 ROLLBACK), `commit=True`는 전부 되거나 전혀 안 된다(실패한 문장에서 멈추고 되돌림). DDL·BEGIN/COMMIT 같은 트랜잭션을 끝내는 문장은 미리보기로 거절하고, DROP/TRUNCATE·WHERE 없는 UPDATE/DELETE는 `confirmed_dangerous=True` 없이는 거절한다. 도구는 26개가 되었다.

**검증**
- 신규 코퍼스 `windows.sql`(26문장)·`max_execution_time.sql`(17)·`fk_names.sql`(22)가 진짜 MySQL 8.4.9와 차이 0; `--emit`이 `mysql_conformance_{windows,max_execution_time,fk_names}.inc`를 만든다. `max_execution_time`에서 `COUNT(*)`만 있는 교차 조인은 뺐다 — MySQL은 조인 버퍼로 한도 안에 세어 버리지만 RuSQL은 멈춘다(한도가 목적).
- `[r46]` 시험 두 개(TRACE와 한도, 외래키 이름), 옛 시험 `ALTER TABLE foreign key / unique / check constraints`는 "이름은 저장하지 않는 옛 이식 버그"를 못 박고 있어서 새 동작으로 고쳤다(부모는 `company.department`로 저장, 이름은 `fk_proj_dept`).
- MCP `run_script`는 살아 있는 서버로 17가지(미리보기가 저장하지 않음, 실패 시 전부 되돌림, DDL·위험 문장 거절 …)를 확인했다.
- 프론트: `tsc` 통과, 두 모달은 `invoke`를 모형으로 바꾼 헤드리스 Chrome 화면으로 확인(여정 재생, 관계 편집·추가 창). **실제 Tauri 앱에서 눌러 본 것은 아니다.**
- 심은 버그 12종(윈도 프레임 끝, `OVER w` 괄호, 윈도 이름 대소문자, 한도 판정, TRACE 단계, FK 이름 저장, 부모 한정, DROP 이름 일치, `_ibfk_` 번호, 규칙 이름, 중복 이름, SHOW CREATE 이름)을 모두 잡았다.
- 전체 시험 **781 케이스 / 1,505,497 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과.

**한계**: `MAX_EXECUTION_TIME`은 SELECT만(MySQL도 같다)이고 병렬 스캔의 작업 스레드는 확인하지 않는다. 한 열에는 외래키 하나만 둘 수 있어(여러 열 FK도 없음) 같은 열에 다른 이름으로 더하면 앞의 것을 덮어쓴다 — 편집기는 이미 FK가 있는 열을 경고한다. 집계 위의 윈도(`SUM(i) OVER (ORDER BY g) … GROUP BY g, i`)는 오류가 아니라 **빈 값**을 돌려주는 알려진 오답이다(`SUM(SUM(i)) OVER ()`는 오류) — 윈도 단축형과는 별개라 손대지 않았다. `run_script`의 미리보기는 DML만 된다(DDL은 엔진에서 트랜잭션을 끝낸다). `TRACE`의 Rows 열은 SELECT의 단계에만 있고 DML에는 없다.

### 10월 21일 — R47: 집계·그룹 위의 윈도(조용히 빈 값을 돌려주던 오답), 식 안의 윈도, 같은 값(피어)을 함께 잡는 프레임

**왜 이 항목인가**: R46 뒤 추천 목록에서 사용자가 "집계 위 윈도 (지금은 빈 값을 돌려주는 오답)"를 골랐다. 진행 순서는 개발 → 시험 → 죽은 코드 정리 → 문서 → 커밋·푸시 → 보고.

**고친 것**
- **집계·그룹 위의 윈도**: `SELECT g, SUM(i) OVER (ORDER BY g) FROM t GROUP BY g, i`의 윈도 열은 비어 있었고, `SUM(SUM(i)) OVER ()`는 "Cannot read the argument", `RANK() OVER (ORDER BY SUM(i) DESC)`는 문법 오류였다. 원인은 윈도를 GROUP BY **앞에서** 원래 행 위에서 계산했고, `PARTITION BY`·`ORDER BY` 항목과 인자가 열 이름만 받았다는 것. 이제 (1) 파서가 OVER의 항목과 `LAG`·`LEAD`·`FIRST_VALUE`·`LAST_VALUE`·`NTH_VALUE`의 인자를 식·집계로 읽고(`parse_sort_item`), (2) 그룹·집계 질의의 윈도는 GROUP BY·HAVING **뒤**, DISTINCT·ORDER BY·LIMIT **앞**에서 그룹 행 위에서 계산하며(`windows_over_groups`, 항목의 식·집계는 그룹마다 한 번 계산하는 `window_sorts`), (3) 바인더가 윈도 항목을 검증하고 ONLY_FULL_GROUP_BY가 윈도가 읽는 열도 본다(MySQL 1055), (4) 집계가 두 겹인 `SUM(SUM(SUM(v))) OVER ()`는 1111 "Invalid use of group function".
- **식 안의 윈도**: `SUM(i) / SUM(SUM(i)) OVER ()`(전체 대비 비율), `i - LAG(i) OVER (ORDER BY id)`, `CASE WHEN ROW_NUMBER() OVER (…) = 1 …`, `ORDER BY RANK() OVER (…)`. 파서가 호출을 질의의 **숨은 윈도 열** `__win<n>`으로 바꾸고 식이 그 열을 읽는다(`hidden_window_name`, `engine/column_text.hpp`). 결과, 뷰, 파생 표, 결과 타입, 서브쿼리의 열 수에 숨은 열은 나오지 않고, 이름 없는 항목의 열 이름은 MySQL처럼 쓴 글자 그대로다. 그 식을 읽는 ORDER BY는 윈도를 계산한 뒤에 값을 구한다.
- **식을 윈도 항목으로**: `OVER (ORDER BY i + id)`, `PARTITION BY g % 2`, 열이 아닌 인자 `SUM(i * 2) OVER (…)`도 받는다(집계 없는 질의에서는 행마다 값을 계산; 전에는 말없이 틀렸다).
- **윈도 프레임의 오답 (이 작업을 하다 찾음)**: ORDER BY에서 같은 값인 행(피어)을 프레임에 넣지 않았다 — `SUM(v) OVER (ORDER BY g)`가 같은 g의 행마다 다르게 누적되고, `COUNT(*) OVER (ORDER BY g)`, `LAST_VALUE`, `RANGE BETWEEN … CURRENT ROW`도 틀렸다. 이제 기본 프레임(`RANGE UNBOUNDED PRECEDING … CURRENT ROW`)과 명시적 RANGE 프레임은 피어를 함께 잡고, ROWS 프레임은 그대로다.
- **그룹 질의의 DISTINCT (이 작업을 하다 찾음)**: `SELECT DISTINCT … GROUP BY`가 DISTINCT를 무시했다(`SELECT DISTINCT SUM(v) FROM t GROUP BY g`가 같은 합을 여러 번 냈다). 이제 윈도 뒤, ORDER BY 앞에서 적용한다.

**검증**
- 신규 코퍼스 `window_agg.sql` 70문장(집계 위 윈도 37, 식 안의 윈도·키 식·뷰·파생 표·DISTINCT·오류 33)이 진짜 MySQL 8.4.9와 차이 0, `windows.sql`에 피어 11문장을 더해 37문장 차이 0; `--emit`이 `mysql_conformance_window_agg.inc`(와 갱신된 `…_windows.inc`)를 만든다. 열 이름도 MySQL과 대조했다(`SUM(i) / SUM(SUM(i)) OVER ()` 그대로).
- `[r47]` 시험: 집계 위 윈도, HAVING 뒤의 윈도, 식 안의 윈도, DISTINCT, ORDER BY의 윈도, 숨은 열이 결과·뷰에 안 나옴, 오류 셋, 피어 프레임.
- 심은 버그 15종을 모두 잡았다.
- 전체 시험 **783 케이스 / 1,505,711 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과.

**한계**: `RANGE BETWEEN 5 PRECEDING AND …`처럼 값을 기준으로 한 오프셋은 여전히 행 수로 읽는다(피어만 고쳤다). 윈도 함수를 `WHERE`·`GROUP BY`·`HAVING`에 쓰면 오류다(MySQL도 3593). 결과 열이 하나도 안 보이는 숨은 윈도의 이름 `__win…`으로 시작하는 열 이름을 사용자가 직접 쓰면 숨은 열과 겹칠 수 있다.

### 10월 22일 — R48: 추천 표 점검, `DEMO.md` 다시 쓰기, B+Tree 페이지

**왜 이 항목인가**: 추천 표를 항목마다 엔진에 돌려 보고 코드를 찾아 점검한 뒤(`RECOMMAND.md`), 사용자가 발표 대비 두 항목 `DEMO.md`와 B+Tree 애니메이션을 골랐다. 고친 것은 코드가 아니라 앱·문서뿐이고 엔진은 그대로다.

**점검에서 바로잡은 것** (`docs/mds/RECOMMAND.md` 맨 아래에 목록): `RANGE n PRECEDING`과 `DATE(x) + 0`은 "일부"가 아니라 **조용한 오답**(MySQL과 다른 값), 라벨 붙은 루프는 "END LOOP 라벨"만이 아니라 라벨 문법 전체가 없음, `max_connections` 변수 자체가 없음, INSERT 쪽 UNIQUE/FK 검사는 이미 O(1)(표 0→21,000행에서 행당 0.2ms)이고 **부모 행 DELETE의 자식 검사만 O(자식 수)**(자식 2천 행 1.1ms → 2만 행 12ms, 자식 FK 열에 인덱스를 만들어도 8ms), `DEFAULT NOW()`류는 이미 됨, 앱 Server Manager의 Stop이 서버를 강제 종료(`child.kill`)하므로 크래시 시험이 이미 가능, 흔한 MySQL 함수 이름 254개 중 45개가 없음.

**`DEMO.md`** (`code/test/demo/DEMO.md`): "피할 것" 목록이 R3에서 고친 것(`SUM(amount * 2)`, `ROUND(AVG(x), 0) * 2`, 열 이름 `ROUND()` …)을 아직 피하라고 적고 있어서, 시연 문장과 그 항목을 지금 빌드에 다시 돌려 확인하고 고쳤다. 수치를 다시 쟀고(집계 0.09초, 조인 0.9초, 인덱스 전 60ms → 후 0.4ms, 인덱스 생성 0.6초), 새 장면을 더했다: 집계 위 윈도 순위, 한 문장의 여행(Journey/`TRACE`), 락 대기와 교착(두 세션, `SHOW LOCKS`, `innodb_lock_waits`를 실제로 돌려 B가 1.06초 기다렸다 풀리는 것 확인), B+Tree 페이지, MCP `run_script` 미리보기→확인→저장. 서버 강제 종료는 앱의 Stop 버튼으로도 된다고 적었다. 남은 한계 목록은 현재 사실로 바꿨다(`RANGE n PRECEDING`, `DATE + 0`, 지원하지 않는 문법).

**앱: B+Tree 페이지** (`components/BTreeView.tsx`, `lib/bplusTree.ts`, 활동 표시줄의 나무 아이콘): 교육용 B+Tree에 키를 넣거나 지우거나 찾으면 일어나는 일을 **한 단계씩 재생**한다 — 잎까지 내려가는 길, 노드가 한도를 넘어 쪼개지고 키가 위로 올라가는 것(잎은 키를 복사해 올리고 안쪽 노드는 옮겨 올림), 루트가 새로 생겨 한 층 높아지는 것, 노드가 모자라 형제에게서 빌리거나 합쳐지는 것, 루트가 한 층 낮아지는 것. 재생·일시정지·한 단계씩·슬라이더·0.5~4x는 Journey와 같은 방식이고, 노드는 CSS 이동으로 자리를 옮기며 새 노드는 서서히 나타난다. 노드당 최대 키 3·4·5, `1–12 넣기`, `무작위 ×8`, KO/EN 전환. 잎은 서로 이어진 화살표로 그린다.
- 트리 모델은 **무작위 삽입·삭제 약 78,000번**(최대 키 3·4·5, 시드 60개)마다 불변식을 검사해 확인했다: 모든 잎이 같은 깊이, 키 정렬과 범위, 최소 채움(잎 ⌈M/2⌉, 안쪽 ⌈(M+1)/2⌉-1), 안쪽 노드의 자식 수, 잎 연결 순서, 도달할 수 없는 노드 없음, 넣고 지운 키 집합과 같음.
- 화면은 `invoke` 없이 헤드리스 Chrome으로 그려 확인했다(1–12 삽입 끝 상태, 삭제 뒤 병합). **실제 Tauri 앱에서 눌러 본 것은 아니다.**

**한계**: 엔진의 실제 인덱스 값(쪽 크기, 페이지)을 읽는 것이 아니라 같은 원리를 보여 주는 모형이다. 키는 정수만 받고 중복 키는 거절한다.

### 10월 22일 — R49: MVCC 나란히 보기, 그리고 연결이 끊긴 트랜잭션이 남던 버그

**왜 이 항목인가**: 추천 표의 발표 대비 항목 "MVCC 나란히 보기"를 사용자가 골랐다(`_xmin`/`_xmax`를 조회할 방법이 없어 엔진에 문을 먼저 열어야 한다고 적었던 항목). 진행 방식은 R46~R48과 같다: 코드 먼저, 항목 시험, 문서, 커밋·푸시, 짧은 보고와 추천 표, 앱 빌드.

**엔진**
- **`information_schema.row_versions`** (RuSQL만의 표, MySQL에는 없다): 모든 표의 모든 행 **버전**을 한 행씩 보여 준다 — `TABLE_SCHEMA`, `TABLE_NAME`, `ROW_KEY`(기본 키 값), `ROW_DATA`(`열=값, …`), `XMIN`(버전을 만든 트랜잭션), `XMAX`(끝낸 트랜잭션, 가장 새 버전은 0), `STATE`, `VISIBLE_TO_ME`. `STATE`: `LIVE`(가장 새롭고 커밋됨), `UNCOMMITTED`(열린 트랜잭션이 만든 버전), `ENDING`(열린 트랜잭션이 끝내는 중: 지우거나 새 버전으로 바꾸는 중), `DEAD`(끝났고 커밋됨, 옛 스냅샷을 위해 VACUUM 전까지 남음). `VISIBLE_TO_ME`는 **읽는 세션 자신의 스냅샷**이 정한다(READ COMMITTED는 지금까지 커밋된 것, REPEATABLE READ는 BEGIN 때의 것, READ UNCOMMITTED는 모두) — 이 표를 읽는 문장이 `exec_select`에서 `current_read_ctx`로 채운다. 한 행의 버전은 오래된 것부터 붙여 보여 주고, 버전이 2,000개를 넘는 표는 평범한 `LIVE`를 빼고 보여 준다. 이 표를 읽는 문장은 데이터베이스 전체를 배타로 잠그는 경로(`information_schema`는 읽기 전용 경로가 아니다)를 타서 다른 세션의 쓰기와 겹치지 않고, 질의 캐시에도 들어가지 않는다.
- **버그 수정 — 연결이 끊긴 트랜잭션이 영원히 남았다**: 클라이언트가 `COMMIT`·`ROLLBACK` 없이 연결을 닫으면(앱이 꺼지거나 프로세스가 죽거나 소켓만 닫아도) 서버가 그 트랜잭션을 그대로 두어 **행 락과 커밋되지 않은 버전이 서버가 꺼질 때까지 남았다**: 그 행을 고치려는 다른 세션이 50초 락 대기 시간 초과를 맞았다(`innodb_trx`에 `RUNNING`이 계속 남음). 이제 네이티브·MySQL 프로토콜 두 처리기가 연결이 끝날 때 열린 트랜잭션을 `ROLLBACK`한다(`server/src/main.cpp`, `mysql.cpp`).

**앱: MVCC 페이지** (`components/MvccView.tsx`, 활동 표시줄의 겹친 상자 아이콘, Tauri 명령 `mvcc_exec`·`mvcc_reset`): **서버에 연결한 두 세션 A·B**(서버에 거는 별도 TCP 연결, 각자 자기 트랜잭션)와 그 사이에 `row_versions`를 두 세션 각각으로 읽어 만든 **버전 표**를 나란히 보여 준다 — 버전마다 xmin·xmax·상태와 "A에게 보이는가, B에게 보이는가"(●/○). 세션 쪽에는 트랜잭션 번호와 격리 수준, SQL 입력, BEGIN·COMMIT·ROLLBACK 단추, 최근 결과가 있다. 시나리오 네 개를 한 단계씩(또는 끝까지) 따라간다: ① UPDATE는 덮어쓰지 않고 버전을 더한다(삭제는 xmax만 붙이고 VACUUM이 치움), ② 같은 질문 다른 답(A를 REPEATABLE READ와 READ COMMITTED로 바꿔 가며), ③ 더티 리드(READ UNCOMMITTED, 롤백하면 사라지는 버전), ④ 기다리는 쓰기(B의 UPDATE가 A의 락 때문에 돌아오지 않다가 A의 COMMIT으로 이어짐; 기다리는 세션은 "기다리는 중" 표시를 달고 다른 세션은 계속 쓸 수 있다). 실습 표 `mvcc_demo`는 지금 쓰는 데이터베이스에 만든다(선택된 데이터베이스가 없으면 안내). KO/EN 전환.
- Tauri: `send_one`의 본문을 연결과 분리한 `exchange`로 빼고, 서버에 AUTH를 마친 `PeerSession`을 이름(`A`·`B`·`S`)으로 보관한다. 세션마다 자기 `Mutex`를 가져서 락을 기다리는 세션이 다른 세션을 막지 않는다.

**손질 (같은 날, d8c2ea8)**: 활동 표시줄에서 MySQL 호환성 아이콘을 MVCC 바로 아래로 옮기고(계정·설정은 맨 아래 그대로), 두 페이지의 한국어 제목을 "B+Tree 애니메이션"·"MVCC 시뮬레이션"으로 바꿨다.

**검증**
- `[r49]` 시험: 같은 표를 두 세션으로 읽어 UPDATE 중인 행의 두 버전(`ENDING`/`UNCOMMITTED`)이 A에게는 새 것만, B에게는 옛 것만 보이는 것, READ UNCOMMITTED의 더티 리드, 커밋 뒤 `DEAD`/`LIVE`, REPEATABLE READ가 옛 버전을 계속 보는 것, VACUUM이 죽은 버전을 치우는 것.
- 페이지의 네 시나리오를 **실제 서버에 두 연결로** 돌려 18가지를 확인했다(각 단계 뒤 두 세션이 보는 버전, 기다리는 B가 있는 동안 A가 버전을 읽을 수 있음, 롤백된 버전이 사라짐, **닫힌 연결이 롤백됨**).
- 화면은 `invoke`를 실제 서버로 중계하는 하니스에서 헤드리스 Chrome으로 확인했다(UPDATE 직후의 두 버전과 A·B 가시성, 스냅샷 시나리오에서 A에게만 보이는 DEAD 버전). **실제 Tauri 앱**(`npm run tauri dev`, WebView2 디버그 포트로 페이지 안에서 단추를 눌러 조작)에서도 확인했다: 연결 → `demo` 선택 → MVCC 페이지에서 실습 표 만들기, UPDATE 시나리오 3단계 뒤 A·B에게 보이는 버전이 서로 다르게 나오고(ENDING ○●, UNCOMMITTED ●○), 기다리는 쓰기 시나리오는 3단계에서 "기다리는 중"이 뜨고 4단계(A COMMIT)에서 풀려 단계마다 2~3초 안에 끝난다(50초 락 대기 시간 초과에 걸리지 않음). 화면 아이콘·언어 전환·모달 같은 겉모습은 사람이 직접 눌러 본 것이 아니다.
- 전체 시험 **784 케이스 / 1,505,745 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과.

**한계**: 자동 `DELETE`(트랜잭션 없이 한 문장)는 열린 스냅샷이 없으면 죽은 버전을 바로 치워서 `DEAD`로 보이지 않는다(그래서 시나리오의 삭제는 트랜잭션 안에서 한다). 버전이 2,000개를 넘는 표는 `LIVE`를 보여 주지 않는다. 실습 표는 `DROP TABLE IF EXISTS mvcc_demo`로 다시 만든다.

### 10월 24일 — 홈 화면: 연결 검색, 즐겨찾기·최근 연결, 자동완성 목록이 빈 글자로 보이던 버그

엔진은 그대로이고 앱만 바뀌었다. 소개글에 넣을 홈 화면 사진을 찍다가 나온 요청들이다.

- **버그 수정 — 자동완성 목록의 글자가 안 보임**: SQL을 치면 뜨는 제안 목록이 아이콘만 있고 글자가 비어 있었다. 글자는 DOM에 있었지만 **앱 CSS의 `.main { flex-direction: column }`가 Monaco 제안 행 안쪽의 `.main`에도 걸려** 아이콘과 글자가 두 줄로 쌓였고, 행 높이(19px)가 둘째 줄을 잘라냈다. 앱 쪽 클래스 이름을 `app-main`으로 바꿔 겹침을 없앴다(CSS·JSX·결과창 드래그의 `querySelector` 세 곳). 실행 중인 앱에서 `CREATE TA`를 입력해 다섯 항목이 글자와 함께 보이는 것을 확인했다.
- **연결 검색창**: 홈 화면 왼쪽 사이드바 맨 위. 이름·사용자·호스트·데이터 폴더 이름에서 대소문자 구분 없이 찾고, 사이드바 목록과 카드 그리드가 같이 걸러진다. 일치하는 것이 없으면 "No matching connections".
- **즐겨찾기(★)와 최근 연결**: 사이드바에 `FAVORITES`(★ 표시한 연결)와 `RECENT`(마지막으로 연결한 순서 3개와 "2h ago" 같은 경과 시간)를 `CONNECTIONS` 위에 바로가기로 둔다. 별은 목록 항목(마우스를 올리면)과 카드(올리면, 켜져 있으면 항상)에서 켜고 끄며, 즐겨찾기 줄의 별을 눌러도 끌 수 있다. 연결이 성공할 때(`doLogin`) 그 연결의 `lastUsed`(epoch ms)를 기록한다. 검색은 즐겨찾기·최근에도 적용된다.
- **저장 형식**: `code/data/connections.json`의 항목에 `favorite`(bool)와 `lastUsed`(숫자)가 더해졌다. Rust `ConnectionEntry`에 `#[serde(default)]`로 넣어서 이 필드가 없는 예전 파일과 MCP가 쓴 항목도 그대로 읽히고, MCP 서버는 항목을 딕셔너리째 읽고 쓰므로 새 필드를 지우지 않는다.
- **확인**: 앱을 다시 빌드해 띄운 뒤(Rust 구조체 변경) 별을 켜고 끄면 `connections.json`에 바로 반영되고, `local-dev`(자동 로그인)로 실제 로그인하면 `lastUsed`가 기록되어 `RECENT`에 맨 위로 오는 것을 확인했다. 한 번에 별 두 개를 같은 틱에 누르면 나중 것만 남는 것은 시험 스크립트가 만든 상황이고, 사람의 클릭은 다시 그려진 뒤에 들어온다.
- **한계**: 사진용 예시 연결 8개(`prod-orders` 등)는 `connections.json`에만 있고 저장소에는 올라가지 않는다. `RECENT`는 3개 고정이다.

### 10월 25일 — R50: RANGE 값 오프셋, 날짜 함수와 SUM(날짜)의 수 값, FLOAT 단정밀도, 오류 1093 외 (추천표 1순위)

**왜 이 항목인가**: 추천표 1순위(오류도 없이 틀린 값을 돌려주는 것)를 사용자가 골랐다("1순위 진행합니다"). 진짜 MySQL 8.4.9에 같은 문장을 돌려 차이를 하나씩 확인하고 고쳤다. 새 코퍼스 넷(`range_frames`, `temporal_values`, `float_single`, `target_table`)이 이 라운드의 증거다.

**고친 것 (엔진)**
- **`RANGE BETWEEN n PRECEDING AND m FOLLOWING`이 값 범위로 동작**: 오프셋을 행 수로 읽던 오답을 고쳤다. `ORDER BY v RANGE BETWEEN 10 PRECEDING AND CURRENT ROW`는 v가 현재 행보다 10 이하로 작은 행까지(10, 15, 30이면 10·25·30; 전엔 10·25·55). 소수 오프셋(`1.5`), 날짜·날짜시간 키의 `INTERVAL n unit`(MICROSECOND~WEEK은 고정 길이, MONTH·QUARTER·YEAR는 달력 — 2024-03-31에서 한 달 전은 2024-02-29), ASC와 DESC(DESC는 PRECEDING이 더 큰 값 쪽), NULL 키(자기들끼리 한 무리: 오프셋 끝은 NULL 행만, 다른 행의 틀에는 NULL 행이 없음)를 MySQL처럼 처리하고, 정렬 식이 하나가 아니거나 수·시간 형이 아니면 3587, 수 키에 INTERVAL이면 3589, 날짜 키에 수 오프셋이면 3588 (`executor_window.cpp`의 `RangeAxis`).
- **행이 하나도 없는 프레임**: `ROWS BETWEEN 1 FOLLOWING AND 2 FOLLOWING`의 마지막 행, `ROWS BETWEEN 2 PRECEDING AND 1 PRECEDING`의 첫 행은 전엔 마지막(첫) 행 하나로 잘려 들어갔다(`COUNT`가 1, `SUM`이 그 행 값). 이제 `COUNT` 0, 나머지 NULL, `FIRST_VALUE`·`LAST_VALUE` NULL. MySQL이 거절하는 프레임도 거절한다: 시작이 `UNBOUNDED FOLLOWING`(3584), 끝이 `UNBOUNDED PRECEDING`(3585), `CURRENT ROW .. n PRECEDING`·`n FOLLOWING .. CURRENT ROW`·`n FOLLOWING .. m PRECEDING`(3586), `ROWS`의 소수·INTERVAL 오프셋(3586).
- **날짜 함수의 결과와 `SUM`·`AVG`(날짜)가 수로 읽힘**: `DATE('2024-03-05') + 0`이 `2024`였다(MySQL 20240305). 날짜·시간 **열**은 R44에서 되었지만 함수 결과는 아니었다. 이제 `DATE`·`CURDATE`·`LAST_DAY`·`STR_TO_DATE`·`NOW`·`TIMESTAMP`·`TIME`·`CURTIME`·`CAST(x AS DATE / DATETIME / TIME)`·`DATE_ADD(날짜, INTERVAL …)` 같은 함수의 결과가 산술·수 함수·수와의 비교(`=`, `<`, `BETWEEN`, `IN`)에서 20240305 / 123045 / 20240305123045로 읽힌다(`ast.hpp`의 `kind_of_expr`). `MIN(d)`·`MAX(d)`도 날짜의 종류를 가진 채 식에 들어간다(`MAX(d) + 0`). `SUM`·`AVG`는 날짜·시간·DATETIME 열과 날짜 함수 인자를 `(인자) + 0`으로 읽어 더한다(`SUM(날짜 열)`이 `4048`이었고 MySQL은 `40480406`; `AVG`는 `20240203.0000`) — 바인더가 집계의 `source`를 바꾸고(결과 열 이름은 그대로), 윈도 `SUM/AVG … OVER`도 같다.
- **FLOAT 열은 단정밀도 수**: `f = d`(FLOAT 0.1 = DOUBLE 0.1)가 같다고 나왔다(MySQL은 다름: FLOAT 0.1은 DOUBLE로 넓히면 0.10000000149011612). `f = 0.1`도 MySQL은 거짓. 이제 FLOAT 열은 산술(`f + 0` = 0.10000000149011612, `SUM(f)`·`AVG(f)`)과 비교(왼쪽·오른쪽, `BETWEEN`, `IN`, 수 함수 인자)에서 단정밀도 수를 DOUBLE로 넓힌 값으로 읽는다(`ArithExpr::Col::single`). 열이 보여 주는 글자는 그대로(`0.1`). 인덱스가 있는 FLOAT 열의 `f = 0.1`도 MySQL처럼 비어 있고 `f > 0.1`은 두 행(확인).
- **오류 1093** "You can't specify target table 'x' for update in FROM clause": `UPDATE`·`DELETE`(여러 표 형태 포함)의 `WHERE`·`SET` 안 하위 질의, `INSERT … VALUES (…, (SELECT … FROM 같은 표))`와 `ON DUPLICATE KEY UPDATE`, `REPLACE`가 자기가 쓰는 표를 읽으면 거절한다(전엔 받아들여 실행했다). 표를 바꾼 별칭이 있으면 그 별칭이 오류에 나온다(`UPDATE t a … (SELECT … FROM t)`는 'a'). 파생 표(`FROM (SELECT … FROM t) x`), 자기 자신과의 JOIN, `INSERT … SELECT`는 MySQL도 허락하므로 그대로다.
- **하는 김에 찾은 버그**: `INSERT … ON DUPLICATE KEY UPDATE v = (SELECT … FROM 다른표)`가 "Table 'other' not found" — 그 하위 질의의 표 이름에 현재 데이터베이스를 붙이지 않았다(`qualify_stmt`).
- **`HAVING ANY_VALUE(x)`의 x는 select 목록의 열이어야 함**: 아니면 1054 `Unknown column 'x' in 'having clause'`(MySQL처럼; 전엔 받아들임).
- **저장 프로시저 안 `SELECT a + b`의 열 이름**: 변수를 값으로 바꾸기 전의 식 `a + b`(전엔 `2+6`).
- **`VERSION()`**이 `@@version`·MySQL 프로토콜 인사와 같은 `5.7.0-rusql`(전엔 `2.3.0`이라 `SELECT @@version = VERSION()`이 거짓). **알리는 버전 자체(5.7)는 일부러 안 바꿨다**: `@@version LIKE '8%'`는 계속 거짓이다. 8.x로 알리면 JDBC 같은 드라이버가 8.x 전용 문장을 보낼 수 있어 확인 없이 바꾸지 않았다(사용자 결정 거리).
- **`SHOW COLLATION`에 `Pad_attribute` 열**(MySQL 8의 7번째 열). `SHOW ENGINES`(MySQL은 9행: 이 엔진이 없는 것들을 "지원"한다고 적어야 해서)와 `SHOW TABLE STATUS`의 크기 값은 일부러 그대로다.

**검증**
- 새 코퍼스 넷을 진짜 MySQL 8.4.9와 비교해 차이 0: `range_frames` 44문장, `temporal_values` 32, `float_single` 28, `target_table` 28 — `--emit`으로 만든 사례가 `test_mysql_conformance.cpp`에서 돈다(`any_value` 코퍼스도 `HAVING ANY_VALUE` 사례를 더해 다시 만듦). 처음 `float_single`은 표 설정이 양쪽에서 조용히 실패해(`DECIMAL(8,2)`에 16777216) 빈 표를 비교하고 있었다 — 설정을 고치자 진짜 차이 넷이 보였고(`d = f` 오른쪽 FLOAT, 집계 표시, `SET d = f`) 그중 하나를 고쳤다.
- 기존 코퍼스 21개는 어느 것도 늘지 않았고 오히려 줄었다: 차이 15개 → 11개(함수 3 → 2, 제약 5 → 3, 프로토콜 4 → 3; 표·protocol_types는 그대로). (`SHOW COLLATION`의 `Pad_attribute`는 확실히 이 라운드의 몫이고, 나머지 줄어든 문장은 1093과 날짜·FLOAT의 수 값이 고친 것으로 보이나 하나씩 가려내지는 않았다.) 무작위 대조 12종(`verify_compare`, `verify_aggregates`, `verify_agg_expressions`, `verify_agg_arguments`, `verify_null_expressions`, `verify_value_expressions`, `verify_sort_group`, `verify_orderby_distinct`, `verify_joins`, `verify_subqueries`, `verify_correlated`, `verify_writes`) 위반 0.
- `[r50]` 시험(RANGE·빈 프레임·날짜 수·FLOAT·1093·ON DUPLICATE·ANY_VALUE·프로시저 열 이름·VERSION)과 위 사례들. **심은 버그 21종 모두 잡힘**(처음엔 DESC 심기가 효과가 없어 살아남았고, 달 오프셋의 DESC 시험을 더해 잡았다).
- 전체 시험 **789 케이스 / 1,506,079 assertions**을 Release 기본과 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과. 호환성 점수판: 코퍼스 25개, 문장 2,647개 중 차이 11개.
- 성능(바로 앞 커밋의 서버를 따로 빌드해 번갈아 2라운드, 5만 행, 가장 빠른 값; 앞 → 이번): 날짜·FLOAT와 상관없는 문장은 그대로다 — `COUNT(*) WHERE a + k > c` 29.7·29.9 → 29.4·32.7ms, `SUM(a + b + k)` 135.8·135.5 → 133.6·135.6, `GROUP BY` + 집계 4개 41.9·42.6 → 41.4·41.8, `SUM(a)`·`AVG(b)` 26.2·25.6 → 25.6·26.1, DOUBLE 열 비교 34.3·32.9 → 32.4·31.6. 바뀌는 것은 하나다: **FLOAT 열을 비교하면 행마다 단정밀도로 넓히므로** `WHERE f > c`가 32~34 → 45~46ms (이 값이 MySQL과 같아지는 값이다; 처음엔 52ms였고 글자 변환을 줄여 45ms로 내렸다). `SUM(f)`·`SUM(날짜)`는 23~25 → 23~26ms, `WHERE DATE(dt) + 0 > c`는 70 → 60ms(답이 달라져 비교 자체가 다르다).
- **하지 않은 것**: Debug 빌드, 크래시 라운드.

**한계 (새로 찾은 것 포함)**
- (R51에서 고침) `ORDER BY ANY_VALUE(x)`에서 x가 select 목록에 없으면 정렬이 무시되고, select 항목 `ANY_VALUE(x) AS a`를 `HAVING a > 5`로 거르면 0행이 나왔다.
- `MIN(f)`·`MAX(f)`의 FLOAT 결과가 여섯 자리가 아니라 저장된 글자 그대로 나온다(16777216 → MySQL 16777200). `UPDATE … SET d = f`(FLOAT를 DOUBLE 열에)는 짧은 글자를 그대로 복사한다(MySQL 0.10000000149011612).
- 1093은 저장 루틴 안의 문장에서는 확인하지 않는다(바인더의 `check`가 꺼져 있음). 날짜로 읽는 함수는 위 목록이 전부다(`IFNULL`·`COALESCE`·`IF`가 감싼 날짜, 문자열 `DATE_ADD('2024-03-05', INTERVAL 1 DAY)`는 아님). `HAVING SUM(날짜 열)`의 집계 참조와 시간(TIME) 키의 RANGE 오프셋은 안 된다.

### 10월 25일 — R51: EXPLAIN 단계 막대·복구 시간 표시, `ORDER BY ANY_VALUE`·HAVING 별칭, MCP `suggest_indexes`

**왜 이 항목인가**: 추천표에서 사용자가 "EXPLAIN 단계 막대와 크래시 복구 시간 표시 / ORDER BY ANY_VALUE와 HAVING 별칭 / suggest_indexes"를 골라 한꺼번에 진행하라고 했다("시작"). SQL 문법 추가는 같은 날 정한 대로 보류(`RECOMMAND.md` 6번)했다.

**고친 것·만든 것 (엔진·서버)**
- **`ORDER BY ANY_VALUE(x)`와 HAVING 별칭**: 두 오답을 고쳤다. ① x가 select 목록에 없는 `ORDER BY ANY_VALUE(x)`는 정렬이 NULL로 되어 그룹이 g 순서로 나왔다(그룹이 첫 행의 열을 들고 있어야 하는 조건 `needs_group_row`가 select 목록만 봤다) → ORDER BY·HAVING의 식이 키가 아닌 열을 읽어도 그 행을 들게 했다. ② `SELECT g, ANY_VALUE(v) AS a … HAVING a > 5`가 0행이었다(식 항목의 별칭은 출력할 때에야 계산했다) → HAVING이 부르는 별칭의 식 항목은 HAVING 전에 그룹에서 계산한다(`cond_columns`, `make_group_row`).
- **`information_schema.recovery`** (RuSQL만의 표, 한 행): 엔진이 마지막으로 시작할 때 `OPEN_MS`(데이터를 열기까지)·`UNDO_MS`/`WAL_TXNS_REPLAYED`/`WAL_TXNS_UNDONE`(WAL)·`REDO_MS`/`REDO_OPS`/`REDO_TABLES`(redo 로그)·`CRASH_DETECTED`. 값은 `SharedDatabase::recovery`(`RecoveryStats`)에 `Executor(dir)` 생성자가 `recover_from_wal`·`recover_from_redo` 둘레의 시계와 두 복구가 세는 수로 채운다. 서버는 시작하면서 "Opened the data in … ms (no recovery needed)" 또는 "Recovered after an unclean stop in … ms: N redo operations in M tables replayed (… ms), K WAL transactions replayed and L undone (… ms)" 한 줄을 남긴다.

**앱·MCP**
- **EXPLAIN 단계 막대**: `EXPLAIN ANALYZE`의 `Step N` 줄을 시작 위치(앞 단계들이 쓴 시간)와 길이(그 단계의 시간)가 있는 막대로 그린다(가장 오래 걸린 단계는 노란색, 행 수·ms·비율 표시). **하다가 발견한 것**: 엔진의 `EXPLAIN` 결과는 `+---`로 시작해 Tauri의 `parse_output`이 한 열짜리 표("QUERY PLAN (ANALYZE)")로 만들기 때문에, 코드에 있던 "계획 트리" 화면(`r.columns.length === 0`일 때만)은 **한 번도 뜨지 않고 있었다**. 트리를 `ExplainPlan` 컴포넌트로 빼서 한 열짜리 `QUERY PLAN` 표도 이 화면으로 그리게 했다(기존 메시지 쪽 분기는 그대로 두고 같은 컴포넌트를 부른다).
- **Server Manager 복구 카드**: 상태 줄 아래에 `information_schema.recovery`와 "접속 가능해질 때까지"(프로세스 시작→로그인 성공) 시간을 보여 준다. `get_server_status`(Tauri)가 `recovery`·`ready_ms`를 더 주고, Start/Stop 뒤 ACTIVITY LOG에 한 줄("비정상 종료에서 복구: redo N건(M개 테이블, … ms), WAL 커밋 … 재적용·미완료 … 되돌림(… ms), 엔진 열기 … ms")을 남긴다. Start·Stop은 엔진 프로세스를 강제 종료하고 같은 데이터 폴더로 다시 여는 것이라 그 자체가 크래시 복구 시연이다.
- **MCP `suggest_indexes`** (도구 27개): SELECT·UPDATE·DELETE 문장을 정규식으로 읽어(주석·문자열 제거, FROM·JOIN·별칭, WHERE의 AND 조각, 같음·IN·범위·BETWEEN·LIKE 접두사·OR·함수로 감싼 열) 표의 열·인덱스(`information_schema`)·`ANALYZE TABLE`의 열별 서로 다른 값 수·(SELECT는) `EXPLAIN ANALYZE`의 계획과 가장 오래 걸린 단계를 모아 `CREATE INDEX` 제안(같음 열 → 범위 열 하나 → 같음 검색일 때의 ORDER BY 열)을 JSON으로 돌려준다. 제안마다 이유와 "인덱스가 있으면 읽을 행 수" 추정(같음 열의 서로 다른 값 수만 쓰고 범위는 추정에 넣지 않는다), 이미 있는 인덱스는 `already_indexed`, 효과가 없는 경우는 `notes`에 이유와 함께 남긴다(100행 미만 표, 20% 넘게 읽게 되는 열, 열을 함수로 감쌈, 앞이 `%`인 LIKE, OR, LIMIT 없는 ORDER BY만). UPDATE·DELETE는 `EXPLAIN ANALYZE`가 실제로 실행되므로 계획을 돌리지 않는다. **아무것도 만들지 않는다.** `ANALYZE TABLE`의 열 이름이 12자로 잘려 나오는 것(`region_manager_id`가 `region_manag`)도 처리했다.

**검증**
- 새 C++ 시험 `[r51]` 2 케이스(ANY_VALUE ORDER BY·HAVING 별칭 9문장, `information_schema.recovery`의 크래시 → YES·정상 → NO). **심은 버그 4종 모두 잡힘**: 그룹 행 조건 제거(ORDER BY가 g 순서), HAVING 별칭 계산 제거(0행), redo 개수 0, WAL 개수 제거.
- `any_value` 코퍼스에 7문장을 더해 진짜 MySQL 8.4.9와 비교: 26문장 차이 0 (`--emit`으로 `mysql_conformance_any_value.inc` 다시 만듦). 처음에는 별칭 `d`가 표의 열 `d`(DATE)와 겹쳐 한 문장이 달랐다 — 이건 이번 변경과 무관한 기존 한계(`SUM(v) * 2 AS d`도 같음)라 코퍼스의 별칭을 바꾸고 한계로 적었다. 호환성 점수판: 코퍼스 25개, 문장 2,654개 중 차이 11개(R50은 2,647개 중 11개).
- 무작위 대조 12종(`verify_compare` … `verify_writes`) 위반 0.
- MCP: 가짜 엔진으로 단위 시험 18개(`code/test/mcp/test_suggest_indexes.py`: 같음·범위·이미 있는 인덱스·작은 표·안 좁히는 열·함수·LIKE·OR·ORDER BY·조인·UPDATE/DELETE·12자 열 이름·주석과 문자열), 진짜 엔진(5천 행)으로도 확인: `customer_id = 42`는 인덱스 제안(읽을 행 10) 뒤 `Seq Scan` 2.7ms → 만든 뒤 `Index Scan` 0.2ms, `status = 'open'`(2값)은 "2500/5000행을 읽게 되어 제안 안 함".
- 앱(`npm run tauri dev` + WebView2 디버그 포트, 페이지 안에서 DOM으로 조작 — 마우스는 쓰지 않음; 임시 연결을 더했다가 `connections.json`을 원래대로 되돌림): 2만 행 표에서 `EXPLAIN ANALYZE … GROUP BY … ORDER BY … LIMIT`이 계획 트리 + 막대 다섯 개(Scan 60%·Sort 27%·Group 12%…, 가장 긴 막대 노란색)로 뜸. 커밋한 2행과 열어 둔 트랜잭션 1건을 만든 뒤 Start Server로 강제 종료·재시작하니 복구 카드가 "RECOVERED AFTER AN UNCLEAN STOP: 접속 558ms, 엔진 열기 235ms, redo 2건 1테이블 80ms, WAL 0 재적용·1 되돌림 48ms"로 바뀌고 ACTIVITY LOG에 같은 줄이 남음; 커밋한 2행은 살아 있고 열어 둔 행은 없음(표 20,002행). Stop 쪽도 확인(redo 1건).
- 전체 시험 **791 케이스 / 1,506,139 assertions**을 Release 기본으로, 1,506,140을 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과. `tsc --noEmit`·`cargo check` 통과.
- **하지 않은 것**: Debug 빌드, 크래시 퍼저 라운드, 성능 측정(변경은 HAVING이 있는 GROUP BY의 그룹당 식 항목 계산과 시작 시계 둘뿐), 빌드 간 차분. 앱 화면은 사람이 눌러 본 것이 아니다(아이콘·폰트·전체 모양).

**한계 (새로 찾은 것 포함)**
- HAVING이 부르는 별칭이 같은 이름의 실제 열과 겹치면(`SUM(v) * 2 AS d`인데 표에 열 `d`) 별칭이 아니라 열을 읽어 0행이다(MySQL은 별칭). 이전부터 있었고 별칭 이름 공간을 열과 따로 둬야 해서 고치지 않았다.
- `suggest_indexes`는 정규식 해석이라 하위 질의·CTE·여러 범위 열·방향이 섞인 ORDER BY는 읽지 않고, 추정은 열이 독립이라고 가정한다. 제안한 인덱스의 효과는 사용자가 만든 뒤 `EXPLAIN`으로 다시 재야 한다.
- 복구 카드의 값은 Start/Stop 뒤에만 바뀐다(앱이 켤 때 연 엔진의 값은 그대로 보임). "접속 가능해질 때까지"에는 프로세스 시작 뒤 접속 재시도 간격(최대 100ms 단위)이 들어 있다.

### 10월 25일 — R52: HAVING 별칭이 열과 겹칠 때, 자기 자신과 조인하는 UPDATE·DELETE, select 하위 질의의 ORDER BY·DISTINCT (조용한 오답 연속 수정)

**왜 이 항목인가**: R51 마지막 보고에서 "남은 문제"로 적은 HAVING 별칭 충돌을 사용자가 "오답은 같은 진행방식으로 계속 고칩니다"로 이어 가라고 했다. 같은 방식(진짜 MySQL 8.4.9와 견주기 → 고치기 → 시험·코퍼스 → 심은 버그 → 전체 시험)으로 점수판에 이미 있던 차이 11개와 새로 훑은 문장 62개에서 조용한 오답을 찾았다. 새 코퍼스 셋(`having_alias` 36문장, `self_join_write` 50, `subquery_item_order` 19)이 증거다.

**고친 것 (엔진·파서)**
- **HAVING의 별칭이 같은 이름의 열보다 먼저 읽힘**: `SELECT g, SUM(v) * 2 AS d … HAVING d = 40`인데 표에 열 `d`(DATE)가 있으면 0행이었다(MySQL은 별칭을 읽는다). 원인은 R51에서 넣은 "HAVING이 부르는 이름이면 그룹이 첫 행을 들게" 조건이 **별칭 이름까지** 세어 열 값을 그룹 행에 복사했고 그 값이 별칭 계산을 막은 것. 지금은 ① 별칭인 이름은 그 조건에서 빼서(R51이 별칭만 쓰는 흔한 `HAVING s > 5`마다 그룹당 한 행을 복사하던 것도 같이 없어짐) ② HAVING이 부르는 식·열 항목의 별칭을 그룹에서 계산하고 ③ 같은 이름의 열을 select 목록이 따로 읽어야 하면 HAVING이 볼 복사본에만 별칭을 넣는다. 같이 고쳐진 것: **열의 별칭**(`SELECT g, name AS n … HAVING n = 'a'`)은 어느 그룹도 찾지 못해 0행이었다, **GROUP BY 없는 집계**의 `HAVING d = 90`도 같은 문제였다.
- **자기 자신과 조인하는 UPDATE·DELETE가 아무 행도 안 바꿈**: `UPDATE up u1 JOIN up u2 ON u1.id = u2.id - 1 SET u1.b = u2.a`, 직원-관리자 `UPDATE up e JOIN up m ON e.mgr = m.id SET e.b = m.a`, 쉼표 형태 `UPDATE up a, up b SET …`, `DELETE u2 FROM up u1 JOIN up u2 …`가 오류도 없이 "0 row(s) updated"였다(점수판의 `constraints` 차이 3개가 이것). 파서가 별칭을 모두 표 이름으로 바꿔 두 사용이 하나가 되던 것을, SELECT의 자기 조인처럼 **두 번째 사용은 별칭을 이름으로 유지**하게 했고(`Join::alias`), 실행기는 조인된 행의 열을 그 별칭으로 구분하며 대상 표(`SET u2.a`, `DELETE u2`)도 별칭에서 실제 표를 찾는다. 별칭 없이 두 번 쓰면 "Not unique table/alias".
- **다중 표 UPDATE의 SET 값이 다른 표를 별칭으로 부름**: `UPDATE t1 x JOIN t2 y ON … SET x.b = y.a`의 오른쪽 `y.a`는 별칭을 풀지 않아, 두 표에 같은 이름의 열 `a`가 있으면 **첫 표의 값이 들어갔다**(이름이 다르면 우연히 맞았음).
- **select 목록 하위 질의의 별칭·위치로 ORDER BY, 그것에 대한 DISTINCT**: `SELECT name, (SELECT COUNT(*) FROM … WHERE … = e.id) AS reports FROM e ORDER BY reports DESC`가 정렬을 무시(표 순서)했고 `ORDER BY 2`는 오류였으며 별칭이 정렬 키로 인정되지 않는데도 오류조차 없었다. `SELECT DISTINCT (SELECT …) AS n`은 모든 행을 한 행으로 합쳤다. 바인더가 하위 질의 항목을 `__sq_<k>__`로 불러 주고 실행기가 정렬·DISTINCT 전에 행마다 그 값을 계산한다(묶는 질의의 정렬은 오류로 거절). (처음에는 이 항목을 파서에서 다른 식 항목과 같은 경로로 바꿔 보았으나 CTE의 `ORDER BY`가 깨져 되돌렸다.)

**검증**
- 새 C++ 시험 `[r52]` 2 케이스. **심은 버그 8종 모두 잡힘**: 충돌하는 별칭 복사본 제거, 열 별칭 계산 제거, GROUP BY 없는 경로의 별칭 제거, 자기 조인 별칭 유지 제거, SET 값의 별칭 풀기 제거, DELETE의 별칭 대상 찾기 제거, 하위 질의 정렬 제거, DISTINCT의 하위 질의 값 제거.
- 새 코퍼스 셋을 진짜 MySQL 8.4.9와 비교해 차이 0(`--emit`으로 `mysql_conformance_having_alias/self_join_write/subquery_item_order.inc`). 호환성 점수판: 코퍼스 28개, 문장 2,759개 중 차이 **8개**(R51은 2,654개 중 11개: `constraints`의 3개가 사라짐).
- 무작위 대조 12종 위반 0. 전체 시험 **796 케이스 / 1,506,430 assertions**을 Release 기본으로, 1,506,431을 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과.
- 성능(새 빌드에서 10만 행·5만 그룹, 가장 빠른 값): `HAVING SUM(v) > 100` 213ms, 별칭 `HAVING s > 100` 181ms, 식 별칭 249ms vs 식 직접 232ms, HAVING 없음 161ms — 별칭을 쓴다고 느려지지 않는다. 앞 빌드와의 직접 비교는 하지 않았다.
- **하지 않은 것**: Debug 빌드, 크래시 퍼저, 빌드 간 차분(`diff_builds`), 앞 빌드와의 성능 비교.

**조사하다 알게 된 것 (고치지 않음)**
- 다중 표 UPDATE에서 한 문장의 대입끼리 서로의 새 값을 읽는 순서는 MySQL도 계획에 따라 달라(문서도 "보장 없음") 한 번 순차 읽기로 만들어 봤다가 MySQL과 3문장이 달라져 되돌렸다. RuSQL은 모든 대입이 옛 값을 읽는다.
- `UPDATE … JOIN (SELECT …) d ON …`(파생 표)는 오류로 거절한다(오답은 아님 — `RECOMMAND.md` 보류 목록). 첫 사용에만 별칭을 붙인 `UPDATE t u1 JOIN t ON …`, `ORDER BY (SELECT …)`를 식으로 직접 쓰는 것도 오류로 거절한다.
- FLOAT 열 값이 유효숫자 여섯 자리를 넘으면 `MIN`/`MAX`/`GREATEST`/`COALESCE`가 저장된 글자를 보여 주고(MySQL은 여섯 자리) `UPDATE … SET d = f`·`INSERT … SELECT`(FLOAT → DOUBLE 열)는 짧은 글자를 복사한다. 드물어서 남겼다.
- `PI()`는 MySQL이 화면에 `3.141593`으로 보여 주는데 RuSQL은 전체 자릿수를 낸다(식에 쓰면 RuSQL 쪽이 정확해서 일부러 그대로).

### 10월 25일 — R53: `KILL [QUERY] n`, 실제 상태 카운터 `SHOW STATUS`, `EXPLAIN ANALYZE` 확장, 그리고 `SQL.md`(지원 문법 전체 목록)

**왜 이 항목인가**: 추천표에서 사용자가 "KILL [QUERY] n, 실제 상태 카운터(SHOW STATUS), EXPLAIN ANALYZE 확장"을 고르고, 지원하는 SQL 문법을 하나도 빠짐없이 한 줄씩 정리한 `docs/mds/SQL.md`를 만들라고 했다. 처리 순서는 늘 하던 대로: 코드 먼저 → 항목마다 시험 → 문서 → 커밋·푸시.

**만든 것 (엔진·서버)**
- **`KILL [QUERY | CONNECTION] n`**: 세션 목록(`SharedDatabase::process_list`)은 데이터베이스 락(`FairSharedMutex`: 쓰는 쪽이 기다리면 새 읽기도 막힘)과 따로 뮤텍스를 가지고 있어, `KILL`은 `execute_sql` 맨 앞(락을 잡기 전)에서 처리한다. 대상 세션이 락을 쥐고 있어도 닿는다. `KILL QUERY`는 대상이 문장을 실행 중일 때만(`command == "Query"`) 세션의 `kill_state`를 1로 올리고(자는 세션은 아무 일 없음), 그 상태는 세션이 "Sleep"으로 돌아갈 때 지워진다. 중단 지점은 두 곳이다: ① SELECT의 행 루프(`statement_deadline.hpp`의 `tick()`/`check_now()`가 `kill_read_flag`를 읽음, 문장 시계와 같은 자리), ② 행 잠금 대기(`LockManager::acquire/acquire_shared`가 `cv_.wait_until`을 100ms마다 깨워 `kill_flag`를 확인). 중단된 문장은 `ERROR 1317 (70100): Query execution was interrupted`로 끝난다. 실행 중인 쓰기 문장은 반쯤 쓰다 멈추면 되돌릴 길이 없어 일부러 끝까지 돌게 했다(`kill_flag`는 잠금 대기에서만 읽고 쓰기 루프는 읽지 않음). `KILL [CONNECTION] n`은 `kill_state`를 2(끊김, 지워지지 않음)로 올리고 세션이 등록할 때 맡긴 `close_connection`(서버: `shutdown` + `CancelIoEx`로 소켓 읽기를 깨움; Windows에서 `shutdown`만으로는 막힌 `recv`가 안 깨어난다)을 부른다. 서버 루프가 답을 보낸 뒤 빠져나가며 세션을 정리해 열린 트랜잭션을 되돌리고 락을 푼다. 권한은 같은 사용자 또는 root, 오류는 없는 번호 1094·남의 세션 1095·문법 1064, 자기 문장을 `KILL QUERY`하면 1317, 자기 연결을 `KILL`하면 응답 뒤에 끊긴다.
- **MySQL 핸드셰이크의 연결 번호 = 세션 번호**: MySQL 클라이언트가 읽는 번호(`mysql_thread_id`)는 리스너의 별도 카운터였고 프로세스 목록의 Id와 달라, 클라이언트가 읽은 번호로 `KILL`하면 "Unknown thread id"였다. 세션을 핸드셰이크 전에 만들어 그 번호를 보낸다(`mysql.cpp`).
- **`SHOW [GLOBAL | SESSION] STATUS [LIKE]`와 `information_schema.global_status`** (`server_status.hpp`, `executor_show.cpp`): MySQL 프로토콜에는 고정값(Questions 0, Uptime 0, Threads 1)뿐이었고 네이티브에는 없었다. 이제 두 프로토콜 모두 실제 카운터: `Com_select/insert/update/delete/replace/begin/commit/rollback/call/create_table/drop_table/alter_table/create_index/drop_index/kill`, `Questions`·`Queries`, `Connections`, `Max_used_connections`, `Aborted_connects`(인증 실패), `Threads_connected`·`Threads_running`, `Uptime`, `Innodb_buffer_pool_pages_data/total`·`read_requests`·`reads`, `Innodb_deadlocks`, `Innodb_row_lock_current_waits`·`Innodb_row_lock_waits`, `Innodb_redo_log_bytes`. `GLOBAL`은 서버 전체(원자 변수, 세션이 `SharedDatabase`에서 한 번 받아 둠), `SESSION`(기본)은 그 세션이 보낸 문장 수. 데이터베이스 락을 잡지 않아서 쓰기 문장이 락을 쥐고 있어도 막히지 않는다. 실패한 문장도 "보낸 문장"으로 센다. 일반 SHOW 경로로 보내면 읽기 락을 잡아 쓰는 문장 뒤에 줄을 서므로, `execute_sql` 앞쪽에서 가로채 락 없이 답한다.
- **`EXPLAIN ANALYZE` 확장** (`executor_select.cpp`, `executor_explain.cpp`, `explain_trace.hpp`): 조인이 단계가 되었다. 전에는 `Scan a + JOIN b` 한 줄이던 것이 `Scan a` / `Join b (inner|left|right|full outer|cross|natural, hash join|sort-merge join|index nested loop|reverse index nested loop|nested loop)` / `Join b (lateral, subquery for every row)` / `Filter (WHERE) and expressions`로 나뉘고, 플래너가 고른 알고리즘이 실행에서 다른 것으로 바뀌면(트랜잭션 안에서 인덱스 NL이 해시로 폴백 등) 실제로 쓴 이름을 말한다. 하위 질의는 단계로 만들지 않았다(시간이 그것을 부른 단계 안에 들어 있어 단계로 세면 비율의 합이 100%를 넘는다): 별도 줄로 "Subqueries not correlated, run once : N run(s), X ms (inside the steps below)", "Subqueries correlated, run per row : …"를 보여 준다(가장 바깥 하위 질의만 센다). 긴 단계 이름이 오른쪽 변을 넘어 상자가 어긋나서, 가장 긴 줄에 맞춰 상자 전체를 넓힌다(문자 수 기준이라 `→`·`≈` 같은 여러 바이트 글자가 있는 옛 줄의 어긋남도 같이 고쳐졌다).

**문서·앱**
- **`docs/mds/SQL.md` (새 문서)**: 지원 문법 전체를 한 줄씩(`SELECT : 데이터 조회` 식). 16개 장 — 데이터베이스, CREATE TABLE(컬럼·제약·파티션), ALTER/DROP/RENAME/TRUNCATE, 인덱스·뷰·별칭, INSERT·REPLACE·UPDATE·DELETE·MERGE, SELECT(절·조인·집합·CTE·잠금 읽기·윈도), 표현식·연산자·리터럴, 타입, 함수 204개(집계·윈도·문자열·정규식·숫자·날짜·조건·JSON·시스템 — 이름 205개를 불러 보아 `BIT_XOR`만 없음), 트랜잭션·SET·PREPARE, 조회·관리·모니터링(`information_schema` 19개 표 포함), 사용자·권한·역할, 프로시저·함수·트리거와 제어문, MySQL 프로토콜 전용 문법, 주석 형식, 지원하지 않는 문법. 쓰는 방법: 문법 하나하나를 `$TEMP/mcp_it/sql_probe.py` 등으로 실행 중인 엔진에 넣어 받아들여지는 것만 적고(459문장 + 추가 탐침 약 120문장), 함수 이름은 호출해 보아 "does not exist"가 아닌 것만 남겼다. MySQL 프로토콜에서만 되는 문법은 따로 모았다(`pymysql`로 확인).
- **앱**: `EXPLAIN ANALYZE`의 단계 이름이 길어져 단계 막대의 이름 칸(190px 고정)이 "2. Join d.eb (inner,…"에서 잘렸다. 단계 목록을 하나의 격자로 만들어(이름 칸 = 가장 긴 이름의 너비, 최대 60%) 이름이 다 보이고 막대가 줄 맞춰 있다(헤드리스 Edge로 실제 CSS 전후를 그려 확인; 좁은 칸에서는 이전처럼 말줄임). Diagram 페이지의 서버 문구에 KILL·SHOW STATUS를 더했다.
- 문서: README, `FUNCTIONS.md`(R53 네 항목, information_schema 19개), `DIFF.md`(KILL·SHOW STATUS·EXPLAIN 행), `SUMMARY.md`, `DEMO.md`(락 대기를 `KILL QUERY`로 푸는 장면), `RECOMMAND.md`(세 항목 제거, 새 항목 추가).

**검증**
- 새 C++ 시험 `[r53]` 3 케이스(SHOW STATUS의 문장 종류별·세션별·서버 전체 카운터·연결 수·redo·information_schema, KILL의 SELECT 중단·락 대기 중단·권한·오류·연결 끊기·자는 세션, EXPLAIN ANALYZE의 조인 단계·하위 질의 줄·상자 너비). **심은 버그 9종 모두 잡힘**: 락 대기가 KILL을 무시, 소유자 검사 제거, SESSION이 서버 값을 보임, 상자 넓히기 제거, 하위 질의 종류 뒤바꿈, 조인 단계 표시 제거, SELECT가 중단되지 않음(시험이 끝나지 않아 100초 제한에 걸림), `KILL QUERY`가 연결까지 끊음, `Com_kill` 미집계. **처음에는 락 대기 mutant가 살아남았다**: 잠금 대기가 KILL을 못 봐도 50초 뒤 락 대기 시간 초과로 문장이 끝나고, 중단 요청이 있으면 그 오류를 1317로 바꾸기 때문에 시험이 51초 걸려 통과했다. 중단 뒤 5초 안에 끝나야 한다는 단언을 넣고 같은 mutant가 잡힘(SELECT 쪽에도 같은 단언).
- 옛 시험 하나 고침: R45의 EXPLAIN ANALYZE 시험이 단계 이름 `Scan d.o + JOIN d.c`를 찾던 것을 `Scan d.o` + `Join d.c (inner, `로.
- 실서버 확인: 네이티브(`kill_test.py`: 긴 SELECT 1.5초에 중단→1317·세션 계속 사용, 락 대기 중단, 연결 끊김→트랜잭션 되돌려져 행이 풀림, 오류 1094·1064·자기 자신)와 MySQL 프로토콜(`pymysql`: 핸드셰이크 번호로 `KILL QUERY`, `SHOW STATUS`, `KILL CONNECTION`). `SHOW STATUS`는 쓰기가 락을 쥔 동안에도 바로 답했다.
- 무작위 대조 12종 위반 0. 전체 시험 **799 케이스 / 1,506,715 assertions**을 Release 기본으로, 1,506,714를 `RUSQL_DML_INDEX_MIN_ROWS=0`으로 통과(첫 바퀴에서 옛 R45 시험 하나가 실패해 위처럼 고친 뒤 두 설정 모두 처음부터 다시 돌림). 호환성 점수판은 R52와 같다: 코퍼스 28개, 문장 2,759개 중 차이 8개(스냅샷은 시험 수만 바뀜).
- 성능(SELECT의 행 루프에 중단 요청 읽기 하나, 문장마다 세션 상태 기록과 카운터가 늘었다): 10만 행·5만 그룹의 GROUP BY·HAVING 7문장을 **HEAD(R52) 빌드와 새 빌드로 번갈아 두 바퀴**(각 문장 가장 빠른 값) 재서 둘이 ±3% 안에서 같았다 — 예: `no HAVING` 새 206·202ms / HEAD 199·205ms, `HAVING SUM(v) > 100` 새 242·231 / HEAD 236·234. (R52 기록의 161ms는 그때 기계 상태의 값이라 이번 기계에서는 두 빌드 모두 200ms 안팎이다.)
- **하지 않은 것**: Debug 빌드, 크래시 퍼저, MySQL 코퍼스(카운터 값은 MySQL과 같을 수 없어 비교 대상이 아님).

**조사하다 알게 된 것 (고치지 않음)**
- **`LIKE`가 `\%`·`\_`를 모른다**: 렉서가 문자열 안의 `\%`·`\_`에서 역슬래시를 지워(`'axb' LIKE 'a\_b'`가 1, MySQL은 0, MySQL은 두 글자를 그대로 남긴다) 와일드카드가 되고, `LIKE … ESCAPE '!'`는 문법 오류. `LIKE` 비교기(`collation::like`)가 이스케이프를 읽지 않는다. 진짜 MySQL과 비교되는 조용한 오답이라 다음 오답 라운드의 후보(`SHOW STATUS LIKE 'Com\_%'` 시험을 쓰다 발견).
- **`USE`가 redo 로그를 비운다**: `USE db`는 읽기 전용으로 분류되지 않아 쌓인 redo 로그를 체크포인트(테이블 파일 재작성)한 뒤 실행한다(`INSERT` 뒤 `Innodb_redo_log_bytes` 84 → `USE` 뒤 0). 앱과 MCP(`execute_sql(database=…)`)는 문장마다 `USE`를 먼저 보내서 redo의 이점(커밋 = 작은 배치 + fsync 1번)이 없어질 수 있다. 성능 동결 중이라 하지 않고 여쭙기로 했다.
- **MySQL 프로토콜의 오류 번호는 전부 1064**다(진짜 번호는 메시지 글자 `ERROR 1317 (70100): …` 안에만). 드라이버는 번호로 재시도·중단을 정한다.
- `Innodb_buffer_pool_read_requests`·`reads`는 SQL을 아무리 돌려도 0이다: 읽기는 메모리의 표에서 하고 버퍼 풀은 쓰기(write-through)와 CTE만 지난다. 카운터가 거짓이라기보다 구조의 정직한 표시다.
- `DELIMITER`는 이 엔진이 모르고(앱은 `;`로 나눔) `DESC`·`SHOW COLUMNS` 같은 몇몇 문장은 MySQL 프로토콜에서만 된다(`SQL.md` 14장).

### 10월 26일 — R54: LIKE 이스케이프, `USE`가 redo 로그를 비우지 않게, 부모 행 DELETE의 자식 검사, 서버 지표 패널(Metrics), MCP 도구 다섯 개

**왜 이 항목인가**: R53 끝에 드린 추천표에서 사용자가 "LIKE의 `\%`·ESCAPE, 서버 지표 패널(추세 그래프), health_report, list_locks, get_table_stats, FK 관계 그래프, trace_query, USE가 redo 로그를 비움, 부모 행 DELETE의 자식 검사 O(자식 수)"를 골라 "같은 진행 방식으로" 하라고 했다(코드 먼저 → 항목마다 시험 → 문서 → 커밋·푸시). 성능 동결 중이라 "먼저 여쭙겠다"고 적어 둔 두 항목(`USE`, 부모 DELETE)도 이 선택으로 승인된 것이다.

**고친 것·만든 것 (엔진)**
- **`LIKE`의 이스케이프** (조용한 오답 + 없던 문법): 문자열 안의 `\%`·`\_`가 역슬래시를 잃어 와일드카드가 되었다(`'axb' LIKE 'a\_b'`가 1, MySQL은 0). 렉서가 두 글자를 그대로 남기고, 일치 검사(`collation::like`의 ASCII 길과 일반 길)가 `\x`를 글자 x 자체로, 끝의 역슬래시를 역슬래시로 읽는다. `LIKE … ESCAPE 'c'`가 새로 된다: 문자열 패턴은 파서가 한 번 다시 쓰고(`c`+x → `\x`, 이스케이프 아닌 역슬래시 → `\\`, 끝의 `c`는 자기 자신, `ESCAPE ''`는 이스케이프 없음), 열·식에서 읽은 패턴은 숨은 함수 `__LIKE_ESCAPE`가 행마다 같은 변환을 한다(`like_escape.hpp` 한 곳). 여러 글자 ESCAPE는 오류 1210. 접두 인덱스 길은 역슬래시가 든 접두를 인덱스에서 찾지 않는다. `SHOW … LIKE`(`SHOW TABLES`·`SHOW STATUS`, MySQL 프로토콜의 `SHOW VARIABLES`·`SHOW COLLATION` …)도 같은 일치 검사를 쓴다(서버에 따로 있던 ASCII 일치 함수를 없앰). 진짜 MySQL의 답으로 모든 기대값을 먼저 확인했다.
- **`USE`와 세션 변수만 바꾸는 `SET`이 redo 로그를 비우지 않음**: 이 문장들은 읽기 전용으로 분류되지 않아 구조 쓰기 락(실행 중인 모든 문장을 기다림)을 잡고, 쌓인 redo 로그를 체크포인트(테이블 파일 재작성)한 뒤 실행했다. R53에서 `INSERT` 뒤 `Innodb_redo_log_bytes` 84 → `USE` 뒤 0으로 드러났다. 앱과 MCP(`execute_sql(database=…)`)는 문장마다 `USE`를 먼저 보낸다. `USE`·`SET @x = …`(하위 질의 없는)·`SET autocommit`·`SET NAMES`·`SET … ISOLATION LEVEL`은 `BEGIN`·`SAVEPOINT`처럼 구조 읽기 락만 잡는다(하위 질의가 든 `SET`은 표를 읽으니 예전 길 그대로).
- **부모 행 DELETE·UPDATE가 자식 표를 훑지 않음**: 지우는 부모 값을 가진 자식 행을 찾으려고 자식 표 전체를 읽었다(참조되지 않은 부모여도, 자식 FK 열에 인덱스가 있어도 — 보조 인덱스는 한 값에 한 행만 담아 "없다"를 증명하지 못하므로). R20~22의 UNIQUE 값 캐시(`UniqueProbes`: 잠금 도장이 맞을 때만 쓰고, 쓰는 쪽이 아는 만큼만 갱신)가 이제 외래키 열의 값도 센다(`specs_of`에 FK 열). 새 `fk_children_absent`가 부모 DELETE·UPDATE의 RESTRICT 검사와 CASCADE/SET NULL의 자식 찾기에서 "이 값을 가진 살아 있는 자식이 없다"를 O(1)로 답한다. 캐시는 둘째 부모 문장이 만들고(행 1,024개 미만의 표는 훑는 쪽이 싸서 만들지 않음), 자식에 INSERT·UPDATE·DELETE·CASCADE가 일어나면 따라가고, ROLLBACK·DDL·VACUUM은 도장이 어긋나 다시 만든다. 부모 문장이 어떻게 끝나든(오류·재시도 포함) 자식 캐시를 현재로 되돌리는 RAII 보호자가 `try_cascade_once` 안에 있다. 정확성은 시험 전체를 `RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0`(캐시를 쓸 때마다 행을 다시 세어 비교, 다르면 예외)로도 돌려 확인.
- **NULL인 부모 키는 아무도 참조하지 않음** (조용한 오답, 위를 고치다 발견): UNIQUE 열은 NULL을 여럿 가질 수 있는데, 그 열을 가리키는 FK의 자식에 NULL이 있으면 부모의 NULL 행을 지울 때 "Foreign key violation"이 났다(NULL끼리 같다고 본 것; MySQL은 지움). RESTRICT 검사 네 곳과 자식 찾기가 NULL을 참조 없음으로 본다.

**앱·MCP**
- **Server Manager의 Metrics 패널**: 오른쪽 막대의 새 버튼. `SHOW GLOBAL STATUS`(새 Tauri 명령 `get_status_counters`)를 1초마다 읽어 최근 2분의 추세를 SVG 선 그래프로 그린다: 초당 문장 수(전체·읽기·쓰기; 패널 자신의 읽기 1건은 뺌), 세션(연결·실행 중), 행 락(지금 기다림·초당 대기·교착), redo 로그 크기(체크포인트에서 0으로 떨어지는 톱니), 버퍼 풀(페이지 사용·적중률). 읽은 값은 앱이 도는 동안 남아 패널을 닫았다 열어도 이어지고, 엔진이 다시 시작되면(Uptime이 줄어듦) 새로 시작하며, 5초 넘게 읽지 못한 구간은 선을 이어 그리지 않는다.
- **MCP 도구 다섯 개(27 → 32개)**: `list_locks`(열린 트랜잭션·잠근 행·기다리는 쌍·뿌리 차단자·교착, `KILL` 안내), `health_report`(연결·문장·버퍼 풀·redo·락·세션·마지막 시작의 복구·DB별 크기와 `findings`, 전체 상태 `ok`/`attention`), `get_table_stats`(행 수, 열별 distinct·NULL·선택도·min·max·사분위·덮는 인덱스; 엔진이 12자로 자르는 열 이름 처리), `get_fk_graph`(외래키 변과 ON DELETE/UPDATE 규칙, 바로 쓸 `JOIN` 조건, 두 표 사이의 가장 짧은 사슬과 `FROM … JOIN … ON …`), `trace_query`(`TRACE`의 단계표를 JSON으로; 쓰기 문장은 `BEGIN … ROLLBACK` 안에서, DDL은 거절). 읽기 전용 넷은 Claude Desktop 자동 설정의 `alwaysAllow`에 더함.
- 문서: `SQL.md`(LIKE ESCAPE 줄, 문자열의 `\%`), README, `FUNCTIONS.md`, `DIFF.md`, `SUMMARY.md`, `DEMO.md`(락 대기 장면의 Metrics 그래프, MCP 질문), Diagram 문구(도구 32개, Metrics), `RECOMMAND.md`.

**검증**
- 새 C++ 시험 `[r54]` 5 케이스(LIKE 이스케이프 — MySQL 8.4.9로 먼저 확인한 기대값, ASCII·비ASCII 길, 접두 인덱스 길, UPDATE·DELETE, 열 패턴·ESCAPE; `USE`·`SET`이 redo를 지키고 크래시 뒤에도 복구; 큰 자식 표에서 부모 DELETE·UPDATE의 모든 경로(RESTRICT·CASCADE·SET NULL, INSERT·UPDATE·DELETE·ROLLBACK·DDL 뒤, 캐시 건수·만든 횟수 단언 포함); NULL 부모 키; 부모 삭제와 자식 쓰기의 스레드 시험). **심은 버그 18종 중 17종이 잡힘**: 일치 검사의 이스케이프 무시(ASCII·일반 길), 렉서가 `\%` 역슬래시를 지움, ESCAPE 다시 쓰기 제거·끝의 이스케이프 문자가 사라짐·역슬래시를 두 번 쓰지 않음, 플래너가 역슬래시 접두를 인덱스에서 찾음, `USE`·`SET`이 다시 배타 길, 캐시가 절대 없다고 안 함·모두 없다고 함, NULL 먼저 처리 제거, FK 열을 세지 않음, DELETE·UPDATE가 캐시를 현재로 돌려놓지 않음, 캐시가 자식 찾기를 건너뜀, 트랜잭션이 끝낸 행을 살아 있다고 셈. **처음에 3종이 살아남아** 시험을 보강했다(비ASCII 이스케이프, UPDATE 뒤 캐시 재작성 횟수, CASCADE의 자식 찾기 건너뜀). 마지막 하나(하위 질의가 든 `SET`이 가벼운 길로 감)는 혼자 도는 시험으로는 보이지 않는다 — 다른 세션의 쓰기와 겹칠 때만 의미 있는 안전장치다. MCP: 가짜 엔진 단위 시험 20개, 심은 버그 10종 모두 잡힘.
- 새 코퍼스 `like_escape`(41문장)와 `fk_parent`(47문장, 1,000행 넘는 자식 표)를 진짜 MySQL 8.4.9와 비교해 **차이 0**(두 프로토콜 모두; R53 빌드는 각각 32문장·5문장이 달랐다). `--emit`으로 `mysql_conformance_like_escape.inc`·`mysql_conformance_fk_parent.inc`를 만들어 C++ 시험에 넣고 점수판에 더함.
- 실서버에서 A/B(R53 빌드 대 새 빌드, 같은 작업): 20,000행 자식 표에서 **참조되지 않은 부모 한 행 DELETE 중앙값 4.5~5.5 ms → 0.82~0.88 ms**(문장 하나의 바닥 비용), 자식 INSERT 사이사이에서도 같고, **`USE` + `INSERT` 한 쌍 4.9~5.6 ms → 0.84~0.91 ms**(6배). MySQL 프로토콜의 `SHOW VARIABLES LIKE 'sql\_mode'`·`SHOW STATUS LIKE 'Com\_sel%'`·`SELECT 'a%b' LIKE 'a!%b' ESCAPE '!'`도 확인.
- 무작위 대조 12종 위반 0. 크래시 퍼저 3종(`fuzz_crash` 15라운드, `fuzz_crash_fk` 15라운드 — FK 캐시와 자식 표 복구, `fuzz_concurrent` 8라운드) 불일치 0. 전체 시험 **806 케이스 / 1,507,268 assertions**(Release 기본), 1,507,269(`RUSQL_DML_INDEX_MIN_ROWS=0`), 1,507,268(`RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0`: 캐시를 쓸 때마다 행을 다시 세어 비교)을 모두 통과. 호환성 점수판: 코퍼스 30개, 문장 2,847개 중 차이 8개(앞과 같은 8개이고 새 코퍼스 둘은 0).
- 앱(`npm run tauri dev` + WebView2 디버그 포트, 페이지 안에서 DOM으로 조작 — 마우스·키보드는 쓰지 않음; 임시 연결과 임시 데이터 폴더를 쓰고 `connections.json`·`app_instances.json`·`ui_state.json`·`ui_commands.json`을 끝나고 원래대로 되돌려 바이트 단위로 확인): Metrics 패널을 엔진에 일부러 만든 부하(삽입 반복·4초짜리 락 대기·삭제)로 보니 추세 선이 모양대로 그려지고(락 대기 때 빨간 선이 1로 솟았다 내려오고 redo가 KB 단위로 늘어남) 타일 이름이 잘려서 2열로 넘기게 고쳤다(Vite 핫 리로드로 바로 확인). `EXPLAIN ANALYZE`의 단계 막대에서는 "2. Join r54app.lk (inner, hash join)"처럼 이름이 다 보이고 막대가 줄 맞춰 있다(R53의 격자 CSS 확인, 가장 느린 단계가 노란색).
- **하지 않은 것**: Debug 빌드, 앱의 다른 화면 점검, `SET`에 하위 질의가 든 경우의 동시 실행 시험.

**조사하다 알게 된 것 (고치지 않음)**
- **자기 자신을 가리키는 외래키(`emp.mgr → emp.id`)의 부모 쪽은 검사되지 않는다**: `DELETE FROM emp WHERE id = 1`이 `id = 2`가 가리키는 행을 오류 없이 지운다(MySQL은 1451). `ON DELETE CASCADE`·`SET NULL`도 적용되지 않아 매달린 행이 남는다. 부모 문장이 "다른 표"만 자식으로 세기 때문(`other_tables`가 자기 표를 뺌). 이전부터 있던 무결성 구멍(R53 빌드도 같음). `RECOMMAND.md` 맨 위 항목.
- **읽기만 하는 다른 문장도 redo 로그를 비운다**: `WITH`(CTE) SELECT(임시 표를 `s.tables`에 만드는 쓰기 길), `PREPARE`·`EXECUTE`·`DEALLOCATE`, `LOCK`·`UNLOCK TABLES`, `ANALYZE TABLE`, 하위 질의가 든 `SET`, `CALL`. 문장마다 `USE`를 보내던 것만큼 흔하지는 않아 이번에는 `USE`·`SET`만 했다.
- `DELETE t1 FROM t1 JOIN …`·`MERGE`의 부모 검사와 참조되는 부모의 CASCADE가 자식을 **찾는** 일은 아직 자식 표를 훑는다(참조되지 않은 부모만 O(1)).

### 10월 26일 — R55: 자기 자신을 가리키는 외래키의 부모 쪽, 읽기 문장이 redo 로그를 비우지 않게

**왜 이 항목인가**: R54를 끝내며 "조사하다 알게 된 것"으로 적어 둔 두 가지를 사용자가 그대로 "자기 자신을 가리키는 외래키의 부모 쪽, CTE 등 읽기 문장이 redo 로그를 비움 시작"이라고 골랐다(같은 진행 방식: 코드 먼저 → 항목마다 시험 → 문서 → 커밋·푸시).

**고친 것 (엔진)**
- **자기 표를 가리키는 외래키(`emp.mgr → emp.id`)의 부모 쪽 DELETE** (무결성 구멍, 전부터): `DELETE FROM emp WHERE id = 1`이 `id = 2`가 가리키는 행을 오류 없이 지웠고(MySQL은 1451) `ON DELETE CASCADE`·`SET NULL`도 적용되지 않아 매달린 행이 남았다. 부모 문장이 "다른 표"만 자식으로 셌기 때문(`other_tables`가 자기 표를 뺌). 먼저 진짜 MySQL 8.4.9로 동작을 재 보았다(`selfref_probe*.py`): InnoDB는 행을 **하나씩, 스캔이 읽는 순서로**(키 순서, `ORDER BY`가 있으면 그 순서) 처리한다 — RESTRICT는 아직 남아 있는 자식이 있으면 문장 전체를 거절하고(`ORDER BY id DESC`로 자식부터 지우면 통과), 자기 자신을 부모로 가진 행은 자기 자식이라 거절되고, CASCADE는 자식의 자식까지 되풀이하되 **14단계까지**(15번째 단계는 오류 3008), SET NULL은 자식을 NULL로 바꾸고, 앞선 행의 연쇄로 이미 지워진 행은 스캔이 도달하지 않아 **세지 않으며**(`DELETE … WHERE id IN (1, 2)`가 CASCADE에서 1행), 앞선 행의 SET NULL로 WHERE에서 빠진 행은 지워지지 않는다(`DELETE … WHERE mgr IS NOT NULL`). 연쇄로 지워지는 행이 다른 표의 자식을 가지면 그 표의 외래키도 묻는다(RESTRICT면 문장 전체 거절, CASCADE면 따라 지움).
  새 `plan_self_delete`(`executor_self_reference.cpp`)가 이 일을 **아무것도 쓰기 전에 메모리의 행으로** 한다: 자기 표의 외래키 열마다 값→위치 표를 만들고, 문장이 이름 붙인 행을 InnoDB의 순서로 하나씩 꺼내면서 RESTRICT 검사·CASCADE 재귀(깊이 상한)·SET NULL/SET DEFAULT 다시 쓰기를 모의한다. 결과는 지울 행의 상(다른 표의 외래키가 읽음), 연쇄로 지울 위치, 다시 쓸 행. `exec_delete_inner`가 이를 받아 같은 시도 안에서(자기 표 데이터 락 배타) ① 연쇄로 지울 행과 다시 쓸 행을 다른 자식 표의 행처럼 락 매니저에 클레임하고 ② 다른 표의 RESTRICT·클레임·CASCADE를 이 행들까지 넓혀 묻고 ③ 다시 쓸 행을 먼저, 지울 행을 그 뒤에 `cascade_write`로 쓴다(트랜잭션·undo·redo·인덱스가 다른 연쇄와 같음) ④ 끝나면 지울 행을 다시 읽어(`collect_targets`) 문장의 본 삭제가 연쇄 뒤의 표를 보게 한다. 자식이 아무도 없는 행(대부분)은 R54의 외래키 값 캐시(`fk_children_absent`)가 O(1)로 답하고 모의 자체를 건너뛴다. `DELETE … ORDER BY`의 순서는 `delete_row_order_`(UPDATE의 `update_row_order_`와 같은 방식)로 전달한다.
- **`ON UPDATE CASCADE`·`SET NULL`이 자기 표의 키에서는 RESTRICT처럼** (MySQL 문서: "자기 표로 되돌아오는 연쇄 UPDATE는 RESTRICT처럼 동작"): 자식이 있는 키를 바꾸면 1451, 자식이 없는 키는 바뀐다. 전에는 CASCADE·SET NULL이 아무 일도 하지 않아 키만 바뀌고 자식이 매달렸다(`update_restrict_violation`이 자기 표일 때 RESTRICT가 아닌 동작도 거절로 읽음).
- **읽기만 하는 문장이 redo 로그를 비우지 않음**: 구조 쓰기 락 길은 "쓰기 전용"이라 안에서 쌓인 redo 로그를 체크포인트(테이블 파일 재작성)한 뒤 실행했다. 이 길로 가는 이유가 둘로 나뉘는데(임시 표를 `s.tables`에 만들어야 해서 락이 필요한 것과, 디스크의 것을 바꿔서 체크포인트가 필요한 것) 둘째만 체크포인트가 필요하다. 새 `may_change_stored_data`(`executor_proc.cpp`)가 "표 파일·카탈로그를 redo 밖에서 바꿀 수 있나"를 가린다: `WITH`(CTE, 재귀 포함)·파생 표가 든 SELECT·하위 질의가 든 `SET`·`ANALYZE TABLE`(통계는 메모리에만)·`LOCK`/`UNLOCK TABLES`·`PREPARE`·`DEALLOCATE`는 바꾸지 않고, **본문이 읽기뿐인 `CALL`**은 프로시저 본문을 따라가 본다(제어문·`SELECT … INTO`·중첩 `CALL`, 깊이 8, 모르는 것은 "바꿈"). `PREPARE`·`DEALLOCATE`·`LOCK`·`UNLOCK TABLES`는 `USE`처럼 구조 읽기 락만 잡는다(세션에 글자나 락 목록을 둘 뿐). **`EXECUTE`는 준비된 문장을 그 문장을 직접 친 것처럼** 실행한다(`prepared_statement`로 `?`를 채워 파싱하고 `execute()`로 다시 들어감): 준비된 `INSERT`가 이제 행 락·redo 배치를 보통 INSERT처럼 쓰고, 준비된 `SELECT`가 락 없이 읽는다(전에는 어떤 문장이든 배타 길). 체크포인트를 계속 하는 것: DDL, `VACUUM`·`OPTIMIZE`, **쓰는 프로시저의 `CALL`**(본문의 쓰기가 redo로 덮이는지 따로 확인이 필요해 이번에는 그대로 둠).
- **캐시를 유지하는 일을 빠뜨려 생긴 느려짐을 A/B로 찾아 고침**: 자기 표의 값 캐시를 쓰게 되자 4만 행 표에서 잎 행 DELETE가 0.8 → 14.5 ms로 느려졌다. 물리 삭제 길(`globally_quiescent`)이 지운 행을 캐시에 알리지 않아 문장마다 캐시가 어긋나 다시 세었기 때문이다(전에는 자기 표의 캐시를 이 길에서 쓸 일이 없어 드러나지 않았다). 그 구역에서 실제로 지워진 행만 캐시에 알리고(`erased`), 뒷부분에서 도장만 새로 찍는다 → 0.80~0.82 ms로 돌아옴.

**조사하다 알게 된 것 (고치지 않음)**
- **테이블을 건너는 연쇄가 한 단계에서 멈춘다**(전부터): `a ← b ← c`(둘 다 ON DELETE CASCADE)에서 `DELETE FROM a …`가 `b`의 행은 지우지만 `c`의 행은 지우지 않아 **매달린 행**이 남고, `c ← d`가 RESTRICT면 MySQL은 1451인데 여기는 통과한다. 자기 표 연쇄는 이번에 같은 모의로 풀었지만 서로 다른 표를 잇는 사슬은 아직 한 단계. `RECOMMAND.md` 맨 위.
- **0행 SELECT가 열 이름을 잃는다**: 엔진이 결과가 없는 `SELECT`에 열 머리줄 없이 `0 rows returned.`만 돌려줘서(네이티브 프로토콜도 같음) MySQL 프로토콜에서는 열 정의 없는 OK 패킷이 되고 `cursor.description`이 None이다(MySQL은 열 이름이 든 빈 결과). 드라이버·도구가 빈 결과의 열 이름을 못 얻는다. `SELECT 1 WHERE 1 = 0`(FROM 없는 WHERE)도 문법 오류.
- 자기 표 UPDATE는 행 단위가 아니라 문장 전체를 한 번에 본다: `UPDATE emp SET mgr = NULL, id = id + 10 ORDER BY id DESC`를 MySQL은 통과시키는데(자식을 먼저 비우므로) 여기는 거절. REPLACE·MERGE의 선검사(`delete_restrict_violation`)와 다중 표 DELETE는 자기 표를 보지 않아, 여러 행 REPLACE의 둘째 행이 거절되면 첫째 행은 이미 바뀐 채 남을 수 있다.
- 연쇄 삭제 한 번이 표 크기에 비례한다(4만 행 표에서 부모 하나 + 자식 하나 52~65 ms: 값→위치 표와 연쇄 쓰기의 압축이 O(n)). 잎 행 DELETE는 O(1).

**검증**
- 새 C++ 시험 `[r55]` 9 케이스(RESTRICT의 행 단위·키 순서·ORDER BY·자기 부모, CASCADE/SET NULL의 세는 행 수·Halloween·텍스트 키·ROLLBACK·SAVEPOINT·재시작, 14단계, 다른 표로 이어지는 연쇄, 두 외래키, ON UPDATE, 부모 삭제와 자식 쓰기의 스레드, 다른 트랜잭션이 쥔 자식 행을 기다림, redo 로그가 그대로인 문장 열두 가지와 `EXECUTE`·크래시 복구) + 새 코퍼스 `self_fk`(79문장)를 진짜 MySQL 8.4.9와 비교해 **차이 0**(R53 빌드는 처음 76문장 중 32문장이 달랐다; `--emit`으로 `mysql_conformance_self_fk.inc`를 만들어 C++ 시험에 넣고 점수판에 더함).
- **심은 버그 30종 중 26종이 잡히고 4종은 잡을 수 없는 것**: 처음에 7종이 살아남았다. 3종은 시험의 구멍이라 보강했다(키 순서 대신 쓴 순서로 읽기 → 입력 순서가 키 순서와 다른 표 시험, 연쇄 뒤 지울 행을 다시 읽지 않음 → 남은 행이 기본키로 찾아지는지, 연쇄로 지울 행을 클레임하지 않음 → 다른 트랜잭션이 쥔 행을 기다리는 스레드 시험). 4종은 결과가 같은 변이다: 이미 연쇄로 지워진 행을 스캔이 다시 처리해도 같은 결과(최적화), 지울 행에도 SET NULL을 쓰는 것(헛수고일 뿐), `LOCK TABLES`와 `PREPARE`는 두 군데에서 같은 일을 하도록 막아 한쪽을 빼도 같다(락 모드만 다름).
- 크래시 퍼저 4종: `fuzz_crash` 15, `fuzz_crash_fk` 15, `fuzz_concurrent` 8, 새 **`fuzz_crash_selfref`** 15라운드(emp 한 표, ON DELETE 동작 3가지, 한 행·여러 행 삭제의 오름차순·`ORDER BY id DESC`, UPDATE, 트랜잭션·세이브포인트, 연쇄·거절 모델 대 복구 뒤 표; 복구 뒤에도 매달린 행이 거절되는지) 불일치 0. 무작위 대조 12종 위반 0.
- 전체 시험 **815 케이스 / 1,507,675 assertions**(Release 기본), 1,507,675(`RUSQL_DML_INDEX_MIN_ROWS=0`), 1,507,674(`RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0`: 캐시를 쓸 때마다 행을 다시 세어 비교) 통과. 호환성 점수판: 코퍼스 31개, 문장 2,926개 중 차이 8개(앞과 같은 8개이고 새 코퍼스는 0).
- 실서버 A/B(R53 빌드 대 새 빌드, 같은 작업, 2회): 2만 행 표에서 INSERT 직후의 **`WITH` SELECT 13.3 → 0.23 ms(58배), 파생 표 SELECT 13.6 → 0.23 ms**(일반 SELECT는 0.2 ms로 그대로), 자기 표(4만 행)의 잎 행 DELETE 0.80 ms → 0.80 ms(고친 뒤; 고치기 전 14.5 ms).
- **하지 않은 것**: Debug 빌드, 앱 화면(이번에 UI 변경은 Diagram 문구뿐), 다중 표 DELETE·MERGE·REPLACE의 자기 표 검사.

### 10월 26일 — R56: 표를 건너는 외래키 연쇄 끝까지(DELETE·키 UPDATE), 행이 없는 SELECT도 열 이름을 말함

**왜 이 항목인가**: R55 끝에 드린 추천표에서 사용자가 "표를 건너는 연쇄가 한 단계에서 멈춤 (a ← b ← c), 0행 SELECT가 열 이름을 잃음"을 골라 "같은 진행 방식으로" 하라고 했다(코드 먼저 → 항목마다 시험 → 문서 → 커밋·푸시).

**고친 것 (엔진)**
- **외래키 연쇄가 한 단계에서 멈춤** (무결성 구멍, 전부터): `a ← b ← c`(둘 다 `ON DELETE CASCADE`)에서 `DELETE FROM a …`가 `b`의 행은 지웠지만 `c`의 행은 그대로 둬 매달린 행이 남았고, `c`가 RESTRICT면 MySQL은 1451인데 여기는 통과했다. `ON UPDATE CASCADE`도 같아서 `a`의 키를 바꾸면 `b.p`는 따라 바뀌지만 그 열을 가리키는 `c.q`는 옛 값에 남았다. 먼저 진짜 MySQL 8.4.9로 열두 가지 모양을 재 보았다(`chain_probe2.py`): 연쇄는 사슬 끝까지 가고, 아래쪽 RESTRICT는 문장 전체를 거절하고 아무것도 바꾸지 않고, 표는 이름 순서로 묻고(두 부모를 가진 표가 하나는 CASCADE 하나는 RESTRICT면 어느 순서로 만들었든 거절 — 이름 순서에서 `b`가 `c`보다 앞), 두 표를 오가는 순환은 끝나고, 부모의 연쇄 SET NULL이 어느 열을 NULL로 바꿨는데 그 열을 다른 표가 가리키면 그 표가 `ON UPDATE`를 따른다.
  R55의 `plan_self_delete`를 표 전체로 넓힌 **`plan_cascade_delete`**(`executor_cascade_plan.cpp`; 이름이 바뀜): 쓰기 전에 메모리의 행으로 모든 표를 모의한다. 표마다 "이 표를 가리키는 외래키 변" 목록(자식 표 이름·열 순서), 변마다 "이 값을 가진 살아 있는 자식"(R54의 외래키 값 캐시가 "없다"를 O(1)로 말하고, 아니면 훑되 한 문장에서 같은 열을 여섯 번 넘게 찾으면 그 열을 한 번 색인해서 이어 감), `delete_row`(RESTRICT 거절·CASCADE 되풀이·깊이 상한 14), `change_key`/`rewrite_column`(SET NULL·SET DEFAULT·ON UPDATE CASCADE로 어떤 열이 새 값을 가지면, 그 열을 가리키는 것이 옛 값을 따라감; 이미 바꾸고 있는 표로 되돌아오는 연쇄는 RESTRICT처럼 — MySQL 설명서의 규칙). `exec_delete_inner`는 이 계획을 받아 같은 시도 안에서 ① 연쇄로 지울·다시 쓸 모든 표의 행을 클레임하고 ② 지워지는 모든 행의 "누가 가리키는 값"을 클레임하고(전에는 문장이 이름 붙인 표만) ③ 다시 쓸 행을 먼저, 지울 행을 그 뒤에 쓴다. 한 단계짜리 반복문(RESTRICT 검사, `collect_children`, `other_tables`)은 없앴다. 락은 새 `fk_children_closure`(자식의 자식까지, 자기 표로 되돌아오면 자기 표도)로 잡는다(`table_lock_set_for`).
- **키가 바뀌는 UPDATE의 연쇄** (같은 구멍): 새 `plan_cascade_update`가 같은 모의로 사슬을 따라간다. UPDATE는 자기 행을 먼저 쓴 다음 연쇄를 쓰는 구조라, 아래쪽 RESTRICT를 쓰기 전에 알아야 문장이 원자적이다: 바꾸는 열을 가리키는 자식 열이 다시 다른 표에 가리켜질 때만(`chain_possible`) 행을 쓰기 전에 미리 한 번 돌려 본다(자식 한 단계는 전과 같이 `update_restrict_violation`이 본다). 연쇄는 행을 쓴 뒤 그 문장이 쓴 옛·새 행의 이미지(JSON)로 다시 계산해 쓴다 — 전에는 새 값을 식으로 다시 계산했는데(`eval_arith`), 이제 실제로 저장된 값을 쓴다. `fk_restrict_children`은 쓸 데가 없어져 없앴다.
- **행이 없는 SELECT도 열 이름을 말함**: 엔진이 결과가 없는 SELECT에 열 머리줄 없이 `0 rows returned.`만 돌려줘서 MySQL 프로토콜에서는 열 정의 없는 OK 패킷이 되었고 `cursor.description`이 None이었다(pandas `read_sql`, DBeaver, JDBC 메타데이터가 빈 결과의 열 이름을 못 얻음). 이제 `execute_sql`의 SELECT·UNION·INTERSECT·EXCEPT·WITH(그리고 `EXECUTE`한 SELECT)가 행이 없으면 머리줄만 있는 표와 `0 rows returned.`를 돌려준다(`result_text::empty_table`): 앱은 머리글이 있는 빈 표를 그리고, MySQL 프로토콜은 타입이 붙은 열을 가진 빈 결과 집합을 보낸다. 열 이름은 문장에서 알아낸다: R38의 `Resolver`(열 타입을 가려내던 것)가 select 항목마다 SELECT가 찍는 머리글을 함께 알려 주고(`Executor::column_header`: 열은 이름, 별칭, 집계의 라벨, 별칭 없는 식은 `Parser::arith_to_string` — 행이 있는 결과가 찍는 것과 같은 규칙), `*`는 표·뷰·파생 표의 열을 펼치고, **WITH 목록**(각 CTE는 뒤의 표가 됨; 열 목록 `c (a, b)`도)을 읽는다. 알아낼 수 없는 것(USING·NATURAL 조인 등)은 전과 같이 `0 rows returned.`만. 파생 표·`CREATE TABLE … AS SELECT`는 이제 빈 안쪽 질의의 열도 머리줄에서 얻는다. 부수 효과로 **WITH 질의의 열 타입**이 MySQL 프로토콜에 실리게 되었다(전엔 문자열). MySQL 계층의 빈 결과 판별(`SHOW COLUMNS`·`SHOW INDEX`·`SHOW TABLE STATUS`)은 새 `no_rows`가 머리줄이 붙은 모양도 읽는다.
- **`SHOW TABLES LIKE '패턴'`의 열 이름** (새 비교로 찾음): MySQL은 `Tables_in_db (패턴)`, 여기는 `Tables_in_db`였다.

**조사하다 알게 된 것 (고치지 않음)**
- **연쇄가 표를 건너 쓰는 행은 트리거를 부르지 않는다**(MySQL도 그렇다).
- `SELECT 'a'`의 머리글이 MySQL은 `a`, 여기는 `'a'`이다(행이 있는 결과도 같은 전부터의 차이). `SELECT 1 WHERE 1 = 0`(FROM 없는 WHERE)은 아직 문법 오류.
- USING·NATURAL 조인과 `CALL`이 돌려주는 빈 SELECT는 여전히 열 이름이 없다. `SHOW …`의 빈 결과 일부도.
- 연쇄를 쓰는 비용: 4만 행 표에서 부모 하나(자식 하나)를 지우는 것이 R55의 52~65 ms에서 R56의 21~22 ms로 내려갔다(값→위치 표를 표 전체로 만들던 것을 없앴다). 2만 행 자식 표에서 부모 하나와 자식 하나를 지우는 것은 18~20 ms로 R53과 같다(자식 표의 행 이동이 O(n)).
- 자기 표 UPDATE가 행 단위가 아닌 것, 다중 표 DELETE·MERGE·REPLACE의 사전 검사가 자기 표·사슬을 보지 않는 것(R55에 적은 것)은 그대로.
- MySQL 프로토콜의 오류 번호는 여전히 전부 1064다(이번 비교에서 사슬의 거절이 MySQL은 1451인데 번호만 다름).

**검증**
- 새 C++ 시험 `[r56]` 7 케이스: 사슬의 모든 모양(MySQL이 말한 상태로: CASCADE 끝까지, 아래쪽 RESTRICT 거절·상태 그대로, SET NULL이 위·아래, 네 단계, 두 자식 표, 다이아몬드, 표 전체 삭제, 순환, 가리켜지는 열의 SET NULL), 키 UPDATE의 사슬과 아래쪽 RESTRICT, ROLLBACK·SAVEPOINT·재시작, 다른 트랜잭션이 쥔 손자 행을 기다림, 스무 부모를 한 문장으로(색인 길)·글자 키의 대소문자, 삭제하며 손자를 쓰는 스레드·키를 바꾸며 손자를 쓰는 스레드, 빈 결과의 열 이름(서른 가지 select 항목·FROM 모양이 행이 있는 결과의 머리글과 같은지, EXECUTE, SHOW TABLES LIKE) + 새 코퍼스 `fk_chain`(62문장)과 `empty_columns`(40문장)를 진짜 MySQL 8.4.9와 비교해 **차이 0**(두 프로토콜 모두; R53 빌드는 각각 21문장·39문장이 달랐다), `protocol` 코퍼스에 빈 결과 4문장을 더해(107문장, 차이는 앞과 같은 3개). `vs_mysql.py`가 빈 결과의 열 이름을 비교하도록 넓혔다(이 비교가 `SHOW TABLES LIKE`와 `EXECUTE`의 빈 결과를 찾아냈다). 빈 결과가 `== "0 rows returned."`이던 시험 6곳은 새 모양으로 고쳤다.
- **심은 버그 29종 중 26종이 잡힘**: 처음에 7종이 살아남았다. 4종은 시험의 구멍이라 보강했다: 이미 바꾸고 있는 표로 되돌아오는 `ON UPDATE` 연쇄가 RESTRICT처럼 동작하는지(MySQL이 1451을 말하는 모양을 찾아 시험에 더함), 한 문장이 일곱 부모 넘게 지울 때 쓰는 자식 색인이 글자 키의 대소문자를 따르는지(부모와 자식의 글자 모양을 서로 어긋나게), 아래 표의 지워지는 행·바뀌는 키의 값을 클레임하는지(결정적 시험: 부모를 막 찾아본 세션이 쥔 클레임을 락 매니저에 직접 잡아 두고 문장이 기다리는지 본다). 3종은 시험이 잡을 수 없는 것이다: 지울 행을 처음 읽은 위치로 찾지 못하게 함(찾는 길이 둘 더 있어 결과는 같고 느려질 뿐), 자식을 처음 값이 아니라 지금 값으로 읽음(한 문장에서 같은 값을 두 번 찾는 일이 없어 같은 결과), 사슬 아래 표들을 미리 보는 동안 읽기 락을 잡지 않음(혼자 도는 시험으로는 보이지 않는 동시 실행 안전장치).
- 크래시 퍼저 5종: `fuzz_crash` 15, `fuzz_crash_fk` 15, `fuzz_concurrent` 8, `fuzz_crash_selfref` 15, 새 **`fuzz_crash_chain`** 24라운드(a ← b ← c, 두 외래키의 ON DELETE 동작 9가지, 한 행·여러 행 삭제의 오름차순, 거절과 연쇄 모델 대 복구 뒤 표; 복구 뒤에도 매달린 행이 거절되는지) 불일치 0. 무작위 대조 12종 위반 0. R53 빌드와의 차분 0.
- 전체 시험 **825 케이스 / 1,508,461 assertions**(Release 기본), 1,508,461(`RUSQL_DML_INDEX_MIN_ROWS=0`), 1,508,461(`RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0`) 통과. 호환성 점수판: 코퍼스 33개, 문장 3,032개 중 차이 8개(앞과 같은 8개이고 새 코퍼스는 0).
- 실서버 A/B(R53 빌드 대 새 빌드, 2만 행 자식 표, 같은 작업, 2회): 키가 바뀌는 UPDATE(자식 한 행이 따라감) 1.6 → 1.3 ms, 자식 없는 부모 DELETE 1.28 → 0.66 ms(R54의 캐시), 키 UPDATE(자식 없음) 1.6 → 0.64 ms, 자식 있는 부모 DELETE와 부모 표의 다른 열 UPDATE는 그대로(약 18 ms, 0.64 ms), 행이 없는 점 조회 0.10 → 0.11 ms(머리줄을 만드는 값).
- **하지 않은 것**: Debug 빌드, 앱 화면에서 빈 결과를 본 것은 아니다(데스크톱 클라이언트와 같은 순서로 답을 읽는 해석을 실서버의 빈 답에 돌려 열 이름만 있는 빈 표로 읽힘을 확인했고, 앱은 열이 있으면 머리글이 있는 표와 `0 row(s)`를 그린다 — 화면을 열어 본 것은 아님), 연쇄를 쓰는 중 크래시(문장 중간에 죽이기).

### 10월 28일 — R57: 조용한 오답 여섯 가지(REPLACE의 행 유실, FLOAT 값, 한 열의 외래키 둘, 빈 결과의 열 이름, 문자열 상수의 머리글, FROM 없는 WHERE)

**왜 이 항목인가**: 10-26의 점검(`RECOMMAND.md`의 "1. 정확성")에서 사용자가 여덟 개 중 여섯 개를 골라 "이 오답들부터 해결 시작"이라고 했다. 진행 방식도 정해 주었다: ① 코드 작성에만 집중 → ② 전부 끝나면 시험, 통과하면 문서(`docs/`·`README.md`) → 커밋·푸시 → ③ 추천표 다시 쓰기와 화면 띄우기.

**고친 것 (엔진)**
- **REPLACE가 행을 잃음** (데이터 유실, 전부터): 사슬 `t1 ← t2(ON DELETE CASCADE) ← t3(RESTRICT)`에서 `REPLACE INTO t1 VALUES (2), (1)`은 두 번째 행(1)이 사슬 아래에서 막혀 오류로 끝나는데, 첫 행(2)의 "지우고 다시 넣기"는 이미 지우는 쪽만 적용되어 `t1`에서 행 2가 사라졌다(MySQL은 둘 다 그대로). 원인은 `exec_insert`의 사전 검사가 자식 한 단계의 RESTRICT만 보는 `delete_restrict_violation`이어서 사슬 아래의 보호를 몰랐던 것이고, 실제 삭제는 행마다 DELETE 문장(`replace_delete_conflicts`)이라 앞 행의 삭제는 되돌릴 길이 없었다. 이제 R56의 InnoDB 행 단위 모의 `plan_cascade_delete`로 **지워질 행 전부를 쓰기 전에** 사슬 끝까지 모의한다(아래쪽 RESTRICT, 자기 표를 가리키는 키, 깊이 상한까지 DELETE와 같은 판정). 아래 표들을 읽는 동안의 읽기 락은 `fk_children_closure`(자기 표 제외)로 잡는다. 같은 구멍이 MERGE에도 있었다(UPDATE와 DELETE가 한 문장에 든 MERGE는 UPDATE를 먼저 쓴 뒤 사슬 아래에서 DELETE가 거절될 수 있음): 같은 사전 검사로 바꿨다. 한 문장으로 모은 DELETE만 있는 MERGE·`DELETE … JOIN`·`DELETE … IN (…)`은 전부터 원자적이었다.
- **FLOAT 값** (조용한 오답, 전부터): FLOAT 열은 단정밀도 수를 들고 있고 MySQL은 6자리로 보여 주는데(16777217은 16777200), 열을 그대로 읽을 때만 그렇고 집계·함수가 고르면 저장된 글자가 나왔다. 진짜 MySQL 8.4.9로 먼저 모양을 쟀다(`float_probe_mysql.py`): ① `MIN`/`MAX`는 FLOAT라서 6자리로 보인다. ② `COALESCE`·`IFNULL`·`NULLIF`·`GREATEST`·`LEAST`는 인자가 **FLOAT 열, 정수 상수, NULL**뿐이면 FLOAT(6자리)이고, 정수 *열*이나 DOUBLE·DECIMAL·소수·문자가 섞이면 DOUBLE이다(`GREATEST(f, 0)`은 1234570, `GREATEST(f, i)`는 1234567). ③ FLOAT를 FLOAT가 아닌 열(DOUBLE·DECIMAL·정수·**글자**)로 복사하면 6자리가 아니라 **넓힌 값**이 들어간다(0.1은 0.10000000149011612, 16777217은 16777216; VARCHAR에도 같은 글자). 구현: 바인더가 집계 인자가 FLOAT 열 하나인지를 `Agg`·`AggAlias`의 `arg_single`로 적어 두고(`MIN`·`MAX`만), 새 `Executor::single_result`가 식이 FLOAT인지 판정한다(FLOAT 열·정수 상수·NULL과 위 다섯 함수의 겹침). 6자리 표시는 **출력 단계에서만** 한다(조건 없는 집계의 출력, 일반 출력의 `single_value`): 식 안(`GREATEST(f, 0) + 1`)·비교·HAVING은 전과 같이 수 전체를 쓴다. 복사는 `sqlval::float_as_double_text`(FLOAT를 DOUBLE로 넓힌 글자): `UPDATE … SET d = f`(`single_result`인 식도)는 대입 루프에서, `INSERT … SELECT`는 안쪽 SELECT가 FLOAT 열을 6자리로 줄이기 전의 저장된 값을 내도록(`float_exact_`) 한 뒤 출처가 FLOAT인 열(`Resolver`의 열 타입 + 집계·식 항목)을 FLOAT가 아닌 대상 열의 위치에서 넓힌다(FLOAT 대상은 그대로).
- **한 열에 외래키 둘** (조용한 오답, 전부터): `CREATE TABLE … FOREIGN KEY (x) REFERENCES a(id), FOREIGN KEY (x) REFERENCES b(id)`와 `ALTER TABLE … ADD FOREIGN KEY`가 열의 `foreign_key`(하나뿐인 자리)를 덮어써서 앞의 키가 말없이 사라졌다. `ColumnDef::foreign_key`(optional)를 **`foreign_keys`(벡터)**로 바꿨다. 컴파일러가 약 80곳을 모두 짚어 주었고, 모두 키마다 따로 본다: 자식 값 검사(INSERT·UPDATE·MERGE: 값은 **모든** 부모에 있어야 하고 어느 부모에 없는지 메시지에 나옴), 부모 값 클레임, 부모 쪽 RESTRICT·연쇄 모의의 변(`CascadeSim`: 키마다 한 변, 부모가 다른 표여도 각자 CASCADE·SET NULL), `fk_children_closure`·잠금 이웃, `DROP TABLE`의 참조 검사, ALTER의 이름 중복·`DROP FOREIGN KEY 이름`(그 키만 지움), SHOW CREATE TABLE·백업·information_schema(`key_column_usage`·`referential_constraints`·`table_constraints`)에 키마다 한 줄, 이름 정규화. 연쇄가 자식 열에 쓰는 값(`ON UPDATE CASCADE`·`SET DEFAULT`)은 그 열의 **다른 외래키의 부모에도 있어야** 한다: 진짜 MySQL로 보니 `UPDATE pa SET id = 7`이 열 `x`를 7로 따라 바꾸는데 다른 부모 `pb`에 7이 없으면 1452로 문장 전체를 거절했다(키가 하나뿐이던 때는 닿지 않던 구멍이라 이 변경으로 새로 생기는 것을 같이 막았다): `CascadeSim::other_parent_missing`(`fk_child_violation`으로 확인). UPDATE는 이 거절도 **행을 쓰기 전에** 알아야 문장이 원자적이라, 쓰기 전 모의를 하는 조건(`chain_possible`)에 "자식 열에 키가 여럿"을 넣었다(처음에는 거절이 행을 쓴 뒤에 나서 부모만 바뀐 채 남았다). 자동 이름 `<표>_ibfk_<n>`은 열 순서, 같은 열 안에서는 만든 순서로 센다. **카탈로그 JSON은 옛 모양 그대로**: 첫 키는 `"foreign_key"`(없으면 null), 둘째부터는 `"more_foreign_keys"` 배열에만 쓴다(열에 키가 하나 이하이면 파일이 전과 같다; 옛 파일은 그대로 읽힘).
- **빈 결과의 열 이름** (R56의 남은 모양): `USING`·`NATURAL` 조인의 `*`와 `CALL`이 돌려주는 빈 SELECT. `Resolver`가 USING·NATURAL 조인의 합쳐진 열을 `resolve_join_columns`(행이 있는 결과)와 같은 규칙으로 전개한다: 합쳐진 열을 앞에 한 번, 이어서 표마다 남은 열. 이 비교로 알게 된 것: **`a RIGHT JOIN b USING (c)`의 `*`는 MySQL이 "합쳐진 열 → b의 나머지 → a의 나머지" 순서로 내는데** 이 엔진은 행이 있는 결과에서도 왼쪽부터 놓고 있었다(전부터의 차이, 값이 위치로 읽히는 클라이언트에는 오답). 두 곳(`resolve_join_columns`, `Resolver`)을 함께 고쳤다(LEFT·INNER·FULL은 그대로). USING 조인의 열 타입도 이제 알려져서 MySQL 프로토콜의 타입이 붙는다. `CALL`: 프로시저 본문의 SELECT가 행이 없으면 그 열 이름을 가진 빈 표가 CALL의 답이 된다(`exec_proc_stmts`; 아직 결과 표가 없을 때만 — 먼저 행이 있는 SELECT가 있으면 그 표가 답으로 남는다. 본문이 락을 쥔 채 도므로 `result_column_names_in`/`result_columns_in`을 새로 만들어 락을 다시 잡지 않는다). 바뀐 점: 빈 SELECT 다음에 쓰기 문장이 이어지는 프로시저는 전에는 마지막 문장의 메시지를, 이제는 그 빈 표를 답한다(MySQL 클라이언트가 읽는 첫 결과 집합과 같다).
- **`UNION`의 왼쪽이 0행이면 오른쪽 값이 사라짐** (새 비교로 찾음, R53 빌드에도 있던 조용한 오답): `SELECT id FROM p WHERE id < 0 UNION SELECT 2`가 행 하나짜리 **빈 글자** 결과였다(왼쪽 답에 머리줄이 없어 열 이름이 비고, 오른쪽 행이 이름 없는 빈 행이 됨). MySQL은 왼쪽의 열 이름으로 오른쪽 행을 돌려준다. 왼쪽 SELECT의 열 이름(`Resolver`)으로 오른쪽 행을 위치로 놓는다(이름을 못 알면 오른쪽 이름).
- **`SELECT 'a'`의 머리글**: 문자열 상수 하나뿐인 select 항목은 별칭이 없으면 그 글자를 열 이름으로 쓴다(MySQL `a`, 전에는 `'a'`; 행이 있는 결과·뷰·`CREATE TABLE … AS SELECT`의 열도). `'a' + 1`처럼 식이면 쓴 그대로.
- **FROM 없는 SELECT의 WHERE**: `SELECT 1 WHERE 1 = 0`은 문법 오류였다(MySQL 8은 열 `1`과 0행). 파서가 FROM 없는 SELECT의 WHERE를 읽고, 실행기가 그 한 행을 두거나(참) 열 이름만 있는 빈 표를 돌려준다(거짓, UNION의 한쪽·`INSERT … SELECT … WHERE`도).

**조사하다 알게 된 것 (고치지 않음)**
- **`DROP FOREIGN KEY` 뒤의 자동 이름**: 이름 없는 키의 `<표>_ibfk_<n>`은 지금도 열 순서로 세어 정해져서, 앞의 키를 지우면 남은 키의 이름이 바뀐다(MySQL은 만들 때 정한 이름을 그대로 둠).
- **FLOAT**: 함수 결과(`GREATEST(f, 0) + 1`)에 대한 산술은 FLOAT 정밀도로 넓히지 않는다(0.1 행이 MySQL 1.1000000014901161, 여기 1.1). FLOAT와 DOUBLE·정수 열·DECIMAL이 섞인 함수는 MySQL이 넓힌 값(0.10000000149011612)을 내는데 여기는 저장된 짧은 글자(0.1)를 낸다. 윈도 `MIN`·`MAX`는 이번에 다루지 않았다.
- **문법**: `NATURAL LEFT | RIGHT JOIN`은 받지 않는다(파서가 `NATURAL` 뒤에 `JOIN`만 기대). `SHOW DATABASES LIKE`, `SHOW ERRORS`는 문법 오류, `SHOW GRANTS FOR 없는사용자`는 오류 1141 대신 빈 성공.
- `referential_constraints`의 `DELETE_RULE`·`UPDATE_RULE`는 규칙을 안 적은 키를 `RESTRICT`로 말한다(MySQL 8은 `NO ACTION`). 서로 다른 USING 열을 가진 조인이 이어질 때 합쳐진 열의 순서도 MySQL과 다르다(`y,x,a,b,c` 대 여기 `x,y,a,b,c`).
- 그대로인 것: 자기 표 UPDATE가 행 단위가 아닌 것, MySQL 프로토콜의 오류 번호가 전부 1064인 것, 쓰는 `CALL`·`OPTIMIZE`가 redo 로그를 비우는 것.

**검증**
- 새 C++ 시험 `[r57]` 6 케이스: REPLACE의 사슬 아래 보호(VALUES·SELECT 형식, 보호된 행이 앞·뒤, 사이에 보호 없는 행, 보호가 풀린 뒤의 연쇄, CASCADE·SET NULL 사슬, 자기 표 자식, 재시작 뒤 같은 행), MERGE의 UPDATE+DELETE, FLOAT(MIN·MAX·GREATEST·LEAST·COALESCE·IFNULL·NULLIF, 산술·비교·HAVING은 수 전체, 정수 열이 섞이면 DOUBLE, INSERT … SELECT 다섯 대상 열, 집계·식 항목, UPDATE), 한 열의 외래키 둘(INSERT·UPDATE·DELETE 양쪽 부모, 이름·SHOW CREATE·information_schema, DROP TABLE, CASCADE와 SET NULL을 한 열에, ALTER ADD·DROP과 이름 중복, ON UPDATE CASCADE가 다른 부모의 값을 보는지, **카탈로그 재시작 뒤**), 빈 결과의 열 이름(USING·NATURAL·RIGHT·LEFT, CALL, RIGHT JOIN USING의 순서, UNION 왼쪽 0행), 문자열 상수 머리글과 FROM 없는 WHERE(서브쿼리·UNION·`INSERT … SELECT … WHERE`·사용자 변수). 옛 시험 둘이 옛 동작을 기대하고 있어 고쳤다(`RIGHT JOIN … USING`의 열 순서, USING 조인의 타입).
- 새 코퍼스(진짜 MySQL 8.4.9와 비교, **차이 0**): `multi_fk` 54문장(둘 다 거절하는 것 13), `replace_chain` 33(둘 다 거절 5), `float_copy` 43, 그리고 `empty_columns`가 48 → 81문장(USING·NATURAL·RIGHT·CALL·상수·FROM 없는 WHERE·UNION). 검증 도구 `verify_joins.py`의 `*` 모델을 MySQL 규칙(RIGHT JOIN USING)으로 고쳤다.
- **심은 버그 36종 중 36종이 잡힘**. 한 번에 전부 잡혔다(생존 0): REPLACE·MERGE를 옛 사전 검사(`delete_restrict_violation`)로 되돌림, FROM 없는 WHERE를 무시·뒤집기·파서에서 빼기, 문자열 상수 별칭 끄기, `Resolver`의 USING 전개(합쳐진 열을 건너뛰지 않음·앞의 열이 겹침·RIGHT의 순서·NATURAL의 공통 열)와 행이 있는 결과의 RIGHT 순서, `CALL`의 열 이름과 "아직 결과 표가 없을 때만" 조건, UNION 왼쪽의 이름, FLOAT(`MIN`·`MAX` 표시를 바인더에서 빼기·조건 없는 집계의 출력·식 출력·`NULLIF`·정수 상수를 FLOAT로 보지 않음·정수 열도 FLOAT로 받아들임), `INSERT … SELECT`(저장된 값 그대로 받기·넓히기·식·집계 항목 알아보기), `UPDATE`의 넓히기, 외래키 벡터(CREATE·ALTER가 덮어씀, JSON 쓰기·읽기, UPDATE·INSERT의 검사가 첫 키만, 연쇄 모의의 변이 첫 키만, `DROP FOREIGN KEY`가 전부 지움, 이름 중복을 못 봄, `DROP TABLE`이 첫 키만 봄, 연쇄가 다른 부모의 값을 확인하지 않음, UPDATE의 쓰기 전 모의에서 "열에 키가 여럿" 조건을 뺌). 시험이 못 잡는 것으로 따로 심지 않은 것: 자식 표 읽기 락 목록(`fk_children_closure`의 모든 키 보기)과 부모 값 클레임의 키별 반복(동시 실행에서만 보임).
- 크래시 퍼저 6종: `fuzz_crash` 15, `fuzz_crash_fk` 15, `fuzz_concurrent` 8, `fuzz_crash_selfref` 15, `fuzz_crash_chain` 24, 새 **`fuzz_crash_multifk`** 40라운드(부모 `pa`·`pb`와 둘을 모두 가리키는 자식 열, 두 외래키의 ON DELETE 동작 9가지, INSERT·UPDATE·DELETE·REPLACE(여러 행), 트랜잭션·세이브포인트, 한 부모에만 있는 값은 거절된다는 모델; 복구 뒤에도 두 키가 모두 일하는지) 불일치 0. 무작위 대조 12종 위반 0(`verify_joins`는 시드 세 개). R53 빌드와의 차분(`diff_builds`, 문장 1,500개를 시드 둘로, 열 이름·머리줄은 무시) 0.
- 전체 시험 **834 케이스 / 1,509,237 assertions**(Release 기본), 1,509,237(`RUSQL_DML_INDEX_MIN_ROWS=0`), 1,509,238(`RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0`) 통과. 호환성 점수판: 코퍼스 36개, 문장 3,203개 중 차이 8개(앞과 같은 8개이고 새 코퍼스 셋은 0).
- 실서버 A/B(R53 빌드 대 새 빌드, 같은 작업): (5,000행 표, 한 작업을 40~500번 반복한 중앙값의 세 번 평균) 자식 표 INSERT 0.62 → 0.62 ms, 외래키 열 UPDATE 0.64 → 0.64 ms, `REPLACE`(아무도 가리키지 않는 표) 0.65 → 0.65 ms, `REPLACE`(부모, 자식 없음) 1.52 → 1.58 ms, **`REPLACE`(자식이 따라 지워지는 부모) 1.55 → 1.99 ms**(지울 행 전부를 사슬 끝까지 미리 모의하는 값; 이 길이 아닌 문장은 그대로), `INSERT … SELECT` 한 행 0.66 → 0.66 ms, `UNION` 0.13 → 0.13 ms, 프로시저 `CALL` 0.098 → 0.098 ms, `COALESCE(f, 0)` 0.117 → 0.120 ms, `MAX(f)` 1.15 → 1.14 ms, 점 조회 0.115 → 0.114 ms, 표 없는 SELECT 0.059 → 0.059 ms.
- **하지 않은 것**: Debug 빌드, 앱 화면에서 바뀐 열 이름을 본 것은 아님(화면 변경은 Diagram 문구뿐), 문장 중간 크래시, 커서·프로시저의 여러 결과 집합(CALL은 여전히 결과 표 하나).

### 10월 28일 — R58: R57이 남긴 일곱 가지(FLOAT 혼합·함수 결과의 산술·윈도 MIN/MAX, 이어지는 USING의 `*` 열 순서, 자동 외래키 이름, `NO ACTION` 표기, MySQL 프로토콜의 오류 번호)

**왜 이 항목인가**: R57의 "조사하다 알게 된 것"과 `RECOMMAND.md`에 남아 있던 것 중 사용자가 일곱 개를 골라 "같은 진행방식으로 진행합니다"라고 했다(① 코드 작성에만 집중 → ② 전부 끝나면 시험, 통과하면 문서 → 커밋·푸시 → ③ 추천표 다시 쓰기와 화면 띄우기).

**고친 것 (엔진)**
- **FLOAT가 DOUBLE·정수 열·DECIMAL과 섞인 함수** (조용한 오답): 진짜 MySQL 8.4.9로 먼저 쟀다(`float_mixed` 코퍼스, 처음 27문장이 달랐음). `COALESCE`·`IFNULL`·`GREATEST`·`LEAST`는 인자가 FLOAT 열·정수 상수·NULL뿐이면 FLOAT(6자리)이고, **DOUBLE·정수 열·DECIMAL·소수가 하나라도 섞이면 DOUBLE이며 그 안의 FLOAT는 넓힌 값**이다(`COALESCE(f, d)`의 0.1은 0.10000000149011612, 3.14159265는 3.1415927410125732). `NULLIF`는 **첫 인자의 타입**을 따라 FLOAT면 6자리로 보이고, 비교는 FLOAT를 넓혀서 한다(`NULLIF(f, d)`가 0.1 행에서 NULL이 아니라 0.1: 단정밀도 0.1과 배정밀도 0.1은 다른 수) — 돌려주는 값은 넓힌 값이 아니라 첫 인자 그대로(`CONCAT(NULLIF(f, d), '')`는 `0.1`)이고, 두 번째 자리의 FLOAT도 넓혀서 비교한다(`NULLIF(d, f)`). 구현: `single_result`를 `float_kind`(FLOAT 열 / 정수 상수·NULL / 그 밖)로 다시 짰고, `eval_arith`가 이 다섯 함수에서 "FLOAT가 섞였는데 값 전체가 FLOAT는 아님"(`mixes_float`)이면 그 인자들을 넓혀서 넘기고, `NULLIF`는 따로 처리한다.
- **함수 결과에 대한 산술·비교** (조용한 오답): FLOAT인 함수 결과(`GREATEST(f, 0)`)는 FLOAT 열처럼 단정밀도 수로 읽어야 한다. `GREATEST(f, 0) + 1`의 0.1 행이 MySQL 1.1000000014901161인데 여기는 1.1이었고, `WHERE GREATEST(f, 0) = 0.1`은 MySQL 0행인데 여기는 1행, `WHERE COALESCE(f, 0) > 0.1`은 4행 대 3행이었다. 산술 잎(`number_tree`)이 FLOAT인 함수 결과를 근사 수로 읽고(넓힌 값), 비교(`eval_single3`의 `left_single`·`right_single`, `BETWEEN`·`IN` 포함)가 함수 결과도 넓힌다.
- **윈도 `MIN`·`MAX`·`LAG`·`LEAD`·`FIRST_VALUE`·`LAST_VALUE`·`NTH_VALUE`가 FLOAT 열을 낼 때**: MySQL은 FLOAT(6자리)인데 저장된 글자(16777217 → 16777216, 1234567)가 나왔다. 바인더가 `WinFunc::col_single`을 적고 출력 단계가 6자리로 보인다(`SUM`·`AVG`는 DOUBLE이라 그대로).
- **이어지는 USING·NATURAL 조인의 `*` 열 순서** (조용한 오답, 열 위치로 읽는 클라이언트에 오답): MySQL은 조인을 트리로 읽어 `(a JOIN b USING (x)) JOIN c USING (y)`의 `*`를 **나중 조인의 합쳐진 열 → 왼쪽의 나머지 → 오른쪽의 나머지**로 낸다(`a(x,y,a) b(x,b) c(y,c)`는 `y,x,a,b,c`, 여기는 `x,y,a,b,c`). 한 조인의 합쳐진 열은 USING에 적은 순서가 아니라 **왼쪽이 가진 순서**(`USING (y, x)`를 `a(x,y,…)`에 쓰면 `x,y`, 비교하다 알게 됨), `a RIGHT JOIN b USING`은 양쪽을 맞바꾼 `b LEFT JOIN a`, ON 조인은 합치지 않고 이어 붙인다. 행이 있는 결과(`resolve_join_columns`)와 빈 결과의 열 이름(`Resolver::select_columns`)이 같은 규칙을 쓰도록 새 헤더 `engine/join_star.hpp`의 `star_columns` 하나로 모았다(두 곳에 있던 `merged`·`listed`·`table_order`는 지움).
- **이름 없는 외래키의 자동 이름** (열 순서로 세어서 `DROP FOREIGN KEY` 뒤에 남은 키의 이름이 바뀜): 진짜 MySQL로 규칙을 쟀다 — CREATE TABLE은 **이름이 없는 키만, 적은 순서대로** `<표>_ibfk_1, _2 …`(열 순서가 아님, 이름 붙인 키와 `fk_ibfk_5`처럼 번호 모양인 이름은 세지 않음), `ALTER TABLE … ADD FOREIGN KEY`는 **가장 큰 `<표>_ibfk_<n>`보다 하나 큰 번호**(마지막 키를 지우고 다시 달면 그 번호를 다시 씀), `DROP`은 다른 키를 그대로 두고, `RENAME TABLE`·`ALTER … RENAME TO`는 `<옛 표>_ibfk_`로 시작하는 이름의 접두사만 바꾼다. 이제 이름을 키에 **저장**한다(`ForeignKey::name`, 카탈로그 JSON에 이미 있던 칸): 파서가 CREATE TABLE에서 표 이름과 센 수(`fk_table_`·`unnamed_fk_name`)로 정하고(열 안의 `REFERENCES`도 적은 순서), ALTER는 `fk_next_name`, 이름이 없는 옛 카탈로그는 처음 건드릴 때 옛 규칙(열 순서)으로 이름을 정해 저장한다(`fk_materialize_names`).
- **`RENAME TABLE`의 두 구멍** (이 비교로 찾음, 전부터): ① 표를 바꾼 뒤 `information_schema`가 **옛 표 이름**을 말했다(카탈로그 항목의 `name`이 안 바뀜). ② **부모 표의 이름을 바꾸면 자식의 외래키가 옛 이름을 가리킨 채** 남아 이후 자식 INSERT가 "Referenced table … not found"로 실패했다(MySQL은 키가 새 이름을 따라감, `SHOW CREATE`도 새 이름). 둘 다 고쳤다(자기 표를 가리키는 키 포함).
- **`referential_constraints`의 `NO ACTION`**: MySQL은 규칙을 안 적거나 `NO ACTION`이면 `NO ACTION`, **적은 `RESTRICT`만 `RESTRICT`**로 말하고, `SHOW CREATE TABLE`은 적은 RESTRICT만 출력하고 NO ACTION은 출력하지 않는다(진짜 MySQL로 일곱 모양을 확인). 둘의 거절 동작은 같아서 `on_delete`·`on_update`(`Restrict`)는 그대로 두고 `ForeignKey`에 `restrict_delete`·`restrict_update`("RESTRICT를 적었음")를 더했다. 파서(`parse_fk_action(bool*)`, CREATE의 열·표 수준 키와 ALTER ADD), `information_schema`, MySQL 모양의 `SHOW CREATE TABLE`, 백업·`SHOW CREATE TABLE`(엔진 자체 모양은 규칙을 전부 적어 다시 실행해도 같은 표), 카탈로그 JSON(참일 때만 씀: 옛 파일은 그대로 읽히고 `NO ACTION`으로 읽힘)이 이 표시를 따른다. ERD의 외래키 편집기는 `NO ACTION`도 고를 수 있다.
- **MySQL 프로토콜의 오류 번호**: 모든 오류가 1064(ER_PARSE_ERROR)·SQL 상태 42000으로 나갔다. 클라이언트는 번호로 오류를 구별한다(중복 키 1062, 외래키 1451·1452, 모르는 열 1054 …). `mysql.cpp`의 `wire_error`가 정한다: 엔진 메시지가 `ERROR 1205 (HY000): …`처럼 번호를 이미 달고 있으면(락 대기·KILL·MAX_EXECUTION_TIME·`ESCAPE`) 그것을 쓰고, 나머지는 메시지의 시작과 내용으로 약 40종(1062·1048·1364·1406·1264·1265·1366·1292·1451·1452·3819·3730·1054·1146·1050·1051·1060·1061·1091·1052·1066·1093·1110·1136·1241·1242·1111·1365·1305·1318·1426·1582·1690·1007·1008·1049·1210·1213·1045·3579·3584~3586·3008·1826)을 정하고 나머지는 1064. 번호를 알기 위해 `vs_mysql.py`에 `--proto --errno`를 넣어 모든 코퍼스를 돌렸다: **둘 다 실패하는 문장 중 번호가 다른 것 385개 → 약 20개**. 같은 하나의 메시지가 둘이던 것은 엔진 메시지를 MySQL 것으로 맞췄다: 빠뜨린 NOT NULL 열 `Field 'a' doesn't have a default value`(1364, 전에는 쓴 NULL과 같은 `Column 'a' cannot be NULL`(1048)), `DROP TABLE 없는표`의 `Unknown table`(1051, 한 표일 때는 1146 모양이었음), 집계 안의 집계 `Invalid use of group function`(1111). 글자가 `12abc`처럼 수로 시작하면 1265, `abc`처럼 수가 아니면 1366(MySQL의 `Data truncated`와 `Incorrect integer value`).

**조사하다 알게 된 것 (고치지 않음)**
- **모호한 열**: `a JOIN b USING (x) JOIN c USING (y) JOIN d USING (x)`에서 `d`도 `y`를 가지면 `ORDER BY y`는 MySQL이 1052(모호함)로 거절하는데 여기는 행을 돌려준다. `SELECT a.*, c.* … ORDER BY y`도 같은 모양.
- **FLOAT**: `CONCAT(f, '')`는 MySQL이 6자리 글자(`3.14159`), 여기는 저장된 글자(`3.1415927`)이고, FLOAT를 내는 함수 결과를 글자로 바꾸면 MySQL은 넓힌 값을 준다(`CONCAT(COALESCE(f, 0), '')`가 `0.10000000149011612`): 글자 함수가 인자의 FLOAT 여부를 모른다.
- **오류 번호의 남은 약 20개**는 엔진이 MySQL과 다른 이유로 실패하는 경우다(`ONLY_FULL_GROUP_BY`의 1055, `RANGE` 프레임 오류 3587~3589, 윈도 함수를 쓸 수 없는 자리 3593, `SUM(SUM(x)) OVER`의 1140, 일부 `Data truncated`, `MOD`로 0을 나누는 UPDATE의 1365, 없는 윈도 이름 3579). 행을 다른 서브쿼리와 비교하는 `(1, 2) = (SELECT 1)`은 MySQL이 1241로 거절하는데 여기는 "지원하지 않음"으로 거절한다(번호만 1064).
- 그대로인 것: 자기 표 UPDATE가 행 단위가 아닌 것, 쓰는 `CALL`·`OPTIMIZE`가 redo 로그를 비우는 것, `NATURAL LEFT | RIGHT JOIN`·여러 열 외래키·`SHOW DATABASES LIKE` 같은 문법.

**검증**
- 새 C++ 시험 `[r58]` 9 케이스: FLOAT 혼합(DOUBLE·정수 열·DECIMAL·소수, 안쪽 함수, `NULLIF`의 값과 두 번째 자리), 함수 결과의 산술·비교(`=`·`>`·`BETWEEN`·`IN`, 정확히 담기는 값은 그대로), 윈도 일곱 함수, 이어지는 USING의 열 순서(ON 조인 사이, USING 목록 순서, NATURAL, RIGHT 맞바꿈, 세 번째 조인, 빈 결과), 자동 외래키 이름(적은 순서, DROP·ADD·가장 큰 번호 재사용, RENAME 두 문법, 이름 붙인 키와 `n3_ibfk_5`·`n5_ibfk_x`, 한 ALTER의 DROP과 ADD, 데이터베이스 접두사), **재시작 뒤** 이름·규칙·바뀐 표 이름과 자식 키, 규칙 표기 일곱 모양과 ALTER ADD·SHOW CREATE 두 가지, BACKUP → RESTORE가 규칙을 보존하는지, 빠뜨린 NOT NULL 열의 메시지. 옛 시험 둘이 옛 동작을 기대하고 있어 고쳤다(`referential_constraints`의 규칙 생략 → `NO ACTION`, 빠뜨린 열의 메시지).
- 새 코퍼스(진짜 MySQL 8.4.9와 비교, **차이 0**): `float_mixed` 52문장, `using_chain` 21, `fk_auto_names` 51(둘 다 거절 6), `errno` 49(`--proto --errno`, 둘 다 거절 48). 앞의 셋은 C++ 적합성 시험(`mysql_conformance_*.inc`)으로도 들어갔다.
- **심은 버그 39종 중 39종이 잡힘**(오류 번호 분류기의 심은 버그 1종은 `errno` 코퍼스가 잡음). 처음에 넷이 살아남았고(`NULLIF`이 넓힌 값을 돌려줌 — 6자리 표시에 가려짐, `NULLIF`의 두 번째 자리를 넓히지 않음, `n5_ibfk_x`처럼 번호가 아닌 이름을 번호로 읽음, 백업이 규칙을 `RESTRICT`로 적음) 그 넷을 잡는 시험(`CONCAT(NULLIF(f, d), '')`, `NULLIF(d, f)`, `ibfk_x` 이름, BACKUP·RESTORE)을 더한 뒤 모두 잡혔다. 나머지: FLOAT 혼합(`COALESCE`·`GREATEST`·안쪽 함수·`NULLIF`의 비교를 넓히지 않음, 함수 결과의 산술·비교·근사 수 표시, `NULLIF`을 FLOAT로 보지 않음, 다른 타입을 무시), 윈도(바인더·`LAG`·표시), `*` 열 순서(맞바꿈·오른쪽 먼저·합쳐진 열이 남음·새 합쳐진 열이 뒤·USING 순서), `RENAME`(옛 이름·자식이 안 따라감·접두사 안 바꿈), 자동 이름(센 수가 안 오름·이름 붙인 키도 셈·ALTER가 항상 1·키 수를 셈·번호 아닌 이름), 규칙(RESTRICT를 못 읽음·항상 RESTRICT·항상 NO ACTION·SHOW CREATE가 NO ACTION을 적음·적은 RESTRICT를 숨김·JSON 쓰기·읽기·ALTER ADD의 표시), NOT NULL 메시지.
- 크래시 퍼저 6종(`fuzz_crash` 15, `fuzz_crash_fk` 15, `fuzz_concurrent` 8, `fuzz_crash_selfref` 15, `fuzz_crash_chain` 24, `fuzz_crash_multifk` 40)과 무작위 대조 12종: 불일치·위반 0. (이 둘은 마지막에 FLOAT 판정 도우미 한 곳을 문자열을 복사하지 않게 다시 쓰기 **전**에 돌렸다. 그 뒤로는 전체 시험·적합성·A/B를 다시 돌렸다.) R57 빌드와의 차분(`diff_builds`, 문장 1,500개씩 시드 둘, 열 이름·머리줄은 무시) 0.
- 전체 시험 **846 케이스 / 1,509,795 assertions**(Release 기본), 1,509,795(`RUSQL_DML_INDEX_MIN_ROWS=0`), 1,509,795(`RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0`) 통과. 호환성 점수판: 코퍼스 40개, 문장 3,376개 중 차이 8개(앞과 같은 8개이고 새 코퍼스 넷은 0).
- 실서버 A/B(R57 빌드 대 새 빌드, 같은 작업, 세 번 평균): 한 행 INSERT(외래키 열·NOT NULL 열) 0.62 → 0.62 ms, `COALESCE(f, 0)`·`COALESCE(f, d)`·`GREATEST(f, 0) + 1` 한 행 조회 0.114~0.123 → 같음(±1%), `MAX(f)` 1.24 → 1.25 ms, 윈도 `MAX(f) OVER ()` 1.98 → 2.02 ms, USING 조인 2.38 → 2.36 ms와 세 표 5.38 → 5.13 ms, 점조회 0.108 → 0.109 ms, 표 없는 SELECT 0.053 → 0.053 ms, 외래키 둘인 표 CREATE + DROP 1.13 → 1.14 ms(세 번 중 한 번 튄 값 1.58 제외), 5,000행 `SUM(GREATEST(f, 0) + k)` 10.36 → 10.91 ms(+5%), `ROUND(d, 1) * 2 > k` 6.67 → 6.92 ms(+4%). **`WHERE COALESCE(f, 0) > k`(5,000행) 2.50 → 3.17 ms(+27%)**: 비교가 이제 FLOAT를 넓혀서 하기 때문이다(FLOAT 열 자신의 비교가 이미 치르던 값과 같고, 행마다 약 130 ns). FLOAT를 안 쓰는 식에는 닿지 않는다. 처음에는 +33%였고 판정 도우미가 이름을 복사하던 것과 조건 검사 순서를 고쳐 줄였다.
- **하지 않은 것**: Debug 빌드, 앱 화면에서 바뀐 결과를 본 것은 아님(화면 변경은 Diagram 문구와 외래키 편집기의 `NO ACTION` 선택지뿐), 문장 중간 크래시, `fuzz_crash_*`에 이름·RENAME을 섞은 것(이름은 재시작 시험이 확인).

### 10월 29일 — R59: 느린 쿼리 로그, 세션 패널의 KILL·차단자 표시, 실행 계획 그림, 결과 JSON·차트, 스키마 비교, ERD 드래그 연결, `suggest_indexes` 후속

**왜 이 항목인가**: `RECOMMAND.md`의 "보이는 장면 다듬기"와 "MCP 남은 것"에서 사용자가 아홉 개를 골라 "같은 진행방식으로 진행합니다"라고 했다(① 코드 작성에만 집중 → ② 전부 끝나면 시험, 통과하면 문서 → 커밋·푸시 → ③ 추천표 다시 쓰기와 화면 띄우기). 엔진에서는 느린 쿼리 로그와 "어느 세션의 트랜잭션인가"만 새로 만들었고, 나머지는 앱(Tauri/React)과 MCP 쪽이다.

**만든 것 (엔진)**
- **느린 쿼리 로그** (`engine/slow_log.hpp` 새 헤더, `executor_core.cpp`): `SET GLOBAL slow_query_log = ON | OFF | TRUE | FALSE | 1 | 0`, `SET GLOBAL long_query_time = 초`(소수 가능, 기본 10). 맨 바깥 문장을 `execute_sql`이 재서(`run_sql_text`를 감싼 얇은 겉껍질: 안쪽 문장·프로시저 본문은 세지 않음), `long_query_time` 이상이면 `Slow_queries`(SHOW GLOBAL STATUS)를 **로그가 꺼져 있어도** 올리고(MySQL과 같음), 켜져 있으면 기록한다. 기록은 두 곳: 메모리의 마지막 1000개(`information_schema.slow_log`: START_TIME · USER_HOST · QUERY_TIME · ROWS_SENT · DB · SQL_TEXT · THREAD_ID, 이 표는 RuSQL 자체 표)와 `<데이터 폴더>/slow_query.log`(MySQL 느린 로그 모양: `# Time` · `# User@Host … Id` · `# Query_time … Rows_sent` · `use db;` · 문장). 두 스위치는 **서버 전체**(원자 변수 둘)라 느리지 않은 문장은 읽기 한 번과 비교 한 번만 치르고, 다른 세션이 바꾼 값은 `version` 번호가 달라졌을 때 `@@slow_query_log`·`@@long_query_time`로 다시 읽는다. 오류: 켜고 끄는 값이 아니면 1231, 글자·NaN이면 1232, **범위(0 ~ 31536000) 밖의 수는 오류가 아니라 그 안으로 맞춘다**(진짜 MySQL이 경고만 하고 그렇게 한다). 한 가지 다름: MySQL의 `long_query_time`은 세션 값이 따로 있어 `SET GLOBAL`이 이미 열린 연결에는 닿지 않지만, 여기는 하나뿐이다(앱의 세션 패널이 켜고 바로 보이게).
- **`information_schema.innodb_trx`가 세션을 말함**: `TRX_MYSQL_THREAD_ID`(그 트랜잭션을 연 세션 번호, 전에는 0)와 `TRX_STARTED`(시작 시각). `ProcessInfo`에 `txn_id`·`txn_started`를 두고 문장이 시작·끝날 때(`update_process_command`) 갱신한다. 세션 패널이 "누가 누구를 막는가"를 그리려면 `innodb_lock_waits`의 트랜잭션 번호를 세션 번호로 바꿀 길이 필요했다.
- **`FOUND_ROWS()`와 `ROWS_SENT`가 집계 결과에서 0이던 것** (조용한 오답, R37부터 있었음): `SELECT COUNT(*) FROM t`처럼 집계만 있는 한 행짜리 답은 표 끝에 "1 row(s) returned." 줄이 없어서 `returned_rows`가 0으로 읽었다(MySQL은 `SELECT COUNT(*) …; SELECT FOUND_ROWS()`가 1). 느린 쿼리 로그의 `ROWS_SENT`를 앱에서 보다가 찾았다. 줄이 없으면 표의 데이터 줄 수(머리줄 빼고)를 센다. 새 코퍼스 `slow_log`로 진짜 MySQL 8.4.9와 비교.

**만든 것 (앱)**
- **세션 패널** (`ActivityPanel.tsx`, `lib/activityMarks.ts`): 문장마다 **Stop statement**(`KILL QUERY`)과 **Disconnect**(`KILL CONNECTION`) 버튼(눌러서 "…? yes"가 뜨면 한 번 더, 4초 안에 안 누르면 되돌아감; 패널 자신의 연결에는 버튼이 없음). **차단자 표시**: 다른 트랜잭션이 기다리는 행 잠금을 쥔 세션은 `BLOCKS n`, 기다리는 쪽은 `waits for #n`, 맨 위 빨간 줄에 "⚠ #2 blocks 1 transaction"과 그 세션의 Disconnect(맨 뿌리 차단자만: 이 세션을 끝내면 나머지가 풀림; 사슬 a ← b ← c는 a). **오래 도는 문장**: 5초 이상 호박색, 30초 이상 빨강(`SLOW 12s`), **트랜잭션을 열어 둔 채 30초 넘게 쉬는 세션**은 `idle in transaction`. 마크를 정하는 규칙은 DOM이 없는 `activityMarks.ts`에 있어 따로 시험한다.
- **Slow queries 구역**: 기준 시간 선택(0.1 · 0.5 · 1 · 5 · 10초와 지금 값)과 `Turn on/off`(`SET GLOBAL`), 센 개수(`Slow_queries`), 최근 15개(걸린 시간 · 보낸 행 · 세션 · 시각 · 문장 앞 160자, 마우스를 올리면 전체).
- **EXPLAIN 계획 그림** (`PlanDiagram.tsx`, `lib/planDiagram.ts`): `EXPLAIN`·`EXPLAIN ANALYZE`의 계획 줄을 상자와 화살표(스캔 → 조인 → 필터 → 결과)로 계획 위쪽에 그린다. 가장 오래 걸린 단계는 금색, 단계마다 행 수·시간·비중 막대, 조인은 알고리즘과 비용.
- **결과 JSON 내보내기** (`lib/resultExport.ts`): "⬇ CSV" 옆의 "⬇ JSON". 한 줄이 열 이름을 키로 하는 객체. `NULL`은 `null`, **JSON 수로 적어도 잃는 것이 없는 값만 수**(`-12`, `3.5`는 수, `10.50`·`007`·`12345678901234567890`은 글자 그대로), 같은 이름의 열은 `_2`, `_3`.
- **결과 차트** (`ResultChart.tsx`, `lib/chartData.ts`): "📊 Chart". 수가 아닌 첫 열이 점의 이름, 수인 열이 막대·꺾은선·파이. 처음 그림은 첫 수 열과 **크기가 10배 안인** 열만 겹쳐 그린다(자릿수가 다른 열을 한 눈금에 그리면 한쪽이 바닥에 붙는 것을 화면에서 보고 규칙을 넣음), 날짜 모양 열이면 꺾은선. 막대는 0부터, 눈금은 둥근 수. 점 60개·계열 4개·조각 12개까지.
- **스키마 비교** (`SchemaDiffModal.tsx`, `lib/schemaDiff.ts`): 데이터베이스 오른쪽 클릭 → "Compare Schema…". 두 데이터베이스의 표 · 열(타입, NOT NULL, 기본값, AUTO_INCREMENT) · 인덱스(기본키 제외) · 외래키(규칙 포함)를 `information_schema`로 읽어 다른 점과 **첫째를 둘째처럼 만드는 SQL**을 만든다(새 표는 부모 먼저, 바뀐 표는 외래키·인덱스 삭제 → 열 삭제 → 열 추가 → 열 변경 → 인덱스·외래키 추가, 없어진 표의 `DROP`은 주석). **아무것도 실행하지 않고** 복사하거나 에디터에 넣는다.
- **ERD 드래그 연결** (`ErdView.tsx`, `FkEditorModal.tsx`): 열 줄 끝의 점을 다른 표의 열로 끌면(자식 열 → 부모 열) 점선이 따라오고, 놓으면 두 열이 채워진 외래키 편집기가 열려 DDL 미리보기를 보고 적용한다.

**만든 것 (MCP)**: `suggest_indexes` 후속. ① **전·후 모드** `verify=true`: 제안한 인덱스를 임시 이름으로 만들고 `EXPLAIN ANALYZE`를 세 번 재 중앙값으로 전·후(계획 · 읽은 행 · 시간 · 배수)를 보고하고 다시 지운다(SELECT만, 20만 행 넘는 표는 안 함). ② **하위 질의·CTE·파생 표**를 블록으로 나눠 따로 읽는다(`IN`·`EXISTS`·비교의 하위 질의, 상관 하위 질의의 `o.customer_id = c.id`, `WITH`). ③ **`OR`**: 가지마다 읽고, 엔진이 문장당 인덱스 하나만 읽으므로 가지마다 인덱스를 제안한 뒤 `UNION` 다시 쓰기를 함께 알린다(괄호 안의 `AND` 묶음도 풀어서 읽음). 도구 수는 그대로 32개.

**조사하다 알게 된 것 (고치지 않음)**
- **한 덩어리 단계는 `KILL QUERY`로 못 멈춘다**: 교차 조인 `FROM a, b, c`는 행 쌍을 한꺼번에 만들어서, 300행 표 셋이면 27M 쌍을 만드는 동안 메모리가 6 GB까지 오르고 멈추지 않으며(세션 패널의 Stop statement도 소용없음) 같은 표를 읽는 다른 문장의 쓰기가 기다린다. 전부터 있던 한계로 `RECOMMAND.md`의 "문장 중단의 남은 것"에 있다.
- **외래키 값 클레임의 드문 경합(원인 미확인)**: 위 검증 항목의 실패. 같은 시험을 가볍게 돌리면 나오지 않아서, 시험 쪽에서 실패한 걸음의 문장 순서를 남기게 하거나(`doing` 표를 실패 때 출력) 시드를 고정해 반복하는 것이 첫 걸음이다.
- `SLEEP()`은 여전히 잠들지 않는다(시험은 `long_query_time = 0`으로 느린 문장을 만든다).
- MySQL의 `SET slow_query_log = ON`(GLOBAL 없이)은 오류 1229인데 여기는 받는다. `long_query_time`의 세션 값은 위에 쓴 대로 따로 없다.

**검증**
- 새 C++ 시험 `[r59]` 4 케이스(느린 문장이 세어지고 켜졌을 때만 적히는지 · 걸린 시간·보낸 행·데이터베이스·세션·파일, 두 스위치가 서버 것이고 틀린 값은 거절되고 범위 밖은 맞춰지는지, 1000개에서 오래된 것부터 빠지는지, `innodb_trx`가 세션·시작 시각을 말하는지), 집계 결과의 `FOUND_ROWS()`·`ROWS_SENT`.
- 새 코퍼스 `slow_log`(진짜 MySQL 8.4.9와 비교, **차이 0**, 처음 4개가 달랐다: 음수와 큰 수를 오류로 거절하던 것): 스위치의 값들과 오류 번호, `FOUND_ROWS()`를 집계·`HAVING`·그룹·`DISTINCT`·`LIMIT` 뒤에. 전에 쓴 것: `SET GLOBAL long_query_time = -1`이 MySQL은 오류가 아니었다.
- **심은 버그 36종 중 36종이 잡힘**: 처음에 넷이 살아남았고(`THREAD_ID`를 0으로 적음 — 첫 실행기의 세션 번호가 원래 0이라 구별되지 않았음, `QUERY_TIME`을 1000배로 적음, 시작 시각을 비움, `QUERY_TIME`의 소수 자릿수) 그 넷을 잡는 시험(등록한 둘째 세션의 번호와 `USER_HOST`, 300행 교차 조인을 `long_query_time = 0.05`로 재서 값이 0.05 ~ 60초 · 소수 여섯 자리, 트랜잭션이 방금 열렸는지)을 더한 뒤 모두 잡혔다. 나머지: 기준 비교 방향·무시, 세지 않음·로그가 켜졌을 때만 셈·꺼져 있어도 적음, 보낸 행·데이터베이스·문장·시간 단위, 집계 행 수 두 가지, 겉껍질이 다른 세션의 값을 안 읽음, 두 스위치의 값(`ON`·`TRUE`·`OFF`·`FALSE` 거절·받아들임, 버전 번호, 단위, 범위 맞춤 셋, 글자·NaN), 트랜잭션의 세션·시작 시각, `innodb_trx`·`slow_log` 열, `Slow_queries`.
- MCP `suggest_indexes` 단위 시험 33개와 감시 도구 시험 20개 통과, 진짜 엔진에서 `verify` 실행(만든 임시 인덱스가 남지 않음). **프론트의 순수 로직 시험 17개**(새 `code/test/frontend/test_libs.mjs`: 프론트 자신의 TypeScript로 `lib/*.ts`를 컴파일해 돌림): JSON 값 규칙, 차트 기본 그림·눈금·절단, 계획 줄 읽기, 스키마 비교와 마이그레이션 순서, 세션 마크(차단자·기다리는 쪽·사슬·느린 문장·열어 둔 트랜잭션).
- **앱에서 직접 확인**(CDP로 Tauri WebView2를 움직임): JSON·차트(기본 그림이 한쪽만 보이던 것을 고침)·계획 그림·ERD 드래그와 적용·세션 패널의 차단자 줄과 두 단계 Disconnect(차단자를 끊으니 기다리던 트랜잭션이 행 잠금을 얻음)·`Stop statement`·느린 쿼리 구역의 기준 시간 선택과 목록·스키마 비교 창과 "Open in editor". 스키마 비교에서 **한쪽에만 있는 표의 `CREATE TABLE`을 못 읽는 오류**를 찾아 고쳤다(앱의 연결은 `SHOW CREATE TABLE`을 행이 아니라 글자로 돌려줌).
- 전체 시험 **850 케이스 / 1,510,945 assertions**(Release 기본), 1,510,944(`RUSQL_DML_INDEX_MIN_ROWS=0`), 1,510,945(`RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0`) 통과. **다만 `idx0` 모드를 한 번 더 돌렸을 때 동시성 시험 하나가 실패했다**: `sessions racing a child row against its parent leave no child without a parent`(`test_unique_claims.cpp`, 다섯 세션이 부모 삭제·자식 쓰기·부모 키 바꾸기를 엇갈려 하고 매 걸음 끝에 부모 없는 자식을 세는 시험)가 `orphans == 1`. 그 시험만 `idx0`로 40번, 기본으로 40번 따로 돌려 **실패 0**, `idx0` 전체 시험을 두 번 더 돌려 둘 다 통과(1,510,945와 1,510,946: 무작위 시험 때문에 모드마다·실행마다 개수가 하나쯤 달라짐). 이번 변경은 외래키 값 잠금 경로를 건드리지 않았고(문장 앞뒤에 시계와 원자 변수 읽기가 붙었을 뿐) 세 번의 재실행에서 재현되지 않았지만, **원인을 찾지 못했고 이 빌드 이전부터 있던 드문 경합이라는 증거도 없다**(약 90번 중 1번). 걸음이 끝난 뒤의 검사라 읽는 도중의 일시적 상태가 아니라 진짜 고아 행이다. 남은 일로 적는다. 호환성 점수판: 코퍼스 41개, 문장 3,429개 중 차이 8개(앞과 같은 8개, 새 코퍼스는 0). 크래시 퍼저 6종(`fuzz_crash` 15, `fuzz_crash_fk` 15, `fuzz_concurrent` 8, `fuzz_crash_selfref` 15, `fuzz_crash_chain` 24, `fuzz_crash_multifk` 40)과 무작위 대조 12종: 불일치·위반 0. R58 빌드와의 차분(`diff_builds`, 문장 1,500개씩 시드 둘) 차이 0.
- 실서버 A/B(R58 빌드 대 새 빌드, 같은 작업, 세 번 평균): 문장 앞뒤에 시계 두 번과 원자 변수 읽기가 붙었지만 **재어지지 않는다** — 점조회 0.140 → 0.135 ms, 표 없는 SELECT 0.075 → 0.073 ms, 한 행 INSERT(외래키·NOT NULL) 0.716 → 0.718, 0.704 → 0.709 ms, `MAX(f)` 1.424 → 1.425 ms, USING 조인 4.944 → 4.891 ms. 세 번 평균에서 `COALESCE(f, d)`(+11%)와 `GREATEST(f, 0) + 1`(+10%) 한 행 조회가 튀어서(0.141~0.152 ms 근처의 흔들림) 두 서버를 나란히 띄우고 번갈아 여섯 번씩 다시 쟀더니 −6%와 +6%, 점조회 +0.4%, `SELECT 'a', 1` −5%로 흔들림 안이었다.
- **하지 않은 것**: Debug 빌드, 앱 화면의 자동 시험(화면은 손으로·CDP로 확인), 두 클라이언트가 동시에 `SET GLOBAL`을 바꾸는 경합 시험, 교차 조인을 멈추게 하는 것.

### 10월 31일 — R60: 호환성 점수판의 남은 차이 8개를 모두 메움 (`WITH ROLLUP`, `PI()` 표기, `->`의 왼쪽, 뷰의 열, `SHOW ENGINES`, `SHOW TABLE STATUS`의 크기, 서버 버전, 변수 열의 타입)

**왜 이 항목인가**: 사용자가 MySQL 호환성 화면에서 100%가 아닌 것을 물었고, 코퍼스 4개(`tables` 1, `functions` 2, `protocol` 3, `protocol_types` 2)의 8문장이라고 답한 뒤 "점수판 전부 채우는건 어때"라고 했다. 사용자 결정이 필요한 둘은 물어서 정했다: **`SHOW ENGINES`는 MySQL과 똑같이 11행**(RuSQL은 `ENGINE=`을 받아서 무시하고 InnoDB 방식으로만 만드는데도 MyISAM·MEMORY 등이 `YES`로 보이게 됨), **버전은 `8.4.9`**.

**고친 것 (엔진·서버)**
- **`GROUP BY … WITH ROLLUP`** (없던 문법): 묶음이 끝난 뒤 키의 접두사마다 한 행(남은 키는 NULL)과 모든 행에 대한 마지막 총계 행을 낸다. 진짜 MySQL 8.4.9에 같은 문장을 먼저 던져 규칙을 쟀다 — 묶음은 키 순서(NULL이 앞)로 놓이고 각 접두사의 행이 그 묶음 뒤에 따라붙는다(`(1,1) (1,2) (1,NULL) (2,1) … (NULL,NULL)`); 총계 행의 `AVG`·`COUNT(DISTINCT)`는 합쳐진 행들 위에서 계산하고, 키가 진짜 NULL인 묶음은 롤업 행이 아니며(`(3,NULL)`이 두 번 나옴), `HAVING`은 롤업 행에도 걸리고, `ORDER BY`·`LIMIT`·`DISTINCT`는 그 뒤에 오고, 입력에 행이 없으면 총계 행도 없다; `GROUP BY` 목록 뒤에 한 번만 쓸 수 있다(없거나 두 번이거나 `ORDER BY` 뒤면 문법 오류). 구현: `Statement::Select`에 `with_rollup`(AST·JSON·복사·이름 한정·`exec_select` 사슬 전체; 뷰에 저장되어 재시작 뒤에도 남음), 묶음을 만든 직후 키로 정렬하고 접두사가 끝나는 자리마다 "그 행들을 모은 묶음"을 끼워 넣는 것이 전부라서 집계·`HAVING`·윈도·`DISTINCT`는 그대로 쓴다. **시험을 쓰다 MySQL과 다른 두 가지를 더 찾아 맞췄다**: ① 롤업 아래에서는 **기본키가 묶음에 있어도 다른 열이 따라온다고 보지 않는다**(`SELECT id, g, COUNT(*) FROM t GROUP BY id WITH ROLLUP`이 MySQL은 `ONLY_FULL_GROUP_BY` 오류 1055: 롤업 행에서는 키가 NULL이라서; 롤업이 없으면 그대로 됨), ② 키가 아닌 열(`ANY_VALUE(x)`)은 묶음에서는 **첫 행**의 값이고 롤업 행에서는 NULL이 아니라 **그 행들의 마지막 행** 값이다(MySQL로 세 번 재서 정함: 총계 행의 `ANY_VALUE(id)`가 마지막 행의 6). **`GROUPING()`은 아직 없다**(목록에 남김).
- **`PI()`가 `3.141593`**: MySQL은 `PI()`에 소수 여섯 자리를 고정해 보인다(값은 전체 자릿수여서 `DEGREES(PI())`는 180). select 목록에 홀로 선 `PI()`만 여섯 자리로 보이게 했다. **남은 것**: MySQL은 `PI()`에서 파생된 식(`PI() + 0`, `PI() * 2`)도 여섯 자리 고정으로 보이는데 여기는 전체 자릿수다(고정 자릿수가 식으로 전해지는 규칙이 없음).
- **`->`·`->>`는 열 뒤에서만**: `'{"a": 5}' -> '$.a'`처럼 문자열·수·함수 결과(`->`를 이은 것도) 뒤의 화살표는 MySQL이 문법 오류인데 여기는 받아서 값을 냈다(더 너그러운 쪽). 이제 문법 오류.
- **`information_schema.columns`가 뷰의 열도 낸다**(MySQL은 낸다; 이름을 안 붙인 열은 식의 글자 `COUNT(*)`·`id + 1`로): 뷰가 그대로 가져오는 열은 그 열의 타입·NOT NULL로, 계산한 열은 TEXT로(선언된 타입이 없음). `view_column_defs`(새 헤더 `engine/view_columns.hpp`).
- **`SHOW ENGINES`·`information_schema.engines`**가 MySQL 8.4의 11행을 글자 그대로 낸다(두 곳이던 목록을 하나로: 프로토콜의 `SHOW ENGINES`는 이 표를 읽음). InnoDB의 `XA`가 `YES`가 된 것이 가장 큰 거짓이다 — XA 문장은 없다. `SQL.md`에 적었다.
- **`SHOW TABLE STATUS`·`information_schema.tables`의 크기**: `Avg_row_length`·`Data_length`·`Index_length`가 전부 0이었다. InnoDB가 세는 16 KB 쪽(page) 수를 행에서 추정한다 — 진짜 MySQL 8.4에서 0 ~ 100,000행 표를 재서 맞췄다: 레코드는 머리 5 + 숨은 열 13 + 쪽 디렉터리 2 + NULL 비트맵 + 값(수·날짜는 타입의 크기, 글자는 길이 + 길이 바이트), 쪽당 15/16, 위쪽 쪽 하나, **32쪽을 넘으면 64쪽 단위(extent)로 자람**(20,000행 = 97쪽, 100,000행 = 289쪽이 진짜와 같음); 보조 인덱스는 한 쪽에 들어가는 동안 1쪽, 넘으면 쪽의 60% 채움으로 센다(5,000행에서 진짜와 같고 100,000행에서도 같음; 중간 크기는 몇 % 어긋남). 유일 키·외래키 열도 인덱스로 센다(MySQL은 외래키 열에 인덱스를 만든다). `Avg_row_length`는 `Data_length / Rows`. MySQL이 이 값을 하루 동안 캐시하므로 정확한 일치는 작은 표에서만 말할 수 있다. **행이 500개를 넘는 표는 표본(n번째 행마다 500개)으로 잰다** — 앱의 사이드바가 표 목록을 `information_schema.tables`로 만들어서 서버의 모든 표를 매번 훑는데, 처음에는 모든 행을 읽어 이 조회가 0.18 → 1.05 ms로 5배 느려졌다(표 두 개, 각 5,000행); 표본으로 바꿔 0.28 ms.
- **서버 버전 `8.4.9`**: `VERSION()`, `@@version`, MySQL 프로토콜의 접속 인사말, `SHOW VARIABLES`. `@@version_comment`는 `RuSQL` 그대로(MySQL은 `MySQL Community Server - GPL`). 진짜 `mysql` 8.4 클라이언트와 pymysql로 접속해 확인했다.
- **화면의 "알려진 차이" 목록을 다시 썼다** (`CompatView.tsx`): 낡아서 이미 같아진 다섯 가지(같은 행의 `REPLACE`, DATE 열의 `SUM`·`AVG`, `f = 0.1`, 갱신하는 표를 읽는 하위 질의, UNIQUE 밀어 옮기기)를 아직 "다르다"고 적고 있었다 — 진짜 MySQL과 다시 재서 모두 같음을 확인하고 지웠고, 지금 남은 여덟 가지(`GROUPING()`, `PI()`에서 만든 식, 글자 함수의 FLOAT, 이어지는 USING의 모호한 열, `SHOW TABLE STATUS`의 시계 열, `SHOW ENGINES`가 보이는 것과 실제의 차이, `long_query_time`의 세션 값, 집계가 섞인 뷰를 만들 때의 거절)를 각각 MySQL로 확인해서 적었다.
- **변수를 읽는 열의 타입** (이 비교로 찾음, 아홉 번째 차이): `SELECT @@autocommit`이 MySQL은 정수 열인데 여기는 글자 열이었다. 8번째 차이(`VERSION()`)에 가려져 있다가 값이 같아지자 드러났다. 변수 열은 값이 수면 수로 타입을 정한다.

**코퍼스를 바꾼 것 (점수판을 정직하게 읽으려면 알아야 함)**
- `protocol.sql`의 `SHOW TABLE STATUS LIKE 'acct'`는 **`Create_time`·`Update_time`이 서버마다 다른 시계라 영원히 같을 수 없어서** 그 두 열을 뺀 `information_schema.tables` 조회(`SHOW`가 읽는 것)로 바꾸고, `SHOW TABLE STATUS`는 없는 표 이름으로 열 이름만 비교한다. RuSQL의 두 열은 NULL이다.
- `functions.sql`의 한 문장에서 `'{"a": 5}' -> '$.a'`를 떼어 따로 두고(JSON 함수들은 그대로 비교), `->`는 JSON 열로 쓴 것과 틀린 모양들(문자열·수·함수 뒤, 이은 것)로 따로 비교한다.
- 새 코퍼스 `rollup` 39문장(진짜 MySQL과 **차이 0**, 둘 다 거절 4). 변한 `tables`·`functions`의 C++ 적합성 케이스를 다시 뽑았고 `rollup`은 `mysql_conformance_rollup.inc`로 새로 넣었다.

**검증**
- 새 C++ 시험 `[r60]` 9 케이스(롤업: 순서·두 키·`HAVING`·`ORDER BY`·`LIMIT`·`DISTINCT`·`COUNT(DISTINCT)`·식 키·빈 입력·한정된 이름·별칭·조인·파생 표·`UNION` 양쪽·뷰와 **재시작 뒤**·3,000행에서 세 키의 총계가 접두사별 묶음과 같은지·문법 오류, `PI()`, `->`, 뷰의 열, 엔진 목록, 크기(진짜 MySQL이 낸 숫자: 0·1·7행, 유일 키, 500·1,000·5,000·20,000행, 인덱스가 있는 5,000행, 외래키 열), 버전, 변수 열의 타입).
- 진짜 MySQL 8.4.9와 비교: `tables` 699문장·`functions` 72·`protocol` 108·`protocol_types` 30·`rollup` 39·`errno` 49 모두 **차이 0**(처음 8개가 달랐고, 고친 뒤 하나가 더 드러났다).
- 전체 시험 **860 케이스 / 1,511,281 assertions**(Release 기본 1,511,280, `RUSQL_DML_INDEX_MIN_ROWS=0` 1,511,281, `RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0` 1,511,281) 통과. 호환성 점수판: **코퍼스 42개, 문장 3,474개 중 차이 0**(R59: 41개 / 3,429개 / 8개).
- **심은 버그 50종 중 49종이 잡힘**: 처음 44종 중 넷이 살아남았다 — 바깥 질의의 `WITH ROLLUP`이 파생 표 위에서·하위 질의 안에서 플래그를 잃는 둘, 뷰의 열이 식의 글자 이름을 잃는 것, 롤업 행의 다른 표기의 키. 그 넷을 잡는 시험을 더하다 위의 롤업의 두 차이를 찾아 고쳤다. 표본 크기를 바꾸는 심은 버그 하나만 **살아남았다** — 몇 행을 보든 답이 "추정"이라 시험이 그 값을 못 박지 않는다(행 수 곱셈을 빼먹는 둘은 잡힘).
- 크래시 퍼저 6종(`fuzz_crash` 15, `fuzz_crash_fk` 15, `fuzz_concurrent` 8, `fuzz_crash_selfref` 15, `fuzz_crash_chain` 24, `fuzz_crash_multifk` 40)과 무작위 대조 12종: 불일치·위반 0, R59 빌드와의 차분(`diff_builds`, 문장 1,500개씩 시드 둘) 차이 0. (이 셋은 마지막에 표 크기를 표본으로 바꾸기 **전** 빌드로 돌렸다; 그 변경은 `information_schema`의 크기 열만 건드리고, 그 뒤 전체 시험·점수판·A/B를 다시 돌렸다.)
- 실서버 A/B(R59 빌드 대 새 빌드, 두 서버를 나란히 띄워 번갈아 여섯 번씩): 점조회 0.073 → 0.075 ms, 표 없는 SELECT 0.066 → 0.065 ms, 함수 열 0.077 → 0.076 ms, GROUP BY(5,000행) 0.087 → 0.079 ms — 흔들림 안. `information_schema.tables`만 0.18 → 0.28 ms(위의 표본 크기 계산).
- **앱 화면은 보지 않았다**: 화면이 바뀐 것은 Compat 페이지의 알려진 차이 글과 점수판 숫자뿐(`tsc` 통과).
- **하지 않은 것**: `GROUPING()`, `PI()`에서 파생된 식의 여섯 자리, 계산한 열을 가진 뷰의 타입, Debug 빌드, 10만 행을 넘는 표의 크기.

### 11월 1일 — R61: B+Tree·MVCC·MySQL 호환성 페이지를 Diagram 페이지처럼 (공용 `PageKit`)

**왜 이 항목인가**: 사용자가 세 페이지(B+Tree 애니메이션, MVCC 시뮬레이션, MySQL 호환성)가 Diagram 페이지나 홈 화면보다 밋밋하다며 "가장 이쁘고 깔끔한 UI … 애니메이션처럼 다 이쁘게 꾸미고 싶습니다. 일단 한 번 해보세요"라고 했다. 엔진 변경 없음, 화면만.

**한 것**
- **공용 부품 `PageKit.tsx`·`PageKit.css`** (Diagram 페이지의 말 그대로): 아이콘 배지가 숨 쉬는 **머리 영역**(페이지 색의 번지는 빛, 제목, 설명, 한국어/EN 토글), 위로 떠오르며 차례로 나타나는 상자(`pk-rise`, `--d`로 순서), 마우스를 올리면 떠오르고 제 색으로 빛나는 **카드**, 번호 배지의 퍼지는 파동, 눌리고 빛나는 **버튼**, 초점이 가면 제 색의 고리가 생기는 **입력**, 숫자가 0에서 올라가는 `CountUp`. 페이지마다 Diagram의 층 색을 하나 받는다: B+Tree = 저장소 분홍, MVCC = 트랜잭션 호박색, 호환성 = 서버 청록. `prefers-reduced-motion`이면 움직임을 끈다.
- **B+Tree**: 점 격자 판 위에 트리를 그린다; 노드는 그라데이션(잎은 초록, 안쪽은 보라) · 지금 단계가 가리키는 노드는 단계 색으로 **숨 쉬듯 빛나고** · 새로 생긴 노드는 **톡 튀어나오고** · 탐색 경로의 간선은 **밝은 점선이 흘러가고** · 잎끼리 이은 화살표도 흐른다; 단계 설명 줄은 단계 색의 배지·맥박 점·**차오르는 진행선**·키 수/층 수가 올라가는 알약; 트리 그림을 웅장하고 단정하게 다시 그림(노드는 모서리가 각진 직사각형 52×48을 납작하게 — 노드 종류의 색(**루트 금색, 안쪽 노드 보라, 잎 청록**, 다이어그램 페이지의 층 색)을 섞은 어두운 바탕, 그 색의 선명한 윤곽선과 은은한 빛, 흰 글자는 앱의 Segoe UI 20px(굵기 600, 숫자 폭 고정; 처음의 그라데이션·윗줄·모노스페이스 글꼴은 안 예뻐서 뺌); 가지는 부모의 키와 키 사이의 아래 가장자리에서 자식의 윗면 가운데로 곧게 뻗는 **직선**(처음에 있던 연결 점은 사용자 요청으로 뺌)(자식이 바로 아래가 아니면 대각선; 직각선은 마음에 들지 않아 되돌림); 층마다 은은한 띠와 "루트 / 안쪽 노드 / 잎" 이름표; 판 너비보다 좁은 트리는 최대 1.6배까지 커지고 넓은 트리는 0.62배까지 줄여 판에 맞추며 그 아래에서만 가로 스크롤, 판 안에서 세로로도 가운데); 빈 트리에는 둥실거리는 안내; 재생 줄은 둥근 버튼과 색 있는 슬라이더; 범례는 알약; 맨 아래에 네 가지 개념 카드(아래로 걷기·쪼개기·빌리기와 합치기·이어진 잎).
- **MVCC**: 세션 A(파랑)·B(분홍)가 각자의 색을 가진 카드(트랜잭션이 열려 있으면 켜지고, 잠금을 기다리면 금빛으로 깜박임); 시나리오 표시줄에 **단계마다 점 하나**(끝난 단계는 세션 색으로 차고 지금 단계는 맥박); 설명 상자는 옆에서 미끄러져 들어옴; 가운데 버전 표는 **새 버전 행이 떠오르며 한 번 번쩍**, UNCOMMITTED·ENDING 알약은 숨 쉬며 빛나고, A·B가 보는 버전은 제 색으로 빛나는 점.
- **MySQL 호환성**: 위쪽에 **링 게이지**(원이 차오르고 가운데 퍼센트가 올라감)와 여섯 개의 숫자 카드(각자 색, 숫자가 올라감); 코퍼스 줄은 **막대가 왼쪽에서 자라고**(줄마다 조금씩 늦게) 막대 위로 빛줄기가 훑고 지나가며 완전하면 ✓ 배지가 번갈아 파동, 차이가 있으면 붉은 줄과 `!`; 알려진 차이는 번호 붙은 두 열의 카드.

**확인**: 앱을 디버그 포트로 움직이며 **Before/After 스크린샷**을 비교했고, 애니메이션 중간 프레임(링이 88.5%까지 차오르는 순간, 카드가 떠오르는 순간)과 B+Tree 1–12 삽입, MVCC 시나리오 다섯 단계를 직접 돌려 보았다. 세 페이지 모두 한국어/EN 토글이 제목을 바꾼다. `tsc`와 `vite build` 통과, 프론트 순수 로직 시험 17개 통과(바뀐 곳은 화면뿐).

**하지 않은 것**: 화면의 자동 시험, 좁은 창(1100px 아래)과 아주 큰 트리의 모양 확인, `prefers-reduced-motion` 동작 확인, 홈 화면·서버 매니저·ERD 같은 다른 화면에 같은 틀을 입히는 것(`RECOMMAND.md`에 올림).

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
