"""
bench.py와 같은 4개 항목을 MySQL에서 측정하는 참고용 스크립트 (RuSQL 수치의 맥락을 잡기 위한 것).

  1. 단순 INSERT/DELETE 10,000건 (단건, autocommit)
  2. Bulk INSERT/DELETE 100,000건 (500행 묶음)
  3. 포인트 조회 — 인덱스 없음 vs 보조 인덱스 (5,000행, 300회)
  4. 트랜잭션 TPS — AutoCommit / 건당 BEGIN·COMMIT / 하나의 트랜잭션 (1,000건)

공정성: RuSQL의 autocommit은 이제 문장마다 redo 로그를 fsync하므로(내구성), MySQL도 InnoDB 기본값
innodb_flush_log_at_trx_commit=1 (커밋마다 redo fsync)로 돌린다. 바이너리 로그는 끈다(--skip-log-bin) --
켜 두면 커밋마다 fsync가 한 번 더 일어나 RuSQL(로그 1개)보다 불리해진다.

사용법:
  mysqld --no-defaults --datadir=<dir> --port=3307 --skip-log-bin --innodb-flush-log-at-trx-commit=1
  python bench_mysql.py [port]        # 결과 → result_mysql.json
"""

import json
import sys
import time

import pymysql

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 3307
N_SINGLE, N_BULK, CHUNK, N_SEL, N_REPS, N_TXN = 10_000, 100_000, 500, 5_000, 300, 1_000


def connect(autocommit=True):
    return pymysql.connect(host="127.0.0.1", port=PORT, user="root", password="", autocommit=autocommit)


def bench_single(c):
    cur = c.cursor()
    cur.execute("CREATE DATABASE IF NOT EXISTS bench_db")
    cur.execute("USE bench_db")
    cur.execute("DROP TABLE IF EXISTS bench_single")
    cur.execute("CREATE TABLE bench_single (id INT, val INT, PRIMARY KEY (id))")
    t0 = time.perf_counter()
    for i in range(N_SINGLE):
        cur.execute(f"INSERT INTO bench_single (id, val) VALUES ({i}, {i})")
    insert_s = time.perf_counter() - t0
    t0 = time.perf_counter()
    for i in range(N_SINGLE):
        cur.execute(f"DELETE FROM bench_single WHERE id = {i}")
    delete_s = time.perf_counter() - t0
    cur.execute("DROP TABLE bench_single")
    return {"rows": N_SINGLE, "insert_s": round(insert_s, 2), "delete_s": round(delete_s, 2)}


def bench_bulk(c):
    cur = c.cursor()
    cur.execute("DROP TABLE IF EXISTS bench_bulk")
    cur.execute("CREATE TABLE bench_bulk (id INT, val INT, PRIMARY KEY (id))")
    t0 = time.perf_counter()
    for start in range(0, N_BULK, CHUNK):
        vals = ", ".join(f"({i}, {i})" for i in range(start, min(start + CHUNK, N_BULK)))
        cur.execute(f"INSERT INTO bench_bulk (id, val) VALUES {vals}")
    insert_s = time.perf_counter() - t0
    t0 = time.perf_counter()
    for lo in range(0, N_BULK, CHUNK):
        cur.execute(f"DELETE FROM bench_bulk WHERE id BETWEEN {lo} AND {lo + CHUNK - 1}")
    delete_s = time.perf_counter() - t0
    cur.execute("DROP TABLE bench_bulk")
    return {"rows": N_BULK, "insert_s": round(insert_s, 2), "delete_s": round(delete_s, 2)}


def bench_point(c):
    cur = c.cursor()
    cur.execute("DROP TABLE IF EXISTS sel_noidx")
    cur.execute("DROP TABLE IF EXISTS sel_idx")
    cur.execute("CREATE TABLE sel_noidx (id INT AUTO_INCREMENT, code VARCHAR(20) NOT NULL, val INT, PRIMARY KEY (id))")
    cur.execute("CREATE TABLE sel_idx (id INT AUTO_INCREMENT, code VARCHAR(20) NOT NULL, val INT, PRIMARY KEY (id))")
    cur.execute("CREATE INDEX idx_si_code ON sel_idx (code)")
    cur.execute("CREATE INDEX idx_si_val ON sel_idx (val)")
    for start in range(0, N_SEL, CHUNK):
        vals = ", ".join(f"('CODE{i}', {i})" for i in range(start, min(start + CHUNK, N_SEL)))
        cur.execute(f"INSERT INTO sel_noidx (code, val) VALUES {vals}")
        cur.execute(f"INSERT INTO sel_idx (code, val) VALUES {vals}")
    t0 = time.perf_counter()
    for i in range(N_REPS):
        cur.execute(f"SELECT * FROM sel_noidx WHERE code = 'CODE{i % N_SEL}'")
        cur.fetchall()
    seq_ms = (time.perf_counter() - t0) / N_REPS * 1000
    t0 = time.perf_counter()
    for i in range(N_REPS):
        cur.execute(f"SELECT * FROM sel_idx WHERE code = 'CODE{i % N_SEL}'")
        cur.fetchall()
    idx_ms = (time.perf_counter() - t0) / N_REPS * 1000
    cur.execute("DROP TABLE sel_noidx")
    cur.execute("DROP TABLE sel_idx")
    return {"seq_ms": round(seq_ms, 3), "idx_ms": round(idx_ms, 3), "speedup": round(seq_ms / idx_ms if idx_ms else 0, 1)}


def bench_txn():
    c = connect(autocommit=True)
    cur = c.cursor()
    cur.execute("USE bench_db")

    def fresh():
        cur.execute("DROP TABLE IF EXISTS bench_txn")
        cur.execute("CREATE TABLE bench_txn (id INT, val INT, PRIMARY KEY (id))")

    fresh()
    t0 = time.perf_counter()
    for i in range(N_TXN):
        cur.execute(f"INSERT INTO bench_txn (id, val) VALUES ({i}, {i})")
    auto_s = time.perf_counter() - t0

    fresh()
    t0 = time.perf_counter()
    for i in range(N_TXN):
        cur.execute("BEGIN")
        cur.execute(f"INSERT INTO bench_txn (id, val) VALUES ({i}, {i})")
        cur.execute("COMMIT")
    txn_s = time.perf_counter() - t0

    fresh()
    t0 = time.perf_counter()
    cur.execute("BEGIN")
    for i in range(N_TXN):
        cur.execute(f"INSERT INTO bench_txn (id, val) VALUES ({i}, {i})")
    cur.execute("COMMIT")
    txn_batch_s = time.perf_counter() - t0
    cur.execute("DROP TABLE bench_txn")
    return {"rows": N_TXN, "auto_s": round(auto_s, 2), "txn_s": round(txn_s, 2), "txn_batch_s": round(txn_batch_s, 2)}


def main():
    c = connect()
    result = {"single": bench_single(c), "bulk": bench_bulk(c)}
    result["point_lookup"] = bench_point(c)
    result["transaction"] = bench_txn()
    c.cursor().execute("DROP DATABASE IF EXISTS bench_db")
    with open("result_mysql.json", "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
