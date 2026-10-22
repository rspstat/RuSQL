# RuSQL 추천 목록 (남은 항목만, R47 이후 · 2026-10-22 · 항목마다 엔진·코드로 다시 확인한 판)

완료된 항목은 뺐습니다(R44~R47에서 끝낸 것은 `DATE.md`에 있습니다). 이 판은 표의 항목을 엔진에 실제로 돌려 보고(진짜 MySQL 8.4.9와 비교) 앱·MCP 코드를 찾아서, 이미 있거나 일부만 있는 것을 가려낸 결과입니다. 점검에서 바뀐 것은 맨 아래 "점검에서 바로잡은 것"에 모았습니다.

- **현재 상태:** `일부`(일부만 있음) · `없음` · `오답`(지금 조용히 틀린 값을 돌려줌)
- **추천도:** ★ 다섯 개. 기준은 실사용 가치 + 발표 임팩트 + 비용 대비 효과입니다. 졸업작품 마감(11월 말)까지 약 5주를 전제로 합니다.
- **비용:** 제가 구현하고 검증(코퍼스 비교, 심은 버그, 전체 시험, 문서)까지 마치는 데 걸리는 시간입니다. 하 ≈ 2~4시간, 중 ≈ 하루, 상 ≈ 2~4일. 빌드와 전체 시험 한 바퀴가 약 35분이라 헤더를 건드리는 변경은 여러 바퀴가 듭니다. **프론트(UI)는 자동 시험이 없어서 눈으로 확인하는 시간이 따로 붙습니다.**

## 1. 문법 추가 (지금은 오류가 나는 것)

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| `RANGE BETWEEN n PRECEDING AND …` (값 기준 프레임 오프셋) | **오답** — 오프셋을 행 수로 읽음. `SUM(v) OVER (ORDER BY v RANGE BETWEEN 10 PRECEDING AND CURRENT ROW)`에서 v = 10, 15, 30이면 MySQL은 10·25·30, 여기는 10·25·55 | ★★★ | 하~중 | 오류도 없이 틀립니다. 숫자·날짜 ORDER BY 키의 값 범위로 프레임을 잡아야 합니다. |
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
| `JSON_ARRAYAGG` · `JSON_OBJECTAGG` | 없음 (`JSON_AGG`는 있음) | ★★ | 하 | 별칭입니다. `JSON_OBJECTAGG`는 하~중. |
| `UPDATE IGNORE` · `DELETE IGNORE` | 없음 (`INSERT IGNORE`는 있음) | ★★ | 중 | 오류 난 행만 건너뛰는 의미가 새로 필요합니다. |
| `ALTER … ADD/DROP PRIMARY KEY` | 없음 (명시적으로 거절: "declare the PRIMARY KEY in CREATE TABLE") | ★★ | 중 | 인덱스·참조 재구성이 걸립니다. |
| `DECLARE … DEFAULT 식` | 없음 (`DEFAULT a * 3`이 든 프로시저는 만들 때 오류) | ★★ | 하 | 파서 수준입니다. |

## 2. 기능 추가

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| `KILL [QUERY] n` | 없음 ("Unknown statement: KILL") | ★★★ | 하~중 | R46에서 취소 확인점(루프의 시계 확인)이 생겨 비용이 줄었습니다. 세션마다 "중단" 표시를 두고 같은 확인점에서 읽으면 됩니다. `SHOW PROCESSLIST`·`information_schema.processlist`는 이미 있어 누구를 죽일지는 볼 수 있습니다. |
| 느린 쿼리 로그 | 없음 (`SET GLOBAL slow_query_log`·`long_query_time`은 받기만 하고 아무 일도 안 함. 앱의 쿼리 히스토리에는 소요 시간이 있음) | ★★★ | 중 | 임계값을 넘은 문장을 기록하고 `SHOW`로 보여 주면 됩니다. `TRACE`의 단계 기록을 쓸 수 있습니다. |
| `EXPLAIN ANALYZE` 더 확장 | 일부 (단계별 행 수·시간·비율, `TRACE`는 커밋 단계까지) | ★★★ | 중~상 | 남은 것은 조인 안의 알고리즘별, 하위 질의별 시간입니다. |
| `EXPLAIN FORMAT=JSON` · `FORMAT=TREE` | 없음 ("Unknown statement: FORMAT") | ★★ | 하~중 | 처음 표에 없던 항목입니다. MySQL Workbench 같은 도구가 `FORMAT=JSON`을 보냅니다. |
| `FULLTEXT` / `MATCH … AGAINST` | 없음 (`FULLTEXT KEY`가 든 CREATE TABLE이 오류) | ★★★ | 상 | 검색 데모는 강하지만 역색인과 점수 계산이 큽니다. |
| `USE` · `FORCE` · `IGNORE INDEX` | 없음 (문법 오류). 힌트 주석 `/*+ … */`은 `MAX_EXECUTION_TIME` 말고는 읽고 무시함 | ★★ | 중 | 플래너에 힌트를 전달해야 합니다. |
| `JSON_TABLE` | 없음 | ★★ | 상 | 쓰임새에 비해 큽니다. |
| `LOAD DATA` / `INTO OUTFILE` | 없음 (UI의 CSV 가져오기·내보내기는 있음) | ★★ | 중 | 서버 파일 접근 보안 문제가 있고 UI가 이미 해결합니다. |
| 없는 함수 묶음 | 일부 — 흔한 MySQL 함수 이름 254개를 불러 보니 **45개가 없음**: JSON 13(`JSON_MERGE_PATCH`, `JSON_QUOTE`, `JSON_SEARCH`, `JSON_PRETTY` …), `INET_ATON`·`INET_NTOA`·`INET6_*`·`IS_IPV4/6` 6, `UUID_TO_BIN`·`BIN_TO_UUID`·`UUID_SHORT`·`IS_UUID` 4, `AES_*`·`COMPRESS`·`RANDOM_BYTES` 7, `GET_LOCK` 계열 3, `CONNECTION_ID`·`CHARSET`·`COLLATION`·`COERCIBILITY` 4, `CONVERT_TZ`, `BIT_XOR`, `BENCHMARK`, `LOAD_FILE`, `ST_*` | ★★ | 하 | 하나씩은 싸지만 `CONNECTION_ID`·`CHARSET`·`COLLATION`은 도구가 접속 직후 부르는 경우가 있어 먼저 하면 좋습니다. |
| `CREATE EVENT` | 없음 | ★ | 중~상 | 스케줄러가 필요한데 쓰임새가 작습니다. |

## 3. 프론트

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| EXPLAIN 결과 패널의 단계 막대(워터폴) | 일부 (텍스트 트리 행 목록. 단계별 시간 막대는 Journey 창에만 있음) | ★★★ | 하 | Journey의 막대 그림을 결과 패널에도 쓰면 됩니다. |
| 결과 JSON 내보내기 | 없음 (CSV 내보내기는 있음: 결과 표의 "Export as CSV") | ★★ | 하 | CSV 버튼과 같은 모양으로 추가합니다. |
| 결과 간단 차트 | 없음 | ★★ | 중 | 보기에는 좋지만 핵심은 아닙니다. |
| 스키마 diff·마이그레이션 생성 | 없음 | ★★ | 중 | 쓰임새가 한정적입니다. |
| ERD 드래그 연결 | 없음 (관계선 클릭·`＋ Relationship`으로 편집은 됨) | ★★ | 중 | 편집기가 이미 있어 편의 기능입니다. |
| 라이트/다크 테마 | 없음 (다크만) | ★★ | 중~상 | 색이 코드에 박혀 있어 변수화가 먼저입니다. |

## 4. MCP (지금 26개 도구, resources·prompts는 0개)

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| `suggest_indexes` | 없음 (`explain_query`는 있음) | ★★★★ | 중 | `EXPLAIN ANALYZE`(단계별) + `ANALYZE TABLE` + `SHOW INDEX`를 조합합니다. AI와 플래너를 함께 보여 주는 발표 소재입니다. |
| `list_locks` | 일부 — 전용 도구는 없지만 `execute_sql`로 `SHOW LOCKS`, `SELECT * FROM information_schema.innodb_locks`(`innodb_trx`·`innodb_lock_waits`·`innodb_deadlocks` 포함)가 이미 돌아감 | ★★★ | 하 | 이름 있는 도구로 감싸 AI가 찾기 쉽게 하는 정도입니다. |
| `get_table_stats` | 일부 — `ANALYZE TABLE t`가 열별 distinct·null 수·최솟값·최댓값·사분위·최빈값 표를 이미 돌려줌 | ★★★ | 하 | JSON으로 정리해 주는 래퍼입니다. |
| FK 관계 그래프 도구 | 일부 (`get_table_schema`, `key_column_usage`, `referential_constraints`) | ★★★ | 하 | AI가 조인 경로를 정확히 짜게 됩니다. 규칙(CASCADE 등)까지 한 번에 읽습니다. |
| `trace_query` | 일부 — `execute_sql`로 `TRACE …`를 부르면 단계표가 나옴 | ★★ | 하 | AI가 느린 문장의 병목 단계를 짚게 하는 얇은 래퍼입니다. |
| `kill_session` | 없음 (엔진에 `KILL`이 없음) | ★★ | 중 | 엔진의 `KILL` 구현에 의존합니다. |
| resources(스키마)·prompts(템플릿) | 없음 | ★★ | 하 | 도구 호출 없이 스키마를 읽게 하는 정석 기능이지만 체감은 작습니다. |
| CSV 가져오기·내보내기 도구 | 없음 (UI에는 있음) | ★★ | 하~중 | UI의 `import_csv` 명령과 같은 일을 하는 도구입니다. |
| `diff_schema` | 없음 | ★★ | 중 | 쓰임새가 한정적입니다. |

## 5. 백엔드 (서버·엔진)

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| 남은 한계 정리 | 알려진 차이 15개(코퍼스 2,511문장 중). 이번에 하나씩 다시 확인: ① `SUM(date 열)`·`AVG(date 열)`이 `4048`(MySQL은 날짜를 수로 읽어 `40480406`) ② **`DATE('2024-03-05') + 0`이 `2024`(MySQL은 `20240305`)** — 조용한 오답 ③ FLOAT 열 = DOUBLE 열 비교가 같다고 나옴(MySQL은 다름) ④ `HAVING ANY_VALUE(v)`를 받아들임(MySQL은 오류) ⑤ 같은 표를 읽는 하위 질의가 든 `UPDATE`를 받아들임(MySQL은 1093) ⑥ `SHOW ENGINES`가 1행, `SHOW TABLE STATUS`·`SHOW COLLATION`의 수치·열 차이 ⑦ `VERSION()`이 `2.3.0`이라 `@@version LIKE '8%'`가 거짓 ⑧ 프로시저 안의 `SELECT a + b`의 열 이름이 `a + b`가 아니라 값을 넣은 `2+6`으로 나옴 | ★★★ | 하~중 | 항목마다 작아서 한 라운드로 묶기 좋습니다. ②는 가장 먼저 고칠 만합니다. |
| **부모 행 DELETE의 자식 검사가 O(자식 수)** | 일부 — INSERT 쪽은 O(1)로 고쳐져 있음(표 크기 0→21,000행에서 행당 0.2ms로 일정, UNIQUE·FK 있어도 같음). 남은 것: 참조되지 않은 부모 행을 지울 때 자식 표를 훑음(자식 2천 행 1.1ms → 2만 행 12ms, **자식 FK 열에 인덱스를 만들어도 8ms로 안 빨라짐**) | ★★★ | 중 | 처음 표의 "UNIQUE/FK 중복 검사 O(n)"은 INSERT는 이미 해결됐고 이 부분만 남았습니다. 성능 동결 중이라 하기 전에 먼저 여쭙겠습니다. |
| 코퍼스 비교 CI | 없음 (`.github` 없음) | ★★★ | 상 | MSVC 빌드, MySQL 컨테이너(리눅스 러너)에 리눅스 빌드 가능 여부 확인이 필요합니다. |
| 연결 수 상한 | 없음 — 처음 표에는 "설정만 받고 무시"라고 적었지만 **`max_connections` 변수 자체가 없음**(`@@max_connections`가 `NULL`) | ★★ | 하 | 서버 accept 루프의 작은 변경이고 변수도 같이 열어야 합니다. |
| TLS | 없음 | ★★ | 상 | Windows에서 TLS 라이브러리 도입과 핸드셰이크 확장이 큽니다. |
| `caching_sha2_password` | 없음 (`mysql_native_password`만) | ★★ | 상 | 지금 쓰는 mysql CLI와 pymysql은 모두 접속됩니다. 일부 최신 드라이버가 옵션을 요구할 때만 문제입니다. |
| WAL 복제, XA, 페이지 단위 버퍼 풀 | 없음 | ★ | 상 | 이미 범위 밖으로 정했고 남은 5주에는 맞지 않습니다. |

## 6. 발표 대비

| 항목 | 현재 상태 | 추천도 | 비용 | 근거 |
|---|---|---|---|---|
| **`DEMO.md`의 "피할 것" 목록 고치기** | **낡음** — `code/test/demo/DEMO.md`가 "`SUM(amount * 2)`는 파싱 오류", "함수 결과를 계산의 왼쪽에 쓰지 마세요" 같은 이미 고친 것을 아직 피하라고 적고 있음 | ★★★★ | 하 | 발표자가 멀쩡한 기능을 일부러 피하게 됩니다. 지금 엔진에 다시 돌려 확인하고 고치면 됩니다. |
| MCP 시연 대본·리허설 | 일부 (`DEMO.md` 4단계에 한 줄, "자동 검증 못함") | ★★★★★ | 하 | 대본을 쓰고 Claude Desktop에서 직접 한 번 돌려 봐야 합니다. `run_script` 미리보기(영향 행 확인 후 커밋)가 좋은 장면입니다. 연결은 직접 해 보셔야 합니다. |
| 크래시 복구 데모 | 일부 — 앱 Server Manager의 **Stop은 서버 프로세스를 강제 종료(`child.kill`)한 뒤 같은 데이터 폴더로 다시 띄우므로 사실상 크래시 시험**임. 없는 것은 복구 시간을 보여 주는 화면과 대본(`DEMO.md` 3단계는 `taskkill` 수동 절차) | ★★★★ | 하 | 버튼을 새로 만들 필요 없이 대본과 복구 시간 표시만 있으면 됩니다. |
| 락·데드락 데모 | 일부 (락·대기·교착 패널과 `SHOW LOCKS`·`information_schema.innodb_*`는 있음, 대본은 없음) | ★★★★ | 하 | 두 세션으로 락 대기와 교착을 일으키는 대본만 쓰면 패널이 실시간으로 보여 줍니다. |
| MVCC 나란히 보기 | 없음 (`SELECT _xmin`은 "Unknown column") | ★★★★ | 중~상 | 버전을 보여 주는 문(`SHOW VERSIONS` 같은 것)을 엔진에 먼저 열어야 합니다. |
| 한 문장의 여행에 보여 줄 문장 고르기 | 일부 (재생기는 있음) | ★★★ | 하 | JOIN+GROUP BY, INSERT의 커밋 단계 같은 3~4문장을 골라 두면 됩니다. |
| B+Tree 분할·재균형 애니메이션 | 없음 | ★★★ | 중 | 엔진 연동 없이 프론트에서 교육용 B+Tree를 그리면 됩니다. Journey 재생 방식을 재사용할 수 있습니다. |
| 최적화 전후 속도 | 일부 (Benchmark 패널이 "AutoCommit n배 빠름" 같은 배지를 실제 측정으로 보여 줌, 정적 `speedup_chart.png`) | ★★★ | 하 | 이미 있는 데이터를 애니메이션으로 바꾸면 됩니다. |
| RuSQL vs MySQL 분할 화면 | 없음 (Compat 페이지는 코퍼스별 일치 수만 보여 줌) | ★★★ | 중 | 발표장에 MySQL이 떠 있어야 해서 불안정합니다. |

## 점검에서 바로잡은 것 (처음 표와 달라진 곳)

- **더 나쁨:** `RANGE … n PRECEDING`은 "일부"가 아니라 **조용한 오답**이고, `DATE(x) + 0`도 조용한 오답입니다. 라벨 붙은 루프는 "END LOOP 라벨"만이 아니라 **라벨 문법 전체**가 없습니다. 연결 수 상한은 "무시"가 아니라 **변수 자체가 없습니다.** `DEMO.md`의 "피할 것" 목록이 낡았습니다.
- **더 좋음:** `UNIQUE/FK 검사 O(n)`은 INSERT 쪽이 이미 O(1)입니다(부모 DELETE만 남음). `DEFAULT (식)`은 `NOW()`류가 이미 됩니다. 크래시 복구 시험은 Server Manager의 Stop으로 이미 가능합니다. `list_locks`·`get_table_stats`·`trace_query`는 `execute_sql`로 이미 부를 수 있습니다.
- **새 항목:** `EXPLAIN FORMAT=JSON/TREE`, `SIGNAL` 묶음(`RESIGNAL`·`GET DIAGNOSTICS`), `UPDATE/DELETE IGNORE`의 `DELETE`, 없는 함수 45개의 구체 목록, `VERSION()` 문자열.

## 추천 순서 (약 5주)

1. **먼저 (하루 안팎):** `DEMO.md` 낡은 목록 고치기, MCP 시연 대본·리허설, 락·데드락 대본, 크래시 복구 대본(Stop 버튼 이용), `list_locks`·`get_table_stats`.
2. **조용한 오답부터:** `RANGE n PRECEDING`, `DATE(x) + 0`, `SUM(date 열)`, FLOAT = DOUBLE 비교 — 보고서·시연에서 틀린 값이 나오면 가장 곤란합니다.
3. **발표의 뼈대:** EXPLAIN 단계 막대, 여정에 보여 줄 문장 고르기.
4. **엔진:** `KILL`, `suggest_indexes`, 라벨 붙은 루프·`SIGNAL`.
5. **시간이 되면:** MVCC 나란히 보기, `WITH ROLLUP`, `INSERT INTO t () VALUES ()`, `DEFAULT (식)`, 느린 쿼리 로그, 부모 DELETE의 자식 검사(허락 후).
6. **미루거나 하지 않는 것:** `FULLTEXT`, 커서·`DECLARE HANDLER`, 생성 열, 한 열에 FK 여러 개, `caching_sha2_password`, TLS, WAL 복제.
