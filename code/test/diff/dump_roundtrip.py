"""dump_roundtrip.py <rusql_server.exe> <schema.sql> [--show-ok]

Builds <schema.sql> on a real MySQL (127.0.0.1:3391, database rt_dump), takes a real `mysqldump` of it, loads that dump into RuSQL
(over the MySQL protocol, statement by statement as the mysql client does: DELIMITER handled, comments dropped, /*!NNNNN ... */ kept)
and compares every table's rows. Then loads the same dump with the real `mysql` client (--force) and counts its errors.
Prints each statement RuSQL refused, with its answer; exit code 1 when anything is refused or differs.
"""
import os, re, shutil, socket, subprocess, sys, tempfile, time

import pymysql

BIN = r"C:\Program Files\MySQL\MySQL Server 8.4\bin"
MYSQL, MYSQLDUMP = os.path.join(BIN, "mysql.exe"), os.path.join(BIN, "mysqldump.exe")
REAL = dict(host="127.0.0.1", port=3391, user="root", password="")
DB = "rt_dump"


def split_statements(text):
    """The statements of a dump as the mysql client cuts them."""
    out, cur, delim, i, n = [], [], ";", 0, len(text)
    at_line_start = True
    while i < n:
        c = text[i]
        if at_line_start and text[i:i + 9].upper() == "DELIMITER" and text[i + 9:i + 10] in " \t":
            j = text.find("\n", i)
            j = n if j < 0 else j
            delim = text[i + 9:j].strip()
            i = j + 1
            continue
        at_line_start = c == "\n"
        if text.startswith("-- ", i) or text.startswith("--\n", i) or c == "#":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i) and not text.startswith("/*!", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if text.startswith("/*!", i):
            j = text.find("*/", i + 3)
            j = n if j < 0 else j + 2
            cur.append(text[i:j])
            i = j
            continue
        if c in "'\"`":
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\" and c != "`":
                    j += 1
                elif text[j] == c:
                    pass
                j += 1
            # a doubled quote ('') continues the literal
            while j + 1 < n and text[j + 1] == c:
                j += 2
                while j < n and text[j] != c:
                    j += 2 if text[j] == "\\" and c != "`" else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if text.startswith(delim, i):
            stmt = "".join(cur).strip()
            if stmt:
                out.append(stmt)
            cur = []
            i += len(delim)
            continue
        cur.append(c)
        i += 1
    tail = "".join(cur).strip()
    if tail:
        out.append(tail)
    return out


def wait_port(port, secs=20):
    for _ in range(int(secs / 0.1)):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def norm(v):
    if v is None:
        return "NULL"
    if isinstance(v, (bytes, bytearray)):
        return v.decode("utf-8", "replace")
    return str(v)


def num(x):
    try:
        return float(x)
    except ValueError:
        return None


def same(a, b):
    if a == b:
        return True
    na, nb = num(a), num(b)
    return na is not None and nb is not None and abs(na - nb) <= 1e-9 * max(1.0, abs(na))


def main():
    exe, schema = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    work = tempfile.mkdtemp(prefix="dump_rt_")
    real = [MYSQL, "-h127.0.0.1", "-P3391", "-uroot", "--default-character-set=utf8mb4"]
    subprocess.run(real + ["-e", f"DROP DATABASE IF EXISTS {DB}; CREATE DATABASE {DB} DEFAULT CHARACTER SET utf8mb4"], check=True)
    with open(schema, encoding="utf-8") as f:
        subprocess.run(real + [DB], stdin=f, check=True)
    dump = os.path.join(work, "dump.sql")
    with open(dump, "wb") as f:
        subprocess.run([MYSQLDUMP, "-h127.0.0.1", "-P3391", "-uroot", "--routines", "--triggers", "--default-character-set=utf8mb4", DB], stdout=f, check=True)
    text = open(dump, encoding="utf-8").read()

    port, mport = 17991, 3493
    server = subprocess.Popen([exe, "--port", str(port), "--mysql-port", str(mport), "--data-dir", os.path.join(work, "data")],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    bad = 0
    try:
        if not wait_port(mport):
            raise SystemExit("the RuSQL server did not start")
        conn = pymysql.connect(host="127.0.0.1", port=mport, user="root", password="root", autocommit=True, charset="utf8mb4")
        cur = conn.cursor()
        cur.execute(f"CREATE DATABASE {DB}")
        cur.execute(f"USE {DB}")
        statements = split_statements(text)
        refused = 0
        for st in statements:
            try:
                cur.execute(st)
                if "--show-ok" in sys.argv:
                    print("ok  ", " ".join(st.split())[:110])
            except Exception as e:  # noqa
                refused += 1
                print("REFUSED:", " ".join(st.split())[:160], "\n        ->", str(e)[:140])
        print(f"{len(statements)} statements, {refused} refused")
        bad += refused

        rc = pymysql.connect(autocommit=True, charset="utf8mb4", database=DB, **REAL).cursor()
        rc.execute("SET time_zone = '+00:00'")  # (the dump is in UTC; RuSQL keeps a TIMESTAMP as the text it was given, without a time zone)
        rc.execute("SHOW TABLES")
        tables = [r[0] for r in rc.fetchall()]
        for t in tables:
            rc.execute(f"SELECT * FROM `{t}`")
            want = sorted(tuple(norm(v) for v in r) for r in rc.fetchall())
            try:
                cur.execute(f"SELECT * FROM `{t}`")
                got = sorted(tuple(norm(v) for v in r) for r in cur.fetchall())
            except Exception as e:  # noqa
                print(f"TABLE {t}: cannot be read ({str(e)[:100]})")
                bad += 1
                continue
            if len(want) != len(got) or not all(len(a) == len(b) and all(same(x, y) for x, y in zip(a, b)) for a, b in zip(want, got)):
                print(f"TABLE {t}: rows differ\n   mysql: {want[:3]}\n   rusql: {got[:3]}")
                bad += 1
            else:
                print(f"TABLE {t}: {len(want)} rows equal")

        cur.execute(f"CREATE DATABASE {DB}_cli")
        with open(dump, encoding="utf-8") as f:
            r = subprocess.run([MYSQL, "-h127.0.0.1", f"-P{mport}", "-uroot", "-proot", "--force", "--default-character-set=utf8mb4", f"{DB}_cli"],
                               stdin=f, capture_output=True, text=True, encoding="utf-8", errors="replace")
        errors = [l for l in r.stderr.splitlines() if l.startswith("ERROR")]
        print(f"the mysql client: {len(errors)} errors")
        for l in errors[:10]:
            print("   ", l)
        bad += len(errors)
    finally:
        server.kill()
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if bad else 0)


main()
