# RustDB 개발 계획 (v2.2.0 → 잔여 2주)

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

---

## 🔧 엔진

| 항목 | 난이도 | 예상 기간 | 효과 | 우선순위 |
|---|:---:|:---:|---|:---:|
| ~~**Hash Index**~~ | ✅ 완료 | — | `USING HASH` 구문, 등호 O(1), 플래너 우선 선택, EXPLAIN 표시 | ★★★ |
| **병렬 실행 확장** | 중 | 4일 | 집계 map-reduce(SUM/COUNT/AVG chunk별 partial→merge) + Hash Join probe 병렬화 → 코어 수 비례 스케일 | ★★★ |
| ~~**히스토그램 통계**~~ | ✅ 완료 | — | equi-depth 10-bucket, ANALYZE TABLE 빌드, 플래너 selectivity 추정 적용 | ★★★ |
| **Buffer Pool 개선** | 중 | 3일 | dirty 페이지 eviction 버그 수정 완료 / LRU → Clock-Pro 교체 + SeqScan read-ahead 미구현 | ★★ |
| **GAP Lock / Next-key Lock** | 중 | 3일 | Serializable 팬텀 리드 실제 방지, 현재 행 수 비교 방식 교체 | ★★ |
| **MVCC 버전 체인** | 상 | 8일+ | 읽기·쓰기 잠금 충돌 제거 → 동시 접속 TPS 대폭 향상, SSI 선행 조건 | ★★ |

---

## 📊 성능 측정

### 측정 항목

| 항목 | 측정 방법 | 발표 포인트 | 우선순위 |
|---|---|---|:---:|
| **INSERT TPS** | 1만 행 bulk insert 소요 시간 | RustDB vs MySQL 수치 비교 | ★★★ |
| **SELECT — 등호 (Hash/B-tree/SeqScan)** | 동일 쿼리, 인덱스 종류별 응답 시간 | Hash Index O(1) 효과 증명 | ★★★ |
| **SELECT — 범위 (히스토그램 전/후)** | ANALYZE 전·후 EXPLAIN est_rows 비교 | 플래너 selectivity 개선 증명 | ★★★ |
| **병렬 스케일링** | 코어 수(1/2/4) 대비 집계 처리량 | "왜 Rust" — 선형 스케일 증명 | ★★★ |
| **동시 접속 TPS** | 다중 클라이언트 동시 쿼리 | 서버 처리량 한계 측정 | ★★ |

### 기술 스택

```
rustdb-server (포트 7878)  ←──┐
                               Python 벤치마크 스크립트 ──→ matplotlib 차트
MySQL (포트 3306)          ←──┘
```

- **RustDB 접속**: `socket` TCP (AUTH + SQL 프로토콜)
- **MySQL 접속**: `mysql-connector-python`
- **시각화**: `matplotlib` bar chart / line chart

### 진행 순서

1. MySQL 설치 확인 (비교 대상)
2. Python 벤치마크 스크립트 작성 (`code/bench/bench.py`)
3. 측정 실행 → JSON 결과 저장
4. 차트 생성 스크립트 작성 (`code/bench/chart.py`)
5. 발표 자료용 PNG 출력

| 스크립트 | 역할 | 난이도 | 예상 기간 |
|---|---|:---:|:---:|
| `bench.py` | 양쪽 TPS/latency 측정, JSON 저장 | 하 | 1일 |
| `chart.py` | matplotlib 시각화, PNG 출력 | 하 | 0.5일 |

---

## 🤖 AI

| 항목 | 난이도 | 예상 기간 | 효과 | 우선순위 |
|---|:---:|:---:|---|:---:|
| **데이터 분석 리포트** | 하 | 1일 | SELECT 결과 → AI 요약·패턴·인사이트 자동 도출, `/api/report` 추가 | ★★★ |
| **AI 자동완성 (Tab)** | 중 | 3일 | Monaco `inlineCompletionsProvider` + `/api/nl-to-sql` 연동 | ★★ |

---

## 🛠 기타

| 항목 | 난이도 | 예상 기간 | 효과 | 우선순위 |
|---|:---:|:---:|---|:---:|
| **벤치마크 자동 실행 UI** | 하 | 1일 | Server Manager에서 버튼 하나로 벤치마크 실행·결과 표시 | ★★ |
| **EXPLAIN ANALYZE 실행 시간 정확도** | 하 | 0.5일 | 현재 실제 측정 중이나 정밀도 개선 | ★★ |
| **INSERT … ON DUPLICATE KEY UPDATE 검증** | 하 | 0.5일 | AST 존재하나 executor 구현 완전성 확인·수정 | ★★ |
| **SHOW PROCESSLIST 실시간 갱신 UI** | 하 | 1일 | Server Manager에 실시간 세션 모니터링 패널 | ★ |

---

## 📅 2주 추천 순서

```
1주차  벤치마크 스크립트 작성 → Hash Index 구현 → RustDB vs MySQL 차트
2주차  병렬 실행 확장 → 데이터 분석 리포트 → GAP Lock
```

벤치마크를 먼저 잡는 이유: Hash Index·병렬 확장 구현 직후 수치로 바로 증명할 수 있어 시너지가 가장 크다.
