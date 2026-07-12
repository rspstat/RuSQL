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
| P1 | SELECT USER()가 항상 root@localhost | `mysql.rs:648-652` — 실제 인증 사용자명 미반영 | 권한 확인 툴 오작동 방지 | 낮음 |
| P1 | 모든 SET 문이 무조건 OK 응답 | `mysql.rs:637-638` — SET PASSWORD 등 실제 변경 없이도 성공 응답 | DBA 도구 신뢰성 | 낮음 |
| ✅ 완료 | BEGIN...END 내부 세미콜론으로 프로시저 조기 실행 | 원인(Rust `rusql-server/main.rs:94-111,241-247`): UI 실사용 테스트로 2건 확인 — (1) 트랜잭션 `BEGIN;`이 블록으로 오인되어 이후 전체가 한 문장으로 합쳐짐(`test_full.sql`), (2) 트리거/프로시저처럼 본문이 여러 줄에 걸친 `BEGIN...END`는 커넥션 루프가 버퍼에 `;`이 하나라도 보이면(깊이 무시) 바로 분리 후 버퍼를 통째로 비워, 본문 내부 문장·`END`가 최상위 문장으로 새어나감(`test_full-ver2.sql`의 멀티라인 트리거로 실증). C++ `server/main.cpp`의 커넥션 루프를 CLI의 깊이 인식 방식(`find_stmt_end`)과 동일한 점진적 추출 방식으로 재작성; `test_server_stmt_split.cpp`에 회귀 테스트 추가 | 저장 프로시저/트리거 생성 및 실행 안정성 | 중간 |
| P1 | rusql-client 세미콜론 카운트 불일치→hang | `rusql-client/main.rs:33-57,184-204` — BEGIN/END 미인식, 타임아웃 없음 | 클라이언트 안정성 | 중간 |

## B. 동시성 아키텍처 (가장 가치 있는 구조 개선)

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| P1 | 전역 단일 락으로 모든 문장 완전 직렬화 | `executor.rs:674-683` — SELECT 포함 모든 문장이 전체 write lock 획득, 두 세션 동시 실행 불가 | LockManager·격리수준·MVCC·병렬실행이 실질적 이득으로 연결 (최우선 리팩토링) | 높음 |
| P1 | 트랜잭션 스냅샷이 DB 전체 deep clone | `txn_manager.rs:262-278` — BEGIN마다 `tables.clone()` 전체 복제 | 행 단위 버전 체인 전환 시 비용 절감 | 높음 |
| P1 | 버퍼 풀이 테이블 전체 단위 캐싱(사실상 무의미) | `buffer_pool.rs` — 시작 시 전 테이블이 이미 로드되어 get_page 경로 도달 불가 | 페이지 캐싱 구현 시 대용량 테이블 지원 | 높음 |
| P1 | 내부 쿼리 합성이 ASCII 표 문자열 재파싱 방식 | `executor.rs` 다수 — UNION/CTE/서브쿼리가 표시용 표를 `\|`로 split해 재파싱, 값에 `\|`/개행 있으면 깨짐 | 구조화된 내부 API로 정확성·성능 개선 | 높음 |
| P2 | 다중 조인 알고리즘 선택이 누적 카디널리티 미반영 | `planner.rs:121-125` — 2·3번째 조인이 항상 원래 base 테이블 행수 기준 | 다중 테이블 조인 실행계획 정확도 | 중간 |
| P1 | 프로시저 루프/트리거 재귀에 상한·타임아웃 없음 | `executor.rs:9210-9285,9381-9391` — 전역 락 구조상 무한루프 하나가 서버 전체 정지 | 서버 가용성 확보 | 중간 |

## C. 트랜잭션 내구성 (WAL·Undo·Checkpoint)

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| ✅ 완료 | (C++ 신규 회귀) WAL 커밋 fsync가 완전히 무동작 | 원인: 원본 Rust `wal.rs`/`group_commit.rs`는 `file.sync_all()`로 실제 fsync를 하는데, C++ 이식 과정에서 "std::fstream has no portable fsync"라는 (틀린) 주석과 함께 `flush()`만 호출 — `group_commit.cpp`는 그 `flush()`조차 아무것도 안 쓴 새로 연 빈 스트림에 호출해 완전히 무동작이었음. **이식 때 새로 생긴 회귀지 원본 버그가 아님.** C++ `wal.cpp`/`group_commit.cpp`를 C stdio(`FILE*`)로 바꿔 `_commit`(Windows)/`fsync`(POSIX)로 실제 디스크 동기화하도록 수정 | 커밋 직후 크래시 시에도 데이터 유실 방지 (그룹 커밋 기능의 존재 이유 자체) | 중간 |
| ✅ 완료 | 테이블/스키마 파일이 fsync 없이 flush만 됨 + 저장이 원자적이지 않음 | 원인(Rust `disk.rs:150-179,154-156,203-245`): `truncate(true)`로 열자마자 즉시 write, fsync 없음 — 크래시 시 손상되거나(truncate 후 write 실패) 디스크에 실제로 반영 안 됐을 수 있음. C++ `disk.cpp`의 `write_file`/`write_file_bytes`를 "임시파일(.tmp)에 쓰기 → fsync → 원래 경로로 원자적 rename" 방식으로 재작성(WAL과 동일한 `_commit`/`fsync` 사용) | 테이블/스키마/인덱스메타 등 모든 영속 파일의 내구성·원자성 확보 | 중간 |
| ✅ 완료 | (위 두 수정이 남긴 신규 갭) fwrite/fflush/fsync 실패를 아무도 확인 안 함 | 원인: 원본 Rust는 `.expect(...)`로 파일 열기/쓰기/fsync 실패 시 즉시 panic(=디스크 풀 등 I/O 에러 시 "커밋됐다"는 거짓 성공 응답 대신 프로세스 중단)하는데, 위 두 항목을 고치며 새로 쓴 C stdio 코드(`wal.cpp`/`disk.cpp`)와 기존 `txn_manager.cpp`의 Undo 로그 append가 반환값을 하나도 확인 안 하고 있었음(C++-대-Rust 전체 대조 감사에서 발견). 세 곳 모두 `fwrite`/`fflush`/`_commit`·`fsync`/스트림 open 실패 시 예외를 던지도록 수정, Rust의 `.expect()` 실패 시맨틱과 동일하게 맞춤 | I/O 실패를 거짓 커밋 성공으로 조용히 삼키는 것 방지 | 낮음 |
| ✅ 완료 | (실사용 중 실제로 크래시 재현) 커넥션 핸들러에 예외 안전장치 없음 → 서버 전체 다운 | 원인: 위 항목대로 예외를 던지게 고친 뒤, UI에서 벤치마크 실행 중 실제로 `engine_server.exe`가 `abort()`로 죽는 걸 사용자가 직접 겪음(Debug Error 다이얼로그) — Rust는 커넥션 스레드 하나가 panic해도 `std::thread::spawn`이 그 스레드만 격리해서 죽이는데, C++은 스레드 안에서 예외가 하나라도 처리 안 되고 새어나가면 `std::terminate()`가 프로세스 전체를 abort시켜 **접속해있는 모든 클라이언트가 같이 끊김**. 정확한 예외 트리거 지점은 재현 못 했지만(사용자의 오래 누적된 데이터 디렉터리에서만 발생, 새 디렉터리 3종 시나리오로 재현 시도했으나 실패), 근본 원인(예외 안전장치 부재)은 명확해 서버(`server/main.cpp`)와 CLI(`cli/main.cpp`) 양쪽의 쿼리 실행부를 try/catch로 감싸 예외를 그 쿼리 하나의 ERR 응답으로 변환하도록 수정 | 어떤 내부 오류가 나도 서버 전체가 아니라 해당 쿼리 하나만 실패 (Rust의 스레드별 panic 격리와 동등한 효과) | 낮음 |
| P1 | WAL/Undo "트랜잭션 제거"가 비원자적 전체 재작성 | `wal.rs:234-249, txn_manager.rs:181-206` — 다른 세션과 공유 파일이라 크래시 시 타 세션 기록도 손상 가능 | 안전한 로그 관리 | 높음 |
| P1 | 체크포인트-그룹커밋 TOCTOU 레이스 | `executor.rs:824` vs 실제 fsync 시점 불일치 | 크래시 복구 시 커밋 유실 방지 | 높음 |
| P1 | WAL/Undo 디코딩이 첫 손상 지점서 이후 전부 폐기 | `wal.rs:80-95, txn_manager.rs:101-123` — 체크섬 없음 | 부분 손상에도 최대 복구 | 중간 |
| ✅ 완료 | 크래시 복구가 보조/복합 인덱스 미갱신 | 원인(Rust `executor.rs:8130-8298`, 이식 시 그대로 보존): REDO/UNDO replay가 `s.tables`와(REDO INSERT에 한해서만) PK B+Tree만 갱신하고, 보조 btree/hash/복합 인덱스는 물론 UPDATE/DELETE/UNDO 경로의 PK B+Tree조차 갱신 안 함. C++ `executor_txn.cpp`의 `recover_from_wal`에서 replay 중 건드린 테이블을 모아 replay 완료 후 PK/보조/해시/복합 인덱스를 전부 재구축하도록 수정. `test_executor_txn.cpp`에 `commit_write_record()`로 "커밋 WAL은 썼지만 finalize 전 크래시"를 재현하는 회귀 테스트 추가 | 크래시 복구 후 인덱스 기반 조회 정확성 | 높음 |
| P2 | SERIALIZABLE이 phantom(행 개수)만 감지 | `txn_manager.rs:293-310` — write-skew 미탐지 | 이상현상 탐지 정교화 | 높음 |
| P1 | Lock wait timeout이 실제로 대기 안 함 | `executor.rs:3319-3378` — 즉시 에러 반환, sleep 없음(사실상 NOWAIT) | 락 타임아웃 설명대로 동작 | 중간 |
| P2 | READ UNCOMMITTED/COMMITTED 코드상 미분화 | `txn_manager.rs:262-289` — 동일 분기 처리 | 격리수준 문서-동작 일치 | 중간 |

## D. 보안

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| ✅ 완료 | 사용자 테이블 비면 인증 무조건 통과 | 원인(Rust `executor.rs:198,215`, 이식 시 그대로 보존): fail-open (`if users.is_empty() { return true }`). 서버는 두 리스너(native/MySQL) 모두 `ensure_default_user()`를 부팅 시 미리 호출해 실제로는 이 분기가 도달 안 되지만, 방어적으로 C++ `validate_credentials`/`verify_mysql_native_password`에서 open-mode 폴백 완전히 제거(fail-closed) | 향후 진입점 추가/`ensure_default_user` 누락 시에도 인증 우회 불가 | 낮음 |
| ✅ 완료 | BACKUP/RESTORE 경로 미검증 | 원인(Rust `parser.rs:3783-3830, executor.rs:7435-7478`, 이식 시 그대로 보존): 사용자가 준 파일명을 그대로 `fs::write`/`fs::read_to_string`에 전달 — BACKUP은 임의 파일 쓰기, RESTORE는 임의 파일을 읽어 SQL로 **실행**(사실상 SQL include). C++ `executor_backup.cpp`에 `resolve_backup_path` 추가 — 영문/숫자/`_`/`-`/`.`만 허용하는 순수 파일명만 받아 `<data_dir>/_backups/`로 샌드박싱, 절대경로·`..`·구분자 전부 거부. 부수적으로 `DiskManager::list_databases()`가 새로 생긴 `_backups` 디렉터리를 실제 DB로 오인해 기본 `current_db`를 가로채던 버그도 같이 발견해 수정(밑줄로 시작하는 최상위 디렉터리는 전부 제외) | 임의 파일 읽기/쓰기(SQL include) 취약점 제거 | 낮음~중간 |
| P1 | Native TCP가 비밀번호 평문 전송 | `main.rs:185-199, client/main.rs:120` — MySQL 프로토콜은 이미 SHA1 챌린지-응답 구현했는데 반대로 이게 더 취약 | 두 프로토콜 보안 수준 통일 | 중간 |
| P1 | MySQL 리스너 기본 0.0.0.0 바인딩 | `mysql.rs:1030` — native는 127.0.0.1인데 이건 LAN 전체 노출, root/root 기본계정과 결합 시 위험 | 안전한 기본값/명시적 바인드 선택 | 낮음 |
| P2 | TLS/SSL 전무 | `code/backend/third_party`에 OpenSSL/Schannel 등 TLS 라이브러리 없음 (원본 Rust도 rustls/native-tls 없었음) | 전송 계층 보안 | 높음 |
| P2 | mcp_server.py 하드코딩 접속정보, 재시도 없음 | `mcp_server.py:14-32` | 배포 유연성, 응답속도 개선 | 낮음 |

## E. 인덱스·성능 심화

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| P2 | B+Tree 삭제 시 언더플로우 리밸런싱 없음 | `btree.rs:357` 주석에 명시 — 삭제 잦으면 트리 무한정 성겨짐 | 장기 운영 성능 저하 방지 | 높음 |
| P2 | FK/UNIQUE 검증이 O(n) 선형 스캔 | `executor.rs:2021-2025, 1885-1968` — 이미 있는 인덱스 미활용 | 대량 INSERT 시 O(log n)으로 개선 | 중간 |
| P2 | Index Intersection이 결국 전체 재스캔 | `executor.rs:2623-2680` — 포인트 룩업 미사용 | 문서상 성능 이득 실제 구현 | 중간 |
| P1 | 재귀 CTE 1000회 상한 도달 시 무경고 종료 | `executor.rs:1603` | 결과 완전성 신뢰 | 낮음 |
| P2 | 재귀 CTE 중복제거 O(n²) | `executor.rs:1621` — Vec::contains 사용 | HashSet 전환 시 성능 개선 | 낮음 |
| P2 | GROUP BY 병렬집계 임계값 체크 누락 | `executor.rs:3062-3063` | 소규모 쿼리 오버헤드 제거 | 낮음 |
| P2 | 병렬 임계값 환경변수 오타로 항상 무시 | `executor.rs:38-40` — `RUSTDB_parallel_min_rows()` 오타 | 튜닝 옵션 실제 동작 | 낮음 |

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
| P1 | 파괴적 작업에 확인 절차 없음 | DROP DATABASE/TABLE/VIEW/INDEX, 연결삭제가 클릭 한 번에 즉시 실행 | 실수 방지, UX 완성도 | 낮음 |
| P1 | 컴파일타임 개발자 경로 하드코딩→배포 불가 | `main.rs` — `env!("CARGO_MANIFEST_DIR")`, open_terminal 절대경로 리터럴 | 다른 PC 배포 가능 | 중간 |
| P1 | MCP 설정 병합 실패 시 기존 설정 덮어씀 | `main.rs:953-960` — JSON 파싱 실패 시 빈 객체로 대체 | 사용자 기존 설정 보존 | 낮음 |
| P2 | App.tsx 4090줄 단일 컴포넌트 | useState 41개·useEffect 12개가 한 함수에 혼재 | 유지보수성 향상 | 높음 |
| P3 | 백엔드 죽은 코드 4개 + CSV 임포트 UI 부재 | export_csv/import_csv/get_views/get_indexes 미호출 | 정리 + 기능 하나 추가 가능 | 낮음~중간 |
| P2 | Mutex/RwLock unwrap 다수→패닉 전파 | main.rs 52곳 | 단일 장애점 제거 | 낮음~중간 |

## H. 신규 기능 확장 (DIFF.md 갭 분석, 여유 시간에 따라 선택)

| 우선순위 | 항목 | 기대 효과 | 난이도 |
|---|---|---|---|
| P3 | LOCK TABLES / Gap Lock | 동시성 제어 갭 해소 | 중간 |
| P3 | REPLACE INTO / DEFERRABLE 제약 | 실무 관용구 지원 | 낮음~중간 |
| P3 | 표현식/부분/내림차순 인덱스 | 인덱스 실무 완성도 | 중간 |
| P3 | LATERAL JOIN | 표준 SQL 지원 확장 | 높음 |
| P3 | FILTER절/ARRAY_AGG/JSON_AGG/PERCENTILE_CONT | 집계 표현력 확장 | 중간 |
| P3 | CREATE SEQUENCE / 파티셔닝 | DDL 완성도(범위 큼, 후순위) | 높음 |
| P3 | 열 레벨 권한 / 이벤트 스케줄러 | DCL·운영 완성도 | 중간 |

## I. 테스트·문서 품질

| 우선순위 | 항목 | 현재 문제 (근거) | 기대 효과 | 난이도 |
|---|---|---|---|---|
| ✅ 완료 | MCP execute_sql 독스트링이 실제 능력과 정반대 | `mcp_server.py:118-130` — AUTO_INCREMENT·CHECK·FK·EXISTS 등을 "미지원"이라 명시했으나 실제 서버로 직접 검증한 결과 전부 정상 지원(`docs/mds/FUNCTIONS.md`와도 일치). 독스트링을 실제 기능 요약으로 교체 | AI 자동화 품질 즉시 개선 | 매우 낮음 |
| ✅ 완료 | (신규 발견) SUM/COUNT(비교식) 파싱 불가 | `SELECT SUM(age > 26)` 같은 "조건부 집계" 패턴이 "Expected ')' after aggregate"로 실패 — 집계함수 인자 파서가 `*` 또는 단일 컬럼 식별자만 받고 그 뒤 어떤 연산자도 허용 안 함(원본 Rust `parser.rs`도 동일해 포팅 버그 아닌 기존 한계). `mysql` CLI로 재현. WHERE절이 이미 쓰던 조건-꼬리 파서(`parse_single_pred`)를 `parse_pred_tail(ArithExpr)`로 분리해 재사용 — `SUM(col IS NULL)`에 쓰이던 기존 CaseWhen 합성 방식과 동일하게 SumCase/CountCase로 변환(SELECT 목록 한정; HAVING 등 다른 경로의 `parse_arith_factor` 쪽 별도의 문자열 기반 집계 표현은 미해당, 별도 이슈로 분리) | `SUM`/`COUNT`(비교식·IS NULL·LIKE·BETWEEN·IN) 조건부 집계 정상 동작 | 중간 |
| ✅ 완료 | (신규 발견) MCP의 UI 제어형 도구 9개(write_to_editor 등)가 전부 미작동 | `mcp_server.py`의 `_run_ui`가 `UI:{...}` 문자열을 엔진 서버의 SQL 포트로 직접 전송하는데, C++ 엔진 서버는 물론 원본 Rust 서버(`rusql-server`)에도 이 접두사를 처리하는 코드가 전혀 없음(포팅 회귀 아닌 원래부터 죽어있던 기능). 실제 MCP 클라이언트 프로토콜(stdio JSON-RPC)로 직접 붙어 확인 — payload에 세미콜론이 있으면 `ERR Unknown statement: UI`로 즉시 실패, 없으면(예: get_current_database) 서버가 문장 종료를 못 찾아 클라이언트가 응답 없이 멈춤(테스트에서 30초 타임아웃). 새 IPC 채널을 만드는 대신 9개 도구와 `_run_ui`를 제거하고 `main.rs`의 Claude Desktop 설정 등록(`alwaysAllow`)도 정리 | 존재하지 않는 기능이 조용히 멈추는 대신 "Unknown tool"로 즉시 실패, MCP가 실제로 지원하는 7개 도구만 정직하게 노출 | 낮음(제거) / 높음(실제 구현 시, 별도 후속 논의 필요) |
| P1 | executor.rs·parser.rs 자동화 테스트 0개 | `#[test]` 20개뿐, CI 없음 | 리팩토링 안전망 확보 | 중간~높음 |
| P3 | 코드-문서 불일치 정리 | mysql.rs 헤더 주석, rusql-ui README, 스크래치 파일 | 저장소 정리 | 낮음 |

---

### 분석 방법

rusql-core(엔진·플래너·파서·스토리지·트랜잭션), rusql-server/rusql-client/rusql-mcp(프로토콜·클라이언트), rusql-ui(Tauri+React)를 4개 영역으로 나눠 각 파일을 처음부터 끝까지 정독하며 FUNCTIONS.md/DIFF.md의 서술과 대조했다. 일부 항목(파서 EOF 미검증, B+Tree 숫자 키 충돌, 이중 음수 파싱 등)은 실제로 rusql-cli를 빌드해 해당 SQL을 실행해 결과를 직접 확인했다. 파일:라인 번호는 분석 시점 기준이며, 이후 코드 변경으로 달라졌을 수 있다.
