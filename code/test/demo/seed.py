"""RuSQL 시연용 데이터: demo 데이터베이스에 customers 5,000행 + orders 100,000행을 만든다.

  python seed.py [port]      (서버가 떠 있어야 함, 기본 7878. 약 몇 초)

demo 데이터베이스가 이미 있으면 지우고 다시 만든다(다른 데이터베이스는 건드리지 않음).
orders.customer_id 에는 일부러 인덱스를 만들지 않는다 -- 시연에서 인덱스를 만들기 전/후를 비교하기 위해서다.
시연 순서는 DEMO.md.
"""
import random
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "perf"))
import bench  # noqa: E402  (접속·인증 도우미 bench.RuSQL 재사용)

CUSTOMERS = 5_000
ORDERS = 100_000
CHUNK = 500
CITIES = ["서울", "부산", "대구", "인천", "광주", "대전", "울산", "수원"]
GRADES = ["BRONZE", "SILVER", "GOLD"]
STATUSES = ["PAID", "SHIPPED", "DONE", "CANCELLED"]
STATUS_WEIGHTS = [20, 20, 55, 5]


def main(port):
    bench.RUSQL_PORT = port
    db = bench.RuSQL()
    rng = random.Random(42)  # 같은 값이 매번 나오도록 시드 고정
    t0 = time.time()
    db.execute("DROP DATABASE IF EXISTS demo")
    db.execute("CREATE DATABASE demo")
    db.execute("USE demo")
    db.execute("CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(30), city VARCHAR(20), grade VARCHAR(10))")
    db.execute("CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, status VARCHAR(12), amount INT, ordered_at DATE)")
    for s in range(0, CUSTOMERS, CHUNK):
        rows = ", ".join(
            f"({i}, 'customer{i:04d}', '{rng.choice(CITIES)}', '{rng.choices(GRADES, weights=[60, 30, 10])[0]}')"
            for i in range(s, min(s + CHUNK, CUSTOMERS)))
        db.execute("INSERT INTO customers VALUES " + rows)
    for s in range(0, ORDERS, CHUNK):
        rows = ", ".join(
            f"({i}, {rng.randrange(CUSTOMERS)}, '{rng.choices(STATUSES, weights=STATUS_WEIGHTS)[0]}', "
            f"{rng.randrange(1000, 100000, 100)}, '2026-{rng.randint(1, 12):02d}-{rng.randint(1, 28):02d}')"
            for i in range(s, min(s + CHUNK, ORDERS)))
        db.execute("INSERT INTO orders VALUES " + rows)
    print(f"demo.customers {CUSTOMERS:,}행, demo.orders {ORDERS:,}행 생성 ({time.time() - t0:.1f}초)")
    for sql in ("SELECT COUNT(*) FROM customers", "SELECT COUNT(*) FROM orders"):
        print(sql, "->", db.execute(sql).split("---END---")[0].strip().replace("\n", " | "))
    db.close()


if __name__ == "__main__":
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 7878)
