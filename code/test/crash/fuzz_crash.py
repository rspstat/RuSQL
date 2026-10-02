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
    # The tables here are tiny; by default the server only uses indexes to find UPDATE/DELETE targets on
    # tables of 64+ rows. 0 makes the index paths run on every statement (override to test the scan).
    env = dict(os.environ)
    env.setdefault("RUSQL_DML_INDEX_MIN_ROWS", "0")
    # checkpoint at the (tiny) RUSQL_REDO_CHECKPOINT_BYTES size instead of letting the log grow with the table
    env.setdefault("RUSQL_REDO_CHECKPOINT_ROW_BYTES", "0")
    p = subprocess.Popen([EXE, "--port", str(PORT), "--no-mysql", "--data-dir", DATA],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
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


def index_problems(db, oracle):
    """The recovered INDEXES must agree with the recovered rows: a lookup through the secondary index (g) and through
    the primary key has to return exactly what the oracle says. (They used to come back stale after a checkpoint +
    restart, and a table scan -- which is all the content comparison does -- cannot see that.)"""
    problems = []
    for t, rows in oracle.items():
        for g in range(5):
            r = db.execute(f"SELECT id FROM {t} WHERE g = {g}")
            got = {int(m) for m in re.findall(r"\|\s*(\d+)\s*\|", r)}
            want = {k for k in rows if k % 5 == g}
            if got != want:
                problems.append(f"{t}: g = {g}: index returned {sorted(got)}, expected {sorted(want)}")
        for k, v in sorted(rows.items())[:12]:
            r = db.execute(f"SELECT v FROM {t} WHERE id = {k}")
            m = re.search(r"\|\s*(-?\d+)\s*\|", r)
            if not m or int(m.group(1)) != v:
                problems.append(f"{t}: id = {k}: pk lookup returned {r.splitlines()[-3:] if r else r}, expected v={v}")
    return problems


def run_round(seed, n_ops):
    rnd = random.Random(seed)
    shutil.rmtree(DATA, ignore_errors=True)
    p = start()
    db = bench.RuSQL()
    db.execute("CREATE DATABASE d"); db.execute("USE d")
    # g = id % 5 is fixed by the key, so the oracle needs no extra state; it gives group statements an index to use
    for tbl in ("a", "b"):
        db.execute(f"CREATE TABLE {tbl} (id INT PRIMARY KEY, v INT, g INT)")
        db.execute(f"CREATE INDEX {tbl}_g ON {tbl} (g)")
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
            r = db.execute(f"INSERT INTO {t} VALUES ({k},{v},{k % 5})")
            if r.startswith("OK"):
                record(t, k, v)
        elif op < 0.75:      # UPDATE
            if not cur:
                continue
            if rnd.random() < 0.35:  # a whole group through the secondary index on g
                grp = rnd.randint(0, 4); v = rnd.randint(0, 999)
                r = db.execute(f"UPDATE {t} SET v = {v} WHERE g = {grp}")
                if r.startswith("OK"):
                    for k in [k for k in cur if k % 5 == grp]:
                        record(t, k, v)
                continue
            k = rnd.choice(list(cur)); v = rnd.randint(0, 999)
            r = db.execute(f"UPDATE {t} SET v = {v} WHERE id = {k}")
            if r.startswith("OK"):
                record(t, k, v)
        elif op < 0.90:      # DELETE
            if not cur:
                continue
            if rnd.random() < 0.3:   # a whole group through the secondary index on g
                grp = rnd.randint(0, 4)
                r = db.execute(f"DELETE FROM {t} WHERE g = {grp}")
                if r.startswith("OK"):
                    for k in [k for k in cur if k % 5 == grp]:
                        record(t, k, None)
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
                r = db.execute(f"REPLACE INTO {t} VALUES ({k},{v},{k % 5})")
                if r.startswith("OK"):
                    oracle[t][k] = v
            elif kind < 0.9:
                r = db.execute(f"INSERT INTO {t} VALUES ({k},{v},{k % 5}) ON DUPLICATE KEY UPDATE v = {v}")
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
    problems = index_problems(db, oracle) if ok else []
    if problems:
        ok = False
    db.close(); p.terminate(); p.wait()
    if not ok:
        print(f"MISMATCH seed={seed} ops={n_ops}")
        for t in ("a", "b"):
            if got[t] != oracle[t]:
                miss = {k: v for k, v in oracle[t].items() if got[t].get(k) != v}
                extra = {k: v for k, v in got[t].items() if oracle[t].get(k) != v}
                print(f"  table {t}: expected-but-wrong/missing={miss} unexpected/wrong={extra}")
        for line in problems:
            print(f"  index: {line}")
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
