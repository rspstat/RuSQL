"""Check comparisons, ordering, MIN / MAX and joins of ONE build against MySQL's rules for the TYPES of the two sides.

  python verify_compare.py <engine_server.exe> [--statements N] [--seed S] [--rows R]

MySQL compares by type, not by what a text looks like: two strings compare as strings ('10' < '9', '007' and '7' are different), a number and anything
else compare as numbers (a string by the number it starts with: 'abc' is 0, '12abc' is 12). Four tables hold the same random rows -- a VARCHAR column
`s`, an INT `n`, a DECIMAL `p` and a BIGINT `b` (values around 2^53) -- with no index, with an index on every column, with hash indexes, and with `s` as
the PRIMARY KEY. Every random statement is run on each and compared with a reference computed here from the rows (exact rational arithmetic, so
integers beyond 2^53 and decimals are exact):

    WHERE <column> {= <> < <= > >=} <literal> | BETWEEN | IN | NOT IN     (the literal written as a string or as a number), AND / OR
    SELECT ... ORDER BY <column> [DESC], id [LIMIT]
    SELECT MIN(<column>), MAX(<column>) ...
    SELECT ... FROM <table> JOIN <table> ON <column> = <column>           (text = text, number = number, text = number)

Exit code 1 and the statement on the first violation.
"""
import argparse, os, random, re, sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D

NUMBER = re.compile(r"^[ \t\n\r\v\f]*([+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)")
STRINGS = ["7", "007", "7.0", "07", "10", "9", "abc", "ABC", "12abc", "", " 5", "-3", "1e1", "x", "9a", "10a", "9007199254740993", "0"]
QUOTED = ["7", "007", "7.0", "10", "9", "abc", "12abc", "", "x", "-3", "5", "9a", "9007199254740993", "9007199254740992"]
NUMBERS = ["7", "7.0", "007", "10", "9", "0", "-3", "7.5", "12", "5", "9007199254740993", "9007199254740992", "-9007199254740993"]
BIGS = ["9007199254740991", "9007199254740992", "9007199254740993", "9007199254740994", "5", "-9007199254740993", "0"]
COLUMNS = {"s": "text", "n": "number", "p": "number", "b": "number"}
TABLES = ["plain", "indexed", "hashed", "keyed"]


def number_of(text):
    m = NUMBER.match(text)
    return Fraction(m.group(1)) if m else Fraction(0)


def compare(column_class, value, quoted, written):
    """-1, 0 or 1: the value of a column against a literal written as a string (quoted) or as a number."""
    if column_class == "text" and quoted:
        return (value > written) - (value < written)
    x, y = number_of(value), number_of(written)
    return (x > y) - (x < y)


def cells(out):
    body = out.split("---END---")[0]
    if body.startswith("ERR"):
        print("ERROR:", " ".join(body.split())[:200])
        return None
    rows, header = [], False
    for line in body.splitlines():
        if not line.startswith("|"):
            continue
        if not header:
            header = True
            continue
        rows.append(tuple(c.strip() for c in line.strip("|").split("|")))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--statements", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rows", type=int, default=70)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    proc, db = D.start(args.exe, 17983, os.path.join(os.environ.get("TEMP", "/tmp"), "verify_compare"), False)
    try:
        rows = []
        seen = set()
        for i in range(1, args.rows + 1):
            row = {"id": i,
                   "s": None if rng.random() < 0.1 else rng.choice(STRINGS),
                   "n": None if rng.random() < 0.1 else str(rng.randint(-5, 15)),
                   "p": None if rng.random() < 0.1 else f"{rng.randint(-500, 2500) / 100:.2f}",
                   "b": None if rng.random() < 0.1 else rng.choice(BIGS)}
            rows.append(row)

        def sql_value(row, column):
            return "NULL" if row[column] is None else (f"'{row[column]}'" if column == "s" else str(row[column]))

        values = ", ".join("(" + ", ".join(sql_value(r, c) for c in ("id", "s", "n", "p", "b")) + ")" for r in rows)
        for t in ("plain", "indexed", "hashed"):
            db.execute(f"CREATE TABLE {t} (id INT PRIMARY KEY, s VARCHAR(20), n INT, p DECIMAL(10, 2), b BIGINT)")
            out = db.execute(f"INSERT INTO {t} VALUES {values}")
            if out.startswith("ERR"):
                print("SETUP:", " ".join(out.split("---END---")[0].split())[:200])
                return 1
        for c in ("s", "n", "p", "b"):
            db.execute(f"CREATE INDEX i_{c} ON indexed ({c})")
        db.execute("CREATE INDEX h_s ON hashed (s) USING HASH")
        db.execute("CREATE INDEX h_n ON hashed (n) USING HASH")
        db.execute("CREATE INDEX h_b ON hashed (b) USING HASH")
        db.execute("CREATE TABLE keyed (s VARCHAR(20) PRIMARY KEY, id INT, n INT, p DECIMAL(10, 2), b BIGINT)")
        keyed_rows = []
        for r in rows:
            if r["s"] is not None and r["s"] not in seen:
                seen.add(r["s"])
                keyed_rows.append(r)
        out = db.execute("INSERT INTO keyed VALUES " + ", ".join("(" + ", ".join(sql_value(r, c) for c in ("s", "id", "n", "p", "b")) + ")" for r in keyed_rows))
        if out.startswith("ERR"):
            print("SETUP:", " ".join(out.split("---END---")[0].split())[:200])
            return 1
        data = {"plain": rows, "indexed": rows, "hashed": rows, "keyed": keyed_rows}

        def written(pool_quoted):
            return (rng.choice(QUOTED), True) if pool_quoted else (rng.choice(NUMBERS), False)

        def literal(rng_quoted):
            text, quoted = written(rng_quoted)
            return (f"'{text}'" if quoted else text), text, quoted

        def predicate():
            column = rng.choice(list(COLUMNS))
            cls = COLUMNS[column]
            shape = rng.randrange(5)
            if shape <= 2:
                op = rng.choice(["=", "<>", "<", "<=", ">", ">="])
                sql, text, quoted = literal(rng.random() < 0.5)

                def holds(r, column=column, cls=cls, op=op, text=text, quoted=quoted):
                    if r[column] is None:
                        return False
                    c = compare(cls, r[column], quoted, text)
                    return {"=": c == 0, "<>": c != 0, "<": c < 0, "<=": c <= 0, ">": c > 0, ">=": c >= 0}[op]
                return f"{column} {op} {sql}", holds
            if shape == 3:
                lo, hi = literal(rng.random() < 0.5), literal(rng.random() < 0.5)
                negated = rng.random() < 0.25

                def holds(r, column=column, cls=cls, lo=lo, hi=hi, negated=negated):
                    if r[column] is None:
                        return False
                    inside = compare(cls, r[column], lo[2], lo[1]) >= 0 and compare(cls, r[column], hi[2], hi[1]) <= 0
                    return (not inside) if negated else inside
                return f"{column} {'NOT BETWEEN' if negated else 'BETWEEN'} {lo[0]} AND {hi[0]}", holds
            items = [literal(rng.random() < 0.5) for _ in range(rng.randint(1, 3))]
            negated = rng.random() < 0.33

            def holds(r, column=column, cls=cls, items=items, negated=negated):
                if r[column] is None:
                    return False
                any_equal = any(compare(cls, r[column], it[2], it[1]) == 0 for it in items)
                return (not any_equal) if negated else any_equal
            return f"{column} {'NOT IN' if negated else 'IN'} ({', '.join(it[0] for it in items)})", holds

        def key_of(column, row, text=None):
            value = row[column]
            return value

        def sort_key(column):
            if COLUMNS[column] == "text":
                return lambda r: (r[column] is not None, r[column] or "", r["id"])
            return lambda r: (r[column] is not None, number_of(r[column]) if r[column] is not None else Fraction(0), r["id"])

        checked = {"where": 0, "order": 0, "extreme": 0, "join": 0}
        for n in range(args.statements):
            kind = rng.random()
            table = rng.choice(TABLES)
            rows_t = data[table]
            if kind < 0.55:
                sql_cond, holds = predicate()
                if rng.random() < 0.3:
                    sql_b, holds_b = predicate()
                    joiner = rng.choice(["AND", "OR"])
                    sql_cond = f"({sql_cond}) {joiner} ({sql_b})"
                    holds_a = holds
                    holds = (lambda r, a=holds_a, b=holds_b: a(r) and b(r)) if joiner == "AND" else (lambda r, a=holds_a, b=holds_b: a(r) or b(r))
                sql = f"SELECT id FROM {table} WHERE {sql_cond} ORDER BY id"
                got = cells(db.execute(sql))
                want = [(str(r["id"]),) for r in sorted(rows_t, key=lambda r: r["id"]) if holds(r)]
                checked["where"] += 1
                if got != want:
                    print("WHERE:", sql, "| table", table, "| got", [g[0] for g in got or []][:20], "expected", [w[0] for w in want][:20])
                    return 1
            elif kind < 0.72:
                column = rng.choice(list(COLUMNS))
                desc = rng.random() < 0.5
                limit = rng.choice([None, 3, 10, 40])
                sql = f"SELECT id FROM {table} ORDER BY {column}{' DESC' if desc else ''}, id" + (f" LIMIT {limit}" if limit else "")
                got = cells(db.execute(sql))
                ordered = sorted(rows_t, key=sort_key(column), reverse=False)
                if desc:
                    # DESC reverses the column order; the ties keep the ascending id order
                    ordered = sorted(rows_t, key=lambda r: r["id"])
                    ordered = sorted(ordered, key=lambda r: ((r[column] is not None), (r[column] or "") if COLUMNS[column] == "text" else (number_of(r[column]) if r[column] is not None else Fraction(0))), reverse=True)
                want = [(str(r["id"]),) for r in ordered]
                if limit:
                    want = want[:limit]
                checked["order"] += 1
                if got != want:
                    print("ORDER BY:", sql, "| table", table, "| got", [g[0] for g in got or []][:20], "expected", [w[0] for w in want][:20])
                    return 1
            elif kind < 0.82:
                column = rng.choice(list(COLUMNS))
                present = [r[column] for r in rows_t if r[column] is not None]
                got = cells(db.execute(f"SELECT MIN({column}), MAX({column}) FROM {table}"))
                if not present:
                    want = [("NULL", "NULL")]
                elif COLUMNS[column] == "text":
                    want = [(min(present), max(present))]
                else:
                    smallest = min(present, key=number_of)
                    largest = max(present, key=number_of)
                    want = [(smallest, largest)]
                checked["extreme"] += 1
                if got is None or len(got) != 1 or (COLUMNS[column] == "text" and got != want) or (
                        COLUMNS[column] == "number" and tuple(number_of(c) for c in got[0]) != tuple(number_of(c) for c in want[0])):
                    print("MIN/MAX:", column, table, "got", got, "expected", want)
                    return 1
            else:
                other = rng.choice(TABLES)
                left, right = rng.choice(list(COLUMNS)), rng.choice(list(COLUMNS))
                sql = f"SELECT x.id, y.id FROM {table} x JOIN {other} y ON x.{left} = y.{right} ORDER BY x.id, y.id"
                got = cells(db.execute(sql))
                want = []
                for a in sorted(data[table], key=lambda r: r["id"]):
                    for b in sorted(data[other], key=lambda r: r["id"]):
                        if a[left] is None or b[right] is None:
                            continue
                        if COLUMNS[left] == "text" and COLUMNS[right] == "text":
                            equal = a[left] == b[right]
                        else:
                            equal = number_of(a[left]) == number_of(b[right])
                        if equal:
                            want.append((str(a["id"]), str(b["id"])))
                checked["join"] += 1
                if got != want:
                    print("JOIN:", sql, "| got", len(got or []), "rows, expected", len(want), "| first differences",
                          sorted(set(got or []) ^ set(want))[:6])
                    return 1
    finally:
        proc.kill()
        proc.wait()
    print(f"statements={args.statements} checked={checked} -- no violation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
