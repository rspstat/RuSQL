"""dump_to_mysql.py <rusql_server.exe> <schema.sql> [--show-ok]

The other way round of dump_roundtrip.py (R83): builds <schema.sql> on RuSQL (over its MySQL port), takes a real `mysqldump` of it with several sets of options, loads each dump into a real
MySQL (127.0.0.1:3391) and compares what came out: every table's rows, the views, the routines and triggers (they must be there and answer the same), the comments and the keys.
Prints what went wrong; exit code 1 when anything did.
"""
import os, shutil, socket, subprocess, sys, tempfile, time

import pymysql

BIN = r"C:\Program Files\MySQL\MySQL Server 8.4\bin"
MYSQL, MYSQLDUMP = os.path.join(BIN, "mysql.exe"), os.path.join(BIN, "mysqldump.exe")
REAL = dict(host="127.0.0.1", port=3391, user="root", password="")
SRC = "rt_src"

# the sets of options a person runs mysqldump with: (name, options, what the dump must hold)
OPTION_SETS = [
    ("default", [], {"data": True, "routines": False}),
    ("single-transaction + routines + triggers", ["--single-transaction", "--routines", "--triggers"], {"data": True, "routines": True}),
    ("skip-lock-tables, complete-insert, one row to an insert", ["--skip-lock-tables", "--complete-insert", "--skip-extended-insert", "--routines", "--triggers"], {"data": True, "routines": True}),
    ("no-data + routines + triggers", ["--no-data", "--routines", "--triggers"], {"data": False, "routines": True}),
    ("--databases with a CREATE DATABASE", ["--databases", "--routines", "--triggers", "--add-drop-database"], {"data": True, "routines": True}),
    ("compact, events, order-by-primary, insert-ignore, hex-blob", ["--compact", "--events", "--routines", "--triggers", "--order-by-primary", "--insert-ignore", "--hex-blob"], {"data": True, "routines": True}),
    ("opt: add-drop-table / add-drop-trigger, set-charset, quick", ["--add-drop-table", "--add-drop-trigger", "--set-charset", "--quick", "--triggers", "--routines", "--dump-date"], {"data": True, "routines": True}),
]


def split_statements(text):
    """The statements of a dump as the mysql client cuts them (DELIMITER lines, quotes, /*! */ comments kept)."""
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
            while j < n:
                if text[j] == "\\" and c != "`":
                    j += 2
                    continue
                if text[j] == c:
                    if j + 1 < n and text[j + 1] == c:  # a doubled quote continues the literal
                        j += 2
                        continue
                    break
                j += 1
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


def rows_of(cur, sql):
    cur.execute(sql)
    return sorted(tuple(norm(v) for v in r) for r in cur.fetchall())


def equal_rows(a, b):
    return len(a) == len(b) and all(len(x) == len(y) and all(same(p, q) for p, q in zip(x, y)) for x, y in zip(a, b))


def main():
    exe, schema = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    show_ok = "--show-ok" in sys.argv
    work = tempfile.mkdtemp(prefix="dump_to_mysql_")
    port, mport = 17991, 3494
    server = subprocess.Popen([exe, "--port", str(port), "--mysql-port", str(mport), "--data-dir", os.path.join(work, "data")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    bad = 0
    routines_run = False
    try:
        if not wait_port(mport):
            raise SystemExit("the RuSQL server did not start")
        rus = pymysql.connect(host="127.0.0.1", port=mport, user="root", password="root", autocommit=True, charset="utf8mb4")
        rcur = rus.cursor()
        rcur.execute(f"CREATE DATABASE {SRC}")
        rcur.execute(f"USE {SRC}")
        refused = 0
        for st in split_statements(open(schema, encoding="utf-8").read()):
            try:
                rcur.execute(st)
            except Exception as e:  # noqa
                refused += 1
                print("SCHEMA REFUSED:", " ".join(st.split())[:120], "->", str(e)[:120])
        bad += refused
        rcur.execute("SHOW TABLES")
        tables = [r[0] for r in rcur.fetchall()]
        print(f"schema loaded into RuSQL ({len(tables)} tables, {refused} refused)")

        for name, options, expect in OPTION_SETS:
            print(f"\n== {name}")
            dump = os.path.join(work, "dump.sql")
            cmd = [MYSQLDUMP, "-h127.0.0.1", f"-P{mport}", "-uroot", "-proot", "--default-character-set=utf8mb4"] + options + ([SRC] if "--databases" not in options else [SRC])
            r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            if r.returncode != 0 or not r.stdout:
                print(f"   mysqldump FAILED (exit {r.returncode}): {r.stderr.strip()[:1200]}")
                bad += 1
                continue
            for line in r.stderr.splitlines():
                if line.strip():
                    print("   mysqldump says:", line.strip()[:200])
            open(dump, "w", encoding="utf-8", newline="").write(r.stdout)
            mdb = SRC if "--databases" in options else "rt_copy"
            mcur = pymysql.connect(autocommit=True, charset="utf8mb4", **REAL).cursor()
            mcur.execute(f"DROP DATABASE IF EXISTS {SRC}")
            mcur.execute("DROP DATABASE IF EXISTS rt_copy")
            if mdb != SRC:
                mcur.execute("CREATE DATABASE rt_copy DEFAULT CHARACTER SET utf8mb4")
            mcur.execute("SET time_zone = '+00:00'")
            mcur.execute("SET sql_mode = ''")  # (the dump is the user's own: a mode that refuses 0000-00-00 would refuse a faithful dump too)
            statements = split_statements(r.stdout)
            if mdb != SRC:
                mcur.execute(f"USE {mdb}")
            errors = 0
            for st in statements:
                try:
                    mcur.execute(st)
                    if show_ok:
                        print("   ok  ", " ".join(st.split())[:110])
                except Exception as e:  # noqa
                    errors += 1
                    print("   MYSQL REFUSED:", " ".join(st.split())[:150], "\n           ->", str(e)[:140])
            print(f"   {len(statements)} statements, MySQL refused {errors}")
            bad += errors

            for t in tables:
                if expect["data"]:
                    want = rows_of(rcur, f"SELECT * FROM `{SRC}`.`{t}`")
                    try:
                        got = rows_of(mcur, f"SELECT * FROM `{mdb}`.`{t}`")
                    except Exception as e:  # noqa
                        print(f"   TABLE {t}: cannot be read on MySQL ({str(e)[:100]})")
                        bad += 1
                        continue
                    if not equal_rows(want, got):
                        print(f"   TABLE {t}: rows differ\n      rusql: {want[:3]}\n      mysql: {got[:3]}")
                        bad += 1
                    elif show_ok:
                        print(f"   TABLE {t}: {len(want)} rows equal")
                else:
                    got = rows_of(mcur, f"SELECT * FROM `{mdb}`.`{t}`")
                    if got:
                        print(f"   TABLE {t}: --no-data but MySQL holds {len(got)} rows")
                        bad += 1
            # the structure: comments, keys, views
            checks = [
                ("table comments", "SELECT table_name, table_comment FROM information_schema.tables WHERE table_schema = '{db}' AND table_type = 'BASE TABLE' ORDER BY table_name"),
                ("column comments", "SELECT table_name, column_name, column_comment FROM information_schema.columns WHERE table_schema = '{db}' ORDER BY table_name, ordinal_position"),
                ("unique and primary keys", "SELECT table_name, constraint_name, constraint_type FROM information_schema.table_constraints WHERE table_schema = '{db}' ORDER BY table_name, constraint_name"),
                ("views", "SELECT table_name FROM information_schema.views WHERE table_schema = '{db}' ORDER BY table_name"),
            ]
            for label, sql in checks:
                want = rows_of(rcur, sql.format(db=SRC))
                got = rows_of(mcur, sql.format(db=mdb))
                if not equal_rows(want, got):
                    only_rusql = [r for r in want if r not in got]
                    only_mysql = [r for r in got if r not in want]
                    print(f"   {label} differ\n      only in RuSQL: {only_rusql[:6]}\n      only in MySQL: {only_mysql[:6]}")
                    bad += 1
            if expect["routines"]:
                for label, sql in [
                    ("routines", "SELECT routine_name, routine_type FROM information_schema.routines WHERE routine_schema = '{db}' ORDER BY routine_name"),
                    ("triggers", "SELECT trigger_name, event_manipulation, event_object_table, action_timing FROM information_schema.triggers WHERE trigger_schema = '{db}' ORDER BY trigger_name"),
                ]:
                    want = rows_of(rcur, sql.format(db=SRC))
                    got = rows_of(mcur, sql.format(db=mdb))
                    if not equal_rows(want, got) or not want:
                        print(f"   {label} differ\n      rusql: {want}\n      mysql: {got}")
                        bad += 1
                    else:
                        print(f"   {label}: {len(want)} on both")
                # the routines and triggers answer the same on both (checked once: they change the rows)
                if expect["data"] and not routines_run:
                    routines_run = True
                    mcur.execute(f"USE {mdb}")
                    for call in ["SELECT fn_discount(100.00)", "SELECT fn_label('x')", "SELECT fn_discount(fn_discount(50))"]:
                        a, b = rows_of(rcur, call), rows_of(mcur, call)
                        if not equal_rows(a, b):
                            print(f"   {call}: rusql {a} / mysql {b}")
                            bad += 1
                    for stmt in ["CALL sp_bump(1, 5.00)", "CALL sp_note(2)", "INSERT INTO customers (name) VALUES ('  padded  ')", "INSERT INTO orders (customer_id, total) VALUES (1, 1.00)"]:
                        mcur.execute(stmt)
                        rcur.execute(stmt)
                    for sql in ["SELECT name, balance, note FROM customers", "SELECT msg FROM audit"]:
                        a, b = rows_of(rcur, sql), rows_of(mcur, sql)
                        if not equal_rows(a, b):
                            print(f"   after the routines and triggers ran, `{sql}` differs\n      rusql: {a[:5]}\n      mysql: {b[:5]}")
                            bad += 1
                        else:
                            print(f"   after the routines and triggers ran: {sql} is the same ({len(a)} rows)")
                    rcur.execute(f"USE {SRC}")
            mcur.execute(f"DROP DATABASE IF EXISTS {mdb}")
            print("   done" if not bad else f"   (problems so far: {bad})")
    finally:
        server.kill()
        shutil.rmtree(work, ignore_errors=True)
    print("\nTOTAL PROBLEMS:", bad)
    sys.exit(1 if bad else 0)


main()
