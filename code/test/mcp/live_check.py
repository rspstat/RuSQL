"""The MCP tools of R70 and the engine's connection limit, against a REAL engine_server (not a fake): py -3 code/test/mcp/live_check.py

It starts engine_server.exe (the Release build) on free ports with a data folder under the temp folder, fills a small database through the MCP module itself, and goes through
suggest_indexes (its answer is the engine's SUGGEST INDEX, also for subqueries, a CTE and an OR; verify on an UPDATE runs nothing), explain_query on a write, slow_queries, kill_session
(a real KILL of a running statement), import_csv / export_csv, diff_schema, the resources, and max_connections (a third connection is refused with 1040 on both ports). It ends the
server again. The fake-engine unit tests (test_*.py) cover the decisions; this covers that the engine answers what the tools ask.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / "build" / "backend" / "server" / "Release" / "engine_server.exe"


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


PORT, MYSQL_PORT = free_port(), free_port()
os.environ.update(RUSQL_HOST="127.0.0.1", RUSQL_PORT=str(PORT), RUSQL_USER="root", RUSQL_PASS="root")
sys.path.insert(0, str(ROOT / "mcp"))
import mcp_server as m  # noqa: E402

CHECKS: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = ""):
    CHECKS.append((name, bool(ok), detail))
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f"\n     {detail[:400]}"))


def run(sql: str, db: str = "shop") -> str:
    return m._run(sql, db)


def main() -> int:
    data = Path(tempfile.mkdtemp(prefix="rusql_live_"))
    proc = subprocess.Popen([str(SERVER), "--port", str(PORT), "--mysql-port", str(MYSQL_PORT), "--data-dir", str(data)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.3).close()
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise RuntimeError("the server did not start")
        time.sleep(0.5)

        # ── a small database ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
        run("CREATE DATABASE shop", "")
        run("CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(20), city VARCHAR(20))")
        run("CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, status VARCHAR(10), total INT)")
        run("INSERT INTO customers VALUES " + ",".join(f"({i},'c{i}','{('Seoul', 'Busan', 'Daegu')[i % 3]}')" for i in range(1, 301)))
        run("INSERT INTO orders VALUES " + ",".join(f"({i},{i % 300 + 1},'{('new', 'paid', 'sent')[i % 3]}',{i % 97})" for i in range(1, 1501)))

        # ── suggest_indexes: the engine's reading ────────────────────────────────────────────────────────────────────────────────────────────────
        r = m.suggest_indexes_for("SELECT * FROM orders WHERE customer_id = 7", "shop")
        check("suggest_indexes: an equality search", [s["columns"] for s in r.get("suggestions", [])] == [["customer_id"]], json.dumps(r)[:300])
        r = m.suggest_indexes_for("SELECT * FROM customers WHERE id IN (SELECT customer_id FROM orders WHERE total = 5)", "shop")
        check("suggest_indexes: a subquery is read on its own", any(s.get("in") == "an IN subquery" and s["table"] == "orders" for s in r.get("suggestions", [])), json.dumps(r)[:300])
        r = m.suggest_indexes_for("WITH big AS (SELECT * FROM orders WHERE total = 5) SELECT * FROM big", "shop")
        check("suggest_indexes: a CTE is read where it is defined", any(s.get("in") == "the CTE big" for s in r.get("suggestions", [])), json.dumps(r)[:300])
        r = m.suggest_indexes_for("SELECT * FROM orders WHERE customer_id = 7 OR total = 5", "shop")
        check("suggest_indexes: an OR is read branch by branch, with the rewrite", len([s for s in r.get("suggestions", []) if s.get("needs_rewrite")]) == 2 and "rewrites" in r, json.dumps(r)[:400])
        r = m.suggest_indexes_for("UPDATE orders SET total = 0 WHERE customer_id = 7", "shop", verify=True)
        trial = (r.get("verification") or {}).get("each_index_alone", [{}])[0]
        check("suggest_indexes: an UPDATE is tried by estimate and not run", "estimated_cost" in trial and trial.get("verdict") in ("cheaper (estimated)", "uses the index"), json.dumps(r)[:500])
        check("... the table was not changed by the trial", m._rows("SELECT COUNT(*) AS n FROM orders WHERE customer_id = 7 AND total = 0", "shop")[0]["n"] == "0")
        check("... and no trial index is left", "__try_" not in run("SHOW INDEX FROM orders"), run("SHOW INDEX FROM orders")[:300])
        r = m.suggest_indexes_for("SELECT * FROM orders WHERE customer_id = 7", "shop", verify=True)
        check("suggest_indexes: a SELECT is timed before and after", "speedup" in (r["verification"]["each_index_alone"][0]), json.dumps(r["verification"])[:300])

        # ── explain_query never runs a write ─────────────────────────────────────────────────────────────────────────────────────────────────────
        def orders_count() -> str:
            return m._rows("SELECT COUNT(*) AS n FROM orders", "shop")[0]["n"]  # (the answer text carries a timing that differs between two runs)

        before = orders_count()
        out = m.explain_query("DELETE FROM orders WHERE status = 'sent'", "shop")
        check("explain_query: a DELETE is planned, not run", "Statement" in out and "DELETE" in out and orders_count() == before, out[:300])
        check("explain_query: EXPLAIN ANALYZE DELETE is still only planned", "Statement" in m.explain_query("EXPLAIN ANALYZE DELETE FROM orders WHERE status = 'sent'", "shop") and orders_count() == before)

        # ── slow_queries ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
        s = json.loads(m.slow_queries())
        check("slow_queries: the log is off and the answer says how to turn it on", s["slow_query_log"] == "OFF" and "SET GLOBAL slow_query_log = ON" in s["next"], json.dumps(s)[:300])
        run("SET GLOBAL slow_query_log = ON", "")
        run("SET GLOBAL long_query_time = 0", "")
        for _ in range(2):
            run("SELECT COUNT(*) FROM orders a, orders b WHERE a.total + b.total > 190")
        s = json.loads(m.slow_queries())
        check("slow_queries: the slow statement is listed, grouped", s["slow_query_log"] == "ON" and any("orders a, orders b" in g["sql"] and g["runs"] == 2 for g in s["statements"]), json.dumps(s)[:500])
        check("health_report: the slow queries are counted", json.loads(m.health_report())["server"]["slow_queries_total"] >= 2)

        # ── kill_session: a real statement is stopped ────────────────────────────────────────────────────────────────────────────────────────────
        box = {}

        def long_query():
            t0 = time.time()
            box["out"] = run("SELECT COUNT(*) FROM orders a, orders b, orders c WHERE a.total + b.total + c.total > 1000")
            box["t"] = time.time() - t0

        th = threading.Thread(target=long_query)
        th.start()
        time.sleep(1.0)
        sessions = m._rows("SELECT ID, INFO FROM information_schema.processlist")
        target = next((x["ID"] for x in sessions if "orders a, orders b, orders c" in (x.get("INFO") or "")), None)
        check("kill_session: the running statement is found in the processlist", target is not None, str(sessions)[:300])
        if target:
            refused = m.kill_session(int(target))
            check("kill_session: the first call only describes and refuses", refused.startswith("Refused") and th.is_alive(), refused[:200])
            done = json.loads(m.kill_session(int(target), confirmed=True))
            th.join(30)
            check("kill_session: confirmed, the statement stops", done["killed"] == "query" and not th.is_alive() and "1317" in box.get("out", "") + str(box), str(box)[:300])
        th.join(60)
        run("SET GLOBAL slow_query_log = OFF", "")

        # ── import_csv / export_csv ──────────────────────────────────────────────────────────────────────────────────────────────────────────────
        run("CREATE TABLE notes (id INT PRIMARY KEY, body VARCHAR(40))")
        f = data / "notes.csv"
        f.write_text("id,body\n1,hello\n2,\"it's, a \"\"quote\"\"\"\n3,\n", encoding="utf-8")
        preview = json.loads(m.import_csv("notes", str(f), "shop"))
        count = lambda: m._rows("SELECT COUNT(*) AS n FROM notes", "shop")[0]["n"]
        check("import_csv: the preview inserts and rolls back", preview["mode"] == "preview" and count() == "0", json.dumps(preview)[:300])
        saved = json.loads(m.import_csv("notes", str(f), "shop", commit=True))
        check("import_csv: commit saves the rows", saved["committed"] is True and count() == "3", json.dumps(saved)[:300])
        check("import_csv: quotes, commas and an empty cell (NULL) arrive as written", "it's, a \"quote\"" in run("SELECT body FROM notes WHERE id = 2") and "NULL" in run("SELECT body FROM notes WHERE id = 3"), run("SELECT * FROM notes")[:300])
        out_file = data / "out.csv"
        exported = json.loads(m.export_csv("SELECT id, body FROM notes ORDER BY id", str(out_file), "shop"))
        check("export_csv: the file holds the rows", exported["rows"] == 3 and out_file.read_text(encoding="utf-8-sig").splitlines()[0] == "id,body", out_file.read_text(encoding="utf-8-sig")[:200])
        bad = json.loads(m.import_csv("notes", str(f), "shop", commit=True))
        check("import_csv: a duplicate key fails and nothing is loaded", bad["committed"] is False and "error" in bad, json.dumps(bad)[:300])

        # ── diff_schema ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
        run("CREATE DATABASE shop2", "")
        run("CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(40))", "shop2")
        d = json.loads(m.diff_schema("shop", "shop2"))
        check("diff_schema: a table only one has, a column that differs", "orders" in d["tables_only_in_shop"] and "customers" in d["tables_that_differ"], json.dumps(d)[:500])
        check("diff_schema: the same database is identical", json.loads(m.diff_schema("shop", "shop"))["identical"] is True)

        # ── resources ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
        schema = json.loads(m.schema_resource("shop"))
        check("resource: the schema of a database", {"customers", "orders", "notes"} <= set(schema["tables"]), json.dumps(schema)[:300])

        # ── max_connections ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
        check("max_connections: 151 at the start", "151" in run("SELECT @@max_connections", ""), run("SELECT @@max_connections", ""))
        run("SET GLOBAL max_connections = 2", "")
        held = []
        try:
            c1 = m._Conn()   # this and the next one are the two places (the statements above used and gave back theirs)
            held.append(c1)
            c2 = m._Conn()
            held.append(c2)
            refused = False
            try:
                c3 = socket.create_connection(("127.0.0.1", PORT), timeout=5)
                c3.settimeout(5)
                first = c3.recv(4096).decode(errors="replace")
                refused = "Too many connections" in first
                c3.close()
            except OSError as e:
                first = str(e)
            check("max_connections: a third native connection is refused", refused, first[:200])
            mysql = shutil.which("mysql") or r"C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe"
            if Path(mysql).exists():
                r = subprocess.run([mysql, "-h127.0.0.1", f"-P{MYSQL_PORT}", "-uroot", "--protocol=TCP", "-e", "SELECT 1"], capture_output=True, text=True, env=dict(os.environ, MYSQL_PWD="root"), timeout=30)
                check("max_connections: the mysql client is told 1040", "1040" in r.stderr and "Too many connections" in r.stderr, r.stderr[:300])
        finally:
            for c in held:
                c.close()
            time.sleep(0.5)
        check("max_connections: places are given back when connections end", "ERR" not in m._run("SELECT 1 AS one", "")[:4])  # (a new connection gets in again)
        status = run("SHOW GLOBAL STATUS LIKE 'Connection_errors_max_connections'", "")
        refused_count = [int(x) for x in status.replace("|", " ").split() if x.isdigit()]
        check("max_connections: the refusals are counted", refused_count and refused_count[0] >= 1, status[:200])
        run("SET GLOBAL max_connections = 151", "")
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(data, ignore_errors=True)
    failed = [c for c in CHECKS if not c[1]]
    print(f"{len(CHECKS) - len(failed)} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
