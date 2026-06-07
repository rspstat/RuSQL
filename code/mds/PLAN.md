# RustDB 개발 계획 (v2.2.0)

---

## ✅ 최근 완료

| 항목 | 내용 |
|---|---|
| **JOIN 알고리즘 분리** | `engine/join.rs` 구현 완료 — Sort-Merge / Hash / Nested Loop + System-R DP 조인 순서 최적화 |
| **SHOW PROCESSLIST 실제 구현** | `ProcessInfo` + `process_list: Arc<Mutex<...>>` 세션 추적, native/MySQL 양쪽 연결 모두 등록 |
| **rustdb-client** | 전용 TCP 클라이언트 크레이트 추가 (`-u/-p/-h/-P` 플래그, ANSI 컬러, 멀티라인 SQL) |
| **MySQL wire protocol** | `mysql.rs` 신규 — COM_QUERY/PING/INIT_DB/STMT_PREPARE/EXECUTE/CLOSE, MySQL 호환 쿼리 처리 |
| **MySQL 인증 구현** | `mysql_native_password` 챌린지-응답 검증 — 연결별 nonce, SHA1(SHA1(pw)) 저장, DBeaver/mysql-connector-python 인증 완전 작동 |
| **MySQL result set 수정** | `parse_table` 탭 구분 형식 지원 — SHOW DATABASES/TABLES, SELECT 등이 MySQL 클라이언트에 정상 표시 |
| **데이터 디렉터리 재구성** | `data/_system/` 전역 파일 분리, 레거시 자동 마이그레이션, 연결별 독립 폴더(`local/`, `data_숫자/`), UI·CLI 동일 경로 공유 |
| **Tauri UI MySQL 포트 설정** | Server Manager에 MySQL 포트 입력 필드 추가 — UI에서 MySQL 프로토콜 포트 직접 설정 가능 |
| **Server Manager UI 개편** | 탭 제거, 우측 슬라이드 패널(CLI 가이드 / MySQL 연결), 버퍼 풀 크기 설정, 병렬 쿼리 토글, 버튼 스타일 통일 |
| **EXPLAIN ANALYZE 정확도 개선** | 추정 행수(`est_rows`) vs 실제 행수 비교 출력, 실행 시간 ms/sec 단위 자동 선택 |
| **데이터 분석 리포트** | 결과 패널 "AI 분석" 버튼 — SELECT 결과를 Gemini가 요약·패턴·인사이트 한국어 분석 (`/api/report`) |
| **벤치마크 자동 실행 UI** | Server Manager Bench 패널 — result.json 불러오기·포맷 표시, 터미널에서 bench.py 실행 버튼 |
| **접속 세션 실시간 모니터링** | Server Manager Session 패널 — addr·user·접속 경과 시간·쿼리 건수, 1.5s 자동 갱신 (`SessionInfo` Tauri 백엔드) |

---

## 🔧 엔진

| 항목 | 상태 | 내용 |
|---|:---:|---|
| ~~**Hash Index**~~ | ✅ | `USING HASH` 구문, 등호 O(1), 플래너 우선 선택, EXPLAIN 표시 |
| ~~**병렬 실행 확장**~~ | ✅ | SeqScan WHERE 필터 (par_chunks) + GROUP BY 집계 (par_iter) + Hash Join probe (par_iter) — rayon, `RUSTDB_PARALLEL` 토글 |
| ~~**히스토그램 통계**~~ | ✅ | equi-depth 10-bucket, ANALYZE TABLE 빌드, 플래너 PkRange·SecondaryRange selectivity 추정 |
| ~~**Buffer Pool 개선**~~ | ✅ | LRU 캐시 + dirty eviction 버그 수정 / Clock-Pro·read-ahead는 전체 테이블 단위 구조상 효과 없어 미적용 |
| **GAP Lock / Next-key Lock** | ⏭ 스킵 | 행 수 비교 방식으로 팬텀 감지 이미 동작 / 데모·벤치마크 임팩트 낮음 |
| **MVCC 버전 체인** | ⏭ 스킵 | 8일+ 공수 / Deferred Write로 ACID 이미 충족 / 발표 범위 초과 |

---

## 📊 성능 측정

| 항목 | 상태 |
|---|:---:|
| MySQL 설치 확인 | ✅ |
| `bench.py` 작성 (INSERT TPS / SELECT 등호·범위 / 병렬 / 동시 접속) | ✅ |
| `chart.py` 작성 (matplotlib PNG 5장) | ✅ |
| 측정 실행 → `result.json` 저장 | ⬜ |
| `python chart.py` → `charts/*.png` 생성 | ⬜ |
| 수치를 발표 자료에 반영 | ⬜ |

스크립트 위치: `code/test/perf/` — 실행 가이드: `README.txt`

---

## 🤖 AI

| 항목 | 난이도 | 예상 기간 | 효과 | 우선순위 |
|---|:---:|:---:|---|:---:|
| ~~**True MCP 전환**~~ | ✅ | — | `mcp_server.py` 추가 — Anthropic MCP 표준 구현, Claude Desktop 연동, API 키 불필요 | — |
| ~~**데이터 분석 리포트**~~ | ✅ | — | SELECT 결과 → AI 요약·패턴·인사이트 자동 도출, `/api/report` 추가 | — |
| **AI 자동완성 (Tab)** | 중 | 3일 | Monaco `inlineCompletionsProvider` + `/api/nl-to-sql` 연동 | ★★ |

---

## 🛠 기타

| 항목 | 난이도 | 예상 기간 | 효과 | 우선순위 |
|---|:---:|:---:|---|:---:|
| ~~**벤치마크 자동 실행 UI**~~ | ✅ | — | Server Manager Bench 패널 — result.json 불러오기·포맷 표시, bench.py 터미널 실행 | — |
| ~~**EXPLAIN ANALYZE 실행 시간 정확도**~~ | ✅ | — | 추정 행수 vs 실제 행수 비교, 실행 시간 ms/sec 자동 단위 선택 | — |
| ~~**SHOW PROCESSLIST 실시간 갱신 UI**~~ | ✅ | — | Server Manager Session 패널 — addr·user·경과 시간·쿼리 건수, 1.5s 자동 갱신 | — |
