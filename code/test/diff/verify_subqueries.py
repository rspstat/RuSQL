"""Check scalar subqueries, IN / NOT IN subqueries and HAVING with a subquery of ONE build against a model of what SQL says.

  python verify_subqueries.py <engine_server.exe> [--statements N] [--seed S]

Tables t(id, v, g) and u(id, a, b, g) hold random rows (NULLs included). For every random statement

    SELECT id FROM t WHERE v <op> (SELECT a FROM u WHERE id <= k)             a scalar subquery: two rows are an error, no row is NULL
    SELECT id FROM t WHERE v <op> (SELECT a FROM u WHERE u.g = t.g)            correlated: the error comes from the first row of t with two matches
    SELECT id, (SELECT a FROM u WHERE u.g = t.g) FROM t                        in the select list
    SELECT g FROM t GROUP BY g HAVING SUM(v) <op> (SELECT a FROM u WHERE id <= k)
    SELECT SUM(v) FROM t HAVING SUM(v) <op> (SELECT a FROM u WHERE id <= k)    HAVING without GROUP BY
    SELECT id FROM t WHERE v [NOT] IN (SELECT a FROM u WHERE id <= k)           three-valued: a NULL in the list
    SELECT id FROM t WHERE v <op> (SELECT a, b FROM u WHERE id <= k)           two columns: always an error
    UPDATE / DELETE ... WHERE v <op> (SELECT ...)                               an error changes nothing

the answer (or the error) is computed here and compared. Exit code 1 and the statement on the first violation.
"""
import argparse, os, random, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D

ROWS = "Subquery returns more than 1 row"
COLUMNS = "Operand should contain 1 column(s)"
OPS = {"=": lambda x, y: x == y, "<>": lambda x, y: x != y, "<": lambda x, y: x < y, "<=": lambda x, y: x <= y, ">": lambda x, y: x > y, ">=": lambda x, y: x >= y}


def parse(out):
    """('error', message) | ('rows', [tuple of cell texts])"""
    body = out.split("---END---")[0].strip()
    if body.startswith("ERR"):
        return "error", body.splitlines()[1].strip() if len(body.splitlines()) > 1 else body
    lines = [l for l in body.splitlines() if l.startswith("|")]
    return "rows", [tuple(c.strip() for c in l.strip("|").split("|")) for l in lines[1:]]


def text(v):
    return "NULL" if v is None else str(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--statements", type=int, default=400)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    proc, db = D.start(args.exe, 17992, os.path.join(os.environ.get("TEMP", "/tmp"), "verify_subqueries"), False)
    checked = 0
    try:
        db.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT, g INT)")
        db.execute("CREATE TABLE u (id INT PRIMARY KEY, a INT, b INT, g INT)")
        t = [{"id": i, "v": None if rng.random() < 0.2 else rng.randrange(6), "g": rng.randrange(4)} for i in range(14)]
        u = [{"id": i, "a": None if rng.random() < 0.2 else rng.randrange(6), "b": rng.randrange(9), "g": rng.randrange(4)} for i in range(rng.randrange(1, 7))]
        db.execute("INSERT INTO t VALUES " + ", ".join(f"({r['id']}, {text(r['v'])}, {r['g']})" for r in t))
        db.execute("INSERT INTO u VALUES " + ", ".join(f"({r['id']}, {text(r['a'])}, {r['b']}, {r['g']})" for r in u))

        def run(sql, want):
            nonlocal checked
            got = parse(db.execute(sql))
            checked += 1
            if got != want:
                print("VIOLATION:", sql)
                print("  engine:", got)
                print("  wanted:", want)
                sys.exit(1)

        for n in range(args.statements):
            op = rng.choice(list(OPS))
            k = rng.randrange(-1, 7)
            picked = [r for r in u if r["id"] <= k]
            kind = rng.randrange(9)
            asked = any(r["v"] is not None for r in t)  # (a comparison whose left side is NULL does not evaluate the subquery)
            if kind == 0:  # uncorrelated scalar
                sql = f"SELECT id FROM t WHERE v {op} (SELECT a FROM u WHERE id <= {k}) ORDER BY id"
                if len(picked) > 1 and asked:
                    run(sql, ("error", ROWS))
                else:
                    a = picked[0]["a"] if picked else None
                    run(sql, ("rows", [(str(r["id"]),) for r in t if r["v"] is not None and a is not None and OPS[op](r["v"], a)]))
            elif kind == 1:  # correlated
                counts = {r["id"]: [x for x in u if x["g"] == r["g"]] for r in t}
                sql = f"SELECT id FROM t WHERE v {op} (SELECT a FROM u WHERE u.g = t.g) ORDER BY id"
                if any(len(counts[r["id"]]) > 1 and r["v"] is not None for r in t):
                    run(sql, ("error", ROWS))
                else:
                    keep = []
                    for r in t:
                        m = counts[r["id"]]
                        a = m[0]["a"] if len(m) == 1 else None
                        if r["v"] is not None and a is not None and OPS[op](r["v"], a):
                            keep.append((str(r["id"]),))
                    run(sql, ("rows", keep))
            elif kind == 2:  # select list
                counts = {r["id"]: [x for x in u if x["g"] == r["g"]] for r in t}
                sql = "SELECT id, (SELECT a FROM u WHERE u.g = t.g) AS x FROM t ORDER BY id"
                if any(len(m) > 1 for m in counts.values()):
                    run(sql, ("error", ROWS))
                else:
                    run(sql, ("rows", [(str(r["id"]), text(counts[r["id"]][0]["a"]) if counts[r["id"]] else "NULL") for r in t]))
            elif kind == 3:  # HAVING with GROUP BY
                sql = f"SELECT g FROM t GROUP BY g HAVING SUM(v) {op} (SELECT a FROM u WHERE id <= {k}) ORDER BY g"
                groups = sorted({r["g"] for r in t})
                sums = {g: [r["v"] for r in t if r["g"] == g and r["v"] is not None] for g in groups}
                if len(picked) > 1 and any(sums.values()):
                    run(sql, ("error", ROWS))
                else:
                    a = picked[0]["a"] if picked else None
                    keep = []
                    for g in groups:
                        vals = [r["v"] for r in t if r["g"] == g and r["v"] is not None]
                        if vals and a is not None and OPS[op](sum(vals), a):
                            keep.append((str(g),))
                    run(sql, ("rows", keep))
            elif kind == 4:  # HAVING without GROUP BY
                sql = f"SELECT SUM(v) FROM t HAVING SUM(v) {op} (SELECT a FROM u WHERE id <= {k})"
                if len(picked) > 1 and asked:
                    run(sql, ("error", ROWS))
                else:
                    a = picked[0]["a"] if picked else None
                    vals = [r["v"] for r in t if r["v"] is not None]
                    run(sql, ("rows", [(str(sum(vals)),)] if vals and a is not None and OPS[op](sum(vals), a) else []))
            elif kind == 5:  # IN / NOT IN
                negate = rng.random() < 0.5
                sql = f"SELECT id FROM t WHERE v {'NOT IN' if negate else 'IN'} (SELECT a FROM u WHERE id <= {k}) ORDER BY id"
                values = [r["a"] for r in picked]
                keep = []
                for r in t:
                    if r["v"] is None:
                        continue
                    if r["v"] in values:
                        hit = True
                    elif None in values:
                        continue  # UNKNOWN either way
                    else:
                        hit = False
                    if hit != negate:
                        keep.append((str(r["id"]),))
                run(sql, ("rows", keep))
            elif kind == 6:  # two columns
                form = rng.choice(["v {op} (SELECT a, b FROM u WHERE id <= {k})", "v IN (SELECT a, b FROM u WHERE id <= {k})", "v NOT IN (SELECT a, b FROM u)"])
                run("SELECT id FROM t WHERE " + form.format(op=op, k=k), ("error", COLUMNS))
            elif kind == 7:  # UPDATE: an error changes nothing, otherwise the rows that match
                sql = f"UPDATE t SET v = 9 WHERE v {op} (SELECT a FROM u WHERE id <= {k})"
                before = [dict(r) for r in t]
                out = parse(db.execute(sql))
                checked += 1
                if len(picked) > 1 and asked:
                    if out != ("error", ROWS):
                        print("VIOLATION:", sql, out)
                        sys.exit(1)
                else:
                    a = picked[0]["a"] if picked else None
                    for r in t:
                        if r["v"] is not None and a is not None and OPS[op](r["v"], a):
                            r["v"] = 9
                    if out[0] != "rows":
                        print("VIOLATION:", sql, out)
                        sys.exit(1)
                got = parse(db.execute("SELECT id, v FROM t ORDER BY id"))
                if got != ("rows", [(str(r["id"]), text(r["v"])) for r in t]):
                    print("VIOLATION after", sql)
                    print("  engine:", got)
                    print("  wanted:", [(r["id"], r["v"]) for r in t], "before:", [(r["id"], r["v"]) for r in before])
                    sys.exit(1)
            else:  # DELETE of a row that is put back
                sql = f"DELETE FROM t WHERE id = 0 AND v {op} (SELECT a FROM u WHERE id <= {k})"
                out = parse(db.execute(sql))
                checked += 1
                if len(picked) > 1 and t[0]["v"] is not None and out != ("error", ROWS):
                    print("VIOLATION:", sql, out)
                    sys.exit(1)
                if len(picked) <= 1 and out[0] == "rows":
                    a = picked[0]["a"] if picked else None
                    if t[0]["v"] is not None and a is not None and OPS[op](t[0]["v"], a):
                        db.execute(f"INSERT INTO t VALUES (0, {text(t[0]['v'])}, {t[0]['g']})")
        print(f"statements={args.statements} checks={checked} -- no violation")
    finally:
        proc.kill()


if __name__ == "__main__":
    main()
