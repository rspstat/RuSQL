"""Crash-consistency check for tables with a foreign key (the sibling of fuzz_crash.py, which only has tables without one).

An INSERT into a child table has two tables in its lock set (the child and the parent it reads); it is made durable by the redo log like an INSERT into any
table. UPDATE / DELETE of a child, and anything on a parent that has children, keep the older flush-the-tables persistence (a cascade is not logged).
Random autocommit statements and explicit transactions (COMMIT / ROLLBACK) run on

    p (id INT PRIMARY KEY, v INT)
    c (id INT PRIMARY KEY, pid INT, v INT, FOREIGN KEY (pid) REFERENCES p(id))      -- RESTRICT

the server is killed (kill -9) at a random moment and restarted, and both tables must hold exactly what the server acknowledged; the foreign key must
still work after recovery (a child of a missing parent is refused, a parent with a child cannot be deleted).

  python fuzz_crash_fk.py [rounds] [base_seed]        (RUSQL_SERVER_EXE=<path> to use another build)
"""
import os, random, re, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzz_crash as F

bench = F.bench


def rows_of(db, table, cols):
    r = db.execute(f"SELECT {', '.join(cols)} FROM {table} ORDER BY id")
    out = {}
    for line in r.splitlines():
        m = re.match(r"\|" + r"\s*(-?\d+|NULL)\s*\|" * len(cols), line)
        if m:
            vals = tuple(None if x == "NULL" else int(x) for x in m.groups())
            out[vals[0]] = vals[1:]
    return out


def run_round(seed, n_ops):
    rnd = random.Random(seed)
    shutil.rmtree(F.DATA, ignore_errors=True)
    proc = F.start()
    db = bench.RuSQL()
    db.execute("CREATE DATABASE d"); db.execute("USE d")
    db.execute("CREATE TABLE p (id INT PRIMARY KEY, v INT)")
    db.execute("CREATE TABLE c (id INT PRIMARY KEY, pid INT, v INT, FOREIGN KEY (pid) REFERENCES p(id))")
    oracle = {"p": {}, "c": {}}   # acknowledged-committed state: id -> (v,) / (pid, v)
    buf = None                     # open explicit transaction: {table: {id: tuple | None}}

    def view(t):
        base = dict(oracle[t])
        if buf is not None:
            for k, val in buf.get(t, {}).items():
                if val is None:
                    base.pop(k, None)
                else:
                    base[k] = val
        return base

    def record(t, k, val):
        if buf is not None:
            buf.setdefault(t, {})[k] = val
        elif val is None:
            oracle[t].pop(k, None)
        else:
            oracle[t][k] = val

    for _ in range(n_ops):
        op = rnd.random()
        pv, cv = view("p"), view("c")
        if op < 0.05 and buf is None:
            db.execute("BEGIN"); buf = {}
        elif op < 0.12 and buf is not None:
            db.execute("COMMIT")
            for t, ch in buf.items():
                for k, val in ch.items():
                    if val is None: oracle[t].pop(k, None)
                    else: oracle[t][k] = val
            buf = None
        elif op < 0.16 and buf is not None:
            db.execute("ROLLBACK"); buf = None
        elif op < 0.30:                       # INSERT parent
            k = rnd.randint(1, 12)
            if k in pv: continue
            v = rnd.randint(0, 99)
            if db.execute(f"INSERT INTO p VALUES ({k}, {v})").startswith("OK"):
                record("p", k, (v,))
        elif op < 0.62:                       # INSERT child (one row, or two) of an existing parent
            if not pv: continue
            rows = []
            for _ in range(rnd.choice([1, 1, 1, 2])):
                k = rnd.randint(1, 40)
                if k in cv or any(r[0] == k for r in rows): continue
                rows.append((k, rnd.choice(list(pv)), rnd.randint(0, 99)))
            if not rows: continue
            r = db.execute("INSERT INTO c VALUES " + ", ".join(f"({a}, {b}, {v})" for a, b, v in rows))
            if r.startswith("OK"):
                for a, b, v in rows: record("c", a, (b, v))
        elif op < 0.70:                       # INSERT child of a parent that is not there: refused
            missing = [k for k in range(1, 13) if k not in pv]
            if not missing: continue
            k = rnd.randint(41, 60)
            if db.execute(f"INSERT INTO c VALUES ({k}, {rnd.choice(missing)}, 0)").startswith("OK"):
                print(f"MISMATCH seed={seed}: a child of a missing parent was accepted")
                return False
        elif op < 0.78:                       # UPDATE child (moves it to another parent, or changes v)
            if not cv: continue
            k = rnd.choice(list(cv))
            if rnd.random() < 0.5 and pv:
                np_ = rnd.choice(list(pv))
                if db.execute(f"UPDATE c SET pid = {np_} WHERE id = {k}").startswith("OK"):
                    record("c", k, (np_, cv[k][1]))
            else:
                v = rnd.randint(0, 99)
                if db.execute(f"UPDATE c SET v = {v} WHERE id = {k}").startswith("OK"):
                    record("c", k, (cv[k][0], v))
        elif op < 0.86:                       # DELETE child
            if not cv: continue
            k = rnd.choice(list(cv))
            if db.execute(f"DELETE FROM c WHERE id = {k}").startswith("OK"):
                record("c", k, None)
        elif op < 0.93:                       # UPDATE parent
            if not pv: continue
            k = rnd.choice(list(pv)); v = rnd.randint(0, 99)
            if db.execute(f"UPDATE p SET v = {v} WHERE id = {k}").startswith("OK"):
                record("p", k, (v,))
        else:                                 # DELETE parent: refused while a child refers to it
            if not pv: continue
            k = rnd.choice(list(pv))
            has_child = any(val[0] == k for val in cv.values())
            r = db.execute(f"DELETE FROM p WHERE id = {k}")
            if r.startswith("OK"):
                if has_child:
                    print(f"MISMATCH seed={seed}: parent {k} was deleted with a child")
                    return False
                record("p", k, None)
    proc.kill(); proc.wait()          # (an open explicit transaction was never acknowledged)
    proc = F.start()
    db = bench.RuSQL(); db.execute("USE d")
    got = {"p": rows_of(db, "p", ["id", "v"]), "c": rows_of(db, "c", ["id", "pid", "v"])}
    ok = got == oracle
    extra = []
    if ok:                            # the foreign key still works on the recovered tables
        if db.execute("INSERT INTO c VALUES (999, 9999, 0)").startswith("OK"):
            extra.append("a child of a missing parent was accepted after recovery")
        for k, (pid, v) in list(oracle["c"].items())[:3]:
            if db.execute(f"DELETE FROM p WHERE id = {pid}").startswith("OK"):
                extra.append(f"parent {pid} of child {k} was deleted after recovery")
    db.close(); proc.terminate(); proc.wait()
    if not ok or extra:
        print(f"MISMATCH seed={seed} ops={n_ops}")
        for t in ("p", "c"):
            if got[t] != oracle[t]:
                print(f"  table {t}: expected-but-wrong/missing={ {k: v for k, v in oracle[t].items() if got[t].get(k) != v} } unexpected/wrong={ {k: v for k, v in got[t].items() if oracle[t].get(k) != v} }")
        for line in extra:
            print("  " + line)
        return False
    return True


if __name__ == "__main__":
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    base = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
    bad = 0
    for i in range(rounds):
        if not run_round(base + i, random.Random(base + i).randint(10, 120)):
            bad += 1
    print(f"rounds={rounds} mismatches={bad}")
    shutil.rmtree(F.DATA, ignore_errors=True)
    sys.exit(1 if bad else 0)
