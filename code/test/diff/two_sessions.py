"""two_sessions.py <engine_server.exe> [--level RC|RR|both] [--only NAME] [--show]

Two sessions (A and B) run the same steps on RuSQL (MySQL protocol) and on a real MySQL (127.0.0.1:3391); every step's answer is compared: the rows of a SELECT, the rows
a DML statement affected, the number of an error. The steps are what a transaction sees of what another session commits, and what it sees of its own writes -- the
things a single-connection corpus (vs_mysql.py) cannot ask. Session A runs at the isolation level asked for; B is autocommit.

--show prints what MySQL answered at each step (to write the rows a test expects).

The levels are READ COMMITTED (RC) and REPEATABLE READ (RR). SERIALIZABLE is not compared: MySQL blocks a writer behind a reader there and this script runs the steps one
after the other. Not compared yet: nothing that waits for a lock.
"""
import os, shutil, socket, subprocess, sys, time
import pymysql

SETUP = [
    "CREATE TABLE t (id INT PRIMARY KEY, v INT, g INT, KEY (g))",
    "INSERT INTO t VALUES (1, 10, 1), (2, 20, 1), (3, 30, 2)",
    "CREATE TABLE c (a INT, b INT, v INT, PRIMARY KEY (a, b))",
    "INSERT INTO c VALUES (1, 1, 10), (1, 2, 20)",
    "CREATE TABLE p (id INT PRIMARY KEY)",
    "INSERT INTO p VALUES (1), (2)",
    "CREATE TABLE ch (id INT PRIMARY KEY, p INT, v INT, FOREIGN KEY (p) REFERENCES p(id) ON DELETE CASCADE ON UPDATE CASCADE)",
    "INSERT INTO ch VALUES (1, 1, 10), (2, 2, 20)",
]
CH = "SELECT * FROM ch ORDER BY id"

# scenarios whose differences are known and not fixed: name -> why. They are run and printed as "known", and do not count.
KNOWN = {
    "the_snapshot_is_taken_at_the_first_read": "MySQL takes the snapshot at the first read of the transaction, RuSQL at BEGIN (a change another session commits between the two is seen by MySQL)",
}
ALL = "SELECT * FROM t ORDER BY id"

# name -> steps; a step is (session, sql)
SCENARIOS = {
    "update_a_row_another_session_changed": [
        ("A", "BEGIN"), ("A", ALL), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", ALL), ("A", "UPDATE t SET v = v + 1 WHERE id = 1"),
        ("A", ALL), ("A", "SELECT COUNT(*) FROM t"), ("A", "SELECT * FROM t WHERE id = 1"), ("A", "COMMIT"), ("B", ALL),
    ],
    "delete_a_row_another_session_changed": [
        ("A", "BEGIN"), ("A", ALL), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", "DELETE FROM t WHERE id = 1"),
        ("A", ALL), ("A", "SELECT COUNT(*) FROM t"), ("A", "COMMIT"), ("B", ALL),
    ],
    "delete_then_insert_the_same_key": [
        ("A", "BEGIN"), ("A", ALL), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", "DELETE FROM t WHERE id = 1"),
        ("A", "INSERT INTO t VALUES (1, 99, 1)"), ("A", ALL), ("A", "SELECT COUNT(*) FROM t"), ("A", "COMMIT"), ("B", ALL),
    ],
    "savepoint_takes_the_write_back": [
        ("A", "BEGIN"), ("A", ALL), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", "SAVEPOINT s"), ("A", "UPDATE t SET v = v + 1 WHERE id = 1"),
        ("A", ALL), ("A", "ROLLBACK TO SAVEPOINT s"), ("A", ALL), ("A", "COMMIT"), ("B", ALL),
    ],
    "savepoint_after_the_first_write": [
        ("A", "BEGIN"), ("A", ALL), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", "SAVEPOINT s"),
        ("B", "UPDATE t SET v = v + 100 WHERE id = 2"), ("A", "UPDATE t SET v = v + 1 WHERE id = 2"), ("A", ALL), ("A", "ROLLBACK TO SAVEPOINT s"), ("A", ALL),
        ("A", "COMMIT"), ("B", ALL),
    ],
    "rollback_then_a_new_transaction": [
        ("A", "BEGIN"), ("A", ALL), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", "ROLLBACK"),
        ("A", "BEGIN"), ("A", ALL), ("A", "COMMIT"), ("B", ALL),
    ],
    "two_writes_to_the_same_row": [
        ("A", "BEGIN"), ("A", ALL), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", "UPDATE t SET v = v + 1 WHERE id = 1"),
        ("A", "UPDATE t SET v = v * 2 WHERE id = 1"), ("A", ALL), ("A", "SELECT COUNT(*) FROM t"), ("A", "COMMIT"), ("B", ALL),
    ],
    "the_key_changes": [
        ("A", "BEGIN"), ("A", ALL), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", "UPDATE t SET id = 10 WHERE id = 1"),
        ("A", ALL), ("A", "SELECT COUNT(*) FROM t"), ("A", "COMMIT"), ("B", ALL),
    ],
    "a_key_of_two_columns": [
        ("A", "BEGIN"), ("A", "SELECT * FROM c ORDER BY a, b"), ("B", "UPDATE c SET v = v + 1 WHERE a = 1 AND b = 1"),
        ("A", "UPDATE c SET v = v + 5 WHERE a = 1 AND b = 1"), ("A", "SELECT * FROM c ORDER BY a, b"), ("A", "SELECT COUNT(*) FROM c"), ("A", "COMMIT"),
        ("B", "SELECT * FROM c ORDER BY a, b"),
    ],
    "read_through_the_index": [
        ("A", "BEGIN"), ("A", "SELECT * FROM t WHERE g = 1 ORDER BY id"), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"),
        ("A", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", "SELECT * FROM t WHERE g = 1 ORDER BY id"), ("A", "SELECT * FROM t WHERE id = 1"),
        ("A", "SELECT COUNT(*) FROM t WHERE g = 1"), ("A", "COMMIT"),
    ],
    "a_row_the_other_session_inserted": [
        ("A", "BEGIN"), ("A", ALL), ("B", "INSERT INTO t VALUES (4, 40, 2)"), ("A", ALL), ("A", "UPDATE t SET v = 41 WHERE id = 4"), ("A", ALL),
        ("A", "SELECT COUNT(*) FROM t"), ("A", "COMMIT"), ("B", ALL),
    ],
    "a_row_the_other_session_deleted": [
        ("A", "BEGIN"), ("A", ALL), ("B", "DELETE FROM t WHERE id = 2"), ("A", ALL), ("A", "UPDATE t SET v = v + 1 WHERE id = 2"), ("A", ALL),
        ("A", "COMMIT"), ("B", ALL),
    ],
    "several_rows_changed_by_the_other": [
        ("A", "BEGIN"), ("A", ALL), ("B", "UPDATE t SET v = v + 1 WHERE g = 1"), ("A", "UPDATE t SET v = v + 10 WHERE g = 1"), ("A", ALL),
        ("A", "SELECT COUNT(*) FROM t"), ("A", "COMMIT"), ("B", ALL),
    ],
    "the_snapshot_is_taken_at_the_first_read": [
        ("A", "BEGIN"), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", ALL), ("B", "UPDATE t SET v = v + 1 WHERE id = 2"), ("A", ALL), ("A", "COMMIT"),
    ],
    "a_consistent_snapshot_is_taken_at_once": [
        ("A", "START TRANSACTION WITH CONSISTENT SNAPSHOT"), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", ALL), ("A", "COMMIT"),
    ],
    "a_cascade_deletes_a_child_the_other_session_changed": [
        ("A", "BEGIN"), ("A", CH), ("B", "UPDATE ch SET v = v + 1 WHERE id = 1"), ("A", "DELETE FROM p WHERE id = 1"), ("A", CH),
        ("A", "SELECT COUNT(*) FROM ch"), ("A", "COMMIT"), ("B", CH),
    ],
    "a_cascade_rewrites_a_child_the_other_session_changed": [
        ("A", "BEGIN"), ("A", CH), ("B", "UPDATE ch SET v = v + 1 WHERE id = 1"), ("A", "UPDATE p SET id = 10 WHERE id = 1"), ("A", CH),
        ("A", "SELECT COUNT(*) FROM ch"), ("A", "COMMIT"), ("B", CH),
    ],
    "locking_reads_see_the_latest": [
        ("A", "BEGIN"), ("A", ALL), ("B", "UPDATE t SET v = v + 1 WHERE id = 1"), ("A", "SELECT * FROM t WHERE id = 1 FOR UPDATE"), ("A", ALL),
        ("A", "SELECT * FROM t WHERE id = 1 LOCK IN SHARE MODE"), ("A", "COMMIT"),
    ],
}


def conn(port, password):
    c = pymysql.connect(host="127.0.0.1", port=port, user="root", password=password, autocommit=True, charset="utf8mb4")
    return c, c.cursor()


def step(cur, sql):
    try:
        cur.execute(sql)
        if cur.description:
            return ("rows", [[("NULL" if v is None else str(v)) for v in r] for r in cur.fetchall()])
        return ("ok", cur.rowcount if sql.split()[0].upper() in ("UPDATE", "DELETE", "INSERT") else None)
    except pymysql.err.MySQLError as e:
        return ("err", e.args[0] if e.args else 0)


def run(kind, port, password, name, level):
    a, ca = conn(port, password)
    b, cb = conn(port, password)
    out = []
    try:
        ca.execute("DROP DATABASE IF EXISTS ts")
        ca.execute("CREATE DATABASE ts")
        ca.execute("USE ts")
        cb.execute("USE ts")
        for sql in SETUP:
            ca.execute(sql)
        ca.execute("SET SESSION TRANSACTION ISOLATION LEVEL " + level)
        for who, sql in SCENARIOS[name]:
            out.append((who, sql, step(ca if who == "A" else cb, sql)))
    finally:
        a.close()
        b.close()
    return out


def main():
    exe = sys.argv[1]
    level = sys.argv[sys.argv.index("--level") + 1] if "--level" in sys.argv else "both"
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else ""
    levels = {"RC": ["READ COMMITTED"], "RR": ["REPEATABLE READ"], "both": ["READ COMMITTED", "REPEATABLE READ"]}[level]
    data = os.path.join(os.environ.get("TEMP", "/tmp"), "two_sessions_rusql")
    shutil.rmtree(data, ignore_errors=True)
    proc = subprocess.Popen([exe, "--port", "17996", "--mysql-port", "13396", "--data-dir", data], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    different = checked = known = 0
    try:
        for _ in range(300):
            try:
                socket.create_connection(("127.0.0.1", 13396), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        for name in SCENARIOS:
            if only and only not in name:
                continue
            for lv in levels:
                mysql = run("mysql", 3391, "", name, lv)
                rusql = run("rusql", 13396, "root", name, lv)
                for (who, sql, m), (_, _, r) in zip(mysql, rusql):
                    checked += 1
                    if "--show" in sys.argv:
                        print(f"[{lv}] {name}: {who}: {sql} -> {m}")
                    if m != r and name in KNOWN:
                        known += 1
                        print(f"known [{lv}] {name}: {who}: {sql}\n   mysql={m}\n   rusql={r}\n   ({KNOWN[name]})")
                    elif m != r:
                        different += 1
                        print(f"DIFF [{lv}] {name}: {who}: {sql}\n   mysql={m}\n   rusql={r}")
    finally:
        proc.kill()
    print(f"two_sessions: steps={checked} different={different} (known and not fixed: {known})")
    return 1 if different else 0


sys.exit(main())
