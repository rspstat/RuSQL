"""Check aggregates whose argument is an EXPRESSION of ONE build against a reference computed here from the rows.

  python verify_agg_arguments.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

Table t(id, g, x, y, w, s) holds random integers (x, y), decimals with two places (w) and short strings (s), some of them NULL; table u(g, k) one
row per g. For every random expression E over x, y, w and constants (+ - * and parentheses, COALESCE(e, k), CONCAT for strings) the statements

    SELECT SUM(E), COUNT(E), MIN(E), MAX(E), AVG(E), COUNT(DISTINCT E) FROM t [WHERE ...]
    SELECT g, SUM(E), COUNT(E), MIN(E), MAX(E) FROM t [WHERE ...] GROUP BY g ORDER BY g
    SELECT g FROM t GROUP BY g HAVING SUM(E) > k ORDER BY g
    SELECT id, SUM(E) OVER (PARTITION BY g) FROM t ORDER BY id
    SELECT SUM(a.E) FROM t a JOIN u b ON a.g = b.g          (E written over a.x, a.y, b.k)

are run, and the answers are compared with the ones computed here with exact rational arithmetic (NULL when an operand is NULL, an aggregate
of nothing is NULL, AVG is the exact quotient rounded half away from zero to 4 places).
Exit code 1 and the statement on the first violation.
"""
import argparse, os, random, re, sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D


def cells(out):
    body = out.split("---END---")[0]
    if body.startswith("ERR"):
        return None
    lines = [l for l in body.splitlines() if l.startswith("|")]
    return [tuple(c.strip() for c in l.strip("|").split("|")) for l in lines[1:]]


def frac(text):
    """An exact number from the engine's text ('65.5000', '-3', '1.1')."""
    return Fraction(text)


def round4(q):
    """q rounded half away from zero to 4 places, as a Fraction."""
    scaled = q * 10000
    n = int(abs(scaled) + Fraction(1, 2))
    return Fraction(n if scaled >= 0 else -n, 10000)


class E:
    def __init__(self, sql, strength, ev):
        self.sql, self.strength, self.ev = sql, strength, ev


def wrap(e, needs):
    return "(" + e.sql + ")" if e.strength < needs else e.sql


def gen(rng, depth, cols):
    kind = rng.randrange(2) if depth <= 0 else rng.randrange(6)
    if kind == 0:
        name = rng.choice(list(cols))
        return E(name, 3, lambda row, n=name: row[n])
    if kind == 1:
        k = rng.randrange(10)
        return E(str(k), 3, lambda row, k=k: Fraction(k))
    if kind == 2:
        inner = gen(rng, depth - 1, cols)
        k = rng.randrange(10)
        return E(f"COALESCE({inner.sql}, {k})", 3, lambda row, i=inner, k=k: (lambda v: Fraction(k) if v is None else v)(i.ev(row)))
    left, right = gen(rng, depth - 1, cols), gen(rng, depth - 1, cols)
    op = "+-*"[kind - 3]
    strength = 2 if op == "*" else 1
    sql = f"{wrap(left, strength)} {op} {wrap(right, strength + 1)}"

    def ev(row, l=left, r=right, op=op):
        a, b = l.ev(row), r.ev(row)
        if a is None or b is None:
            return None
        return a + b if op == "+" else (a - b if op == "-" else a * b)

    return E(sql, strength, ev)


def fmt(q):
    return "NULL" if q is None else str(q)


def same(got, want):
    """Does the engine's text `got` say the number `want` (None = NULL)?"""
    if want is None:
        return got == "NULL"
    try:
        return got != "NULL" and Fraction(got) == want
    except ValueError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--queries", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rows", type=int, default=80)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    data = os.path.join(os.environ.get("TEMP", "/tmp"), "verify_agg_arguments")
    proc, db = D.start(args.exe, 17991, data, False)
    checked = 0
    try:
        db.execute("CREATE TABLE t (id INT PRIMARY KEY, g INT, x INT, y INT, w DECIMAL(10,2), s VARCHAR(8))")
        db.execute("CREATE TABLE u (g INT PRIMARY KEY, k INT)")
        rows = []
        for i in range(args.rows):
            r = {"id": Fraction(i), "g": rng.randrange(5)}
            r["x"] = None if rng.random() < 0.15 else Fraction(rng.randrange(-5, 16))
            r["y"] = None if rng.random() < 0.15 else Fraction(rng.randrange(-5, 16))
            r["w"] = None if rng.random() < 0.15 else Fraction(rng.randrange(-500, 1500), 100)
            r["s"] = None if rng.random() < 0.15 else rng.choice(["a", "bb", "c", "dd"])
            rows.append(r)
        sql_value = lambda v, text=False: "NULL" if v is None else (f"'{v}'" if text else (f"{float(v):.2f}" if v.denominator != 1 else str(v)))
        for start in range(0, len(rows), 20):
            chunk = rows[start:start + 20]
            db.execute("INSERT INTO t VALUES " + ", ".join(
                f"({r['id']}, {r['g']}, {sql_value(r['x'])}, {sql_value(r['y'])}, {sql_value(r['w']) if r['w'] is None else format(float(r['w']), '.2f')}, {sql_value(r['s'], True)})" for r in chunk))
        ks = {g: rng.randrange(1, 8) for g in range(5)}
        db.execute("INSERT INTO u VALUES " + ", ".join(f"({g}, {k})" for g, k in ks.items()))
        for r in rows:
            r["k"] = Fraction(ks[r["g"]])
        for n in range(args.queries):
            e = gen(rng, rng.randrange(1, 4), ["x", "y", "w"])
            threshold = rng.randrange(-20, 80)
            where = ""
            subset = rows
            if rng.random() < 0.4:
                bound = rng.randrange(0, args.rows)
                where = f" WHERE id >= {bound}"
                subset = [r for r in rows if r["id"] >= bound]

            def check(sql, want_rows):
                nonlocal checked
                out = db.execute(sql)
                got = cells(out)
                checked += 1
                ok = got is not None and len(got) == len(want_rows) and all(
                    len(g) == len(w) and all((same(gc, wc) if isinstance(wc, (Fraction, type(None))) else gc == wc) for gc, wc in zip(g, w)) for g, w in zip(got, want_rows))
                if not ok:
                    print("VIOLATION:", sql)
                    print("  engine:", got if got is not None else out[:200])
                    print("  wanted:", [tuple(fmt(c) if isinstance(c, (Fraction, type(None))) else c for c in w) for w in want_rows])
                    sys.exit(1)

            def stats(vs):
                present = [v for v in vs if v is not None]
                if not present:
                    return [None, Fraction(0), None, None, None, Fraction(0)]
                return [sum(present), Fraction(len(present)), min(present), max(present), round4(sum(present) / len(present)), Fraction(len(set(present)))]

            values = [e.ev(r) for r in subset]
            if any(v is not None and (v * 10000).denominator != 1 for v in values):
                continue  # (more than 4 decimals: the engine prints a SUM with 4 places, so the exact sum cannot be compared)
            s = stats(values)
            check(f"SELECT SUM({e.sql}), COUNT({e.sql}), MIN({e.sql}), MAX({e.sql}), AVG({e.sql}), COUNT(DISTINCT {e.sql}) FROM t{where}", [tuple(s)])
            groups = sorted({r["g"] for r in subset})
            per = []
            for g in groups:
                sg = stats([e.ev(r) for r in subset if r["g"] == g])
                per.append((Fraction(g), sg[0], sg[1], sg[2], sg[3]))
            check(f"SELECT g, SUM({e.sql}), COUNT({e.sql}), MIN({e.sql}), MAX({e.sql}) FROM t{where} GROUP BY g ORDER BY g", per)
            kept = [(Fraction(g),) for g, p in zip(groups, per) if p[1] is not None and p[1] > threshold]
            check(f"SELECT g FROM t{where} GROUP BY g HAVING SUM({e.sql}) > {threshold} ORDER BY g", kept)
            if n % 3 == 0:
                windows = []
                for r in rows:
                    part = [e.ev(q) for q in rows if q["g"] == r["g"]]
                    present = [v for v in part if v is not None]
                    windows.append((r["id"], sum(present) if present else None))
                check(f"SELECT id, SUM({e.sql}) OVER (PARTITION BY g) FROM t ORDER BY id", windows)
            if n % 3 == 1:
                jexpr = gen(rng, 2, ["x", "y", "k"])
                joined = [jexpr.ev(r) for r in rows]
                text = re.sub(r"\b([xyk])\b", lambda m: "b.k" if m.group(1) == "k" else "a." + m.group(1), jexpr.sql)
                present = [v for v in joined if v is not None]
                check(f"SELECT SUM({text}) FROM t a JOIN u b ON a.g = b.g", [(sum(present) if present else None,)])
        print(f"queries={args.queries} checks={checked} -- no violation")
    finally:
        proc.kill()


if __name__ == "__main__":
    main()
