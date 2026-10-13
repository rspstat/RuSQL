"""Crash-consistency check for tables with a foreign key (the sibling of fuzz_crash.py, which only has tables without one).

Random autocommit statements and explicit transactions (COMMIT / ROLLBACK / savepoints) run on

    p (id INT PRIMARY KEY, v INT)
    c (id INT PRIMARY KEY, pid INT, v INT, FOREIGN KEY (pid) REFERENCES p(id) ON DELETE <a> ON UPDATE <b>)

where <a> and <b> are each RESTRICT, CASCADE or SET NULL (chosen by the seed): INSERT / REPLACE / INSERT IGNORE / ON DUPLICATE KEY UPDATE of children, UPDATE and DELETE of
children, INSERT / UPDATE / DELETE of parents with whatever the foreign key then does to the children. The server is killed (kill -9) at a random moment and restarted, and
both tables must hold exactly what the server acknowledged (a model of the three actions says what an acknowledged statement did); the foreign key must still work after
recovery (a child of a missing parent is refused, a parent with a child cannot be deleted under RESTRICT).

  python fuzz_crash_fk.py [rounds] [base_seed]        (RUSQL_SERVER_EXE=<path> to use another build)
"""
import os, random, re, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzz_crash as F

bench = F.bench
ACTIONS = ["RESTRICT", "CASCADE", "SET NULL"]


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
    on_delete, on_update = ACTIONS[seed % 3], ACTIONS[(seed // 3) % 3]
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
    db.execute("CREATE TABLE p (id INT PRIMARY KEY, v INT)")
    db.execute(f"CREATE TABLE c (id INT PRIMARY KEY, pid INT, v INT, FOREIGN KEY (pid) REFERENCES p(id) ON DELETE {on_delete} ON UPDATE {on_update})")
    oracle = {"p": {}, "c": {}}   # acknowledged-committed state: id -> (v,) / (pid, v)
    buf = None                     # open explicit transaction: a copy of the state the session sees
    savepoints = []                # [(name, state)]

    def copy(state):
        return {"p": dict(state["p"]), "c": dict(state["c"])}

    def view():
        return buf if buf is not None else oracle

    def commit_change(new_state):
        nonlocal buf
        if buf is not None:
            buf = new_state
        else:
            oracle["p"], oracle["c"] = new_state["p"], new_state["c"]

    def children_of(state, parent):
        return [k for k, (pid, _) in state["c"].items() if pid == parent]

    n_savepoint = 0
    for _ in range(n_ops):
        op = rnd.random()
        cur = copy(view())
        pv, cv = cur["p"], cur["c"]
        if op < 0.04 and buf is None:
            db.execute("BEGIN"); buf = copy(oracle); savepoints = []
        elif op < 0.10 and buf is not None:
            db.execute("COMMIT"); oracle["p"], oracle["c"] = buf["p"], buf["c"]; buf = None; savepoints = []
        elif op < 0.13 and buf is not None:
            db.execute("ROLLBACK"); buf = None; savepoints = []
        elif op < 0.15 and buf is not None:
            n_savepoint += 1
            db.execute(f"SAVEPOINT sp{n_savepoint}"); savepoints.append((f"sp{n_savepoint}", copy(buf)))
        elif op < 0.17 and buf is not None and savepoints:
            i = rnd.randrange(len(savepoints))
            name, state = savepoints[i]
            db.execute(f"ROLLBACK TO {name}"); buf = copy(state); savepoints = savepoints[:i + 1]
        elif op < 0.27:                       # INSERT parent
            k = rnd.randint(1, 12)
            if k in pv: continue
            v = rnd.randint(0, 99)
            if db.execute(f"INSERT INTO p VALUES ({k}, {v})").startswith("OK"):
                pv[k] = (v,); commit_change(cur)
        elif op < 0.50:                       # INSERT child (one row, or two) of an existing parent, or NULL
            if not pv: continue
            rows = []
            for _ in range(rnd.choice([1, 1, 1, 2])):
                k = rnd.randint(1, 40)
                if k in cv or any(r[0] == k for r in rows): continue
                rows.append((k, rnd.choice(list(pv)) if rnd.random() < 0.9 else None, rnd.randint(0, 99)))
            if not rows: continue
            vals = ", ".join(f"({a}, {'NULL' if b is None else b}, {v})" for a, b, v in rows)
            r = db.execute("INSERT INTO c VALUES " + vals)
            if r.startswith("OK"):
                for a, b, v in rows: cv[a] = (b, v)
                commit_change(cur)
        elif op < 0.56:                       # REPLACE / IGNORE / ON DUPLICATE KEY UPDATE on a child
            if not pv: continue
            k = rnd.randint(1, 40); parent = rnd.choice(list(pv)); v = rnd.randint(0, 99)
            kind = rnd.choice(["replace", "ignore", "odku"])
            if kind == "replace":
                r = db.execute(f"REPLACE INTO c VALUES ({k}, {parent}, {v})")
                if r.startswith("OK"): cv[k] = (parent, v); commit_change(cur)
            elif kind == "ignore":
                r = db.execute(f"INSERT IGNORE INTO c VALUES ({k}, {parent}, {v})")
                if r.startswith("OK"):
                    if k not in cv: cv[k] = (parent, v)
                    commit_change(cur)
            else:
                r = db.execute(f"INSERT INTO c VALUES ({k}, {parent}, {v}) ON DUPLICATE KEY UPDATE v = {v}")
                if r.startswith("OK"):
                    cv[k] = (cv[k][0], v) if k in cv else (parent, v)
                    commit_change(cur)
        elif op < 0.62:                       # INSERT child of a parent that is not there: refused
            missing = [k for k in range(1, 13) if k not in pv]
            if not missing: continue
            k = rnd.randint(41, 60)
            if db.execute(f"INSERT INTO c VALUES ({k}, {rnd.choice(missing)}, 0)").startswith("OK"):
                print(f"MISMATCH seed={seed}: a child of a missing parent was accepted")
                return False
        elif op < 0.70:                       # UPDATE child (moves it to another parent, or changes v)
            if not cv: continue
            k = rnd.choice(list(cv))
            if rnd.random() < 0.5 and pv:
                np_ = rnd.choice(list(pv))
                if db.execute(f"UPDATE c SET pid = {np_} WHERE id = {k}").startswith("OK"):
                    cv[k] = (np_, cv[k][1]); commit_change(cur)
            else:
                v = rnd.randint(0, 99)
                if db.execute(f"UPDATE c SET v = {v} WHERE id = {k}").startswith("OK"):
                    cv[k] = (cv[k][0], v); commit_change(cur)
        elif op < 0.76:                       # DELETE child
            if not cv: continue
            k = rnd.choice(list(cv))
            if db.execute(f"DELETE FROM c WHERE id = {k}").startswith("OK"):
                del cv[k]; commit_change(cur)
        elif op < 0.82:                       # UPDATE parent v
            if not pv: continue
            k = rnd.choice(list(pv)); v = rnd.randint(0, 99)
            if db.execute(f"UPDATE p SET v = {v} WHERE id = {k}").startswith("OK"):
                pv[k] = (v,); commit_change(cur)
        elif op < 0.91:                       # UPDATE parent id: ON UPDATE <b> says what the children do
            if not pv: continue
            k = rnd.choice(list(pv)); target = rnd.randint(31, 70)
            if target in pv: continue
            kids = children_of(cur, k)
            r = db.execute(f"UPDATE p SET id = {target} WHERE id = {k}")
            if kids and on_update == "RESTRICT":
                if r.startswith("OK"):
                    print(f"MISMATCH seed={seed}: parent {k} was renumbered under RESTRICT with children"); return False
                continue
            if r.startswith("OK"):
                pv[target] = pv.pop(k)
                for c_ in kids: cv[c_] = (target if on_update == "CASCADE" else None, cv[c_][1])
                commit_change(cur)
        else:                                 # DELETE parent: ON DELETE <a> says what the children do
            if not pv: continue
            k = rnd.choice(list(pv))
            kids = children_of(cur, k)
            r = db.execute(f"DELETE FROM p WHERE id = {k}")
            if kids and on_delete == "RESTRICT":
                if r.startswith("OK"):
                    print(f"MISMATCH seed={seed}: parent {k} was deleted under RESTRICT with children"); return False
                continue
            if r.startswith("OK"):
                del pv[k]
                for c_ in kids:
                    if on_delete == "CASCADE": del cv[c_]
                    else: cv[c_] = (None, cv[c_][1])
                commit_change(cur)
    proc.kill(); proc.wait()          # (an open explicit transaction was never acknowledged)
    proc = F.start()
    db = bench.RuSQL(); db.execute("USE d")
    got = {"p": rows_of(db, "p", ["id", "v"]), "c": rows_of(db, "c", ["id", "pid", "v"])}
    ok = got == oracle
    extra = []
    if ok:                            # the foreign key still works on the recovered tables
        if db.execute("INSERT INTO c VALUES (999, 9999, 0)").startswith("OK"):
            extra.append("a child of a missing parent was accepted after recovery")
        if on_delete == "RESTRICT":
            for k, (pid, v) in list(oracle["c"].items())[:3]:
                if pid is not None and db.execute(f"DELETE FROM p WHERE id = {pid}").startswith("OK"):
                    extra.append(f"parent {pid} of child {k} was deleted after recovery")
    db.close(); proc.terminate(); proc.wait()
    if not ok or extra:
        print(f"MISMATCH seed={seed} ops={n_ops} ON DELETE {on_delete} ON UPDATE {on_update}")
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
