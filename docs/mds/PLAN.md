# RuSQL 개발 로드맵 — 우선순위별 정리

> **참고 (2026-07-08):** 이 문서는 원본 Rust 구현(`old/`)을 분석한 시점의 기록이라, 파일:라인 근거가
> 전부 `.rs` 파일 기준입니다. RuSQL은 이후 C++(`code/backend`)로 전체 포팅되었고, "충실한 포팅"
> 원칙에 따라 여기 적힌 버그 대부분이 C++ 쪽에도 의도적으로 그대로 남아있을 가능성이 높지만,
> 파일:라인 번호 자체는 C++ 코드에 맞게 재검증되지 않았습니다.
>
> **업데이트 (2026-07-09):** 섹션 A의 엔진 쪽 P0 8개 전부 C++에서 실제 재현 확인 후 수정·회귀
> 테스트 추가 완료 (아래 ✅ 표시). UI 쪽 P0 2개(App.tsx)와 섹션 B~I는 아직 미착수.
>
> **업데이트 (2026-07-08 세션 연속):** UI로 `test_full.sql`/`test_full-ver2.sql`을 직접 돌려보며
> 실사용 테스트를 하던 중, 아래 P1 "BEGIN...END 내부 세미콜론으로 프로시저 조기 실행" 항목이 C++
> 서버(`code/backend/server/src/main.cpp`)에도 실재함을 확인하고 수정 완료 (✅ 표시).
>
> **업데이트 (같은 세션, 계속):** 섹션 A의 UI 쪽 남은 P0 2건(`App.tsx` 셀 편집)도 수정 완료. 검증
> 과정에서 엔진 자체의 복합 PK UPDATE 버그(PLAN.md에 없던 신규 발견, 위 표 참고)를 찾아 plain UPDATE
> (`executor_update.cpp`)에서 먼저 수정했고, 이어서 같은 "첫 PK 컬럼만 사용" 패턴이 다중 테이블
> UPDATE/DELETE(`executor_multi.cpp`)와 MERGE(`executor_merge.cpp`)에도 있는 걸 확인해 전부 동일한
> 방식(복합 인덱스와 같은 `\x00` 결합 키)으로 수정.
>
> **업데이트 (같은 세션, 계속):** 섹션 C·D의 남은 P0 5건도 전부 수정 완료 — WAL 커밋 fsync
> 무동작(C++ 신규 회귀, Rust엔 없던 버그), 테이블/스키마 파일 fsync+원자적 쓰기, 크래시 복구의
> 보조/복합 인덱스 미갱신, 인증 fail-open, BACKUP/RESTORE 경로 미검증(+ 그 수정 과정에서 발견한
> `_backups` 디렉터리가 실제 DB로 오인되던 부수 버그).
>
> **업데이트 (같은 세션, 계속):** 사용자 요청으로 C++ 포팅 전체를 원본 Rust와 대조하는 별도 감사를
> 서브에이전트로 진행 — 버퍼풀/페이지/해시인덱스/복합인덱스/락매니저/btree/group_commit/트랜잭션
> 매니저(rollback/savepoint/checkpoint)/CLI·클라이언트는 전부 원본과 일치 확인(신규 이슈 없음).
> 2건의 신규 회귀 발견 후 수정: (1) 위 fsync 수정이 남긴 갭 — fwrite/fflush/fsync 실패를 확인 안
> 하던 것(섹션 C에 추가), (2) MySQL 바이너리 프로토콜의 COM_STMT_EXECUTE에서 FLOAT/DOUBLE
> 파라미터가 `std::to_string`의 고정 6자리 반올림으로 정밀도 손실되던 것(섹션 F에 추가, 직접 구현한
> MySQL 프로토콜 클라이언트로 라이브 재현·수정 확인).
>
> **업데이트 (같은 세션, 계속):** UI로 실사용 테스트 하던 중 벤치마크 실행 중 서버가 실제로
> `abort()`로 크래시하는 걸 발견 — 근본 원인(커넥션 핸들러 예외 안전장치 부재)을 찾아 서버/CLI
> 양쪽에 try/catch 추가(섹션 C에 추가). 그 외 UI 쪽에서: (a) `code/test/perf/bench.py`가 콘솔
> cp949 코드페이지에서 em dash(—) 출력 시 크래시하던 것 수정 + UI가 실제로 쓰는 4개 항목만
> 측정하도록 트리밍, (b) UI가 항상 Debug 빌드 `engine_server.exe`를 띄우고 있었던 것을
> Debug/Release 중 더 최근에 빌드된 쪽을 자동 선택하도록 수정(Release 대비 9~22배 느려서
> "Rust보다 훨씬 느리다"는 오해의 원인이었음, Release로 전환 후 정상 범위 확인), (c) Server
> Manager의 ACTIVITY LOG가 서버 시작 메시지 1건 외엔 아무것도 기록 안 하던 것을 발견해 매 쿼리
> 실행 결과를 기록하도록 추가, (d) Connect to Server 폼의 레이아웃 오버플로 CSS 버그 수정.
> 세션 종료 전 `test_full.sql`/`test_full-ver2.sql` 재실행 + `engine_tests.exe` 전체 재확인 —
> **3276/3276 통과** (Debug/Release 둘 다).
>
> **업데이트 (같은 프로젝트, 새 세션):** `legacy/`(원본 Rust 참고 소스)를 `code/legacy/`로 이동 후
> — 원본 전체가 `old` 브랜치(원격 `origin/old`에도 push됨)에 그대로 남아있음을 확인하고 — 완전
> 삭제. `code/data/`의 빈 임시 디렉터리·테스트 잔여물 정리 + 저장된 연결에 속하지 않는 데이터
> 디렉터리를 앱 시작 시 자동 정리하는 기능 추가(UI, Tauri). 이어서 `mysql` CLI 실접속과 MCP 서버를
> 실제 클라이언트 프로토콜(stdio JSON-RPC)로 점검하다 신규 버그 2건 + MCP 설계 결함 1건 발견해
> 전부 수정(섹션 F·I에 추가): SHOW INDEX 하드코딩/PK 누락, SUM/COUNT(비교식) 파싱 불가, MCP의
> UI 제어형 도구 9개가 원래부터 미작동이던 것 제거. 회귀 테스트 3건 추가 후 전체 244 테스트 케이스
> 3292 assertion 통과 확인.
>
> **업데이트 (같은 세션, 계속):** 사용자가 섹션 B(동시성 아키텍처, "가장 가치 있는 구조 개선")를
> 지목 — 코드 조사 후 완전한 MVCC 재설계 없이 실질적 이득을 낼 수 있는 범위로 계획을 잡고(Plan
> Mode로 사용자 승인 받음) 진행. 핵심: `SharedDatabase`를 감싸는 `RwLock`이 이미 진짜
> `std::shared_mutex`인데도 SELECT 포함 모든 문장이 exclusive write lock을 잡던 것을,
> `is_pure_read_only()` 분류기로 진짜 읽기 전용 문장만 `shared->read()`로 동시 실행하도록 수정
> (WHERE/HAVING/JOIN-ON에 중첩된 서브쿼리까지 재귀 검사하는 게 핵심 — 안 그러면 파생테이블을 숨긴
> 서브쿼리가 새 동시-읽기 경로에서 `s.tables`를 조용히 mutate하는 레이스가 생김). 이 과정에서
> 전에는 전역 락이 사실상 보호막이던 `BufferPool`에 진짜 스레드 안전성이 필요해져 자체 뮤텍스 추가.
> 덤으로, 이 수정 이후에도 여전히 write-분류로 남는 프로시저 루프/트리거 재귀에 상한이 없어 한
> 클라이언트의 무한루프가 서버 전체를 영구 정지시킬 수 있던 것도 같이 수정(WHILE/LOOP/REPEAT 10만
> 회 상한, 트리거 재귀 32단 상한). `test_concurrency.cpp` 신규(실제 스레드로 동시 SELECT/쓰기
> 스트레스 테스트) + Part B 회귀 테스트 3건 추가, 전체 249 테스트 케이스 3352 assertion 통과.
> 실제 서버로 대용량 풀스캔 SELECT 진행 중 다른 세션의 `SELECT 1`이 1.06초 대신 0.0002초 만에
> 응답하는 것으로 실측 확인. 섹션 B의 나머지 항목(트랜잭션 스냅샷 deep-clone, 버퍼풀 read-cache,
> ASCII 표 재파싱, 조인 카디널리티)은 동시성과 무관하거나 훨씬 큰 별도 작업이라 의도적으로 미착수.
>
> **업데이트 (2026-07-19):** 섹션 E "재귀 CTE 1000회 상한 무경고 종료", 섹션 A "SELECT USER()가
> 항상 root@localhost", 섹션 G "파괴적 작업 확인 절차 없음" 3건 수정 완료(위 ✅ 표시).
> USER() 수정 과정에서 파서가 `SELECT USER()` 자체를 못 받아들이던 갭과, USER()가 세션별로
> 달라진 이후 쿼리 캐시가 여전히 이를 비결정 함수로 취급 안 해 다른 세션의 캐싱된 값을 반환하던
> 갭 2건을 자체 회귀 테스트로 추가 발견해 같이 수정. 검증 중 `engine_cli`/`engine_server`
> Release 바이너리 일부가 마지막 소스 변경보다 먼저 빌드된 상태(불완전한 이전 빌드)로 남아있던
> 것도 발견 — 전체 타겟 Debug/Release 재빌드 후 `engine_tests.exe`(256 테스트 케이스, 16911
> assertion, 양쪽 구성 모두 통과) + `test_full.sql`/`test_full-ver2.sql` 재실행(기존에 알려진
> 오류만 재현, 신규 회귀 없음) + 실제 `mysql` CLI·`engine_client`(서로 다른 두 계정)로 라이브
> 재검증 완료.
>
> **업데이트 (2026-07-22):** 남은 P1 10건을 사용자와 함께 실제 위험도 기준으로 재검토해 "꼭
> 필요" 5건만 진행(나머지는 문서-동작 불일치 정도이거나 특정 조건에서만 발동하거나 이미 해소된
> 것으로 판단해 제외) — 섹션 A "SET 문 무조건 OK", 섹션 C "WAL/Undo 비원자적 재작성", 섹션 C
> "체크포인트-그룹커밋 TOCTOU", 섹션 G "Tauri 컴파일타임 경로 하드코딩", 섹션 G "MCP 설정 병합
> 실패 시 덮어씀" — 5건 전부 수정 완료(위 ✅ 표시, PLAN.md 파일:라인 근거가 전부 포팅 이전 Rust
> 기준이라 이번엔 C++/Rust(Tauri) 현재 코드를 직접 재확인한 내용으로 서술). WAL/Undo 원자적
> 쓰기는 `disk.cpp`가 테이블 파일에 이미 쓰던 검증된 `.tmp`+fsync+원자적rename 패턴을 공유
> 헤더로 추출해 재사용, 체크포인트 TOCTOU는 `active_txn_ids` 삭제 시점을 실제 fsync 완료 후로
> 이동. 각 항목 구현 직후 개별 회귀 테스트 추가(`test_txn.cpp` 3건, `test_executor_txn.cpp` 2건,
> Tauri `#[cfg(test)]` 2건 신규) + 라이브 검증(Tauri는 실제 Release 빌드를 그 자리에서 실행해
> 배포 시나리오 확인, MySQL SET은 실제 `mysql` CLI로 `SET @x=5`가 실제 반영되는 것과 잘못된
> ISOLATION LEVEL이 이제 진짜 에러를 내는 것 확인). 전체 마무리: Debug/Release 전체 타겟
> 재빌드 → `engine_tests.exe` **261 테스트 케이스 / 16936 assertion, 양쪽 구성 모두 통과**
> (256/16911에서 5건 증가) → `test_full.sql`/`test_full-ver2.sql` 재실행(신규 회귀 없음).
>
> **업데이트 (2026-07-22 ~ 2026-08-05, MVCC 실제 재설계 세션):** 사용자가 "진짜 MVCC + XA"를
> 물어봐 MVCC부터 단계적으로 진행하기로 하고(XA는 이후로 연기), Section B "트랜잭션 스냅샷이 DB
> 전체 deep clone"과 Section C "SERIALIZABLE이 phantom(행 개수)만 감지" 두 항목을 실제로 해결.
> **Stage 1~2**: `SnapshotCtx` 기반 실제 격리수준별 가시성(RU/RC/RR/Serializable이 이제 코드상
> 실제로 다르게 동작), UPDATE가 항상 새 물리 버전을 append, `session_tables` 세션 로컬 버퍼
> 방식 완전 삭제(모든 DML이 `s.tables`에 직접 실제 txn id로 태깅되어 기록), GC를 호라이즌
> 기반으로 교체. **Stage 3**: `validate_serializable`을 행 개수 비교에서 read-set 기반 재검증으로
> 교체(같은 개수·다른 내용 인터리빙도 감지) — 이 과정에서 이제 완전히 죽은 `snapshot_` deep-clone
> 필드 자체를 삭제해 Section B의 그 항목도 함께 해결(O(DB 크기)→O(1)). **Stage 4**: "읽기 전용만
> 동시, 그 외 전부 배타"였던 락을 구조적 락(DDL 등)+테이블별 `shared_mutex` 2계층으로 재설계해
> 서로 다른 테이블에 대한 쓰기도 실제로 병렬 실행되도록 확장 — 실제 멀티스레드 스트레스 테스트로
> 데이터 레이스 2건(테이블-키 맵 지연초기화 레이스, UNION/EXPLAIN 테이블 락 누락) 발견해 수정.
> **Gap Lock**: Section H에 P3로 남아있던 항목 — InnoDB 스타일로 `FOR UPDATE`/`FOR SHARE`·
> 트랜잭션 내 UPDATE/DELETE가 PK 범위를 잠가 동시 INSERT phantom을 차단하도록 구현(단일 컬럼 PK
> V1 범위), 검증 중 INSERT측 PK 컬럼 판정 버그(복합 PK 전용 필드를 잘못 사용해 흔한 인라인 PK
> 테이블에서 항상 스킵되던 것) 발견해 수정. 상세 내역은 새로 추가된 Section B의 3개 행(MVCC
> Stage 1~2/Stage 4/Gap Lock)과 Section C의 갱신된 SERIALIZABLE 행 참고. **아직 의도적으로 보류**:
> 행 단위 완전 동시 쓰기(Stage 4는 테이블 단위까지만), 잠금 대기가 진짜로 블로킹되는 방식(현재도
> 여전히 "즉시 충돌/데드락 감지", sleep 없음 — Section C "Lock wait timeout이 실제로 대기 안 함"
> 항목 그대로), Postgres 수준 완전한 SSI(predicate lock 기반, 잠금 없는 SELECT의 phantom까지
> 방지), XA 분산 트랜잭션 — 4가지 모두 사용자와 논의 후 "필수 아님" 판단, 다음 세션으로 이월.
> 전체 마무리: Debug/Release 전체 타겟 재빌드 → `engine_tests.exe` **284 테스트 케이스 / 17372
> assertion, 양쪽 구성 모두 통과** → `test_full.sql`/`test_full-ver2.sql` 재실행(신규 회귀 없음)
> → 실제 서버 + Python 소켓 스크립트(네이티브 프로토콜 챌린지-응답 인증 재사용, `threading`으로
> 진짜 동시 연결)로 각 단계 라이브 검증.

**P0**=정확성/무결성 직결(즉시 수정) · **P1**=핵심 아키텍처/보안 · **P2**=성능/호환성/사용성 · **P3**=장기 확장·정리

## A. 데이터 무결성 버그 (즉시 수정 권장)

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| ✅ 완료 | VACUUM이 PK 인덱스를 손상 | 원인(Rust `executor.rs:7679,8105`): `row.values().next()`로 PK값을 가져옴(HashMap 순서 무작위). C++ `executor_maint.cpp`/`executor_dml.cpp`에서 스키마의 실제 PK 컬럼을 조회하도록 수정, `test_btree.cpp`/`test_executor_dcl.cpp`에 회귀 테스트 추가 | PK 조회·O(1) DELETE·중복검사가 항상 정확 | 낮음 |
| ✅ 완료 | WHERE 우변 산술식 미지원→오답 | 원인(Rust `parser.rs:900-984`): `WHERE v > id + 100`에서 `+100`이 버려져 `v > id`로 잘못 평가(실증 확인). C++ `ast.hpp`에 `ConditionValue::Arith` 추가, `parser_expr.cpp`에서 우변도 좌변과 동일하게 산술식으로 파싱(단순 값은 기존 Literal로 축약해 플래너 인덱스 선택 호환 유지); 상관 서브쿼리 쪽(`executor_subquery.cpp`/`executor_eval.cpp`)도 같이 수정 | WHERE 절 결과 신뢰성 확보 | 중간 |
| ✅ 완료 | 파서가 문장 끝 잔여 토큰 미검증 | 원인(Rust `parser.rs:338`): 전체 토큰 소비 확인 안 해 `...))) garbage`도 조용히 실행(실증 확인). C++ `parser_core.cpp`의 최상위 `parse()`에만 검증 추가(재귀 호출은 그대로 유지); 이 과정에서 발견된 테스트 스플리터 버그(`test_parser.cpp`)와 `test_full-ver2.sql`의 실제 문법 오류 2건도 같이 수정 | 잘못된 SQL을 명확한 에러로 표시 | 낮음 |
| ✅ 완료 | B+Tree가 숫자형 문자열 키를 충돌 | 원인(Rust `btree.rs:34-38`): "007"과 "07"이 둘 다 7.0으로 파싱돼 동일 키 취급(실증: 삽입 오류). C++ `btree.cpp`의 `cmp_keys`에 동일-문자열 우선 체크 + 숫자값 동일 시 문자열 비교로 tie-break 추가 | 우편번호·SKU 등 문자열 PK 안전 지원 | 중간 |
| ✅ 완료 | 복합 인덱스가 숫자를 텍스트로 정렬 | 원인(Rust `composite_index.rs`): `\x00` 결합 키라 숫자 인식 실패, "10"<"9"로 정렬. C++ `btree.cpp`의 `cmp_keys`가 `\x00`로 세그먼트 분리 후 컬럼별로 숫자 인식 비교하도록 수정(멀티컬럼 ORDER BY와 동일한 방식) | 복합 인덱스 범위/정렬 쿼리 정확도 | 중간 |
| ✅ 완료 | 쿼리 캐시가 비결정 함수 영구 캐싱 | 원인(Rust `executor.rs:686-759`): NOW()/RAND()/UUID() 포함 SELECT가 무효화 없이 과거값 반환. C++ `executor_core.cpp`에 단어경계 인식 비결정 함수 탐지(`contains_nondeterministic_func`) 추가해 캐시 저장 단계에서 제외 (`brand` 같은 컬럼명 오탐 방지 확인됨) | 시간·난수 쿼리 정확성 | 낮음 |
| ✅ 완료 | 이중 단항 마이너스 파싱 오류 | 원인(Rust `parser.rs:1039-1051`, C++에선 근본 원인이 lexer): `-` 바로 뒤에 숫자가 오고 직전 토큰이 값이 아니면 렉서가 음수 리터럴로 접어버림 — `- -5`의 두 번째 `-5`가 이미 음수 토큰이 되어, 파서가 거기에 또 `-`를 붙여 "--5" 문자열 생성(실증 확인). C++ `parser_expr.cpp`의 4개 지점(단항 마이너스/IN 리스트/BETWEEN/INTERVAL) 모두 부호 토글 방식(`negate_number_text`)으로 수정 | 산술식 파싱 신뢰성 | 낮음 |
| ✅ 완료 | RENAME COLUMN 후 인덱스 무효화 | 원인(Rust `executor.rs:5863-5882`): 인덱스 메타 미갱신. 실제로는 문서 설명(SeqScan 전락)보다 심각해서, C++에서 재현해보니 **PK 인덱스 기반 조회가 이름이 바뀐 컬럼을 빈 값으로 반환**하는 정확성 버그였음(캐시된 인덱스 JSON이 예전 컬럼명 그대로). C++ `executor_ddl.cpp`에서 PK/보조/해시/복합 인덱스 전부 재구축 + 메타데이터 갱신하도록 수정 | 스키마 변경 후 인덱스 성능 유지 | 중간 |
| ✅ 완료 | UI 셀 편집 — PK값 미이스케이프 | 원인(`App.tsx:1338-1352`): WHERE절 pkValue를 항상 숫자로 취급, 문자열 PK 편집 시 조용히 실패. `ColumnDetail.data_type` 기준으로 숫자 타입일 때만 비따옴표 처리하는 `quoteForColType` 헬퍼 추가, WHERE절에 적용 | 모든 PK 타입에서 셀 편집 동작 | 낮음 |
| ✅ 완료 | UI 셀 편집 — 복합 PK 미지원 | 원인(`App.tsx:1327`): 첫 PK 컬럼만 사용, 복합 PK 테이블에서 조건 불충분(다중 행 오업데이트 위험). `cols.find` → `cols.filter`로 PK 컬럼 전부 수집해 `pkCols` 배열로 저장, WHERE절을 AND로 전부 결합하도록 수정 | 복합 PK 테이블 안전 편집 | 중간 |
| ✅ 완료 | (신규 발견) UPDATE/MERGE가 복합 PK 첫 컬럼만으로 행 식별 | 원인(Rust `code/legacy/rusql-core/src/engine/executor.rs:4960-4988`, 이식 시 그대로 보존): 단일 테이블 `UPDATE`(`exec_update_inner`), 다중 테이블 `UPDATE`/`DELETE`(`executor_multi.cpp`), `MERGE`(`executor_merge.cpp`) 전부 대상 행을 PK컬럼 중 첫 번째 값만으로 식별해, 복합 PK 테이블에서 `WHERE a=1 AND b=1`이 `a=1`인 행 전부를 잘못 건드림(UPDATE의 RETURNING, 다중 UPDATE/DELETE, MERGE 모두 동일 버그 확인). 위 두 UI 수정을 검증하다 CLI로 직접 재현해 발견 — UI가 완벽한 복합 WHERE절을 보내도 엔진이 내부적으로 더 많은 행을 건드리는 상태였음. 3개 파일 전부 PK 컬럼 전체를 `\x00`로 결합한 복합 키(복합 인덱스와 동일한 컨벤션)로 행 매칭 로직을 수정; 락/undo-log/PK 인덱스 갱신은 기존처럼 첫 PK 컬럼만 사용(엔진 전반의 기존 관례와 일관되게 유지, 별도 이슈). `test_executor_update_delete.cpp`(단일 UPDATE+RETURNING)·`test_executor_misc.cpp`(다중 UPDATE/DELETE, MERGE)에 회귀 테스트 6건 추가 | 복합 PK 테이블에서 UPDATE/DELETE/MERGE가 실제로 안전해짐 | 중간 |
| ✅ 완료 | SELECT USER()가 항상 root@localhost | 원인(Rust `mysql.rs:648-652`, 이식 시 그대로 보존): 실제 인증 사용자명 미반영. `Executor::auth_user`(기본값 `"root"`) 신규 추가 — native/MySQL 프로토콜 서버가 AUTH 성공 직후 실제 인증 사용자명을 대입, `sync_udf_context`(기존 `DATABASE()`용 `g_current_db_ctx`와 동일 패턴)로 스레드 로컬 `g_current_user_ctx`에 동기화해 `USER()`/`CURRENT_USER()`/`SESSION_USER()`/`SYSTEM_USER()` 스칼라 함수가 이를 반환하도록 수정. 검증 중 신규 버그 2건 추가 발견: (1) 파서 갭 — `TokenKind::User`가 `parser_select.cpp`의 SELECT 컬럼-리스트 스칼라 함수 인식 목록(`DATABASE()`는 이미 있던)에서 누락돼 `SELECT USER()` 자체가 파싱 실패(직접 작성한 회귀 테스트로 발견, mysql.cpp의 리터럴-문자열 호환 shim이 이 경로를 우회해 이전엔 안 드러남) — 인식 조건과 `fname` switch 양쪽에 추가해 수정. (2) 쿼리 캐시 비결정 함수 목록(`contains_nondeterministic_func`)에 `user`/`current_user`/`session_user`/`system_user`가 없어, USER()가 세션별로 달라지게 된 이후 두 세션이 동일 SQL 텍스트를 실행하면 먼저 캐싱된 세션의 값을 계속 반환하던 것(USER()를 동적으로 만든 직접적 부작용) — 4개 단어 추가해 수정. `mysql.cpp`의 `SELECT USER()` shim도 하드코딩 리터럴 대신 실제 파서 실행 결과로 교체(폴백은 유지). `test_executor_proc.cpp`에 회귀 테스트 추가, 실제 `mysql` CLI·`engine_client`로 서로 다른 두 계정(root/신규 생성한 bob)에 대해 라이브 검증 | 권한 확인 툴 오작동 방지 | 낮음 |
| ✅ 완료 | 모든 SET 문이 무조건 OK 응답 | 원인(Rust `mysql.rs:637-638`, 이식 시 그대로 보존): C++ `mysql.cpp`의 `mysql_compat`도 모든 `SET ...` 변형을 실행 없이 무조건 OK 처리 — native 프로토콜(`parser_dcl.cpp`의 `parse_set()`)이 실제로 지원하는 `SET @var = expr`/`SET ISOLATION LEVEL ...` 두 형태조차 MySQL 프로토콜에서는 조용히 씹히고 있었음. 두 형태만 실제 실행(`exec_inner`)으로 라우팅하도록 수정 — 그 외(SET NAMES/autocommit/SESSION/GLOBAL/PASSWORD 등 미지원 형태)는 기존처럼 호환용 OK 유지(실제 MySQL 클라이언트의 세션 초기화 잡담이 에러 없이 넘어가야 하므로). 실제 `mysql` CLI로 검증: `SET @x=5; SELECT @x;`가 실제로 5 반환(이전엔 항상 무시), `SET ISOLATION LEVEL GARBAGE;`가 이제 진짜 파싱 에러 반환(이전엔 가짜 OK), `SET NAMES utf8mb4;`는 여전히 에러 없이 통과 확인 | DBA 도구 신뢰성 | 낮음 |
| ✅ 완료 | BEGIN...END 내부 세미콜론으로 프로시저 조기 실행 | 원인(Rust `rusql-server/main.rs:94-111,241-247`): UI 실사용 테스트로 2건 확인 — (1) 트랜잭션 `BEGIN;`이 블록으로 오인되어 이후 전체가 한 문장으로 합쳐짐(`test_full.sql`), (2) 트리거/프로시저처럼 본문이 여러 줄에 걸친 `BEGIN...END`는 커넥션 루프가 버퍼에 `;`이 하나라도 보이면(깊이 무시) 바로 분리 후 버퍼를 통째로 비워, 본문 내부 문장·`END`가 최상위 문장으로 새어나감(`test_full-ver2.sql`의 멀티라인 트리거로 실증). C++ `server/main.cpp`의 커넥션 루프를 CLI의 깊이 인식 방식(`find_stmt_end`)과 동일한 점진적 추출 방식으로 재작성; `test_server_stmt_split.cpp`에 회귀 테스트 추가 | 저장 프로시저/트리거 생성 및 실행 안정성 | 중간 |
| P1 | rusql-client 세미콜론 카운트 불일치→hang | `rusql-client/main.rs:33-57,184-204` — BEGIN/END 미인식, 타임아웃 없음 | 클라이언트 안정성 | 중간 |
| ✅ 완료 | (신규 발견) `CompositeIndexPath`(복합 인덱스 정확일치 조회)가 MVCC 가시성 검사를 아예 안 함 | 원인: MVCC Stage 1에서 `is_visible_for_read`를 15개 AccessPath 전부에 배선했다고 기록됐지만, `executor_select.cpp:693-700`의 `CompositeIndexPath` 분기(`WHERE col1=x AND col2=y` 형태로 복합 인덱스의 모든 컬럼이 등호 조건일 때)만 실제로는 누락 — `search_from_eq_map`으로 찾은 행을 가시성 체크 없이 그대로 반환. 바로 옆의 `CompositeIndexPrefix` 분기는 정상적으로 체크하고 있어 대조로 발견됨. 실제로 재현: soft-delete(트랜잭션 내 DELETE 후 COMMIT)된 행이 복합 인덱스 정확일치 조회로는 계속 보임. `CompositeIndexPrefix`와 동일한 패턴으로 `is_visible_for_read(row, read_ctx)` 체크 한 줄 추가 후 안 보이면 "0 rows returned." 반환하도록 수정. UPDATE/복합인덱스 증분화 작업(아래 Section E) 중 새로 작성한 테스트가 발견 — `test_executor_update_delete.cpp`(soft-delete 시나리오)와 `test_concurrency.cpp`(다른 세션의 미커밋 INSERT가 안 보이는지, PK 포인트룩업 대상 기존 회귀 테스트의 복합 인덱스 버전)에 회귀 테스트 추가 | 복합 인덱스로 조회할 때도 다른 트랜잭션의 미커밋/삭제된 행이 새지 않음 | 낮음 |

## B. 동시성 아키텍처 (가장 가치 있는 구조 개선)

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| ✅ 완료 | 전역 단일 락으로 모든 문장 완전 직렬화 | 원인(Rust `executor.rs:674-683`, C++도 동일하게 `executor_core.cpp`의 `Executor::execute()`가 무조건 `shared->write()`): SELECT 포함 모든 문장이 전체 write lock 획득, 두 세션 동시 실행 불가. `SharedDatabase`를 감싸는 `RwLock<T>`(`sync.hpp`)가 이미 진짜 `std::shared_mutex` 기반이라는 걸 활용 — 완전한 MVCC 재설계 없이, 진짜 읽기 전용 문장(FOR UPDATE/FOR SHARE·파생테이블 없는 SELECT, WHERE/HAVING/JOIN-ON에 중첩된 서브쿼리까지 재귀 검사, SHOW류, DESCRIBE, EXPLAIN)만 `is_pure_read_only()`로 분류해 `shared->read()`로 동시 실행하도록 수정(`executor_core.cpp`). 이 경로에서도 도달 가능한 `BufferPool::get_page`의 캐시미스 채움이 실제 레이스였어서 `BufferPool`에 자체 뮤텍스+atomic 카운터 추가(move 생성자 직접 작성 필요); `LockManager`는 모든 변경 경로가 여전히 write-분류로 남아 레이스 시나리오 자체가 없어 뮤텍스 불필요(불변조건을 헤더에 명시). `test_concurrency.cpp` 신규 — 실제 스레드로 대용량 풀스캔 SELECT 진행 중 다른 세션의 `SELECT 1`이 즉시 응답하는지 실측(1.06초 vs 0.0002초로 확인). **후속(MVCC Stage 4, 아래 새 행 참고)으로 이 항목 자체가 더 확장됨**: 이 시점엔 "읽기 전용만 동시, 그 외 전부 배타"였지만, 이후 서로 다른 테이블에 대한 쓰기도 실제로 동시 실행되도록(테이블별 `shared_mutex`) 재설계 — 이때 "모든 변경 경로가 write-분류로 남아 뮤텍스 불필요"였던 `LockManager`도 실제로 동시 호출될 수 있게 되어 자체 뮤텍스가 추가됨 | LockManager·격리수준·병렬실행이 실질적 동시성 이득으로 연결 | 높음 |
| ✅ 완료 | 트랜잭션 스냅샷이 DB 전체 deep clone | 원인(Rust `txn_manager.rs:262-278`, 이식 시 그대로 보존): BEGIN마다 REPEATABLE READ/SERIALIZABLE이면 `tables.clone()`으로 DB 전체 복제. MVCC 실제 재설계(Stage 1~3)로 근본 해결 — 행 단위 버전 체인(`_xmin`/`_xmax` + `SnapshotCtx`)이 도입되면서 BEGIN은 이제 가벼운 값 몇 개(`{self_txn_id, cutoff, in_progress}`)만 캡처하면 됨, DB 전체를 복제하는 무거운 `snapshot_` 필드는 완전히 제거(`begin_with_snapshot`의 이제 안 쓰는 `tables` 매개변수도 함께 제거) — O(DB 크기)에서 O(1)로 | 행 단위 버전 체인 전환 시 비용 절감 | 높음 |
| P1 | 버퍼 풀이 테이블 전체 단위 캐싱(사실상 무의미) | `buffer_pool.rs` — 시작 시 전 테이블이 이미 로드되어 get_page 경로 도달 불가. 위 동시성 수정으로 이 경로에 스레드 안전성은 확보했으나(자체 뮤텍스), "죽은 read-cache 경로를 실제로 페이지 단위로 살리는" 근본 수정 자체는 이번 패스에서 의도적으로 제외 | 페이지 캐싱 구현 시 대용량 테이블 지원 | 높음 |
| P1 | 내부 쿼리 합성이 ASCII 표 문자열 재파싱 방식 | `executor.rs` 다수 — UNION/CTE/서브쿼리가 표시용 표를 `\|`로 split해 재파싱, 값에 `\|`/개행 있으면 깨짐. 동시성 개선과 무관한 별도 정확성 이슈라 이번 패스에서 제외 | 구조화된 내부 API로 정확성·성능 개선 | 높음 |
| P2 | 다중 조인 알고리즘 선택이 누적 카디널리티 미반영 | `planner.rs:121-125` — 2·3번째 조인이 항상 원래 base 테이블 행수 기준(join *순서* 결정에는 누적 카디널리티 DP가 이미 쓰이지만, join *알고리즘 선택*에는 반영 안 됨). 동시성과 무관한 planner 품질 이슈라 이번 패스에서 제외 | 다중 테이블 조인 실행계획 정확도 | 중간 |
| ✅ 완료 | 프로시저 루프/트리거 재귀에 상한·타임아웃 없음 | 원인(Rust `executor.rs:9210-9285,9381-9391`, C++도 동일): `exec_proc_while`/`exec_proc_loop`/`exec_proc_repeat`(`executor_proc.cpp`)에 반복 상한이 전혀 없고 `fire_triggers`(`executor_dml.cpp`)도 재귀 깊이 제한이 없어, 위 전역 락 항목이 고쳐진 뒤에도 DML/DDL/프로시저 CALL은 여전히 write-분류로 남기 때문에 무한루프 하나가 서버 전체를 영원히 멈출 수 있었음. 이미 있던 재귀 CTE의 1000회 상한(`executor_cte.cpp`) 패턴을 재사용해 세 반복문에 10만회 상한(초과 시 에러) 추가, `fire_triggers`에 재귀 깊이 32단 상한 추가(반환형을 `StringResult`로 변경해 INSERT/UPDATE/DELETE 3곳 호출부에 에러 전파 추가). 단, `fire_triggers`의 개별 트리거문 실패를 무시하는 기존 동작(Rust 원본의 `let _ = ...`와 동일, 의도적으로 보존)때문에 재귀 상한 초과 에러가 최상위 호출까지 전파되진 않음 — 그래도 재귀 자체는 확실히 유한 깊이에서 멈춤(실제 관찰 가능한 안전성 속성이자 이번 수정의 핵심 목표). `test_executor_proc.cpp`에 회귀 테스트 3건 추가 | 서버 가용성 확보 — 한 클라이언트의 버그가 전체 서버를 영구 정지시키지 않음 | 중간 |
| ✅ 완료 | (MVCC Stage 1~2, 신규) `session_tables` 세션 로컬 버퍼 방식 — 진짜 다중버전이 아니라 "DML을 세션별 버퍼에 쌓았다가 COMMIT 때 한꺼번에 반영"하는 방식이라 행 단위 버전 체인이 없었음 | 원인: 위 두 항목(전역 단일 락, deep-clone 스냅샷)의 근본 배경 — `_xmin`/`_xmax`는 있었지만 실제 다중버전 판정 로직이 없어 사실상 장식이었음. `SnapshotCtx{self_txn_id, cutoff, in_progress}` 도입 + `is_visible_for_read(row, ctx)`(격리수준별로 실제로 다르게 동작 — RU는 진짜 dirty read, RC는 문장마다 새 스냅샷, RR/Serializable은 BEGIN 시점 고정)로 전면 교체. UPDATE는 이제 항상 새 물리 버전을 append(구버전은 `_xmax`만 스탬프)하고 `session_tables`/`session_swap_in`/`session_swap_out`은 완전히 삭제 — 모든 DML이 `s.tables`에 직접, 실제 트랜잭션 ID로 태깅되어 기록됨. VACUUM도 "GC 호라이즌"(현재 활성 트랜잭션들 중 가장 오래된 스냅샷보다 먼저 죽은 행만 제거) 기반으로 교체. 15개 인덱스 기반 fast-path 읽기 전부에 새 가시성 검사 배선, 라이브 교차세션 검증(RR 격리 확인, ROLLBACK 완전 정리, SAVEPOINT undo, VACUUM 호라이즌 보호) 완료 | 위 두 항목이 임시방편이 아니라 실제 아키텍처가 됨, `_xmin`/`_xmax`가 이름값을 함 | 매우 높음 |
| ✅ 완료 | (MVCC Stage 4, 신규) 위 "전역 단일 락" 수정이 읽기 전용에만 그쳐 서로 다른 테이블에 대한 쓰기도 여전히 완전 직렬화 | 원인: `is_pure_read_only()` 분류는 SELECT류만 동시 실행을 허용, INSERT/UPDATE/DELETE는 테이블이 겹치든 안 겹치든 전부 배타 락. 구조적 잠금(`RwLock<SharedDatabase>`, DDL/DCL/VACUUM/CHECKPOINT + CTE·FROM-서브쿼리·뷰·발화 트리거가 있는 문장 전용 — 이런 문장은 실행 중 `s.tables`/`s.catalog`에 없던 이름을 임시로 삽입/삭제해 고정된 테이블 락 세트를 미리 잡을 수 없음)과 테이블별 `shared_mutex`(그 외 평범한 단일/조인 INSERT/UPDATE/DELETE/SELECT/MERGE/다중 UPDATE·DELETE — 대상 + FK 부모/자식 1-hop을 정렬된 순서로) 2계층으로 재설계. `LockManager`/`QueryResultCache`에 자체 뮤텍스 추가(더 이상 전역 락이 보호막이 아니게 됨), `maybe_auto_vacuum`을 전체-DB 스윕에서 단일 테이블 스코프로 축소. 실제 멀티스레드 스트레스 테스트로 데이터 레이스 2건 발견해 수정(테이블-키 맵 지연 초기화 레이스, UNION/EXPLAIN의 누락된 테이블 락). 실제 서버 + Python `threading`으로 서로 다른 테이블 동시 쓰기 실측 검증 | 서로 다른 테이블에 대한 쓰기가 실제로 병렬 실행됨 (행 단위 동시 쓰기는 여전히 범위 밖, 아래 참고) | 높음 |
| ✅ 완료 | (Gap Lock, 신규) Section H에 P3로 남아있던 "LOCK TABLES / Gap Lock" 갭 중 Gap Lock 부분 | 원인: 행만 잠그는 방식이라, REPEATABLE READ/SERIALIZABLE 트랜잭션이 범위를 잠그고 재조회해도 다른 트랜잭션이 그 범위에 새 행을 INSERT하면 유령 행(phantom)이 보일 수 있었음. InnoDB 스타일로 구현: `FOR UPDATE`/`FOR SHARE`와 트랜잭션 내 UPDATE/DELETE가 WHERE절의 PK 범위(Eq/Gt/Gte/Lt/Lte/Between, AND 조합 — `collect_and_leaves`로 추출, 그 외엔 안전하게 테이블 전체 범위로 폴백)를 잠그고, 같은 범위로의 동시 INSERT를 거부. Gap Lock끼리는 무충돌(실제 InnoDB와 동일), 데드락 그래프는 행 잠금과 공유. V1 범위: 단일 컬럼 PK만. 검증 중 실제 버그 발견: INSERT 쪽 체크가 처음엔 `schema.primary_key_columns`(테이블 레벨 복합 PK 제약 전용 필드)를 읽어서, 흔한 인라인 `id INT PRIMARY KEY` 테이블에서 항상 조용히 스킵되고 있었음 — 통합 테스트가 즉시 잡아냄, 다른 3곳과 같은 컬럼 스캔 방식으로 수정. Debug+Release 284케이스/17372assertion 통과, 실제 서버 라이브 검증 완료 | REPEATABLE READ/SERIALIZABLE에서 잠그는 읽기·범위 UPDATE/DELETE의 phantom 방지 (LOCK TABLES 자체는 여전히 미지원, Postgres 수준 predicate-lock 기반 완전 SSI도 아직 아님 — Section C의 SERIALIZABLE 행 참고) | 중간 |
| ✅ 완료 | (행 단위 완전 동시 쓰기, 신규) 위 "MVCC Stage 4"가 테이블 단위까지만 — 같은 테이블의 서로 다른 행에 대한 INSERT/UPDATE/DELETE는 여전히 그 테이블 하나의 배타 락으로 완전 직렬화 | 신규 계층 2개(테이블마다, CREATE TABLE에서 생성): `table_locks[table]`를 평범한 단일 테이블 Insert/InsertSelect/Update/Delete/Select(FOR UPDATE/FOR SHARE 포함)는 전부 SHARED로(MultiUpdate/MultiDelete/Merge만 배타 유지), 신규 `table_data_locks[table]`가 `s.tables[table]` 벡터의 **모양**(push_back/insert/erase) + `row_pk_pos`를 보호 — 전체 스캔은 SHARED로 스캔 내내, 모양이 바뀌는 순간만 EXCLUSIVE로 짧게. `LockManager`의 행 클레임을 오토커밋까지 확장(`RowClaimGuard` RAII). `exec_insert_inner`/`exec_update_inner`/`exec_delete_inner`/`exec_select`를 국면(phase) 단위로 재구성. 검증 스트레스 테스트(`test_concurrency.cpp` 신규 4건 — 동일 테이블 다른 행 다중 writer, 같은 행 명시적 트랜잭션 경합, FK CASCADE 동시 부하, TRUNCATE 배타성 회귀) 작성 중 진짜 버그 6건을 발견해 모두 수정: (1) 쿼리 캐시 무효화 타이밍(락 해제 후 무효화라 스테일 캐시 반환 가능) — `invalidate_table` 호출을 각 쓰기 경로의 EXCLUSIVE 임계구역 안으로, 세대 카운터(`table_generation_`)까지 추가해 `put()`이 동시 `invalidate_table`과 경합해도 스테일 항목을 못 남기게 이중 방어; (2) B+Tree PK/복합 인덱스가 `remove()`+`insert()` 두 번의 개별 락 사이클이라 값이 안 바뀌는 UPDATE에서도 순간적으로 "그 키가 인덱스에 없는" 윈도우 발생(동시 SELECT가 "0 rows returned" 유령 결과) — 키가 안 바뀌면 단일 `insert()`(원자적 덮어쓰기)로 통일; (3) 제자리 필드 변경(`row["_xmax"]=...`)이 `table_data_locks` SHARED만으로는 안전하지 않음(행 클레임 없는 동시 SELECT가 SHARED를 같이 쥔 채 같은 Row 객체를 복사하는 실제 데이터 레이스) — EXCLUSIVE로 승격; (4) 백그라운드 스레드에서 발생한 예외(기존에도 있던 Windows/백신 파일-교체 flake)가 Catch2 매크로 없이 그대로 `std::terminate()`를 불러 테스트 바이너리 전체가 죽던 문제 — `execute_sql`을 `execute_sql_inner`로 이름 바꾸고 try/catch 래퍼로 감쌈; (5) `std::shared_mutex`가 Windows SRWLOCK 기반이라 writer 우선순위를 보장 안 해 지속적 reader 부하 아래 writer가 무기한 굶는 실제 재현(8 reader+2 writer 스레드가 수 시간 멈춤) — 조건변수 기반의 완전히 명시적인 writer-preferring `FairSharedMutex`(`sync.hpp`) 신규 도입, `table_locks`/`table_data_locks`/`RwLock<T>` 전부 교체; (6) 서브쿼리 캐시 키를 매 행마다 AST 전체 복사+JSON 직렬화로 계산해(캐시 자체는 맞았지만 키 계산이 O(N)) 8-reader 스트레스 테스트가 "몇 시간째 안 끝남"으로 보이던 진짜 근본 원인 — `subquery_cache_` 키를 문자열 대신 서브쿼리 AST의 안정적 포인터 주소로 변경, 캐시 조회를 복사보다 먼저 하도록 재구성(`executor_subquery.cpp`). 이어서 신규 스트레스 테스트 자체에서 실제 버그 2건 추가 발견: (7) 동시 INSERT/UPDATE/DELETE가 짧은 시간에 한 파일(`t.rdb`/`rusql.wal`/`_undo.log`)을 반복적으로 저장→교체하면서 Windows 백신/인덱싱이 막 닫힌 `.tmp` 파일을 순간적으로 붙잡아 `MoveFileEx` 교체가 종종 "Access is denied"로 실패하던 기존 flake가 이 기능 덕분에 훨씬 자주 재현 — `write_bytes_atomic`(`atomic_write.hpp`)에 Windows 전용 지수 백오프 재시도(최대 5회) 추가; (8) **가장 심각**: `execute_commit_grouped`(그룹 커밋)가 `table_locks` EXCLUSIVE를 phase1과 `active_txn_ids` erase 사이에 한 번 풀었다가 다시 잡는 두 단계 임계구역이었는데, 그 사이 창(fsync+commit_finalize 동안)에 다른 세션의 UPDATE가 `table_locks` SHARED만으로 끼어들어 자신의 MVCC 가시성 스냅샷을 계산하면, 방금 커밋을 완료한(즉 `s.tables`에 새 버전이 이미 물리적으로 존재하는) 트랜잭션이 `active_txn_ids`에서 아직 안 지워졌다는 이유만으로 "아직 진행 중"으로 오판 — 이미 대체된 구버전이 여전히 보이고 새 버전은 안 보이는 버전 분기(fork)가 발생, 매 커밋마다 재발하며 복리로 누적되어 같은 행에 대한 두 스레드 경합 스트레스 테스트에서 테이블 행 수가 문자 그대로 피보나치 수열(233→377→610→987→1597→...)로 발산, 커밋 지연이 수 초에서 수십 초로 기하급수 증가(라이브 서버에서는 60초 이상 걸려 클라이언트 타임아웃까지 재현). `table_locks` EXCLUSIVE를 phase1부터 fsync·`commit_finalize()`·`active_txn_ids` erase까지 하나의 연속된 임계구역으로 병합해 그 창 자체를 제거(같은 테이블에 대한 동시 COMMIT의 fsync 배칭 이득은 줄지만 정확성이 우선; 서로 다른 테이블끼리의 그룹 커밋 배칭은 그대로 유효) — 수정 후 동일 스트레스 테스트가 타임아웃 대신 4초 내 통과. Debug+Release 306케이스/21676assertion 통과(2회 반복 확인), `test_full.sql`/`test_full-ver2.sql` 재실행(신규 회귀 없음, ver2의 BACKUP/RESTORE 자기참조 FK 이슈는 기존에 알려진 별개 항목), 실제 서버 + Python `threading`으로 동일 테이블 다른 행 동시 쓰기 및 같은 행 명시적 트랜잭션 경합(버그 8 수정 전엔 서버가 60초+ 응답 없음 → 수정 후 0.9초, 최종값 정확히 일치) 라이브 검증 완료 | 같은 테이블의 서로 다른 행에 대한 INSERT/UPDATE/DELETE가 실제로 동시 실행됨 — Section B "행 단위 동시 쓰기" 항목이 완전히 해소됨(진짜 블로킹 대기+데드락 감지, Postgres 수준 완전 SSI는 여전히 범위 밖) | 매우 높음 |

| ✅ 완료 | (진짜 블로킹 대기 락 + 데드락 감지, 신규 — 원래 1순위 항목) `LockManager::acquire()`/`acquire_shared()`가 충돌 시 즉시 실패만 하고 절대 기다리지 않음 — MySQL/InnoDB의 `innodb_lock_wait_timeout` 같은 진짜 대기가 없음 | `LockManager`에 `condition_variable` 추가, `acquire()`/`acquire_shared()`에 기본값 있는 `timeout` 매개변수 추가(기본 0 = 기존 즉시 실패 경로, 기존 14개 호출부는 손 안 대도 100% 동일하게 동작) — 충돌 시 `creates_cycle` 한 번만 검사(자는 동안 재검사 불필요: 나중에 사이클을 완성시키는 쪽의 `acquire()` 호출이 스스로 즉시 감지해 victim이 됨, 먼저 온 쪽은 그냥 계속 기다림 — 표준 데드락 희생자 선정과 동일), 아니면 `cv_.wait_until`로 진짜 잠들었다가 깨어날 때마다 **현재** 홀더를 다시 읽어 판정(자기 전 스냅샷을 절대 안 믿음), 시간 초과 시 신규 `Kind::Timeout` 반환. `@lock_wait_timeout`(기존 필드, 장식으로만 쓰이던 것)이 이제 실제 대기 시간으로 처음 쓰임. **개발 중 실제 라이브 행(hang)으로 발견한 핵심 아키텍처 문제**: `table_data_locks`만 풀고 블로킹하면 될 줄 알았으나, `Executor::execute()`의 디스패처가 `table_locks[table]`을 문장 전체 동안 SHARED로 쥔 채 `exec_*_inner`를 호출하고 있어서(그 가드는 `execute()`의 스택 프레임 소유라 안쪽에서 풀 방법이 없었음), 행에 블로킹된 문장이 `table_locks` SHARED를 계속 쥔 채 잠들면 같은 테이블에 대해 `table_locks` EXCLUSIVE가 필요한 `execute_commit_grouped()`(바로 위 항목의 MVCC 레이스 수정으로 그렇게 됨)의 COMMIT과 서로 다른 두 종류의 락(FairSharedMutex ↔ LockManager) 사이의 교차 데드락이 발생 — `LockManager`의 행 전용 wait-for 그래프에는 절대 안 잡히는 종류. 해결: `execute()`가 자신의 `TableLockGuard`를 `std::optional`에 담아 포인터로 저장해두고(`TableLockGuard`는 참조 멤버가 있는 `RwLock::ReadGuard`를 갖고 있어 이동 생성은 되지만 대입은 안 됨 — `std::optional::reset()`+`emplace()`로 제자리 재구성), `exec_*_inner`가 `release_table_locks_for_block()`/`reacquire_table_locks_after_block()`으로 `block_on_row()` 직전/직후에 풀었다 다시 잡을 수 있게 함(구조적 공유 락까지 통째로 재구성하지만 `SharedDatabase&` 참조 자체는 `RwLock`의 영구 내부 값을 가리키므로 안전). UPDATE의 뮤테이션 국면은 기존 phantom-disappearance 수정의 원자성 불변조건을 지키기 위해 **probe-then-mutate**로 분리(먼저 후보 행 전부를 `timeout=0`으로 확인만, 전부 통과해야만 실제 뮤테이션 실행 — 충돌 시 아무것도 안 바꾼 채 풀고 블로킹 후 처음부터 재시도). INSERT(중복 확인 클레임, ON DUPLICATE KEY UPDATE)/DELETE(트랜잭션 내·오토커밋 소프트삭제 2곳)/UPDATE(주 뮤테이션 루프)/UPDATE·DELETE의 FK 캐스케이드(5단 중첩 루프를 람다로 감싸 `return`으로 한 번에 탈출, 재시도는 전체 재계산 — `it->second == old_val`류 매치 조건이 이미 처리된 행을 자연히 걸러내 재시작이 안전/멱등) 전부 동일 release-block-retry 규율 적용. Gap Lock 블로킹과 SELECT FOR UPDATE/FOR SHARE 블로킹(`DataLockGuard`가 `execute()` 디스패처 소유라 `exec_select`에서 못 풀음 — 별도 재배선 필요)은 의도적으로 범위 밖. **검증 중 완전히 별개의 사전 존재 버그 발견**(이번 기능과 무관, 고치지 않고 기록만): FK 캐스케이드(UPDATE/DELETE 양쪽, Cascade/SetNull/SetDefault 전부)가 캐스케이드 대상 테이블의 `s.tables`는 정확히 갱신하면서 그 테이블의 PK B+Tree/보조/복합 인덱스는 전혀 갱신 안 함 — `SELECT ... WHERE <비PK 컬럼>`(전체 스캔)은 정답을 보여주지만 `SELECT ... WHERE <PK>`(인덱스 포인트 조회)는 캐스케이드 이후에도 무기한 스테일 값을 반환. 새 종단 테스트가 PK 조회 대신 전체 스캔 조회로 검증하도록 우회하고 별도 후속 과제로 기록. `test_lock_manager.cpp`에 신규 멀티스레드 케이스 4건(블로킹→해제 시 획득, 진짜 타임아웃, 자는 동안의 데드락, 홀더 교체 시 wait_for_ 갱신), `test_concurrency.cpp`에 신규 종단 테스트 5건(블로킹→해제 타이밍, `@lock_wait_timeout` 실측, 다른 행은 안 막힘 회귀, FK 캐스케이드 블로킹, 경합 스트레스). Debug+Release 315케이스/21760assertion 통과(각 2회 반복 확인, 306/21676에서 +9케이스/+84assertion), `test_full.sql`/`test_full-ver2.sql` 재실행(신규 회귀 없음), 실제 서버 + Python `threading`으로 블로킹→해제(~0.36초)/타임아웃 실측(~0.5초, 즉시도 기본 50초도 아님)/교차 행 경합 무행(no-hang) 라이브 검증 완료 | InnoDB/PostgreSQL과 동등한 "충돌 시 대기, 진짜 데드락만 즉시 실패" 의미론 확보 — Section B 원래 1순위 항목 완전 해소(Postgres 수준 완전 SSI만 여전히 범위 밖) | 매우 높음 |
| ✅ 완료 | (블로킹 대기, 후속 3종 — SELECT FOR UPDATE/FOR SHARE·Gap Lock·FK 캐스케이드 인덱스) 바로 위 항목이 SELECT FOR UPDATE/FOR SHARE·INSERT-vs-Gap-Lock 블로킹을 의도적으로 범위 밖에 남기고, 검증 중 발견한 FK 캐스케이드 인덱스 스테일 버그도 미수정 상태였음 | **(1) FK 캐스케이드 인덱스 버그**: `executor_update.cpp`/`executor_delete.cpp`의 FK 캐스케이드(Cascade/SetNull/SetDefault, UPDATE·DELETE 양쪽)가 캐스케이드 대상 테이블의 `s.tables`만 갱신하고 PK B+Tree/보조·해시 인덱스/복합 인덱스는 전혀 안 건드리던 것 — 주 테이블 자신의 UPDATE/DELETE가 이미 쓰던 증분 인덱스 갱신 패턴을 `other_table`/`pk_col` 매개변수를 받는 일반화된 `refresh_cascade_indexes` 헬퍼로 양쪽 파일에 추가, 6개 캐스케이드 지점(3개 FkAction × UPDATE/DELETE) 전부에 배선(DELETE 오토커밋 Cascade는 삭제될 행을 먼저 수집 후 erase 뒤 인덱스에서 제거, 트랜잭션 내 소프트삭제는 재-upsert — 주 테이블 자신의 소프트삭제가 이미 쓰던 것과 동일 이유). **(2) SELECT FOR UPDATE/FOR SHARE 블로킹**: 바로 위 `table_locks` 수정과 동일한 구조적 문제(`execute()` 디스패처가 소유한 `DataLockGuard`를 `exec_select`에서 못 풂) — 포인터로 저장해 release/reacquire하는 동일 패턴 적용. `DataLockGuard`는 참조 멤버가 없어 `TableLockGuard`보다 단순(옵셔널 래핑 불필요, 그냥 `x = DataLockGuard{};` 재대입). 이미 메모리에 확정된 `result`를 건드리지 않으므로 UPDATE류의 probe-then-mutate 복잡성은 불필요 — 충돌 시 `table_data_locks`+`table_locks` 둘 다 놓고 `block_on_row`, 재획득 후 다음 행으로. **(3) INSERT-vs-Gap-Lock 블로킹**: `LockManager::register_gap_conflict`에 `table`/`timeout` 매개변수 추가(기본 0=기존과 완전히 동일). Gap Lock은 `release()`가 이미 해당 txn 소유 갭을 쓸어내며 같은 `cv_`를 notify하므로 행 락과 동일한 조건변수를 재사용 — 행 락과 달리 갭은 홀더가 바뀌는 개념이 없어, 깨어날 때마다 "그 holder가 여전히 갭을 쥐고 있는지"만 재확인. `executor_dml.cpp`의 갭 충돌 검사 루프를 재시도 가능한 `for(;;)`로 감싸 매 반복 처음부터 재스캔(한 홀더가 풀려도 다른 홀더가 여전히 겹칠 수 있으므로). `test_lock_manager.cpp`/`test_concurrency.cpp`/`test_executor_update_delete.cpp`에 신규 회귀 테스트 다수(PK 인덱스 포인트 조회 기준 캐스케이드 검증, FOR UPDATE/FOR SHARE 블로킹+타임아웃, Gap Lock 블로킹+타임아웃 등) 추가, 직전 세션이 PK 경로를 일부러 피해 갔던 FK 캐스케이드 블로킹 테스트도 이제 PK 조회로 전환해 통과 확인. Debug+Release **324케이스/21863assertion** 통과(양쪽 동일, 315/21760에서 +9케이스/+103assertion), `test_full.sql`/`test_full-ver2.sql` 재실행(신규 회귀 없음, ver2의 BACKUP/RESTORE 자기참조 FK 이슈는 기존에 알려진 별개 항목) | Section B "범위 밖" 항목 3개 추가 해소 — Postgres 수준 완전 SSI·XA 분산 트랜잭션만 남음 | 중간 |

## C. 트랜잭션 내구성 (WAL·Undo·Checkpoint)

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| ✅ 완료 | (C++ 신규 회귀) WAL 커밋 fsync가 완전히 무동작 | 원인: 원본 Rust `wal.rs`/`group_commit.rs`는 `file.sync_all()`로 실제 fsync를 하는데, C++ 이식 과정에서 "std::fstream has no portable fsync"라는 (틀린) 주석과 함께 `flush()`만 호출 — `group_commit.cpp`는 그 `flush()`조차 아무것도 안 쓴 새로 연 빈 스트림에 호출해 완전히 무동작이었음. **이식 때 새로 생긴 회귀지 원본 버그가 아님.** C++ `wal.cpp`/`group_commit.cpp`를 C stdio(`FILE*`)로 바꿔 `_commit`(Windows)/`fsync`(POSIX)로 실제 디스크 동기화하도록 수정 | 커밋 직후 크래시 시에도 데이터 유실 방지 (그룹 커밋 기능의 존재 이유 자체) | 중간 |
| ✅ 완료 | 테이블/스키마 파일이 fsync 없이 flush만 됨 + 저장이 원자적이지 않음 | 원인(Rust `disk.rs:150-179,154-156,203-245`): `truncate(true)`로 열자마자 즉시 write, fsync 없음 — 크래시 시 손상되거나(truncate 후 write 실패) 디스크에 실제로 반영 안 됐을 수 있음. C++ `disk.cpp`의 `write_file`/`write_file_bytes`를 "임시파일(.tmp)에 쓰기 → fsync → 원래 경로로 원자적 rename" 방식으로 재작성(WAL과 동일한 `_commit`/`fsync` 사용) | 테이블/스키마/인덱스메타 등 모든 영속 파일의 내구성·원자성 확보 | 중간 |
| ✅ 완료 | (위 두 수정이 남긴 신규 갭) fwrite/fflush/fsync 실패를 아무도 확인 안 함 | 원인: 원본 Rust는 `.expect(...)`로 파일 열기/쓰기/fsync 실패 시 즉시 panic(=디스크 풀 등 I/O 에러 시 "커밋됐다"는 거짓 성공 응답 대신 프로세스 중단)하는데, 위 두 항목을 고치며 새로 쓴 C stdio 코드(`wal.cpp`/`disk.cpp`)와 기존 `txn_manager.cpp`의 Undo 로그 append가 반환값을 하나도 확인 안 하고 있었음(C++-대-Rust 전체 대조 감사에서 발견). 세 곳 모두 `fwrite`/`fflush`/`_commit`·`fsync`/스트림 open 실패 시 예외를 던지도록 수정, Rust의 `.expect()` 실패 시맨틱과 동일하게 맞춤 | I/O 실패를 거짓 커밋 성공으로 조용히 삼키는 것 방지 | 낮음 |
| ✅ 완료 | (실사용 중 실제로 크래시 재현) 커넥션 핸들러에 예외 안전장치 없음 → 서버 전체 다운 | 원인: 위 항목대로 예외를 던지게 고친 뒤, UI에서 벤치마크 실행 중 실제로 `engine_server.exe`가 `abort()`로 죽는 걸 사용자가 직접 겪음(Debug Error 다이얼로그) — Rust는 커넥션 스레드 하나가 panic해도 `std::thread::spawn`이 그 스레드만 격리해서 죽이는데, C++은 스레드 안에서 예외가 하나라도 처리 안 되고 새어나가면 `std::terminate()`가 프로세스 전체를 abort시켜 **접속해있는 모든 클라이언트가 같이 끊김**. 정확한 예외 트리거 지점은 재현 못 했지만(사용자의 오래 누적된 데이터 디렉터리에서만 발생, 새 디렉터리 3종 시나리오로 재현 시도했으나 실패), 근본 원인(예외 안전장치 부재)은 명확해 서버(`server/main.cpp`)와 CLI(`cli/main.cpp`) 양쪽의 쿼리 실행부를 try/catch로 감싸 예외를 그 쿼리 하나의 ERR 응답으로 변환하도록 수정 | 어떤 내부 오류가 나도 서버 전체가 아니라 해당 쿼리 하나만 실패 (Rust의 스레드별 panic 격리와 동등한 효과) | 낮음 |
| ✅ 완료 | WAL/Undo "트랜잭션 제거"가 비원자적 전체 재작성 | 원인(Rust `wal.rs:234-249, txn_manager.rs:181-206`, 이식 시 그대로 보존): C++ `wal.cpp`의 `remove_txn`/`truncate_to_last_checkpoint`는 남길 레코드를 모아 `std::ofstream(...trunc)`로 통째로 덮어씀(fsync·임시파일 없음), `txn_manager.cpp`의 `UndoLogFile::remove_txn`/`rewrite_txn`은 더 심각 — `clear_locked()`로 파일을 아예 삭제한 뒤 한 건씩 재기록. 다른 세션과 공유하는 파일이라 재작성 도중 크래시하면 자기 트랜잭션뿐 아니라 남의 것까지 유실. `disk.cpp`가 테이블/스키마 파일에 이미 쓰던 검증된 패턴(`.tmp`에 쓰기 → fsync → 원자적 rename)을 `atomic_write.hpp` 공유 헤더로 추출해 WAL/Undo 재작성 4곳 전부 적용(disk.cpp도 중복 제거 겸 이 공유 함수로 전환, 동작 변화 없음). `test_txn.cpp`에 회귀 테스트 3건 추가(내용 정확성 + `.tmp` 잔존 없음 확인) | 안전한 로그 관리 | 높음 |
| ✅ 완료 | 체크포인트-그룹커밋 TOCTOU 레이스 | 원인(Rust `executor.rs:824`, 이식 시 그대로 보존): `executor_txn.cpp`의 그룹 커밋 경로(`exec_commit_phase1`/`execute_commit_grouped`)에서 `active_txn_ids`(체크포인트가 "WAL 잘라내도 안전한가"의 유일한 판단 근거) 삭제가 `commit_write_record()`(WAL COMMIT 레코드를 **fsync 없이** 기록) 직후, 즉 `sync_commit()`(실제 fsync, 락 밖에서 실행)과 `commit_finalize()`(WAL/undo 정리)가 끝나기 *전에* 일어남 — 그 사이 창에서 체크포인트가 끼어들면 아직 디스크에 안전하게 fsync되지 않은 커밋을 안전으로 오판해 WAL을 잘라낼 수 있었음(단순/비그룹 커밋 경로는 원래도 안전 — `txn.commit()`이 동기 fsync 후 삭제). `active_txn_ids` 삭제를 `sync_commit()`+`commit_finalize()` 완료 후로 이동(자체 뮤텍스로 보호돼 `shared->write()` 재획득 불필요). `test_executor_txn.cpp`에 회귀 테스트 2건 추가 — 그룹 커밋 후 정말로 지워지는지, 그리고 진행 중인 트랜잭션이 `active_txn_ids`에 남아있는 동안 CHECKPOINT가 실제로 트렁케이션을 보류하는지 | 크래시 복구 시 커밋 유실 방지 | 높음 |
| P1 | WAL/Undo 디코딩이 첫 손상 지점서 이후 전부 폐기 | `wal.rs:80-95, txn_manager.rs:101-123` — 체크섬 없음 | 부분 손상에도 최대 복구 | 중간 |
| ✅ 완료 | 크래시 복구가 보조/복합 인덱스 미갱신 | 원인(Rust `executor.rs:8130-8298`, 이식 시 그대로 보존): REDO/UNDO replay가 `s.tables`와(REDO INSERT에 한해서만) PK B+Tree만 갱신하고, 보조 btree/hash/복합 인덱스는 물론 UPDATE/DELETE/UNDO 경로의 PK B+Tree조차 갱신 안 함. C++ `executor_txn.cpp`의 `recover_from_wal`에서 replay 중 건드린 테이블을 모아 replay 완료 후 PK/보조/해시/복합 인덱스를 전부 재구축하도록 수정. `test_executor_txn.cpp`에 `commit_write_record()`로 "커밋 WAL은 썼지만 finalize 전 크래시"를 재현하는 회귀 테스트 추가 | 크래시 복구 후 인덱스 기반 조회 정확성 | 높음 |
| 🟡 부분 완료 | SERIALIZABLE이 phantom(행 개수)만 감지 | 원래 원인(Rust `txn_manager.rs:293-310`): 행 개수 비교만 해서 "개수는 같지만 내용이 다른" 인터리빙(한 행 삭제 + 다른 행 삽입 등)을 못 잡았음. `validate_serializable`을 read-set 기반 재검증(트랜잭션이 실제로 읽은 각 행이 BEGIN~커밋 사이 이미 커밋된 다른 트랜잭션에 의해 바뀌었는지 확인)으로 교체 + Gap Lock(InnoDB 스타일, `FOR UPDATE`/`FOR SHARE`·트랜잭션 내 UPDATE/DELETE가 PK 범위를 잠가 동시 INSERT phantom을 차단) 추가 — 둘 다 완료. **아직 남은 갭**: predicate lock이 없어 **잠금 없는 일반 SELECT**로 범위를 조회하는 경우엔 read-set에 아직 존재하지 않던 phantom 행이 잡히지 않고, Gap Lock도 잠그는 읽기(FOR UPDATE/SHARE)와 범위 UPDATE/DELETE에만 걸림 — Postgres 수준의 진짜 SSI(SIREAD 락 기반 predicate 추적)는 아님. 사용자와 논의 후 "필수 아님, 나중에 여유 있으면" 판단으로 보류 | 이상현상 탐지 정교화 | 높음(잔여분) |
| P1 | Lock wait timeout이 실제로 대기 안 함 | `executor.rs:3319-3378` — 즉시 에러 반환, sleep 없음(사실상 NOWAIT) | 락 타임아웃 설명대로 동작 | 중간 |
| ✅ 완료 | READ UNCOMMITTED/COMMITTED 코드상 미분화 | 원인(Rust `txn_manager.rs:262-289`, 이식 시 그대로 보존): RU/RC가 완전히 동일한 코드 경로. MVCC 실제 재설계(Stage 1, `SnapshotCtx`) 과정에서 실제로 갈라짐 — RU는 `{self, UINT64_MAX, {}}`(모든 트랜잭션 id를 이미 커밋으로 간주하는 진짜 dirty read), RC는 문장마다 새로 캡처하는 스냅샷. 라이브 검증: 같은 세션이 RU/RC 각각으로 접속해 다른 세션의 아직 커밋 안 된 INSERT를 조회 — RU는 보이고 RC는 안 보이는 것 확인 | 격리수준 문서-동작 일치 | 중간 |

## D. 보안

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| ✅ 완료 | 사용자 테이블 비면 인증 무조건 통과 | 원인(Rust `executor.rs:198,215`, 이식 시 그대로 보존): fail-open (`if users.is_empty() { return true }`). 서버는 두 리스너(native/MySQL) 모두 `ensure_default_user()`를 부팅 시 미리 호출해 실제로는 이 분기가 도달 안 되지만, 방어적으로 C++ `validate_credentials`/`verify_mysql_native_password`에서 open-mode 폴백 완전히 제거(fail-closed) | 향후 진입점 추가/`ensure_default_user` 누락 시에도 인증 우회 불가 | 낮음 |
| ✅ 완료 | BACKUP/RESTORE 경로 미검증 | 원인(Rust `parser.rs:3783-3830, executor.rs:7435-7478`, 이식 시 그대로 보존): 사용자가 준 파일명을 그대로 `fs::write`/`fs::read_to_string`에 전달 — BACKUP은 임의 파일 쓰기, RESTORE는 임의 파일을 읽어 SQL로 **실행**(사실상 SQL include). C++ `executor_backup.cpp`에 `resolve_backup_path` 추가 — 영문/숫자/`_`/`-`/`.`만 허용하는 순수 파일명만 받아 `<data_dir>/_backups/`로 샌드박싱, 절대경로·`..`·구분자 전부 거부. 부수적으로 `DiskManager::list_databases()`가 새로 생긴 `_backups` 디렉터리를 실제 DB로 오인해 기본 `current_db`를 가로채던 버그도 같이 발견해 수정(밑줄로 시작하는 최상위 디렉터리는 전부 제외) | 임의 파일 읽기/쓰기(SQL include) 취약점 제거 | 낮음~중간 |
| ✅ 완료 | Native TCP가 비밀번호 평문 전송 | 원인(Rust `main.rs:185-199, client/main.rs:120`, 이식 시 그대로 보존): MySQL 프로토콜은 이미 SHA1 챌린지-응답(`mysql_native_password`) 구현했는데 자체 native 프로토콜은 `AUTH user password`로 평문 전송. 이미 구현·검증된 `verify_mysql_native_password`를 native 프로토콜에도 그대로 재사용 — 서버가 연결마다 20바이트 nonce를 생성해 배너에 `NONCE <hex>` 줄로 전송, 클라이언트(`engine_client`/Tauri UI)가 `SHA1(password) XOR SHA1(nonce \|\| SHA1(SHA1(password)))` 토큰을 계산해 hex로 전송(평문 비밀번호는 와이어에 절대 안 실림). `engine_client`는 벤더링된 헤더온리 SHA1(`third_party/sha1`)을 엔진 링크 없이 직접 사용, Tauri UI는 `sha1` crate 신규 추가. 이 변경으로 유일한 호출부를 잃은 `validate_credentials`/`migrate_mysql_hash` 완전 삭제. Python으로 독립 구현해 실제 서버에 접속해 검증 — 평문 전송 시 거부, 올바른 챌린지-응답만 인증 성공하는 것을 직접 확인, Rust 구현도 별도 스탠드얼론 프로그램으로 동일 입력에 대해 Python·C++와 바이트 단위로 일치하는 토큰을 계산하는 것 확인 | 두 프로토콜 보안 수준 통일, 평문 비밀번호 와이어 전송 완전 제거 | 중간 |
| ✅ 완료 | MySQL 리스너 기본 0.0.0.0 바인딩 | 원인(Rust `mysql.rs:1030`, 이식 시 그대로 보존): native는 127.0.0.1인데 이건 LAN 전체 노출, root/root 기본계정과 결합 시 위험. `start_mysql_listener`에 `bind_addr` 매개변수 추가, `main.cpp`에 `--mysql-bind <addr>` 플래그 신규(기본값 `127.0.0.1`로 변경, 필요시 `--mysql-bind 0.0.0.0`으로 명시적 선택 가능 — Tauri UI는 이 플래그를 안 넘기므로 자동으로 새 안전한 기본값 적용). `netstat`으로 기본값이 `127.0.0.1`에만, 명시적 지정 시 `0.0.0.0`에 바인딩되는 것 직접 확인 | 안전한 기본값 + 명시적 바인드 선택 모두 확보 | 낮음 |
| ✅ 완료 | (신규 발견) native TCP 챌린지-응답 전환 이후 MCP 서버가 완전히 접속 불가 상태였음 | 원인: 위 "Native TCP가 비밀번호 평문 전송" 수정으로 native 프로토콜이 평문 `AUTH user password`를 더 이상 받지 않게 됐는데, `mcp_server.py`(그리고 벤치마크 스크립트 `bench.py`)는 그 시점에 갱신되지 않고 여전히 평문 `AUTH root root`를 보내고 있었음 — 사용자가 "MCP 기능이 잘 작동하는가" 질문을 계기로 실제 MCP 클라이언트 프로토콜(stdio JSON-RPC)로 직접 붙여 재현: 서버 로그에 `AUTH failed: 'root'` 반복, 7개 도구 전부 접속 에러만 반환. `engine_client`(C++)에 이미 구현된 것과 동일한 `mysql_native_password` 챌린지-응답 계산을 Python으로 이식해 `mcp_server.py`/`bench.py` 둘 다 수정 | MCP·벤치마크 스크립트 재작동 | 낮음 |
| P2 | TLS/SSL 전무 | `code/backend/third_party`에 OpenSSL/Schannel 등 TLS 라이브러리 없음 (원본 Rust도 rustls/native-tls 없었음) | 전송 계층 보안 | 높음 |
| P2 | mcp_server.py 하드코딩 접속정보, 재시도 없음 | `mcp_server.py:14-32` | 배포 유연성, 응답속도 개선 | 낮음 |

## E. 인덱스·성능 심화

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| ✅ 완료 | B+Tree 삭제 시 언더플로우 리밸런싱 없음 | 원인(원본 Rust `btree.rs:357` 주석에 명시, C++ 이식본엔 관련 상수·체크·주석 전무 확인): 리프가 완전히 비어야만(0개) 노드를 버리고, 그 외엔 키가 1개까지 줄어도 그대로 방치 — 병합/재분배 전무. 표준 B-tree 공식(`ORDER=16` 기준 `t=8`, 비-루트 최소 키 수 `MIN_KEYS=t-1=7`)에 맞춰 `remove_node`의 internal-node 분기를 재작성 — 자식이 언더플로우하면 형제가 여유 있으면 빌리고(회전), 아니면 병합(왼쪽 우선). 리프/internal 노드 모두 처리(internal은 부모 구분키가 자식으로 내려가고 형제 키가 부모로 올라가는 표준 회전). 부모 포인터·리프 형제 연결 리스트가 원래 없는 구조라 병합/재분배가 sibling-link 불변조건을 신경 쓸 필요 없어 범위가 좁음. 공개 API(`remove(key)`) 시그니처 불변, 순수 내부 구현. 2000개 키 삽입 후 앞/뒤/중간/산발/무작위 순서로 대량 삭제하는 회귀 테스트 5건 추가 — 매 삭제 후 트리 재귀 순회로 "루트 아닌 모든 노드가 MIN_KEYS 이상"인 실제 구조적 불변조건까지 검증(단순 검색 정확성뿐 아니라) | 대량 삭제 워크로드에서 트리가 무한정 성겨지는 것 방지, 장기 운영 성능 저하 방지 | 높음 |
| P2 | FK/UNIQUE 검증이 O(n) 선형 스캔 | `executor.rs:2021-2025, 1885-1968` — 이미 있는 인덱스 미활용 | 대량 INSERT 시 O(log n)으로 개선 | 중간 |
| P2 | Index Intersection이 결국 전체 재스캔 | `executor.rs:2623-2680` — 포인트 룩업 미사용 | 문서상 성능 이득 실제 구현 | 중간 |
| ✅ 완료 | 재귀 CTE 1000회 상한 도달 시 무경고 종료 | 원인(Rust `executor.rs:1603`, 이식 시 그대로 보존): `exec_with`의 재귀 반복 루프가 "고정점 도달로 정상 종료"와 "여전히 새 행을 만들어내는 중인데 1000회 상한에 걸려 종료"를 구분할 방법이 없어 둘 다 동일하게 그때까지 누적된(불완전할 수 있는) 결과를 조용히 반환. `executor_cte.cpp`에 `reached_fixed_point` 플래그 추가 — 상한에 걸렸는데 고정점이 아니면 CTE 임시 테이블/인덱스를 정리하고 명확한 에러(`"did not reach a fixed point after 1000 iterations"`)로 실패하도록 변경(이미 있던 `WHILE`/`LOOP`/`REPEAT` 10만회 상한이 이 CTE 상한을 원조로 인용하던 것과 동일한 원칙 적용). `test_executor_cte.cpp`에 종료 조건 없는 재귀 CTE 회귀 테스트 추가, 실제 `mysql` CLI·`engine_client` 양쪽에서 동일 에러 메시지로 라이브 확인 | 결과 완전성 신뢰 | 낮음 |
| P2 | 재귀 CTE 중복제거 O(n²) | `executor.rs:1621` — Vec::contains 사용 | HashSet 전환 시 성능 개선 | 낮음 |
| P2 | GROUP BY 병렬집계 임계값 체크 누락 | `executor.rs:3062-3063` | 소규모 쿼리 오버헤드 제거 | 낮음 |
| P2 | 병렬 임계값 환경변수 오타로 항상 무시 | `executor.rs:38-40` — `RUSTDB_parallel_min_rows()` 오타 | 튜닝 옵션 실제 동작 | 낮음 |
| ✅ 완료 | (신규 발견, C++ 이식 시 도입된 관행) UPDATE/복합인덱스가 몇 행이 바뀌든 항상 테이블 전체를 복제해서 PK/복합 인덱스를 통째로 재구축 | 원인: "행 단위 동시 쓰기"를 계획하려고 Explore 에이전트로 전체 코드베이스를 조사하다 발견 — plain UPDATE(`executor_update.cpp`), INSERT의 ON DUPLICATE KEY UPDATE 분기(`executor_dml.cpp`), 다중 테이블 UPDATE/DELETE(`executor_multi.cpp`)가 전부 `idx = BPlusTree(); for (row : 전체_테이블_복제본) insert(...)` 패턴으로 PK B+Tree를(그리고 UPDATE/DELETE는 복합 인덱스도 `CompositeIndex::rebuild(전체복제본)`으로) **항상** 통째로 재구축 — 문장 하나가 행 1개만 건드려도 O(테이블 크기)의 낭비. 반면 INSERT/DELETE의 PK B+Tree 유지는 이미 단일 키 증분(`insert`/`remove`) 방식이었음. 대칭을 맞추기 위해 `CompositeIndex`에 `insert_row`와 대칭인 `remove_row(row)` 신규 추가(`key_from_row`로 키 계산 후 `tree_.remove`), 위 3개 파일 전부를 "바뀐 행마다 old-key remove + new-key insert" 증분 방식으로 재작성(PK 컬럼 자체가 SET 대상이 되는 경우도 처리 — old/new 키를 각각 old_row/new_row에서 읽음). DELETE의 소프트/하드 두 경로 모두(`executor_delete.cpp`) 복합 인덱스만 전체재구축이었던 것도 같은 방식으로 증분화(PK B+Tree는 이미 증분이라 손 안 댐). 검증 중 위의 `CompositeIndexPath` MVCC 가시성 버그를 별개로 발견해 같이 수정(위 Section A 참고). Debug+Release 292케이스/17454assertion(263케이스/16961에서 시작한 이번 세션 누적분 중 +8케이스/+82assertion), `test_full.sql`/`test_full-ver2.sql` 재실행(신규 회귀 없음) | UPDATE/DELETE 성능이 실제로 바뀐 행 수에 비례(전체 테이블 크기 무관); 향후 행 단위 동시 쓰기 재설계의 필수 선행 작업 하나를 제거 | 중간 |
| P2 | (위 작업 중 발견, 이번엔 안 고침) INSERT ON DUPLICATE KEY UPDATE·다중 DELETE·MERGE가 일부 인덱스 종류를 아예 안 건드림 | 세 곳 모두 "전체 재구축이 낭비"가 아니라 "애초에 유지가 안 됨"이라는 다른 종류의 버그: INSERT ON DUPLICATE KEY UPDATE(`executor_dml.cpp`)는 복합/secondary/hash 인덱스를 전혀 안 건드림, 다중 DELETE(`executor_multi.cpp`)는 secondary/hash를 전혀 안 건드림, `MERGE`(`executor_merge.cpp`)는 PK/secondary/hash/복합 전부 안 건드림(가장 심각). 위 작업(전체재구축→증분 전환)과는 범위가 달라 이번엔 제외, 발견만 기록 | 이 세 경로 이후의 인덱스 기반 조회 결과가 stale해지는 것 방지 | 중간~높음(특히 MERGE) |

## F. MySQL 프로토콜 호환성

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| ✅ 완료 | SHOW INDEX/TABLE STATUS 등 하드코딩 빈 결과 | 원인 2건: (1) MySQL 프로토콜 `mysql.cpp`의 SHOW INDEX가 항상 빈 결과를 반환(원본 Rust `mysql.rs:726-750`와 동일하게 이식됨), (2) native 프로토콜 `exec_show_index`(`executor_show.cpp`)는 CREATE INDEX로 만든 보조/복합 인덱스만 순회하고 테이블 자신의 PRIMARY KEY는 아예 조회 대상에서 빠져있어 PK만 있는 테이블은 "No indexes found"로 나옴. `mysql` CLI로 실제 접속해 재현. exec_show_index에 PK 행 추가, mysql.cpp는 native 결과를 재조회해 MySQL의 13개 컬럼 형식(Non_unique/Seq_in_index 등)으로 재구성(복합 인덱스는 컬럼별로 행 분리)하도록 수정 | 실제 조회 가능, MCP get_indexes 도구 정상화 | 중간 |
| P2 | information_schema 실구현 부재 | JDBC/ORM(Hibernate 등) 메타데이터 조회 실패 위험 | 표준 드라이버 호환성 | 높음 |
| P2 | 커넥션풀 커맨드 미지원→패킷 디싱크 | `mysql.rs:921-1021` — COM_CHANGE_USER 등 | 풀링 드라이버 호환성 | 중간 |
| ✅ 완료 | (C++ 이식 시 신규 발견) COM_STMT_EXECUTE FLOAT/DOUBLE 파라미터 정밀도 손실 | 원인: 원본 Rust `mysql.rs:285-298`는 `format!("{}", v)`(반올림 없는 최단 왕복 표현)를 쓰는데, C++ 이식은 `std::to_string(double)`(고정 소수점 6자리)을 써서 바인딩된 FLOAT/DOUBLE 파라미터가 쿼리 실행 전에 이미 정밀도 손실(예: `3.141592653589793` → `3.141593`). 원본 코드 리뷰 중이 아니라 이후 C++-대-Rust 전체 대조 감사에서 발견. 실제 MySQL 바이너리 프로토콜(수동 구현한 핸드셰이크+COM_STMT_PREPARE/EXECUTE)로 라이브 재현 — 수정 전/후 정밀도 차이 직접 확인. C++ `mysql.cpp`에 `fmt_double_param`(엔진 코어의 `fmt_double`과 동일한 `to_chars`+`chars_format::fixed` 기법) 추가해 두 케이스 모두 교체 | 프리페어드 스테이트먼트로 FLOAT/DOUBLE 바인딩하는 모든 클라이언트(ORM 등)의 정확성 | 낮음 |

## G. UI/UX

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| ✅ 완료 | 파괴적 작업에 확인 절차 없음 | 원인: DROP DATABASE/TABLE/VIEW/INDEX, 연결삭제가 클릭 한 번에 즉시 실행(기존 재사용 가능한 확인 다이얼로그 컴포넌트 자체가 없었음 — `window.confirm()` 1곳, `.dlg-*` CSS 컨벤션 기반 개별 다이얼로그만 존재). `App.tsx`에 `confirmDialog` 상태 + `confirmThenRun(title, message, action)` 헬퍼 신규 추가해 6개 트리거 지점(DB/테이블/뷰/인덱스 드롭, 테이블 TRUNCATE, 저장된 연결 삭제) 전부에서 재사용, `App.css`에 `.dlg-danger` 스타일 추가(기존 `.dlg-connect` 구조 재사용, `.ctx-item-danger`의 빨간 톤 적용). `tsc --noEmit` 클린 확인 | 실수 방지, UX 완성도 | 낮음 |
| ✅ 완료 | 컴파일타임 개발자 경로 하드코딩→배포 불가 | 원인: `main.rs`의 `env!("CARGO_MANIFEST_DIR")`가 5곳(`engine_server_path`/`get_app_data_dir`/`setup_mcp_config`/`open_terminal`/`bench_dir`)에서 빌드 당시 소스 체크아웃 경로를 실행 파일에 그대로 박아 넣어, 빌드한 PC와 다른 곳에 복사하면 전부 존재하지 않는 디렉터리를 가리키게 됨(이 프로젝트엔 정식 설치형 번들 파이프라인이 없어 "code/ 트리 전체 복사해서 어디서든 실행"하는 모델이므로, 그 모델은 유지). 공유 헬퍼 `code_dir()` 신규 추가 — 디버그 빌드는 기존처럼 `CARGO_MANIFEST_DIR` 기준, 릴리즈 빌드는 `std::env::current_exe()` 기준(`ancestors().nth(5)`)으로 계산해 실행 파일이 실제로 지금 있는 위치를 따라감. 5개 함수 전부 `code_dir().join(...)`으로 리팩터링(중복 제거 겸). 실제 `cargo build --release`로 빌드해 그 자리(`target/release/`)에서 직접 실행 — 이전 세션에서 저장해둔 연결(같은 `code/data`)이 정상 인식되고, 실제로 `engine_server.exe`를 올바른 경로(`code/build/backend/server/Release/`)에서 찾아 스폰해 정상 로그인까지 라이브로 확인 | 다른 PC 배포 가능 | 중간 |
| ✅ 완료 | MCP 설정 병합 실패 시 기존 설정 덮어씀 | 원인: `main.rs`의 `write_mcp_into`가 기존 `claude_desktop_config.json` 파싱 실패 시 `unwrap_or(json!({}))`로 조용히 빈 객체로 대체한 뒤 그대로 덮어써, 사용자의 기존 설정(다른 MCP 서버 등록 등)이 전부 사라짐. 파싱 실패 시 파일을 건드리지 않고 즉시 에러 반환하도록 수정(`?`로 전파, 기존 "mcp_server.py를 찾을 수 없습니다" 에러와 동일한 표면화 경로 재사용). `#[cfg(test)]` 유닛테스트 2건 신규 추가(이 파일에 테스트 인프라 자체가 없어 새로 만듦) — 깨진 JSON을 건드리지 않고 에러 반환하는지(바이트 단위 동일성 확인), 정상 JSON은 기존 항목 보존하며 병합되는지, `cargo test`로 둘 다 통과 확인 | 사용자 기존 설정 보존 | 낮음 |
| P2 | App.tsx 4090줄 단일 컴포넌트 | useState 41개·useEffect 12개가 한 함수에 혼재 | 유지보수성 향상 | 높음 |
| P3 | 백엔드 죽은 코드 4개 + CSV 임포트 UI 부재 | export_csv/import_csv/get_views/get_indexes 미호출 | 정리 + 기능 하나 추가 가능 | 낮음~중간 |
| P2 | Mutex/RwLock unwrap 다수→패닉 전파 | main.rs 52곳 | 단일 장애점 제거 | 낮음~중간 |

## H. 신규 기능 확장 (DIFF.md 갭 분석, 여유 시간에 따라 선택)

| 우선순위 | 항목 | 기대 효과 | 난이도 |
|---|---|---|---|
| P3 | LOCK TABLES | 동시성 제어 갭 해소 (Gap Lock은 Section B에서 완료) | 중간 |
| P3 | REPLACE INTO / DEFERRABLE 제약 | 실무 관용구 지원 | 낮음~중간 |
| P3 | 표현식/부분/내림차순 인덱스 | 인덱스 실무 완성도 | 중간 |
| ✅ 완료 | LATERAL JOIN | 아래 "LATERAL JOIN + 신규 집계 함수 확장 구현 완료" 참고 | 높음 |
| ✅ 완료 | FILTER절/BIT_AND/BIT_OR/JSON_AGG | 아래 "LATERAL JOIN + 신규 집계 함수 확장 구현 완료" 참고 | 중간 |
| P3 | ARRAY_AGG/PERCENTILE_CONT | 집계 표현력 확장 | 중간 |
| P3 | CREATE SEQUENCE | DDL 완성도 | 중간 |
| P3 | 열 레벨 권한 / 이벤트 스케줄러 | DCL·운영 완성도 | 중간 |
| ✅ 완료 | 파티셔닝(PARTITION BY RANGE/LIST/HASH) | 아래 "파티셔닝 구현 완료" 참고 | 중간 |

### 대규모 확장 후보 비교 — SSI / XA / 파티셔닝 / 샤딩 (2026-08-09)

> Section B "진짜 블로킹 대기 락"의 후속 3종(FK 캐스케이드 인덱스 버그·SELECT FOR UPDATE/FOR SHARE 블로킹·INSERT-vs-Gap-Lock 블로킹)까지 끝난 뒤, 남은 범위 밖 항목 중 규모가 큰 4개를 사용자 요청으로 비교 정리. **넷 다 아직 미착수** — 착수 여부/순서 결정을 위한 기록.
>
> 근거: RuSQL은 현재 완전 단일 프로세스/단일 노드 엔진(복제·자동 장애 복구·읽기 복제본·연결 풀링 전부 `✗`, `DIFF.md` "14. 고가용성/분산" 섹션 참고)이고, 테이블 파티셔닝도 전무(윈도우 함수 `OVER(PARTITION BY...)`만 존재, `DIFF.md` "파티셔닝" 행 `✗`)하며, SSI는 read-set 기반 검증(Phase 24 Stage 3) + Gap Lock(Phase 25)으로 이미 부분 구현된 상태다.

| 항목 | 개발 범위 | 난이도 | 이득 | 규모 |
|---|---|---|---|---|
| **SSI**(완전한 Serializable) | Predicate lock(SIREAD) 인프라 신규 구축, rw-antidependency 사이클 감지를 `LockManager`에 배선 — 기존 read-set 기반 검증을 대체/보강 | 매우 높음 — 학술 알고리즘(Cahill et al.) 구현, 잠금 없는 일반 SELECT까지 추적 대상 확장 | 중간 — Gap Lock+read-set 검증이 이미 실용적 케이스 대부분 커버, "완전성"(잠금 없는 SELECT의 phantom/write-skew)만 남은 갭 | 중간~큼 — `LockManager`, `Executor` 전반, 커밋 경로 |
| **XA**(분산 트랜잭션) | 2단계 커밋(PREPARE/COMMIT/ROLLBACK), 트랜잭션 브랜치 ID, in-doubt 복구, WAL에 PREPARE 상태 기록, `XA START/...` 파싱 | 매우 높음 — 완전히 새로운 상태 기계 + 크래시 복구 로직 확장 | 낮음 — RuSQL은 완전 단일 노드 엔진, 다른 RuSQL 인스턴스/외부 DB와 연동하는 시나리오가 없는 한 실사용 가치 거의 없음 | 큼 — 파서·실행기·WAL·`LockManager`·서버 프로토콜 전반 |
| **파티셔닝**(테이블) | `PARTITION BY RANGE/LIST/HASH` DDL, 카탈로그에 파티션 메타 추가, 파티션 프루닝(쿼리 라우팅), 파티션별 독립 파일/인덱스 | 중간~높음 — 기존 단일 프로세스 저장구조를 논리적으로 나누는 것, 새 아키텍처는 불필요 | 큼 — 대용량 테이블 쿼리 성능(프루닝), 파티션 단위 DROP/TRUNCATE로 유지보수 용이 — 지금은 테이블을 항상 통짜로 다루는 구조라 체감 이득이 큼 | 중간 — 카탈로그+DDL 파서+플래너/실행기 라우팅(저장 엔진 자체는 재사용) |
| **샤딩** | 여러 독립 노드 간 데이터 분산, 코디네이터/라우팅 레이어, 크로스-샤드 조인·트랜잭션, 리밸런싱 | 극히 높음 — 복제·HA·연결 풀링조차 없는 상태에서 시작하는 사실상 새 분산 시스템 | 낮음(현재 스코프 기준) — 단일 머신 용량을 초과하는 실사용 트래픽이 없는 한 불필요 | 매우 큼 — 사실상 별도 프로젝트 |

**요약**: ROI로는 **파티셔닝**이 가장 현실적(기존 아키텍처 안에서 국소적 확장, 이득 큼). **SSI**는 이미 상당 부분 커버돼 있어 나머지 이득이 제한적. **XA**와 **샤딩**은 "단일 프로세스 커스텀 RDBMS"라는 RuSQL의 현재 스코프 자체와 안 맞아, 하더라도 아키텍처 전제부터 재논의가 필요한 수준.
>
> **업데이트 (2026-08-10):** 사용자가 파티셔닝부터 진행하기로 결정, 같은 세션에서 V1 구현 완료 — 아래 "파티셔닝 구현 완료" 참고. SSI/XA/샤딩은 여전히 미착수.

### 파티셔닝 구현 완료 (PARTITION BY RANGE/LIST/HASH, V1 — 2026-08-10)

**설계**: 파티션 테이블 `t`는 카탈로그·락·인덱스 8개 맵을 전부 정상적으로 갖지만 **`s.tables[t]`는 영원히 비어있는 유령(phantom) 엔티티**로 남긴다(기존의 "테이블 존재 확인" 코드 전부 — FK 참조, `SHOW TABLES`, `DESCRIBE`, 백업 등 — 손 안 대도 계속 정상 동작). 실제 행은 `t__p0`, `t__p1`, ... 이름의 **완전히 평범한 자식 테이블**(부모와 동일 스키마, 동일 8개 맵을 정상적으로 다 가짐)에 저장된다. `Executor::execute()`의 최상단(어떤 락도 잡기 전)에서 대상 테이블이 파티션 테이블인지 확인해, 맞으면 `Statement`를 자식별로 재작성한 뒤 **`execute()`를 다시 호출**(재귀, 최상위 진입점 그대로 재진입)해서 자식마다 처리 — 기존 실행기 내부(락, MVCC, 인덱스 유지보수, FK 캐스케이드 등 INSERT/UPDATE/DELETE/MultiUpdate/MultiDelete/Merge 5개 함수에 걸친 약 50개 호출부)를 단 한 줄도 안 건드리는 순수 전처리 계층으로 구현 — 그 아래 모든 기존 코드는 자식 테이블을 그냥 "평범한 테이블"로만 본다. `execute_with_s`(뷰/서브쿼리/CTE/트리거가 실제로 재진입하는 지점)에는 별도의 안전망을 둬서, 라우팅을 우회해 파티션 테이블의 유령 부모에 직접 도달하는 모든 경로(V1이 라우팅하지 않는 문장 종류 포함)를 침묵하는 빈 결과 대신 명확한 에러로 거부한다 — `execute_with_s`에서 재귀적으로 `execute()`를 다시 부르는 건 이미 outer `execute()`가 쥐고 있는 구조적 락을 같은 스레드에서 재획득하려는 시도라 안전하지 않기 때문(이 코드베이스가 반복적으로 피해온 "같은 뮤텍스를 한 스레드에서 두 번" 위험과 동일 계열).

**pruning**: RANGE는 기존 Gap Lock의 `extract_pk_gap_range`(컬럼명에 대해 이미 제네릭)를 그대로 재사용해 WHERE절에서 범위를 뽑고, 각 파티션의 암묵적 구간과 겹치는지 표준 구간-교차 검사로 판정. LIST/HASH는 AND절의 Eq/IN 리프를 스캔해서 좁힌다. 항상 "더 많이 스캔"하는 쪽으로만 틀릴 수 있어(정확성엔 영향 없음, 성능만) 안전.

**원자성**: 오토커밋 상태에서 여러 자식에 걸친 문장(예: 3개 파티션에 값이 걸친 다중 행 INSERT)은 내부적으로 `BEGIN`을 걸고 전체 자식 처리 후 `COMMIT`(중간 실패 시 `ROLLBACK`)해서 "한 문장처럼" 원자적으로 보이게 함 — 이미 명시적 트랜잭션 안이면 그 트랜잭션의 undo 로그가 그대로 커버(기존 FK 캐스케이드와 동일한 보장 수준).

**V1 제약 사항 (명확한 에러로 거부, 침묵 없음)**: 파티션 컬럼을 바꾸는 UPDATE(행이 자식 간 이동해야 함 — DELETE+INSERT로 우회), 파티션 테이블에 대한 ALTER TABLE ADD/DROP/MODIFY COLUMN(ADD/DROP PARTITION은 지원), 파티션 테이블을 FK 캐스케이드의 자식 쪽(ON UPDATE/DELETE CASCADE·SET NULL·SET DEFAULT 대상)으로 쓰는 것(부모 쪽 FK REFERENCES는 지원 — 존재 확인 로직이 자식 전체를 스캔하도록 별도 수정), RETURNING, GROUP BY/집계함수/DISTINCT/JOIN이 있는 SELECT, InsertSelect/MultiUpdate/MultiDelete/Merge, 다중 컬럼 파티션 키, 파티션 컬럼의 AUTO_INCREMENT(라우팅이 값을 미리 알아야 하는데 auto-increment는 자식 안에서 나중에 배정됨 — 선후관계 모순), 파티션 컬럼이 PK와 무관한 경우(PK가 있으면 파티션 컬럼이 PK의 일부여야 함 — 안 그러면 서로 다른 자식이 독립적으로 같은 PK 값을 배정할 수 있어 전역 유일성이 깨짐, MySQL의 "모든 유니크 키는 파티션 컬럼을 포함해야 함" 규칙과 동일한 이유).

**검증 중 발견한 실제 파서 버그**: `VALUES LESS THAN MAXVALUE`(MAXVALUE는 괄호 없음 — MySQL 문법에서 "괄호 안의 값" 형태와 "MAXVALUE" 형태는 서로 다른 두 가지 형태지, MAXVALUE가 괄호 안에 들어가는 게 아님)를 처음엔 항상 괄호 형태로 잘못 가정해 파싱 실패 — `engine_cli.exe`로 직접 재현해서 발견, 수정.

**테스트**: `test_partitioning.cpp` 신규 16케이스(RANGE/LIST/HASH 라우팅+pruning, UPDATE/DELETE 라우팅, 파티션 컬럼 UPDATE 거부, DROP/TRUNCATE/ALTER ADD·DROP PARTITION 생명주기, PK/AUTO_INCREMENT 제약 검증, 파티션 부모로의 FK 참조, 오토커밋 다중-자식 원자성, RETURNING/MultiUpdate 등 미지원 문장 거부가 침묵하지 않고 명확히 실패하는지, 비파티션 테이블 회귀 가드). Debug+Release **340케이스/22024assertion** 통과(324/21863에서 +16케이스/+161assertion), `test_full.sql`/`test_full-ver2.sql` 재실행(신규 회귀 없음).

**남은 범위**: SSI, XA — Section H 위 비교표 참고.

### LATERAL JOIN + 신규 집계 함수 확장 구현 완료 (BIT_AND/BIT_OR, FILTER, JSON_AGG, LATERAL JOIN — 2026-08-12)

`DIFF.md` Section 6/7(쿼리 기능/집계 함수) 비교표에서 사용자 요청으로 ROI 순 4개 항목을 골라 구현. BIT_AND/BIT_OR·JSON_AGG는 기존 STDDEV/VARIANCE·GROUP_CONCAT 패턴을 그대로 재사용하는 저난이도 확장, FILTER절은 기존 ~150줄짜리 집계별 분기를 건드리지 않는 scoped-shadowing 방식, LATERAL JOIN은 파서(AST 신규 필드)·실행기(조인 루프 조기 분기)·qualify(재귀 처리) 3개 계층에 걸친 아키텍처 확장이라 별도 `EnterPlanMode` 승인을 거쳐 진행.

**BIT_AND/BIT_OR**: `std::int64_t` 누적(다른 대부분 집계처럼 `double`이 아님) — AND의 빈 집합 항등값은 `-1`(all-bits-set), OR는 `0`.

**FILTER (WHERE ...)**: PostgreSQL 문법, 한 집계만 별도 행 집합으로 좁히고 같은 SELECT의 다른 집계·쿼리 자체의 WHERE/HAVING과는 독립(`COUNT(*) FILTER (WHERE active=1)`가 필터 없는 `COUNT(*)`와 같은 SELECT에 공존 가능). `compute_aggregates`에서 `grp_ptr`을 먼저 계산한 뒤 `const std::vector<Row>& grp = *grp_ptr;`로 바깥 `grp` 파라미터를 shadow(자기 자신을 참조하는 초기화자는 UB이므로 반드시 이 순서로 작성). GROUP_CONCAT 포함, OVER와의 결합은 V1 범위 밖(명확히 미지원).

**JSON_AGG**: `nlohmann::json::array()` 빌드, GROUP_CONCAT과 동일한 수집 루프 재사용. `format_num_or_int`와 동일한 정수 판별 규칙으로 `[1.0,2.0]`이 아닌 `[1,2]` 출력, NULL은 JSON `null`.

**LATERAL JOIN**: `[INNER|LEFT|CROSS] JOIN LATERAL (SELECT ...) alias [ON cond]` — 서브쿼리가 앞선 FROM/JOIN 테이블 컬럼을 참조하고 바깥 행마다 재평가됨(일반 FROM 서브쿼리는 1회만 평가되는 고정 임시 테이블). `Join` 구조체에 `subquery`(`Select::subquery`와 동일한 `optional<pair<StatementPtr,string>>`)와 `lateral` 필드 신규 추가 — `unique_ptr` 보유로 암시적 복사가 깨지는 문제는 `SelectColumn`과 동일한 명시적 깊은 복사 생성자 패턴으로 해결해, `Statement::Select`의 기존 `joins` 벡터 복사 코드는 한 줄도 안 건드림. 실행은 `exec_select`의 조인 루프 맨 앞에서 `j.lateral`이면 조기 분기해 왼쪽 행마다 `execute_with_s`로 서브쿼리를 재실행(→`parse_table_output`으로 재파싱) 후 `merge_right`/`null_right`(UNION 등 기존 코드가 이미 쓰던 병합 헬퍼)를 그대로 재사용. 서브쿼리 자신의 최상위 WHERE절만 바깥 행 값으로 치환(중첩 JOIN/HAVING/서브쿼리 내부는 V1 범위 밖). RIGHT/NATURAL/FULL OUTER + LATERAL은 파서 단계에서 명확한 에러로 거부. 락 계산 fast-path(`select_tables_ok`)는 LATERAL이면 FROM-서브쿼리와 동일한 이유(서브쿼리가 실행 시점에만 알 수 있는 테이블을 건드릴 수 있음)로 포기하고 전체 구조적 배타 락으로 폴백.

**검증 중 발견한 실제 실행기 버그**: `qualify_stmt`(모든 문장 실행 전 테이블명에 현재 DB 접두어를 붙이는 단계)가 호출하는 `qualify_join_` 헬퍼가 `Join`을 필드별로 새로 조립하면서 `subquery`/`lateral`을 그냥 누락시키고 있었고, 게다가 LATERAL의 별칭(`j.table`, 실테이블이 아님)까지 실테이블처럼 `qualify_name_with_synonyms`로 DB 접두어를 붙여버려 `"rusql.o" 테이블을 찾을 수 없음` 에러가 발생했다 — `engine_cli.exe`로 직접 재현해 발견, `qualify_join_`이 `lateral && subquery`일 때는 별칭을 그대로 두고 서브쿼리 자체만 재귀적으로 `qualify_stmt`하도록 수정.

**테스트**: `test_executor_select.cpp`에 신규 7케이스(BIT_AND/BIT_OR 1, FILTER 1, JSON_AGG 1, LATERAL JOIN INNER/LEFT/CROSS 의미론 + RIGHT/NATURAL/FULL 거부 4). Debug+Release 전체 스위트 통과, `test_full.sql`/`test_full-ver2.sql` 재실행 — 신규 회귀 없음(ver2의 BACKUP/RESTORE 에러 카운트는 기존에 이미 알려진 self-referential FK 이슈로, 이 작업 이전 커밋에서도 동일하게 재현되는 것을 직접 확인해 무관함을 검증 — [[rusql-test-full-ver2]] 참고).

## I. 테스트·문서 품질

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| ✅ 완료 | MCP execute_sql 독스트링이 실제 능력과 정반대 | `mcp_server.py:118-130` — AUTO_INCREMENT·CHECK·FK·EXISTS 등을 "미지원"이라 명시했으나 실제 서버로 직접 검증한 결과 전부 정상 지원(`docs/mds/FUNCTIONS.md`와도 일치). 독스트링을 실제 기능 요약으로 교체 | AI 자동화 품질 즉시 개선 | 매우 낮음 |
| ✅ 완료 | (신규 발견) SUM/COUNT(비교식) 파싱 불가 | `SELECT SUM(age > 26)` 같은 "조건부 집계" 패턴이 "Expected ')' after aggregate"로 실패 — 집계함수 인자 파서가 `*` 또는 단일 컬럼 식별자만 받고 그 뒤 어떤 연산자도 허용 안 함(원본 Rust `parser.rs`도 동일해 포팅 버그 아닌 기존 한계). `mysql` CLI로 재현. WHERE절이 이미 쓰던 조건-꼬리 파서(`parse_single_pred`)를 `parse_pred_tail(ArithExpr)`로 분리해 재사용 — `SUM(col IS NULL)`에 쓰이던 기존 CaseWhen 합성 방식과 동일하게 SumCase/CountCase로 변환(SELECT 목록 한정; HAVING 등 다른 경로의 `parse_arith_factor` 쪽 별도의 문자열 기반 집계 표현은 미해당, 별도 이슈로 분리) | `SUM`/`COUNT`(비교식·IS NULL·LIKE·BETWEEN·IN) 조건부 집계 정상 동작 | 중간 |
| ✅ 완료 | (신규 발견) MCP의 UI 제어형 도구 9개(write_to_editor 등)가 전부 미작동 | `mcp_server.py`의 `_run_ui`가 `UI:{...}` 문자열을 엔진 서버의 SQL 포트로 직접 전송하는데, C++ 엔진 서버는 물론 원본 Rust 서버(`rusql-server`)에도 이 접두사를 처리하는 코드가 전혀 없음(포팅 회귀 아닌 원래부터 죽어있던 기능). 실제 MCP 클라이언트 프로토콜(stdio JSON-RPC)로 직접 붙어 확인 — payload에 세미콜론이 있으면 `ERR Unknown statement: UI`로 즉시 실패, 없으면(예: get_current_database) 서버가 문장 종료를 못 찾아 클라이언트가 응답 없이 멈춤(테스트에서 30초 타임아웃). 새 IPC 채널을 만드는 대신 9개 도구와 `_run_ui`를 제거하고 `main.rs`의 Claude Desktop 설정 등록(`alwaysAllow`)도 정리 | 존재하지 않는 기능이 조용히 멈추는 대신 "Unknown tool"로 즉시 실패, MCP가 실제로 지원하는 7개 도구만 정직하게 노출 | 낮음(제거) / 높음(실제 구현 시, 별도 후속 논의 필요) |
| ✅ 완료 | (신규 발견) MCP `list_databases`/`list_tables`/`sample_data`/`get_indexes`/`execute_sql`이 독스트링과 달리 실제 행 배열을 반환 안 함 | 원인: `_parse_table_output`이 응답의 첫 줄(항상 상태 줄 "OK")을 표 헤더로 오인해 탭 존재 여부를 검사 — 항상 실패해 원본 텍스트를 `{"result": "..."}`로 통째로 감싸서 반환. 게다가 native 프로토콜의 `SHOW DATABASES`/`SHOW TABLES`/`SELECT` 응답 자체가 탭 구분이 아니라 박스 그림(`+---+`/`\|`) 형식이라 이 파서가 애초에 처리 못 하는 형식이었음. `execute_sql`도 별개로 `not stripped.startswith("OK")` 조건이 있어 성공 응답(항상 "OK"로 시작)에서는 파싱 자체가 항상 스킵되던 논리 오류 있었음. "OK"/"ERR" 상태 줄과 타이밍/행수 요약 줄을 먼저 걷어낸 뒤 박스 그림·탭 구분 두 형식 모두 파싱하도록 `_parse_table_output` 재작성, `execute_sql`의 스킵 조건도 "ERR이 아니면 일단 파싱 시도"로 수정. 실제 MCP 클라이언트로 재검증 — `list_databases`가 `[{"Database": "mcp_check2"}]`처럼 실제 행 객체 반환하는 것, `execute_sql`의 SELECT도 `[{"id": "1", "name": "alice"}, ...]`로 정상 반환하는 것, 에러 케이스는 여전히 원본 ERR 텍스트 그대로 반환하는 것 확인 | MCP 도구가 독스트링대로 실제 JSON 배열 반환 | 중간 |
| P1 | executor.rs·parser.rs 자동화 테스트 0개 | `#[test]` 20개뿐, CI 없음 | 리팩토링 안전망 확보 | 중간~높음 |
| P3 | 코드-문서 불일치 정리 | mysql.rs 헤더 주석, rusql-ui README, 스크래치 파일 | 저장소 정리 | 낮음 |

---

### 분석 방법

rusql-core(엔진·플래너·파서·스토리지·트랜잭션), rusql-server/rusql-client/rusql-mcp(프로토콜·클라이언트), rusql-ui(Tauri+React)를 4개 영역으로 나눠 각 파일을 처음부터 끝까지 정독하며 FUNCTIONS.md/DIFF.md의 서술과 대조했다. 일부 항목(파서 EOF 미검증, B+Tree 숫자 키 충돌, 이중 음수 파싱 등)은 실제로 rusql-cli를 빌드해 해당 SQL을 실행해 결과를 직접 확인했다. 파일:라인 번호는 분석 시점 기준이며, 이후 코드 변경으로 달라졌을 수 있다.
