"""
RuSQL vs MySQL 성능 벤치마크
측정 항목:
  1. INSERT TPS          — 1만 행 단건 INSERT
  2. SELECT 등호         — Hash Index vs B-tree vs SeqScan
  3. SELECT 범위         — 인덱스 있을 때 range query latency
  4. 병렬 스케일링       — RUSTDB_PARALLEL 0/1, GROUP BY 집계 처리량
  5. 동시 접속 SELECT    — 1/4/8 스레드 동시 쿼리 TPS

사용법:
  pip install -r requirements.txt
  python bench.py           # rusql-server 가 7878 포트로 실행 중이어야 함
"""

import socket
import time
import json
import statistics
import threading
import mysql.connector
import os

# ── 설정 ──────────────────────────────────────────────────────────────────────
RUSTDB_HOST = "127.0.0.1"
RUSTDB_PORT = 7878
RUSTDB_USER = "root"
RUSTDB_PASS = "root"

MYSQL_HOST  = "127.0.0.1"
MYSQL_PORT  = 3306
MYSQL_USER  = "root"
MYSQL_PASS  = "root"
MYSQL_DB    = "bench_db"

N_INSERT    = 10_000    # INSERT TPS 측정 행 수
N_SELECT    = 1_000     # SELECT 반복 횟수
N_PARALLEL  = 5_000     # 병렬 스케일링용 집계 행 수
RESULT_FILE = "result.json"

# ── RuSQL 커넥터 ──────────────────────────────────────────────────────────────
class RuSQL:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((RUSTDB_HOST, RUSTDB_PORT))
        self.sock.settimeout(30)
        self._read_until_end()                        # welcome banner
        self._send(f"AUTH {RUSTDB_USER} {RUSTDB_PASS}")
        self._read_until_end()                        # AUTH OK

    def _send(self, data: str):
        self.sock.sendall((data + "\n").encode())

    def _read_until_end(self) -> str:
        buf = ""
        while True:
            chunk = self.sock.recv(4096).decode(errors="replace")
            buf += chunk
            if "---END---" in buf:
                return buf

    def execute(self, sql: str) -> str:
        s = sql.rstrip().rstrip(';') + ";"
        self._send(s)
        return self._read_until_end()

    def close(self):
        self.sock.close()

# ── MySQL 커넥터 ──────────────────────────────────────────────────────────────
def mysql_connect(db=None):
    cfg = dict(host=MYSQL_HOST, port=MYSQL_PORT,
               user=MYSQL_USER, password=MYSQL_PASS)
    if db:
        cfg["database"] = db
    return mysql.connector.connect(**cfg)

def mysql_exec(conn, sql: str):
    cur = conn.cursor()
    cur.execute(sql)
    try:
        cur.fetchall()
    except Exception:
        pass
    conn.commit()

# ── 공통 유틸 ────────────────────────────────────────────────────────────────
def fmt(v):
    return f"{v:,.1f}"

def print_row(label, rusql_val, mysql_val, unit=""):
    print(f"  {label:<40} RuSQL: {fmt(rusql_val):>10}{unit}   MySQL: {fmt(mysql_val):>10}{unit}")

# ─────────────────────────────────────────────────────────────────────────────
# 1. INSERT TPS
# ─────────────────────────────────────────────────────────────────────────────
def bench_insert_rusql() -> float:
    db = RuSQL()
    db.execute("CREATE DATABASE IF NOT EXISTS bench_db")
    db.execute("USE bench_db")
    db.execute("DROP TABLE IF EXISTS bench_insert")
    db.execute("CREATE TABLE bench_insert (id INT AUTO INCREMENT, val INT, label VARCHAR(50), CONSTRAINT pk_bi PRIMARY KEY (id))")

    start = time.perf_counter()
    for i in range(N_INSERT):
        db.execute(f"INSERT INTO bench_insert (val, label) VALUES ({i}, 'row{i}')")
    elapsed = time.perf_counter() - start

    db.execute("DROP TABLE IF EXISTS bench_insert")
    db.close()
    return N_INSERT / elapsed

def bench_insert_mysql() -> float:
    conn = mysql_connect()
    cur = conn.cursor()
    cur.execute("CREATE DATABASE IF NOT EXISTS bench_db")
    conn.commit()
    conn = mysql_connect(MYSQL_DB)
    cur = conn.cursor()
    cur.execute("DROP TABLE IF EXISTS bench_insert")
    cur.execute("CREATE TABLE bench_insert (id INT AUTO_INCREMENT PRIMARY KEY, val INT, label VARCHAR(50))")
    conn.commit()

    start = time.perf_counter()
    for i in range(N_INSERT):
        cur.execute("INSERT INTO bench_insert (val, label) VALUES (%s, %s)", (i, f"row{i}"))
        conn.commit()
    elapsed = time.perf_counter() - start

    cur.execute("DROP TABLE IF EXISTS bench_insert")
    conn.commit()
    conn.close()
    return N_INSERT / elapsed

# ─────────────────────────────────────────────────────────────────────────────
# 2. SELECT 등호 — Hash Index vs B-tree vs SeqScan
# ─────────────────────────────────────────────────────────────────────────────
def setup_select_rusql(db: RuSQL, n: int):
    db.execute("CREATE DATABASE IF NOT EXISTS bench_db")
    db.execute("USE bench_db")
    db.execute("DROP TABLE IF EXISTS bench_select")
    db.execute("CREATE TABLE bench_select (id INT AUTO INCREMENT, code VARCHAR(20) NOT NULL, val INT, CONSTRAINT pk_bs PRIMARY KEY (id))")
    # bulk insert
    chunk = 200
    for start in range(0, n, chunk):
        vals = ", ".join(f"('CODE{i}', {i})" for i in range(start, min(start+chunk, n)))
        db.execute(f"INSERT INTO bench_select (code, val) VALUES {vals}")

def setup_select_mysql(conn, n: int):
    cur = conn.cursor()
    cur.execute("DROP TABLE IF EXISTS bench_select")
    cur.execute("CREATE TABLE bench_select (id INT AUTO_INCREMENT PRIMARY KEY, code VARCHAR(20) NOT NULL, val INT)")
    conn.commit()
    chunk = 200
    for start in range(0, n, chunk):
        vals = ", ".join(f"('CODE{i}', {i})" for i in range(start, min(start+chunk, n)))
        cur.execute(f"INSERT INTO bench_select (code, val) VALUES {vals}")
    conn.commit()

def bench_select_rusql() -> dict:
    db = RuSQL()
    n = 5_000
    setup_select_rusql(db, n)

    # SeqScan (인덱스 없음)
    t0 = time.perf_counter()
    for i in range(N_SELECT):
        db.execute(f"SELECT * FROM bench_select WHERE code = 'CODE{i % n}'")
    seq_ms = (time.perf_counter() - t0) / N_SELECT * 1000

    # B-tree Index
    db.execute("CREATE INDEX idx_bs_code ON bench_select (code)")
    t0 = time.perf_counter()
    for i in range(N_SELECT):
        db.execute(f"SELECT * FROM bench_select WHERE code = 'CODE{i % n}'")
    btree_ms = (time.perf_counter() - t0) / N_SELECT * 1000
    db.execute("DROP INDEX IF EXISTS idx_bs_code")

    # Hash Index
    db.execute("CREATE INDEX idx_bs_code_h ON bench_select (code) USING HASH")
    t0 = time.perf_counter()
    for i in range(N_SELECT):
        db.execute(f"SELECT * FROM bench_select WHERE code = 'CODE{i % n}'")
    hash_ms = (time.perf_counter() - t0) / N_SELECT * 1000
    db.execute("DROP INDEX IF EXISTS idx_bs_code_h")

    db.execute("DROP TABLE IF EXISTS bench_select")
    db.close()
    return {"seq": seq_ms, "btree": btree_ms, "hash": hash_ms}

def bench_select_mysql() -> dict:
    conn = mysql_connect(MYSQL_DB)
    n = 5_000
    setup_select_mysql(conn, n)
    cur = conn.cursor()

    # SeqScan
    t0 = time.perf_counter()
    for i in range(N_SELECT):
        cur.execute(f"SELECT * FROM bench_select WHERE code = 'CODE{i % n}'")
        cur.fetchall()
    seq_ms = (time.perf_counter() - t0) / N_SELECT * 1000

    # B-tree Index
    cur.execute("CREATE INDEX idx_bs_code ON bench_select (code)")
    conn.commit()
    t0 = time.perf_counter()
    for i in range(N_SELECT):
        cur.execute(f"SELECT * FROM bench_select WHERE code = 'CODE{i % n}'")
        cur.fetchall()
    btree_ms = (time.perf_counter() - t0) / N_SELECT * 1000

    cur.execute("DROP TABLE IF EXISTS bench_select")
    conn.commit()
    conn.close()
    return {"seq": seq_ms, "btree": btree_ms, "hash": btree_ms}  # MySQL은 hash=btree

# ─────────────────────────────────────────────────────────────────────────────
# 3. SELECT 범위 (Range Query latency)
# ─────────────────────────────────────────────────────────────────────────────
def bench_range_rusql() -> dict:
    db = RuSQL()
    n = 5_000
    setup_select_rusql(db, n)

    # 인덱스 없음
    t0 = time.perf_counter()
    for i in range(N_SELECT):
        db.execute(f"SELECT * FROM bench_select WHERE val BETWEEN {i % (n-100)} AND {i % (n-100) + 100}")
    no_idx_ms = (time.perf_counter() - t0) / N_SELECT * 1000

    # B-tree 인덱스
    db.execute("CREATE INDEX idx_bs_val ON bench_select (val)")
    t0 = time.perf_counter()
    for i in range(N_SELECT):
        db.execute(f"SELECT * FROM bench_select WHERE val BETWEEN {i % (n-100)} AND {i % (n-100) + 100}")
    idx_ms = (time.perf_counter() - t0) / N_SELECT * 1000

    db.execute("DROP TABLE IF EXISTS bench_select")
    db.close()
    return {"no_index": no_idx_ms, "index": idx_ms}

def bench_range_mysql() -> dict:
    conn = mysql_connect(MYSQL_DB)
    n = 5_000
    setup_select_mysql(conn, n)
    cur = conn.cursor()

    t0 = time.perf_counter()
    for i in range(N_SELECT):
        cur.execute(f"SELECT * FROM bench_select WHERE val BETWEEN {i % (n-100)} AND {i % (n-100) + 100}")
        cur.fetchall()
    no_idx_ms = (time.perf_counter() - t0) / N_SELECT * 1000

    cur.execute("CREATE INDEX idx_bs_val ON bench_select (val)")
    conn.commit()
    t0 = time.perf_counter()
    for i in range(N_SELECT):
        cur.execute(f"SELECT * FROM bench_select WHERE val BETWEEN {i % (n-100)} AND {i % (n-100) + 100}")
        cur.fetchall()
    idx_ms = (time.perf_counter() - t0) / N_SELECT * 1000

    cur.execute("DROP TABLE IF EXISTS bench_select")
    conn.commit()
    conn.close()
    return {"no_index": no_idx_ms, "index": idx_ms}

# ─────────────────────────────────────────────────────────────────────────────
# 4. 병렬 스케일링 (RuSQL only — RUSTDB_PARALLEL 0 vs 1)
# ─────────────────────────────────────────────────────────────────────────────
def bench_parallel_rusql() -> dict:
    results = {}
    for mode in ["0", "1"]:
        os.environ["RUSTDB_PARALLEL"] = mode
        db = RuSQL()
        db.execute("CREATE DATABASE IF NOT EXISTS bench_db")
        db.execute("USE bench_db")
        db.execute("DROP TABLE IF EXISTS bench_parallel")
        db.execute("CREATE TABLE bench_parallel (id INT AUTO INCREMENT, val INT, grp INT, CONSTRAINT pk_bp PRIMARY KEY (id))")
        chunk = 500
        for s in range(0, N_PARALLEL, chunk):
            vals = ", ".join(f"({v}, {v % 10})" for v in range(s, min(s+chunk, N_PARALLEL)))
            db.execute(f"INSERT INTO bench_parallel (val, grp) VALUES {vals}")

        t0 = time.perf_counter()
        for _ in range(20):
            db.execute("SELECT grp, COUNT(*), SUM(val), AVG(val) FROM bench_parallel GROUP BY grp")
        elapsed = (time.perf_counter() - t0) / 20 * 1000

        db.execute("DROP TABLE IF EXISTS bench_parallel")
        db.close()
        results["off" if mode == "0" else "on"] = elapsed

    return results

# ─────────────────────────────────────────────────────────────────────────────
# 5. 동시 접속 SELECT TPS
# ─────────────────────────────────────────────────────────────────────────────
def _concurrent_rusql_worker(n_queries: int, results: list, idx: int):
    try:
        db = RuSQL()
        db.execute("USE bench_db")
        t0 = time.perf_counter()
        for i in range(n_queries):
            db.execute(f"SELECT * FROM bench_conc WHERE id = {(i % 1000) + 1}")
        elapsed = time.perf_counter() - t0
        db.close()
        results[idx] = n_queries / elapsed
    except Exception as e:
        results[idx] = 0.0

def _concurrent_mysql_worker(n_queries: int, results: list, idx: int):
    try:
        conn = mysql_connect(MYSQL_DB)
        cur = conn.cursor()
        t0 = time.perf_counter()
        for i in range(n_queries):
            cur.execute(f"SELECT * FROM bench_conc WHERE id = {(i % 1000) + 1}")
            cur.fetchall()
        elapsed = time.perf_counter() - t0
        conn.close()
        results[idx] = n_queries / elapsed
    except Exception as e:
        results[idx] = 0.0

def setup_conc_rusql(db: RuSQL):
    db.execute("CREATE DATABASE IF NOT EXISTS bench_db")
    db.execute("USE bench_db")
    db.execute("DROP TABLE IF EXISTS bench_conc")
    db.execute("CREATE TABLE bench_conc (id INT AUTO INCREMENT, val INT, CONSTRAINT pk_bc PRIMARY KEY (id))")
    chunk = 500
    for s in range(0, 1000, chunk):
        vals = ", ".join(f"({v})" for v in range(s, min(s+chunk, 1000)))
        db.execute(f"INSERT INTO bench_conc (val) VALUES {vals}")

def setup_conc_mysql(conn):
    cur = conn.cursor()
    cur.execute("DROP TABLE IF EXISTS bench_conc")
    cur.execute("CREATE TABLE bench_conc (id INT AUTO_INCREMENT PRIMARY KEY, val INT)")
    vals = ", ".join(f"({v})" for v in range(1000))
    cur.execute(f"INSERT INTO bench_conc (val) VALUES {vals}")
    conn.commit()

def bench_concurrent(n_threads: int) -> dict:
    # RuSQL 준비
    db = RuSQL()
    setup_conc_rusql(db)
    db.close()

    # MySQL 준비
    conn = mysql_connect(MYSQL_DB)
    setup_conc_mysql(conn)
    conn.close()

    n_queries = 500
    r_results = [0.0] * n_threads
    m_results = [0.0] * n_threads

    # RuSQL
    threads = [threading.Thread(target=_concurrent_rusql_worker, args=(n_queries, r_results, i))
               for i in range(n_threads)]
    for t in threads: t.start()
    for t in threads: t.join()
    rusql_tps = sum(r_results)

    # MySQL
    threads = [threading.Thread(target=_concurrent_mysql_worker, args=(n_queries, m_results, i))
               for i in range(n_threads)]
    for t in threads: t.start()
    for t in threads: t.join()
    mysql_tps = sum(m_results)

    # cleanup
    db = RuSQL()
    db.execute("USE bench_db")
    db.execute("DROP TABLE IF EXISTS bench_conc")
    db.close()
    conn = mysql_connect(MYSQL_DB)
    mysql_exec(conn, "DROP TABLE IF EXISTS bench_conc")
    conn.close()

    return {"rusql": rusql_tps, "mysql": mysql_tps}

# ─────────────────────────────────────────────────────────────────────────────
# main
# ─────────────────────────────────────────────────────────────────────────────
def main():
    result = {}

    print("=" * 60)
    print("  RuSQL v2.2.0 vs MySQL — Performance Benchmark")
    print("=" * 60)

    # 1. INSERT TPS
    print("\n[1/5] INSERT TPS (10,000 rows, auto-commit) ...")
    r = bench_insert_rusql()
    m = bench_insert_mysql()
    result["insert_tps"] = {"rusql": r, "mysql": m}
    print_row("INSERT TPS", r, m, " TPS")

    # 2. SELECT 등호
    print("\n[2/5] SELECT 등호 latency (5,000 rows) ...")
    r = bench_select_rusql()
    m = bench_select_mysql()
    result["select_eq"] = {"rusql": r, "mysql": m}
    print_row("  SeqScan", r["seq"], m["seq"], " ms/query")
    print_row("  B-tree Index", r["btree"], m["btree"], " ms/query")
    print_row("  Hash Index", r["hash"], m["hash"], " ms/query")

    # 3. 범위 쿼리
    print("\n[3/5] SELECT 범위 latency (5,000 rows, BETWEEN) ...")
    r = bench_range_rusql()
    m = bench_range_mysql()
    result["select_range"] = {"rusql": r, "mysql": m}
    print_row("  No Index", r["no_index"], m["no_index"], " ms/query")
    print_row("  B-tree Index", r["index"], m["index"], " ms/query")

    # 4. 병렬 스케일링
    print("\n[4/5] 병렬 스케일링 — GROUP BY 집계 (RuSQL only) ...")
    p = bench_parallel_rusql()
    result["parallel"] = p
    speedup = p["off"] / p["on"] if p["on"] > 0 else 0
    print(f"  PARALLEL OFF: {fmt(p['off'])} ms/query")
    print(f"  PARALLEL ON:  {fmt(p['on'])} ms/query  (speedup: {speedup:.2f}x)")

    # 5. 동시 접속
    print("\n[5/5] 동시 접속 SELECT TPS ...")
    conc = {}
    for n in [1, 4, 8]:
        r = bench_concurrent(n)
        conc[str(n)] = r
        print_row(f"  {n} threads", r["rusql"], r["mysql"], " TPS")
    result["concurrent"] = conc

    # JSON 저장
    with open(RESULT_FILE, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    print(f"\n결과 저장: {RESULT_FILE}")
    print("차트 생성: python chart.py")

    # 벤치마크 DB 정리
    try:
        db = RuSQL()
        db.execute("DROP DATABASE IF EXISTS bench_db")
        db.close()
    except Exception:
        pass
    try:
        conn = mysql_connect()
        mysql_exec(conn, "DROP DATABASE IF EXISTS bench_db")
        conn.close()
    except Exception:
        pass
    print("bench_db 정리 완료")

if __name__ == "__main__":
    main()
