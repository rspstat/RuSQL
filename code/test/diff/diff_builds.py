"""Run the same random SELECT corpus on two engine_server builds and compare what they print.

  python diff_builds.py <old engine_server.exe> <new engine_server.exe> [--queries N] [--seed S] [--rows R] [--parallel]

For a change that must not alter any result (a faster aggregate / GROUP BY / DISTINCT / join path): both servers get the
same schema, data, mutations and queries, and every answer is compared as text (the "(0.001 sec)" timing line is
ignored). Aggregates (every aggregate function, FILTER, DISTINCT forms), GROUP BY / HAVING, DISTINCT, ORDER BY / LIMIT /
OFFSET in every combination (including the odd ones: LIMIT applied before an aggregate), joins (inner, left, three
tables, with WHERE / GROUP BY / DISTINCT), window functions, subqueries, FOR UPDATE and statements inside an explicit
transaction are generated from the seed. Statements that fail must fail with the same message on both builds.

By default both servers run with RUSTDB_PARALLEL=0, so ties in ORDER BY come out in the same (stable) order and the
text must be identical. --parallel keeps the thread pool on for both and compares each answer as a set of lines instead,
because an unstable parallel sort may order tied rows differently from run to run (and a floating-point SUM over a
different row order can print 4711 or 4711.0000). --modes seq,par runs the first server sequentially and the second with
the thread pool, and --exact compares the text itself: pass the same build twice to prove a build gives one answer
whatever the parallelism.

Exit code 1 and the query plus both outputs on the first --max-report differences. A seed is deterministic.
"""
import argparse, os, random, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "perf"))
import bench

TAGS = ["alpha", "beta", "gamma", "delta", "x", "y"]
NUM_COLS = ["val", "price", "grp", "id"]
ANY_COLS = ["val", "price", "grp", "code", "tag", "note"]
TIMING = re.compile(r"\(\d+\.\d+ sec\)")


class Client(bench.RuSQL):
    """bench.RuSQL decodes every 4096-byte chunk on its own, which cuts a multi-byte character that straddles two chunks."""

    def _read_until_end(self):
        buf = b""
        while True:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            buf += chunk
            if b"---END---" in buf:
                return buf.decode(errors="replace")


def start(exe, port, data, parallel, log=None):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.3).close()
        raise SystemExit(f"port {port} is already served by another process (a server left over from an earlier run?) -- stop it first")
    except OSError:
        pass
    shutil.rmtree(data, ignore_errors=True)
    env = dict(os.environ)
    if not parallel:
        env["RUSTDB_PARALLEL"] = "0"
    p = subprocess.Popen([exe, "--port", str(port), "--no-mysql", "--data-dir", data],
                         stdout=subprocess.DEVNULL, stderr=open(log, "w") if log else subprocess.DEVNULL, env=env)
    for _ in range(400):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.05)
    bench.RUSQL_PORT = port
    return p, Client()


def norm(out, unordered):
    out = TIMING.sub("", out)
    lines = [l.rstrip() for l in out.splitlines() if l.strip()]
    return sorted(lines) if unordered else lines


class Gen:
    def __init__(self, rng, big=False, chain_refs=True):
        self.r = rng
        self.chain_refs = chain_refs  # ON clauses of a later join that read an earlier joined table (builds before b3ca102 answered them wrongly)
        self.big = big  # large tables: no LEFT JOIN / three-way joins / WHERE scalar subquery (quadratic: minutes per query)

    def leaf(self, a=""):
        r = self.r
        k = r.randrange(13)
        if k == 0: return f"{a}val > {r.randint(0, 99)}"
        if k == 1: return f"{a}val <= {r.randint(0, 99)}"
        if k == 2: return f"{a}grp = {r.randint(0, 8)}"
        if k == 3: return f"{a}grp IN ({', '.join(str(r.randint(0, 8)) for _ in range(3))})"
        if k == 4: return f"{a}tag = '{r.choice(TAGS)}'"
        if k == 5: return f"{a}tag IS NULL"
        if k == 6: return f"{a}tag IS NOT NULL"
        if k == 7: return f"{a}val IS NULL"
        if k == 8:
            x = r.randint(0, 300)
            return f"{a}id BETWEEN {x} AND {x + r.randint(0, 200)}"
        if k == 9: return f"{a}code LIKE 'C{r.randint(0, 9)}%'"
        if k == 10: return f"{a}price > {r.randint(0, 50)}.5"
        if k == 11: return f"{a}note LIKE '%{r.choice('abc')}%'"
        return f"{a}code = '{r.choice(['7', '007', '7.0', 'C1', 'C10'])}'"

    def where(self, a="", p_none=0.3):
        r = self.r
        if r.random() < p_none:
            return ""
        parts = [self.leaf(a) for _ in range(r.choice([1, 1, 2, 2, 3]))]
        s = parts[0]
        for p in parts[1:]:
            s = f"{s} {r.choice(['AND', 'OR', 'OR'])} {p}"
        if r.random() < 0.1:
            s = f"NOT ({s})"
        return " WHERE " + s

    def tail(self, order_cols, allow_limit=True):
        r = self.r
        s = ""
        if order_cols and r.random() < 0.6:
            s += " ORDER BY " + ", ".join(f"{c} {r.choice(['ASC', 'DESC', ''])}".strip() for c in r.sample(order_cols, r.randint(1, min(2, len(order_cols)))))
        if allow_limit and r.random() < 0.4:
            s += f" LIMIT {r.randint(0, 12)}"
            if r.random() < 0.4:
                s += f" OFFSET {r.randint(0, 6)}"
        return s

    def agg(self, a=""):
        r = self.r
        k = r.randrange(20)
        n, c = r.choice(NUM_COLS), r.choice(ANY_COLS)
        if k == 0: return "COUNT(*)"
        if k == 1: return f"COUNT({a}{c})"
        if k == 2: return f"COUNT(DISTINCT {a}{c})"
        if k == 3: return f"SUM({a}{n})"
        if k == 4: return f"SUM(DISTINCT {a}{n})"
        if k == 5: return f"AVG({a}{n})"
        if k == 6: return f"AVG(DISTINCT {a}{n})"
        if k == 7: return f"MIN({a}{c})"
        if k == 8: return f"MAX({a}{c})"
        if k == 9: return f"STDDEV({a}{n})"
        if k == 10: return f"VARIANCE({a}{n})"
        if k == 11: return f"MEDIAN({a}{n})"
        if k == 12: return f"GROUP_CONCAT({a}{c})"
        if k == 13: return f"BIT_AND({a}{r.choice(['grp', 'id'])})"
        if k == 14: return f"BIT_OR({a}{r.choice(['grp', 'id'])})"
        if k == 15: return f"JSON_AGG({a}{c})"
        if k == 16: return f"ARRAY_AGG({a}{c})"
        if k == 17: return f"COUNT(*) FILTER (WHERE {self.leaf(a)})"
        if k == 18: return f"SUM({a}val) FILTER (WHERE {self.leaf(a)})"
        return f"MAX({a}{n})"

    # -- statement families -------------------------------------------------------------------------------------------
    def whole_agg(self):
        r = self.r
        aggs = [self.agg() for _ in range(r.randint(1, 4))]
        s = f"SELECT {', '.join(aggs)} FROM t{self.where()}"
        if r.random() < 0.15:
            s += self.tail(["val", "id"], True)  # ORDER BY / LIMIT / OFFSET before an aggregate: an old quirk, must stay
        return s

    def group_by(self):
        r = self.r
        keys = r.choice([["grp"], ["tag"], ["grp", "tag"], ["code"], ["grp", "val"]])
        aggs = [self.agg() for _ in range(r.randint(1, 3))]
        aliases = [f"a{i}" for i in range(len(aggs))]
        s = f"SELECT {', '.join(keys + [f'{a} AS {al}' for a, al in zip(aggs, aliases)])} FROM t{self.where()} GROUP BY {', '.join(keys)}"
        if r.random() < 0.5:
            s += " HAVING " + r.choice(["COUNT(*) > 2", "COUNT(*) >= 1", "SUM(val) > 50", "AVG(val) < 60", "MAX(val) > 10", "COUNT(*) < 40"])
        s += self.tail(keys + aliases)
        return s

    def distinct(self):
        r = self.r
        cols = r.choice([["tag"], ["grp"], ["grp", "tag"], ["tag", "val"], ["*"], ["code"], ["UPPER(tag)"], ["grp", "val", "tag"], ["val * 2"]])
        order = [c for c in cols if c != "*" and "(" not in c and "*" not in c] or ["id"]
        return f"SELECT DISTINCT {', '.join(cols)} FROM t{self.where()}{self.tail(order)}"

    def plain(self):
        r = self.r
        cols = r.choice([["id"], ["id", "val"], ["*"], ["grp", "tag", "val"], ["id", "val * 2 AS dbl"], ["id", "UPPER(tag) AS ut"], ["id", "CASE WHEN val > 50 THEN 'hi' ELSE 'lo' END AS band"]])
        return f"SELECT {', '.join(cols)} FROM t{self.where()}{self.tail(['val', 'id', 'grp', 'tag', 'code', 'price'])}"

    # ON clauses of the tables joined to t (they read t or a table joined before)
    JOIN_ON = {
        "u": ["t.grp = u.id", "u.id = t.grp", "t.grp = u.grp AND u.id < 5", "t.val = u.id", "t.grp = u.id OR u.id = 3",
              "t.grp = u.id AND u.grp = t.grp", "u.id = t.grp AND t.val > u.id", "t.grp = u.grp AND t.id = u.id", "t.grp = u.id AND u.name LIKE 'N1%'",
              "u.grp = t.grp AND (u.id < 4 OR t.val < 30)", "t.grp > u.id AND t.val = u.grp"],
        "v": ["v.t_id = t.id", "t.id = v.t_id", "v.t_id = t.id AND v.qty > 3", "v.t_id = u.id", "v.t_id = t.id AND v.qty = t.grp",
              "v.qty = t.grp AND v.t_id = t.id", "t.id = v.t_id AND NOT (v.qty > 5)"],
        "w": ["t.code = w.code", "w.code = t.code", "t.val = w.k", "w.k = t.grp", "w.k = u.id", "t.code = w.code AND w.k = t.grp",
              "w.k = t.grp AND t.code = w.code", "t.code = w.code AND t.val > w.k", "t.val = w.k AND t.grp = w.k"],
    }

    def jleaf(self, tables):
        r = self.r
        k, x = r.randint(0, 9), r.randint(0, 300)
        pool = [f"t.val > {r.randint(0, 99)}", f"t.grp = {r.randint(0, 8)}", f"t.tag = '{r.choice(TAGS)}'", "t.tag IS NULL",
                f"val < {r.randint(0, 99)}", f"id BETWEEN {x} AND {x + r.randint(0, 200)}", "grp IN (1, 2, 3)", f"t.code LIKE 'C{k}%'",
                "t.tag = 'name'", "t.code = 'grp'", "t.note LIKE '%b%'", f"t.id % {r.randint(2, 7)} = 0"]
        if "u" in tables:
            pool += [f"u.name LIKE 'N{r.randint(1, 3)}%'", f"u.id < {r.randint(1, 20)}", f"u.grp = {r.randint(0, 8)}", "name = 'N3'", "u.name IS NULL", "t.grp = u.grp"]
        if "v" in tables:
            pool += [f"v.qty > {k}", f"v.t_id < {x}", "qty = 4", "t.id < v.qty", "t.grp = 'qty'"]
        if "w" in tables:
            pool += ["w.code = '7'", f"w.k > {r.randint(0, 8)}", "w.code LIKE 'C1%'", "k < 5", "t.val = w.k", "t.grp = 'k'", "t.code = 'k'"]
        traps = []  # comparisons against an identifier-looking literal that names a column of ANOTHER joined table
        if "v" in tables:
            traps += ["t.grp = 'qty'", "t.val = 'qty'"]
        if "w" in tables:
            traps += ["t.grp = 'k'", "t.code = 'k'", "t.val = 'k'"]
        if traps and r.random() < 0.3:
            return r.choice(traps)
        return r.choice(pool)

    def jwhere(self, tables):
        r = self.r
        if r.random() < 0.15:
            return ""
        parts = [self.jleaf(tables) for _ in range(r.choice([1, 1, 2, 2, 3]))]
        s = parts[0]
        for p in parts[1:]:
            s = f"{s} {r.choice(['AND', 'AND', 'OR'])} {p}"
        if r.random() < 0.1:
            s = f"NOT ({s})"
        return " WHERE " + s

    def join(self):
        r = self.r
        order = r.sample(["u", "v", "w"], r.choice([1, 1, 2, 3]))
        if self.big:
            order = [x for x in order if x != "w"][:1] or ["u"]  # only the PK-keyed joins stay fast on big tables
        joined, base = [], "FROM t"
        for name in order:
            readable = ["t"] + (joined if self.chain_refs else []) + [name]
            ons = [o for o in self.JOIN_ON[name] if all(tb in readable for tb in re.findall(r"\b([tuvw])\.", o))]
            if self.big:  # an ON that is not a single equality is a nested loop in older builds: minutes on big tables
                ons = [o for o in ons if " AND " not in o and " OR " not in o] or ons[:1]
            jt = r.choice(["JOIN", "INNER JOIN"] if self.big else ["JOIN", "INNER JOIN", "LEFT JOIN", "LEFT JOIN"])
            base += f" {jt} {name} ON {r.choice(ons)}"
            joined.append(name)
        w = self.jwhere(joined)
        cols = ["t.id", "t.val"] + {"u": ["u.name"], "v": ["v.qty"], "w": ["w.code"]}.get(joined[-1], [])
        k = r.randrange(8)
        if k == 0:
            return f"SELECT {', '.join(cols)} {base}{w}{self.tail(['t.id', 't.val'])}"
        if k == 1:
            key = {"u": "u.name", "v": "v.qty", "w": "w.code"}[joined[0]]
            return f"SELECT {key}, COUNT(*), SUM(t.val) {base}{w} GROUP BY {key}{self.tail([key])}"
        if k == 2:
            return f"SELECT COUNT(*), MAX(t.val), AVG(t.price) {base}{w}"
        if k == 3:
            key = {"u": "u.name", "v": "v.qty", "w": "w.code"}[joined[-1]]
            return f"SELECT DISTINCT {key} {base}{w}{self.tail([key])}"
        if k == 4:
            return f"SELECT t.grp, COUNT(*) {base}{w} GROUP BY t.grp HAVING COUNT(*) > 1{self.tail(['t.grp'])}"
        if k == 5:
            return f"SELECT * {base}{w}{self.tail(['t.id'])}"
        if k == 6:
            return f"SELECT COUNT(*) {base}{w}"
        return f"SELECT {', '.join(cols)} {base}{w}{self.tail(['t.val', 't.id'], True)}"

    def window(self):
        r = self.r
        f = r.choice(["ROW_NUMBER() OVER (PARTITION BY grp ORDER BY val, id) AS w", "RANK() OVER (ORDER BY val) AS w",
                      "SUM(val) OVER (PARTITION BY grp) AS w", "COUNT(*) OVER (PARTITION BY tag) AS w",
                      "LAG(val) OVER (ORDER BY id) AS w"])
        return f"SELECT id, val, {f} FROM t{self.where()}{self.tail(['id', 'val'])}"

    def other(self):
        r = self.r
        k = r.randrange(6)
        if k == 2 and self.big:
            k = 5  # a scalar subquery in WHERE is re-run for every row: quadratic on large tables
        if k == 0: return f"SELECT id, val FROM t WHERE grp IN (SELECT id FROM u WHERE id < {r.randint(1, 20)}){self.tail(['id'])}"
        if k == 1: return f"SELECT (SELECT COUNT(*) FROM u) AS c, id FROM t{self.where()} ORDER BY id LIMIT {r.randint(1, 5)}"
        if k == 2: return f"SELECT COUNT(*) FROM t WHERE val > (SELECT AVG(val) FROM t)"
        if k == 3: return f"SELECT id, val FROM t{self.where()}{self.tail(['id'])} FOR UPDATE"
        if k == 4: return f"SELECT tag, COUNT(*) AS n FROM t{self.where()} GROUP BY tag ORDER BY n DESC, tag"
        return f"SELECT MIN(id), MAX(id), COUNT(*) FROM t{self.where()}"

    def subquery_where(self):
        """A WHERE-able predicate with a subquery: scalar comparisons, EXISTS, IN -- correlated or not."""
        r = self.r
        n = r.randint(0, 20)
        op = r.choice([">", "<", ">=", "<=", "="])
        outer = r.choice(["val", "grp", "id % 20"])
        shapes = [
            f"{outer} {op} (SELECT AVG(val) FROM t)",
            f"val {op} (SELECT MAX(val) FROM t WHERE grp = {r.randint(0, 8)})",
            f"grp {op} (SELECT MIN(grp) FROM u WHERE id > {n})",
            f"val {op} (SELECT val FROM t WHERE id = {r.randint(0, 300)})",          # one row, or none, or NULL
            f"val {op} (SELECT val FROM t WHERE id > {r.randint(0, 400)})",          # several rows: only the first counts
            f"val {op} (SELECT val FROM t WHERE id < 0)",                              # no rows
            f"val {op} (SELECT nosuchcol FROM t)",                                     # an error inside the subquery
            f"EXISTS (SELECT 1 FROM u WHERE name = 'N{n}')",
            f"NOT EXISTS (SELECT 1 FROM u WHERE id > {n + 30})",
            f"EXISTS (SELECT 1 FROM u WHERE u.id = t.grp AND u.grp {op} {r.randint(0, 8)})",
            f"NOT EXISTS (SELECT 1 FROM v WHERE v.t_id = t.id)",
            f"EXISTS (SELECT 1 FROM v WHERE v.t_id = t.id AND v.qty > {r.randint(0, 9)})",
            f"grp IN (SELECT id FROM u WHERE grp = {r.randint(0, 8)})",
            f"grp NOT IN (SELECT grp FROM u WHERE id < {n})",
            f"id IN (SELECT t_id FROM v WHERE qty > {r.randint(0, 9)})",
            f"val IN (SELECT qty FROM v WHERE v.t_id = t.id)",
            f"grp {op} (SELECT grp FROM u WHERE u.id = t.grp)",                       # correlated scalar
            f"id = {r.randint(0, 300)} AND val {op} (SELECT AVG(val) FROM t)",       # an indexed equality AND a subquery
        ]
        return r.choice(shapes)

    def subquery_select(self):
        r = self.r
        w = self.subquery_where()
        k = r.randrange(4)
        if k == 0: return f"SELECT COUNT(*) FROM t WHERE {w}"
        if k == 1: return f"SELECT id, val FROM t WHERE {w} AND {self.leaf()}{self.tail(['id'])}"
        if k == 2: return f"SELECT grp, COUNT(*) AS n FROM t WHERE {w} GROUP BY grp ORDER BY grp"
        return f"SELECT id FROM t WHERE {self.leaf()} OR {w}{self.tail(['id'])}"

    def and_point(self):
        """An indexed equality (primary key, hash index on tag, B+Tree index on grp) AND other predicates."""
        r = self.r
        keyed = r.choice([f"id = {r.randint(0, 400)}", f"id = {r.randint(0, 400)}.0", f"grp = {r.randint(0, 8)}", f"grp = {r.randint(0, 8)}.0",
                          f"tag = '{r.choice(TAGS)}'", f"id = {r.randint(0, 400)}"])
        parts = [keyed] + [self.leaf() for _ in range(r.randint(1, 2))]
        r.shuffle(parts)
        cond = " AND ".join(parts)
        cols = r.choice(["id", "*", "grp", "tag", "COUNT(*)", "id, val", "val"])
        return f"SELECT {cols} FROM t WHERE {cond}"

    def mutation(self):
        r = self.r
        k = r.randrange(7)
        if k == 4: return f"UPDATE t SET val = val + 1 WHERE {self.subquery_where()} AND id % 7 = {r.randint(0, 6)}"
        if k == 5: return f"DELETE FROM t WHERE {self.subquery_where()} AND id % 23 = {r.randint(0, 22)}"
        if k == 6: return f"UPDATE t SET tag = 'sq' WHERE grp = {r.randint(0, 8)} AND val {r.choice(['>', '<'])} (SELECT AVG(val) FROM t)"
        k = k % 4
        if k == 0: return f"UPDATE t SET val = val + 1 WHERE id % {r.randint(5, 20)} = 0"
        if k == 1: return f"DELETE FROM t WHERE id % {r.randint(17, 40)} = {r.randint(0, 10)}"
        if k == 2: return f"UPDATE t SET tag = '{r.choice(TAGS)}' WHERE grp = {r.randint(0, 8)}"
        return f"INSERT INTO t VALUES ({self.r.randint(10 ** 6, 10 ** 7)}, {r.randint(0, 8)}, {r.randint(0, 99)}, {r.randint(0, 99)}.25, 'C{r.randint(0, 99)}', 'beta', 'abc')"

    def query(self):
        k = self.r.random()
        for limit, fn in ((0.10, self.whole_agg), (0.28, self.group_by), (0.40, self.distinct), (0.48, self.plain), (0.66, self.join), (0.72, self.window),
                          (0.84, self.subquery_select), (0.92, self.and_point)):
            if k < limit:
                return fn()
        return self.other()


def load(rng, rows):
    stmts = ["CREATE DATABASE IF NOT EXISTS d", "USE d",
             "CREATE TABLE t (id INT PRIMARY KEY, grp INT, val INT, price DECIMAL(10,2), code VARCHAR(20), tag VARCHAR(10), note VARCHAR(30))",
             "CREATE TABLE u (id INT PRIMARY KEY, name VARCHAR(20), grp INT)",
             "CREATE TABLE v (id INT PRIMARY KEY, t_id INT, qty INT)",
             "CREATE TABLE w (id INT PRIMARY KEY, code VARCHAR(20), k INT)"]
    codes = ["7", "007", "7.0", "07"]
    batch = []
    for i in range(rows):
        val = "NULL" if rng.random() < 0.1 else str(rng.randint(0, 99))
        tag = "NULL" if rng.random() < 0.15 else "'" + rng.choice(TAGS) + "'"
        roll = rng.random()
        code = "NULL" if roll < 0.04 else "''" if roll < 0.07 else "'" + (rng.choice(codes) if roll < 0.2 else f"C{rng.randint(0, 99)}") + "'"
        note = rng.choice(["'abc'", "'a,b'", "'it''s'", "'zzz'", "NULL", "'café'"])
        batch.append(f"({i}, {rng.randint(0, 8)}, {val}, {rng.randint(0, 99)}.{rng.randint(0, 99):02d}, {code}, {tag}, {note})")
        if len(batch) == 200 or i == rows - 1:
            stmts.append("INSERT INTO t VALUES " + ", ".join(batch))
            batch = []
    stmts.append("INSERT INTO u VALUES " + ", ".join(f"({i}, 'N{i}', {rng.randint(0, 8)})" for i in range(1, 21)))
    vb = [f"({i}, {rng.randint(0, rows - 1)}, {rng.randint(1, 9)})" for i in range(max(rows // 2, 1))]
    for i in range(0, len(vb), 200):
        stmts.append("INSERT INTO v VALUES " + ", ".join(vb[i:i + 200]))
    wcodes = codes + [f"C{i}" for i in range(0, 40)] + ["NULL", "''", "7.00", "+7", "1e1", "10"]
    stmts.append("INSERT INTO w VALUES " + ", ".join(
        f"({i}, {c if c in ('NULL', chr(39) * 2) else repr(c)}, {'NULL' if rng.random() < 0.15 else rng.randint(0, 8)})" for i, c in enumerate(wcodes)))
    stmts += ["CREATE INDEX idx_grp ON t (grp)", "CREATE INDEX idx_tag ON t (tag) USING HASH",
              "UPDATE t SET val = val + 1 WHERE id % 13 = 0", "DELETE FROM t WHERE id % 29 = 0"]
    return stmts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--queries", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rows", type=int, default=400)
    ap.add_argument("--parallel", action="store_true", help="thread pool on for both servers; answers compared as sets of lines")
    ap.add_argument("--modes", default="seq,seq", help="'seq' or 'par' for the old and the new server, e.g. seq,par")
    ap.add_argument("--exact", action="store_true", help="with --parallel or --modes: compare the text itself, not sets of lines")
    ap.add_argument("--skip-chain-refs", action="store_true", help="no ON clause that reads an earlier joined table (older builds answered those wrongly)")
    ap.add_argument("--max-report", type=int, default=5)
    ap.add_argument("--log-new", help="write the new server's stderr to this file (for builds with debug output)")
    ap.add_argument("--show", type=int, default=0, help="print the first N queries with the first lines of the old build's answer")
    args = ap.parse_args()
    modes = ["par", "par"] if args.parallel else args.modes.split(",")
    unordered = args.parallel and not args.exact

    tmp = os.environ.get("TEMP", "/tmp")
    pa, a = start(args.old, 17961, os.path.join(tmp, "diff_builds_a"), modes[0] == "par")
    pb, b = start(args.new, 17962, os.path.join(tmp, "diff_builds_b"), modes[1] == "par", args.log_new)
    rng = random.Random(args.seed)
    gen = Gen(random.Random(args.seed * 7919 + 1), big=args.rows > 3000, chain_refs=not args.skip_chain_refs)
    diffs = errors = checked = 0

    def both(sql):
        try:
            return a.execute(sql), b.execute(sql)
        except TimeoutError:
            print("TIMEOUT (one of the builds took over 120 s):", sql)
            raise SystemExit(2)

    try:
        for sql in load(rng, args.rows):
            oa, ob = both(sql)
            if norm(oa, False) != norm(ob, False):
                print("SETUP DIFFERS:", sql[:120], oa[:200], ob[:200])
                return 1
        for n in range(args.queries):
            if n and n % 100 == 0:
                for _ in range(3):
                    both(gen.mutation())
            sql = gen.query()
            in_txn = "FOR UPDATE" in sql or gen.r.random() < 0.12
            if in_txn:
                iso = gen.r.choice([None, "SERIALIZABLE", "REPEATABLE READ", "READ COMMITTED"])
                if iso:
                    both(f"SET TRANSACTION ISOLATION LEVEL {iso}")
                both("BEGIN")
            oa, ob = both(sql)
            if in_txn:
                both("ROLLBACK")
            checked += 1
            if n < args.show:
                print(f"[{n}] {sql}\n      -> " + " | ".join(norm(oa, False)[:3])[:300])
            if oa.startswith("ERR"):
                errors += 1
            if norm(oa, unordered) != norm(ob, unordered):
                diffs += 1
                if diffs <= args.max_report:
                    la, lb = norm(oa, False), norm(ob, False)
                    only_old = [l for l in la if l not in set(lb)][:3]
                    only_new = [l for l in lb if l not in set(la)][:3]
                    print(f"DIFFERENT (query {n}, seed {args.seed}): {sql}\n  lines old/new: {len(la)}/{len(lb)}; only in old: {[l[:140] for l in only_old]}; only in new: {[l[:140] for l in only_new]}")
                    if len(la) < 60:
                        print(f"--- old\n{TIMING.sub('', oa)[:1500]}\n--- new\n{TIMING.sub('', ob)[:1500]}\n")
        print(f"queries={checked} errors(on both)={errors} differences={diffs}")
        return 1 if diffs else 0
    finally:
        for c, p in ((a, pa), (b, pb)):
            try:
                c.close()
            except OSError:
                pass
            p.kill()
            p.wait()


if __name__ == "__main__":
    sys.exit(main())
