# RuSQL 시연 (사석용, 약 3분)

아래 수치는 이 PC의 Release 빌드에서 실제로 잰 값입니다(10만 행 주문 + 5천 행 고객). PC 상태에 따라 조금 달라집니다.

## 준비 (미리, 1분)

1. 서버를 띄웁니다(앱의 Server Manager, 또는 `code\build\backend\server\Release\engine_server.exe --port 7878 --no-mysql --data-dir <빈 폴더>`).
2. `python code/test/demo/seed.py` — `demo` 데이터베이스에 `customers` 5,000행, `orders` 100,000행을 만듭니다(약 1.5초).
   `demo` 이외의 데이터베이스는 건드리지 않고, 다시 실행하면 처음 상태로 돌아갑니다(이미 만든 인덱스도 사라짐).
3. 앱 쿼리 에디터에서 `USE demo;`

## 1. 큰 데이터에서도 바로 답한다

```sql
SELECT status, COUNT(*), SUM(amount) FROM orders GROUP BY status;               -- 10만 행 집계, 약 0.07초

SELECT c.city, COUNT(*) AS orders, SUM(o.amount) AS total
FROM orders o JOIN customers c ON o.customer_id = c.id
WHERE o.status = 'DONE' GROUP BY c.city ORDER BY total DESC;                   -- 조인 + 집계, 약 1초

SELECT ROUND(AVG(amount), 0) AS avg_amount, MAX(amount) - MIN(amount) AS spread FROM orders;   -- 집계 식, 약 0.1초
```

## 2. 인덱스를 만들면 계획이 바뀐다

```sql
SELECT COUNT(*), SUM(amount) FROM orders WHERE customer_id = 1234;             -- 26행, 약 40ms
EXPLAIN SELECT COUNT(*), SUM(amount) FROM orders WHERE customer_id = 1234;     -- Seq Scan, Est. cost 100000

CREATE INDEX idx_orders_customer ON orders(customer_id);                       -- 약 0.5초

EXPLAIN SELECT COUNT(*), SUM(amount) FROM orders WHERE customer_id = 1234;     -- Index Scan, Est. cost 33.2
SELECT COUNT(*), SUM(amount) FROM orders WHERE customer_id = 2345;             -- 22행, 1ms 미만
```

- 같은 질의를 두 번 치면 결과 캐시 때문에 0ms가 나옵니다. "인덱스 후" 시간은 **다른 고객 번호**(2345)로 보여 주고, 근거는 EXPLAIN의 계획 변화로 설명하세요.
- 인덱스를 만든 뒤에 1번의 조인을 다시 돌려도 느려지지 않습니다(약 1초 그대로).

## 3. 서버가 죽어도 커밋한 데이터는 남는다

```sql
BEGIN;
INSERT INTO orders VALUES (100001, 1234, 'PAID', 5000, '2026-10-04');
COMMIT;                                                  -- 커밋됨
BEGIN;
INSERT INTO orders VALUES (100002, 1234, 'PAID', 7000, '2026-10-04');   -- 커밋 안 함
SELECT id FROM orders WHERE id >= 100001;                -- 지금 세션에서는 둘 다 보임
```

이 상태에서 서버를 강제 종료합니다(`taskkill /F /IM engine_server.exe` 또는 작업 관리자). 서버를 다시 띄우면(같은 데이터 폴더) 복구에 몇 초 걸립니다(10만 행 적재 직후 종료한 경우 약 3초).

```sql
USE demo;
SELECT id, customer_id, amount FROM orders WHERE id >= 100001;   -- 100001만 있고 100002는 없음
SELECT COUNT(*) FROM orders;                                      -- 100001
```

- 재시작 직후에는 인덱스는 살아 있지만(`EXPLAIN`은 Index Scan), `COUNT`·`SUM` 같은 **집계 질의는 전체를 읽습니다(약 36ms)**. 행 위치 캐시가 비어 있어서 그렇고, 인덱스를 쓰는 `UPDATE`/`DELETE`가 한 번 실행되어야 채워집니다. 이 단계에서는 집계 대신 위처럼 집계 없는 조회로 보여 주세요.

## 4. (선택) Claude로 자연어 질의

Claude Desktop에 MCP가 연결되어 있다면 "demo 데이터베이스에서 도시별 취소 주문 비율을 알려 줘"처럼 물어봅니다.
이 단계는 자동으로 검증하지 못했으니 **발표 전에 한 번 미리 돌려 보세요**(연결 상태에 달려 있음).

## 피할 것 (알려진 문제)

- **집계 안에서 계산하지 마세요.** `SUM(amount * 2)`, `SUM(price * qty)`는 아직 파싱 오류입니다(`SUM(amount) * 2`는 됩니다). **함수 결과를 계산의 왼쪽에 쓰지 마세요**: `ROUND(AVG(amount), 0) * 2`는 오류, `2 * ROUND(AVG(amount), 0)`은 됩니다.
- NULL이 든 계산(`v + 1`, `v * 2`, `ROUND(v)`)은 NULL로 나옵니다(2026-10-05에 고침; 전에는 `NULL1`·0). 데모 데이터에는 NULL이 없습니다. `UPDATE`가 NOT NULL 열에 NULL을 넣는 것은 아직 막지 않습니다.
- 집계 식(`MAX(amount) - MIN(amount)`, `ROUND(AVG(amount), 0)`, `COALESCE(SUM(amount), 0)`)은 정상입니다(2026-10-05에 고침). 결과 열 이름만 함수의 인자가 빠져서 `ROUND()`로 나옵니다 — `AS avg_amount`처럼 별칭을 붙이면 깔끔합니다.
- `LEFT JOIN`에서 `COUNT(o.id)`, `ORDER BY o.amount DESC`, `SELECT DISTINCT o.status`, `HAVING COUNT(o.id) = 0`처럼 `테이블.열`을 쓰는 것은 모두 정상입니다(2026-10-05에 고침). 결과 열 이름도 쓴 그대로(`COUNT(o.id)`) 나옵니다.

## 되돌리기

`python code/test/demo/seed.py` 한 번이면 처음 상태(인덱스 없음, 추가한 행 없음)로 돌아갑니다.
