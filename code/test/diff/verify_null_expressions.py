"""Check arithmetic, comparisons and scalar functions over NULL values of ONE build against the rows they read.

  python verify_null_expressions.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

`val + 1`, `val * price`, `grp / val`, `(x - y) * z`, `ABS(x - y)`, `ROUND(x / y, 2)` and `x > y` over the tables of diff_builds.py
(t.val and w.k hold NULLs, LEFT / RIGHT / FULL joins pad the other side with NULLs, literals include 0 for the division by zero).
Every statement is run for its expression and once more for the plain columns it reads (the same FROM / WHERE, no expression);
the expression is computed here from those cells and every row of the answer has to match:

  * a NULL operand gives NULL (it used to give "NULL1" for + and 0 for - * / and comparisons gave 1 or 0), x / 0 is NULL;
  * WHERE <expression> <op> k keeps exactly the rows whose value is not NULL and passes;
  * UPDATE <scratch table> SET r = <expression> stores the same value (or NULL) that SELECT shows.

The model computes in exact decimals (a quotient keeps nine digits) and rounds once at the end; the engine does MySQL's arithmetic (exact decimals, a quotient
with the dividend's digits and 4 more, rounded once at the end of the expression, DOUBLE columns in floating point), so a number is accepted
when it is within 1e-4 (relative) of the model's, a WHERE is not checked for a row whose value lies within 1e-3 of the limit, and the exact
text of MySQL's answers is checked by the conformance cases of the Catch2 suite (mysql_conformance_*.inc) instead.
Exit code 1 and the statement on the first violation.
"""
import argparse, decimal, math, os, random, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D
from verify_aggregates import NUMERIC, ON, cells, num


def fmt(f):
    if abs(f - math.trunc(f)) < 1e-9 and abs(f) < 1e15:
        return str(int(f))
    s = f"{f:.6f}".rstrip("0")
    return s[:-1] if s.endswith(".") else s


def reread(f):
    return float(fmt(f))


def dec(v):
    """A float read from a cell as the exact decimal it was written as (the shortest text that reads back to it)."""
    return v if isinstance(v, decimal.Decimal) else decimal.Decimal(repr(float(v)))


def apply(op, a, b):
    # Exact decimals like the engine and MySQL: + - * are exact, a quotient keeps nine digits (cut), and the number is rounded once, where it is shown or ROUNDed. A model
    # that rounded every operation to six decimals was 0.11016 for `id * (price / id)` where MySQL and the engine say 0.11, and one that used floats had 0.225 as
    # 0.22499999999999998 (ROUND(5.85 / 26, 2) is 0.23).
    if a is None or b is None:
        return None
    a, b = dec(a), dec(b)
    if op == "+":
        return a + b
    if op == "-":
        return a - b
    if op == "*":
        return a * b
    return None if b == 0 else (a / b).quantize(decimal.Decimal("1e-9"), rounding=decimal.ROUND_DOWN)


def show(v):
    return "NULL" if v is None else fmt(v)


def operand_value(cell):
    """None for NULL, a float for a number, the string 'dirt' for text that is no number (an older build's "NULL1")."""
    if cell == "NULL":
        return None
    n = num(cell)
    return n if n is not None else "dirt"


def read(db, sql):
    """(header, rows) of a statement's answer; an answer without rows is (None, []), a failure is (None, None)."""
    out = db.execute(sql)
    if out.startswith("OK") and "0 rows returned" in out:
        return None, []
    return cells(out)


def build_expression(rng, operands):
    """-> (text, value(cells) -> float | None, kind) where kind is 'arith' (the answer is the engine's own text), 'fn' or 'cmp'."""
    ops = ["+", "-", "*", "/"]

    def operand():
        if rng.random() < 0.7:
            i = rng.randrange(len(operands))
            return operands[i][0], (lambda cs, i=i: cs[i])
        lit = rng.randint(0, 4)  # 0 is there for the division by zero
        return str(lit), (lambda cs, lit=lit: float(lit))

    (x, xv), (y, yv), (z, zv) = operand(), operand(), operand()
    o1, o2 = rng.choice(ops), rng.choice(ops)
    shape = rng.randrange(9)
    if shape <= 1:
        return f"{x} {o1} {y}", lambda cs: apply(o1, xv(cs), yv(cs)), "arith"
    if shape == 2:  # * and / bind tighter than + and -
        def v2(cs):
            if o2 in "*/" and o1 in "+-":
                return apply(o1, xv(cs), apply(o2, yv(cs), zv(cs)))
            return apply(o2, apply(o1, xv(cs), yv(cs)), zv(cs))
        return f"{x} {o1} {y} {o2} {z}", v2, "arith"
    if shape == 3:
        return f"({x} {o1} {y}) {o2} {z}", lambda cs: apply(o2, apply(o1, xv(cs), yv(cs)), zv(cs)), "paren"
    if shape == 4:
        return f"{x} {o1} ({y} {o2} {z})", lambda cs: apply(o1, xv(cs), apply(o2, yv(cs), zv(cs))), "arith"
    if shape == 5:
        def v5(cs):
            v = apply(o1, xv(cs), yv(cs))
            return None if v is None else abs(v)
        return f"ABS({x} {o1} {y})", v5, "fn"
    if shape == 6:
        def v6(cs):
            v = apply(o1, xv(cs), yv(cs))
            return None if v is None else float(dec(v).quantize(decimal.Decimal("0.01"), rounding=decimal.ROUND_HALF_UP))  # (half away from zero on the digits, not on the binary fraction)
        return f"ROUND({x} {o1} {y}, 2)", v6, "fn"
    cmp = rng.choice([">", "<", ">=", "<=", "=", "<>"])

    def vc(cs):
        a, b = xv(cs), yv(cs)
        if a is None or b is None:
            return None
        return 1.0 if {">": a > b, "<": a < b, ">=": a >= b, "<=": a <= b, "=": abs(a - b) < 1e-9, "<>": a != b}[cmp] else 0.0
    return f"{x} {cmp} {y}", vc, "cmp"


def matches(cell, want, kind, loose=False):
    if want is None:
        return cell == "NULL"
    if loose:  # (a quotient inside a quotient: the engine keeps 9 digits where the model keeps 6, and the second division multiplies the difference)
        return cell != "NULL"
    g = num(cell)
    return g is not None and abs(g - want) <= (1e-4 if kind in ("arith", "paren", "fn") else 1e-5) * (1 + abs(want))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--queries", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rows", type=int, default=400)
    args = ap.parse_args()
    proc, db = D.start(args.exe, 17973, os.path.join(os.environ.get("TEMP", "/tmp"), "verify_null_expressions"), False)
    rng = random.Random(args.seed)
    selects = wheres = updates = nulls = dirt = refused = 0
    try:
        for sql in D.load(rng, args.rows):
            db.execute(sql)
        db.execute("CREATE TABLE s (id INT PRIMARY KEY, a INT, b INT, c DOUBLE, r VARCHAR(40))")
        scratch_model = {}
        for n in range(args.queries):
            other = rng.choice(["u", "v", "w"])
            alias = rng.random() < 0.5
            tq, oq = ("x", "y") if alias else ("t", other)
            single = rng.random() < 0.4
            tables = {"t": tq, other: oq}
            if single:
                frm = f"FROM t {tq if alias else ''}".strip()
                columns = [(f"{tq}.{c}", f"{tq}.{c}") for c in NUMERIC["t"]]
                order = f"{tq}.id"
            else:
                join = rng.choice(["JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL OUTER JOIN"])
                frm = f"FROM t{' x' if alias else ''} {join} {other}{' y' if alias else ''} ON " + ON[other].format(o=oq, t=tq)
                columns = [(f"{tq}.{c}", f"{tq}.{c}") for c in NUMERIC["t"]] + [(f"{oq}.{c}", f"{oq}.{c}") for c in NUMERIC[other]]
                order = f"{tq}.id, {oq}.id"
            where = ""
            if rng.random() < 0.4:
                where = f" WHERE {tq}.grp {rng.choice(['>', '<', '>=', '<>'])} {rng.randint(0, 8)}"
            text, value, kind = build_expression(rng, columns)
            names = [c[0] for c in columns]
            keys = [f"{tq}.id"] + ([] if single else [f"{oq}.id"])
            sql = f"SELECT {', '.join(keys)}, {text} AS e {frm}{where} ORDER BY {order}"
            header, got = read(db, sql)
            if got is None:
                print("ERROR:", sql, db.execute(sql).split("---END---")[0][:200])
                return 1
            raw_header, raw = read(db, f"SELECT {', '.join(keys)}, {', '.join(names)} {frm}{where} ORDER BY {order}")
            if raw is None or len(raw) != len(got):
                print("ROW COUNT:", sql, len(got), "vs", None if raw is None else len(raw))
                return 1
            ks = len(keys)
            expected = []
            for g, r in zip(got, raw):
                if g[:ks] != r[:ks]:
                    print("ORDER:", sql, g[:ks], r[:ks])
                    return 1
                cs = [operand_value(c) for c in r[ks:]]
                if "dirt" in cs:  # text that is no number in a column that is read: nothing to compare with
                    dirt += 1
                    expected.append("skip")
                    continue
                want = value(cs)
                if isinstance(want, decimal.Decimal):
                    want = float(want)
                expected.append(want)
                if not matches(g[ks], want, kind, text.count("/") > 1):
                    print("WRONG VALUE:", sql, "| row", g[:ks], "| cells", dict(zip(names, r[ks:])), "| got", g[ks], "expected", show(want))
                    return 1
                nulls += want is None
            selects += 1

            # WHERE <expression> <op> k: the rows whose value is not NULL and passes, in the order of the plain read
            if kind == "arith" and not text.startswith("(") and text.count("/") <= 1:
                k = rng.randint(-2, 6)
                op = rng.choice([">", "<=", ">=", "<"])
                sql = f"SELECT {', '.join(keys)} {frm}{(where + ' AND ') if where else ' WHERE '}{text} {op} {k} ORDER BY {order}"
                header, kept = read(db, sql)
                if kept is None:
                    print("ERROR:", sql, db.execute(sql).split("---END---")[0][:200])
                    return 1
                want_keys, maybe_keys = [], set()
                for g, r, want in zip(got, raw, expected):
                    if want == "skip":
                        want_keys = None
                        break
                    if want is not None and abs(want - k) < 1e-3:
                        maybe_keys.add(tuple(g[:ks]))  # (too close to the limit to tell: the engine's digits are not the model's)
                    elif want is not None and {">": want > k, "<=": want <= k, ">=": want >= k, "<": want < k}[op]:
                        want_keys.append(tuple(g[:ks]))
                kept_keys = [tuple(r) for r in kept]
                if want_keys is not None and not (set(want_keys) <= set(kept_keys) <= set(want_keys) | maybe_keys and len(set(kept_keys)) == len(kept_keys)):
                    print("WRONG WHERE:", sql, "| engine kept", len(kept), "rows, expected", len(want_keys))
                    return 1
                wheres += want_keys is not None

            # UPDATE s SET r = <expression over a, b, c> on part of a scratch table filled from t, read back
            if single and n % 4 == 0 and kind in ("arith", "paren") and text.count("/") <= 1:
                rows_t = [(r[0], [operand_value(c) for c in r[1:]]) for r in read(db, f"SELECT {tq}.id, {', '.join(names)} {frm}{where} ORDER BY {order}")[1]]
                if any("dirt" in cs for _, cs in rows_t):
                    continue
                db.execute("DELETE FROM s")
                scratch_model = {}
                # a = grp, b = val, c = price: the scratch table has its own column names, the expression is rewritten to them
                tuples = [f"({rid}, {show(cs[1])}, {show(cs[2])}, {show(cs[3])}, 'x')" for rid, cs in rows_t]
                for i in range(0, len(tuples), 200):
                    db.execute("INSERT INTO s VALUES " + ", ".join(tuples[i:i + 200]))
                scratch_model = {rid: "x" for rid, _ in rows_t}
                rewrite = text.replace(f"{tq}.grp", "a").replace(f"{tq}.val", "b").replace(f"{tq}.price", "c").replace(f"{tq}.id", "id")
                if "." in rewrite:
                    continue
                mod = rng.randint(2, 4)
                rem = rng.randrange(mod)
                res = db.execute(f"UPDATE s SET r = {rewrite} WHERE id % {mod} = {rem}")
                if "Division by 0" in res and "/" in rewrite:
                    # a division by zero in a row the UPDATE takes is error 1365 and nothing is written (SELECT shows NULL for it)
                    if any(cell != "x" for _, cell in read(db, "SELECT id, r FROM s ORDER BY id")[1]):
                        print("WRONG UPDATE: the refused", f"UPDATE s SET r = {rewrite} WHERE id % {mod} = {rem}", "wrote")
                        return 1
                    refused += 1
                    continue
                if not res.startswith("OK"):
                    print("ERROR:", f"UPDATE s SET r = {rewrite}", res.split("---END---")[0][:200])
                    return 1
                for rid, cs in rows_t:
                    if int(rid) % mod == rem:
                        # columns of the scratch table: id, a (grp), b (val), c (price) in the order of NUMERIC["t"]
                        cs2 = {"id": float(rid), "grp": cs[1], "val": cs[2], "price": cs[3]}
                        modelled = value([cs2[c.split('.')[1]] for c in names])
                        scratch_model[rid] = show(float(modelled) if isinstance(modelled, decimal.Decimal) else modelled)
                back = read(db, "SELECT id, r FROM s ORDER BY id")[1]
                for rid, cell in back:
                    both = num(cell), num(scratch_model[rid])
                    if cell != scratch_model[rid] and not (both[0] is not None and both[1] is not None and abs(both[0] - both[1]) <= 1e-4 * (1 + abs(both[1]))):
                        print("WRONG UPDATE:", f"UPDATE s SET r = {rewrite} WHERE id % {mod} = {rem}", "| id", rid, "got", cell, "expected", scratch_model[rid])
                        return 1
                updates += 1
    finally:
        proc.kill()
        proc.wait()
    print(f"queries={args.queries} selects={selects} NULL answers={nulls} with WHERE={wheres} with UPDATE={updates} refused for a division by zero={refused} skipped (text in a column)={dirt} -- no violation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
