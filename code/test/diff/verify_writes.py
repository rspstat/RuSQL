"""Check INSERT / REPLACE / INSERT ... ON DUPLICATE KEY UPDATE / UPDATE / DELETE of ONE build against a model of what MySQL does.

  python verify_writes.py <engine_server.exe> [--statements N] [--seed S]

Two tables

    t (id INT PRIMARY KEY, a INT NOT NULL, u INT UNIQUE, s VARCHAR(3))
    c (cid INT PRIMARY KEY, tid INT, FOREIGN KEY (tid) REFERENCES t(id))        -- ON DELETE / ON UPDATE RESTRICT

and random statements over small value domains, with NULLs, numeric strings ('3'), fractions (7.5), text that is no number ('q') and
text that is too long ('toolong'). A statement either succeeds or fails and, when it fails, changes NOTHING (statement atomicity); the
model below says which of the two it is and what the tables then hold, and after every statement both tables are read back and compared:

  * NOT NULL, UNIQUE (NULLs excepted), PRIMARY KEY, the type of every column (strict mode), foreign keys on both sides (RESTRICT);
  * REPLACE deletes every row it conflicts with, but only if the whole statement is valid (it used to delete first and lose the rows
    when the insert then failed) and no deleted row is referenced by a child;
  * ON DUPLICATE KEY UPDATE rewrites the conflicting row like an UPDATE: the same constraints, VALUES(col), a failure changes nothing;
  * an UPDATE of several rows is checked as a whole (`SET u = u + 1` shifts a chain; `SET u = 3` over two rows is a duplicate);
  * UPDATE ... JOIN, DELETE ... JOIN and MERGE against a source table `sr` are as strict as the one-table statements, and fail as a whole.

Exit code 1 and the statement on the first difference.
"""
import argparse, decimal, os, random, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D

ERR = object()  # a value the column does not accept
decimal.getcontext().prec = 40


# ---- values: (sql text, python value) -----------------------------------------------------------------------------------------------
def gen_a(rng):
    return rng.choice([("NULL", None), ("1", 1), ("2", 2), ("3", 3), ("4", 4), ("5", 5), ("'3'", "3"), ("'q'", "q"), ("7.5", decimal.Decimal("7.5"))])


def gen_u(rng):
    return rng.choice([("NULL", None), ("1", 1), ("2", 2), ("3", 3), ("4", 4), ("5", 5), ("6", 6), ("'q'", "q")])


def gen_s(rng):
    return rng.choice([("NULL", None), ("''", ""), ("'x'", "x"), ("'yy'", "yy"), ("'toolong'", "toolong")])


def coerce_int(v):
    if v is None:
        return None
    if isinstance(v, int):
        return v
    try:
        d = decimal.Decimal(v)
    except decimal.InvalidOperation:
        return ERR
    return int(d.to_integral_value(rounding=decimal.ROUND_HALF_UP))  # half away from zero: for the positive values used here


def coerce_s(v):
    if v is None or len(v) <= 3:
        return v
    return ERR


# ---- the model ------------------------------------------------------------------------------------------------------------------------
SR = {}  # the source table: id -> (a, u); the statements read it, none writes it


class Model:
    def __init__(self):
        self.t = {}  # id -> [a, u, s]
        self.c = {}  # cid -> tid | None

    def copy(self):
        m = Model()
        m.t = {k: list(v) for k, v in self.t.items()}
        m.c = dict(self.c)
        return m

    def referenced(self, tid):
        return any(v == tid for v in self.c.values())

    def unique_ok(self, rows):
        us = [r[1] for r in rows.values() if r[1] is not None]
        return len(us) == len(set(us))

    # a row as the INSERT builds it; ERR when a column rejects its value
    @staticmethod
    def build(idv, av, uv, sv):
        a, u, s = coerce_int(av), coerce_int(uv), coerce_s(sv)
        i = coerce_int(idv)
        if ERR in (a, u, s, i):
            return None
        if a is None:
            return None  # NOT NULL
        return i, [a, u, s]

    def insert(self, rows, mode="abort"):
        m = self.copy()
        for idv, av, uv, sv in rows:
            built = Model.build(idv, av, uv, sv)
            if built is None:
                return False
            i, row = built
            if i in m.t or (row[1] is not None and any(r[1] == row[1] for r in m.t.values())):
                return False
            m.t[i] = row
        self.t = m.t
        return True

    def replace(self, rows):
        m = self.copy()
        for idv, av, uv, sv in rows:  # sequential: a later row replaces an earlier one
            built = Model.build(idv, av, uv, sv)
            if built is None:
                return False
            i, row = built
            victims = [k for k, r in m.t.items() if k == i or (row[1] is not None and r[1] == row[1])]
            if any(m.referenced(k) for k in victims):
                return False
            for k in victims:
                del m.t[k]
            m.t[i] = row
        self.t = m.t
        return True

    def odku(self, idv, av, uv, sv, kind, newv):
        built = Model.build(idv, av, uv, sv)
        if built is None:
            return False
        i, row = built
        conflicts = [k for k, r in self.t.items() if k == i or (row[1] is not None and r[1] == row[1])]
        if len(set(conflicts)) > 1:
            return None  # undefined which row MySQL updates: the generator skips it
        if not conflicts:
            self.t[i] = row
            return True
        k = conflicts[0]
        new = list(self.t[k])
        if kind == "a+1":
            new[0] = None if new[0] is None else new[0] + 1
        elif kind == "u=values":
            new[1] = row[1]
        elif kind == "s=z":
            new[2] = "z"
        elif kind == "a=null":
            new[0] = None
        elif kind == "a,s=values":
            new[0], new[2] = row[0], row[2]
        elif kind == "u=new":
            new[1] = coerce_int(newv)
        if new[0] is None or new[1] is ERR:
            return False
        m = self.copy()
        m.t[k] = new
        if not m.unique_ok(m.t):
            return False
        self.t = m.t
        return True

    def update(self, kind, arg, where_max):
        m = self.copy()
        for k, r in m.t.items():
            if k > where_max:
                continue
            if kind == "a+k":
                r[0] = r[0] + arg
            elif kind == "a=null":
                r[0] = None
            elif kind == "u+1":
                r[1] = None if r[1] is None else r[1] + 1
            elif kind == "u=v":
                r[1] = coerce_int(arg)
            elif kind == "s=v":
                r[2] = coerce_s(arg)
            elif kind == "a=v":
                r[0] = coerce_int(arg)
            if r[0] is None or r[0] is ERR or r[1] is ERR or r[2] is ERR:
                return False
        if not m.unique_ok(m.t):
            return False
        self.t = m.t
        return True

    def multi_update(self, sets, where_min):
        m = self.copy()
        for k, r in m.t.items():
            if k not in SR:
                continue
            sa, su = SR[k]
            if where_min is not None and (sa is None or not sa > where_min):
                continue
            if "a" in sets:
                r[0] = sa
            if "u" in sets:
                r[1] = su
            if r[0] is None:
                return False
        if not m.unique_ok(m.t):
            return False
        self.t = m.t
        return True

    def multi_delete(self, where_min):
        victims = [k for k in self.t if k in SR and (where_min is None or (SR[k][0] is not None and SR[k][0] > where_min))]
        if any(self.referenced(k) for k in victims):
            return False
        for k in victims:
            del self.t[k]
        return True

    def merge(self, delete_min):
        m = self.copy()
        deleted = set()
        for sid, (sa, su) in SR.items():
            if sid in self.t:  # matched (against the table as the statement found it)
                if delete_min is not None and sa is not None and sa > delete_min:
                    if self.referenced(sid):
                        return False
                    deleted.add(sid)
                    m.t.pop(sid, None)
                else:
                    if sa is None:
                        return False
                    m.t[sid][0] = sa
            else:
                if sa is None:
                    return False
                m.t[sid] = [sa, su, None]
        if any(self.t[k][1] is not None and self.t[k][1] == SR[s][1] for k in deleted for s in SR if s not in self.t and SR[s][1] is not None):
            return None  # an insert that the row being deleted in the same statement was blocking: left undefined
        if not m.unique_ok(m.t):
            return False
        self.t = m.t
        return True

    def update_id(self, k, newid):
        if k not in self.t or newid == k:
            return True
        if self.referenced(k) or newid in self.t:
            return False
        self.t[newid] = self.t.pop(k)
        return True

    def delete_t(self, where_max):
        victims = [k for k in self.t if k <= where_max]
        if any(self.referenced(k) for k in victims):
            return False
        for k in victims:
            del self.t[k]
        return True

    def insert_c(self, cid, tid):
        if cid in self.c or (tid is not None and tid not in self.t):
            return False
        self.c[cid] = tid
        return True

    def update_c(self, cid, tid):
        if cid not in self.c:
            return True
        if tid is not None and tid not in self.t:
            return False
        self.c[cid] = tid
        return True

    def delete_c(self, cid):
        self.c.pop(cid, None)
        return True


def read(db, sql):
    out = db.execute(sql).split("---END---")[0]
    if out.startswith("ERR"):
        return None
    lines = [l for l in out.splitlines() if l.startswith("|")]
    return [tuple(c.strip() for c in l.strip("|").split("|")) for l in lines[1:]]


def show(v):
    return "NULL" if v is None else str(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--statements", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    proc, db = D.start(args.exe, 17977, os.path.join(os.environ.get("TEMP", "/tmp"), "verify_writes"), False)
    rng = random.Random(args.seed)
    model = Model()
    stats = {"ok": 0, "failed": 0}
    try:
        db.execute("CREATE TABLE t (id INT PRIMARY KEY, a INT NOT NULL, u INT UNIQUE, s VARCHAR(3))")
        db.execute("CREATE TABLE c (cid INT PRIMARY KEY, tid INT, FOREIGN KEY (tid) REFERENCES t(id))")
        db.execute("CREATE TABLE sr (id INT PRIMARY KEY, a INT, u INT)")
        for i in rng.sample(range(1, 9), rng.randint(2, 5)):
            a, u = rng.choice([None, 1, 2, 3, 4, 5, 6, 7, 8, 9]), rng.choice([None, 1, 2, 3, 4, 5, 6])
            SR[i] = (a, u)
            db.execute(f"INSERT INTO sr VALUES ({i}, {show(a)}, {show(u)})")
        for n in range(args.statements):
            kind = rng.choice(["insert"] * 4 + ["replace"] * 3 + ["odku"] * 3 + ["update"] * 3 + ["update_id", "delete_t"] + ["insert_c"] * 2 + ["update_c", "delete_c"] + ["multi"] * 2)
            ident = lambda: rng.randint(1, 8)
            if kind in ("insert", "replace"):
                rows, texts = [], []
                subset = kind == "insert" and rng.random() < 0.2
                for _ in range(rng.randint(1, 3 if kind == "insert" else 2)):
                    i = ident()
                    (at, av), (ut, uv), (st, sv) = gen_a(rng), gen_u(rng), gen_s(rng)
                    if subset:  # (id, a) only: u and s are NULL
                        rows.append((i, av, None, None))
                        texts.append(f"({i}, {at})")
                    else:
                        rows.append((i, av, uv, sv))
                        texts.append(f"({i}, {at}, {ut}, {st})")
                if kind == "insert":
                    sql = f"INSERT INTO t {'(id, a) ' if subset else ''}VALUES " + ", ".join(texts)
                    want = model.insert(rows)
                else:
                    sql = "REPLACE INTO t VALUES " + ", ".join(texts)
                    want = model.replace(rows)
            elif kind == "odku":
                i = ident()
                (at, av), (ut, uv), (st, sv) = gen_a(rng), gen_u(rng), gen_s(rng)
                form = rng.choice(["a+1", "u=values", "s=z", "a=null", "a,s=values", "u=new"])
                newt, newv = gen_u(rng)
                assign = {"a+1": "a = a + 1", "u=values": "u = VALUES(u)", "s=z": "s = 'z'", "a=null": "a = NULL", "a,s=values": "a = VALUES(a), s = VALUES(s)",
                          "u=new": f"u = {newt}"}[form]
                sql = f"INSERT INTO t VALUES ({i}, {at}, {ut}, {st}) ON DUPLICATE KEY UPDATE {assign}"
                want = model.odku(i, av, uv, sv, form, newv)
                if want is None:
                    continue
            elif kind == "update":
                m = rng.randint(1, 8)
                form = rng.choice(["a+k", "a=null", "u+1", "u=v", "s=v", "a=v"])
                if form == "a+k":
                    k = rng.randint(0, 3)
                    sql, arg = f"UPDATE t SET a = a + {k} WHERE id <= {m}", k
                elif form == "a=null":
                    sql, arg = f"UPDATE t SET a = NULL WHERE id <= {m}", None
                elif form == "u+1":
                    sql, arg = f"UPDATE t SET u = u + 1 WHERE id <= {m}", None
                elif form == "u=v":
                    t, arg = gen_u(rng)
                    sql = f"UPDATE t SET u = {t} WHERE id <= {m}"
                elif form == "s=v":
                    t, arg = gen_s(rng)
                    sql = f"UPDATE t SET s = {t} WHERE id <= {m}"
                else:
                    t, arg = gen_a(rng)
                    sql = f"UPDATE t SET a = {t} WHERE id <= {m}"
                want = model.update(form, arg, m)
            elif kind == "update_id":
                k, newid = ident(), rng.randint(9, 14) if rng.random() < 0.7 else ident()
                sql = f"UPDATE t SET id = {newid} WHERE id = {k}"
                want = model.update_id(k, newid)
            elif kind == "delete_t":
                m = rng.randint(0, 8)
                sql = f"DELETE FROM t WHERE id <= {m}"
                want = model.delete_t(m)
            elif kind == "insert_c":
                cid, tid = rng.randint(1, 12), rng.choice([None, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12])
                sql = f"INSERT INTO c VALUES ({cid}, {show(tid)})"
                want = model.insert_c(cid, tid)
            elif kind == "multi":
                which = rng.choice(["update", "delete", "merge"])
                lim = rng.choice([None, None, 2, 4, 6])
                if which == "update":
                    sets = rng.choice([["a"], ["u"], ["a", "u"]])
                    sql = "UPDATE t JOIN sr ON t.id = sr.id SET " + ", ".join(f"t.{c} = sr.{c}" for c in sets) + (f" WHERE sr.a > {lim}" if lim is not None else "")
                    want = model.multi_update(sets, lim)
                elif which == "delete":
                    sql = "DELETE t FROM t JOIN sr ON t.id = sr.id" + (f" WHERE sr.a > {lim}" if lim is not None else "")
                    want = model.multi_delete(lim)
                else:
                    sql = ("MERGE INTO t USING sr ON t.id = sr.id " + (f"WHEN MATCHED AND sr.a > {lim} THEN DELETE " if lim is not None else "") +
                           "WHEN MATCHED THEN UPDATE SET a = sr.a WHEN NOT MATCHED THEN INSERT (id, a, u) VALUES (sr.id, sr.a, sr.u)")
                    want = model.merge(lim)
                    if want is None:
                        continue
            elif kind == "update_c":
                cid, tid = rng.randint(1, 12), rng.choice([None, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12])
                sql = f"UPDATE c SET tid = {show(tid)} WHERE cid = {cid}"
                want = model.update_c(cid, tid)
            else:
                cid = rng.randint(1, 12)
                sql = f"DELETE FROM c WHERE cid = {cid}"
                want = model.delete_c(cid)
            out = db.execute(sql)
            got = out.startswith("OK")
            if got != want:
                print(f"STATEMENT {n}: {sql}\n  engine: {'ok' if got else 'error: ' + out.splitlines()[1] if len(out.splitlines()) > 1 else 'error'}   model: {'ok' if want else 'error'}")
                return 1
            stats["ok" if got else "failed"] += 1
            rows_t = read(db, "SELECT id, a, u, s FROM t ORDER BY id")
            rows_c = read(db, "SELECT cid, tid FROM c ORDER BY cid")
            if rows_t is None:
                rows_t = []
            if rows_c is None:
                rows_c = []
            exp_t = [(str(k), show(r[0]), show(r[1]), show(r[2])) for k, r in sorted(model.t.items())]
            exp_c = [(str(k), show(v)) for k, v in sorted(model.c.items())]
            if rows_t != exp_t or rows_c != exp_c:
                print(f"STATEMENT {n}: {sql}\n  engine t: {rows_t}\n  model  t: {exp_t}\n  engine c: {rows_c}\n  model  c: {exp_c}")
                return 1
    finally:
        proc.kill()
        proc.wait()
    print(f"statements={args.statements} succeeded={stats['ok']} rejected={stats['failed']} -- no violation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
