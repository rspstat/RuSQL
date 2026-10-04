# RuSQL 시연 (사석용, 약 3분)

아래 수치는 이 PC의 Release 빌드에서 실제로 잰 값입니다(10만 행 주문 + 5천 행 고객). PC 상태에 따라 조금 달라집니다.

## 준비 (미리, 1분)

1. 서버를 띄웁니다(앱의 Server Manager, 또는 `code\build\backend\server\Release\engine_server.exe --port 7878 --no-mysql --data-dir <빈 폴더>`).
2. `python code/test/demo/seed.py` — `demo` 데이터베이스에 `customers` 5,000행, `orders` 100,000행을 만듭니다(약 1초).
   `demo` 이외의 데이터베이스는 건드리지 않고, 다시 실행하면 처음 상태로 돌아갑니다(이미 만든 인덱스도 사라짐).
3. 앱 쿼리 에디터에서 `USE demo;`

## 1. 큰 데이터에서도 바로 답한다

```sql
SELECT status, COUNT(*), SUM(amount) FROM orders GROUP BY status;               -- 10만 행 집계, 약 0.07초

SELECT c.city, COUNT(*) AS orders, SUM(o.amount) AS total
FROM orders o JOIN customers c ON o.customer_id = c.id
WHERE o.status = 'DONE' GROUP BY c.city ORDER BY total DESC;                   -- 조인 + 집계, 약 0.7초
```

## 2. 인덱스를 만들면 계획이 바뀐다

```sql
SELECT id, status, amount FROM orders WHERE customer_id = 1234;                -- 26행, 약 35ms
EXPLAIN SELECT id, status, amount FROM orders WHERE customer_id = 1234;        -- Seq Scan, Est. cost 100000

CREATE INDEX idx_orders_customer ON orders(customer_id);                       -- 약 0.5초

EXPLAIN SELECT id, status, amount FROM orders WHERE customer_id = 1234;        -- Index Scan, Est. cost 33.2
SELECT id, status, amount FROM orders WHERE customer_id = 2345;                -- 22행, 1ms 미만
```

- 같은 질의를 두 번 치면 결과 캐시 때문에 0ms가 나옵니다. "인덱스 후" 시간은 **다른 고객 번호**(2345)로 보여 주고, 근거는 EXPLAIN의 계획 변화로 설명하세요.
- **`COUNT(*)`·`SUM()` 같은 집계 질의로는 이 시연을 하지 마세요.** `SELECT COUNT(*) FROM orders WHERE customer_id = 1234`는 EXPLAIN에는 Index Scan이 나오지만 실제로는 인덱스를 타지 않고 테이블 전체를 읽어서 인덱스를 만들어도 35ms 그대로입니다(집계는 인덱스 경로를 쓰지 않는 기존 동작).
- **인덱스를 만든 뒤에는 `orders`와 `customers`를 조인하는 질의를 다시 돌리지 마세요.** 플래너가 이 인덱스를 쓰는 조인(Reverse Index NL)을 골라서 `orders ... JOIN customers` 형태가 0.7초 → 약 2초로 느려지는 알려진 문제가 있습니다(해시 조인이 더 빠름). 조인은 인덱스를 만들기 **전에** 보여 주세요.

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

복구 뒤에도 인덱스가 그대로 쓰입니다(2번을 먼저 했다면 `EXPLAIN SELECT id, status, amount FROM orders WHERE customer_id = 1234;`가 Index Scan).

## 4. (선택) Claude로 자연어 질의

Claude Desktop에 MCP가 연결되어 있다면 "demo 데이터베이스에서 도시별 취소 주문 비율을 알려 줘"처럼 물어봅니다.
이 단계는 자동으로 검증하지 못했으니 **발표 전에 한 번 미리 돌려 보세요**(연결 상태에 달려 있음).
인덱스를 만든 뒤라면 Claude가 만드는 조인이 느릴 수 있으니, 이 단계는 2번 이전이나 `seed.py`를 다시 돌린 직후에 하세요.

## 되돌리기

`python code/test/demo/seed.py` 한 번이면 처음 상태(인덱스 없음, 추가한 행 없음)로 돌아갑니다.
