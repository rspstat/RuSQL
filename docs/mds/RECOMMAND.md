# RuSQL 추천 목록 (남은 항목만, R54 이후 · 2026-10-26 · 항목마다 엔진·코드로 다시 확인한 판)

완료된 항목은 뺐습니다(R44~R54에서 끝낸 것은 `DATE.md`에 있습니다). 이 판은 표의 항목을 엔진에 실제로 돌려 보고(진짜 MySQL 8.4.9와 비교) 앱·MCP 코드를 찾아서, 이미 있거나 일부만 있는 것을 가려낸 결과입니다. 점검에서 바뀐 것은 맨 아래 "점검에서 바로잡은 것"에 모았습니다. 지금 받아들이는 SQL 문법 전체는 `SQL.md`에 있습니다.

- **현재 상태:** `일부`(일부만 있음) · `없음` · `오답`(지금 조용히 틀린 값을 돌려줌)
- **추천도:** ★ 다섯 개. 기준은 실사용 가치 + 발표 임팩트 + 비용 대비 효과입니다. 졸업작품 마감(11월 말)까지 약 5주를 전제로 합니다.
- **범위 (2026-10-25):** SQL 문법 추가는 맨 아래 "6. 보류"로 뺐고, 지금은 기능(엔진 동작·도구·화면·발표)에 집중합니다.
- **비용:** 제가 구현하고 검증(코퍼스 비교, 심은 버그, 전체 시험, 문서)까지 마치는 데 걸리는 시간입니다. 하 ≈ 2~4시간, 중 ≈ 하루, 상 ≈ 2~4일. 빌드와 전체 시험 한 바퀴가 약 35분이라 헤더를 건드리는 변경은 여러 바퀴가 듭니다. **프론트(UI)는 자동 시험이 없어서 눈으로 확인하는 시간이 따로 붙습니다.**

## 1. 엔진 기능

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| **자기 자신을 가리키는 외래키의 부모 쪽** (`emp.mgr → emp.id`) | 오답·무결성 — `DELETE FROM emp WHERE id = 1`이 `id = 2`가 가리키는 행을 지워도 오류 없이 성공(MySQL은 1451). `ON DELETE CASCADE`·`SET NULL`도 적용되지 않아 매달린 행이 남음. 부모 문장이 "다른 표"만 보기 때문(R54에서 `USE`·부모 DELETE를 고치다 발견) | ★★★★ | 중 | 자기 표를 자식으로 세되, 같은 문장이 지우는 행은 가리키는 행으로 세지 않기(InnoDB는 행 순서에 따라 오류를 내는 것과 다름), CASCADE는 되풀이해서 따라가야 합니다. 부모 키 UPDATE도 같습니다. 코퍼스로 MySQL과 비교할 수 있습니다. |
| 읽기만 하는 문장 중 redo 로그를 비우는 것 | 발견(R54) — `USE`와 세션 변수만 바꾸는 `SET`은 고쳤지만 `WITH`(CTE) SELECT, `PREPARE`·`EXECUTE`·`DEALLOCATE`, `LOCK`·`UNLOCK TABLES`, `ANALYZE TABLE`, 하위 질의가 든 `SET`, `CALL`은 여전히 구조 쓰기 락(실행 중인 모든 문장을 기다림)을 잡고 쌓인 redo 로그를 체크포인트(테이블 파일 재작성)함. 특히 CTE 질의가 쓰기 사이에 끼면 redo의 이점이 사라짐 | ★★★ | 하~중 | CTE는 임시 표를 `s.tables`에 만들어서 쓰기 경로였던 것이라 임시 표를 문장 안에서만 쓰게 하거나(락 없이) 체크포인트 없이 쓰기 락만 잡게 해야 합니다. 나머지는 `USE`와 같은 한 줄 분류입니다. |
| 다중 표 DELETE·MERGE의 부모 검사, 참조되는 부모의 CASCADE | 일부 — R54에서 DELETE·UPDATE의 "참조되지 않은 부모"는 O(1)로 만들었지만 `DELETE t1 FROM t1 JOIN …`·`MERGE`의 RESTRICT 검사(`delete_restrict_violation`)와, 참조되는 부모의 `CASCADE`/`SET NULL`이 자식을 **찾는** 일은 자식 표를 훑음 | ★★ | 중 | 앞의 것은 같은 캐시를 부르면 되지만 호출하는 쪽이 자식 표의 데이터 락을 어떻게 잡는지 확인이 필요하고, 뒤의 것은 값 → 행 위치를 담는 구조가 새로 필요합니다. |
| 문장 중단의 남은 것 | 일부 — `KILL QUERY`는 SELECT의 행 루프와 행 잠금 대기를 멈춤. 실행 중인 쓰기 문장은 끝까지 돌고(반쯤 쓰다 멈추면 되돌릴 길이 없어서 일부러), 정렬·집계·조인 해시 구성 같은 한 덩어리 단계는 끝나야 멈춤 | ★★ | 중 | 쓰기 문장의 중단은 문장 단위 롤백이 있어야 안전하고, 덩어리 단계는 확인점(`tick`)을 더 넣어야 합니다. |
| 느린 쿼리 로그 | 없음 (`SET GLOBAL slow_query_log`는 받기만 하고 아무 일도 안 함. 앱의 쿼리 히스토리에는 소요 시간이 있음) | ★★★ | 중 | 임계값을 넘은 문장을 기록하고 `SHOW`·`information_schema` 표로 보여 주면 됩니다. `TRACE`의 단계 기록을 쓸 수 있습니다. 감사 로그의 바탕이 되기도 합니다. |
| `EXPLAIN UPDATE` · `EXPLAIN DELETE` | 없음 ("EXPLAIN: Update → not a SELECT") | ★★ | 하~중 | MySQL은 쓰기 문장의 계획도 보여 줍니다. 쓰기 문장이 인덱스를 타는지 앱에서 확인할 수 있게 됩니다. |
| `OPTIMIZE TABLE` · `CHECK TABLE` · `FLUSH` | 없음 ("Unknown statement"). `VACUUM`·체크포인트는 이미 있음 | ★★ | 하~중 | `OPTIMIZE`는 `VACUUM`+체크포인트의 이름 바꿈, `CHECK TABLE`은 인덱스와 행이 맞는지 검사하는 새 기능입니다. |
| `FULLTEXT` / `MATCH … AGAINST` | 없음 (`FULLTEXT KEY`가 든 CREATE TABLE이 오류) | ★★★ | 상 | 검색 데모는 강하지만 역색인과 점수 계산이 큽니다. |
| 표현식(함수) 인덱스 | 없음 (`CREATE INDEX … ((a + 1))`가 문법 오류) | ★★ | 중~상 | MySQL·PostgreSQL·Oracle 모두 있습니다. 인덱스 키가 식이라 플래너와 유지가 바뀝니다. |
| 열 단위 권한 (`GRANT SELECT (col) ON t`) | 없음 | ★★ | 중 | MySQL·PostgreSQL에 있고 Oracle은 INSERT·UPDATE만입니다. |
| 감사 로그 | 없음 | ★★ | 중 | 셋 다 있습니다(PostgreSQL은 확장). 누가 무엇을 했는지 기록합니다. |
| 공간 타입·인덱스, 특수 인덱스(비트맵·BRIN·GIN/GiST) | 없음 | ★ | 상 | 범위가 크고 이번 마감에는 맞지 않습니다. |

## 2. 프론트 (앱)

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| 세션 목록에서 `KILL` 버튼, 차단자·오래 도는 문장 강조 | 없음 — 엔진에 `KILL`은 R53에서, 차단자를 가리키는 `list_locks`·`health_report`(MCP)는 R54에서 생겼고 Server Manager의 세션 표(`SHOW PROCESSLIST`)에는 버튼도 강조도 없음 | ★★★ | 하~중 | 행마다 "문장 중단 / 연결 끊기"를 붙이면 락 대기 시연(두 세션이 한 행을 기다림) 끝에 풀어 주는 장면이 됩니다. 확인창이 필요합니다. |
| 앱 안의 "인덱스 제안" | 없음 (`suggest_indexes`는 MCP 도구뿐이고 Python 로직) | ★★★ | 중~상 | 에디터의 문장에 대해 EXPLAIN 막대 옆에 "이 인덱스를 만들면 …"을 보여 주면 AI 없이도 플래너를 보여 줄 수 있습니다. 로직을 JS나 엔진으로 옮겨야 합니다. |
| 실행 계획을 노드 그림으로 (스캔 → 조인 → 정렬) | 일부 — 계획은 "이름: 값" 행과 단계 막대(R51)로 보임 | ★★ | 중 | 발표용으로 보기 좋지만 핵심은 아닙니다. |
| 결과 JSON 내보내기 | 없음 (CSV 내보내기는 있음: 결과 표의 "Export as CSV") | ★★ | 하 | CSV 버튼과 같은 모양으로 추가합니다. |
| 결과 간단 차트 | 없음 | ★★ | 중 | 보기에는 좋지만 핵심은 아닙니다. |
| 스키마 diff·마이그레이션 생성 | 없음 | ★★ | 중 | 쓰임새가 한정적입니다. |
| ERD 드래그 연결 | 없음 (관계선 클릭·`＋ Relationship`으로 편집은 됨) | ★★ | 중 | 편집기가 이미 있어 편의 기능입니다. |
| 라이트/다크 테마 | 없음 (다크만) | ★★ | 중~상 | 색이 코드에 박혀 있어 변수화가 먼저입니다. |
| SQL 스니펫(자주 쓰는 문장 저장·삽입) | 없음 (쿼리 히스토리·SQL 포맷터·셀 편집은 있음) | ★ | 하~중 | 있으면 편하지만 졸업작품 핵심과는 거리가 있습니다. |

## 3. MCP (지금 32개 도구, resources·prompts는 0개)

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| `suggest_indexes` 후속 | 일부 — R51에서 만듦(정규식으로 문장을 읽음). 없는 것: 하위 질의·CTE·`OR`·여러 범위 열·정렬 방향이 섞인 `ORDER BY` 해석, 열 사이의 상관, 커버링 인덱스 | ★★ | 중 | 만든 뒤 효과를 같은 도구가 `EXPLAIN`으로 다시 재서 보여 주는 "전·후" 모드가 발표에 쓸모 있습니다. 만들기는 `run_script` 미리보기를 거치게 합니다. |
| `kill_session` | 없음 — 엔진의 `KILL [QUERY \| CONNECTION] n`은 R53에서 생김, 도구만 없음 | ★★ | 하 | `execute_sql`로 `KILL`을 부를 수는 있어서 이름 있는 도구로 감싸 확인 단계를 붙이는 정도입니다(`DROP`처럼 확인 없이는 막는 쪽이 안전). |
| resources(스키마)·prompts(템플릿) | 없음 | ★★ | 하 | 도구 호출 없이 스키마를 읽게 하는 정석 기능이지만 체감은 작습니다. |
| CSV 가져오기·내보내기 도구 | 없음 (UI에는 있음) | ★★ | 하~중 | UI의 `import_csv` 명령과 같은 일을 하는 도구입니다. |
| `diff_schema` | 없음 | ★★ | 중 | 쓰임새가 한정적입니다. |

## 4. 백엔드 (서버·운영)

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| 코퍼스 비교 CI | 없음 (`.github` 없음) | ★★★ | 상 | 지금은 사람이 `make_scoreboard.py`를 돌립니다. MSVC 빌드, MySQL 컨테이너(리눅스 러너)에 리눅스 빌드 가능 여부 확인이 필요합니다. |
| MySQL 프로토콜의 오류 번호 | 없음 — 모든 오류가 번호 1064(ER_PARSE_ERROR)로 나가고 진짜 번호는 메시지 글자 안(`ERROR 1317 (70100): …`)에만 있음 | ★★ | 하 | 드라이버는 번호로 재시도·중단 여부를 정합니다(1205·1213 재시도, 1317 `QueryInterruptedException`). 메시지 앞의 `ERROR nnnn (state):`를 읽어 패킷에 넣으면 됩니다. 번호를 비교하는 기존 시험 확인이 필요합니다. |
| 연결 수 상한 | 없음 — **`max_connections` 변수 자체가 없음**(`@@max_connections`가 `NULL`) | ★★ | 하 | 서버 accept 루프의 작은 변경이고 변수도 같이 열어야 합니다. |
| FLOAT 열의 일곱 자리 이상 값 | 오답 — `MIN`/`MAX`/`GREATEST`/`COALESCE`가 저장된 글자를 보여 주고(MySQL은 여섯 자리), `UPDATE … SET d = f`·`INSERT … SELECT`(FLOAT → DOUBLE 열)는 짧은 글자를 복사 | ★★ | 하~중 | 조용히 틀리지만 일곱 자리 넘는 FLOAT를 쓸 때만 보입니다. |
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
| 누적 점검 한 바퀴 (Debug 전체 + 크래시 퍼저 + 빌드 간 차분) | 일부 — R54는 크래시 퍼저 3종(38라운드)과 R53 빌드와의 A/B까지 돌렸지만 Debug는 R50~R54 다섯 라운드 건너뛰었고, R53의 락 대기·세션 정리 코드는 퍼저가 아직 다루지 않음(`KILL`을 섞은 퍼저가 없음) | ★★★ | 하 (기계 시간 약 1시간) | 마감 전에 Debug 전체와 `KILL`을 섞은 크래시 시험을 한 번은 돌려야 안심입니다. |
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
| 한 열에 FK 둘 이상, 여러 열 FK | 없음 (같은 열에 더하면 앞의 것을 덮어씀, 여러 열 FK는 오류) | ★★ | 중~상 | 열마다 FK 하나라는 저장 구조를 바꿔야 합니다. ERD 편집기는 이미 FK가 있는 열을 경고합니다. |
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
- **새 항목:** `EXPLAIN FORMAT=JSON/TREE`, `SIGNAL` 묶음(`RESIGNAL`·`GET DIAGNOSTICS`), `UPDATE/DELETE IGNORE`의 `DELETE`, 없는 함수 45개의 구체 목록, `VERSION()` 문자열.

## 추천 순서 (기능만, 마감까지 약 4주)

1. **발표 PC에서 앱이 뜨게 만들기 (중):** 설치본에 엔진 묶기와 새 PC 점검(위 "발표 PC용 설치본"). 고장 나면 나머지가 소용없는 항목입니다.
2. **보이는 장면 다듬기 (하~중):** Server Manager 세션 표에 `KILL` 버튼과 차단자·오래 도는 문장 강조(재료는 R53의 `KILL`, R54의 Metrics 패널·`list_locks`). 크래시 복구 카드와 EXPLAIN 막대, Metrics 패널 다음으로 발표에 보이는 것입니다.
3. **MCP 남은 것 (각 하):** `kill_session`, `suggest_indexes` 전·후 모드, resources·prompts, CSV 도구.
4. **무결성과 운영:** 자기 자신을 가리키는 외래키의 부모 쪽(오답·무결성, 가장 먼저), CTE 등 읽기 문장이 redo 로그를 비우는 문제, 느린 쿼리 로그, `EXPLAIN UPDATE/DELETE`, 연결 수 상한, MySQL 프로토콜 오류 번호.
5. **마감 1주 전:** 누적 점검 한 바퀴, 새 영역 훑기 한 번 더.
6. **시간이 되면:** 앱 안 인덱스 제안, 최적화 전후 속도 애니메이션, 결과 JSON 내보내기·차트·ERD 드래그 연결, 코퍼스 비교 CI, RuSQL vs MySQL 분할 화면.
7. **미루거나 하지 않는 것:** `FULLTEXT`, 표현식 인덱스, 열 단위 권한, 감사 로그, TLS, `caching_sha2_password`, WAL 복제·XA, 테마.
8. **직접 하실 일:** MCP 시연 리허설(`DEMO.md` 8번), 크래시 복구 장면 리허설(`DEMO.md` 3번), 화면 녹화, 앱 화면 점검. **결정할 것:** `@@version`을 8.x로 알릴지.
