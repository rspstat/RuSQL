# RustDB 개발 계획 (v2.2.0)

> ✅ 완료 / 🔲 미완료 / 🚧 진행 중  
> **심사 핵심 3가지: AI(MCP) · 엔진 · 성능 측정**

---

## 우선순위 요약

| 우선순위 | 항목 | 상태 | 예상 소요 |
|----------|------|------|-----------|
| 🔴 1순위 | MCP 서버 구현 (Python) | 🔲 | 2~3일 |
| 🔴 2순위 | UI ↔ MCP 서버 연동 | 🔲 | 1일 |
| 🟡 3순위 | 성능 벤치마크 측정·문서화 | 🔲 | 1일 |
| 🟢 완료 | 엔진 (rustdb-core) | ✅ | — |
| 🟢 완료 | UI (rustdb-ui) | ✅ | — |
| 🟢 완료 | 서버 (rustdb-server) | ✅ | — |

---

## 1. MCP 서버 구현 🔴 (미완료 — 최우선)

> 프로젝트 제목이 **"MCP 기반 커스텀 RDBMS"** — 이게 없으면 제목 자체가 성립 안 됨

### 1-1. 서버 구조

```
rustdb-mcp/
├── main.py          # FastAPI 서버 진입점
├── llm.py           # Claude API 호출 (anthropic SDK)
├── db.py            # RustDB 연결 (mysql-connector-python, 포트 3306)
├── schema.py        # SHOW TABLES + DESCRIBE → 스키마 문자열 생성
└── requirements.txt
```

### 1-2. 구현 태스크

| # | 태스크 | 설명 | 상태 |
|---|--------|------|------|
| 1 | 프로젝트 세팅 | `pip install fastapi uvicorn anthropic mysql-connector-python`, requirements.txt | 🔲 |
| 2 | RustDB 연결 (`db.py`) | `mysql.connector.connect(host, port=3306, user, password)`, 쿼리 실행 함수 | 🔲 |
| 3 | 스키마 수집 (`schema.py`) | `SHOW TABLES` → 각 테이블 `DESCRIBE` → 하나의 문자열로 직렬화 | 🔲 |
| 4 | Claude API 호출 (`llm.py`) | 스키마 + 자연어 → SQL 생성 프롬프트, `claude-opus-4-7` 모델 사용 | 🔲 |
| 5 | `/ask` 엔드포인트 (`main.py`) | `POST /ask { question, db }` → SQL 생성 → RustDB 실행 → 결과 반환 | 🔲 |
| 6 | `/generate` 엔드포인트 | `POST /generate { question, db }` → SQL만 반환 (에디터 삽입용) | 🔲 |
| 7 | 오류 처리 | SQL 실행 실패 시 에러 메시지 + 재시도 프롬프트 | 🔲 |

### 1-3. 핵심 프롬프트 설계

```
당신은 RustDB SQL 전문가입니다.
아래 스키마를 참고하여 사용자 질문에 맞는 SQL을 작성하세요.
SELECT만 반환하고, 마크다운 없이 SQL만 출력하세요.

[스키마]
{schema}

[질문]
{question}

[SQL]
```

### 1-4. 데모 시나리오 (심사용)

```
사용자: "부서별 평균 급여를 높은 순으로 보여줘"
         ↓
schema.py: SHOW TABLES → DESCRIBE emp, dept, sal
         ↓
llm.py: Claude API → SELECT d.name, AVG(e.salary) ...
         ↓
db.py: RustDB 실행 → 결과 rows
         ↓
UI 결과창: 테이블 표시
```

---

## 2. UI ↔ MCP 서버 연동 🔴 (미완료)

> AI 채팅 패널(UI)은 이미 완성 — MCP 서버 HTTP 호출만 연결하면 됨

| # | 태스크 | 설명 | 상태 |
|---|--------|------|------|
| 1 | Tauri `ask_ai` 커맨드 | `invoke("ask_ai", { question, db })` → `http://localhost:8000/ask` fetch | 🔲 |
| 2 | Tauri `generate_sql` 커맨드 | `invoke("generate_sql", { question, db })` → SQL 문자열 반환 → 에디터 삽입 | 🔲 |
| 3 | 채팅 패널 연결 | 기존 `sendChat()` 함수를 Tauri 커맨드로 교체 | 🔲 |
| 4 | 현재 DB 자동 전달 | 채팅 시 `currentDb` 상태를 자동으로 MCP 서버에 전달 | 🔲 |
| 5 | 에러 표시 | MCP 서버 미실행 시 "AI 서버를 먼저 실행하세요" 메시지 | 🔲 |

---

## 3. 성능 벤치마크 🟡 (미완료)

> "왜 Rust인가"의 근거 — 수치 없이는 주장만 있는 발표

### 3-1. 측정 항목

| # | 항목 | 방법 | 목표 |
|---|------|------|------|
| 1 | INSERT TPS | 1만 건 단건 INSERT, 소요 시간 측정 | RustDB vs MySQL 비교 |
| 2 | SELECT TPS | 인덱스 있는 단건 SELECT 1만 회 | RustDB vs MySQL 비교 |
| 3 | JOIN 쿼리 응답시간 | emp + dept + sal 3-테이블 JOIN | RustDB vs MySQL 비교 |
| 4 | 대용량 스캔 | 10만 행 Full Scan SELECT | RustDB vs MySQL 비교 |

### 3-2. 구현 태스크

| # | 태스크 | 설명 | 상태 |
|---|--------|------|------|
| 1 | 벤치마크 스크립트 | `benchmark.py` — `mysql-connector-python`으로 RustDB/MySQL 각각 접속 | 🔲 |
| 2 | 더미 데이터 생성 | `faker`로 10만 행 INSERT SQL 생성 | 🔲 |
| 3 | TPS 계산 | `time.perf_counter()` 기반 정확한 측정, 3회 평균 | 🔲 |
| 4 | 그래프 출력 | `matplotlib` 막대 그래프 — RustDB(teal) vs MySQL(orange) | 🔲 |
| 5 | README 반영 | 측정 결과 수치·그래프를 README.md에 공개 | 🔲 |

---

## 4. 엔진 (rustdb-core) ✅ 완료

> 추가 개발 불필요. 심사 대비 질문 방어 준비만 하면 됨.

| 항목 | 상태 |
|------|------|
| DDL / DML / JOIN / 서브쿼리 / CTE / 재귀 CTE | ✅ |
| B+Tree 인덱스 (단일 / 복합 / 클러스터드) | ✅ |
| 비용 기반 옵티마이저 + EXPLAIN | ✅ |
| 트랜잭션 — BEGIN / COMMIT / ROLLBACK / SAVEPOINT | ✅ |
| WAL + 크래시 복구 | ✅ |
| MVCC + 격리 수준 4단계 | ✅ |
| 저장 프로시저 / 트리거 / 사용자 정의 함수 | ✅ |
| MySQL wire protocol 호환 (포트 3306) | ✅ |
| INFORMATION_SCHEMA 가상 테이블 | ✅ |
| 사용자 관리 / GRANT / REVOKE | ✅ |

---

## 5. UI (rustdb-ui) ✅ 완료

> 데모용으로 충분. 추가 개발 불필요.

| 항목 | 상태 |
|------|------|
| Monaco 에디터 (다중 탭, 분할, 고정, 컨텍스트 메뉴) | ✅ |
| MySQL 스타일 에디터 툴바 (파일 열기/저장/실행) | ✅ |
| 결과 테이블 (Canvas 자동 너비, 정렬, 검색, 셀 편집, 진행 바) | ✅ |
| AI Agent 채팅 패널 (UI 완성, 백엔드 연동 미완료) | ✅/🔲 |
| ERD 다이어그램 (FK 관계선, Auto Layout, 드래그) | ✅ |
| 사이드바 (DB/테이블/뷰/인덱스 컨텍스트 메뉴) | ✅ |
| 쿼리 히스토리 / 북마크 | ✅ |

---

## 6. 서버 (rustdb-server) ✅ 완료

| 항목 | 상태 |
|------|------|
| TCP 서버 (포트 7878, 멀티 클라이언트) | ✅ |
| MySQL wire protocol (포트 3306, DBeaver 완전 연동) | ✅ |
