# RustDB 개발 계획 (v2.2.0 이후)

> 우선순위: 상 / 중 / 하  
> 난이도: 소 (1~2일) / 중 (3~7일) / 대 (1주+)

---

## 졸업작품 필수 항목

> 프로젝트 핵심 가치: **"MCP 기반 커스텀 RDBMS"** — AI 연동 + 완성도 높은 엔진 + 깔끔한 UI  
> 아래 항목이 모두 완료되어야 데모·심사에서 완결성을 인정받을 수 있음

### 엔진

| # | 항목 | 이유 | 난이도 |
|---|------|------|--------|
| 1 | 저장 프로시저 제어문 | `IF / ELSEIF / WHILE / BEGIN…END` 없이는 프로시저가 단순 단일문 실행에 불과 — 현재 CREATE PROCEDURE는 있지만 제어문 미지원 | 중 |
| 2 | 세션 변수 + PREPARE / EXECUTE | `SET @v = 1; PREPARE s FROM '…'; EXECUTE s USING @v;` — MySQL 기본 기능, DBeaver 접속 시 내부적으로 사용됨 | 중 |
| 3 | EXPLAIN 출력 포맷 수정 | 현재 `(dept_id =↵1)` 처럼 줄이 잘림 — 심사 데모에서 직접 보여주는 화면이므로 polish 필수 | 소 |

### 프론트 (UI)

| # | 항목 | 이유 | 난이도 |
|---|------|------|--------|
| 1 | AI 뷰 — 자연어 → SQL 변환 | 프로젝트 제목이 **"MCP 기반"** 인데 AI 기능이 없으면 핵심 차별점 없음 | 중 |
| 2 | 다크 / 라이트 테마 전환 | 현재 다크 고정 — 발표장 환경(빔프로젝터 밝은 화면)에서 가시성 문제, 구현 난이도 낮음 | 소 |
| 3 | 결과 셀 직접 편집 | 단순 조회 전용이 아닌 "DB 관리 도구"임을 증명하는 핵심 UX | 중 |
| 4 | 탭 이름 변경 | 더블클릭으로 탭에 이름 부여 — 발표 시 "쿼리1, 쿼리2" 대신 의미 있는 이름 사용 가능, 구현 난이도 낮음 | 소 |

### 기타

| # | 항목 | 이유 | 난이도 |
|---|------|------|--------|
| 1 | rustdb-mcp 완성 | 프로젝트의 핵심 구성 요소 — 자연어 → SQL 변환 파이프라인 완성 없이는 제목과 내용이 불일치 | 중 |
| 2 | MySQL 호환성 개선 | `SHOW VARIABLES`, `SHOW STATUS` 등 DBeaver가 접속 시 자동으로 보내는 쿼리 처리 → 심사에서 DBeaver 연동 데모 가능 | 중 |
| 3 | BACKUP 복원 완성 | 현재 BACKUP SQL에 `ON DELETE / ON UPDATE` 액션 누락 → 복원 후 FK 동작 달라짐, 수정 난이도 낮음 | 소 |
| 4 | 통합 테스트 자동화 | `test_full.sql` 예상 출력과 실제 출력 자동 비교 — 코드 품질·안정성을 심사위원에게 증명 | 중 |

---

## 전체 항목 (졸업 이후 로드맵)

### 엔진

| # | 항목 | 설명 | 우선순위 | 난이도 |
|---|------|------|----------|--------|
| 1 | 저장 프로시저 제어문 | `IF / ELSEIF / ELSE`, `WHILE`, `LOOP / LEAVE`, `BEGIN…END` 복합 본문, 변수 선언(`DECLARE`) | 상 | 중 |
| 2 | GAP Lock | SERIALIZABLE 격리 수준에서 범위 조건 팬텀 읽기를 롤백 대신 실제 GAP Lock으로 방지 | 상 | 대 |
| 3 | JSON 함수 보강 | `JSON_OBJECT`, `JSON_ARRAY`, `JSON_SET`, `JSON_REMOVE`, `JSON_KEYS`, `JSON_LENGTH`, `JSON_CONTAINS` | 상 | 중 |
| 4 | PREPARE / EXECUTE | `PREPARE stmt FROM '…'; SET @v = 1; EXECUTE stmt USING @v;` 사용자 세션 변수 + 파라미터 바인딩 | 상 | 중 |
| 5 | 부분 인덱스 | `CREATE INDEX idx ON t(col) WHERE condition` — 조건 만족 행만 인덱싱 | 중 | 중 |
| 6 | 표현식 인덱스 | `CREATE INDEX idx ON t(LOWER(name))` 등 함수 기반 인덱스 | 중 | 중 |
| 7 | GENERATED COLUMNS | `col INT GENERATED ALWAYS AS (a + b) STORED` — 가상/저장 파생 컬럼 | 중 | 중 |
| 8 | LATERAL JOIN | `FROM t1, LATERAL (SELECT … WHERE col = t1.id) sub` — FROM 절 상관 서브쿼리 | 중 | 중 |
| 9 | 정규식 인덱스 지원 개선 | REGEXP 조건에도 인덱스 힌트 적용 가능하도록 플래너 확장 | 중 | 중 |
| 10 | JOIN 최적화 고도화 | LEFT / RIGHT / FULL OUTER JOIN도 그리디 순서 재정렬 적용 | 중 | 중 |
| 11 | 완전한 MVCC | 튜플 레벨 다중 버전 → 불필요한 VACUUM 없이 일관된 스냅샷 읽기 | 중 | 대 |
| 12 | 이벤트 스케줄러 | `CREATE EVENT e ON SCHEDULE EVERY 1 HOUR DO …` — 주기적 SQL 자동 실행 | 중 | 중 |
| 13 | TLS / SSL | 서버-클라이언트 암호화 연결 (`rustls` 등) | 중 | 대 |
| 14 | 행 저장 → 페이지별 분할 | 현재 테이블 전체 단일 파일 → 페이지 단위 분할로 대용량 대응 | 하 | 대 |
| 15 | 파티셔닝 | `PARTITION BY RANGE (col)` — 날짜·범위 기반 파티션 테이블 | 하 | 대 |
| 16 | 복제 | WAL 스트리밍으로 읽기 복제본(Read Replica) 지원 | 하 | 대 |
| 17 | 서버 측 커넥션 풀 | 내부 연결 풀 관리 (스레드 풀 + 큐) | 하 | 대 |

---

## 프론트 (UI)

| # | 항목 | 설명 | 우선순위 | 난이도 |
|---|------|------|----------|--------|
| 1 | 다크 / 라이트 테마 전환 | 현재 다크 고정 → 헤더 토글 버튼, `localStorage` 저장 | 상 | 소 |
| 2 | 결과 셀 직접 편집 | 셀 더블클릭 → 인라인 입력 → `UPDATE t SET col = val WHERE pk = …` 자동 생성·실행 | 상 | 중 |
| 3 | 탭 이름 변경 | 탭 더블클릭 → 인라인 편집으로 탭에 이름 부여 | 상 | 소 |
| 4 | 테이블 시각적 편집기 | 사이드바 테이블 우클릭 → "Edit Table" → `ALTER TABLE` GUI (컬럼 추가/삭제/타입 변경) | 중 | 중 |
| 5 | ERD 자동 레이아웃 | FK 관계 기반 자동 배치 (Dagre / ELK 알고리즘 연동) | 중 | 중 |
| 6 | 결과 내보내기 확장 | CSV 외 JSON / Excel(`.xlsx`) 내보내기 옵션 추가 | 중 | 소 |
| 7 | 쿼리 북마크 폴더 | 현재 단일 목록 → 폴더/태그로 북마크 분류 | 중 | 소 |
| 8 | 자동완성 개선 | 컬럼 타입·제약 정보 표시, JOIN 대상 테이블 컬럼 자동완성, 서브쿼리 내 컨텍스트 인식 | 중 | 중 |
| 9 | 실행 취소 / 재실행 | 탭별 쿼리 에디터 Undo/Redo 히스토리 (Monaco 내장 활용) | 중 | 소 |
| 10 | 멀티 결과 탭 분리 | 세미콜론으로 구분된 멀티 쿼리 결과를 각각 별도 탭으로 표시 | 중 | 중 |
| 11 | ERD 테이블 필터 | ERD 뷰에서 테이블 이름 검색·필터로 원하는 테이블만 표시 | 중 | 소 |
| 12 | AI 뷰 | 자연어 입력 → SQL 변환 (Claude API 연동), 변환 결과 에디터에 삽입 | 하 | 중 |
| 13 | 쿼리 실행 진행 표시 | 장시간 쿼리 실행 시 스피너 + 경과 시간 표시, 취소 버튼 | 하 | 소 |
| 14 | 연결 프로파일 관리 | 여러 서버 연결 설정을 이름 붙여 저장·전환 | 하 | 소 |

---

## 기타

| # | 항목 | 설명 | 우선순위 | 난이도 |
|---|------|------|----------|--------|
| 1 | rustdb-mcp 완성 | `rustdb-mcp` 모듈에서 Claude API 연동, 자연어 → SQL → 실행 파이프라인 완성 | 상 | 중 |
| 2 | MySQL 호환성 개선 | DBeaver / JDBC / Workbench에서 더 많은 기능 동작 (메타데이터 쿼리, `SHOW VARIABLES`, `SHOW STATUS` 등) | 상 | 중 |
| 3 | 통합 테스트 자동화 | `test_full.sql` 기반 CI 테스트 (`cargo test`), 예상 출력과 실제 출력 자동 비교 | 상 | 중 |
| 4 | EXPLAIN 출력 개선 | 줄 잘림 없이 포맷 정렬, JSON 포맷 옵션 (`EXPLAIN FORMAT=JSON`) | 중 | 소 |
| 5 | BACKUP 복원 개선 | `ON DELETE / ON UPDATE` 액션 BACKUP SQL에 포함, 복원 후 FK 제약 순서 보장 | 중 | 소 |
| 6 | 에러 코드 표준화 | MySQL 에러 코드 번호 체계 적용 (1064, 1146 등) — JDBC 드라이버 호환성 향상 | 중 | 중 |
| 7 | 성능 벤치마크 | TPC-C 일부 또는 직접 설계한 벤치마크로 INSERT/SELECT/JOIN TPS 측정 및 문서화 | 중 | 중 |
| 8 | rustdb-client 개선 | 배치 실행, `--file` 옵션(`-f query.sql`), 출력 포맷 선택(`--format table/csv/json`) | 중 | 소 |
| 9 | 설치 패키지 | Windows `.msi` / macOS `.dmg` / Linux `.deb` 빌드 자동화 (GitHub Actions) | 하 | 중 |
| 10 | API 문서화 | `rustdoc` 기반 내부 API 문서 + 사용자 가이드 (SQL 레퍼런스 페이지) | 하 | 중 |
