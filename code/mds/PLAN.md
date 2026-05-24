# RustDB 개발 계획 (v2.2.0)

> ✅ 완료 / 🔲 미완료 / ➖ 범위 외(졸업 초과)  
> 난이도: 소 (1~2일) / 중 (3~7일) / 대 (1주+)

---

## 1. 엔진 (rustdb-core)

> **결론: 졸업작품 기준 전부 완료. 추가 개발 불필요.**

### 졸업 기준 (완료)

| # | 항목 | 이유 | 상태 |
|---|------|------|------|
| 1 | DDL — CREATE / DROP / ALTER / TRUNCATE | RDBMS 기본 중 기본 | ✅ |
| 2 | DML — INSERT / SELECT / UPDATE / DELETE | 데이터 읽기·쓰기 없이는 DB 증명 불가 | ✅ |
| 3 | WHERE / JOIN / 서브쿼리 / CTE / 재귀 CTE | 실무 쿼리 패턴 전체 커버 | ✅ |
| 4 | 인덱스 — B+Tree / 단일 / 복합 / 클러스터드 | Rust로 만든 이유의 핵심 | ✅ |
| 5 | 비용 기반 옵티마이저 + EXPLAIN | 쿼리 실행 경로 시각 증명 | ✅ |
| 6 | 트랜잭션 — BEGIN / COMMIT / ROLLBACK / SAVEPOINT | ACID 보장 = RDBMS의 정의 | ✅ |
| 7 | WAL + 크래시 복구 | Durability 직접 시연 가능 | ✅ |
| 8 | MVCC + 격리 수준 4단계 | 동시성 제어 이론 전체 커버 | ✅ |
| 9 | 저장 프로시저 + 제어문 (IF / WHILE / LOOP / REPEAT) | 로직 캡슐화·제어 흐름 시연 | ✅ |
| 10 | 트리거 | 자동 이벤트 처리 — audit_log 예시 데모 가능 | ✅ |
| 11 | 사용자 정의 함수 (CREATE FUNCTION) | SQL 확장성 시연 | ✅ |
| 12 | MySQL wire protocol 호환 (포트 3306) | DBeaver / mysql CLI 직접 접속 데모 | ✅ |
| 13 | INFORMATION_SCHEMA 가상 테이블 | DBeaver 테이블·컬럼 자동 목록 표시 | ✅ |
| 14 | 사용자 관리 / GRANT / REVOKE | 인증·권한 관리 시연 | ✅ |
| 15 | PREPARE / EXECUTE / DEALLOCATE + 사용자 변수 (@var) | DBeaver 접속 시 내부적으로 사용, MySQL 기본 기능 | ✅ |
| 16 | EXPLAIN 출력 포맷 (74자 너비, 단어 경계 줄바꿈) | 심사 데모에서 직접 보여주는 화면 | ✅ |

### 졸업 이후 로드맵 (선택)

| # | 항목 | 설명 | 난이도 |
|---|------|------|--------|
| 1 | GAP Lock | SERIALIZABLE에서 팬텀 읽기를 롤백 대신 실제 GAP Lock으로 방지 | 대 |
| 2 | 완전한 MVCC | 튜플 레벨 다중 버전 → VACUUM 없이 일관된 스냅샷 읽기 | 대 |
| 3 | 파티셔닝 / 복제 | RANGE 파티션, WAL 스트리밍 Read Replica | 대 |
| 4 | 부분 인덱스 / 표현식 인덱스 | WHERE 조건 인덱스, LOWER(name) 인덱스 등 | 중 |
| 5 | GENERATED COLUMNS | `GENERATED ALWAYS AS (a + b) STORED` 파생 컬럼 | 중 |
| 6 | 이벤트 스케줄러 | `CREATE EVENT … ON SCHEDULE EVERY …` 주기적 SQL 실행 | 중 |
| 7 | JSON 함수 보강 | JSON_OBJECT / JSON_ARRAY / JSON_SET / JSON_KEYS 등 | 중 |
| 8 | 행 저장 → 페이지별 분할 | 대용량 대응을 위한 스토리지 개편 | 대 |

---

## 2. 프론트 (rustdb-ui)

### 졸업 전 완료 권장

| # | 항목 | 이유 | 난이도 | 상태 |
|---|------|------|--------|------|
| 1 | AI 뷰 — 자연어 → SQL 변환 | 프로젝트 제목이 **"MCP 기반 커스텀 RDBMS"** — AI 기능 없으면 제목과 내용 불일치 | 중 | ✅ |
| 2 | 결과 셀 직접 편집 | 셀 더블클릭 → `UPDATE … SET col = val WHERE pk = …` 자동 생성 — "조회 전용"이 아님을 증명 | 중 | ✅ |

### 졸업 이후 로드맵 (선택)

| # | 항목 | 설명 | 난이도 | 상태 |
|---|------|------|--------|------|
| 1 | 테이블 시각적 편집기 | 우클릭 → "Edit Table" → ALTER TABLE GUI | 중 | ✅ |
| 2 | ERD 자동 레이아웃 | FK 기반 위상 정렬으로 자동 배치 | 중 | ✅ |
| 3 | 자동완성 개선 | Monaco 버전 호환 문제로 제거 | 중 | ➖ |
| 4 | 멀티 결과 탭 | 세미콜론 구분 멀티 쿼리 결과를 각각 별도 탭으로 표시 | 중 | ➖ |
| 5 | 결과 내보내기 확장 | JSON / Excel(.xlsx) 내보내기 추가 | 소 | 🔲 |
| 6 | 쿼리 실행 진행 표시 | 장시간 쿼리 애니메이션 바 | 소 | ✅ |
| 7 | 연결 프로파일 관리 | 여러 서버 연결 설정을 이름 붙여 저장·전환 | 소 | 🔲 |
| 8 | 뷰/인덱스 우클릭 메뉴 | 사이드바 뷰·인덱스에 MySQL 동등 컨텍스트 메뉴 추가 | 소 | ✅ |
| 9 | 탭 우클릭 컨텍스트 메뉴 | VSCode 스타일 — 닫기 / 다른 탭 닫기 / 오른쪽 탭 닫기 / 모두 닫기 / SQL 다운로드 / 이름 변경 / 고정 / 분할; 분할 탭바에서도 동일 메뉴 | 소 | ✅ |
| 10 | 탭 고정 | 📌 아이콘, 고정 탭 닫기 불가 (`pinnedTabs: Set<string>`) | 소 | ✅ |
| 11 | 분할 에디터 | 오른쪽으로 분할 / 왼쪽으로 분할 / 분할 및 이동 3종, 드래그 구분선, 독립 Monaco 인스턴스 | 중 | ✅ |
| 12 | AI Agent 채팅 패널 | 에디터 오른쪽 사이드 (width:320px), 채팅 버블 UI, 타이핑 인디케이터, 자연어 → SQL 제안 → 에디터 삽입 | 중 | ✅ |
| 13 | MySQL 스타일 에디터 툴바 | 에디터 상단 툴바 — SQL 파일 열기(폴더 아이콘), SQL 파일로 저장(플로피 아이콘), 번개 아이콘 실행 버튼 | 소 | ✅ |
| 14 | 패널 토글 버튼 (3종) | 탭바 우측 — 왼쪽 사이드바 토글, 하단 결과 패널 토글, 오른쪽 패널(비활성) | 소 | ✅ |
| 15 | 분할 탭 왼쪽 바 이동·복원 | 오른쪽 분할 시 해당 탭을 왼쪽 탭바에서 제거(stash), 분할 닫으면 원래 위치로 복원 (`splitTabStash`) | 소 | ✅ |
| 16 | 결과 컬럼 자동 너비 | Canvas `measureText` (Consolas + Malgun Gothic) 로 각 컬럼의 헤더/데이터 최대 px 측정 후 동적 배치, 한글·CJK 정확 측정 | 소 | ✅ |
| 17 | Ctrl+Enter stale closure 수정 | `runQueryRef`로 매 렌더링마다 최신 `runQuery` 유지 → 탭 전환 후에도 항상 현재 탭 기준 실행 | 소 | ✅ |
| 18 | 저장 버튼 WebView2 호환 수정 | `a.click()` 전 `document.body.appendChild(a)` 추가 → Tauri WebView2 환경에서 파일 다운로드 정상 동작 | 소 | ✅ |
| 19 | 쿼리 진행 바 표시 수정 | `.result-tab-bar`에 `position: relative` 추가(절대 위치 기준점 수정), 최소 400ms 표시 보장 | 소 | ✅ |
| 20 | 라이트 모드 | CSS 변수 기반 테마 시스템 구축, 다크/라이트 토글 버튼, Monaco Editor 테마 전환 (`vs-dark` ↔ `vs`), localStorage 저장 | 중 | 🔲 |

---

## 3. AI (rustdb-mcp)

> 프로젝트 핵심 차별점 — 이게 없으면 "MCP 기반"이라는 제목이 성립 안 됨

### 졸업 전 완료 권장

| # | 항목 | 설명 | 난이도 | 상태 |
|---|------|------|--------|------|
| 1 | MCP 서버 구현 | Claude API 연동 — 자연어 입력 → SQL 생성 → RustDB 실행 → 결과 반환 파이프라인 | 중 | ✅ |
| 2 | 스키마 컨텍스트 주입 | SHOW TABLES / DESCRIBE 결과를 Claude 프롬프트에 자동 삽입 → 정확한 SQL 생성 | 소 | ✅ |
| 3 | UI 연동 | AI 뷰에서 자연어 입력 → MCP 서버 호출 → 결과 에디터 삽입 | 중 | ✅ |

### 졸업 이후 로드맵 (선택)

| # | 항목 | 설명 | 난이도 |
|---|------|------|--------|
| 1 | 대화형 SQL 생성 | 멀티턴 대화로 쿼리 점진적 수정 | 중 |
| 2 | 쿼리 설명 | 실행 결과를 자연어로 해석해 반환 | 소 |
| 3 | ERD 자동 분석 | 스키마 구조를 AI가 설명 | 소 |

---

## 4. 서버 (rustdb-server)

### 졸업 전 완료 권장

| # | 항목 | 설명 | 난이도 | 상태 |
|---|------|------|--------|------|
| 1 | MySQL 호환성 개선 | `SHOW VARIABLES` / `SHOW STATUS` / `SHOW COLLATION` 등 DBeaver가 접속 시 자동으로 보내는 쿼리 처리 → 심사에서 DBeaver 완전 연동 데모 가능 | 중 | ✅ |

### 졸업 이후 로드맵 (선택)

| # | 항목 | 설명 | 난이도 |
|---|------|------|--------|
| 1 | 에러 코드 표준화 | MySQL 에러 코드 번호 체계 (1064, 1146 등) — JDBC 드라이버 호환성 향상 | 중 |
| 2 | TLS / SSL | rustls 기반 암호화 연결 | 대 |
| 3 | 커넥션 풀 | 스레드 풀 + 큐 기반 내부 연결 관리 | 대 |
| 4 | BACKUP 복원 개선 | ON DELETE / ON UPDATE 액션 BACKUP SQL 포함, FK 순서 보장 | 소 |

---

## 5. Python 활용

> **방침**: 엔진·서버는 Rust 유지. Python은 주변 도구·AI 연동 전용.

### 즉시 활용 가능

| # | 항목 | 설명 | 난이도 |
|---|------|------|--------|
| 1 | 통합 테스트 자동화 | `pytest`로 `test_full.sql` 실행 후 출력 파싱 → 예상값 자동 비교 / CI 구축 | 소 |
| 2 | 벤치마크 스크립트 | `mysql-connector-python`으로 접속, INSERT/SELECT TPS 측정·그래프 출력 (`matplotlib`) | 소 |
| 3 | 더미 데이터 생성 | `faker` 라이브러리로 대용량 INSERT SQL 자동 생성 | 소 |

### MCP 서버 (rustdb-mcp)

```python
# rustdb-mcp 구조 예시
import anthropic, mysql.connector

client = anthropic.Anthropic()

def natural_to_sql(question: str, schema: str) -> str:
    response = client.messages.create(
        model="claude-opus-4-7",
        messages=[{
            "role": "user",
            "content": f"Schema:\n{schema}\n\nQuestion: {question}\n\nSQL:"
        }]
    )
    return response.content[0].text
```

| # | 항목 | 설명 | 난이도 |
|---|------|------|--------|
| 1 | MCP 서버 | Claude API 연동, 자연어 → SQL 변환 파이프라인 | 중 |
| 2 | 스키마 컨텍스트 관리 | SHOW TABLES / DESCRIBE 결과 자동 수집·주입 | 소 |
| 3 | 서버 모니터링 CLI | `SHOW BUFFER POOL` / `SHOW WAL` 주기 조회 → `rich` 터미널 대시보드 | 소 |

---

## 6. 기타

| # | 항목 | 설명 | 난이도 |
|---|------|------|--------|
| 1 | 성능 벤치마크 문서화 | INSERT/SELECT/JOIN TPS 측정 결과를 README에 수치로 공개 — "왜 Rust인가" 의 근거 | 중 |
| 2 | API 문서화 | `rustdoc` 기반 내부 API 문서 + SQL 레퍼런스 페이지 | 중 |
| 3 | 설치 패키지 | Windows `.msi` / macOS `.dmg` / Linux `.deb` 빌드 자동화 (GitHub Actions) | 중 |
| 4 | rustdb-client 개선 | `--file` 옵션, 출력 포맷 선택 (`--format table/csv/json`) | 소 |
