# RuSQL 추천 목록 (남은 항목만, R59 이후 · 2026-10-31 · 항목마다 엔진·코드로 다시 확인한 판)

완료된 항목은 뺐습니다(R44~R59에서 끝낸 것은 `DATE.md`에 있습니다). 이 판은 10-26에 표의 항목을 엔진에 실제로 돌려 보고(진짜 MySQL 8.4.9와 비교) 앱·MCP 코드를 찾아 이미 있거나 일부만 있는 것을 가려낸 결과에서, R57이 고친 것(REPLACE의 행 유실, FLOAT 값, 한 열의 외래키 둘, USING·NATURAL·CALL의 빈 결과 열 이름, `SELECT 'a'` 머리글, FROM 없는 WHERE)과 R58이 고친 것(FLOAT가 다른 타입과 섞인 함수·함수 결과의 산술·윈도 `MIN`/`MAX`, 이어지는 USING 조인의 `*` 열 순서, 자동 외래키 이름, `NO ACTION` 표기, MySQL 프로토콜의 오류 번호)과 R59가 한 것(느린 쿼리 로그, 세션 패널의 KILL 버튼·차단자 표시, EXPLAIN 계획 그림, 결과 JSON 내보내기·차트, 스키마 비교, ERD 드래그 연결, `suggest_indexes`의 전·후 모드와 하위 질의·CTE·OR 해석)을 뺀 것입니다. 점검에서 바뀐 것은 맨 아래 "점검에서 바로잡은 것"에 모았습니다. 지금 받아들이는 SQL 문법 전체는 `SQL.md`에 있습니다. 발표·제출물 중 포스터·소개글·메인 화면 캡처는 끝났습니다(`docs/deliverables/`).

- **현재 상태:** `일부`(일부만 있음) · `없음` · `오답`(지금 조용히 틀린 값을 돌려줌)
- **추천도:** ★ 다섯 개. 기준은 실사용 가치 + 발표 임팩트 + 비용 대비 효과입니다. 졸업작품 마감(11월 말)까지 약 5주를 전제로 합니다.
- **범위 (2026-10-25):** SQL 문법 추가는 맨 아래 "6. 보류"로 뺐고, 지금은 기능(엔진 동작·도구·화면·발표)에 집중합니다.
- **비용:** 제가 구현하고 검증(코퍼스 비교, 심은 버그, 전체 시험, 문서)까지 마치는 데 걸리는 시간입니다. 하 ≈ 2~4시간, 중 ≈ 하루, 상 ≈ 2~4일. 빌드와 전체 시험 한 바퀴가 약 35분이라 헤더를 건드리는 변경은 여러 바퀴가 듭니다. **프론트(UI)는 자동 시험이 없어서 눈으로 확인하는 시간이 따로 붙습니다.**

## 1. 엔진 기능

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| 쓰는 프로시저의 `CALL`·`OPTIMIZE`가 redo 로그를 비움 | 일부 — R55에서 읽기 문장(`WITH`·파생 표·`PREPARE`·`EXECUTE`·`LOCK`·`ANALYZE`·하위 질의가 든 `SET`·본문이 읽기뿐인 `CALL`)은 로그를 두게 했지만, 본문에 쓰기가 있는 `CALL`과 `OPTIMIZE TABLE`은 여전히 체크포인트함 | ★ | 하~중 | `CALL` 본문의 `INSERT`·`UPDATE`·`DELETE`만이면 redo로 덮이는지 확인하고 통과시킬 수 있습니다(지금은 본문의 쓰기마다 테이블 파일도 그 자리에서 씀). 프로시저가 쓰는 장면에서만 보입니다. |
| 자기 표 UPDATE가 행 단위가 아님 | 일부 — 문장 전체를 한 번에 봐서 `UPDATE emp SET mgr = NULL, id = id + 10 ORDER BY id DESC`를 MySQL은 통과시키는데 여기는 거절합니다(R57에서 다중 표 DELETE·MERGE·REPLACE의 사슬 아래 검사는 끝남) | ★ | 중 | UPDATE를 행마다 모의해야 합니다. 오류로 거절하는 쪽이라 조용한 오답은 아닙니다. |
| `USING` 조인 뒤의 모호한 열 | 오답(작음) — `a JOIN b USING (x) JOIN c USING (y) JOIN d USING (x)`에서 `d`도 `y`를 가지면 `ORDER BY y`는 MySQL이 오류 1052(모호한 열)로 거절하는데 여기는 행을 돌려줌. `SELECT a.*, c.* … ORDER BY y`도 같은 모양 | ★ | 하 | R58에서 `*` 열 순서를 맞추다 알게 된 것입니다. 합쳐진 열이 뒤 조인의 표에도 있을 때의 모호함 검사입니다. |
| FLOAT 열을 글자 함수에 넣으면 6자리가 아님 | 오답(작음) — `CONCAT(f, '')`가 MySQL은 `3.14159`(FLOAT는 6자리 글자), 여기는 저장된 글자 `3.1415927`. (FLOAT를 내는 함수 결과를 글자로 바꾸면 MySQL은 넓힌 값 `0.10000000149011612`를 줌: `CONCAT(COALESCE(f, 0), '')`) | ★ | 하 | R58의 코퍼스를 만들다 알게 된 것입니다. 글자 함수가 인자의 FLOAT 여부를 알아야 합니다. |
| 프로토콜 오류 번호의 남은 것 | 일부 — R58에서 메시지 글자로 번호·SQL 상태를 정함(`wire_error`, 약 40종). 코퍼스 전체에서 번호만 다른 문장 385개 → 약 20개. 남은 것은 엔진이 MySQL과 다른 이유로 실패하는 경우(문법을 못 받음, 메시지가 아무 말도 안 함): `ONLY_FULL_GROUP_BY`(1055), `RANGE` 프레임 오류(3587~3589), 윈도 함수를 쓸 수 없는 자리(3593), `SUM(SUM(x)) OVER`의 1140, `Data truncated`(1265) 일부, `MOD`로 0을 나누는 UPDATE(1365), 없는 윈도 이름(3579) | ★ | 하 | 번호는 `vs_mysql.py --proto --errno`가 코퍼스마다 비교합니다. 엔진 메시지를 MySQL 것으로 바꾸는 쪽이 정석이라 하나씩 고치면 됩니다. |
| 몇 가지 `SHOW`의 빈 결과·문법 | 일부 — R57에서 USING·NATURAL 조인과 `CALL`의 빈 결과도 열 이름을 말하게 됨. 남은 것: `SHOW DATABASES LIKE`·`SHOW ERRORS`는 문법 오류, `SHOW GRANTS FOR 없는사용자`는 오류 1141 대신 빈 성공 | ★ | 하 | 작은 모양들입니다. MySQL 클라이언트가 접속 직후 보내는 문장은 아닙니다. |
| 문장 중단의 남은 것 | 일부 — `KILL QUERY`는 SELECT의 행 루프와 행 잠금 대기를 멈춤. 실행 중인 쓰기 문장은 끝까지 돌고(반쯤 쓰다 멈추면 되돌릴 길이 없어서 일부러), 정렬·집계·조인 해시 구성 같은 한 덩어리 단계는 끝나야 멈춤. **R59에서 세션 패널의 `Stop statement`를 눌러 보다 확인**: 교차 조인 `FROM a, b, c`는 멈추지 않음 | ★★ | 중 | 쓰기 문장의 중단은 문장 단위 롤백이 있어야 안전하고, 덩어리 단계는 확인점(`tick`)을 더 넣어야 합니다. 이제 버튼이 있어서 눌러도 안 멈추는 것이 화면에서 보입니다. |
| **외래키 값 클레임의 드문 경합 (원인 미확인)** | 오답(드묾) — R59의 `idx0` 전체 시험 한 번에서 `sessions racing a child row against its parent leave no child without a parent`가 `orphans == 1`로 실패: 걸음이 끝난 뒤에 부모 없는 자식 행이 하나 있었음. 같은 시험을 따로 80번, 전체 시험을 두 번 더 돌렸을 때는 나오지 않음(약 90번 중 1번). 이 빌드 이전부터 있었는지도 모름 | ★★★★ | 중~상 | 데이터 무결성이 걸린 드문 실패라 마감 전에 한 번은 잡아야 합니다. 첫 걸음: 시험이 실패한 걸음의 문장 순서를 남기게 하고(`doing` 표), 시드를 고정해 반복하고, R58 빌드로 같은 반복을 돌려 이전부터인지 가립니다. |
| **교차 조인이 메모리를 한꺼번에 씀** | 일부 — 300행 표 셋의 `FROM a, b, c WHERE …`(27M 쌍)이 메모리를 약 6 GB까지 올리고 멈추지 않으며, 그동안 같은 표를 쓰는 다른 문장이 기다림. 1500행 둘(2.25M 쌍)도 34초 | ★★★ | 중 | 조인의 중간 결과를 행 쌍으로 모두 만들어 둔 뒤 거릅니다. 스트리밍으로 걸러 내거나 `tick`을 넣으면 발표 중 실수로 서버가 죽는 일을 막고 `KILL`도 듣게 됩니다. R59에서 세션 패널을 시험하다 알았습니다. |
| `SET slow_query_log`(GLOBAL 없이)와 `long_query_time`의 세션 값 | 다름(작음) — MySQL은 `SET slow_query_log = ON`을 오류 1229로 거절하고 `long_query_time`에는 세션 값이 따로 있어(`SET GLOBAL`은 이미 열린 연결에 닿지 않음) 여기는 둘 다 서버 전체 하나로 받음. `slow_query.log`는 커지기만 함 | ★ | 하 | 일부러 단순하게 둔 것이라(앱의 패널이 켜자마자 보이게) 서버 변수 범위(GLOBAL/SESSION)를 구분하는 일이 먼저 필요합니다. 파일 크기 상한·회전은 따로 하~중. |
| `EXPLAIN UPDATE` · `EXPLAIN DELETE` | 없음 ("EXPLAIN: Update → not a SELECT") | ★★ | 하~중 | MySQL은 쓰기 문장의 계획도 보여 줍니다. 쓰기 문장이 인덱스를 타는지 앱에서 확인할 수 있게 됩니다. |
| `OPTIMIZE TABLE` · `CHECK TABLE`을 앱(네이티브)에서도, 진짜 `CHECK`, `FLUSH` · `REPAIR` · `CHECKSUM TABLE` | 일부 — **MySQL 프로토콜로 접속하면 `OPTIMIZE`(= `VACUUM`)·`CHECK`·`ANALYZE TABLE`이 이미 됩니다**(`mysql.cpp`; `CHECK`는 표가 있으면 OK만 돌려주는 껍데기, 실제 검사는 안 함). 앱·CLI(네이티브)에서는 `OPTIMIZE`·`CHECK`가 "Unknown statement"이고, `FLUSH`·`REPAIR`·`CHECKSUM TABLE`은 어느 쪽에도 없음. `VACUUM`·`ANALYZE`·체크포인트는 네이티브에도 있음 | ★★ | 하~중 | `OPTIMIZE`는 `VACUUM`+체크포인트의 이름 바꿈, `CHECK TABLE`은 인덱스와 행이 맞는지 검사하는 새 기능입니다. `mysqldump`가 `FLUSH TABLES`를 보낼 수 있습니다. |
| `FULLTEXT` / `MATCH … AGAINST` | 없음 (`FULLTEXT KEY`가 든 CREATE TABLE이 오류) | ★★★ | 상 | 검색 데모는 강하지만 역색인과 점수 계산이 큽니다. |
| 표현식(함수) 인덱스 | 없음 (`CREATE INDEX … ((a + 1))`가 문법 오류) | ★★ | 중~상 | MySQL·PostgreSQL·Oracle 모두 있습니다. 인덱스 키가 식이라 플래너와 유지가 바뀝니다. |
| 열 단위 권한 (`GRANT SELECT (col) ON t`) | 없음 | ★★ | 중 | MySQL·PostgreSQL에 있고 Oracle은 INSERT·UPDATE만입니다. |
| 감사 로그 | 없음 | ★★ | 중 | 셋 다 있습니다(PostgreSQL은 확장). 누가 무엇을 했는지 기록합니다. |
| 공간 타입·인덱스, 특수 인덱스(비트맵·BRIN·GIN/GiST) | 없음 | ★ | 상 | 범위가 크고 이번 마감에는 맞지 않습니다. |

## 2. 프론트 (앱)

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| 앱 안의 "인덱스 제안" | 없음 (`suggest_indexes`는 MCP 도구뿐이고 Python 로직) | ★★★ | 중~상 | 에디터의 문장에 대해 EXPLAIN 막대 옆에 "이 인덱스를 만들면 …"을 보여 주면 AI 없이도 플래너를 보여 줄 수 있습니다. 로직을 JS나 엔진으로 옮겨야 합니다. |
| 스키마 비교의 빠진 것 | 일부 — R59의 "Compare Schema…"는 표·열·인덱스·외래키만 비교하고 마이그레이션 SQL도 그것만 만듦. 없는 것: 뷰·트리거·프로시저·`CHECK`·기본키 변경·열 순서·`ON UPDATE`가 아닌 표 옵션, 마이그레이션의 의존 순서가 사슬을 넘는 경우의 검증 | ★★ | 중 | 뷰와 트리거는 `SHOW CREATE`로 통째 비교하면 됩니다. 쓰임새는 한정적입니다. |
| 앱 화면의 자동 시험 | 없음 — R59는 순수 로직 17개(`code/test/frontend/test_libs.mjs`)를 Node로 시험하고 화면은 디버그 포트(CDP)로 손으로 눌러 확인했는데, 그 스크립트는 저장소에 없음 | ★★★ | 중 | 세션 패널·차트·계획 그림·스키마 비교·ERD 드래그를 CDP 스크립트로 저장소에 넣으면 라운드마다 화면이 고장 났는지 눈 없이 확인할 수 있습니다. |
| 라이트/다크 테마 | 없음 (다크만) | ★★ | 중~상 | 색이 코드에 박혀 있어 변수화가 먼저입니다. |
| SQL 스니펫(자주 쓰는 문장 저장·삽입) | 없음 (쿼리 히스토리·SQL 포맷터·셀 편집은 있음) | ★ | 하~중 | 있으면 편하지만 졸업작품 핵심과는 거리가 있습니다. |

## 3. MCP (지금 32개 도구, resources·prompts는 0개)

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| `suggest_indexes` 후속 | 일부 — R59에서 하위 질의·CTE·`OR` 해석과 `verify` 전·후 모드가 생김. 없는 것: 여러 범위 열·정렬 방향이 섞인 `ORDER BY` 해석, 열 사이의 상관, 커버링 인덱스, 쓰기 문장(`UPDATE`·`DELETE`)의 `verify`, 20만 행 넘는 표 | ★ | 중 | 쓰기 문장의 전·후는 `EXPLAIN UPDATE`가 먼저 필요합니다(위 엔진 표). |
| `kill_session` | 없음 — 엔진의 `KILL [QUERY \| CONNECTION] n`은 R53에서 생김, 도구만 없음 | ★★ | 하 | `execute_sql`로 `KILL`을 부를 수는 있어서 이름 있는 도구로 감싸 확인 단계를 붙이는 정도입니다(`DROP`처럼 확인 없이는 막는 쪽이 안전). |
| `slow_queries` 도구 (느린 쿼리 로그 읽기) | 없음 — R59에서 엔진에 로그가 생겼고 `health_report`는 `Slow_queries`를 말하지 않음 | ★★★ | 하 | "요즘 느린 쿼리가 뭐야?"에 `information_schema.slow_log`를 읽어 문장·걸린 시간·세션과 `suggest_indexes`를 이어 주는 도구입니다. 시연 대본이 하나 늘어납니다. |
| resources(스키마)·prompts(템플릿) | 없음 | ★★ | 하 | 도구 호출 없이 스키마를 읽게 하는 정석 기능이지만 체감은 작습니다. |
| CSV 가져오기·내보내기 도구 | 없음 (UI에는 있음) | ★★ | 하~중 | UI의 `import_csv` 명령과 같은 일을 하는 도구입니다. |
| `diff_schema` | 없음 | ★★ | 중 | 쓰임새가 한정적입니다. |

## 4. 백엔드 (서버·운영)

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| 코퍼스 비교 CI | 없음 (`.github` 없음) | ★★★ | 상 | 지금은 사람이 `make_scoreboard.py`를 돌립니다. MSVC 빌드, MySQL 컨테이너(리눅스 러너)에 리눅스 빌드 가능 여부 확인이 필요합니다. |
| 연결 수 상한 | 없음 — **`max_connections` 변수 자체가 없음**(`@@max_connections`가 `NULL`) | ★★ | 하 | 서버 accept 루프의 작은 변경이고 변수도 같이 열어야 합니다. |
| TLS | 없음 | ★★ | 상 | Windows에서 TLS 라이브러리 도입과 핸드셰이크 확장이 큽니다. |
| `caching_sha2_password` | 없음 (`mysql_native_password`만) | ★★ | 상 | 지금 쓰는 mysql CLI와 pymysql은 모두 접속됩니다. 일부 최신 드라이버가 옵션을 요구할 때만 문제입니다. |
| WAL 복제, XA, 페이지 단위 버퍼 풀 | 없음 | ★ | 상 | 이미 범위 밖으로 정했고 남은 기간에는 맞지 않습니다. |

## 5. 발표·제출 대비

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| MCP 시연 리허설 | 일부 (`DEMO.md` 8번에 `run_script` 미리보기·`suggest_indexes` 대본, Claude Desktop과의 연결은 자동으로 검증하지 못함) | ★★★★★ | 하 | **직접** 해 보셔야 합니다. Claude Desktop에서 한 번 돌려 봐야 합니다. |
| 크래시 복구 장면 리허설 | 일부 — 대본은 `DEMO.md` 3번, 복구 카드는 R51에서 생김. 사람이 눌러 본 적은 없음 | ★★★★ | 하 | **직접** 해 보셔야 합니다. 앱 Server Manager의 Start/Stop → 복구 카드 → ACTIVITY LOG. |
| **발표 PC용 설치본·새 PC 점검** | 없음 — `tauri build`의 bundle 설정에 `engine_server.exe`·MCP 서버를 넣는 `externalBin`·`resources`가 없어서, 만든 설치본은 엔진을 못 찾음(지금은 `code/` 폴더 통째로 복사하는 방식) | ★★★★ | 중 | 발표 PC에서 앱이 안 뜨는 것이 가장 큰 위험입니다. 엔진을 같이 묶고, 깨끗한 PC(또는 새 사용자 계정)에서 설치부터 연결까지 한 번 돌려 봐야 합니다. |
| 시연 실패 대비 녹화·스크린샷 | 없음 | ★★★★ | 하 | **직접** 하셔야 합니다(화면 녹화). 라이브가 안 될 때 바로 틀 수 있습니다. |
| 최적화 전후 속도 애니메이션 | 일부 (Benchmark 패널이 "AutoCommit n배 빠름" 같은 배지를 실제 측정으로 보여 줌, 정적 `speedup_chart.png`) | ★★★ | 하~중 | 이미 있는 데이터를 애니메이션으로 바꾸면 됩니다. |
| RuSQL vs MySQL 분할 화면 | 없음 (Compat 페이지는 코퍼스별 일치 수만 보여 줌) | ★★★ | 중 | 발표장에 MySQL이 떠 있어야 해서 불안정합니다. |

## 5-1. 품질 점검

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| 누적 점검 한 바퀴 (Debug 전체 + 크래시 퍼저 + 빌드 간 차분) | 일부 — R54는 크래시 퍼저 3종(38라운드)과 R53 빌드와의 A/B까지 돌렸지만 Debug는 R50~R59 열 라운드 건너뛰었고, R53의 락 대기·세션 정리 코드는 퍼저가 아직 다루지 않음(`KILL`을 섞은 퍼저가 없음) | ★★★ | 하 (기계 시간 약 1시간) | 마감 전에 Debug 전체와 `KILL`을 섞은 크래시 시험을 한 번은 돌려야 안심입니다. |
| 새 영역 훑기 (진짜 MySQL과 문장 비교) | 일부 — 훑을수록 나옴: R52에서 문장 62개를 새로 훑어 오답 3묶음을 찾음 | ★★★ | 중 | 아직 안 훑은 모양: 트랜잭션·격리 수준 안의 문장, 트리거·프로시저 안의 쓰기, 뷰 갱신, 문자셋·정렬 규칙, 날짜 계산, 외래키 동작. |
| 앱 화면 사람 눈 점검 | 일부 — 화면은 디버그 포트로 DOM을 눌러 확인했을 뿐, 아이콘·폰트·전체 모양은 본 적 없음 | ★★★ | 하 | **직접** 앱을 한 바퀴 눌러 보시고 어색한 곳을 알려 주세요. |

## 6. 보류 — SQL 문법 추가 (2026-10-25 결정: 지금은 기능에 집중)

새 문장·절·함수·자료형을 받아들이는 일은 여기로 뺐습니다. 기능 쪽이 끝나거나 시간이 남으면 다시 꺼냅니다(`KILL`과 `FULLTEXT`처럼 문법이 따라붙지만 중심이 동작인 것은 기능 쪽에 남겼습니다).

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| `INSERT INTO t () VALUES ()`, `INSERT INTO t VALUES ()` | 없음 (둘 다 오류) | ★★★ | 하~중 | 파서와 빈 행 삽입(기본값·AUTO_INCREMENT) 경로입니다. |
| `DEFAULT (식)` | 일부 — `DEFAULT NOW()`, `CURRENT_TIMESTAMP(3)`, 음수·`TRUE` 같은 기본값은 됨, 괄호 식 `DEFAULT (a * 2)`·`DEFAULT (UUID())`은 오류 | ★★★ | 중 | 다른 열을 읽는 기본값 평가가 새로 필요합니다. MySQL 8 덤프에 나옵니다. |
| `WITH ROLLUP` | 없음 | ★★★ | 중 | 소계 행 생성과 `NULL` 표시가 필요합니다. 코퍼스에서 확인되는 차이 15개 중 하나입니다. |
| `SELECT @x := 식` | 없음 (R43에서 일부러 오류로 둠) | ★★★ | 중 | 행마다 평가하는 순서와 세션 변수 상태가 걸립니다. |
| `SIGNAL` · `RESIGNAL` · `GET DIAGNOSTICS` | 없음 (`SIGNAL`이 든 프로시저는 만들 때 "Unknown statement") | ★★★ | 하~중 | 루틴에서 오류를 던지는 기본 수단입니다. |
| 라벨 붙은 루프 (`lbl: LOOP … LEAVE lbl`, `ITERATE lbl`, `END LOOP lbl`) | 없음 — 라벨 없는 `LOOP`·`WHILE`·`REPEAT`·`LEAVE`는 됨 | ★★★ | 중 | 처음 표에는 "END LOOP 라벨만"으로 적었는데 **라벨 문법 전체**가 없습니다(`lbl:`에서 "Unknown statement"). 중첩 루프를 빠져나오려면 필요합니다. |
| 커서(`DECLARE c CURSOR`, `OPEN`·`FETCH`·`CLOSE`) · `DECLARE … HANDLER` | 없음 | ★★★ | 상 | 저장 루틴 해석기에 상태가 새로 필요합니다. |
| `CREATE TEMPORARY TABLE` | 없음 | ★★★ | 중~상 | 세션별 이름 공간과 연결 종료 시 정리가 필요합니다. |
| 생성 열 `GENERATED ALWAYS AS` / `AS (식)` | 없음 (STORED·VIRTUAL 둘 다 오류) | ★★★ | 상 | 읽기 경로, 인덱스, ALTER까지 걸립니다. |
| 여러 열 FK | 없음 (오류로 거절: "one column per foreign key"). 한 열에 FK 둘 이상은 R57에서 됨 | ★★ | 중~상 | 한 열에 여러 키는 이제 키 목록이라 되지만, 키 하나가 여러 열을 가리키는 구조(키 = 열 목록, 복합 값의 존재·연쇄 검사)는 아직입니다. ERD 편집기는 이미 FK가 있는 열을 경고합니다. |
| `NATURAL LEFT JOIN` · `NATURAL RIGHT JOIN` | 없음 (문법 오류: `NATURAL` 뒤에 `JOIN`만 기대) | ★★ | 하 | R57에서 USING·NATURAL 조인의 열 순서를 맞추며 알았습니다. 파서에 `LEFT`·`RIGHT`를 받고 `RIGHT`면 USING처럼 순서를 바꾸면 됩니다. |
| `UPDATE … JOIN (SELECT …) d ON …`, `DELETE x FROM t x JOIN (SELECT …) d ON …` | 없음 ("Expected identifier": 갱신 조인에 파생 표가 올 수 없음) | ★★★ | 중 | 집계한 값으로 갱신하는 흔한 MySQL 문장입니다. 갱신 조인 실행기가 파생 표를 실체화해야 합니다. |
| `UPDATE t u1 JOIN t ON …` (첫 사용에만 별칭, 두 번째는 별칭 없음), `ORDER BY (SELECT …)` 식으로 직접 | 없음 ("Not unique table/alias", "A subquery inside this expression is not supported") | ★★ | 하~중 | 둘 다 MySQL은 받아들입니다. 조용히 틀리는 것이 아니라 오류로 거절합니다. |
| `JSON_ARRAYAGG` · `JSON_OBJECTAGG` | 없음 (`JSON_AGG`는 있음) | ★★ | 하 | 별칭입니다. `JSON_OBJECTAGG`는 하~중. |
| `UPDATE IGNORE` · `DELETE IGNORE` | 없음 (`INSERT IGNORE`는 있음) | ★★ | 중 | 오류 난 행만 건너뛰는 의미가 새로 필요합니다. |
| `ALTER … ADD/DROP PRIMARY KEY` | 없음 (명시적으로 거절: "declare the PRIMARY KEY in CREATE TABLE") | ★★ | 중 | 인덱스·참조 재구성이 걸립니다. |
| `DECLARE … DEFAULT 식` | 없음 (`DEFAULT a * 3`이 든 프로시저는 만들 때 오류) | ★★ | 하 | 파서 수준입니다. |
| `EXPLAIN FORMAT=JSON` · `FORMAT=TREE` | 없음 ("Unknown statement: FORMAT") | ★★ | 하~중 | 처음 표에 없던 항목입니다. MySQL Workbench 같은 도구가 `FORMAT=JSON`을 보냅니다. |
| `USE` · `FORCE` · `IGNORE INDEX` | 없음 (문법 오류). 힌트 주석 `/*+ … */`은 `MAX_EXECUTION_TIME` 말고는 읽고 무시함 | ★★ | 중 | 플래너에 힌트를 전달해야 합니다. |
| `JSON_TABLE` | 없음 | ★★ | 상 | 쓰임새에 비해 큽니다. |
| `LOAD DATA` / `INTO OUTFILE` | 없음 (UI의 CSV 가져오기·내보내기는 있음) | ★★ | 중 | 서버 파일 접근 보안 문제가 있고 UI가 이미 해결합니다. |
| 없는 함수 묶음 | 일부 — 흔한 MySQL 함수 이름 254개를 불러 보니 **45개가 없음**: JSON 13(`JSON_MERGE_PATCH`, `JSON_QUOTE`, `JSON_SEARCH`, `JSON_PRETTY` …), `INET_ATON`·`INET_NTOA`·`INET6_*`·`IS_IPV4/6` 6, `UUID_TO_BIN`·`BIN_TO_UUID`·`UUID_SHORT`·`IS_UUID` 4, `AES_*`·`COMPRESS`·`RANDOM_BYTES` 7, `GET_LOCK` 계열 3, `CONNECTION_ID`·`CHARSET`·`COLLATION`·`COERCIBILITY` 4, `CONVERT_TZ`, `BIT_XOR`, `BENCHMARK`, `LOAD_FILE`, `ST_*` | ★★ | 하 | 하나씩은 싸지만 `CONNECTION_ID`·`CHARSET`·`COLLATION`은 도구가 접속 직후 부르는 경우가 있어 먼저 하면 좋습니다. |
| `CREATE EVENT` | 없음 | ★ | 중~상 | 스케줄러가 필요한데 쓰임새가 작습니다. |
| `PERCENTILE_CONT` · `PERCENTILE_DISC` `… WITHIN GROUP (ORDER BY …)` | 일부 — `MEDIAN`만 | ★★ | 하~중 | PostgreSQL·Oracle에 있습니다(MySQL에는 없음). |
| 뷰 `WITH CHECK OPTION` | 없음 (문법 오류) | ★★ | 하~중 | MySQL·PostgreSQL·Oracle 모두 있습니다. 갱신 가능한 뷰와 짝입니다. |
| `CREATE SEQUENCE` · `NEXT VALUE FOR` | 없음 (`AUTO_INCREMENT`만) | ★★ | 중 | PostgreSQL·Oracle에 있습니다. |
| 온라인 DDL 문법 (`ALTER … ALGORITHM=INPLACE, LOCK=NONE`) | 없음 (문법 오류) | ★ | 하(주의) | 셋 다 있습니다. 받기만 하고 락을 그대로 쓰면 조용한 차이가 되므로 신중히. |
| `GROUPS` 윈도 프레임 | 없음 | ★ | 중 | PostgreSQL에만 있습니다. |
| `CREATE TYPE`, `DEFERRABLE` 제약, 부분 인덱스, 행 레벨 보안(`CREATE POLICY`) | 없음 | ★ | 상 | PostgreSQL·Oracle에 있고 범위가 큽니다. |

## 점검에서 바로잡은 것 (처음 표와 달라진 곳)

- **더 나쁨:** (`DEMO.md`의 낡은 목록은 R48에서 고침) `RANGE … n PRECEDING`은 "일부"가 아니라 **조용한 오답**이고, `DATE(x) + 0`도 조용한 오답입니다. 라벨 붙은 루프는 "END LOOP 라벨"만이 아니라 **라벨 문법 전체**가 없습니다. 연결 수 상한은 "무시"가 아니라 **변수 자체가 없습니다.** `DEMO.md`의 "피할 것" 목록이 낡았습니다.
- **더 좋음:** `UNIQUE/FK 검사 O(n)`은 INSERT 쪽이 이미 O(1)입니다(부모 DELETE만 남음). `DEFAULT (식)`은 `NOW()`류가 이미 됩니다. 크래시 복구 시험은 Server Manager의 Stop으로 이미 가능합니다. `list_locks`·`get_table_stats`·`trace_query`는 `execute_sql`로 이미 부를 수 있습니다.
- **R51에서 알게 된 것:** 앱에서 `EXPLAIN`·`EXPLAIN ANALYZE`의 결과는 한 열짜리 표로 도착해서, 코드에 있던 "계획 트리" 화면이 **한 번도 뜨지 않고 있었습니다**(R51에서 표를 계획 화면으로 그리게 고쳤고 단계 막대도 그 화면에 붙였습니다).
- **R56에서 알게 된 것:** `a ← b ← c` 연쇄(DELETE와 키 UPDATE 모두)와 0행 SELECT의 열 이름을 끝냈고(`DATE.md` R56), 그 과정에서 `vs_mysql.py`가 빈 결과의 열 이름도 비교하게 되어 `SHOW TABLES LIKE`의 열 이름과 `EXECUTE`한 SELECT의 빈 결과 문제도 찾아 고쳤습니다.
- **R57에서 알게 된 것:** 점검의 "조용한 오답" 여섯 개를 끝냈고(`DATE.md` R57), 그 비교에서 **`UNION`의 왼쪽이 0행이면 오른쪽 값이 사라지는 오답**(R53 빌드에도 있었음)과 **`a RIGHT JOIN b USING (c)`의 `*` 열 순서**가 MySQL과 달랐던 것을 함께 고쳤습니다. 10-26의 "다중 표 DELETE·MERGE·REPLACE의 사슬 아래 검사"는 DELETE·`DELETE … JOIN`·MERGE-DELETE가 이미 맞았고(한 문장으로 모아 도는 DELETE가 R56의 모의를 씀) REPLACE와 UPDATE+DELETE가 든 MERGE만 어긋나 있었습니다. 같은 점검의 "OPTIMIZE·CHECK TABLE 없음"은 MySQL 프로토콜 쪽에서는 이미 됩니다(위 표).
- **R58에서 알게 된 것:** 점검의 일곱 항목을 끝냈고(`DATE.md` R58), 그 비교로 목록에 없던 조용한 오답 둘을 더 찾아 고쳤습니다: **`RENAME TABLE` 뒤에 `information_schema`가 옛 표 이름을 말하는 것**(카탈로그의 표 이름이 갱신되지 않았음)과 **부모 표의 이름을 바꾸면 자식의 외래키가 옛 이름을 가리킨 채 남는 것**(INSERT가 "Referenced table not found"로 실패). `USING (y, x)`처럼 열을 거꾸로 적으면 합쳐진 열이 적은 순서로 나오던 것(MySQL은 왼쪽 표의 순서), `DROP TABLE 없는표`의 오류 번호(1051, 한 표일 때만 1146이던 것)도 함께 맞췄습니다. 새 코퍼스는 모두 진짜 MySQL과 차이 0입니다.
- **R59에서 알게 된 것:** 아홉 항목을 끝냈고(`DATE.md` R59), 화면을 실제로 눌러 보다 세 가지를 더 찾았습니다. ① **집계만 있는 한 행짜리 답의 `FOUND_ROWS()`가 0**(R37부터; 표 끝의 "1 row(s) returned." 줄이 없어서 세던 방식이 놓침 — 고침, 진짜 MySQL과 비교). ② 앱의 연결은 `SHOW CREATE TABLE`을 행이 아니라 글자로 돌려줘서 스키마 비교가 한쪽에만 있는 표의 `CREATE TABLE`을 못 읽던 것(고침). ③ 교차 조인이 메모리 6 GB까지 오르고 `KILL`도 안 듣는 것(위 표, 고치지 않음). 진짜 MySQL과 비교한 `long_query_time`은 범위 밖의 수를 오류가 아니라 0 ~ 31536000으로 맞추는데 처음에는 오류로 거절했습니다(고침). 그리고 전체 시험의 `idx0` 한 번에서 외래키 경합 시험이 한 번 실패했습니다(위 표 첫 줄, 원인 미확인).
- **새 항목:** `EXPLAIN FORMAT=JSON/TREE`, `SIGNAL` 묶음(`RESIGNAL`·`GET DIAGNOSTICS`), `UPDATE/DELETE IGNORE`의 `DELETE`, 없는 함수 45개의 구체 목록, `VERSION()` 문자열.

## 추천 순서 (기능만, 마감까지 약 4주)

1. **발표 PC에서 앱이 뜨게 만들기 (중):** 설치본에 엔진 묶기와 새 PC 점검(위 "발표 PC용 설치본"). 고장 나면 나머지가 소용없는 항목입니다.
2. **외래키 경합의 원인 찾기 (중~상):** 드물게 부모 없는 자식 행이 남는 실패(위 "외래키 값 클레임의 드문 경합"). 데이터 무결성이 걸려 있고 마감 전에 한 번은 가려야 합니다.
3. **보이는 장면 다듬기 (하~중):** `slow_queries` 도구와 시연 대본(느린 쿼리 로그 → `suggest_indexes` 전·후 → `run_script`로 만들기), 앱 화면의 자동 시험(CDP 스크립트를 저장소에)을 먼저. 세션 패널·계획 그림·차트·스키마 비교·ERD 드래그는 R59에서 끝났습니다.
4. **무결성과 운영:** 교차 조인의 메모리(6 GB)와 `KILL`, `EXPLAIN UPDATE/DELETE`, 연결 수 상한, 프로토콜 오류 번호의 남은 약 20개, 몇 가지 `SHOW`, MCP의 `kill_session`·resources·prompts·CSV 도구.
5. **마감 1주 전:** 누적 점검 한 바퀴, 새 영역 훑기 한 번 더.
6. **시간이 되면:** 앱 안 인덱스 제안, 최적화 전후 속도 애니메이션, 코퍼스 비교 CI, RuSQL vs MySQL 분할 화면, 스키마 비교에 뷰·트리거.
7. **미루거나 하지 않는 것:** `FULLTEXT`, 표현식 인덱스, 열 단위 권한, 감사 로그, TLS, `caching_sha2_password`, WAL 복제·XA, 테마.
8. **직접 하실 일:** MCP 시연 리허설(`DEMO.md` 8번), 크래시 복구 장면 리허설(`DEMO.md` 3번), 화면 녹화, 앱 화면 점검. **결정할 것:** `@@version`을 8.x로 알릴지.
