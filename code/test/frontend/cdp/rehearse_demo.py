"""Rehearsal of the exhibition demo (docs/mds/EXHIBITION.md): every step of the five-minute run is done in the real desktop app, with the SQL of the guide, and what it shows is printed.

Starts the Vite dev server and the debug build of the app the way test_screens.py does, signs in with a connection of its own, pastes docs/mds/EXHIBITION_setup.sql into the editor and
runs it (Run All), then plays the steps: the home screen's View Topology window, the join and the chart, the index scene (EXPLAIN ANALYZE, the advice card with Try it, Insert into editor,
CREATE INDEX, EXPLAIN ANALYZE again), the Journey window, the session panel with a transaction that holds row locks, the B+Tree and MVCC pages, and the MCP part (Start Server, an MCP
client over stdio: execute_sql, then new_editor_tab / write_to_editor / execute_in_editor). Claude Desktop itself is not part of it. The numbers go to code/test/frontend/cdp/out/rehearse_demo.json.

  py -3 code/test/frontend/cdp/rehearse_demo.py

Needs what test_screens.py needs, plus the mcp package of the Windows Python.
"""
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_screens as ts  # noqa: E402
from cdp_lib import Page, Processes, TempConnection, ROOT  # noqa: E402
from verify_d07_gui import flat, mcp_calls, run_with_keys  # noqa: E402

OUT = Path(__file__).resolve().parent / "out"
SETUP = (ROOT.parent / "docs" / "mds" / "EXHIBITION_setup.sql").read_text(encoding="utf-8")
JOIN = ("SELECT c.name, c.city, COUNT(*) AS orders, SUM(o.total) AS total\nFROM customers c JOIN orders o ON o.customer_id = c.id\n"
        "GROUP BY c.id, c.name, c.city ORDER BY total DESC LIMIT 10;")
STATUS = "SELECT status, COUNT(*) AS n, SUM(total) AS total FROM orders GROUP BY status;"
ANALYZE = "EXPLAIN ANALYZE SELECT * FROM logs WHERE user_id = 777;"  # (SELECT * as the guide says; since R98 COUNT(*) also starts from the index right after an engine restart)
ADVICE = "EXPLAIN SELECT * FROM logs WHERE user_id = 777;"
JOURNEY = "SELECT c.city, SUM(o.total) AS total FROM orders o JOIN customers c ON o.customer_id = c.id GROUP BY c.city ORDER BY total DESC;"  # (run once before the Journey: since R98 TRACE skips the result cache, so the journey of a statement that was just run has all its steps; it used to be one)
LOCKS = "BEGIN;\nUPDATE orders SET status = 'paid' WHERE id <= 3;"
MCP_SQL = "SELECT c.city, COUNT(*) AS orders, SUM(o.total) AS total FROM orders o JOIN customers c ON c.id = o.customer_id GROUP BY c.city ORDER BY total DESC"


def exec_time(text: str):
    m = re.search(r"Execution time\s*:?\s*([\d.]+)\s*ms", text)
    return float(m.group(1)) if m else None


def main() -> int:
    results: list[dict] = []
    facts: dict = {}
    page_ref: list[Page] = []

    def step(name: str, what: str, fn):
        try:
            seen = fn()
            results.append({"step": name, "result": "P", "seen": seen})
            print(f"P  {name}  {what}\n     {seen}")
        except Exception as e:  # noqa: BLE001
            results.append({"step": name, "result": "F", "seen": f"{type(e).__name__}: {str(e)[:300]}"})
            print(f"F  {name}  {what}\n     {type(e).__name__}: {str(e)[:300]}")
            try:
                page_ref[0].screenshot(OUT / f"rehearse_failed_{name}.png")
            except Exception:  # noqa: BLE001
                pass

    procs = Processes()
    try:
        procs.vite()
        page = procs.app()
        page_ref.append(page)
        page.call("Emulation.setDeviceMetricsOverride", width=1320, height=990, deviceScaleFactor=1, mobile=False)
        with TempConnection(ts.CONNECTION, ts.DATA_DIR):
            ts.home_lists_the_saved_connection(page)
            ts.signs_in_and_runs_a_query(page)

            def setup():
                start = time.time()
                ts.run_statement(page, SETUP, "query(s)", timeout=180, button=ts.RUN_ALL)
                text = ts.result_text(page)
                assert "✗" not in text and "rror" not in text, text[:300]
                facts["setup_seconds"] = round(time.time() - start, 1)
                return f"the setup script ran in {facts['setup_seconds']} s: " + flat(re.search(r"✓[^\n]*", text).group(0) if re.search(r"✓[^\n]*", text) else text, 60)
            step("0 setup", "paste EXHIBITION_setup.sql, Run All", setup)

            def home_topology():
                page.click(".menu-logout-btn")
                page.wait("document.querySelectorAll('.home-quick-btn').length === 4", 20, "the home screen with its four buttons")
                page.click(".home-quick-btn", "View Topology")
                page.wait("document.querySelector('.eco-window') !== null", 10, "the Topology window")
                page.wait("document.querySelectorAll('.eco-node').length > 0", 20, "the cylinders")
                nodes = page.count(".eco-node")
                page.eval("window.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape'}))")
                page.wait("document.querySelector('.eco-window') === null && document.querySelectorAll('.home-quick-btn').length === 4", 10, "the home screen again")
                page.click(".home-sidebar-item", ts.CONNECTION)
                page.wait("window.monaco && window.monaco.editor.getEditors().length > 0", 60, "the editor after sign-in")
                time.sleep(1)
                ts.run_statement(page, "USE expo;", "query(s)", button=ts.RUN_ALL)
                return f"home -> View Topology window with {nodes} nodes -> Esc -> home -> signed in, USE expo"
            step("1 home", "View Topology window, then connect", home_topology)

            def join_and_chart():
                run_with_keys(page, JOIN, "10 row(s)")
                first = flat(ts.result_text(page).split("10 row(s)")[-1], 90)
                run_with_keys(page, STATUS, "3 row(s)")
                before = page.count("svg")
                page.click("button", "Chart")
                time.sleep(1.0)
                assert page.count("svg") > before, "the chart was not drawn"
                return f"join: 10 rows ({first}); status query: 3 rows; Chart button draws the chart"
            step("2 editor", "join + GROUP BY with Ctrl+Enter, then the chart", join_and_chart)

            def index_scene():
                run_with_keys(page, ANALYZE, "QUERY PLAN")
                seq = ts.result_text(page)
                assert "Seq Scan" in seq, seq[:300]
                facts["seq_ms"] = exec_time(seq)
                run_with_keys(page, ADVICE, "Seq Scan")
                page.wait("document.querySelectorAll('.plan-node').length >= 1", 15, "the plan nodes")
                page.wait("document.querySelector('.ia') && document.querySelector('.ia').innerText.includes('CREATE INDEX')", 20, "the index advice")
                advice = flat(page.eval("document.querySelector('.ia').innerText"), 400)
                page.click(".ia-btn", "Try it")
                page.wait("document.querySelector('.ia-trial') && document.querySelector('.ia-trial').innerText.includes('→')", 90, "the trial")
                trial = flat(page.eval("document.querySelector('.ia-trial').innerText"), 120)
                page.click(".ia-btn", "Insert into editor")
                inserted = page.eval("window.monaco.editor.getEditors()[0].getValue()")
                assert "CREATE INDEX" in inserted, inserted
                page.click(ts.RUN_ALL)
                page.wait("(document.querySelector('.result-panel') || document.body).innerText.includes('query(s)')", 60, "the index to be built")
                run_with_keys(page, ANALYZE, "QUERY PLAN")
                idx = ts.result_text(page)
                assert "Index Scan" in idx, idx[:300]
                facts["index_ms"] = exec_time(idx)
                facts["trial"] = trial
                return (f"EXPLAIN ANALYZE before: Seq Scan {facts['seq_ms']} ms; advice card: {advice[advice.find('CREATE INDEX'):][:80]}; Try it: {trial}; "
                        f"Insert into editor -> '{flat(inserted, 70)}' run; EXPLAIN ANALYZE after: Index Scan {facts['index_ms']} ms")
            step("3 index", "EXPLAIN ANALYZE, advice card, Try it, create the index, EXPLAIN ANALYZE again", index_scene)

            def reset_index():
                ts.run_statement(page, "DROP INDEX idx_logs_user_id ON logs;", "query(s)", button=ts.RUN_ALL)
                run_with_keys(page, ANALYZE, "QUERY PLAN")
                text = ts.result_text(page)
                assert "Seq Scan" in text, text[:300]
                return f"DROP INDEX idx_logs_user_id ON logs -> EXPLAIN ANALYZE is a Seq Scan again ({exec_time(text)} ms): the index scene can be played again"
            step("3b reset", "drop the index to play the index scene again", reset_index)

            def journey():
                page.click('[title="SQL Editor"]')
                page.wait("window.monaco && window.monaco.editor.getEditors().length > 0", 20, "the editor")
                ts.run_statement(page, JOURNEY, "row(s)")  # (the statement is run first, then selected: the answer is in the result cache)
                page.eval("(() => { const e = window.monaco.editor.getEditors()[0]; e.focus(); e.setSelection(e.getModel().getFullModelRange()); })()")
                time.sleep(0.3)
                page.wait("(() => { const b = [...document.querySelectorAll('button')].find(x => (x.title || '').startsWith('Journey')); return b && !b.disabled; })()", 30, "the Journey button to be enabled")
                page.click('button[title^="Journey"]')
                page.wait("document.querySelector('.jn-modal') !== null", 30, "the journey window")
                if page.exists(".jn-primary"):
                    page.click(".jn-primary")
                time.sleep(3.0)
                full = page.eval("document.querySelector('.jn-modal').innerText")
                steps = re.search(r"Step \d+ of (\d+)", full)
                text = flat(full, 200)
                page.eval("(document.querySelector('.modal-overlay') || {click() {}}).click()")
                assert steps and int(steps.group(1)) >= 8, text  # (11 steps; the result cache used to make it 1 for a statement that was just run)
                facts["journey_steps"] = int(steps.group(1))
                return "Journey window: " + text
            step("4 journey", "select a statement, Journey", journey)

            def locks():
                ts.run_statement(page, LOCKS, "2 query(s)", button=ts.RUN_ALL)
                page.click('[title="Server Manager"]')
                page.wait("document.querySelector('.srv-rbar-btn') !== null", 15, "the Server Manager")
                page.click('[title="Sessions"]')
                page.wait("(document.querySelector('.srv-slide-panel') || {innerText: ''}).innerText.includes('row lock')", 20, "the open transaction with row locks")
                text = flat(page.eval("document.querySelector('.srv-slide-panel').innerText"), 200)
                ts.run_statement(page, "ROLLBACK;", "query(s)", button=ts.RUN_ALL)
                return "Sessions panel: " + text
            step("5 locks", "BEGIN + UPDATE, then the Sessions panel shows the row locks", locks)

            def pages():
                seen = []
                for title, word in (("B+Tree", "B+Tree"), ("MVCC", "MVCC")):
                    page.click(f'[title="{title}"]')
                    time.sleep(2.5)
                    assert word in page.text(), title
                    seen.append(title)
                return "pages opened and drawn: " + ", ".join(seen)
            step("6 pages", "B+Tree and MVCC pages", pages)

            def mcp():
                page.click('[title="Server Manager"]')
                page.wait("document.querySelector('.srv-action-btn') !== null", 15, "the Server Manager")
                if page.exists(".srv-action-btn", "Start Server"):
                    ts.click_text(page, "Start Server", ".srv-action-btn", exact=False)
                page.wait("document.body.innerText.includes('RUNNING')", 40, "RUNNING")
                page.click('[title="SQL Editor"]')
                page.wait("window.monaco && window.monaco.editor.getEditors().length > 0", 20, "the editor")
                out = mcp_calls([("execute_sql", {"sql": MCP_SQL, "database": "expo"}),
                                 ("new_editor_tab", {"name": "claude-demo", "query": ""}),
                                 ("write_to_editor", {"query": MCP_SQL, "tab": "claude-demo"}),
                                 ("execute_in_editor", {"tab": "claude-demo", "timeout_seconds": 30})])
                rows = json.loads(out[0])
                assert isinstance(rows, list) and len(rows) == 5, out[0][:200]
                for text in out[1:]:
                    assert not text.startswith("Error"), text[:200]
                page.wait("(document.querySelector('.result-panel') || document.body).innerText.includes('5 row(s)')", 15, "the result in the app")
                return (f"Start Server -> RUNNING; MCP execute_sql returned {len(rows)} rows (first: {json.dumps(rows[0], ensure_ascii=False)}); "
                        f"new_editor_tab + write_to_editor + execute_in_editor: the app shows tab 'claude-demo' with '5 row(s)'")
            step("7 mcp", "Start Server, MCP execute_sql and the editor tools", mcp)

            page.screenshot(OUT / "rehearse_demo_last.png")
            try:
                page.click('[title="Server Manager"]')
                if page.exists(".srv-action-btn", "Stop Server"):
                    ts.click_text(page, "Stop Server", ".srv-action-btn", exact=False)
                    time.sleep(1.5)
            except Exception:  # noqa: BLE001
                pass
    finally:
        procs.stop()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "rehearse_demo.json").write_text(json.dumps({"steps": results, "facts": facts}, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n" + " ".join(f"{r['step'].split()[0]}={r['result']}" for r in results), "|", json.dumps(facts, ensure_ascii=False))
    return 0 if all(r["result"] == "P" for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
