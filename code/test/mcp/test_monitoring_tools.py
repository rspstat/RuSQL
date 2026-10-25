"""Unit tests of the MCP tools list_locks, health_report, get_table_stats, get_fk_graph and trace_query against a fake engine (no server needed):
py -3 -m unittest code/test/mcp/test_monitoring_tools.py

The fake answers what the tools ask the engine for in the engine's own output format (box-drawn tables), so the reading and the decisions are tested, not the engine.
"""
import json
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "mcp"))
import mcp_server  # noqa: E402


def box(headers, rows):
    if not rows:
        return "OK\n0 rows returned.\n(0.001 sec)"
    widths = [max(len(str(x)) for x in [h] + [r[i] for r in rows]) for i, h in enumerate(headers)]
    bar = "+" + "+".join("-" * (w + 2) for w in widths) + "+"

    def line(cells):
        return "| " + " | ".join(str(c).ljust(w) for c, w in zip(cells, widths)) + " |"
    return "\n".join(["OK", bar, line(headers), bar] + [line(r) for r in rows] + [bar, f"{len(rows)} row(s) returned.", "(0.001 sec)"])


class FakeEngine:
    """Canned answers; `state` is changed by a test to make the situation it wants."""

    def __init__(self):
        self.statements = []
        self.state = {
            "status": {"Uptime": "100", "Questions": "500", "Connections": "7", "Threads_connected": "2", "Max_used_connections": "4", "Threads_running": "1",
                       "Aborted_connects": "0", "Com_select": "400", "Com_insert": "90", "Com_update": "10", "Com_delete": "0",
                       "Innodb_buffer_pool_pages_data": "10", "Innodb_buffer_pool_pages_total": "64", "Innodb_buffer_pool_read_requests": "1000",
                       "Innodb_buffer_pool_reads": "50", "Innodb_deadlocks": "0", "Innodb_row_lock_current_waits": "0", "Innodb_row_lock_waits": "0",
                       "Innodb_redo_log_bytes": "1234"},
            "trx": [], "locks": [], "waits": [], "deadlocks": [],
            "sessions": [["3", "root", "127.0.0.1:5000", "shop", "Query", "0", "NULL", "SELECT * FROM information_schema.processlist"]],
            "recovery": ["11.4", "0.04", "0.06", "0", "0", "0", "0", "NO"],
        }

    def __call__(self, sql, db=""):
        self.statements.append(sql)
        st = self.state
        if sql == "SHOW GLOBAL STATUS":
            return box(["Variable_name", "Value"], sorted(st["status"].items()))
        if "information_schema.innodb_trx" in sql:
            return box(["TRX_ID", "TRX_STATE", "TRX_ROWS_LOCKED", "TRX_WAITING_FOR"], st["trx"])
        if "information_schema.innodb_locks" in sql:
            return box(["LOCK_TRX_ID", "LOCK_MODE", "LOCK_TABLE", "LOCK_DATA"], st["locks"])
        if "information_schema.innodb_lock_waits" in sql:
            return box(["REQUESTING_TRX_ID", "BLOCKING_TRX_ID"], st["waits"])
        if "information_schema.innodb_deadlocks" in sql:
            return box(["REQUESTING_TRX_ID", "BLOCKING_TRX_ID"], st["deadlocks"])
        if "information_schema.processlist" in sql:
            return box(["ID", "USER", "HOST", "DB", "COMMAND", "TIME", "STATE", "INFO"], st["sessions"])
        if "information_schema.recovery" in sql:
            return box(["OPEN_MS", "UNDO_MS", "REDO_MS", "WAL_TXNS_REPLAYED", "WAL_TXNS_UNDONE", "REDO_OPS", "REDO_TABLES", "CRASH_DETECTED"], [st["recovery"]])
        if "FROM information_schema.tables GROUP BY" in sql:
            return box(["TABLE_SCHEMA", "n", "r"], [["shop", "5", "1200"], ["lk", "2", "4"]])
        if "information_schema.key_column_usage" in sql:
            return box(["CONSTRAINT_NAME", "TABLE_NAME", "COLUMN_NAME", "REFERENCED_TABLE_NAME", "REFERENCED_COLUMN_NAME"], st.get("keys", []))
        if "information_schema.referential_constraints" in sql:
            return box(["CONSTRAINT_NAME", "TABLE_NAME", "UPDATE_RULE", "DELETE_RULE"], st.get("rules", []))
        if sql.startswith("SELECT TABLE_NAME FROM information_schema.tables"):
            return box(["TABLE_NAME"], [[t] for t in st.get("tables", [])])
        m = re.search(r"TABLE_NAME = '(\w+)'", sql)
        if "information_schema.columns" in sql:
            return box(["COLUMN_NAME", "COLUMN_TYPE", "IS_NULLABLE", "COLUMN_KEY"], st.get("columns", {}).get(m.group(1), []))
        if "information_schema.statistics" in sql:
            return box(["INDEX_NAME", "SEQ_IN_INDEX", "COLUMN_NAME", "NON_UNIQUE"], st.get("statistics", {}).get(m.group(1), []))
        m = re.match(r"ANALYZE TABLE (\w+)", sql)
        if m:
            t = st["analyze"][m.group(1)]
            if t is None:
                return "ERR\nTable 'x' not found\n(0.000 sec)"
            lines = ["OK", f"| ANALYZE: {m.group(1)} ({t['rows']} rows) |", "| column       | distinct |    nulls | min        | max        | p25        | p50        | p75        |"]
            for name, d, nulls in t["columns"]:
                lines.append(f"| {name[:12].ljust(12)} | {str(d).rjust(8)} | {str(nulls).rjust(8)} | 1          | 9          | 2          | 5          | 7          |")
            return "\n".join(lines + ["(0.001 sec)"])
        raise AssertionError("unexpected statement: " + sql)


class FakeConn:
    """Stands for mcp_server._Conn in trace_query: records the statements of one connection."""
    log: list = []
    trace_answer = ""

    def __init__(self):
        FakeConn.log = []

    def _raw(self, msg):
        FakeConn.log.append(msg)
        if msg.startswith("TRACE"):
            return FakeConn.trace_answer
        return "OK\nfine\n(0.000 sec)"

    def close(self):
        pass


TRACE_ANSWER = "\n".join([
    "OK", "+-------+---------------------------+------+-------+---------+", "| Step  | What                      | Rows | ms    | Share % |",
    "+-------+---------------------------+------+-------+---------+",
    "| 1     | Parse                     | 0    | 0.050 | 10      |", "| 2     | Scan shop.orders          | 500  | 0.400 | 80      |", "| 3     | Return the answer         | 0    | 0.050 | 10      |",
    "| Total | Statement: 5 row(s) returned. |      | 0.500 | 100     |", "+-------+---------------------------+------+-------+---------+", "(0.000 sec)"])


class Tools(unittest.TestCase):
    def setUp(self):
        self.engine = FakeEngine()
        self.saved = (mcp_server._run, mcp_server._Conn)
        mcp_server._run = self.engine
        mcp_server._Conn = FakeConn

    def tearDown(self):
        mcp_server._run, mcp_server._Conn = self.saved

    # ── list_locks ──
    def test_no_transaction_no_lock(self):
        r = json.loads(mcp_server.list_locks())
        self.assertEqual(r["summary"], {"open_transactions": 0, "row_locks_held": 0, "waiting_transactions": 0, "recent_deadlocks": 0})
        self.assertEqual(r["root_blockers"], [])
        self.assertIn("No transaction is open", r["advice"])

    def test_a_wait_names_the_blocker_and_what_it_holds(self):
        st = self.engine.state
        st["trx"] = [["79", "RUNNING", "1", "NULL"], ["80", "LOCK WAIT", "0", "79"]]
        st["locks"] = [["79", "X,REC_NOT_GAP", "shop.orders", "2"]]
        st["waits"] = [["80", "79"]]
        r = json.loads(mcp_server.list_locks())
        self.assertEqual(r["root_blockers"], ["79"])
        self.assertEqual(r["waits"], [{"waiting_trx": "80", "blocked_by_trx": "79", "blocker_holds": [{"table": "shop.orders", "key": "2", "mode": "X,REC_NOT_GAP"}]}])
        self.assertEqual([t["waiting_for_trx"] for t in r["transactions"]], [None, "79"])
        self.assertEqual(r["transactions"][0]["locks"][0]["key"], "2")
        self.assertIn("KILL CONNECTION", r["advice"])

    def test_a_chain_has_one_root(self):
        st = self.engine.state
        st["trx"] = [["1", "RUNNING", "1", "NULL"], ["2", "LOCK WAIT", "1", "1"], ["3", "LOCK WAIT", "0", "2"]]
        st["waits"] = [["2", "1"], ["3", "2"]]
        r = json.loads(mcp_server.list_locks())
        self.assertEqual(r["root_blockers"], ["1"])  # (2 blocks 3 but waits itself)
        self.assertEqual(r["summary"]["waiting_transactions"], 2)

    def test_deadlocks_are_listed(self):
        self.engine.state["deadlocks"] = [["5", "6"]]
        r = json.loads(mcp_server.list_locks())
        self.assertEqual(r["deadlocks"], [{"requesting_trx": "5", "blocking_trx": "6"}])
        self.assertEqual(r["summary"]["recent_deadlocks"], 1)

    def test_an_engine_error_is_reported_not_raised(self):
        mcp_server._run = lambda sql, db="": "Error: RuSQL is not running on 127.0.0.1:7878. Start the server first."
        r = json.loads(mcp_server.list_locks())
        self.assertIn("not running", r["error"])

    # ── health_report ──
    def test_a_quiet_server_is_ok(self):
        r = json.loads(mcp_server.health_report())
        self.assertEqual(r["status"], "ok")
        self.assertEqual(r["findings"], [])
        self.assertEqual(r["server"]["uptime_seconds"], 100)
        self.assertEqual(r["statements"], {"total": 500, "per_second": 5.0, "by_kind": {"select": 400, "insert": 90, "update": 10}})
        self.assertEqual(r["buffer_pool"]["hit_ratio"], 0.95)
        self.assertEqual(r["redo_log_bytes"], 1234)
        self.assertEqual(r["databases"], [{"name": "lk", "tables": 2, "rows": 4}, {"name": "shop", "tables": 5, "rows": 1200}] if r["databases"][0]["name"] == "lk"
                         else [{"name": "shop", "tables": 5, "rows": 1200}, {"name": "lk", "tables": 2, "rows": 4}])
        self.assertEqual(r["last_start"]["crash_detected"], False)
        self.assertEqual(r["sessions"][0]["command"], "Query")

    def test_findings(self):
        st = self.engine.state
        st["status"].update({"Innodb_deadlocks": "2", "Aborted_connects": "3", "Innodb_row_lock_current_waits": "1"})
        st["trx"] = [["9", "RUNNING", "1", "NULL"], ["10", "LOCK WAIT", "0", "9"]]
        st["waits"] = [["10", "9"]]
        st["sessions"] += [["5", "bob", "h", "shop", "Query", "42", "NULL", "SELECT SLEEP(100)"], ["6", "bob", "h", "shop", "Sleep", "900", "NULL", "NULL"]]
        st["recovery"] = ["20", "1", "2", "3", "4", "5", "1", "YES"]
        r = json.loads(mcp_server.health_report())
        text = " | ".join(f["what"] for f in r["findings"])
        self.assertEqual(r["status"], "attention")
        self.assertIn("crash", text)
        self.assertIn("4 unfinished transaction(s) undone", text)
        self.assertIn("Transaction(s) 9 block 1", text)
        self.assertIn("2 deadlock(s)", text)
        self.assertIn("session 5 (42 s)", text)          # a running statement of 42 s
        self.assertNotIn("session 6", text)               # a sleeping session is not a running statement
        self.assertIn("3 login(s) failed", text)
        self.assertEqual(r["locks"]["root_blockers"], ["9"])

    def test_the_big_redo_log_is_an_info_not_a_warning(self):
        self.engine.state["status"]["Innodb_redo_log_bytes"] = str(5 * 1024 * 1024)
        r = json.loads(mcp_server.health_report())
        self.assertEqual(r["status"], "ok")
        self.assertEqual(r["findings"][0]["level"], "info")

    # ── get_table_stats ──
    def test_table_stats_maps_cut_column_names_back(self):
        st = self.engine.state
        st["columns"] = {"orders": [["id", "int", "NO", "PRI"], ["customer_identifier_long", "int", "YES", ""], ["status", "varchar(10)", "YES", ""]]}
        st["statistics"] = {"orders": [["PRIMARY", "1", "id", "0"], ["idx_status", "1", "status", "1"]]}
        st["analyze"] = {"orders": {"rows": 200, "columns": [("id", 200, 0), ("customer_ide", 40, 20), ("status", 3, 0)]}}
        r = json.loads(mcp_server.get_table_stats("orders", "shop"))
        self.assertEqual(r["rows"], 200)
        by = {c["name"]: c for c in r["columns"]}
        self.assertEqual(by["id"]["selectivity"], 1.0)
        self.assertEqual(by["id"]["key"], "PRI")
        self.assertEqual(by["customer_identifier_long"]["distinct"], 40)      # the engine cuts the name to 12 characters
        self.assertEqual(by["customer_identifier_long"]["null_fraction"], 0.1)
        self.assertEqual(by["customer_identifier_long"]["indexes"], [])
        self.assertEqual(by["status"]["indexes"], ["idx_status"])
        self.assertEqual(by["status"]["p50"], "5")
        self.assertEqual(r["indexes"], {"PRIMARY": {"columns": ["id"], "unique": True}, "idx_status": {"columns": ["status"], "unique": False}})

    def test_two_columns_that_share_their_first_twelve_characters_have_no_numbers(self):
        st = self.engine.state
        st["columns"] = {"t": [["customer_name_a", "text", "YES", ""], ["customer_name_b", "text", "YES", ""]]}
        st["statistics"] = {"t": []}
        st["analyze"] = {"t": {"rows": 10, "columns": [("customer_nam", 5, 0), ("customer_nam", 4, 0)]}}
        r = json.loads(mcp_server.get_table_stats("t"))
        self.assertTrue(all("distinct" not in c for c in r["columns"]))  # (the numbers cannot be told apart: none is guessed)

    def test_a_missing_table_is_an_error(self):
        self.engine.state["columns"] = {}
        r = json.loads(mcp_server.get_table_stats("zzz", "shop"))
        self.assertIn("not found", r["error"])

    # ── get_fk_graph ──
    def graph_state(self):
        st = self.engine.state
        st["tables"] = ["regions", "customers", "orders", "items", "lonely"]
        st["keys"] = [["customers_ibfk_1", "customers", "region_id", "regions", "id"], ["items_ibfk_1", "items", "order_id", "orders", "id"],
                      ["orders_ibfk_1", "orders", "customer_id", "customers", "id"]]
        st["rules"] = [["customers_ibfk_1", "customers", "RESTRICT", "SET NULL"], ["items_ibfk_1", "items", "RESTRICT", "CASCADE"], ["orders_ibfk_1", "orders", "CASCADE", "CASCADE"]]

    def test_graph_edges_rules_and_roots(self):
        self.graph_state()
        r = json.loads(mcp_server.get_fk_graph("shop"))
        edge = {e["child"]: e for e in r["edges"]}
        self.assertEqual(edge["orders"]["join"], "orders.customer_id = customers.id")
        self.assertEqual((edge["orders"]["on_delete"], edge["orders"]["on_update"]), ("CASCADE", "CASCADE"))
        self.assertEqual(edge["customers"]["on_delete"], "SET NULL")
        self.assertEqual(r["root_tables"], ["lonely", "regions"])
        self.assertEqual(r["unconnected_tables"], ["lonely"])
        self.assertEqual(r["tables"], sorted(r["tables"]))

    def test_one_table_and_the_chain_between_two(self):
        self.graph_state()
        r = json.loads(mcp_server.get_fk_graph("shop", "items", "regions"))
        self.assertEqual(r["table"]["references"][0]["table"], "orders")
        self.assertEqual([e["child"] for e in r["edges"]], ["items"])  # (only the edges that touch the table)
        self.assertEqual(r["join_path"]["tables"], ["items", "orders", "customers", "regions"])
        self.assertEqual(r["join_path"]["sql"], "FROM items JOIN orders ON items.order_id = orders.id JOIN customers ON orders.customer_id = customers.id JOIN regions ON customers.region_id = regions.id")

    def test_the_chain_runs_either_way_and_a_stranger_has_none(self):
        self.graph_state()
        back = json.loads(mcp_server.get_fk_graph("shop", "regions", "items"))
        self.assertEqual(back["join_path"]["tables"], ["regions", "customers", "orders", "items"])
        self.assertEqual(back["join_path"]["sql"].split(" JOIN ")[1].split(" ON ")[1], "customers.region_id = regions.id")
        none = json.loads(mcp_server.get_fk_graph("shop", "items", "lonely"))
        self.assertIsNone(none["join_path"])
        same = json.loads(mcp_server.get_fk_graph("shop", "items", "items"))
        self.assertEqual(same["join_path"]["tables"], ["items"])

    def test_graph_errors(self):
        self.graph_state()
        self.assertIn("not found", json.loads(mcp_server.get_fk_graph("shop", "nope"))["error"])
        self.assertIn("not found", json.loads(mcp_server.get_fk_graph("shop", "items", "nope"))["error"])
        self.assertIn("needs table", json.loads(mcp_server.get_fk_graph("shop", "", "items"))["error"])
        self.engine.state["tables"] = []
        self.assertIn("No tables", json.loads(mcp_server.get_fk_graph("empty"))["error"])

    # ── trace_query ──
    def test_a_select_is_traced_as_it_is(self):
        FakeConn.trace_answer = TRACE_ANSWER
        r = json.loads(mcp_server.trace_query("SELECT * FROM orders;", "shop"))
        self.assertEqual(FakeConn.log, ["USE shop;", "TRACE SELECT * FROM orders;"])
        self.assertFalse(r["rolled_back"])
        self.assertEqual([s["what"] for s in r["steps"]], ["Parse", "Scan shop.orders", "Return the answer"])
        self.assertEqual(r["steps"][1], {"what": "Scan shop.orders", "rows": 500, "ms": 0.4, "share_percent": 80, "step": 2})
        self.assertEqual(r["total"], {"ms": 0.5, "result": "Statement: 5 row(s) returned."})
        self.assertEqual(r["slowest"], "Step 2 (Scan shop.orders) takes 80% of the time")

    def test_a_write_runs_in_a_transaction_that_is_rolled_back(self):
        FakeConn.trace_answer = TRACE_ANSWER
        r = json.loads(mcp_server.trace_query("UPDATE orders SET total = 1 WHERE id = 3", "shop"))
        self.assertEqual(FakeConn.log, ["USE shop;", "BEGIN;", "TRACE UPDATE orders SET total = 1 WHERE id = 3;", "ROLLBACK;"])
        self.assertTrue(r["rolled_back"])
        self.assertIn("nothing was saved", r["note"])

    def test_the_rollback_happens_when_the_trace_fails(self):
        FakeConn.trace_answer = "ERR\nUnknown column 'zz' in 'field list'\n(0.000 sec)"
        r = json.loads(mcp_server.trace_query("DELETE FROM orders WHERE zz = 1", "shop"))
        self.assertIn("Unknown column", r["error"])
        self.assertEqual(FakeConn.log[-1], "ROLLBACK;")

    def test_ddl_and_nonsense_are_refused_before_anything_is_sent(self):
        for sql in ("DROP TABLE orders", "CREATE TABLE x (a INT)", "TRUNCATE orders", "ALTER TABLE orders ADD c INT", "SHOW TABLES", ""):
            FakeConn.log = []
            r = json.loads(mcp_server.trace_query(sql, "shop"))
            self.assertIn("error", r, sql)
            self.assertEqual(FakeConn.log, [], sql)

    def test_a_leading_trace_word_is_not_doubled(self):
        FakeConn.trace_answer = TRACE_ANSWER
        mcp_server.trace_query("TRACE SELECT 1", "")
        self.assertEqual(FakeConn.log, ["TRACE SELECT 1;"])


if __name__ == "__main__":
    unittest.main()
