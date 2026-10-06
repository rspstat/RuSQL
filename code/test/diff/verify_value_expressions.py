"""Check expressions (functions, CASE, IF, conditions as values, aggregates inside expressions) of ONE build against a reference computed here.

  python verify_value_expressions.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

Table t(id, g, x, y, w, s) holds random integers (x, y), decimals with two places (w) and short lowercase strings (s), some of them NULL. For every
random expression the statements

    SELECT id, E FROM t ORDER BY id                              E a number, a string or a condition (1, 0, NULL)
    SELECT id FROM t WHERE C ORDER BY id                         C a condition (true rows only; NOT C keeps the false ones)
    SELECT SUM(E), COUNT(E), MIN(E), MAX(E), AVG(E) FROM t       E a number
    SELECT g, SUM(CASE WHEN C THEN E END), COUNT(CASE WHEN C THEN E ELSE 0 END) FROM t GROUP BY g ORDER BY g
    SELECT g FROM t GROUP BY g HAVING SUM(CASE WHEN C THEN 1 ELSE 0 END) * 10 + COUNT(*) > k ORDER BY g
    SELECT g, COALESCE(SUM(CASE WHEN C THEN E END), 0) * 100 + COUNT(*) FROM t GROUP BY g ORDER BY g
    UPDATE u SET v = E  /  INSERT INTO w VALUES (1, E-of-constants)

are run and compared with the answers computed here with exact rational arithmetic and three-valued logic (a NULL operand makes an arithmetic
result and a comparison NULL; a CASE takes the first branch whose condition is TRUE; a value is a condition when it is not NULL and not 0).
Exit code 1 and the statement on the first violation.
"""
import argparse, os, random, sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D


def cells(out):
    body = out.split("---END---")[0]
    if body.startswith("ERR"):
        return None
    lines = [l for l in body.splitlines() if l.startswith("|")]
    return [tuple(c.strip() for c in l.strip("|").split("|")) for l in lines[1:]]


def number_text(q):
    return "NULL" if q is None else q


def same_number(got, want):
    """The engine's text of a number (7.5, 7.50, -3) against the exact number it should be."""
    if want is None:
        return got == "NULL"
    if got == "NULL":
        return False
    try:
        value = Fraction(got)
    except (ValueError, ZeroDivisionError):
        return False
    if value == want:
        return True
    # a product of several decimals has more places than the 6 an arithmetic result is printed with: the number is right to those 6 places
    return 10 ** 6 % want.denominator != 0 and abs(value - want) <= Fraction(6, 10 ** 5)


def truth_text(t):
    return "NULL" if t is None else ("1" if t else "0")


def round4(q):
    scaled = q * 10000
    n = int(abs(scaled) + Fraction(1, 2))
    return Fraction(n if scaled >= 0 else -n, 10000)


# ---------------------------------------------------------------------------------------------------------------------------------------------
# expressions: sql text, how tightly the outermost operator binds (OR 1, AND 2, NOT 3, predicate 4, + - 5, * 6, unary minus 7, atom 8), evaluation
# ---------------------------------------------------------------------------------------------------------------------------------------------
class X:
    def __init__(self, sql, prec, ev, bare=False):
        self.sql, self.prec, self.ev, self.bare = sql, prec, ev, bare


def wrap(x, needs):
    return "(" + x.sql + ")" if x.prec < needs else x.sql


def gen_num(rng, depth, cols=True):
    kinds = (3 if cols else 2) if depth <= 0 else 14
    kind = rng.randrange(kinds)
    if kind == 0:
        if rng.randrange(4) == 0:
            return X("NULL", 8, lambda row: None)
        k = rng.randrange(-2, 10)
        return X(str(k), 8, lambda row, k=k: Fraction(k))
    if kind == 1:
        text = f"{rng.randrange(0, 20)}.{rng.randrange(0, 100):02d}"
        return X(text, 8, lambda row, v=Fraction(text): v)
    if kind == 2:
        if not cols:
            return X("7", 8, lambda row: Fraction(7))
        name = rng.choice(["x", "y", "w"])
        return X(name, 8, lambda row, n=name: None if row[n] is None else Fraction(row[n]))
    if kind == 3:
        inner = gen_num(rng, depth - 1, cols)
        sql = "-(" + inner.sql + ")" if inner.sql.startswith("-") else "-" + wrap(inner, 8)
        return X(sql, 7, lambda row, i=inner: (lambda v: None if v is None else -v)(i.ev(row)))
    if kind in (4, 5, 6):
        left, right = gen_num(rng, depth - 1, cols), gen_num(rng, depth - 1, cols)
        op = "+-*"[kind - 4]
        prec = 6 if op == "*" else 5

        def ev(row, l=left, r=right, op=op):
            a, b = l.ev(row), r.ev(row)
            if a is None or b is None:
                return None
            return a + b if op == "+" else (a - b if op == "-" else a * b)

        return X(f"{wrap(left, prec)} {op} {wrap(right, prec + 1)}", prec, ev)
    if kind == 7:
        inner = gen_num(rng, depth - 1, cols)
        return X(f"ABS({inner.sql})", 8, lambda row, i=inner: (lambda v: None if v is None else abs(v))(i.ev(row)))
    if kind == 8:
        left, right = gen_num(rng, depth - 1, cols), gen_num(rng, depth - 1, cols)
        name = rng.choice(["COALESCE", "IFNULL"])
        return X(f"{name}({left.sql}, {right.sql})", 8, lambda row, l=left, r=right: (lambda a: r.ev(row) if a is None else a)(l.ev(row)))
    if kind == 9:
        left, right = gen_num(rng, depth - 1, cols), gen_num(rng, depth - 1, cols)

        def ev(row, l=left, r=right):
            a, b = l.ev(row), r.ev(row)
            return None if a is not None and b is not None and a == b else a

        return X(f"NULLIF({left.sql}, {right.sql})", 8, ev)
    if kind in (10, 11):  # CASE WHEN .. THEN .. [ELSE ..] END
        arms = [(gen_bool(rng, depth - 1, cols), gen_num(rng, depth - 1, cols)) for _ in range(rng.randrange(1, 4))]
        other = gen_num(rng, depth - 1, cols) if rng.randrange(3) else None
        sql = "CASE " + " ".join(f"WHEN {c.sql} THEN {r.sql}" for c, r in arms) + (f" ELSE {other.sql}" if other else "") + " END"

        def ev(row, arms=arms, other=other):
            for c, r in arms:
                if c.ev(row) is True:
                    return r.ev(row)
            return other.ev(row) if other else None

        return X(sql, 8, ev)
    if kind == 12:  # CASE operand WHEN k THEN ..
        operand = gen_num(rng, depth - 1, cols)
        keys = [rng.randrange(-2, 8) for _ in range(rng.randrange(1, 4))]
        thens = [gen_num(rng, depth - 1, cols) for _ in keys]
        other = gen_num(rng, depth - 1, cols) if rng.randrange(2) else None
        sql = f"CASE {operand.sql} " + " ".join(f"WHEN {k} THEN {t.sql}" for k, t in zip(keys, thens)) + (f" ELSE {other.sql}" if other else "") + " END"

        def ev(row, operand=operand, keys=keys, thens=thens, other=other):
            v = operand.ev(row)
            if v is not None:
                for k, t in zip(keys, thens):
                    if v == k:
                        return t.ev(row)
            return other.ev(row) if other else None

        return X(sql, 8, ev)
    cond, a, b = gen_bool(rng, depth - 1, cols), gen_num(rng, depth - 1, cols), gen_num(rng, depth - 1, cols)
    if kind == 13:
        return X(f"IF({cond.sql}, {a.sql}, {b.sql})", 8, lambda row, c=cond, a=a, b=b: a.ev(row) if c.ev(row) is True else b.ev(row))
    return X("0", 8, lambda row: Fraction(0))


def gen_str(rng, depth, cols=True):
    kind = rng.randrange(2 if depth <= 0 else 7)
    if kind == 0:
        if cols and rng.randrange(2):
            return X("s", 8, lambda row: row["s"])
        lit = rng.choice(["a", "b", "ab", "it''s", "z", ""])
        return X(f"'{lit}'", 8, lambda row, v=lit.replace("''", "'"): v)
    if kind == 1:
        if cols:
            return X("s", 8, lambda row: row["s"])
        return X("'q'", 8, lambda row: "q")
    if kind == 2:
        a, b = gen_str(rng, depth - 1, cols), gen_str(rng, depth - 1, cols)
        return X(f"CONCAT({a.sql}, {b.sql})", 8, lambda row, a=a, b=b: (lambda p, q: None if p is None or q is None else p + q)(a.ev(row), b.ev(row)))
    if kind == 3:
        a = gen_str(rng, depth - 1, cols)
        return X(f"UPPER({a.sql})", 8, lambda row, a=a: (lambda v: None if v is None else v.upper())(a.ev(row)))
    if kind == 4:
        arms = [(gen_bool(rng, depth - 1, cols), gen_str(rng, depth - 1, cols)) for _ in range(rng.randrange(1, 3))]
        other = gen_str(rng, depth - 1, cols) if rng.randrange(2) else None
        sql = "CASE " + " ".join(f"WHEN {c.sql} THEN {r.sql}" for c, r in arms) + (f" ELSE {other.sql}" if other else "") + " END"

        def ev(row, arms=arms, other=other):
            for c, r in arms:
                if c.ev(row) is True:
                    return r.ev(row)
            return other.ev(row) if other else None

        return X(sql, 8, ev)
    if kind == 5:
        cond, a, b = gen_bool(rng, depth - 1, cols), gen_str(rng, depth - 1, cols), gen_str(rng, depth - 1, cols)
        return X(f"IF({cond.sql}, {a.sql}, {b.sql})", 8, lambda row, c=cond, a=a, b=b: a.ev(row) if c.ev(row) is True else b.ev(row))
    a, b = gen_str(rng, depth - 1, cols), gen_str(rng, depth - 1, cols)
    return X(f"COALESCE({a.sql}, {b.sql})", 8, lambda row, a=a, b=b: (lambda v: b.ev(row) if v is None else v)(a.ev(row)))


def like(value, pattern):
    if not pattern:
        return not value
    if pattern[0] == "%":
        return any(like(value[i:], pattern[1:]) for i in range(len(value) + 1))
    if not value:
        return False
    if pattern[0] == "_" or pattern[0] == value[0]:
        return like(value[1:], pattern[1:])
    return False


def gen_bool(rng, depth, cols=True):
    kind = rng.randrange(4 if depth <= 0 else 13)
    if kind in (0, 1, 2):
        if kind == 2 and rng.randrange(3) == 0:  # strings
            a, b = gen_str(rng, depth - 1, cols), gen_str(rng, depth - 1, cols)
            op = rng.choice(["=", "<>", "<", ">="])

            def ev_s(row, a=a, b=b, op=op):
                p, q = a.ev(row), b.ev(row)
                if p is None or q is None:
                    return None
                return {"=": p == q, "<>": p != q, "<": p < q, ">=": p >= q}[op]

            return X(f"{a.sql} {op} {b.sql}", 4, ev_s)
        left, right = gen_num(rng, depth - 1, cols), gen_num(rng, depth - 1, cols)
        op = rng.choice(["=", "<>", "<", "<=", ">", ">="])

        def ev(row, l=left, r=right, op=op):
            a, b = l.ev(row), r.ev(row)
            if a is None or b is None:
                return None
            return {"=": a == b, "<>": a != b, "<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[op]

        return X(f"{wrap(left, 5)} {op} {wrap(right, 5)}", 4, ev)
    if kind == 3:
        e = gen_num(rng, depth - 1, cols)
        tail = rng.choice([" IS NULL", " IS NOT NULL", " IS TRUE", " IS NOT TRUE", " IS FALSE", " IS NOT FALSE"])

        def ev(row, e=e, tail=tail):
            v = e.ev(row)
            return {" IS NULL": v is None, " IS NOT NULL": v is not None, " IS TRUE": v is not None and v != 0, " IS NOT TRUE": not (v is not None and v != 0),
                    " IS FALSE": v is not None and v == 0, " IS NOT FALSE": not (v is not None and v == 0)}[tail]

        return X(wrap(e, 5) + tail, 4, ev)
    if kind in (4, 5):
        e, lo, hi = gen_num(rng, depth - 1, cols), gen_num(rng, depth - 1, cols), gen_num(rng, depth - 1, cols)
        negated = rng.randrange(2) == 0

        def ev(row, e=e, lo=lo, hi=hi, negated=negated):
            v, a, b = e.ev(row), lo.ev(row), hi.ev(row)
            left = None if v is None or a is None else v >= a
            right = None if v is None or b is None else v <= b
            if left is False or right is False:
                inside = False
            elif left is True and right is True:
                inside = True
            else:
                inside = None
            return inside if (inside is None or not negated) else not inside

        return X(f"{wrap(e, 5)} {'NOT ' if negated else ''}BETWEEN {wrap(lo, 5)} AND {wrap(hi, 5)}", 4, ev)
    if kind == 6:
        e = gen_num(rng, depth - 1, cols)
        items = [None if rng.randrange(6) == 0 else rng.randrange(-2, 8) for _ in range(rng.randrange(1, 5))]
        negated = rng.randrange(2) == 0

        def ev(row, e=e, items=items, negated=negated):
            v = e.ev(row)
            if v is None:
                return None
            if any(i is not None and i == v for i in items):
                inside = True
            elif any(i is None for i in items):
                inside = None
            else:
                inside = False
            return inside if (inside is None or not negated) else not inside

        return X(f"{wrap(e, 5)} {'NOT ' if negated else ''}IN ({', '.join('NULL' if i is None else str(i) for i in items)})", 4, ev)
    if kind in (7, 8):
        left, right = gen_bool(rng, depth - 1, cols), gen_bool(rng, depth - 1, cols)
        is_and = rng.randrange(2) == 0
        prec = 2 if is_and else 1

        def ev(row, l=left, r=right, is_and=is_and):
            a, b = l.ev(row), r.ev(row)
            if is_and:
                if a is False or b is False:
                    return False
                return True if (a is True and b is True) else None
            if a is True or b is True:
                return True
            return False if (a is False and b is False) else None

        return X(f"{wrap(left, prec)} {'AND' if is_and else 'OR'} {wrap(right, prec + 1)}", prec, ev)
    if kind == 9:
        e = gen_bool(rng, depth - 1, cols)
        return X("NOT " + wrap(e, 3), 3, lambda row, e=e: (lambda v: None if v is None else not v)(e.ev(row)))
    if kind == 10 and cols:
        pattern = rng.choice(["a%", "%b", "_a%", "%a%", "ab", "%"])
        negated = rng.randrange(2) == 0
        return X(f"s {'NOT ' if negated else ''}LIKE '{pattern}'", 4,
                 lambda row, p=pattern, n=negated: None if row["s"] is None else (not like(row["s"], p) if n else like(row["s"], p)))
    e = gen_num(rng, depth - 1, cols)  # a number as a condition
    return X(wrap(e, 5), 5, lambda row, e=e: (lambda v: None if v is None else v != 0)(e.ev(row)), bare=True)


# ---------------------------------------------------------------------------------------------------------------------------------------------
class Violation(Exception):
    pass


def literal(v):
    if v is None:
        return "NULL"
    if isinstance(v, Fraction):
        return f"{float(v):.2f}"
    return f"'{v}'" if isinstance(v, str) else str(v)


def cell_ok(got, want):
    if want is None:
        return got == "NULL"
    if isinstance(want, bool):
        return got == ("1" if want else "0")
    if isinstance(want, Fraction):
        return same_number(got, want)
    return got == want


def check(db, sql, want):
    """want: a list of tuples of expected cells; a cell is a Fraction (a number), a str (text), a bool (a condition) or None (NULL)."""
    got = cells(db.execute(sql))
    if got is None or len(got) != len(want):
        raise Violation(f"{sql}\n   got {got}\n   want {want}")
    for g_row, w_row in zip(got, want):
        if len(g_row) != len(w_row) or not all(cell_ok(g, w) for g, w in zip(g_row, w_row)):
            raise Violation(f"{sql}\n   got {g_row}\n   want {w_row}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--queries", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rows", type=int, default=50)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    datadir = os.path.join(os.environ.get("TEMP", "/tmp"), f"verify_vx_{args.seed}")
    proc, db = D.start(args.exe, 17990 + args.seed % 5, datadir, False)
    try:
        db.execute("CREATE DATABASE IF NOT EXISTS vx")
        db.execute("USE vx")
        db.execute("DROP TABLE IF EXISTS t")
        db.execute("DROP TABLE IF EXISTS u")
        db.execute("DROP TABLE IF EXISTS w")
        db.execute("CREATE TABLE t (id INT PRIMARY KEY, g INT, x INT, y INT, w DECIMAL(10,2), s VARCHAR(10))")
        db.execute("CREATE TABLE u (id INT PRIMARY KEY, g INT, x INT, y INT, w DECIMAL(10,2), s VARCHAR(10), v VARCHAR(40))")
        db.execute("CREATE TABLE w (id INT PRIMARY KEY, v VARCHAR(40))")
        table = []
        values = []
        for i in range(args.rows):
            row = {"id": i, "g": rng.randrange(4)}
            row["x"] = None if rng.randrange(6) == 0 else rng.randrange(-4, 12)
            row["y"] = None if rng.randrange(6) == 0 else rng.randrange(-4, 12)
            row["w"] = None if rng.randrange(6) == 0 else Fraction(rng.randrange(-300, 1500), 100)
            row["s"] = None if rng.randrange(6) == 0 else "".join(rng.choice("abz") for _ in range(rng.randrange(0, 4)))
            table.append(row)
            values.append(f"({i}, {row['g']}, {literal(row['x'])}, {literal(row['y'])}, {literal(row['w'])}, {literal(row['s'])})")
        db.execute("INSERT INTO t VALUES " + ", ".join(values))
        db.execute("INSERT INTO u (id, g, x, y, w, s) VALUES " + ", ".join(values))
        checks = 0
        for n in range(args.queries):
            shape = rng.randrange(5)
            cond = gen_bool(rng, 3)
            number = gen_num(rng, 3)
            text = gen_str(rng, 3)
            if shape == 0:  # select list
                check(db, f"SELECT id, {number.sql} FROM t ORDER BY id", [(Fraction(r["id"]), number.ev(r)) for r in table])
                check(db, f"SELECT id, {text.sql} FROM t ORDER BY id", [(Fraction(r["id"]), text.ev(r)) for r in table])
                shown = f"({cond.sql}) AND TRUE" if cond.bare else cond.sql
                check(db, f"SELECT id, {shown} FROM t ORDER BY id", [(Fraction(r["id"]), cond.ev(r)) for r in table])
            elif shape == 1:  # WHERE
                check(db, f"SELECT id FROM t WHERE {cond.sql} ORDER BY id", [(Fraction(r["id"]),) for r in table if cond.ev(r) is True])
                check(db, f"SELECT id FROM t WHERE NOT ({cond.sql}) ORDER BY id", [(Fraction(r["id"]),) for r in table if cond.ev(r) is False])
            elif shape == 2:  # aggregates of the expression
                present = [v for v in (number.ev(r) for r in table) if v is not None]
                want = [(sum(present) if present else None, Fraction(len(present)), min(present) if present else None, max(present) if present else None,
                         round4(sum(present) / len(present)) if present else None)]
                check(db, f"SELECT SUM({number.sql}), COUNT({number.sql}), MIN({number.sql}), MAX({number.sql}), AVG({number.sql}) FROM t", want)
            elif shape == 3:  # per group
                rows = []
                kept = []
                threshold = rng.randrange(0, 30)
                for g in range(4):
                    group = [r for r in table if r["g"] == g]
                    if not group:
                        continue
                    hits = [number.ev(r) for r in group if cond.ev(r) is True]
                    hits = [v for v in hits if v is not None]
                    counted = [number.ev(r) if cond.ev(r) is True else Fraction(0) for r in group]
                    counted = [v for v in counted if v is not None]
                    rows.append((Fraction(g), sum(hits) if hits else None, Fraction(len(counted))))
                    true_rows = sum(1 for r in group if cond.ev(r) is True)
                    if true_rows * 10 + len(group) > threshold:
                        kept.append((Fraction(g),))
                check(db, f"SELECT g, SUM(CASE WHEN {cond.sql} THEN {number.sql} END), COUNT(CASE WHEN {cond.sql} THEN {number.sql} ELSE 0 END) FROM t GROUP BY g ORDER BY g", rows)
                check(db, f"SELECT g FROM t GROUP BY g HAVING SUM(CASE WHEN {cond.sql} THEN 1 ELSE 0 END) * 10 + COUNT(*) > {threshold} ORDER BY g", kept)
                scores = []
                for g in range(4):
                    group = [r for r in table if r["g"] == g]
                    if not group:
                        continue
                    hits = [number.ev(r) for r in group if cond.ev(r) is True]
                    hits = [v for v in hits if v is not None]
                    scores.append((Fraction(g), (sum(hits) if hits else Fraction(0)) * 100 + len(group)))
                check(db, f"SELECT g, COALESCE(SUM(CASE WHEN {cond.sql} THEN {number.sql} END), 0) * 100 + COUNT(*) FROM t GROUP BY g ORDER BY g", scores)
            else:  # UPDATE ... SET, INSERT of constants
                db.execute("UPDATE u SET v = NULL")
                db.execute(f"UPDATE u SET v = {text.sql}")
                check(db, "SELECT id, v FROM u ORDER BY id", [(Fraction(r["id"]), text.ev(r)) for r in table])
                constant = gen_str(rng, 3, cols=False)
                db.execute("DELETE FROM w")
                db.execute(f"INSERT INTO w VALUES (1, {constant.sql})")
                check(db, "SELECT id, v FROM w", [(Fraction(1), constant.ev({}))])
            checks += 1
        print(f"verify_value_expressions seed {args.seed}: {checks} rounds, no violation")
    except Violation as v:
        print("VIOLATION", v)
        sys.exit(1)
    finally:
        proc.kill()


if __name__ == "__main__":
    main()
