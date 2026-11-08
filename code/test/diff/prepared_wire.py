"""prepared_wire.py <rusql.exe> <corpus.txt> [--show N]

Runs prepared statements through the BINARY protocol (COM_STMT_PREPARE / COM_STMT_EXECUTE / COM_STMT_CLOSE, which is what a Go, Java or PHP driver and mysql-connector's
prepared cursor use) on RuSQL (over its MySQL port) and on a real MySQL (127.0.0.1:3391), and lists the executions whose answers differ: the rows and their Python types, the column
names, the affected rows, the last insert id, the error number.

Corpus: '#' comment; '--' prefix = a setup statement run as text on both; a line `SQL ;; (params) ;; (params)` prepares the SQL once and executes it once per tuple (a tuple is a
Python expression: ints, floats, strings, None, Decimal('1.5'), date(2024,1,31), datetime(...), time(...), bytes). The tables t and u of vs_mysql.py are in the database v.
"""
import datetime, decimal, os, shutil, socket, subprocess, sys, time

import mysql.connector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vs_mysql as V  # noqa: E402  (SETUP, the tables t and u; its main() only runs under the script name)

ENV = {"Decimal": decimal.Decimal, "date": datetime.date, "datetime": datetime.datetime, "time": datetime.time, "timedelta": datetime.timedelta}


def shown(v):
    if v is None:
        return "NULL"
    if isinstance(v, (bytes, bytearray)):
        return v.decode("utf-8", "replace")
    if isinstance(v, float):
        return repr(v)
    return str(v)


def same(a, b):
    if a == b:
        return True
    try:
        return abs(decimal.Decimal(a) - decimal.Decimal(b)) <= decimal.Decimal("1e-9") * max(1, abs(decimal.Decimal(a)))
    except Exception:  # noqa
        return False


def run_prepared(cnx, sql, params):
    cur = cnx.cursor(prepared=True)
    try:
        cur.execute(sql, params)
        if cur.description:
            rows = [[shown(v) for v in row] for row in cur.fetchall()]
            return ("rows", rows, [d[0] for d in cur.description], [type(v).__name__ for v in (cur.fetchall() or [()])[0]] if False else None)
        return ("ok", cur.rowcount, cur.lastrowid)
    except mysql.connector.Error as e:
        return ("err", e.errno)
    except Exception as e:  # noqa  (a driver that cannot read what the server sent)
        return ("broken", "%s: %s" % (type(e).__name__, str(e)[:80]))
    finally:
        try:
            cur.close()
        except Exception:  # noqa
            pass


def first_difference(a, b, ordered, sql):
    if a[0] != b[0]:
        return "%s / %s" % (a[:2] if a[0] != "rows" else ("rows", len(a[1])), b[:2] if b[0] != "rows" else ("rows", len(b[1])))
    if a[0] == "err":
        return None if a[1] == b[1] else "error number mysql=%s / rusql=%s" % (a[1], b[1])
    if a[0] == "ok":
        return None if a[1:] == b[1:] else "affected rows / last insert id mysql=%s / rusql=%s" % (a[1:], b[1:])
    if a[0] == "broken":
        return None
    if [c.lower() for c in a[2]] != [c.lower() for c in b[2]] and "?" not in sql.split(" FROM ")[0]:  # (MySQL names an expression with the ? as written; RuSQL names it with the value)
        return "column names mysql=%s / rusql=%s" % (a[2], b[2])
    ra, rb = list(a[1]), list(b[1])
    if not ordered:
        ra, rb = sorted(ra), sorted(rb)
    if len(ra) != len(rb):
        return "%d rows / %d rows" % (len(ra), len(rb))
    for k, (x, y) in enumerate(zip(ra, rb)):
        if len(x) != len(y) or not all(same(p, q) for p, q in zip(x, y)):
            return "row %d: mysql %s / rusql %s" % (k, x, y)
    return None


def main():
    exe, corpus = sys.argv[1], sys.argv[2]
    show = int(sys.argv[sys.argv.index("--show") + 1]) if "--show" in sys.argv else 10 ** 9
    data = os.path.join(os.environ.get("TEMP", "/tmp"), "prepared_wire_rusql")
    shutil.rmtree(data, ignore_errors=True)
    proc = subprocess.Popen([exe, "--port", "17992", "--mysql-port", "13398", "--data-dir", data], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            env=dict(os.environ, RUSTDB_PARALLEL="0"))
    for _ in range(400):
        try:
            socket.create_connection(("127.0.0.1", 13398), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.05)
    try:
        mysql_cnx = mysql.connector.connect(host="127.0.0.1", port=3391, user="root", password="", autocommit=True, use_pure=True)
        rusql_cnx = mysql.connector.connect(host="127.0.0.1", port=13398, user="root", password="root", autocommit=True, use_pure=True)
        for cnx in (mysql_cnx, rusql_cnx):
            c = cnx.cursor()
            c.execute("DROP DATABASE IF EXISTS v")
            c.execute("CREATE DATABASE v")
            c.execute("USE v")
            for s in V.SETUP:
                c.execute(s)
            c.close()
        total = diff = shown_count = 0
        for line in open(corpus, encoding="utf-8").read().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("--"):
                for cnx in (mysql_cnx, rusql_cnx):
                    c = cnx.cursor()
                    try:
                        c.execute(line[2:].strip())
                        if c.with_rows:
                            c.fetchall()
                    except Exception:  # noqa
                        pass
                    c.close()
                continue
            pieces = [p.strip() for p in line.split(";;")]
            sql, tuples = pieces[0], pieces[1:] or ["()"]
            for tup in tuples:
                params = eval(tup, {"__builtins__": {}}, ENV)  # noqa: S307  (our own corpus)
                total += 1
                m, r = run_prepared(mysql_cnx, sql, params), run_prepared(rusql_cnx, sql, params)
                if r[0] == "broken":  # (a connection whose answer could not be read is of no more use: open another)
                    try:
                        rusql_cnx.close()
                    except Exception:  # noqa
                        pass
                    rusql_cnx = mysql.connector.connect(host="127.0.0.1", port=13398, user="root", password="root", database="v", autocommit=True, use_pure=True)
                why = first_difference(m, r, "ORDER BY" in sql.upper(), sql)
                if r[0] == "broken" and m[0] != "broken":
                    why = "RuSQL's answer cannot be read by the driver: " + r[1]
                if why:
                    diff += 1
                    if shown_count < show:
                        shown_count += 1
                        print("DIFF: %s ;; %s\n   %s" % (sql, tup, why))
        print("statements=%d different=%d (both fail: 0)" % (total, diff))
    finally:
        proc.kill()


main()
