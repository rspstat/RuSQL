"""Unit tests of the MCP tools slow_queries, kill_session, import_csv, export_csv and diff_schema, and of the resources and prompts, against a fake engine (no server needed):
py -3 code/test/mcp/test_r70_tools.py

The fake answers what the tools ask the engine for in the engine's own output format (box-drawn tables), so the reading and the decisions are tested, not the engine.
"""
import asyncio
import json
import re
import sys
import tempfile
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
    def __init__(self):
        self.statements = []
        self.log_on = True
        self.slow = []        # rows of information_schema.slow_log
        self.sessions = {}    # id -> row of the processlist
        self.columns = {"customers": ["id", "name", "email"]}
        self.select_answer = box(["id", "name"], [["1", "Kim"], ["2", "NULL"]])

    def __call__(self, sql, db=""):
        self.statements.append(sql)
        if sql.startswith("SELECT @@slow_query_log"):
            return box(["slow_query_log", "long_query_time"], [["1" if self.log_on else "0", "0.500000"]])
        if sql.startswith("SHOW GLOBAL STATUS LIKE 'Slow_queries'"):
            return box(["Variable_name", "Value"], [["Slow_queries", str(len(self.slow))]])
        if "information_schema.slow_log" in sql:
            return box(["START_TIME", "USER_HOST", "QUERY_TIME", "ROWS_SENT", "DB", "SQL_TEXT", "THREAD_ID"], self.slow)
        if "information_schema.processlist" in sql:
            m = re.search(r"WHERE ID = (\d+)", sql)
            row = self.sessions.get(m.group(1))
            return box(["ID", "USER", "HOST", "DB", "COMMAND", "TIME", "STATE", "INFO"], [row] if row else [])
        m = re.match(r"KILL (QUERY|CONNECTION) (\d+)", sql)
        if m:
            return "OK\nQuery OK"
        if "information_schema.columns" in sql and "COLUMN_TYPE" not in sql:
            m = re.search(r"TABLE_NAME = '(\w+)'", sql)
            return box(["COLUMN_NAME"], [[c] for c in self.columns.get(m.group(1), [])])
        if sql.startswith("SELECT") or sql.startswith("WITH"):
            return self.select_answer
        raise AssertionError("unexpected statement: " + sql)


class R70Tools(unittest.TestCase):
    def setUp(self):
        self.engine = FakeEngine()
        self.saved_run = mcp_server._run
        mcp_server._run = self.engine

    def tearDown(self):
        mcp_server._run = self.saved_run

    # ── slow_queries ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
    def test_slow_queries_groups_the_same_statement_and_puts_the_most_time_first(self):
        self.engine.slow = [
            ["2026-11-07 10:00:00.000000", "root@127.0.0.1", "0.700000", "5", "shop", "SELECT  * FROM orders WHERE status = 1", "3"],
            ["2026-11-07 10:00:05.000000", "root@127.0.0.1", "0.900000", "5", "shop", "SELECT * FROM orders WHERE status = 1", "3"],
            ["2026-11-07 10:00:09.000000", "root@127.0.0.1", "2.000000", "1", "shop", "SELECT COUNT(*) FROM big", "4"],
        ]
        out = json.loads(mcp_server.slow_queries())
        self.assertEqual(out["slow_query_log"], "ON")
        self.assertEqual(out["long_query_time_seconds"], 0.5)
        self.assertEqual(out["slow_queries_counted_since_start"], 3)
        first, second = out["statements"]
        self.assertEqual(first["sql"], "SELECT COUNT(*) FROM big")
        self.assertEqual(second["sql"], "SELECT * FROM orders WHERE status = 1")  # (the blanks of the two are the same statement)
        self.assertEqual(second["runs"], 2)
        self.assertEqual(second["total_seconds"], 1.6)
        self.assertEqual(second["slowest_seconds"], 0.9)
        self.assertIn("suggest_indexes", out["next"])

    def test_slow_queries_limit_and_minimum(self):
        self.engine.slow = [["2026-11-07 10:00:0%d.000000" % i, "u", "%d.000000" % (i + 1), "1", "d", "SELECT %d" % i, "1"] for i in range(5)]
        out = json.loads(mcp_server.slow_queries(limit=2, min_seconds=2.5))
        self.assertEqual([s["sql"] for s in out["statements"]], ["SELECT 4", "SELECT 3"])

    def test_slow_queries_says_how_to_turn_the_log_on(self):
        self.engine.log_on = False
        out = json.loads(mcp_server.slow_queries())
        self.assertEqual(out["slow_query_log"], "OFF")
        self.assertEqual(out["statements"], [])
        self.assertIn("SET GLOBAL slow_query_log = ON", out["next"])
        self.assertIn("agrees", out["next"])  # (it never turns the log on by itself)
        self.assertFalse([s for s in self.engine.statements if s.startswith("SET")])

    def test_slow_queries_with_the_log_on_and_nothing_slow(self):
        out = json.loads(mcp_server.slow_queries())
        self.assertEqual(out["statements"], [])
        self.assertIn("No statement ran", out["next"])

    # ── kill_session ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
    def test_kill_session_describes_and_refuses_until_confirmed(self):
        self.engine.sessions["7"] = ["7", "app", "10.0.0.5:4000", "shop", "Query", "42", "executing", "SELECT SLEEP(100)"]
        out = mcp_server.kill_session(7)
        self.assertTrue(out.startswith("Refused"))
        self.assertIn("app@10.0.0.5:4000", out)
        self.assertIn("SELECT SLEEP(100)", out)
        self.assertIn("confirmed=True", out)
        self.assertFalse([s for s in self.engine.statements if s.startswith("KILL")])

    def test_kill_session_kills_the_query_after_confirmation(self):
        self.engine.sessions["7"] = ["7", "app", "h", "shop", "Query", "42", "executing", "SELECT 1"]
        out = json.loads(mcp_server.kill_session(7, confirmed=True))
        self.assertEqual(out["killed"], "query")
        self.assertIn("KILL QUERY 7", self.engine.statements)

    def test_kill_session_can_close_the_connection(self):
        self.engine.sessions["8"] = ["8", "app", "h", "shop", "Sleep", "3", "", "NULL"]
        out = mcp_server.kill_session(8, kill_connection=True)
        self.assertIn("whole connection", out)
        self.assertNotIn("KILL CONNECTION 8", self.engine.statements)
        out = json.loads(mcp_server.kill_session(8, kill_connection=True, confirmed=True))
        self.assertEqual(out["killed"], "connection")
        self.assertIn("KILL CONNECTION 8", self.engine.statements)

    def test_kill_session_of_a_session_that_is_not_there(self):
        self.assertIn("no session 99", mcp_server.kill_session(99, confirmed=True))
        self.assertFalse([s for s in self.engine.statements if s.startswith("KILL")])

    # ── import_csv / export_csv ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
    def run_script_fake(self):
        """run_script goes to the engine by a connection of its own: a fake one that records the statements and says ok."""
        recorded = []

        class FakeConn:
            def _raw(self, sql):
                recorded.append(sql)
                return "OK\n1 row(s) inserted.\n(0.001 sec)"

            def close(self):
                pass

        saved = mcp_server._Conn
        mcp_server._Conn = FakeConn
        self.addCleanup(lambda: setattr(mcp_server, "_Conn", saved))
        return recorded

    def test_import_csv_previews_by_default_and_names_the_columns_in_the_header(self):
        recorded = self.run_script_fake()
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "c.csv"
            f.write_text("﻿Email,id,name\nkim@x.com,1,Kim\n,2,O'Neil\\x\n", encoding="utf-8")
            out = json.loads(mcp_server.import_csv("customers", str(f), "shop"))
        self.assertEqual(out["rows_in_file"], 2)
        self.assertEqual(out["columns"], ["email", "id", "name"])  # (the table's spelling of the header names)
        self.assertEqual(out["mode"], "preview")
        self.assertFalse(out["committed"])
        self.assertEqual(recorded[-1], "ROLLBACK;")
        insert = next(s for s in recorded if s.startswith("INSERT"))
        self.assertEqual(insert, "INSERT INTO customers (email, id, name) VALUES ('kim@x.com', '1', 'Kim'), (NULL, '2', 'O''Neil\\\\x');")  # (an empty cell is NULL, ' and \\ are escaped)

    def test_import_csv_commit_saves(self):
        recorded = self.run_script_fake()
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "c.csv"
            f.write_text("1,Kim\n", encoding="utf-8")
            out = json.loads(mcp_server.import_csv("customers", str(f), "shop", has_header=False, commit=True))
        self.assertTrue(out["committed"])
        self.assertIn("COMMIT;", recorded)
        self.assertIn("INSERT INTO customers (id, name, email) VALUES ('1', 'Kim', NULL);", recorded[-2:] + [s for s in recorded if s.startswith("INSERT")])

    def test_import_csv_refuses_what_does_not_fit(self):
        self.run_script_fake()
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "c.csv"
            f.write_text("id,nope\n1,2\n", encoding="utf-8")
            self.assertIn("nope", mcp_server.import_csv("customers", str(f), "shop"))
            f.write_text("1,2,3,4\n", encoding="utf-8")
            self.assertIn("more cells", mcp_server.import_csv("customers", str(f), "shop", has_header=False))
            self.assertIn("not a file", mcp_server.import_csv("customers", str(Path(d) / "missing.csv"), "shop"))
            self.assertIn("table name", mcp_server.import_csv("customers; DROP TABLE x", str(f), "shop"))
            self.assertIn("was not found", mcp_server.import_csv("nosuch", str(f), "shop"))

    def test_export_csv_writes_the_answer_and_does_not_replace_a_file(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "out.csv"
            out = json.loads(mcp_server.export_csv("SELECT id, name FROM customers", str(f), "shop"))
            self.assertEqual(out["rows"], 2)
            self.assertEqual(f.read_text(encoding="utf-8-sig").splitlines(), ["id,name", "1,Kim", "2,"])  # (NULL is an empty cell)
            self.assertIn("Refused", mcp_server.export_csv("SELECT id, name FROM customers", str(f), "shop"))
            self.assertIn("rows", mcp_server.export_csv("SELECT id, name FROM customers", str(f), "shop", overwrite=True))
            self.assertIn("Refused", mcp_server.export_csv("DELETE FROM customers", str(Path(d) / "x.csv"), "shop"))
            self.assertIn("Refused", mcp_server.export_csv("SELECT 1", str(Path(d) / "x.exe"), "shop"))
            self.assertIn("does not exist", mcp_server.export_csv("SELECT 1", str(Path(d) / "no" / "x.csv"), "shop"))

    # ── diff_schema ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
    def test_diff_schema_tells_tables_columns_indexes_and_foreign_keys_that_differ(self):
        snapshots = {
            "a": {"t": {"columns": {"id": ["int", "NO", "NULL", ""], "x": ["int", "YES", "NULL", ""]}, "indexes": {"PRIMARY": [True, ["id"]]}, "foreign_keys": {}},
                  "only_a": {"columns": {"id": ["int", "NO", "NULL", ""]}, "indexes": {}, "foreign_keys": {}}},
            "b": {"t": {"columns": {"id": ["bigint", "NO", "NULL", ""], "y": ["int", "YES", "NULL", ""]}, "indexes": {"PRIMARY": [True, ["id"]], "idx_y": [False, ["y"]]}, "foreign_keys": {}},
                  "only_b": {"columns": {"id": ["int", "NO", "NULL", ""]}, "indexes": {}, "foreign_keys": {}}},
        }
        saved = mcp_server._schema_snapshot
        mcp_server._schema_snapshot = lambda db: snapshots[db]
        self.addCleanup(lambda: setattr(mcp_server, "_schema_snapshot", saved))
        out = json.loads(mcp_server.diff_schema("a", "b"))
        self.assertFalse(out["identical"])
        self.assertEqual(out["tables_only_in_a"], ["only_a"])
        self.assertEqual(out["tables_only_in_b"], ["only_b"])
        t = out["tables_that_differ"]["t"]
        self.assertEqual(t["columns_only_in_b"], ["y"])
        self.assertEqual(t["columns_only_in_a"], ["x"])
        self.assertEqual(t["columns_that_differ"]["id"], {"a": ["int", "NO", "NULL", ""], "b": ["bigint", "NO", "NULL", ""]})
        self.assertEqual(t["indexes_only_in_b"], ["idx_y"])
        same = json.loads(mcp_server.diff_schema("a", "a"))
        self.assertTrue(same["identical"])

    def test_diff_schema_refuses_a_name_that_is_not_a_database_name(self):
        self.assertIn("is not a database name", mcp_server.diff_schema("a'; DROP TABLE t; --", "b"))

    # ── the server lists them ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
    def test_the_tools_resources_and_prompts_are_registered(self):
        tools = {t.name for t in asyncio.run(mcp_server.mcp.list_tools())}
        for name in ("slow_queries", "kill_session", "import_csv", "export_csv", "diff_schema", "suggest_indexes"):
            self.assertIn(name, tools)
        resources = {str(r.uri) for r in asyncio.run(mcp_server.mcp.list_resources())}
        templates = {t.uriTemplate for t in asyncio.run(mcp_server.mcp.list_resource_templates())}
        self.assertIn("rusql://databases", resources)
        self.assertIn("rusql://schema/{database}", templates)
        prompts = {p.name for p in asyncio.run(mcp_server.mcp.list_prompts())}
        self.assertTrue({"optimize_query", "find_slow_queries", "explain_schema"} <= prompts)

    def test_the_prompts_ask_for_the_tools_and_for_agreement(self):
        self.assertIn("suggest_indexes", mcp_server.optimize_query("SELECT 1", "shop"))
        self.assertIn("slow_queries", mcp_server.find_slow_queries())
        self.assertIn("rusql://schema/shop", mcp_server.explain_schema("shop"))

    def test_the_schema_resource_reads_information_schema(self):
        self.engine.select_answer = box(["TABLE_NAME", "COLUMN_NAME", "COLUMN_TYPE", "IS_NULLABLE", "COLUMN_KEY"],
                                        [["t", "id", "int", "NO", "PRI"], ["t", "n", "varchar(5)", "YES", ""]])
        out = json.loads(mcp_server.schema_resource("shop"))
        self.assertEqual(out["tables"]["t"][0], {"name": "id", "type": "int", "nullable": False, "key": "PRI"})
        self.assertEqual(out["tables"]["t"][1]["nullable"], True)
        self.assertIn("is not a database name", mcp_server.schema_resource("x'; DROP"))


if __name__ == "__main__":
    unittest.main()
