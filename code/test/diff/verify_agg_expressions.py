"""Check aggregates INSIDE select-list expressions, functions and CASE of ONE build against the rows they aggregate.

  python verify_agg_expressions.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

`SELECT MAX(v) - MIN(v)`, `SUM(v) / COUNT(*)`, `ROUND(AVG(v), 1)`, `COALESCE(SUM(v), 0)`, `CASE WHEN COUNT(*) > 3 THEN ...` over the
tables of diff_builds.py (t, u, v, w: the same column names in all of them), INNER / LEFT / RIGHT / FULL OUTER joins, with aliases
and WHERE, scalar or per group. The expression is computed here from the rows the aggregates run over (the same FROM/WHERE read
without any aggregate, as verify_aggregates.py does) and every group of the answer has to match. A scalar statement has to return
exactly one row: such a statement used to return one 0 row per row of the table.
Exit code 1 and the statement on the first violation.
"""
import argparse, os, random, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D
from verify_aggregates import ANY, KEYS, NUMERIC, ON, aggregate, cells, num

# MIN / MAX only over columns that never hold text that is not a number (val has "NULL1" after the corpus's UPDATE in builds before the NULL fix)
CLEAN = {"t": ["id", "grp", "price"], "u": ["id", "grp"], "v": ["id", "t_id", "qty"], "w": ["id", "k"]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--queries", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rows", type=int, default=400)
    args = ap.parse_args()
    proc, db = D.start(args.exe, 17965, os.path.join(os.environ.get("TEMP", "/tmp"), "verify_agg_expressions"), False)
    rng = random.Random(args.seed)
    checked = groups_checked = skipped = 0
    try:
        for sql in D.load(rng, args.rows):
            db.execute(sql)
        for n in range(args.queries):
            other = rng.choice(["u", "v", "w"])
            alias = rng.random() < 0.5
            tq, oq = ("x", "y") if alias else ("t", other)
            join = rng.choice(["JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL OUTER JOIN"])
            tables = {"t": tq, other: oq}
            frm = f"FROM t{' x' if alias else ''} {join} {other}{' y' if alias else ''} ON " + ON[other].format(o=oq, t=tq)
            if rng.random() < 0.5:
                frm += f" WHERE {tq}.grp {rng.choice(['>', '<', '>=', '<>'])} {rng.randint(0, 8)}"
            key = None
            if rng.random() < 0.7:
                kt = rng.choice(["t", other])
                key = (kt, rng.choice(KEYS[kt]))

            def spec():
                fn = rng.choice(["COUNT*", "COUNT", "SUM", "AVG", "MIN", "MAX"])
                if fn == "COUNT*":
                    return (fn, None)
                tb = rng.choice(["t", other])
                pool = ANY if fn == "COUNT" else (CLEAN if fn in ("MIN", "MAX") else NUMERIC)
                return (fn, (tb, rng.choice(pool[tb])))

            def text(sp):
                fn, col = sp
                if fn == "COUNT*":
                    return "COUNT(*)"
                return f"{fn}({tables[col[0]]}.{col[1]})"

            A, B = spec(), spec()
            a, b = text(A), text(B)
            k = rng.randrange(9)
            shapes = [
                (f"{a} + {b}", lambda x, y, c: x + y),
                (f"{a} - {b}", lambda x, y, c: x - y),
                (f"{a} * {b}", lambda x, y, c: x * y),
                (f"{a} / COUNT(*)", lambda x, y, c: x / c),
                (f"{a} + 7", lambda x, y, c: x + 7),
                (f"3 * {a}", lambda x, y, c: 3 * x),
                (f"ABS({a} - {b})", lambda x, y, c: abs(x - y)),
                (f"COALESCE({a}, 0)", lambda x, y, c: x),
                (f"CASE WHEN {a} > {b} THEN 'gt' ELSE 'le' END", lambda x, y, c: "gt" if x > y else "le"),
            ]
            expr, fn_value = shapes[k]
            # NULL (None) in, NULL out -- except COALESCE, and a CASE whose condition is not true takes the ELSE
            def null_aware(shape_index, vals, count):
                x, y = vals
                if shape_index == 7:  # COALESCE(a, 0)
                    return 0.0 if x is None else x
                if shape_index == 8:  # CASE WHEN a > b THEN 'gt' ELSE 'le' END
                    return "gt" if x is not None and y is not None and x > y else "le"
                needs_y = shape_index in (0, 1, 2, 6)
                needs_count = shape_index == 3
                if x is None or (needs_y and y is None) or (needs_count and count == 0):
                    return None
                return fn_value(x, y if y is not None else 0.0, count)
            sql = f"SELECT {tables[key[0]] + '.' + key[1] + ', ' if key else ''}{expr} AS e {frm}" + (f" GROUP BY {tables[key[0]]}.{key[1]}" if key else "")
            header, got = cells(db.execute(sql))
            if got is None:
                continue
            args_cols = [f"{tables[sp[1][0]]}.{sp[1][1]}" for sp in (A, B) if sp[1]]
            raw_sel = ([f"{tables[key[0]]}.{key[1]}"] if key else []) + (args_cols or [f"{tq}.id"])
            hdr2, raw = cells(db.execute(f"SELECT {', '.join(raw_sel)} {frm}"))
            if raw is None:
                continue
            checked += 1
            ks = 1 if key else 0
            grouped = {}
            for row in raw:
                grouped.setdefault(row[0] if key else "", []).append(row)
            if not key and not raw:
                grouped[""] = []
            # a scalar statement is one row, whatever the table holds; a grouped one has the groups of the plain read
            if len(got) != len(grouped):
                print("ROW COUNT:", sql, len(got), "rows, expected", len(grouped))
                return 1
            for row in got:
                rows = grouped.get(row[0] if key else "")
                if rows is None:
                    print("UNKNOWN GROUP:", sql, row[0])
                    return 1
                vals, pos = [], ks
                for sp in (A, B):
                    if sp[0] == "COUNT*":
                        values = rows
                    else:
                        values = [r[pos] for r in rows]
                        pos += 1
                    vals.append(aggregate(sp[0], values, True))
                if any(isinstance(v, str) and v != "NULL" for v in vals):  # text: no number to calculate with
                    skipped += 1
                    continue
                want = null_aware(k, [None if v == "NULL" else float(v) for v in vals], len(rows))
                cell = row[ks]
                if want is None:
                    ok = cell == "NULL"
                elif isinstance(want, str):
                    ok = cell == want
                else:
                    g = num(cell)
                    ok = g is not None and abs(g - want) <= 1e-4 * (1 + abs(want))
                if not ok:
                    print("WRONG VALUE:", sql, "| group", row[0] if key else "-", "| got", cell, "expected", want)
                    return 1
                groups_checked += 1
    finally:
        proc.kill()
        proc.wait()
    print(f"queries={args.queries} checked={checked} groups={groups_checked} skipped={skipped} -- no violation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
