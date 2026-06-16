"""
RuSQL 성능 벤치마크
  1. 인덱스 효과   — SeqScan vs B-tree (포인트·범위·Top-K)
  2. 병렬 처리     — RUSTDB_PARALLEL 0 vs 1
  3. 단순 INSERT/DELETE 10,000건 (단건 처리)
  4. Bulk  INSERT/DELETE 100,000건 (묶음 처리)

사용법:
  python bench.py    # rusql-server 가 7878 포트로 실행 중이어야 함
"""

import socket, time, json, os

RUSTDB_HOST = "127.0.0.1"
RUSTDB_PORT = 7878
RUSTDB_USER = "root"
RUSTDB_PASS = "root"

N_SINGLE  = 10_000
N_BULK    = 100_000
CHUNK     = 500
N_SEL     = 5_000
N_REPS    = 300
N_PAR     = 50_000
N_TXN     = 1_000
RESULT_FILE = "result.json"


class RuSQL:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((RUSTDB_HOST, RUSTDB_PORT))
        self.sock.settimeout(120)
        self._read_until_end()
        self._send(f"AUTH {RUSTDB_USER} {RUSTDB_PASS}")
        self._read_until_end()

    def _send(self, data):
        self.sock.sendall((data + "\n").encode())

    def _read_until_end(self):
        buf = ""
        while True:
            chunk = self.sock.recv(4096).decode(errors="replace")
            buf += chunk
            if "---END---" in buf:
                return buf

    def execute(self, sql):
        self._send(sql.rstrip().rstrip(";") + ";")
        return self._read_until_end()

    def close(self):
        self.sock.close()


# ── 단순 INSERT / DELETE (단건) ───────────────────────────────────────────────
# 명시적 PK (id = 0..n-1) 사용 — AUTO INCREMENT 시퀀스 불일치 방지
# DELETE WHERE id = X → PK B+Tree 직접 조회, 검색 비용 없음
def bench_single(n=N_SINGLE) -> dict:
    db = RuSQL()
    db.execute("CREATE DATABASE IF NOT EXISTS bench_db")
    db.execute("USE bench_db")
    db.execute("DROP TABLE IF EXISTS bench_single")
    db.execute(
        "CREATE TABLE bench_single (id INT, val INT, "
        "CONSTRAINT pk_single PRIMARY KEY (id))"
    )

    t0 = time.perf_counter()
    for i in range(n):
        db.execute(f"INSERT INTO bench_single (id, val) VALUES ({i}, {i})")
    insert_s = time.perf_counter() - t0

    t0 = time.perf_counter()
    for i in range(n):
        db.execute(f"DELETE FROM bench_single WHERE id = {i}")
    delete_s = time.perf_counter() - t0

    db.execute("DROP TABLE IF EXISTS bench_single")
    db.close()
    return {"rows": n, "insert_s": round(insert_s, 2), "delete_s": round(delete_s, 2)}


# ── Bulk INSERT / DELETE (묶음) ───────────────────────────────────────────────
# 명시적 PK (id = 0..n-1) 사용 — DELETE WHERE id BETWEEN X AND Y → PK range 조회
def bench_bulk(n=N_BULK, chunk=CHUNK) -> dict:
    db = RuSQL()
    db.execute("CREATE DATABASE IF NOT EXISTS bench_db")
    db.execute("USE bench_db")
    db.execute("DROP TABLE IF EXISTS bench_bulk")
    db.execute(
        "CREATE TABLE bench_bulk (id INT, val INT, "
        "CONSTRAINT pk_bulk PRIMARY KEY (id))"
    )

    t0 = time.perf_counter()
    for start in range(0, n, chunk):
        vals = ", ".join(f"({i}, {i})" for i in range(start, min(start + chunk, n)))
        db.execute(f"INSERT INTO bench_bulk (id, val) VALUES {vals}")
    insert_s = time.perf_counter() - t0

    t0 = time.perf_counter()
    for lo in range(0, n, chunk):
        db.execute(f"DELETE FROM bench_bulk WHERE id BETWEEN {lo} AND {lo + chunk - 1}")
    delete_s = time.perf_counter() - t0

    db.execute("DROP TABLE IF EXISTS bench_bulk")
    db.close()
    return {"rows": n, "insert_s": round(insert_s, 2), "delete_s": round(delete_s, 2)}


# ── SELECT 테이블 준비 ─────────────────────────────────────────────────────────
def setup_select_tables(db, n):
    db.execute("CREATE DATABASE IF NOT EXISTS bench_db")
    db.execute("USE bench_db")
    db.execute("DROP TABLE IF EXISTS sel_noidx")
    db.execute("DROP TABLE IF EXISTS sel_idx")
    db.execute(
        "CREATE TABLE sel_noidx (id INT AUTO INCREMENT, code VARCHAR(20) NOT NULL, val INT, "
        "CONSTRAINT pk_sn PRIMARY KEY (id))"
    )
    db.execute(
        "CREATE TABLE sel_idx (id INT AUTO INCREMENT, code VARCHAR(20) NOT NULL, val INT, "
        "CONSTRAINT pk_si PRIMARY KEY (id))"
    )
    db.execute("CREATE INDEX idx_si_code ON sel_idx (code)")
    db.execute("CREATE INDEX idx_si_val  ON sel_idx (val)")
    chunk = 500
    for start in range(0, n, chunk):
        vals = ", ".join(f"('CODE{i}', {i})" for i in range(start, min(start + chunk, n)))
        db.execute(f"INSERT INTO sel_noidx (code, val) VALUES {vals}")
        db.execute(f"INSERT INTO sel_idx   (code, val) VALUES {vals}")


# ── 포인트 조회 ───────────────────────────────────────────────────────────────
def bench_point_lookup(db, n) -> dict:
    t0 = time.perf_counter()
    for i in range(N_REPS):
        db.execute(f"SELECT * FROM sel_noidx WHERE code = 'CODE{i % n}'")
    seq_ms = (time.perf_counter() - t0) / N_REPS * 1000

    t0 = time.perf_counter()
    for i in range(N_REPS):
        db.execute(f"SELECT * FROM sel_idx WHERE code = 'CODE{i % n}'")
    idx_ms = (time.perf_counter() - t0) / N_REPS * 1000

    return {"seq_ms": round(seq_ms, 3), "idx_ms": round(idx_ms, 3),
            "speedup": round(seq_ms / idx_ms if idx_ms else 0, 1)}


# ── 범위 쿼리 ─────────────────────────────────────────────────────────────────
def bench_range_query(db, n) -> dict:
    t0 = time.perf_counter()
    for i in range(N_REPS):
        lo = i % (n - 100)
        db.execute(f"SELECT * FROM sel_noidx WHERE val BETWEEN {lo} AND {lo + 100}")
    seq_ms = (time.perf_counter() - t0) / N_REPS * 1000

    t0 = time.perf_counter()
    for i in range(N_REPS):
        lo = i % (n - 100)
        db.execute(f"SELECT * FROM sel_idx WHERE val BETWEEN {lo} AND {lo + 100}")
    idx_ms = (time.perf_counter() - t0) / N_REPS * 1000

    return {"seq_ms": round(seq_ms, 3), "idx_ms": round(idx_ms, 3),
            "speedup": round(seq_ms / idx_ms if idx_ms else 0, 1)}


# ── Top-K 최적화 ──────────────────────────────────────────────────────────────
def bench_top_k(db, n) -> dict:
    t0 = time.perf_counter()
    for i in range(N_REPS):
        lo = (i * 97) % (n - 500)
        db.execute(
            f"SELECT * FROM sel_noidx WHERE val BETWEEN {lo} AND {lo + 500} "
            f"ORDER BY val DESC LIMIT 5"
        )
    seq_ms = (time.perf_counter() - t0) / N_REPS * 1000

    t0 = time.perf_counter()
    for i in range(N_REPS):
        lo = (i * 97) % (n - 500)
        db.execute(
            f"SELECT * FROM sel_idx WHERE val BETWEEN {lo} AND {lo + 500} "
            f"ORDER BY val DESC LIMIT 5"
        )
    idx_ms = (time.perf_counter() - t0) / N_REPS * 1000

    return {"seq_ms": round(seq_ms, 3), "idx_ms": round(idx_ms, 3),
            "speedup": round(seq_ms / idx_ms if idx_ms else 0, 1)}


# ── 병렬 집계 ─────────────────────────────────────────────────────────────────
def bench_parallel() -> dict:
    results = {}
    for mode in ["0", "1"]:
        db = RuSQL()
        # 서버 병렬 처리 토글 (Python env var는 서버에 영향 없으므로 SQL 커맨드 사용)
        db.execute(f"SET @rusql_parallel = {mode}")
        db.execute("CREATE DATABASE IF NOT EXISTS bench_db")
        db.execute("USE bench_db")
        db.execute("DROP TABLE IF EXISTS bench_parallel")
        db.execute(
            "CREATE TABLE bench_parallel (id INT AUTO INCREMENT, val INT, grp INT, "
            "CONSTRAINT pk_bp PRIMARY KEY (id))"
        )
        chunk = 500
        for s in range(0, N_PAR, chunk):
            vals = ", ".join(f"({v}, {v % 10})" for v in range(s, min(s + chunk, N_PAR)))
            db.execute(f"INSERT INTO bench_parallel (val, grp) VALUES {vals}")
        t0 = time.perf_counter()
        for i in range(20):
            db.execute(f"SELECT grp, COUNT(*), SUM(val), AVG(val) FROM bench_parallel GROUP BY grp LIMIT {1000 + i}")
        elapsed_ms = (time.perf_counter() - t0) / 20 * 1000
        db.execute("DROP TABLE IF EXISTS bench_parallel")
        db.close()
        results["off_ms" if mode == "0" else "on_ms"] = round(elapsed_ms, 1)
    return results


# ── 병렬 ORDER BY ─────────────────────────────────────────────────────────────
# 50k 행 테이블에 ORDER BY 2-key 정렬 (인덱스 없음 → 풀 스캔 + 정렬)
# LIMIT 1000 으로 네트워크 전송량을 줄이되, 정렬 자체는 50k 행 전체 수행
def bench_order_by(n=N_PAR) -> dict:
    results = {}
    for mode in ["0", "1"]:
        db = RuSQL()
        db.execute(f"SET @rusql_parallel = {mode}")
        db.execute("CREATE DATABASE IF NOT EXISTS bench_db")
        db.execute("USE bench_db")
        db.execute("DROP TABLE IF EXISTS bench_order")
        db.execute(
            "CREATE TABLE bench_order (id INT AUTO INCREMENT, val INT, grp INT, "
            "CONSTRAINT pk_bord PRIMARY KEY (id))"
        )
        chunk = 500
        for s in range(0, n, chunk):
            vals = ", ".join(f"({v}, {v % 100})" for v in range(s, min(s + chunk, n)))
            db.execute(f"INSERT INTO bench_order (val, grp) VALUES {vals}")
        t0 = time.perf_counter()
        for i in range(20):
            db.execute(
                f"SELECT id, val, grp FROM bench_order "
                f"ORDER BY val DESC, grp ASC LIMIT {1000 + i}"
            )
        elapsed_ms = (time.perf_counter() - t0) / 20 * 1000
        db.execute("DROP TABLE IF EXISTS bench_order")
        db.close()
        results["off_ms" if mode == "0" else "on_ms"] = round(elapsed_ms, 1)
    results["speedup"] = round(
        results["off_ms"] / results["on_ms"] if results.get("on_ms") else 0, 2
    )
    return results


# ── 쿼리 결과 캐시 ────────────────────────────────────────────────────────────
# 집계 쿼리 (1행 응답) 사용 — 네트워크 전송 오버헤드 제거 후 서버 연산 시간만 측정
# 캐시 히트 시 서버는 HashMap 룩업(~0.1ms)만 수행, 스캔(~30ms)과 차이가 명확
def bench_query_cache(db) -> dict:
    query = "SELECT COUNT(*) AS n, MAX(val) AS mx, MIN(val) AS mn FROM sel_noidx"

    t0 = time.perf_counter()
    db.execute(query)
    scan_ms = (time.perf_counter() - t0) * 1000

    t0 = time.perf_counter()
    for _ in range(N_CACHE - 1):
        db.execute(query)
    hit_ms = (time.perf_counter() - t0) / (N_CACHE - 1) * 1000

    return {
        "scan_ms": round(scan_ms, 3),
        "hit_ms":  round(hit_ms, 3),
        "speedup": round(scan_ms / hit_ms if hit_ms else 0, 1),
    }


# ── 트랜잭션 TPS ──────────────────────────────────────────────────────────────
# AutoCommit(묵시적) vs BEGIN/COMMIT(명시적) INSERT 1,000건 비교
def bench_transaction(n=N_TXN) -> dict:
    db = RuSQL()
    db.execute("CREATE DATABASE IF NOT EXISTS bench_db")
    db.execute("USE bench_db")

    db.execute("DROP TABLE IF EXISTS bench_txn")
    db.execute(
        "CREATE TABLE bench_txn (id INT, val INT, "
        "CONSTRAINT pk_txn PRIMARY KEY (id))"
    )
    t0 = time.perf_counter()
    for i in range(n):
        db.execute(f"INSERT INTO bench_txn (id, val) VALUES ({i}, {i})")
    auto_s = time.perf_counter() - t0

    db.execute("DROP TABLE IF EXISTS bench_txn")
    db.execute(
        "CREATE TABLE bench_txn (id INT, val INT, "
        "CONSTRAINT pk_txn PRIMARY KEY (id))"
    )
    t0 = time.perf_counter()
    for i in range(n):
        db.execute("BEGIN")
        db.execute(f"INSERT INTO bench_txn (id, val) VALUES ({i}, {i})")
        db.execute("COMMIT")
    txn_s = time.perf_counter() - t0

    db.execute("DROP TABLE IF EXISTS bench_txn")
    db.close()
    return {
        "rows": n,
        "auto_s": round(auto_s, 2),
        "txn_s":  round(txn_s, 2),
    }


# ── main ──────────────────────────────────────────────────────────────────────
def main():
    result = {}
    print("=" * 60)
    print("  RuSQL 성능 벤치마크")
    print("=" * 60)

    print(f"\n[1/9] 단순 INSERT/DELETE ({N_SINGLE:,}건 단건) ...")
    result["single"] = bench_single()
    s = result["single"]
    print(f"  INSERT {N_SINGLE:,}건 : {s['insert_s']:.2f}초")
    print(f"  DELETE {N_SINGLE:,}건 : {s['delete_s']:.2f}초")

    print(f"\n[2/9] Bulk INSERT/DELETE ({N_BULK:,}건 {CHUNK}행 묶음) ...")
    result["bulk"] = bench_bulk()
    b = result["bulk"]
    print(f"  INSERT {N_BULK:,}건 : {b['insert_s']:.2f}초")
    print(f"  DELETE {N_BULK:,}건 : {b['delete_s']:.2f}초")

    print(f"\n[3/9] SELECT 테이블 준비 ({N_SEL:,}행) ...")
    db = RuSQL()
    setup_select_tables(db, N_SEL)

    print("[4/9] 포인트 조회 — SeqScan vs BTree ...")
    result["point_lookup"] = bench_point_lookup(db, N_SEL)
    pl = result["point_lookup"]
    print(f"  SeqScan   : {pl['seq_ms']:.3f} ms/q")
    print(f"  BTree Idx : {pl['idx_ms']:.3f} ms/q  =>  {pl['speedup']:.1f}x")

    print("[5/9] 범위 쿼리 — No-Index vs BTree ...")
    result["range_query"] = bench_range_query(db, N_SEL)
    rq = result["range_query"]
    print(f"  No-Index  : {rq['seq_ms']:.3f} ms/q")
    print(f"  BTree Idx : {rq['idx_ms']:.3f} ms/q  =>  {rq['speedup']:.1f}x")

    print("[6/9] Top-K — SeqScan+Sort vs Index LIMIT ...")
    result["top_k"] = bench_top_k(db, N_SEL)
    tk = result["top_k"]
    print(f"  SeqScan+Sort  : {tk['seq_ms']:.3f} ms/q")
    print(f"  Index LIMIT N : {tk['idx_ms']:.3f} ms/q  =>  {tk['speedup']:.1f}x")

    db.execute("DROP DATABASE IF EXISTS bench_db")
    db.close()

    print(f"[7/9] 트랜잭션 TPS — AutoCommit vs BEGIN/COMMIT ({N_TXN:,}건) ...")
    result["transaction"] = bench_transaction()
    tx = result["transaction"]
    print(f"  AutoCommit  : {round(tx['rows'] / tx['auto_s'])} TPS")
    print(f"  BEGIN/COMMIT: {round(tx['rows'] / tx['txn_s'])} TPS")

    print("[8/9] 병렬 집계 스케일링 — GROUP BY ...")
    result["parallel"] = bench_parallel()
    pa = result["parallel"]
    print(f"  PARALLEL OFF : {pa['off_ms']:.1f} ms/q")
    print(f"  PARALLEL ON  : {pa['on_ms']:.1f} ms/q")

    print("[9/9] 병렬 정렬 — ORDER BY ...")
    result["order_by"] = bench_order_by()
    ob = result["order_by"]
    print(f"  PARALLEL OFF : {ob['off_ms']:.1f} ms/q")
    print(f"  PARALLEL ON  : {ob['on_ms']:.1f} ms/q  =>  {ob['speedup']:.2f}x")

    with open(RESULT_FILE, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    print(f"\n결과 저장: {RESULT_FILE}")

    try:
        db = RuSQL()
        db.execute("DROP DATABASE IF EXISTS bench_db")
        db.close()
    except Exception:
        pass
    print("정리 완료")


if __name__ == "__main__":
    main()
