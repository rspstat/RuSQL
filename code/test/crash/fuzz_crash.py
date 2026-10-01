"""Randomized crash-consistency check.

Runs random DML (autocommit + explicit transactions, commits and rollbacks, covered and
non-covered statement kinds) against a live engine_server, hard-kills the server at a random
moment, restarts it and compares the recovered table contents with an oracle that models
exactly what the server ACKNOWLEDGED as committed.

Rules the oracle enforces:
  * every acknowledged autocommit statement / COMMIT must be present after the crash;
  * nothing from a ROLLBACK or from an unacknowledged/open transaction may be present.
"""
import os, random, re, shutil, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "test", "perf"))
import bench

# Release server by default; override with RUSQL_SERVER_EXE (e.g. the Debug build).
EXE = os.environ.get("RUSQL_SERVER_EXE", os.path.join(ROOT, "build", "backend", "server", "Release", "engine_server.exe"))
PORT = 17891
DATA = os.path.join(HERE, "_fuzz_data")
bench.RUSQL_PORT = PORT


def start():
    p = subprocess.Popen([EXE, "--port", str(PORT), "--no-mysql", "--data-dir", DATA],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=dict(os.environ))
    for _ in range(200):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            return p
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def table_rows(db, table):
    r = db.execute(f"SELECT id, v FROM {table} ORDER BY id")
    rows = {}
    for line in r.splitlines():
        m = re.match(r"\|\s*(\d+)\s*\|\s*(-?\d+)\s*\|", line)
        if m:
            rows[int(m.group(1))] = int(m.group(2))
    return rows


def run_round(seed, n_ops):
    rnd = random.Random(seed)
    shutil.rmtree(DATA, ignore_errors=True)
    p = start()
    db = bench.RuSQL()
    db.execute("CREATE DATABASE d"); db.execute("USE d")
    db.execute("CREATE TABLE a (id INT PRIMARY KEY, v INT)")
    db.execute("CREATE TABLE b (id INT PRIMARY KEY, v INT)")
    oracle = {"a": {}, "b": {}}          # acknowledged-committed state
    txn_buf = None                        # pending changes of the open explicit txn: {table: {id: v|None}}
    in_txn = False

    def apply(buf):
        for t, ch in buf.items():
            for k, val in ch.items():
                if val is None:
                    oracle[t].pop(k, None)
                else:
                    oracle[t][k] = val

    def view(t):  # what this session would currently see in table t
        base = dict(oracle[t])
        if in_txn:
            for k, val in txn_buf.get(t, {}).items():
                if val is None:
                    base.pop(k, None)
                else:
                    base[k] = val
        return base

    def record(t, k, val):
        if in_txn:
            txn_buf.setdefault(t, {})[k] = val
        else:
            oracle[t][k] = val if val is not None else oracle[t].pop(k, None) and None
            if val is None:
                oracle[t].pop(k, None)

    for _ in range(n_ops):
        t = rnd.choice(["a", "b"])
        cur = view(t)
        op = rnd.random()
        if op < 0.06 and not in_txn:
            db.execute("BEGIN"); in_txn = True; txn_buf = {}
        elif op < 0.12 and in_txn:
            db.execute("COMMIT"); apply(txn_buf); in_txn = False; txn_buf = None
        elif op < 0.16 and in_txn:
            db.execute("ROLLBACK"); in_txn = False; txn_buf = None
        elif op < 0.50:      # INSERT
            k = rnd.randint(1, 60)
            if k in cur:
                continue
            v = rnd.randint(0, 999)
            r = db.execute(f"INSERT INTO {t} VALUES ({k},{v})")
            if r.startswith("OK"):
                record(t, k, v)
        elif op < 0.75:      # UPDATE
            if not cur:
                continue
            k = rnd.choice(list(cur)); v = rnd.randint(0, 999)
            r = db.execute(f"UPDATE {t} SET v = {v} WHERE id = {k}")
            if r.startswith("OK"):
                record(t, k, v)
        elif op < 0.90:      # DELETE
            if not cur:
                continue
            k = rnd.choice(list(cur))
            r = db.execute(f"DELETE FROM {t} WHERE id = {k}")
            if r.startswith("OK"):
                record(t, k, None)
        else:                # non-covered statements (legacy persistence / structural paths)
            if in_txn:
                continue
            k = rnd.randint(1, 60); v = rnd.randint(0, 999)
            kind = rnd.random()
            if kind < 0.5:
                r = db.execute(f"REPLACE INTO {t} VALUES ({k},{v})")
                if r.startswith("OK"):
                    oracle[t][k] = v
            elif kind < 0.9:
                r = db.execute(f"INSERT INTO {t} VALUES ({k},{v}) ON DUPLICATE KEY UPDATE v = {v}")
                if r.startswith("OK"):
                    oracle[t][k] = v
            elif rnd.random() < 0.3:
                r = db.execute(f"TRUNCATE TABLE {t}")
                if r.startswith("OK"):
                    oracle[t].clear()
    # crash: any open explicit transaction was never acknowledged
    p.kill(); p.wait()
    p = start()
    db = bench.RuSQL(); db.execute("USE d")
    got = {t: table_rows(db, t) for t in ("a", "b")}
    ok = got == oracle
    db.close(); p.terminate(); p.wait()
    if not ok:
        print(f"MISMATCH seed={seed} ops={n_ops}")
        for t in ("a", "b"):
            if got[t] != oracle[t]:
                miss = {k: v for k, v in oracle[t].items() if got[t].get(k) != v}
                extra = {k: v for k, v in got[t].items() if oracle[t].get(k) != v}
                print(f"  table {t}: expected-but-wrong/missing={miss} unexpected/wrong={extra}")
    return ok


if __name__ == "__main__":
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    base_seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
    bad = 0
    for i in range(rounds):
        n_ops = random.Random(base_seed + i).randint(5, 120)
        if not run_round(base_seed + i, n_ops):
            bad += 1
    print(f"rounds={rounds} mismatches={bad}")
    shutil.rmtree(DATA, ignore_errors=True)
    sys.exit(1 if bad else 0)
