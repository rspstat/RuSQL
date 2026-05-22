# RustDB AI 연동 계획

> 프로젝트 제목 "MCP 기반 커스텀 RDBMS"의 핵심 차별점  
> 모델: Claude API (claude-opus-4-7) · 구현 언어: Python  
> 기준일: 2026-05-22 · 발표 예정: 2026년 6월 초

---

## 1. 구현 방향

### 핵심 원칙

- **모델은 교체 가능한 부품** — Claude API, GPT, 로컬 모델 어느 것이든 동작하는 구조
- **기술적 기여는 MCP 서버 아키텍처** — 스키마 수집, 컨텍스트 주입, Tool 설계, RustDB 연동 파이프라인
- API 사용 자체가 아니라 **어떻게 설계했는가**가 평가 포인트

### 사용 모델

| 항목 | 내용 |
|------|------|
| 모델 | Claude (claude-opus-4-7) |
| SDK | `anthropic` Python 패키지 |
| 이유 | 사무용 노트북(내장 그래픽) 환경 → 로컬 모델 실시간 데모 불가 |

---

## 2. 기능 목록

### 2-1. 자연어 → SQL 변환 (필수 · 졸업 전 완료)

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

### 2-2. EXPLAIN 결과 자연어 해석 (필수 · 졸업 전 완료)

`EXPLAIN` 출력을 AI에게 넘겨 성능 문제와 개선 방향을 자연어로 설명한다.

```
입력: EXPLAIN SELECT * FROM orders WHERE customer_id = 123;
출력: "이 쿼리는 Full Table Scan을 수행하고 있습니다.
      customer_id 컬럼에 인덱스를 추가하면 성능이 크게 향상됩니다.
      → CREATE INDEX idx_cid ON orders (customer_id);"
```

- 심사 데모에서 "AI가 쿼리 최적화를 제안한다"는 직접적인 시연 가능

### 2-3. 스키마 설계 추천 (선택 · 시간 여유 시)

자연어로 시스템 요구사항을 설명하면 AI가 테이블 구조와 CREATE TABLE SQL을 제안한다.

```
입력: "주문 관리 시스템을 만들고 싶어"
출력: CREATE TABLE customers (...);
      CREATE TABLE products (...);
      CREATE TABLE orders (...);
```

---

## 3. MCP 서버 구조

```
rustdb-mcp/
  server.py          # MCP 서버 진입점 (FastAPI or stdio)
  tools/
    get_schema.py    # SHOW TABLES + DESCRIBE → 스키마 수집
    execute_query.py # RustDB 쿼리 실행 (mysql-connector-python)
    explain_query.py # EXPLAIN 실행 + 결과 반환
  prompt.py          # 스키마 컨텍스트 주입 + 프롬프트 구성
  requirements.txt   # anthropic, mysql-connector-python, fastapi
```

### Tool 정의

| Tool | 설명 | 입력 | 출력 |
|------|------|------|------|
| `get_schema` | 현재 DB의 테이블·컬럼 정보 수집 | db_name | 스키마 텍스트 |
| `execute_query` | RustDB에 SQL 실행 | sql | 결과 rows |
| `explain_query` | EXPLAIN 실행 | sql | 실행 계획 텍스트 |

### 핵심 파이프라인

```python
import anthropic, mysql.connector

def natural_to_sql(question: str, db: str) -> str:
    schema = get_schema(db)          # 스키마 자동 수집
    prompt = build_prompt(question, schema)  # 컨텍스트 주입
    response = client.messages.create(
        model="claude-opus-4-7",
        max_tokens=1024,
        messages=[{"role": "user", "content": prompt}]
    )
    return response.content[0].text  # 생성된 SQL 반환
```

---

## 4. UI 연동

- 사이드바 4번째 아이콘 **AI Assistant 뷰** (이미 자리 확보됨)
- 자연어 입력창 → MCP 서버 호출 → 생성된 SQL을 에디터에 삽입
- "EXPLAIN 해석" 버튼 → 현재 에디터 쿼리를 AI에게 분석 요청

---

## 5. 교수님 어필 포인트

> "자연어 → SQL 변환 자체는 AI 모델의 역할이고, 저의 구현 범위는 **MCP 서버 설계**입니다.  
> 구체적으로는 RustDB 스키마를 실시간으로 수집해 프롬프트에 주입하는 파이프라인,  
> RustDB 전용 Tool 정의, UI와의 연동 구조를 직접 구현했습니다.  
> 모델은 교체 가능한 부품으로 설계되어 있어 Claude 대신 로컬 모델도 연결 가능합니다."

---

## 6. 진행 체크리스트

### 졸업 전 완료 (2026-05-22 ~ 05-26)

- [x] `rustdb-mcp/` 폴더 및 기본 구조 생성
- [x] `get_schema` Tool 구현 (UI에서 Tauri invoke로 스키마 자동 수집 → 프롬프트 주입)
- [x] `execute_query` Tool 구현 (UI Tauri invoke 경유)
- [x] 스키마 컨텍스트 주입 프롬프트 구성
- [x] 자연어 → SQL 변환 파이프라인 완성
- [x] `explain_query` Tool 구현 + AI 해석 기능
- [x] UI AI Assistant 뷰 연동 (자연어 입력 → SQL 에디터 삽입)
- [ ] 데모 시나리오 작성 및 리허설

### 시간 여유 시 (선택)

- [ ] 스키마 설계 추천 기능
- [ ] 멀티턴 대화 (이전 질문 맥락 유지)
- [ ] 쿼리 결과를 AI가 자연어로 해석

---

## 7. 환경 기록

| 항목 | 내용 |
|------|------|
| OS | Windows 11 Pro |
| Python | 3.x |
| AI 모델 | claude-opus-4-7 |
| DB 연결 | mysql-connector-python (포트 3306) |
| GPU | 내장 그래픽 (로컬 모델 실시간 추론 불가) |
