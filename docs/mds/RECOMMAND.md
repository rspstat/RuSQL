# RuSQL 추천 목록 — 분류별 (남은 항목만, R77 이후 · 2026-11-09)

완료된 항목은 뺐습니다(R44~R77에서 끝낸 것은 `DATE.md`에 있습니다). 항목마다 엔진에 실제로 돌려 보고(진짜 MySQL 8.4.9와 비교) 앱·MCP 코드를 찾아 이미 있거나 일부만 있는 것을 가려낸 판입니다. 점수판은 코퍼스 49개에서 차이 0이고(R77), B+Tree·MVCC 페이지는 R62에서 단순한 자동 재생으로 줄였습니다. 발표·제출물 중 포스터·소개글·메인 화면 캡처는 끝났습니다(`docs/deliverables/`). 점검에서 바뀐 것의 이력은 맨 아래 "참고"에 모았습니다.

- **현재 상태:** `일부`(일부만 있음) · `없음` · `오답`(지금 조용히 틀린 값을 돌려줌) · `다름`(MySQL과 다르지만 오류로 드러남)
- **추천도:** ★ 다섯 개. 기준은 실사용 가치 + 발표 임팩트 + 비용 대비 효과입니다. 졸업작품 마감(11월 말)까지 약 3주를 전제로 합니다. 각 분류 안에서 추천도 순입니다.
- **효과:** 그 일을 하면 얻는 것(발표에서 보이는 것, 막히는 사고, 호환되는 도구). 효과가 작은 항목은 "작습니다"라고 적었습니다.
- **비용:** 제가 구현하고 검증(코퍼스 비교, 심은 버그, 전체 시험, 문서)까지 마치는 데 걸리는 시간입니다. 하 ≈ 2~4시간, 중 ≈ 하루, 상 ≈ 2~4일. 빌드와 전체 시험 한 바퀴가 약 35분이라 헤더를 건드리는 변경은 여러 바퀴가 듭니다. **프론트(UI)는 자동 시험이 없어서 눈으로 확인하는 시간이 따로 붙습니다.**

## 한눈에 보기

| 분류 | 항목 수 | 가장 먼저 | 효과 |
|---|---|---|---|
| 1. 발표·제출 대비 | 6 | 발표 PC용 설치본에 엔진 묶기, **직접**: MCP·복구 장면 리허설, 녹화 | 발표 당일 앱이 안 뜨거나 시연이 끊기는 일을 막습니다 |
| 2. 안정성·무결성 | 2 | 크래시 퍼저의 불일치 하나(`fuzz_crash_fk.py 1 7701`), 아직 `KILL`이 안 듣는 것(`ALTER`·쓰는 도중) | 큰 위험은 R63~R69에서 닫혔고 남은 것은 드문 모양 둘입니다 |
| 3. 품질 점검 | 3 | 마감 1주 전에 Debug 전체 + `KILL`을 섞은 크래시 시험 | 마감 전에 숨은 회귀와 조용한 오답을 잡습니다 |
| 4. 프론트 (앱) | 4 | 인덱스 제안 카드의 남은 것(큰 표), 스키마 비교의 빠진 것 | 앱만으로 큰 표에서도 인덱스를 시험해 볼 수 있습니다 |
| 5. MCP (지금 37개 도구, resources 2개 · prompts 3개) | 1 | `suggest_indexes` 후속(열 사이의 상관 · 큰 표) | 제안이 더 많은 모양의 문장에서 맞아집니다 |
| 6. MySQL과 다른 작은 차이 | 26 | 조인 뷰의 `UPDATE`·`INSERT`, 프로토콜 오류 번호의 남은 약 20개, `USING` 뒤 모호한 열 | 남은 호환 차이가 줄지만 대부분 눈에 안 띕니다 |
| 7. 엔진 기능 추가 | 8 | **권한 강제와 역할**(지금은 `GRANT`가 기록만 함), `COLLATE` 절, `FULLTEXT`, `FLUSH` · `REPAIR` · `CHECKSUM TABLE` | 권한이 실제로 막고, 대소문자를 구분하는 열이 되고, `mysqldump`의 `FLUSH TABLES`를 받습니다 |
| 8. 백엔드 (서버·운영) | 5 | (시간이 되면) 코퍼스 비교 CI | 푸시마다 MySQL과의 차이 0을 자동으로 확인합니다 |
| 9. SQL 문법 추가 — 보류 | 31 | `INSERT INTO t () VALUES ()`, `GROUPING()`, `SIGNAL`, `UPDATE … JOIN (SELECT …)` | 옮겨 온 MySQL 쿼리가 더 많이 그대로 돕니다 |

## 1. 발표·제출 대비

발표 당일에 앱이 뜨고 장면이 돌아가게 하는 일. **직접** 하실 일(리허설·녹화)이 여기에 모여 있습니다.

| 항목 | 현재 상태 | 추천도 | 비용 | 효과 | 근거 |
|---|---|---|---|---|---|
| MCP 시연 리허설 | 일부 (`DEMO.md` 8번에 `run_script` 미리보기·`suggest_indexes` 대본, Claude Desktop과의 연결은 자동으로 검증하지 못함) | ★★★★★ | 하 | 발표장에서 Claude Desktop 연결이 처음 실패하는 일을 미리 막고, 대본의 시간과 문구를 확정합니다. | **직접** 해 보셔야 합니다. Claude Desktop에서 한 번 돌려 봐야 합니다. |
| 크래시 복구 장면 리허설 | 일부 — 대본은 `DEMO.md` 3번, 복구 카드는 R51에서 생김. 사람이 눌러 본 적은 없음 | ★★★★ | 하 | 서버를 죽였다 살리는 장면(복구 카드·ACTIVITY LOG)을 막힘 없이 보여 줄 수 있게 됩니다. | **직접** 해 보셔야 합니다. 앱 Server Manager의 Start/Stop → 복구 카드 → ACTIVITY LOG. |
| **발표 PC용 설치본·새 PC 점검** | 없음 — `tauri build`의 bundle 설정에 `engine_server.exe`·MCP 서버를 넣는 `externalBin`·`resources`가 없어서, 만든 설치본은 엔진을 못 찾음(지금은 `code/` 폴더 통째로 복사하는 방식) | ★★★★ | 중 | 처음 보는 PC에서도 앱이 뜹니다. 발표 당일의 가장 큰 위험(앱이 엔진을 못 찾음)이 사라집니다. | 발표 PC에서 앱이 안 뜨는 것이 가장 큰 위험입니다. 엔진을 같이 묶고, 깨끗한 PC(또는 새 사용자 계정)에서 설치부터 연결까지 한 번 돌려 봐야 합니다. |
| 시연 실패 대비 녹화·스크린샷 | 없음 | ★★★★ | 하 | 라이브가 안 될 때 바로 틀 수 있어 발표가 멈추지 않습니다. | **직접** 하셔야 합니다(화면 녹화). 라이브가 안 될 때 바로 틀 수 있습니다. |
| 최적화 전후 속도 애니메이션 | 일부 (Benchmark 패널이 "AutoCommit n배 빠름" 같은 배지를 실제 측정으로 보여 줌, 정적 `speedup_chart.png`) | ★★★ | 하~중 | 성능 개선을 숫자 표가 아니라 한눈에 보이는 장면으로 전달합니다. 이미 있는 측정값을 쓰므로 새 위험은 없습니다. | 이미 있는 데이터를 애니메이션으로 바꾸면 됩니다. |
| RuSQL vs MySQL 분할 화면 | 없음 (Compat 페이지는 코퍼스별 일치 수만 보여 주고, 활동 표시줄의 아이콘은 11-02부터 숨겨 둠 — `SHOW_COMPAT_ICON`) | ★★★ | 중 | "같은 문장, 같은 답"을 눈앞에서 보여 줘 호환성 주장의 설득력이 커집니다. 발표장에 MySQL이 떠 있어야 하는 것이 약점입니다. | 발표장에 MySQL이 떠 있어야 해서 불안정합니다. |

## 2. 안정성·무결성

데이터가 틀어지거나 서버가 멈출 수 있는 것. 마감 전에 한 번은 가려야 합니다.

| 항목 | 현재 상태 | 추천도 | 비용 | 효과 | 근거 |
|---|---|---|---|---|---|
| 아직 `KILL`이 안 듣는 것 | 일부 — R63~R67에서 SELECT(정렬·`GROUP BY`·`DISTINCT`·윈도·조인·`UNION` 계열·병렬 스캔)·`INSERT … SELECT`·`CALL` 본문·`UPDATE`·`DELETE`·`MERGE`·다중 표 문장의 행 찾기·`EXPLAIN ANALYZE`·`EXECUTE`한 질의·`CREATE TABLE … AS SELECT`의 질의·`CREATE INDEX`의 빌드가 멈춥니다. 남은 것: `ALTER TABLE`와 그 밖의 DDL, 트리거·사용자 함수 본문의 SELECT(쓰는 문장 안이라 이미 쓰기가 시작됨), 쓰는 문장이 행을 쓰기 시작한 뒤(반쯤 쓰다 멈추면 되돌릴 길이 없어서 일부러). **`ALTER`는 표의 행을 제자리에서 바꿉니다**(`ADD COLUMN`은 `for (auto& row : it->second) row[새열] = 기본값`; `DROP`·`RENAME`·`MODIFY`도 같음) — 반쯤 하다 멈추면 살아 있는 표가 반만 바뀌어서, 멈추려면 새 행 목록을 따로 만들었다가 바꿔 치는 식으로 동작마다 다시 써야 합니다 | ★ | 상 | 세션 패널의 `Stop statement`가 읽기 단계의 모든 경우에 듣습니다. 남은 것은 문장 단위 롤백이 있어야 안전한 쓰기 도중과 `ALTER`뿐입니다. | `ALTER`는 큰 일이라 마감 전에 필요한 것은 아닙니다. 읽기만 하는 `ADD UNIQUE`의 중복 확인 같은 일부는 `CREATE INDEX`처럼 `ReadPhase`로 감쌀 수 있으나 걸리는 시간이 짧아 얻는 것이 작습니다. |
| 크래시 퍼저가 찾은 불일치 (`py code/test/crash/fuzz_crash_fk.py 1 7701`) | **오답(복구)** — 결정적으로 나옴. `ON DELETE RESTRICT ON UPDATE SET NULL`인 자식 표에서 한 트랜잭션 안의 `INSERT` → `ROLLBACK TO` → `REPLACE` → `ROLLBACK TO` → `COMMIT` 뒤 kill -9로 복구하면, 모델이 "id 15의 pid가 NULL(부모 키가 바뀌어 SET NULL)"이라 기대한 행이 복구된 표와 다름. R77 이전 커밋(`15fa47a`)에서도 똑같이 나옴 | ★★★ | 중 | 커밋한 일이 크래시 뒤에 틀어지는 드문 모양 하나가 닫힙니다. | 원인은 찾지 않았습니다. `FUZZ_TRACE=1`로 문장을 다 볼 수 있고 `RUSQL_SERVER_EXE`로 다른 빌드와 견줄 수 있습니다. 세이브포인트로 되돌린 `REPLACE`의 redo와 `ON UPDATE SET NULL`의 연쇄가 겹치는 자리로 의심됩니다(추정). |

## 3. 품질 점검

새 기능이 아니라 지금 있는 것이 멀쩡한지 확인하는 일.

| 항목 | 현재 상태 | 추천도 | 비용 | 효과 | 근거 |
|---|---|---|---|---|---|
| 누적 점검 한 바퀴 (Debug 전체 + 크래시 퍼저 + 빌드 간 차분) | 일부 — R54는 크래시 퍼저 3종(38라운드)과 R53 빌드와의 A/B까지 돌렸지만 Debug는 R50 이후 라운드마다 건너뛰었고(R63·R64도), R53의 락 대기·세션 정리 코드는 퍼저가 아직 다루지 않음(`KILL`을 섞은 퍼저가 없음) | ★★★ | 하 (기계 시간 약 1시간) | 최근 열다섯 라운드에 쌓인 숨은 회귀(Debug 빌드에서만 드러나는 오류, `KILL`과 얽힌 동시성)를 마감 전에 잡습니다. | 마감 전에 Debug 전체와 `KILL`을 섞은 크래시 시험을 한 번은 돌려야 안심입니다. |
| 새 영역 훑기 (진짜 MySQL과 문장 비교) | 일부 — R71에서 트랜잭션 · 트리거 쓰기 · 뷰 쓰기 · 외래키 동작 · 날짜 계산(363문장, 차이 124 → 0), R77에서 JSON 함수(245문장, 50 → 0) · 준비된 문장의 바이너리 프로토콜(81번 실행, 읽을 수 없던 답 → 0) · 권한 · 역할(115문장, 90개 다름, 고치지 않음) · `COLLATE` 절(69문장, 62개 다름, 고치지 않음)을 훑음. 아직 안 훑은 모양: 문자셋 변환(`CONVERT(... USING ...)`), 잠금 읽기(`FOR UPDATE`)의 대기와 오류, 파티션이 걸린 표의 문장, `COM_STMT_SEND_LONG_DATA` · 서버 커서 | ★★★ | 중 | 아직 안 본 모양의 조용한 오답을 찾습니다(R77에서도 바이너리 프로토콜의 읽을 수 없는 답과 권한이 강제되지 않는 것이 나왔습니다). | 새 코퍼스를 쓰고 `vs_mysql.py`(바이너리 프로토콜은 `prepared_wire.py`)로 비교 → 고치고 → 점수판에 올립니다. R77의 `privileges.sql` · `collate_clause.sql`은 차이가 남아 있어 점수판 밖에 두었습니다. |
| 앱 화면 사람 눈 점검 | 일부 — 화면은 디버그 포트로 DOM을 눌러 확인했을 뿐, 아이콘·폰트·전체 모양은 본 적 없음 | ★★★ | 하 | 자동 확인이 못 보는 아이콘·폰트·전체 모양의 어색함을 찾습니다. | **직접** 앱을 한 바퀴 눌러 보시고 어색한 곳을 알려 주세요. |

## 4. 프론트 (앱)

앱 화면과 그 안의 도구.

| 항목 | 현재 상태 | 추천도 | 비용 | 효과 | 근거 |
|---|---|---|---|---|---|
| 인덱스 제안 카드의 남은 것 | 일부 — R70에서 하위 질의 · CTE · UNION · OR(가지별)을 엔진이 읽고 카드가 어느 블록의 제안인지 말하며, UPDATE · DELETE의 `EXPLAIN` 아래에도 카드가 뜨고 "Try it (estimate)"가 `EXPLAIN`의 비용 · 접근 경로 전·후를 비교함(실행하지 않음). 남은 것: 20만 행 넘는 표의 시험해 보기, OR 가지의 시험(UNION 다시 쓰기를 앱이 만들지 않음) | ★ | 중 | 큰 표에서도 시험해 볼 수 있고 OR의 가지 제안을 앱에서 한 번에 시험합니다. | 큰 표의 시험은 인덱스를 만드는 시간이 길어 표 크기 상한과 취소 버튼 같은 카드 쪽 정책이 먼저입니다. |
| 스키마 비교의 빠진 것 | 일부 — R59의 "Compare Schema…"는 표·열·인덱스·외래키만 비교하고 마이그레이션 SQL도 그것만 만듦. 없는 것: 뷰·트리거·프로시저·`CHECK`·기본키 변경·열 순서·`ON UPDATE`가 아닌 표 옵션, 마이그레이션의 의존 순서가 사슬을 넘는 경우의 검증 | ★★ | 중 | 뷰·트리거까지 비교해 마이그레이션 SQL이 더 완전해집니다. 쓰임새는 한정적입니다. | 뷰와 트리거는 `SHOW CREATE`로 통째 비교하면 됩니다. 쓰임새는 한정적입니다. |
| 라이트/다크 테마 | 없음 (다크만) | ★★ | 중~상 | 밝은 발표장·프로젝터에서 읽기 편한 선택지가 생기지만 졸업작품 핵심과는 거리가 있습니다. | 색이 코드에 박혀 있어 변수화가 먼저입니다. |
| SQL 스니펫(자주 쓰는 문장 저장·삽입) | 없음 (쿼리 히스토리·SQL 포맷터·셀 편집은 있음) | ★ | 하~중 | 자주 쓰는 문장을 다시 치지 않아도 되어 편해지지만 핵심 가치는 작습니다. | 있으면 편하지만 졸업작품 핵심과는 거리가 있습니다. |

## 5. MCP (지금 37개 도구, resources 2개 · prompts 3개)

Claude가 부르는 도구.

| 항목 | 현재 상태 | 추천도 | 비용 | 효과 | 근거 |
|---|---|---|---|---|---|
| `suggest_indexes` 후속 | 일부 — R70에서 읽기를 엔진의 `SUGGEST INDEX`로 합쳤고(앱과 Claude가 같은 답) 쓰기 문장의 `verify`가 됨, R71에서 범위 조건이 둘이면 값 종류가 많은 열을 고르고 ASC/DESC가 섞인 정렬을 노트로 말함. 없는 것: 열 사이의 상관, 20만 행 넘는 표(커버링 인덱스는 이 엔진이 인덱스만 읽는 길이 없어 뜻이 없음) | ★ | 중 | 복잡한 조건이 섞인 문장에서도 인덱스 제안이 맞아집니다. | 엔진 쪽 `Advisor`(`executor_advice.cpp`)에 규칙을 더하면 앱과 MCP가 함께 좋아집니다. |

## 6. MySQL과 다른 작은 차이

진짜 MySQL 8.4.9와 비교해 남은 것들. 대부분 오류로 거절하는 쪽이거나 모양이 작아서 조용한 큰 오답은 아닙니다(점수판 코퍼스 47개는 차이 0).

| 항목 | 현재 상태 | 추천도 | 비용 | 효과 | 근거 |
|---|---|---|---|---|---|
| 글자와 숫자를 비교하는 남은 자리 | 오답 — `IN (서브쿼리)`: `SELECT s FROM s WHERE s IN (SELECT 7)`가 글자 열의 `'007'`을 빼고 `'7'`만 돌려주고(MySQL은 글자와 숫자를 숫자로 비교해 둘 다), `'007' IN (SELECT 7)`는 0(MySQL 1). `CHECK (s = '7')`는 `'007'`을 받아들임(MySQL 오류 3819). `s IN (7)` · `s = (SELECT 7)` · 프로시저 `IF`는 이미 같음. 갭 락 · 파티션 경계 비교는 점검하지 않았음 | ★★ | 중 | 서브쿼리와 `CHECK`가 조용히 틀리는 자리가 닫힙니다. | 이 둘은 아직 값의 모양으로 비교하는 예전 규칙입니다. 타입 인식 비교(`value_class.hpp`)로 바꾸면 됩니다. 정리한 `PLAN.md`에서 옮긴 항목입니다. |
| `UNION`의 마지막이 FROM 없는 SELECT일 때 `ORDER BY` | 없음(문법 오류 1064) — `SELECT id FROM t UNION SELECT 9 ORDER BY 1`, `SELECT 5 UNION SELECT 3 ORDER BY 1`이 "Unexpected token(s)"로 거절됨. 마지막 SELECT에 FROM이 있으면 됨(`… UNION SELECT id FROM t2 ORDER BY 1`) | ★★ | 하 | 값 목록을 `UNION`으로 이어 정렬하는 흔한 모양이 됩니다. | 파서가 FROM 없는 SELECT 뒤의 `ORDER BY`를 UNION 전체의 것으로 읽지 못합니다. |
| `USING` 조인 뒤의 모호한 열 | 오답(작음) — `a JOIN b USING (x) JOIN c USING (y) JOIN d USING (x)`에서 `d`도 `y`를 가지면 `ORDER BY y`는 MySQL이 오류 1052(모호한 열)로 거절하는데 여기는 행을 돌려줌. `SELECT a.*, c.* … ORDER BY y`도 같은 모양 | ★ | 하 | MySQL이 거절하는 모호한 열을 같이 거절해 조용한 오답 하나가 사라집니다. | R58에서 `*` 열 순서를 맞추다 알게 된 것입니다. 합쳐진 열이 뒤 조인의 표에도 있을 때의 모호함 검사입니다. |
| 자기 표 UPDATE가 행 단위가 아님 | 일부 — 문장 전체를 한 번에 봐서 `UPDATE emp SET mgr = NULL, id = id + 10 ORDER BY id DESC`를 MySQL은 통과시키는데 여기는 거절합니다(R57에서 다중 표 DELETE·MERGE·REPLACE의 사슬 아래 검사는 끝남) | ★ | 중 | MySQL이 받는 자기 표 UPDATE를 그대로 받아 옮겨 온 쿼리가 거절되지 않습니다. | UPDATE를 행마다 모의해야 합니다. 오류로 거절하는 쪽이라 조용한 오답은 아닙니다. |
| 프로토콜 오류 번호의 남은 것 | 일부 — R58에서 메시지 글자로 번호·SQL 상태를 정함(`wire_error`, 약 40종). 코퍼스 전체에서 번호만 다른 문장 385개 → 약 20개. 남은 것은 엔진이 MySQL과 다른 이유로 실패하는 경우(문법을 못 받음, 메시지가 아무 말도 안 함): `ONLY_FULL_GROUP_BY`(1055), `RANGE` 프레임 오류(3587~3589), 윈도 함수를 쓸 수 없는 자리(3593), `SUM(SUM(x)) OVER`의 1140, `Data truncated`(1265) 일부, `MOD`로 0을 나누는 UPDATE(1365), 없는 윈도 이름(3579) | ★ | 하 | 오류 번호로 분기하는 클라이언트·ORM 코드가 MySQL에서처럼 동작합니다. | 번호는 `vs_mysql.py --proto --errno`가 코퍼스마다 비교합니다. 엔진 메시지를 MySQL 것으로 바꾸는 쪽이 정석이라 하나씩 고치면 됩니다. |
| 몇 가지 `SHOW`의 빈 결과·문법 | 일부 — R57에서 USING·NATURAL 조인과 `CALL`의 빈 결과도 열 이름을 말하게 됨. 남은 것: `SHOW DATABASES LIKE`·`SHOW ERRORS`는 문법 오류, `SHOW GRANTS FOR 없는사용자`는 오류 1141 대신 빈 성공 | ★ | 하 | `SHOW DATABASES LIKE` 같은 문장을 쓰는 도구가 오류 없이 돕니다. 영향은 작습니다. | 작은 모양들입니다. MySQL 클라이언트가 접속 직후 보내는 문장은 아닙니다. |
| FLOAT 열을 글자 함수에 넣으면 6자리가 아님 | 오답(작음) — `CONCAT(f, '')`가 MySQL은 `3.14159`(FLOAT는 6자리 글자), 여기는 저장된 글자 `3.1415927`. (FLOAT를 내는 함수 결과를 글자로 바꾸면 MySQL은 넓힌 값 `0.10000000149011612`를 줌: `CONCAT(COALESCE(f, 0), '')`) | ★ | 하 | FLOAT 값을 글자로 바꿀 때 MySQL과 같은 자릿수가 나옵니다. 드문 모양입니다. | R58의 코퍼스를 만들다 알게 된 것입니다. 글자 함수가 인자의 FLOAT 여부를 알아야 합니다. |
| `PI()`에서 만든 식의 자릿수 | 오답(작음) — `PI()` 단독은 `3.141593`이 되었지만 `PI() + 0`·`PI() * 2`는 전체 자릿수(MySQL은 `PI()`가 소수 여섯 자리를 고정해서 식으로 전해짐: `PI() + 0`이 `3.141593`) | ★ | 하 | `PI() * 2` 같은 식의 자릿수가 MySQL과 같아집니다. 거의 눈에 안 띕니다. | R60에서 `WITH ROLLUP`과 함께 맞추다 알게 된 것입니다. 결과 열의 "고정 소수 자릿수" 속성이 식으로 전해지는 규칙이 필요합니다(`FLOAT`의 6자리와 같은 계열). |
| 조인 뷰의 `UPDATE` · `INSERT` | 다름 — `CREATE VIEW v AS SELECT … FROM a JOIN b …`에 `UPDATE v SET (a의 열)`·`INSERT INTO v (a의 열)`을 하면 "View is not updatable"로 거절합니다(MySQL은 한 표의 열만 쓰면 받습니다. 조인 뷰의 `DELETE`는 둘 다 거절: 1395). | ★★ | 중 | 조인으로 만든 뷰에 쓰기가 되는 흔한 모양이 돕니다. | R71의 `resolve_view_target`(뷰 위의 뷰 · 열 이름 대응)가 한 표 뷰만 풉니다. 조인 뷰는 어느 표의 열인지 가려서 그 표로 보내고 조인 조건을 `WHERE`로 거는 일이 더해야 합니다. |
| 격리 수준의 기본값 | 다름 — 아무것도 안 정한 세션은 MySQL이 `REPEATABLE-READ`, 이 엔진은 `READ-COMMITTED`(R71에서 `@@transaction_isolation`이 실제 수준을 말하게 됨). R72에서 REPEATABLE READ의 자기 쓰기가 두 번 보이던 문제와 `FOR UPDATE`가 스냅샷의 옛 값을 읽던 것을, R73에서 스냅샷 시점(이제 첫 읽기, `WITH CONSISTENT SNAPSHOT`은 즉시)과 `INSERT … SELECT` 원본의 최신 읽기를, R74에서 일반 SELECT가 아닌 모든 문장(`UPDATE` · `DELETE` · `INSERT … VALUES` · `SET @x`)의 서브쿼리 최신 읽기와 `INSERT … SELECT` 원본의 공유 잠금을 고쳤다. 남은 것: 기본값이 REPEATABLE READ가 되면 모든 명시적 트랜잭션이 갭 잠금을 잡는 데서 오는 대기 · 교착 재기, 기본값을 바꿔 전체 시험을 돌려 깨지는 것 고치기 | ★★ | 중 | MySQL 도구가 기대하는 기본 격리 수준이 같아집니다. | 스냅샷 시점은 맞췄으니 다음은 갭 잠금의 대기 · 교착 영향을 재 보고, 기본값을 바꿔 전체 시험을 돌려 깨지는 것을 고칩니다. 발표 데모에서는 차이가 거의 보이지 않으므로 마감 이후로 미뤄도 됩니다. |
| 공유 잠금의 남은 모양 | 일부 — R74에서 한 표를 평범하게 읽는 `INSERT … SELECT` 원본(기본키 한 점 · 범위 · 전체)은 MySQL과 같은 행 · 틈 잠금을 건다. 남은 것: 조인 · 집계 · 뷰 원본과 서브쿼리가 읽는 표(`INSERT … SELECT` · `UPDATE` · `DELETE` · `INSERT … VALUES` 모두)는 최신 값만 읽고 잠그지 않음, 기본키가 아닌 조건은 읽어서 돌려준 행만 잠그고(MySQL은 훑은 행 모두) 틈은 표 전체를 잠금(MySQL은 그 인덱스의 범위) | ★ | 중 | 잠금을 보여 주는 데모에서 MySQL과 같은 대기 모양이 더 많이 나옵니다. | 훑는 도중에 서브쿼리를 돌리는 문장은 잠금을 기다리려면 훑던 행에서 손을 놓는 법부터 정해야 합니다. 발표에서는 거의 안 보이는 모양입니다. |
| 트랜잭션 밖의 `SELECT … FOR UPDATE` | 다름(작음) — 잠금을 기다리지 않고 바로 읽음(MySQL은 상대의 잠금이 풀릴 때까지 기다림) | ★ | 하 | 자동 커밋 상태의 잠금 읽기도 MySQL처럼 기다립니다. | R74에서 잠금 표를 재다 보였습니다. 자동 커밋에서는 잠금이 곧 사라지므로 의미가 작습니다. |
| `CALL` · 저장 함수 · `EXECUTE`가 돌려주는 값의 형 | 다름(작음) — 숫자가 글자 형(`'10'`)으로 옴(MySQL은 `10`). `SELECT fi()`(저장 함수)와 `EXECUTE`의 결과도 `'7'`로 옴 | ★ | 하 | 클라이언트가 프로시저 결과 열을 숫자로 받습니다. | R74에서 비교하다 보였습니다. 프로시저가 마지막 `SELECT`의 결과를 글자 표로 넘기는 곳에서 열 형을 잃습니다. |
| JSON의 남은 틈 | 다름(작음) — `JSON_ARRAY(1 = 1)`처럼 비교 · 논리 식의 결과는 JSON 불리언이 아니라 `1`로 들어감(`TRUE` · `FALSE` 글자는 불리언), 지수 리터럴 `1e3`은 어휘 분석이 `1000`으로 써서 JSON에서 `1000.0`이 아니라 `1000`, `JSON_ARRAY(1/3)`이 `0.3333`(MySQL `0.333333333`), `JSON_ARRAY(JSON_ARRAY(1.50))`처럼 한 번 글자가 된 DECIMAL은 다시 읽을 때 DOUBLE이 됨(`1.5`), `JSON_TYPE`이 JSON `null`에 내는 `NULL`이 SQL NULL과 같은 글자라 구별되지 않음, JSON 열에 **수**를 넣으면 MySQL은 3140인데 여기는 받음, `JSON_STORAGE_SIZE` · `JSON_TABLE` 없음 | ★ | 하~중 | JSON을 많이 쓰는 쿼리의 드문 모양이 MySQL과 같은 값을 줍니다. | R77에서 JSON 함수 245문장의 차이를 0으로 만들며 남은 것입니다. 값이 글자로만 오가는 구조의 한계가 대부분이라 식의 형(불리언 · DOUBLE 리터럴)을 인자로 넘기는 길이 필요합니다. |
| 준비된 문장(바이너리 프로토콜)의 남은 틈 | 다름(작음) — `COM_STMT_PREPARE`가 열 개수를 0으로 답함(실행 답에 열 정의가 와서 드라이버들은 읽지만, 준비만 하고 열 정보를 물으면 비어 있음), `SELECT i + ?`의 열 이름이 MySQL은 `i + ?`인데 여기는 값을 넣은 `i + 1`, 명시한 AUTO_INCREMENT 값의 `REPLACE` · `ON DUPLICATE KEY UPDATE`가 OK 패킷의 last insert id를 0으로 줌(MySQL 1; 텍스트 프로토콜도 같음), `COM_STMT_SEND_LONG_DATA` · 서버 커서(`COM_STMT_FETCH`) 없음, 준비 시점의 오류(없는 표 · 열 개수)가 실행 때 나옴 | ★ | 중 | 준비만 하고 열 정보를 읽는 드라이버(JDBC의 `getMetaData`)와 큰 값을 나눠 보내는 클라이언트가 돕니다. | R77의 `prepared_wire.py`가 81번 실행을 견주어 읽을 수 없던 답을 고치며 남은 것입니다. |
| `SET @@transaction_isolation = x` (SESSION 없이) | 다름(작음) — MySQL은 다음 트랜잭션 하나에만 적용하고(`SET TRANSACTION`처럼) 여기는 세션에 적용합니다. `GLOBAL`도 세션처럼 읽음 | ★ | 하 | 같은 문장이 같은 값을 보입니다. | 어휘 단계에서 `@@session.x`의 범위가 지워져 구분하려면 어휘 분석이 범위를 남겨야 합니다. |
| 날짜의 남은 모양 | 다름(작음) — `INTERVAL 1 DAY + '2024-01-31'`(INTERVAL이 앞)이 문법 오류, `DATE_SUB('0001-01-01', INTERVAL 1 DAY)`는 MySQL이 `0000-00-00`, `INTERVAL '1.5' DAY`는 MySQL이 글자를 정수 1로 읽음, 이름 붙은 시간대의 `CONVERT_TZ`는 NULL(MySQL도 시간대 표를 안 올리면 같음) | ★ | 하 | 옮겨 온 쿼리의 드문 날짜 식이 그대로 돕니다. | 식 파서가 `INTERVAL`로 시작하는 식을 받도록 하는 것이 가장 큽니다. |
| MySQL 프로토콜 OK 패킷의 영향 행 수 | 다름(작음) — `CALL p()`는 `UPDATE` 같은 쓰기에서는 이미 같고 `SELECT … INTO`만 다름(MySQL은 1, 여기 0), 표를 다시 쓰는 `ALTER TABLE … MODIFY`는 표의 행 수를 돌려줌(MySQL 3, 여기 0; `ADD COLUMN`은 둘 다 0) | ★ | 하 | 일부 클라이언트가 보여 주는 "영향 받은 행"이 같아집니다. | `affected_rows_`를 `CALL`과 `ALTER`가 채우도록 합니다. |
| 외래키 열 종류 검사 | 다름(작음) — R71의 3780은 큰 갈래(정수끼리 · 글자끼리 …)만 봄: MySQL은 `INT`와 `BIGINT`, 부호가 다른 정수도 거절하는데 여기는 받음 | ★ | 하 | 같은 정의가 같은 오류를 냅니다. | `fk_type_class`에 정수 너비와 부호를 더하면 됩니다(기존 시험 · 시연 스키마가 걸리는지 먼저 확인). |
| 뷰를 만들 때의 `ONLY_FULL_GROUP_BY` | 다름(작음) — `CREATE VIEW v AS SELECT COUNT(*), x FROM t`를 만들 때 거절합니다(MySQL은 만들 때는 받고 조회할 때 거절) | ★ | 하 | MySQL이 받아 주는 뷰가 덤프 복원에서 거절되는 일이 없어집니다. | R60의 시험을 쓰다 알게 된 것입니다. 뷰 정의의 검사는 만들 때 하지 않고 조회에서 하면 됩니다. |
| `SHOW TABLE STATUS`의 `Create_time`·`Update_time` | 다름 — NULL입니다(MySQL은 표를 만들고 마지막으로 바꾼 시각). R60에서 크기는 InnoDB의 쪽 수로 맞췄습니다 | ★ | 하~중 | 관리 도구가 표를 만든·바꾼 시각을 보여 줄 수 있습니다. | 표를 만든 시각은 카탈로그에 저장해야 재시작 뒤에도 남고, 바뀐 시각은 문장마다 갱신해야 합니다. |
| `SET slow_query_log`(GLOBAL 없이)와 `long_query_time`의 세션 값 | 다름(작음) — MySQL은 `SET slow_query_log = ON`을 오류 1229로 거절하고 `long_query_time`에는 세션 값이 따로 있어(`SET GLOBAL`은 이미 열린 연결에 닿지 않음) 여기는 둘 다 서버 전체 하나로 받음. `slow_query.log`는 커지기만 함 | ★ | 하 | 서버 변수의 범위(GLOBAL/SESSION)가 MySQL과 같아집니다. 로그 파일의 크기 상한·회전은 따로 하는 일입니다. | 일부러 단순하게 둔 것이라(앱의 패널이 켜자마자 보이게) 서버 변수 범위(GLOBAL/SESSION)를 구분하는 일이 먼저 필요합니다. 파일 크기 상한·회전은 따로 하~중. |
| `AVG`를 식에서 쓸 때와 DECIMAL 자릿수 | 오답(작음) — 값 1, 2, 2의 `AVG(v) * 3`이 `5.0001`(MySQL `5.0000`)이고 `AVG(v) = 1.6667`이 참(MySQL 거짓): 보이는 4자리 값으로 이어 계산하는데 MySQL은 9자리를 들고 있음. `COALESCE(1.5, 2.25)`는 `1.5`, `SELECT 1.5 UNION SELECT 2.25`의 첫 값은 `1.5`(MySQL은 둘 다 `1.50`, `2.25`로 자릿수를 맞춤) | ★ | 중 | `AVG`로 이어 계산하는 보고서 식이 MySQL과 같은 값을 줍니다. | 정확히 하려면 `AVG`의 중간값을 9자리로 들고 다녀야 합니다. 크기는 작지만 비교에서는 참 · 거짓이 갈립니다. |
| DOUBLE 식의 반올림과 지수 표기 | 다름(작음) — `ROUND(SQRT(6.25))` · `ROUND(2.5e0)`은 MySQL이 DOUBLE이라 짝수 쪽 `2.0`, 여기는 `3.0` · `3`. `1e3` · `1E2 * 3`은 MySQL이 DOUBLE이라 `1000.0` · `300.0`, 여기는 정수 `1000` · `300`. `ROUND(SQRT(2), 3)`은 같음 | ★ | 중 | 부동소수점 식의 반올림이 MySQL과 같은 쪽으로 됩니다. | 지수 표기 리터럴을 항상 DOUBLE로 읽는 길이 필요합니다. |
| 순서 없는 `SELECT`의 행 순서 | 다름(작음) — 기본키 없는 표의 `UPDATE`는 바뀐 행을 끝으로 옮겨 `(1,11), (2,2), (3,3)` 대신 `(2,2), (3,3), (1,11)`로 나옴(InnoDB는 제자리). 조인에서 둘째 표 쪽의 행 순서도 MySQL과 다를 수 있음. 어느 쪽도 `ORDER BY` 없이는 순서를 보장하지 않음 | ★ | 중 | `ORDER BY` 없는 조회를 견주는 도구에서 차이가 줄어듭니다. | 기본키 순서 읽기(R34)는 이미 있지만 기본키 없는 표는 갱신한 행을 제자리에 두지 않습니다. |
| 결과 열 이름 · 표시의 작은 차이 | 다름(작음) — 값은 같고 머리글만 다름: `ROUND(v)*100` → `ROUND(v) * 100`, `IF(c, a, b)` → `CASE WHEN … END`, `POSITION('b' IN 'abc')` → `LOCATE(…)`, `(1, 2) = (1, 2)` → `(1 = 1 AND 2 = 2)`, `1 || 0` → `(1 <> 0 OR 0 <> 0)`, `1e3` → `1000`, `GET_FORMAT(DATE, …)` → `GET_FORMAT(date, …)`. `information_schema`의 열 이름은 소문자(MySQL은 `INDEX_NAME`처럼 대문자). 명령줄 상자 표에서는 값 양 끝의 공백이 `\s`로 보임(MySQL 클라이언트에는 프로토콜 계층이 되돌려 줌) | ★ | 중 | 머리글을 그대로 보여 주는 도구와 화면이 MySQL과 같은 글자를 보입니다. | 파서가 식의 원문을 열 이름으로 보관하는 길이 필요합니다. |
| 표 정의의 작은 틈 | 다름(작음) — `ALTER … RENAME COLUMN`이 `CHECK`가 쓰는 열을 막지 않음(MySQL 오류 3959), 한 열에 `UNIQUE`와 `UNIQUE (a)`를 둘 다 쓰면 색인 하나(MySQL은 둘), `information_schema.columns.column_type`이 `tinyint(1)`을 `tinyint`로 말함, `CREATE INDEX … (a DESC)`는 받지만 방향을 `A`로 기록함(MySQL `D`) | ★ | 하 | 스키마를 읽는 도구가 MySQL과 같은 정의를 봅니다. | 넷 모두 작은 변경입니다. |
| 함수의 작은 틈 | 다름(작음) — 없는 함수 이름을 평가할 때 검사해서 빈 표의 `SELECT nosuch(id) FROM em`이 오류 없이 빈 결과(MySQL 1305), `INSTR('Strasse', 'ß')` · `LOCATE('ss', 'Straße')`는 MySQL이 5, 여기 0(`ß` = `ss` 같은 정렬 규칙의 확장 글자), `ANALYZE` 히스토그램은 글자를 바이트 순서로 정렬함(추정에만 영향) | ★ | 하~중 | 오타 난 함수 이름이 빈 표에서도 오류로 드러납니다. | 함수 이름 검사를 문장을 풀 때로 옮기면 됩니다. |
| 저장 루틴 · DML의 남은 모양 | 다름(작음) — 프로시저가 `SELECT`를 여러 번 하면 마지막 표 하나만 답함(MySQL은 모든 집합을 보내고 클라이언트는 첫 것부터 읽음: `SELECT 100; SELECT 1`이 MySQL `100`, 여기 `1`), 재귀 호출을 허용함(MySQL 기본 `max_sp_recursion_depth = 0`은 오류 1456), 같은 값 행을 `REPLACE`하면 영향 행 수가 2(MySQL 1) | ★ | 중 | 프로시저 결과를 읽는 클라이언트가 MySQL과 같은 첫 집합을 받습니다. | 여러 결과 집합은 프로토콜 계층 변경이라 셋 중 가장 큽니다. |

## 7. 엔진 기능 추가

새 동작을 더하는 일.

| 항목 | 현재 상태 | 추천도 | 비용 | 효과 | 근거 |
|---|---|---|---|---|---|
| **권한 강제와 역할 (`GRANT` · `REVOKE`가 기록만 함)** | **오답(보안)** — 어떤 문장도 권한을 확인하지 않아서 `SELECT`만 받은 사용자가 `INSERT` · `UPDATE` · `DELETE` · `DROP TABLE`을 다 하고, 권한이 하나도 없는 사용자도 표를 읽음(R77의 `corpus/privileges.sql` 115문장 중 **90개가 다름**). 같은 뿌리의 모양: 오류 번호(중복 사용자 1396 · 없는 사용자 1396/1141 · 권한 부족 1142/1143/1227/1044 · `GRANT`가 사용자를 만듦 1410 · 없는 역할 3523 · 순환 역할 4027), `SHOW GRANTS`의 글자 모양(MySQL: `` GRANT SELECT ON `v`.`t` TO `alice`@`%` ``과 맨 앞의 `GRANT USAGE ON *.*` 줄), 문법 빈틈(`ALTER USER` · `RENAME USER` · `CREATE ROLE 'x'` · `GRANT '역할' TO 사용자`(여기는 `GRANT ROLE ...`) · `REVOKE ALL PRIVILEGES, GRANT OPTION` · 열 단위 `GRANT SELECT (col)` · `SET ROLE` · `SET DEFAULT ROLE` · `CURRENT_ROLE()`), `CURRENT_USER()`가 `alice@localhost`(MySQL `alice@%`) | ★★★★ | 상 | `SELECT`만 받은 사용자가 쓰지 못하는 권한 시연이 가능해지고, 권한이 새는 구멍이 닫힙니다. 발표에서 "권한 관리"를 말할 수 있게 됩니다. | 역할이 사용자와 따로 놓여 있고(`RoleRecord`) 문장을 실행하기 전에 "이 계정이 이 표에 이 권한이 있는가"를 묻는 곳이 없습니다. 문장의 종류마다 필요한 권한(`SELECT`는 읽는 표, `INSERT ... SELECT`는 둘 다, DDL은 `CREATE` · `DROP` · `ALTER` · `INDEX`, 사용자 관리는 전역 `CREATE USER` ...)을 모으는 일, 역할을 계정으로 합치는 일, 활성 역할(`SET ROLE`), `SHOW TABLES` · `information_schema`가 권한 있는 것만 보이게 하는 일이 같이 들어갑니다. 코퍼스가 있어 `vs_mysql.py ... corpus/privileges.sql --proto --errno`로 진척을 잴 수 있습니다. |
| `FULLTEXT` / `MATCH … AGAINST` | 없음 (`FULLTEXT KEY`가 든 CREATE TABLE이 오류) | ★★★ | 상 | 글 검색 데모(`MATCH … AGAINST`)가 가능해져 발표 임팩트는 크지만 구현이 큽니다. | 검색 데모는 강하지만 역색인과 점수 계산이 큽니다. |
| 대소문자를 구분하는 열 (`COLLATE utf8mb4_bin` · `BINARY`) | 없음 — R77의 `corpus/collate_clause.sql` 69문장 중 **62개가 다름**(열을 만드는 줄부터 `COLLATE`가 거절되어 이어지는 줄이 줄줄이 달라짐). `COLLATE utf8mb4_0900_ai_ci` · `CHARACTER SET utf8mb4`는 받지만(덤프 호환) `COLLATE utf8mb4_bin`은 "not supported" 오류, 열의 `BINARY` 속성과 `BINARY 식` · `식 COLLATE ...`는 구문 오류, `COLLATION()` · `CHARSET()`은 함수 없음, `SHOW COLLATION LIKE ...`은 빈 결과, `information_schema.collations` · `character_sets` 없음 | ★★ | 중~상 | 대소문자를 구분하는 열(해시 · 토큰 비교)을 만들고 `COLLATE`로 비교를 고를 수 있습니다. | 정렬 규칙이 엔진 전체에 기본 규칙(R15) 하나로 박혀 있어, 열마다 고르려면 비교 · 색인 · 고유 검사를 규칙별로 나눠야 합니다. 코퍼스가 이미 있어 차이 0을 목표로 삼을 수 있습니다(`vs_mysql.py ... corpus/collate_clause.sql`). |
| `FLUSH` · `REPAIR` · `CHECKSUM TABLE` | 없음 — R71에서 `OPTIMIZE TABLE`·`CHECK TABLE`은 네이티브 프로토콜에서도 되고(앱 표 메뉴 포함, `CHECK`는 행을 읽음) `FLUSH`·`REPAIR`·`CHECKSUM TABLE`은 어느 쪽에도 없음 | ★ | 하 | `mysqldump`가 보내는 `FLUSH TABLES`를 받고 관리 도구의 표 점검 메뉴가 더 많이 돕니다. | `FLUSH TABLES`는 받기만 하고(체크포인트), `CHECKSUM TABLE`은 행의 해시, `REPAIR`는 `OPTIMIZE`와 같은 길로 보낼 수 있습니다. |
| 표현식(함수) 인덱스 | 없음 (`CREATE INDEX … ((a + 1))`가 문법 오류) | ★★ | 중~상 | `WHERE a + 1 = …` 같은 식 조건도 인덱스를 탈 수 있습니다. | MySQL·PostgreSQL·Oracle 모두 있습니다. 인덱스 키가 식이라 플래너와 유지가 바뀝니다. |
| 열 단위 권한 (`GRANT SELECT (col) ON t`) | 없음 | ★★ | 중 | 열마다 접근을 제한하는 보안 시연이 가능해집니다. | MySQL·PostgreSQL에 있고 Oracle은 INSERT·UPDATE만입니다. |
| 감사 로그 | 없음 | ★★ | 중 | 누가 무엇을 했는지 기록해 보안·운영 설명이 풍부해집니다. | 셋 다 있습니다(PostgreSQL은 확장). 누가 무엇을 했는지 기록합니다. |
| 공간 타입·인덱스, 특수 인덱스(비트맵·BRIN·GIN/GiST) | 없음 | ★ | 상 | 지리 데이터를 다루게 되지만 범위가 커서 이번 마감과는 맞지 않습니다. | 범위가 크고 이번 마감에는 맞지 않습니다. |

## 8. 백엔드 (서버·운영)

접속·배포·운영 쪽.

| 항목 | 현재 상태 | 추천도 | 비용 | 효과 | 근거 |
|---|---|---|---|---|---|
| 코퍼스 비교 CI | 없음 (`.github` 없음) | ★★★ | 상 | 푸시마다 MySQL과의 차이 0이 자동으로 확인되어 호환성 회귀를 바로 잡습니다. | 지금은 사람이 `make_scoreboard.py`를 돌립니다. MSVC 빌드, MySQL 컨테이너(리눅스 러너)에 리눅스 빌드 가능 여부 확인이 필요합니다. |
| TLS | 없음 | ★★ | 상 | 암호화된 접속이 되지만 Windows에서 큰 작업이고 발표에서는 보이지 않습니다. | Windows에서 TLS 라이브러리 도입과 핸드셰이크 확장이 큽니다. |
| `caching_sha2_password` | 없음 (`mysql_native_password`만) | ★★ | 상 | MySQL 8 기본 인증을 요구하는 최신 드라이버가 옵션 없이 접속합니다. 지금 쓰는 CLI·pymysql은 이미 됩니다. | 지금 쓰는 mysql CLI와 pymysql은 모두 접속됩니다. 일부 최신 드라이버가 옵션을 요구할 때만 문제입니다. |
| 성능 동결로 남긴 후보 | 일부 — 2026-10-03 사용자 결정으로 성능 작업을 멈추고("흔한 모양에 이차 시간 절벽이 없고 10만 행 이하 데모에서 1초를 넘는 문장이 없다"), 사용자가 요청할 때만 새 라운드를 엽니다. 그때(10월 측정, 그 뒤 일부는 좋아졌을 수 있음) 남긴 후보: `ANALYZE` · `INSERT … SELECT` · `UNION ALL`(10만 행에서 1.5~1.7초), 쓰는 문장 안의 상관 서브쿼리, `NATURAL`/`USING` 조인, 보조 인덱스가 있는 표의 전체 행 `UPDATE`(버킷 다시 쓰기), 서버 시작 때 표 읽기 병렬화, 10만 행 표가 조인의 왼쪽이면 선택적인 조인도 0.9~1.0초(조인 전에 왼쪽 행을 `std::vector<Row>`로 복사하는 비용으로 추정, 프로파일링은 안 함), 재시작 직후 위치 캐시가 비어 있는 동안 읽기가 인덱스 시작을 못 함, 통계가 없는 표(큰 문장 몇 개로 적재한 표)의 조인 비용 추정은 인덱스를 16번 찔러 본 표본 | ★★ | 중~상 | 큰 표 데모에서 1초 안팎인 문장이 줄어듭니다. | 정리한 `PLAN.md`에서 옮긴 항목입니다. 방침이 "요청할 때만"이라 먼저 묻고 진행합니다. 조인 왼쪽 복사를 없애려면 조인 경로를 포인터 기반으로 바꿔야 해서 범위가 큽니다. |
| WAL 복제, XA, 페이지 단위 버퍼 풀 | 없음 | ★ | 상 | 복제·분산 트랜잭션이 되지만 이미 범위 밖으로 정했습니다. | 이미 범위 밖으로 정했고 남은 기간에는 맞지 않습니다. |

## 9. SQL 문법 추가 — 보류

2026-10-25 결정: 지금은 기능에 집중. 새 문장·절·함수·자료형을 받아들이는 일은 여기로 뺐고, 기능 쪽이 끝나거나 시간이 남으면 다시 꺼냅니다(`KILL`과 `FULLTEXT`처럼 문법이 따라붙지만 중심이 동작인 것은 기능 쪽에 남겼습니다). 지금 받아들이는 문법 전체는 `SQL.md`.

| 항목 | 현재 상태 | 추천도 | 비용 | 효과 | 근거 |
|---|---|---|---|---|---|
| `INSERT INTO t () VALUES ()`, `INSERT INTO t VALUES ()` | 없음 (둘 다 오류) | ★★★ | 하~중 | ORM이나 덤프가 보내는 빈 행 삽입이 오류 없이 됩니다. | 파서와 빈 행 삽입(기본값·AUTO_INCREMENT) 경로입니다. |
| `DEFAULT (식)` | 일부 — `DEFAULT NOW()`, `CURRENT_TIMESTAMP(3)`, 음수·`TRUE` 같은 기본값은 됨, 괄호 식 `DEFAULT (a * 2)`·`DEFAULT (UUID())`은 오류 | ★★★ | 중 | MySQL 8 덤프에 나오는 괄호 식 기본값을 가진 표를 가져올 수 있습니다. | 다른 열을 읽는 기본값 평가가 새로 필요합니다. MySQL 8 덤프에 나옵니다. |
| `GROUPING()` | 없음 — R60에서 `WITH ROLLUP`은 됨, 짝인 `GROUPING(열)`(롤업 행이면 1)은 오류 | ★★★ | 하~중 | `WITH ROLLUP` 결과에서 합계 행을 구분해 보여 주는 보고서 쿼리가 가능해집니다. | MySQL 8의 롤업은 대개 `GROUPING()`과 함께 씁니다. 롤업 행인지 묶음 행에 표시해 두고 함수가 그 표시를 읽으면 됩니다(지금은 함수 인자가 값으로만 넘어와 열 이름을 모름). |
| `SELECT @x := 식` | 없음 (R43에서 일부러 오류로 둠) | ★★★ | 중 | 행마다 변수에 값을 쌓는 옛 MySQL 관용구(순번 매기기 등)가 돌아갑니다. | 행마다 평가하는 순서와 세션 변수 상태가 걸립니다. |
| `SIGNAL` · `RESIGNAL` · `GET DIAGNOSTICS` | 없음 (`SIGNAL`이 든 프로시저는 만들 때 "Unknown statement") | ★★★ | 하~중 | 프로시저에서 오류를 직접 던져 검증 로직을 쓸 수 있습니다. | 루틴에서 오류를 던지는 기본 수단입니다. |
| 라벨 붙은 루프 (`lbl: LOOP … LEAVE lbl`, `ITERATE lbl`, `END LOOP lbl`) | 없음 — 라벨 없는 `LOOP`·`WHILE`·`REPEAT`·`LEAVE`는 됨 | ★★★ | 중 | 중첩 루프를 빠져나오는 프로시저가 그대로 돌아갑니다. | 처음 표에는 "END LOOP 라벨만"으로 적었는데 **라벨 문법 전체**가 없습니다(`lbl:`에서 "Unknown statement"). 중첩 루프를 빠져나오려면 필요합니다. |
| 커서(`DECLARE c CURSOR`, `OPEN`·`FETCH`·`CLOSE`) · `DECLARE … HANDLER` | 없음 | ★★★ | 상 | 행을 하나씩 처리하는 저장 루틴을 쓸 수 있어 루틴의 표현력이 완성됩니다. | 저장 루틴 해석기에 상태가 새로 필요합니다. |
| `CREATE TEMPORARY TABLE` | 없음 | ★★★ | 중~상 | 세션 안에서만 쓰는 작업 표로 복잡한 단계 계산이 쉬워집니다. | 세션별 이름 공간과 연결 종료 시 정리가 필요합니다. |
| 생성 열 `GENERATED ALWAYS AS` / `AS (식)` | 없음 (STORED·VIRTUAL 둘 다 오류) | ★★★ | 상 | 계산 열로 값을 두 번 저장하지 않고 파생값을 제공하며 MySQL 스키마를 그대로 옮깁니다. | 읽기 경로, 인덱스, ALTER까지 걸립니다. |
| `UPDATE … JOIN (SELECT …) d ON …`, `DELETE x FROM t x JOIN (SELECT …) d ON …` | 없음 ("Expected identifier": 갱신 조인에 파생 표가 올 수 없음) | ★★★ | 중 | 집계한 값으로 갱신하는 흔한 MySQL 문장이 돌아갑니다. | 집계한 값으로 갱신하는 흔한 MySQL 문장입니다. 갱신 조인 실행기가 파생 표를 실체화해야 합니다. |
| `CREATE FUNCTION`의 여러 문장 본문 | 없음 — 본문은 `RETURN 식` 하나만 됨. `BEGIN DECLARE a INT; SET a = 5; RETURN a * 2; END`가 구문 오류(프로시저는 되는 문장들) | ★★★ | 중 | 변수 · 분기를 쓰는 저장 함수를 만들 수 있습니다. | 프로시저 본문 실행기를 함수에서 다시 쓰고 `RETURN`으로 빠져나오는 길이 필요합니다. |
| 여러 열 FK | 없음 (오류로 거절: "one column per foreign key"). 한 열에 FK 둘 이상은 R57에서 됨 | ★★ | 중~상 | 복합 기본키를 가리키는 실제 스키마를 옮겨 올 수 있습니다. | 한 열에 여러 키는 이제 키 목록이라 되지만, 키 하나가 여러 열을 가리키는 구조(키 = 열 목록, 복합 값의 존재·연쇄 검사)는 아직입니다. ERD 편집기는 이미 FK가 있는 열을 경고합니다. |
| `NATURAL LEFT JOIN` · `NATURAL RIGHT JOIN` | 없음 (문법 오류: `NATURAL` 뒤에 `JOIN`만 기대) | ★★ | 하 | 드물게 쓰는 이 조인이 오류로 거절되지 않습니다. | R57에서 USING·NATURAL 조인의 열 순서를 맞추며 알았습니다. 파서에 `LEFT`·`RIGHT`를 받고 `RIGHT`면 USING처럼 순서를 바꾸면 됩니다. |
| `UPDATE t u1 JOIN t ON …` (첫 사용에만 별칭, 두 번째는 별칭 없음), `ORDER BY (SELECT …)` 식으로 직접 | 없음 ("Not unique table/alias", "A subquery inside this expression is not supported") | ★★ | 하~중 | MySQL이 받는 자기 조인 갱신과 `ORDER BY (SELECT …)`가 오류 없이 됩니다. | 둘 다 MySQL은 받아들입니다. 조용히 틀리는 것이 아니라 오류로 거절합니다. |
| `UPDATE IGNORE` · `DELETE IGNORE` | 없음 (`INSERT IGNORE`는 있음) | ★★ | 중 | 오류 난 행만 건너뛰고 나머지를 갱신·삭제하는 대량 작업이 가능합니다. | 오류 난 행만 건너뛰는 의미가 새로 필요합니다. |
| `ALTER … ADD/DROP PRIMARY KEY` | 없음 (명시적으로 거절: "declare the PRIMARY KEY in CREATE TABLE") | ★★ | 중 | 표를 만든 뒤에도 기본키를 바꿀 수 있습니다. | 인덱스·참조 재구성이 걸립니다. |
| `DECLARE … DEFAULT 식` | 없음 (`DEFAULT a * 3`이 든 프로시저는 만들 때 오류) | ★★ | 하 | 프로시저 변수의 초기값에 식을 쓸 수 있습니다. | 파서 수준입니다. |
| `EXPLAIN FORMAT=JSON` · `FORMAT=TREE` | 없음 ("Unknown statement: FORMAT") | ★★ | 하~중 | MySQL Workbench 같은 도구가 보내는 `EXPLAIN FORMAT=JSON`이 오류 없이 됩니다. | 처음 표에 없던 항목입니다. MySQL Workbench 같은 도구가 `FORMAT=JSON`을 보냅니다. |
| `USE` · `FORCE` · `IGNORE INDEX` | 없음 (문법 오류). 힌트 주석 `/*+ … */`은 `MAX_EXECUTION_TIME` 말고는 읽고 무시함 | ★★ | 중 | 인덱스 힌트를 쓴 기존 쿼리가 오류 없이 돌고, 플래너를 사람이 이끌 수 있습니다. | 플래너에 힌트를 전달해야 합니다. |
| `JSON_TABLE` | 없음 | ★★ | 상 | JSON 배열을 행으로 펼치는 쿼리가 가능하지만 쓰임새에 비해 큽니다. | 쓰임새에 비해 큽니다. |
| `LOAD DATA` / `INTO OUTFILE` | 없음 (UI의 CSV 가져오기·내보내기는 있음) | ★★ | 중 | 서버 파일을 한 번에 적재·내보내기 하지만 앱의 CSV 기능이 이미 대신합니다. | 서버 파일 접근 보안 문제가 있고 UI가 이미 해결합니다. |
| 없는 함수 묶음 | 일부 — 점검(R75)에서 흔한 MySQL 함수 53개를 진짜 MySQL과 견주니 **39개가 없었고 R77에서 JSON 11개(`JSON_MERGE_PATCH` · `JSON_MERGE` · `JSON_MERGE_PRESERVE` · `JSON_QUOTE` · `JSON_SEARCH` · `JSON_PRETTY` · `JSON_DEPTH` · `JSON_OVERLAPS` · `JSON_CONTAINS_PATH` · `JSON_ARRAY_APPEND` · `JSON_ARRAY_INSERT`)와 `MEMBER OF` 연산자가 생겨 지금은 28개가 없음**: JSON `JSON_STORAGE_SIZE`, `INET_ATON` · `INET_NTOA` · `INET6_ATON` · `INET6_NTOA` · `IS_IPV4` · `IS_IPV6` 6, `UUID_TO_BIN` · `BIN_TO_UUID` · `UUID_SHORT` · `IS_UUID` 4, `AES_ENCRYPT` · `AES_DECRYPT` · `COMPRESS` · `UNCOMPRESS` · `UNCOMPRESSED_LENGTH` · `RANDOM_BYTES` 6, `GET_LOCK` · `RELEASE_LOCK` · `IS_FREE_LOCK` 3, `CONNECTION_ID` · `CHARSET` · `COLLATION` · `COERCIBILITY` 4, `BIT_XOR` · `BENCHMARK` · `LOAD_FILE` 3, `ST_*`(공간), 그 밖에 `INTERVAL()` · `FORMAT_BYTES` · `WEIGHT_STRING`. (`CONVERT_TZ`는 고정 오프셋으로 이미 됨) | ★★ | 하 | 도구와 쿼리가 부르는 함수가 오류 없이 돕니다. `CONNECTION_ID`·`CHARSET`·`COLLATION`은 접속 직후 호환에 도움이 됩니다. | 흔한 MySQL 함수 53개를 진짜 MySQL과 견준 R75 점검의 목록이고, R77에서 JSON 열한 개를 뺐습니다. |
| `PERCENTILE_CONT` · `PERCENTILE_DISC` `… WITHIN GROUP (ORDER BY …)` | 일부 — `MEDIAN`만 **MySQL에도 없는 문법**(호환 목표 밖). | ★★ | 하~중 | 중앙값 말고도 백분위 통계를 쿼리 한 줄로 냅니다. | PostgreSQL·Oracle에 있습니다(MySQL에는 없음). |
| `CREATE SEQUENCE` · `NEXT VALUE FOR` | 없음 (`AUTO_INCREMENT`만) **MySQL에도 없는 문법**(MariaDB·PostgreSQL 것, 호환 목표 밖). | ★★ | 중 | PostgreSQL·Oracle 방식의 시퀀스를 쓸 수 있습니다(지금은 `AUTO_INCREMENT`만). | PostgreSQL·Oracle에 있습니다. |
| `CREATE EVENT` | 없음 | ★ | 중~상 | 주기 작업을 예약할 수 있지만 쓰임새가 작습니다. | 스케줄러가 필요한데 쓰임새가 작습니다. |
| 온라인 DDL 문법 (`ALTER … ALGORITHM=INPLACE, LOCK=NONE`) | 없음 (문법 오류) | ★ | 하(주의) | `ALGORITHM=INPLACE` 같은 DDL이 오류 없이 되지만, 락을 그대로 쓰면 조용한 차이가 되므로 주의가 필요합니다. | 셋 다 있습니다. 받기만 하고 락을 그대로 쓰면 조용한 차이가 되므로 신중히. |
| `GROUPS` 윈도 프레임 | 없음 **MySQL에도 없는 문법**(오류 1235, 호환 목표 밖). | ★ | 중 | PostgreSQL 전용 윈도 프레임을 쓸 수 있지만 효과는 작습니다. | PostgreSQL에만 있습니다. |
| 다중 표 `DELETE`의 `JOIN … USING (열)`·`FULL OUTER JOIN`·`CROSS JOIN … ON` | 없음 ("Expected ON", "Unexpected token(s) after end of statement": 파서가 `ON`이 있는 `JOIN`·`LEFT JOIN`·`RIGHT JOIN`만 받음) | ★ | 하 | MySQL이 받는 다중 표 `DELETE`의 조인 모양이 오류로 거절되지 않습니다. | R67에서 `KILL`의 확인점을 넣다 알게 됨. `executor_multi.cpp`의 `USING`·`Cross/Natural/FullOuter` 분기는 이 문장에서 닿지 않는 코드입니다. |
| `CREATE TYPE`, `DEFERRABLE` 제약, 부분 인덱스, 행 레벨 보안(`CREATE POLICY`) | 없음 **MySQL에도 없는 문법**(호환 목표 밖). | ★ | 상 | PostgreSQL·Oracle의 고급 기능이 생기지만 범위가 커서 이번 마감과는 맞지 않습니다. | PostgreSQL·Oracle에 있고 범위가 큽니다. |
| 새 자료형 `BINARY` · `VARBINARY` · `BIT` · `SERIAL` · `ZEROFILL` | 없음 ("Expected data type") | ★ | 중 | 이 자료형이 든 덤프의 표를 가져올 수 있습니다. | 저장과 비교 규칙이 새로 필요합니다(`BINARY`는 정렬 규칙 항목과 맞물림). |
| 작은 문법 틈 | 없음 — 산술 안의 `!`(`1 + !0`), `AS` 없는 별칭이 키워드인 것(`FROM t date`), `GROUP BY` 식과 집계 인자 안의 서브쿼리(`SUM((SELECT 1))`는 "A subquery inside this expression is not supported") | ★ | 하~중 | MySQL이 받는 드문 모양이 오류 없이 돕니다. | 모두 작은 파서 변경이지만 서브쿼리 쪽은 텍스트로 보관되는 자리라 바인더도 손봐야 합니다. |

## 참고: 점검에서 바로잡은 것 (처음 표와 달라진 곳)

- **더 나쁨:** (`DEMO.md`의 낡은 목록은 R48에서 고침) `RANGE … n PRECEDING`은 "일부"가 아니라 **조용한 오답**이고, `DATE(x) + 0`도 조용한 오답입니다. 라벨 붙은 루프는 "END LOOP 라벨"만이 아니라 **라벨 문법 전체**가 없습니다. 연결 수 상한은 "무시"가 아니라 **변수 자체가 없습니다.** `DEMO.md`의 "피할 것" 목록이 낡았습니다.
- **더 좋음:** `UNIQUE/FK 검사 O(n)`은 INSERT 쪽이 이미 O(1)입니다(부모 DELETE만 남음). `DEFAULT (식)`은 `NOW()`류가 이미 됩니다. 크래시 복구 시험은 Server Manager의 Stop으로 이미 가능합니다. `list_locks`·`get_table_stats`·`trace_query`는 `execute_sql`로 이미 부를 수 있습니다.
- **R51에서 알게 된 것:** 앱에서 `EXPLAIN`·`EXPLAIN ANALYZE`의 결과는 한 열짜리 표로 도착해서, 코드에 있던 "계획 트리" 화면이 **한 번도 뜨지 않고 있었습니다**(R51에서 표를 계획 화면으로 그리게 고쳤고 단계 막대도 그 화면에 붙였습니다).
- **R56에서 알게 된 것:** `a ← b ← c` 연쇄(DELETE와 키 UPDATE 모두)와 0행 SELECT의 열 이름을 끝냈고(`DATE.md` R56), 그 과정에서 `vs_mysql.py`가 빈 결과의 열 이름도 비교하게 되어 `SHOW TABLES LIKE`의 열 이름과 `EXECUTE`한 SELECT의 빈 결과 문제도 찾아 고쳤습니다.
- **R57에서 알게 된 것:** 점검의 "조용한 오답" 여섯 개를 끝냈고(`DATE.md` R57), 그 비교에서 **`UNION`의 왼쪽이 0행이면 오른쪽 값이 사라지는 오답**(R53 빌드에도 있었음)과 **`a RIGHT JOIN b USING (c)`의 `*` 열 순서**가 MySQL과 달랐던 것을 함께 고쳤습니다. 10-26의 "다중 표 DELETE·MERGE·REPLACE의 사슬 아래 검사"는 DELETE·`DELETE … JOIN`·MERGE-DELETE가 이미 맞았고(한 문장으로 모아 도는 DELETE가 R56의 모의를 씀) REPLACE와 UPDATE+DELETE가 든 MERGE만 어긋나 있었습니다. 같은 점검의 "OPTIMIZE·CHECK TABLE 없음"은 MySQL 프로토콜 쪽에서는 이미 됩니다(위 표).
- **R58에서 알게 된 것:** 점검의 일곱 항목을 끝냈고(`DATE.md` R58), 그 비교로 목록에 없던 조용한 오답 둘을 더 찾아 고쳤습니다: **`RENAME TABLE` 뒤에 `information_schema`가 옛 표 이름을 말하는 것**(카탈로그의 표 이름이 갱신되지 않았음)과 **부모 표의 이름을 바꾸면 자식의 외래키가 옛 이름을 가리킨 채 남는 것**(INSERT가 "Referenced table not found"로 실패). `USING (y, x)`처럼 열을 거꾸로 적으면 합쳐진 열이 적은 순서로 나오던 것(MySQL은 왼쪽 표의 순서), `DROP TABLE 없는표`의 오류 번호(1051, 한 표일 때만 1146이던 것)도 함께 맞췄습니다. 새 코퍼스는 모두 진짜 MySQL과 차이 0입니다.
- **R59에서 알게 된 것:** 아홉 항목을 끝냈고(`DATE.md` R59), 화면을 실제로 눌러 보다 세 가지를 더 찾았습니다. ① **집계만 있는 한 행짜리 답의 `FOUND_ROWS()`가 0**(R37부터; 표 끝의 "1 row(s) returned." 줄이 없어서 세던 방식이 놓침 — 고침, 진짜 MySQL과 비교). ② 앱의 연결은 `SHOW CREATE TABLE`을 행이 아니라 글자로 돌려줘서 스키마 비교가 한쪽에만 있는 표의 `CREATE TABLE`을 못 읽던 것(고침). ③ 교차 조인이 메모리 6 GB까지 오르고 `KILL`도 안 듣는 것(위 표, 고치지 않음). 진짜 MySQL과 비교한 `long_query_time`은 범위 밖의 수를 오류가 아니라 0 ~ 31536000으로 맞추는데 처음에는 오류로 거절했습니다(고침). 그리고 전체 시험의 `idx0` 한 번에서 외래키 경합 시험이 한 번 실패했습니다(위 표 첫 줄, 원인 미확인).
- **R60에서 알게 된 것:** 점수판의 남은 8문장을 모두 맞췄고(`DATE.md` R60), 그 비교로 목록에 없던 것들을 찾았습니다: ① 여덟 번째 차이(`VERSION()`)에 가려져 있던 **변수 열의 타입**(`SELECT @@autocommit`이 정수 열), ② `WITH ROLLUP`은 **기본키가 묶음에 있어도 다른 열이 따라온다고 보지 않고**(MySQL이 `ONLY_FULL_GROUP_BY` 오류 1055) 롤업 행의 `ANY_VALUE`는 NULL이 아니라 **그 행들의 마지막 행** 값이라는 것(실제 MySQL로 세 번 재서 정함), ③ 화면의 "알려진 차이" 목록이 낡아서 **이미 같아진 다섯 가지**(같은 행의 `REPLACE`, DATE의 `SUM`, `f = 0.1`, 갱신하는 표를 읽는 하위 질의, UNIQUE 밀어 옮기기)를 아직 "다르다"고 적고 있었던 것(진짜 MySQL과 다시 재서 지움, 지금 남은 여덟 가지로 다시 씀), ④ `information_schema.tables`에 표 크기를 넣으면 행을 모두 읽어서 5배 느려지는 것(앱의 사이드바가 이 표로 목록을 만듦: 표본으로 바꿔 0.18 → 0.28 ms). **정직하게 읽을 것:** `SHOW ENGINES`는 MySQL과 똑같이 MyISAM·MEMORY를 `YES`, InnoDB의 `XA`를 `YES`로 보이지만 RuSQL은 `ENGINE=`을 받아서 무시하고 XA 문장이 없습니다(사용자 결정). `SHOW TABLE STATUS`의 시계 열 두 개는 서버마다 달라 코퍼스에서 뺐습니다.
- **R63에서 알게 된 것:** 외래키 경합(R59)의 원인은 **부모 DELETE의 계획이 부모 표를 잠그지 않고 읽던 것**이었습니다(`try_cascade_once`가 자식 표의 데이터 락만 잡음): 다른 세션이 부모 표에 행을 더하거나 한 행을 지워 뒤의 행이 당겨지는 순간 계획이 지울 행을 못 찾으면 `kNoPosition`으로 그 부모를 건너뛰어 자식을 확인하지 않았고, 이어지는 DELETE가 방금 자식이 옮겨 온 부모를 지웠습니다(두 번 모두 `UPDATE mov SET pid = c` 직후의 `DELETE FROM par WHERE id = c`). 한 스레드로는 3,000번을 해도 안 나오고, 부모 표를 바꾸는 세션 넷을 곁들이면 2~3초 안에 나옵니다(새 시험). R59에서 의심했던 "이 빌드 이전부터 있었는지"는 이번에 따로 가리지 않았습니다. 교차 조인은 **KILL이 안 들은 것을 재현하지 못했고**(세 표 300행에서 45~450 ms 만에 멈춤) 진짜 문제는 메모리였습니다(1초에 약 350 MB씩, 약 17초에 6 GB; 행 쌍 하나가 약 1.8 KB). `DEMO.md`의 "지원하지 않습니다" 목록에 `KILL`이 아직 남아 있던 낡은 줄은 R64에서 고쳤습니다.
- **R64에서 알게 된 것:** `KILL`이 안 듣는 단계를 한도를 바꿔 가며(100~2,500 ms) 재 보니 `UNION ALL`은 전체 2.2초 가운데 400 ms 뒤로는 끝까지 아무도 시계를 보지 않았습니다(결과 글자를 행으로 되읽는 `parse_table_output`에 확인점이 없었음 — CTE·파생 표·`INSERT … SELECT`가 같이 쓰는 함수). 그리고 **`CALL` 본문의 SELECT는 `KILL QUERY` 뒤에도 11초를 돌았고**(교차 조인을 부르는 프로시저), 그동안 `CALL`이 데이터베이스의 구조 쓰기 락을 쥐고 있어 다른 모든 세션이 기다렸습니다(고침: 본문의 SELECT와 문장 사이마다 확인). redo 쪽은 `CALL w_ins(1)` 뒤 로그가 401 → 194바이트로 비워지는 것을 보고 시작했고, 같은 재기로 **트리거가 걸린 표의 INSERT도 로그를 비운다**(1,351 → 119바이트)는 것을 찾았습니다(위 표). 병렬 단계의 작업자는 예외를 던질 수 없어서(풀 스레드 밖으로 나간 예외는 프로세스를 끝냄) 문장의 스레드가 `Watch`를 건네고 작업자가 보다가 멈추면 돌아온 문장의 스레드가 던집니다. 정직하게 적을 것: 처음 돌린 심은 버그에서 살아남은 여덟 가운데 여섯은 **시험 이름의 쉼표 때문에 Catch 필터가 아무 시험도 고르지 못한 것**이었고(그런 변이는 "살아남은 것"으로 보임), 필터를 고쳐 다시 돌렸습니다. 진짜로 살아남은 것은 시험을 보강하거나(`VACUUM` 표식, UNION의 한 스레드 정렬), 영향이 없는 코드를 거뒀습니다(집계·그룹 작업자의 확인, 뷰/파티션 검사). `DEMO.md`의 "알려진 한계" 가운데 값 기준 `RANGE` 프레임·`DATE + 0`과 DATE 열의 `SUM`·`AVG`·갱신하는 같은 표를 읽는 `UPDATE`는 이미 MySQL과 같아서 지웠습니다(직접 돌려 확인).
- **R65에서 알게 된 것:** 트리거가 걸린 표·뷰를 거친 쓰기와 MERGE·다중 표 문장이 redo 로그를 비우던 것을 끝냈고(`DATE.md` R65), 그 시험을 쓰다 목록에 없던 것을 찾았습니다. ① **트리거 안 `CALL p(NEW.id)`가 프로시저에 NULL을 넘김**(오류 없이, 위 표 — 처음에는 시험의 프로시저가 기본키 표에 쓰다 실패해서 드러났고, 기본키가 없는 표로 다시 해 보니 조용히 NULL이 들어갔습니다). ② 전체 시험에서 R63의 `GROUP BY` 중단 시험이 세 모드 모두 실패했는데, R64 소스에서 새로 빌드한 시험도 같은 자리에서 실패해 R65의 회귀가 아님을 가렸습니다(이 기계의 지금 속도에서는 한도 지점이 그룹 행을 만드는 병렬 단계에 떨어짐; 위 표). ③ 크래시 퍼저 6종은 트리거·뷰·MERGE를 쓰지 않아서(77라운드 불일치 0은 이 세 모양을 보지 않음) 이 모양들의 크래시 복구는 `test_redo.cpp`의 `[r65]` 시험 6개가 보증합니다 — 퍼저에 더하는 것이 다음 점검 후보입니다. ④ 엔진 API(`execute_sql`)는 문장 끝의 `;`을 받지 않습니다(앱은 문장을 잘라 보내고, `SUGGEST INDEX`는 끝의 `;`을 떼고 받습니다). ⑤ 다중 표 `UPDATE`·`DELETE`와 `MERGE`는 200행짜리 표에서도 문장 하나가 30~65 ms 걸립니다(R65의 로그 변경과 무관한 기존 속도; 빨라진 것은 매번 다른 표의 로그를 비우던 쪽).
- **R66에서 알게 된 것:** R65에서 올린 "트리거 안 `CALL p(NEW.id)`가 NULL을 넘김"은 한 줄이 아니라 **뿌리가 같은 넷**이었습니다(`DATE.md` R66): `NEW`/`OLD`가 키워드 토큰이라 `CALL`의 인자가 식으로 저장되는데 트리거의 값 끼워 넣기는 식 인자를 건너뛰고 `CALL`은 호출자의 변수만 봐서 NULL, `IF NEW.qty > 15`는 늘 거짓이라 `ELSE` 가지가 타면서 그 안의 `NEW.id`가 글자 그대로 들어감, `WHILE`은 한 번도 안 돎, `SELECT NEW.id INTO @x`는 글자 `NEW.id`를 넣음(셋 다 오류 없이). 고치고 나니 **`IF NEW.qty < 0 THEN SET NEW.qty = 0`이 값을 못 바꾸는 것**이 드러났습니다(전에는 `IF`가 늘 거짓이라 보이지 않던 것). 마지막으로 `BEFORE UPDATE`의 `SET NEW.x`는 오류로 거절됩니다(위 표; 조용한 오답은 아님). 진짜 MySQL과 비교하는 코퍼스에 이 모양들을 더했습니다.
- **R67에서 알게 된 것:** "아직 `KILL`이 안 듣는 문장 종류"를 코드로 읽는 대신 `KILL QUERY`를 실제로 보내 재 보니(두 세션, 30만 행 표) `EXPLAIN ANALYZE`·`EXECUTE`·`CREATE TABLE … AS SELECT`·`MERGE`·다중 표 `UPDATE`/`DELETE`·`CREATE INDEX`가 모두 끝까지 돌았고(1.2~3.7초) 원인은 하나였습니다 — 읽기 확인점이 첫 단어가 SELECT·WITH일 때만 켜진다(`DATE.md` R67). 그리고 **`GROUP BY`의 그룹 행 단계가 그룹마다 작업 하나**(30만 그룹이면 작업 30만 개를 줄 세우는 데 문장이 몇백 ms)라는 것을 R63 시험의 실패에서 따라가 찾았고, 덩어리로 나누자 같은 질의가 1.8배 빨라졌습니다. 그 과정에서 ① **`MERGE`가 이차 시간**(대상 300행 × 원본 30만 행에 109초 — 위 표), ② 다중 표 `DELETE`가 `USING`·`FULL OUTER JOIN`·`CROSS JOIN … ON`을 문법 오류로 거절한다(위 표), ③ `SELECT CONNECTION_ID()`가 없고(세션 번호는 `SHOW PROCESSLIST`로 안다 — 없는 함수 묶음에 들어 있음) `SHOW INDEX`가 `USING HASH` 인덱스를 나열하지 않는다는 것도 알았습니다. 정직하게 적을 것: 심은 버그 19종 가운데 처음에 5종이 살아남았고 그중 넷(반복문의 `tick()`)은 `eval_condexpr`가 이미 묻는 곳이라 코드를 거뒀으며, 하나(그룹 작업자의 확인)는 한도 지점이 그 단계 밖에 떨어지는 시험이어서 시험을 보강해 잡았습니다.
- **R68·R69에서 알게 된 것:** R67에서 찾은 "`MERGE`·다중 표 문장이 이차 시간"을 끝냈습니다(`DATE.md` R68: `ON`의 같음으로 해시해 300 × 30만 행 109초 → 0.85초, 후보 쌍은 전체 `ON`으로 다시 검사하므로 답은 그대로). 그리고 남은 둘째 항목(`ALTER`의 중단)을 코드로 읽다 **무결성 구멍**을 찾았습니다: `ALTER TABLE … ADD FOREIGN KEY`·`ADD CHECK`가 고아 행이나 어기는 행이 이미 있어도 `OK`이고 그 행이 표에 남았습니다(MySQL은 1452·3819로 거절; 고침 R69, 점수판에 진짜 MySQL과 같은 여섯 문장). 다른 `ALTER`·인덱스(`MODIFY … NOT NULL`·`CREATE UNIQUE INDEX`·`ADD UNIQUE`·`MODIFY … TINYINT`·`MODIFY … VARCHAR(2)`)는 이미 기존 행을 검사해 거절합니다. 그리고 **`ALTER`가 표의 행을 제자리에서 바꾼다**는 것(위 표)과, 자동 커밋의 `DELETE`는 행을 바로 지우고 `UPDATE`는 옛 판을 남긴다는 것을 알았습니다. 정직하게 적을 것: 심은 버그 15종 중 둘(글자 키를 대조 규칙 대신 숫자 정규화로 해시)은 정답에 영향이 없어 살아남았고(`normalize_numeric_key`가 숫자가 아닌 글자에는 이미 대조 규칙을 씀) 그 코드를 거뒀습니다. `ADD COLUMN`의 `CHECK`·`NOT NULL`이 기존 행을 어떻게 다루는지는 보지 않았습니다.
- **화면 모양 (11-03):** 사용자가 "MVCC·B+Tree 두 패널만 빼고 원래 디자인을 원했다"고 했습니다(바꾼 서버 원통색은 제외). R65에서 홈 화면과 서버 매니저에 입힌 `PageKit` 모양(`ScreenLook.css`)을 걷어냈고(`DATE.md` 11-03) 이 목록에서 "같은 디자인을 다른 화면에도" 항목도 지웠습니다. ERD·결과 패널·Journey에 새 모양을 입히자는 제안은 더 하지 않습니다.
- **새 항목:** `EXPLAIN FORMAT=JSON/TREE`, `SIGNAL` 묶음(`RESIGNAL`·`GET DIAGNOSTICS`), `UPDATE/DELETE IGNORE`의 `DELETE`, 없는 함수 45개의 구체 목록, `VERSION()` 문자열.

## 추천 순서 (마감까지 약 4주)

1. **발표·제출 — 발표 PC에서 앱이 뜨게 만들기 (중):** 설치본에 엔진 묶기와 새 PC 점검. 고장 나면 나머지가 소용없는 항목입니다.
2. **안정성·무결성 (상, 급하지 않음):** 외래키 경합과 교차 조인 메모리는 R63에서, 윈도·`UNION`·병렬 단계·`INSERT … SELECT`·`CALL`의 중단과 쓰는 `CALL`·`VACUUM`의 redo는 R64에서, 트리거가 걸린 표·뷰·MERGE·다중 표 문장의 redo는 R65에서, 트리거 본문이 `NEW`/`OLD`를 못 읽던 오답은 R66에서, `EXPLAIN ANALYZE`·`EXECUTE`·`CREATE TABLE … AS SELECT`·`MERGE`·다중 표 문장·`CREATE INDEX`의 중단과 `GROUP BY`의 그룹 행 단계는 R67에서, `MERGE`·다중 표 문장의 이차 시간과 `ADD FOREIGN KEY`·`ADD CHECK`가 기존 행을 안 보던 무결성 구멍은 R68·R69에서 끝났습니다. 남은 것은 `ALTER` 같은 DDL의 중단 하나이고, 표를 제자리에서 바꾸는 구조라 큰 일입니다 — 마감 전에 필요하지 않습니다.
3. **프론트·MCP — 보이는 장면 (하~중):** `slow_queries` 도구와 시연 대본(느린 쿼리 로그 → `suggest_indexes` 전·후 → `run_script`로 만들기), 앱 화면의 자동 시험(CDP 스크립트를 저장소에), (화면 모양은 사용자 결정으로 B+Tree·MVCC 페이지만 새 모양이고 나머지 화면은 원래 디자인을 지킵니다 — 다른 화면에 새 모양을 입히는 일은 하지 않습니다) 세션 패널·계획 그림·차트·스키마 비교·ERD 드래그는 R59에서 끝났습니다.
4. **작은 것 모음 (하):** `EXPLAIN UPDATE`·`DELETE`, 연결 수 상한, 프로토콜 오류 번호의 남은 약 20개, 몇 가지 `SHOW`, MCP의 `kill_session`·resources·prompts·CSV 도구.
5. **품질 점검 — 마감 1주 전:** 누적 점검 한 바퀴(Debug 전체 + `KILL`을 섞은 크래시 시험 + 빌드 간 차분), 새 영역 훑기 한 번 더.
6. **시간이 되면:** 최적화 전후 속도 애니메이션, 코퍼스 비교 CI, RuSQL vs MySQL 분할 화면, 스키마 비교에 뷰·트리거.
7. **미루거나 하지 않는 것:** `FULLTEXT`, 표현식 인덱스, 열 단위 권한, 감사 로그, TLS, `caching_sha2_password`, WAL 복제·XA, 테마, 그리고 SQL 문법 추가 전부("SQL 문법 추가 — 보류").
8. **직접 하실 일:** MCP 시연 리허설(`DEMO.md` 8번), 크래시 복구 장면 리허설(`DEMO.md` 3번), 화면 녹화, 앱 화면 사람 눈 점검.

## 참고: R77에서 끝낸 것 (2026-11-09, 사용자가 고른 "새 영역 훑기: 권한·역할, COLLATE 절, COM_STMT_*, JSON 함수")

- **JSON 함수**: 245문장의 차이 50 → 0(오류 번호까지). 문서를 쓰는 모양 · 경로(`last` · 범위 · 와일드카드 · 따옴표 키) · 새 함수 11개와 `MEMBER OF` · `JSON_ARRAYAGG` · `JSON_OBJECTAGG` · `JSON_VALUE ... RETURNING` · JSON끼리의 비교와 정렬. 자세한 것은 `DATE.md` R77.
- **준비된 문장의 바이너리 프로토콜**: `COM_STMT_EXECUTE`가 텍스트 프로토콜의 행으로 답해 바이너리 프로토콜 드라이버가 **어떤 `SELECT`도 읽지 못하던 것**을 고쳤습니다(81번 실행, 차이 0). 매개변수 쪽 오류 넷(DATE · TIME · 역슬래시 · 큰 DOUBLE)도 함께.
- **훑었지만 고치지 않은 둘**: 권한 · 역할(**강제되지 않음** — 위 7번 표), `COLLATE` 절(통째로 없음 — 위 7번 표). 두 코퍼스는 `code/test/diff/corpus/`에 있고 점수판 밖입니다.

## 참고: R76에서 끝낸 것 (2026-11-08, 사용자가 "PLAN.md는 이제 필요 없다"고 정함)

- **`docs/mds/PLAN.md`를 지웠습니다.** 7월~10월의 감사 · 로드맵 기록이었고 ✅ 83행이 끝난 일, 열린 일은 6행이었습니다. 앞으로의 일은 이 문서와 대화로 정하고 한 일은 `DATE.md`에 있습니다. 옛 내용은 `git show f58c052:docs/mds/PLAN.md`로 볼 수 있습니다.
- **PLAN.md에만 있던 열린 항목을 옮겼습니다.** 열린 6행(버퍼 풀 · TLS · 표현식/부분/내림차순 인덱스 · `CREATE SEQUENCE` · 열 권한/이벤트 스케줄러 · 코드-문서 불일치 정리)은 이 문서에 이미 있거나(내림차순 인덱스만 새 행에 넣음), MySQL에도 없거나(`CREATE SEQUENCE`), R75 · R76의 정리로 갈음했습니다. 뒤쪽의 "사용자 결정 대기" 블록 8개에서는 아직 열린 것으로 적힌 약 50개를 **진짜 MySQL 8.4.9와 같은 SQL로 돌려** 지금도 남은 것만 가렸습니다. 이미 같아진 것(AFTER 트리거 실패 되돌리기 · 윈도 `OVER (ORDER BY 식)` · 괄호 친 `UNION` · `ORDER BY (1)` · `AS` 없는 윈도 별칭 · `RANGE … INTERVAL` 프레임 · 표 옵션 · `ON DUPLICATE KEY UPDATE`의 영향 행 수 · `KEY` 줄이 만드는 색인 · `IF()` 결과의 글자 비교 …)은 버렸고, 남은 것은 위 6 · 7 · 9번 표에 새 행 13개로 들어갔습니다. 성능 동결 때 남긴 후보 목록(10월 3일 블록)은 8번 표의 새 행 하나로 들어갔습니다.
- **소스 주석 29개 파일 약 45곳과 문서 참조(`SUMMARY.md` · `AI.md` · `FUNCTIONS.md`)에서 "PLAN.md"를 없앴습니다.** 주석은 고친 내용을 스스로 설명하는 글로만 남겼고 동작은 바뀌지 않았습니다. `DATE.md`의 옛 항목은 그날의 기록이라 그대로 둡니다.
## 참고: R75에서 끝낸 것 (2026-11-08, 사용자가 고른 "총 점검의 1·2·3번")

- **총 점검**: 남은 73개 항목을 진짜 MySQL과 같은 SQL로 견주었다. 문서가 낡은 곳 둘 — 뷰 `WITH CHECK OPTION`(R71부터 됨)과 `CONVERT_TZ`(됨) — 을 바로잡았고, 없는 함수는 45개가 아니라 39개, MySQL에도 없는 문법 넷(`PERCENTILE_CONT` · `CREATE SEQUENCE` · `GROUPS` 프레임 · `CREATE TYPE`)은 호환 목표 밖으로 표시했다.
- **`JSON_EXTRACT`의 배열 경로 버그**: `JSON_EXTRACT('[1,2]', '$[1]')`이 NULL을 주던 것(MySQL은 `2`)을 고쳤다. 최상위 배열의 `$[n]`, 첨자 뒤의 첨자(`$[1][0]`), 첨자 뒤의 키(`$[1].a`), 배열이 아닌 값의 `$[0]`(그 값 자체), 그리고 안쪽 객체의 같은 이름 키를 먼저 집던 것(`{"x":{"a":1},"a":2}`의 `$.a`가 1이던 것)이다.
- **정리**: 쓰이지 않던 npm 의존성 `@tauri-apps/plugin-opener`, 템플릿 잔재 `src/assets/react.svg` · `public/tauri.svg`, 죽은 상수 `MCP_TOOL_NAMES`, Python 시험 도구의 쓰이지 않는 `import` 3개와 도우미 함수 3개를 지웠다.
- **점검했지만 지우지 않은 것**(판단이 필요함): `assets/`의 아이콘 이미지 3개, `docs/html/index.html`, 낡은 `docs/mds/PLAN.md`(→ 같은 날 뒤에 지움, 아래 "R76"), 저장소에 올라간 `.claude/settings.local.json`, `legacy` 브랜치, `git stash` 한 건, 빈 `src-tauri/src/lib.rs`, 파일 안에서만 쓰이는데 `export`된 28개 선언.

## 참고: R74에서 끝낸 것 (2026-11-08, 사용자가 고른 "UPDATE·DELETE 안의 서브쿼리, INSERT … SELECT의 잠금")

- **일반 SELECT가 아닌 문장은 서브쿼리를 최신 커밋으로 읽음**(REPEATABLE READ · SERIALIZABLE, 진짜 MySQL을 먼저 재서 규칙을 찾음): `UPDATE` · `DELETE` · `INSERT`/`REPLACE … VALUES` · `ON DUPLICATE KEY UPDATE` · `SET @x = (SELECT …)` · 프로시저의 `SET`. `SELECT`(`SELECT … INTO` 포함)는 스냅샷. 트리거 · 프로시저 본문은 문장마다 자기 종류대로.
- **`INSERT … SELECT` 원본의 공유 잠금**: 읽은 행의 쓰기와 읽은 범위로의 삽입이 기다림(REPEATABLE READ · SERIALIZABLE의 명시적 트랜잭션).
- **잠금을 기다린 `FOR UPDATE`/`FOR SHARE`/`INSERT … SELECT`는 다시 읽음** — 기다리기 전의 옛 값을 돌려주던 오답 수정.
- **정상 종료한 MySQL 연결의 열린 트랜잭션이 되돌려지지 않던 것**(`COM_QUIT`) 수정 — 잠금과 커밋 안 된 행이 서버가 꺼질 때까지 남았다.
- **두 세션 시험** 38개 시나리오 596단계, 차이 0, 알려진 차이 0.

## 참고: R73에서 끝낸 것 (2026-11-08, 사용자가 고른 "스냅샷 시점 맞추기")

- **REPEATABLE READ · SERIALIZABLE의 스냅샷을 `BEGIN`이 아니라 트랜잭션의 첫 일반 읽기에 뜸**(MySQL과 같음): 쓰기 · 잠금 읽기 · 표 없는 `SELECT`는 안 뜨고, 뜨면 모든 표에 적용, 다음 트랜잭션 · `COMMIT AND CHAIN`은 새로 뜸. `START TRANSACTION WITH CONSISTENT SNAPSHOT`은 즉시.
- **`INSERT … SELECT` 원본**이 REPEATABLE READ에서 스냅샷의 옛 값을 복사하던 것을 최신 커밋 값으로(두 세션 시험이 찾음).
- **두 세션 시험** 28개 시나리오 442단계 차이 0. 알려진 차이 하나(`UPDATE`/`DELETE` 안 서브쿼리)는 R74에서 고쳤다.

## 참고: R72에서 끝낸 것 (2026-11-08, 사용자가 고른 두 걸음)

- **REPEATABLE READ · SERIALIZABLE에서 자기가 쓴 행이 두 번 보이던 것**: 다른 세션이 스냅샷 뒤에 바꾼 행을 `UPDATE`/`DELETE`/외래키 연쇄로 쓰면 옛 버전이 새 버전과 나란히 보이던 것을 한 줄로(`_xsup` 표시), `ROLLBACK TO SAVEPOINT`도 맞게.
- **`SELECT … FOR UPDATE | FOR SHARE`가 REPEATABLE READ에서 낡은 값을 돌려주던 것**을 최신 커밋 값으로.
- **두 세션 시험을 진짜 MySQL과 비교하는 도구**(`code/test/diff/two_sessions.py`)와 MySQL 없이도 도는 `test_repeatable_read.cpp`.

## 참고: R71에서 끝낸 것 (2026-11-08, 사용자가 고른 다섯 가지)

- **새 영역 훑기**: 코퍼스 다섯 개(`transactions` · `triggers_writes` · `views_update` · `fk_actions` · `dates_arith`, 문장 363개)를 진짜 MySQL 8.4.9와 비교해 처음 차이 124개를 모두 0으로 — DDL의 암묵 커밋, `READ ONLY`(1792), `COMMIT AND CHAIN`, 저장점 1305, 트리거가 실패하면 문장째 되돌림, 참조되는 표의 `TRUNCATE` 1701, 외래키 정의 검사 1824 · 3780 · 1830, `FOREIGN_KEY_CHECKS = 0`이면 부모 쪽도 꺼짐, 열 이름을 바꾼 뷰 · 뷰 위의 뷰 · `WITH CHECK OPTION`에 쓰기, `TIME` 한계, `STR_TO_DATE %j`, `CONVERT_TZ`, 엄격한 날짜 글자, DATE와 날짜 글자 · DATETIME의 비교.
- **`SELECT @@transaction_isolation` 오답**: 변수가 `REPEATABLE-READ`로 박혀 있던 것을 세션의 실제 수준으로(어느 형태로 정해도), `@@transaction_read_only`도.
- **`BEFORE UPDATE` 트리거의 `SET NEW.x`**, 한 `SET`에 여러 대입.
- **`OPTIMIZE TABLE` · `CHECK TABLE`**: 네이티브 프로토콜에서 엔진이 직접(`CHECK`는 행을 읽음), 앱 표 메뉴에 "Check Table" · "Optimize Table".
- **`suggest_indexes` 후속**: 범위 조건이 둘이면 값 종류가 많은 열, ASC/DESC가 섞인 정렬 노트.

## 참고: R70에서 끝낸 것 (2026-11-07, 사용자가 고른 일곱 가지)

- **앱 AI MCP 패널**이 서버에 없는 도구 이름 일곱 개를 적고 있던 것을 고침: 도구 37개를 세 묶음으로 한 목록(`lib/mcpTools.ts`)에서 그리고, `test_libs.mjs`가 `mcp_server.py`의 `@mcp.tool()`과 이름이 같은지 비교함.
- **시간에 민감한 시험 2건**: 바쁜 컴퓨터에서만 실패하던 `test_concurrency.cpp`(전체 시간 < 380ms)와 `test_statement_stop.cpp`(GROUP BY 중단)를 바쁨에 흔들리지 않게 고침(논리 확인 · 아무 말도 못 한 측정은 다시 잼).
- **앱 화면 자동 시험**(CDP): `code/test/frontend/cdp/test_screens.py`, 11개 화면(홈 · 편집기 · 결과와 차트 · EXPLAIN의 계획 그림과 인덱스 카드(SELECT · UPDATE · DELETE · 하위 질의) · ERD(카드 끌기) · 세션 · 지표 · AI MCP 패널 · 스키마 비교).
- **인덱스 제안**: 엔진의 `SUGGEST INDEX`가 하위 질의 · CTE · UNION · OR을 읽음, 앱 카드가 UPDATE · DELETE에도 뜸, MCP `suggest_indexes`가 엔진을 부름(파이썬의 문장 글자 읽기를 지움).
- **MCP**: `slow_queries` · `kill_session` · `import_csv` · `export_csv` · `diff_schema`, resources 2개, prompts 3개; `explain_query`가 쓰기 문장을 실행하지 않음.
- **`EXPLAIN UPDATE` · `DELETE`**와 **`max_connections`**(오류 1040).
