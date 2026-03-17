## MCP 기반 커스텀 RDBMS

- Rust로 구현한 데이터베이스 엔진 + RDBMS + AI MCP 

<br/>

## 핵심 기능

| 분류 | 내용 |
|------|------|
| DB 엔진 | B+Tree, Buffer Pool, WAL, 트랜잭션 직접 구현 |
| SQL 지원 | DDL / DML / JOIN / 트랜잭션 |
| MCP | 자연어 입력 → SQL 자동 생성 → 실행 |
| DBMS | TCP 서버, 다중 클라이언트 동시 접속 |
| 언어 | Rust |

<br/>

## 완료된 기능

### 엔진 코어
- [x] Lexer / Tokenizer
- [x] SQL Parser (AST 기반)
- [x] Executor (쿼리 실행 엔진)

### DDL
- [x] CREATE TABLE
- [x] DROP TABLE
- [x] ALTER TABLE (ADD / DROP / RENAME COLUMN)

### DML
- [x] INSERT
- [x] SELECT
- [x] UPDATE
- [x] DELETE

### 쿼리 기능
- [x] WHERE (=, !=, >, <, >=, <=)
- [x] INNER JOIN
- [x] ORDER BY (ASC / DESC)
- [x] GROUP BY
- [x] LIMIT
- [x] 집계 함수 (COUNT, SUM, AVG, MIN, MAX)
- [x] 서브쿼리 (WHERE col IN (SELECT ...))

### 트랜잭션
- [x] WAL (Write-Ahead Logging)
- [x] BEGIN / COMMIT / ROLLBACK
- [x] Undo Log 기반 롤백

### 인덱스 & 저장
- [x] B+Tree 인덱스
- [x] JSON 기반 디스크 영속성

### 편의 기능
- [x] 세미콜론(;) 구분 멀티 쿼리 입력
- [x] REPL 터미널 인터페이스

<br/>

## 진행 예정

### 네트워크
- [ ] TCP 서버 (포트 7878)
- [ ] 멀티 클라이언트 동시 접속 (스레드 per 클라이언트)
- [ ] 클라이언트 CLI (`rustdb-client`)

### TUI / 출력
- [ ] 컬럼 색상 강조 (헤더 / 데이터 구분)
- [ ] 접속 정보 출력 (서버 주소, DB명, 접속 시간)
- [ ] 쿼리 실행 시간 표시

### MCP 연동
- [ ] Claude API 클라이언트 (`mcp/client.rs`)
- [ ] 자연어 → SQL 변환 (`\ai` 명령어)
- [ ] 변환된 SQL 확인 후 실행

### 저장소 고도화 (선택)
- [ ] 바이너리 페이지 저장 (.rdb 포맷)
- [ ] Buffer Pool (LRU 캐시)
- [ ] 체크포인트 (WAL 압축)

<br/>

## 실행 방법
```bash
# REPL 모드 (현재)
cargo run

# 서버 모드 (예정)
cargo run --bin rustdb-server

# 클라이언트 모드 (예정)
cargo run --bin rustdb-client
```

<br/>

## 지원 SQL 문법 예시
```sql
CREATE TABLE users (id INT, name TEXT, age INT);
INSERT INTO users VALUES (1, Alice, 25);
SELECT * FROM users WHERE age > 20 ORDER BY age DESC LIMIT 3;
SELECT COUNT(*), AVG(age) FROM users;
SELECT * FROM users WHERE id IN (SELECT id FROM users WHERE age > 30);
BEGIN; UPDATE users SET age = 26 WHERE id = 1; COMMIT;
ALTER TABLE users ADD COLUMN email TEXT;
```

<br/>

## 기술 스택

| 항목 | 내용 |
|------|------|
| 언어 | Rust |
| 인덱스 | B+Tree (직접 구현) |
| 트랜잭션 | WAL + Undo Log |
| 저장 | JSON (→ 바이너리 예정) |
| AI 연동 | Claude MCP API (예정) |

<br/>

## Format (1.1 ver.)

```
┌─────────────────────────────────┐
│         rustdb-mcp              │
│                                 │
│  Lexer → Parser → AST           │
│              ↓                  │
│          Executor               │
│    ┌─────────────────┐          │
│    │ CREATE / DROP   │          │
│    │ INSERT / SELECT │          │
│    │ UPDATE / DELETE │          │
│    │ JOIN / WHERE    │          │
│    │ ORDER BY / LIMIT│          │
│    │ GROUP BY        │          │
│    │ COUNT/SUM/AVG.. │          │
│    │ ALTER TABLE     │          │
│    │ 서브쿼리 (IN)   │           │
│    │ COMMIT/ROLLBACK │          │
│    └─────────────────┘          │
│          ↓                      │
│    B+Tree 인덱스                 │
│    WAL 로그                      │
│    JSON 디스크 저장              │
│                                 |
└─────────────────────────────────┘
```

<br/>

## B+ Tree에 관하여
[B+ Tree 구조 이해](https://chanho0912.tistory.com/109)
[B+ Tree 이해 - velog](https://velog.io/@emplam27/%EC%9E%90%EB%A3%8C%EA%B5%AC%EC%A1%B0-%EA%B7%B8%EB%A6%BC%EC%9C%BC%EB%A1%9C-%EC%95%8C%EC%95%84%EB%B3%B4%EB%8A%94-B-Plus-Tree)
