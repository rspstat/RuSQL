# DIFF — 변경 이력

## 2026-05-07

### 버그 수정

#### 1. `exec_insert` — 보조 인덱스 재빌드 누락 (executor.rs)

**증상**: `CREATE INDEX` → `INSERT` → `UNION SELECT` (보조 인덱스 경유) 시 0 rows 반환.

**원인**: `exec_insert`가 PK 인덱스 및 복합 인덱스는 갱신했지만, 단일 컬럼 보조 인덱스(`s.indexes`)를 재빌드하지 않았음. 인덱스가 빈 B+Tree 상태로 남아 `SecondaryPoint` 탐색 시 항상 `None` 반환.

**수정**: `exec_insert` 내 `sort_by_pk` 호출 직후 `rebuild_secondary_indexes(s, &table, &rows)` 추가.

```rust
self.sort_by_pk(s, &table);
let rows = s.tables.get(&table).unwrap().clone();
self.rebuild_secondary_indexes(s, &table, &rows);
s.buffer_pool.write_page(&table, rows);
```

---

#### 2. `is_covering_access` — 복합 인덱스 커버링 미인식 (planner.rs)

**증상**: `EXPLAIN SELECT dept_id, salary FROM emp WHERE dept_id = 1` 에서 `(Covering)` 미표시.

**원인**: `is_covering_access`가 단일 컬럼 인덱스만 검사 (선택 컬럼 전체가 인덱스 컬럼 하나와 일치하는지만 확인). 복합 인덱스(`idx_ds ON emp(dept_id, salary)`)로 커버 가능한 경우를 무시.

**수정**: 인스턴스 메서드로 변경 후 복합 인덱스 커버리지 검사 추가. 선택 컬럼 전체가 복합 인덱스의 컬럼 집합에 속하면 `is_covering = true`.

```rust
fn is_covering_access(&self, access: &AccessPath, columns: &[SelectColumn], table: &str) -> bool {
    // ... (단일 컬럼 검사 후)
    self.composite_indexes.values().any(|ci| {
        ci.table == table && selected.iter().all(|sc| ci.columns.iter().any(|ic| ic == sc))
    })
}
```

---

#### 3. GROUP BY + ORDER BY 집계 별칭 정렬 오작동 (executor.rs)

**증상**: `GROUP BY grade HAVING n >= 1 ORDER BY avg_sal DESC` 에서 `avg_sal` 기준 정렬이 적용되지 않음. 삽입 순서대로 반환.

**원인**: ORDER BY 정렬이 GROUP BY 실행 *전* 원시 행에 적용되었음. 집계 결과 행(`group_rows`)에는 `avg_sal` 별칭이 존재하지 않으므로 정렬이 무효.

**수정**: HAVING 필터 *후* `group_rows`에 ORDER BY 정렬 패스 추가. 숫자 비교 우선 적용.

```rust
if !order_by.is_empty() {
    group_rows.sort_by(|a, b| {
        for ord in &order_by {
            let av = Self::get_col(a, &ord.column).cloned().unwrap_or_default();
            let bv = Self::get_col(b, &ord.column).cloned().unwrap_or_default();
            let cmp = match (av.parse::<f64>(), bv.parse::<f64>()) {
                (Ok(af), Ok(bf)) => af.partial_cmp(&bf).unwrap_or(Ordering::Equal),
                _ => av.cmp(&bv),
            };
            let cmp = if ord.ascending { cmp } else { cmp.reverse() };
            if cmp != Ordering::Equal { return cmp; }
        }
        Ordering::Equal
    });
}
```

---

#### 4. `expect_col_ref` — SQL 키워드를 컬럼명으로 파싱 불가 (parser.rs)

**증상**: `ORDER BY avg DESC`, `GROUP BY count` 등 SQL 예약어를 컬럼/별칭 이름으로 사용 시 `Parse Error: Expected identifier, got Some(Avg)`.

**원인**: `expect_col_ref`가 `expect_ident`를 호출하는데, `expect_ident`는 `Token::Ident`만 수락. `avg`, `count`, `sum` 등 집계 함수명은 별도 키워드 토큰으로 렉싱됨.

**수정**: `expect_any_ident` 함수 추가 (키워드 토큰 → 문자열 변환). `expect_col_ref`가 `expect_any_ident`를 사용하도록 변경.

```rust
fn expect_any_ident(&mut self) -> Result<String, String> {
    match self.advance() {
        Some(Token::Ident(s))  => Ok(s.clone()),
        Some(Token::Count)     => Ok("count".to_string()),
        Some(Token::Sum)       => Ok("sum".to_string()),
        Some(Token::Avg)       => Ok("avg".to_string()),
        Some(Token::Min)       => Ok("min".to_string()),
        Some(Token::Max)       => Ok("max".to_string()),
        Some(Token::Now)       => Ok("now".to_string()),
        Some(Token::Date)      => Ok("date".to_string()),
        Some(Token::Key)       => Ok("key".to_string()),
        Some(Token::Set)       => Ok("set".to_string()),
        Some(Token::Index)     => Ok("index".to_string()),
        Some(Token::View)      => Ok("view".to_string()),
        other => Err(format!("Expected identifier, got {:?}", other)),
    }
}
```

---

### 테스트

`test/test_full.sql` — 260개 구문 전체 통과 (의도된 오류 3개: ENUM/SET 유효성 위반 제외).

---

## 2026-05-10

### 엔진: WAL Group Commit

**목적**: 여러 세션이 동시에 COMMIT할 때 fsync를 한 번으로 묶어 쓰기 TPS 향상.

**변경 파일**: `transaction/group_commit.rs` (신규), `transaction/wal.rs`, `transaction/txn_manager.rs`, `engine/executor.rs`

**핵심 아이디어**: 기존 커밋은 SharedDatabase 쓰기 락을 보유한 채 2회 fsync. Group Commit은 커밋을 3단계로 분리하여 fsync를 락 해제 후 수행.

#### 단계별 흐름

```
Phase 1 (SharedDatabase 락 보유)
  └─ SERIALIZABLE 검증
  └─ dirty page → buffer pool flush → disk
  └─ COMMIT 레코드 WAL 기록 (fsync 없음)
  └─ 락 해제

Phase 2 (락 없음 — GroupCommitCoordinator)
  └─ leader: yield_now() 후 단일 fsync
  └─ follower: leader fsync 완료 대기

Phase 3 (락 없음)
  └─ WAL 파일 삭제, undo log 클리어, 트랜잭션 상태 초기화
```

#### `GroupCommitCoordinator` (신규)

```rust
pub struct GroupCommitCoordinator {
    state: Mutex<GcState>,  // flushing: bool, generation: u64
    cvar:  Condvar,
}

pub fn sync_commit(&self) {
    // 첫 번째 도착 세션 → leader: yield_now() 후 fsync, notify_all
    // 이후 도착 세션 → follower: wait_while(generation == my_gen)
}
```

#### `wal.rs` — `log_commit_no_sync()` 추가

```rust
pub fn log_commit_no_sync(&self) {
    self.write_encoded(&Self::encode(&commit_record), false);  // sync=false
}
```

#### `txn_manager.rs` — commit 분리

```rust
pub fn commit_write_record(&mut self) -> Result<(), String> { /* WAL 기록만 */ }
pub fn commit_finalize(&mut self)                           { /* 상태 정리만 */ }
```

#### `executor.rs` — `execute()` 수정

```rust
pub fn execute(&mut self, stmt: Statement) -> Result<String, String> {
    if let Statement::Commit = stmt {
        return self.execute_commit_grouped();  // Group Commit 경로
    }
    // ... 기존 경로
}
```

**효과**: 단일 세션 — 오버헤드 없음 (leader가 즉시 fsync). 다중 세션 — N 커밋 ≈ 1 fsync.

---

### UI: 탭별 결과 보존 / Monaco SQL 자동완성 / 컬럼 너비 조절 (App.tsx)

#### 1. 탭별 결과 보존

`results: QueryResult[]` 단일 배열 → `tabResults: Record<tabId, QueryResult[]>` 구조로 교체. 탭 전환 시 결과 유지. 페이지 상태·컬럼 너비도 탭별로 독립 관리.

#### 2. Monaco SQL 자동완성

`registerCompletionItemProvider` 등록. `schemaRef`를 통해 실시간 스키마 반영.

- SQL 키워드 약 60개
- 현재 DB 테이블명 (`CompletionItemKind.Class`)
- 컬럼명 + 소속 테이블 (`CompletionItemKind.Field`, `detail`)

#### 3. 결과 테이블 컬럼 너비 조절

각 `th` 우측에 4px resize handle (절대 위치). 첫 드래그 시 DOM 실측값으로 전체 컬럼 너비 초기화, `table-layout: fixed` 전환.

---

### UI: ERD Editor 추가 / Table Browser 제거 (App.tsx, App.css)

#### 1. ERD Editor — 2번째 액티비티 바 아이콘

**목적**: 테이블 스키마와 FK 관계를 VS Code ERD Editor 스타일로 시각화. 기존 GUI 테이블 브라우저 뷰 대체.

**주요 기능**:
- `loadErd()`: `get_tables` + 병렬 `get_columns_detail` 호출, Grid 자동 배치 (`Math.ceil(Math.sqrt(N))` 열)
- 캔버스 팬(`erdCanvasDragRef`), 휠 줌(`erdZoom` 0.2–2.5), 카드 드래그(`erdCardDragRef`)
- 클릭 vs 드래그 구분: `erdCardWasDragged` ref, `Math.hypot(dx,dy) > 4px` 초과 시 드래그로 판정

#### 2. FK 관계선 버그 수정 (parseRef / unqualify)

**증상**: Refresh 후에도 FK 관계선이 화면에 표시되지 않음.

**원인 1 (parseRef)**: Rust 백엔드가 `fk_ref`를 `"db1.dept(id)"` 형식으로 전달. 기존 파서가 점(`.`)을 먼저 찾아 `{table:"db1", col:"dept(id)"}` 로 잘못 분리.

**수정**: 괄호(`(`) 위치를 먼저 확인하도록 순서 변경.

```typescript
function parseRef(ref: string) {
  const paren = ref.indexOf("(");
  if (paren > 0) return { table: ref.slice(0, paren), col: ref.slice(paren+1).replace(")", "") };
  const dot = ref.lastIndexOf(".");
  if (dot > 0) return { table: ref.slice(0, dot), col: ref.slice(dot+1) };
  return null;
}
```

**원인 2 (unqualify)**: `ref_table`이 `"db1.dept"` (DB 한정)로 오나, `erdColumns` 키는 `get_tables` 반환값인 `"dept"` (비한정). DB 접두사 불일치로 대상 카드 조회 실패.

**수정**: `unqualify()` 함수 추가 — 첫 번째 점 이후 부분만 추출.

```typescript
function unqualify(name: string): string {
  const dot = name.indexOf(".");
  return dot >= 0 ? name.slice(dot + 1) : name;
}
```

#### 3. 직각 꺾임 FK 관계선 (erdOrthPath)

**내용**: 베지어 곡선 → 직각 꺾임 H→V→H 경로 + `r=8` 라운드 코너 (Quadratic Bezier 아크).

```typescript
function erdOrthPath(x1, y1, x2, y2): string {
  const r = 8, midX = (x1+x2)/2;
  // 3-segment: H → V → H, 각 꺾임에 r 반경 아크
}
```

세 가지 케이스: 소스가 대상 왼쪽(→), 오른쪽(←), 수평 겹침(+44px 우회 경로).

#### 4. Table Browser 제거

액티비티 바 `"gui"` 뷰 및 관련 상태(`tables`, `_views`, `_indexes`, `guiTable`, `guiResult`, `guiLoading`, `guiFilter`), `loadGuiTable`, `handleGuiTableChange`, `.gui-*` CSS 약 220라인 제거.

#### 5. ERD 하단 테이블 데이터 브라우저 통합

**내용**: ERD 카드 클릭 시 하단 패널에서 해당 테이블 데이터 그리드 + 필터 표시. Table Browser 기능을 ERD 뷰 안에 통합.

- `erdSelectedTable`, `erdTableData`, `erdFilter`, `erdDataHeight` 상태 추가
- `loadErdTableData(tbl)`: `SELECT * FROM tbl` 실행 후 결과 표시
- `erdDataDragging` ref: 패널 상단 divider 드래그로 높이 조절 (`rect.bottom - mouseY - 22`, 클램프 80–`viewH-150`)
- 같은 카드 재클릭 시 패널 닫힘

---

### 네트워크: TCP 인증 (AUTH 핸드셰이크) + rustdb-client CLI

**변경 파일**: `rustdb-core/src/engine/executor.rs`, `rustdb-server/src/main.rs`, `rustdb-client/` (신규)

#### 프로토콜

```
Server → Client:  배너(---END---)
Client → Server:  AUTH username password\n
Server → Client:  OK authenticated as 'root'\n---END---
                  또는 ERR Access denied for user 'root'\n---END---
이후: 기존 쿼리/응답 루프 (---END--- 구분자)
```

#### `SharedDatabase` 메서드 추가 (executor.rs)

```rust
pub fn validate_credentials(&self, user: &str, password: &str) -> bool {
    if self.users.is_empty() { return true; }  // users 없음 → open 모드
    self.users.iter().any(|u| {
        u.user == user && match &u.password_hash {
            None       => password.is_empty(),
            Some(hash) => hash == password,
        }
    })
}

pub fn ensure_default_user(&mut self) -> bool {
    if self.users.is_empty() {
        // root / root 자동 생성 후 _users.json 영속화
        ...
        true
    } else { false }
}
```

#### `rustdb-server/main.rs` 변경

- `handle_client`: 배너 전송 후 `AUTH user pass` 1행을 읽어 `validate_credentials` 검증. 실패 시 `ERR` 전송 후 연결 종료.
- `main()`: 서버 시작 시 `ensure_default_user()` 호출 — `_users.json`이 비어있으면 `root`@`%` / `root` 자동 생성 + 로그 출력.

#### `rustdb-client` 크레이트 (신규, 외부 의존성 없음)

```bash
cargo run -p rustdb-client -- -u root -p root -h 127.0.0.1 -P 7878
```

- 배너 수신 → `AUTH` 전송 → 세션 REPL
- 멀티라인 SQL 지원 (`;` 감지 후 전송)
- `count_semicolons()`: 주석·문자열 안 `;` 제외, 다중 쿼리 응답 수 계산
- ANSI 색상 (OK=white, ERR=red bold)
- `\help`, `\status`, `exit`/`quit` 내장 명령

---

### 버그픽스: Tauri 내장 TCP 서버 AUTH 동기화 (src-tauri/src/main.rs)

**증상**: UI 3번째 아이콘에서 서버 시작 후 `rustdb-client`로 접속 시 연결 실패.

**원인**: `rustdb-server`에는 AUTH 핸드셰이크를 추가했지만, Tauri 앱 내장 TCP 서버의 `handle_client`는 구버전 프로토콜(배너 후 쿼리 직수신)을 그대로 사용. `rustdb-client`가 보내는 `AUTH user pass` 라인을 SQL로 파싱해 오류 반환.

**수정**:
- `handle_client`: 배너(`---END---`) 전송 후 `AUTH user pass` 수신 → `validate_credentials()` 검증 → OK/ERR 응답. 쿼리 응답도 `OK/ERR + (elapsed) + ---END---` 형식으로 통일.
- `main()`: `shared.write().unwrap().ensure_default_user()` 추가 — 앱 시작 시 users 없으면 `root`/`root` 자동 생성.

**결과**: `rustdb-server`와 Tauri 내장 서버가 동일한 AUTH 프로토콜을 사용, `rustdb-client`로 양쪽 모두 접속 가능.

---

### UI: Server Manager 뷰 재설계 (App.tsx, App.css)

**목적**: 기존 카드 3개 나열 방식 → DBeaver 연결 구성 다이얼로그 스타일로 전면 재설계.

#### 변경 내용

| 항목 | 이전 | 이후 |
|------|------|------|
| 레이아웃 | 가로 카드 3개 (STATUS / CONFIG / GUIDE) | 단일 연결 구성 폼 (max-width 660px, 중앙 정렬) |
| 아이콘 | 노란 DB 실린더 SVG | 서버 랙 SVG (2U 유닛, 드라이브 베이, 초록·파란 LED, 전원 버튼, USB 포트) |
| 포트 입력 | 숫자 인풋 | −/+ 버튼이 붙은 포트 스테퍼 |
| 비밀번호 | 없음 | 👁 토글로 표시/숨기기 |
| 탭 구조 | 없음 | ⚙ 메인 / ☰ CLI 가이드 |
| 사용자 입력 | 없음 | 이름·그룹·사용자·비밀번호 필드 |
| 상태 표시 | 별도 카드 | 폼 하단 인라인 (● RUNNING / ○ STOPPED) |
| 버튼 | Start / Stop (카드 내) | ▶ 서버 시작 / ■ 중지 / 저장 (action row) |

#### 신규 상태

```typescript
const [srvConnName, setSrvConnName] = useState("RustDB Local");
const [srvUser, setSrvUser]         = useState("root");
const [srvPass, setSrvPass]         = useState("root");
const [srvTab, setSrvTab]           = useState<"main" | "guide">("main");
const [srvPassVisible, setSrvPassVisible] = useState(false);
```

---

## 이전 변경 이력

git log 참조.
