# RuSQL 전시 시연 순서 (5분) — 2026-11-24

발표자용 한 장짜리입니다. 아래 순서와 SQL은 **실제 앱에서 그대로 돌려 본 것**이고(`py -3 code/test/frontend/cdp/rehearse_demo.py`, 2026-11-21, 9단계 모두 통과), 수치는 이 PC에서 잰 값입니다(전시 PC에서는 조금 달라질 수 있습니다). Claude Desktop 자체는 리허설하지 못했으니 **전시 전에 한 번 직접 해 보세요.**

- **필수 4분 40초** = 장면 1, 2, 3, 5, 7 · **선택 1분** = 장면 4, 6 (시간이 모자라면 건너뜁니다)
- 데이터: `docs/mds/EXHIBITION_setup.sql` → 데이터베이스 `expo` (고객 300행 · 주문 1,500행(고객을 가리키는 외래키) · `logs` 131,072행)
- 더 보여 주고 싶으면(강제 종료 뒤 복구, 교착): `code/test/demo/DEMO.md` — 2026-10-22 기준 옛 안내서이고 이번 리허설 대상은 아닙니다.

## 전시 전 준비 (한 번, 약 10분)

1. `RuSQL_2.3.0_x64-setup.exe`로 설치하고 실행합니다.
2. 홈 → **New Connection** → 이름만 적고(예: `RuSQL Demo`) **Add**. 사용자 root · 비밀번호 root · localhost · 7878이 기본값입니다. 연결을 눌러 들어갑니다.
3. SQL 편집기에 `EXHIBITION_setup.sql` **전체를 붙여 넣고 Run All** → `✓ 25 query(s)` (약 3초). 한 번만 합니다. (이미 `expo`가 있으면 `CREATE DATABASE`가 실패하니 `DROP DATABASE expo;`로 지우고 다시 합니다.)
4. Claude Desktop 연결: **Server Manager → 오른쪽 막대 MCP Agent → Auto-connect Claude Desktop** 을 누르고 **Claude Desktop을 다시 시작**합니다. Server Manager에서 **Start Server**(RUNNING)까지 눌러 둡니다. Claude Desktop은 인터넷이 필요합니다.
5. **백업 영상**: 장면 7(Claude)을 한 번 하면서 화면을 녹화해 둡니다(Windows: `Win + Alt + R`). 인터넷이나 연결이 안 될 때 이 영상을 틉니다.
6. 절전 · 화면 끄기 · 알림을 끄고, 충전기를 꽂아 둡니다. 시연은 **홈 화면(접속 전)** 에서 시작합니다.

## 5분 순서

| 시간 | 장면 | 하는 일 | 한 마디 | 화면에 나오는 것 |
|---|---|---|---|---|
| 0:00 | **1. 홈** (30초) | 4번째 버튼 **View Topology** → 원통에 마우스를 올려 봄 → `Esc` | "C++로 밑바닥부터 만든 관계형 DB입니다. 앱·터미널·MySQL 클라이언트가 같은 엔진에 붙습니다." | 서버 · 엔진 · 저장소 구조 그림 |
| 0:30 | **2. 질의와 차트** (1분) | 저장된 연결 클릭 → 편집기 → `USE expo;` → 조인 질의 `Ctrl+Enter` → 상태별 질의 → **Chart** 버튼 | "조인과 집계가 바로 되고, 결과는 CSV · JSON · 차트로 내보냅니다." | 1위 `Seoyeon M. · Daegu · 5건 · 323000`, 상태 3행과 막대 차트 |
| 1:30 | **3. 인덱스** (1분 30초) | ① `EXPLAIN ANALYZE …` ② `EXPLAIN …` → **INDEX ADVICE** 카드 → **Try it** → **Insert into editor** → **Run All** ③ ①을 다시 | "느린 이유를 엔진이 읽고 인덱스를 제안합니다. 만들기 전에 임시로 시험해 전후를 보여 줍니다." | ① Seq Scan 131,072행 **약 57ms** ② Try it **38.6ms → 0.2ms (약 200배)** ③ Index Scan **약 0.2ms** |
| 3:00 | *4. 문장의 여행* (30초, 선택) | 새 문장을 쓰고 **전체 선택** → 도구 막대의 **Journey** | "한 문장이 지나가는 단계를 느린 화면으로 다시 봅니다." | 11단계(Parse → 락 → 스캔 → … → 결과) |
| 3:30 | **5. 세션 · 락** (40초) | `BEGIN; UPDATE …` **Run All** → **Server Manager → Sessions** → 편집기로 돌아가 `ROLLBACK;` | "열린 트랜잭션과 쥐고 있는 행 락이 보이고, 막힌 세션은 `Stop statement`로 멈출 수 있습니다." | `txn … RUNNING · 3 row lock(s) · expo.orders #1 #2 #3` |
| 4:10 | *6. B+Tree · MVCC* (30초, 선택) | 왼쪽 활동 막대의 **B+Tree**, **MVCC** 아이콘 | "인덱스가 쪼개지는 모습과, 쓰는 쪽과 읽는 쪽이 서로 안 기다리는 모습입니다." | 스스로 재생되는 그림(한국어 / EN 전환) |
| 4:40 | **7. Claude + MCP** (1분) | Claude Desktop에 아래 질문 두 개를 말함 | "AI는 Claude Desktop을 MCP로 연결합니다. RuSQL이 도구 37개를 내줍니다." | 도시별 5행, 편집기에 새 탭이 열려 실행됨 |

## 복사해서 쓸 SQL

**장면 2**

```sql
USE expo;
```

```sql
SELECT c.name, c.city, COUNT(*) AS orders, SUM(o.total) AS total
FROM customers c JOIN orders o ON o.customer_id = c.id
GROUP BY c.id, c.name, c.city ORDER BY total DESC LIMIT 10;
```

```sql
SELECT status, COUNT(*) AS n, SUM(total) AS total FROM orders GROUP BY status;
```

**장면 3** — `SELECT *` 로 (아래 "주의" 참고)

```sql
EXPLAIN ANALYZE SELECT * FROM logs WHERE user_id = 777;
```

```sql
EXPLAIN SELECT * FROM logs WHERE user_id = 777;
```

Insert into editor가 넣어 주는 문장은 `CREATE INDEX idx_logs_user_id ON logs (user_id);` 입니다(약 1초). 만든 뒤 위 첫 문장을 다시 실행합니다. **장면 3을 다시 보여 주려면 인덱스를 지웁니다:**

```sql
DROP INDEX idx_logs_user_id ON logs;
```

**장면 4** — 쓰고 전체 선택 → Journey (방금 실행했어도 됩니다)

```sql
SELECT c.city, SUM(o.total) AS total FROM orders o JOIN customers c ON o.customer_id = c.id GROUP BY c.city ORDER BY total DESC;
```

**장면 5**

```sql
BEGIN;
UPDATE orders SET status = 'paid' WHERE id <= 3;
```

```sql
ROLLBACK;
```

**장면 7 — Claude Desktop에 이렇게 말합니다**

1. "expo 데이터베이스에서 도시별 주문 건수와 주문 총액을 총액이 큰 순서로 표로 보여 줘."
2. "그 쿼리를 RuSQL 편집기에 새 탭으로 열어서 실행해 줘."

기대 결과: Daegu 300건 14,749,000 · Busan 300건 14,736,000 · Gwangju 300건 14,694,000 · Incheon 300건 14,665,000 · Seoul 300건 14,610,000. (Claude가 도구를 부를 때 한 번 허용을 물을 수 있습니다.)

## 주의 (이미 알고 있는 것)

- **장면 3은 위 문장 그대로 `SELECT *`로 하세요.** 앱을 다시 켠 직후에도 됩니다(리허설에서 로그아웃 → 로그인 뒤 확인). `COUNT(*)` 같은 집계도 2026-11-21에 고쳐서 다시 켠 직후에 인덱스를 탑니다(앱에서 확인: Try it 39 → 0.27 ms) — 이 안내서의 시간과 수치는 `SELECT *`로 잰 것입니다.
- **장면 4의 Journey는 방금 실행한 문장에 눌러도 됩니다**(전에는 1단계만 나왔던 것을 2026-11-21에 고침, 리허설에서 11단계 확인). 이 PC에 **옛 설치 파일로 깐 앱**이 있으면 새 `RuSQL_2.3.0_x64-setup.exe`로 다시 설치해야 이 수정이 들어갑니다.
- Claude가 편집기를 움직이려면 **앱이 로그인된 상태**이고 **Server Manager가 RUNNING**이어야 합니다.
- 설치한 앱은 처음에 연결이 하나도 없습니다(위 준비 2번).

## 자주 받는 질문 (한 줄 답)

- **AI는 어디에 있나요?** 앱 안에 AI 모델이 있는 것이 아니라 Claude Desktop을 MCP로 연결합니다. RuSQL이 데이터베이스 · 편집기 · 서버를 다루는 도구 37개를 내줍니다.
- **MySQL과 호환되나요?** 진짜 MySQL 8.4.9에 같은 문장 7,740개(코퍼스 80개)를 보내 결과를 비교해 차이가 0입니다. mysql 클라이언트나 DBeaver로 접속됩니다.
- **얼마나 검증했나요?** 자동 테스트 1,065케이스(확인 약 152만 개), 실제 MySQL과의 대조, 무작위 문장 퍼저로 확인합니다.
- **무엇은 안 하나요?** XA 분산 트랜잭션, 페이지 단위 버퍼 풀 교체, WAL 기반 복제는 범위에서 뺐습니다.
