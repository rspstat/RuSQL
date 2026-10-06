"""Check subqueries that name a column of the query around them (correlated subqueries) of ONE build against a reference computed here from the rows.

  python verify_correlated.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

Tables o(id, g, x, w, s) and i(id, g, x, w, s) hold random integers (g, x), decimals with two places (w) and short lowercase strings (s), some of them NULL.
A predicate is one to three comparisons (joined by AND, some pairs by OR) between an expression of the outer row and an expression of the inner row -- the
column, ABS, + k, * 2 or COALESCE of one, numbers or strings -- with the outer side on the left or the right, the outer table sometimes under an alias and
the inner one sometimes under an alias or the very same table. With it these statements are run:

    SELECT id FROM o WHERE [NOT] EXISTS (SELECT 1 FROM i WHERE <predicate>)
    SELECT id, (SELECT COUNT(*) | SUM(w) | MIN(x) | MAX(w) FROM i WHERE <predicate>) FROM o
    SELECT id FROM o WHERE o.w > | <= (SELECT MIN(i.w) | MAX(i.w) FROM i WHERE <predicate>)
    SELECT id FROM o WHERE o.x IN (SELECT i.x FROM i WHERE <predicate>)
    SELECT id FROM o WHERE EXISTS (SELECT 1 FROM i q WHERE <predicate> AND EXISTS (SELECT 1 FROM i r WHERE <predicate on the outer row and r> AND r.g = q.g))
    DELETE FROM oc WHERE [NOT] EXISTS (...) / UPDATE oc SET x = 100 WHERE EXISTS (...), then the rows of the copy oc of o

and the answers are compared with what the 3-valued logic of the rows says. Exit code 1 and the statement on the first violation.
"""
import argparse, os, random, sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D
import verify_value_expressions as V

COLS = ["g", "x", "w", "s"]


class Operand:
    def __init__(self, sql, ev, kind):
        self.sql, self.ev, self.kind = sql, ev, kind  # kind: "n" number, "s" string


def operand(rng, qualifier, kind):
    """An expression of the columns of one row (a dict), written with `qualifier` before the column (or none)."""
    q = (qualifier + ".") if qualifier else ""
    if kind == "s":
        col = q + "s"
        if rng.randrange(3) == 0:
            return Operand(f"COALESCE({col}, 'z')", lambda r: "z" if r["s"] is None else r["s"], "s")
        return Operand(col, lambda r: r["s"], "s")
    name = rng.choice(["g", "x", "w"])
    col = q + name
    k = rng.randrange(-1, 4)
    shape = rng.randrange(5)

    def num(r):
        v = r[name]
        return None if v is None else Fraction(v)

    if shape == 0:
        return Operand(col, num, "n")
    if shape == 1:
        return Operand(f"ABS({col})", lambda r: None if num(r) is None else abs(num(r)), "n")
    if shape == 2:
        return Operand(f"{col} + {k}", lambda r: None if num(r) is None else num(r) + k, "n")
    if shape == 3:
        return Operand(f"COALESCE({col}, {k})", lambda r: Fraction(k) if num(r) is None else num(r), "n")
    return Operand(f"{col} * 2", lambda r: None if num(r) is None else num(r) * 2, "n")


OPS = {"=": lambda a, b: a == b, "<>": lambda a, b: a != b, "<": lambda a, b: a < b, "<=": lambda a, b: a <= b, ">": lambda a, b: a > b, ">=": lambda a, b: a >= b}


class Comparison:
    def __init__(self, rng, outer_q, inner_q, outer_bare_ok):
        kind = "s" if rng.randrange(4) == 0 else "n"
        self.outer = operand(rng, outer_q, kind)
        self.inner = operand(rng, inner_q, kind)
        self.op = rng.choice(list(OPS))
        self.outer_left = rng.randrange(2) == 0

    def ev(self, o, i):
        a, b = self.outer.ev(o), self.inner.ev(i)
        if a is None or b is None:
            return None
        l, r = (a, b) if self.outer_left else (b, a)
        return OPS[self.op](l, r)

    def sql(self):
        if self.outer_left:
            return f"({self.outer.sql}) {self.op} ({self.inner.sql})"
        return f"({self.inner.sql}) {self.op} ({self.outer.sql})"


def kleene_and(values):
    if any(v is False for v in values):
        return False
    return None if any(v is None for v in values) else True


def kleene_or(values):
    if any(v is True for v in values):
        return True
    return None if any(v is None for v in values) else False


class Predicate:
    def __init__(self, rng, outer_q, inner_q):
        self.terms = []
        for _ in range(rng.randrange(1, 4)):
            parts = [Comparison(rng, outer_q, inner_q, True) for _ in range(2 if rng.randrange(4) == 0 else 1)]
            self.terms.append(parts)

    def ev(self, o, i):
        return kleene_and([kleene_or([c.ev(o, i) for c in parts]) for parts in self.terms])

    def sql(self):
        return " AND ".join(("(" + " OR ".join(c.sql() for c in parts) + ")") if len(parts) > 1 else parts[0].sql() for parts in self.terms)


def kept(pred, o, inner_rows):
    return [i for i in inner_rows if pred.ev(o, i) is True]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--queries", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rows", type=int, default=24)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    datadir = os.path.join(os.environ.get("TEMP", "/tmp"), f"verify_corr_{args.seed}")
    proc, db = D.start(args.exe, 17975 + args.seed % 5, datadir, False)
    try:
        db.execute("CREATE DATABASE IF NOT EXISTS corr")
        db.execute("USE corr")
        for t in ("o", "i", "oc"):
            db.execute(f"DROP TABLE IF EXISTS {t}")
        shape = "(id INT PRIMARY KEY, g INT, x INT, w DECIMAL(10,2), s VARCHAR(8))"
        db.execute("CREATE TABLE o " + shape)
        db.execute("CREATE TABLE i " + shape)

        def make(count):
            rows = []
            for n in range(count):
                rows.append({"id": n, "g": rng.randrange(4), "x": None if rng.randrange(6) == 0 else rng.randrange(-3, 7),
                             "w": None if rng.randrange(6) == 0 else Fraction(rng.randrange(-200, 900), 100),
                             "s": None if rng.randrange(6) == 0 else "".join(rng.choice("abz") for _ in range(rng.randrange(1, 4)))})
            return rows

        def values(rows):
            return ", ".join(f"({r['id']}, {r['g']}, {V.literal(r['x'])}, {V.literal(r['w'])}, {V.literal(r['s'])})" for r in rows)

        o_rows, i_rows = make(args.rows), make(args.rows + 8)
        db.execute("INSERT INTO o VALUES " + values(o_rows))
        db.execute("INSERT INTO i VALUES " + values(i_rows))
        checks = 0
        for n in range(args.queries):
            kind = rng.randrange(7)
            self_join = kind in (0, 1, 2, 3, 4) and rng.randrange(5) == 0
            inner_rows = o_rows if self_join else i_rows
            inner_table = "o" if self_join else "i"
            outer_alias = rng.choice(["", "p"])
            inner_alias = "q" if self_join else rng.choice(["", "q"])
            outer_q = outer_alias or "o"
            inner_q = inner_alias or inner_table
            from_outer = "o" + (f" {outer_alias}" if outer_alias else "")
            from_inner = inner_table + (f" {inner_alias}" if inner_alias else "")
            pred = Predicate(rng, outer_q, inner_q)
            ids = f"{outer_q}.id"
            if kind in (0, 1):  # EXISTS / NOT EXISTS
                negate = kind == 1
                sql = f"SELECT {ids} FROM {from_outer} WHERE {'NOT ' if negate else ''}EXISTS (SELECT 1 FROM {from_inner} WHERE {pred.sql()}) ORDER BY {ids}"
                want = [(Fraction(o["id"]),) for o in o_rows if (not kept(pred, o, inner_rows)) == negate]
                V.check(db, sql, want)
            elif kind == 2:  # an aggregate in the select list
                agg = rng.choice(["COUNT(*)", "SUM(w)", "MIN(x)", "MAX(w)"])
                sql = f"SELECT {ids}, (SELECT {agg.replace('(', '(' + inner_q + '.') if agg != 'COUNT(*)' else agg} FROM {from_inner} WHERE {pred.sql()}) FROM {from_outer} ORDER BY {ids}"
                want = []
                for o in o_rows:
                    rows = kept(pred, o, inner_rows)
                    if agg == "COUNT(*)":
                        v = Fraction(len(rows))
                    else:
                        col = "x" if agg == "MIN(x)" else "w"
                        vals = [Fraction(r[col]) for r in rows if r[col] is not None]
                        v = (sum(vals) if agg == "SUM(w)" else (min(vals) if agg == "MIN(x)" else max(vals))) if vals else None
                    want.append((Fraction(o["id"]), v))
                V.check(db, sql, want)
            elif kind == 3:  # a scalar comparison
                agg, op = rng.choice(["MIN", "MAX"]), rng.choice([">", "<="])
                sql = (f"SELECT {ids} FROM {from_outer} WHERE {outer_q}.w {op} (SELECT {agg}({inner_q}.w) FROM {from_inner} WHERE {pred.sql()}) ORDER BY {ids}")
                want = []
                for o in o_rows:
                    vals = [r["w"] for r in kept(pred, o, inner_rows) if r["w"] is not None]
                    if not vals or o["w"] is None:
                        continue
                    ref = min(vals) if agg == "MIN" else max(vals)
                    if OPS[op](o["w"], ref):
                        want.append((Fraction(o["id"]),))
                V.check(db, sql, want)
            elif kind == 4:  # IN
                sql = f"SELECT {ids} FROM {from_outer} WHERE {outer_q}.x IN (SELECT {inner_q}.x FROM {from_inner} WHERE {pred.sql()}) ORDER BY {ids}"
                want = []
                for o in o_rows:
                    if o["x"] is not None and any(r["x"] == o["x"] for r in kept(pred, o, inner_rows) if r["x"] is not None):
                        want.append((Fraction(o["id"]),))
                V.check(db, sql, want)
            elif kind == 5:  # two levels
                pred2 = Predicate(rng, outer_q, "r")
                outer_alias = outer_alias or ""
                from_outer = "o" + (f" {outer_alias}" if outer_alias else "")
                pred1 = Predicate(rng, outer_q, "q")
                sql = (f"SELECT {ids} FROM {from_outer} WHERE EXISTS (SELECT 1 FROM i q WHERE {pred1.sql()} AND "
                       f"EXISTS (SELECT 1 FROM i r WHERE {pred2.sql()} AND r.g = q.g)) ORDER BY {ids}")
                want = []
                for o in o_rows:
                    hit = any(pred1.ev(o, qr) is True and any(pred2.ev(o, rr) is True and rr["g"] == qr["g"] for rr in i_rows) for qr in i_rows)
                    if hit:
                        want.append((Fraction(o["id"]),))
                V.check(db, sql, want)
            else:  # DELETE / UPDATE of a copy
                db.execute("DROP TABLE IF EXISTS oc")
                db.execute("CREATE TABLE oc " + shape)
                db.execute("INSERT INTO oc VALUES " + values(o_rows))
                pred = Predicate(rng, "oc", "i")
                if rng.randrange(2) == 0:
                    negate = rng.randrange(2) == 0
                    db.execute(f"DELETE FROM oc WHERE {'NOT ' if negate else ''}EXISTS (SELECT 1 FROM i WHERE {pred.sql()})")
                    want = [(Fraction(o["id"]),) for o in o_rows if (not kept(pred, o, i_rows)) != negate]
                    V.check(db, "SELECT id FROM oc ORDER BY id", want)
                else:
                    db.execute(f"UPDATE oc SET x = 100 WHERE EXISTS (SELECT 1 FROM i WHERE {pred.sql()})")
                    want = [(Fraction(o["id"]), Fraction(100) if kept(pred, o, i_rows) else (None if o["x"] is None else Fraction(o["x"]))) for o in o_rows]
                    V.check(db, "SELECT id, x FROM oc ORDER BY id", want)
            checks += 1
        print(f"verify_correlated seed {args.seed}: {checks} statements, no violation")
    except V.Violation as v:
        print("VIOLATION", v)
        sys.exit(1)
    finally:
        proc.kill()


if __name__ == "__main__":
    main()
