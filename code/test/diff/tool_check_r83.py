"""tool_check_r83.py <rusql_server.exe> [corpus/tool_compat_r83.sql]

The statements of corpus/tool_compat_r83.sql that vs_mysql.py cannot compare because a column of the answer is the time the routine was made (SHOW TRIGGERS,
SHOW PROCEDURE / FUNCTION STATUS, SHOW CREATE TRIGGER): run on RuSQL and on a real MySQL (127.0.0.1:3391) with the same set-up, compared without those columns.
Prints what differs; exit code 1 when anything does.
"""
import os, shutil, socket, subprocess, sys, tempfile, time

import pymysql

HERE = os.path.dirname(os.path.abspath(__file__))
# (statement, the columns that hold a time and are left out)
CHECKS = [
    ("SHOW TRIGGERS", [5]),
    ("SHOW TRIGGERS FROM v", [5]),
    ("SHOW TRIGGERS LIKE 'par'", [5]),
    ("SHOW TRIGGERS LIKE 'ch%'", [5]),
    ("SHOW TRIGGERS WHERE `Table` = 'chi'", [5]),
    ("SHOW TRIGGERS WHERE Timing = 'AFTER'", [5]),
    ("SHOW PROCEDURE STATUS WHERE Db = 'v'", [5, 6]),
    ("SHOW FUNCTION STATUS WHERE Db = 'v'", [5, 6]),
    ("SHOW FUNCTION STATUS LIKE 'fn_t%'", [5, 6]),
    ("SHOW PROCEDURE STATUS LIKE 'sp_one'", [5, 6]),
    ("SHOW CREATE TRIGGER tr_par", [6]),
    ("SHOW CREATE TRIGGER tr_chi", [6]),
    ("SHOW CREATE TRIGGER `tr_par`", [6]),
    ("SHOW CREATE TRIGGER nope", []),
    ("SHOW TABLE STATUS LIKE 'vw'", [11]),  # (the Create_time of a view)
    ("SHOW TABLE STATUS WHERE Comment = 'VIEW'", [11]),
    ("SHOW TABLE STATUS WHERE Name IN ('vw', 'vw_agg')", [11]),
    ("SHOW CREATE TABLE vw", [1]),  # (a view: the text of MySQL is its normalised select, ours the one typed)
]


def wait_port(port, secs=20):
    for _ in range(int(secs / 0.1)):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def run(cur, sql, drop):
    try:
        cur.execute(sql)
    except Exception as e:  # noqa
        return ("err", e.args[0] if e.args else str(e), str(e)[:100])
    if cur.description is None:
        return ("ok",)
    names = [d[0] for d in cur.description]
    rows = sorted(tuple("NULL" if v is None else (v.decode("utf-8", "replace") if isinstance(v, bytes) else str(v)) for i, v in enumerate(r) if i not in drop) for r in cur.fetchall())
    return ("rows", names, rows)  # (the header is the whole one: a column that is missing is a difference too)


def main():
    exe = os.path.abspath(sys.argv[1])
    corpus = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else os.path.join(HERE, "corpus", "tool_compat_r83.sql")
    setup = [l.strip()[2:].strip() for l in open(corpus, encoding="utf-8") if l.startswith("--")]
    work = tempfile.mkdtemp(prefix="tool_check_r83_")
    server = subprocess.Popen([exe, "--port", "17992", "--mysql-port", "13398", "--data-dir", os.path.join(work, "data")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              env=dict(os.environ, RUSTDB_PARALLEL="0"))
    bad = 0
    try:
        if not wait_port(13398):
            raise SystemExit("the RuSQL server did not start")
        mysql = pymysql.connect(host="127.0.0.1", port=3391, user="root", password="", autocommit=True, charset="utf8mb4").cursor()
        rusql = pymysql.connect(host="127.0.0.1", port=13398, user="root", password="root", autocommit=True, charset="utf8mb4").cursor()
        for cur in (mysql, rusql):
            cur.execute("DROP DATABASE IF EXISTS v")
            cur.execute("CREATE DATABASE v")
            cur.execute("USE v")
        for stmt in setup:
            for cur in (mysql, rusql):
                try:
                    cur.execute(stmt)
                except Exception as e:  # noqa
                    print("SETUP FAILED on", "mysql" if cur is mysql else "rusql", ":", stmt[:100], "->", str(e)[:100])
                    bad += 1
        for stmt, drop in CHECKS:
            m, r = run(mysql, stmt, drop), run(rusql, stmt, drop)
            if m[0] == "err" and r[0] == "err":
                if m[1] != r[1]:
                    print(f"DIFF (error number): {stmt}\n   mysql {m[1:]}\n   rusql {r[1:]}")
                    bad += 1
                else:
                    print(f"same error {m[1]}: {stmt}")
                continue
            if m != r:
                print(f"DIFF: {stmt}")
                if m[0] == "rows" and r[0] == "rows":
                    if m[1] != r[1]:
                        print(f"   columns mysql {m[1]}\n           rusql {r[1]}")
                    for i in range(max(len(m[2]), len(r[2]))):
                        a = m[2][i] if i < len(m[2]) else None
                        b = r[2][i] if i < len(r[2]) else None
                        if a != b:
                            print(f"   row {i}: mysql {a}\n          rusql {b}")
                            break
                else:
                    print(f"   mysql {m[:2]}\n   rusql {r[:2]}")
                bad += 1
            else:
                print(f"same: {stmt}" + (f"  ({len(m[2])} rows)" if m[0] == "rows" else ""))
    finally:
        server.kill()
        shutil.rmtree(work, ignore_errors=True)
    print("DIFFERENCES:", bad)
    sys.exit(1 if bad else 0)


main()
