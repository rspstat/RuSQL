# RustDB AI 연동 (v2.2.0)

> 프로젝트 제목 "MCP 기반 커스텀 RDBMS"의 핵심 차별점  
> 모델: **Gemini 2.5 Flash** (google-genai) · 구현 언어: Python (FastAPI)  
> 기준일: 2026-05-27

---

## 1. 구현 방향

### 핵심 원칙

- **모델은 교체 가능한 부품** — Gemini, Claude, GPT, 로컬 모델 어느 것이든 동작하는 구조
- **기술적 기여는 MCP 서버 아키텍처** — 스키마 수집, 컨텍스트 주입, 에디터 파일 연동, RustDB 연동 파이프라인
- API 사용 자체가 아니라 **어떻게 설계했는가**가 평가 포인트

### 사용 모델

| 항목 | 내용 |
|------|------|
| 모델 | Gemini 2.5 Flash (`gemini-2.5-flash`) |
| SDK | `google-genai` Python 패키지 |
| 이유 | 1M 토큰 컨텍스트 윈도우, 빠른 응답, 무료 티어 제공 |

---

## 2. 구현된 기능

### 2-1. 자연어 → SQL 변환 ✅

사용자가 자연어로 질문하면 현재 DB 스키마를 기반으로 SQL을 자동 생성한다.

```
입력: "매출 상위 10개 상품 보여줘"
출력: SELECT product_name, SUM(amount) AS total
      FROM orders
      GROUP BY product_name
      ORDER BY total DESC
      LIMIT 10;
```

- SHOW TABLES + DESCRIBE로 스키마를 자동 수집해 프롬프트에 주입
- 생성된 SQL을 UI 에디터에 자동 삽입
- 원클릭으로 RustDB에 즉시 실행 가능

### 2-2. EXPLAIN 결과 자연어 해석 ✅

`EXPLAIN` 출력을 AI에게 넘겨 성능 문제와 개선 방향을 한국어로 설명한다.

```
입력: EXPLAIN SELECT * FROM orders WHERE customer_id = 123;
출력: "이 쿼리는 Full Table Scan을 수행하고 있습니다.
      customer_id 컬럼에 인덱스를 추가하면 성능이 크게 향상됩니다.
      → CREATE INDEX idx_cid ON orders (customer_id);"
```

### 2-3. 스키마 설계 추천 ✅

자연어로 시스템 요구사항을 설명하면 AI가 CREATE TABLE SQL을 제안한다.

```
입력: "주문 관리 시스템을 만들고 싶어"
출력: CREATE TABLE customers (...);
      CREATE TABLE products (...);
      CREATE TABLE orders (...);
```

### 2-4. AI Agent 채팅 패널 ✅

멀티턴 대화로 SQL 질의, 쿼리 최적화, 스키마 설명 등을 자연어로 처리한다.

- 채팅 버블 UI (user / assistant 구분), 마크다운 렌더링 (marked + DOMPurify)
- 타이핑 인디케이터 (점 3개 애니메이션)
- 대화 내역 localStorage 영구 보존 (앱 재시작 후 유지)
- Enter 전송 / Ctrl+Enter 줄바꿈
- 패널 너비 드래그 조절 (240px ~ 640px)

### 2-5. 에디터 파일 컨텍스트 자동 주입 ✅

현재 에디터에 열린 SQL 파일 내용을 AI 컨텍스트로 자동 전달한다.

- **활성 탭 자동 포함** — 현재 편집 중인 SQL 파일이 항상 컨텍스트에 포함됨
- **`@파일명` 멘션** — 입력창에 `@query2.sql` 입력 시 해당 탭 내용 추가, `@` 타이핑 시 자동완성 드롭다운 출현
- 포함된 파일은 입력창 상단에 칩으로 시각적 표시

```
입력: "@query2.sql 이 쿼리를 최적화해줘"
→ AI가 query2.sql 전체 내용을 읽고 최적화 방안 제시
```

### 2-6. AI 에이전트 파일 편집 ✅

AI가 직접 에디터 탭의 SQL 파일을 수정·삽입·삭제할 수 있다.

- AI가 `<<<FILE filename.sql\n...\nFILE>>>` 형식으로 수정된 전체 파일 내용 반환
- 채팅 버블에 주황색 **파일 편집 블록**으로 미리보기 표시
- **"파일에 적용"** 버튼 클릭 → 해당 탭 내용 교체 (Monaco 에디터 Undo 지원)
- 적용 후 "✓ 적용됨"으로 상태 변경, 중복 적용 방지

```
입력: "@query.sql에서 WHERE salary > 500을 salary > 800으로 수정해줘"
→ 파일 편집 블록 표시 → 파일에 적용 클릭 → 에디터 즉시 반영
```

---

## 3. MCP 서버 구조

```
rustdb-mcp/
  server.py          # FastAPI MCP 서버 (단일 파일)
  requirements.txt   # google-genai, fastapi, uvicorn
```

### 엔드포인트

| 경로 | 메서드 | 설명 |
|------|--------|------|
| `GET /health` | GET | 서버 상태 및 모델 확인 |
| `POST /api/nl-to-sql` | POST | 자연어 → SQL 변환 |
| `POST /api/explain` | POST | EXPLAIN 결과 AI 해석 |
| `POST /api/schema-design` | POST | 자연어 → CREATE TABLE SQL |
| `POST /api/chat` | POST | 멀티턴 채팅 (파일 컨텍스트 + 파일 편집 포함) |

### 핵심 파이프라인

```python
from google import genai
from google.genai import types

MODEL = "gemini-2.5-flash"

# 채팅 (멀티턴 + 파일 컨텍스트)
client = genai.Client(api_key=api_key)
chat_session = client.chats.create(
    model=MODEL,
    config=types.GenerateContentConfig(system_instruction=system),
    history=history,
)
response = chat_session.send_message(last_message)
```

### 시스템 프롬프트 구조

```
[역할] RustDB AI 어시스턴트 (MySQL 호환 SQL 엔진)
[컨텍스트] 현재 DB: {current_db}
[스키마] {schema}
[열린 파일들] --- filename.sql --- ... (open_files)
[규칙]
- 한국어 질문에는 한국어로 답변
- SQL 생성 시 ```sql 블록 사용
- 파일 수정 시 <<<FILE filename.sql ... FILE>>> 형식 사용
```

---

## 4. Tauri 자동 시작

앱 실행 시 MCP 서버를 자동으로 시작하고, 앱 종료 시 프로세스를 정리한다.

```rust
// main.rs — setup hook
fn start_mcp_server() -> Option<Child> {
    Command::new("python")
        .args(["-m", "uvicorn", "server:app",
               "--host", "127.0.0.1", "--port", "8765"])
        .current_dir(server_dir)
        .spawn()
        .ok()
}

// RunEvent::Exit — 앱 종료 시 프로세스 kill
if let tauri::RunEvent::Exit = event {
    if let Some(mut child) = app.state::<McpServer>().0.lock().unwrap().take() {
        let _ = child.kill();
    }
}
```

---

## 5. UI 연동 구조

```
에디터 (Monaco) ──→ 활성 탭 SQL + @멘션 탭 SQL
                          ↓
                   sendChat() in App.tsx
                          ↓
             GET schema via Tauri invoke
                          ↓
        POST /api/chat { messages, schema, open_files }
                          ↓
               Gemini 2.5 Flash API
                          ↓
         { content, sql?, file_edits? }
                    ↙         ↘
          채팅 버블 표시    파일 편집 블록 표시
                                ↓
                        "파일에 적용" 버튼
                                ↓
                    Monaco 에디터 탭 내용 교체
```

---

## 6. API 키 관리

- UI 설정 탭에서 Google Gemini API 키 입력
- `localStorage`에 저장 (앱 재시작 후 유지)
- 코드에 하드코딩 없음 — 키 노출 위험 없음

---

## 7. 환경 기록

| 항목 | 내용 |
|------|------|
| OS | Windows 11 Pro |
| Python | 3.x |
| AI 모델 | gemini-2.5-flash |
| 컨텍스트 윈도우 | 1,000,000 토큰 |
| MCP 서버 포트 | 8765 |
| 자동 시작 | Tauri setup hook (python -m uvicorn) |
