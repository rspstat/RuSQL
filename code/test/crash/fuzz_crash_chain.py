"""Crash-consistency check for a chain of tables with foreign keys (R56; the sibling of fuzz_crash_fk.py and fuzz_crash_selfref.py).

Random autocommit statements and explicit transactions (COMMIT / ROLLBACK / savepoints) run on

    a (id INT PRIMARY KEY, v INT)
    b (id INT PRIMARY KEY, a_id INT, v INT, FOREIGN KEY (a_id) REFERENCES a(id) ON DELETE <x>)
    c (id INT PRIMARY KEY, b_id INT, v INT, FOREIGN KEY (b_id) REFERENCES b(id) ON DELETE <y>)

where <x> and <y> are each RESTRICT, CASCADE or SET NULL (chosen by the seed): INSERT of b / c rows under an existing row or none, UPDATE of v and of the foreign key columns, DELETE of one
row or several of a, b and c with whatever the two foreign keys then do down the chain. The server is killed (kill -9) at a random moment and restarted, and the three tables must hold
exactly what the server acknowledged (a model of the cascade -- the grandchildren go with the children, RESTRICT anywhere below refuses the statement whole -- says what an acknowledged
statement did, and which one is refused); the foreign keys must still work after recovery.

  python fuzz_crash_chain.py [rounds] [base_seed]        (RUSQL_SERVER_EXE=<path> to use another build)
"""
import os, random, re, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzz_crash as F

bench = F.bench
ACTIONS = ["RESTRICT", "CASCADE", "SET NULL"]
TABLES = ("a", "b", "c")


def rows_of(db, table, cols):
    r = db.execute(f"SELECT {', '.join(cols)} FROM {table} ORDER BY id")
    out = {}
    for line in r.splitlines():
        m = re.match(r"\|" + r"\s*(-?\d+|NULL)\s*\|" * len(cols), line)
        if m:
            vals = tuple(None if x == "NULL" else int(x) for x in m.groups())
            out[vals[0]] = vals[1:]
    return out


class Refused(Exception):
    pass


def model_delete(state, table, ids, actions):
    """The state after DELETE of `ids` from `table` (read in key order), or None when a RESTRICT refuses the statement."""
    st = {t: dict(state[t]) for t in TABLES}
    child_of = {"a": ("b", 0, actions[0]), "b": ("c", 0, actions[1])}  # parent table -> (child table, index of the foreign key column in the child's tuple, ON DELETE)

    def go(t, k):
        kids = []
        if t in child_of:
            ct, col, action = child_of[t]
            kids = [i for i, row in st[ct].items() if row[col] == k]
            if action == "RESTRICT" and kids:
                raise Refused
        del st[t][k]
        if t in child_of:
            ct, col, action = child_of[t]
            for kid in kids:
                if kid not in st[ct]:
                    continue
                if action == "CASCADE":
                    go(ct, kid)
                else:
                    row = list(st[ct][kid])
                    row[col] = None
                    st[ct][kid] = tuple(row)

    try:
        for k in sorted(ids):
            if k in st[table]:
                go(table, k)
    except Refused:
        return None
    return st


def run_round(seed, n_ops):
    rnd = random.Random(seed)
    actions = (ACTIONS[seed % 3], ACTIONS[(seed // 3) % 3])
    shutil.rmtree(F.DATA, ignore_errors=True)
    proc = F.start()
    db = bench.RuSQL()
    if os.environ.get("FUZZ_TRACE"):  # print every statement (to see what a failing seed did)
        plain_execute = db.execute
        def traced(sql):
            answer = plain_execute(sql)
            print(f"  {sql}  ->  {answer.splitlines()[0][:80] if answer else answer}")
            return answer
        db.execute = traced
    db.execute("CREATE DATABASE d"); db.execute("USE d")
    db.execute("CREATE TABLE a (id INT PRIMARY KEY, v INT)")
    db.execute(f"CREATE TABLE b (id INT PRIMARY KEY, a_id INT, v INT, FOREIGN KEY (a_id) REFERENCES a(id) ON DELETE {actions[0]})")
    db.execute(f"CREATE TABLE c (id INT PRIMARY KEY, b_id INT, v INT, FOREIGN KEY (b_id) REFERENCES b(id) ON DELETE {actions[1]})")
    # the state: id -> (v,) for a, (a_id, v) for b, (b_id, v) for c -- the foreign key column is the first of the tuple
    oracle = {"a": {}, "b": {}, "c": {}}
    buf = None        # open explicit transaction: a copy of the state the session sees
    savepoints = []   # [(name, state)]
    n_savepoint = 0

    def copy(state):
        return {t: dict(state[t]) for t in TABLES}

    def view():
        return buf if buf is not None else oracle

    def commit_change(new_state):
        nonlocal buf, oracle
        if buf is not None:
            buf = new_state
        else:
            oracle = new_state

    for _ in range(n_ops):
        op = rnd.random()
        cur = copy(view())
        a, b, c = cur["a"], cur["b"], cur["c"]
        if op < 0.04 and buf is None:
            db.execute("BEGIN"); buf = copy(oracle); savepoints = []
        elif op < 0.10 and buf is not None:
            db.execute("COMMIT"); oracle = buf; buf = None; savepoints = []
        elif op < 0.13 and buf is not None:
            db.execute("ROLLBACK"); buf = None; savepoints = []
        elif op < 0.15 and buf is not None:
            n_savepoint += 1
            db.execute(f"SAVEPOINT sp{n_savepoint}"); savepoints.append((f"sp{n_savepoint}", copy(buf)))
        elif op < 0.17 and buf is not None and savepoints:
            i = rnd.randrange(len(savepoints))
            name, state = savepoints[i]
            db.execute(f"ROLLBACK TO {name}"); buf = copy(state); savepoints = savepoints[:i + 1]
        elif op < 0.27:                       # INSERT a
            k = rnd.randint(1, 12)
            if k in a: continue
            v = rnd.randint(0, 99)
            if db.execute(f"INSERT INTO a VALUES ({k}, {v})").startswith("OK"):
                a[k] = (v,); commit_change(cur)
        elif op < 0.42:                       # INSERT b (under an existing a, or none)
            if not a: continue
            k = rnd.randint(1, 30)
            if k in b: continue
            parent = rnd.choice(list(a)) if rnd.random() < 0.9 else None
            v = rnd.randint(0, 99)
            if db.execute(f"INSERT INTO b VALUES ({k}, {'NULL' if parent is None else parent}, {v})").startswith("OK"):
                b[k] = (parent, v); commit_change(cur)
        elif op < 0.57:                       # INSERT c (under an existing b, or none)
            if not b: continue
            k = rnd.randint(1, 60)
            if k in c: continue
            parent = rnd.choice(list(b)) if rnd.random() < 0.9 else None
            v = rnd.randint(0, 99)
            if db.execute(f"INSERT INTO c VALUES ({k}, {'NULL' if parent is None else parent}, {v})").startswith("OK"):
                c[k] = (parent, v); commit_change(cur)
        elif op < 0.62:                       # INSERT under a row that is not there: refused
            missing = [k for k in range(1, 13) if k not in a]
            if missing and db.execute(f"INSERT INTO b VALUES ({rnd.randint(31, 60)}, {rnd.choice(missing)}, 0)").startswith("OK"):
                print(f"MISMATCH seed={seed}: a b row under a missing a row was accepted"); return False
            missing = [k for k in range(1, 31) if k not in b]
            if missing and db.execute(f"INSERT INTO c VALUES ({rnd.randint(61, 90)}, {rnd.choice(missing)}, 0)").startswith("OK"):
                print(f"MISMATCH seed={seed}: a c row under a missing b row was accepted"); return False
        elif op < 0.70:                       # UPDATE v / the foreign key column of a b or c row
            table = rnd.choice(["b", "c"])
            rows, parents = (b, a) if table == "b" else (c, b)
            if not rows: continue
            k = rnd.choice(list(rows))
            if rnd.random() < 0.5 and parents:
                target = rnd.choice(list(parents))
                col = "a_id" if table == "b" else "b_id"
                if db.execute(f"UPDATE {table} SET {col} = {target} WHERE id = {k}").startswith("OK"):
                    rows[k] = (target, rows[k][1]); commit_change(cur)
            else:
                v = rnd.randint(0, 99)
                if db.execute(f"UPDATE {table} SET v = {v} WHERE id = {k}").startswith("OK"):
                    rows[k] = (rows[k][0], v); commit_change(cur)
        else:                                 # DELETE one row or several, from a, b or c
            table = rnd.choice(["a", "a", "b", "c"])
            rows = cur[table]
            if not rows: continue
            ids = [rnd.choice(list(rows))] if rnd.random() < 0.6 else rnd.sample(list(rows), min(len(rows), rnd.randint(2, 4)))
            sql = f"DELETE FROM {table} WHERE id " + (f"= {ids[0]}" if len(ids) == 1 else f"IN ({', '.join(map(str, ids))})")
            expected = model_delete(cur, table, ids, actions)
            r = db.execute(sql)
            if expected is None:
                if r.startswith("OK"):
                    print(f"MISMATCH seed={seed}: {sql} was accepted with {actions} and MySQL refuses it"); return False
                continue
            if not r.startswith("OK"):
                print(f"MISMATCH seed={seed}: {sql} was refused with {actions}: {r.splitlines()[0][:100]}"); return False
            commit_change(expected)
    proc.kill(); proc.wait()          # (an open explicit transaction was never acknowledged)
    proc = F.start()
    db = bench.RuSQL(); db.execute("USE d")
    got = {"a": rows_of(db, "a", ["id", "v"]), "b": rows_of(db, "b", ["id", "a_id", "v"]), "c": rows_of(db, "c", ["id", "b_id", "v"])}
    ok = got == oracle
    extra = []
    if ok:                            # the foreign keys still work on the recovered tables
        if db.execute("INSERT INTO b VALUES (999, 9999, 0)").startswith("OK"):
            extra.append("a b row under a missing a row was accepted after recovery")
        if db.execute("INSERT INTO c VALUES (999, 9999, 0)").startswith("OK"):
            extra.append("a c row under a missing b row was accepted after recovery")
        if actions[0] == "RESTRICT":
            for k, (parent, v) in list(oracle["b"].items())[:3]:
                if parent is not None and db.execute(f"DELETE FROM a WHERE id = {parent}").startswith("OK"):
                    extra.append(f"a row {parent} that b {k} refers to was deleted after recovery")
    db.close(); proc.terminate(); proc.wait()
    if not ok or extra:
        print(f"MISMATCH seed={seed} ops={n_ops} ON DELETE {actions}")
        for t in TABLES:
            if got[t] != oracle[t]:
                print(f"  table {t}: expected-but-wrong/missing={ {k: v for k, v in oracle[t].items() if got[t].get(k) != v} } unexpected/wrong={ {k: v for k, v in got[t].items() if oracle[t].get(k) != v} }")
        for line in extra:
            print("  " + line)
        return False
    return True


if __name__ == "__main__":
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    base = int(sys.argv[2]) if len(sys.argv) > 2 else 6000
    bad = 0
    for i in range(rounds):
        if not run_round(base + i, random.Random(base + i).randint(10, 140)):
            bad += 1
    print(f"rounds={rounds} mismatches={bad}")
    shutil.rmtree(F.DATA, ignore_errors=True)
    sys.exit(1 if bad else 0)
