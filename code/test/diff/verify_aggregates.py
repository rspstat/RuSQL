"""Check aggregates over `table.column` arguments of ONE build against the rows they aggregate.

  python verify_aggregates.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

The tables are those of diff_builds.py: t, u, v and w share the column names id, grp and code, which is where a bare column name
cannot say whose column an aggregate means (`COUNT(u.id)` read t.id for as long as the parser dropped the qualifier). For every
random statement

    SELECT <key>, AGG(<table.column>), ... FROM t [LEFT | RIGHT | FULL OUTER] JOIN <u|v|w> ON ... [WHERE ...] GROUP BY <key>

the same FROM/WHERE is run once more as `SELECT <key>, <the arguments> FROM ...` (no aggregate, no GROUP BY), the groups are
formed and the aggregates computed here with the engine's rules (NULLs are skipped, COUNT(*) counts rows, SUM/AVG/MIN/MAX
read numeric columns, MIN/MAX/SUM/AVG of nothing is NULL), and every group of the engine's answer has to match.
The result columns are also checked to be named as the statement wrote them (`COUNT(u.id)`).
Exit code 1 and the statement on the first violation.
"""
import argparse, os, random, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D

NUMERIC = {"t": ["id", "grp", "val", "price"], "u": ["id", "grp"], "v": ["id", "t_id", "qty"], "w": ["id", "k"]}
ANY = {"t": ["id", "grp", "val", "price", "code", "tag", "note"], "u": ["id", "name", "grp"], "v": ["id", "t_id", "qty"], "w": ["id", "code", "k"]}
KEYS = {"t": ["grp", "tag"], "u": ["name", "grp"], "v": ["qty"], "w": ["k", "code"]}
ON = {"u": "{o}.id = {t}.grp", "v": "{o}.t_id = {t}.id", "w": "{o}.k = {t}.grp"}


def cells(out):
    body = out.split("---END---")[0]
    if body.startswith("ERR"):
        return None, None
    lines = [l for l in body.splitlines() if l.startswith("|")]
    if not lines:
        return None, None
    split = lambda l: tuple(c.strip() for c in l.strip("|").split("|"))
    return split(lines[0]), [split(l) for l in lines[1:]]


def num(s):
    try:
        return float(s)
    except ValueError:
        return None


def aggregate(fn, values, numeric):
    """The engine's answer for one aggregate over the raw cell texts of its argument ('NULL' for a NULL)."""
    if fn == "COUNT*":
        return len(values)
    present = [v for v in values if v != "NULL"]
    if fn == "COUNT":
        return len(present)
    if fn == "COUNTD":
        return len(set(present))
    nums = [n for n in map(num, present) if n is not None]  # a text that is no number (builds before the NULL fix left "NULL1" in the corpus) is skipped by SUM / AVG
    if not present:  # no value: SUM, AVG, MIN and MAX have no answer
        return "NULL"
    if fn == "SUM":
        return sum(nums)
    if fn == "AVG":
        return sum(nums) / len(nums) if nums else "NULL"
    if len(nums) == len(present):  # every value is a number: compared as numbers, else as text (the engine's rule)
        return min(nums) if fn == "MIN" else max(nums)
    return min(present) if fn == "MIN" else max(present)


def close(got, want):
    if isinstance(want, str):
        return got == want
    g = num(got)
    return g is not None and abs(g - want) <= 1e-3 + 1e-6 * abs(want)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--queries", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rows", type=int, default=400)
    ap.add_argument("--no-header-check", action="store_true", help="skip the check of the result column names (builds before the argument kept its qualifier)")
    args = ap.parse_args()
    proc, db = D.start(args.exe, 17964, os.path.join(os.environ.get("TEMP", "/tmp"), "verify_aggregates"), False)
    rng = random.Random(args.seed)
    checked = groups_checked = having_checked = 0
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
            if rng.random() < 0.75:
                kt = rng.choice(["t", other])
                key = (kt, rng.choice(KEYS[kt]))
            specs = []
            for _ in range(rng.randint(1, 3)):
                fn = rng.choice(["COUNT*", "COUNT", "COUNTD", "SUM", "AVG", "MIN", "MAX"])
                if fn == "COUNT*":
                    specs.append((fn, None))
                else:
                    pool = ANY if fn in ("COUNT", "COUNTD") else NUMERIC
                    tb = rng.choice(["t", other])
                    specs.append((fn, (tb, rng.choice(pool[tb]))))
            def text(sp):
                fn, col = sp
                if fn == "COUNT*":
                    return "COUNT(*)"
                name = f"{tables[col[0]]}.{col[1]}"
                return {"COUNT": f"COUNT({name})", "COUNTD": f"COUNT(DISTINCT {name})"}.get(fn, f"{fn}({name})")
            # HAVING <an aggregate that the select list does not repeat> > k (a repeated one reads the select list's rounded value)
            having, threshold = None, rng.randint(0, 30)
            if key and rng.random() < 0.4:
                hfn = rng.choice(["COUNT*", "COUNT", "SUM", "AVG", "MIN", "MAX"])
                if hfn == "COUNT*":
                    having = (hfn, None)
                else:
                    pool = ANY if hfn == "COUNT" else NUMERIC
                    tb = rng.choice(["t", other])
                    having = (hfn, (tb, rng.choice(pool[tb])))
                if text(having) in [text(sp) for sp in specs]:
                    having = None
            items = ([f"{tables[key[0]]}.{key[1]}"] if key else []) + [text(sp) for sp in specs]
            sql = (f"SELECT {', '.join(items)} {frm}" + (f" GROUP BY {tables[key[0]]}.{key[1]}" if key else "") +
                   (f" HAVING {text(having)} > {threshold}" if having else ""))
            header, got = cells(db.execute(sql))
            if got is None:
                continue
            args_cols = [f"{tables[sp[1][0]]}.{sp[1][1]}" for sp in specs if sp[1]]
            having_cols = [f"{tables[having[1][0]]}.{having[1][1]}"] if having and having[1] else []
            raw_sel = ([f"{tables[key[0]]}.{key[1]}"] if key else []) + (args_cols + having_cols or [f"{tq}.id"])
            hdr2, raw = cells(db.execute(f"SELECT {', '.join(raw_sel)} {frm}"))
            if raw is None:
                continue
            checked += 1
            # the result columns are named as the statement wrote them
            want_header = tuple(([f"{key[1]}"] if key else []) + [text(sp) for sp in specs])
            if header != want_header and not args.no_header_check:
                print("HEADER:", sql, header, "expected", want_header)
                return 1
            ks = 1 if key else 0
            grouped = {}
            for row in raw:
                grouped.setdefault(row[0] if key else "", []).append(row)
            if not key and not raw:
                grouped[""] = []
            if having:
                # the groups HAVING keeps: the aggregate over the group's rows (the engine's rules) compared with the threshold the
                # way the engine compares (numbers as numbers, else as text)
                hpos = ks + len(args_cols)
                kept = {}
                for group_key, group_rows in grouped.items():
                    values = group_rows if having[0] == "COUNT*" else [r[hpos] for r in group_rows]
                    hv = aggregate(having[0], values, True)
                    if hv == "NULL":  # a comparison with NULL is not true: the group is dropped
                        continue
                    # a text that reads as a number is compared as one (the engine's rule), any other text as text
                    hn = hv if not isinstance(hv, str) else num(hv)
                    if (float(hn) > threshold) if hn is not None else (hv > str(threshold)):
                        kept[group_key] = group_rows
                grouped = kept
                having_checked += 1
            if len(got) != len(grouped):
                print("GROUP COUNT:", sql, len(got), "groups, expected", len(grouped))
                return 1
            for row in got:
                rows = grouped.get(row[0] if key else "")
                if rows is None:
                    print("UNKNOWN GROUP:", sql, row[0])
                    return 1
                pos = ks
                for i, sp in enumerate(specs):
                    fn, col = sp
                    if fn == "COUNT*":
                        values = rows
                    else:
                        values = [r[pos] for r in rows]
                        pos += 1
                    want = aggregate(fn, values, True)
                    if fn == "SUM" and want != "NULL":
                        want = float(want)
                    if isinstance(want, str) and fn in ("MIN", "MAX") and num(want) is not None:
                        want = float(want)
                    cell = row[ks + i]
                    if fn in ("COUNT*", "COUNT", "COUNTD"):
                        ok = cell == str(want)
                    else:
                        ok = close(cell, want)
                    if not ok:
                        print("WRONG VALUE:", sql, "| group", row[0] if key else "-", "|", text(sp), "got", cell, "expected", want)
                        return 1
                groups_checked += 1
    finally:
        proc.kill()
        proc.wait()
    print(f"queries={args.queries} checked={checked} groups={groups_checked} with HAVING={having_checked} -- no violation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
