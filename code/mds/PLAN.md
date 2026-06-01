# RustDB 향후 계획 (v2.2.0)

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

---

## 🔧 엔진

| 항목 | 난이도 | 효과 | 비고 |
|---|:---:|---|---|
| **병렬 실행 확장** | 중 | 대형 집계·조인 추가 가속 | 현재 WHERE 필터만 병렬(rayon) → 집계 map-reduce, Hash Join probe까지 확대 |
| **Hash Index** | 중 | 등호 검색 O(1), 인덱스 다양성 | B+Tree에 이은 2번째 타입. 범위 검색 불가 트레이드오프 |
| **MVCC 버전 체인** | 상 | 읽기·쓰기 잠금 충돌 제거, SSI 선행 조건 | 현재 `_xmin/_xmax` 컬럼 방식 → 별도 버전 레코드로 대수술 |
| **SSI** | 상 | 잠금 없는 Serializable (PostgreSQL 방식) | MVCC 완료 후 착수. SIREAD lock + rw-conflict 그래프 + 사이클 감지 |
| **GAP Lock / Next-key Lock** | 중 | Serializable 팬텀 리드 실제 방지 | 현재 행 수 비교로만 체크. `lock_manager.rs` 범위 락 추가 필요 |

---

## 📊 성능 측정

| 항목 | 내용 | 효과 |
|---|---|---|
| **벤치마크 스크립트** | `bench_insert.py` / `bench_select.py` / `bench_concurrent.py` | 재현 가능한 측정 기반 마련 |
| **RustDB vs MySQL 수치** | INSERT TPS, SELECT 인덱스 유무, 동시 접속 부하 | 경쟁 DB 대비 정량 우위 확보 |
| **병렬 스케일링 그래프** | SeqScan 병렬도를 코어 수(1/2/4/8) 대비 측정 | "왜 Rust" 직접 증명 |
| **matplotlib 시각화** | 측정 결과를 차트로 렌더링 | 발표 자료 임팩트 |

---

## 🤖 AI

| 항목 | 난이도 | 효과 | 비고 |
|---|:---:|---|---|
| **데이터 분석 리포트** | 낮음 | SELECT 결과 패턴·인사이트 자동 도출 | `/api/report` 엔드포인트 추가, 결과창 "AI 분석" 버튼 |
| **AI 자동완성 (Tab)** | 중간 | Monaco Editor 작성 속도 향상 | `inlineCompletionsProvider` + `/api/nl-to-sql` 연동 |

---

## 우선순위

| 순위 | 항목 | 이유 |
|:---:|---|---|
| 1 | **성능 측정 스크립트** | 심사 3축 중 "성능" 빈칸. MySQL 프로토콜 인증이 완성됐으므로 `mysql-connector-python`으로 바로 측정 가능 |
| 2 | **병렬 실행 확장** | 벤치마크와 묶으면 시너지. 집계·Hash Join probe 병렬화로 수치 개선 직결 |
| 3 | **Hash Index** | 구현 패턴 명확. B+Tree 코드 옆에 추가, 등호 검색 TPS 개선 수치로 연결 |
| 4 | **GAP Lock** | Serializable 정확성 향상. 중간 난이도, lock_manager.rs 범위 락 추가 |
| 5 | **MVCC → SSI** | 장기 목표. 학술적 완성도, 동시성 심화 |
| 6 | **AI 기능 추가** | 현재 6개 엔드포인트로 충분히 차별화됨. 후순위 |
