"""Crash-consistency check for a column that is in two foreign keys (R57; the sibling of fuzz_crash_chain.py).

Random autocommit statements and explicit transactions (COMMIT / ROLLBACK / savepoints) run on

    pa (id INT PRIMARY KEY, v INT)
    pb (id INT PRIMARY KEY, v INT)
    c  (id INT PRIMARY KEY, x INT, v INT, FOREIGN KEY (x) REFERENCES pa(id) ON DELETE <X>, FOREIGN KEY (x) REFERENCES pb(id) ON DELETE <Y>)

where <X> and <Y> are each RESTRICT, CASCADE or SET NULL (chosen by the seed): INSERT of c rows under a value that is in both parents, in one of them or in none, UPDATE of x and v, DELETE of
one row or several of pa, pb and c, REPLACE of pa / pb rows (one row or several: the replaced row goes through its foreign keys first). The server is killed (kill -9) at a random
moment and restarted, and the three tables must hold exactly what the server acknowledged (a model of the two keys says what an acknowledged statement did and which one is refused: a value
has to be in BOTH parents; a RESTRICT refuses the statement whole; a CASCADE takes the children, a SET NULL nulls their x); both keys must still work after recovery.

  python fuzz_crash_multifk.py [rounds] [base_seed]        (RUSQL_SERVER_EXE=<path> to use another build)
"""
import os, random, re, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzz_crash as F

bench = F.bench
ACTIONS = ["RESTRICT", "CASCADE", "SET NULL"]
TABLES = ("pa", "pb", "c")


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


def copy(state):
    return {t: dict(state[t]) for t in TABLES}


def remove_parent(st, table, k, actions):
    """The row k of pa / pb goes: what the key of that parent does to the children (c.x = k)."""
    action = actions[0] if table == "pa" else actions[1]
    kids = [i for i, (x, v) in st["c"].items() if x == k]
    if action == "RESTRICT" and kids:
        raise Refused
    del st[table][k]
    for kid in kids:
        if kid not in st["c"]:
            continue
        if action == "CASCADE":
            del st["c"][kid]
        else:
            st["c"][kid] = (None, st["c"][kid][1])


def model_delete(state, table, ids, actions):
    """The state after DELETE of `ids` from `table` (read in key order), or None when a RESTRICT refuses the statement."""
    st = copy(state)
    try:
        for k in sorted(ids):
            if k in st[table]:
                if table == "c":
                    del st["c"][k]
                else:
                    remove_parent(st, table, k, actions)
    except Refused:
        return None
    return st


def model_replace(state, table, rows, actions):
    """REPLACE of (id, v) rows into pa / pb, in the order written: the row that is replaced goes first (through the keys), the new one comes; None when refused."""
    st = copy(state)
    try:
        for k, v in rows:
            if k in st[table]:
                remove_parent(st, table, k, actions)
            st[table][k] = (v,)
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
    db.execute("CREATE TABLE pa (id INT PRIMARY KEY, v INT)")
    db.execute("CREATE TABLE pb (id INT PRIMARY KEY, v INT)")
    db.execute(f"CREATE TABLE c (id INT PRIMARY KEY, x INT, v INT, FOREIGN KEY (x) REFERENCES pa(id) ON DELETE {actions[0]}, FOREIGN KEY (x) REFERENCES pb(id) ON DELETE {actions[1]})")
    oracle = {"pa": {}, "pb": {}, "c": {}}    # id -> (v,) for pa and pb, (x, v) for c
    buf = None        # open explicit transaction: a copy of the state the session sees
    savepoints = []   # [(name, state)]
    n_savepoint = 0

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
        pa, pb, c = cur["pa"], cur["pb"], cur["c"]
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
        elif op < 0.32:                       # INSERT pa / pb
            table = rnd.choice(["pa", "pb"])
            k = rnd.randint(1, 12)
            if k in cur[table]: continue
            v = rnd.randint(0, 99)
            if db.execute(f"INSERT INTO {table} VALUES ({k}, {v})").startswith("OK"):
                cur[table][k] = (v,); commit_change(cur)
        elif op < 0.50:                       # INSERT c under a value that is in both parents, in one, or in none (or NULL)
            k = rnd.randint(1, 60)
            if k in c: continue
            x = rnd.choice([None] + list(range(1, 13)))
            v = rnd.randint(0, 99)
            expected = x is None or (x in pa and x in pb)
            r = db.execute(f"INSERT INTO c VALUES ({k}, {'NULL' if x is None else x}, {v})")
            if r.startswith("OK") != expected:
                print(f"MISMATCH seed={seed}: INSERT INTO c x={x} was {'accepted' if r.startswith('OK') else 'refused'} (in pa: {x in pa}, in pb: {x in pb})"); return False
            if expected:
                c[k] = (x, v); commit_change(cur)
        elif op < 0.62:                       # UPDATE x / v of a c row
            if not c: continue
            k = rnd.choice(list(c))
            if rnd.random() < 0.6:
                x = rnd.choice([None] + list(range(1, 13)))
                expected = x is None or (x in pa and x in pb)
                r = db.execute(f"UPDATE c SET x = {'NULL' if x is None else x} WHERE id = {k}")
                if r.startswith("OK") != expected:
                    print(f"MISMATCH seed={seed}: UPDATE c SET x={x} was {'accepted' if r.startswith('OK') else 'refused'} (in pa: {x in pa}, in pb: {x in pb})"); return False
                if expected:
                    c[k] = (x, c[k][1]); commit_change(cur)
            else:
                v = rnd.randint(0, 99)
                if db.execute(f"UPDATE c SET v = {v} WHERE id = {k}").startswith("OK"):
                    c[k] = (c[k][0], v); commit_change(cur)
        elif op < 0.74:                       # REPLACE of one or several pa / pb rows
            table = rnd.choice(["pa", "pb"])
            rows = [(rnd.randint(1, 12), rnd.randint(0, 99)) for _ in range(rnd.randint(1, 3))]
            sql = f"REPLACE INTO {table} VALUES " + ", ".join(f"({k}, {v})" for k, v in rows)
            expected = model_replace(cur, table, rows, actions)
            r = db.execute(sql)
            if expected is None:
                if r.startswith("OK"):
                    print(f"MISMATCH seed={seed}: {sql} was accepted with {actions} and a RESTRICT refuses it"); return False
                continue
            if not r.startswith("OK"):
                print(f"MISMATCH seed={seed}: {sql} was refused with {actions}: {r.splitlines()[0][:100]}"); return False
            commit_change(expected)
        else:                                 # DELETE one row or several, from pa, pb or c
            table = rnd.choice(["pa", "pb", "c"])
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
    got = {"pa": rows_of(db, "pa", ["id", "v"]), "pb": rows_of(db, "pb", ["id", "v"]), "c": rows_of(db, "c", ["id", "x", "v"])}
    ok = got == oracle
    extra = []
    if ok:                            # both foreign keys still work on the recovered tables
        db.execute("INSERT INTO pa VALUES (900, 0)")
        db.execute("INSERT INTO pb VALUES (901, 0)")
        if db.execute("INSERT INTO c VALUES (999, 900, 0)").startswith("OK"):
            extra.append("a c row under a value that is in pa only was accepted after recovery")
        if db.execute("INSERT INTO c VALUES (998, 901, 0)").startswith("OK"):
            extra.append("a c row under a value that is in pb only was accepted after recovery")
        if db.execute("INSERT INTO c VALUES (997, 9999, 0)").startswith("OK"):
            extra.append("a c row under a value that is in neither parent was accepted after recovery")
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
    base = int(sys.argv[2]) if len(sys.argv) > 2 else 7000
    bad = 0
    for i in range(rounds):
        if not run_round(base + i, random.Random(base + i).randint(10, 140)):
            bad += 1
    print(f"rounds={rounds} mismatches={bad}")
    shutil.rmtree(F.DATA, ignore_errors=True)
    sys.exit(1 if bad else 0)
