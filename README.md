## MCP 기반 커스텀 RDBMS

- Rust로 구현한 데이터베이스 엔진 + RDBMS + AI MCP 

<br/>

## 핵심 기능

| 분류 | 내용 |
|------|------|
| DB 엔진 | B+Tree, WAL, Buffer Pool, 트랜잭션 |
| SQL 지원 | DDL / DML / JOIN / 제약조건 / 트랜잭션 |
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
- [x] CREATE TABLE / DROP TABLE / TRUNCATE TABLE
- [x] ALTER TABLE (ADD / DROP / RENAME COLUMN)
- [x] CREATE INDEX / DROP INDEX
- [x] CREATE VIEW / DROP VIEW

### DML
- [x] INSERT
- [x] SELECT
- [x] UPDATE
- [x] DELETE

### 쿼리 기능
- [x] WHERE (=, !=, >, <, >=, <=)
- [x] AND / OR 복합 조건
- [x] BETWEEN / LIKE
- [x] INNER JOIN
- [x] ORDER BY (ASC / DESC)
- [x] GROUP BY
- [x] HAVING
- [x] LIMIT
- [x] 집계 함수 (COUNT, SUM, AVG, MIN, MAX)
- [x] 서브쿼리 (WHERE col IN (SELECT ...))
- [x] 중첩 서브쿼리 (WHERE col > (SELECT AVG(...)))
- [x] SHOW TABLES / DESCRIBE
- [x] WHERE IS NULL / IS NOT NULL

### 제약 조건
- [x] PRIMARY KEY
- [x] NOT NULL
- [x] UNIQUE
- [x] AUTO INCREMENT
- [x] FOREIGN KEY RESTRICT (삭제 거부)
- [x] FOREIGN KEY CASCADE (연쇄 삭제)
- [x] FOREIGN KEY SET NULL (NULL 변경)

### 트랜잭션
- [x] WAL (Write-Ahead Logging) - 바이너리 redo log
- [x] BEGIN / COMMIT / ROLLBACK
- [x] Undo Log 기반 롤백
- [x] 트랜잭션 내부 작업만 WAL 기록
- [x] WAL 기반 Crash Recovery (재시작 시 자동 복구)  ← 추가

### 인덱스 & 저장
- [x] B+Tree 인덱스
- [x] 바이너리 디스크 저장 (.rdb 포맷, 16KB 페이지)
- [x] Buffer Pool (LRU 캐시, 64페이지)
- [x] TRUNCATE 후 AUTO INCREMENT 리셋  ← 추가

### 모니터링
- [x] SHOW BUFFER POOL (캐시 히트율, 사용량)
- [x] SHOW WAL (로그 레코드, 파일 크기)

### 편의 기능
- [x] 세미콜론(;) 구분 멀티 쿼리 입력
- [x] REPL 터미널 인터페이스

### UI (rustdb-ui)
- [x] Tauri + React 데스크탑 앱
- [x] Monaco Editor (SQL 문법 강조)
- [x] 사이드바 테이블 / 컬럼 목록
- [x] 멀티 쿼리 결과 표시
- [x] 쿼리 자동 저장
- [x] 결과창 크기 조절 (드래그)

<br/>

## 진행 예정

### 엔진 고도화
- [ ] 클러스터드 인덱스
- [ ] 복합 인덱스
- [ ] MVCC
- [ ] 트랜잭션 격리 수준 (4단계)
- [ ] Row-level locking (동시성)
- [ ] 체크포인트

### 네트워크
- [ ] TCP 서버 (포트 7878)
- [ ] 멀티 클라이언트 동시 접속 (스레드 per 클라이언트)
- [ ] 클라이언트 CLI (`rustdb-client`)

### MCP 연동
- [ ] AI API 클라이언트 (`mcp/client.rs`)
- [ ] 자연어 → SQL 변환 (`\ai` 명령어)
- [ ] 변환된 SQL 확인 후 실행

### UI
- [ ] VIEW / INDEX 사이드바 표시
- [ ] 쿼리 히스토리
- [ ] 결과 CSV 내보내기
- [ ] 다크 / 라이트 테마 전환

### 저장소
- [ ] 데이터 압축 (.rdb 파일)

<br/>

## 실행 방법
```bash
# REPL 모드
cargo run -p rustdb-cli

# 서버 모드
cargo run -p rustdb-server

# UI 모드
cd rustdb-ui && npm run tauri dev
```

<br/>

## 지원 SQL 문법 예시
```sql
CREATE TABLE users (id INT PRIMARY KEY AUTO INCREMENT, name TEXT NOT NULL, age INT);
CREATE TABLE orders (id INT AUTO INCREMENT, user_id INT REFERENCES users(id) ON DELETE CASCADE, amount INT);
INSERT INTO users VALUES (, Alice, 25);
SELECT * FROM users WHERE age BETWEEN 20 AND 30;
SELECT * FROM users WHERE name LIKE 'A%';
SELECT * FROM users WHERE age > 20 AND age < 35;
SELECT COUNT(*), AVG(age) FROM users;
SELECT * FROM users WHERE id IN (SELECT id FROM users WHERE age > 30);
SELECT * FROM users WHERE id IN (SELECT user_id FROM orders WHERE amount > (SELECT AVG(amount) FROM orders));
BEGIN; UPDATE users SET age = 26 WHERE id = 1; COMMIT;
ALTER TABLE users ADD COLUMN email TEXT;
CREATE INDEX idx_age ON users (age);
CREATE VIEW adult_users AS SELECT * FROM users WHERE age >= 18;
SHOW TABLES;
DESCRIBE users;
TRUNCATE TABLE users;
SHOW BUFFER POOL;
SHOW WAL;
```

<br/>

## 기술 스택

| 항목 | 내용 |
|------|------|
| 언어 | Rust |
| 버전 | v2.1.3 |
| 인덱스 | B+Tree |
| 트랜잭션 | WAL (바이너리 redo log) + Undo Log |
| 캐시 | Buffer Pool (LRU, 64페이지, 16KB) |
| 저장 | 바이너리 .rdb 포맷 |
| UI | Tauri + React + Monaco Editor |
| AI 연동 | MCP AI API (예정) |

<br/>

## 프로젝트 구조
```
code/
├── rustdb-core/     DB 엔진 라이브러리
├── rustdb-server/   TCP 서버
├── rustdb-cli/      터미널 REPL
└── rustdb-ui/       Tauri + React UI
```

<br/>

## Format (1.3 ver.)
```
┌─────────────────────────────────┐
│         rustdb-core             │
│                                 │
│  Lexer → Parser → AST           │
│              ↓                  │
│          Executor               │
│    ┌─────────────────┐          │
│    │ CREATE / DROP   │          │
│    │ INSERT / SELECT │          │
│    │ UPDATE / DELETE │          │
│    │ JOIN / WHERE    │          │
│    │ AND / OR / LIKE │          │
│    │ BETWEEN / IN    │          │
│    │ ORDER BY / LIMIT│          │
│    │ GROUP BY / HAVING│         │
│    │ COUNT/SUM/AVG.. │          │
│    │ ALTER TABLE     │          │
│    │ INDEX / VIEW    │          │
│    │ 중첩 서브쿼리   │          │
│    │ COMMIT/ROLLBACK │          │
│    │ PK/FK/NN/UNIQUE │          │
│    │ AUTO INCREMENT  │          │
│    │ FK CASCADE/NULL │          │
│    └─────────────────┘          │
│          ↓                      │
│    B+Tree 인덱스                 │
│    WAL 바이너리 redo log         │
│    Buffer Pool (LRU 64p 16KB)   │
│    바이너리 .rdb 저장            │
│                                 │
└─────────────────────────────────┘
        ↓              ↓
  rustdb-cli      rustdb-server
  (터미널 REPL)   (TCP 서버)
        ↓
  rustdb-ui
  (Tauri + React)
```

<br/>

## B+ Tree에 관하여
[B+ Tree 구조 이해](https://chanho0912.tistory.com/109)

[B+ Tree 이해 - velog](https://velog.io/@emplam27/%EC%9E%90%EB%A3%8C%EA%B5%AC%EC%A1%B0-%EA%B7%B8%EB%A6%BC%EC%9C%BC%EB%A1%9C-%EC%95%8C%EC%95%84%EB%B3%B4%EB%8A%94-B-Plus-Tree)