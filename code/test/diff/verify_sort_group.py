"""Check ORDER BY and GROUP BY of ONE build against a reference computed here from the rows.

  python verify_sort_group.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

Table t(id, g, x, y, w, s) holds random integers (x, y), decimals with two places (w) and short lowercase strings (s), some of them NULL; table u(id, g, k)
a few rows per g. The statements

    SELECT id, x, y, w, s, E1 AS e1, E2 AS e2 FROM t ORDER BY <keys>, id         keys: a column, t.column, a name the select list gives, a position,
                                                                                  any expression (numbers or strings), each ASC or DESC
    SELECT GE AS ge, COUNT(*) AS n, SUM(x) AS sx, MAX(w) AS mw FROM t GROUP BY (GE | ge | 1) ORDER BY <keys>, ge
                                                                                  keys: n, COUNT(*), sx, SUM(x), mw, a position, an expression of aggregates
    SELECT t.id, u.k, t.x + u.k AS tk FROM t JOIN u ON t.g = u.g ORDER BY <keys>, t.id
    SELECT g FROM t UNION SELECT k FROM u ORDER BY 1 [DESC]

are run and the order of the answer is compared with the one computed here (NULL before every value, numbers by value, strings bytewise; the ties of a key are
broken by the keys after it, then by id). Exit code 1 and the statement on the first violation.
"""
import argparse, os, random, sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D
import verify_value_expressions as V


def cmp_value(a, b):
    """NULL (None) is smaller than every value; numbers by value, strings bytewise."""
    if a is None or b is None:
        return 0 if a is b else (-1 if a is None else 1)
    return -1 if a < b else (1 if a > b else 0)


class Key:
    def __init__(self, sql, value, ascending):
        self.sql, self.value, self.ascending = sql, value, ascending


def sort_ids(rows, keys):
    import functools

    def compare(a, b):
        for k in keys:
            c = cmp_value(k.value(a), k.value(b))
            if not k.ascending:
                c = -c
            if c:
                return c
        return (a["id"] > b["id"]) - (a["id"] < b["id"])

    return [r["id"] for r in sorted(rows, key=functools.cmp_to_key(compare))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--queries", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rows", type=int, default=40)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    datadir = os.path.join(os.environ.get("TEMP", "/tmp"), f"verify_sg_{args.seed}")
    proc, db = D.start(args.exe, 17995 + args.seed % 5, datadir, False)
    try:
        db.execute("CREATE DATABASE IF NOT EXISTS sg")
        db.execute("USE sg")
        for t in ("t", "u"):
            db.execute(f"DROP TABLE IF EXISTS {t}")
        db.execute("CREATE TABLE t (id INT PRIMARY KEY, g INT, x INT, y INT, w DECIMAL(10,2), s VARCHAR(10))")
        db.execute("CREATE TABLE u (id INT PRIMARY KEY, g INT, k INT)")
        table, values = [], []
        for i in range(args.rows):
            row = {"id": i, "g": rng.randrange(4)}
            row["x"] = None if rng.randrange(6) == 0 else rng.randrange(-4, 12)
            row["y"] = None if rng.randrange(6) == 0 else rng.randrange(-4, 12)
            row["w"] = None if rng.randrange(6) == 0 else Fraction(rng.randrange(-300, 1500), 100)
            row["s"] = None if rng.randrange(6) == 0 else "".join(rng.choice("abz") for _ in range(rng.randrange(0, 4)))
            table.append(row)
            values.append(f"({i}, {row['g']}, {V.literal(row['x'])}, {V.literal(row['y'])}, {V.literal(row['w'])}, {V.literal(row['s'])})")
        db.execute("INSERT INTO t VALUES " + ", ".join(values))
        others = [{"id": j, "g": rng.randrange(4), "k": rng.randrange(-3, 9)} for j in range(10)]
        db.execute("INSERT INTO u VALUES " + ", ".join(f"({o['id']}, {o['g']}, {o['k']})" for o in others))
        checks = 0
        for n in range(args.queries):
            shape = rng.randrange(4)
            if shape == 0:  # rows
                e1, e2 = V.gen_num(rng, 2), (V.gen_str(rng, 2) if rng.randrange(3) == 0 else V.gen_num(rng, 2))
                items = [("id", lambda r: Fraction(r["id"])), ("x", lambda r: None if r["x"] is None else Fraction(r["x"])),
                         ("y", lambda r: None if r["y"] is None else Fraction(r["y"])), ("w", lambda r: r["w"]), ("s", lambda r: r["s"]),
                         (e1.sql, e1.ev), (e2.sql, e2.ev)]
                keys = []
                for _ in range(rng.randrange(1, 4)):
                    asc = rng.randrange(2) == 0
                    kind = rng.randrange(6)
                    if kind == 0:
                        col = rng.randrange(1, 5)  # x y w s
                        name = ["x", "y", "w", "s"][col - 1]
                        keys.append(Key(name, items[col][1], asc))
                    elif kind == 1:
                        col = rng.randrange(1, 5)
                        name = ["x", "y", "w", "s"][col - 1]
                        keys.append(Key("t." + name, items[col][1], asc))
                    elif kind == 2:
                        keys.append(Key("e1", e1.ev, asc))
                    elif kind == 3:
                        keys.append(Key("e2", e2.ev, asc))
                    elif kind == 4:
                        pos = rng.randrange(2, 8)
                        keys.append(Key(str(pos), items[pos - 1][1], asc))
                    else:
                        own = V.gen_num(rng, 2) if rng.randrange(2) else V.gen_str(rng, 2)
                        while own.sql.strip("0123456789()") == "":
                            own = V.gen_num(rng, 2)
                        keys.append(Key(own.sql, own.ev, asc))
                order = ", ".join(k.sql + ("" if k.ascending else " DESC") for k in keys)
                sql = f"SELECT id, x, y, w, s, {e1.sql} AS e1, {e2.sql} AS e2 FROM t ORDER BY {order}, id"
                want = sort_ids(table, keys)
                got = V.cells(db.execute(sql))
                if got is None or [int(r[0]) for r in got] != want:
                    raise V.Violation(f"{sql}\n   got {[r[0] for r in got] if got else got}\n   want {want}")
            elif shape == 1:  # groups
                ge = V.gen_num(rng, 2)
                while ge.sql.strip("0123456789()") == "":
                    ge = V.gen_num(rng, 2)
                groups = {}
                for r in table:
                    groups.setdefault(ge.ev(r), []).append(r)
                out = []
                for gv, rows in groups.items():
                    xs = [Fraction(r["x"]) for r in rows if r["x"] is not None]
                    ws = [r["w"] for r in rows if r["w"] is not None]
                    out.append({"ge": gv, "n": Fraction(len(rows)), "sx": sum(xs) if xs else None, "mw": max(ws) if ws else None})
                choices = [("n", lambda g: g["n"]), ("COUNT(*)", lambda g: g["n"]), ("sx", lambda g: g["sx"]), ("SUM(x)", lambda g: g["sx"]), ("mw", lambda g: g["mw"]),
                           ("2", lambda g: g["n"]), ("3", lambda g: g["sx"]), ("4", lambda g: g["mw"]),
                           ("COALESCE(SUM(x), 0) - COUNT(*)", lambda g: (g["sx"] if g["sx"] is not None else Fraction(0)) - g["n"]),
                           ("COALESCE(MAX(w), 0) * 2", lambda g: (g["mw"] if g["mw"] is not None else Fraction(0)) * 2)]
                import functools
                name, value = rng.choice(choices)
                asc = rng.randrange(2) == 0

                def compare(a, b, value=value, asc=asc):
                    c = cmp_value(value(a), value(b))
                    if not asc:
                        c = -c
                    if c:
                        return c
                    c = cmp_value(a["ge"], b["ge"])
                    return c if asc else -c

                out.sort(key=functools.cmp_to_key(compare))
                by = rng.choice([ge.sql, "ge", "1"])
                sql = (f"SELECT {ge.sql} AS ge, COUNT(*) AS n, SUM(x) AS sx, MAX(w) AS mw FROM t GROUP BY {by} ORDER BY {name}{'' if asc else ' DESC'}, "
                       f"ge{'' if asc else ' DESC'}")
                want = [(g["ge"], g["n"], g["sx"], g["mw"]) for g in out]
                V.check(db, sql, want)
            elif shape == 2:  # a join
                pairs = [(r, o) for r in table for o in others if r["g"] == o["g"]]
                if not pairs:
                    continue
                import functools
                kinds = [("tk", lambda p: None if p[0]["x"] is None else Fraction(p[0]["x"] + p[1]["k"])), ("2", lambda p: Fraction(p[1]["k"])),
                         ("3", lambda p: None if p[0]["x"] is None else Fraction(p[0]["x"] + p[1]["k"])), ("u.k * -1", lambda p: Fraction(-p[1]["k"])),
                         ("t.x", lambda p: None if p[0]["x"] is None else Fraction(p[0]["x"])), ("u.id", lambda p: Fraction(p[1]["id"]))]
                name, value = rng.choice(kinds)
                asc = rng.randrange(2) == 0

                def compare(a, b, value=value, asc=asc):
                    c = cmp_value(value(a), value(b))
                    if not asc:
                        c = -c
                    if c:
                        return c
                    return ((a[0]["id"], a[1]["id"]) > (b[0]["id"], b[1]["id"])) - ((a[0]["id"], a[1]["id"]) < (b[0]["id"], b[1]["id"]))

                pairs.sort(key=functools.cmp_to_key(compare))
                sql = f"SELECT t.id, u.k, t.x + u.k AS tk, u.id AS uid FROM t JOIN u ON t.g = u.g ORDER BY {name}{'' if asc else ' DESC'}, t.id, u.id"
                want = [(Fraction(p[0]["id"]), Fraction(p[1]["k"]), None if p[0]["x"] is None else Fraction(p[0]["x"] + p[1]["k"]), Fraction(p[1]["id"])) for p in pairs]
                V.check(db, sql, want)
            else:  # a set operation
                asc = rng.randrange(2) == 0
                op = rng.choice(["UNION", "UNION ALL"])
                gs = [r["g"] for r in table]
                ks = [o["k"] for o in others]
                combined = gs + ks
                if op == "UNION":
                    combined = sorted(set(combined))
                combined = sorted(combined, reverse=not asc)
                sql = f"SELECT g FROM t {op} SELECT k FROM u ORDER BY {rng.choice(['1', 'g'])}{'' if asc else ' DESC'}"
                V.check(db, sql, [(Fraction(v),) for v in combined])
            checks += 1
        print(f"verify_sort_group seed {args.seed}: {checks} rounds, no violation")
    except V.Violation as v:
        print("VIOLATION", v)
        sys.exit(1)
    finally:
        proc.kill()


if __name__ == "__main__":
    main()
