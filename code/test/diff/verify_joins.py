"""Check joins of ONE build against a model of what SQL says: which rows a join produces, and how the tables are named.

  python verify_joins.py <engine_server.exe> [--queries N] [--seed S]

Three small tables that share column names (p(id, g, v), q(id, g, w, p_id), r(id, g, k); NULLs, repeated values, dangling keys) and
random statements over 1 to 4 table uses:

  * a table used twice or more (a self-join, a lookup table joined twice), each use with its own alias, written `t x` or `t AS x`;
  * INNER / LEFT / RIGHT / FULL OUTER / CROSS joins, the comma form `FROM a, b WHERE ...`, NATURAL and USING (as the first join);
  * derived tables `(SELECT * FROM t WHERE c < 2) AS d` in FROM and in JOIN;
  * ON conditions of several parts (equalities, inequalities and constants over any earlier table), WHERE with AND / OR / NOT / IS NULL;
  * the select list: qualified columns, `*`, `t.*` next to other columns, COUNT(*).

Every answer is computed here from the table contents by nested loops with SQL's three-valued logic (a comparison with NULL is
UNKNOWN, an outer join pads the other side with NULL, NATURAL / USING show the merged column once and first) and compared as a
multiset of rows together with the column names. Exit code 1 and the statement on the first difference.
"""
import argparse, os, random, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D
from verify_aggregates import cells

TABLES = {"p": ["id", "g", "v"], "q": ["id", "g", "w", "p_id"], "r": ["id", "g", "k"]}
OPS = ["=", "<", "<=", ">", ">=", "<>"]


def compare(op, a, b):
    """SQL comparison of two cells: True / False / None (UNKNOWN)."""
    if a is None or b is None:
        return None
    return {"=": a == b, "<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b, "<>": a != b}[op]


def tri_not(x):
    return None if x is None else not x


def tri_and(a, b):
    if a is False or b is False:
        return False
    return None if a is None or b is None else True


def tri_or(a, b):
    if a is True or b is True:
        return True
    return None if a is None or b is None else False


class Instance:
    def __init__(self, table, alias, style, derived, rows):
        self.table, self.alias, self.style, self.derived = table, alias, style, derived  # derived: (column, op, literal) or None
        self.cols = TABLES[table]
        self.rows = rows
        self.eff = alias or table

    def sql(self):
        if self.derived is not None:
            col, op, lit = self.derived
            return f"(SELECT * FROM {self.table} WHERE {col} {op} {lit}) {'AS ' if self.style == 'as' else ''}{self.alias}"
        if not self.alias:
            return self.table
        return f"{self.table} {'AS ' if self.style == 'as' else ''}{self.alias}"


def load(rng, db, limit):
    sizes = {"p": rng.randint(limit // 2 + 1, limit), "q": rng.randint(limit // 2 + 2, limit + 2), "r": rng.randint(max(limit // 3, 2), max(limit - 2, 3))}
    data = {}
    for t, cols in TABLES.items():
        db.execute(f"CREATE TABLE {t} (id INT PRIMARY KEY, " + ", ".join(f"{c} INT" for c in cols[1:]) + ")")
        rows = []
        for i in range(1, sizes[t] + 1):
            row = {"id": i}
            for c in cols[1:]:
                hi = sizes["p"] + 1 if c == "p_id" else max(3, limit // 6)
                row[c] = None if rng.random() < 0.15 else rng.randint(0, hi)
            rows.append(row)
        values = ", ".join("(" + ", ".join("NULL" if r[c] is None else str(r[c]) for c in cols) + ")" for r in rows)
        out = db.execute(f"INSERT INTO {t} VALUES {values}")
        assert out.startswith("OK"), out
        data[t] = rows
    return data


class Query:
    pass


def gen(rng, data):
    q = Query()
    n = rng.choice([1, 2, 2, 2, 3, 3, 4])
    tables = [rng.choice(list(TABLES)) for _ in range(n)]
    if rng.random() < 0.3 and n >= 2:
        tables[1] = tables[0]  # a self-join is common
    seen = set()
    insts = []
    for i, t in enumerate(tables):
        repeated = t in seen
        seen.add(t)
        alias = f"{t}{i}" if repeated or rng.random() < 0.6 else None
        style = rng.choice(["plain", "as"])
        derived = None
        if rng.random() < 0.15:
            if alias is None:
                alias = f"d{i}"
            derived = (rng.choice(TABLES[t]), rng.choice(OPS[:5]), rng.randint(0, 3))
        rows = data[t]
        if derived:
            col, op, lit = derived
            rows = [r for r in rows if compare(op, r[col], lit) is True]
        insts.append(Instance(t, alias, style, derived, rows))
    q.insts = insts

    q.joins = []  # per instance i >= 1: dict(kind, on, using)
    all_cross = n >= 2 and rng.random() < 0.12
    comma = all_cross and rng.random() < 0.7
    for i in range(1, n):
        kind = "CROSS" if all_cross else rng.choice(["INNER", "INNER", "LEFT", "LEFT", "RIGHT", "FULL", "CROSS"])
        j = {"kind": kind, "on": None, "using": None, "natural": False, "comma": comma}
        if i == 1 and kind in ("INNER", "LEFT", "RIGHT", "FULL") and rng.random() < 0.18:
            if kind == "INNER" and rng.random() < 0.4:
                j["natural"] = True
            else:
                j["using"] = rng.choice([["g"], ["id"], ["id", "g"]])
        if kind != "CROSS" and not j["natural"] and not j["using"]:
            j["on"] = gen_on(rng, insts, i)
        q.joins.append(j)

    q.where = gen_where(rng, insts) if rng.random() < (0.9 if all_cross else 0.55) else None
    if all_cross and q.where is not None and rng.random() < 0.8:
        q.where = ("and", gen_atom_eq(rng, insts), q.where)
    elif all_cross and q.where is None:
        q.where = gen_atom_eq(rng, insts)
    q.items = gen_items(rng, insts, q.joins)
    return q


def col_of(rng, inst):
    return rng.choice(inst.cols)


def gen_on(rng, insts, i):
    right = insts[i]
    left = rng.randrange(i)
    atoms = [("cmp", rng.choice(["=", "=", "=", "<", ">=", "<>"]), (i, col_of(rng, right)), (left, col_of(rng, insts[left])))]
    if rng.random() < 0.4:
        owner = rng.randrange(i + 1)
        atoms.append(("cmp", rng.choice(OPS), (owner, col_of(rng, insts[owner])), rng.randint(0, 3)))
    node = atoms[0]
    for a in atoms[1:]:
        node = ("and", node, a)
    return node


def gen_atom_eq(rng, insts):
    a, b = rng.randrange(len(insts)), rng.randrange(len(insts))
    return ("cmp", "=", (a, col_of(rng, insts[a])), (b, col_of(rng, insts[b])))


def gen_where(rng, insts):
    def atom():
        k = rng.random()
        a = rng.randrange(len(insts))
        if k < 0.55:
            return ("cmp", rng.choice(OPS), (a, col_of(rng, insts[a])), rng.randint(0, 3))
        if k < 0.7:
            return ("isnull", (a, col_of(rng, insts[a])), rng.random() < 0.5)
        b = rng.randrange(len(insts))
        return ("cmp", rng.choice(OPS), (a, col_of(rng, insts[a])), (b, col_of(rng, insts[b])))

    def tree(depth):
        if depth == 0 or rng.random() < 0.4:
            return atom()
        k = rng.random()
        if k < 0.2:
            return ("not", tree(depth - 1))
        return ("and" if k < 0.65 else "or", tree(depth - 1), tree(depth - 1))

    return tree(2)


def gen_items(rng, insts, joins):
    k = rng.random()
    if k < 0.12:
        return [("count",)]
    if k < 0.3:
        return [("star",)]
    if k < 0.45:
        a = rng.randrange(len(insts))
        items = [("qstar", a)]
        if rng.random() < 0.6:
            b = rng.randrange(len(insts))
            items.append(("col", b, col_of(rng, insts[b])))
        return items
    out = []
    for _ in range(rng.randint(1, 4)):
        a = rng.randrange(len(insts))
        out.append(("col", a, col_of(rng, insts[a])))
    return out


# ---- text ---------------------------------------------------------------------------------------------------------------------
def operand_sql(insts, x):
    if isinstance(x, tuple):
        return f"{insts[x[0]].eff}.{x[1]}"
    return str(x)


def cond_sql(insts, c):
    if c[0] == "cmp":
        return f"{operand_sql(insts, c[2])} {c[1]} {operand_sql(insts, c[3])}"
    if c[0] == "isnull":
        return f"{operand_sql(insts, c[1])} IS {'' if c[2] else 'NOT '}NULL"
    if c[0] == "not":
        return f"NOT ({cond_sql(insts, c[1])})"
    return f"({cond_sql(insts, c[1])} {c[0].upper()} {cond_sql(insts, c[2])})"


def sql_of(q):
    insts = q.insts
    parts = [insts[0].sql()]
    for i, j in enumerate(q.joins, start=1):
        right = insts[i].sql()
        if j["natural"]:
            parts.append(f"NATURAL JOIN {right}")
        elif j["kind"] == "CROSS":
            parts.append(f", {right}" if j["comma"] else f"CROSS JOIN {right}")
        else:
            word = {"INNER": rng_pick_inner, "LEFT": "LEFT JOIN", "RIGHT": "RIGHT JOIN", "FULL": "FULL OUTER JOIN"}[j["kind"]]
            if callable(word):
                word = word(q, i)
            tail = f"USING ({', '.join(j['using'])})" if j["using"] else f"ON {cond_sql(insts, j['on'])}"
            parts.append(f"{word} {right} {tail}")
    from_sql = " ".join(parts).replace(" ,", ",")
    select = []
    for it in q.items:
        if it[0] == "count":
            select.append("COUNT(*)")
        elif it[0] == "star":
            select.append("*")
        elif it[0] == "qstar":
            select.append(f"{insts[it[1]].eff}.*")
        else:
            select.append(f"{insts[it[1]].eff}.{it[2]}")
    where = f" WHERE {cond_sql(insts, q.where)}" if q.where is not None else ""
    return f"SELECT {', '.join(select)} FROM {from_sql}{where}"


def rng_pick_inner(q, i):
    return "INNER JOIN" if (i + len(q.insts)) % 2 else "JOIN"


# ---- the model -----------------------------------------------------------------------------------------------------------------
def value(insts, tup, x):
    if not isinstance(x, tuple):
        return x
    row = tup[x[0]]
    return None if row is None else row[x[1]]


def holds(insts, tup, c):
    if c is None:
        return True
    if c[0] == "cmp":
        return compare(c[1], value(insts, tup, c[2]), value(insts, tup, c[3]))
    if c[0] == "isnull":
        is_null = value(insts, tup, c[1]) is None
        return is_null if c[2] else not is_null
    if c[0] == "not":
        return tri_not(holds(insts, tup, c[1]))
    a, b = holds(insts, tup, c[1]), holds(insts, tup, c[2])
    return tri_and(a, b) if c[0] == "and" else tri_or(a, b)


def join_rows(q):
    insts = q.insts
    current = [(r,) for r in insts[0].rows]
    for i, j in enumerate(q.joins, start=1):
        right = insts[i].rows
        kind = j["kind"]
        using = j["using"]
        if j["natural"]:
            using = [c for c in insts[i].cols if c in insts[0].cols]

        def match(l, r):
            if kind == "CROSS":
                return True
            if using is not None:
                return all(l[0][c] is not None and r[c] is not None and l[0][c] == r[c] for c in using)
            return holds(insts, l + (r,), j["on"]) is True

        out = []
        used = set()
        width = i
        for l in current:
            hit = False
            for ri, r in enumerate(right):
                if match(l, r):
                    out.append(l + (r,))
                    used.add(ri)
                    hit = True
            if not hit and kind in ("LEFT", "FULL"):
                out.append(l + (None,))
        if kind in ("RIGHT", "FULL"):
            for ri, r in enumerate(right):
                if ri not in used:
                    out.append((None,) * width + (r,))
        current = out
    return [t for t in current if holds(insts, t, q.where) is True]


def star_columns(q):
    """[(header, function(tuple) -> cell)] of `*` in the order SQL gives it."""
    insts = q.insts
    j1 = q.joins[0] if q.joins else None
    using = None
    if j1 and j1["natural"]:
        using = [c for c in insts[1].cols if c in insts[0].cols]
    elif j1 and j1["using"]:
        using = j1["using"]
    out = []
    if using:
        for c in using:
            out.append((c, lambda t, c=c: t[0][c] if t[0] is not None else (t[1][c] if t[1] is not None else None)))
    for i, inst in enumerate(insts):
        for c in inst.cols:
            if using and i <= 1 and c in using:
                continue
            out.append((c, lambda t, i=i, c=c: None if t[i] is None else t[i][c]))
    return out


def expected(q):
    rows = join_rows(q)
    insts = q.insts
    header, getters = [], []
    for it in q.items:
        if it[0] == "count":
            return ["COUNT(*)"], [(str(len(rows)),)]
        if it[0] == "star":
            for h, g in star_columns(q):
                header.append(h)
                getters.append(g)
        elif it[0] == "qstar":
            for c in insts[it[1]].cols:
                header.append(c)
                getters.append(lambda t, i=it[1], c=c: None if t[i] is None else t[i][c])
        else:
            header.append(it[2])
            getters.append(lambda t, i=it[1], c=it[2]: None if t[i] is None else t[i][c])
    out = []
    for t in rows:
        cellvals = []
        for g in getters:
            v = g(t)
            cellvals.append("NULL" if v is None else str(v))
        out.append(tuple(cellvals))
    return header, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--queries", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rows", type=int, default=8, help="up to this many rows per table (a bigger table also meets the hash and index joins)")
    ap.add_argument("--port", type=int, default=17978)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    proc, db = D.start(args.exe, args.port, os.path.join(os.environ.get("TEMP", "/tmp"), f"verify_joins_{args.seed}"), False)
    stats = dict(checked=0, rows=0, self_join=0, star=0, derived=0, comma=0, using=0, outer=0, empty=0)
    try:
        data = load(rng, db, args.rows)
        for n in range(args.queries):
            q = gen(rng, data)
            sql = sql_of(q)
            header, rows = expected(q)
            out = db.execute(sql)
            if out.startswith("ERR"):
                print(f"ERROR (query {n}, seed {args.seed}): {sql}\n  {out.strip()[:300]}")
                sys.exit(1)
            got_header, got_rows = cells(out)
            if got_header is None:
                got_rows = []
            elif list(got_header) != header:
                print(f"DIFFERENT HEADER (query {n}, seed {args.seed}): {sql}\n  expected {header}\n  got      {list(got_header)}")
                sys.exit(1)
            if sorted(got_rows) != sorted(rows):
                print(f"DIFFERENT ROWS (query {n}, seed {args.seed}): {sql}")
                print(f"  expected {len(rows)} rows: {sorted(rows)[:12]}")
                print(f"  got      {len(got_rows)} rows: {sorted(got_rows)[:12]}")
                sys.exit(1)
            stats["checked"] += 1
            stats["rows"] += len(rows)
            stats["empty"] += not rows
            tabs = [i.table for i in q.insts]
            stats["self_join"] += len(set(tabs)) < len(tabs)
            stats["star"] += any(it[0] in ("star", "qstar") for it in q.items)
            stats["derived"] += any(i.derived for i in q.insts)
            stats["comma"] += any(j["comma"] for j in q.joins)
            stats["using"] += any(j["using"] or j["natural"] for j in q.joins)
            stats["outer"] += any(j["kind"] in ("LEFT", "RIGHT", "FULL") for j in q.joins)
    finally:
        proc.kill()
    print(" ".join(f"{k}={v}" for k, v in stats.items()) + " -- no violation")


if __name__ == "__main__":
    main()
