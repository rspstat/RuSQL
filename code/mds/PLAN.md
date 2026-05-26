# RustDB 개발 계획 (v2.2.0)

> ✅ 완료 / 🔲 미완료 / 🚧 진행 중  
> **심사 핵심 3가지: AI(MCP) · 엔진 · 성능 측정**

---

## 우선순위 요약

| 우선순위 | 항목 | 상태 | 예상 소요 |
|----------|------|------|-----------|
| 🟢 완료 | MCP 서버 구현 (Python / Gemini 2.5 Flash) | ✅ | — |
| 🟢 완료 | UI ↔ MCP 서버 연동 | ✅ | — |
| 🟡 3순위 | 성능 벤치마크 측정·문서화 | 🔲 | 1일 |
| 🟢 완료 | 엔진 (rustdb-core) | ✅ | — |
| 🟢 완료 | UI (rustdb-ui) | ✅ | — |
| 🟢 완료 | 서버 (rustdb-server) | ✅ | — |

---

## 1. MCP 서버 구현 ✅ 완료

> 프로젝트 제목 **"MCP 기반 커스텀 RDBMS"** 의 핵심 차별점 — 구현 완료

### 1-1. 서버 구조 (구현됨)

```
rustdb-mcp/
└── server.py        # FastAPI MCP 서버 (단일 파일)
└── requirements.txt # google-genai, fastapi, uvicorn
```

### 1-2. 구현 태스크

| # | 태스크 | 설명 | 상태 |
|---|--------|------|------|
| 1 | 프로젝트 세팅 | `pip install fastapi uvicorn google-genai`, requirements.txt | ✅ |
| 2 | 스키마 수집 | SHOW TABLES + DESCRIBE → 시스템 프롬프트 주입 | ✅ |
| 3 | Gemini API 호출 | 스키마 + 자연어 → SQL 생성, `gemini-2.5-flash` 모델 | ✅ |
| 4 | `/api/nl-to-sql` 엔드포인트 | 자연어 → SQL 변환 | ✅ |
| 5 | `/api/explain` 엔드포인트 | EXPLAIN 결과 AI 해석 | ✅ |
| 6 | `/api/schema-design` 엔드포인트 | 자연어 → CREATE TABLE SQL | ✅ |
| 7 | `/api/chat` 엔드포인트 | 멀티턴 채팅 + 파일 컨텍스트 + 파일 편집 | ✅ |
| 8 | 에디터 파일 컨텍스트 주입 | 현재 열린 SQL 파일 자동 포함, @파일명 멘션 | ✅ |
| 9 | AI 파일 편집 블록 | `<<<FILE filename.sql\n...\nFILE>>>` 형식 파싱 및 적용 | ✅ |

### 1-3. 데모 시나리오 (심사용)

```
사용자: "부서별 평균 급여를 높은 순으로 보여줘"
         ↓
UI: 현재 DB 스키마 수집 (SHOW TABLES + DESCRIBE)
         ↓
POST /api/chat { messages, schema, open_files }
         ↓
Gemini 2.5 Flash → SELECT d.name, AVG(e.salary) ...
         ↓
UI: SQL 제안 → 에디터 삽입 → 원클릭 실행
```

---

## 2. UI ↔ MCP 서버 연동 ✅ 완료

| # | 태스크 | 설명 | 상태 |
|---|--------|------|------|
| 1 | `sendChat()` 함수 | `fetch("http://127.0.0.1:8765/api/chat", ...)` 직접 호출 | ✅ |
| 2 | 현재 DB 자동 전달 | `currentDb` 상태를 MCP 서버에 자동 전달 | ✅ |
| 3 | 스키마 자동 수집 | `invoke("get_schema")` → 시스템 프롬프트 주입 | ✅ |
| 4 | 파일 컨텍스트 전달 | 열린 SQL 탭 내용을 `open_files` 배열로 전달 | ✅ |
| 5 | SQL 제안 삽입 | AI 응답 `sql` 필드 → 에디터 탭에 자동 삽입 | ✅ |
| 6 | 파일 편집 적용 | AI 응답 `file_edits` → "파일에 적용" 버튼 → Monaco 에디터 교체 | ✅ |
| 7 | Tauri 자동 시작 | 앱 실행 시 `python -m uvicorn server:app --port 8765` 자동 기동 | ✅ |
| 8 | API 키 관리 | UI 설정 탭에서 Google Gemini API 키 입력 → localStorage 저장 | ✅ |

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
| AI Agent 채팅 패널 (Gemini 2.5 Flash, 파일 컨텍스트, @멘션, 파일 편집, 드래그 너비 조절) | ✅ |
| ERD 다이어그램 (FK 관계선, Auto Layout, 드래그) | ✅ |
| 사이드바 (DB/테이블/뷰/인덱스 컨텍스트 메뉴) | ✅ |
| 쿼리 히스토리 / 북마크 | ✅ |

---

## 6. 서버 (rustdb-server) ✅ 완료

| 항목 | 상태 |
|------|------|
| TCP 서버 (포트 7878, 멀티 클라이언트) | ✅ |
| MySQL wire protocol (포트 3306, DBeaver 완전 연동) | ✅ |
