"""
RuSQL 성능 벤치마크 (UI의 Benchmark 패널이 표시하는 4개 항목만 측정)
  1. 단순 INSERT/DELETE 10,000건 (단건 처리)
  2. Bulk  INSERT/DELETE 100,000건 (묶음 처리)
  3. 인덱스 효과   — 포인트 조회, SeqScan vs B-tree
  4. 트랜잭션 TPS  — AutoCommit vs BEGIN/COMMIT

사용법:
  python bench.py    # rusql-server 가 7878 포트로 실행 중이어야 함
"""

import hashlib, socket, time, json, os, sys

# Windows에서 cmd.exe 콘솔은 시스템 기본 코드페이지(한국어 환경에선 cp949)로 열리는데,
# 이 스크립트의 print()가 쓰는 em dash(—) 등은 cp949로 인코딩할 수 없어
# UnicodeEncodeError로 죽는다 (예: "[4/9] 포인트 조회 — SeqScan..."). stdout을 UTF-8로
# 강제 전환해 어떤 콘솔 코드페이지에서 실행되든 깨지지 않게 한다.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

RUSQL_HOST = "127.0.0.1"
RUSQL_PORT = 7878
RUSQL_USER = "root"
RUSQL_PASS = "root"

N_SINGLE  = 10_000
N_BULK    = 100_000
CHUNK     = 500
N_SEL     = 5_000
N_REPS    = 300
N_TXN     = 1_000
RESULT_FILE = "result.json"


def _compute_native_password_token(password, nonce):
    """mysql_native_password-style challenge-response, matching engine_client's
    compute_native_password_token (client/src/main.cpp): the native protocol no longer
    accepts a plaintext password (Phase 19 security fix), only this token."""
    stage1 = hashlib.sha1(password.encode()).digest()
    stage2 = hashlib.sha1(stage1).digest()
    xor_key = hashlib.sha1(nonce + stage2).digest()
    return bytes(a ^ b for a, b in zip(stage1, xor_key))


class RuSQL:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((RUSQL_HOST, RUSQL_PORT))
        self.sock.settimeout(120)
        banner = self._read_until_end()
        nonce_hex = next((l.split(" ", 1)[1] for l in banner.splitlines() if l.startswith("NONCE ")), None)
        if not nonce_hex:
            raise RuntimeError("Server did not send an auth challenge (NONCE)")
        token = _compute_native_password_token(RUSQL_PASS, bytes.fromhex(nonce_hex))
        self._send(f"AUTH {RUSQL_USER} {token.hex()}")
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

    print(f"\n[1/4] 단순 INSERT/DELETE ({N_SINGLE:,}건 단건) ...")
    result["single"] = bench_single()
    s = result["single"]
    print(f"  INSERT {N_SINGLE:,}건 : {s['insert_s']:.2f}초")
    print(f"  DELETE {N_SINGLE:,}건 : {s['delete_s']:.2f}초")

    print(f"\n[2/4] Bulk INSERT/DELETE ({N_BULK:,}건 {CHUNK}행 묶음) ...")
    result["bulk"] = bench_bulk()
    b = result["bulk"]
    print(f"  INSERT {N_BULK:,}건 : {b['insert_s']:.2f}초")
    print(f"  DELETE {N_BULK:,}건 : {b['delete_s']:.2f}초")

    print(f"\n[3/4] 인덱스 성능 — SELECT 테이블 준비 ({N_SEL:,}행) + 포인트 조회 ...")
    db = RuSQL()
    setup_select_tables(db, N_SEL)
    result["point_lookup"] = bench_point_lookup(db, N_SEL)
    pl = result["point_lookup"]
    print(f"  SeqScan   : {pl['seq_ms']:.3f} ms/q")
    print(f"  BTree Idx : {pl['idx_ms']:.3f} ms/q  =>  {pl['speedup']:.1f}x")

    db.execute("DROP DATABASE IF EXISTS bench_db")
    db.close()

    print(f"\n[4/4] 트랜잭션 TPS — AutoCommit vs BEGIN/COMMIT ({N_TXN:,}건) ...")
    result["transaction"] = bench_transaction()
    tx = result["transaction"]
    print(f"  AutoCommit  : {round(tx['rows'] / tx['auto_s'])} TPS")
    print(f"  BEGIN/COMMIT: {round(tx['rows'] / tx['txn_s'])} TPS")

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
