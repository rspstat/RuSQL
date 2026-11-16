"""Session-KILL fuzzer: KILL QUERY / KILL <id> shot at random sessions while they write, optionally mixed with a kill -9 of the whole server (R94).

What the other fuzzers do not do: they only kill the PROCESS. Here a killer thread picks random sessions from the process list and sends `KILL QUERY n` (stop the
running statement) or `KILL n` (end the session and roll its transaction back) while four client threads, each with a key range of its own, run autocommit statements, small
explicit transactions (COMMIT or ROLLBACK), a whole-range UPDATE and a long-running SELECT. Every thread keeps an exact oracle of its key range and checks, from what it sees:

  * a statement that was stopped (ERROR 1317) changed nothing                         (all-or-nothing, also for the whole-range UPDATE)
  * a session that was killed in the middle of a transaction left no part of it       (rolled back; a COMMIT in flight lands whole or not at all)
  * a session that was killed does not leave a row lock behind (its keys can be written at once, no statement waits for seconds)
  * a SELECT that is not stopped gives the right answer (no partial result is passed off as an answer)
  * the session list is empty of dead sessions afterwards, the secondary index agrees with the table scan, CHECK TABLE is clean, no row stays locked
  * odd seeds: kill -9 of the server while the KILLs are in flight, then restart and compare with the oracle (a statement in flight may have landed or not)

  python fuzz_kill.py [rounds] [base_seed]      FUZZ_KILL_TRACE=1 prints the violations as they are found; RUSQL_SERVER_EXE=<path> tests another build (e.g. Debug);
                                                FUZZ_KILL_BIG=<rows> the table of the long SELECT (default 1200; lower it on Debug); FUZZ_KILL_PORT=<port> a second copy at the same time
Exit code 1 and a MISMATCH / VIOLATION line (seed, worker, what) on any inconsistency. A seed fixes the statements, not the moments the KILLs land.
"""
import collections, os, random, re, shutil, socket, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "test", "perf"))
import bench

EXE = os.environ.get("RUSQL_SERVER_EXE", os.path.join(ROOT, "build", "backend", "server", "Release", "engine_server.exe"))
PORT = int(os.environ.get("FUZZ_KILL_PORT", "17911"))     # (another port runs a second copy beside the first)
DATA = os.path.join(HERE, "_fuzz_kill_data" + ("" if PORT == 17911 else f"_{PORT}"))
bench.RUSQL_PORT = PORT
THREADS = 4
SPAN = 1000
BIG_ROWS = int(os.environ.get("FUZZ_KILL_BIG", "1200"))   # the long SELECT is quadratic: about 0.7 s on Release, much more on Debug (lower this there)
SLOW = 8.0   # a statement that waits this long (seconds) for a row lock after a KILL is a leaked lock
HEAVY = "SELECT COUNT(*) FROM big a WHERE EXISTS (SELECT 1 FROM big b WHERE b.v = a.v AND b.g = a.g + 1000)"
ROW = re.compile(r"^\|\s*(-?\d+)\s*\|\s*(-?\d+)\s*\|", re.M)
ID_ROW = re.compile(r"^\|\s*(\d+)\s*\|", re.M)
# The session list is read with SHOW PROCESSLIST. information_schema.processlist (FUZZ_KILL_INFOSCHEMA=1) trips the OPEN race of R94 -- a SELECT of an information_schema table changes the
# shared table maps under every other session (docs/mds/DATE.md, backend/tests/test_info_schema_concurrency.cpp) -- about once in a hundred rounds on a Release build; with it the
# fuzzer reproduces that issue on purpose, without it the fuzzer looks for everything else.
PROCESSLIST = "SELECT ID, USER, COMMAND FROM information_schema.processlist" if os.environ.get("FUZZ_KILL_INFOSCHEMA") == "1" else "SHOW PROCESSLIST"


def session_ids(db):
    return [int(i) for i in ID_ROW.findall(db.execute(PROCESSLIST))]
ONE = re.compile(r"^\|\s*(-?\d+)\s*\|", re.M)
TRACE = os.environ.get("FUZZ_KILL_TRACE") == "1"
SERVER_DOWN = threading.Event()   # set just before the server is killed on purpose: a dead connection is then no finding
QUIET = threading.Event()         # cleared while a KILL <id> is being settled: the killed session is gone from the process list again (see Killer); workers look at their data only when it is set
QUIET.set()


def start(checkpoint_bytes):
    env = dict(os.environ)
    env["RUSQL_REDO_CHECKPOINT_ROW_BYTES"] = "0"
    env["RUSQL_REDO_CHECKPOINT_BYTES"] = str(checkpoint_bytes)
    p = subprocess.Popen([EXE, "--port", str(PORT), "--no-mysql", "--data-dir", DATA], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    for _ in range(300):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            return p
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def connect(db="d"):
    c = bench.RuSQL()
    c.sock.settimeout(60)
    if db:
        c.execute(f"USE {db}")
    return c


def server_up():
    try:
        socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
        return True
    except OSError:
        return False


def rows_of(resp):
    return {int(a): int(b) for a, b in ROW.findall(resp)}


class Worker(threading.Thread):
    def __init__(self, wid, seed, heavy_expected):
        super().__init__(daemon=True)
        self.wid, self.rnd = wid, random.Random(seed)
        self.lo, self.hi = wid * SPAN, wid * SPAN + SPAN
        self.heavy_expected = heavy_expected
        self.oracle = {}        # the exact state of this thread's key range
        self.pending = None     # (old state, [states that may also have landed]) while something is in flight; None when the state is known
        self.touch = []         # keys of a transaction that was killed: they are written at once (a leaked row lock would make that wait)
        self.db = None
        self.stop = False
        self.violations = []
        self.last = None        # (statement, when it was sent): where a worker that hangs is stuck
        self.log = collections.deque(maxlen=8)
        self.stats = dict(ops=0, txns=0, interrupted=0, reconnects=0, heavy_done=0)

    # ── helpers ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
    def bad(self, what):
        what += "  | the last statements: " + " ; ".join(self.log)
        self.violations.append(what)
        if TRACE:
            print(f"VIOLATION worker={self.wid}: {what}", flush=True)

    def x(self, sql, watch=True):
        t0 = time.time()
        self.last = (sql[:70], t0)
        try:
            resp = self.db.execute(sql)
        except OSError:
            self.log.append(f"{sql[:50]} -> (the session ended)")
            raise
        self.log.append(f"{sql[:50]} -> {resp[:12].strip()}")
        took = time.time() - t0
        if watch and took > SLOW and not SERVER_DOWN.is_set():
            self.bad(f"`{sql[:60]}` took {took:.1f}s (a leaked row lock after a KILL?)")
        return resp

    def read_range(self):
        for _ in range(40):
            resp = self.x(f"SELECT id, v FROM c WHERE id >= {self.lo} AND id < {self.hi} ORDER BY id")
            if resp.startswith("OK"):
                return rows_of(resp)
            if "1317" not in resp:
                self.bad(f"read of the range failed: {resp[:120]!r}")
                return None
            self.stats["interrupted"] += 1
        return None

    def resolve(self, what):
        """Something may or may not have landed (a COMMIT in flight, a killed session): the state must be one of the allowed ones; it becomes the oracle."""
        seen = self.read_range()
        if seen is None:
            return
        old, news = self.pending if self.pending else (self.oracle, [])
        if seen == old or any(seen == n for n in news):
            self.oracle = seen
        else:
            miss = {k: v for k, v in old.items() if seen.get(k) != v}
            extra = {k: v for k, v in seen.items() if old.get(k) != v}
            self.bad(f"{what}: the range is neither the old state nor an allowed new one (old rows missing/changed {dict(list(miss.items())[:4])}, unexpected/changed {dict(list(extra.items())[:4])})")
            self.oracle = seen
        self.pending = None

    def unchanged(self, what):
        """A statement was stopped (ERROR 1317): nothing of it may have landed."""
        self.pending = None
        seen = self.read_range()
        if seen is not None and seen != self.oracle:
            miss = {k: v for k, v in self.oracle.items() if seen.get(k) != v}
            extra = {k: v for k, v in seen.items() if self.oracle.get(k) != v}
            self.bad(f"{what} was stopped by KILL QUERY but changed data (missing/changed {dict(list(miss.items())[:4])}, unexpected/changed {dict(list(extra.items())[:4])})")
            self.oracle = seen

    # ── one step ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
    def drop(self):
        try:
            self.db.close()
        except Exception:
            pass
        self.db = None

    def after_kill(self):
        """The session was killed: find out what landed, and write the keys it had locked (a leaked row lock would make that wait)."""
        QUIET.wait(15)    # (KILL answers at once but the session ends a moment later: its statement may still be on its way until it is off the process list)
        self.resolve("after the session was killed")
        for k in self.touch:
            if k in self.oracle:
                resp = self.x(f"UPDATE c SET v = v WHERE id = {k}")
                if not resp.startswith("OK") and "1317" not in resp:
                    self.bad(f"write of key {k} after the kill: {resp[:100]!r}")
        self.touch = []

    def run(self):
        need_resolve = False
        try:
            while not self.stop:
                try:
                    if self.db is None:
                        self.db = connect()
                    if need_resolve:
                        self.after_kill()
                        need_resolve = False
                    self.step()
                except socket.timeout:
                    if not SERVER_DOWN.is_set():
                        self.bad("no answer for 60 s (a hang)")
                    self.drop()
                    need_resolve = True
                except OSError:                       # the session was killed (or the server was)
                    if SERVER_DOWN.is_set():
                        return
                    self.drop()
                    need_resolve = True
                    self.stats["reconnects"] += 1
                    time.sleep(0.01)
                    if not server_up():
                        if not SERVER_DOWN.is_set():   # (the test may have killed it a moment ago: the flag is set before the kill)
                            self.bad("the server is gone (it was not killed by the test)")
                        return
        except Exception as e:                        # noqa: BLE001
            if not SERVER_DOWN.is_set():
                self.bad(f"worker died: {type(e).__name__}: {e}")
        finally:
            self.drop()

    def step(self):
        r = self.rnd.random()
        self.stats["ops"] += 1
        if r < 0.14:
            self.txn()
        elif r < 0.20:
            self.range_update()
        elif r < 0.30:
            self.heavy()
        else:
            self.point()

    def done(self, resp, new, what):
        """The answer of a statement that changes `oracle` into `new`."""
        if resp.startswith("OK"):
            self.oracle, self.pending = new, None
        elif "1317" in resp:
            self.stats["interrupted"] += 1
            self.unchanged(what)
        else:
            self.pending = None
            self.bad(f"{what}: unexpected answer {resp[:120]!r}")

    def point(self):
        k = self.lo + self.rnd.randint(1, 200)
        new = dict(self.oracle)
        if k not in self.oracle:
            v = self.rnd.randint(0, 999)
            sql, new[k] = f"INSERT INTO c VALUES ({k},{v})", v
        elif self.rnd.random() < 0.6:
            v = self.rnd.randint(0, 999)
            sql, new[k] = f"UPDATE c SET v = {v} WHERE id = {k}", v
        else:
            sql = f"DELETE FROM c WHERE id = {k}"
            new.pop(k)
        self.pending = (dict(self.oracle), [new])
        self.done(self.x(sql), new, sql)

    def range_update(self):
        new = {k: v + 1 for k, v in self.oracle.items()}
        sql = f"UPDATE c SET v = v + 1 WHERE id >= {self.lo} AND id < {self.hi}"
        self.pending = (dict(self.oracle), [new])
        self.done(self.x(sql), new, "the whole-range UPDATE")

    def heavy(self):
        resp = self.x(HEAVY, watch=False)   # (a long SELECT may take long: that is what it is for)
        if resp.startswith("OK"):
            m = ONE.search(resp)
            self.stats["heavy_done"] += 1
            if not m or int(m.group(1)) != self.heavy_expected:
                self.bad(f"the long SELECT answered {m.group(1) if m else resp[:80]!r}, not {self.heavy_expected} (a partial result passed off as an answer?)")
        elif "1317" in resp:
            self.stats["interrupted"] += 1
        else:
            self.bad(f"the long SELECT: unexpected answer {resp[:120]!r}")

    def txn(self):
        self.stats["txns"] += 1
        keys = list(dict.fromkeys(self.lo + self.rnd.randint(1, 200) for _ in range(3)))
        vals = {k: self.rnd.randint(0, 999) for k in keys}
        old = dict(self.oracle)
        self.touch = keys
        self.pending = (old, [])                 # until COMMIT is sent nothing of it may be visible
        stopped = False
        self.x("BEGIN")
        for k, v in vals.items():
            resp = self.x(f"UPDATE c SET v = {v} WHERE id = {k}" if k in old else f"INSERT INTO c VALUES ({k},{v})")
            if "1317" in resp:
                stopped = True
                self.stats["interrupted"] += 1
                break
            if not resp.startswith("OK"):
                self.bad(f"statement of a transaction: unexpected answer {resp[:120]!r}")
                stopped = True
                break
        if stopped or self.rnd.random() < 0.3:
            for _ in range(10):
                if self.x("ROLLBACK").startswith("OK"):
                    break
            self.unchanged("a rolled-back transaction")
            self.touch = []
            return
        new = dict(old)
        new.update(vals)
        self.pending = (old, [new])              # the COMMIT is in flight
        resp = self.x("COMMIT")
        if resp.startswith("OK"):
            self.oracle, self.pending = new, None
        elif "1317" in resp:
            self.stats["interrupted"] += 1
            self.resolve("a COMMIT that was stopped")
        else:
            self.bad(f"COMMIT: unexpected answer {resp[:120]!r}")
        self.touch = []


class Killer(threading.Thread):
    def __init__(self, seed):
        super().__init__(daemon=True)
        self.rnd, self.own, self.stop = random.Random(seed), None, False
        self.stats = dict(query=0, conn=0, missed=0)
        self.violations = []

    def run(self):
        try:
            db = connect(None)
            self.own = session_ids(db)[0]   # (alone on the server: the only row is this session)
            while not self.stop:
                time.sleep(self.rnd.uniform(0.005, 0.04))
                ids = [i for i in session_ids(db) if i != self.own]
                if not ids:
                    continue
                victim = self.rnd.choice(ids)
                conn = self.rnd.random() < 0.3
                if conn:
                    QUIET.clear()
                try:
                    resp = db.execute(f"KILL {victim}" if conn else f"KILL QUERY {victim}")
                    if resp.startswith("OK"):
                        self.stats["conn" if conn else "query"] += 1
                        if conn:        # KILL answers at once; the session is gone from the process list a moment later, and not before its statement is over
                            t0 = time.time()
                            while time.time() - t0 < 10:
                                if victim not in session_ids(db):
                                    break
                                time.sleep(0.02)
                            else:
                                self.violations.append(f"session {victim} was still on the process list 10 s after KILL {victim}")
                    else:
                        self.stats["missed"] += 1      # (the session was gone already: Unknown thread id)
                finally:
                    QUIET.set()
        except Exception:                          # noqa: BLE001
            pass                                   # the server went away


def setup():
    db = connect(None)
    db.execute("CREATE DATABASE d")
    db.execute("USE d")
    db.execute("CREATE TABLE c (id INT PRIMARY KEY, v INT)")
    db.execute("CREATE INDEX iv ON c (v)")
    db.execute("CREATE TABLE big (id INT PRIMARY KEY, g INT, v INT)")
    for start in range(1, BIG_ROWS + 1, 500):
        values = ",".join(f"({i},{i % 100},{i % 7})" for i in range(start, min(start + 500, BIG_ROWS + 1)))
        assert db.execute(f"INSERT INTO big VALUES {values}").startswith("OK")
    m = ONE.search(db.execute(HEAVY))
    db.close()
    return int(m.group(1))


def run_round(seed, run_seconds, crash):
    rnd = random.Random(seed)
    SERVER_DOWN.clear()
    shutil.rmtree(DATA, ignore_errors=True)
    checkpoint = rnd.choice([3000, 20000, 4 * 1024 * 1024])
    p = start(checkpoint)
    heavy_expected = setup()
    killer = Killer(seed * 7 + 1)
    workers = [Worker(i, seed * 10 + i, heavy_expected) for i in range(THREADS)]
    killer.start()          # the killer connects first and learns its own session id while it is alone on the server
    time.sleep(0.3)
    for w in workers:
        w.start()
    time.sleep(run_seconds)

    ok = True
    if crash:
        SERVER_DOWN.set()
        p.kill()
        p.wait()
    killer.stop = True
    for w in workers:
        w.stop = True
    for w in workers:
        w.join(timeout=90)
        if w.is_alive():
            sql, when = w.last or ("?", time.time())
            print(f"VIOLATION seed={seed} worker={w.wid}: still running 90 s after the stop (hung?); last statement `{sql}` sent {time.time() - when:.0f}s ago, pending={w.pending is not None}, db={'none' if w.db is None else 'open'}")
            import faulthandler
            faulthandler.dump_traceback(all_threads=True)   # where is every thread stuck
            try:
                print("  sessions on the server:", session_ids(connect(None)))
            except Exception as e:                   # noqa: BLE001
                print("  the server does not answer a new session either:", type(e).__name__, e)
            ok = False
    killer.join(timeout=15)
    for v in killer.violations:
        print(f"VIOLATION seed={seed} killer: {v}")
        ok = False
    if crash:
        p = start(checkpoint)
    verify = connect()
    # ── the checks on the server that is up ────────────────────────────────────────────────────────────────────────────────────────────────────
    if not crash:
        alive = session_ids(verify)
        for _ in range(50):
            if len(alive) == 1:
                break
            time.sleep(0.1)
            alive = session_ids(verify)
        if len(alive) != 1:
            print(f"VIOLATION seed={seed}: {len(alive) - 1} session(s) left on the server after every client was gone: {alive}")
            ok = False
    resp = verify.execute("SELECT id, v FROM c ORDER BY id")
    got = rows_of(resp)
    for w in workers:
        mine = {k: v for k, v in got.items() if w.lo <= k < w.hi}
        allowed = [w.oracle] + (list(w.pending[1]) + [w.pending[0]] if w.pending else [])
        if not any(mine == a for a in allowed):
            miss = {k: v for k, v in w.oracle.items() if mine.get(k) != v}
            extra = {k: v for k, v in mine.items() if w.oracle.get(k) != v}
            print(f"MISMATCH seed={seed} worker={w.wid} pending={w.pending is not None} missing/wrong={dict(list(miss.items())[:5])} unexpected/wrong={dict(list(extra.items())[:5])}")
            ok = False
    for v in sorted(set(got.values()))[:40]:       # the secondary index must agree with the scan
        m = ONE.search(verify.execute(f"SELECT COUNT(*) FROM c WHERE v = {v}"))
        want = sum(1 for x in got.values() if x == v)
        if not m or int(m.group(1)) != want:
            print(f"MISMATCH seed={seed} index iv: v = {v} finds {m.group(1) if m else '?'} row(s), the table has {want}")
            ok = False
    check = verify.execute("CHECK TABLE c")
    if not check.startswith("OK") or re.search(r"corrupt|error|invalid", check, re.I):
        print(f"MISMATCH seed={seed} CHECK TABLE c: {check[:200]!r}")
        ok = False
    # no row may stay locked: a statement over the whole table has to get through
    box = {}

    def lock_all():
        try:
            c = connect()
            c.sock.settimeout(SLOW + 4)
            c.execute("BEGIN")
            box["resp"] = c.execute("UPDATE c SET v = v WHERE id >= 0")[:60]
            c.execute("ROLLBACK")
            c.close()
        except Exception as e:                    # noqa: BLE001
            box["resp"] = f"{type(e).__name__}: {e}"
    t = threading.Thread(target=lock_all, daemon=True)
    t.start()
    t.join(SLOW + 6)
    if t.is_alive() or not box.get("resp", "").startswith("OK"):
        print(f"VIOLATION seed={seed}: a row stayed locked after the KILLs: {box.get('resp')!r}")
        ok = False
    verify.close()
    stats = {k: sum(w.stats[k] for w in workers) for k in workers[0].stats}
    for w in workers:
        for v in w.violations:
            print(f"VIOLATION seed={seed} worker={w.wid}: {v}")
            ok = False
    if crash:
        p.terminate()
    else:
        p.kill()
    p.wait()
    return ok, stats, killer.stats, len(got)


if __name__ == "__main__":
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    base = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    bad, total = 0, dict(ops=0, txns=0, interrupted=0, reconnects=0, heavy_done=0, query=0, conn=0, rows=0)
    try:
        for i in range(rounds):
            seed = base + i
            ok, ws, ks, n = run_round(seed, random.Random(seed).uniform(1.5, 4.0), crash=bool(seed % 2))
            for k, v in list(ws.items()) + list(ks.items()):
                total[k] = total.get(k, 0) + v
            total["rows"] += n
            print(f"seed {seed} ({'kill -9 + restart' if seed % 2 else 'live'}): {'ok' if ok else 'FAILED'}  ops={ws['ops']} txns={ws['txns']} stopped statements={ws['interrupted']} reconnects={ws['reconnects']} "
                  f"KILL QUERY={ks['query']} KILL={ks['conn']} rows={n}", flush=True)
            if not ok:
                bad += 1
    finally:
        shutil.rmtree(DATA, ignore_errors=True)
    print(f"rounds={rounds} failed={bad} | ops={total['ops']} txns={total['txns']} stopped statements={total['interrupted']} reconnects={total['reconnects']} "
          f"long SELECTs that finished={total['heavy_done']} KILL QUERY={total['query']} KILL={total['conn']} rows checked={total['rows']}")
    sys.exit(1 if bad else 0)
