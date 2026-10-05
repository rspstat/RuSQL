"""Check ORDER BY and DISTINCT of the random corpus of diff_builds.py against the same statement without them, on ONE server.

  python verify_orderby_distinct.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

The corpus joins tables that share column names (id, grp), and spells columns with their table (`t.id`, `u.name`) -- exactly where
`ORDER BY t.col` once sorted nothing and `SELECT DISTINCT t.col` collapsed to one row. diff_builds.py compares two builds, so it
cannot say which of them is right; this checks a build by itself. For every statement that selects plain columns:
  * DISTINCT:  its rows are the set of rows of the statement without DISTINCT, with no duplicate.
  * ORDER BY:  its rows are the rows of the statement without ORDER BY (a prefix of them under LIMIT/OFFSET: a subset), and when
               the first sort key is one of the selected columns, that column is in order (direction and compare rule of the
               engine: numeric when both values are numbers, else text).
Statements with aggregates, GROUP BY, window functions, subqueries or set operations are skipped (they are checked elsewhere).
Exit code 1 and the statement on the first violation.
"""
import argparse, collections, os, random, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D


def cells(out):
    body = out.split("---END---")[0]
    if body.startswith("ERR"):
        return None
    rows = []
    seen_header = False
    for line in body.splitlines():
        if not line.startswith("|"):
            continue
        if not seen_header:
            seen_header = True
            continue
        rows.append(tuple(c.strip() for c in line.strip("|").split("|")))
    return rows


def compare(a, b):
    if a == "NULL" or b == "NULL":  # NULL sorts before every value (builds before 2026-10-05 compared it as the text "NULL")
        return (a != "NULL") - (b != "NULL")
    try:
        x, y = float(a), float(b)
        return (x > y) - (x < y)
    except ValueError:
        return (a > b) - (a < b)


SIMPLE_ITEM = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)?$")


def parse(sql):
    """(distinct, select items, from/where text, order keys [(text, asc)], limit/offset text) or None when the statement is not plain."""
    m = re.match(r"^SELECT (DISTINCT )?(.*?) (FROM .*?)(?: ORDER BY (.*?))?(?: (LIMIT \d+(?: OFFSET \d+)?))?$", sql)
    if not m or re.search(r"GROUP BY|HAVING|OVER|UNION|INTERSECT|EXCEPT|SELECT .*SELECT|\(|FOR UPDATE", sql):
        return None
    distinct, select, rest, order, limit = m.group(1) is not None, m.group(2), m.group(3), m.group(4), m.group(5)
    items = [i.strip() for i in select.split(",")]
    if not all(SIMPLE_ITEM.match(i) for i in items):
        return None
    keys = []
    if order:
        for k in order.split(","):
            k = k.strip()
            asc = not k.upper().endswith(" DESC")
            k = re.sub(r"\s+(ASC|DESC)$", "", k, flags=re.I)
            keys.append((k, asc))
    return distinct, items, rest, keys, limit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--queries", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rows", type=int, default=400)
    args = ap.parse_args()
    proc, db = D.start(args.exe, 17963, os.path.join(os.environ.get("TEMP", "/tmp"), "verify_orderby"), False)
    rng = random.Random(args.seed)
    gen = D.Gen(random.Random(args.seed * 7919 + 1), big=args.rows > 3000, chain_refs=True)
    checked = {"distinct": 0, "order": 0, "sorted_key": 0, "skipped": 0}
    try:
        for sql in D.load(rng, args.rows):
            db.execute(sql)
        for n in range(args.queries):
            if n and n % 100 == 0:
                for _ in range(3):
                    db.execute(gen.mutation())
            sql = gen.query()
            parsed = parse(sql)
            if not parsed:
                checked["skipped"] += 1
                continue
            distinct, items, rest, keys, limit = parsed
            got = cells(db.execute(sql))
            plain = cells(db.execute(f"SELECT {', '.join(items)} {rest}"))
            if got is None or plain is None:
                checked["skipped"] += 1
                continue
            if distinct:
                checked["distinct"] += 1
                if len(set(got)) != len(got):
                    print("DUPLICATE ROWS under DISTINCT:", sql)
                    return 1
                if (set(got) != set(plain) and not limit) or not set(got) <= set(plain):
                    print("DISTINCT rows differ from the distinct rows of the plain statement:", sql, len(got), len(set(plain)))
                    return 1
            elif keys:
                checked["order"] += 1
                if not limit and collections.Counter(got) != collections.Counter(plain):
                    print("ORDER BY changed the rows:", sql)
                    return 1
                if limit and not (collections.Counter(got) - collections.Counter(plain) == collections.Counter()):
                    print("ORDER BY ... LIMIT returned rows the plain statement does not have:", sql)
                    return 1
                first, asc = keys[0]
                if first in items and len(got) > 1:
                    col = items.index(first)
                    checked["sorted_key"] += 1
                    for a, b in zip(got, got[1:]):
                        c = compare(a[col], b[col])
                        if (c > 0 and asc) or (c < 0 and not asc):
                            print("NOT IN ORDER on", first, "(asc)" if asc else "(desc)", ":", sql, "|", a[col], "then", b[col])
                            return 1
    finally:
        proc.kill()
        proc.wait()
    print(f"queries={args.queries} checked: {checked} -- no violation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
