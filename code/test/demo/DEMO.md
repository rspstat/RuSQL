# RuSQL 시연 (사석용, 약 3분 + 선택 장면)

아래 수치는 이 PC의 Release 빌드(2026-10-22)에서 실제로 잰 값입니다(10만 행 주문 + 5천 행 고객). PC 상태에 따라 조금 달라집니다.

## 준비 (미리, 1분)

1. 서버를 띄웁니다(앱의 Server Manager, 또는 `code\build\backend\server\Release\engine_server.exe --port 7878 --no-mysql --data-dir <빈 폴더>`).
2. `python code/test/demo/seed.py` — `demo` 데이터베이스에 `customers` 5,000행, `orders` 100,000행을 만듭니다(약 1.5초).
   `demo` 이외의 데이터베이스는 건드리지 않고, 다시 실행하면 처음 상태로 돌아갑니다(이미 만든 인덱스도 사라짐).
3. 앱 쿼리 에디터에서 `USE demo;`

## 1. 큰 데이터에서도 바로 답한다

```sql
SELECT status, COUNT(*), SUM(amount) FROM orders GROUP BY status;               -- 10만 행 집계, 약 0.09초

SELECT c.city, COUNT(*) AS orders, SUM(o.amount) AS total
FROM orders o JOIN customers c ON o.customer_id = c.id
WHERE o.status = 'DONE' GROUP BY c.city ORDER BY total DESC;                   -- 조인 + 집계, 약 0.9초

SELECT ROUND(AVG(amount), 0) AS avg_amount, MAX(amount) - MIN(amount) AS spread FROM orders;   -- 집계 식, 약 0.07초

SELECT status, COUNT(*) AS n, RANK() OVER (ORDER BY COUNT(*) DESC) AS r
FROM orders GROUP BY status;                                                    -- 집계 위의 윈도(순위), 약 0.17초
```

## 2. 인덱스를 만들면 계획이 바뀐다

```sql
SELECT COUNT(*), SUM(amount) FROM orders WHERE customer_id = 1234;             -- 26행, 약 60ms
EXPLAIN SELECT COUNT(*), SUM(amount) FROM orders WHERE customer_id = 1234;     -- Seq Scan, Est. cost 100000

CREATE INDEX idx_orders_customer ON orders(customer_id);                       -- 약 0.6초

EXPLAIN SELECT COUNT(*), SUM(amount) FROM orders WHERE customer_id = 1234;     -- Index Scan, Est. cost 33.2
SELECT COUNT(*), SUM(amount) FROM orders WHERE customer_id = 2345;             -- 22행, 1ms 미만
```

- 같은 질의를 두 번 치면 결과 캐시 때문에 0ms가 나옵니다. "인덱스 후" 시간은 **다른 고객 번호**(2345)로 보여 주고, 근거는 EXPLAIN의 계획 변화로 설명합니다.
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

이 상태에서 서버를 강제 종료합니다. **앱의 Server Manager에서 Stop을 누르면 서버 프로세스를 그대로 죽입니다**(정상 종료 절차가 없는 강제 종료이고, 같은 데이터 폴더로 곧바로 다시 띄웁니다). 터미널에서 하려면 `taskkill /F /IM engine_server.exe`. 다시 띄우면(같은 데이터 폴더) 복구에 몇 초 걸립니다(10만 행 기준).

```sql
USE demo;
SELECT id, customer_id, amount FROM orders WHERE id >= 100001;   -- 100001만 있고 100002는 없음
SELECT COUNT(*) FROM orders;                                      -- 100001
```

- 재시작 직후에는 인덱스는 살아 있지만(`EXPLAIN`은 Index Scan), `COUNT`·`SUM` 같은 **집계 질의는 전체를 읽습니다(약 36ms)**. 행 위치 캐시가 비어 있어서 그렇습니다.

## 4. 한 문장의 여행 (선택, 30초)

에디터에 문장을 쓰고 도구 막대의 **점선 경로 아이콘(Journey)** 을 누릅니다. 문장이 한 번 실행되고, 지나온 단계(파싱 → 락 → 스캔 → 정렬 → 그룹 → 커밋)가 느린 화면으로 다시 재생됩니다(일시정지·한 단계씩·0.5~4x). 단계별 시간 막대는 실제 값입니다.

```sql
SELECT c.city, SUM(o.amount) AS total
FROM orders o JOIN customers c ON o.customer_id = c.id
GROUP BY c.city ORDER BY total DESC;                    -- 읽기: 스캔·정렬·그룹 단계가 나옴

INSERT INTO orders VALUES (100003, 1234, 'PAID', 9000, '2026-10-04');   -- 쓰기: 마지막에 "Commit (redo log, fsync)"가 전체의 대부분을 차지함
```

- SELECT가 아닌 문장은 **실제로 실행됩니다**(Journey가 먼저 알려 줍니다). 같은 일을 터미널에서 보려면 문장 앞에 `TRACE`를 붙이세요.

## 5. 락 대기와 교착 (선택, 1분)

세션이 둘 필요합니다. 하나는 앱, 다른 하나는 터미널에서 `engine_client.exe -u root -p root -h 127.0.0.1 -P 7878`(그다음 `USE demo;`)으로 엽니다. 앱의 **Server Manager → Sessions 아래 패널**이 열린 트랜잭션·락·기다리는 쌍·교착을 1.5초마다 보여 줍니다.

```sql
-- 앱 (세션 A)
BEGIN;
UPDATE orders SET amount = amount + 1 WHERE id = 7;

-- 터미널 (세션 B): 기다립니다 (패널에 LOCK WAIT, "waits for txn …")
BEGIN;
UPDATE orders SET amount = amount + 2 WHERE id = 7;

-- 앱 (세션 A): 커밋하면 B가 풀립니다
COMMIT;
```

교착은 서로 엇갈리게 잡으면 만들어집니다. A가 7번, B가 8번을 잡은 뒤 A가 8번, B가 7번을 요청하면 한쪽이 교착으로 되돌려지고 패널의 "최근 교착"에 남습니다. SQL로 직접 보려면:

```sql
SHOW LOCKS;
SELECT * FROM information_schema.innodb_trx;          -- 열린 트랜잭션, RUNNING / LOCK WAIT
SELECT * FROM information_schema.innodb_lock_waits;   -- 누가 누구를 기다리는지
```

## 6. B+Tree 한 번 보기 (선택, 30초)

앱 왼쪽 활동 표시줄의 **B+Tree 아이콘**. 노드당 최대 키를 3으로 두고 `1–12 넣기`를 누르면 잎이 쪼개지며 키가 위로 올라가고 트리가 한 층씩 높아지는 모습이 재생됩니다. 이어서 키 하나를 `삭제`하면 노드가 모자라 형제에게서 빌리거나 합쳐지는 것을 볼 수 있습니다. (교육용 모형이며 엔진 인덱스와 같은 원리입니다.)

## 7. MVCC 나란히 보기 (선택, 1분)

앱 왼쪽 활동 표시줄의 **MVCC 아이콘**(겹친 상자). 먼저 에디터에서 `USE demo;`로 데이터베이스를 고른 뒤 `실습 표 만들기`를 누르면 그 데이터베이스에 `mvcc_demo`(3행)를 만듭니다. 가운데 표는 행의 **모든 버전**(xmin·xmax·상태)과 그 버전이 세션 A·B에게 보이는지(●/○)이고, 양쪽은 서버에 거는 두 세션입니다. 시나리오를 `다음 단계 ▶`로 한 단계씩 보여 주세요.

- **UPDATE는 덮어쓰지 않고 버전을 더한다**: A가 UPDATE하면 새 버전(UNCOMMITTED)이 생기고 옛 버전은 끝나는 중(ENDING)이 됩니다. A에게는 새 버전만, B에게는 옛 버전만 ●로 보입니다.
- **같은 질문, 다른 답**: A를 REPEATABLE READ로 두면 B가 값을 바꾼 뒤에도 A는 100을 계속 읽습니다(옛 버전이 A에게만 보임). READ COMMITTED로 바꾸면 999.
- **기다리는 쓰기**: B의 UPDATE가 A의 락 때문에 돌아오지 않고 "기다리는 중"이 뜨다가 A가 COMMIT하면 이어집니다.

`SELECT * FROM information_schema.row_versions WHERE TABLE_NAME = 'orders'`처럼 SQL로도 같은 표를 볼 수 있습니다(`VISIBLE_TO_ME`는 묻는 세션의 스냅샷 기준).

## 8. (선택) Claude로 자연어 질의와 안전한 변경

Claude Desktop에 MCP가 연결되어 있다면 "demo 데이터베이스에서 도시별 취소 주문 비율을 알려 줘"처럼 물어봅니다. 변경은 `run_script`가 **미리보기 → 확인 → 저장** 순서로 처리합니다:

> "demo의 orders에서 status가 CANCELLED이고 amount가 90000 이상인 주문을 PAID로 바꾸는 UPDATE를 run_script로 먼저 미리보기해 줘."

미리보기는 문장을 실제로 실행해 영향 행 수를 보여 준 다음 되돌립니다(저장 안 됨). 사용자가 동의한 뒤 `commit=True`로 다시 부르면 저장됩니다. `DROP`·`TRUNCATE`·`WHERE` 없는 `UPDATE/DELETE`는 확인 없이는 막힙니다.
`run_script` 자체는 살아 있는 서버로 확인했지만 **Claude Desktop과의 연결은 자동으로 검증하지 못했으니 발표 전에 한 번 미리 돌려 보세요.**

## 알려진 한계 (이 시연에서는 피하세요)

2026-10-22에 위 시연 문장들과 아래 항목을 지금 엔진에 직접 돌려 확인했습니다. 예전 목록에 있던 `SUM(amount * 2)`, `ROUND(AVG(amount), 0) * 2`, 열 이름 `ROUND()`, `LEFT JOIN`의 `테이블.열`, 쉼표 조인·별칭 등은 **모두 고쳐졌으니 써도 됩니다.**

- **`RANGE BETWEEN 5 PRECEDING AND …`처럼 값 기준 윈도 프레임**은 아직 행 수로 읽어 틀린 값이 나옵니다(`ROWS`·`UNBOUNDED`·`CURRENT ROW` 프레임은 정상).
- `DATE('2026-03-05') + 0`은 `20260305`가 아니라 `2026`이 나오고, DATE 열의 `SUM`·`AVG`도 MySQL과 다릅니다. 날짜는 `YEAR()`·`MONTH()`·`DATEDIFF()`처럼 함수로 쓰세요.
- `WITH ROLLUP`, `SELECT @x := …`, `INSERT INTO t () VALUES ()`, `CREATE TEMPORARY TABLE`, `SIGNAL`, 커서, 라벨 붙은 루프, `KILL`, `FULLTEXT`, `USE INDEX`는 지원하지 않습니다.
- 서버를 강제 종료한 뒤 첫 `COUNT`·`SUM`은 전체를 읽어 느립니다(위 3번).
- `SELECT … FROM orders WHERE id IN (SELECT id FROM orders …)`처럼 **갱신하는 같은 표를 하위 질의로 읽는 `UPDATE`** 는 MySQL이 오류를 내는데 여기서는 통과합니다.

## 되돌리기

`python code/test/demo/seed.py` 한 번이면 처음 상태(인덱스 없음, 추가한 행 없음)로 돌아갑니다.
