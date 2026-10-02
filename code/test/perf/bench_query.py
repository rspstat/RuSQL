"""RuSQL query-path benchmark: scans, aggregates, GROUP BY, DISTINCT, ORDER BY and joins on a table of 50,000 rows.

  python bench_query.py [rows]      (a server must be running on 7878, like for bench.py; result -> result_query.json)

Every query is timed 7 times, the result cache is invalidated before each run (a trivial UPDATE), and the median of the
last 6 runs is reported in milliseconds. Not part of the UI's benchmark panel (result.json): it is for comparing two
builds of the engine on the paths that read tables, not for a headline number. A query that takes over 40 s is reported
as "timeout" and the queries after it are skipped (an older build's nested-loop LEFT JOIN of 50,000 x 2,000 rows does, so
the slow joins are last in the list).
"""
import json
import statistics
import sys
import time
from pathlib import Path

import bench

HERE = Path(__file__).resolve().parent

QUERIES = [
    ("COUNT(*)", "SELECT COUNT(*) FROM t"),
    ("SUM / AVG", "SELECT SUM(val), AVG(val) FROM t"),
    ("GROUP BY (50 groups)", "SELECT grp, COUNT(*), SUM(val) FROM t GROUP BY grp"),
    ("WHERE val = 4242 (no index)", "SELECT id FROM t WHERE val = 4242"),
    ("WHERE val < 300 (no index)", "SELECT id, val FROM t WHERE val < 300"),
    ("WHERE code LIKE (no index)", "SELECT id FROM t WHERE code LIKE 'CODE4999%'"),
    ("ORDER BY val DESC LIMIT 10", "SELECT id, val FROM t ORDER BY val DESC LIMIT 10"),
    ("DISTINCT grp", "SELECT DISTINCT grp FROM t"),
    ("SELECT id, val (50,000 rows out)", "SELECT id, val FROM t"),
    ("join u, WHERE t.val < 50", "SELECT t.id, u.name FROM t JOIN u ON t.grp = u.id WHERE t.val < 50"),
    ("join u, no WHERE (50,000 rows out)", "SELECT t.id, u.name FROM t JOIN u ON t.grp = u.id"),
    ("join u + GROUP BY", "SELECT u.name, COUNT(*), SUM(t.val) FROM t JOIN u ON t.grp = u.id GROUP BY u.name"),
    ("LEFT JOIN u, WHERE t.val < 50", "SELECT t.id, u.name FROM t LEFT JOIN u ON t.grp = u.id WHERE t.val < 50"),
    ("join u ON two conditions", "SELECT t.id, u.name FROM t JOIN u ON t.grp = u.id AND t.val > 5000"),
    ("LEFT JOIN u ON two conditions", "SELECT t.id, u.name FROM t LEFT JOIN u ON t.grp = u.id AND t.val > 5000"),
]


def main(rows):
    db = bench.RuSQL()
    db.sock.settimeout(40)
    db.execute("CREATE DATABASE IF NOT EXISTS bench_query_db")
    db.execute("USE bench_query_db")
    db.execute("DROP TABLE IF EXISTS t")
    db.execute("DROP TABLE IF EXISTS u")
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, grp INT, val INT, code VARCHAR(20))")
    db.execute("CREATE TABLE u (id INT PRIMARY KEY, name VARCHAR(20))")
    for s in range(0, rows, 500):
        db.execute("INSERT INTO t VALUES " + ", ".join(f"({i}, {i % 50}, {(i * 7919) % 10007}, 'CODE{i}')" for i in range(s, min(s + 500, rows))))
    for s in range(0, 2000, 500):
        db.execute("INSERT INTO u VALUES " + ", ".join(f"({i}, 'N{i}')" for i in range(s, s + 500)))

    result = {"rows": rows, "median_ms": {}}
    for label, sql in QUERIES:
        times = []
        try:
            for _ in range(7):
                db.execute("UPDATE t SET val = val WHERE id = 0")  # invalidates the result cache
                t0 = time.perf_counter()
                db.execute(sql)
                times.append((time.perf_counter() - t0) * 1000)
        except TimeoutError:
            # the server is still busy with that statement and the connection is out of step: the rest is not run
            print(f"{label:38s} timeout (> 40 s) -- the remaining queries are skipped")
            for rest, _ in QUERIES[[q for q, _ in QUERIES].index(label):]:
                result["median_ms"][rest] = None
            break
        med = statistics.median(times[1:])
        result["median_ms"][label] = round(med, 2)
        print(f"{label:38s} {med:10.2f} ms")
    try:
        db.sock.settimeout(120)
        db.execute("DROP DATABASE bench_query_db")
        db.close()
    except OSError:
        pass
    with open(HERE / "result_query.json", "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
        f.write("\n")


if __name__ == "__main__":
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 50000)
