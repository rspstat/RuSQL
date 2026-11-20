"""The seven rows of D07 table 7 that are done in the app (STC-E07-001 ... STC-E11-001), run for real against the running desktop app and printed with what was seen.

Starts the Vite dev server and the debug build of the app the way test_screens.py does, signs in with a connection of its own, fills the small shop database and then:
  E07-001  SELECT in the editor, run with the Ctrl+Enter key        E09-001  Start Server, a client connects, the session panel lists it
  E07-002  EXPLAIN SELECT: plan boxes and the index advice card     E09-002  a client runs a long statement, "Stop statement" in the panel stops it
  E08-001  ERD Editor: table cards and a foreign key line           E11-001  an MCP client (the real mcp_server.py over stdio) opens a tab, writes SQL, runs it
  E08-002  ERD: drag a column onto another table's column, read the DDL preview, apply
Each row is P or F with a line of what was observed; the result goes to code/test/frontend/cdp/out/d07_gui.json too. A failed row leaves a screenshot in out/.

  py -3 code/test/frontend/cdp/verify_d07_gui.py

Needs what test_screens.py needs, plus the mcp and pymysql packages of the Windows Python.
"""
import asyncio
import json
import re
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_screens as ts  # noqa: E402
from cdp_lib import Page, Processes, TempConnection, ROOT  # noqa: E402

OUT = Path(__file__).resolve().parent / "out"
MYSQL_PORT = 13306
LONG_QUERY = "SELECT COUNT(*) FROM orders a, orders b, orders c WHERE a.total + b.total + c.total = 1"  # (about 3.4 billion row combinations: it runs until it is stopped)


def flat(text: str, limit: int = 110) -> str:
    return re.sub(r"\s+", " ", text or "").strip()[:limit]


def run_with_keys(page: Page, sql: str, wait_text: str, timeout: float = 30.0):
    """Put `sql` in the editor and run it with the Ctrl+Enter key (a real key event), not with the Run button."""
    page.click('[title="SQL Editor"]')
    page.wait("window.monaco && window.monaco.editor.getEditors().length > 0", 20, "the editor")
    page.set_editor(sql)
    page.wait(f"(() => {{ const b = document.querySelector({json.dumps(ts.RUN)}); return b && !b.disabled; }})()", 30, "the Run button to be enabled")
    page.eval("window.monaco.editor.getEditors()[0].focus()")
    time.sleep(0.3)
    for kind in ("rawKeyDown", "keyUp"):
        page.call("Input.dispatchKeyEvent", type=kind, modifiers=2, key="Enter", code="Enter", windowsVirtualKeyCode=13, nativeVirtualKeyCode=13)
    page.wait(f"(document.querySelector('.result-panel') || document.body).innerText.includes({json.dumps(wait_text)})", timeout, f"{wait_text!r} in the result")


def mouse_drag(page: Page, a: dict, b: dict):
    page.call("Input.dispatchMouseEvent", type="mouseMoved", x=a["x"], y=a["y"])
    page.call("Input.dispatchMouseEvent", type="mousePressed", x=a["x"], y=a["y"], button="left", clickCount=1)
    for step in range(1, 13):
        page.call("Input.dispatchMouseEvent", type="mouseMoved", x=a["x"] + (b["x"] - a["x"]) * step / 12, y=a["y"] + (b["y"] - a["y"]) * step / 12, buttons=1)
        time.sleep(0.04)
    page.call("Input.dispatchMouseEvent", type="mouseReleased", x=b["x"], y=b["y"], button="left", clickCount=1)


def center(page: Page, js_element: str):
    return page.eval(f"(() => {{ const e = {js_element}; if (!e) return null; const r = e.getBoundingClientRect(); return {{x: r.x + r.width / 2, y: r.y + r.height / 2}}; }})()")


def mcp_calls(calls: list[tuple[str, dict]]) -> list[str]:
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    async def go():
        params = StdioServerParameters(command=sys.executable, args=[str(ROOT / "mcp" / "mcp_server.py")])
        out = []
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                for name, args in calls:
                    result = await session.call_tool(name, args)
                    out.append("".join(getattr(c, "text", "") for c in result.content))
        return out
    return asyncio.run(go())


def main() -> int:
    import pymysql
    results: list[dict] = []
    page_ref: list[Page] = []

    def row(sid: str, what: str, fn):
        try:
            seen = fn()
            results.append({"id": sid, "result": "P", "seen": seen})
            print(f"P  {sid}  {what}\n     {seen}")
        except Exception as e:  # noqa: BLE001
            results.append({"id": sid, "result": "F", "seen": f"{type(e).__name__}: {str(e)[:300]}"})
            print(f"F  {sid}  {what}\n     {type(e).__name__}: {str(e)[:300]}")
            try:
                page_ref[0].screenshot(OUT / f"d07_failed_{sid}.png")
            except Exception:  # noqa: BLE001
                pass

    procs = Processes()
    clients = []
    try:
        procs.vite()
        page = procs.app()
        page_ref.append(page)
        page.call("Emulation.setDeviceMetricsOverride", width=1320, height=990, deviceScaleFactor=1, mobile=False)
        with TempConnection(ts.CONNECTION, ts.DATA_DIR):
            ts.home_lists_the_saved_connection(page)
            ts.signs_in_and_runs_a_query(page)
            ts.a_script_fills_the_shop_database(page)

            def e07_001():
                run_with_keys(page, "SELECT id, name, city FROM customers ORDER BY id LIMIT 5", "5 row(s)")
                text = ts.result_text(page)
                assert "c1" in text and "c5" in text, text[:200]
                return "Ctrl+Enter key: result '5 row(s)', grid shows c1 .. c5: " + flat(text.split("5 row(s)")[-1], 70)
            row("STC-E07-001", "SELECT in the editor, Ctrl+Enter", e07_001)

            def e07_002():
                run_with_keys(page, "EXPLAIN SELECT * FROM orders WHERE customer_id = 7", "Seq Scan")
                page.wait("document.querySelectorAll('.plan-node').length >= 1", 15, "the plan nodes")
                page.wait("document.querySelector('.ia') && document.querySelector('.ia').innerText.includes('CREATE INDEX')", 20, "the index advice")
                nodes = page.count(".plan-node")
                advice = page.eval("document.querySelector('.ia').innerText")
                assert "idx_orders_customer_id" in advice, advice
                return f"{nodes} plan box(es) drawn, advice card: " + flat(advice[advice.find('CREATE INDEX'):], 80)
            row("STC-E07-002", "EXPLAIN SELECT: plan diagram and index advice", e07_002)

            def e08_001():
                page.click('[title="ERD Editor"]')
                page.wait("document.querySelectorAll('.erd-card').length >= 2", 20, "the ERD cards")
                names = page.eval("[...document.querySelectorAll('.erd-card-header')].map(e => e.innerText.split('\\n')[0].trim())")
                fks = page.count(".erd-fk")
                assert fks >= 1, "no foreign key line"
                return f"{len(names)} table cards ({', '.join(names)}), {fks} foreign key line(s)"
            row("STC-E08-001", "ERD Editor draws cards and the foreign key line", e08_001)

            def e08_002():
                ts.run_statement(page, "CREATE TABLE shipments (id INT PRIMARY KEY, order_id INT, carrier VARCHAR(20))", "query(s)", button=ts.RUN_ALL)
                page.click('[title="ERD Editor"]')
                page.wait("document.querySelectorAll('.erd-card').length >= 2", 20, "the ERD cards")
                page.click(".erd-tool-btn", "Refresh")
                page.wait("document.querySelectorAll('.erd-card').length >= 3", 20, "the third card")
                before = page.count(".erd-fk")
                time.sleep(0.8)
                src = center(page, "document.querySelector('[data-erd-table=\"shipments\"][data-erd-column=\"order_id\"] .erd-col-handle')")
                dst = center(page, "document.querySelector('[data-erd-table=\"orders\"][data-erd-column=\"id\"]')")
                assert src and dst, f"the handle {src} / the target column {dst}"
                mouse_drag(page, src, dst)
                page.wait("document.querySelector('.fk-modal') !== null", 10, "the relationship window")
                ddl = flat(page.eval("document.querySelector('.fk-ddl').innerText"), 160)
                assert "FOREIGN KEY" in ddl and "shipments" in ddl and "orders" in ddl, ddl
                page.click(".fk-btn.fk-primary")
                page.wait("document.querySelector('.fk-modal') === null", 20, "the window to close")
                page.click(".erd-tool-btn", "Refresh")
                page.wait(f"document.querySelectorAll('.erd-fk').length > {before}", 20, "the new relationship line")
                after = page.count(".erd-fk")
                ts.run_statement(page, "SHOW CREATE TABLE shipments", "FOREIGN KEY")
                return f"dragged shipments.order_id onto orders.id; preview: {ddl}; applied, lines {before} -> {after}, SHOW CREATE TABLE has the FOREIGN KEY"
            row("STC-E08-002", "ERD: drag a column onto another table's column, preview, apply", e08_002)

            def connect():
                c = pymysql.connect(host="127.0.0.1", port=MYSQL_PORT, user="root", password="root", database="shop", autocommit=True, connect_timeout=15)
                clients.append(c)
                return c

            def e09_001():
                page.click('[title="Server Manager"]')
                page.wait("document.querySelector('.srv-action-btn') !== null", 15, "the Server Manager")
                assert page.exists(".srv-action-btn", "Start Server"), "the server is already running"
                ts.click_text(page, "Start Server", ".srv-action-btn", exact=False)
                page.wait("document.body.innerText.includes('RUNNING')", 40, "RUNNING")
                client = connect()
                with client.cursor() as cur:
                    cur.execute("SELECT 1")
                page.click('[title="Sessions"]')
                page.wait("document.querySelector('.srv-slide-panel') && document.querySelector('.srv-slide-panel').innerText.includes('ACTIVE SESSIONS')", 15, "the session panel")
                page.wait("(document.querySelector('.srv-slide-panel').innerText.match(/#\\d+/g) || []).length >= 2", 15, "two sessions")
                panel = page.eval("document.querySelector('.srv-slide-panel').innerText")
                sessions = len(set(re.findall(r"#(\d+)\s+root", panel)))
                assert sessions >= 2, panel[:300]
                return f"Start Server -> RUNNING; a MySQL client (127.0.0.1:{MYSQL_PORT}) connected; the panel lists {sessions} sessions (this app + the client)"
            row("STC-E09-001", "Start Server, the session panel shows the connected session", e09_001)

            def e09_002():
                box: dict = {}

                def long_statement():
                    c = connect()
                    start = time.time()
                    try:
                        with c.cursor() as cur:
                            cur.execute(LONG_QUERY)
                        box["end"] = "finished"
                    except Exception as e:  # noqa: BLE001
                        box["end"] = e.args
                    box["seconds"] = time.time() - start
                    box["alive"] = False
                    try:
                        with c.cursor() as cur:
                            cur.execute("SELECT 1")
                            box["alive"] = cur.fetchone() == (1,)
                    except Exception:  # noqa: BLE001
                        pass
                t = threading.Thread(target=long_statement, daemon=True)
                t.start()
                page.click('[title="Sessions"]') if not page.exists(".srv-slide-panel", "ACTIVE SESSIONS") else None
                page.wait("[...document.querySelectorAll('.srv-slide-panel button')].some(b => b.innerText.trim() === 'Stop statement')", 30, "the Stop statement button of the running session")
                ts.click_text(page, "Stop statement", ".srv-slide-panel button")
                page.wait("[...document.querySelectorAll('.srv-slide-panel button')].some(b => b.innerText.includes('Stop it?'))", 5, "the confirm button")
                ts.click_text(page, "Stop it? yes", ".srv-slide-panel button", exact=False)
                t.join(30)
                assert not t.is_alive(), "the statement was not stopped within 30 s"
                end = box.get("end")
                assert isinstance(end, tuple) and end and end[0] == 1317, f"the client got {end}"
                message = flat(page.eval("(document.querySelector('.srv-slide-panel') || {innerText: ''}).innerText"), 400)
                note = re.search(r"Asked the statement of session #\d+ to stop\.", message)
                return (f"'Stop statement' + 'Stop it? yes': the client's statement ended after {box['seconds']:.1f} s with {end[0]} '{end[1]}'; "
                        f"the connection still answers SELECT 1: {box.get('alive')}; panel: {note.group(0) if note else 'no message seen'}")
            row("STC-E09-002", "session panel: stop a running statement (KILL QUERY)", e09_002)

            def e11_001():
                page.click('[title="SQL Editor"]')
                page.wait("window.monaco && window.monaco.editor.getEditors().length > 0", 20, "the editor")
                sql = "SELECT COUNT(*) AS n FROM customers"
                out = mcp_calls([("new_editor_tab", {"name": "claude-check", "query": ""}),
                                 ("write_to_editor", {"query": sql, "tab": "claude-check"}),
                                 ("execute_in_editor", {"tab": "claude-check", "timeout_seconds": 30})])
                for text in out:
                    assert not text.startswith("Error"), text[:200]
                page.wait("document.body.innerText.includes('claude-check')", 10, "the new tab")
                editor = page.eval("window.monaco.editor.getEditors()[0].getValue()")
                assert editor.strip() == sql, editor
                page.wait("(document.querySelector('.result-panel') || document.body).innerText.includes('1 row(s)')", 15, "the result in the app")
                shown = ts.result_text(page)
                assert "300" in shown, shown[:200]
                return (f"MCP over stdio: new_editor_tab -> '{flat(out[0], 50)}'; write_to_editor -> '{flat(out[1], 40)}'; execute_in_editor -> '{flat(out[2], 70)}'; "
                        f"the app shows tab 'claude-check' with the SQL and the result 1 row(s) n = 300")
            row("STC-E11-001", "MCP drives the editor: new tab, write, run", e11_001)

            page.screenshot(OUT / "d07_gui_last.png")
            try:
                page.click('[title="Server Manager"]')
                if page.exists(".srv-action-btn", "Stop Server"):
                    ts.click_text(page, "Stop Server", ".srv-action-btn", exact=False)
                    time.sleep(1.5)
            except Exception:  # noqa: BLE001
                pass
    finally:
        for c in clients:
            try:
                c.close()
            except Exception:  # noqa: BLE001
                pass
        procs.stop()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "d07_gui.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\n" + " ".join(f"{r['id']}={r['result']}" for r in results))
    return 0 if all(r["result"] == "P" for r in results) and len(results) == 7 else 1


if __name__ == "__main__":
    sys.exit(main())
