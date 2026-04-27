# RustDB — 발표 준비 자료

---

## 예상 질문

**Q. 모듈, 컴포넌트가 몇 개인가?**

크레이트(컴포넌트) 5개 — rustdb-core, rustdb-cli, rustdb-server, rustdb-ui, rustdb-mcp(예정)
rustdb-core 내부 모듈 5개 — parser, engine, storage, transaction, catalog

---

**Q. 데이터베이스 엔진 고도화 및 최적화 작업에는 무엇이 있는가?**

Clustered Index, 트랜잭션 격리 수준 4단계, Row-level Locking + 데드락 감지, MVCC, 비용 기반 쿼리 옵티마이저 (AccessPath 선택 + Hash Join / Nested Loop 자동 선택), LZ4 데이터 압축

---

**Q. 이 최적화 및 고도화 작업이 완료된 건 무엇이고 안 된 건 무엇인가?**

완료 : Clustered Index, 격리 수준 4단계, Row-level Locking + 데드락 감지, MVCC, 비용 기반 옵티마이저, LZ4 압축
미완료(예정) : GRANT / REVOKE 권한 제어, 비밀번호 인증, rustdb-client TCP 클라이언트

---

**Q. TCP 포트를 7878로 잡은 이유?**

Rust 공식 학습서 "The Rust Programming Language"의 TCP 서버 예제에서 관례적으로 사용하는 포트입니다. MySQL(3306), PostgreSQL(5432) 등 기존 RDBMS와 충돌을 피하면서 Rust 프로젝트임을 상징적으로 나타냅니다. `--port` 옵션으로 변경 가능합니다.

---

**Q. Rust를 선택한 이유?**

메모리 안전성을 컴파일 타임에 보장합니다. null 포인터 역참조, 데이터 경쟁 같은 버그를 런타임이 아닌 컴파일 단계에서 차단하며, GC 없이도 C/C++ 수준 성능을 냅니다. RDBMS처럼 데이터 무결성이 최우선인 시스템에 적합합니다.

---

**Q. MySQL 같은 기존 RDBMS가 있는데 왜 직접 만드는가?**

기존 RDBMS를 쓰는 것은 데이터베이스를 활용하는 것이고, 이 프로젝트는 데이터베이스가 어떻게 동작하는지를 직접 구현하며 이해하는 것입니다. 또한 MCP 기반 AI 자연어 쿼리라는 기존 RDBMS에 없는 차별화된 기능 구현도 목표입니다.

---

**Q. B+Tree를 사용한 이유? B-Tree와의 차이?**

B+Tree는 모든 데이터를 리프 노드에만 저장하고 리프 노드들이 연결 리스트로 이어져 범위 탐색(BETWEEN, ORDER BY)에 효율적입니다. B-Tree는 내부 노드에도 데이터가 있어 범위 조회 시 비효율적입니다. MySQL InnoDB, PostgreSQL도 B+Tree를 표준으로 사용합니다.

---

**Q. WAL이란 무엇이고 왜 사용하는가?**

데이터를 디스크에 쓰기 전에 먼저 로그를 기록하는 기법입니다. 시스템이 갑자기 꺼져도 재시작 시 WAL을 replay하여 데이터를 복구합니다. INSERT / UPDATE / DELETE를 바이너리로 기록하며 512KB마다 자동 체크포인트로 파일 크기를 관리합니다.

---

**Q. MVCC란 무엇인가?**

Multi-Version Concurrency Control. 각 행에 _xmin(생성 트랜잭션 ID), _xmax(삭제 트랜잭션 ID)를 저장하여 트랜잭션마다 해당 시점의 스냅샷을 보게 합니다. 읽기가 쓰기를 차단하지 않아 동시성이 향상되고 ROLLBACK이 빠릅니다.

---

**Q. 다중 사용자가 동시에 접속하면 데이터 충돌은 어떻게 처리하는가?**

Arc<Mutex<Executor>>로 쿼리 실행을 직렬화하고, SELECT FOR UPDATE / UPDATE / DELETE 시 Row-level Lock을 획득합니다. 충돌 시 wait-for 그래프로 데드락을 감지하여 victim 트랜잭션을 즉시 중단합니다.

---

**Q. AI MCP란 무엇인가?**

Model Context Protocol. AI 모델이 외부 도구나 데이터에 접근하도록 Anthropic이 제안한 표준 프로토콜입니다. 자연어 질의와 DB 스키마를 AI에 전달하면 AI가 적절한 SQL로 변환하여 반환합니다.

---

**Q. GUI는 어떤 기술로 만들었는가?**

Tauri + React + TypeScript. Tauri는 Rust 기반 데스크탑 앱 프레임워크로 Electron보다 메모리 사용량이 훨씬 적습니다. 에디터는 VSCode와 동일한 Monaco Editor를 사용합니다.

---

**Q. 현재 가장 어려웠던 점은?**

트랜잭션 격리 수준 구현입니다. READ COMMITTED는 매 쿼리마다 최신 데이터를, REPEATABLE READ는 트랜잭션 시작 시점 스냅샷을, SERIALIZABLE은 팬텀 읽기 방지까지 구현해야 합니다. MVCC + Undo Log + 스냅샷을 조합하는 과정이 복잡했습니다.

---

**Q. Clustered Index와 일반 Index의 차이는?**

Clustered Index는 데이터가 PK 기준으로 물리적으로 정렬되어 저장됩니다. PK 조회 시 별도 탐색 없이 바로 접근 가능합니다. Secondary Index는 인덱스 키와 PK만 저장하고 실제 데이터는 별도로 읽어야 합니다. sort_by_pk()로 삽입마다 물리 정렬을 유지합니다.

---

**Q. 비용 기반 옵티마이저는 어떻게 동작하는가?**

QueryPlanner::plan()이 실행 전 각 테이블의 접근 비용을 추정합니다. 인덱스 유무에 따라 PkPoint / PkRange / SecondaryIndex / SeqScan 경로를 선택하고, Join 시 행 수를 비교하여 대용량 Equi-Join은 Hash Join(O(N+M)), 소규모/비등가 조건은 Nested Loop Join을 선택합니다.

---

**Q. 데이터 압축은 왜 하는가? 성능에 영향은?**

디스크 I/O를 줄이기 위해서입니다. LZ4는 압축/해제 속도가 매우 빠른 알고리즘으로, 압축 CPU 오버헤드보다 디스크 읽기 감소 효과가 큽니다. 기존 비압축 .rdb도 자동 인식하여 하위 호환성을 유지합니다.

---

**Q. 트랜잭션 ROLLBACK은 어떻게 구현되어 있는가?**

Undo Log에 변경 전 데이터를 저장해두고, ROLLBACK 시 Undo Log를 역순으로 적용합니다. B+Tree 인덱스도 함께 재빌드하여 완전한 원상 복구를 보장합니다. SAVEPOINT 이후의 Undo Log만 적용하는 부분 롤백도 지원합니다.

---

**Q. VACUUM은 무엇인가?**

MVCC에서 _xmax가 설정된 행(논리 삭제된 행)을 물리적으로 제거하는 작업입니다. 논리 삭제만으로는 디스크 공간이 계속 늘어나므로 VACUUM으로 실제 정리합니다. `VACUUM [table]` 명령으로 수동 실행합니다.

---

**Q. Buffer Pool은 왜 필요한가?**

디스크 I/O는 메모리 접근보다 수천 배 느립니다. Buffer Pool은 LRU 캐시(64페이지)로 자주 접근하는 데이터를 메모리에 유지하여 디스크 접근 횟수를 줄입니다. SHOW BUFFER POOL로 히트율을 확인할 수 있습니다.

---

## 용어 정리

**RDBMS (Relational Database Management System)**
관계형 데이터베이스 관리 시스템. 데이터를 테이블(행/열) 형태로 저장하고 SQL로 조작하는 시스템.

---

**SQL (Structured Query Language)**
데이터베이스를 조작하기 위한 표준 질의 언어. DDL(구조), DML(데이터), DCL(제어)로 구분.

---

**DDL (Data Definition Language)**
데이터 구조를 정의하는 SQL. CREATE TABLE, DROP TABLE, ALTER TABLE, CREATE INDEX, CREATE VIEW 등.

---

**DML (Data Manipulation Language)**
데이터를 조작하는 SQL. SELECT, INSERT, UPDATE, DELETE.

---

**DCL (Data Control Language)**
트랜잭션 제어 및 권한 관련 SQL. BEGIN, COMMIT, ROLLBACK, GRANT, REVOKE 등.

---

**AST (Abstract Syntax Tree)**
SQL 문자열을 파싱한 결과인 추상 구문 트리. 각 SQL 구문이 구조화된 데이터 형태로 표현된 것. Executor가 이 트리를 해석하여 실행한다.

---

**Lexer (어휘 분석기)**
SQL 문자열을 최소 단위의 토큰(키워드, 식별자, 연산자, 리터럴)으로 분리하는 모듈.

---

**Parser (구문 분석기)**
Lexer가 만든 토큰 스트림을 AST로 변환하는 모듈. 재귀 하강(Recursive Descent) 방식 사용.

---

**Executor (실행기)**
AST를 해석하여 실제 데이터 조작을 수행하는 핵심 모듈. 쿼리 실행, 트랜잭션 제어, 인덱스 갱신 등 모든 DB 동작의 중심.

---

**WAL (Write-Ahead Log)**
데이터를 디스크에 쓰기 전에 반드시 로그를 먼저 기록하는 기법. 시스템 장애 시 로그를 재실행(replay)하여 데이터를 복구한다. RustDB는 바이너리 형식으로 기록하며 파일명은 `rustdb.wal`.

---

**Checkpoint (체크포인트)**
WAL 파일의 크기를 줄이기 위해 현재 상태를 안전하게 저장하고 WAL 일부를 삭제하는 작업. 512KB마다 자동 수행.

---

**Crash Recovery (장애 복구)**
비정상 종료 후 재시작 시 WAL을 자동으로 replay하여 데이터를 복구하는 기능.

---

**Transaction (트랜잭션)**
하나의 논리적 작업 단위. BEGIN으로 시작하고 COMMIT(확정) 또는 ROLLBACK(취소)으로 종료한다. 원자성(Atomicity)을 보장한다.

---

**ACID**
트랜잭션의 4가지 특성.
- Atomicity (원자성) : 전부 성공하거나 전부 실패
- Consistency (일관성) : 트랜잭션 전후 DB 상태가 일관성 유지
- Isolation (격리성) : 동시 트랜잭션이 서로 간섭하지 않음
- Durability (지속성) : COMMIT된 데이터는 장애 후에도 유지

---

**Isolation Level (격리 수준)**
동시 트랜잭션 간 데이터 가시성을 제어하는 수준.
- READ UNCOMMITTED : 다른 트랜잭션의 미커밋 데이터 읽기 허용
- READ COMMITTED : 커밋된 데이터만 읽음
- REPEATABLE READ : 트랜잭션 내 같은 쿼리는 항상 같은 결과
- SERIALIZABLE : 트랜잭션이 순차 실행된 것처럼 동작, 팬텀 읽기 차단

---

**Phantom Read (팬텀 읽기)**
같은 쿼리를 두 번 실행했을 때 두 번째 실행에서 새로운 행이 나타나는 현상. SERIALIZABLE 격리 수준에서 차단됨.

---

**MVCC (Multi-Version Concurrency Control)**
행마다 _xmin(생성 트랜잭션 ID), _xmax(삭제 트랜잭션 ID)를 부여해 동시 트랜잭션이 서로 다른 버전의 데이터를 보도록 하는 기법. 읽기와 쓰기가 서로를 차단하지 않아 동시성이 향상됨.

---

**Row-level Locking (행 단위 잠금)**
테이블 전체가 아닌 특정 행에만 잠금을 거는 방식. SELECT ... FOR UPDATE / UPDATE / DELETE 시 적용. 테이블 전체 잠금보다 동시성이 훨씬 높음.

---

**Deadlock (데드락)**
두 트랜잭션이 서로 상대방이 가진 잠금을 기다리며 무한 대기하는 상황. RustDB는 wait-for 그래프의 사이클을 DFS로 감지하여 victim 트랜잭션을 즉시 중단시킴.

---

**SAVEPOINT (세이브포인트)**
트랜잭션 내 특정 지점을 저장하여 그 지점까지만 롤백할 수 있게 하는 기능. SAVEPOINT name / ROLLBACK TO name / RELEASE name.

---

**Undo Log**
ROLLBACK 시 데이터를 원상 복구하기 위해 변경 전 값을 저장하는 로그.

---

**VACUUM**
MVCC에서 _xmax가 설정된 행(논리 삭제 행)을 디스크에서 물리적으로 제거하는 작업. 스토리지 공간 회수 목적.

---

**B+Tree (B+트리)**
모든 데이터를 리프 노드에만 저장하고 리프 노드들이 연결 리스트로 이어진 균형 트리 구조. 범위 탐색에 효율적이며 대부분의 RDBMS가 인덱스 구조로 사용. ORDER=4 (노드당 최대 키 4개).

---

**Clustered Index (클러스터드 인덱스)**
데이터가 PK 기준으로 물리적으로 정렬되어 저장되는 인덱스. PK 조회 시 별도 인덱스 탐색 없이 바로 데이터 접근 가능.

---

**Secondary Index (보조 인덱스)**
PK가 아닌 컬럼에 생성된 인덱스. 인덱스 키 → PK를 저장하고, PK를 통해 실제 데이터에 접근.

---

**Composite Index (복합 인덱스)**
두 개 이상의 컬럼을 조합하여 생성한 인덱스. `WHERE a = ? AND b = ?` 같은 다중 컬럼 조건에서 효율적.

---

**Buffer Pool (버퍼 풀)**
디스크에서 읽은 페이지를 메모리에 캐싱하는 LRU 기반 구조. 자주 접근하는 데이터를 메모리에 유지하여 디스크 I/O를 줄임. 64페이지 (각 16KB).

---

**Page (페이지)**
디스크 I/O의 최소 단위. RustDB에서는 16KB (PAGE_SIZE=16384). 테이블 데이터는 페이지 단위로 읽고 씀.

---

**Heap File / .rdb 파일**
테이블 데이터를 저장하는 바이너리 파일. RustDB에서 각 테이블마다 `.rdb` 파일로 저장되며, LZ4로 투명 압축됨. magic 번호 0x52444200.

---

**LZ4 압축**
매우 빠른 속도의 손실 없는(lossless) 압축 알고리즘. 압축/해제 속도가 빨라 실시간 디스크 I/O에 적합. RustDB에서 .rdb 파일 저장 시 투명하게 적용됨.

---

**Cost-Based Query Optimizer (비용 기반 쿼리 옵티마이저)**
쿼리 실행 전 여러 실행 계획의 비용을 추정하여 가장 효율적인 계획을 선택하는 모듈. AccessPath 선택과 Join 알고리즘 결정을 담당.

---

**AccessPath (접근 경로)**
테이블 데이터에 접근하는 방법.
- SeqScan : 전체 순차 탐색
- PkPoint : PK 단일 조회
- PkRange : PK 범위 조회
- SecondaryIndex : 보조 인덱스 사용
- CompositeIndex : 복합 인덱스 사용

---

**Hash Join**
두 테이블 중 작은 테이블로 해시 맵을 만들고 큰 테이블을 스캔하며 매칭하는 Join 방식. 시간 복잡도 O(N+M). 대용량 Equi-Join에 효율적.

---

**Nested Loop Join**
외부 테이블의 각 행에 대해 내부 테이블을 탐색하는 Join 방식. 소규모 테이블이나 비등가 조건(>, <, ≠)에서 사용.

---

**EXPLAIN**
쿼리 실행 계획을 출력하는 명령. `EXPLAIN SELECT ...` 형태로 사용. 비용, AccessPath, Join 알고리즘을 확인할 수 있음.

---

**CTE (Common Table Expression)**
`WITH name AS (SELECT ...)` 형태로 임시 결과 집합에 이름을 붙여 재사용하는 SQL 구문. 복잡한 쿼리를 단계적으로 표현할 때 유용.

---

**UNION / UNION ALL**
두 SELECT 결과를 합치는 연산. UNION은 중복 제거, UNION ALL은 중복 포함.

---

**Subquery (서브쿼리)**
쿼리 안에 중첩된 쿼리. WHERE IN / EXISTS / FROM 절에 사용. 외부 쿼리의 값을 참조하면 상관 서브쿼리(Correlated Subquery).

---

**CASE WHEN**
`CASE WHEN 조건 THEN 값 ELSE 기본값 END` 형태의 조건부 표현식. SELECT 컬럼이나 WHERE 절에서 사용.

---

**Auto Increment**
INSERT 시 PK 값을 자동으로 증가시키는 기능. TRUNCATE 시 카운터 리셋.

---

**Foreign Key (외래 키)**
다른 테이블의 PK를 참조하는 제약조건. ON DELETE / ON UPDATE 옵션으로 CASCADE(연쇄), RESTRICT(차단), SET NULL(null 설정) 동작을 지정.

---

**Indexing (인덱싱)**
특정 컬럼에 B+Tree 구조를 생성하여 탐색 속도를 높이는 것. 조회 성능은 O(log N)으로 향상되지만 INSERT/UPDATE 시 인덱스 갱신 비용이 발생.

---

**Arc<Mutex<>> (Rust)**
Rust에서 멀티스레드 환경에 안전하게 데이터를 공유하는 구조. Arc(Atomic Reference Count)는 여러 스레드가 같은 데이터를 공유하고, Mutex는 한 번에 하나의 스레드만 접근하도록 잠금을 걸어줌.

---

**Tauri**
Rust 기반 데스크탑 앱 프레임워크. 웹 기술(React, TypeScript)로 UI를 만들고 Rust로 백엔드를 구현. Electron보다 메모리 사용량이 훨씬 적음.

---

**Monaco Editor**
VSCode에서 사용하는 코드 에디터 컴포넌트. 문법 강조, 자동완성, 키보드 단축키 등을 지원. RustDB-UI에서 SQL 편집기로 사용.

---

**IPC (Inter-Process Communication)**
프로세스 간 통신. Tauri에서 프론트엔드(React)가 백엔드(Rust)를 호출할 때 사용. `invoke("함수명", 인자)` 형태.

---

**MCP (Model Context Protocol)**
Anthropic이 제안한 AI 모델과 외부 도구 간 표준 통신 프로토콜. AI가 DB 스키마를 이해하고 자연어를 SQL로 변환하는 데 활용.

---

## 모듈의 역할

### parser 모듈 (src/parser/)

SQL 문자열을 입력받아 실행 가능한 AST로 변환하는 역할.

**동작 흐름**
```
SQL 문자열 → Lexer(tokenize) → Vec<Token> → Parser(parse) → Statement(AST)
```

- **Lexer** : SQL 문자열을 순회하며 키워드, 식별자, 연산자, 리터럴 토큰으로 분리. `tokenize()` 호출 시 `Vec<Token>` 반환.
- **Parser** : 재귀 하강 방식으로 토큰 스트림을 소비하며 AST를 생성. `parse_select()`, `parse_insert()`, `parse_create()` 등 구문별 메서드를 가짐.
- **Statement (AST)** : 파싱 결과. Select, Insert, Update, Delete, CreateTable, AlterTable, Begin, Commit, Rollback, CreateDatabase, Use 등 모든 SQL 구문이 열거형으로 표현됨.
- **CondExpr / Condition** : WHERE 절 조건을 AND > OR 우선순위를 반영한 트리 구조로 표현.

---

### engine 모듈 (src/engine/)

쿼리 실행의 중심. Parser가 만든 AST를 받아 실제 데이터 조작을 수행.

**동작 흐름**
```
Statement(AST) → Executor::execute() → exec_select/insert/update/delete/...
→ BufferPool(데이터 읽기) → LockManager(잠금) → QueryPlanner(최적화) → 결과 반환
```

- **Executor** : 모든 SQL 실행의 진입점. catalog, indexes, views, txn, buffer_pool, disk, lock_mgr 필드를 보유하며 각 쿼리 유형에 맞는 exec_* 메서드를 호출. 초기화 시 disk에서 인덱스와 뷰를 자동 복원.
- **LockManager** : Row-level Lock 관리. 행(테이블명 + PK)마다 잠금 소유 트랜잭션을 기록하고, 충돌 시 wait-for 그래프를 생성하여 DFS로 데드락 사이클을 감지. COMMIT/ROLLBACK 시 해당 트랜잭션의 모든 잠금 해제.
- **QueryPlanner** : 쿼리 실행 전 각 테이블의 AccessPath와 비용을 추정하여 최적 실행 계획을 반환. 인덱스 유무, 조건 타입, 테이블 크기를 기반으로 Hash Join / Nested Loop Join을 자동 선택.

---

### storage 모듈 (src/storage/)

데이터의 실제 저장과 인덱싱을 담당.

**동작 흐름**
```
Executor → BufferPool::get_page(table, disk) → 캐시 히트: 메모리 반환
                                              → 캐시 미스: DiskManager::load_table → LRU 캐시 저장
쓰기: BufferPool::write_page → flush_page → DiskManager::save_table(.rdb + LZ4)
```

- **DiskManager** : .rdb 파일 읽기/쓰기. LZ4로 투명 압축/해제. 스키마(JSON), 인덱스 메타(indexes.json), 뷰(views.json) 영속화도 담당.
- **BufferPool** : LRU 기반 64페이지 메모리 캐시. 자주 접근하는 페이지를 메모리에 유지하여 디스크 I/O 감소. hit_count / miss_count로 성능 측정.
- **BPlusTree** : ORDER=4의 B+Tree 인덱스. 단일 컬럼 인덱스에 사용. 수치 인식 키 비교로 "10" > "9" 정상 처리.
- **CompositeIndex** : 다중 컬럼 인덱스. 컬럼 값 조합을 키로 사용.
- **Page** : 16KB 단위 디스크 저장 블록. magic 번호 0x52444200으로 파일 식별.
- **IndexMeta** : 인덱스 이름, 대상 테이블, 컬럼 목록을 저장. 재시작 시 이 정보로 B+Tree를 자동 재빌드.

---

### transaction 모듈 (src/transaction/)

트랜잭션 생명주기와 WAL 로그를 관리.

**동작 흐름**
```
BEGIN → txn.begin_with_snapshot(tables)
INSERT/UPDATE/DELETE → txn.log_insert/update/delete + undo_log 추가
COMMIT → wal.log_commit() + 잠금 해제
ROLLBACK → undo_log 역순 적용 → B+Tree 인덱스 재빌드 → 잠금 해제
```

- **TransactionManager** : 트랜잭션 상태(active, txn_id), Undo Log, WAL 참조, 격리 수준, 스냅샷, 세이브포인트 스택을 관리. `begin_with_snapshot()`으로 REPEATABLE READ / SERIALIZABLE용 스냅샷 복사.
- **WalManager** : 바이너리 WAL 파일(rustdb.wal) 관리. WalOp 열거형(Insert=0x01, Update=0x02, Delete=0x03, Commit=0xFF, Rollback=0xFE, Checkpoint=0xFD)으로 기록. 512KB 초과 시 자동 체크포인트.
- **UndoEntry** : 롤백을 위한 변경 전 데이터 기록. (operation, table, key, old_data) 구조.

---

### catalog 모듈 (src/catalog/)

테이블 스키마 정보를 메모리에서 관리.

**동작 흐름**
```
CREATE TABLE → Catalog::create_table(name, columns) → HashMap에 등록 → DiskManager로 JSON 영속화
쿼리 실행 시 → Catalog::get_table(name) → TableSchema 참조
```

- **Catalog** : 테이블 이름 → TableSchema 해시 맵. 다중 DB 지원 시 데이터베이스별로 독립적으로 관리.
- **TableSchema** : 테이블 이름, 컬럼 정의(ColumnDef), AUTO_INCREMENT 카운터를 보유. TRUNCATE 시 카운터 리셋.
- **ColumnDef** : 컬럼 이름, 데이터 타입(Int/Varchar/DateTime/Enum/Set 등), PK/NOT NULL/UNIQUE/DEFAULT/CHECK/FK 제약조건 정보 포함.
