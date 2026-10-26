"""Crash-consistency check for a table whose foreign key points at the table itself (R55; the sibling of fuzz_crash_fk.py).

Random autocommit statements and explicit transactions (COMMIT / ROLLBACK / savepoints) run on

    emp (id INT PRIMARY KEY, mgr INT, v INT, FOREIGN KEY (mgr) REFERENCES emp(id) ON DELETE <a>)

where <a> is RESTRICT, CASCADE or SET NULL (chosen by the seed): INSERT of rows under an existing row or none, UPDATE of v and of mgr (never under a row's own subtree),
DELETE of one row or of several (`WHERE id IN (...)`, ascending or `ORDER BY id DESC`) with whatever the foreign key then does to the rows that refer to them. The server is
killed (kill -9) at a random moment and restarted, and the table must hold exactly what the server acknowledged (a model of InnoDB's row-by-row work says what an acknowledged
statement did, and which statement is refused); the foreign key must still work after recovery.

  python fuzz_crash_selfref.py [rounds] [base_seed]        (RUSQL_SERVER_EXE=<path> to use another build)
"""
import os, random, re, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzz_crash as F

bench = F.bench
ACTIONS = ["RESTRICT", "CASCADE", "SET NULL"]


def rows_of(db):
    r = db.execute("SELECT id, mgr, v FROM emp ORDER BY id")
    out = {}
    for line in r.splitlines():
        m = re.match(r"\|\s*(-?\d+|NULL)\s*\|\s*(-?\d+|NULL)\s*\|\s*(-?\d+|NULL)\s*\|", line)
        if m:
            vals = tuple(None if x == "NULL" else int(x) for x in m.groups())
            out[vals[0]] = vals[1:]
    return out


class Refused(Exception):
    pass


def model_delete(state, order, action):
    """The rows left after DELETE of `order` (ids, in the order the scan reads them), or None when InnoDB refuses the statement."""
    st = dict(state)

    def go(k, level):
        kids = [i for i, (m, _) in st.items() if m == k and i != k]
        if action == "RESTRICT":
            if kids or st[k][0] == k:
                raise Refused
            del st[k]
        elif action == "CASCADE":
            if kids and level + 1 >= 15:
                raise Refused
            del st[k]
            for c in kids:
                if c in st:
                    go(c, level + 1)
        else:
            for c in kids:
                st[c] = (None, st[c][1])
            del st[k]

    try:
        for k in order:
            if k in st:
                go(k, 0)
    except Refused:
        return None
    return st


def subtree(state, k):
    out, todo = {k}, [k]
    while todo:
        cur = todo.pop()
        for i, (m, _) in state.items():
            if m == cur and i not in out:
                out.add(i)
                todo.append(i)
    return out


def run_round(seed, n_ops):
    rnd = random.Random(seed)
    action = ACTIONS[seed % 3]
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
    db.execute(f"CREATE TABLE emp (id INT PRIMARY KEY, mgr INT, v INT, FOREIGN KEY (mgr) REFERENCES emp(id) ON DELETE {action})")
    oracle = {}           # acknowledged-committed state: id -> (mgr, v)
    buf = None            # open explicit transaction: a copy of the state the session sees
    savepoints = []       # [(name, state)]
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
        cur = dict(view())
        if op < 0.04 and buf is None:
            db.execute("BEGIN"); buf = dict(oracle); savepoints = []
        elif op < 0.10 and buf is not None:
            db.execute("COMMIT"); oracle = buf; buf = None; savepoints = []
        elif op < 0.13 and buf is not None:
            db.execute("ROLLBACK"); buf = None; savepoints = []
        elif op < 0.15 and buf is not None:
            n_savepoint += 1
            db.execute(f"SAVEPOINT sp{n_savepoint}"); savepoints.append((f"sp{n_savepoint}", dict(buf)))
        elif op < 0.17 and buf is not None and savepoints:
            i = rnd.randrange(len(savepoints))
            name, state = savepoints[i]
            db.execute(f"ROLLBACK TO {name}"); buf = dict(state); savepoints = savepoints[:i + 1]
        elif op < 0.45:                       # INSERT rows under an existing row, or under none (a later row may name an earlier one of the same statement)
            rows = []
            for _ in range(rnd.choice([1, 1, 2, 3])):
                k = rnd.randint(1, 40)
                if k in cur or any(r[0] == k for r in rows): continue
                known = list(cur) + [r[0] for r in rows]
                rows.append((k, rnd.choice(known) if known and rnd.random() < 0.8 else None, rnd.randint(0, 99)))
            if not rows: continue
            vals = ", ".join(f"({a}, {'NULL' if b is None else b}, {v})" for a, b, v in rows)
            if db.execute("INSERT INTO emp VALUES " + vals).startswith("OK"):
                for a, b, v in rows: cur[a] = (b, v)
                commit_change(cur)
        elif op < 0.50:                       # INSERT under a row that is not there: refused
            missing = [k for k in range(1, 41) if k not in cur]
            if len(missing) < 2: continue
            if db.execute(f"INSERT INTO emp VALUES ({missing[0]}, {missing[1]}, 0)").startswith("OK"):
                print(f"MISMATCH seed={seed}: a row under a missing row was accepted")
                return False
        elif op < 0.58:                       # UPDATE v
            if not cur: continue
            k = rnd.choice(list(cur)); v = rnd.randint(0, 99)
            if db.execute(f"UPDATE emp SET v = {v} WHERE id = {k}").startswith("OK"):
                cur[k] = (cur[k][0], v); commit_change(cur)
        elif op < 0.68:                       # UPDATE mgr: under another row (not one of its own subtree), or none
            if not cur: continue
            k = rnd.choice(list(cur))
            below = subtree(cur, k)
            choices = [i for i in cur if i not in below]
            target = rnd.choice(choices) if choices and rnd.random() < 0.8 else None
            if db.execute(f"UPDATE emp SET mgr = {'NULL' if target is None else target} WHERE id = {k}").startswith("OK"):
                cur[k] = (target, cur[k][1]); commit_change(cur)
        elif op < 0.74:                       # UPDATE of the key of a row without children (ON UPDATE RESTRICT): moved; with children: refused
            if not cur: continue
            k = rnd.choice(list(cur)); target = rnd.randint(41, 80)
            if target in cur: continue
            kids = [i for i, (m, _) in cur.items() if m == k]
            r = db.execute(f"UPDATE emp SET id = {target} WHERE id = {k}")
            if kids:
                if r.startswith("OK"):
                    print(f"MISMATCH seed={seed}: row {k} was renumbered with children"); return False
                continue
            if r.startswith("OK"):
                cur[target] = cur.pop(k); commit_change(cur)
        else:                                 # DELETE one row or several, in the order the scan reads them
            if not cur: continue
            if rnd.random() < 0.5:
                ids = [rnd.choice(list(cur))]
                sql = f"DELETE FROM emp WHERE id = {ids[0]}"
                order = ids
            else:
                ids = rnd.sample(list(cur), min(len(cur), rnd.randint(2, 4)))
                descending = rnd.random() < 0.5
                order = sorted(ids, reverse=descending)
                sql = f"DELETE FROM emp WHERE id IN ({', '.join(map(str, ids))})" + (" ORDER BY id DESC" if descending else "")
            expected = model_delete(cur, order, action)
            r = db.execute(sql)
            if expected is None:
                if r.startswith("OK"):
                    print(f"MISMATCH seed={seed}: {sql} was accepted under {action} and MySQL refuses it"); return False
                continue
            if not r.startswith("OK"):
                print(f"MISMATCH seed={seed}: {sql} was refused under {action}: {r.splitlines()[0][:100]}"); return False
            commit_change(expected)
    proc.kill(); proc.wait()          # (an open explicit transaction was never acknowledged)
    proc = F.start()
    db = bench.RuSQL(); db.execute("USE d")
    got = rows_of(db)
    ok = got == oracle
    extra = []
    if ok:                            # the foreign key still works on the recovered table
        missing = [k for k in range(1, 41) if k not in oracle]
        if len(missing) >= 2 and db.execute(f"INSERT INTO emp VALUES ({missing[0]}, {missing[1]}, 0)").startswith("OK"):
            extra.append("a row under a missing row was accepted after recovery")
        if action == "RESTRICT":
            for k, (mgr, v) in list(oracle.items())[:3]:
                if mgr is not None and db.execute(f"DELETE FROM emp WHERE id = {mgr}").startswith("OK"):
                    extra.append(f"row {mgr}, the manager of {k}, was deleted after recovery")
    db.close(); proc.terminate(); proc.wait()
    if not ok or extra:
        print(f"MISMATCH seed={seed} ops={n_ops} ON DELETE {action}")
        print(f"  expected-but-wrong/missing={ {k: v for k, v in oracle.items() if got.get(k) != v} } unexpected/wrong={ {k: v for k, v in got.items() if oracle.get(k) != v} }")
        for line in extra:
            print("  " + line)
        return False
    return True


if __name__ == "__main__":
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    base = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    bad = 0
    for i in range(rounds):
        if not run_round(base + i, random.Random(base + i).randint(10, 120)):
            bad += 1
    print(f"rounds={rounds} mismatches={bad}")
    shutil.rmtree(F.DATA, ignore_errors=True)
    sys.exit(1 if bad else 0)
