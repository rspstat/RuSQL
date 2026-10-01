"""Concurrent crash-consistency check: several client threads hammer ONE table with
autocommit ops and small explicit transactions (disjoint key ranges per thread, so every
thread has an exact oracle), the server is kill -9'ed mid-flight, restarted, and compared.

Per thread the oracle holds the acknowledged state; the single operation (or the single
COMMIT) that was in flight at kill time may legitimately have landed or not, but explicit
transactions must be all-or-nothing.
"""
import os, random, re, shutil, socket, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "test", "perf"))
import bench

# Release server by default; override with RUSQL_SERVER_EXE (e.g. the Debug build).
EXE = os.environ.get("RUSQL_SERVER_EXE", os.path.join(ROOT, "build", "backend", "server", "Release", "engine_server.exe"))
PORT = 17893
DATA = os.path.join(HERE, "_fuzz_conc_data")
bench.RUSQL_PORT = PORT
THREADS = 4
SPAN = 1000


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


class Worker(threading.Thread):
    def __init__(self, wid, seed):
        super().__init__(daemon=True)
        self.wid, self.rnd = wid, random.Random(seed)
        self.base = wid * SPAN
        self.oracle = {}            # acknowledged committed state for this thread's key range
        self.inflight = None        # ("op", key, newval|None) or ("txn", {key: val})
        self.stop = False

    def run(self):
        try:
            db = bench.RuSQL(); db.execute("USE d")
            while not self.stop:
                r = self.rnd.random()
                if r < 0.15:  # explicit transaction of 3 inserts/updates
                    keys = [self.base + self.rnd.randint(1, 200) for _ in range(3)]
                    keys = list(dict.fromkeys(keys))
                    vals = {k: self.rnd.randint(0, 999) for k in keys}
                    db.execute("BEGIN")
                    for k, v in vals.items():
                        if k in self.oracle:
                            db.execute(f"UPDATE c SET v = {v} WHERE id = {k}")
                        else:
                            db.execute(f"INSERT INTO c VALUES ({k},{v})")
                    self.inflight = ("txn", vals)
                    resp = db.execute("COMMIT")
                    if resp.startswith("OK"):
                        self.oracle.update(vals)
                    self.inflight = None
                else:
                    k = self.base + self.rnd.randint(1, 200)
                    op = self.rnd.random()
                    if k not in self.oracle:
                        v = self.rnd.randint(0, 999)
                        self.inflight = ("op", k, v)
                        if db.execute(f"INSERT INTO c VALUES ({k},{v})").startswith("OK"):
                            self.oracle[k] = v
                    elif op < 0.6:
                        v = self.rnd.randint(0, 999)
                        self.inflight = ("op", k, v)
                        if db.execute(f"UPDATE c SET v = {v} WHERE id = {k}").startswith("OK"):
                            self.oracle[k] = v
                    else:
                        self.inflight = ("op", k, None)
                        if db.execute(f"DELETE FROM c WHERE id = {k}").startswith("OK"):
                            self.oracle.pop(k, None)
                    self.inflight = None
        except Exception:
            pass  # connection died with the server


def run_round(seed, run_seconds):
    shutil.rmtree(DATA, ignore_errors=True)
    p = start()
    db = bench.RuSQL()
    db.execute("CREATE DATABASE d"); db.execute("USE d")
    db.execute("CREATE TABLE c (id INT PRIMARY KEY, v INT)")
    workers = [Worker(i, seed * 10 + i) for i in range(THREADS)]
    for w in workers: w.start()
    time.sleep(run_seconds)
    p.kill(); p.wait()                       # crash while everyone is mid-statement
    for w in workers: w.stop = True
    for w in workers: w.join(timeout=5)
    p = start()
    db = bench.RuSQL(); db.execute("USE d")
    got = {}
    for line in db.execute("SELECT id, v FROM c ORDER BY id").splitlines():
        m = re.match(r"\|\s*(\d+)\s*\|\s*(-?\d+)\s*\|", line)
        if m: got[int(m.group(1))] = int(m.group(2))
    db.close(); p.terminate(); p.wait()

    ok = True
    for w in workers:
        lo, hi = w.base, w.base + SPAN
        mine = {k: v for k, v in got.items() if lo <= k < hi}
        exp = dict(w.oracle)
        inf = w.inflight
        if inf and inf[0] == "op":
            _, k, nv = inf
            alt = dict(exp)
            if nv is None: alt.pop(k, None)
            else: alt[k] = nv
            if mine == exp or mine == alt: continue
        elif inf and inf[0] == "txn":
            alt = dict(exp); alt.update(inf[1])      # committed-in-flight: all of it or none of it
            if mine == exp or mine == alt: continue
        elif mine == exp:
            continue
        ok = False
        miss = {k: v for k, v in exp.items() if mine.get(k) != v}
        extra = {k: v for k, v in mine.items() if exp.get(k) != v}
        print(f"MISMATCH seed={seed} worker={w.wid} inflight={inf} missing/wrong={dict(list(miss.items())[:5])} unexpected/wrong={dict(list(extra.items())[:5])}")
    return ok, sum(len(w.oracle) for w in workers)


if __name__ == "__main__":
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    base = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    bad = 0; total_rows = 0
    for i in range(rounds):
        ok, n = run_round(base + i, random.Random(base + i).uniform(0.6, 2.5))
        total_rows += n
        if not ok: bad += 1
    print(f"rounds={rounds} mismatches={bad} (acked rows checked across rounds={total_rows})")
    shutil.rmtree(DATA, ignore_errors=True)
    sys.exit(1 if bad else 0)
